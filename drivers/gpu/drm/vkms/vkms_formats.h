/* SPDX-License-Identifier: GPL-2.0+ */

#include <drm/drm_rect.h>

#define pixel_offset(composer, y, x) \
	((composer)->offset + ((y) * (composer)->pitch) + ((x) * (composer)->cpp))

#define get_v_plane_offset(composer, src_addr, h) \
	((u8 *)(src_addr) + (composer)->offset + (h) * (composer)->pitch)

#define get_uv_plane_offset get_v_plane_offset

/*
 * FP stands for _Fixed Point_ and **not** _Float Point_
 * LF stands for Long Float (i.e. double)
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
	* The magic ratio constants came from 65535(2^16 - 1) divided by
	* 31(2^5 - 1) and 63(2^6 - 1) respectively.
	*/
	int fp_rb_ratio = LF_TO_FP(2114.032258065);
	int fp_g_ratio = LF_TO_FP(1040.238095238);

	u64 r = (u64)FP_TO_INT_ROUND(FP_MUL(fp_r, fp_rb_ratio));
	u64 g = (u64)FP_TO_INT_ROUND(FP_MUL(fp_g, fp_g_ratio));
	u64 b = (u64)FP_TO_INT_ROUND(FP_MUL(fp_b, fp_rb_ratio));

	return 0xffffllu << 48 | r << 32 | g << 16 | b;
}

u64 yuv_8bit_to_argb16161616(int y, int u, int v)
{
	int r, g, b;
	u64 output;
	int fp_y = INT_TO_FP(y);
	int fp_u = INT_TO_FP(u);
	int fp_v = INT_TO_FP(v);
	int hr = INT_TO_FP(128);

	int fp_r = fp_y + FP_MUL(LF_TO_FP(1.370705), fp_v - hr);
	int fp_g = fp_y - FP_MUL(LF_TO_FP(0.698001), fp_v - hr)
			- FP_MUL(LF_TO_FP(0.337633), fp_u - hr);
	int fp_b = fp_y + FP_MUL(LF_TO_FP(1.732446), fp_u - hr);

	fp_r = FP_TO_INT_ROUND(fp_r);
	fp_g = FP_TO_INT_ROUND(fp_g);
	fp_b = FP_TO_INT_ROUND(fp_b);

	r = clamp(fp_r, 0, 255);
	g = clamp(fp_g, 0, 255);
	b = clamp(fp_b, 0, 255);

	output = 0xffffllu << 48 |
		 ((u64)r * 257) << 32 |
		 ((u64)g * 257) << 16 |
		 ((u64)b * 257);

	return cpu_to_le64(output);
}

u64 YUV420_to_ARGB16161616(struct vkms_composer *composer, int y, int x)
{
	int h_dst = drm_rect_height(&composer->dst);
	void *src_addr = (u8 *)composer->map[0].vaddr;

	u8 *u_plane_offset = get_v_plane_offset(composer, src_addr, h_dst);
	u8 *v_plane_offset = u_plane_offset + composer->pitch;

	int uv_offset = (x / 2 + y / 2) * composer->cpp;
	int y_offset = pixel_offset(composer, y, x);

	u8 Y = ((u8 *)src_addr)[y_offset];
	u8 U = u_plane_offset[uv_offset];
	u8 V = v_plane_offset[uv_offset];

	return yuv_8bit_to_argb16161616(Y, U, V);
}

u64 NV12_to_ARGB16161616(struct vkms_composer *composer, int y, int x)
{
	int h_dst = drm_rect_height(&composer->dst);
	void *src_addr = (u8 *)composer->map[0].vaddr;

	u8 *uv_plane_offset = get_uv_plane_offset(composer, src_addr, h_dst);

	int uv_offset = y / 2 * composer->pitch + x / 2 * composer->cpp;
	int y_offset = pixel_offset(composer, y, x);

	u8 Y = ((u8 *)src_addr)[y_offset];
	u8 U = uv_plane_offset[uv_offset];
	u8 V = uv_plane_offset[uv_offset + 1];

	return yuv_8bit_to_argb16161616(Y, U, V);
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

void convert_to_NV12(u64 argb_src1, u64 argb_src2, int y, int x,
		     struct vkms_composer *dst_composer)
{
}

void convert_to_YUV420(u64 argb_src1, u64 argb_src2, int y, int x,
		       struct vkms_composer *dst_composer)
{
}
