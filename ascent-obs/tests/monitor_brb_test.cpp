// Monitor payload 'method' / 'brb_when_unfocused' parsing and the
// monitor-primary BRB visibility decisions.
// Built and run against a local OBS build by scripts/run_tests.ps1.
#include <windows.h>
#include <obs.hpp>
#include <cstdio>
#include <cstring>

#include "obs_control/settings.h"

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

int Method(const char* monitor_json) {
  OBSDataAutoRelease monitor = obs_data_create_from_json(monitor_json);
  return settings::ParseMonitorCaptureMethod(monitor);
}

bool Brb(const char* monitor_json) {
  OBSDataAutoRelease monitor = obs_data_create_from_json(monitor_json);
  return settings::ParseMonitorBrbWhenUnfocused(monitor);
}

void TestProtocolKeys() {
  printf("protocol keys (snake_case, like monitor_handle)\n");
  CHECK(strcmp(settings::kSettingsMonitorMethod, "method") == 0);
  CHECK(strcmp(settings::kSettingsMonitorBrbWhenUnfocused,
               "brb_when_unfocused") == 0);
}

void TestMethod() {
  printf("absent method -> auto (0)\n");
  CHECK(Method(R"({"enable":true,"monitor_handle":65537})") == 0);
  CHECK(settings::ParseMonitorCaptureMethod(nullptr) == 0);

  printf("valid methods pass through\n");
  CHECK(Method(R"({"method":0})") == 0);
  CHECK(Method(R"({"method":1})") == 1);
  CHECK(Method(R"({"method":2})") == 2);

  printf("out of range -> auto (0)\n");
  CHECK(Method(R"({"method":3})") == 0);
  CHECK(Method(R"({"method":-1})") == 0);
  // would wrap to 1 if narrowed to int before the range check
  CHECK(Method(R"({"method":4294967297})") == 0);
}

void TestBrbFlag() {
  printf("brb_when_unfocused: absent -> false\n");
  CHECK(!Brb(R"({"enable":true})"));
  CHECK(!settings::ParseMonitorBrbWhenUnfocused(nullptr));
  CHECK(!Brb(R"({"brb_when_unfocused":false})"));
  CHECK(Brb(R"({"brb_when_unfocused":true})"));
  printf("other spellings are ignored\n");
  CHECK(!Brb(R"({"brbWhenUnfocused":true})"));
}

void TestMonitorPrimary() {
  printf("monitor primary only with the flag, a monitor and nothing else\n");
  // (flag, monitor, game, window)
  CHECK(settings::IsMonitorPrimaryWithBrb(true, true, false, false));
  CHECK(!settings::IsMonitorPrimaryWithBrb(false, true, false, false));
  CHECK(!settings::IsMonitorPrimaryWithBrb(true, false, false, false));
  CHECK(!settings::IsMonitorPrimaryWithBrb(true, true, true, false));
  CHECK(!settings::IsMonitorPrimaryWithBrb(true, true, false, true));
}

void TestShowsMonitor() {
  printf("monitor shown only while the game is foreground and not minimized\n");
  CHECK(settings::MonitorPrimaryShowsMonitor(true, false));
  CHECK(!settings::MonitorPrimaryShowsMonitor(false, false));
  CHECK(!settings::MonitorPrimaryShowsMonitor(false, true));
  CHECK(!settings::MonitorPrimaryShowsMonitor(true, true));
}

void TestRefusesGameSource() {
  printf("game sources are refused while brb_when_unfocused is set\n");
  CHECK(settings::MonitorPrimaryRefusesGameSource(true, true));
  CHECK(!settings::MonitorPrimaryRefusesGameSource(false, true));
  CHECK(!settings::MonitorPrimaryRefusesGameSource(true, false));
  CHECK(!settings::MonitorPrimaryRefusesGameSource(false, false));
}

void TestStartDecision() {
  using settings::DecideMonitorPrimaryStart;
  using S = settings::MonitorPrimaryStart;
  // (requested, current mode, retained game/window source, output active)
  printf("no output running: retained sources are released for the mode\n");
  CHECK(DecideMonitorPrimaryStart(true, false, true, false) == S::kRelease);
  CHECK(DecideMonitorPrimaryStart(true, true, true, false) == S::kRelease);
  CHECK(DecideMonitorPrimaryStart(true, false, false, false) == S::kProceed);
  CHECK(DecideMonitorPrimaryStart(true, true, false, false) == S::kProceed);

  printf("no output running, mode not requested: as before\n");
  CHECK(DecideMonitorPrimaryStart(false, false, true, false) == S::kProceed);
  CHECK(DecideMonitorPrimaryStart(false, false, false, false) == S::kProceed);
  CHECK(DecideMonitorPrimaryStart(false, true, true, false) == S::kProceed);
  CHECK(DecideMonitorPrimaryStart(false, true, false, false) == S::kProceed);

  printf("joining a running capture: same mode proceeds\n");
  CHECK(DecideMonitorPrimaryStart(true, true, false, true) == S::kProceed);
  CHECK(DecideMonitorPrimaryStart(false, false, true, true) == S::kProceed);
  CHECK(DecideMonitorPrimaryStart(false, false, false, true) == S::kProceed);

  printf("joining a running capture: mixing modes is refused\n");
  CHECK(DecideMonitorPrimaryStart(true, true, true, true) == S::kRefuse);
  CHECK(DecideMonitorPrimaryStart(true, false, true, true) == S::kRefuse);
  CHECK(DecideMonitorPrimaryStart(true, false, false, true) == S::kRefuse);
  CHECK(DecideMonitorPrimaryStart(false, true, false, true) == S::kRefuse);
  CHECK(DecideMonitorPrimaryStart(false, true, true, true) == S::kRefuse);
}
}  // namespace

int main(int, char**) {
  TestProtocolKeys();
  TestMethod();
  TestBrbFlag();
  TestMonitorPrimary();
  TestShowsMonitor();
  TestRefusesGameSource();
  TestStartDecision();

  printf(failures ? "FAILED (%d)\n" : "ALL PASSED\n", failures);
  return failures ? 1 : 0;
}
