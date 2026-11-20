// SPDX-License-Identifier: MIT
/* Standard-DV transport through native AMD DC HDMI. The player owns pixels
 * and frame metadata; this adapter only protects and signals their transport.
 * No global color/debug bypass is used and ordinary streams are untouched.
 */
#include <linux/hdmi.h>
#include <linux/moduleparam.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_dvbridge.h>
#include <drm/drm_edid.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include "amdgpu.h"
#include "amdgpu_dm_dv.h"
#include "dc.h"
#include "dc_types.h"

static bool dv_standard = true;
module_param_named(dv_standard, dv_standard, bool, 0444);
MODULE_PARM_DESC(dv_standard, "Expose experimental byte-preserving Standard-DV on native AMD HDMI");

static bool dv_lldv;
module_param_named(dv_lldv, dv_lldv, bool, 0444);
MODULE_PARM_DESC(dv_lldv, "Expose experimental player-led 4:2:2 on native AMD HDMI");

static bool dv_rx550(const struct dc_context *ctx)
{
	return ctx && ctx->dce_version == DCE_VERSION_11_2 &&
		ctx->asic_id.chip_id == 0x699f; /* Polaris12 / Lexa */
}

static bool dv_rx550_mode(const struct drm_display_mode *mode)
{
	return mode->hdisplay == 3840 && mode->vdisplay == 2160 &&
		mode->clock == 296703 && drm_match_cea_mode(mode) == 93;
}

static bool dv_display_supported(enum dce_version version)
{
	/* Explicit families, not a >= comparison that enables unknown hardware. */
	switch (version) {
	case DCE_VERSION_11_22: /* Vega M only, not Polaris DCE11.2. */
	case DCN_VERSION_4_01:
	case DCN_VERSION_4_2: case DCN_VERSION_4_2B:
	case DCN_VERSION_1_0: case DCN_VERSION_1_01:
	case DCN_VERSION_2_0: case DCN_VERSION_2_01: case DCN_VERSION_2_1:
	case DCN_VERSION_3_0: case DCN_VERSION_3_01:
	case DCN_VERSION_3_02: case DCN_VERSION_3_03:
	case DCN_VERSION_3_1: case DCN_VERSION_3_14:
	case DCN_VERSION_3_15: case DCN_VERSION_3_16:
	case DCN_VERSION_3_2: case DCN_VERSION_3_21:
	case DCN_VERSION_3_5: case DCN_VERSION_3_51: case DCN_VERSION_3_6:
		return true;
	default:
		return false;
	}
}

static bool dv_lldv_native_supported(enum dce_version version)
{
	return version == DCN_VERSION_2_1;
}

void amdgpu_dm_dv_attach(struct amdgpu_dm_connector *connector)
{
	struct amdgpu_device *adev = drm_to_adev(connector->base.dev);

	const struct dc_context *ctx = adev->dm.dc ? adev->dm.dc->ctx : NULL;
	const bool rx550 = dv_rx550(ctx);
	const bool family = ctx && (rx550 || dv_display_supported(ctx->dce_version));
	const bool hdmi = connector->base.connector_type == DRM_MODE_CONNECTOR_HDMIA &&
		connector->dc_link &&
		connector->dc_link->connector_signal == SIGNAL_TYPE_HDMI_TYPE_A;

	/* Attachment runs once per connector, including rejected families. */
	drm_info(connector->base.dev,
		 "DVBridge: AMD capability connector=%u dce=0x%x family_supported=%u native_hdmi=%u dv_parameter=%u\n",
		 connector->base.base.id,
		 adev->dm.dc ? adev->dm.dc->ctx->dce_version : 0,
		 family, hdmi, dv_standard);
	if (!family || !hdmi)
		return;

	if (dv_standard) {
		connector->dv_standard_property = drm_property_create_bool(connector->base.dev,
				DRM_MODE_PROP_ATOMIC, "DVBRIDGE_DV_STANDARD_LAB");
		if (connector->dv_standard_property)
			drm_object_attach_property(&connector->base.base,
					connector->dv_standard_property, 0);
	}
	if (dv_lldv && !rx550) {
		static const struct drm_prop_enum_list formats[] = {
			{ 0, "Off" },
			{ 1, "YCbCr 4:2:2 10 bits forced" },
			{ AMDGPU_LLDV_RGB10_LAB, "YCbCr 4:2:2 10 bits RGB10 lab" },
		};

		connector->lldv_format_property = drm_property_create_enum(connector->base.dev,
			DRM_MODE_PROP_ATOMIC, "CB1_LLDV_FORMAT", formats,
			dv_lldv_native_supported(adev->dm.dc->ctx->dce_version) ?
			ARRAY_SIZE(formats) : ARRAY_SIZE(formats) - 1);
		if (connector->lldv_format_property)
			drm_object_attach_property(&connector->base.base,
					connector->lldv_format_property, 0);
	}
	connector->hdr10_deep_color_property = drm_property_create_bool(connector->base.dev,
				DRM_MODE_PROP_ATOMIC, "DVBRIDGE_HDR10_DEEP_COLOR");
	if (connector->hdr10_deep_color_property)
		drm_object_attach_property(&connector->base.base,
				connector->hdr10_deep_color_property, 0);
}

int amdgpu_dm_hdr10_validate(const struct dm_connector_state *state)
{
	const struct drm_connector_state *con = &state->base;
	const struct hdr_output_metadata *hdr;

	if (!state->hdr10_deep_color && !con->hdr10plus_metadata)
		return 0;
	if (amdgpu_dm_dv_transport(state) || !con->crtc ||
	    con->connector->connector_type != DRM_MODE_CONNECTOR_HDMIA ||
	    (state->hdr10_deep_color &&
	     !to_amdgpu_dm_connector(con->connector)->hdr10_deep_color_property) ||
	    con->max_requested_bpc < 10 ||
	    (con->colorspace != DRM_MODE_COLORIMETRY_BT2020_RGB &&
	     con->colorspace != DRM_MODE_COLORIMETRY_BT2020_YCC) ||
	    !con->hdr_output_metadata || con->hdr_output_metadata->length < sizeof(*hdr))
		return -EINVAL;
	hdr = con->hdr_output_metadata->data;
	return hdr->metadata_type == 0 && hdr->hdmi_metadata_type1.metadata_type == 0 &&
		hdr->hdmi_metadata_type1.eotf == HDMI_EOTF_SMPTE_ST2084 ? 0 : -EINVAL;
}

static unsigned int hdr10_depth(enum dc_color_depth depth)
{
	/* DC enum order is not precision order (9-bit follows 16-bit). */
	switch (depth) {
	case COLOR_DEPTH_101010: return 10;
	case COLOR_DEPTH_111111: return 11;
	case COLOR_DEPTH_121212: return 12;
	case COLOR_DEPTH_141414: return 14;
	case COLOR_DEPTH_161616: return 16;
	default: return 0;
	}
}

int amdgpu_dm_hdr10_atomic_check(struct drm_atomic_commit *state)
{
	struct drm_connector *connector;
	struct drm_connector_state *con;
	int i, ret;

	for_each_new_connector_in_state(state, connector, con, i) {
		struct drm_crtc_state *cs;
		struct dc_stream_state *stream;

		if (connector->connector_type != DRM_MODE_CONNECTOR_HDMIA ||
		    !to_dm_connector_state(con)->hdr10_deep_color)
			continue;
		ret = amdgpu_dm_hdr10_validate(to_dm_connector_state(con));
		if (ret)
			return ret;
		cs = drm_atomic_get_new_crtc_state(state, con->crtc);
		stream = cs ? to_dm_crtc_state(cs)->stream : NULL;
		if (!cs || !cs->active || !stream || stream->dv_standard ||
		    connector->connector_type != DRM_MODE_CONNECTOR_HDMIA ||
		    stream->signal != SIGNAL_TYPE_HDMI_TYPE_A ||
		    !stream->link || stream->link->connector_signal != SIGNAL_TYPE_HDMI_TYPE_A ||
		    hdr10_depth(stream->timing.display_color_depth) < 10)
			return -EINVAL;
	}
	return 0;
}

static int dv_copy_packet(struct dc_info_packet *packet, const u8 *wire, size_t size)
{
	memset(packet, 0, sizeof(*packet));
	if (size < 4 || size > 3 + sizeof(packet->sb))
		return -EINVAL;
	packet->hb0 = wire[0];
	packet->hb1 = wire[1];
	packet->hb2 = wire[2];
	/* DC's sb starts with the checksum, unlike its separate three-byte header. */
	memcpy(packet->sb, wire + 3, size - 3);
	packet->valid = true;
	return 0;
}

/* Hades RC1 9a06cf50, d1e00b45: the stream owns this sink reference.
 * A connector EDID blob may be replaced independently during hotplug. Keep
 * packet selection tied to this stream and reject unowned EDID bytes.
 */
static const struct dc_edid *dv_stream_edid(const struct dc_stream_state *stream)
{
	const struct dc_edid *edid;

	if (!stream || !stream->sink)
		return NULL;
	edid = &stream->sink->dc_edid;
	return edid->length && edid->length <= sizeof(edid->raw_edid) ? edid : NULL;
}

static bool dv_sink_supported(const struct dvbridge_sink_caps *caps,
			      const struct drm_display_mode *mode)
{
	return caps->status == DV_EDID_STANDARD &&
		(caps->version == 1 || caps->version == 2) &&
		(mode->vdisplay < 2160 || drm_mode_vrefresh(mode) <= 30 ||
		 caps->supports_2160p60);
}

static bool lldv_sink_supported(const struct dvbridge_sink_caps *caps,
			       const struct drm_display_mode *mode)
{
	const unsigned int vic = drm_match_cea_mode(mode);

	return dvbridge_lldv_422(*caps) && !caps->legacy_vsif && vic >= 93 && vic <= 95 &&
		(mode->clock == 297000 || (mode->clock == 296703 && vic != 94));
}

int amdgpu_dm_dv_configure_stream(struct dc_stream_state *stream,
				const struct dm_connector_state *state,
				const struct drm_display_mode *mode)
{
	struct drm_connector *connector = state->base.connector;
	struct amdgpu_dm_connector *aconnector = to_amdgpu_dm_connector(connector);
	const struct dc_edid *edid = dv_stream_edid(stream);
	const struct dvbridge_sink_caps caps = dvbridge_parse_edid(
		edid ? edid->raw_edid : NULL, edid ? edid->length : 0);
	struct hdmi_dolby_vendor_infoframe dv;
	struct hdmi_vendor_infoframe legacy;
	struct hdmi_avi_infoframe avi;
	struct dc_info_packet avi_packet, vsp_packet;
	u8 wire[HDMI_INFOFRAME_SIZE(DOLBY_VENDOR)];
	const bool lldv = state->lldv_format != 0;
	const bool native = state->lldv_format == AMDGPU_LLDV_RGB10_LAB;
	const bool rx550 = dv_rx550(stream->ctx);
	struct drm_dvbridge_link link = {
		.requested = amdgpu_dm_dv_transport(state),
		.lab_enabled = (lldv ? dv_lldv : dv_standard) && stream->ctx &&
			(rx550 ? !lldv : dv_display_supported(stream->ctx->dce_version)),
		.sink_standard = lldv ? lldv_sink_supported(&caps, mode) :
			dv_sink_supported(&caps, mode),
		.hdmi = stream->signal == SIGNAL_TYPE_HDMI_TYPE_A &&
			connector->display_info.is_hdmi && connector->display_info.has_hdmi_infoframe,
		.native_hdmi = connector->connector_type == DRM_MODE_CONNECTOR_HDMIA &&
			stream->link && stream->link->connector_signal == SIGNAL_TYPE_HDMI_TYPE_A,
		.rgb = stream->timing.pixel_encoding == PIXEL_ENCODING_RGB,
		.limited_range = state->base.hdmi.broadcast_rgb != DRM_HDMI_BROADCAST_RGB_FULL,
		.hdr_blob = !!state->base.hdr_output_metadata,
		.progressive = !(mode->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN |
				DRM_MODE_FLAG_DBLCLK | DRM_MODE_FLAG_3D_MASK)),
		.requested_bpc = state->base.max_requested_bpc,
		.pipe_bpp = stream->timing.display_color_depth == COLOR_DEPTH_888 ? 24 : 0,
		.colorspace = state->base.colorspace,
		.width = mode->hdisplay, .height = mode->vdisplay,
		.rounded_refresh = drm_mode_vrefresh(mode),
	};
	int ret;

	if (!link.requested)
		return 0;
	if (rx550 && !dv_rx550_mode(mode))
		return -EOPNOTSUPP;
	if (state->hdr10_deep_color || (lldv && (state->dv_standard ||
	    (state->lldv_format != 1 && !native) || state->base.hdr10plus_metadata)))
		return -EINVAL;
	if (native) {
		const struct drm_display_info *info = &connector->display_info;

		if (!stream->ctx || !dv_lldv_native_supported(stream->ctx->dce_version) ||
		    link.requested_bpc != 10 || drm_mode_is_420_only(info, mode) ||
		    !(info->color_formats & BIT(DRM_OUTPUT_COLOR_FORMAT_YCBCR422)) ||
		    info->max_tmds_clock <= 0 || mode->clock > info->max_tmds_clock)
			return -EOPNOTSUPP;
	}
	ret = native ? drm_dvbridge_validate_link(&link) : drm_dvbridge_validate(&link);
	if (ret)
		return ret;
	if (!(lldv ? aconnector->lldv_format_property : aconnector->dv_standard_property) ||
	    state->scaling != RMX_OFF ||
	    state->underscan_enable || aconnector->timing_changed ||
	    stream->timing.flags.DSC ||
	    stream->timing.h_addressable != mode->hdisplay ||
	    stream->timing.v_addressable != mode->vdisplay ||
	    stream->timing.pix_clk_100hz != mode->clock * 10 ||
	    !drm_dvbridge_avi_vic(drm_match_cea_mode(mode)))
		return -EINVAL;

	ret = drm_hdmi_avi_infoframe_from_display_mode(&avi, connector, mode);
	if (ret)
		return ret;
	avi.video_code = drm_match_cea_mode(mode);
	if (caps.legacy_vsif && avi.video_code >= 93 && avi.video_code <= 95)
		avi.video_code = 0;
	avi.colorspace = lldv ? HDMI_COLORSPACE_YUV422 : HDMI_COLORSPACE_RGB;
	avi.colorimetry = lldv ? HDMI_COLORIMETRY_EXTENDED : HDMI_COLORIMETRY_NONE;
	avi.extended_colorimetry = lldv ? HDMI_EXTENDED_COLORIMETRY_BT2020 : 0;
	avi.quantization_range = lldv ? HDMI_QUANTIZATION_RANGE_DEFAULT : HDMI_QUANTIZATION_RANGE_FULL;
	if (lldv)
		avi.ycc_quantization_range = HDMI_YCC_QUANTIZATION_RANGE_LIMITED;
	ret = hdmi_avi_infoframe_pack(&avi, wire, sizeof(wire));
	if (ret < 0)
		return ret;
	ret = dv_copy_packet(&avi_packet, wire, ret);
	if (ret)
		return ret;
	if (caps.legacy_vsif) {
		unsigned int vic = drm_match_cea_mode(mode);

		hdmi_vendor_infoframe_init(&legacy);
		legacy.dolby_vision = true;
		legacy.vic = vic >= 93 && vic <= 95 ? 96 - vic : 0;
		ret = hdmi_vendor_infoframe_pack(&legacy, wire, sizeof(wire));
	} else {
		hdmi_dolby_vendor_infoframe_init(&dv);
		dv.signal = true;
		dv.low_latency = lldv;
		ret = hdmi_dolby_vendor_infoframe_pack(&dv, wire, sizeof(wire));
	}
	if (ret < 0)
		return ret;
	ret = dv_copy_packet(&vsp_packet, wire, ret);
	if (ret)
		return ret;

	/* Publish the candidate only after every packet has been prepared. */
	stream->avi_infopacket = avi_packet;
	stream->vsp_infopacket = vsp_packet;
	stream->dv_standard = true;
	stream->dv_low_latency = lldv;
	stream->dv_native_422 = native;
	stream->timing.hdmi_vic = caps.legacy_vsif ? legacy.vic : 0;
	stream->timing.vic = avi.video_code;
	if (native) {
		stream->timing.pixel_encoding = PIXEL_ENCODING_YCBCR422;
		stream->timing.display_color_depth = COLOR_DEPTH_101010;
	}
	stream->output_color_space = native ? COLOR_SPACE_2020_YCBCR_LIMITED : COLOR_SPACE_SRGB;
	stream->out_transfer_func.type = TF_TYPE_BYPASS;
	stream->out_transfer_func.tf = TRANSFER_FUNCTION_LINEAR;
	stream->dither_option = native ? DITHER_OPTION_TRUN10 :
		(rx550 ? DITHER_OPTION_TRUN8 : DITHER_OPTION_DISABLE);
	stream->qs_bit = 1;
	return 0;
}

/* Include unchanged planes/connectors: a cursor-only update must not evade
 * transport validation merely because userspace omitted the DV property.
 */
int amdgpu_dm_dv_add_affected(struct drm_atomic_commit *state)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	struct drm_connector *connector;
	struct drm_connector_state *con_state;
	int i, j, ret;

	for_each_new_crtc_in_state(state, crtc, crtc_state, i) {
		struct dc_stream_state *stream = to_dm_crtc_state(crtc_state)->stream;
		bool requested = stream && stream->dv_standard;
		bool hdr10 = false;
		struct drm_connector_list_iter iter;

		/* Connector state is protected by the native connection lock. */
		ret = drm_modeset_lock(&state->dev->mode_config.connection_mutex,
				       state->acquire_ctx);
		if (ret)
			return ret;
		/* Plane-only updates can omit a connector with an active depth guard. */
		drm_connector_list_iter_begin(state->dev, &iter);
		drm_for_each_connector_iter(connector, &iter) {
			if (!(crtc_state->connector_mask & drm_connector_mask(connector)))
				continue;
			con_state = connector->state;
			if (connector->connector_type == DRM_MODE_CONNECTOR_HDMIA &&
			    con_state && con_state->crtc == crtc &&
			    to_dm_connector_state(con_state)->hdr10_deep_color)
				hdr10 = true;
		}
		drm_connector_list_iter_end(&iter);

		for_each_new_connector_in_state(state, connector, con_state, j) {
			if (connector->connector_type == DRM_MODE_CONNECTOR_HDMIA &&
			    con_state->crtc == crtc && amdgpu_dm_dv_transport(to_dm_connector_state(con_state)))
				requested = true;
			if (connector->connector_type == DRM_MODE_CONNECTOR_HDMIA &&
			    con_state->crtc == crtc && to_dm_connector_state(con_state)->hdr10_deep_color)
				hdr10 = true;
		}
		if (!requested && !hdr10)
			continue;
		ret = drm_atomic_add_affected_connectors(state, crtc);
		if (ret)
			return ret;
		if (!requested)
			continue;
		ret = drm_atomic_add_affected_planes(state, crtc);
		if (ret)
			return ret;
	}
	return 0;
}

static bool dv_identity_tf(enum amdgpu_transfer_function tf)
{
	return tf == AMDGPU_TRANSFER_FUNCTION_DEFAULT || tf == AMDGPU_TRANSFER_FUNCTION_IDENTITY;
}

static bool dv_plane_neutral(const struct drm_plane_state *p,
			     const struct drm_display_mode *mode, bool native)
{
	const struct dm_plane_state *dm = to_dm_plane_state(p);
	const struct dc_plane_state *dc = dm->dc_state;
	const struct dc *display = dc && dc->ctx ? dc->ctx->dc : NULL;
	const struct dc_cm2_func_luts *mcm;

	if (!display || display->debug.always_scale)
		return false;
	if (display->config.use_spl && !display->debug.disable_spl &&
	    (display->debug.force_sharpness == SHARPNESS_ZERO ||
	     (display->debug.force_sharpness == SHARPNESS_CUSTOM &&
	      (display->debug.force_sharpness_level || dc->adaptive_sharpness_en))))
		return false;
	mcm = &dc->mcm_luts;
	/* The VIDMEM union contains a GPU address, not a kernel LUT pointer. */
	if (mcm->lut3d_data.lut3d_src != DC_CM2_TRANSFER_FUNC_SOURCE_SYSMEM)
		return false;

	return p->plane->type != DRM_PLANE_TYPE_CURSOR && p->fb &&
		(native ? (p->fb->format->format == DRM_FORMAT_XRGB2101010 ||
			   p->fb->format->format == DRM_FORMAT_XBGR2101010) :
			  (p->fb->format->format == DRM_FORMAT_XRGB8888 ||
			   p->fb->format->format == DRM_FORMAT_XBGR8888)) &&
		p->rotation == DRM_MODE_ROTATE_0 && p->alpha == DRM_BLEND_ALPHA_OPAQUE &&
		/* Primary planes do not expose pixel blend mode on AMD. XRGB/XBGR
		 * have no alpha channel: upstream disables per-pixel blending even
		 * when the property's implicit default is PREMULTI.
		 */
		!p->color_pipeline && p->visible &&
		p->fb->width == mode->hdisplay && p->fb->height == mode->vdisplay &&
		!p->src_x && !p->src_y && !p->crtc_x && !p->crtc_y &&
		p->src_w == (mode->hdisplay << 16) && p->src_h == (mode->vdisplay << 16) &&
		p->crtc_w == mode->hdisplay && p->crtc_h == mode->vdisplay &&
		!dm->degamma_lut && !dm->ctm && !dm->shaper_lut && !dm->lut3d &&
		!dm->blend_lut && dv_identity_tf(dm->degamma_tf) &&
		dv_identity_tf(dm->shaper_tf) && dv_identity_tf(dm->blend_tf) &&
		dm->hdr_mult == AMDGPU_HDR_MULT_DEFAULT &&
		!dc->mcm_lut1d_enable &&
		dc->mcm_shaper_3dlut_setting == DC_CM2_SHAPER_3DLUT_SETTING_BYPASS_ALL &&
		(!mcm->shaper || mcm->shaper->type == TF_TYPE_BYPASS) &&
		(!mcm->lut1d_func || mcm->lut1d_func->type == TF_TYPE_BYPASS) &&
		(!mcm->lut3d_data.lut3d_func || !mcm->lut3d_data.lut3d_func->state.bits.initialized) &&
		!mcm->lut3d_data.mpc_3dlut_enable && !mcm->lut3d_data.rmcm_3dlut_enable &&
		dc->visible && !dc->horizontal_mirror && dc->rotation == ROTATION_ANGLE_0 &&
		!dc->per_pixel_alpha && !dc->global_alpha &&
		dc->gamma_correction.is_identity &&
		dc->in_transfer_func.type == TF_TYPE_BYPASS &&
		dc->in_shaper_func.type == TF_TYPE_BYPASS &&
		dc->blend_tf.type == TF_TYPE_BYPASS && !dc->lut3d_func.state.bits.initialized &&
		dc->hdr_mult.value == dc_fixpt_one.value &&
		!dc->gamut_remap_matrix.enable_remap &&
		!dc->input_csc_color_matrix.enable_adjustment;
}

int amdgpu_dm_dv_atomic_check(struct drm_atomic_commit *state)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *cs;
	struct drm_plane *plane;
	struct drm_plane_state *ps;
	struct drm_connector *connector;
	struct drm_connector_state *con;
	int i, j;

	for_each_new_crtc_in_state(state, crtc, cs, i) {
		struct dm_crtc_state *dm = to_dm_crtc_state(cs);
		struct dc_stream_state *stream = dm->stream;
		unsigned int planes = 0, connectors = 0;
		const bool native = stream && stream->dv_native_422;

		if (!cs->active || !stream || !stream->dv_standard)
			continue;
		if (!stream->ctx || !stream->ctx->dc ||
		    stream->ctx->dc->debug.visual_confirm != VISUAL_CONFIRM_DISABLE ||
		    (stream->func_shaper && stream->func_shaper->type != TF_TYPE_BYPASS) ||
		    (stream->lut3d_func && stream->lut3d_func->state.bits.initialized) ||
		    dm->freesync_config.state == VRR_STATE_ACTIVE_FIXED ||
		    dm->freesync_config.state == VRR_STATE_ACTIVE_VARIABLE ||
		    cs->ctm || cs->gamma_lut || cs->degamma_lut || cs->vrr_enabled ||
		    cs->async_flip || !dv_identity_tf(dm->regamma_tf) ||
		    stream->dither_option != (native ? DITHER_OPTION_TRUN10 :
			(dv_rx550(stream->ctx) ? DITHER_OPTION_TRUN8 : DITHER_OPTION_DISABLE)) ||
		    stream->out_transfer_func.type != TF_TYPE_BYPASS ||
		    stream->gamut_remap_matrix.enable_remap || stream->csc_color_matrix.enable_adjustment ||
		    stream->output_color_space != (native ? COLOR_SPACE_2020_YCBCR_LIMITED : COLOR_SPACE_SRGB) ||
		    stream->signal != SIGNAL_TYPE_HDMI_TYPE_A ||
		    !stream->avi_infopacket.valid || !stream->vsp_infopacket.valid ||
		    /* Recheck the DC raster after global validation, not only the plane. */
		    (dv_rx550(stream->ctx) && !dv_rx550_mode(&cs->mode)) ||
		    stream->timing.flags.INTERLACE ||
		    stream->timing.timing_3d_format != TIMING_3D_FORMAT_NONE ||
		    stream->timing.h_border_left || stream->timing.h_border_right ||
		    stream->timing.v_border_top || stream->timing.v_border_bottom ||
		    stream->timing.h_addressable != cs->mode.hdisplay ||
		    stream->timing.v_addressable != cs->mode.vdisplay ||
		    stream->src.x || stream->src.y ||
		    stream->src.width != cs->mode.hdisplay || stream->src.height != cs->mode.vdisplay ||
		    stream->dst.x || stream->dst.y ||
		    stream->dst.width != cs->mode.hdisplay || stream->dst.height != cs->mode.vdisplay ||
		    stream->timing.pixel_encoding != (native ? PIXEL_ENCODING_YCBCR422 : PIXEL_ENCODING_RGB) ||
		    stream->timing.display_color_depth != (native ? COLOR_DEPTH_101010 : COLOR_DEPTH_888) || stream->timing.flags.DSC)
			return -EINVAL;
		for_each_new_connector_in_state(state, connector, con, j) {
			const struct dc_edid *edid = dv_stream_edid(stream);
			const struct dvbridge_sink_caps caps = dvbridge_parse_edid(
				edid ? edid->raw_edid : NULL, edid ? edid->length : 0);

			if (con->crtc != crtc)
				continue;

			if (connector->connector_type != DRM_MODE_CONNECTOR_HDMIA ||
			    (stream->dv_low_latency ?
			     (!lldv_sink_supported(&caps, &cs->mode) ||
			      to_dm_connector_state(con)->lldv_format != (native ? AMDGPU_LLDV_RGB10_LAB : 1) ||
			      to_dm_connector_state(con)->dv_standard) :
			     (!dv_sink_supported(&caps, &cs->mode) ||
			      !to_dm_connector_state(con)->dv_standard ||
			      to_dm_connector_state(con)->lldv_format)) ||
			    to_dm_connector_state(con)->hdr10_deep_color ||
			    con->hdr10plus_metadata || con->hdr_output_metadata || con->colorspace ||
			    con->max_requested_bpc != (native ? 10 : 8) ||
			    con->hdmi.broadcast_rgb != DRM_HDMI_BROADCAST_RGB_FULL ||
			    to_dm_connector_state(con)->scaling != RMX_OFF ||
			    to_dm_connector_state(con)->underscan_enable)
				return -EINVAL;
			connectors++;
		}
		for_each_new_plane_in_state(state, plane, ps, j) {
			if (ps->crtc != crtc || !ps->fb)
				continue;
			if (!dv_plane_neutral(ps, &cs->mode, native))
				return -EINVAL;
			planes++;
		}
		if (connectors != 1 || planes != 1 || hweight32(cs->connector_mask) != 1 ||
		    hweight32(cs->plane_mask) != 1)
			return -EINVAL;
	}
	return 0;
}
