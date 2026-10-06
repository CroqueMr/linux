/* SPDX-License-Identifier: MIT */
/* Preserve Intel call sites, native topology and platform admission. */
#ifndef __INTEL_DV_LAB_POLICY_H__
#define __INTEL_DV_LAB_POLICY_H__
#include <drm/drm_dvbridge.h>
#define intel_dv_lab_link drm_dvbridge_link
#define intel_dv_lab_avi_vic drm_dvbridge_avi_vic
#define intel_dv_lab_validate drm_dvbridge_validate

static inline bool intel_dv_lab_native_hdmi(bool lspcon_active,
					 bool dual_mode_detected, bool vbt_dual_mode)
{
	return !lspcon_active && (!dual_mode_detected || !vbt_dual_mode);
}

/* Display9+ shares the native HDMI vendor-infoframe transport. */
static inline bool intel_dv_lab_platform_supported(unsigned int display_ver)
{
	return display_ver >= 9;
}
#endif
