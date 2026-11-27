// Window and monitor handles keep their sign from the desktop's payload to
// the Win32 call. Windows keeps 32 significant bits in a handle and 64-bit
// code sees it sign-extended: a monitor the desktop sends as -1573583461 is
// HMONITOR 0xffffffffa235059b, not 0x00000000a235059b (F5, sweep 2026-11-26:
// a Windows 10 follow-window recording found no monitor and was BRB for the
// whole match).
// Built and run against a local OBS build by scripts/run_tests.ps1.
#include <windows.h>
#include <obs.hpp>
#include <cstdio>
#include <string>
#include <vector>

#include "obs_control/settings.h"
#include "obs_control/scene/follow_window_filter.h"
#include "obs_control/scene/follow_window_win32.h"

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

// Havok's fourth monitor, as the desktop sent it ("monitor_handle")
const long long kTopBit = -1573583461ll;
// the same handle from a sender that zero-extended it (0xa235059b)
const long long kTopBitZeroExtended = 0xa235059bll;

// what Windows gives 64-bit code for a handle value
uint64_t Bits(long long value) {
  return (uint64_t)(uintptr_t)(HMONITOR)(intptr_t)(int32_t)value;
}

int64_t Parse(const char* key, long long value) {
  OBSDataAutoRelease data = obs_data_create();
  obs_data_set_int(data, key, value);
  return settings::ParseHandle(data, key);
}

// the settings carry the handle the way 1.3.2's duplicator read it, a plain
// (HMONITOR) cast of the integer, so they must hold the sign-extended value
uint64_t DuplicatorBits(int64_t handle) {
  OBSDataAutoRelease out = obs_data_create();
  settings::FillMonitorCaptureSettings(out, handle, 3, true, 1);
  return (uint64_t)(uintptr_t)(HMONITOR)obs_data_get_int(out,
                                                         "monitor_handle");
}

uint64_t FilterBits(int64_t handle) {
  obs_source_t* filter = follow_window::CreateFilter(0, handle, true);
  uint64_t bits = follow_window::MonitorBits(filter);
  obs_source_release(filter);
  return bits;
}

uint64_t FactsBits(int64_t handle) {
  follow_window::Win32Facts facts(0, handle);
  return facts.monitor_bits();
}

uint64_t Follow(long long value) {
  OBSDataAutoRelease monitor = obs_data_create();
  obs_data_set_int(monitor, settings::kSettingsMonitorFollowWindow, value);
  return settings::ParseMonitorFollowWindow(monitor);
}

void TestSettings() {
  printf("settings: the handle keeps its sign\n");
  CHECK(Bits(kTopBit) == 0xffffffffa235059bull);
  CHECK(Parse("monitor_handle", kTopBit) == kTopBit);
  CHECK(Parse("monitor_handle", kTopBitZeroExtended) == kTopBit);
  CHECK(Parse("monitor_handle", 65537) == 65537);
  CHECK(Parse("monitor_handle", 0) == 0);
  CHECK(Parse("window_handle", kTopBit) == kTopBit);
  CHECK(Parse("window_handle", 264748) == 264748);
}

void TestDuplicator() {
  printf("duplicator: monitor_capture gets the sign-extended handle\n");
  CHECK(DuplicatorBits(Parse("monitor_handle", kTopBit)) == Bits(kTopBit));
  CHECK(DuplicatorBits(Parse("monitor_handle", kTopBitZeroExtended)) ==
        Bits(kTopBit));
  CHECK(DuplicatorBits(Parse("monitor_handle", 65537)) == 65537ull);
}

void TestFilter() {
  printf("filter: the guard reads the sign-extended handle\n");
  CHECK(FilterBits(Parse("monitor_handle", kTopBit)) == Bits(kTopBit));
  CHECK(FilterBits(Parse("monitor_handle", kTopBitZeroExtended)) ==
        Bits(kTopBit));
  CHECK(FilterBits(Parse("monitor_handle", 65537)) == 65537ull);
}

void TestWin32Facts() {
  printf("Win32Facts: GetMonitorInfo is asked about the real handle\n");
  CHECK(FactsBits(Parse("monitor_handle", kTopBit)) == Bits(kTopBit));
  CHECK(FactsBits(Parse("monitor_handle", kTopBitZeroExtended)) ==
        Bits(kTopBit));
  CHECK(FactsBits(Parse("monitor_handle", 65537)) == 65537ull);
}

BOOL CALLBACK CollectMonitor(HMONITOR handle, HDC, LPRECT, LPARAM param) {
  reinterpret_cast<std::vector<HMONITOR>*>(param)->push_back(handle);
  return TRUE;
}

// every monitor on this PC, sent the way the desktop sends it (handle.0 as
// isize), reaches each reader as the handle Windows gave
void TestThisPcsMonitors() {
  printf("this PC's monitors round-trip\n");
  std::vector<HMONITOR> monitors;
  EnumDisplayMonitors(NULL, NULL, CollectMonitor, (LPARAM)&monitors);
  CHECK(!monitors.empty());
  for (HMONITOR h : monitors) {
    long long sent = (long long)(intptr_t)h;
    uint64_t want = (uint64_t)(uintptr_t)h;
    printf("  monitor 0x%llx\n", (unsigned long long)want);
    int64_t parsed = Parse("monitor_handle", sent);
    CHECK(DuplicatorBits(parsed) == want);
    CHECK(FilterBits(parsed) == want);
    CHECK(FactsBits(parsed) == want);
  }
}

void TestFollowWindow() {
  printf("follow_window: a sign-extended HWND is a window\n");
  CHECK(Follow(kTopBit) == (uint64_t)(int64_t)kTopBit);
  CHECK(Follow(kTopBitZeroExtended) == (uint64_t)(int64_t)kTopBit);
  CHECK(Follow(264748) == 264748ull);
  // beyond 32 bits either way is not a handle
  CHECK(Follow(0x100000000ll) == 0);
  CHECK(Follow(-0x80000001ll) == 0);
}
}  // namespace

int main(int, char**) {
  TestSettings();
  TestDuplicator();
  TestWin32Facts();
  TestFollowWindow();

  if (!obs_startup("en-US", nullptr, nullptr)) {
    printf("FAIL: obs_startup\n");
    return 1;
  }
  TestFilter();
  TestThisPcsMonitors();
  obs_shutdown();

  printf(failures ? "FAILED (%d)\n" : "ALL PASSED\n", failures);
  return failures ? 1 : 0;
}
