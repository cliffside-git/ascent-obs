#include <util/dstr.h>
// ASCENT_EDIT_START: frame provenance for follow-window capture
#include <util/platform.h>
// ASCENT_EDIT_END: frame provenance for follow-window capture
#include "dc-capture.h"

/* clang-format off */

#define TEXT_MONITOR_CAPTURE obs_module_text("MonitorCapture")
#define TEXT_CAPTURE_CURSOR  obs_module_text("CaptureCursor")
#define TEXT_COMPATIBILITY   obs_module_text("Compatibility")
#define TEXT_MONITOR         obs_module_text("Monitor")
#define TEXT_PRIMARY_MONITOR obs_module_text("PrimaryMonitor")

/* clang-format on */

struct monitor_capture {
	obs_source_t *source;

	int monitor;
	bool capture_cursor;
	bool compatibility;
	char monitor_id[128];

	struct dc_capture data;
	// ASCENT_EDIT_START: frame provenance for follow-window capture
	/* os_gettime_ns just before the last BitBlt that worked, 0: none */
	uint64_t captured_ns;
	uint64_t generation;
	// ASCENT_EDIT_END: frame provenance for follow-window capture
};

struct monitor_info {
	int cur_id;
	int desired_id;
	int id;
	char desired_monitor_id[128];
	RECT rect;
	uint32_t desired_handle;
};

/* ------------------------------------------------------------------------- */

static inline void do_log(int level, const char *msg, ...)
{
	va_list args;
	struct dstr str = {0};

	va_start(args, msg);

	dstr_copy(&str, "[GDI monitor capture]: ");
	dstr_vcatf(&str, msg, args);
	blog(level, "%s", str.array);
	dstr_free(&str);

	va_end(args);
}

static BOOL CALLBACK enum_monitor(HMONITOR handle, HDC hdc, LPRECT rect,
				  LPARAM param)
{
	struct monitor_info *monitor = (struct monitor_info *)param;

	// ASCENT_EDIT_START: Carried over (use by monitor_id)
	if (strlen(monitor->desired_monitor_id) > 0) {
		MONITORINFOEXA mi;
		mi.cbSize = sizeof(mi);
		if (GetMonitorInfoA(handle, (LPMONITORINFO)&mi)) {
			DISPLAY_DEVICEA device;
			device.cb = sizeof(device);
			if (EnumDisplayDevicesA(mi.szDevice, 0, &device,
						EDD_GET_DEVICE_INTERFACE_NAME)) {
				bool match = strcmp(monitor->desired_monitor_id,
						    device.DeviceID) == 0;
				if (match) {
					monitor->rect = *rect;
					monitor->id = monitor->cur_id;
					return false;
				}
			}
		}
	}
	// ASCENT_EDIT_END: Carried over (use by monitor_id)

	if (monitor->desired_handle != 0) {
		if (monitor->cur_id == 0 ||
		    monitor->desired_handle == (uint32_t)(uintptr_t)handle) {
			monitor->rect = *rect;
			monitor->id = monitor->cur_id;
		}
	} else {
		if (monitor->cur_id == 0 ||
		    monitor->desired_id == monitor->cur_id) {
			monitor->rect = *rect;
			monitor->id = monitor->cur_id;
		}
	}

	UNUSED_PARAMETER(hdc);
	UNUSED_PARAMETER(handle);
	return (monitor->desired_id > monitor->cur_id++ ||
		monitor->desired_handle > 0);
}

static void update_monitor(struct monitor_capture *capture,
			   obs_data_t *settings)
{
	struct monitor_info monitor = {0};
	uint32_t width, height;

	monitor.desired_handle = (uint32_t)obs_data_get_int(settings, "monitor_handle");
	
	const char* desired_monitor_id = obs_data_get_string(settings, "monitor_id");
	if (strlen(desired_monitor_id) > 0) {
		strcpy_s(monitor.desired_monitor_id,
			 _countof(monitor.desired_monitor_id),
			 desired_monitor_id);
	}

	monitor.desired_id = (int)obs_data_get_int(settings, "monitor");

	EnumDisplayMonitors(NULL, NULL, enum_monitor, (LPARAM)&monitor);

	capture->monitor = monitor.id;

	width = monitor.rect.right - monitor.rect.left;
	height = monitor.rect.bottom - monitor.rect.top;

	dc_capture_init(&capture->data, monitor.rect.left, monitor.rect.top,
			width, height, capture->capture_cursor,
			capture->compatibility);
}

static inline void update_settings(struct monitor_capture *capture,
				   obs_data_t *settings)
{
	capture->monitor = (int)obs_data_get_int(settings, "monitor");
	capture->capture_cursor = obs_data_get_bool(settings, "capture_cursor");
	capture->compatibility = obs_data_get_bool(settings, "compatibility");
	strcpy_s(capture->monitor_id, _countof(capture->monitor_id),
		 obs_data_get_string(settings, "monitor_id"));
	
	dc_capture_free(&capture->data);
	update_monitor(capture, settings);
}

/* ------------------------------------------------------------------------- */

static const char *monitor_capture_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Monitor Capture (old)"; //TEXT_MONITOR_CAPTURE;
}

static void monitor_capture_destroy(void *data)
{
	struct monitor_capture *capture = data;

	obs_enter_graphics();
	dc_capture_free(&capture->data);
	obs_leave_graphics();

	bfree(capture);
}

static void monitor_capture_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "monitor", 0);
	obs_data_set_default_bool(settings, "capture_cursor", true);
	obs_data_set_default_bool(settings, "compatibility", false);
}

static void monitor_capture_update(void *data, obs_data_t *settings)
{
	struct monitor_capture *mc = data;
	update_settings(mc, settings);
	// ASCENT_EDIT_START: frame provenance for follow-window capture
	mc->captured_ns = 0;
	// ASCENT_EDIT_END: frame provenance for follow-window capture
}

// ASCENT_EDIT_START: frame provenance for follow-window capture
/* "ascent_frame_info", as duplicator-monitor-capture.c answers it: the GDI
 * copy is taken every tick, so present_ns is when the last good copy began */
static void monitor_capture_frame_info(void *data, calldata_t *cd)
{
	struct monitor_capture *capture = data;
	const bool valid = capture->data.valid &&
			   capture->data.texture_written &&
			   capture->captured_ns != 0;
	calldata_set_bool(cd, "valid", valid);
	calldata_set_int(cd, "present_ns",
			 valid ? (long long)capture->captured_ns : 0);
	calldata_set_int(cd, "generation", (long long)capture->generation);
	calldata_set_int(cd, "x", capture->data.x);
	calldata_set_int(cd, "y", capture->data.y);
	calldata_set_int(cd, "width", valid ? capture->data.width : 0);
	calldata_set_int(cd, "height", valid ? capture->data.height : 0);
}
// ASCENT_EDIT_END: frame provenance for follow-window capture

static void *monitor_capture_create(obs_data_t *settings, obs_source_t *source)
{
	struct monitor_capture *capture;

	capture = bzalloc(sizeof(struct monitor_capture));
	capture->source = source;

	update_settings(capture, settings);

	// ASCENT_EDIT_START: frame provenance for follow-window capture
	proc_handler_add(obs_source_get_proc_handler(source),
			 "void ascent_frame_info(out bool valid, out int present_ns, "
			 "out int generation, out int x, out int y, out int width, "
			 "out int height)",
			 monitor_capture_frame_info, capture);
	// ASCENT_EDIT_END: frame provenance for follow-window capture

	return capture;
}

static void monitor_capture_tick(void *data, float seconds)
{
	struct monitor_capture *capture = data;

	if (!obs_source_showing(capture->source))
		return;

	obs_enter_graphics();
	// ASCENT_EDIT_START: frame provenance for follow-window capture
	const uint64_t start_ns = os_gettime_ns();
	dc_capture_capture(&capture->data, NULL);
	if (capture->data.valid && capture->data.last_capture_ok) {
		capture->captured_ns = start_ns;
		capture->generation++;
	}
	// ASCENT_EDIT_END: frame provenance for follow-window capture
	obs_leave_graphics();

	UNUSED_PARAMETER(seconds);
}

static void monitor_capture_render(void *data, gs_effect_t *effect)
{
	struct monitor_capture *capture = data;
	dc_capture_render(&capture->data,
			  obs_source_get_texcoords_centered(capture->source));

	UNUSED_PARAMETER(effect);
}

static uint32_t monitor_capture_width(void *data)
{
	struct monitor_capture *capture = data;
	return capture->data.width;
}

static uint32_t monitor_capture_height(void *data)
{
	struct monitor_capture *capture = data;
	return capture->data.height;
}

static BOOL CALLBACK enum_monitor_props(HMONITOR handle, HDC hdc, LPRECT rect,
					LPARAM param)
{
	UNUSED_PARAMETER(hdc);
	UNUSED_PARAMETER(rect);

	obs_property_t *monitor_list = (obs_property_t *)param;
	MONITORINFO mi;
	size_t monitor_id = 0;
	struct dstr monitor_desc = {0};
	struct dstr resolution = {0};
	struct dstr format_string = {0};

	monitor_id = obs_property_list_item_count(monitor_list);

	mi.cbSize = sizeof(mi);
	GetMonitorInfo(handle, &mi);

	dstr_catf(&resolution, "%dx%d @ %d,%d",
		  mi.rcMonitor.right - mi.rcMonitor.left,
		  mi.rcMonitor.bottom - mi.rcMonitor.top, mi.rcMonitor.left,
		  mi.rcMonitor.top);

	dstr_copy(&format_string, "%s %d: %s");
	if (mi.dwFlags == MONITORINFOF_PRIMARY) {
		dstr_catf(&format_string, " (%s)", TEXT_PRIMARY_MONITOR);
	}

	dstr_catf(&monitor_desc, format_string.array, TEXT_MONITOR,
		  monitor_id + 1, resolution.array);

	obs_property_list_add_int(monitor_list, monitor_desc.array,
				  (int)monitor_id);

	dstr_free(&monitor_desc);
	dstr_free(&resolution);
	dstr_free(&format_string);

	return TRUE;
}

static obs_properties_t *monitor_capture_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();

	obs_property_t *monitors = obs_properties_add_list(
		props, "monitor", TEXT_MONITOR, OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);

	obs_properties_add_bool(props, "compatibility", TEXT_COMPATIBILITY);
	obs_properties_add_bool(props, "capture_cursor", TEXT_CAPTURE_CURSOR);

	EnumDisplayMonitors(NULL, NULL, enum_monitor_props, (LPARAM)monitors);

	return props;
}

struct obs_source_info monitor_capture_info = {
	.id = "monitor_capture_low",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW |
			OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_SRGB,
	.get_name = monitor_capture_getname,
	.create = monitor_capture_create,
	.destroy = monitor_capture_destroy,
	.video_render = monitor_capture_render,
	.video_tick = monitor_capture_tick,
	.update = monitor_capture_update,
	.get_width = monitor_capture_width,
	.get_height = monitor_capture_height,
	.get_defaults = monitor_capture_defaults,
	.get_properties = monitor_capture_properties,
	.icon_type = OBS_ICON_TYPE_DESKTOP_CAPTURE,
};
