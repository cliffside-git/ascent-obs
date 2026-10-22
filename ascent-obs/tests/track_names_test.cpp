// 'track_names' protocol parsing tests.
// Built and run against a local OBS build by scripts/run_tests.ps1.
#include <obs.hpp>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

const size_t kTracks = 6;

std::vector<std::string> Parse(const char* extra_options_json) {
  OBSDataAutoRelease extra = obs_data_create_from_json(extra_options_json);
  return settings::ParseTrackNames(extra, kTracks);
}

bool AllDefault(const std::vector<std::string>& names) {
  if (names.size() != kTracks) {
    return false;
  }
  for (auto& name : names) {
    if (!name.empty()) {
      return false;
    }
  }
  return true;
}

void TestProtocolKey() {
  printf("protocol key\n");
  CHECK(strcmp(settings::kSettingsTrackNames, "track_names") == 0);
}

void TestMissing() {
  printf("missing 'track_names' keeps the default names\n");
  CHECK(AllDefault(settings::ParseTrackNames(nullptr, kTracks)));
  CHECK(AllDefault(Parse(R"({"separate_tracks":true,"tracks":7})")));
  CHECK(AllDefault(Parse(R"({"track_names":[]})")));
}

void TestNames() {
  printf("names map to mixers by position\n");
  auto names = Parse(
      R"({"track_names":[{"name":"Master"},{"name":"Game"},{"name":"Microphone"}]})");
  CHECK(names.size() == kTracks);
  CHECK(names[0] == "Master");
  CHECK(names[1] == "Game");
  CHECK(names[2] == "Microphone");
  CHECK(names[3].empty() && names[4].empty() && names[5].empty());

  // {} and "" keep a slot on its default name
  names = Parse(
      R"({"track_names":[{"name":"Master"},{},{"name":""},{"name":"Discord"}]})");
  CHECK(names[1].empty());
  CHECK(names[2].empty());
  CHECK(names[3] == "Discord");

  // UTF-8 passes through unchanged (JSON \u2013 == U+2013 EN DASH)
  names = Parse(R"({"track_names":[{"name":"Spiel \u2013 Ton"}]})");
  CHECK(names[0] == "Spiel \xE2\x80\x93 Ton");
}

void TestLimits() {
  printf("more names than mixers are ignored\n");
  auto names = Parse(
      R"({"track_names":[{"name":"1"},{"name":"2"},{"name":"3"},{"name":"4"},)"
      R"({"name":"5"},{"name":"6"},{"name":"7"}]})");
  CHECK(names.size() == kTracks);
  CHECK(names[5] == "6");

  printf("non-object entries are dropped by obs_data (documented shift)\n");
  names = Parse(R"({"track_names":[{"name":"Master"},"Game",{"name":"Mic"}]})");
  CHECK(names[0] == "Master");
  CHECK(names[1] == "Mic");
  CHECK(names[2].empty());

  printf("a non-string name falls back to the default\n");
  names = Parse(R"({"track_names":[{"name":5},{"name":"Game"}]})");
  CHECK(names[0].empty());
  CHECK(names[1] == "Game");
}
}  // namespace

int main(int, char**) {
  TestProtocolKey();
  TestMissing();
  TestNames();
  TestLimits();

  printf(failures ? "FAILED (%d)\n" : "ALL PASSED\n", failures);
  return failures ? 1 : 0;
}
