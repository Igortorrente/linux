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
		.fb = &(struct drm_framebuffer) {			\
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
	void (*get_src_line)(struct vkms_composer *composer, int y,
			     u64 *line_buffer);
	void (*set_output_line)(struct vkms_composer *src_composer,
				struct vkms_composer *dst_composer,
				int y, u64 *line_buffer);
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
 * @src_composer: source framebuffer's metadata
 * @dst_composer: destination framebuffer's metadata
 * @y: The y coodinate(heigth) of the line that will be processed
 * @line_buffer: The line with the pixels from src_compositor
 *
 * blend pixels using premultiplied blend formula. The current DRM assumption
 * is that pixel color values have been already pre-multiplied with the alpha
 * channel values. See more drm_plane_create_blend_mode_property(). Also, this
 * formula assumes a completely opaque background.
 *
 * For performance reasons this function also fetches the pixels from the
 * destination of the frame line y.
 * We use the information that one of the source pixels are in the output
 * buffer to fetch it here instead of separate function. And because the
 * output format is ARGB16161616, we know that they don't need to be
 * converted.
 * This save us a indirect function call for each line.
 */
static void alpha_blend(struct vkms_composer *src_composer,
			struct vkms_composer *dst_composer,
			int y, u64 *line_buffer)
{
	int i, x_dst = src_composer->dst.x1;
	__le16 *output_pixel = packed_pixels_addr(dst_composer, x_dst, y);

	for_each_pixel_in_line(i, src_composer) {
		u16 src1_a = line_buffer[i] >> 48;
		u16 src1_r = (line_buffer[i] >> 32) & 0xffff;
		u16 src1_g = (line_buffer[i] >> 16) & 0xffff;
		u16 src1_b = line_buffer[i] & 0xffff;

		u16 src2_r = le16_to_cpu(output_pixel[2]);
		u16 src2_g = le16_to_cpu(output_pixel[1]);
		u16 src2_b = le16_to_cpu(output_pixel[0]);

		output_pixel[0] = blend_channel(src1_b, src2_b, src1_a);
		output_pixel[1] = blend_channel(src1_g, src2_g, src1_a);
		output_pixel[2] = blend_channel(src1_r, src2_r, src1_a);
		output_pixel[3] = 0xffff;

		output_pixel += 4;
	}
}

/**
 * @src_composer: source framebuffer's metadata
 * @dst_composer: destiny framebuffer's metadata
 * @funcs: A struct containing all the composition functions(get_src_line,
 *         and set_output_pixel)
 * @line_buffer: The line with the pixels from src_compositor
 *
 * Using the pixel_blend function passed as parameter, this function blends
 * all pixels from src plane into a output buffer (with a blend function
 * passed as parameter).
 * Information of the output buffer is in the dst_composer parameter
 * and the source plane in the src_composer.
 * The get_src_line will use the src_composer to get the respective line,
 * convert, and return it as ARGB_16161616.
 * And finally, the blend function will receive the dst_composer, dst_composer,
 * the line y coodinate, and the line buffer. Blend all pixels, and store the
 * result in the output.
 *
 * TODO: completely clear the primary plane (a = 0xff) before starting to blend
 * pixel color values
 */
static void blend(struct vkms_composer *src_composer,
		  struct vkms_composer *dst_composer,
		  struct vkms_pixel_composition_functions *funcs,
		  u64 *line_buffer)
{
	int i, i_dst;

	int y_src = src_composer->src.y1 >> 16;
	int y_dst = src_composer->dst.y1;
	int h_dst = drm_rect_height(&src_composer->dst);
	int y_limit = y_src + h_dst;

	for (i = y_src, i_dst = y_dst; i < y_limit; ++i, i_dst++) {
		funcs->get_src_line(src_composer, i, line_buffer);
		funcs->set_output_line(src_composer, dst_composer,
				       i_dst, line_buffer);
	}
}

static void ((*get_line_fmt_transform_function(u32 format))
	    (struct vkms_composer *, int, u64 *))
{
	if (format == DRM_FORMAT_ARGB8888)
		return &ARGB8888_to_ARGB16161616;
	else if (format == DRM_FORMAT_ARGB16161616)
		return &get_ARGB16161616;
	else
		return &XRGB8888_to_ARGB16161616;
}

static void ((*get_output_line_function(u32 format))
	     (struct vkms_composer *, struct vkms_composer *, int, u64 *))
{
	if (format == DRM_FORMAT_ARGB8888)
		return &convert_to_ARGB8888;
	else if (format == DRM_FORMAT_ARGB16161616)
		return &convert_to_ARGB16161616;
	else
		return &convert_to_XRGB8888;
}

static void compose_plane(struct vkms_composer *src_composer,
			  struct vkms_composer *dst_composer,
			  struct vkms_pixel_composition_functions *funcs,
			  u64 *line_buffer)
{
	u32 src_format = src_composer->fb->format->format;

	funcs->get_src_line = get_line_fmt_transform_function(src_format);

	blend(src_composer, dst_composer, funcs, line_buffer);
}

static __le64 *compose_active_planes(struct vkms_composer *primary_composer,
				     struct vkms_crtc_state *crtc_state,
				     u64 *line_buffer)
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
	funcs.set_output_line = get_output_line_function(DRM_FORMAT_ARGB16161616);
	compose_plane(active_planes[0]->composer, &dst_composer,
		      &funcs, line_buffer);

	/* If there are other planes besides primary, we consider the active
	 * planes should be in z-order and compose them associatively:
	 * ((primary <- overlay) <- cursor)
	 */
	funcs.set_output_line = alpha_blend;
	for (i = 1; i < crtc_state->num_active_planes; i++)
		compose_plane(active_planes[i]->composer, &dst_composer,
			      &funcs, line_buffer);

	return vaddr_out;
}

static void write_wb_buffer(struct vkms_writeback_job *active_wb,
			    struct vkms_composer *primary_composer,
			    __le64 *vaddr_out, u64 *line_buffer)
{
	u32 dst_fb_format = active_wb->composer.fb->format->format;
	struct vkms_pixel_composition_functions funcs;
	struct vkms_composer src_composer;

	src_composer = get_output_vkms_composer(vaddr_out, primary_composer);
	funcs.set_output_line = get_output_line_function(dst_fb_format);
	active_wb->composer.src = primary_composer->src;
	active_wb->composer.dst = primary_composer->dst;

	compose_plane(&src_composer, &active_wb->composer, &funcs, line_buffer);
}

u64 *alloc_line_buffer(struct vkms_composer *primary_composer)
{
	int line_width = drm_rect_width(&primary_composer->dst);
	u64 *line_buffer;

	line_buffer = kvmalloc(line_width * sizeof(u64), GFP_KERNEL);
	if (!line_buffer)
		DRM_ERROR("Cannot allocate memory for intermediate line buffer");

	return line_buffer;
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
	u64 frame_start, frame_end, *line_buffer;
	bool crc_pending, wb_pending;
	__le64 *vaddr_out = NULL;
	u32 crc32 = 0;

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

	line_buffer = alloc_line_buffer(primary_composer);
	if (!line_buffer)
		return;

	vaddr_out = compose_active_planes(primary_composer, crtc_state,
					  line_buffer);
	if (!vaddr_out) {
		kvfree(line_buffer);
		return;
	}

	if (wb_pending) {
		write_wb_buffer(active_wb, primary_composer,
				vaddr_out, line_buffer);
		drm_writeback_signal_completion(&out->wb_connector, 0);
		spin_lock_irq(&out->composer_lock);
		crtc_state->wb_pending = false;
		spin_unlock_irq(&out->composer_lock);
	}

	kvfree(line_buffer);
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
