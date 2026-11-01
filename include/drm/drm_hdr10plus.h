/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DRM_HDR10PLUS_H
#define DRM_HDR10PLUS_H
#include <linux/types.h>

#define DRM_HDR10PLUS_INFOFRAME_SIZE 31

static inline int drm_hdr10plus_vsvdb_version(const void *data, unsigned long size)
{
	const unsigned char *p = data;

	if (!p || size < 6 || (p[0] >> 5) != 7 ||
	    (p[0] & 31) < 5 || (p[0] & 31) + 1u > size ||
	    p[1] != 1 || p[2] != 0x8b || p[3] != 0x84 || p[4] != 0x90)
		return -1;
	return p[5] & 3;
}

static inline bool drm_hdr10plus_infoframe_valid(const void *data, unsigned long size)
{
	const unsigned char *p = data;
	unsigned int i, sum = 0, anchors;

	if (!p || size != DRM_HDR10PLUS_INFOFRAME_SIZE ||
	    p[0] != 0x81 || p[1] != 1 || p[2] != 27 ||
	    p[4] != 0x8b || p[5] != 0x84 || p[6] != 0x90)
		return false;
	/* Version 0/1, reserved bits zero, packet latched with its framebuffer. */
	if ((p[7] >> 6) > 1 || (p[7] & 1) ||
	    (p[30] & 0x3f) || !(p[30] & 0x40))
		return false;
	/* V1 slot 1 is luminance; slot 2 is a percentage or unavailable. */
	if ((p[7] >> 6) == 1 && p[11] > 100 && p[11] != 255)
		return false;
	anchors = p[18] >> 4;
	if (anchors > 9)
		return false;
	for (i = 1; i < anchors; i++)
		if (p[21 + i] < p[20 + i])
			return false;
	for (i = 21 + anchors; i < 30; i++)
		if (p[i])
			return false;
	for (i = 0; i < size; i++)
		sum += p[i];
	return !(sum & 0xff);
}
#endif
