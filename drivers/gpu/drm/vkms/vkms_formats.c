// SPDX-License-Identifier: GPL-2.0+

#include <drm/drm_rect.h>
#include <linux/minmax.h>

#include "vkms_formats.h"

/* The following macros help doing fixed point arithmetic. */
/*
 * With Fixed-Point scale 15 we have 17 and 15 bits of integer and fractional
 * parts respectively.
 *  | 0000 0000 0000 0000 0.000 0000 0000 0000 |
 * 31                                          0
 */
#define SHIFT 15

#define INT_TO_FIXED(a) ((a) << SHIFT)
#define FIXED_MUL(a, b) ((s32)(((s64)(a) * (b)) >> SHIFT))
#define FIXED_DIV(a, b) ((s32)(((s64)(a) << SHIFT) / (b)))
/* Conversion from long float (Double) to Fixed-Point */
#define LF_TO_FIXED(a) ((a) * (u64)(1 << SHIFT))
/* This macro converts a fixed point number to int, and round half up it */
#define FIXED_TO_INT_ROUND(a) (((a) + (1 << (SHIFT - 1))) >> SHIFT)
#define INT_TO_FIXED_DIV(a, b) (FIXED_DIV(INT_TO_FIXED(a), INT_TO_FIXED(b)))

#define get_uv_plane_offset get_v_plane_offset

static int row_offset(const struct vkms_frame_info *frame_info, int y)
{
	return frame_info->offset + (y * frame_info->pitch);
}

static size_t pixel_offset(const struct vkms_frame_info *frame_info, int x, int y)
{
	return row_offset(frame_info, y) + (x * frame_info->cpp);
}

static void *get_v_plane_offset(const struct vkms_frame_info *frame_info,
				u8 *src_addr, int y)
{
	return (u8 *)src_addr + row_offset(frame_info, y);
}

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
static void *packed_pixels_addr(const struct vkms_frame_info *frame_info,
				int x, int y)
{
	size_t offset = pixel_offset(frame_info, x, y);

	return (u8 *)frame_info->map[0].vaddr + offset;
}

static void *get_packed_src_addr(const struct vkms_frame_info *frame_info, int y)
{
	int x_src = frame_info->src.x1 >> 16;
	int y_src = y - frame_info->dst.y1 + (frame_info->src.y1 >> 16);

	return packed_pixels_addr(frame_info, x_src, y_src);
}

static void ARGB8888_to_argb_u16(struct line_buffer *stage_buffer,
				 const struct vkms_frame_info *frame_info, int y)
{
	struct pixel_argb_u16 *out_pixels = stage_buffer->pixels;
	u8 *src_pixels = get_packed_src_addr(frame_info, y);
	int x_limit = min_t(size_t, drm_rect_width(&frame_info->dst),
			    stage_buffer->n_pixels);

	for (size_t x = 0; x < x_limit; x++, src_pixels += 4) {
		/*
		 * The 257 is the "conversion ratio". This number is obtained by the
		 * (2^16 - 1) / (2^8 - 1) division. Which, in this case, tries to get
		 * the best color value in a pixel format with more possibilities.
		 * A similar idea applies to others RGB color conversions.
		 */
		out_pixels[x].a = (u16)src_pixels[3] * 257;
		out_pixels[x].r = (u16)src_pixels[2] * 257;
		out_pixels[x].g = (u16)src_pixels[1] * 257;
		out_pixels[x].b = (u16)src_pixels[0] * 257;
	}
}

static void XRGB8888_to_argb_u16(struct line_buffer *stage_buffer,
				 const struct vkms_frame_info *frame_info, int y)
{
	struct pixel_argb_u16 *out_pixels = stage_buffer->pixels;
	u8 *src_pixels = get_packed_src_addr(frame_info, y);
	int x_limit = min_t(size_t, drm_rect_width(&frame_info->dst),
			    stage_buffer->n_pixels);

	for (size_t x = 0; x < x_limit; x++, src_pixels += 4) {
		out_pixels[x].a = (u16)0xffff;
		out_pixels[x].r = (u16)src_pixels[2] * 257;
		out_pixels[x].g = (u16)src_pixels[1] * 257;
		out_pixels[x].b = (u16)src_pixels[0] * 257;
	}
}

static void ARGB16161616_to_argb_u16(struct line_buffer *stage_buffer,
				     const struct vkms_frame_info *frame_info,
				     int y)
{
	struct pixel_argb_u16 *out_pixels = stage_buffer->pixels;
	u16 *src_pixels = get_packed_src_addr(frame_info, y);
	int x_limit = min_t(size_t, drm_rect_width(&frame_info->dst),
			    stage_buffer->n_pixels);

	for (size_t x = 0; x < x_limit; x++, src_pixels += 4) {
		out_pixels[x].a = le16_to_cpu(src_pixels[3]);
		out_pixels[x].r = le16_to_cpu(src_pixels[2]);
		out_pixels[x].g = le16_to_cpu(src_pixels[1]);
		out_pixels[x].b = le16_to_cpu(src_pixels[0]);
	}
}

static void XRGB16161616_to_argb_u16(struct line_buffer *stage_buffer,
				     const struct vkms_frame_info *frame_info,
				     int y)
{
	struct pixel_argb_u16 *out_pixels = stage_buffer->pixels;
	u16 *src_pixels = get_packed_src_addr(frame_info, y);
	int x_limit = min_t(size_t, drm_rect_width(&frame_info->dst),
			    stage_buffer->n_pixels);

	for (size_t x = 0; x < x_limit; x++, src_pixels += 4) {
		out_pixels[x].a = (u16)0xffff;
		out_pixels[x].r = le16_to_cpu(src_pixels[2]);
		out_pixels[x].g = le16_to_cpu(src_pixels[1]);
		out_pixels[x].b = le16_to_cpu(src_pixels[0]);
	}
}

static void RGB565_to_argb_u16(struct line_buffer *stage_buffer,
			       const struct vkms_frame_info *frame_info, int y)
{
	struct pixel_argb_u16 *out_pixels = stage_buffer->pixels;
	u16 *src_pixels = get_packed_src_addr(frame_info, y);
	int x_limit = min_t(size_t, drm_rect_width(&frame_info->dst),
			       stage_buffer->n_pixels);

	s32 fp_rb_ratio = INT_TO_FIXED_DIV(65535, 31);
	s32 fp_g_ratio = INT_TO_FIXED_DIV(65535, 63);

	for (size_t x = 0; x < x_limit; x++, src_pixels++) {
		u16 rgb_565 = le16_to_cpu(*src_pixels);
		s32 fp_r = INT_TO_FIXED((rgb_565 >> 11) & 0x1f);
		s32 fp_g = INT_TO_FIXED((rgb_565 >> 5) & 0x3f);
		s32 fp_b = INT_TO_FIXED(rgb_565 & 0x1f);

		out_pixels[x].a = (u16)0xffff;
		out_pixels[x].r = FIXED_TO_INT_ROUND(FIXED_MUL(fp_r, fp_rb_ratio));
		out_pixels[x].g = FIXED_TO_INT_ROUND(FIXED_MUL(fp_g, fp_g_ratio));
		out_pixels[x].b = FIXED_TO_INT_ROUND(FIXED_MUL(fp_b, fp_rb_ratio));
	}
}

static struct pixel_argb_u16 yuv_8bits_to_argb16161616(int y, int u, int v)
{
	int fp_y = INT_TO_FIXED(y);
	int fp_u = INT_TO_FIXED(u);
	int fp_v = INT_TO_FIXED(v);
	int hr = INT_TO_FIXED(128);

	int fp_r = fp_y + FIXED_MUL(LF_TO_FIXED(1.370705), fp_v - hr);
	int fp_g = fp_y - FIXED_MUL(LF_TO_FIXED(0.698001), fp_v - hr)
			- FIXED_MUL(LF_TO_FIXED(0.337633), fp_u - hr);
	int fp_b = fp_y + FIXED_MUL(LF_TO_FIXED(1.732446), fp_u - hr);

	int r = FIXED_TO_INT_ROUND(fp_r);
	int g = FIXED_TO_INT_ROUND(fp_g);
	int b = FIXED_TO_INT_ROUND(fp_b);

	r = clamp(r, 0, 255);
	g = clamp(g, 0, 255);
	b = clamp(b, 0, 255);

	return (struct pixel_argb_u16) {
			.a = (u16)0xffff,
			.r = (u16)r * 257,
			.g = (u16)g * 257,
			.b = (u16)b * 257,
		};
}

static void YUV420_to_argb_u16(struct line_buffer *stage_buffer,
			       const struct vkms_frame_info *frame_info, int y)
{
	struct pixel_argb_u16 *out_pixels = stage_buffer->pixels;
	int x_limit = min_t(size_t, drm_rect_width(&frame_info->dst),
			    stage_buffer->n_pixels);
	u8 *src_addr = (u8 *)frame_info->map[0].vaddr;

	u8 *u_plane_offset = get_v_plane_offset(frame_info, src_addr, x_limit);
	u8 *v_plane_offset = u_plane_offset + frame_info->pitch;
	int y_offset = row_offset(frame_info, y);

	for (size_t x = 0; x < x_limit; x++) {
		int uv_offset = (x / 2 + y / 2) * frame_info->cpp;

		u8 Y = src_addr[y_offset + x];
		u8 U = u_plane_offset[uv_offset];
		u8 V = v_plane_offset[uv_offset];

		out_pixels[x] = yuv_8bits_to_argb16161616(Y, U, V);
	}
}

static void NV12_to_argb_u16(struct line_buffer *stage_buffer,
			     const struct vkms_frame_info *frame_info, int y)
{
	struct pixel_argb_u16 *out_pixels = stage_buffer->pixels;
	int x_limit = min_t(size_t, drm_rect_width(&frame_info->dst),
			    stage_buffer->n_pixels);
	u8 *src_addr = frame_info->map[0].vaddr;

	u8 *uv_plane_offset = get_uv_plane_offset(frame_info, src_addr, x_limit);
	int y_offset = row_offset(frame_info, y);

	for (size_t x = 0; x < x_limit; x++) {
		int uv_offset = y / 2 * frame_info->pitch + x / 2 * frame_info->cpp;

		u8 Y = src_addr[y_offset + x];
		u8 U = uv_plane_offset[uv_offset];
		u8 V = uv_plane_offset[uv_offset + 1];

		out_pixels[x] = yuv_8bits_to_argb16161616(Y, U, V);
	}
}

/*
 * The following  functions take an line of argb_u16 pixels from the
 * src_buffer, convert them to a specific format, and store them in the
 * destination.
 *
 * They are used in the `compose_active_planes` to convert and store a line
 * from the src_buffer to the writeback buffer.
 */
static void argb_u16_to_ARGB8888(struct vkms_frame_info *frame_info,
				 const struct line_buffer *src_buffer, int y)
{
	int x_dst = frame_info->dst.x1;
	u8 *dst_pixels = packed_pixels_addr(frame_info, x_dst, y);
	struct pixel_argb_u16 *in_pixels = src_buffer->pixels;
	int x_limit = min_t(size_t, drm_rect_width(&frame_info->dst),
			    src_buffer->n_pixels);

	for (size_t x = 0; x < x_limit; x++, dst_pixels += 4) {
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
		dst_pixels[3] = DIV_ROUND_CLOSEST(in_pixels[x].a, 257);
		dst_pixels[2] = DIV_ROUND_CLOSEST(in_pixels[x].r, 257);
		dst_pixels[1] = DIV_ROUND_CLOSEST(in_pixels[x].g, 257);
		dst_pixels[0] = DIV_ROUND_CLOSEST(in_pixels[x].b, 257);
	}
}

static void argb_u16_to_XRGB8888(struct vkms_frame_info *frame_info,
				 const struct line_buffer *src_buffer, int y)
{
	int x_dst = frame_info->dst.x1;
	u8 *dst_pixels = packed_pixels_addr(frame_info, x_dst, y);
	struct pixel_argb_u16 *in_pixels = src_buffer->pixels;
	int x_limit = min_t(size_t, drm_rect_width(&frame_info->dst),
			    src_buffer->n_pixels);

	for (size_t x = 0; x < x_limit; x++, dst_pixels += 4) {
		dst_pixels[3] = 0xff;
		dst_pixels[2] = DIV_ROUND_CLOSEST(in_pixels[x].r, 257);
		dst_pixels[1] = DIV_ROUND_CLOSEST(in_pixels[x].g, 257);
		dst_pixels[0] = DIV_ROUND_CLOSEST(in_pixels[x].b, 257);
	}
}

static void argb_u16_to_ARGB16161616(struct vkms_frame_info *frame_info,
				     const struct line_buffer *src_buffer, int y)
{
	int x_dst = frame_info->dst.x1;
	u16 *dst_pixels = packed_pixels_addr(frame_info, x_dst, y);
	struct pixel_argb_u16 *in_pixels = src_buffer->pixels;
	int x_limit = min_t(size_t, drm_rect_width(&frame_info->dst),
			    src_buffer->n_pixels);

	for (size_t x = 0; x < x_limit; x++, dst_pixels += 4) {
		dst_pixels[3] = cpu_to_le16(in_pixels[x].a);
		dst_pixels[2] = cpu_to_le16(in_pixels[x].r);
		dst_pixels[1] = cpu_to_le16(in_pixels[x].g);
		dst_pixels[0] = cpu_to_le16(in_pixels[x].b);
	}
}

static void argb_u16_to_XRGB16161616(struct vkms_frame_info *frame_info,
				     const struct line_buffer *src_buffer, int y)
{
	int x_dst = frame_info->dst.x1;
	u16 *dst_pixels = packed_pixels_addr(frame_info, x_dst, y);
	struct pixel_argb_u16 *in_pixels = src_buffer->pixels;
	int x_limit = min_t(size_t, drm_rect_width(&frame_info->dst),
			    src_buffer->n_pixels);

	for (size_t x = 0; x < x_limit; x++, dst_pixels += 4) {
		dst_pixels[3] = 0xffff;
		dst_pixels[2] = cpu_to_le16(in_pixels[x].r);
		dst_pixels[1] = cpu_to_le16(in_pixels[x].g);
		dst_pixels[0] = cpu_to_le16(in_pixels[x].b);
	}
}

static void argb_u16_to_RGB565(struct vkms_frame_info *frame_info,
			       const struct line_buffer *src_buffer, int y)
{
	int x_dst = frame_info->dst.x1;
	u16 *dst_pixels = packed_pixels_addr(frame_info, x_dst, y);
	struct pixel_argb_u16 *in_pixels = src_buffer->pixels;
	int x_limit = min_t(size_t, drm_rect_width(&frame_info->dst),
			    src_buffer->n_pixels);

	s32 fp_rb_ratio = INT_TO_FIXED_DIV(65535, 31);
	s32 fp_g_ratio = INT_TO_FIXED_DIV(65535, 63);

	for (size_t x = 0; x < x_limit; x++, dst_pixels++) {
		s32 fp_r = INT_TO_FIXED(in_pixels[x].r);
		s32 fp_g = INT_TO_FIXED(in_pixels[x].g);
		s32 fp_b = INT_TO_FIXED(in_pixels[x].b);

		u16 r = FIXED_TO_INT_ROUND(FIXED_DIV(fp_r, fp_rb_ratio));
		u16 g = FIXED_TO_INT_ROUND(FIXED_DIV(fp_g, fp_g_ratio));
		u16 b = FIXED_TO_INT_ROUND(FIXED_DIV(fp_b, fp_rb_ratio));

		*dst_pixels = cpu_to_le16(r << 11 | g << 5 | b);
	}
}

static void rgb_16bits_to_yuv_8bits(const struct pixel_argb_u16 *argb,
				    u8 *y, u8 *u, u8 *v)
{
	// maybe: int r = DIV_ROUND_CLOSEST((argb->r * argb->a) / 0xffff, 257);
	int r = DIV_ROUND_CLOSEST(argb->r, 257);
	int g = DIV_ROUND_CLOSEST(argb->g, 257);
	int b = DIV_ROUND_CLOSEST(argb->b, 257);

	int fp_r = INT_TO_FIXED(r);
	int fp_g = INT_TO_FIXED(g);
	int fp_b = INT_TO_FIXED(b);

	int fp_y = FIXED_MUL(LF_TO_FIXED(0.299), fp_r)
		 + FIXED_MUL(LF_TO_FIXED(0.587), fp_g)
		 + FIXED_MUL(LF_TO_FIXED(0.114), fp_b);
	int fp_u = FIXED_MUL(LF_TO_FIXED(-0.147), fp_r)
		 - FIXED_MUL(LF_TO_FIXED(0.289), fp_g)
		 + FIXED_MUL(LF_TO_FIXED(0.436), fp_b);
	int fp_v = FIXED_MUL(LF_TO_FIXED(0.615), fp_r)
		 - FIXED_MUL(LF_TO_FIXED(0.515), fp_g)
		 - FIXED_MUL(LF_TO_FIXED(0.100), fp_b);

	int Y = FIXED_TO_INT_ROUND(fp_y);
	int U = FIXED_TO_INT_ROUND(fp_u);
	int V = FIXED_TO_INT_ROUND(fp_v);

	*y = clamp(Y, 0, 255);
	*u = clamp(U, 0, 255);
	*v = clamp(V, 0, 255);
}

static void argb_u16_to_YUV420(struct vkms_frame_info *frame_info,
			       const struct line_buffer *src_buffer, int y)
{
	struct pixel_argb_u16 *in_pixels = src_buffer->pixels;
	int x_limit = min_t(size_t, drm_rect_width(&frame_info->dst),
			    src_buffer->n_pixels);
	u8 *dst_addr = frame_info->map[0].vaddr;

	u8 *u_plane_offset = get_v_plane_offset(frame_info, dst_addr, x_limit);
	u8 *v_plane_offset = u_plane_offset + frame_info->pitch;
	int y_offset = row_offset(frame_info, y);

	for (size_t x = 0; x < x_limit; x++) {
		int uv_offset = (x / 2 + y / 2) * frame_info->cpp;
		u8 Y, U, V;

		rgb_16bits_to_yuv_8bits(&in_pixels[x], &Y, &U, &V);

		dst_addr[y_offset + x] = Y;
		u_plane_offset[uv_offset] = U;
		v_plane_offset[uv_offset] = V;
	}
}

static void argb_u16_to_NV12(struct vkms_frame_info *frame_info,
			     const struct line_buffer *src_buffer, int y)
{
	struct pixel_argb_u16 *in_pixels = src_buffer->pixels;
	int x_limit = min_t(size_t, drm_rect_width(&frame_info->dst),
			    src_buffer->n_pixels);
	u8 *dst_addr = frame_info->map[0].vaddr;

	u8 *uv_plane_offset = get_uv_plane_offset(frame_info, dst_addr, x_limit);

	int y_offset = row_offset(frame_info, y);

	for (size_t x = 0; x < x_limit; x++) {
		int uv_offset = y / 2 * frame_info->pitch
			      + x / 2 * frame_info->cpp;
		u8 Y, U, V;

		rgb_16bits_to_yuv_8bits(&in_pixels[x], &Y, &U, &V);

		dst_addr[y_offset + x] = Y;
		uv_plane_offset[uv_offset] = U;
		uv_plane_offset[uv_offset + 1] = V;
	}
}

frame_to_line_func get_frame_to_line_function(u32 format)
{
	switch (format) {
	case DRM_FORMAT_ARGB8888:
		return &ARGB8888_to_argb_u16;
	case DRM_FORMAT_XRGB8888:
		return &XRGB8888_to_argb_u16;
	case DRM_FORMAT_ARGB16161616:
		return &ARGB16161616_to_argb_u16;
	case DRM_FORMAT_XRGB16161616:
		return &XRGB16161616_to_argb_u16;
	case DRM_FORMAT_RGB565:
		return &RGB565_to_argb_u16;
	case DRM_FORMAT_NV12:
		return &NV12_to_argb_u16;
	case DRM_FORMAT_YUV420:
		return &YUV420_to_argb_u16;
	default:
		return NULL;
	}
}

line_to_frame_func get_line_to_frame_function(u32 format)
{
	switch (format) {
	case DRM_FORMAT_ARGB8888:
		return &argb_u16_to_ARGB8888;
	case DRM_FORMAT_XRGB8888:
		return &argb_u16_to_XRGB8888;
	case DRM_FORMAT_ARGB16161616:
		return &argb_u16_to_ARGB16161616;
	case DRM_FORMAT_XRGB16161616:
		return &argb_u16_to_XRGB16161616;
	case DRM_FORMAT_RGB565:
		return &argb_u16_to_RGB565;
	case DRM_FORMAT_NV12:
		return &argb_u16_to_NV12;
	case DRM_FORMAT_YUV420:
		return &argb_u16_to_YUV420;
	default:
		return NULL;
	}
}
