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
// Rule 7: the followed window is gone or draws nothing.

struct TargetFacts {
  uint64_t target = 0;          // the HWND we follow now
  bool target_alive = false;    // IsWindow(target) and its pid is game_pid
  // visible, not cloaked, not minimized: its client rect shows its pixels
  bool target_drawable = false;
  uint32_t game_pid = 0;        // pid of the HWND the START named
  uint64_t foreground = 0;      // GetForegroundWindow()
  uint32_t foreground_pid = 0;
  bool foreground_drawable = false;
};

struct TargetResolution {
  uint64_t target = 0;  // what to follow from now on (0: nothing)
  bool adopted = false; // switched to the foreground window of the game pid
  bool gone = false;    // nothing to follow: BRB
};

// Keep a live, drawable target. Otherwise (destroyed, hidden, cloaked,
// minimized) adopt the foreground window when it is a drawable window of the
// game's pid; else nothing to follow.
inline TargetResolution ResolveTarget(const TargetFacts& f) {
  TargetResolution r;
  if (f.target != 0 && f.target_alive && f.target_drawable) {
    r.target = f.target;
    return r;
  }
  if (f.game_pid != 0 && f.foreground != 0 &&
      f.foreground_pid == f.game_pid && f.foreground_drawable &&
      f.foreground != f.target) {
    r.target = f.foreground;
    r.adopted = true;
    return r;
  }
  r.gone = true;
  return r;
}

// ---------------------------------------------------------------------------
// The per-frame decision.

// Everything the guard reads about the followed window at one moment. It is
// read twice per frame: before the duplicator acquires and again at render.
struct Snapshot {
  // every read finished; false (a z-order walk cut short, a window that
  // vanished mid-read) means unknown, which is BRB
  bool complete = false;
  uint64_t target = 0;              // the window followed
  bool window_alive = false;        // rule 7, after ResolveTarget
  bool foreground_is_game = false;  // rule 1
  bool occluded = false;            // rule 2
  Rect client;                      // the target's client rect, screen px
  Rect monitor;                     // the recorded monitor, screen px

  bool operator==(const Snapshot& o) const {
    return complete == o.complete && target == o.target &&
           window_alive == o.window_alive &&
           foreground_is_game == o.foreground_is_game &&
           occluded == o.occluded && client == o.client &&
           monitor == o.monitor;
  }
  bool operator!=(const Snapshot& o) const { return !(*this == o); }
};

// What the capture holds right now (rule 8: frame provenance).
struct FrameFacts {
  bool valid = false;       // the capture holds an image
  // when the desktop image it holds was updated, os_gettime_ns clock; a
  // pointer-only update doesn't count. 0: never
  uint64_t present_ns = 0;
  Rect area;                // what the image covers, screen px
};

enum class Reason {
  kGame = 0,
  kUnknown,              // rule 8: a read didn't finish
  kWindowGone,           // rule 7
  kDesktopUnfocused,     // rule 6 (CMD_GAME_FOCUS_CHANGED)
  kNotForeground,        // rule 1
  kOccluded,             // rule 2
  kOffMonitor,           // rule 3
  kChangedSinceAcquire,  // rules 4 and 9
  kCaptureMismatch,      // rule 8: the image isn't the monitor
  kSettling,             // rule 5
  kStaleFrame,           // rule 8: no new desktop image since the change
};

inline const char* ReasonName(Reason r) {
  switch (r) {
    case Reason::kGame: return "game";
    case Reason::kUnknown: return "unknown (a window read did not finish)";
    case Reason::kWindowGone: return "window gone or not drawable";
    case Reason::kDesktopUnfocused: return "desktop says unfocused";
    case Reason::kNotForeground: return "not foreground";
    case Reason::kOccluded: return "occluded";
    case Reason::kOffMonitor: return "not fully on the monitor";
    case Reason::kChangedSinceAcquire: return "changed since acquire";
    case Reason::kSettling: return "settling";
    case Reason::kCaptureMismatch: return "capture does not match the monitor";
    case Reason::kStaleFrame: return "no new desktop frame since the change";
  }
  return "?";
}

// The reason as sent to the desktop in EVT_DISPLAY_SOURCE_CHANGED: a stable
// key, unlike ReasonName (log text)
inline const char* ReasonKey(Reason r) {
  switch (r) {
    case Reason::kGame: return "game";
    case Reason::kUnknown: return "unknown";
    case Reason::kWindowGone: return "window_gone";
    case Reason::kDesktopUnfocused: return "desktop_unfocused";
    case Reason::kNotForeground: return "not_foreground";
    case Reason::kOccluded: return "occluded";
    case Reason::kOffMonitor: return "off_monitor";
    case Reason::kChangedSinceAcquire: return "changed_since_acquire";
    case Reason::kSettling: return "settling";
    case Reason::kCaptureMismatch: return "capture_mismatch";
    case Reason::kStaleFrame: return "stale_frame";
  }
  return "unknown";
}

// Once per reason change, never per frame: Changed() is true for the first
// decision and whenever the reason differs from the last one.
class ReasonNotifier {
public:
  bool Changed(Reason r) {
    if (primed_ && r == last_) {
      return false;
    }
    primed_ = true;
    last_ = r;
    return true;
  }

private:
  bool primed_ = false;
  Reason last_ = Reason::kGame;
};

struct GuardInputs {
  Snapshot pre;     // read before the duplicator acquires this frame
  Snapshot render;  // read again at render
  bool desktop_front = false;  // rule 6
  // rule 5: the source was hidden (not showing) at some tick since the last
  // decision, e.g. the desktop's focus BRB was up: the duplicator restarted
  // and its first frames may be stale
  bool reshown = false;
  FrameFacts frame;  // rule 8
  uint64_t pre_ns = 0;  // when |pre| was read (before the acquire)
  uint64_t now_ns = 0;  // when |render| was read
};

struct GuardDecision {
  bool show_game = false;
  Rect crop;  // monitor-relative physical px, valid when show_game
  Reason reason = Reason::kSettling;
};

// rule 5: BRB for this long after any input changes
const uint64_t kSettleNs = 100000000ull;  // 100 ms
// rule 5, animated transitions: DWM animates a window that opens, closes,
// hides, minimizes or restores for ~200-300 ms, blending it with what lies
// under it (VM r1: an adopted window faded in over the desktop 116 ms after
// adoption; a closing cover leaves a fading ghost after it is gone). After a
// cover goes away, the target (re)appears or changes, or the source is shown
// again, BRB holds this long
const uint64_t kAnimatedSettleNs = 400000000ull;  // 400 ms
// no decision for this long counts as a change. A hidden source is caught
// by GuardInputs::reshown; this only backs it up, so it sits well above
// render lag (VM r1 at 150% had 57% lagged frames, gaps of 66-133 ms, and a
// 50 ms limit kept moves on BRB for 11 s)
const uint64_t kRenderGapNs = 1000000000ull;  // 1 s

struct SettleState {
  bool primed = false;
  Snapshot last;  // the render snapshot of the last decision
  bool desktop_front = false;
  bool frame_valid = false;
  Rect frame_area;
  uint64_t last_change_ns = 0;
  uint64_t quiet_until_ns = 0;  // BRB (settling) until then
  // rule 8: an image must be presented after this (the first read that saw
  // the current state: the pre-acquire read when it already saw it, else
  // the render read)
  uint64_t fresh_after_ns = 0;
  uint64_t last_seen_ns = 0;
};

// Show BRB when any rule holds, else the monitor cropped to the client rect.
// Updates |settle| (call once per rendered frame).
inline GuardDecision Decide(const GuardInputs& in, SettleState& settle) {
  const Snapshot& pre = in.pre;
  const Snapshot& now = in.render;
  const bool on_monitor =
      Inside(pre.client, pre.monitor) && Inside(now.client, now.monitor);

  // rule 9: a difference between the two reads of this frame is a change
  // too, so A at acquire, B at render, then A again settles (A->B->A)
  const bool changed =
      !settle.primed || pre != now || settle.last != pre ||
      settle.last != now || settle.desktop_front != in.desktop_front ||
      settle.frame_valid != in.frame.valid ||
      settle.frame_area != in.frame.area || in.reshown ||
      in.now_ns < settle.last_seen_ns ||
      in.now_ns - settle.last_seen_ns > kRenderGapNs;
  if (changed) {
    const Snapshot& was = settle.last;
    const bool animated =
        settle.primed &&
        ((was.occluded && (!pre.occluded || !now.occluded)) ||
         (!was.window_alive && (pre.window_alive || now.window_alive)) ||
         was.target != pre.target || was.target != now.target ||
         in.reshown);
    const uint64_t until =
        in.now_ns + (animated ? kAnimatedSettleNs : kSettleNs);
    const bool clock_back = in.now_ns < settle.last_seen_ns;
    if (clock_back || until > settle.quiet_until_ns) {
      settle.quiet_until_ns = until;
    }
    settle.last_change_ns = in.now_ns;
    // when both reads of this frame agree, the window state was already
    // what it is now at the pre-acquire read, so an image presented after
    // that read shows it (also on the first frame and after a re-show, when
    // the capture restarts). A change seen only at render counts from there.
    const bool seen_before_acquire =
        pre == now && in.pre_ns != 0 && in.pre_ns <= in.now_ns;
    settle.fresh_after_ns = seen_before_acquire ? in.pre_ns : in.now_ns;
  }
  settle.primed = true;
  settle.last = now;
  settle.desktop_front = in.desktop_front;
  settle.frame_valid = in.frame.valid;
  settle.frame_area = in.frame.area;
  settle.last_seen_ns = in.now_ns;

  GuardDecision d;
  if (!pre.complete || !now.complete) {
    d.reason = Reason::kUnknown;
  } else if (!pre.window_alive || !now.window_alive) {
    d.reason = Reason::kWindowGone;
  } else if (!in.desktop_front) {
    d.reason = Reason::kDesktopUnfocused;
  } else if (!pre.foreground_is_game || !now.foreground_is_game) {
    d.reason = Reason::kNotForeground;
  } else if (pre.occluded || now.occluded) {
    d.reason = Reason::kOccluded;
  } else if (!on_monitor) {
    d.reason = Reason::kOffMonitor;
  } else if (pre != now) {
    d.reason = Reason::kChangedSinceAcquire;
  } else if (!in.frame.valid || in.frame.area != now.monitor) {
    d.reason = Reason::kCaptureMismatch;
  } else if (in.now_ns < settle.quiet_until_ns) {
    d.reason = Reason::kSettling;
  } else if (in.frame.present_ns <= settle.fresh_after_ns) {
    d.reason = Reason::kStaleFrame;
  } else {
    d.show_game = true;
    d.reason = Reason::kGame;
    d.crop.left = now.client.left - now.monitor.left;
    d.crop.top = now.client.top - now.monitor.top;
    d.crop.right = now.client.right - now.monitor.left;
    d.crop.bottom = now.client.bottom - now.monitor.top;
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
