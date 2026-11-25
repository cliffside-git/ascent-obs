#ifndef ASCENTOBS_OBS_CONTROL_MONITOR_SOURCE_H_
#define ASCENTOBS_OBS_CONTROL_MONITOR_SOURCE_H_

#include "obs_control/scene/source.h"

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
  const bool& brb_when_unfocused() { return brb_when_unfocused_; }
  bool follows_window() const { return follow_filter_ != nullptr; }

public:
  virtual bool Create(OBSData& data, obs_scene_t* scene,
    bool visible = true);

  virtual const char* name() override {
    return "monitor";
  };

  // a later START on the running source: switch, start or stop following
  // the window its payload names (follow_window)
  void UpdateFollowWindow(OBSData& data);

  // rule 6 of follow-window mode: the desktop's focus state
  void SetDesktopFront(bool front);

  // |follow_window|: the window the replaced source followed; the
  // compatibility source keeps following it (0: none)
  bool CreateCompatibility(int monitor_id,
                           uint32_t monitor_handle,
                           obs_scene_t* scene,
                           bool force,
                           uint64_t follow_window = 0);

  uint64_t follow_window() const {
    return follow_filter_ ? follow_window_ : 0;
  }

protected:
  virtual void OnVisiblityChanged(bool visible);
  virtual void OnSetVisibility();

private:
  bool CreateMonitorsSource(obs_scene_t* scene,
                           bool compatible = false);
  void ApplyFollowWindow(uint64_t follow_window);
  void RemoveFollowFilter();

private:
  bool cursor_ = false;
  bool force_ = false;
  int monitor_index_ = 0;
  uint32_t monitor_handle_ = 0;
  int method_ = 0;
  bool brb_when_unfocused_ = false;
  bool compatible_mode_;
  uint64_t follow_window_ = 0;
  obs_source_t* follow_filter_ = nullptr;
};

#endif // ASCENTOBS_OBS_CONTROL_MONITOR_SOURCE_H_