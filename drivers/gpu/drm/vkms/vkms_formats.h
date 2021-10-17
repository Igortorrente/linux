/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _VKMS_FORMATS_H_
#define _VKMS_FORMATS_H_

#include <drm/drm_rect.h>

#define pixel_offset(composer, x, y) \
	((composer)->offset + ((y) * (composer)->pitch) + ((x) * (composer)->cpp))

#define __concat(a, b) a ## b

#define func_name(op_name, src_fmt) \
	__concat(op_name, src_fmt)

#define func(op_name, src_fmt, ...) \
	func_name(op_name, src_fmt)(__VA_ARGS__)

#define func_signature(op_name, src_fmt, ...) \
	static void func(op_name, src_fmt, __VA_ARGS__)

#define func_common_sig(op_name, src_fmt) \
	func_signature(op_name, src_fmt, \
		       struct vkms_composer *src_composer, \
		       struct vkms_composer *dst_composer)

#define for_each_pixel_do(operation) do { \
	int i, j, j_dst, i_dst; \
	u64 src_pixel \
	int x_src = src_composer->src.x1 >> 16; \
	int y_src = src_composer->src.y1 >> 16; \
	int x_dst = src_composer->dst.x1; \
	int y_dst = src_composer->dst.y1; \
	int h_dst = drm_rect_height(&src_composer->dst); \
	int w_dst = drm_rect_width(&src_composer->dst); \
	int y_limit = y_src + h_dst; \
	int x_limit = x_src + w_dst; \
	for (i = y_src, i_dst = y_dst; i < y_limit; ++i, i_dst++) { \
		for (j = x_src, j_dst = x_dst; j < x_limit; ++j, j_dst++) { \
			operation \
		} \
	 } \
} while(0)

#define blend_to(src_fmt, operation) \
	func_common_sig(blend_to_, src_fmt) \
	{ \
		for_each_pixel_do(operation); \
		alpha_blend(src_pixel, j, i, dst_composer);
	}

#define copy_to_wb(output_fmt, operation) \
	func_common_sig(copy_to_wb_, src_fmt) \
	{ \
		for_each_pixel_do(operation); \
	}

#define copy_plane_to_output(output_fmt, operation) \
	func_common_sig(copy_plane_to_output_, src_fmt) \
	{ \
		for_each_pixel_do(operation); \
	}

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

static void alpha_blend(u64 argb_src, int y, int x,
			struct vkms_composer *dst_composer);

blend_to(ARGB8888,
	u8 *pixel_addr = packed_pixels_addr(src_composer, j, i);

	/*
	 * Organizes the channels in their respective positions and converts
	 * the 8 bits channel to 16.
	 * The 257 is the "conversion ratio". This number is obtained by the
	 * (2^16 - 1) / (2^8 - 1) division. Which, in this case, tries to get
	 * the best color value in a color space with more possibilities.
	 * And a similar idea applies to others RGB color conversions.
	 */
	src_pixel = ((u64)pixel_addr[3] * 257) << 48 |
			((u64)pixel_addr[2] * 257) << 32 |
			((u64)pixel_addr[1] * 257) << 16 |
			((u64)pixel_addr[0] * 257);

)

blend_to(XRGB8888,
	u8 *pixel_addr = packed_pixels_addr(src_composer, j, i);

	/*
	 * The same as the ARGB8888 but with the alpha channel as the
	 * maximum value as possible.
	 */
	src_pixel = 0xffffllu << 48 |
			((u64)pixel_addr[2] * 257) << 32 |
			((u64)pixel_addr[1] * 257) << 16 |
			((u64)pixel_addr[0] * 257);

	alpha_blend(src_pixel, j, i, dst_composer);
)

blend_to(ARGB16161616,
	__le64 *pixel_addr = packed_pixels_addr(src_composer, j, i);

	/*
	 * Because the format byte order is in little-endian and this code
	 * needs to run on big-endian machines too, we need modify
	 * the byte order from little-endian to the CPU native byte order.
	 */
	src_pixel = le64_to_cpu(*pixel_addr);
)

/*
 * The following functions are used as blend operations. But unlike the
 * `alpha_blend`, these functions take an ARGB16161616 pixel from the
 * source, convert it to a specific format, and store it in the destination.
 *
 * They are used in the `compose_active_planes` and `write_wb_buffer` to
 * copy and convert one pixel from/to the output buffer to/from
 * another buffer (e.g. writeback buffer, primary plane buffer).
 */

static void convert_to_ARGB8888(u64 argb_src, int x, int y,
				struct vkms_composer *dst_composer)
{
	u8 *pixel_addr = packed_pixels_addr(dst_composer, x, y);

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
	pixel_addr[0] = DIV_ROUND_UP(argb_src & 0xffffllu, 257);
	pixel_addr[1] = DIV_ROUND_UP((argb_src & (0xffffllu << 16)) >> 16, 257);
	pixel_addr[2] = DIV_ROUND_UP((argb_src & (0xffffllu << 32)) >> 32, 257);
	pixel_addr[3] = DIV_ROUND_UP((argb_src & (0xffffllu << 48)) >> 48, 257);
}

static void convert_to_XRGB8888(u64 argb_src, int x, int y,
				struct vkms_composer *dst_composer)
{
	u8 *pixel_addr = packed_pixels_addr(dst_composer, x, y);

	pixel_addr[0] = DIV_ROUND_UP(argb_src & 0xffffllu, 257);
	pixel_addr[1] = DIV_ROUND_UP((argb_src & (0xffffllu << 16)) >> 16, 257);
	pixel_addr[2] = DIV_ROUND_UP((argb_src & (0xffffllu << 32)) >> 32, 257);
	pixel_addr[3] = 0xff;
}

static void convert_to_ARGB16161616(u64 argb_src, int x, int y,
				    struct vkms_composer *dst_composer)
{
	__le64 *pixel_addr = packed_pixels_addr(dst_composer, x, y);

	*pixel_addr = cpu_to_le64(argb_src);
}

#endif /* _VKMS_FORMATS_H_ */
