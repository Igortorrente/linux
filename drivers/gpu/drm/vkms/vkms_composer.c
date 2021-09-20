// SPDX-License-Identifier: GPL-2.0+

#include <linux/crc32.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_vblank.h>

#include "vkms_drv.h"
#include "vkms_formats.h"

static u16 pre_mul_blend_channel(u16 src, u16 dst, u16 alpha)
{
	u32 new_color;

	new_color = (src * 0xffff + dst * (0xffff - alpha));

	return DIV_ROUND_UP(new_color, 0xffff);
}

/**
 * pre_mul_alpha_blend - alpha blending equation
 * @src_frame_info: source framebuffer's metadata
 * @stage_buffer: The line with the pixels from src_plane
 * @output_buffer: A line buffer that receives all the blends output
 *
 * Using the information from the `frame_info`, this blends only the
 * necessary pixels from the `stage_buffer` to the `output_buffer`
 * using premultiplied blend formula.
 *
 * The current DRM assumption is that pixel color values have been already
 * pre-multiplied with the alpha channel values. See more
 * drm_plane_create_blend_mode_property(). Also, this formula assumes a
 * completely opaque background.
 */
static void pre_mul_alpha_blend(struct vkms_frame_info *frame_info,
				struct line_buffer *stage_buffer,
				struct line_buffer *output_buffer)
{
	int x, x_dst = frame_info->dst.x1;
	int x_limit = drm_rect_width(&frame_info->dst);
	struct line_buffer *out = output_buffer + x_dst;
	struct line_buffer *in = stage_buffer;

	for (x = 0; x < x_limit; x++) {
		out[x].a = (u16)0xffff;
		out[x].r = pre_mul_blend_channel(in[x].r, out[x].r, in[x].a);
		out[x].g = pre_mul_blend_channel(in[x].g, out[x].g, in[x].a);
		out[x].b = pre_mul_blend_channel(in[x].b, out[x].b, in[x].a);
	}
}

static bool check_y_limit(struct vkms_frame_info *frame_info, int y)
{
	if (y >= frame_info->dst.y1 && y < frame_info->dst.y2)
		return true;

	return false;
}

/**
 * @wb_frame_info: The writeback frame buffer metadata
 * @wb_fmt_func: The format tranformatio function to the wb buffer
 * @crtc_state: The crtc state
 * @plane_fmt_func: A format tranformation function to each plane
 * @crc32: The crc output of the final frame
 * @output_buffer: A buffer of a row that will receive the result of the blend(s)
 * @stage_buffer: The line with the pixels from src_compositor
 *
 * This function blends the pixels (Using the `pre_mul_alpha_blend`)
 * from all planes, calculates the crc32 of the output from the former step,
 * and, if necessary, convert and store the output to the writeback buffer.
 *
 * TODO: completely clear the primary plane (a = 0xff) before starting to blend
 * pixel color values
 */
static void blend(struct vkms_frame_info *wb_frame_info,
		  format_transform_func wb_fmt_func,
		  struct vkms_crtc_state *crtc_state,
		  format_transform_func *plane_fmt_func,
		  u32 *crc32, struct line_buffer *stage_buffer,
		  struct line_buffer *output_buffer, s64 row_size)
{
	struct vkms_plane_state **plane = crtc_state->active_planes;
	struct vkms_frame_info *primary_plane_info = plane[0]->frame_info;
	u32 n_active_planes = crtc_state->num_active_planes;

	int y_src = primary_plane_info->dst.y1;
	int h_dst = drm_rect_height(&primary_plane_info->dst);
	int y_limit = y_src + h_dst;
	int y, i;

	for (y = y_src; y < y_limit; y++) {
		plane_fmt_func[0](primary_plane_info, y, output_buffer);

		/* If there are other planes besides primary, we consider the active
		 * planes should be in z-order and compose them associatively:
		 * ((primary <- overlay) <- cursor)
		 */
		for (i = 1; i < n_active_planes; i++) {
			if (!check_y_limit(plane[i]->frame_info, y))
				continue;

			plane_fmt_func[i](plane[i]->frame_info, y, stage_buffer);
			pre_mul_alpha_blend(plane[i]->frame_info, stage_buffer,
					    output_buffer);
		}

		*crc32 = crc32_le(*crc32, (void *)output_buffer, row_size);

		if (wb_frame_info)
			wb_fmt_func(wb_frame_info, y, output_buffer);
	}
}

static void get_format_transform_functions(struct vkms_crtc_state *crtc_state,
					   format_transform_func plane_funcs[])
{
	struct vkms_plane_state **active_planes = crtc_state->active_planes;
	u32 n_active_planes = crtc_state->num_active_planes, s_fmt;
	int i;

	for (i = 0; i < n_active_planes; i++) {
		s_fmt = active_planes[i]->frame_info->fb->format->format;
		plane_funcs[i] = get_fmt_transform_function(s_fmt);
	}
}

static bool check_planes_x_bounds(struct vkms_crtc_state *crtc_state,
				  struct vkms_frame_info *wb_frame_info)
{
	struct vkms_plane_state **planes = crtc_state->active_planes;
	struct vkms_frame_info *primary_plane_info = planes[0]->frame_info;
	int line_width = drm_rect_width(&primary_plane_info->dst);
	u32 n_active_planes = crtc_state->num_active_planes;
	int i;

	for (i = 0; i < n_active_planes; i++) {
		int x_dst = planes[i]->frame_info->dst.x1;
		int x_src = planes[i]->frame_info->src.x1 >> 16;
		int x2_src = planes[i]->frame_info->src.x2 >> 16;
		int x_limit = drm_rect_width(&planes[i]->frame_info->dst);

		if (x_dst + x_limit > line_width)
			return false;
		if (x_src + x_limit > x2_src)
			return false;
	}

	return true;
}

static int compose_active_planes(struct vkms_frame_info *wb_frame_info,
				 struct vkms_crtc_state *crtc_state,
				 u32 *crc32)
{
	format_transform_func plane_funcs[NUM_OVERLAY_PLANES], wb_func = NULL;
	int line_width, ret = 0, pixel_size = sizeof(struct line_buffer);
	struct vkms_frame_info *primary_plane_info = NULL;
	struct line_buffer *output_buffer, *stage_buffer;
	struct vkms_plane_state *act_plane = NULL;
	u32 wb_format;

	if (WARN_ON(pixel_size != 8))
		return -EINVAL;

	if (crtc_state->num_active_planes >= 1) {
		act_plane = crtc_state->active_planes[0];
		if (act_plane->base.base.plane->type == DRM_PLANE_TYPE_PRIMARY)
			primary_plane_info = act_plane->frame_info;
	}

	if (!primary_plane_info)
		return -EINVAL;

	if (WARN_ON(dma_buf_map_is_null(&primary_plane_info->map[0])))
		return -EINVAL;

	if (WARN_ON(!check_planes_x_bounds(crtc_state, wb_frame_info)))
		return -EINVAL;

	line_width = drm_rect_width(&primary_plane_info->dst);

	stage_buffer = kvmalloc(line_width * pixel_size, GFP_KERNEL);
	if (!stage_buffer) {
		DRM_ERROR("Cannot allocate memory for the output line buffer");
		return -ENOMEM;
	}

	output_buffer = kvmalloc(line_width * pixel_size, GFP_KERNEL);
	if (!output_buffer) {
		DRM_ERROR("Cannot allocate memory for intermediate line buffer");
		ret = -ENOMEM;
		goto free_stage_buffer;
	}

	get_format_transform_functions(crtc_state, plane_funcs);

	if (wb_frame_info) {
		wb_format = wb_frame_info->fb->format->format;
		wb_func = get_wb_fmt_transform_function(wb_format);
		wb_frame_info->src = primary_plane_info->src;
		wb_frame_info->dst = primary_plane_info->dst;
	}

	blend(wb_frame_info, wb_func, crtc_state, plane_funcs, crc32,
	      stage_buffer, output_buffer, (s64)line_width * pixel_size);

	kvfree(output_buffer);
free_stage_buffer:
	kvfree(stage_buffer);

	return ret;
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
	struct vkms_frame_info *wb_frame_info = &active_wb->frame_info;
	struct vkms_output *out = drm_crtc_to_vkms_output(crtc);
	bool crc_pending, wb_pending;
	u64 frame_start, frame_end;
	u32 crc32 = 0;
	int ret;

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

	if (wb_pending)
		ret = compose_active_planes(wb_frame_info, crtc_state, &crc32);
	else
		ret = compose_active_planes(NULL, crtc_state, &crc32);

	if (ret)
		return;

	if (wb_pending) {
		drm_writeback_signal_completion(&out->wb_connector, 0);
		spin_lock_irq(&out->composer_lock);
		crtc_state->wb_pending = false;
		spin_unlock_irq(&out->composer_lock);
	}

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
