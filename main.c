#include <stdio.h>
#include <math.h>

#define s32 int
#define s64 long long int
#define u64 unsigned long long int
#define u32 unsigned int
#define __le16 short int
#define u16 unsigned short int
#define u8 unsigned char

#define FP_SCALE 15
#define INT_TO_FP(a) \
	((a) << FP_SCALE)
#define FP_TO_INT(a) \
	((s32)(a) >> FP_SCALE)
#define FP_MUL(a, b) \
	((s32)(((s64)(a) * (s64)(b)) >> FP_SCALE))
#define FP_DIV(a, b) \
	((s32)(((s64)(a) << FP_SCALE) / (b)))
#define LF_TO_FP(a) ((a) * (double)(1 << FP_SCALE))
#define FP_TO_LF(a) ((a) / (double)(1 << FP_SCALE))

#define FP_TO_INT_ROUND(a) (((a) + (1 << (FP_SCALE -1))) >> FP_SCALE)
#define DIV_ROUND_UP(n,d) (((n) + (d) - 1) / (d))

void printBits(size_t const size, void const * const ptr)
{
	unsigned char *b = (unsigned char*) ptr;
	unsigned char byte;
	int i, j;

	for (i = size-1; i >= 0; i--) {
		for (j = 7; j >= 0; j--) {
			byte = (b[i] >> j) & 1;
			printf("%u", byte);
		}
	}
	puts("");
}

static u64 RGB565_to_ARGB16161616(u16 rgb_565)
{
	int fp_r = INT_TO_FP((rgb_565 & (0x1f << 11)) >> 11);
	int fp_g = INT_TO_FP((rgb_565 & (0x3f << 5)) >> 5);
	int fp_b = INT_TO_FP(rgb_565 & 0x1f);
	printf("fp_r = %lf\n", FP_TO_LF(fp_r));
	printf("fp_g = %lf\n", FP_TO_LF(fp_g));
	printf("fp_b = %lf\n", FP_TO_LF(fp_b));

	u16 ra = (rgb_565 & (0x1f << 11)) >> 11;
	u16 ga =(rgb_565 & (0x3f << 5)) >> 5;
	u16 ba = rgb_565 & 0x1f;

	printf("rgb_565  = "); printBits(sizeof(u16), &rgb_565);
	printf("16bits R = "); printBits(sizeof(u16), &ra);
	printf("16bits G = "); printBits(sizeof(u16), &ga);
	printf("16bits B = "); printBits(sizeof(u16), &ba);

	/*
	 * The magic constants is the "conversion ratio" and is calculated
	 * dividing 65535(2^16 - 1) by 31(2^5 -1) and 63(2^6 - 1) respectively.
	 */
	int fp_rb_ratio = LF_TO_FP(2114.032258065);
	int fp_g_ratio = LF_TO_FP(1040.238095238);
	printf("fp_rb_ratio = %lf\n", FP_TO_LF(fp_rb_ratio));
	printf("fp_g_ratio = %lf\n", FP_TO_LF(fp_g_ratio));

	u64 r = FP_TO_INT_ROUND(FP_MUL(fp_r, fp_rb_ratio));
	u64 g = FP_TO_INT_ROUND(FP_MUL(fp_g, fp_g_ratio));
	u64 b = FP_TO_INT_ROUND(FP_MUL(fp_b, fp_rb_ratio));
	printf("FP_MUL(fp_r, fp_rb_ratio) = %lf\n", FP_TO_LF(FP_MUL(fp_r, fp_rb_ratio)));
	printf("FP_MUL(fp_g, fp_g_ratio) = %lf = %lx\n", FP_TO_LF(FP_MUL(fp_g, fp_g_ratio)), FP_MUL(fp_g, fp_g_ratio));
	printf("FP_MUL(fp_b, fp_rb_ratio) = %lf\n", FP_TO_LF(FP_MUL(fp_b, fp_rb_ratio)));
	printf("64 bits R = 0x%lx = 0b", r); printBits(sizeof(u64), &r);
	printf("64 bits G = 0x%lx = 0b", g); printBits(sizeof(u64), &g);
	printf("64 bits B = 0x%lx = 0b", b); printBits(sizeof(u64), &b);

	return 0xffffllu << 48 | r << 32 | g << 16 | b;
}


static __le16 blend_channel(u16 src, u16 dst, u16 alpha)
{
	u64 pre_blend;
	u16 new_color;

	pre_blend = (src * 0xffff + dst * (0xffff - alpha));

	/* Faster div by 65535 */
	new_color = DIV_ROUND_UP(pre_blend, 0xffff);

	return new_color;
}

static void alpha_blend(u64 argb_src1, u64 argb_src2)
{
	u16 output_pixel[4];

	u16 *src1 = (u16 *)&argb_src1;
	u16 *src2 = (u16 *)&argb_src2;

	output_pixel[0] = blend_channel(src1[0], src2[0], src1[3]);
	output_pixel[1] = blend_channel(src1[1], src2[1], src1[3]);
	output_pixel[2] = blend_channel(src1[2], src2[2], src1[3]);
	output_pixel[3] = 0xffff;
}

static u8 blend_channel_8bits(u8 src, u8 dst, u8 alpha)
{
	u32 pre_blend;
	u8 new_color;

	pre_blend = (src * 255 + dst * (255 - alpha));

	/* Faster div by 255 */
	new_color = ((pre_blend + ((pre_blend + 257) >> 8)) >> 8);

	return new_color;
}

static void alpha_blend_8bits(u64 argb_src1, u64 argb_src2)
{
	u8 output_pixel[4];

	u8 *src1 = (u8 *)&argb_src1;
	u8 *src2 = (u8 *)&argb_src2;

	output_pixel[0] = blend_channel_8bits(src1[0], src2[0], src1[3]);
	output_pixel[1] = blend_channel_8bits(src1[1], src2[1], src1[3]);
	output_pixel[2] = blend_channel_8bits(src1[2], src2[2], src1[3]);
	output_pixel[3] = 0xff;
}

int main()
{
	u64 result = RGB565_to_ARGB16161616(0b0000011111100000);
	printf("0b0000011111100000 to 0x%lx = 0b", result); printBits(sizeof(u64), &result);
	result = RGB565_to_ARGB16161616(0b1111100000000000);
	printf("0b0000011111100000 to 0x%lx = 0b", result); printBits(sizeof(u64), &result);
	result = RGB565_to_ARGB16161616(0b0000000000011111);
	printf("0b0000011111100000 to 0x%lx = 0b", result); printBits(sizeof(u64), &result);
	result = RGB565_to_ARGB16161616(0b0000000000000000);
	printf("0b0000000000000000 to 0x%lx = 0b", result); printBits(sizeof(u64), &result);
	result = RGB565_to_ARGB16161616(0b0000000000000001);
	printf("0b0000000000000001 to 0x%lx = 0b", result); printBits(sizeof(u64), &result);
	alpha_blend(0xffff0000ffff0000llu, 0xffff000000000000llu);
	alpha_blend_8bits(0xff00ff00llu, 0xff000000llu);
	printf("65535 / 31 = %lf\n", FP_TO_LF(FP_DIV(65535, 31)));

	int y = 101;
	int u = 32;
	int v = 202;

	const int c[] = {
		INT_TO_FP(128),
		LF_TO_FP(1.370705),
		LF_TO_FP(0.698001),
		LF_TO_FP(1.732446),
		LF_TO_FP(0.337633)
	};

	int FP_Y = INT_TO_FP(y);
	int FP_U = INT_TO_FP(u);
	int FP_V = INT_TO_FP(v);


	double div = FP_TO_LF(FP_DIV(FP_Y, FP_U));
	printf("%d / %d = %lf\n", y, u, div);
	div = FP_TO_LF(FP_DIV(FP_U, FP_Y));
	printf("%d / %d = %lf\n", u, y, div);

	double r = y + ((double)1.370705 * (v - 128));
	double g = y - ((double)0.698001 * (v - 128)) - ((double)0.337633 * (u - 128));
	double b = y + ((double)1.732446 * (u - 128));

	int FP_R = FP_Y + FP_MUL(LF_TO_FP(1.370705), FP_V - INT_TO_FP(128));
	int FP_G = FP_Y - FP_MUL(LF_TO_FP(0.698001), FP_V - INT_TO_FP(128))
		 - FP_MUL(LF_TO_FP(0.337633), FP_U - INT_TO_FP(128));
	int FP_B = FP_Y + FP_MUL(LF_TO_FP(1.732446), FP_U - INT_TO_FP(128));

	for (int i = 0; i < 6; i++)
		printf("Contant[%d] = %3.12lf(Double) = %d(int)\n", i, FP_TO_LF(c[i]), c[i]);

	printf("FP_TO_LF(FP_R)        = %.8lf |//////| r = %lf\n", FP_TO_LF(FP_R), r);
	printf("FP_TO_LF(FP_G)        = %.8lf |//////| g = %lf\n", FP_TO_LF(FP_G), g);
	printf("FP_TO_lF(FP_B)        = %.8lf |//////| b = %lf\n", FP_TO_LF(FP_B), b);

	printf("FP_TO_INT(FP_R)       = %4d |//////| r = %lf\n", FP_TO_INT(FP_R), r);
	printf("FP_TO_INT(FP_G)       = %4d |//////| g = %lf\n", FP_TO_INT(FP_G), g);
	printf("FP_TO_INT(FP_B)       = %4d |//////| b = %lf\n", FP_TO_INT(FP_B), b);

	printf("FP_TO_INT_ROUND(FP_R) = %4d |//////| r = %lf\n", FP_TO_INT_ROUND(FP_R), r);
	printf("FP_TO_INT_ROUND(FP_G) = %4d |//////| g = %lf\n", FP_TO_INT_ROUND(FP_G), g);
	printf("FP_TO_INT_ROUND(FP_B) = %4d |//////| b = %lf\n", FP_TO_INT_ROUND(FP_B), b);

	FP_R = (s32)round(FP_TO_LF(FP_R));
	FP_G = (s32)round(FP_TO_LF(FP_G));
	FP_B = (s32)round(FP_TO_LF(FP_B));

	printf("FP_R                  = %4d |//////| r = %d\n", FP_R, r);
	printf("FP_G                  = %4d |//////| g = %d\n", FP_G, g);
	printf("FP_B                  = %4d |//////| b = %d\n", FP_B, b);

	char *endian = (char *)&y;
	y = 0xea23da54;
	printf("0xea23da54\n");
	for (int i = 0; i < 4; i++)
		printf("endian[%d](%p) = %x\n", i, &endian[i], (unsigned int)endian[i] & 0xff);

	int be = ((int)endian[0] & 0xff) << 24 |
			 ((int)endian[1] & 0xff) << 16 |
			 ((int)endian[2] & 0xff) << 8  |
			 ((int)endian[3] & 0xff);

	printf("be = 0x%x\n", be);

	int le = ((int)endian[3] & 0xff) << 24 |
			 ((int)endian[2] & 0xff) << 16 |
			 ((int)endian[1] & 0xff) << 8  |
			 ((int)endian[0] & 0xff);

	printf("le = 0x%x\n", le);

	unsigned char a = 0xff;
	char sa = 0xff;
	printf("a = %x (u64)a = %lx\n",a, (unsigned long long)a);
	printf("sa = %x (u64)sa = %lx\n",sa, (unsigned long long)sa);

	return 0;
}
