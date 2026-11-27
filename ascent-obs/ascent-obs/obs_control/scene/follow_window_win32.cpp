#include "obs_control/scene/follow_window_win32.h"

#include <windows.h>
#include <dwmapi.h>
#include <vector>
#include <util/platform.h>

#pragma comment(lib, "dwmapi.lib")

namespace follow_window {

namespace {

// same cap as the desktop's walk (OCCLUSION_WALK_LIMIT)
const uint32_t kWalkLimit = 4096;
// cap on the whole top-level list and the FindWindowExW enumeration
const size_t kListLimit = 16384;
// more walks of the normal list while a window looks outside it
const int kListRetries = 2;

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

static_assert(kLwaAlpha == LWA_ALPHA, "follow_window_guard.h kLwaAlpha");

// SetThreadDpiAwarenessContext exists from Windows 10 1607; load it so the
// recorder still starts where it doesn't
typedef HANDLE(WINAPI* SetThreadDpiAwarenessContextFn)(HANDLE);

SetThreadDpiAwarenessContextFn DpiFn() {
  static SetThreadDpiAwarenessContextFn fn =
      (SetThreadDpiAwarenessContextFn)GetProcAddress(
          GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
  return fn;
}

// per-monitor-v2 for the scope, previous context restored after
class DpiScope {
public:
  DpiScope() {
    auto fn = DpiFn();
    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    previous_ = fn ? fn((HANDLE)(LONG_PTR)-4) : nullptr;
  }
  ~DpiScope() {
    auto fn = DpiFn();
    if (fn && previous_) {
      fn(previous_);
    }
  }

private:
  HANDLE previous_;
};

uint32_t PidOf(HWND hwnd) {
  DWORD pid = 0;
  if (hwnd) {
    GetWindowThreadProcessId(hwnd, &pid);
  }
  return pid;
}

bool Cloaked(HWND hwnd) {
  DWORD cloaked = 0;
  return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked,
                                         sizeof(cloaked))) &&
         cloaked != 0;
}

// the window's client area shows its own pixels: visible, not minimized,
// not cloaked (a cloaked window keeps its rect but DWM draws nothing)
bool Drawable(HWND hwnd) {
  return hwnd && IsWindowVisible(hwnd) && !IsIconic(hwnd) && !Cloaked(hwnd);
}

Rect ClientRect(HWND hwnd) {
  Rect r;
  RECT client;
  POINT origin = {0, 0};
  if (!hwnd || !GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &origin)) {
    return r;
  }
  r.left = origin.x;
  r.top = origin.y;
  r.right = origin.x + client.right;
  r.bottom = origin.y + client.bottom;
  return r;
}

// a shell-band window's mask rect: GetWindowRect, joined with the DWM frame
// bounds when they read. A toast's DWM bounds change during its slide-in
// while GetWindowRect covered every toast pixel (w10w-toast probe)
bool ShellBandRect(HWND hwnd, Rect* r) {
  RECT wr;
  if (!GetWindowRect(hwnd, &wr)) {
    return false;
  }
  r->left = wr.left;
  r->top = wr.top;
  r->right = wr.right;
  r->bottom = wr.bottom;
  RECT bounds;
  if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                      &bounds, sizeof(bounds))) &&
      bounds.right > bounds.left && bounds.bottom > bounds.top) {
    r->left = bounds.left < r->left ? bounds.left : r->left;
    r->top = bounds.top < r->top ? bounds.top : r->top;
    r->right = bounds.right > r->right ? bounds.right : r->right;
    r->bottom = bounds.bottom > r->bottom ? bounds.bottom : r->bottom;
  }
  return true;
}

// |*dwm|: the rect is DWM's extended frame bounds (else GetWindowRect's)
bool FrameRect(HWND hwnd, Rect* r, bool* dwm) {
  RECT bounds;
  *dwm = SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                         &bounds, sizeof(bounds)));
  if (*dwm || GetWindowRect(hwnd, &bounds)) {
    r->left = bounds.left;
    r->top = bounds.top;
    r->right = bounds.right;
    r->bottom = bounds.bottom;
    return true;
  }
  return false;
}

// GetDpiForMonitor (shcore, Windows 8.1+), loaded so the recorder still
// starts where it doesn't exist
typedef HRESULT(WINAPI* GetDpiForMonitorFn)(HMONITOR, int, UINT*, UINT*);

// the monitor's effective DPI; 0 when it can't be read
uint32_t MonitorDpi(HMONITOR monitor) {
  static GetDpiForMonitorFn fn = [] {
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    return shcore ? (GetDpiForMonitorFn)GetProcAddress(shcore,
                                                        "GetDpiForMonitor")
                  : nullptr;
  }();
  UINT x = 0;
  UINT y = 0;
  // MDT_EFFECTIVE_DPI = 0
  if (!fn || FAILED(fn(monitor, 0, &x, &y))) {
    return 0;
  }
  return x;
}

enum class ReadResult { kSkip, kFacts, kFailed };

// Facts for one window above the game. Cheap facts first: a window that is
// invisible or doesn't overlap is not an occluder whatever the rest says, so
// the rest is read only for windows that could be one. A visible window
// whose rect can't be read is kFailed (unknown), unless it is gone. |area|
// is the client rect grown by the mask margin (OcclusionArea). A
// |shell_band| window (outside the normal z-order list) is masked by
// ShellBandRect.
ReadResult ReadFacts(HWND hwnd, const Rect& area, bool shell_band,
                     const Rect& monitor, int32_t margin, WindowFacts* f) {
  f->visible = IsWindowVisible(hwnd) != FALSE;
  if (!f->visible) {
    return ReadResult::kSkip;
  }
  bool rect_ok = false;
  if (shell_band) {
    rect_ok = ShellBandRect(hwnd, &f->rect);
    f->dwm_bounds = rect_ok;
    if (rect_ok) {
      f->rect = SnapToMonitorEdges(f->rect, monitor, 2 * margin);
    }
  } else {
    rect_ok = FrameRect(hwnd, &f->rect, &f->dwm_bounds);
  }
  if (!rect_ok) {
    return IsWindow(hwnd) ? ReadResult::kFailed : ReadResult::kSkip;
  }
  if (!Overlaps(f->rect, area)) {
    return ReadResult::kSkip;
  }
  f->pid = PidOf(hwnd);
  // failed reads below count as "draws" (not cloaked, not click-through,
  // not excluded, not zero alpha), so a failure can only make a window an
  // occluder
  f->cloaked = Cloaked(hwnd);
  f->minimized = IsIconic(hwnd) != FALSE;
  const LONG ex_style = GetWindowLongW(hwnd, GWL_EXSTYLE);
  f->click_through = (ex_style & WS_EX_TRANSPARENT) != 0;
  if ((ex_style & WS_EX_LAYERED) != 0) {
    // fails for an UpdateLayeredWindow window: opacity unknown, it draws
    BYTE alpha = 255;
    DWORD flags = 0;
    const bool read_ok =
        GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &flags) != FALSE;
    f->zero_alpha = ZeroAlpha(read_ok, alpha, flags);
  }
  DWORD affinity = 0;
  f->excluded_from_capture = GetWindowDisplayAffinity(hwnd, &affinity) &&
                             affinity == WDA_EXCLUDEFROMCAPTURE;
  return ReadResult::kFacts;
}

std::string ClassOf(HWND hwnd) {
  char name[256] = {0};
  int n = GetClassNameA(hwnd, name, (int)sizeof(name));
  return std::string(name, n > 0 ? (size_t)n : 0);
}

}  // namespace

namespace {
// the handle Windows gave 64-bit code: sign-extended from its 32 bits
HMONITOR ToHmonitor(int64_t monitor_handle) {
  return (HMONITOR)(intptr_t)(int32_t)monitor_handle;
}
}  // namespace

Win32Facts::Win32Facts(uint64_t hwnd, int64_t monitor_handle)
    : target_(hwnd), game_pid_(0), monitor_handle_(monitor_handle) {
  HWND h = (HWND)(uintptr_t)hwnd;
  if (IsWindow(h)) {
    game_pid_ = PidOf(h);
  }
}

uint64_t Win32Facts::monitor_bits() const {
  return (uint64_t)(uintptr_t)ToHmonitor(monitor_handle_);
}

Probe Win32Facts::Read(bool may_adopt) {
  uint64_t start = os_gettime_ns();
  DpiScope dpi;
  Probe p;
  Snapshot& s = p.snap;
  s.complete = true;

  HWND foreground = GetForegroundWindow();
  uint32_t foreground_pid = PidOf(foreground);

  // rule 7
  TargetFacts tf;
  tf.target = target_;
  HWND current = (HWND)(uintptr_t)target_;
  tf.target_alive = target_ != 0 && game_pid_ != 0 && IsWindow(current) &&
                    PidOf(current) == game_pid_;
  tf.target_drawable = tf.target_alive && Drawable(current);
  tf.game_pid = game_pid_;
  tf.foreground = (uint64_t)(uintptr_t)foreground;
  tf.foreground_pid = foreground_pid;
  tf.foreground_drawable = Drawable(foreground);
  TargetResolution res = ResolveTarget(tf);
  if (res.adopted && may_adopt) {
    target_ = res.target;
    p.adopted = true;
  }
  s.target = target_;
  s.window_alive = !res.gone && res.target == target_;

  // rule 1
  s.foreground_is_game = foreground && game_pid_ != 0 &&
                         foreground_pid == game_pid_;

  // rule 3: the monitor
  MONITORINFO mi;
  mi.cbSize = sizeof(mi);
  HMONITOR monitor = ToHmonitor(monitor_handle_);
  if (GetMonitorInfoW(monitor, &mi)) {
    s.monitor.left = mi.rcMonitor.left;
    s.monitor.top = mi.rcMonitor.top;
    s.monitor.right = mi.rcMonitor.right;
    s.monitor.bottom = mi.rcMonitor.bottom;
  } else {
    s.complete = false;
    p.unknown_why = "monitor read failed";
    p.unknown_error = GetLastError();
  }
  // rule 2 masking: the margin for this monitor's DPI. Unknown DPI is an
  // unfinished read (BRB), like a failed monitor read
  s.margin = MarginPx(MonitorDpi(monitor));
  if (s.margin < 0 && s.complete) {
    s.complete = false;
    p.unknown_why = "monitor DPI read failed";
    p.unknown_error = GetLastError();
  }

  if (s.window_alive) {
    HWND game = (HWND)(uintptr_t)target_;
    s.client = ClientRect(game);

    // rule 2: walk the windows above the game, nearest first, and collect
    // every occluder (its DWM bounds are its mask). A walk that doesn't
    // reach the top (limit, a window gone mid-walk, a failed read) is
    // unknown, never "uncovered". Past kMaxMasks occluders the frame is
    // full BRB whatever lies above, so the walk stops there.
    const Rect area = OcclusionArea(s.client, s.margin);
    uint64_t walk_start = os_gettime_ns();
    // one window: false when the read failed (unknown) or the walk can stop
    // (past kMaxMasks occluders: full BRB whatever else is above)
    auto consider = [&](HWND hwnd, bool shell_band) {
      WindowFacts f;
      ReadResult r =
          ReadFacts(hwnd, area, shell_band, s.monitor, s.margin, &f);
      if (r == ReadResult::kFailed) {
        s.complete = false;
        p.unknown_why = shell_band ? "a shell-band window could not be read"
                                   : "a window above could not be read";
        p.unknown_error = GetLastError();
        return false;
      }
      if (r != ReadResult::kFacts || !IsOccluder(game_pid_, area, f)) {
        return true;
      }
      s.occluded = true;
      s.occluders.push_back(f.rect);
      if (!f.dwm_bounds) {
        s.occluder_bounds_unknown = true;
      }
      const std::string cls = ClassOf(hwnd);
      if (p.occluder_pid == 0) {
        p.occluder_pid = f.pid;
        p.occluder_class = cls;
      }
      char name[320];
      snprintf(name, sizeof(name), "pid %u class %s at %d,%d %dx%d%s%s",
               f.pid, cls.c_str(), f.rect.left, f.rect.top, f.rect.width(),
               f.rect.height(), shell_band ? " (shell band)" : "",
               f.dwm_bounds ? "" : " (no DWM bounds)");
      p.occluders.push_back(name);
      return s.occluders.size() <= kMaxMasks;
    };

    // rule 2, the normal band: the windows above the game, nearest first
    SetLastError(ERROR_SUCCESS);
    HWND above = GetWindow(game, GW_HWNDPREV);
    bool stopped = false;
    while (above) {
      if (++p.walked > kWalkLimit) {
        s.complete = false;
        p.unknown_why = "z-order walk over the limit";
        break;
      }
      if (!consider(above, false)) {
        stopped = true;
        break;
      }
      SetLastError(ERROR_SUCCESS);
      above = GetWindow(above, GW_HWNDPREV);
    }
    if (!above && s.complete) {
      DWORD error = GetLastError();
      if (error != ERROR_SUCCESS) {
        s.complete = false;
        p.unknown_why = "z-order walk broke off";
        p.unknown_error = error;
      }
    }

    // rule 2, shell bands (PROTOCOL 17:10): toasts, Start and Action Center
    // are top-level windows outside the GetTopWindow/GW_HWNDNEXT list (and
    // EnumWindows), in bands above every normal window. Every top-level
    // window FindWindowExW returns that the list doesn't hold counts as
    // above the game.
    // the normal list, top to bottom; false when it didn't finish (unknown)
    auto walk_list = [&](std::vector<uint64_t>* listed) {
      SetLastError(ERROR_SUCCESS);
      HWND w = GetTopWindow(nullptr);
      while (w) {
        if (listed->size() > kListLimit) {
          s.complete = false;
          p.unknown_why = "z-order list over the limit";
          return false;
        }
        listed->push_back((uint64_t)(uintptr_t)w);
        SetLastError(ERROR_SUCCESS);
        w = GetWindow(w, GW_HWNDNEXT);
      }
      DWORD error = GetLastError();
      if (error != ERROR_SUCCESS) {
        s.complete = false;
        p.unknown_why = "z-order list broke off";
        p.unknown_error = error;
        return false;
      }
      return true;
    };
    if (s.complete && !stopped) {
      std::vector<std::vector<uint64_t>> walks(1);
      walk_list(&walks[0]);
      p.listed = (uint32_t)walks[0].size();
      std::vector<uint64_t> enumerated;
      HWND prev = nullptr;
      while (s.complete) {
        SetLastError(ERROR_SUCCESS);
        HWND next = FindWindowExW(nullptr, prev, nullptr, nullptr);
        if (!next) {
          // the end leaves no error; a |prev| destroyed since the last call
          // gives ERROR_INVALID_WINDOW_HANDLE and stops short (Codex): the
          // windows after it were never seen, so unknown
          DWORD error = GetLastError();
          if (error != ERROR_SUCCESS) {
            s.complete = false;
            p.unknown_why = "top-level enumeration broke off";
            p.unknown_error = error;
          }
          break;
        }
        if (enumerated.size() > kListLimit) {
          s.complete = false;
          p.unknown_why = "top-level enumeration over the limit";
          break;
        }
        enumerated.push_back((uint64_t)(uintptr_t)next);
        prev = next;
      }
      p.enumerated = (uint32_t)enumerated.size();
      // a window reordered behind the game during a walk is missing from
      // that walk only. Before a window outside the list counts, walk the
      // list again (up to 2 more times) and keep only windows every walk
      // missed. Only windows that would cover the game (or can't be read)
      // need it, so most probes walk once.
      std::vector<uint64_t> covering;
      if (s.complete) {
        const std::vector<uint64_t> band =
            ShellBandWindows(enumerated, walks, (uint64_t)(uintptr_t)game);
        p.shell_band = (uint32_t)band.size();
        for (uint64_t b : band) {
          WindowFacts f;
          ReadResult r = ReadFacts((HWND)(uintptr_t)b, area, true, s.monitor,
                                   s.margin, &f);
          if (r == ReadResult::kFailed ||
              (r == ReadResult::kFacts && IsOccluder(game_pid_, area, f))) {
            covering.push_back(b);
          }
        }
      }
      for (int retry = 0;
           retry < kListRetries && s.complete && !covering.empty(); retry++) {
        walks.emplace_back();
        if (walk_list(&walks.back())) {
          covering =
              ShellBandWindows(covering, walks, (uint64_t)(uintptr_t)game);
        }
      }
      p.walks = (uint32_t)walks.size();
      if (s.complete) {
        for (uint64_t b : covering) {
          if (!consider((HWND)(uintptr_t)b, true)) {
            break;
          }
        }
      }
    }
    p.walk_ns = os_gettime_ns() - walk_start;
  }

  p.total_ns = os_gettime_ns() - start;
  return p;
}

}  // namespace follow_window
