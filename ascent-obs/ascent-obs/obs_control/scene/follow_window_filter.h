#ifndef ASCENTOBS_OBS_CONTROL_SCENE_FOLLOW_WINDOW_FILTER_H_
#define ASCENTOBS_OBS_CONTROL_SCENE_FOLLOW_WINDOW_FILTER_H_

// Follow-window mode as an OBS filter on the monitor_capture source.
//
// The filter is attached before the source enters the scene and stays
// attached for the source's life: a later START retargets it in place, it is
// never removed (PROTOCOL rule 8). Per frame, nothing in the scene changes.
// libobs runs tick callbacks before it ticks any source (obs-video.c
// tick_sources), so the filter's tick callback reads the window facts before
// the duplicator's video_tick acquires the frame. The filter's render reads
// every fact again, asks the capture what image it holds, asks the pure
// guard, and draws either the duplicated monitor (cursor included) cropped
// 1:1 to the client rect and then scaled to fit the canvas, or the BRB fill.

#include <obs.h>
#include <cstdint>
#include <functional>
#include <string>

namespace follow_window {

// a private filter source following |hwnd| on |monitor_handle|; attach it
// with obs_source_filter_add before the source is shown, never remove it,
// release it with obs_source_release
obs_source_t* CreateFilter(uint64_t hwnd, uint32_t monitor_handle,
                           bool desktop_front);

// a later START names another window: the graphics thread switches to it at
// the next frame boundary (and settles)
void Retarget(obs_source_t* filter, uint64_t hwnd);

// called on the graphics thread once per change of what the filter draws
// (game or BRB, and why, and the masked regions): |reason| is a ReasonKey, a
// static string; |masks| is the event's "masks" JSON (MasksJson), empty when
// nothing is masked. Set it before the filter is attached; the receiver
// posts to the command thread.
typedef std::function<void(bool show_game, const char* reason,
                           const std::string& masks)>
    EventSink;
void SetEventSink(obs_source_t* filter, EventSink sink);

// the source was hidden or shown: the next decision is reported to the
// desktop even if its reason did not change
void ReportAgain(obs_source_t* filter);

// rule 6: the desktop's CMD_GAME_FOCUS_CHANGED state, ORed into the guard
void SetDesktopFront(obs_source_t* filter, bool front);

// the HMONITOR |filter|'s window facts read, as its 64 pointer bits (0 when
// |filter| is not a follow-window filter)
uint64_t MonitorBits(obs_source_t* filter);

// the name of the proc both monitor capture plugins answer with what their
// image is: bool valid, int present_ns, int x, int y, int width, int height
extern const char kFrameInfoProc[];

}  // namespace follow_window

#endif  // ASCENTOBS_OBS_CONTROL_SCENE_FOLLOW_WINDOW_FILTER_H_
