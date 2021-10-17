// SPDX-License-Identifier: GPL-2.0+

#include <linux/crc32.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_vblank.h>

#include "vkms_drv.h"
#include "vkms_formats.h"

#define get_output_vkms_composer(buffer_pointer, composer)		\
	((struct vkms_composer) {					\
		.fb = (struct drm_framebuffer) {			\
			.format = &(struct drm_format_info) {		\
				.format = DRM_FORMAT_ARGB16161616,	\
			},						\
		},							\
		.map[0].vaddr = (buffer_pointer),			\
		.src = (composer)->src,					\
		.dst = (composer)->dst,					\
		.cpp = sizeof(u64),					\
		.pitch = drm_rect_width(&(composer)->dst) * sizeof(u64)	\
	})

struct vkms_pixel_composition_functions {
	u64 (*get_src_pixel)(struct vkms_composer *composer, int x, int y);
	void (*set_output_pixel)(u64 argb_src, int x, int y,
		       struct vkms_composer *dst_composer);
};

/**
 * compute_crc - Compute CRC value on output frame
 *
 * @vaddr: address to final framebuffer
 * @composer: framebuffer's metadata
 *
 * returns CRC value computed using crc32 on the visible portion of
 * the final framebuffer at vaddr_out
 */
static uint32_t compute_crc(const __le64 *vaddr,
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

	new_color = DIV_ROUND_UP(pre_blend, 0xffff);

	return cpu_to_le16(new_color);
}

/**
 * alpha_blend - alpha blending equation
 * @argb_src: pixel of the source plane on premultiplied alpha mode
 * @x: The x coodinate(width) of the pixel
 * @y: The y coodinate(heigth) of the pixel
 *
 * blend pixels using premultiplied blend formula. The current DRM assumption
 * is that pixel color values have been already pre-multiplied with the alpha
 * channel values. See more drm_plane_create_blend_mode_property(). Also, this
 * formula assumes a completely opaque background.
 *
 * For performance reasons this function also fetches the dst_pixel.
 * We use the information that one of the source pixels is the output buffer
 * with ARGB16161616 format to fetch it here instead of separate function,
 * and this save us a indirect function call to each pixel.
 */
static void alpha_blend(u64 argb_src, int y, int x,
			struct vkms_composer *dst_composer)
{
	__le16 *output_pixel = packed_pixels_addr(dst_composer, y, x);

	u16 src1_a = (argb_src & (0xffffllu << 48)) >> 48;
	u16 src1_r = (argb_src & (0xffffllu << 32)) >> 32;
	u16 src1_g = (argb_src & (0xffffllu << 16)) >> 16;
	u16 src1_b = argb_src & 0xffffllu;

	u16 src2_r = le16_to_cpu(output_pixel[2]);
	u16 src2_g = le16_to_cpu(output_pixel[1]);
	u16 src2_b = le16_to_cpu(output_pixel[0]);

	output_pixel[0] = blend_channel(src1_b, src2_b, src1_a);
	output_pixel[1] = blend_channel(src1_g, src2_g, src1_a);
	output_pixel[2] = blend_channel(src1_r, src2_r, src1_a);
	output_pixel[3] = 0xffff;
}

static void compose_plane(struct vkms_composer *src_composer,
			  struct vkms_composer *dst_composer,
			  struct vkms_pixel_composition_functions *funcs)
{
	if (format == DRM_FORMAT_ARGB8888)
		func_name(blend_to_, ARGB8888)(src_composer, dst_composer);
	else if (format == DRM_FORMAT_ARGB16161616)
		func_name(blend_to_, ARGB16161616)(src_composer, dst_composer);
	else
		func_name(blend_to_, XRGB8888)(src_composer, dst_composer);
}

static __le64 *compose_active_planes(struct vkms_composer *primary_composer,
				     struct vkms_crtc_state *crtc_state)
{
	struct vkms_plane_state **active_planes = crtc_state->active_planes;
	int h = drm_rect_height(&primary_composer->dst);
	int w = drm_rect_width(&primary_composer->dst);
	struct vkms_pixel_composition_functions funcs;
	struct vkms_composer dst_composer;
	__le64 *vaddr_out;
	int i;

	if (WARN_ON(dma_buf_map_is_null(&primary_composer->map[0])))
		return NULL;

	vaddr_out = kvzalloc(w * h * sizeof(__le64), GFP_KERNEL);
	if (!vaddr_out) {
		DRM_ERROR("Cannot allocate memory for output frame.");
		return NULL;
	}

	dst_composer = get_output_vkms_composer(vaddr_out, primary_composer);
	funcs.set_output_pixel = get_set_output_pixel_function(DRM_FORMAT_ARGB16161616);
	compose_plane(active_planes[0]->composer, &dst_composer, &funcs);

	/* If there are other planes besides primary, we consider the active
	 * planes should be in z-order and compose them associatively:
	 * ((primary <- overlay) <- cursor)
	 */
	funcs.set_output_pixel = alpha_blend;
	for (i = 1; i < crtc_state->num_active_planes; i++)
		compose_plane(active_planes[i]->composer, &dst_composer, &funcs);

	return vaddr_out;
}

static void write_wb_buffer(struct vkms_writeback_job *active_wb,
			    struct vkms_composer *primary_composer,
			    __le64 *vaddr_out)
{
	u32 dst_fb_format = active_wb->composer.fb.format->format;
	struct vkms_pixel_composition_functions funcs;
	struct vkms_composer src_composer;

	src_composer = get_output_vkms_composer(vaddr_out, primary_composer);
	funcs.set_output_pixel = get_set_output_pixel_function(dst_fb_format);
	active_wb->composer.src = primary_composer->src;
	active_wb->composer.dst = primary_composer->dst;

	compose_plane(&src_composer, &active_wb->composer, &funcs);
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
	__le64 *vaddr_out = NULL;
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
