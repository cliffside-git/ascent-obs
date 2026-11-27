// Follow-window mode: the 'follow_window' payload key, the later-START
// reconcile and the per-frame guard (w10win PROTOCOL.md rules 1-9 and the
// settle), without OBS or Win32.
// Built and run against a local OBS build by scripts/run_tests.ps1.
#include <windows.h>
#include <obs.hpp>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
using follow_window::FrameFacts;
using follow_window::GuardDecision;
using follow_window::GuardInputs;
using follow_window::Reason;
using follow_window::Rect;
using follow_window::SettleState;
using follow_window::Snapshot;
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
  printf("32-bit HWNDs keep Windows' sign extension\n");
  CHECK(Follow(R"({"follow_window":4294967295})") == 0xFFFFFFFFFFFFFFFFull);
  CHECK(Follow(R"({"follow_window":-5})") == 0xFFFFFFFFFFFFFFFBull);
  CHECK(Follow(R"({"follow_window":-2147483648})") == 0xFFFFFFFF80000000ull);

  printf("0 -> off\n");
  CHECK(Follow(R"({"follow_window":0})") == 0);

  printf("garbage -> off\n");
  CHECK(Follow(R"({"follow_window":"133090"})") == 0);
  CHECK(Follow(R"({"follow_window":"abc"})") == 0);
  CHECK(Follow(R"({"follow_window":1.5})") == 0);
  CHECK(Follow(R"({"follow_window":-2147483649})") == 0);
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
// the monitor config and a later START (Codex findings 1, 2, 6)

settings::MonitorConfig Config(const char* json) {
  OBSDataAutoRelease monitor = obs_data_create_from_json(json);
  return settings::ParseMonitorConfig(monitor);
}

void TestMonitorConfig() {
  printf("the config a fallback rebuilds from keeps follow, method, BRB and "
         "cursor\n");
  settings::MonitorConfig c = Config(
      R"({"enable":true,"method":1,"brb_when_unfocused":true,"cursor":true,)"
      R"("follow_window":133090})");
  CHECK(c.cursor && c.method == 1 && c.brb_when_unfocused &&
        c.follow_window == 133090);
  CHECK(settings::FollowWindowActive(c));
  settings::MonitorConfig none = settings::ParseMonitorConfig(nullptr);
  CHECK(!none.cursor && none.method == 0 && !none.brb_when_unfocused &&
        none.follow_window == 0);
}

void TestLaterStart() {
  using settings::LaterStart;
  using settings::ReconcileLaterStart;
  const settings::MonitorConfig a = Config(
      R"({"method":1,"brb_when_unfocused":true,"cursor":true,)"
      R"("follow_window":1000})");
  const settings::MonitorConfig b = Config(
      R"({"method":1,"brb_when_unfocused":true,"cursor":true,)"
      R"("follow_window":2000})");
  const settings::MonitorConfig plain =
      Config(R"({"method":1,"brb_when_unfocused":true,"cursor":true})");
  const settings::MonitorConfig gdi_b =
      Config(R"({"method":0,"brb_when_unfocused":true,"follow_window":2000})");

  printf("later START, guarded A -> B: the guard retargets in place\n");
  CHECK(ReconcileLaterStart(a, true, b) == LaterStart::kRetarget);
  printf("later START, guarded A -> A: nothing changes\n");
  CHECK(ReconcileLaterStart(a, true, a) == LaterStart::kKeep);
  printf("later START without follow_window: the guard stays on A\n");
  CHECK(ReconcileLaterStart(a, true, plain) == LaterStart::kKeep);
  printf("later START with a window but not DXGI: the guard stays on A\n");
  CHECK(ReconcileLaterStart(a, true, gdi_b) == LaterStart::kKeep);
  printf("later START asks to follow on an unguarded source: rebuild it "
         "guarded\n");
  CHECK(ReconcileLaterStart(plain, false, b) == LaterStart::kRebuildGuarded);
  printf("later START, unguarded, no usable window: nothing changes\n");
  CHECK(ReconcileLaterStart(plain, false, plain) == LaterStart::kKeep);
  CHECK(ReconcileLaterStart(plain, false, gdi_b) == LaterStart::kKeep);
}

// ---------------------------------------------------------------------------
// guard

const Rect kMonitor = R(0, 0, 1920, 1080);
const Rect kGame = R(108, 111, 1474, 879);  // 1366x768 client
const uint64_t kTarget = 0x1234;

Snapshot GoodSnap() {
  Snapshot s;
  s.complete = true;
  s.target = kTarget;
  s.window_alive = true;
  s.foreground_is_game = true;
  s.occluded = false;
  s.client = kGame;
  s.monitor = kMonitor;
  return s;
}

GuardInputs Good(uint64_t now) {
  GuardInputs in;
  in.pre = GoodSnap();
  in.render = GoodSnap();
  in.desktop_front = true;
  in.frame.valid = true;
  in.frame.area = kMonitor;
  in.frame.present_ns = now - kMs;
  in.now_ns = now;
  return in;
}

// the same change in both reads of a frame
void Both(GuardInputs& in, void (*change)(Snapshot&)) {
  change(in.pre);
  change(in.render);
}

// a live game: every frame holds a desktop image presented 1 ms before it.
// Turn off to freeze the image (a failed acquire, a stalled desktop).
bool live_frames = true;

GuardDecision Run(GuardInputs& in, SettleState& s) {
  if (live_frames) {
    in.frame.present_ns = in.now_ns - kMs;
  }
  return Decide(in, s);
}

// a settle state that has seen |in| steadily for a long time
SettleState Settled(const GuardInputs& in) {
  SettleState s;
  GuardInputs warm = in;
  for (uint64_t t = 0; t <= 800; t += 16) {
    warm.now_ns = in.now_ns - 800 * kMs + t * kMs;
    Run(warm, s);
  }
  return s;
}

// renders frames every 10 ms (steady inputs) up to |until|; returns the
// decision of the frame at |until|
GuardDecision StepTo(GuardInputs& in, SettleState& s, uint64_t until) {
  while (in.now_ns + 10 * kMs < until) {
    in.now_ns += 10 * kMs;
    Run(in, s);
  }
  in.now_ns = until;
  return Run(in, s);
}

void TestSteadyGame() {
  printf("steady, in front, uncovered, on monitor -> game, crop = client\n");
  GuardInputs in = Good(10000 * kMs);
  SettleState s = Settled(in);
  in.now_ns += 16 * kMs;
  GuardDecision d = Run(in, s);
  CHECK(d.show_game);
  CHECK(d.reason == Reason::kGame);
  CHECK(d.crop == R(108, 111, 1474, 879));

  printf("crop is monitor-relative (second monitor at x=1920)\n");
  GuardInputs in2 = Good(10000 * kMs);
  Both(in2, [](Snapshot& x) {
    x.monitor = R(1920, 0, 3840, 1080);
    x.client = R(2020, 100, 3300, 820);
  });
  in2.frame.area = R(1920, 0, 3840, 1080);
  SettleState s2 = Settled(in2);
  in2.now_ns += 16 * kMs;
  GuardDecision d2 = Run(in2, s2);
  CHECK(d2.show_game);
  CHECK(d2.crop == R(100, 100, 1380, 820));

  printf("borderless: crop == the whole monitor\n");
  GuardInputs in3 = Good(10000 * kMs);
  Both(in3, [](Snapshot& x) { x.client = kMonitor; });
  SettleState s3 = Settled(in3);
  in3.now_ns += 16 * kMs;
  GuardDecision d3 = Run(in3, s3);
  CHECK(d3.show_game);
  CHECK(d3.crop == kMonitor);
}

// one rule broken on an otherwise settled, good frame
GuardDecision Broken(void (*breakit)(GuardInputs&)) {
  GuardInputs in = Good(10000 * kMs);
  SettleState s = Settled(in);
  in.now_ns += 16 * kMs;
  in.frame.present_ns = in.now_ns - kMs;
  breakit(in);
  return Decide(in, s);
}

void TestEachRule() {
  printf("rule 1: another pid in front -> BRB\n");
  GuardDecision d = Broken([](GuardInputs& in) {
    Both(in, [](Snapshot& x) { x.foreground_is_game = false; });
  });
  CHECK(!d.show_game && d.reason == Reason::kNotForeground);

  printf("rule 2: occluded -> BRB\n");
  d = Broken([](GuardInputs& in) {
    Both(in, [](Snapshot& x) { x.occluded = true; });
  });
  CHECK(!d.show_game && d.reason == Reason::kOccluded);

  printf("rule 3: client rect not fully on the monitor -> BRB\n");
  d = Broken([](GuardInputs& in) {
    Both(in, [](Snapshot& x) { x.client = R(-10, 100, 1356, 868); });
  });
  CHECK(!d.show_game && d.reason == Reason::kOffMonitor);
  d = Broken([](GuardInputs& in) {
    Both(in, [](Snapshot& x) { x.client = R(600, 400, 1966, 1168); });
  });
  CHECK(!d.show_game && d.reason == Reason::kOffMonitor);
  printf("rule 3: an edge exactly on the monitor edge is on the monitor\n");
  GuardInputs edge = Good(10000 * kMs);
  Both(edge, [](Snapshot& x) { x.client = R(554, 312, 1920, 1080); });
  SettleState se = Settled(edge);
  edge.now_ns += 16 * kMs;
  CHECK(Run(edge, se).show_game);
  printf("rule 3: minimized (empty rect at -32000) -> BRB\n");
  d = Broken([](GuardInputs& in) {
    Both(in, [](Snapshot& x) {
      x.client = R(-32000, -32000, -32000, -32000);
    });
  });
  CHECK(!d.show_game && d.reason == Reason::kOffMonitor);

  printf("rule 4: rect at render != rect before acquire -> BRB\n");
  d = Broken([](GuardInputs& in) { in.render.client = R(110, 111, 1476, 879); });
  CHECK(!d.show_game && d.reason == Reason::kChangedSinceAcquire);
  d = Broken([](GuardInputs& in) { in.render.client = R(108, 111, 1475, 879); });
  CHECK(!d.show_game && d.reason == Reason::kChangedSinceAcquire);

  printf("rule 6: desktop focus says not in front -> BRB\n");
  d = Broken([](GuardInputs& in) { in.desktop_front = false; });
  CHECK(!d.show_game && d.reason == Reason::kDesktopUnfocused);

  printf("rule 7: window gone -> BRB\n");
  d = Broken([](GuardInputs& in) {
    Both(in, [](Snapshot& x) { x.window_alive = false; });
  });
  CHECK(!d.show_game && d.reason == Reason::kWindowGone);

  printf("several rules: the reason is the first in rule order 7,6,1,2,3,4\n");
  d = Broken([](GuardInputs& in) {
    Both(in, [](Snapshot& x) {
      x.foreground_is_game = false;
      x.occluded = true;
    });
  });
  CHECK(d.reason == Reason::kNotForeground);
  d = Broken([](GuardInputs& in) {
    Both(in, [](Snapshot& x) { x.occluded = true; });
    in.render.client = R(0, 0, 10, 10);
  });
  CHECK(d.reason == Reason::kOccluded);
}

// Codex 4 / rule 9: every fact is read again at render; a change seen only
// there vetoes the frame
void TestRenderRecheck() {
  printf("rule 9: an occluder seen only at render -> BRB\n");
  GuardDecision d =
      Broken([](GuardInputs& in) { in.render.occluded = true; });
  CHECK(!d.show_game && d.reason == Reason::kOccluded);

  printf("rule 9: another pid in front only at render -> BRB\n");
  d = Broken([](GuardInputs& in) { in.render.foreground_is_game = false; });
  CHECK(!d.show_game && d.reason == Reason::kNotForeground);

  printf("rule 9: the window is gone at render -> BRB\n");
  d = Broken([](GuardInputs& in) { in.render.window_alive = false; });
  CHECK(!d.show_game && d.reason == Reason::kWindowGone);

  printf("rule 9: a different target at render (HWND adopted or reused), same "
         "rect -> BRB\n");
  d = Broken([](GuardInputs& in) { in.render.target = 0x5678; });
  CHECK(!d.show_game && d.reason == Reason::kChangedSinceAcquire);

  printf("rule 9: the monitor changes at render (mode change) -> BRB\n");
  d = Broken([](GuardInputs& in) { in.render.monitor = R(0, 0, 1600, 900); });
  CHECK(!d.show_game);

  printf("rule 8: a z-order walk that did not finish is unknown -> BRB\n");
  d = Broken([](GuardInputs& in) { in.pre.complete = false; });
  CHECK(!d.show_game && d.reason == Reason::kUnknown);
  d = Broken([](GuardInputs& in) { in.render.complete = false; });
  CHECK(!d.show_game && d.reason == Reason::kUnknown);

  printf("rule 9: an occluder seen only at render restarts the settle\n");
  GuardInputs in = Good(10000 * kMs);
  SettleState s = Settled(in);
  in.now_ns += 16 * kMs;
  in.render.occluded = true;
  Run(in, s);
  uint64_t seen = in.now_ns;
  in.render.occluded = false;  // gone again by the next frame
  CHECK(!StepTo(in, s, seen + 16 * kMs).show_game);
  CHECK(!StepTo(in, s, seen + 99 * kMs).show_game);
  // the cover is gone from the next frame on: an animated transition
  CHECK(!StepTo(in, s, seen + 409 * kMs).show_game);
  CHECK(StepTo(in, s, seen + 420 * kMs).show_game);
}

// VM r1 (obs2): DWM animates a window that opens, closes, hides, minimizes
// or restores for ~200-300 ms, blending it with what is under it (the
// adopted window faded in over the desktop 116 ms after adoption). Those
// transitions settle 400 ms; moves and focus changes keep 100 ms.
void AnimatedSettle(const char* what, void (*before)(GuardInputs&),
                    void (*after)(GuardInputs&)) {
  printf("animated transition, 400 ms: %s\n", what);
  GuardInputs in = Good(10000 * kMs);
  before(in);
  SettleState s = Settled(in);
  in.now_ns += 16 * kMs;
  after(in);
  uint64_t t0 = in.now_ns;
  Run(in, s);
  CHECK(!StepTo(in, s, t0 + 150 * kMs).show_game);
  CHECK(!StepTo(in, s, t0 + 399 * kMs).show_game);
  CHECK(StepTo(in, s, t0 + 400 * kMs).show_game);
}

void TestAnimatedSettle() {
  AnimatedSettle(
      "a cover closes (its fading ghost)",
      [](GuardInputs& in) { Both(in, [](Snapshot& x) { x.occluded = true; }); },
      [](GuardInputs& in) { Both(in, [](Snapshot& x) { x.occluded = false; }); });
  AnimatedSettle(
      "the target is restored from minimized (drawable again)",
      [](GuardInputs& in) { Both(in, [](Snapshot& x) { x.window_alive = false; }); },
      [](GuardInputs& in) { Both(in, [](Snapshot& x) { x.window_alive = true; }); });
  AnimatedSettle(
      "another window of the game adopted (it fades in)",
      [](GuardInputs&) {},
      [](GuardInputs& in) { Both(in, [](Snapshot& x) { x.target = 0x9999; }); });
  printf("not animated, 100 ms: back in front by alt-tab\n");
  GuardInputs in = Good(10000 * kMs);
  Both(in, [](Snapshot& x) { x.foreground_is_game = false; });
  SettleState s = Settled(in);
  in.now_ns += 16 * kMs;
  Both(in, [](Snapshot& x) { x.foreground_is_game = true; });
  uint64_t t0 = in.now_ns;
  Run(in, s);
  CHECK(StepTo(in, s, t0 + 100 * kMs).show_game);

  printf("animated transition, 400 ms: the source shown again (a restore "
         "may have run while it was hidden)\n");
  GuardInputs r = Good(10000 * kMs);
  SettleState sr = Settled(r);
  r.now_ns += 33 * kMs;
  r.reshown = true;
  Run(r, sr);
  r.reshown = false;
  uint64_t t1 = r.now_ns;
  CHECK(!StepTo(r, sr, t1 + 399 * kMs).show_game);
  CHECK(StepTo(r, sr, t1 + 400 * kMs).show_game);
}

// Codex 7: A at acquire, B at render, then A again
void TestRoundTrip() {
  printf("rule 9: A->B->A within two frames still settles 100 ms\n");
  const Rect a = kGame;
  const Rect b = R(140, 111, 1506, 879);
  GuardInputs in = Good(10000 * kMs);
  SettleState s = Settled(in);
  uint64_t t1 = in.now_ns + 33 * kMs;
  in.now_ns = t1;
  in.pre.client = a;
  in.render.client = b;
  GuardDecision d1 = Run(in, s);
  CHECK(!d1.show_game && d1.reason == Reason::kChangedSinceAcquire);
  in.render.client = a;  // back at A for both reads
  in.now_ns = t1 + 33 * kMs;
  GuardDecision d2 = Run(in, s);
  CHECK(!d2.show_game && d2.reason == Reason::kSettling);
  CHECK(!StepTo(in, s, t1 + 99 * kMs).show_game);
  CHECK(StepTo(in, s, t1 + 33 * kMs + 100 * kMs).show_game);
}

// Codex 5 / rule 8: frame provenance
void TestFrameProvenance() {
  printf("rule 8: no new desktop image since the change -> BRB past the "
         "settle\n");
  live_frames = false;
  GuardInputs in = Good(10000 * kMs);
  SettleState s = Settled(in);  // frozen image presented at 9999 ms
  in.now_ns += 16 * kMs;
  Both(in, [](Snapshot& x) { x.occluded = true; });  // Notepad on top
  Decide(in, s);
  Both(in, [](Snapshot& x) { x.occluded = false; });  // and gone again
  uint64_t back = in.now_ns + 16 * kMs;
  in.now_ns = back;
  Decide(in, s);
  // the acquire keeps timing out: the held image is from before the change
  GuardDecision d = StepTo(in, s, back + 500 * kMs);
  CHECK(!d.show_game && d.reason == Reason::kStaleFrame);
  printf("rule 8: a new desktop image after the change -> game\n");
  in.frame.present_ns = back + 510 * kMs;
  d = StepTo(in, s, back + 520 * kMs);
  CHECK(d.show_game);
  printf("rule 8: a still desktop keeps the approved image (no change since "
         "it)\n");
  d = StepTo(in, s, back + 2000 * kMs);
  CHECK(d.show_game);

  printf("rule 8: no desktop image at all since start -> BRB\n");
  GuardInputs f = Good(5000 * kMs);
  f.frame.present_ns = 0;
  SettleState sf;
  CHECK(!StepTo(f, sf, 5300 * kMs).show_game);
  CHECK(StepTo(f, sf, 5300 * kMs).reason == Reason::kStaleFrame);
  live_frames = true;

  // Codex review 2: the last image a window paints after a move can arrive
  // between the pre-acquire read and the render read of the change frame
  printf("rule 8: a change seen before the acquire: an image presented after "
         "that read counts\n");
  live_frames = false;
  GuardInputs a = Good(10000 * kMs);
  SettleState sa = Settled(a);
  uint64_t t = a.now_ns + 33 * kMs;
  a.pre_ns = t - 5 * kMs;
  a.now_ns = t;
  Both(a, [](Snapshot& x) { x.client = R(200, 111, 1566, 879); });
  a.frame.present_ns = t - 2 * kMs;  // after the pre read, before the render
  Decide(a, sa);
  a.pre_ns = 0;
  CHECK(StepTo(a, sa, t + 200 * kMs).show_game);
  printf("rule 8: a change seen only at render: an image from before that "
         "read stays out\n");
  GuardInputs b2 = Good(10000 * kMs);
  SettleState sb2 = Settled(b2);
  b2.now_ns += 33 * kMs;
  b2.pre_ns = b2.now_ns - 5 * kMs;
  b2.render.client = R(200, 111, 1566, 879);  // the pre read still saw A
  b2.frame.present_ns = b2.now_ns - 2 * kMs;
  Decide(b2, sb2);
  b2.pre_ns = 0;
  b2.pre.client = b2.render.client;
  GuardDecision db = StepTo(b2, sb2, b2.now_ns + 200 * kMs);
  CHECK(!db.show_game && db.reason == Reason::kStaleFrame);
  printf("rule 8: the first frame (or a re-show): an image presented after "
         "the matching pre read counts\n");
  GuardInputs st = Good(5000 * kMs);
  SettleState sst;
  st.pre_ns = st.now_ns - 5 * kMs;
  st.frame.present_ns = st.now_ns - 2 * kMs;
  Decide(st, sst);
  st.pre_ns = 0;
  CHECK(StepTo(st, sst, 5200 * kMs).show_game);
  GuardInputs rs = Good(10000 * kMs);
  SettleState srs = Settled(rs);
  rs.now_ns += 33 * kMs;
  rs.reshown = true;
  rs.pre_ns = rs.now_ns - 5 * kMs;
  rs.frame.present_ns = rs.now_ns - 2 * kMs;
  Decide(rs, srs);
  rs.reshown = false;
  rs.pre_ns = 0;
  CHECK(StepTo(rs, srs, rs.now_ns + 450 * kMs).show_game);  // re-show: 400 ms
  live_frames = true;

  printf("rule 8: the capture holds no image -> BRB\n");
  d = Broken([](GuardInputs& in2) { in2.frame.valid = false; });
  CHECK(!d.show_game && d.reason == Reason::kCaptureMismatch);
  printf("rule 8: the image is not the monitor's size -> BRB\n");
  d = Broken([](GuardInputs& in2) { in2.frame.area = R(0, 0, 1280, 720); });
  CHECK(!d.show_game && d.reason == Reason::kCaptureMismatch);
  printf("rule 8: the image is another monitor (origin differs) -> BRB\n");
  d = Broken([](GuardInputs& in2) {
    in2.frame.area = R(1920, 0, 3840, 1080);
  });
  CHECK(!d.show_game && d.reason == Reason::kCaptureMismatch);
}

// rule 5: BRB until 100 ms after the last change of any input
void SettleAfter(const char* what, void (*change)(GuardInputs&)) {
  printf("rule 5: settle after %s\n", what);
  GuardInputs in = Good(10000 * kMs);
  SettleState s = Settled(in);
  uint64_t t0 = in.now_ns + 16 * kMs;
  in.now_ns = t0;
  change(in);
  Run(in, s);  // the change frame (may be BRB for its own rule)
  // the change is undone or holds; either way 100 ms of BRB follows it
  bool all_brb = true;
  for (uint64_t t = 16; t < 100; t += 16) {
    in.now_ns = t0 + t * kMs;
    GuardDecision d = Run(in, s);
    if (d.show_game) {
      all_brb = false;
    }
  }
  CHECK(all_brb);
  in.now_ns = t0 + 100 * kMs;
  GuardDecision d = Run(in, s);
  CHECK(d.show_game || d.reason != Reason::kSettling);
}

void TestSettle() {
  SettleAfter("a move (rect changes and stays)", [](GuardInputs& in) {
    Both(in, [](Snapshot& x) { x.client = R(200, 111, 1566, 879); });
  });
  SettleAfter("a resize", [](GuardInputs& in) {
    Both(in, [](Snapshot& x) { x.client = R(108, 111, 1374, 829); });
  });

  printf("rule 5: back in front after alt-tab -> 100 ms of BRB (P3 stale "
         "Notepad frame)\n");
  GuardInputs in = Good(10000 * kMs);
  SettleState s = Settled(in);
  uint64_t t = in.now_ns;
  Both(in, [](Snapshot& x) { x.foreground_is_game = false; });
  for (int i = 0; i < 30; i++) {
    t += 16 * kMs;
    in.now_ns = t;
    if (Run(in, s).show_game) {
      failures++;
      printf("FAIL: game shown while Notepad in front\n");
    }
  }
  Both(in, [](Snapshot& x) { x.foreground_is_game = true; });
  uint64_t back = t + 16 * kMs;
  in.now_ns = back;
  GuardDecision d = Run(in, s);
  CHECK(!d.show_game && d.reason == Reason::kSettling);
  d = StepTo(in, s, back + 33 * kMs);  // P3 saw the stale frame 2 ticks later
  CHECK(!d.show_game && d.reason == Reason::kSettling);
  CHECK(!StepTo(in, s, back + 99 * kMs).show_game);
  CHECK(StepTo(in, s, back + 100 * kMs).show_game);

  SettleAfter("an occluder appears", [](GuardInputs& in) {
    Both(in, [](Snapshot& x) { x.occluded = true; });
  });
  SettleAfter("back on the monitor", [](GuardInputs& in) {
    Both(in, [](Snapshot& x) { x.client = R(-10, 100, 1356, 868); });
  });
  SettleAfter("desktop focus back", [](GuardInputs& in) {
    in.desktop_front = false;
  });

  printf("rule 5: the change frame then steady inputs: the cover leaving "
         "settles 400 ms\n");
  GuardInputs m = Good(10000 * kMs);
  SettleState sm = Settled(m);
  m.now_ns += 16 * kMs;
  Both(m, [](Snapshot& x) { x.occluded = true; });
  Run(m, sm);
  uint64_t change = m.now_ns;
  // cover leaves on the next frame: that is a change too
  Both(m, [](Snapshot& x) { x.occluded = false; });
  m.now_ns = change + 16 * kMs;
  CHECK(!Run(m, sm).show_game);
  CHECK(!StepTo(m, sm, change + 16 * kMs + 399 * kMs).show_game);
  CHECK(StepTo(m, sm, change + 16 * kMs + 400 * kMs).show_game);

  printf("rule 5: first frame after start settles\n");
  GuardInputs f = Good(5000 * kMs);
  SettleState sf;
  GuardDecision df = Run(f, sf);
  CHECK(!df.show_game && df.reason == Reason::kSettling);
  f.now_ns += 50 * kMs;
  CHECK(!Run(f, sf).show_game);
  f.now_ns += 50 * kMs;
  CHECK(Run(f, sf).show_game);

  printf("rule 5: a long render gap (2 s) settles\n");
  GuardInputs g = Good(10000 * kMs);
  SettleState sg = Settled(g);
  g.now_ns += 2000 * kMs;  // not rendered for 2 s, inputs look the same
  GuardDecision dg = Run(g, sg);
  CHECK(!dg.show_game && dg.reason == Reason::kSettling);
  g.now_ns += 16 * kMs;
  CHECK(!Run(g, sg).show_game);
  printf("rule 5: a normal 33 ms frame gap (30 fps) does not settle\n");
  GuardInputs h = Good(10000 * kMs);
  SettleState sh = Settled(h);
  h.now_ns += 33 * kMs;
  CHECK(Run(h, sh).show_game);
  printf("rule 5: render lag (VM r1 at 150%%: 57%% lagged frames) does not "
         "settle on its own\n");
  bool lag_ok = true;
  for (int i = 0; i < 20; i++) {
    h.now_ns += (i % 2 ? 66 : 133) * kMs;  // frames 66-133 ms apart
    if (!Run(h, sh).show_game) {
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
  GuardDecision dr = Run(r, sr);
  CHECK(!dr.show_game && dr.reason == Reason::kSettling);
  r.reshown = false;
  uint64_t reshown_at = r.now_ns;
  CHECK(!StepTo(r, sr, reshown_at + 99 * kMs).show_game);
  CHECK(StepTo(r, sr, reshown_at + 400 * kMs).show_game);
  printf("rule 5: a clock going backwards settles\n");
  GuardInputs b = Good(10000 * kMs);
  SettleState sb = Settled(b);
  b.now_ns -= 5 * kMs;
  CHECK(!Run(b, sb).show_game);
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

  printf("a layered window read as alpha 0 is ignored; nonzero or unknown "
         "alpha still occludes\n");
  WindowFacts zero = Pip();
  zero.zero_alpha = true;
  CHECK(!IsOccluder(kGamePid, game, zero));
  CHECK(FirstOccluder(kGamePid, game, {zero, notepad}) == 1);
  WindowFacts nonzero = Pip();  // LWA_ALPHA with alpha > 0
  CHECK(IsOccluder(kGamePid, game, nonzero));
  WindowFacts unknown = Pip();  // GetLayeredWindowAttributes failed
  CHECK(IsOccluder(kGamePid, game, unknown));

  printf("zero_alpha only from a read that says LWA_ALPHA with alpha 0\n");
  using follow_window::kLwaAlpha;
  using follow_window::ZeroAlpha;
  CHECK(ZeroAlpha(true, 0, kLwaAlpha));
  CHECK(ZeroAlpha(true, 0, kLwaAlpha | 0x1));  // plus LWA_COLORKEY
  CHECK(!ZeroAlpha(true, 1, kLwaAlpha));       // nonzero
  CHECK(!ZeroAlpha(true, 255, kLwaAlpha));
  CHECK(!ZeroAlpha(false, 0, kLwaAlpha));      // unknown: the call failed
  CHECK(!ZeroAlpha(true, 0, 0x1));             // colour key only
  CHECK(!ZeroAlpha(true, 0, 0));               // no flags

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
  printf("rule 7: a live, drawable window of the game is kept\n");
  TargetFacts f;
  f.target = 0x1234;
  f.target_alive = true;
  f.target_drawable = true;
  f.game_pid = kGamePid;
  f.foreground = 0x9999;
  f.foreground_pid = 7;
  f.foreground_drawable = true;
  auto r = ResolveTarget(f);
  CHECK(r.target == 0x1234 && !r.adopted && !r.gone);

  printf("rule 7: gone, the foreground window is the game's pid -> adopt\n");
  f.target_alive = false;
  f.target_drawable = false;
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
  z.foreground_drawable = true;
  CHECK(ResolveTarget(z).gone);

  // Codex 8: a live HWND that draws nothing is not a game picture
  printf("rule 7: alive but hidden, a drawable window of the game in front "
         "-> adopt it\n");
  TargetFacts h;
  h.target = 0x1234;
  h.target_alive = true;
  h.target_drawable = false;  // hidden, cloaked or minimized
  h.game_pid = kGamePid;
  h.foreground = 0x9999;
  h.foreground_pid = kGamePid;
  h.foreground_drawable = true;
  r = ResolveTarget(h);
  CHECK(r.target == 0x9999 && r.adopted && !r.gone);
  printf("rule 7: alive but hidden, another pid in front -> BRB\n");
  h.foreground_pid = 7;
  r = ResolveTarget(h);
  CHECK(r.gone && r.target == 0);
  printf("rule 7: alive but hidden, the game's other window in front is not "
         "drawable either -> BRB\n");
  h.foreground_pid = kGamePid;
  h.foreground_drawable = false;
  CHECK(ResolveTarget(h).gone);
  printf("rule 7: the hidden target itself is the foreground (minimized game "
         "still active) -> BRB\n");
  h.foreground = 0x1234;
  h.foreground_drawable = false;
  CHECK(ResolveTarget(h).gone);
}

// popup/RESULT.md Q3 design 3: EVT_DISPLAY_SOURCE_CHANGED per reason change
void TestSourceEvent() {
  using follow_window::ReasonKey;
  using follow_window::ReasonNotifier;
  printf("event payload: source, reason key, follow\n");
  OBSDataAutoRelease brb = obs_data_create();
  settings::FillFollowWindowSourceEvent(brb, false, ReasonKey(Reason::kOccluded));
  std::string jb = obs_data_get_json(brb);
  printf("  %s\n", jb.c_str());
  CHECK(std::string(obs_data_get_string(brb, "source")) == "brb");
  CHECK(std::string(obs_data_get_string(brb, "reason")) == "occluded");
  CHECK(obs_data_get_bool(brb, "follow"));
  OBSDataAutoRelease game = obs_data_create();
  settings::FillFollowWindowSourceEvent(game, true, ReasonKey(Reason::kGame));
  CHECK(std::string(obs_data_get_string(game, "source")) == "monitor");
  CHECK(std::string(obs_data_get_string(game, "reason")) == "game");
  CHECK(obs_data_get_bool(game, "follow"));

  printf("event: once per reason change over a run of frames, never per "
         "frame\n");
  GuardInputs in = Good(10000 * kMs);
  SettleState s;
  ReasonNotifier n;
  int sent = 0;
  std::string keys;
  auto frame = [&]() {
    GuardDecision d = Run(in, s);
    if (n.Changed(d.reason)) {
      sent++;
      keys += std::string(ReasonKey(d.reason)) + " ";
    }
  };
  for (int i = 0; i < 30; i++) {  // settling, then game
    in.now_ns += 10 * kMs;
    frame();
  }
  Both(in, [](Snapshot& x) { x.occluded = true; });
  for (int i = 0; i < 30; i++) {  // covered for 300 ms
    in.now_ns += 10 * kMs;
    frame();
  }
  Both(in, [](Snapshot& x) { x.occluded = false; });
  for (int i = 0; i < 50; i++) {  // settling (400 ms), then game again
    in.now_ns += 10 * kMs;
    frame();
  }
  printf("  sent %d: %s\n", sent, keys.c_str());
  CHECK(sent == 5);
  CHECK(keys == "settling game occluded settling game ");
}

// ---------------------------------------------------------------------------
// rule 2 masking (SIDECAR.md section 5)

using follow_window::MaskPlan;
using follow_window::MaskRefusal;
using follow_window::NormMask;
using follow_window::PlanMasks;

// client-relative rect
Rect C(int32_t l, int32_t t, int32_t r, int32_t b) { return R(l, t, r, b); }

void TestMaskGeometry() {
  using follow_window::Grow;
  using follow_window::Intersect;
  using follow_window::MarginPx;
  using follow_window::OcclusionArea;
  using follow_window::kMaskMarginDip;
  printf("mask geometry: grow, intersect, margin by DPI\n");
  CHECK(Grow(R(100, 100, 200, 150), 10) == R(90, 90, 210, 160));
  CHECK(Grow(R(100, 100, 200, 150), 0) == R(100, 100, 200, 150));
  CHECK(Intersect(R(0, 0, 100, 100), R(50, 60, 150, 160)) == R(50, 60, 100, 100));
  CHECK(Intersect(R(0, 0, 100, 100), R(100, 0, 200, 100)).empty());  // touching
  CHECK(Intersect(R(0, 0, 100, 100), R(300, 300, 400, 400)).empty());
  CHECK(MarginPx(96) == kMaskMarginDip);
  CHECK(MarginPx(144) == (kMaskMarginDip * 3 + 1) / 2);  // 150%, rounded up
  CHECK(MarginPx(120) == (kMaskMarginDip * 120 + 95) / 96);  // 125%
  CHECK(MarginPx(192) == kMaskMarginDip * 2);
  CHECK(MarginPx(0) == -1);
  printf("a window whose grown bounds reach the client counts as covering\n");
  const Rect area = OcclusionArea(kGame, 20);
  CHECK(area == Grow(kGame, 20));
  WindowFacts beside;
  beside.pid = 99;
  beside.visible = true;
  beside.rect = R(kGame.right + 10, 300, kGame.right + 300, 500);  // 10 px off
  CHECK(!follow_window::IsOccluder(7, kGame, beside));
  CHECK(follow_window::IsOccluder(7, area, beside));
  beside.rect = R(kGame.right + 20, 300, kGame.right + 300, 500);  // touching
  CHECK(!follow_window::IsOccluder(7, area, beside));
}

void TestShellBand() {
  using follow_window::ShellBandWindows;
  printf("shell bands: top-level windows outside the z-order list count as "
         "above the game\n");
  // 0x50 is the toast: FindWindowExW returns it, the list never does
  const std::vector<uint64_t> listed = {0x10, 0x20, 0x30, 0x40};
  std::vector<uint64_t> got =
      ShellBandWindows({0x10, 0x50, 0x20, 0x30, 0x40}, listed, 0x30);
  CHECK(got.size() == 1 && got[0] == 0x50);
  printf("several, in enumeration order; the game itself never\n");
  got = ShellBandWindows({0x60, 0x10, 0x50, 0x99}, listed, 0x99);
  CHECK(got.size() == 2 && got[0] == 0x60 && got[1] == 0x50);
  printf("nothing outside the list: none\n");
  CHECK(ShellBandWindows({0x40, 0x30, 0x20, 0x10}, listed, 0x30).empty());
  printf("a window created between the two reads is outside the list: it "
         "counts (fails toward BRB or a mask)\n");
  got = ShellBandWindows({0x10, 0x20, 0x30, 0x40, 0x70}, listed, 0x30);
  CHECK(got.size() == 1 && got[0] == 0x70);

  printf("several walks: only a window every walk misses counts "
         "(orchestrator)\n");
  // 0x40 was being reordered behind the game during the first walk
  const std::vector<uint64_t> first = {0x10, 0x20, 0x30};
  const std::vector<uint64_t> again = {0x10, 0x20, 0x40, 0x30};
  got = ShellBandWindows({0x10, 0x50, 0x20, 0x30, 0x40},
                         std::vector<std::vector<uint64_t>>{first, again},
                         0x30);
  CHECK(got.size() == 1 && got[0] == 0x50);
  got = ShellBandWindows({0x10, 0x50, 0x20, 0x30, 0x40},
                         std::vector<std::vector<uint64_t>>{first, first, again},
                         0x30);
  CHECK(got.size() == 1 && got[0] == 0x50);
  got = ShellBandWindows({0x10, 0x50, 0x20, 0x30, 0x40},
                         std::vector<std::vector<uint64_t>>{first, first},
                         0x30);
  CHECK(got.size() == 2 && got[0] == 0x50 && got[1] == 0x40);
  CHECK(ShellBandWindows({0x50},
                         std::vector<std::vector<uint64_t>>{}, 0x30)
            .size() == 1);
}

void TestSnapToMonitorEdges() {
  using follow_window::SnapToMonitorEdges;
  printf("shell-band regions reach the near monitor edges\n");
  const Rect mon = R(0, 0, 1920, 1080);
  // the toast at 100%: right edge on the monitor's, 40 px above its bottom
  CHECK(SnapToMonitorEdges(R(1524, 647, 1920, 1040), mon, 64) ==
        R(1524, 647, 1920, 1080));
  // far from every edge: unchanged
  CHECK(SnapToMonitorEdges(R(600, 300, 900, 500), mon, 64) ==
        R(600, 300, 900, 500));
  // near the left and top
  CHECK(SnapToMonitorEdges(R(10, 63, 300, 200), mon, 64) ==
        R(0, 0, 300, 200));
  // exactly |reach| away: not snapped
  CHECK(SnapToMonitorEdges(R(64, 64, 300, 200), mon, 64) ==
        R(64, 64, 300, 200));
  // a second monitor's coordinates
  const Rect right = R(1920, 0, 3840, 1080);
  CHECK(SnapToMonitorEdges(R(3444, 647, 3840, 1040), right, 64) ==
        R(3444, 647, 3840, 1080));
}

void TestPlanMasks() {
  using follow_window::kMaxMasks;
  using follow_window::kMaxMaskedPercent;
  const Rect client = R(100, 100, 1100, 1100);  // 1000x1000
  printf("no occluder: nothing to mask\n");
  MaskPlan p = PlanMasks(client, false, {}, false, 10);
  CHECK(p.refusal == MaskRefusal::kNone && p.masks.empty());

  printf("one occluder inside: its bounds grown by the margin, "
         "client-relative\n");
  p = PlanMasks(client, true, {R(500, 600, 700, 700)}, false, 10);
  CHECK(p.refusal == MaskRefusal::kNone);
  CHECK(p.masks.size() == 1 && p.masks[0] == C(390, 490, 610, 610));

  printf("an occluder over the edge: cut to the client\n");
  p = PlanMasks(client, true, {R(1000, 50, 1300, 300)}, false, 10);
  CHECK(p.refusal == MaskRefusal::kNone);
  CHECK(p.masks.size() == 1 && p.masks[0] == C(890, 0, 1000, 210));

  printf("an occluder whose shadow only reaches the client: the margin "
         "strip\n");
  p = PlanMasks(client, true, {R(1105, 500, 1400, 600)}, false, 10);
  CHECK(p.refusal == MaskRefusal::kNone);
  CHECK(p.masks.size() == 1 && p.masks[0] == C(995, 390, 1000, 510));

  printf("two occluders: one mask each, z-order kept\n");
  p = PlanMasks(client, true, {R(200, 200, 300, 300), R(800, 800, 900, 900)},
                false, 0);
  CHECK(p.refusal == MaskRefusal::kNone && p.masks.size() == 2 &&
        p.masks[0] == C(100, 100, 200, 200) &&
        p.masks[1] == C(700, 700, 800, 800));

  printf("unknown: an occluder without DWM bounds, no rects, no margin\n");
  p = PlanMasks(client, true, {R(500, 600, 700, 700)}, true, 10);
  CHECK(p.refusal == MaskRefusal::kBoundsUnknown && p.masks.empty());
  p = PlanMasks(client, true, {}, false, 10);
  CHECK(p.refusal == MaskRefusal::kBoundsUnknown && p.masks.empty());
  p = PlanMasks(client, true, {R(500, 600, 700, 700)}, false, -1);
  CHECK(p.refusal == MaskRefusal::kBoundsUnknown && p.masks.empty());

  printf("N: %zu occluders masked, %zu is full BRB\n", kMaxMasks,
         kMaxMasks + 1);
  std::vector<Rect> many;
  for (size_t i = 0; i < kMaxMasks; i++) {
    int32_t x = 120 + (int32_t)i * 150;
    many.push_back(R(x, 120, x + 100, 220));
  }
  p = PlanMasks(client, true, many, false, 0);
  CHECK(p.refusal == MaskRefusal::kNone && p.masks.size() == kMaxMasks);
  many.push_back(R(120, 900, 220, 1000));
  p = PlanMasks(client, true, many, false, 0);
  CHECK(p.refusal == MaskRefusal::kTooMany && p.masks.empty());

  printf("X: masks up to %u%% of the client masked, more is full BRB\n",
         kMaxMaskedPercent);
  // 1000 x (10 * X) px = X% of 1000x1000
  const int32_t rows = (int32_t)kMaxMaskedPercent * 10;
  p = PlanMasks(client, true, {R(100, 100, 1100, 100 + rows)}, false, 0);
  CHECK(p.refusal == MaskRefusal::kNone && p.masks.size() == 1);
  p = PlanMasks(client, true, {R(100, 100, 1100, 100 + rows + 1)}, false, 0);
  CHECK(p.refusal == MaskRefusal::kTooLarge && p.masks.empty());
  // the margin counts toward the area
  p = PlanMasks(client, true, {R(100, 100, 1100, 100 + rows - 5)}, false, 10);
  CHECK(p.refusal == MaskRefusal::kTooLarge);
  // two halves that together pass X
  p = PlanMasks(client, true,
                {R(100, 100, 1100, 100 + rows / 2 + 1),
                 R(100, 600, 1100, 600 + rows / 2 + 1)},
                false, 0);
  CHECK(p.refusal == MaskRefusal::kTooLarge);
}

// occluders at |rects| (screen px) in both reads
void Cover(GuardInputs& in, std::vector<Rect> rects) {
  in.pre.occluded = in.render.occluded = !rects.empty();
  in.pre.occluders = in.render.occluders = rects;
}

void TestMaskedDecision() {
  printf("masking: an occluder that appears is full BRB for 400 ms, then "
         "masked\n");
  GuardInputs in = Good(10000 * kMs);
  in.pre.margin = in.render.margin = 10;
  SettleState s = Settled(in);
  CHECK(Run(in, s).reason == Reason::kGame);
  const Rect toast = R(1100, 700, 1460, 800);  // over the game's lower right
  Cover(in, {toast});
  const uint64_t t0 = in.now_ns + 10 * kMs;
  in.now_ns = t0;
  GuardDecision d = Run(in, s);
  CHECK(!d.show_game && d.reason == Reason::kSettling);
  d = StepTo(in, s, t0 + 390 * kMs);
  CHECK(!d.show_game && d.reason == Reason::kSettling);
  d = StepTo(in, s, t0 + 410 * kMs);
  CHECK(d.show_game && d.reason == Reason::kMasked);
  // crop-relative: client (108,111); toast grown by 10, cut to the client
  CHECK(d.masks.size() == 1 &&
        d.masks[0] == C(1090 - 108, 690 - 111, 1470 - 108, 810 - 111));
  CHECK(d.crop == R(108, 111, 1474, 879));

  printf("the occluder moves: full BRB and settle again\n");
  Cover(in, {R(1100, 650, 1460, 750)});
  const uint64_t t1 = in.now_ns + 10 * kMs;
  in.now_ns = t1;
  CHECK(Run(in, s).reason == Reason::kSettling);
  CHECK(StepTo(in, s, t1 + 390 * kMs).reason == Reason::kSettling);
  d = StepTo(in, s, t1 + 410 * kMs);
  CHECK(d.reason == Reason::kMasked && d.masks.size() == 1 &&
        d.masks[0] == C(1090 - 108, 640 - 111, 1470 - 108, 760 - 111));

  printf("the occluder resizes: full BRB and settle again\n");
  Cover(in, {R(1100, 650, 1460, 770)});
  const uint64_t t2 = in.now_ns + 10 * kMs;
  in.now_ns = t2;
  CHECK(Run(in, s).reason == Reason::kSettling);
  CHECK(StepTo(in, s, t2 + 390 * kMs).reason == Reason::kSettling);
  CHECK(StepTo(in, s, t2 + 410 * kMs).reason == Reason::kMasked);

  printf("a second occluder joins: full BRB and settle, then two masks\n");
  Cover(in, {R(1100, 650, 1460, 770), R(200, 200, 400, 300)});
  const uint64_t t3 = in.now_ns + 10 * kMs;
  in.now_ns = t3;
  CHECK(Run(in, s).reason == Reason::kSettling);
  CHECK(StepTo(in, s, t3 + 390 * kMs).reason == Reason::kSettling);
  d = StepTo(in, s, t3 + 410 * kMs);
  CHECK(d.reason == Reason::kMasked && d.masks.size() == 2);

  printf("the occluders leave: full BRB for 400 ms, then the game\n");
  Cover(in, {});
  const uint64_t t4 = in.now_ns + 10 * kMs;
  in.now_ns = t4;
  CHECK(Run(in, s).reason == Reason::kSettling);
  CHECK(StepTo(in, s, t4 + 390 * kMs).reason == Reason::kSettling);
  d = StepTo(in, s, t4 + 410 * kMs);
  CHECK(d.reason == Reason::kGame && d.show_game && d.masks.empty());

  printf("the first decision already sees an occluder (start, retarget): "
         "400 ms, not 100 (Codex)\n");
  {
    GuardInputs f = Good(10000 * kMs);
    Cover(f, {toast});
    SettleState fs;
    CHECK(Run(f, fs).reason == Reason::kSettling);
    const uint64_t start = f.now_ns;
    CHECK(StepTo(f, fs, start + 110 * kMs).reason == Reason::kSettling);
    CHECK(StepTo(f, fs, start + 390 * kMs).reason == Reason::kSettling);
    CHECK(StepTo(f, fs, start + 410 * kMs).reason == Reason::kMasked);
  }

  printf("masks need an image presented after the settle: one captured "
         "mid-animation and kept by a stalled capture is full BRB (Codex)\n");
  {
    GuardInputs f = Good(10000 * kMs);
    SettleState fs = Settled(f);
    Cover(f, {toast});
    const uint64_t t = f.now_ns + 10 * kMs;
    f.now_ns = t;
    CHECK(Run(f, fs).reason == Reason::kSettling);
    live_frames = false;
    f.frame.present_ns = t + 50 * kMs;  // the last image: mid-slide
    f.now_ns = t + 410 * kMs;
    GuardDecision fd = Decide(f, fs);
    CHECK(!fd.show_game && fd.reason == Reason::kSettling);
    f.now_ns = t + 900 * kMs;  // still that image, long after
    fd = Decide(f, fs);
    CHECK(!fd.show_game);
    f.frame.present_ns = t + 405 * kMs;  // presented after the settle ended
    f.now_ns = t + 910 * kMs;
    fd = Decide(f, fs);
    CHECK(fd.show_game && fd.reason == Reason::kMasked);
    live_frames = true;
  }

  printf("the render read sees another region than the pre-acquire read: "
         "BRB, settle\n");
  {
    GuardInputs m = Good(10000 * kMs);
    Cover(m, {toast});
    SettleState ms = Settled(m);
    CHECK(Run(m, ms).reason == Reason::kMasked);
    m.now_ns += 10 * kMs;
    m.render.occluders = {R(1100, 690, 1460, 790)};  // moved after acquire
    CHECK(Run(m, ms).reason == Reason::kChangedSinceAcquire);
    m.pre.occluders = m.render.occluders;  // and stays there: the settle
    // counts from the render read that first saw it, 10 ms earlier
    const uint64_t t = m.now_ns + 10 * kMs;
    m.now_ns = t;
    CHECK(Run(m, ms).reason == Reason::kSettling);
    CHECK(StepTo(m, ms, t + 380 * kMs).reason == Reason::kSettling);
    CHECK(StepTo(m, ms, t + 410 * kMs).reason == Reason::kMasked);
    printf("A->B->A across the two reads settles too\n");
    m.now_ns += 10 * kMs;
    m.render.occluders = {toast};
    CHECK(Run(m, ms).reason == Reason::kChangedSinceAcquire);
    m.now_ns += 10 * kMs;
    m.render.occluders = m.pre.occluders;
    CHECK(Run(m, ms).reason == Reason::kSettling);
  }

  printf("too many, too large, unknown bounds: full BRB (occluded)\n");
  {
    GuardInputs m = Good(10000 * kMs);
    std::vector<Rect> five;
    for (int i = 0; i < 5; i++) {
      five.push_back(R(200 + i * 100, 200, 260 + i * 100, 260));
    }
    Cover(m, five);
    SettleState ms = Settled(m);
    d = Run(m, ms);
    CHECK(!d.show_game && d.reason == Reason::kOccluded &&
          d.refusal == MaskRefusal::kTooMany);
    Cover(m, {R(108, 111, 1474, 600)});  // most of the client
    ms = Settled(m);
    d = Run(m, ms);
    CHECK(!d.show_game && d.reason == Reason::kOccluded &&
          d.refusal == MaskRefusal::kTooLarge);
    Cover(m, {toast});
    m.pre.occluder_bounds_unknown = m.render.occluder_bounds_unknown = true;
    ms = Settled(m);
    d = Run(m, ms);
    CHECK(!d.show_game && d.reason == Reason::kOccluded &&
          d.refusal == MaskRefusal::kBoundsUnknown);
    printf("only one read can't mask: full BRB\n");
    m.pre.occluder_bounds_unknown = false;
    ms = Settled(m);
    d = Run(m, ms);
    CHECK(!d.show_game && d.reason == Reason::kOccluded);
    m.render.occluder_bounds_unknown = false;
    m.pre.margin = -1;
    ms = Settled(m);
    d = Run(m, ms);
    CHECK(!d.show_game && d.reason == Reason::kOccluded);
  }

  printf("every other rule still wins over masking\n");
  {
    GuardInputs m = Good(10000 * kMs);
    Cover(m, {toast});
    SettleState ms = Settled(m);
    CHECK(Run(m, ms).reason == Reason::kMasked);
    m.now_ns += 10 * kMs;
    Both(m, [](Snapshot& x) { x.foreground_is_game = false; });
    CHECK(Run(m, ms).reason == Reason::kNotForeground);
    GuardInputs u = Good(10000 * kMs);
    Cover(u, {toast});
    u.desktop_front = false;
    ms = Settled(u);
    CHECK(Run(u, ms).reason == Reason::kDesktopUnfocused);
    GuardInputs o = Good(10000 * kMs);
    Cover(o, {toast});
    Both(o, [](Snapshot& x) { x.client = R(1500, 800, 2100, 1200); });
    ms = Settled(o);
    CHECK(Run(o, ms).reason == Reason::kOffMonitor);
    GuardInputs st = Good(10000 * kMs);
    Cover(st, {toast});
    ms = Settled(st);
    live_frames = false;
    st.frame.present_ns = 1;  // nothing new since long before
    Cover(st, {R(1100, 690, 1460, 790)});  // a change the image never showed
    st.now_ns += 10 * kMs;
    CHECK(Run(st, ms).reason == Reason::kSettling);
    st.now_ns += 410 * kMs;
    CHECK(Run(st, ms).reason == Reason::kStaleFrame);
    live_frames = true;
  }
}

void TestMaskEvent() {
  using follow_window::MasksJson;
  using follow_window::NormalizeMasks;
  using follow_window::ReasonKey;
  using follow_window::ReasonNotifier;
  printf("mask event: key, normalized outward to 1e-4, JSON\n");
  CHECK(std::string(ReasonKey(Reason::kMasked)) == "masked");
  // crop 1366x768: (100,50)-(250,120) -> x 0.0732..0.1831, y 0.0651..0.1563
  std::vector<NormMask> n = NormalizeMasks({C(100, 50, 250, 120)}, 1366, 768);
  CHECK(n.size() == 1);
  CHECK(n.size() == 1 && n[0].x == 732 && n[0].y == 651);
  CHECK(n.size() == 1 && n[0].x + n[0].w == 1831 && n[0].y + n[0].h == 1563);
  // exact edges stay exact
  n = NormalizeMasks({C(0, 0, 1366, 384)}, 1366, 768);
  CHECK(n.size() == 1 && n[0] == (NormMask{0, 0, 10000, 5000}));
  CHECK(MasksJson(n) == "[[0.0000,0.0000,1.0000,0.5000]]");
  n = NormalizeMasks({C(0, 0, 683, 384), C(683, 384, 1366, 768)}, 1366, 768);
  CHECK(MasksJson(n) ==
        "[[0.0000,0.0000,0.5000,0.5000],[0.5000,0.5000,0.5000,0.5000]]");
  CHECK(MasksJson({}) == "[]");

  printf("event payload: the masks member spliced into the JSON object\n");
  OBSDataAutoRelease ev = obs_data_create();
  settings::FillFollowWindowSourceEvent(ev, true, ReasonKey(Reason::kMasked));
  obs_data_set_int(ev, "event", 7);
  const std::string base = obs_data_get_json(ev);
  const std::string with = settings::AddRawJsonMember(
      base, "masks", "[[0.1000,0.2000,0.3000,0.4000]]");
  printf("  %s\n", with.c_str());
  CHECK(with.find("\"masks\": [[0.1000,0.2000,0.3000,0.4000]]") !=
        std::string::npos);
  OBSDataAutoRelease back = obs_data_create_from_json(with.c_str());
  CHECK(back.Get() != nullptr);
  CHECK(back && std::string(obs_data_get_string(back, "source")) == "monitor");
  CHECK(back && std::string(obs_data_get_string(back, "reason")) == "masked");
  CHECK(back && obs_data_get_bool(back, "follow"));
  CHECK(back && obs_data_get_int(back, "event") == 7);
  CHECK(settings::AddRawJsonMember(base, "masks", "") == base);
  CHECK(settings::AddRawJsonMember("{}", "masks", "[]") == "{\"masks\": []}");
  CHECK(settings::AddRawJsonMember("{\"a\": 1}", "masks", "[]") ==
        "{\"a\": 1, \"masks\": []}");
  CHECK(settings::AddRawJsonMember("not json", "masks", "[]") == "not json");

  printf("event once per change of the mask set, never per frame\n");
  GuardInputs in = Good(10000 * kMs);
  SettleState s;
  ReasonNotifier note;
  int sent = 0;
  std::string keys;
  auto frames = [&](int count) {
    for (int i = 0; i < count; i++) {
      in.now_ns += 10 * kMs;
      GuardDecision d = Run(in, s);
      std::vector<NormMask> nm =
          d.show_game
              ? NormalizeMasks(d.masks, d.crop.width(), d.crop.height())
              : std::vector<NormMask>();
      if (note.Changed(d.reason, nm)) {
        sent++;
        keys += std::string(ReasonKey(d.reason)) + "/" +
                std::to_string(nm.size()) + " ";
      }
    }
  };
  frames(30);  // settling, game
  Cover(in, {R(1100, 700, 1460, 800)});
  frames(60);  // settling, masked
  Cover(in, {R(1100, 650, 1460, 750)});
  frames(60);  // settling, masked (moved)
  Cover(in, {R(1100, 650, 1460, 750), R(200, 200, 300, 300)});
  frames(60);  // settling, masked x2
  Cover(in, {});
  frames(60);  // settling, game
  printf("  sent %d: %s\n", sent, keys.c_str());
  CHECK(sent == 10);
  CHECK(keys ==
        "settling/0 game/0 settling/0 masked/1 settling/0 masked/1 "
        "settling/0 masked/2 settling/0 game/0 ");

  printf("same reason, other masks: sent; same masks again: not sent\n");
  ReasonNotifier n2;
  CHECK(n2.Changed(Reason::kMasked, {NormMask{1, 2, 3, 4}}));
  CHECK(!n2.Changed(Reason::kMasked, {NormMask{1, 2, 3, 4}}));
  CHECK(n2.Changed(Reason::kMasked, {NormMask{1, 2, 3, 5}}));
  CHECK(n2.Changed(Reason::kMasked,
                   {NormMask{1, 2, 3, 5}, NormMask{9, 9, 9, 9}}));
  CHECK(!n2.Changed(Reason::kMasked,
                    {NormMask{1, 2, 3, 5}, NormMask{9, 9, 9, 9}}));
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
  TestMonitorConfig();
  TestLaterStart();
  TestSteadyGame();
  TestEachRule();
  TestRenderRecheck();
  TestRoundTrip();
  TestAnimatedSettle();
  TestFrameProvenance();
  TestSettle();
  TestOccluder();
  TestResolveTarget();
  TestSourceEvent();
  TestShellBand();
  TestSnapToMonitorEdges();
  TestMaskGeometry();
  TestPlanMasks();
  TestMaskedDecision();
  TestMaskEvent();
  TestFitInside();

  printf(failures ? "FAILED (%d)\n" : "ALL PASSED\n", failures);
  return failures ? 1 : 0;
}
