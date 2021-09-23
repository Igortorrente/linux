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

#endif /* _VKMS_FORMATS_H_ */
