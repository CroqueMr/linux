/* SPDX-License-Identifier: MIT */
/* Experimental Standard-DV RGB8 tunnel policy. No hardware accesses here.
 * Validated EDID v0/v1/v2 and CTA progressive 1080p/2160p at 24–60 Hz.
 * Pixel packing and correctly transformed frame metadata are userspace duties;
 * passing this policy is NOT a Dolby Vision color/fidelity validation.
 */
#ifndef __INTEL_DV_LAB_POLICY_H__
#define __INTEL_DV_LAB_POLICY_H__
#include <linux/types.h>
#include <linux/errno.h>
#include "dvbridge_edid.h"

static inline bool intel_dv_lab_edid(const u8 *edid, size_t size)
{
	return dvbridge_parse_edid(edid, size).status == DV_EDID_STANDARD;
}

/* Native HDMI ports can expose a dual-mode level-shifter ID. Match the
 * topology distinction used by intel_hdmi_dp_dual_mode_detect(), rather
 * than treating that ID alone as proof of an external DP conversion.
 * Active LSPCON conversion remains outside the supported transport path.
 */
static inline bool intel_dv_lab_native_hdmi(bool lspcon_active,
					 bool dual_mode_detected, bool vbt_dual_mode)
{
	return !lspcon_active && (!dual_mode_detected || !vbt_dual_mode);
}

struct intel_dv_lab_link {
	bool requested, lab_enabled, sink_standard, hdmi, native_hdmi;
	bool rgb, limited_range, hdr_blob, cloned, progressive;
	unsigned int requested_bpc, pipe_bpp, colorspace;
	unsigned int width, height, rounded_refresh;
};

/* CTA 1080p/2160p 24,25,30,50,60 Hz (including fractional clocks).
 * DV occupies the vendor slot, so the timing goes in AVI rather than the
 * legacy HDMI-1.4 4K vendor packet. Other modes remain out of this lab's scope.
 */
static inline unsigned int intel_dv_lab_avi_vic(unsigned int cea_vic)
{
	switch (cea_vic) {
        case 16: case 31: case 32: case 33: case 34:
        case 93: case 94: case 95: case 96: case 97:
            return cea_vic;
        default:
            return 0;
        }
}

static inline int intel_dv_lab_validate(const struct intel_dv_lab_link *s)
{
	if (!s->requested)
		return 0;
	if (!s->lab_enabled || !s->sink_standard || !s->native_hdmi)
		return -EOPNOTSUPP;
	if (!s->hdmi || !s->rgb || s->limited_range || s->hdr_blob || s->cloned ||
	    !s->progressive || s->requested_bpc != 8 || s->pipe_bpp != 24 ||
	    s->colorspace ||
        (s->rounded_refresh != 24 && s->rounded_refresh != 25 &&
         s->rounded_refresh != 30 && s->rounded_refresh != 50 &&
         s->rounded_refresh != 60))
		return -EINVAL;
	if (!((s->width == 1920 && s->height == 1080) ||
	      (s->width == 3840 && s->height == 2160)))
		return -EINVAL;
	return 0;
}
#endif
