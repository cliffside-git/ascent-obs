// Follow-window mode: the 'follow_window' payload key and the per-frame
// guard (w10win PROTOCOL.md rules 1-7 and the settle), without OBS or Win32.
// Built and run against a local OBS build by scripts/run_tests.ps1.
#include <windows.h>
#include <obs.hpp>
#include <cstdio>
#include <cstring>
#include <string>

#include "obs_control/settings.h"
#include "obs_control/scene/follow_window_guard.h"

namespace {
int failures = 0;

#define CHECK(cond)                                          \
  do {                                                       \
    if (cond) {                                              \
      printf("  ok: %s\n", #cond);                           \
    } else {                                                 \
      printf("FAIL: %s (line %d)\n", #cond, __LINE__);       \
      failures++;                                            \
    }                                                        \
  } while (0)

using follow_window::Decide;
using follow_window::GuardDecision;
using follow_window::GuardInputs;
using follow_window::Reason;
using follow_window::Rect;
using follow_window::SettleState;
using follow_window::WindowFacts;

const uint64_t kMs = 1000000ull;

uint64_t Follow(const char* monitor_json) {
  OBSDataAutoRelease monitor = obs_data_create_from_json(monitor_json);
  return settings::ParseMonitorFollowWindow(monitor);
}

Rect R(int32_t l, int32_t t, int32_t r, int32_t b) {
  Rect x;
  x.left = l;
  x.top = t;
  x.right = r;
  x.bottom = b;
  return x;
}

// ---------------------------------------------------------------------------
// payload

void TestProtocolKey() {
  printf("protocol key\n");
  CHECK(strcmp(settings::kSettingsMonitorFollowWindow, "follow_window") == 0);
}

void TestParse() {
  printf("follow_window absent -> 0 (off)\n");
  CHECK(Follow(R"({"enable":true,"method":1,"brb_when_unfocused":true})") ==
        0);
  CHECK(settings::ParseMonitorFollowWindow(nullptr) == 0);

  printf("valid HWND integers pass through\n");
  CHECK(Follow(R"({"follow_window":133090})") == 133090);
  CHECK(Follow(R"({"follow_window":4294967295})") == 4294967295ull);

  printf("0 -> off\n");
  CHECK(Follow(R"({"follow_window":0})") == 0);

  printf("garbage -> off\n");
  CHECK(Follow(R"({"follow_window":"133090"})") == 0);
  CHECK(Follow(R"({"follow_window":"abc"})") == 0);
  CHECK(Follow(R"({"follow_window":1.5})") == 0);
  CHECK(Follow(R"({"follow_window":-5})") == 0);
  CHECK(Follow(R"({"follow_window":4294967296})") == 0);
  CHECK(Follow(R"({"follow_window":true})") == 0);
  CHECK(Follow(R"({"follow_window":{"hwnd":1}})") == 0);
  CHECK(Follow(R"({"follow_window":null})") == 0);
  printf("other spellings are ignored\n");
  CHECK(Follow(R"({"followWindow":133090})") == 0);
}

void TestActive() {
  printf("follow mode only with a window, DXGI (1) and brb_when_unfocused\n");
  CHECK(settings::FollowWindowActive(133090, 1, true));
  CHECK(!settings::FollowWindowActive(0, 1, true));
  CHECK(!settings::FollowWindowActive(133090, 0, true));
  CHECK(!settings::FollowWindowActive(133090, 2, true));
  CHECK(!settings::FollowWindowActive(133090, 1, false));
}

// Without follow_window the monitor source must hand OBS exactly what 1.3.1
// did: the same four keys, nothing added.
void TestNoFollowUnchanged() {
  printf("no follow_window: monitor_capture settings are 1.3.1's\n");
  const char* today =
      R"({"enable":true,"monitor_handle":65537,"method":1,)"
      R"("brb_when_unfocused":true,"cursor":false})";
  OBSDataAutoRelease monitor = obs_data_create_from_json(today);
  uint64_t follow = settings::ParseMonitorFollowWindow(monitor);
  CHECK(follow == 0);
  CHECK(!settings::FollowWindowActive(
      follow, settings::ParseMonitorCaptureMethod(monitor),
      settings::ParseMonitorBrbWhenUnfocused(monitor)));

  // what MonitorSource::CreateMonitorsSource built inline in 1.3.1
  OBSDataAutoRelease expected = obs_data_create();
  obs_data_set_int(expected, "monitor_handle", 65537);
  obs_data_set_int(expected, "monitor_index", 0);
  obs_data_set_bool(expected, "capture_cursor", false);
  obs_data_set_int(expected, "method", 1);

  OBSDataAutoRelease built = obs_data_create();
  settings::FillMonitorCaptureSettings(built, 65537, 0, false, 1);
  std::string a = obs_data_get_json(expected);
  std::string b = obs_data_get_json(built);
  printf("  expected %s\n  built    %s\n", a.c_str(), b.c_str());
  CHECK(a == b);

  printf("follow_window present: the same monitor_capture settings\n");
  OBSDataAutoRelease built_follow = obs_data_create();
  settings::FillMonitorCaptureSettings(built_follow, 65537, 0, false, 1);
  CHECK(std::string(obs_data_get_json(built_follow)) == a);
}

// ---------------------------------------------------------------------------
// guard

const Rect kMonitor = R(0, 0, 1920, 1080);
const Rect kGame = R(108, 111, 1474, 879);  // 1366x768 client

GuardInputs Good(uint64_t now) {
  GuardInputs in;
  in.window_alive = true;
  in.desktop_front = true;
  in.foreground_is_game = true;
  in.occluded = false;
  in.monitor = kMonitor;
  in.rect_before_acquire = kGame;
  in.rect_at_render = kGame;
  in.now_ns = now;
  return in;
}

// a settle state that has seen |in| steadily for a long time
SettleState Settled(const GuardInputs& in) {
  SettleState s;
  GuardInputs warm = in;
  for (uint64_t t = 0; t <= 400; t += 16) {
    warm.now_ns = in.now_ns - 400 * kMs + t * kMs;
    Decide(warm, s);
  }
  return s;
}

// renders frames every 10 ms (steady inputs) up to |until|; returns the
// decision of the frame at |until|
GuardDecision StepTo(GuardInputs& in, SettleState& s, uint64_t until) {
  GuardDecision d;
  while (in.now_ns + 10 * kMs < until) {
    in.now_ns += 10 * kMs;
    Decide(in, s);
  }
  in.now_ns = until;
  return Decide(in, s);
}

void TestSteadyGame() {
  printf("steady, in front, uncovered, on monitor -> game, crop = client\n");
  GuardInputs in = Good(10000 * kMs);
  SettleState s = Settled(in);
  in.now_ns += 16 * kMs;
  GuardDecision d = Decide(in, s);
  CHECK(d.show_game);
  CHECK(d.reason == Reason::kGame);
  CHECK(d.crop == R(108, 111, 1474, 879));

  printf("crop is monitor-relative (second monitor at x=1920)\n");
  GuardInputs in2 = Good(10000 * kMs);
  in2.monitor = R(1920, 0, 3840, 1080);
  in2.rect_before_acquire = in2.rect_at_render = R(2020, 100, 3300, 820);
  SettleState s2 = Settled(in2);
  in2.now_ns += 16 * kMs;
  GuardDecision d2 = Decide(in2, s2);
  CHECK(d2.show_game);
  CHECK(d2.crop == R(100, 100, 1380, 820));

  printf("borderless: crop == the whole monitor\n");
  GuardInputs in3 = Good(10000 * kMs);
  in3.rect_before_acquire = in3.rect_at_render = kMonitor;
  SettleState s3 = Settled(in3);
  in3.now_ns += 16 * kMs;
  GuardDecision d3 = Decide(in3, s3);
  CHECK(d3.show_game);
  CHECK(d3.crop == kMonitor);
}

// one rule broken on an otherwise settled, good frame
GuardDecision Broken(void (*breakit)(GuardInputs&)) {
  GuardInputs in = Good(10000 * kMs);
  SettleState s = Settled(in);
  in.now_ns += 16 * kMs;
  breakit(in);
  return Decide(in, s);
}

void TestEachRule() {
  printf("rule 1: another pid in front -> BRB\n");
  GuardDecision d = Broken([](GuardInputs& in) { in.foreground_is_game = false; });
  CHECK(!d.show_game && d.reason == Reason::kNotForeground);

  printf("rule 2: occluded -> BRB\n");
  d = Broken([](GuardInputs& in) { in.occluded = true; });
  CHECK(!d.show_game && d.reason == Reason::kOccluded);

  printf("rule 3: client rect not fully on the monitor -> BRB\n");
  d = Broken([](GuardInputs& in) {
    in.rect_before_acquire = in.rect_at_render = R(-10, 100, 1356, 868);
  });
  CHECK(!d.show_game && d.reason == Reason::kOffMonitor);
  d = Broken([](GuardInputs& in) {
    in.rect_before_acquire = in.rect_at_render = R(600, 400, 1966, 1168);
  });
  CHECK(!d.show_game && d.reason == Reason::kOffMonitor);
  printf("rule 3: an edge exactly on the monitor edge is on the monitor\n");
  GuardInputs edge = Good(10000 * kMs);
  edge.rect_before_acquire = edge.rect_at_render = R(554, 312, 1920, 1080);
  SettleState se = Settled(edge);
  edge.now_ns += 16 * kMs;
  CHECK(Decide(edge, se).show_game);
  printf("rule 3: minimized (empty rect at -32000) -> BRB\n");
  d = Broken([](GuardInputs& in) {
    in.rect_before_acquire = in.rect_at_render =
        R(-32000, -32000, -32000, -32000);
  });
  CHECK(!d.show_game && d.reason == Reason::kOffMonitor);

  printf("rule 4: rect at render != rect before acquire -> BRB\n");
  d = Broken([](GuardInputs& in) { in.rect_at_render = R(110, 111, 1476, 879); });
  CHECK(!d.show_game && d.reason == Reason::kRectMoved);
  d = Broken([](GuardInputs& in) { in.rect_at_render = R(108, 111, 1475, 879); });
  CHECK(!d.show_game && d.reason == Reason::kRectMoved);

  printf("rule 6: desktop focus says not in front -> BRB\n");
  d = Broken([](GuardInputs& in) { in.desktop_front = false; });
  CHECK(!d.show_game && d.reason == Reason::kDesktopUnfocused);

  printf("rule 7: window gone -> BRB\n");
  d = Broken([](GuardInputs& in) { in.window_alive = false; });
  CHECK(!d.show_game && d.reason == Reason::kWindowGone);

  printf("several rules: the reason is the first in rule order 7,6,1,2,3,4\n");
  d = Broken([](GuardInputs& in) {
    in.foreground_is_game = false;
    in.occluded = true;
  });
  CHECK(d.reason == Reason::kNotForeground);
  d = Broken([](GuardInputs& in) {
    in.occluded = true;
    in.rect_at_render = R(0, 0, 10, 10);
  });
  CHECK(d.reason == Reason::kOccluded);
}

// rule 5: BRB until 100 ms after the last change of any input
void SettleAfter(const char* what, void (*change)(GuardInputs&)) {
  printf("rule 5: settle after %s\n", what);
  GuardInputs in = Good(10000 * kMs);
  SettleState s = Settled(in);
  uint64_t t0 = in.now_ns + 16 * kMs;
  in.now_ns = t0;
  change(in);
  Decide(in, s);  // the change frame (may be BRB for its own rule)
  // the change is undone or holds; either way 100 ms of BRB follows it
  bool all_brb = true;
  for (uint64_t t = 16; t < 100; t += 16) {
    in.now_ns = t0 + t * kMs;
    GuardDecision d = Decide(in, s);
    if (d.show_game) {
      all_brb = false;
    }
  }
  CHECK(all_brb);
  in.now_ns = t0 + 100 * kMs;
  GuardDecision d = Decide(in, s);
  CHECK(d.show_game || d.reason != Reason::kSettling);
}

void TestSettle() {
  SettleAfter("a move (rect changes and stays)", [](GuardInputs& in) {
    in.rect_before_acquire = in.rect_at_render = R(200, 111, 1566, 879);
  });
  SettleAfter("a resize", [](GuardInputs& in) {
    in.rect_before_acquire = in.rect_at_render = R(108, 111, 1374, 829);
  });

  printf("rule 5: back in front after alt-tab -> 100 ms of BRB (P3 stale "
         "Notepad frame)\n");
  GuardInputs in = Good(10000 * kMs);
  SettleState s = Settled(in);
  uint64_t t = in.now_ns;
  in.foreground_is_game = false;
  for (int i = 0; i < 30; i++) {
    t += 16 * kMs;
    in.now_ns = t;
    if (Decide(in, s).show_game) {
      failures++;
      printf("FAIL: game shown while Notepad in front\n");
    }
  }
  in.foreground_is_game = true;
  uint64_t back = t + 16 * kMs;
  in.now_ns = back;
  GuardDecision d = Decide(in, s);
  CHECK(!d.show_game && d.reason == Reason::kSettling);
  d = StepTo(in, s, back + 33 * kMs);  // P3 saw the stale frame 2 ticks later
  CHECK(!d.show_game && d.reason == Reason::kSettling);
  CHECK(!StepTo(in, s, back + 99 * kMs).show_game);
  CHECK(StepTo(in, s, back + 100 * kMs).show_game);

  SettleAfter("an occluder appears", [](GuardInputs& in) { in.occluded = true; });
  SettleAfter("back on the monitor", [](GuardInputs& in) {
    in.rect_before_acquire = in.rect_at_render = R(-10, 100, 1356, 868);
  });
  SettleAfter("desktop focus back", [](GuardInputs& in) {
    in.desktop_front = false;
  });

  printf("rule 5: the change frame then steady inputs: game at +100 ms\n");
  GuardInputs m = Good(10000 * kMs);
  SettleState sm = Settled(m);
  m.now_ns += 16 * kMs;
  m.occluded = true;
  Decide(m, sm);
  uint64_t change = m.now_ns;
  m.occluded = false;  // cover leaves on the next frame: that is a change too
  m.now_ns = change + 16 * kMs;
  CHECK(!Decide(m, sm).show_game);
  CHECK(!StepTo(m, sm, change + 16 * kMs + 99 * kMs).show_game);
  CHECK(StepTo(m, sm, change + 16 * kMs + 100 * kMs).show_game);

  printf("rule 5: first frame after start settles\n");
  GuardInputs f = Good(5000 * kMs);
  SettleState sf;
  GuardDecision df = Decide(f, sf);
  CHECK(!df.show_game && df.reason == Reason::kSettling);
  f.now_ns += 50 * kMs;
  CHECK(!Decide(f, sf).show_game);
  f.now_ns += 50 * kMs;
  CHECK(Decide(f, sf).show_game);

  printf("rule 5: a long render gap (2 s) settles\n");
  GuardInputs g = Good(10000 * kMs);
  SettleState sg = Settled(g);
  g.now_ns += 2000 * kMs;  // not rendered for 2 s, inputs look the same
  GuardDecision dg = Decide(g, sg);
  CHECK(!dg.show_game && dg.reason == Reason::kSettling);
  g.now_ns += 16 * kMs;
  CHECK(!Decide(g, sg).show_game);
  printf("rule 5: a normal 33 ms frame gap (30 fps) does not settle\n");
  GuardInputs h = Good(10000 * kMs);
  SettleState sh = Settled(h);
  h.now_ns += 33 * kMs;
  CHECK(Decide(h, sh).show_game);
  printf("rule 5: render lag (VM r1 at 150%%: 57%% lagged frames) does not "
         "settle on its own\n");
  bool lag_ok = true;
  for (int i = 0; i < 20; i++) {
    h.now_ns += (i % 2 ? 66 : 133) * kMs;  // frames 66-133 ms apart
    if (!Decide(h, sh).show_game) {
      lag_ok = false;
    }
  }
  CHECK(lag_ok);
  printf("rule 5: the source was hidden since the last frame (desktop BRB "
         "shown and back) -> settle\n");
  GuardInputs r = Good(10000 * kMs);
  SettleState sr = Settled(r);
  r.now_ns += 33 * kMs;
  r.reshown = true;
  GuardDecision dr = Decide(r, sr);
  CHECK(!dr.show_game && dr.reason == Reason::kSettling);
  r.reshown = false;
  uint64_t reshown_at = r.now_ns;
  CHECK(!StepTo(r, sr, reshown_at + 99 * kMs).show_game);
  CHECK(StepTo(r, sr, reshown_at + 100 * kMs).show_game);
  printf("rule 5: a clock going backwards settles\n");
  GuardInputs b = Good(10000 * kMs);
  SettleState sb = Settled(b);
  b.now_ns -= 5 * kMs;
  CHECK(!Decide(b, sb).show_game);
}

// ---------------------------------------------------------------------------
// rule 2 facts: the desktop's first_occluder table

const uint32_t kGamePid = 42;

WindowFacts Window(uint32_t pid, Rect rect, const char* cls) {
  WindowFacts w;
  w.pid = pid;
  w.rect = rect;
  w.visible = true;
  w.class_name = cls;
  return w;
}

WindowFacts Pip() {
  return Window(7, R(1500, 780, 1900, 1005), "Chrome_WidgetWin_1");
}

void TestOccluder() {
  using follow_window::FirstOccluder;
  using follow_window::IsOccluder;
  const Rect game = kMonitor;

  printf("a window of another process over the game occludes it\n");
  CHECK(IsOccluder(kGamePid, game, Pip()));
  CHECK(FirstOccluder(kGamePid, game, {Pip()}) == 0);
  printf("nothing above -> none\n");
  CHECK(FirstOccluder(kGamePid, game, {}) == -1);

  printf("the first drawing window is named when several are above\n");
  WindowFacts hidden = Pip();
  hidden.visible = false;
  WindowFacts notepad = Window(9, R(100, 100, 900, 700), "Notepad");
  CHECK(FirstOccluder(kGamePid, game, {hidden, notepad, Pip()}) == 1);

  printf("windows that put no other pixels in the video are ignored\n");
  WindowFacts same_pid = Pip();
  same_pid.pid = kGamePid;
  CHECK(!IsOccluder(kGamePid, game, same_pid));
  WindowFacts overlay = Window(11, kMonitor, "CEF-OSC-WIDGET");
  overlay.click_through = true;
  CHECK(!IsOccluder(kGamePid, game, overlay));
  WindowFacts panel = Pip();
  panel.excluded_from_capture = true;
  CHECK(!IsOccluder(kGamePid, game, panel));
  WindowFacts cloaked = Window(12, kMonitor, "Windows.UI.Core.CoreWindow");
  cloaked.cloaked = true;
  CHECK(!IsOccluder(kGamePid, game, cloaked));
  WindowFacts minimized = Pip();
  minimized.minimized = true;
  CHECK(!IsOccluder(kGamePid, game, minimized));
  CHECK(!IsOccluder(kGamePid, game, hidden));
  CHECK(!IsOccluder(kGamePid, game, Window(13, R(500, 500, 500, 700), "Zero")));

  printf("a window beside the game, or touching its edge, is ignored\n");
  const Rect small_game = R(0, 0, 1280, 720);
  CHECK(!IsOccluder(kGamePid, small_game, Window(7, R(1300, 0, 1900, 500), "B")));
  CHECK(!IsOccluder(kGamePid, small_game, Window(7, R(1280, 0, 1900, 500), "B")));
  CHECK(IsOccluder(kGamePid, small_game, Window(7, R(1279, 0, 1900, 500), "B")));

  printf("the taskbar over the game's bottom edge counts\n");
  CHECK(IsOccluder(kGamePid, R(162, 92, 1698, 1031),
                   Window(3, R(0, 1020, 1920, 1080), "Shell_TrayWnd")));
}

// ---------------------------------------------------------------------------
// rule 7 facts

void TestResolveTarget() {
  using follow_window::ResolveTarget;
  using follow_window::TargetFacts;
  printf("rule 7: a live window of the game is kept\n");
  TargetFacts f;
  f.target = 0x1234;
  f.target_alive = true;
  f.game_pid = kGamePid;
  f.foreground = 0x9999;
  f.foreground_pid = 7;
  auto r = ResolveTarget(f);
  CHECK(r.target == 0x1234 && !r.adopted && !r.gone);

  printf("rule 7: gone, the foreground window is the game's pid -> adopt\n");
  f.target_alive = false;
  f.foreground_pid = kGamePid;
  r = ResolveTarget(f);
  CHECK(r.target == 0x9999 && r.adopted && !r.gone);

  printf("rule 7: gone, foreground is another pid -> BRB\n");
  f.foreground_pid = 7;
  r = ResolveTarget(f);
  CHECK(r.gone && r.target == 0);

  printf("rule 7: gone, no foreground window -> BRB\n");
  f.foreground = 0;
  f.foreground_pid = 0;
  r = ResolveTarget(f);
  CHECK(r.gone);

  printf("rule 7: the start HWND never was a window (pid 0) -> BRB\n");
  TargetFacts z;
  z.target = 0x1234;
  z.target_alive = false;
  z.game_pid = 0;
  z.foreground = 0x9999;
  z.foreground_pid = 0;
  CHECK(ResolveTarget(z).gone);
}

void TestFitInside() {
  using follow_window::FitInside;
  printf("crop placement: fit inside the canvas, centered\n");
  auto p = FitInside(1920, 1080, 1920, 1080);
  CHECK(p.scale == 1.f && p.x == 0.f && p.y == 0.f);
  p = FitInside(1366, 768, 1920, 1080);  // same aspect within rounding
  CHECK(p.scale > 1.40f && p.scale < 1.41f);
  p = FitInside(1024, 1024, 1920, 1080);  // square: pillarbox
  CHECK(p.scale > 1.054f && p.scale < 1.055f && p.y == 0.f && p.x > 419.f &&
        p.x < 421.f);
  p = FitInside(1920, 600, 1920, 1080);  // wide: letterbox
  CHECK(p.scale == 1.f && p.x == 0.f && p.y == 240.f);
}
}  // namespace

int main(int, char**) {
  TestProtocolKey();
  TestParse();
  TestActive();
  TestNoFollowUnchanged();
  TestSteadyGame();
  TestEachRule();
  TestSettle();
  TestOccluder();
  TestResolveTarget();
  TestFitInside();

  printf(failures ? "FAILED (%d)\n" : "ALL PASSED\n", failures);
  return failures ? 1 : 0;
}
