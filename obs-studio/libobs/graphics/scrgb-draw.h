#pragma once

// ASCENT_EDIT_START: SDR output of an HDR (scRGB) desktop capture

#include "graphics.h"

#ifdef __cplusplus
extern "C" {
#endif

/* DISPLAYCONFIG_SDR_WHITE_LEVEL.SDRWhiteLevel counts thousandths of 80 nits */
static inline unsigned long gs_sdr_white_nits(bool queried,
					      unsigned long sdr_white_level)
{
	return queried ? (sdr_white_level * 80) / 1000 : 80;
}

struct gs_scrgb_draw {
	const char *tech;
	float multiplier;
};

/* how to draw an FP16 scRGB capture of an HDR desktop into current_space */
static inline struct gs_scrgb_draw
gs_scrgb_capture_draw(enum gs_color_space current_space,
		      float monitor_sdr_white_nits, float video_sdr_white_nits)
{
	(void)monitor_sdr_white_nits;
	struct gs_scrgb_draw draw = {"Draw", 1.f};
	switch (current_space) {
	case GS_CS_SRGB:
	case GS_CS_SRGB_16F:
		draw.tech = "DrawMultiplyTonemap";
		draw.multiplier = 80.f / video_sdr_white_nits;
		break;
	case GS_CS_709_EXTENDED:
		draw.tech = "DrawMultiply";
		draw.multiplier = 80.f / video_sdr_white_nits;
		break;
	default:
		break;
	}
	return draw;
}

#ifdef __cplusplus
}
#endif

// ASCENT_EDIT_END: SDR output of an HDR (scRGB) desktop capture
