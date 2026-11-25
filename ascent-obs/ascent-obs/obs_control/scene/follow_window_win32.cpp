#include "obs_control/scene/follow_window_win32.h"

#include <windows.h>
#include <dwmapi.h>
#include <util/platform.h>

#pragma comment(lib, "dwmapi.lib")

namespace follow_window {

namespace {

// same cap as the desktop's walk (OCCLUSION_WALK_LIMIT)
const uint32_t kWalkLimit = 4096;

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

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

bool FrameRect(HWND hwnd, Rect* r) {
  RECT bounds;
  if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                      &bounds, sizeof(bounds))) ||
      GetWindowRect(hwnd, &bounds)) {
    r->left = bounds.left;
    r->top = bounds.top;
    r->right = bounds.right;
    r->bottom = bounds.bottom;
    return true;
  }
  return false;
}

enum class ReadResult { kSkip, kFacts, kFailed };

// Facts for one window above the game. Cheap facts first: a window that is
// invisible or doesn't overlap is not an occluder whatever the rest says, so
// the rest is read only for windows that could be one. A visible window
// whose rect can't be read is kFailed (unknown), unless it is gone.
ReadResult ReadFacts(HWND hwnd, const Rect& game_client, WindowFacts* f) {
  f->visible = IsWindowVisible(hwnd) != FALSE;
  if (!f->visible) {
    return ReadResult::kSkip;
  }
  if (!FrameRect(hwnd, &f->rect)) {
    return IsWindow(hwnd) ? ReadResult::kFailed : ReadResult::kSkip;
  }
  if (!Overlaps(f->rect, game_client)) {
    return ReadResult::kSkip;
  }
  f->pid = PidOf(hwnd);
  // failed reads below count as "draws" (not cloaked, not click-through,
  // not excluded), so a failure can only make a window an occluder
  f->cloaked = Cloaked(hwnd);
  f->minimized = IsIconic(hwnd) != FALSE;
  f->click_through =
      (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0;
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

Win32Facts::Win32Facts(uint64_t hwnd, uint32_t monitor_handle)
    : target_(hwnd), game_pid_(0), monitor_handle_(monitor_handle) {
  HWND h = (HWND)(uintptr_t)hwnd;
  if (IsWindow(h)) {
    game_pid_ = PidOf(h);
  }
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
  if (GetMonitorInfoW((HMONITOR)(uintptr_t)monitor_handle_, &mi)) {
    s.monitor.left = mi.rcMonitor.left;
    s.monitor.top = mi.rcMonitor.top;
    s.monitor.right = mi.rcMonitor.right;
    s.monitor.bottom = mi.rcMonitor.bottom;
  } else {
    s.complete = false;
    p.unknown_why = "monitor read failed";
    p.unknown_error = GetLastError();
  }

  if (s.window_alive) {
    HWND game = (HWND)(uintptr_t)target_;
    s.client = ClientRect(game);

    // rule 2: walk the windows above the game, nearest first. A walk that
    // doesn't reach the top (limit, a window gone mid-walk, a failed read)
    // is unknown, never "uncovered"
    uint64_t walk_start = os_gettime_ns();
    SetLastError(ERROR_SUCCESS);
    HWND above = GetWindow(game, GW_HWNDPREV);
    while (above) {
      if (++p.walked > kWalkLimit) {
        s.complete = false;
        p.unknown_why = "z-order walk over the limit";
        break;
      }
      WindowFacts f;
      ReadResult r = ReadFacts(above, s.client, &f);
      if (r == ReadResult::kFailed) {
        s.complete = false;
        p.unknown_why = "a window above could not be read";
        p.unknown_error = GetLastError();
        break;
      }
      if (r == ReadResult::kFacts && IsOccluder(game_pid_, s.client, f)) {
        s.occluded = true;
        p.occluder_pid = f.pid;
        p.occluder_class = ClassOf(above);
        break;
      }
      SetLastError(ERROR_SUCCESS);
      above = GetWindow(above, GW_HWNDPREV);
    }
    if (!above && s.complete && !s.occluded) {
      DWORD error = GetLastError();
      if (error != ERROR_SUCCESS) {
        s.complete = false;
        p.unknown_why = "z-order walk broke off";
        p.unknown_error = error;
      }
    }
    p.walk_ns = os_gettime_ns() - walk_start;
  }

  p.total_ns = os_gettime_ns() - start;
  return p;
}

}  // namespace follow_window
