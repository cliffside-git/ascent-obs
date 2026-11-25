#ifndef ASCENTOBS_OBS_CONTROL_MONITOR_SOURCE_H_
#define ASCENTOBS_OBS_CONTROL_MONITOR_SOURCE_H_

#include <functional>

#include "obs_control/scene/source.h"
#include "obs_control/settings.h"

class MonitorSource : public Source {
public:
  static bool IsEnabled(OBSData& monitor_settings);

public:
  MonitorSource(Delegate* delegate) :
    Source(delegate),
    force_(false),
    monitor_index_(0),
    monitor_handle_(0),
    compatible_mode_(false){
  }

  virtual ~MonitorSource();

  const bool& force() { return force_; }
  const bool& compatible_mode() { return compatible_mode_; }
  const int& monitor_id() { return monitor_index_; }
  const uint32_t monitor_handle() { return monitor_handle_; }
  const bool& brb_when_unfocused() { return config_.brb_when_unfocused; }
  bool follows_window() const { return follow_filter_ != nullptr; }
  const settings::MonitorConfig& config() const { return config_; }
  bool desktop_front() const { return desktop_front_; }

public:
  virtual bool Create(OBSData& data, obs_scene_t* scene,
    bool visible = true);

  virtual const char* name() override {
    return "monitor";
  };

  // a later START on the running source (settings::ReconcileLaterStart):
  // retargets the guard in place or keeps it; kRebuildGuarded asks the
  // caller to replace this unguarded source with a guarded one
  settings::LaterStart OnLaterStart(OBSData& data);

  // rule 6 of follow-window mode: the desktop's focus state. Kept here too,
  // so a replacement source starts with it
  void SetDesktopFront(bool front);

  // follow-window reason changes for EVT_DISPLAY_SOURCE_CHANGED (graphics
  // thread); set before Create / CreateCompatibility
  void SetFollowEventSink(
      std::function<void(bool show_game, const char* reason)> sink) {
    follow_event_sink_ = std::move(sink);
  }

  // what the follow-window filter last reported to the desktop (command
  // thread); unknown until its first report after the source is shown
  void SetFollowState(bool show_game, const std::string& reason) {
    follow_state_known_ = true;
    follow_show_game_ = show_game;
    follow_reason_ = reason;
  }
  // false + "unknown" before the first report: the filter draws BRB then
  bool follow_show_game() const {
    return follow_state_known_ && follow_show_game_;
  }
  const char* follow_reason() const {
    return follow_state_known_ ? follow_reason_.c_str() : "unknown";
  }

  // never show this source again (a guard was asked for and could not be
  // attached to its replacement)
  void Block();

  // the black-texture fallback: the same monitor through GDI, built from
  // the replaced source's whole |config| (window, cursor, method, BRB mode)
  bool CreateCompatibility(int monitor_id,
                           uint32_t monitor_handle,
                           obs_scene_t* scene,
                           bool force,
                           const settings::MonitorConfig& config);

protected:
  virtual void OnVisiblityChanged(bool visible);
  virtual void OnSetVisibility();

private:
  bool CreateMonitorsSource(obs_scene_t* scene,
                           bool compatible = false);

private:
  settings::MonitorConfig config_;
  bool force_ = false;
  int monitor_index_ = 0;
  uint32_t monitor_handle_ = 0;
  bool compatible_mode_;
  bool desktop_front_ = true;
  bool blocked_ = false;
  // the follow-window guard: attached before the source is in the scene,
  // never removed (PROTOCOL rule 8)
  obs_source_t* follow_filter_ = nullptr;
  std::function<void(bool show_game, const char* reason)> follow_event_sink_;
  bool follow_state_known_ = false;
  bool follow_show_game_ = false;
  std::string follow_reason_;
};

#endif // ASCENTOBS_OBS_CONTROL_MONITOR_SOURCE_H_
