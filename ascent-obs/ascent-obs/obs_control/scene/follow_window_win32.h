#ifndef ASCENTOBS_OBS_CONTROL_SCENE_FOLLOW_WINDOW_WIN32_H_
#define ASCENTOBS_OBS_CONTROL_SCENE_FOLLOW_WINDOW_WIN32_H_

// The thin Win32 layer of follow-window mode: reads the facts the pure guard
// (follow_window_guard.h) decides on. Every read runs in the per-monitor-v2
// DPI context, so rects are physical pixels like the duplicated texture.

#include <cstdint>
#include <string>
#include <vector>

#include "obs_control/scene/follow_window_guard.h"

namespace follow_window {

// One complete read of the followed window's facts, plus what the log and
// the cost summary need.
struct Probe {
  Snapshot snap;
  bool adopted = false;  // switched to another window of the game's pid
  uint32_t occluder_pid = 0;
  std::string occluder_class;
  // every occluder seen, for the log: "pid N class C at x,y wxh"
  std::vector<std::string> occluders;
  const char* unknown_why = nullptr;  // set when !snap.complete
  uint32_t unknown_error = 0;         // GetLastError() for unknown_why
  uint32_t walked = 0;    // windows looked at above the game
  uint32_t listed = 0;      // windows in the normal z-order list
  uint32_t enumerated = 0;  // top-level windows from FindWindowExW
  uint32_t shell_band = 0;  // of those, outside the list (shell bands)
  uint32_t walks = 0;       // walks of the normal list (1 + retries)
  uint64_t walk_ns = 0;   // cost of the occlusion walk
  uint64_t total_ns = 0;  // cost of the whole probe
};

class Win32Facts {
public:
  Win32Facts(uint64_t hwnd, uint32_t monitor_handle);

  uint32_t game_pid() const { return game_pid_; }
  uint64_t target() const { return target_; }

  // rules 1, 2, 3, 7 and the client rect. |may_adopt|: the read before
  // acquire may switch to the game's foreground window when the target is
  // gone or draws nothing (rule 7); the read at render never switches, it
  // reports the target as gone instead.
  Probe Read(bool may_adopt);

  // the HMONITOR every read asks Windows about, as its 64 pointer bits
  uint64_t monitor_bits() const;

private:
  uint64_t target_;
  uint32_t game_pid_;
  uint32_t monitor_handle_;
};

}  // namespace follow_window

#endif  // ASCENTOBS_OBS_CONTROL_SCENE_FOLLOW_WINDOW_WIN32_H_
