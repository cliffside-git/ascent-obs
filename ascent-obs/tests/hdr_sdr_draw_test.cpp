// SDR recording of an HDR (scRGB) desktop, ticket 1278: the DISPLAYCONFIG
// SDR white level mapping, the draw chosen by gs_scrgb_capture_draw, the
// ticket's HUD colours through a CPU copy of color.effect / default.effect,
// and the same draw on the GPU through libobs-d3d11.
// Built and run against a local OBS build by scripts/run_tests.ps1.
#include <windows.h>
#include <DirectXPackedVector.h>
#include <obs.hpp>
#include <graphics/scrgb-draw.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

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

struct Rgb {
  double r, g, b;
};

// 1278's HUD, sampled from the user's Before and After screenshots
const Rgb kBefore[] = {
    {207.9, 225.5, 225.3},  // white HUD text
    {21.3, 160.7, 24.8},    // health bar green
    {224, 224, 224},
    {119, 119, 119},
};
const Rgb kAfter[] = {
    {174.1, 181.3, 183.4},
    {61.6, 145.6, 43.1},
};
constexpr float kVideoWhite = 300.f;  // the app's sdr_white
const float kWindowsWhites[] = {80.f, 168.f, 200.f, 240.f, 280.f, 300.f, 480.f};

double SrgbToLinear(double u) {
  return (u <= 0.04045) ? (u / 12.92) : pow((u + 0.055) / 1.055, 2.4);
}

double LinearToSrgb(double u) {
  return (u <= 0.0031308) ? (12.92 * u) : ((1.055 * pow(u, 1. / 2.4)) - 0.055);
}

Rgb Map(Rgb v, double (*f)(double)) { return {f(v.r), f(v.g), f(v.b)}; }

Rgb Rec709ToRec2020(Rgb v) {
  return {0.62740389593469903 * v.r + 0.32928303837788370 * v.g +
              0.043313065687417225 * v.b,
          0.069097289358232075 * v.r + 0.91954039507545871 * v.g +
              0.011362315566309178 * v.b,
          0.016391438875150280 * v.r + 0.088013307877225749 * v.g +
              0.89559525324762401 * v.b};
}

Rgb Rec2020ToRec709(Rgb v) {
  return {1.6604910021084345 * v.r - 0.58764113878854951 * v.g -
              0.072849863319884883 * v.b,
          -0.12455047452159074 * v.r + 1.1328998971259603 * v.g -
              0.0083494226043694768 * v.b,
          -0.018150763354905303 * v.r - 0.10057889800800739 * v.g +
              1.1187296613629127 * v.b};
}

double Saturate(double u) { return u < 0. ? 0. : (u > 1. ? 1. : u); }

double ReinhardChannel(double u) {
  u /= u + 1.;
  u = Saturate(u);
  u = pow(u, 1. / 2.4);
  return SrgbToLinear(u);
}

// what Windows hands a WGC/DXGI capture for an SDR colour on an HDR desktop
// whose "SDR content brightness" is windows_white nits
Rgb DesktopScrgb(Rgb srgb8, double windows_white) {
  const Rgb lin = Map({srgb8.r / 255., srgb8.g / 255., srgb8.b / 255.},
                      SrgbToLinear);
  const double k = windows_white / 80.;
  return {lin.r * k, lin.g * k, lin.b * k};
}

// default.effect's Draw / DrawMultiply / DrawMultiplyTonemap
bool ApplyDraw(const gs_scrgb_draw& draw, Rgb v, Rgb* out) {
  const bool multiply = strcmp(draw.tech, "DrawMultiply") == 0 ||
                        strcmp(draw.tech, "DrawMultiplyTonemap") == 0;
  const bool tonemap = strcmp(draw.tech, "DrawMultiplyTonemap") == 0;
  if (!multiply && strcmp(draw.tech, "Draw") != 0) return false;
  if (multiply) v = {v.r * draw.multiplier, v.g * draw.multiplier,
                     v.b * draw.multiplier};
  if (tonemap)
    v = Rec2020ToRec709(Map(Rec709ToRec2020(v), ReinhardChannel));
  *out = v;
  return true;
}

// an sRGB framebuffer write: clamp, then encode
Rgb EncodeSrgb8(Rgb v) {
  const Rgb s = Map(Map(v, Saturate), LinearToSrgb);
  return {s.r * 255., s.g * 255., s.b * 255.};
}

Rgb Recorded(Rgb before, float windows_white, float monitor_white) {
  const gs_scrgb_draw draw =
      gs_scrgb_capture_draw(GS_CS_SRGB, monitor_white, kVideoWhite);
  Rgb lin{};
  if (!ApplyDraw(draw, DesktopScrgb(before, windows_white), &lin))
    return {-1000, -1000, -1000};
  return EncodeSrgb8(lin);
}

double MaxDiff(Rgb a, Rgb b) {
  return fmax(fabs(a.r - b.r), fmax(fabs(a.g - b.g), fabs(a.b - b.b)));
}

double Rms(const Rgb* got, const Rgb* want, size_t n) {
  double sum = 0.;
  for (size_t i = 0; i < n; i++) {
    sum += (got[i].r - want[i].r) * (got[i].r - want[i].r) +
           (got[i].g - want[i].g) * (got[i].g - want[i].g) +
           (got[i].b - want[i].b) * (got[i].b - want[i].b);
  }
  return sqrt(sum / (3. * n));
}

void PrintRgb(const char* label, Rgb v) {
  printf("    %s (%.1f, %.1f, %.1f)\n", label, v.r, v.g, v.b);
}

bool Is(gs_scrgb_draw draw, const char* tech, float multiplier) {
  return strcmp(draw.tech, tech) == 0 &&
         fabsf(draw.multiplier - multiplier) < 1e-6f;
}

void TestWhiteLevel() {
  printf("DISPLAYCONFIG SDRWhiteLevel -> nits\n");
  CHECK(gs_sdr_white_nits(true, 1000) == 80);
  CHECK(gs_sdr_white_nits(true, 3750) == 300);
  CHECK(gs_sdr_white_nits(true, 6000) == 480);

  printf("query failed -> unknown (0)\n");
  CHECK(gs_sdr_white_nits(false, 3750) == 0);
  CHECK(gs_sdr_white_nits(false, 0) == 0);
}

void TestDecision() {
  printf("SDR output, monitor white known -> plain multiply by 80/white\n");
  CHECK(Is(gs_scrgb_capture_draw(GS_CS_SRGB, 300.f, 300.f), "DrawMultiply",
           80.f / 300.f));
  CHECK(Is(gs_scrgb_capture_draw(GS_CS_SRGB, 480.f, 300.f), "DrawMultiply",
           80.f / 480.f));
  CHECK(Is(gs_scrgb_capture_draw(GS_CS_SRGB_16F, 200.f, 300.f),
           "DrawMultiply", 80.f / 200.f));

  printf("SDR output, monitor white unknown -> tonemap as before\n");
  CHECK(Is(gs_scrgb_capture_draw(GS_CS_SRGB, 0.f, 300.f),
           "DrawMultiplyTonemap", 80.f / 300.f));
  CHECK(Is(gs_scrgb_capture_draw(GS_CS_SRGB_16F, 0.f, 300.f),
           "DrawMultiplyTonemap", 80.f / 300.f));

  printf("HDR outputs unchanged\n");
  CHECK(Is(gs_scrgb_capture_draw(GS_CS_709_EXTENDED, 200.f, 300.f),
           "DrawMultiply", 80.f / 300.f));
  CHECK(Is(gs_scrgb_capture_draw(GS_CS_709_EXTENDED, 0.f, 300.f),
           "DrawMultiply", 80.f / 300.f));
  CHECK(Is(gs_scrgb_capture_draw(GS_CS_709_SCRGB, 200.f, 300.f), "Draw",
           1.f));
}

void TestRegressionTable() {
  printf("1278 Before colours through an HDR desktop at each Windows SDR "
         "brightness come back unchanged (within 0.5/255)\n");
  for (float w : kWindowsWhites) {
    for (const Rgb& before : kBefore) {
      const Rgb got = Recorded(before, w, w);
      printf("  W=%.0f", w);
      PrintRgb("got", got);
      CHECK(MaxDiff(got, before) <= 0.5);
    }
  }
}

void TestUnknownWhiteFallback() {
  printf("unknown monitor white at W=300 reproduces 1278's After "
         "(within 4/255 RMS)\n");
  Rgb got[2];
  for (int i = 0; i < 2; i++) {
    got[i] = Recorded(kBefore[i], 300.f, 0.f);
    PrintRgb("got", got[i]);
  }
  const double rms = Rms(got, kAfter, 2);
  printf("    rms %.2f\n", rms);
  CHECK(rms <= 4.);
}

bool StartVideo() {
  obs_video_info ovi = {};
  ovi.graphics_module = "libobs-d3d11";
  ovi.fps_num = 30;
  ovi.fps_den = 1;
  ovi.base_width = ovi.output_width = 64;
  ovi.base_height = ovi.output_height = 64;
  ovi.output_format = VIDEO_FORMAT_NV12;
  ovi.colorspace = VIDEO_CS_709;
  ovi.range = VIDEO_RANGE_PARTIAL;
  ovi.adapter = 0;
  ovi.gpu_conversion = true;
  ovi.scale_type = OBS_SCALE_BICUBIC;
  if (obs_reset_video(&ovi) != OBS_VIDEO_SUCCESS) return false;
  obs_set_video_levels(kVideoWhite, 1000.f);
  return true;
}

// winrt_capture_render's draw of an FP16 capture into an sRGB render target
bool GpuRecord(float windows_white, float monitor_white,
               std::vector<Rgb>* out) {
  const uint32_t n = (uint32_t)(sizeof(kBefore) / sizeof(kBefore[0]));
  using DirectX::PackedVector::XMConvertFloatToHalf;
  std::vector<DirectX::PackedVector::HALF> pixels;
  for (const Rgb& before : kBefore) {
    const Rgb v = DesktopScrgb(before, windows_white);
    pixels.push_back(XMConvertFloatToHalf((float)v.r));
    pixels.push_back(XMConvertFloatToHalf((float)v.g));
    pixels.push_back(XMConvertFloatToHalf((float)v.b));
    pixels.push_back(XMConvertFloatToHalf(1.f));
  }
  const uint8_t* data = (const uint8_t*)pixels.data();

  bool ok = false;
  gs_texture_t* texture = gs_texture_create(n, 1, GS_RGBA16F, 1, &data, 0);
  gs_texrender_t* texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
  gs_stagesurf_t* stage = gs_stagesurface_create(n, 1, GS_RGBA);
  if (texture && texrender && stage &&
      gs_texrender_begin_with_color_space(texrender, n, 1, GS_CS_SRGB)) {
    vec4 clear;
    vec4_zero(&clear);
    gs_clear(GS_CLEAR_COLOR, &clear, 0.f, 0);
    gs_ortho(0.f, (float)n, 0.f, 1.f, -100.f, 100.f);

    const gs_scrgb_draw draw = gs_scrgb_capture_draw(
        gs_get_color_space(), monitor_white, obs_get_video_sdr_white_level());
    gs_effect_t* effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_technique_t* tech = gs_effect_get_technique(effect, draw.tech);

    const bool previous = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(true);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"),
                               texture);
    gs_effect_set_float(gs_effect_get_param_by_name(effect, "multiplier"),
                        draw.multiplier);
    const size_t passes = gs_technique_begin(tech);
    for (size_t i = 0; i < passes; i++) {
      if (gs_technique_begin_pass(tech, i)) {
        gs_draw_sprite(texture, 0, 0, 0);
        gs_technique_end_pass(tech);
      }
    }
    gs_technique_end(tech);
    gs_blend_state_pop();
    gs_enable_framebuffer_srgb(previous);
    gs_texrender_end(texrender);

    gs_stage_texture(stage, gs_texrender_get_texture(texrender));
    uint8_t* mapped;
    uint32_t linesize;
    if (gs_stagesurface_map(stage, &mapped, &linesize)) {
      out->clear();
      for (uint32_t i = 0; i < n; i++) {
        out->push_back({(double)mapped[i * 4], (double)mapped[i * 4 + 1],
                        (double)mapped[i * 4 + 2]});
      }
      gs_stagesurface_unmap(stage);
      ok = true;
    }
  }
  gs_stagesurface_destroy(stage);
  gs_texrender_destroy(texrender);
  gs_texture_destroy(texture);
  return ok;
}

void TestGpu() {
  printf("GPU (libobs-d3d11, default.effect): Before colours come back "
         "unchanged (within 0.75/255)\n");
  for (float w : kWindowsWhites) {
    std::vector<Rgb> got;
    CHECK(GpuRecord(w, w, &got));
    for (size_t i = 0; i < got.size(); i++) {
      printf("  W=%.0f", w);
      PrintRgb("gpu", got[i]);
      CHECK(MaxDiff(got[i], kBefore[i]) <= 0.75);
    }
  }

  printf("GPU: unknown monitor white at W=300 matches the CPU model and "
         "1278's After\n");
  std::vector<Rgb> got;
  CHECK(GpuRecord(300.f, 0.f, &got));
  if (got.size() >= 2) {
    for (int i = 0; i < 2; i++) {
      PrintRgb("gpu", got[i]);
      CHECK(MaxDiff(got[i], Recorded(kBefore[i], 300.f, 0.f)) <= 1.0);
    }
    const double rms = Rms(got.data(), kAfter, 2);
    printf("    rms %.2f\n", rms);
    CHECK(rms <= 4.);
  }
}
BOOL CALLBACK AddMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM list) {
  ((std::vector<HMONITOR>*)list)->push_back(monitor);
  return TRUE;
}

void TestMonitorWhite() {
  printf("gs_get_monitor_sdr_white_level through libobs-d3d11\n");
  std::vector<HMONITOR> monitors;
  EnumDisplayMonitors(nullptr, nullptr, AddMonitor, (LPARAM)&monitors);
  CHECK(!monitors.empty());
  for (HMONITOR monitor : monitors) {
    const float nits = gs_get_monitor_sdr_white_level(monitor);
    printf("    monitor %p: %.0f nits\n", (void*)monitor, nits);
    CHECK(nits >= 80.f);
  }
  printf("not a monitor -> unknown (0)\n");
  CHECK(gs_get_monitor_sdr_white_level((void*)(intptr_t)0x7fff0001) == 0.f);
}
}  // namespace

int main(int, char**) {
  TestWhiteLevel();
  TestDecision();
  TestRegressionTable();
  TestUnknownWhiteFallback();

  if (!obs_startup("en-US", nullptr, nullptr)) {
    printf("FAIL: obs_startup\n");
    return 1;
  }
  const bool video = StartVideo();
  CHECK(video);
  if (video) {
    obs_enter_graphics();
    TestMonitorWhite();
    TestGpu();
    obs_leave_graphics();
  }
  obs_shutdown();

  printf(failures ? "FAILED (%d)\n" : "ALL PASSED\n", failures);
  return failures ? 1 : 0;
}
