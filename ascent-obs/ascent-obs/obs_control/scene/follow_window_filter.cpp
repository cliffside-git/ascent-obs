#include "obs_control/scene/follow_window_filter.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <callback/calldata.h>
#include <callback/proc.h>
#include <graphics/vec4.h>
#include <util/platform.h>

#include "obs_control/scene/follow_window_guard.h"
#include "obs_control/scene/follow_window_win32.h"

namespace follow_window {

const char kFrameInfoProc[] = "ascent_frame_info";

namespace {

const char kFilterId[] = "ascent_follow_window_filter";
const char kSettingHwnd[] = "hwnd";
const char kSettingMonitorHandle[] = "monitor_handle";
const char kSettingDesktopFront[] = "desktop_front";
// the BRB look: background 0x0d0d0d, opaque (abgr)
const uint32_t kBrbFill = 0xFF0D0D0D;
// one cost summary line per this many probes (2 per frame: 30 s at 30 fps)
const size_t kCostWindow = 1800;

struct Filter {
  obs_source_t* self = nullptr;
  const uint32_t monitor_handle;
  std::atomic<bool> desktop_front{true};
  std::atomic<bool> report_again{false};

  // command thread -> graphics thread: a later START's window, applied by
  // the tick callback at a frame boundary
  std::mutex config_mutex;
  uint64_t pending_hwnd = 0;
  uint64_t pending_generation = 0;
  EventSink sink;

  // graphics thread only
  uint64_t applied_generation = 0;
  std::unique_ptr<Win32Facts> facts;
  Probe pre;
  uint64_t pre_ns = 0;
  bool have_pre = false;
  bool hidden_since_render = false;
  SettleState settle;
  ReasonNotifier notifier;
  bool logged = false;
  Reason last_reason = Reason::kGame;
  uint32_t last_occluder_pid = 0;
  std::vector<Rect> last_masks;
  gs_texrender_t* texrender = nullptr;  // the whole monitor, cursor drawn
  gs_texture_t* client_texture = nullptr;  // the client rect, copied 1:1
  uint32_t client_cx = 0;
  uint32_t client_cy = 0;
  std::vector<uint32_t> walk_us;
  std::vector<uint32_t> probe_us;
  uint64_t walked_sum = 0;
  uint32_t walked_max = 0;
  uint64_t enumerated_sum = 0;
  uint32_t shell_band_max = 0;
  uint64_t game_frames = 0;
  uint64_t brb_frames = 0;

  Filter(obs_source_t* source, uint64_t hwnd, uint32_t monitor, bool front)
      : self(source),
        monitor_handle(monitor),
        facts(new Win32Facts(hwnd, monitor)) {
    desktop_front.store(front);
  }
};

uint32_t Percentile(std::vector<uint32_t>& v, double q) {
  if (v.empty()) {
    return 0;
  }
  size_t i = (size_t)(q * (double)(v.size() - 1));
  std::nth_element(v.begin(), v.begin() + i, v.end());
  return v[i];
}

void LogCost(Filter* f) {
  if (f->walk_us.empty()) {
    return;
  }
  size_t n = f->walk_us.size();
  uint64_t sum = 0;
  for (uint32_t us : f->walk_us) {
    sum += us;
  }
  uint32_t max_walk = *std::max_element(f->walk_us.begin(), f->walk_us.end());
  uint32_t max_probe =
      *std::max_element(f->probe_us.begin(), f->probe_us.end());
  blog(LOG_INFO,
       "follow-window: cost over %zu probes (2 per frame): z-order walk mean %llu us "
       "p50 %u us p99 %u us max %u us (windows walked mean %llu max %u; "
       "top-level windows enumerated mean %llu, shell-band max %u); "
       "whole probe p50 %u us p99 %u us max %u us; frames game %llu brb %llu",
       n, (unsigned long long)(sum / n), Percentile(f->walk_us, 0.5),
       Percentile(f->walk_us, 0.99), max_walk,
       (unsigned long long)(f->walked_sum / n), f->walked_max,
       (unsigned long long)(f->enumerated_sum / n), f->shell_band_max,
       Percentile(f->probe_us, 0.5), Percentile(f->probe_us, 0.99), max_probe,
       (unsigned long long)f->game_frames, (unsigned long long)f->brb_frames);
  f->walk_us.clear();
  f->probe_us.clear();
  f->walked_sum = 0;
  f->walked_max = 0;
  f->enumerated_sum = 0;
  f->shell_band_max = 0;
  f->game_frames = 0;
  f->brb_frames = 0;
}

void CountCost(Filter* f, const Probe& p) {
  f->walk_us.push_back((uint32_t)(p.walk_ns / 1000));
  f->probe_us.push_back((uint32_t)(p.total_ns / 1000));
  f->walked_sum += p.walked;
  f->walked_max = std::max(f->walked_max, p.walked);
  f->enumerated_sum += p.enumerated;
  f->shell_band_max = std::max(f->shell_band_max, p.shell_band);
  if (f->walk_us.size() >= kCostWindow) {
    LogCost(f);
  }
}

// a later START's window, at a frame boundary: new facts and a fresh settle
void ApplyPendingTarget(Filter* f) {
  uint64_t hwnd = 0;
  {
    std::lock_guard<std::mutex> lock(f->config_mutex);
    if (f->pending_generation == f->applied_generation) {
      return;
    }
    f->applied_generation = f->pending_generation;
    hwnd = f->pending_hwnd;
  }
  f->facts.reset(new Win32Facts(hwnd, f->monitor_handle));
  f->settle = SettleState();
  f->have_pre = false;
  f->logged = false;
  blog(LOG_INFO, "follow-window: now following hwnd 0x%llx (pid %u)",
       (unsigned long long)hwnd, f->facts->game_pid());
}

// libobs calls tick callbacks before ticking any source: this runs before
// the duplicator's acquire for the frame
void OnTick(void* data, float seconds) {
  UNUSED_PARAMETER(seconds);
  Filter* f = static_cast<Filter*>(data);
  ApplyPendingTarget(f);

  // the duplicator stops while its source isn't showing (the desktop's
  // focus BRB is up, or a replaced source left hidden in the scene); no
  // probe then, and the render settles before trusting it again
  obs_source_t* parent = obs_filter_get_parent(f->self);
  if (!parent || !obs_source_showing(parent)) {
    f->hidden_since_render = true;
    f->have_pre = false;
    // the first decision once shown again is reported to the desktop
    f->notifier = ReasonNotifier();
    return;
  }

  f->pre = f->facts->Read(true);
  // after the read: an image presented later shows a state at least this new
  f->pre_ns = os_gettime_ns();
  f->have_pre = true;
  if (f->pre.adopted) {
    blog(LOG_INFO,
         "follow-window: target gone or not drawable, following 0x%llx "
         "(same pid %u)",
         (unsigned long long)f->pre.snap.target, f->facts->game_pid());
  }
  CountCost(f, f->pre);
}

std::string JoinOccluders(const Probe& p) {
  std::string out;
  for (const std::string& o : p.occluders) {
    out += out.empty() ? o : "; " + o;
  }
  return out;
}

void LogDecision(Filter* f, const GuardDecision& d, const Probe& now) {
  // the occluder named is the one either read saw
  const Probe& seen = f->pre.snap.occluded ? f->pre : now;
  bool occluder_changed = d.reason == Reason::kOccluded &&
                          seen.occluder_pid != f->last_occluder_pid;
  if (f->logged && d.reason == f->last_reason && !occluder_changed &&
      d.masks == f->last_masks) {
    return;
  }
  f->logged = true;
  f->last_reason = d.reason;
  f->last_occluder_pid = seen.occluder_pid;
  f->last_masks = d.masks;
  if (d.show_game && d.reason == Reason::kMasked) {
    std::string regions;
    for (const Rect& m : d.masks) {
      char buf[64];
      snprintf(buf, sizeof(buf), "%s%d,%d %dx%d", regions.empty() ? "" : "; ",
               m.left, m.top, m.width(), m.height());
      regions += buf;
    }
    blog(LOG_INFO,
         "follow-window: masked (crop %d,%d %dx%d; %zu region(s) in crop px, "
         "margin %d px: %s; occluders: %s)",
         d.crop.left, d.crop.top, d.crop.width(), d.crop.height(),
         d.masks.size(), now.snap.margin, regions.c_str(),
         JoinOccluders(now).c_str());
  } else if (d.show_game) {
    blog(LOG_INFO, "follow-window: game (crop %d,%d %dx%d)", d.crop.left,
         d.crop.top, d.crop.width(), d.crop.height());
  } else if (d.reason == Reason::kOccluded) {
    blog(LOG_INFO,
         "follow-window: brb (occluded by pid %u class %s, not masked: %s; "
         "occluders: %s)",
         seen.occluder_pid, seen.occluder_class.c_str(),
         MaskRefusalName(d.refusal), JoinOccluders(seen).c_str());
  } else if (d.reason == Reason::kUnknown) {
    const Probe& unknown = !f->pre.snap.complete ? f->pre : now;
    blog(LOG_INFO, "follow-window: brb (unknown: %s, error %u)",
         unknown.unknown_why ? unknown.unknown_why : "?",
         unknown.unknown_error);
  } else {
    blog(LOG_INFO, "follow-window: brb (%s)", ReasonName(d.reason));
  }
}

// EVT_DISPLAY_SOURCE_CHANGED for the desktop, once per change of what was
// actually drawn (popup/RESULT.md Q3 design 3): game or BRB and why, and
// the masked regions normalized to the crop (SIDECAR.md section 5)
void Notify(Filter* f, bool drew_game, Reason reason,
            const std::vector<NormMask>& masks = {}) {
  if (f->report_again.exchange(false)) {
    f->notifier = ReasonNotifier();
  }
  if (!f->notifier.Changed(reason, masks)) {
    return;
  }
  EventSink sink;
  {
    std::lock_guard<std::mutex> lock(f->config_mutex);
    sink = f->sink;
  }
  if (sink) {
    sink(drew_game, ReasonKey(reason),
         masks.empty() ? std::string() : MasksJson(masks));
  }
}

// What the capture under the filter holds: asked from the monitor capture
// plugin (DXGI duplicator or GDI), which counts only real desktop updates
FrameFacts QueryFrame(obs_source_t* parent) {
  FrameFacts frame;
  if (!parent) {
    return frame;
  }
  proc_handler_t* ph = obs_source_get_proc_handler(parent);
  if (!ph) {
    return frame;
  }
  uint8_t stack[512];
  calldata_t cd;
  calldata_init_fixed(&cd, stack, sizeof(stack));
  if (!proc_handler_call(ph, kFrameInfoProc, &cd)) {
    return frame;
  }
  frame.valid = calldata_bool(&cd, "valid");
  frame.present_ns = (uint64_t)calldata_int(&cd, "present_ns");
  int32_t x = (int32_t)calldata_int(&cd, "x");
  int32_t y = (int32_t)calldata_int(&cd, "y");
  frame.area.left = x;
  frame.area.top = y;
  frame.area.right = x + (int32_t)calldata_int(&cd, "width");
  frame.area.bottom = y + (int32_t)calldata_int(&cd, "height");
  return frame;
}

void DrawFill(uint32_t cx, uint32_t cy) {
  gs_effect_t* solid = obs_get_base_effect(OBS_EFFECT_SOLID);
  gs_eparam_t* color = gs_effect_get_param_by_name(solid, "color");
  struct vec4 c;
  vec4_from_rgba_srgb(&c, kBrbFill);

  const bool previous = gs_framebuffer_srgb_enabled();
  gs_enable_framebuffer_srgb(true);
  gs_effect_set_vec4(color, &c);
  while (gs_effect_loop(solid, "Solid")) {
    gs_draw_sprite(nullptr, 0, cx, cy);
  }
  gs_enable_framebuffer_srgb(previous);
}

const char* GetName(void*) {
  return "Ascent follow window";
}

void* Create(obs_data_t* settings, obs_source_t* source) {
  uint64_t hwnd = (uint64_t)obs_data_get_int(settings, kSettingHwnd);
  uint32_t monitor =
      (uint32_t)obs_data_get_int(settings, kSettingMonitorHandle);
  bool front = obs_data_get_bool(settings, kSettingDesktopFront);
  Filter* f = new Filter(source, hwnd, monitor, front);
  blog(LOG_INFO, "follow-window: on (hwnd 0x%llx pid %u monitor 0x%x)",
       (unsigned long long)hwnd, f->facts->game_pid(), monitor);
  obs_add_tick_callback(OnTick, f);
  return f;
}

void Destroy(void* data) {
  Filter* f = static_cast<Filter*>(data);
  obs_remove_tick_callback(OnTick, f);
  LogCost(f);
  if (f->texrender || f->client_texture) {
    obs_enter_graphics();
    if (f->texrender) {
      gs_texrender_destroy(f->texrender);
    }
    if (f->client_texture) {
      gs_texture_destroy(f->client_texture);
    }
    obs_leave_graphics();
  }
  blog(LOG_INFO, "follow-window: off");
  delete f;
}

uint32_t CanvasWidth(void*) {
  obs_video_info ovi;
  return obs_get_video_info(&ovi) ? ovi.base_width : 0;
}

uint32_t CanvasHeight(void*) {
  obs_video_info ovi;
  return obs_get_video_info(&ovi) ? ovi.base_height : 0;
}

// the whole monitor (the parent draws the cursor on it) into |texrender|
gs_texture_t* RenderMonitor(Filter* f, uint32_t mw, uint32_t mh) {
  if (!f->texrender) {
    f->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
  }
  gs_texrender_reset(f->texrender);
  gs_blend_state_push();
  gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
  bool drawn = false;
  if (gs_texrender_begin_with_color_space(f->texrender, mw, mh,
                                          GS_CS_SRGB)) {
    struct vec4 clear;
    vec4_zero(&clear);
    gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
    gs_ortho(0.0f, (float)mw, 0.0f, (float)mh, -100.0f, 100.0f);
    obs_source_skip_video_filter(f->self);
    gs_texrender_end(f->texrender);
    drawn = true;
  }
  gs_blend_state_pop();
  return drawn ? gs_texrender_get_texture(f->texrender) : nullptr;
}

// the client rect copied texel for texel into a client-sized texture: the
// scaling draw after it samples (and clamps at) the client's own edges, so
// no pixel outside the client rect can bleed in (Codex 3)
gs_texture_t* CopyClient(Filter* f, gs_texture_t* monitor, const Rect& crop) {
  const uint32_t cw = (uint32_t)crop.width();
  const uint32_t ch = (uint32_t)crop.height();
  if (!f->client_texture || f->client_cx != cw || f->client_cy != ch) {
    if (f->client_texture) {
      gs_texture_destroy(f->client_texture);
    }
    f->client_texture =
        gs_texture_create(cw, ch, GS_RGBA, 1, nullptr, GS_RENDER_TARGET);
    f->client_cx = f->client_texture ? cw : 0;
    f->client_cy = f->client_texture ? ch : 0;
  }
  if (!f->client_texture) {
    return nullptr;
  }
  gs_copy_texture_region(f->client_texture, 0, 0, monitor,
                         (uint32_t)crop.left, (uint32_t)crop.top, cw, ch);
  return f->client_texture;
}

// rule 2 masking: the BRB fill over each mask (client px) in the copied
// client texture, before it is scaled, so the scaling draw never samples an
// occluder's texel
bool PaintMasks(gs_texture_t* client, const std::vector<Rect>& masks) {
  const uint32_t cw = gs_texture_get_width(client);
  const uint32_t ch = gs_texture_get_height(client);
  gs_texture_t* previous_target = gs_get_render_target();
  gs_zstencil_t* previous_zs = gs_get_zstencil_target();
  const enum gs_color_space previous_space = gs_get_color_space();
  gs_viewport_push();
  gs_projection_push();
  gs_matrix_push();
  gs_matrix_identity();
  gs_set_render_target_with_color_space(client, nullptr, GS_CS_SRGB);
  gs_set_viewport(0, 0, (int)cw, (int)ch);
  gs_ortho(0.0f, (float)cw, 0.0f, (float)ch, -100.0f, 100.0f);
  gs_blend_state_push();
  gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
  gs_effect_t* solid = obs_get_base_effect(OBS_EFFECT_SOLID);
  gs_eparam_t* color = gs_effect_get_param_by_name(solid, "color");
  struct vec4 c;
  vec4_from_rgba_srgb(&c, kBrbFill);
  const bool previous_srgb = gs_framebuffer_srgb_enabled();
  gs_enable_framebuffer_srgb(true);
  gs_effect_set_vec4(color, &c);
  bool painted = gs_get_render_target() == client;
  while (painted && gs_effect_loop(solid, "Solid")) {
    for (const Rect& m : masks) {
      gs_matrix_push();
      gs_matrix_translate3f((float)m.left, (float)m.top, 0.0f);
      gs_draw_sprite(nullptr, 0, (uint32_t)m.width(), (uint32_t)m.height());
      gs_matrix_pop();
    }
  }
  gs_enable_framebuffer_srgb(previous_srgb);
  gs_blend_state_pop();
  gs_set_render_target_with_color_space(previous_target, previous_zs,
                                        previous_space);
  gs_matrix_pop();
  gs_projection_pop();
  gs_viewport_pop();
  return painted;
}

void Render(void* data, gs_effect_t* effect) {
  UNUSED_PARAMETER(effect);
  Filter* f = static_cast<Filter*>(data);
  obs_video_info ovi;
  if (!obs_get_video_info(&ovi)) {
    return;
  }
  const uint32_t cx = ovi.base_width;
  const uint32_t cy = ovi.base_height;

  // rule 8: no read before this frame's acquire (just retargeted, or the
  // source was hidden at the tick) is not ready: BRB
  if (!f->have_pre) {
    f->brb_frames++;
    DrawFill(cx, cy);
    Notify(f, false, Reason::kUnknown);
    return;
  }

  // rule 9: every fact again, after the acquire
  Probe now = f->facts->Read(false);
  CountCost(f, now);
  obs_source_t* parent = obs_filter_get_parent(f->self);
  GuardInputs in;
  in.pre = f->pre.snap;
  in.render = now.snap;
  in.desktop_front = f->desktop_front.load();
  in.reshown = f->hidden_since_render;
  f->hidden_since_render = false;
  in.frame = QueryFrame(parent);
  in.pre_ns = f->pre_ns;
  in.now_ns = os_gettime_ns();
  GuardDecision d = Decide(in, f->settle);
  LogDecision(f, d, now);

  obs_source_t* target = obs_filter_get_target(f->self);
  const uint32_t mw = target ? obs_source_get_base_width(target) : 0;
  const uint32_t mh = target ? obs_source_get_base_height(target) : 0;
  // the guard checked the image against the monitor; the texture the
  // parent draws must hold the crop too
  const bool crop_fits = d.show_game && d.crop.left >= 0 &&
                         d.crop.top >= 0 && (uint32_t)d.crop.right <= mw &&
                         (uint32_t)d.crop.bottom <= mh;
  gs_texture_t* monitor = crop_fits ? RenderMonitor(f, mw, mh) : nullptr;
  gs_texture_t* client = monitor ? CopyClient(f, monitor, d.crop) : nullptr;
  // a masked frame whose masks could not be painted is not drawn
  if (client && !d.masks.empty() && !PaintMasks(client, d.masks)) {
    client = nullptr;
  }
  if (!client) {
    f->brb_frames++;
    DrawFill(cx, cy);
    // a game decision that could not be drawn is a capture mismatch
    Notify(f, false, d.show_game ? Reason::kCaptureMismatch : d.reason);
    return;
  }

  f->game_frames++;
  Notify(f, true, d.reason,
         NormalizeMasks(d.masks, d.crop.width(), d.crop.height()));
  Placement p = FitInside(d.crop.width(), d.crop.height(), cx, cy);
  gs_effect_t* def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
  gs_eparam_t* image = gs_effect_get_param_by_name(def, "image");

  const bool previous = gs_framebuffer_srgb_enabled();
  gs_enable_framebuffer_srgb(true);
  gs_effect_set_texture_srgb(image, client);
  gs_matrix_push();
  gs_matrix_translate3f(p.x, p.y, 0.0f);
  gs_matrix_scale3f(p.scale, p.scale, 1.0f);
  while (gs_effect_loop(def, "Draw")) {
    gs_draw_sprite(client, 0, 0, 0);
  }
  gs_matrix_pop();
  gs_enable_framebuffer_srgb(previous);
}

void RegisterOnce() {
  static std::once_flag once;
  std::call_once(once, [] {
    obs_source_info info = {};
    info.id = kFilterId;
    info.type = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CAP_DISABLED;
    info.get_name = GetName;
    info.create = Create;
    info.destroy = Destroy;
    info.get_width = CanvasWidth;
    info.get_height = CanvasHeight;
    info.video_render = Render;
    obs_register_source(&info);
  });
}

Filter* FilterOf(obs_source_t* filter) {
  return filter ? static_cast<Filter*>(obs_obj_get_data(filter)) : nullptr;
}

}  // namespace

obs_source_t* CreateFilter(uint64_t hwnd, uint32_t monitor_handle,
                           bool desktop_front) {
  RegisterOnce();
  obs_data_t* settings = obs_data_create();
  obs_data_set_int(settings, kSettingHwnd, (long long)hwnd);
  obs_data_set_int(settings, kSettingMonitorHandle, monitor_handle);
  obs_data_set_bool(settings, kSettingDesktopFront, desktop_front);
  obs_source_t* filter = obs_source_create_private(
      kFilterId, "ascent follow window", settings);
  obs_data_release(settings);
  if (filter && !FilterOf(filter)) {
    obs_source_release(filter);
    return nullptr;
  }
  return filter;
}

void Retarget(obs_source_t* filter, uint64_t hwnd) {
  Filter* f = FilterOf(filter);
  if (!f) {
    return;
  }
  std::lock_guard<std::mutex> lock(f->config_mutex);
  f->pending_hwnd = hwnd;
  f->pending_generation++;
}

void SetEventSink(obs_source_t* filter, EventSink sink) {
  Filter* f = FilterOf(filter);
  if (!f) {
    return;
  }
  std::lock_guard<std::mutex> lock(f->config_mutex);
  f->sink = std::move(sink);
}

void ReportAgain(obs_source_t* filter) {
  Filter* f = FilterOf(filter);
  if (f) {
    f->report_again.store(true);
  }
}

void SetDesktopFront(obs_source_t* filter, bool front) {
  Filter* f = FilterOf(filter);
  if (f) {
    f->desktop_front.store(front);
  }
}

uint64_t MonitorBits(obs_source_t* filter) {
  Filter* f = FilterOf(filter);
  return f && f->facts ? f->facts->monitor_bits() : 0;
}

}  // namespace follow_window
