/* SPDX-License-Identifier: GPL-2.0+ */

#include <drm/drm_rect.h>
#include "vkms_formats.h"

format_transform_func get_fmt_transform_function(u32 format)
{
	if (format == DRM_FORMAT_ARGB8888)
		return &ARGB8888_to_ARGB16161616;
	else if (format == DRM_FORMAT_ARGB16161616)
		return &get_ARGB16161616;
	else if (format == DRM_FORMAT_XRGB16161616)
		return &XRGB16161616_to_ARGB16161616;
	else if (format == DRM_FORMAT_RGB565)
		return &RGB565_to_ARGB16161616;
	else
		return &XRGB8888_to_ARGB16161616;
}

format_transform_func get_wb_fmt_transform_function(u32 format)
{
	if (format == DRM_FORMAT_ARGB8888)
		return &convert_to_ARGB8888;
	else if (format == DRM_FORMAT_ARGB16161616)
		return &convert_to_ARGB16161616;
	else if (format == DRM_FORMAT_XRGB16161616)
		return &convert_to_XRGB16161616;
	else if (format == DRM_FORMAT_RGB565)
		return &convert_to_RGB565;
	else
		return &convert_to_XRGB8888;
}

static int pixel_offset(struct vkms_frame_info *frame_info, int x, int y)
{
	return frame_info->offset + (y * frame_info->pitch)
				  + (x * frame_info->cpp);
}

/*
 * FP stands for _Fixed Point_ and **not** _Float Point_
 * LF stands for Long Float (i.e. double)
 * The following macros help doing fixed point arithmetic.
 */
/*
 * With FP scale 15 we have 17 and 15 bits of integer and fractional parts
 * respectively.
 *  | 0000 0000 0000 0000 0.000 0000 0000 0000 |
 * 31                                          0
 */
#define FP_SCALE 15

#define LF_TO_FP(a) ((a) * (u64)(1 << FP_SCALE))
#define INT_TO_FP(a) ((a) << FP_SCALE)
#define FP_MUL(a, b) ((s32)(((s64)(a) * (b)) >> FP_SCALE))
#define FP_DIV(a, b) ((s32)(((s64)(a) << FP_SCALE) / (b)))
/* This macro converts a fixed point number to int, and round half up it */
#define FP_TO_INT_ROUND_UP(a) (((a) + (1 << (FP_SCALE - 1))) >> FP_SCALE)

/*
 * packed_pixels_addr - Get the pointer to pixel of a given pair of coordinates
 *
 * @frame_info: Buffer metadata
 * @x: The x(width) coordinate of the 2D buffer
 * @y: The y(Heigth) coordinate of the 2D buffer
 *
 * Takes the information stored in the frame_info, a pair of coordinates, and
 * returns the address of the first color channel.
 * This function assumes the channels are packed together, i.e. a color channel
 * comes immediately after another in the memory. And therefore, this function
 * doesn't work for YUV with chroma subsampling (e.g. YUV420 and NV21).
 */
static void *packed_pixels_addr(struct vkms_frame_info *frame_info, int x, int y)
{
	int offset = pixel_offset(frame_info, x, y);

	return (u8 *)frame_info->map[0].vaddr + offset;
}

static void *get_packed_src_addr(struct vkms_frame_info *frame_info, int y)
{
	int x_src = frame_info->src.x1 >> 16;
	int y_src = y - frame_info->dst.y1 + (frame_info->src.y1 >> 16);

	return packed_pixels_addr(frame_info, x_src, y_src);
}

void ARGB8888_to_ARGB16161616(struct vkms_frame_info *frame_info, int y,
			      struct line_buffer *stage_buffer)
{
	u8 *src_pixels = get_packed_src_addr(frame_info, y);
	int x, x_limit = drm_rect_width(&frame_info->dst);

	for (x = 0; x < x_limit; x++, src_pixels += 4) {
		/*
		 * Organizes the channels in their respective positions and converts
		 * the 8 bits channel to 16.
		 * The 257 is the "conversion ratio". This number is obtained by the
		 * (2^16 - 1) / (2^8 - 1) division. Which, in this case, tries to get
		 * the best color value in a pixel format with more possibilities.
		 * And a similar idea applies to others RGB color conversions.
		 */
		stage_buffer[x].a = (u16)src_pixels[3] * 257;
		stage_buffer[x].r = (u16)src_pixels[2] * 257;
		stage_buffer[x].g = (u16)src_pixels[1] * 257;
		stage_buffer[x].b = (u16)src_pixels[0] * 257;
	}
}

void XRGB8888_to_ARGB16161616(struct vkms_frame_info *frame_info, int y,
			      struct line_buffer *stage_buffer)
{
	u8 *src_pixels = get_packed_src_addr(frame_info, y);
	int x, x_limit = drm_rect_width(&frame_info->dst);

	for (x = 0; x < x_limit; x++, src_pixels += 4) {
		stage_buffer[x].a = (u16)0xffff;
		stage_buffer[x].r = (u16)src_pixels[2] * 257;
		stage_buffer[x].g = (u16)src_pixels[1] * 257;
		stage_buffer[x].b = (u16)src_pixels[0] * 257;
	}
}

void get_ARGB16161616(struct vkms_frame_info *frame_info, int y,
		      struct line_buffer *stage_buffer)
{
	u16 *src_pixels = get_packed_src_addr(frame_info, y);
	int x, x_limit = drm_rect_width(&frame_info->dst);

	for (x = 0; x < x_limit; x++, src_pixels += 4) {
		stage_buffer[x].a = src_pixels[3];
		stage_buffer[x].r = src_pixels[2];
		stage_buffer[x].g = src_pixels[1];
		stage_buffer[x].b = src_pixels[0];
	}
}

void XRGB16161616_to_ARGB16161616(struct vkms_frame_info *frame_info, int y,
				  struct line_buffer *stage_buffer)
{
	u16 *src_pixels = get_packed_src_addr(frame_info, y);
	int x, x_limit = drm_rect_width(&frame_info->dst);

	for (x = 0; x < x_limit; x++, src_pixels += 4) {
		stage_buffer[x].a = (u16)0xffff;
		stage_buffer[x].r = src_pixels[2];
		stage_buffer[x].g = src_pixels[1];
		stage_buffer[x].b = src_pixels[0];
	}
}

void RGB565_to_ARGB16161616(struct vkms_frame_info *frame_info, int y,
			    struct line_buffer *stage_buffer)
{
	u16 *src_pixels = get_packed_src_addr(frame_info, y);
	int x, x_limit = drm_rect_width(&frame_info->dst);

	for (x = 0; x < x_limit; x++, src_pixels++) {
		u16 rgb_565 = le16_to_cpu(*src_pixels);
		int fp_r = INT_TO_FP((rgb_565 >> 11) & 0x1f);
		int fp_g = INT_TO_FP((rgb_565 >> 5) & 0x3f);
		int fp_b = INT_TO_FP(rgb_565 & 0x1f);

		/*
		 * The magic constants is the "conversion ratio" and is calculated
		 * dividing 65535(2^16 - 1) by 31(2^5 -1) and 63(2^6 - 1)
		 * respectively.
		 */
		int fp_rb_ratio = LF_TO_FP(2114.032258065);
		int fp_g_ratio = LF_TO_FP(1040.238095238);

		stage_buffer[x].a = (u16)0xffff;
		stage_buffer[x].r = FP_TO_INT_ROUND_UP(FP_MUL(fp_r, fp_rb_ratio));
		stage_buffer[x].g = FP_TO_INT_ROUND_UP(FP_MUL(fp_g, fp_g_ratio));
		stage_buffer[x].b = FP_TO_INT_ROUND_UP(FP_MUL(fp_b, fp_rb_ratio));
	}
}


/*
 * The following  functions take an line of ARGB16161616 pixels from the
 * src_buffer, convert them to a specific format, and store them in the
 * destination.
 *
 * They are used in the `compose_active_planes` to convert and store a line
 * from the src_buffer to the writeback buffer.
 */
void convert_to_ARGB8888(struct vkms_frame_info *frame_info,
			 int y, struct line_buffer *src_buffer)
{
	int x, x_dst = frame_info->dst.x1;
	u8 *dst_pixels = packed_pixels_addr(frame_info, x_dst, y);
	int x_limit = drm_rect_width(&frame_info->dst);

	for (x = 0; x < x_limit; x++, dst_pixels += 4) {
		/*
		 * This sequence below is important because the format's byte order is
		 * in little-endian. In the case of the ARGB8888 the memory is
		 * organized this way:
		 *
		 * | Addr     | = blue channel
		 * | Addr + 1 | = green channel
		 * | Addr + 2 | = Red channel
		 * | Addr + 3 | = Alpha channel
		 */
		dst_pixels[3] = DIV_ROUND_UP(src_buffer[x].a, 257);
		dst_pixels[2] = DIV_ROUND_UP(src_buffer[x].r, 257);
		dst_pixels[1] = DIV_ROUND_UP(src_buffer[x].g, 257);
		dst_pixels[0] = DIV_ROUND_UP(src_buffer[x].b, 257);
	}
}

void convert_to_XRGB8888(struct vkms_frame_info *frame_info,
			 int y, struct line_buffer *src_buffer)
{
	int x, x_dst = frame_info->dst.x1;
	u8 *dst_pixels = packed_pixels_addr(frame_info, x_dst, y);
	int x_limit = drm_rect_width(&frame_info->dst);

	for (x = 0; x < x_limit; x++, dst_pixels += 4) {
		dst_pixels[3] = (u8)0xff;
		dst_pixels[2] = DIV_ROUND_UP(src_buffer[x].r, 257);
		dst_pixels[1] = DIV_ROUND_UP(src_buffer[x].g, 257);
		dst_pixels[0] = DIV_ROUND_UP(src_buffer[x].b, 257);
	}
}

void convert_to_ARGB16161616(struct vkms_frame_info *frame_info, int y,
			     struct line_buffer *src_buffer)
{
	int x, x_dst = frame_info->dst.x1;
	u16 *dst_pixels = packed_pixels_addr(frame_info, x_dst, y);
	int x_limit = drm_rect_width(&frame_info->dst);

	for (x = 0; x < x_limit; x++, dst_pixels += 4) {
		dst_pixels[3] = src_buffer[x].a;
		dst_pixels[2] = src_buffer[x].r;
		dst_pixels[1] = src_buffer[x].g;
		dst_pixels[0] = src_buffer[x].b;
	}
}

void convert_to_XRGB16161616(struct vkms_frame_info *frame_info, int y,
			     struct line_buffer *src_buffer)
{
	int x, x_dst = frame_info->dst.x1;
	u16 *dst_pixels = packed_pixels_addr(frame_info, x_dst, y);
	int x_limit = drm_rect_width(&frame_info->dst);

	for (x = 0; x < x_limit; x++, dst_pixels += 4) {
		dst_pixels[3] = src_buffer[x].a;
		dst_pixels[2] = src_buffer[x].r;
		dst_pixels[1] = src_buffer[x].g;
		dst_pixels[0] = src_buffer[x].b;
	}
}

void convert_to_RGB565(struct vkms_frame_info *frame_info, int y,
		       struct line_buffer *src_buffer)
{
	int x, x_dst = frame_info->dst.x1;
	u16 *dst_pixels = packed_pixels_addr(frame_info, x_dst, y);
	int x_limit = drm_rect_width(&frame_info->dst);

	for (x = 0; x < x_limit; x++, dst_pixels++) {
		int fp_r = INT_TO_FP(src_buffer[x].r);
		int fp_g = INT_TO_FP(src_buffer[x].g);
		int fp_b = INT_TO_FP(src_buffer[x].b);

		int fp_rb_ratio = LF_TO_FP(2114.032258065);
		int fp_g_ratio = LF_TO_FP(1040.238095238);

		u16 r = FP_TO_INT_ROUND_UP(FP_DIV(fp_r, fp_rb_ratio));
		u16 g = FP_TO_INT_ROUND_UP(FP_DIV(fp_g, fp_g_ratio));
		u16 b = FP_TO_INT_ROUND_UP(FP_DIV(fp_b, fp_rb_ratio));

		*dst_pixels = cpu_to_le16(r << 11 | g << 5 | b);
	}
}
