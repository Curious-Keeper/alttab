#include "../include/manager.hpp"
#include "../include/renderpasses.hpp"
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprlang.hpp>
#include <hyprutils/math/Misc.hpp>
#include <linux/input-event-codes.h>
#include <src/Compositor.hpp>
#include <src/config/ConfigDataValues.hpp>
#include <src/config/ConfigManager.hpp>
#include <src/config/ConfigValue.hpp>
#include <src/desktop/state/FocusState.hpp>
#include <src/devices/IKeyboard.hpp>
#include <src/managers/input/InputManager.hpp>
#include <src/render/Renderer.hpp>
#include <xkbcommon/xkbcommon-keysyms.h>

APICALL EXPORT std::string PLUGIN_API_VERSION() {
  return HYPRLAND_API_VERSION;
}

static bool unloadGuard = false;

static void onRender(eRenderStage stage) {
  if (!g_pCarouselManager->active)
    return;

  if (stage == eRenderStage::RENDER_PRE) {
    g_pHyprRenderer->setCursorHidden(true);
    g_pCarouselManager->update();
  }

  if (stage == eRenderStage::RENDER_LAST_MOMENT) {
    g_pHyprRenderer->m_renderPass.add(makeUnique<BlurPass>());
    g_pHyprRenderer->m_renderPass.add(makeUnique<RenderPass>(g_pCarouselManager->getRenderList()));
    g_pHyprRenderer->setCursorHidden(false);
  }

  g_pCarouselManager->damageMonitors();
}

static void onWindowCreated(PHLWINDOW w) {
  if (!CarouselManager::shouldIncludeWindow(w) || !w->m_monitor)
    return;

  int id = w->m_monitor->m_id;
  g_pCarouselManager->monitors[id].windows.emplace_back(makeUnique<WindowContainer>(w));
  g_pCarouselManager->refreshLayout();
}

static void onWindowClosed(PHLWINDOW w) {
  bool found = false;
  for (auto &[id, mon] : g_pCarouselManager->monitors) {
    for (auto &el : mon.windows) {
      if (el->window == w) {
        el->markForRemoval();
        found = true;
        break;
      }
    }
    if (found)
      break;
  }
  g_pCarouselManager->refreshLayout();
}

static void onWindowMoved(std::any p) {
  if (!g_pCarouselManager->active)
    return;

  try {
    auto args = std::any_cast<std::vector<std::any>>(p);
    if (args.empty())
      return;

    auto w = std::any_cast<PHLWINDOW>(args[0]);
    if (!w)
      return;

    Log::logger->log(Log::TRACE, "[{}] onWindowMoved for window: {}", PLUGIN_NAME, w->m_title);

    g_pCarouselManager->rebuildAll();
  } catch (const std::bad_any_cast &e) {
    Log::logger->log(Log::ERR, "[{}] onWindowMoved: Cast failed: {}", PLUGIN_NAME, e.what());
  }
}

static void onMonitorAdded() {
  ;
  // TODO: Add monitor row
}

// STUPID FOCUSTATE BUG!
static void onMonitorFocusChange(PHLMONITOR m) {
  MONITOR = m;
}

CFunctionHook *keyhookfn = nullptr;
typedef bool (*CKeybindManager_onKeyEvent)(void *self, std::any &event, SP<IKeyboard> pKeyboard);

static bool onKeyEvent(void *self, std::any event, SP<IKeyboard> pKeyboard) {
  if (!keyhookfn || !keyhookfn->m_original || unloadGuard)
    return true;

  auto e = std::any_cast<IKeyboard::SKeyEvent>(event);
  const auto MODS = g_pInputManager->getModsFromAllKBs();

  if (!g_pCarouselManager->active && e.state == WL_KEYBOARD_KEY_STATE_PRESSED) {
    if (e.keycode == 15 && (MODS & HL_MODIFIER_ALT)) {
      g_pCarouselManager->activate();
      return false;
    }
  }

  if (!g_pCarouselManager->active)
    return ((CKeybindManager_onKeyEvent)keyhookfn->m_original)(self, event, pKeyboard);

  const auto KEYSYM = xkb_state_key_get_one_sym(pKeyboard->m_xkbState, e.keycode + 8);

  if (e.state == WL_KEYBOARD_KEY_STATE_PRESSED) {
    switch (KEYSYM) {
    case XKB_KEY_Tab:
    case XKB_KEY_ISO_Left_Tab:
    case XKB_KEY_d:
    case XKB_KEY_Right:
      (MODS & HL_MODIFIER_SHIFT) ? g_pCarouselManager->prev() : g_pCarouselManager->next();
      break;

    case XKB_KEY_Down:
    case XKB_KEY_s:
      g_pCarouselManager->down();
      break;

    case XKB_KEY_a:
    case XKB_KEY_Left:
      g_pCarouselManager->prev();
      break;

    case XKB_KEY_w:
    case XKB_KEY_Up:
      g_pCarouselManager->up();
      break;

    case XKB_KEY_Return:
    case XKB_KEY_space:
      g_pCarouselManager->confirm();
      break;

    case XKB_KEY_Escape:
      g_pCarouselManager->deactivate();
      break;
    }
  } else {
    if (KEYSYM == XKB_KEY_Alt_L || KEYSYM == XKB_KEY_Alt_R || KEYSYM == XKB_KEY_Super_L) {
      g_pCarouselManager->confirm();
    }
  }

  return false;
}

static void onMouseClick(SCallbackInfo &i, std::any p) {
  if (!g_pCarouselManager->active)
    return;

  auto e = std::any_cast<IPointer::SButtonEvent>(p);
  if (e.state != WL_POINTER_BUTTON_STATE_PRESSED)
    return;

  i.cancelled = true;
  Vector2D mouseCoords = g_pInputManager->getMouseCoordsInternal();
  mouseCoords *= MONITOR->m_scale;

  auto list = g_pCarouselManager->getRenderList();
  for (auto *el : list) {
    if (el->shouldBeRemoved())
      continue;

    if (el->onMouseClick(mouseCoords)) {
      break;
    }
  }
}

static void onMouseMove(SCallbackInfo &i, std::any p) {
  if (!g_pCarouselManager->active)
    return;
  Vector2D mousePos = g_pInputManager->getMouseCoordsInternal();

  auto list = g_pCarouselManager->getRenderList();
  for (auto *el : list) {
    el->onMouseMove(mousePos);
  }
};

// Straight from ConfigManager.cpp. THANKS GUYS!
inline Hyprlang::CParseResult configHandleGradientSet(const char *VALUE, void **data) {
  if (unloadGuard)
    return {};
  std::string V = VALUE;

  if (!*data)
    *data = new CGradientValueData();

  const auto DATA = sc<CGradientValueData *>(*data);

  CVarList2 varlist(std::string(V), 0, ' ');
  DATA->m_colors.clear();

  std::string parseError = "";

  for (auto const &var : varlist) {
    if (var.find("deg") != std::string::npos) {
      try {
        DATA->m_angle = std::stoi(std::string(var.substr(0, var.find("deg")))) * (PI / 180.0); // radians
      } catch (...) {
        Log::logger->log(Log::WARN, "Error parsing gradient {}", V);
        parseError = "Error parsing gradient " + V;
      }

      break;
    }

    if (DATA->m_colors.size() >= 10) {
      Log::logger->log(Log::WARN, "Error parsing gradient {}: max colors is 10.", V);
      parseError = "Error parsing gradient " + V + ": max colors is 10.";
      break;
    }

    try {
      const auto COL = configStringToInt(std::string(var));
      if (!COL)
        throw std::runtime_error(std::format("failed to parse {} as a color", var));
      DATA->m_colors.emplace_back(COL.value());
    } catch (std::exception &e) {
      Log::logger->log(Log::WARN, "Error parsing gradient {}", V);
      parseError = "Error parsing gradient " + V + ": " + e.what();
    }
  }

  if (DATA->m_colors.empty()) {
    Log::logger->log(Log::WARN, "Error parsing gradient {}", V);
    if (parseError.empty())
      parseError = "Error parsing gradient " + V + ": No colors?";

    DATA->m_colors.emplace_back(0); // transparent
  }

  DATA->updateColorsOk();

  Hyprlang::CParseResult result;
  if (!parseError.empty())
    result.setError(parseError.c_str());

  return result;
}

inline void configHandleGradientDestroy(void **data) {
  if (unloadGuard)
    return;
  if (*data)
    delete sc<CGradientValueData *>(*data);
}

static void onConfigReload() {
  Log::logger->log(Log::TRACE, "[{}] onConfigReload", PLUGIN_NAME);
  g_pCarouselManager->rebuildAll();
}

SP<HOOK_CALLBACK_FN> PRENDER = nullptr;
SP<HOOK_CALLBACK_FN> PMONITORADD = nullptr;
SP<HOOK_CALLBACK_FN> POPENWINDOW = nullptr;
SP<HOOK_CALLBACK_FN> PCLOSEWINDOW = nullptr;
SP<HOOK_CALLBACK_FN> PONWINDOWMOVED = nullptr;
SP<HOOK_CALLBACK_FN> PONRELOAD = nullptr;
SP<HOOK_CALLBACK_FN> PONMONITORFOCUSCHANGE = nullptr;
SP<HOOK_CALLBACK_FN> PONMOUSECLICK = nullptr;
SP<HOOK_CALLBACK_FN> PONMOUSEMOVE = nullptr;

template <typename T = void *>
void reg(HANDLE handle, SP<HOOK_CALLBACK_FN> &ptr, const std::string &event, auto &&func) {
  ptr = HyprlandAPI::registerCallbackDynamic(handle, event, [func, event](void *s, SCallbackInfo &i, std::any p) {
    if (unloadGuard || !g_pCarouselManager)
      return;

    if constexpr (std::is_same_v<T, void *>) {
      func();
    } else if constexpr (std::is_same_v<T, std::pair<SCallbackInfo &, std::any>>) {
      func(i, p);
    } else {
      try {
        func(std::any_cast<T>(p));
      } catch (const std::bad_any_cast &e) {
        Log::logger->log(Log::ERR, "[{}] Callback {} cast failed: {}", PLUGIN_NAME, event, e.what());
      }
    }
  });
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
  PHANDLE = handle;
  if (const std::string hash = __hyprland_api_get_hash(); hash != __hyprland_api_get_client_hash())
    throw std::runtime_error("Version mismatch");

  HyprlandAPI::addConfigValue(PHANDLE, "plugin:alttab:font_size", Hyprlang::INT{24});
  HyprlandAPI::addConfigValue(PHANDLE, "plugin:alttab:border_size", Hyprlang::INT{1});
  HyprlandAPI::addConfigValue(PHANDLE, "plugin:alttab:border_rounding", Hyprlang::INT{0});
  HyprlandAPI::addConfigValue(PHANDLE, "plugin:alttab:border_rounding_power", Hyprlang::FLOAT{2});
  HyprlandAPI::addConfigValue(PHANDLE, "plugin:alttab:border_active", Hyprlang::CConfigCustomValueType{&configHandleGradientSet, &configHandleGradientDestroy, "0xff00ccdd"});
  HyprlandAPI::addConfigValue(PHANDLE, "plugin:alttab:border_inactive", Hyprlang::CConfigCustomValueType{&configHandleGradientSet, &configHandleGradientDestroy, "0xaabbccddff"});
  HyprlandAPI::addConfigValue(PHANDLE, "plugin:alttab:window_spacing", Hyprlang::INT{10});
  HyprlandAPI::addConfigValue(PHANDLE, "plugin:alttab:window_size_inactive", Hyprlang::FLOAT{0.8});
  HyprlandAPI::addConfigValue(PHANDLE, "plugin:alttab:monitor_size", Hyprlang::FLOAT{0.3});
  HyprlandAPI::addConfigValue(PHANDLE, "plugin:alttab:animation_speed", Hyprlang::FLOAT{1.0});
  HyprlandAPI::addConfigValue(PHANDLE, "plugin:alttab:unfocused_alpha", Hyprlang::FLOAT{0.6});
  HyprlandAPI::addConfigValue(PHANDLE, "plugin:alttab:include_special", Hyprlang::INT{1});
  HyprlandAPI::addConfigValue(PHANDLE, "plugin:alttab:monitor_spacing", Hyprlang::INT{10});
  HyprlandAPI::addConfigValue(PHANDLE, "plugin:alttab:monitor_size_active", Hyprlang::FLOAT{0.4});
  HyprlandAPI::addConfigValue(PHANDLE, "plugin:alttab:monitor_size_inactive", Hyprlang::FLOAT{0.3});
  HyprlandAPI::addConfigValue(PHANDLE, "plugin:alttab:blur_enabled", Hyprlang::INT{1});
  HyprlandAPI::addConfigValue(PHANDLE, "plugin:alttab:unfocused_alpha", Hyprlang::FLOAT{0.6});
  HyprlandAPI::addConfigValue(PHANDLE, "plugin:alttab:dim_enabled", Hyprlang::INT{1});
  HyprlandAPI::addConfigValue(PHANDLE, "plugin:alttab:dim_amount", Hyprlang::FLOAT{0.15});

  g_pCarouselManager = makeUnique<CarouselManager>();

  reg(handle, PMONITORADD, "monitorAdded", []() { onMonitorAdded(); });
  reg(handle, PONRELOAD, "configReloaded", []() { onConfigReload(); });
  reg<eRenderStage>(handle, PRENDER, "render", [](auto p) { onRender(p); });
  reg<PHLWINDOW>(handle, POPENWINDOW, "openWindow", [](auto p) { onWindowCreated(p); });
  reg<PHLWINDOW>(handle, PCLOSEWINDOW, "closeWindow", [](auto p) { onWindowClosed(p); });
  reg<PHLMONITOR>(handle, PONMONITORFOCUSCHANGE, "focusedMon", [](auto p) { onMonitorFocusChange(p); });
  reg<std::vector<std::any>>(handle, PONWINDOWMOVED, "moveWindow", [](auto p) { onWindowMoved(p); });
  reg<std::pair<SCallbackInfo &, std::any>>(handle, PONMOUSECLICK, "mouseButton", [](auto &i, auto p) { onMouseClick(i, p); });
  reg<std::pair<SCallbackInfo &, std::any>>(handle, PONMOUSEMOVE, "mouseMove", [](auto &i, auto p) { onMouseMove(i, p); });

  HyprlandAPI::reloadConfig();

  MONITOR = Desktop::focusState()->monitor();

  try {
    auto keyhooklookup = HyprlandAPI::findFunctionsByName(PHANDLE, "onKeyEvent");
    if (keyhooklookup.size() != 1) {
      for (auto &f : keyhooklookup)
        Log::logger->log(Log::ERR, "onKeyEvent found at {} :: sig: {}, demangled: {}", f.address, f.signature, f.demangled);
      throw std::runtime_error("CKeybindManager::onKeyEvent not found");
    }
    Log::logger->log(Log::ERR, "onKeyEvent found at {} :: sig: {}, demangled: {}", keyhooklookup[0].address, keyhooklookup[0].signature,
                     keyhooklookup[0].demangled);
    keyhookfn = HyprlandAPI::createFunctionHook(PHANDLE, keyhooklookup[0].address, (void *)onKeyEvent);
    if (!keyhookfn->hook())
      throw std::runtime_error("Failed to hook CKeybindManager::onKeyEvent");
  } catch (const std::exception &e) {
    Log::logger->log(Log::ERR, "Failed to hook CKeybindManager::onKeyEvent: {}", e.what());
  }
  return {PLUGIN_NAME, PLUGIN_DESCRIPTION, PLUGIN_AUTHOR, PLUGIN_VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
  unloadGuard = true;
  g_pCarouselManager.reset();
}
