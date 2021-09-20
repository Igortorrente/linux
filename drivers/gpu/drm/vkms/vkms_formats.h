/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _VKMS_FORMATS_H_
#define _VKMS_FORMATS_H_

#include <drm/drm_rect.h>

#define pixel_offset(composer, x, y) \
	((composer)->offset + ((y) * (composer)->pitch) + ((x) * (composer)->cpp))

#define src_x1(composer) ((composer)->src.x1 >> 16)

#define for_each_pixel_in_line(i, composer)				\
	for (i = src_x1(composer);					\
	     i < src_x1(composer) + drm_rect_width(&(composer)->dst);	\
	     i++)

#define get_v_plane_offset(composer, src_addr, h) \
	((u8 *)(src_addr) + (composer)->offset + (h) * (composer)->pitch)

#define get_uv_plane_offset get_v_plane_offset

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
 * @composer: Buffer metadata
 * @x: The x(width) coordinate of the 2D buffer
 * @y: The y(Heigth) coordinate of the 2D buffer
 *
 * Takes the information stored in the composer, a pair of coordinates, and
 * returns the address of the first color channel.
 * This function assumes the channels are packed together, i.e. a color channel
 * comes immediately after another. And therefore, this function doesn't work
 * for YUV with chroma subsampling (e.g. YUV420 and NV21).
 */
static void *packed_pixels_addr(struct vkms_composer *composer, int x, int y)
{
	int offset = pixel_offset(composer, x, y);

	return (u8 *)composer->map[0].vaddr + offset;
}

static void ARGB8888_to_ARGB16161616(struct vkms_composer *composer, int y,
				     u64 *line_buffer)
{
	int i, x_src = composer->src.x1 >> 16;
	u8 *src_pixels = packed_pixels_addr(composer, x_src, y);

	for_each_pixel_in_line(i, composer) {
		/*
		 * Organizes the channels in their respective positions and converts
		 * the 8 bits channel to 16.
		 * The 257 is the "conversion ratio". This number is obtained by the
		 * (2^16 - 1) / (2^8 - 1) division. Which, in this case, tries to get
		 * the best color value in a pixel format with more possibilities.
		 * And a similar idea applies to others RGB color conversions.
		 */
		line_buffer[i] = ((u64)src_pixels[3] * 257) << 48 |
				 ((u64)src_pixels[2] * 257) << 32 |
				 ((u64)src_pixels[1] * 257) << 16 |
				 ((u64)src_pixels[0] * 257);

		src_pixels += 4;
	}
}

static void XRGB8888_to_ARGB16161616(struct vkms_composer *composer, int y,
				     u64 *line_buffer)
{
	int i, x_src = composer->src.x1 >> 16;
	u8 *src_pixels = packed_pixels_addr(composer, x_src, y);

	for_each_pixel_in_line(i, composer) {
		/*
		 * The same as the ARGB8888 but with the alpha channel as the
		 * maximum value as possible.
		 */
		line_buffer[i] = 0xffffllu << 48 |
				 ((u64)src_pixels[2] * 257) << 32 |
				 ((u64)src_pixels[1] * 257) << 16 |
				 ((u64)src_pixels[0] * 257);

		src_pixels += 4;
	}
}

static void get_ARGB16161616(struct vkms_composer *composer, int y,
			     u64 *line_buffer)
{
	int i, x_src = composer->src.x1 >> 16;
	__le64 *src_pixels = packed_pixels_addr(composer, x_src, y);

	for_each_pixel_in_line(i, composer) {
		/*
		 * Because the format byte order is in little-endian and this code
		 * needs to run on big-endian machines too, we need modify
		 * the byte order from little-endian to the CPU native byte order.
		 */
		line_buffer[i] = le64_to_cpu(*src_pixels);

		src_pixels++;
	}
}

static void XRGB16161616_to_ARGB16161616(struct vkms_composer *composer, int y,
					 u64 *line_buffer)
{
	int i, x_src = composer->src.x1 >> 16;
	__le64 *src_pixels = packed_pixels_addr(composer, x_src, y);

	for_each_pixel_in_line(i, composer) {
		line_buffer[i] = le64_to_cpu(*src_pixels) | (0xffffllu << 48);

		src_pixels++;
	}
}

static void RGB565_to_ARGB16161616(struct vkms_composer *composer, int y,
				   u64 *line_buffer)
{
	int i, x_src = composer->src.x1 >> 16;
	__le16 *src_pixels = packed_pixels_addr(composer, x_src, y);

	for_each_pixel_in_line(i, composer) {
		u16 rgb_565 = le16_to_cpu(*src_pixels);
		int fp_r = INT_TO_FP((rgb_565 >> 11) & 0x1f);
		int fp_g = INT_TO_FP((rgb_565 >> 5) & 0x3f);
		int fp_b = INT_TO_FP(rgb_565 & 0x1f);

		/*
		 * The magic constants is the "conversion ratio" and is calculated
		 * dividing 65535(2^16 - 1) by 31(2^5 -1) and 63(2^6 - 1) respectively.
		 */
		int fp_rb_ratio = LF_TO_FP(2114.032258065);
		int fp_g_ratio = LF_TO_FP(1040.238095238);

		u64 r = FP_TO_INT_ROUND_UP(FP_MUL(fp_r, fp_rb_ratio));
		u64 g = FP_TO_INT_ROUND_UP(FP_MUL(fp_g, fp_g_ratio));
		u64 b = FP_TO_INT_ROUND_UP(FP_MUL(fp_b, fp_rb_ratio));

		line_buffer[i] = 0xffffllu << 48 | r << 32 | g << 16 | b;

		src_pixels++;
	}
}

static u64 yuv_8bit_to_argb16161616(int y, int u, int v)
{
	int fp_y = INT_TO_FP(y);
	int fp_u = INT_TO_FP(u);
	int fp_v = INT_TO_FP(v);
	int hr = INT_TO_FP(128);

	int fp_r = fp_y + FP_MUL(LF_TO_FP(1.370705), fp_v - hr);
	int fp_g = fp_y - FP_MUL(LF_TO_FP(0.698001), fp_v - hr)
			- FP_MUL(LF_TO_FP(0.337633), fp_u - hr);
	int fp_b = fp_y + FP_MUL(LF_TO_FP(1.732446), fp_u - hr);

	int r = FP_TO_INT_ROUND_UP(fp_r);
	int g = FP_TO_INT_ROUND_UP(fp_g);
	int b = FP_TO_INT_ROUND_UP(fp_b);

	r = clamp(r, 0, 255);
	g = clamp(g, 0, 255);
	b = clamp(b, 0, 255);

	return 0xffffllu << 48 |
	       ((u64)r * 257) << 32 |
	       ((u64)g * 257) << 16 |
	       ((u64)b * 257);
}

static void YUV420_to_ARGB16161616(struct vkms_composer *composer, int y,
				   u64 *line_buffer)
{
	int h_dst = drm_rect_height(&composer->dst);
	void *src_addr = (u8 *)composer->map[0].vaddr;

	u8 *u_plane_offset = get_v_plane_offset(composer, src_addr, h_dst);
	u8 *v_plane_offset = u_plane_offset + composer->pitch;

	int uv_offset = (x / 2 + y / 2) * composer->cpp;
	int y_offset = pixel_offset(composer, x, y);

	u8 Y = ((u8 *)src_addr)[y_offset];
	u8 U = u_plane_offset[uv_offset];
	u8 V = v_plane_offset[uv_offset];

	return yuv_8bit_to_argb16161616(Y, U, V);
}

static void NV12_to_ARGB16161616(struct vkms_composer *composer, int y,
				 u64 *line_buffer)
{
	int h_dst = drm_rect_height(&composer->dst);
	void *src_addr = (u8 *)composer->map[0].vaddr;

	u8 *uv_plane_offset = get_uv_plane_offset(composer, src_addr, h_dst);

	int uv_offset = y / 2 * composer->pitch + x / 2 * composer->cpp;
	int y_offset = pixel_offset(composer, x, y);

	u8 Y = ((u8 *)src_addr)[y_offset];
	u8 U = uv_plane_offset[uv_offset];
	u8 V = uv_plane_offset[uv_offset + 1];

	return yuv_8bit_to_argb16161616(Y, U, V);
}

/*
 * The following functions are used as blend operations. But unlike the
 * `alpha_blend`, these functions take an ARGB16161616 pixel from the
 * source, convert it to a specific format, and store it in the destination.
 *
 * They are used in the `compose_active_planes` and `write_wb_buffer` to
 * copy and convert one line of the frame from/to the output buffer to/from
 * another buffer (e.g. writeback buffer, primary plane buffer).
 */

static void convert_to_ARGB8888(struct vkms_composer *src_composer,
				struct vkms_composer *dst_composer,
				int y, u64 *line_buffer)
{
	int i, x_dst = src_composer->dst.x1;
	u8 *dst_pixels = packed_pixels_addr(dst_composer, x_dst, y);

	for_each_pixel_in_line(i, src_composer) {
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
		dst_pixels[0] = DIV_ROUND_UP(line_buffer[i] & 0xffff, 257);
		dst_pixels[1] = DIV_ROUND_UP((line_buffer[i] >> 16) & 0xffff, 257);
		dst_pixels[2] = DIV_ROUND_UP((line_buffer[i] >> 32) & 0xffff, 257);
		dst_pixels[3] = DIV_ROUND_UP(line_buffer[i] >> 48, 257);

		dst_pixels += 4;
	}
}

static void convert_to_XRGB8888(struct vkms_composer *src_composer,
				struct vkms_composer *dst_composer,
				int y, u64 *line_buffer)
{
	int i, x_dst = src_composer->dst.x1;
	u8 *dst_pixels = packed_pixels_addr(dst_composer, x_dst, y);

	for_each_pixel_in_line(i, src_composer) {
		dst_pixels[0] = DIV_ROUND_UP(line_buffer[i] & 0xffff, 257);
		dst_pixels[1] = DIV_ROUND_UP((line_buffer[i] >> 16) & 0xffff, 257);
		dst_pixels[2] = DIV_ROUND_UP((line_buffer[i] >> 32) & 0xffff, 257);
		dst_pixels[3] = 0xff;

		dst_pixels += 4;
	}
}

static void convert_to_ARGB16161616(struct vkms_composer *src_composer,
				    struct vkms_composer *dst_composer,
				    int y, u64 *line_buffer)
{
	int i, x_dst = src_composer->dst.x1;
	__le64 *dst_pixels = packed_pixels_addr(dst_composer, x_dst, y);

	for_each_pixel_in_line(i, src_composer) {

		*dst_pixels = cpu_to_le64(line_buffer[i]);
		dst_pixels++;
	}
}

static void convert_to_XRGB16161616(struct vkms_composer *src_composer,
				    struct vkms_composer *dst_composer,
				    int y, u64 *line_buffer)
{
	int i, x_dst = src_composer->dst.x1;
	__le64 *dst_pixels = packed_pixels_addr(dst_composer, x_dst, y);

	for_each_pixel_in_line(i, src_composer) {
		*dst_pixels = cpu_to_le64(line_buffer[i] | (0xffffllu << 48));
		dst_pixels++;
	}
}

static void convert_to_RGB565(struct vkms_composer *src_composer,
			      struct vkms_composer *dst_composer,
			      int y, u64 *line_buffer)
{
	int i, x_dst = src_composer->dst.x1;
	__le16 *dst_pixels = packed_pixels_addr(dst_composer, x_dst, y);

	for_each_pixel_in_line(i, src_composer) {
		int fp_r = INT_TO_FP((line_buffer[i] >> 32) & 0xffff);
		int fp_g = INT_TO_FP((line_buffer[i] >> 16) & 0xffff);
		int fp_b = INT_TO_FP(line_buffer[i] & 0xffffllu);

		int fp_rb_ratio = LF_TO_FP(2114.032258065);
		int fp_g_ratio = LF_TO_FP(1040.238095238);

		u16 r = FP_TO_INT_ROUND_UP(FP_DIV(fp_r, fp_rb_ratio));
		u16 g = FP_TO_INT_ROUND_UP(FP_DIV(fp_g, fp_g_ratio));
		u16 b = FP_TO_INT_ROUND_UP(FP_DIV(fp_b, fp_rb_ratio));

		*dst_pixels = cpu_to_le16(r << 11 | g << 5 | b);
		dst_pixels++;
	}
}

static void rgb_16bits_to_yuv(u64 argb, u8 *y, u8 *u, u8 *v)
{
	int r = DIV_ROUND_UP((argb & (0xffffllu << 32)) >> 32, 257);
	int g = DIV_ROUND_UP((argb & (0xffffllu << 16)) >> 16, 257);
	int b = DIV_ROUND_UP(argb & 0xffffllu, 257);

	int fp_r = INT_TO_FP(r);
	int fp_g = INT_TO_FP(g);
	int fp_b = INT_TO_FP(b);

	int fp_y = FP_MUL(LF_TO_FP(0.299), fp_r) + FP_MUL(LF_TO_FP(0.587), fp_g)
		 + FP_MUL(LF_TO_FP(0.114), fp_b);
	int fp_u = FP_MUL(LF_TO_FP(-0.147), fp_r) - FP_MUL(LF_TO_FP(0.289), fp_g)
		 + FP_MUL(LF_TO_FP(0.436), fp_b);
	int fp_v = FP_MUL(LF_TO_FP(0.615), fp_r) - FP_MUL(LF_TO_FP(0.515), fp_g)
		 - FP_MUL(LF_TO_FP(0.100), fp_b);

	int Y = FP_TO_INT_ROUND_UP(fp_y);
	int U = FP_TO_INT_ROUND_UP(fp_u);
	int V = FP_TO_INT_ROUND_UP(fp_v);

	*y = clamp(Y, 0, 255);
	*u = clamp(U, 0, 255);
	*v = clamp(V, 0, 255);
}

static void convert_to_YUV420(struct vkms_composer *src_composer,
			      struct vkms_composer *dst_composer,
			      int y, u64 *line_buffer)
{
	void *dst_addr = (u8 *)dst_composer->map[0].vaddr;

	int h_dst = drm_rect_height(&dst_composer->dst);
	u8 *u_plane_offset = get_v_plane_offset(dst_composer, dst_addr, h_dst);
	u8 *v_plane_offset = u_plane_offset + dst_composer->pitch;

	int uv_offset = (x / 2 + y / 2) * dst_composer->cpp;
	int y_offset = pixel_offset(dst_composer, x, y);

	u8 Y, U, V;

	rgb_16bits_to_yuv(argb_src, &Y, &U, &V);

	((u8 *)dst_addr)[y_offset] = Y;
	u_plane_offset[uv_offset] = U;
	v_plane_offset[uv_offset] = V;
}

static void convert_to_NV12(struct vkms_composer *src_composer,
			    struct vkms_composer *dst_composer,
			    int y, u64 *line_buffer)
{
	int h_dst = drm_rect_height(&dst_composer->dst);
	void *dst_addr = (u8 *)dst_composer->map[0].vaddr;

	u8 *uv_plane_offset = get_uv_plane_offset(dst_composer, dst_addr, h_dst);

	int uv_offset = y / 2 * dst_composer->pitch + x / 2 * dst_composer->cpp;
	int y_offset = pixel_offset(dst_composer, x, y);

	u8 Y, U, V;

	rgb_16bits_to_yuv(argb_src, &Y, &U, &V);

	((u8 *)dst_addr)[y_offset] = Y;
	uv_plane_offset[uv_offset] = U;
	uv_plane_offset[uv_offset + 1] = V;
}

#endif /* _VKMS_FORMATS_H_ */
