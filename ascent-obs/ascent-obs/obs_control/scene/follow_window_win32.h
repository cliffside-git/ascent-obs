#ifndef ASCENTOBS_OBS_CONTROL_SCENE_FOLLOW_WINDOW_WIN32_H_
#define ASCENTOBS_OBS_CONTROL_SCENE_FOLLOW_WINDOW_WIN32_H_

// The thin Win32 layer of follow-window mode: reads the facts the pure guard
// (follow_window_guard.h) decides on. Every read runs in the per-monitor-v2
// DPI context, so rects are physical pixels like the duplicated texture.

#include <cstdint>
#include <string>

#include "obs_control/scene/follow_window_guard.h"

namespace follow_window {

// Facts read once per frame, before the duplicator acquires.
struct Probe {
  uint64_t target = 0;  // the window followed for this frame
  bool window_alive = false;
  bool adopted = false;  // switched to another window of the game's pid
  bool foreground_is_game = false;
  bool occluded = false;
  uint32_t occluder_pid = 0;
  std::string occluder_class;
  Rect client;   // the target's client rect, screen px
  Rect monitor;  // the recorded monitor, screen px
  uint32_t walked = 0;    // windows looked at above the game
  uint64_t walk_ns = 0;   // cost of the occlusion walk
  uint64_t total_ns = 0;  // cost of the whole probe
};

class Win32Facts {
public:
  Win32Facts(uint64_t hwnd, uint32_t monitor_handle);

  uint32_t game_pid() const { return game_pid_; }
  uint64_t target() const { return target_; }

  // rules 1, 2, 3 (monitor rect), 7, and the rect before acquire
  Probe ReadBeforeAcquire();

  // rule 4: the target's client rect now; empty when it can't be read
  Rect ReadClientRect(uint64_t hwnd) const;

private:
  uint64_t target_;
  uint32_t game_pid_;
  uint32_t monitor_handle_;
};

}  // namespace follow_window

#endif  // ASCENTOBS_OBS_CONTROL_SCENE_FOLLOW_WINDOW_WIN32_H_
