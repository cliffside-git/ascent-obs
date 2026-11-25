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
  (void)a; (void)b;
  return false;  // STUB
}

// |inner| is non-empty and lies fully inside |outer|.
inline bool Inside(const Rect& inner, const Rect& outer) {
  (void)inner; (void)outer;
  return true;  // STUB
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
  (void)game_pid; (void)game_client; (void)w;
  return false;  // STUB
}

// Index of the first occluder in |above| (nearest the game first or top
// first, the caller's order), or -1.
inline int FirstOccluder(uint32_t game_pid, const Rect& game_client,
                         const std::vector<WindowFacts>& above) {
  (void)game_pid; (void)game_client; (void)above;
  return -1;  // STUB
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
  r.target = f.target;  // STUB
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
  uint64_t now_ns = 0;
};

struct GuardDecision {
  bool show_game = false;
  Rect crop;  // monitor-relative physical px, valid when show_game
  Reason reason = Reason::kSettling;
};

// rule 5: BRB for this long after any input changes
const uint64_t kSettleNs = 100000000ull;  // 100 ms
// no decision for this long (the source wasn't rendered: hidden by the
// desktop's focus BRB) counts as a change, so a re-shown source settles
// first: the duplicator restarts and its first frames may be stale
const uint64_t kRenderGapNs = 50000000ull;  // 50 ms

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
  // STUB: today's behaviour, always the whole monitor
  (void)settle;
  GuardDecision d;
  d.show_game = true;
  d.reason = Reason::kGame;
  d.crop = in.monitor;
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
  (void)src_w; (void)src_h; (void)canvas_w; (void)canvas_h;
  return Placement();  // STUB
}

}  // namespace follow_window

#endif  // ASCENTOBS_OBS_CONTROL_SCENE_FOLLOW_WINDOW_GUARD_H_
