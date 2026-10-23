// 'track_names' protocol parsing tests.
// Built and run against a local OBS build by scripts/run_tests.ps1.
#include <windows.h>
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

// independent of the sanitizer: Windows' own strict UTF-8 decoder
bool ValidUtf8(const std::string& s) {
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                             (int)s.size(), nullptr, 0) > 0;
}

std::string Repeat(const std::string& s, size_t count) {
  std::string result;
  for (size_t i = 0; i < count; i++) {
    result += s;
  }
  return result;
}

void TestLength() {
  const size_t max = settings::kTrackNameMaxBytes;
  printf("names are capped at %zu bytes\n", max);
  CHECK(max == 64);
  CHECK(settings::SanitizeTrackName(Repeat("a", 64).c_str()) == Repeat("a", 64));
  CHECK(settings::SanitizeTrackName(Repeat("a", 65).c_str()) == Repeat("a", 64));
  CHECK(settings::SanitizeTrackName(Repeat("a", 100000).c_str()).size() == max);

  printf("the cap never splits a UTF-8 character\n");
  const std::string zhong = "\xE4\xB8\xAD";        // U+4E2D, 3 bytes
  const std::string grin = "\xF0\x9F\x98\x80";     // U+1F600, 4 bytes
  auto cjk = settings::SanitizeTrackName(Repeat(zhong, 22).c_str());  // 66
  CHECK(cjk == Repeat(zhong, 21));                                    // 63
  CHECK(ValidUtf8(cjk));
  CHECK(settings::SanitizeTrackName(Repeat(grin, 16).c_str()) ==
        Repeat(grin, 16));  // exactly 64
  auto emoji = settings::SanitizeTrackName(("a" + Repeat(grin, 16)).c_str());
  CHECK(emoji == "a" + Repeat(grin, 15));  // 61, the 16th would make 65
  CHECK(ValidUtf8(emoji));

  printf("six max-length names all fit\n");
  std::string json = R"({"track_names":[)";
  for (int i = 0; i < 6; i++) {
    json += std::string(i ? "," : "") + R"({"name":")" + Repeat("x", 80) +
            R"("})";
  }
  json += "]}";
  auto names = Parse(json.c_str());
  bool all_max = names.size() == kTracks;
  for (auto& name : names) {
    all_max = all_max && name == Repeat("x", 64);
  }
  CHECK(all_max);

  printf("an oversized JSON name is cut, not rejected\n");
  names = Parse((R"({"track_names":[{"name":")" + Repeat(zhong, 10000) +
                 R"("},{"name":"Game"}]})")
                    .c_str());
  CHECK(names[0] == Repeat(zhong, 21));
  CHECK(names[1] == "Game");
}

void TestSanitize() {
  printf("control characters, quotes and backslashes are dropped\n");
  CHECK(settings::SanitizeTrackName("Ga\tme\r\n") == "Game");
  CHECK(settings::SanitizeTrackName("\x01\x1F\x7F").empty());
  CHECK(settings::SanitizeTrackName("\xC2\x85Mic\xC2\x9F") == "Mic");  // C1
  CHECK(settings::SanitizeTrackName("Game \"Audio\" \\") == "Game Audio");
  CHECK(settings::SanitizeTrackName("Mic\\") == "Mic");
  // U+00A0 is the first character after C1 and stays
  CHECK(settings::SanitizeTrackName("a\xC2\xA0" "b") == "a\xC2\xA0" "b");

  printf("spaces are trimmed; nothing left -> default name\n");
  CHECK(settings::SanitizeTrackName("  Game  ") == "Game");
  CHECK(settings::SanitizeTrackName("   ").empty());
  CHECK(settings::SanitizeTrackName(nullptr).empty());

  printf("invalid UTF-8 is dropped byte by byte\n");
  CHECK(settings::SanitizeTrackName("\xFFGame\xC3") == "Game");
  CHECK(settings::SanitizeTrackName("\xC0\xAFx") == "x");          // overlong
  CHECK(settings::SanitizeTrackName("\xED\xA0\x80x") == "x");      // surrogate
  CHECK(settings::SanitizeTrackName("\xF4\x90\x80\x80x") == "x");  // > U+10FFFF

  printf("sanitizing applies to parsed names\n");
  auto names = Parse(R"({"track_names":[{"name":"\tDis\"cord\u0007 "},{"name":"\u0001"}]})");
  CHECK(names[0] == "Discord");
  CHECK(names[1].empty());
}
}  // namespace

int main(int, char**) {
  TestProtocolKey();
  TestMissing();
  TestNames();
  TestLimits();
  TestLength();
  TestSanitize();

  printf(failures ? "FAILED (%d)\n" : "ALL PASSED\n", failures);
  return failures ? 1 : 0;
}
