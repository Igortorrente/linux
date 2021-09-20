// SPDX-License-Identifier: GPL-2.0+

#include <linux/crc32.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_vblank.h>

#include "vkms_drv.h"
#include "vkms_formats.h"

#define get_output_vkms_composer(buffer_pointer, w, h)              \
	((struct vkms_composer) {                                   \
		.fb = (struct drm_framebuffer) {                    \
			.format = &(struct drm_format_info) {       \
				.format = DRM_FORMAT_ARGB16161616,  \
			},                                          \
		},                                                  \
		.map[0] = (buffer_pointer),                         \
		.src = (struct drm_rect) {                          \
			.x2 = (w),                                  \
			.y2 = (h),                                  \
		},                                                  \
		.dst = (struct drm_rect) {                          \
			.x2 = (w),                                  \
			.y2 = (h),                                  \
		},                                                  \
		.cpp = sizeof(u64),                                 \
		.pitch = (w) * sizeof(u64)                          \
	})

/**
 * compute_crc - Compute CRC value on output frame
 *
 * @vaddr: address to final framebuffer
 * @composer: framebuffer's metadata
 *
 * returns CRC value computed using crc32 on the visible portion of
 * the final framebuffer at vaddr_out
 */
static uint32_t compute_crc(const u64 *vaddr,
			    const struct vkms_composer *composer)
{
	int h = drm_rect_height(&composer->dst);
	int w = drm_rect_width(&composer->dst);

	return crc32_le(0, (void *)vaddr, w * h * sizeof(u64));
}

static __le16 blend_channel(u16 src, u16 dst, u16 alpha)
{
	u64 pre_blend;
	u16 new_color;

	pre_blend = (src * 0xffff + dst * (0xffff - alpha));

	/* Faster div by 65535 */
	new_color = ((pre_blend + ((pre_blend + 0x10001) >> 16)) >> 16);

	return cpu_to_le16(new_color);
}

/**
 * alpha_blend - alpha blending equation
 * @argb_src1: pixel of the source plane on premultiplied alpha mode
 * @argb_src2: pixel of the destiny planes on premultiplied alpha mode
 * @x: The x coodinate(width) of the pixel
 * @y: The y coodinate(heigth) of the pixel
 * @argb_dst: dst pixel completely opaque
 *
 * blend pixels using premultiplied blend formula. The current DRM assumption
 * is that pixel color values have been already pre-multiplied with the alpha
 * channel values. See more drm_plane_create_blend_mode_property(). Also, this
 * formula assumes a completely opaque background.
 */
static void alpha_blend(u64 argb_src1, u64 argb_src2, int y, int x,
			struct vkms_composer *dst_composer)
{
	u16 *output_pixel = packed_pixels_addr(dst_composer, y, x);

	u16 *src1 = (u16 *)&argb_src1;
	u16 *src2 = (u16 *)&argb_src2;

	output_pixel[0] = blend_channel(src1[0], src2[0], src1[3]);
	output_pixel[1] = blend_channel(src1[1], src2[1], src1[3]);
	output_pixel[2] = blend_channel(src1[2], src2[2], src1[3]);
	output_pixel[3] = 0xffff;
}

/**
 * @src_composer: source framebuffer's metadata
 * @dst_composer: destiny framebuffer's metadata
 * @get_src_pixel: pointer to function that returns a ARGB16161616 of the
 *                 source plane
 * @get_dst_pixel: pointer to function that returns a ARGB16161616 of the
 *                 destiny plane
 * @pixel_blend: pointer to a function that blends the value returned from
 *               the get_src_pixel with value from the get_dst_pixel
 *
 * Using the pixel_blend function passed as parameter, it blends all the
 * pixels from src planes into a output buffer.
 * Information of the output buffer are in the dst_composer parameter
 * and the source plane are in the src_composer.
 * The get_src_pixel will use the src_composer to get the respective pixel,
 * convert to the ARGB_16161616, and return it.
 * And the same is true for the dst_composer and get_dst_pixel respectively.
 * And the finally, the blend function will receive the dst_composer, src
 * and dst pixels. Blend them, and store in the output using the dst_composer
 * buffer information.
 *
 * TODO: completely clear the primary plane (a = 0xff) before starting to blend
 * pixel color values
 */
static void blend(struct vkms_composer *src_composer,
		  struct vkms_composer *dst_composer,
		  u64 (*get_src_pixel)(struct vkms_composer *composer,
				      int y, int x),
		  u64 (*get_dst_pixel)(struct vkms_composer *composer,
				       int y, int x),
		  void pixel_blend(u64 argb_src1, u64 argb_src2, int y, int x,
				   struct vkms_composer *dst_composer))
{
	int i, j, j_dst, i_dst;
	u64 pixel_dst, pixel_src;

	int x_src = src_composer->src.x1 >> 16;
	int y_src = src_composer->src.y1 >> 16;

	int x_dst = src_composer->dst.x1;
	int y_dst = src_composer->dst.y1;
	int h_dst = drm_rect_height(&src_composer->dst);
	int w_dst = drm_rect_width(&src_composer->dst);

	int y_limit = y_src + h_dst;
	int x_limit = x_src + w_dst;

	for (i = y_src, i_dst = y_dst; i < y_limit; ++i, i_dst++) {
		for (j = x_src, j_dst = x_dst; j < x_limit; ++j, j_dst++) {
			pixel_src = get_src_pixel(src_composer, i, j);
			pixel_dst = get_dst_pixel(dst_composer, i_dst, j_dst);

			pixel_blend(pixel_src, pixel_dst, i_dst, j_dst,
				    dst_composer);
		}
	}
}

static u64 ((*get_format_function(u32 format))
	    (struct vkms_composer *, int, int))
{
	if (format == DRM_FORMAT_ARGB8888)
		return &ARGB8888_to_ARGB16161616;
	else if (format == DRM_FORMAT_ARGB16161616)
		return &ARGB16161616_to_ARGB16161616;
	else if (format == DRM_FORMAT_XRGB16161616)
		return &XRGB16161616_to_ARGB16161616;
	else
		return &XRGB8888_to_ARGB16161616;
}

static void ((*get_pixel_blend_function(u32 format))
	     (u64, u64, int, int, struct vkms_composer *))
{
	if (format == DRM_FORMAT_ARGB8888)
		return &convert_to_ARGB8888;
	else if (format == DRM_FORMAT_ARGB16161616)
		return &convert_to_ARGB16161616;
	else if (format == DRM_FORMAT_XRGB16161616)
		return &convert_to_XRGB16161616;
	else
		return &convert_to_XRGB8888;
}

static void compose_plane(struct vkms_composer *src_composer,
			  struct vkms_composer *dst_composer,
			  void pixel_blend(u64 argb_src1, u64 argb_src2,
					   int y, int x,
					   struct vkms_composer *dst_composer))
{
	u64 (*get_src_pixel)(struct vkms_composer *composer, int y, int x);
	u64 (*get_dst_pixel)(struct vkms_composer *composer, int y, int x);
	u32 src_format = src_composer->fb.format->format;
	u32 dst_format = dst_composer->fb.format->format;

	get_src_pixel = get_format_function(src_format);
	get_dst_pixel = get_format_function(dst_format);

	blend(src_composer, dst_composer, get_src_pixel,
	      get_dst_pixel, pixel_blend);
}

static u64 *compose_active_planes(struct vkms_composer *primary_composer,
				  struct vkms_crtc_state *crtc_state)
{
	void (*pixel_blend)(u64 argb_src1, u64 argb_src2, int y, int x,
			    struct vkms_composer *dst_composer);
	struct vkms_plane_state **active_planes = crtc_state->active_planes;
	int h = drm_rect_height(&primary_composer->dst);
	int w = drm_rect_width(&primary_composer->dst);
	struct vkms_composer dst_composer;
	u64 *vaddr_out;
	int i;

	if (WARN_ON(dma_buf_map_is_null(&primary_composer->map[0])))
		return NULL;

	vaddr_out = kvzalloc(w * h * sizeof(u64), GFP_KERNEL);
	if (!vaddr_out) {
		DRM_ERROR("Cannot allocate memory for output frame.");
		return NULL;
	}

	dst_composer = get_output_vkms_composer(vaddr_out, w, h);
	pixel_blend = get_pixel_blend_function(DRM_FORMAT_ARGB16161616);
	compose_plane(active_planes[0]->composer, &dst_composer, pixel_blend);

	/* If there are other planes besides primary, we consider the active
	 * planes should be in z-order and compose them associatively:
	 * ((primary <- overlay) <- cursor)
	 */
	for (i = 1; i < crtc_state->num_active_planes; i++)
		compose_plane(active_planes[i]->composer,
			      &dst_composer, alpha_blend);

	return vaddr_out;
}

static void write_wb_buffer(struct vkms_writeback_job *active_wb,
			    struct vkms_composer *primary_composer,
			    u64 *vaddr_out)
{
	u64 (*get_dst_pixel)(struct vkms_composer *composer, int y, int x);
	u64 (*get_src_pixel)(struct vkms_composer *composer, int y, int x);
	void (*pixel_blend)(u64 argb_src1, u64 argb_src2, int y, int x,
			    struct vkms_composer *dst_composer);
	struct drm_framebuffer *dst_fb = &active_wb->composer.fb;
	int h = drm_rect_height(&primary_composer->dst);
	int w = drm_rect_width(&primary_composer->dst);
	struct vkms_composer src_composer;

	src_composer = get_output_vkms_composer(vaddr_out, w, h);
	active_wb->composer.src = primary_composer->src;
	active_wb->composer.dst = primary_composer->dst;

	get_src_pixel = get_format_function(DRM_FORMAT_ARGB16161616);
	get_dst_pixel = get_format_function(dst_fb->format->format);
	pixel_blend = get_pixel_blend_function(dst_fb->format->format);

	blend(&src_composer, &active_wb->composer, get_src_pixel,
	      get_dst_pixel, pixel_blend);
}

/**
 * vkms_composer_worker - ordered work_struct to compute CRC
 *
 * @work: work_struct
 *
 * Work handler for composing and computing CRCs. work_struct scheduled in
 * an ordered workqueue that's periodically scheduled to run by
 * _vblank_handle() and flushed at vkms_atomic_crtc_destroy_state().
 */
void vkms_composer_worker(struct work_struct *work)
{
	struct vkms_crtc_state *crtc_state = container_of(work,
						struct vkms_crtc_state,
						composer_work);
	struct drm_crtc *crtc = crtc_state->base.crtc;
	struct vkms_writeback_job *active_wb = crtc_state->active_writeback;
	struct vkms_output *out = drm_crtc_to_vkms_output(crtc);
	struct vkms_composer *primary_composer = NULL;
	struct vkms_plane_state *act_plane = NULL;
	bool crc_pending, wb_pending;
	u64 *vaddr_out = NULL;
	u32 crc32 = 0;
	u64 frame_start, frame_end;

	spin_lock_irq(&out->composer_lock);
	frame_start = crtc_state->frame_start;
	frame_end = crtc_state->frame_end;
	crc_pending = crtc_state->crc_pending;
	wb_pending = crtc_state->wb_pending;
	crtc_state->frame_start = 0;
	crtc_state->frame_end = 0;
	crtc_state->crc_pending = false;
	spin_unlock_irq(&out->composer_lock);

	/*
	 * We raced with the vblank hrtimer and previous work already computed
	 * the crc, nothing to do.
	 */
	if (!crc_pending)
		return;

	if (crtc_state->num_active_planes >= 1) {
		act_plane = crtc_state->active_planes[0];
		if (act_plane->base.base.plane->type == DRM_PLANE_TYPE_PRIMARY)
			primary_composer = act_plane->composer;
	}

	if (!primary_composer)
		return;

	vaddr_out = compose_active_planes(primary_composer, crtc_state);
	if (!vaddr_out)
		return;

	if (wb_pending) {
		write_wb_buffer(active_wb, primary_composer, vaddr_out);
		drm_writeback_signal_completion(&out->wb_connector, 0);
		spin_lock_irq(&out->composer_lock);
		crtc_state->wb_pending = false;
		spin_unlock_irq(&out->composer_lock);
	}

	crc32 = compute_crc(vaddr_out, primary_composer);
	kvfree(vaddr_out);

	/*
	 * The worker can fall behind the vblank hrtimer, make sure we catch up.
	 */
	while (frame_start <= frame_end)
		drm_crtc_add_crc_entry(crtc, true, frame_start++, &crc32);
}

static const char * const pipe_crc_sources[] = {"auto"};

const char *const *vkms_get_crc_sources(struct drm_crtc *crtc,
					size_t *count)
{
	*count = ARRAY_SIZE(pipe_crc_sources);
	return pipe_crc_sources;
}

static int vkms_crc_parse_source(const char *src_name, bool *enabled)
{
	int ret = 0;

	if (!src_name) {
		*enabled = false;
	} else if (strcmp(src_name, "auto") == 0) {
		*enabled = true;
	} else {
		*enabled = false;
		ret = -EINVAL;
	}

	return ret;
}

int vkms_verify_crc_source(struct drm_crtc *crtc, const char *src_name,
			   size_t *values_cnt)
{
	bool enabled;

	if (vkms_crc_parse_source(src_name, &enabled) < 0) {
		DRM_DEBUG_DRIVER("unknown source %s\n", src_name);
		return -EINVAL;
	}

	*values_cnt = 1;

	return 0;
}

void vkms_set_composer(struct vkms_output *out, bool enabled)
{
	bool old_enabled;

	if (enabled)
		drm_crtc_vblank_get(&out->crtc);

	spin_lock_irq(&out->lock);
	old_enabled = out->composer_enabled;
	out->composer_enabled = enabled;
	spin_unlock_irq(&out->lock);

	if (old_enabled)
		drm_crtc_vblank_put(&out->crtc);
}

int vkms_set_crc_source(struct drm_crtc *crtc, const char *src_name)
{
	struct vkms_output *out = drm_crtc_to_vkms_output(crtc);
	bool enabled = false;
	int ret = 0;

	ret = vkms_crc_parse_source(src_name, &enabled);

	vkms_set_composer(out, enabled);

	return ret;
}
