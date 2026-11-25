#include "obs_control/scene/follow_window_filter.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <graphics/vec4.h>
#include <util/platform.h>

#include "obs_control/scene/follow_window_guard.h"
#include "obs_control/scene/follow_window_win32.h"

namespace follow_window {

namespace {

const char kFilterId[] = "ascent_follow_window_filter";
const char kSettingHwnd[] = "hwnd";
const char kSettingMonitorHandle[] = "monitor_handle";
// the BRB look: background 0x0d0d0d, opaque (abgr)
const uint32_t kBrbFill = 0xFF0D0D0D;
// one cost summary line per this many probes (1 min at 30 fps)
const size_t kCostWindow = 1800;

struct Filter {
  obs_source_t* self = nullptr;
  Win32Facts facts;
  std::atomic<bool> desktop_front{true};

  // graphics thread only
  Probe pre;
  bool have_pre = false;
  bool hidden_since_render = false;
  SettleState settle;
  bool logged = false;
  Reason last_reason = Reason::kGame;
  uint32_t last_occluder_pid = 0;
  gs_texrender_t* texrender = nullptr;
  std::vector<uint32_t> walk_us;
  std::vector<uint32_t> probe_us;
  uint64_t walked_sum = 0;
  uint32_t walked_max = 0;
  uint64_t game_frames = 0;
  uint64_t brb_frames = 0;

  Filter(obs_source_t* source, uint64_t hwnd, uint32_t monitor)
      : self(source), facts(hwnd, monitor) {}
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
       "follow-window: cost over %zu frames: z-order walk mean %llu us "
       "p50 %u us p99 %u us max %u us (windows walked mean %llu max %u); "
       "whole probe p50 %u us p99 %u us max %u us; frames game %llu brb %llu",
       n, (unsigned long long)(sum / n), Percentile(f->walk_us, 0.5),
       Percentile(f->walk_us, 0.99), max_walk,
       (unsigned long long)(f->walked_sum / n), f->walked_max,
       Percentile(f->probe_us, 0.5), Percentile(f->probe_us, 0.99), max_probe,
       (unsigned long long)f->game_frames, (unsigned long long)f->brb_frames);
  f->walk_us.clear();
  f->probe_us.clear();
  f->walked_sum = 0;
  f->walked_max = 0;
  f->game_frames = 0;
  f->brb_frames = 0;
}

// libobs calls tick callbacks before ticking any source: this runs before
// the duplicator's acquire for the frame
void OnTick(void* data, float seconds) {
  UNUSED_PARAMETER(seconds);
  Filter* f = static_cast<Filter*>(data);
  f->pre = f->facts.ReadBeforeAcquire();
  f->have_pre = true;
  // the duplicator stops while its source isn't showing (the desktop's
  // focus BRB is up); the render then settles before trusting it again
  obs_source_t* parent = obs_filter_get_parent(f->self);
  if (parent && !obs_source_showing(parent)) {
    f->hidden_since_render = true;
  }
  if (f->pre.adopted) {
    blog(LOG_INFO, "follow-window: window gone, following 0x%llx (same pid %u)",
         (unsigned long long)f->pre.target, f->facts.game_pid());
  }

  f->walk_us.push_back((uint32_t)(f->pre.walk_ns / 1000));
  f->probe_us.push_back((uint32_t)(f->pre.total_ns / 1000));
  f->walked_sum += f->pre.walked;
  f->walked_max = std::max(f->walked_max, f->pre.walked);
  if (f->walk_us.size() >= kCostWindow) {
    LogCost(f);
  }
}

void LogDecision(Filter* f, const GuardDecision& d) {
  bool occluder_changed = d.reason == Reason::kOccluded &&
                          f->pre.occluder_pid != f->last_occluder_pid;
  if (f->logged && d.reason == f->last_reason && !occluder_changed) {
    return;
  }
  f->logged = true;
  f->last_reason = d.reason;
  f->last_occluder_pid = f->pre.occluder_pid;
  if (d.show_game) {
    blog(LOG_INFO, "follow-window: game (crop %d,%d %dx%d)", d.crop.left,
         d.crop.top, d.crop.width(), d.crop.height());
  } else if (d.reason == Reason::kOccluded) {
    blog(LOG_INFO, "follow-window: brb (occluded by pid %u class %s)",
         f->pre.occluder_pid, f->pre.occluder_class.c_str());
  } else {
    blog(LOG_INFO, "follow-window: brb (%s)", ReasonName(d.reason));
  }
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
  Filter* f = new Filter(source, hwnd, monitor);
  blog(LOG_INFO, "follow-window: on (hwnd 0x%llx pid %u monitor 0x%x)",
       (unsigned long long)hwnd, f->facts.game_pid(), monitor);
  obs_add_tick_callback(OnTick, f);
  return f;
}

void Destroy(void* data) {
  Filter* f = static_cast<Filter*>(data);
  obs_remove_tick_callback(OnTick, f);
  LogCost(f);
  if (f->texrender) {
    obs_enter_graphics();
    gs_texrender_destroy(f->texrender);
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

void Render(void* data, gs_effect_t* effect) {
  UNUSED_PARAMETER(effect);
  Filter* f = static_cast<Filter*>(data);
  obs_video_info ovi;
  if (!obs_get_video_info(&ovi)) {
    return;
  }
  const uint32_t cx = ovi.base_width;
  const uint32_t cy = ovi.base_height;

  GuardDecision d;
  if (f->have_pre) {
    GuardInputs in;
    in.window_alive = f->pre.window_alive;
    in.desktop_front = f->desktop_front.load();
    in.foreground_is_game = f->pre.foreground_is_game;
    in.occluded = f->pre.occluded;
    in.monitor = f->pre.monitor;
    in.rect_before_acquire = f->pre.client;
    in.rect_at_render =
        f->pre.window_alive ? f->facts.ReadClientRect(f->pre.target) : Rect();
    in.reshown = f->hidden_since_render;
    f->hidden_since_render = false;
    in.now_ns = os_gettime_ns();
    d = Decide(in, f->settle);
  }
  LogDecision(f, d);

  obs_source_t* target = obs_filter_get_target(f->self);
  const uint32_t mw = target ? obs_source_get_base_width(target) : 0;
  const uint32_t mh = target ? obs_source_get_base_height(target) : 0;
  // the duplicated texture is the monitor; a crop outside it (rotated
  // monitor, a mode change not seen yet, no frame yet) draws BRB
  const bool crop_fits = d.show_game && d.crop.left >= 0 &&
                         d.crop.top >= 0 && (uint32_t)d.crop.right <= mw &&
                         (uint32_t)d.crop.bottom <= mh;
  if (!crop_fits) {
    f->brb_frames++;
    DrawFill(cx, cy);
    return;
  }

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

  gs_texture_t* tex = drawn ? gs_texrender_get_texture(f->texrender) : nullptr;
  if (!tex) {
    f->brb_frames++;
    DrawFill(cx, cy);
    return;
  }

  f->game_frames++;
  Placement p = FitInside(d.crop.width(), d.crop.height(), cx, cy);
  gs_effect_t* def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
  gs_eparam_t* image = gs_effect_get_param_by_name(def, "image");

  const bool previous = gs_framebuffer_srgb_enabled();
  gs_enable_framebuffer_srgb(true);
  gs_effect_set_texture_srgb(image, tex);
  gs_matrix_push();
  gs_matrix_translate3f(p.x, p.y, 0.0f);
  gs_matrix_scale3f(p.scale, p.scale, 1.0f);
  while (gs_effect_loop(def, "Draw")) {
    gs_draw_sprite_subregion(tex, 0, (uint32_t)d.crop.left,
                             (uint32_t)d.crop.top, (uint32_t)d.crop.width(),
                             (uint32_t)d.crop.height());
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

}  // namespace

obs_source_t* CreateFilter(uint64_t hwnd, uint32_t monitor_handle) {
  RegisterOnce();
  obs_data_t* settings = obs_data_create();
  obs_data_set_int(settings, kSettingHwnd, (long long)hwnd);
  obs_data_set_int(settings, kSettingMonitorHandle, monitor_handle);
  obs_source_t* filter = obs_source_create_private(
      kFilterId, "ascent follow window", settings);
  obs_data_release(settings);
  return filter;
}

void SetDesktopFront(obs_source_t* filter, bool front) {
  if (!filter) {
    return;
  }
  Filter* f = static_cast<Filter*>(obs_obj_get_data(filter));
  if (f) {
    f->desktop_front.store(front);
  }
}

}  // namespace follow_window
