#include "../include/renderpasses.hpp"
#include <algorithm>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprlang.hpp>
#include <linux/input-event-codes.h>
#include <src/Compositor.hpp>
#include <src/config/ConfigDataValues.hpp>
#include <src/desktop/state/FocusState.hpp>
#include <src/devices/IKeyboard.hpp>
#include <src/managers/input/InputManager.hpp>
#include <src/render/Renderer.hpp>
#include <xkbcommon/xkbcommon-keysyms.h>

APICALL EXPORT std::string PLUGIN_API_VERSION() {
  return HYPRLAND_API_VERSION;
}

class CarouselManager {
public:
  bool active = false;
  struct Monitor {
    std::vector<UP<WindowContainer>> windows;
    size_t activeIndex = 0;
    int offset = 0;
  };
  std::map<int, Monitor> monitors;
  std::chrono::time_point<std::chrono::steady_clock> lastframe = std::chrono::steady_clock::now();
  size_t activeMonitorIndex = 0;

  void toggle() {
    active = !active;
    if (!active)
      deactivate();
  }

  void activate() {
    if (active)
      return;

    rebuildAll();
    if (monitors.empty())
      return;

    active = true;
    MONITOR = Desktop::focusState()->monitor();
    lastframe = std::chrono::steady_clock::now();

    auto focusedWindow = Desktop::focusState()->window();
    bool foundFocus = false;

    int mIdx = 0;
    for (auto &[id, mon] : monitors) {
      for (size_t wIdx = 0; wIdx < mon.windows.size(); ++wIdx) {
        if (mon.windows[wIdx]->window == focusedWindow) {
          activeMonitorIndex = mIdx;
          mon.activeIndex = wIdx;
          foundFocus = true;
          break;
        }
      }
      if (foundFocus)
        break;
      mIdx++;
    }

    refreshLayout(true);
  }

  void damageMonitors() {
    for (auto &mon : g_pCompositor->m_monitors) {
      if (!mon || !mon->m_enabled)
        continue;
      g_pHyprRenderer->damageMonitor(mon);
    }
  }

  void deactivate() {
    active = false;
    g_pHyprRenderer->m_renderPass.removeAllOfType("TabCarouselPassElement");
    g_pHyprRenderer->m_renderPass.removeAllOfType("TabCarouselBlurElement");
    damageMonitors();
  }

  void next(bool snap = false) {
    if (monitors.empty())
      return;

    auto it = std::next(monitors.begin(), activeMonitorIndex);
    auto &mon = it->second;

    if (mon.windows.empty())
      return;

    mon.activeIndex = (mon.activeIndex + 1) % mon.windows.size();
    refreshLayout(snap);
  }

  void prev(bool snap = false) {
    if (monitors.empty())
      return;

    auto it = std::next(monitors.begin(), activeMonitorIndex);
    auto &mon = it->second;

    if (mon.windows.empty())
      return;

    mon.activeIndex = (mon.activeIndex + mon.windows.size() - 1) % mon.windows.size();
    refreshLayout(snap);
  }

  void up(bool snap = false) {
    if (monitors.empty())
      return;
    activeMonitorIndex = (activeMonitorIndex - 1 + monitors.size()) % monitors.size();
    refreshLayout(snap);
  }

  void down(bool snap = false) {
    if (monitors.empty())
      return;
    activeMonitorIndex = (activeMonitorIndex + 1) % monitors.size();
    refreshLayout(snap);
  }

  void confirm() {
    if (!monitors.empty()) {
      auto window = monitors[activeMonitorIndex].windows[monitors[activeMonitorIndex].activeIndex]->window;
      // Fuck the stupid follow mouse behaviour. We force it.
      g_pInputManager->unconstrainMouse();
      window->m_relativeCursorCoordsOnLastWarp = g_pInputManager->getMouseCoordsInternal() - window->m_position;
      Desktop::focusState()->fullWindowFocus(window);
      if (window->m_monitor != MONITOR) {
        window->warpCursor();
        g_pInputManager->m_forcedFocus = window;
        g_pInputManager->simulateMouseMovement();
        g_pInputManager->m_forcedFocus.reset();
        Desktop::focusState()->rawMonitorFocus(MONITOR);
      }
    }
    deactivate();
  }

  static bool shouldIncludeWindow(PHLWINDOW w) {
    if (INCLUDE_SPECIAL)
      return true;
    return w->m_workspace && !w->m_workspace->m_isSpecialWorkspace;
  }

  void rebuildAll() {
    monitors.clear();

    for (auto &el : g_pCompositor->m_windows) {
      if (!el || !el->m_isMapped || !el->m_monitor)
        continue;

      if (shouldIncludeWindow(el)) {
        auto id = el->m_monitor->m_id;
        monitors[id].windows.emplace_back(makeUnique<WindowContainer>(el));
      }
    }
    refreshLayout();
  }

  void refreshLayout(bool snap = false) {
    if (!MONITOR || monitors.empty())
      return;

    const auto msize = (MONITOR->m_size * MONITOR->m_scale).round();
    const auto center = msize / 2.0;
    const auto spacing = BORDERSIZE + WINDOW_SPACING;

    const double activeRowH = msize.y * MONITOR_SIZE_ACTIVE;
    const double inactiveRowH = msize.y * MONITOR_SIZE_INACTIVE;

    const double verticalStep = ((activeRowH + (inactiveRowH * WINDOW_SIZE_INACTIVE)) / 2.0) + MONITOR_SPACING;

    int currentRow = 0;
    for (auto &[id, monData] : monitors) {
      auto activeList = monData.windows | std::views::transform([](auto &w) { return w.get(); }) | std::views::filter([](auto *w) { return !w->shouldBeRemoved(); }) | std::ranges::to<std::vector<WindowContainer *>>();

      if (activeList.empty()) {
        currentRow++;
        continue;
      }

      bool isRowSelected = (currentRow == (int)activeMonitorIndex);
      double rowBaseH = isRowSelected ? activeRowH : inactiveRowH;
      double rowCenterY = center.y + (currentRow - (int)activeMonitorIndex) * verticalStep;

      for (size_t j = 0; j < activeList.size(); ++j) {
        auto goal = activeList[j]->window->m_realSize->goal();
        double aspect = std::clamp(goal.x / std::max(goal.y, 1.0), 0.1, 5.0);

        double h = (j == monData.activeIndex && isRowSelected) ? rowBaseH : (rowBaseH * WINDOW_SIZE_INACTIVE);
        activeList[j]->animSize.set(Vector2D(h * aspect, h), snap);
      }

      auto activeWin = activeList[std::min(monData.activeIndex, activeList.size() - 1)];
      activeWin->animPos.set(Vector2D(center.x - (activeWin->animSize.target.x / 2.0), rowCenterY - (activeWin->animSize.target.y / 2.0)), snap);

      auto leftX = activeWin->animPos.target.x;
      for (auto *w : activeList | std::views::take(monData.activeIndex) | std::views::reverse) {
        leftX -= (w->animSize.target.x + spacing);
        w->animPos.set(Vector2D(leftX, rowCenterY - (w->animSize.target.y / 2.0)), snap);
      }

      auto rightX = activeWin->animPos.target.x + activeWin->animSize.target.x;
      for (auto *w : activeList | std::views::drop(monData.activeIndex + 1)) {
        w->animPos.set(Vector2D(rightX + spacing, rowCenterY - (w->animSize.target.y / 2.0)), snap);
        rightX += (w->animSize.target.x + spacing);
      }

      for (auto *w : activeList) {
        bool isFocused = (w == activeWin && isRowSelected);
        w->alpha.set(isFocused ? 1.0f : UNFOCUSEDALPHA, snap);
        w->border->isActive = isFocused;
      }

      currentRow++;
    }
  }

  bool isElementOnScreen(WindowContainer *w) {
    if (!MONITOR)
      return false;

    auto mBox = CBox{MONITOR->m_position, MONITOR->m_size}.scale(MONITOR->m_scale);
    auto wBox = CBox{w->pos, w->size};
    return mBox.intersection(wBox).width > 0;
  }

  void update() {
    if (!active || monitors.empty())
      return;

    auto now = std::chrono::steady_clock::now();
    double delta = std::chrono::duration_cast<std::chrono::microseconds>(now - lastframe).count() / 1000000.0;
    lastframe = now;

    std::vector<WindowContainer *> needsUpdate;
    size_t currentRowIdx = 0;
    bool anyWindowsLeft = false;

    for (auto it = monitors.begin(); it != monitors.end();) {
      auto &row = it->second;

      std::erase_if(row.windows, [](auto &el) { return el->shouldBeRemoved(); });

      if (row.windows.empty()) {
        it = monitors.erase(it);
        continue;
      }

      if (row.activeIndex >= row.windows.size())
        row.activeIndex = row.windows.empty() ? 0 : row.windows.size() - 1;

      bool isRowActive = (currentRowIdx == activeMonitorIndex);

      for (size_t i = 0; i < row.windows.size(); ++i) {
        auto &w = row.windows[i];
        w->update(delta);

        auto age = now - w->snapshot->lastUpdated;
        bool onScreen = isElementOnScreen(w.get());
        bool isWindowActive = (isRowActive && i == row.activeIndex);

        std::chrono::milliseconds threshold;
        if (isWindowActive) {
          threshold = std::chrono::milliseconds(30);
        } else if (onScreen) {
          threshold = std::chrono::milliseconds(200);
        } else {
          threshold = std::chrono::milliseconds(1000);
        }

        if (!w->snapshot->ready || age > threshold) {
          needsUpdate.push_back(w.get());
        }
      }

      anyWindowsLeft = true;
      currentRowIdx++;
      ++it;
    }

    if (!anyWindowsLeft) {
      deactivate();
      return;
    }

    if (activeMonitorIndex >= monitors.size())
      activeMonitorIndex = monitors.empty() ? 0 : monitors.size() - 1;

    std::sort(needsUpdate.begin(), needsUpdate.end(), [this](auto *a, auto *b) {
      bool aOn = isElementOnScreen(a);
      bool bOn = isElementOnScreen(b);
      if (aOn != bOn)
        return aOn;
      return a->snapshot->lastUpdated < b->snapshot->lastUpdated;
    });

    int processed = 0;
    const int FRAME_BUDGET = 3;
    for (auto *w : needsUpdate) {
      if (processed >= FRAME_BUDGET)
        break;
      w->snapshot->snapshot();
      processed++;
    }
  }

  std::vector<Element *> getRenderList() {
    std::vector<std::pair<int, Element *>> indexed;

    size_t rowIndex = 0;
    for (auto &[id, monData] : monitors) {
      int rowDist = std::abs(static_cast<int>(rowIndex) - static_cast<int>(activeMonitorIndex));

      for (size_t winIndex = 0; winIndex < monData.windows.size(); ++winIndex) {
        int winDist = std::abs(static_cast<int>(winIndex) - static_cast<int>(monData.activeIndex));

        int priority = (rowDist * 1000) + winDist;
        indexed.push_back({priority, monData.windows[winIndex].get()});
      }
      rowIndex++;
    }

    std::ranges::sort(indexed, std::greater<>{}, [](const auto &p) { return p.first; });

    std::vector<Element *> result;
    for (auto &p : indexed) {
      result.push_back(p.second);
    }

    return result;
  }
};

inline static UP<CarouselManager> g_pCarouselManager = makeUnique<CarouselManager>();

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

static void onWindowMoved(PHLWINDOW w) {
  if (!w->m_monitor)
    return;

  int id = w->m_monitor->m_id;
  auto &mon = g_pCarouselManager->monitors[id];

  for (auto &el : mon.windows) {
    if (el->window == w) {
      el->markForRemoval();
      break;
    }
  }

  mon.windows.emplace_back(makeUnique<WindowContainer>(w));
  g_pCarouselManager->refreshLayout();
}

static void onMonitorAdded() {
  ;
  // TODO: Add monitor row
}

// STUPID FOCUSTATE BUG!
static void onMonitorFocusChange(PHLMONITOR m) {
  MONITOR = m;
}

static CFunctionHook *keyhookfn = nullptr;
typedef bool (*CKeybindManager_onKeyEvent)(void *self, std::any &event, SP<IKeyboard> pKeyboard);

static bool onKeyEvent(void *self, std::any event, SP<IKeyboard> pKeyboard) {
  if (!keyhookfn || !keyhookfn->m_original)
    return true;

  auto e = std::any_cast<IKeyboard::SKeyEvent>(event);
  const auto MODS = g_pInputManager->getModsFromAllKBs();

  if (!g_pCarouselManager->active && e.state == WL_KEYBOARD_KEY_STATE_PRESSED) {
    if (e.keycode == 15 && (MODS & HL_MODIFIER_ALT)) {
      g_pCarouselManager->activate();
      g_pCarouselManager->next();
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

// Straight from ConfigManager.cpp. THANKS GUYS!
static Hyprlang::CParseResult configHandleGradientSet(const char *VALUE, void **data) {
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

static void configHandleGradientDestroy(void **data) {
  if (*data)
    delete sc<CGradientValueData *>(*data);
}

static void onConfigReload() {
  Log::logger->log(Log::TRACE, "[{}] onConfigReload", PLUGIN_NAME);
  FONTSIZE = std::any_cast<Hyprlang::INT>(HyprlandAPI::getConfigValue(PHANDLE, "plugin:alttab:font_size")->getValue());
  BORDERSIZE = std::any_cast<Hyprlang::INT>(HyprlandAPI::getConfigValue(PHANDLE, "plugin:alttab:border_size")->getValue());
  BORDERROUNDING = std::any_cast<Hyprlang::INT>(HyprlandAPI::getConfigValue(PHANDLE, "plugin:alttab:border_rounding")->getValue());
  BORDERROUNDINGPOWER = std::any_cast<Hyprlang::FLOAT>(HyprlandAPI::getConfigValue(PHANDLE, "plugin:alttab:border_rounding_power")->getValue());
  ACTIVEBORDERCOLOR = rc<CGradientValueData *>(std::any_cast<void *>(HyprlandAPI::getConfigValue(PHANDLE, "plugin:alttab:border_active")->getValue()));
  INACTIVEBORDERCOLOR = rc<CGradientValueData *>(std::any_cast<void *>(HyprlandAPI::getConfigValue(PHANDLE, "plugin:alttab:border_inactive")->getValue()));
  WINDOW_SPACING = std::any_cast<Hyprlang::INT>(HyprlandAPI::getConfigValue(PHANDLE, "plugin:alttab:window_spacing")->getValue());
  WINDOW_SIZE_INACTIVE = std::any_cast<Hyprlang::FLOAT>(HyprlandAPI::getConfigValue(PHANDLE, "plugin:alttab:window_size_inactive")->getValue());
  MONITOR_SPACING = std::any_cast<Hyprlang::INT>(HyprlandAPI::getConfigValue(PHANDLE, "plugin:alttab:monitor_spacing")->getValue());
  MONITOR_SIZE_ACTIVE = std::any_cast<Hyprlang::FLOAT>(HyprlandAPI::getConfigValue(PHANDLE, "plugin:alttab:monitor_size_active")->getValue());
  MONITOR_SIZE_INACTIVE = std::any_cast<Hyprlang::FLOAT>(HyprlandAPI::getConfigValue(PHANDLE, "plugin:alttab:monitor_size_inactive")->getValue());
  ANIMATIONSPEED = std::any_cast<Hyprlang::FLOAT>(HyprlandAPI::getConfigValue(PHANDLE, "plugin:alttab:animation_speed")->getValue());
  UNFOCUSEDALPHA = std::any_cast<Hyprlang::FLOAT>(HyprlandAPI::getConfigValue(PHANDLE, "plugin:alttab:unfocused_alpha")->getValue());
  INCLUDE_SPECIAL = std::any_cast<Hyprlang::INT>(HyprlandAPI::getConfigValue(PHANDLE, "plugin:alttab:include_special")->getValue()) != 0;
  g_pCarouselManager->rebuildAll();
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
  PHANDLE = handle;
  if (const std::string hash = __hyprland_api_get_hash(); hash != __hyprland_api_get_client_hash())
    throw std::runtime_error("Version mismatch");

  static auto PRENDER = HyprlandAPI::registerCallbackDynamic(handle, "render", [&](void *s, SCallbackInfo &i, std::any p) { onRender(std::any_cast<eRenderStage>(p)); });
  static auto PMONITORADD = HyprlandAPI::registerCallbackDynamic(handle, "monitorAdded", [&](void *s, SCallbackInfo &i, std::any p) { onMonitorAdded(); });
  static auto POPENWINDOW = HyprlandAPI::registerCallbackDynamic(handle, "openWindow", [&](void *s, SCallbackInfo &i, std::any p) { onWindowCreated(std::any_cast<PHLWINDOW>(p)); });
  static auto PCLOSEWINDOW = HyprlandAPI::registerCallbackDynamic(handle, "closeWindow", [&](void *s, SCallbackInfo &i, std::any p) { onWindowClosed(std::any_cast<PHLWINDOW>(p)); });
  static auto PONWINDOWMOVED = HyprlandAPI::registerCallbackDynamic(handle, "moveWindow", [&](void *s, SCallbackInfo &i, std::any p) { onWindowMoved(std::any_cast<PHLWINDOW>(p)); });
  static auto PONRELOAD = HyprlandAPI::registerCallbackDynamic(handle, "configReloaded", [&](void *s, SCallbackInfo &i, std::any p) { onConfigReload(); });
  static auto PONMONITORFOCUSCHANGE = HyprlandAPI::registerCallbackDynamic(handle, "focusedMon", [&](void *s, SCallbackInfo &i, std::any p) { onMonitorFocusChange(std::any_cast<PHLMONITOR>(p)); });

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

  HyprlandAPI::reloadConfig();
  onConfigReload();

  MONITOR = Desktop::focusState()->monitor();

  auto keyhooklookup = HyprlandAPI::findFunctionsByName(PHANDLE, "onKeyEvent");
  if (keyhooklookup.size() != 1) {
    for (auto &f : keyhooklookup)
      Log::logger->log(Log::ERR, "onKeyEvent found at {} :: sig: {}, demangled: {}", f.address, f.signature, f.demangled);
    throw std::runtime_error("CKeybindManager::onKeyEvent not found");
  }
  Log::logger->log(Log::ERR, "onKeyEvent found at {} :: sig: {}, demangled: {}", keyhooklookup[0].address, keyhooklookup[0].signature,
                   keyhooklookup[0].demangled);
  keyhookfn = HyprlandAPI::createFunctionHook(PHANDLE, keyhooklookup[0].address, (void *)onKeyEvent);
  auto success = keyhookfn->hook();
  if (!success)
    throw std::runtime_error("Failed to hook CKeybindManager::onKeyEvent");

  return {PLUGIN_NAME, PLUGIN_DESCRIPTION, PLUGIN_AUTHOR, PLUGIN_VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
  g_pCarouselManager.reset();
}
