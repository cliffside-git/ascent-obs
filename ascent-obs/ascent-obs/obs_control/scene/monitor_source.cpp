#include "obs_control/scene/monitor_source.h"
#include "obs_control/scene/follow_window_filter.h"
#include "obs_control/settings.h"
#include <map>
#include <windows.h>

namespace {
  const char kSettingEnabled[] = "enable";
  const char kSettingForce[] = "force";
  const char kSettingMonitorHandle[] = "monitor_handle";
  const char kSettingCursor[] = "cursor";
}


HMONITOR GetMainDisplay() {
  // Get the handle to the desktop window
  HWND hwnd = GetDesktopWindow();

  // Get the HMONITOR for the primary display
  return MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
}

std::map<uint32_t, int> monitors;
static BOOL CALLBACK enum_monitor_props(HMONITOR handle, 
                                        HDC hdc, 
                                        LPRECT rect,
                                        LPARAM param) {
  UNUSED_PARAMETER(hdc);
  UNUSED_PARAMETER(rect);
  UNUSED_PARAMETER(param);
  uint32_t handle_uint32_t = (uint32_t)handle;
  monitors[handle_uint32_t] = (int)monitors.size();
  blog(LOG_INFO, "detect monitor [%d]: handle 0x%x", monitors.size() - 1,
       handle_uint32_t);
  return TRUE;
}

bool MonitorSource::IsEnabled(OBSData& monitor_settings) {
  if (monitor_settings == nullptr) {
    return false;
  }
  
  bool enabled = obs_data_get_bool(monitor_settings, kSettingEnabled);
  return enabled;
}

bool MonitorSource::Create(OBSData& data, obs_scene_t* scene, 
  bool visible /*= true*/) {

  //MessageBox(NULL, L"MonitorSource", L"MonitorSource", 0);

  if (data == nullptr) {
    return false;
  }

  if (scene == nullptr) {
    blog(LOG_ERROR, "Failed create monitor source : scene undefined ");
    return false;
  }

  if (!IsEnabled(data)) {
    blog(LOG_INFO, "monitor source disabled");
    return false;
  }
  
  force_ = obs_data_get_bool(data, kSettingForce);
  config_ = settings::ParseMonitorConfig(data);
  blog(LOG_INFO, "monitor capture method: %d brb_when_unfocused: %d",
       config_.method, config_.brb_when_unfocused);

  monitor_index_ = 0;
  monitor_handle_ =
      (uint32_t)settings::ParseHandle(data, kSettingMonitorHandle);

  if (monitors.empty()) {
    EnumDisplayMonitors(NULL, NULL, enum_monitor_props, NULL);
  }
 
  auto monitor_iter = monitor_handle_ ? 
    monitors.find(monitor_handle_) : monitors.end();

  if (monitor_iter != monitors.end()) {
    monitor_index_ = monitor_iter->second;
  } else {
    if (monitor_handle_ == NULL) {
      monitor_handle_ = (uint32_t)GetMainDisplay();
    }
    blog(LOG_WARNING,
         "invalid monitor handle 0x%x (int: 0x%x), using main screen.",
         (HMONITOR)monitor_handle_, monitor_handle_);
  }

  blog(LOG_INFO, "capture monitor index %d.", monitor_index_);
  if (!CreateMonitorsSource(scene)) {
    return false;
  }

  SetVisible(visible);
  return true;
}

MonitorSource::~MonitorSource() {
  // the guard stays attached to its source for the source's life (the
  // scene keeps the source); only this reference goes
  if (follow_filter_) {
    obs_source_release(follow_filter_);
    follow_filter_ = nullptr;
  }
}

settings::LaterStart MonitorSource::OnLaterStart(OBSData& data) {
  if (data == nullptr || !IsEnabled(data)) {
    return settings::LaterStart::kKeep;
  }
  settings::MonitorConfig requested = settings::ParseMonitorConfig(data);
  settings::LaterStart action =
      settings::ReconcileLaterStart(config_, follows_window(), requested);
  switch (action) {
    case settings::LaterStart::kRetarget:
      blog(LOG_INFO,
           "follow-window: START changes the window 0x%llx -> 0x%llx",
           (unsigned long long)config_.follow_window,
           (unsigned long long)requested.follow_window);
      config_.follow_window = requested.follow_window;
      ::follow_window::Retarget(follow_filter_, requested.follow_window);
      break;
    case settings::LaterStart::kRebuildGuarded:
      blog(LOG_INFO,
           "follow-window: START asks to follow 0x%llx on a source without "
           "the guard: rebuilding it guarded",
           (unsigned long long)requested.follow_window);
      break;
    case settings::LaterStart::kKeep:
      if (follows_window() &&
          requested.follow_window != config_.follow_window) {
        blog(LOG_WARNING,
             "follow-window: START without a usable follow_window (0x%llx "
             "method %d brb_when_unfocused %d): still following 0x%llx",
             (unsigned long long)requested.follow_window, requested.method,
             requested.brb_when_unfocused,
             (unsigned long long)config_.follow_window);
      }
      break;
  }
  return action;
}

void MonitorSource::SetDesktopFront(bool front) {
  desktop_front_ = front;
  ::follow_window::SetDesktopFront(follow_filter_, front);
}

void MonitorSource::Block() {
  blog(LOG_ERROR, "monitor source blocked: follow_window was asked for and "
                  "the guard could not be attached");
  blocked_ = true;
  SetVisible(false);
}

bool MonitorSource::CreateCompatibility(int monitor_id,
                                        uint32_t monitor_handle,
                                        obs_scene_t* scene,
                                        bool force,
                                        const settings::MonitorConfig& config) {
  force_ = force;
  monitor_handle_ = monitor_handle;
  monitor_index_ = monitor_id;
  // the replaced source's whole config: the same cursor, method and BRB
  // mode, and the same window, so the gate and a later START still see a
  // follow-window source and never fall back to the whole monitor
  config_ = config;
  return CreateMonitorsSource(scene, true);
}

bool MonitorSource::CreateMonitorsSource(obs_scene_t* scene,
                                        bool compatible /*= false*/) {

  //MessageBox(NULL, L"MonitorSource", L"MonitorSource", 0);

  obs_data_t *moitor_settings = obs_data_create();
  settings::FillMonitorCaptureSettings(moitor_settings, monitor_handle_,
                                       monitor_index_, config_.cursor,
                                       config_.method);

  obs_source_t* source = obs_source_create(
      !compatible ?"monitor_capture" :"monitor_capture_low",
      "monitor capture", moitor_settings, nullptr);
  obs_data_release(moitor_settings);

  // follow-window mode: the guard is attached before the source enters the
  // scene, so no frame of it is ever drawn unguarded. No guard, no source
  // (PROTOCOL rule 8)
  if (settings::FollowWindowActive(config_)) {
    obs_source_t* filter =
        source ? ::follow_window::CreateFilter(config_.follow_window,
                                               monitor_handle_,
                                               desktop_front_)
               : nullptr;
    if (filter) {
      ::follow_window::SetEventSink(filter, follow_event_sink_);
      obs_source_filter_add(source, filter);
    }
    if (!filter || obs_filter_get_parent(filter) != source) {
      blog(LOG_ERROR,
           "follow-window: the guard could not be attached (source %p "
           "filter %p): no monitor source",
           source, filter);
      if (filter) {
        obs_source_release(filter);
      }
      if (source) {
        obs_source_release(source);
      }
      return false;
    }
    follow_filter_ = filter;
  } else if (config_.follow_window != 0) {
    blog(LOG_WARNING,
         "follow_window 0x%llx ignored: needs method 1 (DXGI) and "
         "brb_when_unfocused (method %d brb_when_unfocused %d)",
         (unsigned long long)config_.follow_window, config_.method,
         config_.brb_when_unfocused);
  }

  source_.reset(new SourceContext(source));
  source_item_ = obs_scene_add(scene, source_->get_source());

  SetTransform(source_item_, OBS_BOUNDS_SCALE_INNER);

  blog(LOG_INFO, 
       "monitor source [index: %d handle:0x%x (%d) cursor:%d] added", 
       monitor_index_, monitor_handle_, monitor_handle_, config_.cursor);

  return true;
}

void MonitorSource::OnVisiblityChanged(bool visible) {
  // the filter reports again on its first decision once shown
  follow_state_known_ = false;
  ::follow_window::ReportAgain(follow_filter_);
  if (!source_.get() || !source_->get_source()) {
    __super::OnVisiblityChanged(visible);
    return;
  }

  if (visible && blocked_) {
    blog(LOG_WARNING, "monitor source blocked: stays hidden");
    return;
  }

  auto deskop_cursor_visible = config_.cursor && visible;
  obs_data_t *moitor_settings = obs_data_create();
  obs_data_set_int(moitor_settings, "monitor_handle", monitor_handle_);
  obs_data_set_int(moitor_settings, "monitor_index", monitor_index_);
  obs_data_set_bool(moitor_settings, "capture_cursor", deskop_cursor_visible);
  obs_source_update(source_->get_source(), moitor_settings);
  blog(LOG_INFO, "update desktop cursor visibility: %d", deskop_cursor_visible);
  __super::OnVisiblityChanged(visible);
}

void MonitorSource::OnSetVisibility() {

  SetTransform(source_item_, OBS_BOUNDS_SCALE_INNER);
  obs_sceneitem_set_order(source_item_, OBS_ORDER_MOVE_TOP);
}

