#ifndef ASCENTOBS_OBS_CONTROL_SCENE_FOLLOW_WINDOW_GUARD_H_
#define ASCENTOBS_OBS_CONTROL_SCENE_FOLLOW_WINDOW_GUARD_H_

// Follow-window mode for the monitor source (contract: w10win PROTOCOL.md).
//
// The recorder duplicates the whole monitor (DXGI) and, per rendered frame,
// either draws the part under the game window's client rect or a BRB fill.
// Everything here is pure: plain facts in, a decision out, so the unit tests
// reach it without OBS or Win32. follow_window_win32.* gathers the facts and
// follow_window_filter.* draws the decision.

#include <cstdint>
#include <string>
#include <vector>

namespace follow_window {

// Physical pixels, screen coordinates, right/bottom exclusive.
struct Rect {
  int32_t left = 0;
  int32_t top = 0;
  int32_t right = 0;
  int32_t bottom = 0;

  int32_t width() const { return right - left; }
  int32_t height() const { return bottom - top; }
  bool empty() const { return right <= left || bottom <= top; }
  bool operator==(const Rect& o) const {
    return left == o.left && top == o.top && right == o.right &&
           bottom == o.bottom;
  }
  bool operator!=(const Rect& o) const { return !(*this == o); }
};

// Whether two rects share at least one pixel. Touching edges don't; an empty
// rect never does.
inline bool Overlaps(const Rect& a, const Rect& b) {
  if (a.empty() || b.empty()) {
    return false;
  }
  int32_t l = a.left > b.left ? a.left : b.left;
  int32_t r = a.right < b.right ? a.right : b.right;
  int32_t t = a.top > b.top ? a.top : b.top;
  int32_t btm = a.bottom < b.bottom ? a.bottom : b.bottom;
  return l < r && t < btm;
}

// |inner| is non-empty and lies fully inside |outer|.
inline bool Inside(const Rect& inner, const Rect& outer) {
  return !inner.empty() && inner.left >= outer.left &&
         inner.top >= outer.top && inner.right <= outer.right &&
         inner.bottom <= outer.bottom;
}

// ---------------------------------------------------------------------------
// Rule 2: occlusion. Same rules as the desktop's `first_occluder`
// (recorder/occlusion.rs on kyle/win10-no-border).

// What Windows says about one top-level window above the game in z-order.
struct WindowFacts {
  uint32_t pid = 0;
  Rect rect;  // DWM extended frame bounds, physical px
  bool visible = false;
  bool cloaked = false;
  bool minimized = false;
  bool click_through = false;          // WS_EX_TRANSPARENT
  bool excluded_from_capture = false;  // WDA_EXCLUDEFROMCAPTURE
  std::string class_name;
};

// A window above the game puts another program's pixels over the game's
// client rect. Not occluders: the game's own pid, windows that draw nothing
// (invisible, cloaked, minimized, zero area), click-through overlays,
// windows excluded from capture, windows that don't overlap (touching an
// edge included). The taskbar is an ordinary window here: it counts.
inline bool IsOccluder(uint32_t game_pid, const Rect& game_client,
                       const WindowFacts& w) {
  return w.pid != game_pid && w.visible && !w.cloaked && !w.minimized &&
         !w.click_through && !w.excluded_from_capture &&
         Overlaps(w.rect, game_client);
}

// Index of the first occluder in |above| (nearest the game first or top
// first, the caller's order), or -1.
inline int FirstOccluder(uint32_t game_pid, const Rect& game_client,
                         const std::vector<WindowFacts>& above) {
  for (size_t i = 0; i < above.size(); i++) {
    if (IsOccluder(game_pid, game_client, above[i])) {
      return (int)i;
    }
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Rule 7: the followed window is gone.

struct TargetFacts {
  uint64_t target = 0;         // the HWND we follow now
  bool target_alive = false;   // IsWindow(target) and its pid is game_pid
  uint32_t game_pid = 0;       // pid of the HWND the START named
  uint64_t foreground = 0;     // GetForegroundWindow()
  uint32_t foreground_pid = 0;
};

struct TargetResolution {
  uint64_t target = 0;  // what to follow from now on (0: nothing)
  bool adopted = false; // switched to the foreground window of the game pid
  bool gone = false;    // nothing to follow: BRB
};

inline TargetResolution ResolveTarget(const TargetFacts& f) {
  TargetResolution r;
  if (f.target != 0 && f.target_alive) {
    r.target = f.target;
    return r;
  }
  if (f.game_pid != 0 && f.foreground != 0 &&
      f.foreground_pid == f.game_pid) {
    r.target = f.foreground;
    r.adopted = f.foreground != f.target;
    return r;
  }
  r.gone = true;
  return r;
}

// ---------------------------------------------------------------------------
// The per-frame decision.

enum class Reason {
  kGame = 0,
  kWindowGone,        // rule 7
  kDesktopUnfocused,  // rule 6 (CMD_GAME_FOCUS_CHANGED)
  kNotForeground,     // rule 1
  kOccluded,          // rule 2
  kOffMonitor,        // rule 3
  kRectMoved,         // rule 4
  kSettling,          // rule 5
};

inline const char* ReasonName(Reason r) {
  switch (r) {
    case Reason::kGame: return "game";
    case Reason::kWindowGone: return "window gone";
    case Reason::kDesktopUnfocused: return "desktop says unfocused";
    case Reason::kNotForeground: return "not foreground";
    case Reason::kOccluded: return "occluded";
    case Reason::kOffMonitor: return "not fully on the monitor";
    case Reason::kRectMoved: return "rect changed since acquire";
    case Reason::kSettling: return "settling";
  }
  return "?";
}

struct GuardInputs {
  bool window_alive = false;        // rule 7, after ResolveTarget
  bool desktop_front = false;       // rule 6
  bool foreground_is_game = false;  // rule 1
  bool occluded = false;            // rule 2
  Rect monitor;                     // the recorded monitor, screen px
  Rect rect_before_acquire;         // rule 4, read before the duplicator tick
  Rect rect_at_render;              // rule 4, read in the render
  // rule 5: the source was hidden (not showing) at some tick since the last
  // decision, e.g. the desktop's focus BRB was up: the duplicator restarted
  // and its first frames may be stale
  bool reshown = false;
  uint64_t now_ns = 0;
};

struct GuardDecision {
  bool show_game = false;
  Rect crop;  // monitor-relative physical px, valid when show_game
  Reason reason = Reason::kSettling;
};

// rule 5: BRB for this long after any input changes
const uint64_t kSettleNs = 100000000ull;  // 100 ms
// no decision for this long counts as a change. A hidden source is caught
// by GuardInputs::reshown; this only backs it up, so it sits well above
// render lag (VM r1 at 150% had 57% lagged frames, gaps of 66-133 ms, and a
// 50 ms limit kept moves on BRB for 11 s)
const uint64_t kRenderGapNs = 1000000000ull;  // 1 s

struct SettleState {
  bool primed = false;
  bool window_alive = false;
  bool desktop_front = false;
  bool foreground_is_game = false;
  bool occluded = false;
  bool on_monitor = false;
  Rect rect;
  uint64_t last_change_ns = 0;
  uint64_t last_seen_ns = 0;
};

// Show BRB when any rule holds, else the monitor cropped to the client rect.
// Updates |settle| (call once per rendered frame).
inline GuardDecision Decide(const GuardInputs& in, SettleState& settle) {
  const bool on_monitor = Inside(in.rect_before_acquire, in.monitor) &&
                          Inside(in.rect_at_render, in.monitor);

  const bool changed =
      !settle.primed || settle.window_alive != in.window_alive ||
      settle.desktop_front != in.desktop_front ||
      settle.foreground_is_game != in.foreground_is_game ||
      settle.occluded != in.occluded || settle.on_monitor != on_monitor ||
      settle.rect != in.rect_before_acquire ||
      in.reshown || in.now_ns < settle.last_seen_ns ||
      in.now_ns - settle.last_seen_ns > kRenderGapNs;
  if (changed) {
    settle.last_change_ns = in.now_ns;
  }
  settle.primed = true;
  settle.window_alive = in.window_alive;
  settle.desktop_front = in.desktop_front;
  settle.foreground_is_game = in.foreground_is_game;
  settle.occluded = in.occluded;
  settle.on_monitor = on_monitor;
  settle.rect = in.rect_before_acquire;
  settle.last_seen_ns = in.now_ns;

  GuardDecision d;
  if (!in.window_alive) {
    d.reason = Reason::kWindowGone;
  } else if (!in.desktop_front) {
    d.reason = Reason::kDesktopUnfocused;
  } else if (!in.foreground_is_game) {
    d.reason = Reason::kNotForeground;
  } else if (in.occluded) {
    d.reason = Reason::kOccluded;
  } else if (!on_monitor) {
    d.reason = Reason::kOffMonitor;
  } else if (in.rect_before_acquire != in.rect_at_render) {
    d.reason = Reason::kRectMoved;
  } else if (in.now_ns - settle.last_change_ns < kSettleNs) {
    d.reason = Reason::kSettling;
  } else {
    d.show_game = true;
    d.reason = Reason::kGame;
    d.crop.left = in.rect_at_render.left - in.monitor.left;
    d.crop.top = in.rect_at_render.top - in.monitor.top;
    d.crop.right = in.rect_at_render.right - in.monitor.left;
    d.crop.bottom = in.rect_at_render.bottom - in.monitor.top;
  }
  return d;
}

// Where the crop lands in the canvas: scaled to fit inside, centered, aspect
// kept (what OBS_BOUNDS_SCALE_INNER does for the whole monitor today).
struct Placement {
  float x = 0.f;
  float y = 0.f;
  float scale = 1.f;
};

inline Placement FitInside(int32_t src_w, int32_t src_h, uint32_t canvas_w,
                           uint32_t canvas_h) {
  Placement p;
  if (src_w <= 0 || src_h <= 0) {
    return p;
  }
  float sx = (float)canvas_w / (float)src_w;
  float sy = (float)canvas_h / (float)src_h;
  p.scale = sx < sy ? sx : sy;
  p.x = ((float)canvas_w - (float)src_w * p.scale) / 2.f;
  p.y = ((float)canvas_h - (float)src_h * p.scale) / 2.f;
  return p;
}

}  // namespace follow_window

#endif  // ASCENTOBS_OBS_CONTROL_SCENE_FOLLOW_WINDOW_GUARD_H_
