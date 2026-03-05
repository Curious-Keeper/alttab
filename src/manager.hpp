#pragma once
#include "monitor.hpp"
#include "styles.hpp"
#include <map>
#include <src/SharedDefs.hpp>
#include <src/helpers/time/Timer.hpp>
#include <src/managers/eventLoop/EventLoopTimer.hpp>
#include <src/render/Texture.hpp>
#include <src/render/pass/PassElement.hpp>

class Manager {
public:
  Manager();
  void activate();
  void init();
  void deactivate();
  void toggle();
  void confirm();
  void move(Direction dir);
  void update(float delta);
  void rebuild();
  void draw(MONITORID monid, const CRegion &damage);
  void damageMonitors();
  bool isActive() const;

protected:
  bool active = false;
  MONITORID activeMonitor = MONITOR_INVALID;

private:
  void onConfigReload();
  void onWindowCreated(PHLWINDOW window);
  void onWindowDestroyed(PHLWINDOW window);
  void onRender(eRenderStage stage);
  void onFocusChange(PHLMONITOR monitor);

  bool setLayout();

  struct {
    CHyprSignalListener config;
    CHyprSignalListener windowCreated;
    CHyprSignalListener windowDestroyed;
    CHyprSignalListener render;
    CHyprSignalListener focusChange;
    CHyprSignalListener monitorAdded;
    CHyprSignalListener monitorRemoved;
  } listeners;

  SP<CEventLoopTimer> loopTimer;
  SP<CEventLoopTimer> graceTimer;

  Timestamp lastFrame;
  std::map<MONITORID, UP<Monitor>> monitors;
  AnimatedValue<float> monitorOffset;
  AnimatedValue<float> monitorFade;
  Timestamp lastUpdate;
  SP<IStyle> layoutStyle;

  friend class Monitor;
};

inline UP<Manager> manager;

class RenderPass : public IPassElement {
public:
  virtual void draw(const CRegion &damage);
  virtual bool needsLiveBlur() { return false; }
  virtual bool needsPrecomputeBlur() { return false; }
  virtual const char *passName() { return "TabCarouselPassElement"; }
};
