/* SPDX-License-Identifier: GPL-2.0+ */

#include <drm/drm_rect.h>

#define pixel_offset(composer, y, x) \
	((composer)->offset + ((y) * (composer)->pitch) + ((x) * (composer)->cpp))

/*
 * FP stands for _Fixed Point_ and **not** _Float Point_
 * The following macros help doing fixed point arithmetic.
 */
/* With FP scale 19 we have 13 and 19 bits of integer and fractional parts
 * respectively.
 *  | 0000 0000 0000 0.000 0000 0000 0000 0000 |
 * 31                                          0
 */
#define FP_SCALE 19

#define LF_TO_FP(a) ((a) * (u64)(1 << FP_SCALE))
#define FP_TO_LF(a) ((a) / (u64)(1 << FP_SCALE))
#define INT_TO_FP(a) ((a) << FP_SCALE)
#define FP_MUL(a, b) ((s32)(((s64)(a) * (b)) >> FP_SCALE))
#define FP_DIV(a, b) ((s32)(((s64)(a) << FP_SCALE) / (b)))
/* This macro converts a fixed point number to int, and round half up it */
#define FP_TO_INT_ROUND(a) (((a) + (1 << (FP_SCALE - 1))) >> FP_SCALE)

void *packed_pixels_addr(struct vkms_composer *composer, int y, int x)
{
	int offset = pixel_offset(composer, y, x);

	return (u8 *)composer->map[0].vaddr + offset;
}

u64 ARGB8888_to_ARGB16161616(struct vkms_composer *composer, int y, int x)
{
	u8 *pixel_addr = packed_pixels_addr(composer, y, x);

	return ((u64)pixel_addr[3] * 257) << 48 |
	       ((u64)pixel_addr[2] * 257) << 32 |
	       ((u64)pixel_addr[1] * 257) << 16 |
	       ((u64)pixel_addr[0] * 257);
}

u64 XRGB8888_to_ARGB16161616(struct vkms_composer *composer, int y, int x)
{
	u8 *pixel_addr = packed_pixels_addr(composer, y, x);

	return 0xffffllu << 48 |
	       ((u64)pixel_addr[2] * 257) << 32 |
	       ((u64)pixel_addr[1] * 257) << 16 |
	       ((u64)pixel_addr[0] * 257);
}

u64 ARGB16161616_to_ARGB16161616(struct vkms_composer *composer, int y, int x)
{
	__le64 *pixel_addr = packed_pixels_addr(composer, y, x);

	return le64_to_cpu(*pixel_addr);
}

u64 XRGB16161616_to_ARGB16161616(struct vkms_composer *composer, int y, int x)
{
	__le64 *pixel_addr = packed_pixels_addr(composer, y, x);

	return le64_to_cpu(*pixel_addr) | (0xffffllu << 48);
}

u64 RGB565_to_ARGB16161616(struct vkms_composer *composer, int y, int x)
{
	__le16 *pixel_addr = packed_pixels_addr(composer, y, x);

	u16 rgb_565 = le16_to_cpu(*pixel_addr);
	int fp_r = INT_TO_FP((rgb_565 & (0x1f << 11)) >> 11);
	int fp_g = INT_TO_FP((rgb_565 & (0x3f << 5)) >> 5);
	int fp_b = INT_TO_FP(rgb_565 & 0x1f);

	/*
	* The magic constants came from 65535(2^16 - 1) divided by
	* 31(2^5 -1) and 63(2^6 - 1) respectively.
	*/
	int fp_rb_ratio = LF_TO_FP(2114.032258065);
	int fp_g_ratio = LF_TO_FP(1040.238095238);

	u64 r = (u64)FP_TO_INT_ROUND(FP_MUL(fp_r, fp_rb_ratio));
	u64 g = (u64)FP_TO_INT_ROUND(FP_MUL(fp_g, fp_g_ratio));
	u64 b = (u64)FP_TO_INT_ROUND(FP_MUL(fp_b, fp_rb_ratio));

	return 0xffffllu << 48 | r << 32 | g << 16 | b;
}

void convert_to_ARGB8888(u64 argb_src1, u64 argb_src2, int y, int x,
			 struct vkms_composer *dst_composer)
{
	u8 *pixel_addr = packed_pixels_addr(dst_composer, y, x);
	int FP_a = INT_TO_FP((argb_src1 & (0xffffllu << 48)) >> 48);
	int FP_r = INT_TO_FP((argb_src1 & (0xffffllu << 32)) >> 32);
	int FP_g = INT_TO_FP((argb_src1 & (0xffffllu << 16)) >> 16);
	int FP_b = INT_TO_FP(argb_src1 & 0xffffllu);
	int FP_ratio = INT_TO_FP(257);

	pixel_addr[0] = (u8)FP_TO_INT_ROUND(FP_DIV(FP_b, FP_ratio));
	pixel_addr[1] = (u8)FP_TO_INT_ROUND(FP_DIV(FP_g, FP_ratio));
	pixel_addr[2] = (u8)FP_TO_INT_ROUND(FP_DIV(FP_r, FP_ratio));
	pixel_addr[3] = (u8)FP_TO_INT_ROUND(FP_DIV(FP_a, FP_ratio));
}

void convert_to_XRGB8888(u64 argb_src1, u64 argb_src2, int y, int x,
			 struct vkms_composer *dst_composer)
{
	u8 *pixel_addr = packed_pixels_addr(dst_composer, y, x);
	int FP_r = INT_TO_FP((argb_src1 & (0xffffllu << 32)) >> 32);
	int FP_g = INT_TO_FP((argb_src1 & (0xffffllu << 16)) >> 16);
	int FP_b = INT_TO_FP(argb_src1 & 0xffffllu);
	int FP_ratio = INT_TO_FP(257);

	pixel_addr[0] = (u8)FP_TO_INT_ROUND(FP_DIV(FP_b, FP_ratio));
	pixel_addr[1] = (u8)FP_TO_INT_ROUND(FP_DIV(FP_g, FP_ratio));
	pixel_addr[2] = (u8)FP_TO_INT_ROUND(FP_DIV(FP_r, FP_ratio));
	pixel_addr[3] = (u8)0xffllu;
}

void convert_to_ARGB16161616(u64 argb_src1, u64 argb_src2, int y, int x,
			     struct vkms_composer *dst_composer)
{
	__le64 *pixel_addr = packed_pixels_addr(dst_composer, y, x);

	*pixel_addr = cpu_to_le64(argb_src1);
}

void convert_to_XRGB16161616(u64 argb_src1, u64 argb_src2, int y, int x,
			     struct vkms_composer *dst_composer)
{
	__le64 *pixel_addr = packed_pixels_addr(dst_composer, y, x);

	*pixel_addr = cpu_to_le64(argb_src1 | (0xffffllu << 48));
}

void convert_to_RGB565(u64 argb_src1, u64 argb_src2, int y, int x,
		     struct vkms_composer *dst_composer)
{
	u16 *pixel_addr = packed_pixels_addr(dst_composer, y, x);

	int fp_r = INT_TO_FP((argb_src1 & (0xffffllu << 32)) >> 32);
	int fp_g = INT_TO_FP((argb_src1 & (0xffffllu << 16)) >> 16);
	int fp_b = INT_TO_FP(argb_src1 & 0xffffllu);

	int fp_rb_ratio = LF_TO_FP(2114.032258065);
	int fp_g_ratio = LF_TO_FP(1040.238095238);

	u16 r = (u16)FP_TO_INT_ROUND(FP_DIV(fp_r, fp_rb_ratio));
	u16 g = (u16)FP_TO_INT_ROUND(FP_DIV(fp_g, fp_g_ratio));
	u16 b = (u16)FP_TO_INT_ROUND(FP_DIV(fp_b, fp_rb_ratio));

	*pixel_addr = cpu_to_le16(r << 11 | g << 5 | b);
}
