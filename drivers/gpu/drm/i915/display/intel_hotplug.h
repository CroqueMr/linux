/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __INTEL_HOTPLUG_H__
#define __INTEL_HOTPLUG_H__

#include <linux/types.h>

enum port;
enum pipe;
struct drm_crtc_commit;
struct intel_connector;
struct intel_digital_port;
struct intel_display;
struct intel_encoder;

void intel_hpd_poll_enable(struct intel_display *display);
void intel_hpd_poll_disable(struct intel_display *display);
void intel_hpd_poll_fini(struct intel_display *display);
enum intel_hotplug_state intel_encoder_hotplug(struct intel_encoder *encoder,
					       struct intel_connector *connector);
void intel_hpd_irq_handler(struct intel_display *display,
			   u32 pin_mask, u32 long_mask);
void intel_hpd_trigger_irq(struct intel_digital_port *dig_port);
void intel_hpd_init(struct intel_display *display);
void intel_hpd_init_early(struct intel_display *display);
void intel_hpd_cancel_work(struct intel_display *display);
bool intel_hpd_hdmi_wait_for_commit(struct intel_encoder *encoder, enum pipe pipe,
				    struct drm_crtc_commit *commit);
void intel_hpd_hdmi_cancel(struct intel_encoder *encoder);
void intel_hpd_hdmi_commit_done(struct intel_display *display, u8 pipe_mask);
enum hpd_pin intel_hpd_pin_default(enum port port);
void intel_hpd_block(struct intel_encoder *encoder);
void intel_hpd_unblock(struct intel_encoder *encoder);
void intel_hpd_clear_and_unblock(struct intel_encoder *encoder);
void intel_hpd_debugfs_register(struct intel_display *display);

void intel_hpd_enable_detection_work(struct intel_display *display);
void intel_hpd_disable_detection_work(struct intel_display *display);
bool intel_hpd_schedule_detection(struct intel_display *display);

#endif /* __INTEL_HOTPLUG_H__ */
