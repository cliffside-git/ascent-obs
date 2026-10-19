// Noise suppression protocol + filter lifecycle tests.
// Built and run against a local OBS build by scripts/run_tests.ps1.
#include <obs.hpp>
#include <cstdio>
#include <cstring>

#include "communications/protocol.h"
#include "obs_control/obs_audio_source_control.h"

using namespace obs_control;

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

size_t FilterCount(obs_source_t* source) {
  size_t count = 0;
  obs_source_enum_filters(
      source,
      [](obs_source_t*, obs_source_t*, void* param) {
        (*(size_t*)param)++;
      },
      &count);
  return count;
}

bool LoadModule(const char* rundir, const char* name) {
  char bin_path[512];
  char data_path[512];
  snprintf(bin_path, sizeof(bin_path), "%s/obs-plugins/64bit/%s.dll", rundir,
           name);
  snprintf(data_path, sizeof(data_path), "%s/data/obs-plugins/%s", rundir,
           name);

  obs_module_t* module = nullptr;
  if (obs_open_module(&module, bin_path, data_path) != MODULE_SUCCESS) {
    return false;
  }
  return obs_init_module(module);
}

bool ParseEntry(const char* json, bool is_input_device) {
  OBSDataAutoRelease entry = obs_data_create_from_json(json);
  return ParseNoiseSuppression(entry, is_input_device);
}

void TestProtocolKeys() {
  printf("protocol keys\n");
  CHECK(strcmp(libascentobs::protocol::kNoiseSuppressionSupported,
               "noise_suppression_supported") == 0);
  CHECK(strcmp(kNoiseSuppression, "noise_suppression") == 0);
}

void TestParseNoiseSuppression() {
  printf("audio_sources[] 'noise_suppression' parsing\n");
  CHECK(!ParseEntry(R"({"device_id":"default"})", true));
  CHECK(!ParseEntry(R"({"device_id":"default","noise_suppression":false})",
                    true));
  CHECK(ParseEntry(R"({"device_id":"default","noise_suppression":true})",
                   true));
  // output devices ignore it
  CHECK(!ParseEntry(R"({"device_id":"default","noise_suppression":true})",
                    false));
}

void TestFilterLifecycle() {
  printf("filter lifecycle\n");
  OBSWeakSourceAutoRelease weak_filter;
  {
    OBSDataAutoRelease settings = obs_data_create();
    obs_data_set_string(settings, "device_id", "default");
    OBSSourceAutoRelease mic = obs_source_create_private(
        "wasapi_input_capture", "test mic", settings);
    CHECK(mic != nullptr);
    if (!mic) {
      return;
    }

    {
      OBSAudioSourceControl control(mic, "test mic", true);
      control.SetNoiseSuppression(false);  // missing/false: unchanged
      CHECK(FilterCount(mic) == 0);

      control.SetNoiseSuppression(true);
      control.SetNoiseSuppression(true);  // re-config: created once
      CHECK(FilterCount(mic) == 1);

      OBSSourceAutoRelease filter = obs_source_get_filter_by_name(
          mic, "ascentobs noise suppression");
      CHECK(filter != nullptr);
      if (filter) {
        CHECK(strcmp(obs_source_get_id(filter), kNoiseSuppressFilterId) == 0);
        OBSDataAutoRelease filter_settings = obs_source_get_settings(filter);
        CHECK(strcmp(obs_data_get_string(filter_settings, "method"),
                     kNoiseSuppressMethodRNNoise) == 0);
      }

      control.SetNoiseSuppression(false);  // toggle off
      CHECK(FilterCount(mic) == 0);

      control.SetNoiseSuppression(true);  // toggle back on
      CHECK(FilterCount(mic) == 1);

      OBSSourceAutoRelease attached = obs_source_get_filter_by_name(
          mic, "ascentobs noise suppression");
      weak_filter = obs_source_get_weak_source(attached);
    }
    // the source keeps the filter after the control goes away
    CHECK(FilterCount(mic) == 1);
  }

  // releasing the source releases its filter
  obs_wait_for_destroy_queue();
  OBSSourceAutoRelease alive = obs_weak_source_get_source(weak_filter);
  CHECK(alive == nullptr);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: %s <obs rundir>\n", argv[0]);
    return 2;
  }
  const char* rundir = argv[1];

  if (!obs_startup("en-US", nullptr, nullptr)) {
    printf("FAIL: obs_startup\n");
    return 1;
  }

  obs_audio_info audio_info = {48000, SPEAKERS_STEREO};
  CHECK(obs_reset_audio(&audio_info));

  TestProtocolKeys();
  TestParseNoiseSuppression();

  printf("capability (obs-filters not loaded)\n");
  CHECK(!OBSAudioSourceControl::IsNoiseSuppressionSupported());

  CHECK(LoadModule(rundir, "obs-filters"));
  CHECK(LoadModule(rundir, "win-wasapi"));

  printf("capability (obs-filters loaded)\n");
  CHECK(OBSAudioSourceControl::IsNoiseSuppressionSupported());

  TestFilterLifecycle();

  obs_shutdown();

  printf(failures ? "FAILED (%d)\n" : "ALL PASSED\n", failures);
  return failures ? 1 : 0;
}
