#ifndef ASCENTOBS_OBS_CONTROL_SCENE_FOLLOW_WINDOW_FILTER_H_
#define ASCENTOBS_OBS_CONTROL_SCENE_FOLLOW_WINDOW_FILTER_H_

// Follow-window mode as an OBS filter on the monitor_capture source.
//
// Per frame, nothing in the scene changes. libobs runs tick callbacks before
// it ticks any source (obs-video.c tick_sources), so the filter's tick
// callback reads the window facts before the duplicator's video_tick
// acquires the frame. The filter's render then reads the client rect again,
// asks the pure guard, and draws either the duplicated monitor cropped to the
// client rect (scaled to fit the canvas) or the BRB fill.

#include <obs.h>
#include <cstdint>

namespace follow_window {

// a private filter source following |hwnd| on |monitor_handle|; release
// with obs_source_release after obs_source_filter_remove
obs_source_t* CreateFilter(uint64_t hwnd, uint32_t monitor_handle);

// rule 6: the desktop's CMD_GAME_FOCUS_CHANGED state, ORed into the guard
void SetDesktopFront(obs_source_t* filter, bool front);

}  // namespace follow_window

#endif  // ASCENTOBS_OBS_CONTROL_SCENE_FOLLOW_WINDOW_FILTER_H_
