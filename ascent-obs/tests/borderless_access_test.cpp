// WGC borderless access: what Windows answered "Ascent OBS" about hiding the
// capture border, as event 22 and the QUERY_MACHINE_INFO field, plus source
// pins on libobs-winrt (no test compiles that module).
// Built and run against a local OBS build by scripts/run_tests.ps1.
#include <windows.h>
#include <obs.hpp>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "communications/protocol.h"
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

namespace protocol = libascentobs::protocol;
using settings::BorderlessAccessProbe;

BorderlessAccessProbe Probe(int status, const char* site, int hresult = 0) {
  BorderlessAccessProbe probe;
  probe.status = status;
  probe.hresult = hresult;
  probe.site = site;
  return probe;
}

bool Is(const char* a, const char* b) {
  return a && b && strcmp(a, b) == 0;
}

void TestProtocol() {
  printf("event 22 and the machine-info key (mirrored in ascent-obs-comm)\n");
  CHECK(protocol::events::OBS_WARNING == 21);
  CHECK(protocol::events::WGC_BORDERLESS_ACCESS == 22);
  CHECK(Is(protocol::kWgcBorderlessAccess, "wgc_borderless_access"));
  CHECK(Is(settings::kBorderlessAccessSignal, "wgc_borderless_access"));
  CHECK(Is(settings::kBorderlessAccessSiteVideoStart, "video_start"));
}

void TestNames() {
  printf("Windows' AppCapabilityAccessStatus names\n");
  CHECK(Is(settings::BorderlessAccessName(-2), "not asked"));
  CHECK(Is(settings::BorderlessAccessName(-1), "no answer"));
  CHECK(Is(settings::BorderlessAccessName(0), "DeniedBySystem"));
  CHECK(Is(settings::BorderlessAccessName(1), "NotDeclaredByApp"));
  CHECK(Is(settings::BorderlessAccessName(2), "DeniedByUser"));
  CHECK(Is(settings::BorderlessAccessName(3), "UserPromptRequired"));
  CHECK(Is(settings::BorderlessAccessName(4), "Allowed"));
  CHECK(Is(settings::BorderlessAccessName(5), "unknown"));
  CHECK(Is(settings::BorderlessAccessName(-3), "unknown"));
  CHECK(settings::kBorderlessAccessNotAsked == -2);
  CHECK(settings::kBorderlessAccessNoAnswer == -1);
  CHECK(settings::kBorderlessAccessDeniedBySystem == 0);
  CHECK(settings::kBorderlessAccessAllowed == 4);
}

void TestEventPayload() {
  printf("event payload: access and site, no identifier\n");
  {
    OBSDataAutoRelease out = obs_data_create();
    settings::FillBorderlessAccessEvent(out, Probe(2, "video_start"));
    CHECK(obs_data_has_user_value(out, "access"));
    CHECK(obs_data_get_int(out, "access") == 2);
    CHECK(Is(obs_data_get_string(out, "site"), "video_start"));
    CHECK(!obs_data_has_user_value(out, "hresult"));
    CHECK(!obs_data_has_user_value(out, protocol::kCommandIdentifier));

    obs_data_set_int(out, protocol::kEventField,
                     protocol::events::WGC_BORDERLESS_ACCESS);
    OBSDataAutoRelease back =
        obs_data_create_from_json(obs_data_get_json(out));
    CHECK(back != nullptr);
    CHECK(obs_data_get_int(back, protocol::kEventField) == 22);
    CHECK(obs_data_get_int(back, "access") == 2);
    CHECK(Is(obs_data_get_string(back, "site"), "video_start"));
  }

  printf("Allowed (4) and DeniedBySystem (0) are sent as themselves\n");
  {
    OBSDataAutoRelease out = obs_data_create();
    settings::FillBorderlessAccessEvent(out, Probe(0, "window"));
    CHECK(obs_data_has_user_value(out, "access"));
    CHECK(obs_data_get_int(out, "access") == 0);
    CHECK(Is(obs_data_get_string(out, "site"), "window"));
  }

  printf("a request that threw: -1 with its hresult\n");
  {
    OBSDataAutoRelease out = obs_data_create();
    settings::FillBorderlessAccessEvent(
        out, Probe(-1, "monitor", static_cast<int>(0x80070005)));
    CHECK(obs_data_get_int(out, "access") == -1);
    CHECK(Is(obs_data_get_string(out, "site"), "monitor"));
    CHECK(obs_data_has_user_value(out, "hresult"));
    CHECK(obs_data_get_int(out, "hresult") ==
          static_cast<int>(0x80070005));
  }

  printf("a timed-out request: -1 without hresult\n");
  {
    OBSDataAutoRelease out = obs_data_create();
    settings::FillBorderlessAccessEvent(out, Probe(-1, "video_start"));
    CHECK(obs_data_get_int(out, "access") == -1);
    CHECK(!obs_data_has_user_value(out, "hresult"));
  }
}

void TestDueRule() {
  using settings::BorderlessAccessEventDue;
  printf("video_start is always sent (one per START)\n");
  CHECK(BorderlessAccessEventDue(Probe(2, "video_start"), -2));
  CHECK(BorderlessAccessEventDue(Probe(2, "video_start"), 2));
  CHECK(BorderlessAccessEventDue(Probe(4, "video_start"), 4));

  printf("a capture start only when the answer changed\n");
  CHECK(BorderlessAccessEventDue(Probe(2, "window"), -2));
  CHECK(!BorderlessAccessEventDue(Probe(2, "window"), 2));
  CHECK(BorderlessAccessEventDue(Probe(4, "window"), 2));
  CHECK(!BorderlessAccessEventDue(Probe(2, "monitor"), 2));
  CHECK(BorderlessAccessEventDue(Probe(-1, "monitor"), 2));
  CHECK(!BorderlessAccessEventDue(Probe(-1, "monitor"), -1));
}

void TestMachineInfo() {
  printf("machine info carries Windows' answer, 0..4 only\n");
  for (int status = 0; status <= 4; status++) {
    OBSDataAutoRelease out = obs_data_create();
    settings::FillBorderlessAccessMachineInfo(out, status);
    CHECK(obs_data_has_user_value(out, protocol::kWgcBorderlessAccess));
    CHECK(obs_data_get_int(out, protocol::kWgcBorderlessAccess) == status);
  }
  for (int status : {-2, -1, 5, 7}) {
    OBSDataAutoRelease out = obs_data_create();
    settings::FillBorderlessAccessMachineInfo(out, status);
    CHECK(!obs_data_has_user_value(out, protocol::kWgcBorderlessAccess));
  }
}

struct Heard {
  int calls = 0;
  BorderlessAccessProbe probe;
};

void OnSignal(void* data, calldata_t* params) {
  Heard* heard = static_cast<Heard*>(data);
  heard->calls++;
  heard->probe = settings::ReadBorderlessAccessSignal(params);
}

void Raise(int status, int hresult, const char* site) {
  calldata_t cd = {0};
  calldata_set_int(&cd, "status", status);
  calldata_set_int(&cd, "hresult", hresult);
  calldata_set_string(&cd, "site", site);
  signal_handler_signal(obs_get_signal_handler(),
                        settings::kBorderlessAccessSignal, &cd);
  calldata_free(&cd);
}

void TestSignalRoundTrip() {
  printf("global signal: libobs-winrt raises, the exe reads\n");
  signal_handler_t* signals = obs_get_signal_handler();
  CHECK(signals != nullptr);

  // plain OBS never declares it: libobs-winrt's raise is a no-op there
  Raise(2, 0, "video_start");

  CHECK(signal_handler_add(signals, settings::kBorderlessAccessSignalDecl));
  Heard heard;
  signal_handler_connect(signals, settings::kBorderlessAccessSignal, OnSignal,
                         &heard);

  Raise(2, 0, "video_start");
  CHECK(heard.calls == 1);
  CHECK(heard.probe.status == 2);
  CHECK(heard.probe.hresult == 0);
  CHECK(heard.probe.site == "video_start");

  Raise(-1, static_cast<int>(0x80004005), "window");
  CHECK(heard.calls == 2);
  CHECK(heard.probe.status == -1);
  CHECK(heard.probe.hresult == static_cast<int>(0x80004005));
  CHECK(heard.probe.site == "window");

  printf("missing params read as no answer from an unknown site\n");
  calldata_t empty = {0};
  signal_handler_signal(signals, settings::kBorderlessAccessSignal, &empty);
  calldata_free(&empty);
  CHECK(heard.calls == 3);
  CHECK(heard.probe.status == settings::kBorderlessAccessNoAnswer);
  CHECK(heard.probe.hresult == 0);
  CHECK(heard.probe.site == "unknown");

  signal_handler_disconnect(signals, settings::kBorderlessAccessSignal,
                            OnSignal, &heard);
  Raise(4, 0, "monitor");
  CHECK(heard.calls == 3);
}

std::string ReadWinrtCapture() {
  std::string here = __FILE__;
  size_t slash = here.find_last_of("\\/");
  std::string tests_dir = slash == std::string::npos ? "." : here.substr(0, slash);
  std::ifstream file(tests_dir +
                     "/../../obs-studio/libobs-winrt/winrt-capture.cpp");
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

std::vector<size_t> FindAll(const std::string& text, const std::string& what) {
  std::vector<size_t> at;
  for (size_t pos = text.find(what); pos != std::string::npos;
       pos = text.find(what, pos + 1)) {
    at.push_back(pos);
  }
  return at;
}

// the body of the function whose definition starts with |signature|
std::string FunctionBody(const std::string& text, const std::string& signature) {
  size_t start = text.find(signature);
  if (start == std::string::npos) {
    return "";
  }
  size_t end = text.find("\n}\n", start);
  return text.substr(start, end == std::string::npos ? std::string::npos
                                                     : end - start);
}

void TestWinrtCaptureSource() {
  printf("libobs-winrt reports before it hides the border\n");
  const std::string source = ReadWinrtCapture();
  CHECK(!source.empty());

  CHECK(source.find(std::string("\"") + settings::kBorderlessAccessSignal +
                    "\"") != std::string::npos);

  const std::vector<size_t> hides = FindAll(source, "IsBorderRequired(false)");
  CHECK(hides.size() == 2);
  size_t previous_hide = 0;
  for (size_t hide : hides) {
    size_t report = source.rfind("report_borderless_access(", hide);
    CHECK(report != std::string::npos && report > previous_hide &&
          hide - report < 200);
    previous_hide = hide;
  }

  CHECK(FunctionBody(source, "EXPORT void winrt_capture_thread_start()")
            .find("report_borderless_access(") != std::string::npos);

  printf("the one request to Windows is inside the never-throwing helper\n");
  const std::vector<size_t> requests = FindAll(source, "RequestAccessAsync(");
  CHECK(requests.size() == 1);
  const std::string helper =
      FunctionBody(source, "static int request_borderless_access(");
  CHECK(helper.find("noexcept") != std::string::npos);
  CHECK(helper.find("RequestAccessAsync(") != std::string::npos);
  CHECK(FunctionBody(source, "static void report_borderless_access(")
            .find("noexcept") != std::string::npos);
}
}  // namespace

int main(int, char**) {
  TestProtocol();
  TestNames();
  TestEventPayload();
  TestDueRule();
  TestMachineInfo();
  TestWinrtCaptureSource();

  if (!obs_startup("en-US", nullptr, nullptr)) {
    printf("FAIL: obs_startup\n");
    return 1;
  }
  TestSignalRoundTrip();
  obs_shutdown();

  printf(failures ? "FAILED (%d)\n" : "ALL PASSED\n", failures);
  return failures ? 1 : 0;
}
