/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _VKMS_FORMATS_H_
#define _VKMS_FORMATS_H_

#include "vkms_drv.h"

struct line_buffer {
	u16 a, r, g, b;
};

void ARGB8888_to_ARGB16161616(struct vkms_frame_info *frame_info, int y,
			      struct line_buffer *stage_buffer);

void XRGB8888_to_ARGB16161616(struct vkms_frame_info *frame_info, int y,
			      struct line_buffer *stage_buffer);

void get_ARGB16161616(struct vkms_frame_info *frame_info, int y,
		      struct line_buffer *stage_buffer);

void XRGB16161616_to_ARGB16161616(struct vkms_frame_info *frame_info, int y,
				  struct line_buffer *stage_buffer);

void convert_to_ARGB8888(struct vkms_frame_info *frame_info, int y,
			 struct line_buffer *src_buffer);

void convert_to_XRGB8888(struct vkms_frame_info *frame_info, int y,
			 struct line_buffer *src_buffer);

void convert_to_ARGB16161616(struct vkms_frame_info *frame_info, int y,
			     struct line_buffer *src_buffer);

void convert_to_XRGB16161616(struct vkms_frame_info *frame_info, int y,
			     struct line_buffer *src_buffer);

typedef void (*format_transform_func)(struct vkms_frame_info *frame_info, int y,
				      struct line_buffer *buffer);

format_transform_func get_fmt_transform_function(u32 format);

format_transform_func get_wb_fmt_transform_function(u32 format);

#endif /* _VKMS_FORMATS_H_ */
