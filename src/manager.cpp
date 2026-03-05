#include "manager.hpp"
#include "defines.hpp"
#include <aquamarine/output/Output.hpp>
#include <chrono>
#include <hyprutils/math/Vector2D.hpp>
#include <src/Compositor.hpp>
#include <src/desktop/history/WindowHistoryTracker.hpp>
#include <src/desktop/state/FocusState.hpp>
#include <src/helpers/Color.hpp>
#include <src/helpers/Monitor.hpp>
#include <src/managers/eventLoop/EventLoopManager.hpp>
#include <src/plugins/PluginAPI.hpp>
#include <src/protocols/PresentationTime.hpp>
#include <src/render/pass/RectPassElement.hpp>
#include <src/render/pass/TexPassElement.hpp>
#define private public
#include <src/render/OpenGL.hpp>
#include <src/render/Renderer.hpp>
#undef private

#ifndef NDEBUG
static int counter = 0;
static int lastCounter = 0;
#endif

Manager::Manager() {
  LOG_SCOPE()
  listeners.config = HOOK_EVENT(config.reloaded, [this]() {
    onConfigReload();
  });
  listeners.windowCreated = HOOK_EVENT(window.open, [this](auto w) {
    onWindowCreated(w);
  });
  listeners.windowDestroyed = HOOK_EVENT(window.close, [this](auto w) {
    onWindowDestroyed(w);
  });
  listeners.render = HOOK_EVENT(render.stage, [this](auto s) {
    onRender(s);
  });
  listeners.focusChange = HOOK_EVENT(monitor.focused, [this](auto m) {
    onFocusChange(m);
  });
  listeners.monitorAdded = HOOK_EVENT(monitor.added, [this](auto m) {
    rebuild();
  });
  listeners.monitorRemoved = HOOK_EVENT(monitor.removed, [this](auto m) {
    rebuild();
  });

  lastFrame = lastUpdate = NOW;
}

void Manager::damageMonitors() {
  for (auto &[id, mon] : monitors) {
    g_pHyprRenderer->damageMonitor(mon->monitor);
  }
}

void Manager::activate() {
  LOG_SCOPE()
  active = true;
  graceTimer = makeShared<CEventLoopTimer>(std::chrono::milliseconds(GRACE), [this](SP<CEventLoopTimer> timer, void *data) { this->init(); }, nullptr);
  g_pEventLoopManager->addTimer(graceTimer);
  // g_pHyprRenderer->damageMonitor(Desktop::focusState()->monitor());
}

void Manager::init() {
  activeMonitor = Desktop::focusState()->monitor()->m_id;
  monitorFade.set(1.0f, false);
  rebuild();
  loopTimer = makeShared<CEventLoopTimer>(std::chrono::milliseconds(10), [this](SP<CEventLoopTimer> timer, void *data) {
    auto d = FloatTime(NOW - lastFrame).count();
    auto min = std::min(d, (monitors[activeMonitor]->monitor->m_refreshRate * 2) / 1000);
    update(min);
    lastFrame = NOW;
    loopTimer->updateTimeout(std::chrono::milliseconds(16)); }, nullptr);
  g_pEventLoopManager->addTimer(loopTimer);
}

void Manager::deactivate() {
  LOG_SCOPE()
  active = false;
  graceTimer->cancel();
  for (const auto &[id, mon] : monitors) {
    g_pHyprRenderer->damageMonitor(mon->monitor);
  }
  loopTimer.reset();
  graceTimer.reset();
  monitors.clear();
}

void Manager::toggle() {
  LOG_SCOPE()
  LOG(ERR, "toggle");
  active = !active;
  if (active)
    activate();
  else
    deactivate();
}

void Manager::confirm() {
  if (!monitors.contains(activeMonitor)) {
    const auto history = Desktop::History::windowTracker()->fullHistory();
    PHLWINDOWREF lastWindow;
    if (history.size() >= 2) {
      lastWindow = *(history | std::views::reverse | std::views::drop(1)).begin();
    } else {
      lastWindow = Desktop::focusState()->window();
    }
    Desktop::focusState()->fullWindowFocus(lastWindow.lock(), Desktop::FOCUS_REASON_KEYBIND);
    deactivate();
    return;
  }
  const auto &mon = monitors[activeMonitor];
  if (!mon || mon->windows.empty()) {
    deactivate();
    return;
  }
  auto selected = mon->windows[mon->activeWindow]->window;
  if (BRINGTOACTIVE)
    g_pKeybindManager->m_dispatchers["focusworkspaceoncurrentmonitor"](selected->m_workspace->m_name);
  Desktop::focusState()->fullWindowFocus(selected, Desktop::FOCUS_REASON_KEYBIND);
  deactivate();
}

void Manager::update(float delta) {
  LOG_SCOPE()
  const auto MONITOR = Desktop::focusState()->monitor();
  monitorFade.tick(delta, 0.4);
  monitorOffset.tick(delta, MONITORANIMATIONSPEED);
  for (const auto &[id, m] : monitors) {
    m->update(delta, id == activeMonitor);
    if (m->animating || !monitorOffset.done()) {
      // God i'm stupid sometimes. Ofc only damage the active monitor or animations will be fucked.
      g_pHyprRenderer->damageMonitor(MONITOR);
    }
  }
}

void Manager::up() {
  if (monitors.contains(activeMonitor))
    activeMonitor = (activeMonitor - 1 + monitors.size()) % monitors.size();
  monitorOffset.set(activeMonitor, false);
}

void Manager::down() {
  if (monitors.contains(activeMonitor))
    activeMonitor = (activeMonitor + 1) % monitors.size();
  monitorOffset.set(activeMonitor, false);
}

void Manager::next() {
  if (monitors.contains(activeMonitor))
    monitors[activeMonitor]->next();
}

void Manager::prev() {
  if (monitors.contains(activeMonitor))
    monitors[activeMonitor]->prev();
}

void Manager::draw(MONITORID monid, const CRegion &damage) {
  LOG_SCOPE()

  // probably not inited from grace yet.
  if (monitors.empty())
    return;
  const auto cur = Desktop::focusState()->monitor();
  auto dmg = damage;

  if (!POWERSAVE) {
    g_pHyprOpenGL->renderRect(dmg.getExtents(), CHyprColor(0.0, 0.0, 0.0, (DIMENABLED) ? DIMAMOUNT : 0), {.blur = sc<bool>(BLURBG)});
  } else {
    if (monitors.contains(monid))
      monitors[monid]->renderTexture(damage);
  }

  if (monid == cur->m_id) {
#ifndef NDEBUG
    Overlay->add(std::format("ActiveInternal: {}, ActiveInFocus: {}, monid: {}", activeMonitor, cur->m_name, monid));
#endif

    if (!SPLITMONITOR) {
      monitors[monid]->draw(damage, 0, true, monitorFade.current);
    } else {
      const auto spacing = cur->m_size.y * MONITORSPACING;
      int i = 0;
      for (const auto &[id, mon] : monitors) {
        if (id == activeMonitor) {
          i++;
          continue;
        }

        float offset = (i - monitorOffset.current) * spacing;
        mon->draw(damage, offset, false, monitorFade.current);
        i++;
      }
      int activeMon = 0;
      int counter = 0;
      for (const auto &[id, mon] : monitors) {
        if (id == activeMonitor) {
          activeMon = counter;
          break;
        }
        counter++;
      }
      float activeOffset = (activeMon - monitorOffset.current) * spacing;
      monitors[activeMonitor]->draw(damage, activeOffset, true, monitorFade.current);
#ifndef NDEBUG
      Overlay->add(std::format("monitor->m_size.x: {}, monitor->m_size.y: {}\nmonitor->m_pixelSize.x: {}, monitor->m_pixelSize.y: {}", monitors[activeMonitor]->monitor->m_size.x, monitors[activeMonitor]->monitor->m_size.y, monitors[activeMonitor]->monitor->m_size.x, monitors[activeMonitor]->monitor->m_size.y));
#endif
    }
  }

#ifndef NDEBUG
  Overlay->draw(cur);
#endif
}

void Manager::onConfigReload() {
  FONTSIZE = *CConfigValue<Hyprlang::INT>("plugin:alttab:font_size");
  BORDERSIZE = *CConfigValue<Hyprlang::INT>("plugin:alttab:border_size");
  BORDERROUNDING = *CConfigValue<Hyprlang::INT>("plugin:alttab:border_rounding");
  BORDERROUNDINGPOWER = *CConfigValue<Hyprlang::FLOAT>("plugin:alttab:border_rounding_power");

  ACTIVEBORDERCOLOR = rc<CGradientValueData *>(std::any_cast<void *>(HyprlandAPI::getConfigValue(PHANDLE, "plugin:alttab:border_active")->getValue()));
  INACTIVEBORDERCOLOR = rc<CGradientValueData *>(std::any_cast<void *>(HyprlandAPI::getConfigValue(PHANDLE, "plugin:alttab:border_inactive")->getValue()));

  DIMENABLED = *CConfigValue<Hyprlang::INT>("plugin:alttab:dim");
  DIMAMOUNT = *CConfigValue<Hyprlang::FLOAT>("plugin:alttab:dim_amount");
  BLURBG = *CConfigValue<Hyprlang::INT>("plugin:alttab:blur");
  UNFOCUSEDALPHA = *CConfigValue<Hyprlang::FLOAT>("plugin:alttab:unfocused_alpha");
  POWERSAVE = *CConfigValue<Hyprlang::INT>("plugin:alttab:powersave");
  ROTATIONSPEED = *CConfigValue<Hyprlang::FLOAT>("plugin:alttab:animation_speed");
  CAROUSELSIZE = *CConfigValue<Hyprlang::FLOAT>("plugin:alttab:carousel_size");
  WINDOWSIZE = *CConfigValue<Hyprlang::FLOAT>("plugin:alttab:window_size");
  WARP = *CConfigValue<Hyprlang::FLOAT>("plugin:alttab:warp");
  TILT = *CConfigValue<Hyprlang::FLOAT>("plugin:alttab:tilt");
  WINDOWSIZEACTIVE = *CConfigValue<Hyprlang::FLOAT>("plugin:alttab:window_size_active");
  WINDOWSIZEINACTIVE = *CConfigValue<Hyprlang::FLOAT>("plugin:alttab:window_size_inactive");
  MONITORSPACING = *CConfigValue<Hyprlang::FLOAT>("plugin:alttab:monitor_spacing");
  MONITORANIMATIONSPEED = *CConfigValue<Hyprlang::FLOAT>("plugin:alttab:monitor_animation_speed");
  SPLITMONITOR = *CConfigValue<Hyprlang::INT>("plugin:alttab:split_monitor");
  UNFOCUSEDALPHA = *CConfigValue<Hyprlang::FLOAT>("plugin:alttab:unfocused_alpha");
  POWERSAVE = *CConfigValue<Hyprlang::INT>("plugin:alttab:powersave");
  BRINGTOACTIVE = *CConfigValue<Hyprlang::INT>("plugin:alttab:bring_to_active");
  GRACE = *CConfigValue<Hyprlang::INT>("plugin:alttab:grace");
}

void Manager::onWindowCreated(PHLWINDOW window) {
  // TODO: add window to specific monitor
  rebuild();
}

void Manager::onWindowDestroyed(PHLWINDOW window) {
  if (!window)
    return;

  auto mon = window->m_monitor.lock();

  if (mon && monitors.contains(mon->m_id)) {
    if (monitors[mon->m_id]->removeWindow(window) == 0) {
      monitors.erase(mon->m_id);
    }
  } else {
    for (auto &[id, mon] : monitors) {
      if (mon->removeWindow(window) == 0) {
        monitors.erase(id);
        break;
      }
    }
  }
}

void Manager::onRender(eRenderStage stage) {
  if (!active)
    return;

  switch (stage) {
  case eRenderStage::RENDER_PRE: {
    ;
  } break;
  case eRenderStage::RENDER_LAST_MOMENT:
    g_pHyprRenderer->m_renderPass.add(makeUnique<RenderPass>());
    break;
  default:
    break;
  }
}

void Manager::onFocusChange(PHLMONITOR monitor) {
  if (monitor == nullptr)
    return;
  activeMonitor = monitor->m_id;
  monitorOffset.set(activeMonitor);
}

void Manager::rebuild() {
  LOG_SCOPE()

  const auto INCLUDESPECIAL = *CConfigValue<Hyprlang::INT>("plugin:alttab:include_special");

  for (const auto &m : g_pCompositor->m_monitors) {
    if (!m->m_enabled || m->m_isUnsafeFallback)
      continue;
    monitors[m->m_id] = makeUnique<Monitor>(m);
  }

  // auto activeWindow = Desktop::focusState()->window();
  PHLWINDOWREF activeWindow;

  const auto history = Desktop::History::windowTracker()->fullHistory();
  if (history.size() >= 2) {
    activeWindow = *(history | std::views::reverse | std::views::drop(1)).begin();
  } else {
    activeWindow = Desktop::focusState()->window();
  }
  activeMonitor = Desktop::focusState()->monitor()->m_id;
  monitorOffset.snap(activeMonitor);

  for (auto &[monID, mon] : monitors) {
    std::vector<PHLWINDOW> monitorWindows;

    for (auto it = history.rbegin(); it != history.rend(); ++it) {
      auto w = it->lock();
      if (!w)
        continue;

      if (!INCLUDESPECIAL && w->m_workspace && w->m_workspace->m_isSpecialWorkspace)
        continue;

      if (w->m_isMapped && (!SPLITMONITOR || w->m_monitor.lock() == mon->monitor)) {
        monitorWindows.emplace_back(w);
      }
    }
    // TODO: Find a better fallback in-case some window is in the history but not mapped..
    for (const auto &w : g_pCompositor->m_windows) {
      if (std::find(monitorWindows.begin(), monitorWindows.end(), w) == monitorWindows.end()) {
        if (w->m_isMapped && (!SPLITMONITOR || w->m_monitor.lock() == mon->monitor)) {
          monitorWindows.emplace_back(w);
        }
      }
    }

    for (const auto &w : monitorWindows) {
      auto card = mon->addWindow(w);
      if (w == activeWindow) {
        mon->activeWindow = mon->windows.size() - 1;
        const int count = monitorWindows.size();
        const float angle = (M_PI / 2.0f) + ((2.0f * M_PI * mon->activeWindow) / count);
        mon->rotation.snap(angle);
      }
    }
    g_pHyprRenderer->damageMonitor(mon->monitor);
    // damageMonitor should do this??
    // g_pCompositor->scheduleFrameForMonitor(mon->monitor);
  }
}

void RenderPass::draw(const CRegion &damage) {
  const auto MON = g_pHyprOpenGL->m_renderData.pMonitor;
  manager->draw(MON->m_id, damage);
}
