/*
 * Copyright © 2011 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *   Jesse Barnes <jbarnes@virtuousgeek.org>
 *
 * New plane/sprite handling.
 *
 * The older chips had a separate interface for programming plane related
 * registers; newer ones are much simpler and we can use the new DRM plane
 * support.
 */
#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_rect.h>
#include "intel_drv.h"
#include <drm/i915_drm.h>
#include "i915_drv.h"

void intel_pipe_handle_vblank(struct drm_device *dev, enum pipe pipe)
{
	struct intel_crtc *intel_crtc = to_intel_crtc(intel_get_crtc_for_pipe(dev, pipe));

	intel_crtc->vbl_received = true;
	wake_up(&intel_crtc->vbl_wait);
}

static int usecs_to_scanlines(struct drm_crtc *crtc, int usecs)
{
	/* paranoia */
	if (!crtc->linedur_ns)
		return 1;

	return DIV_ROUND_UP_ULL(1000ULL * usecs, crtc->linedur_ns);
}

static void intel_pipe_vblank_evade(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	const struct drm_display_mode *adjusted_mode = &intel_crtc->config.adjusted_mode;
	enum pipe pipe = intel_crtc->pipe;
	/* FIXME needs to be calibrated sensibly */
	int min = adjusted_mode->crtc_vdisplay - usecs_to_scanlines(crtc, 100);
	int max = adjusted_mode->crtc_vdisplay - 1;
	long timeout = msecs_to_jiffies(3);
	bool vblank_ref = drm_vblank_get(dev, pipe) == 0;
	int vpos;

	intel_crtc->vbl_received = false;
	vpos = i915_get_crtc_vpos(crtc);

	while (vpos >= min && vpos <= max && timeout > 0) {
		local_irq_enable();
		timeout = wait_event_timeout(intel_crtc->vbl_wait,
					     intel_crtc->vbl_received,
					     timeout);
		local_irq_disable();

		intel_crtc->vbl_received = false;
		vpos = i915_get_crtc_vpos(crtc);
	}

	if (vblank_ref)
		drm_vblank_put(dev, pipe);

	trace_i915_sprite_start(crtc, min, max);
}

void
__alpha_setting_noncursor(u32 pixformat, int plane, u32 *dspcntr, int alpha)
{
	/* For readability, can split to individual cases */
	/* 5 no alphas, 6-9 common, a-d reserved for sprite, e-f common */
	switch (pixformat) {
	case DISPPLANE_RGBX888:
	case DISPPLANE_RGBA888:
		if (alpha)
			*dspcntr |= DISPPLANE_RGBA888;
		else
			*dspcntr |= DISPPLANE_RGBX888;
		break;
	case DISPPLANE_BGRX888:
	case DISPPLANE_BGRA888:
		if (alpha)
			*dspcntr |= DISPPLANE_BGRA888;
		else
			*dspcntr |= DISPPLANE_BGRX888;
		break;
	case DISPPLANE_RGBX101010:
	case DISPPLANE_RGBA101010:
		if (alpha)
			*dspcntr |= DISPPLANE_RGBA101010;
		else
			*dspcntr |= DISPPLANE_RGBX101010;
		break;
	case DISPPLANE_BGRX101010:
	case DISPPLANE_BGRA101010:
		if (alpha)
			*dspcntr |= DISPPLANE_BGRA101010;
		else
			*dspcntr |= DISPPLANE_BGRX101010;
		break;
	case DISPPLANE_RGBX161616:
	case DISPPLANE_RGBA161616:
		if ((plane == PLANEA) || (plane == PLANEB)) {
			if (alpha)
				*dspcntr |= DISPPLANE_RGBA161616;
			else
				*dspcntr |= DISPPLANE_RGBX161616;
		}
		break;
	default:
		DRM_ERROR("Unknown pixel format 0x%08x\n", pixformat);
		break;
	}
}

void
__alpha_setting_cursor(u32 pixformat, int plane, u32 *dspcntr, int alpha)
{
	/* For readability, can split to individual cases */
	switch (pixformat) {
	case CURSOR_MODE_128_32B_AX:
	case CURSOR_MODE_128_ARGB_AX:
		if (alpha)
			*dspcntr |= CURSOR_MODE_128_ARGB_AX;
		else
			*dspcntr |= CURSOR_MODE_128_32B_AX;
		break;

	case CURSOR_MODE_256_ARGB_AX:
	case CURSOR_MODE_256_32B_AX:
		if (alpha)
			*dspcntr |= CURSOR_MODE_256_ARGB_AX;
		else
			*dspcntr |= CURSOR_MODE_256_32B_AX;
		break;

	case CURSOR_MODE_64_ARGB_AX:
	case CURSOR_MODE_64_32B_AX:
		if (alpha)
			*dspcntr |= CURSOR_MODE_64_ARGB_AX;
		else
			*dspcntr |= CURSOR_MODE_64_32B_AX;
		break;
	default:
		DRM_ERROR("Unknown pixel format:Cursor 0x%08x\n", pixformat);
		break;
	}
}
/*
 * enable/disable alpha for planes
 */
int
i915_set_plane_alpha(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_i915_set_plane_alpha *alphadata = data;
	int plane = alphadata->plane;
	bool alpha = alphadata->alpha;
	bool IsCursor = false;
	u32 dspcntr;
	u32 reg;
	u32 pixformat;
	u32 mask = DISPPLANE_PIXFORMAT_MASK;

	DRM_DEBUG_DRIVER("In i915_set_plane_alpha\n");

	switch (plane) {
	case PLANEA:
		reg = DSPCNTR(0);
		break;
	case PLANEB:
		reg = DSPCNTR(1);
		break;
	case SPRITEA:
		reg = SPCNTR(0, 0);
		break;
	case SPRITEB:
		reg = SPCNTR(0, 1);
		break;
	case SPRITEC:
		reg = SPCNTR(1, 0);
		break;
	case SPRITED:
		reg = SPCNTR(1, 1);
		break;
	case CURSORA:
		reg = CURCNTR(0);
		mask = CURSOR_MODE;
		IsCursor = true;
		break;
	case CURSORB:
		reg = CURCNTR(1);
		mask = CURSOR_MODE;
		IsCursor = true;
		break;
	default:
		DRM_ERROR("No plane selected properly\n");
		return -EINVAL;
	}

	dspcntr = I915_READ(reg);
	DRM_DEBUG_DRIVER("dspcntr = %x\n", dspcntr);

	pixformat = dspcntr & mask;
	dspcntr &= ~mask;
	DRM_DEBUG_DRIVER("pixformat = %x, alpha = %x\n", pixformat, alpha);

	if (pixformat) {
		if (!IsCursor)
			__alpha_setting_noncursor(pixformat, plane,
						&dspcntr, alpha);
		else
			__alpha_setting_cursor(pixformat, plane,
						&dspcntr, alpha);

		DRM_DEBUG_DRIVER("Reg should be written with = %x\n", dspcntr);

		if (pixformat != (dspcntr & mask)) {
			I915_WRITE(reg, dspcntr);
			DRM_DEBUG_DRIVER("Reg written with = %x\n", dspcntr);
		}
	} else
		DRM_DEBUG_DRIVER("Plane might not be enabled/configured!\n");

	return 0;
}

/*
 * enable/disable primary plane alpha channel based on the z-order
 */
void
i915_set_primary_alpha(struct drm_i915_private *dev_priv, int zorder, int plane)
{
	u32 dspcntr;
	u32 reg;
	u32 pixformat;
	bool alpha = false;

	if (zorder != P1S1S2C1 && zorder != P1S2S1C1)
		alpha = true;
	else
		alpha = false;

	reg = DSPCNTR(plane);
	dspcntr = I915_READ(reg);

	if (!(dspcntr & DISPLAY_PLANE_ENABLE))
		return;

	pixformat = dspcntr & DISPPLANE_PIXFORMAT_MASK;
	dspcntr &= ~DISPPLANE_PIXFORMAT_MASK;

	DRM_DEBUG_DRIVER("pixformat = %x, alpha = %d", pixformat, alpha);

	switch (pixformat) {
	case DISPPLANE_BGRX888:
	case DISPPLANE_BGRA888:
		if (alpha)
			dspcntr |= DISPPLANE_BGRA888;
		else
			dspcntr |= DISPPLANE_BGRX888;
		break;
	case DISPPLANE_RGBX888:
	case DISPPLANE_RGBA888:
		if (alpha)
			dspcntr |= DISPPLANE_RGBA888;
		else
			dspcntr |= DISPPLANE_RGBX888;
		break;
	case DISPPLANE_BGRX101010:
	case DISPPLANE_BGRA101010:
		if (alpha)
			dspcntr |= DISPPLANE_BGRA101010;
		else
			dspcntr |= DISPPLANE_BGRX101010;
		break;
	case DISPPLANE_RGBX101010:
	case DISPPLANE_RGBA101010:
		if (alpha)
			dspcntr |= DISPPLANE_RGBA101010;
		else
			dspcntr |= DISPPLANE_RGBX101010;
	case DISPPLANE_BGRX565:
		dspcntr |= DISPPLANE_BGRX565;
		break;
	case DISPPLANE_8BPP:
		dspcntr |= DISPPLANE_8BPP;
		break;
	default:
		DRM_ERROR("Unknown pixel format 0x%08x\n", pixformat);
		break;
	}

	if (pixformat != (dspcntr & DISPPLANE_PIXFORMAT_MASK)) {
		I915_WRITE(reg, dspcntr);
		DRM_DEBUG_DRIVER("dspcntr = %x", dspcntr);
	}
}

/*
 * enable/disable sprite alpha channel based on the z-order
 */
void i915_set_sprite_alpha(struct drm_i915_private *dev_priv, int zorder,
				int pipe, int plane)
{
	u32 spcntr;
	u32 pixformat;
	bool alpha = false;

	if (zorder != S1P1S2C1 && zorder != S1S2P1C1 && plane == 0)
		alpha = true;
	else if (zorder != S2P1S1C1 && zorder != S2S1P1C1 && plane == 1)
		alpha = true;
	else
		alpha = false;

	spcntr = I915_READ(SPCNTR(pipe, plane));
	if (!(spcntr & DISPLAY_PLANE_ENABLE))
		return;
	pixformat = spcntr & SP_PIXFORMAT_MASK;
	spcntr &= ~SP_PIXFORMAT_MASK;

	DRM_DEBUG_DRIVER("sprite pixformat = %x plane = %d", pixformat, plane);

	switch (pixformat) {
	case SP_FORMAT_BGRA8888:
	case SP_FORMAT_BGRX8888:
		if (alpha)
			spcntr |= SP_FORMAT_BGRA8888;
		else
			spcntr |= SP_FORMAT_BGRX8888;
		break;
	case SP_FORMAT_RGBA8888:
	case SP_FORMAT_RGBX8888:
		if (alpha)
			spcntr |= SP_FORMAT_RGBA8888;
		else
			spcntr |= SP_FORMAT_RGBX8888;
		break;
	case SP_FORMAT_RGBA1010102:
	case SP_FORMAT_RGBX1010102:
		if (alpha)
			spcntr |= SP_FORMAT_RGBA1010102;
		else
			spcntr |= SP_FORMAT_RGBX1010102;
		break;
	case SP_FORMAT_YUV422:
		spcntr |= SP_FORMAT_YUV422;
		break;
	case SP_FORMAT_BGR565:
		spcntr |= SP_FORMAT_BGR565;
		break;
	default:
		DRM_ERROR("Unknown pixel format 0x%08x\n", pixformat);
		break;
	}

	if (pixformat != (spcntr & SP_PIXFORMAT_MASK)) {
		I915_WRITE(SPCNTR(pipe, plane), spcntr);
		DRM_DEBUG_DRIVER("spcntr = %x ", spcntr);
	}
}

int i915_set_plane_zorder(struct drm_device *dev, void *data,
			  struct drm_file *file)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	u32 val;
	struct drm_i915_set_plane_zorder *zorder = data;
	u32 order = zorder->order;
	int s1_zorder, s1_bottom, s2_zorder, s2_bottom;
	int pipe = (order >> 31) & 0x1;
	int z_order = order & 0x000F;

	s1_zorder = (order >> 3) & 0x1;
	s1_bottom = (order >> 2) & 0x1;
	s2_zorder = (order >> 1) & 0x1;
	s2_bottom = (order >> 0) & 0x1;

	/* Clear the older Z-order */
	val = I915_READ(SPCNTR(pipe, 0));
	val &= ~(SPRITE_FORCE_BOTTOM | SPRITE_ZORDER_ENABLE);
	I915_WRITE(SPCNTR(pipe, 0), val);

	val = I915_READ(SPCNTR(pipe, 1));
	val &= ~(SPRITE_FORCE_BOTTOM | SPRITE_ZORDER_ENABLE);
	I915_WRITE(SPCNTR(pipe, 1), val);

	/* Program new Z-order */
	val = I915_READ(SPCNTR(pipe, 0));
	if (s1_zorder)
		val |= SPRITE_ZORDER_ENABLE;
	if (s1_bottom)
		val |= SPRITE_FORCE_BOTTOM;
	I915_WRITE(SPCNTR(pipe, 0), val);

	val = I915_READ(SPCNTR(pipe, 1));
	if (s2_zorder)
		val |= SPRITE_ZORDER_ENABLE;
	if (s2_bottom)
		val |= SPRITE_FORCE_BOTTOM;
	I915_WRITE(SPCNTR(pipe, 1), val);

	i915_set_primary_alpha(dev_priv, z_order, pipe);

	i915_set_sprite_alpha(dev_priv, z_order, pipe, 0);
	i915_set_sprite_alpha(dev_priv, z_order, pipe, 1);

	return 0;
}

static void
intel_update_primary(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	int reg = DSPCNTR(intel_crtc->plane);
	uint32_t tmp = I915_READ(reg);

	if (intel_crtc->primary_enabled == (tmp & DISPLAY_PLANE_ENABLE))
		goto out;

	if (!(intel_crtc->primary_enabled)) {
		if (dev_priv->fbc.plane == intel_crtc->plane)
			intel_disable_fbc(dev);

		if (IS_HASWELL(dev))
			hsw_disable_ips(intel_crtc, IPS_NO_WAIT_FOR_VBLANK);

		tmp &= ~DISPLAY_PLANE_ENABLE;
		I915_WRITE(reg, tmp);
		intel_flush_primary_plane(dev_priv, intel_crtc->plane);
	} else {
		tmp |= DISPLAY_PLANE_ENABLE;
		I915_WRITE(reg, tmp);
		intel_flush_primary_plane(dev_priv, intel_crtc->plane);

		if (IS_HASWELL(dev))
			hsw_enable_ips(intel_crtc, IPS_NO_WAIT_FOR_VBLANK);

		mutex_lock(&dev->struct_mutex);
		intel_update_fbc(dev);
		mutex_unlock(&dev->struct_mutex);
	}

out:
	trace_i915_sprite_end(crtc);
}

static void
vlv_update_plane(struct drm_plane *dplane, struct drm_crtc *crtc,
		 struct drm_framebuffer *fb,
		 struct drm_i915_gem_object *obj, int crtc_x, int crtc_y,
		 unsigned int crtc_w, unsigned int crtc_h,
		 uint32_t x, uint32_t y,
		 uint32_t src_w, uint32_t src_h,
		 bool disable_primary)
{
	struct drm_device *dev = dplane->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_plane *intel_plane = to_intel_plane(dplane);
	int pipe = intel_plane->pipe;
	int plane = intel_plane->plane;
	u32 sprctl;
	bool rotate = false;
	unsigned long sprsurf_offset, linear_offset;
	int pixel_size = drm_format_plane_cpp(fb->pixel_format, 0);
	struct intel_pipe_wm pipe_wm = {};

	sprctl = I915_READ(SPCNTR(pipe, plane));
	/* Mask out pixel format bits in case we change it */
	sprctl &= ~SP_PIXFORMAT_MASK;
	sprctl &= ~SP_YUV_BYTE_ORDER_MASK;
	sprctl &= ~SP_TILED;

	switch (fb->pixel_format) {
	case DRM_FORMAT_YUYV:
		sprctl |= SP_FORMAT_YUV422 | SP_YUV_ORDER_YUYV;
		break;
	case DRM_FORMAT_YVYU:
		sprctl |= SP_FORMAT_YUV422 | SP_YUV_ORDER_YVYU;
		break;
	case DRM_FORMAT_UYVY:
		sprctl |= SP_FORMAT_YUV422 | SP_YUV_ORDER_UYVY;
		break;
	case DRM_FORMAT_VYUY:
		sprctl |= SP_FORMAT_YUV422 | SP_YUV_ORDER_VYUY;
		break;
	case DRM_FORMAT_RGB565:
		sprctl |= SP_FORMAT_BGR565;
		break;
	case DRM_FORMAT_XRGB8888:
		sprctl |= SP_FORMAT_BGRX8888;
		break;
	case DRM_FORMAT_ARGB8888:
		sprctl |= SP_FORMAT_BGRA8888;
		break;
	case DRM_FORMAT_XBGR2101010:
		sprctl |= SP_FORMAT_RGBX1010102;
		break;
	case DRM_FORMAT_ABGR2101010:
		sprctl |= SP_FORMAT_RGBA1010102;
		break;
	case DRM_FORMAT_XBGR8888:
		sprctl |= SP_FORMAT_RGBX8888;
		break;
	case DRM_FORMAT_ABGR8888:
		sprctl |= SP_FORMAT_RGBA8888;
		break;
	default:
		/*
		 * If we get here one of the upper layers failed to filter
		 * out the unsupported plane formats
		 */
		BUG();
		break;
	}

	/*
	 * Enable gamma to match primary/cursor plane behaviour.
	 * FIXME should be user controllable via propertiesa.
	 */
	sprctl |= SP_GAMMA_ENABLE;

	if (obj->tiling_mode != I915_TILING_NONE)
		sprctl |= SP_TILED;

	sprctl |= SP_ENABLE;

	to_intel_crtc(crtc)->primary_enabled = !disable_primary;
	intel_update_sprite_watermarks(dplane, crtc, src_w, pixel_size, true,
				       src_w != crtc_w || src_h != crtc_h,
				       &pipe_wm);

	if (sprctl & DISPPLANE_180_ROTATION_ENABLE)
		rotate = true;

	/* Sizes are 0 based */
	src_w--;
	src_h--;
	crtc_w--;
	crtc_h--;

	linear_offset = y * fb->pitches[0] + x * pixel_size;
	sprsurf_offset = intel_gen4_compute_page_offset(&x, &y,
							obj->tiling_mode,
							pixel_size,
							fb->pitches[0]);
	linear_offset -= sprsurf_offset;

	local_irq_disable();
	intel_pipe_vblank_evade(crtc);

	intel_program_watermarks(crtc, &pipe_wm);

	intel_update_primary(crtc);

	I915_WRITE(SPSTRIDE(pipe, plane), fb->pitches[0]);
	if (rotate)
		I915_WRITE(SPPOS(pipe, plane), ((rot_mode.vdisplay -
			(crtc_y + crtc_h + 1)) << 16) |
				(rot_mode.hdisplay - (crtc_x + crtc_w + 1)));
	else
		I915_WRITE(SPPOS(pipe, plane), (crtc_y << 16) | crtc_x);

	if (obj->tiling_mode != I915_TILING_NONE) {
		if (rotate) {
			I915_WRITE(SPTILEOFF(pipe, plane),
				(((crtc_h + 1) << 16) | (crtc_w + 1)));
		} else
			I915_WRITE(SPTILEOFF(pipe, plane), (y << 16) | x);
	} else {
		if (rotate) {
			I915_WRITE(SPLINOFF(pipe, plane),
				(((crtc_h + 1) * (crtc_w + 1) *
				pixel_size)) - pixel_size);
		} else
			I915_WRITE(SPLINOFF(pipe, plane), linear_offset);
	}

	I915_WRITE(SPSIZE(pipe, plane), (crtc_h << 16) | crtc_w);
	if (rotate)
		sprctl |= DISPPLANE_180_ROTATION_ENABLE;

	I915_WRITE(SPCNTR(pipe, plane), sprctl);
	I915_MODIFY_DISPBASE(SPSURF(pipe, plane), i915_gem_obj_ggtt_offset(obj) +
			     sprsurf_offset);
	POSTING_READ(SPSURF(pipe, plane));

	local_irq_enable();
}

static void
vlv_disable_plane(struct drm_plane *dplane, struct drm_crtc *crtc)
{
	struct drm_device *dev = dplane->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_plane *intel_plane = to_intel_plane(dplane);
	int pipe = intel_plane->pipe;
	int plane = intel_plane->plane;
	struct intel_pipe_wm pipe_wm = {};

	to_intel_crtc(crtc)->primary_enabled = true;
	intel_update_sprite_watermarks(dplane, crtc, 0, 0, false, false, &pipe_wm);

	local_irq_disable();
	intel_pipe_vblank_evade(crtc);

	intel_program_watermarks(crtc, &pipe_wm);

	intel_update_primary(crtc);

	I915_WRITE(SPCNTR(pipe, plane), I915_READ(SPCNTR(pipe, plane)) &
		   ~SP_ENABLE);
	/* Activate double buffered register update */
	I915_MODIFY_DISPBASE(SPSURF(pipe, plane), 0);
	POSTING_READ(SPSURF(pipe, plane));

	local_irq_enable();
}

static int
vlv_update_colorkey(struct drm_plane *dplane,
		    struct drm_intel_sprite_colorkey *key)
{
	struct drm_device *dev = dplane->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_plane *intel_plane = to_intel_plane(dplane);
	int pipe = intel_plane->pipe;
	int plane = intel_plane->plane;
	u32 sprctl;

	if (key->flags & I915_SET_COLORKEY_DESTINATION)
		return -EINVAL;

	I915_WRITE(SPKEYMINVAL(pipe, plane), key->min_value);
	I915_WRITE(SPKEYMAXVAL(pipe, plane), key->max_value);
	I915_WRITE(SPKEYMSK(pipe, plane), key->channel_mask);

	sprctl = I915_READ(SPCNTR(pipe, plane));
	sprctl &= ~SP_SOURCE_KEY;
	if (key->flags & I915_SET_COLORKEY_SOURCE)
		sprctl |= SP_SOURCE_KEY;
	I915_WRITE(SPCNTR(pipe, plane), sprctl);

	POSTING_READ(SPKEYMSK(pipe, plane));

	return 0;
}

static void
vlv_get_colorkey(struct drm_plane *dplane,
		 struct drm_intel_sprite_colorkey *key)
{
	struct drm_device *dev = dplane->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_plane *intel_plane = to_intel_plane(dplane);
	int pipe = intel_plane->pipe;
	int plane = intel_plane->plane;
	u32 sprctl;

	key->min_value = I915_READ(SPKEYMINVAL(pipe, plane));
	key->max_value = I915_READ(SPKEYMAXVAL(pipe, plane));
	key->channel_mask = I915_READ(SPKEYMSK(pipe, plane));

	sprctl = I915_READ(SPCNTR(pipe, plane));
	if (sprctl & SP_SOURCE_KEY)
		key->flags = I915_SET_COLORKEY_SOURCE;
	else
		key->flags = I915_SET_COLORKEY_NONE;
}

static void
ivb_update_plane(struct drm_plane *plane, struct drm_crtc *crtc,
		 struct drm_framebuffer *fb,
		 struct drm_i915_gem_object *obj, int crtc_x, int crtc_y,
		 unsigned int crtc_w, unsigned int crtc_h,
		 uint32_t x, uint32_t y,
		 uint32_t src_w, uint32_t src_h,
		 bool disable_primary)
{
	struct drm_device *dev = plane->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_plane *intel_plane = to_intel_plane(plane);
	int pipe = intel_plane->pipe;
	u32 sprctl, sprscale = 0;
	unsigned long sprsurf_offset, linear_offset;
	int pixel_size = drm_format_plane_cpp(fb->pixel_format, 0);
	struct intel_pipe_wm pipe_wm = {};

	sprctl = I915_READ(SPRCTL(pipe));

	/* Mask out pixel format bits in case we change it */
	sprctl &= ~SPRITE_PIXFORMAT_MASK;
	sprctl &= ~SPRITE_RGB_ORDER_RGBX;
	sprctl &= ~SPRITE_YUV_BYTE_ORDER_MASK;
	sprctl &= ~SPRITE_TILED;

	switch (fb->pixel_format) {
	case DRM_FORMAT_XBGR8888:
		sprctl |= SPRITE_FORMAT_RGBX888 | SPRITE_RGB_ORDER_RGBX;
		break;
	case DRM_FORMAT_XRGB8888:
		sprctl |= SPRITE_FORMAT_RGBX888;
		break;
	case DRM_FORMAT_YUYV:
		sprctl |= SPRITE_FORMAT_YUV422 | SPRITE_YUV_ORDER_YUYV;
		break;
	case DRM_FORMAT_YVYU:
		sprctl |= SPRITE_FORMAT_YUV422 | SPRITE_YUV_ORDER_YVYU;
		break;
	case DRM_FORMAT_UYVY:
		sprctl |= SPRITE_FORMAT_YUV422 | SPRITE_YUV_ORDER_UYVY;
		break;
	case DRM_FORMAT_VYUY:
		sprctl |= SPRITE_FORMAT_YUV422 | SPRITE_YUV_ORDER_VYUY;
		break;
	default:
		BUG();
	}

	/*
	 * Enable gamma to match primary/cursor plane behaviour.
	 * FIXME should be user controllable via propertiesa.
	 */
	sprctl |= SPRITE_GAMMA_ENABLE;

	if (obj->tiling_mode != I915_TILING_NONE)
		sprctl |= SPRITE_TILED;

	if (IS_HASWELL(dev) || IS_BROADWELL(dev))
		sprctl &= ~SPRITE_TRICKLE_FEED_DISABLE;
	else
		sprctl |= SPRITE_TRICKLE_FEED_DISABLE;

	sprctl |= SPRITE_ENABLE;

	if (IS_HASWELL(dev) || IS_BROADWELL(dev))
		sprctl |= SPRITE_PIPE_CSC_ENABLE;

	to_intel_crtc(crtc)->primary_enabled = !disable_primary;
	intel_update_sprite_watermarks(plane, crtc, src_w, pixel_size, true,
				       src_w != crtc_w || src_h != crtc_h,
				       &pipe_wm);

	/* Sizes are 0 based */
	src_w--;
	src_h--;
	crtc_w--;
	crtc_h--;

	linear_offset = y * fb->pitches[0] + x * pixel_size;
	sprsurf_offset =
		intel_gen4_compute_page_offset(&x, &y, obj->tiling_mode,
					       pixel_size, fb->pitches[0]);
	linear_offset -= sprsurf_offset;

	/*
	 * IVB workaround: must disable low power watermarks for at least
	 * one frame before enabling scaling.  LP watermarks can be re-enabled
	 * when scaling is disabled.
	 */
	if (crtc_w != src_w || crtc_h != src_h) {
		sprscale = SPRITE_SCALE_ENABLE | (src_w << 16) | src_h;

		/* WaCxSRDisabledForSpriteScaling:ivb */
		if (i915_ivb_sprite_fix && ilk_disable_lp_wm(dev))
			intel_wait_for_vblank(dev, pipe);
	}

	local_irq_disable();
	intel_pipe_vblank_evade(crtc);

	intel_program_watermarks(crtc, &pipe_wm);

	intel_update_primary(crtc);

	I915_WRITE(SPRSTRIDE(pipe), fb->pitches[0]);
	I915_WRITE(SPRPOS(pipe), (crtc_y << 16) | crtc_x);

	/* HSW consolidates SPRTILEOFF and SPRLINOFF into a single SPROFFSET
	 * register */
	if (IS_HASWELL(dev) || IS_BROADWELL(dev))
		I915_WRITE(SPROFFSET(pipe), (y << 16) | x);
	else if (obj->tiling_mode != I915_TILING_NONE)
		I915_WRITE(SPRTILEOFF(pipe), (y << 16) | x);
	else
		I915_WRITE(SPRLINOFF(pipe), linear_offset);

	I915_WRITE(SPRSIZE(pipe), (crtc_h << 16) | crtc_w);
	if (intel_plane->can_scale)
		I915_WRITE(SPRSCALE(pipe), sprscale);
	I915_WRITE(SPRCTL(pipe), sprctl);
	I915_MODIFY_DISPBASE(SPRSURF(pipe),
			     i915_gem_obj_ggtt_offset(obj) + sprsurf_offset);
	POSTING_READ(SPRSURF(pipe));

	local_irq_enable();
}

static void
ivb_disable_plane(struct drm_plane *plane, struct drm_crtc *crtc)
{
	struct drm_device *dev = plane->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_plane *intel_plane = to_intel_plane(plane);
	int pipe = intel_plane->pipe;
	struct intel_pipe_wm pipe_wm = {};

	to_intel_crtc(crtc)->primary_enabled = true;
	intel_update_sprite_watermarks(plane, crtc, 0, 0, false, false, &pipe_wm);

	local_irq_disable();
	intel_pipe_vblank_evade(crtc);

	intel_program_watermarks(crtc, &pipe_wm);

	intel_update_primary(crtc);

	I915_WRITE(SPRCTL(pipe), I915_READ(SPRCTL(pipe)) & ~SPRITE_ENABLE);
	/* Can't leave the scaler enabled... */
	if (intel_plane->can_scale)
		I915_WRITE(SPRSCALE(pipe), 0);

	/* Scheduling the sprite disable to corresponding flip */
	to_intel_crtc(crtc)->disable_sprite = true;

	local_irq_enable();
}

static int
ivb_update_colorkey(struct drm_plane *plane,
		    struct drm_intel_sprite_colorkey *key)
{
	struct drm_device *dev = plane->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_plane *intel_plane;
	u32 sprctl;
	int ret = 0;

	intel_plane = to_intel_plane(plane);

	I915_WRITE(SPRKEYVAL(intel_plane->pipe), key->min_value);
	I915_WRITE(SPRKEYMAX(intel_plane->pipe), key->max_value);
	I915_WRITE(SPRKEYMSK(intel_plane->pipe), key->channel_mask);

	sprctl = I915_READ(SPRCTL(intel_plane->pipe));
	sprctl &= ~(SPRITE_SOURCE_KEY | SPRITE_DEST_KEY);
	if (key->flags & I915_SET_COLORKEY_DESTINATION)
		sprctl |= SPRITE_DEST_KEY;
	else if (key->flags & I915_SET_COLORKEY_SOURCE)
		sprctl |= SPRITE_SOURCE_KEY;
	I915_WRITE(SPRCTL(intel_plane->pipe), sprctl);

	POSTING_READ(SPRKEYMSK(intel_plane->pipe));

	return ret;
}

static void
ivb_get_colorkey(struct drm_plane *plane, struct drm_intel_sprite_colorkey *key)
{
	struct drm_device *dev = plane->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_plane *intel_plane;
	u32 sprctl;

	intel_plane = to_intel_plane(plane);

	key->min_value = I915_READ(SPRKEYVAL(intel_plane->pipe));
	key->max_value = I915_READ(SPRKEYMAX(intel_plane->pipe));
	key->channel_mask = I915_READ(SPRKEYMSK(intel_plane->pipe));
	key->flags = 0;

	sprctl = I915_READ(SPRCTL(intel_plane->pipe));

	if (sprctl & SPRITE_DEST_KEY)
		key->flags = I915_SET_COLORKEY_DESTINATION;
	else if (sprctl & SPRITE_SOURCE_KEY)
		key->flags = I915_SET_COLORKEY_SOURCE;
	else
		key->flags = I915_SET_COLORKEY_NONE;
}

static u32
ivb_current_surface(struct drm_plane *plane)
{
	struct intel_plane *intel_plane;

	intel_plane = to_intel_plane(plane);

	return SPRSURFLIVE(intel_plane->pipe);
}

static void
ilk_update_plane(struct drm_plane *plane, struct drm_crtc *crtc,
		 struct drm_framebuffer *fb,
		 struct drm_i915_gem_object *obj, int crtc_x, int crtc_y,
		 unsigned int crtc_w, unsigned int crtc_h,
		 uint32_t x, uint32_t y,
		 uint32_t src_w, uint32_t src_h,
		 bool disable_primary)
{
	struct drm_device *dev = plane->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_plane *intel_plane = to_intel_plane(plane);
	int pipe = intel_plane->pipe;
	unsigned long dvssurf_offset, linear_offset;
	u32 dvscntr, dvsscale;
	int pixel_size = drm_format_plane_cpp(fb->pixel_format, 0);
	struct intel_pipe_wm pipe_wm = {};

	dvscntr = I915_READ(DVSCNTR(pipe));

	/* Mask out pixel format bits in case we change it */
	dvscntr &= ~DVS_PIXFORMAT_MASK;
	dvscntr &= ~DVS_RGB_ORDER_XBGR;
	dvscntr &= ~DVS_YUV_BYTE_ORDER_MASK;
	dvscntr &= ~DVS_TILED;

	switch (fb->pixel_format) {
	case DRM_FORMAT_XBGR8888:
		dvscntr |= DVS_FORMAT_RGBX888 | DVS_RGB_ORDER_XBGR;
		break;
	case DRM_FORMAT_XRGB8888:
		dvscntr |= DVS_FORMAT_RGBX888;
		break;
	case DRM_FORMAT_YUYV:
		dvscntr |= DVS_FORMAT_YUV422 | DVS_YUV_ORDER_YUYV;
		break;
	case DRM_FORMAT_YVYU:
		dvscntr |= DVS_FORMAT_YUV422 | DVS_YUV_ORDER_YVYU;
		break;
	case DRM_FORMAT_UYVY:
		dvscntr |= DVS_FORMAT_YUV422 | DVS_YUV_ORDER_UYVY;
		break;
	case DRM_FORMAT_VYUY:
		dvscntr |= DVS_FORMAT_YUV422 | DVS_YUV_ORDER_VYUY;
		break;
	default:
		BUG();
	}

	/*
	 * Enable gamma to match primary/cursor plane behaviour.
	 * FIXME should be user controllable via propertiesa.
	 */
	dvscntr |= DVS_GAMMA_ENABLE;

	if (obj->tiling_mode != I915_TILING_NONE)
		dvscntr |= DVS_TILED;

	if (IS_GEN6(dev))
		dvscntr |= DVS_TRICKLE_FEED_DISABLE; /* must disable */
	dvscntr |= DVS_ENABLE;

	to_intel_crtc(crtc)->primary_enabled = !disable_primary;
	intel_update_sprite_watermarks(plane, crtc, src_w, pixel_size, true,
				       src_w != crtc_w || src_h != crtc_h,
				       &pipe_wm);

	/* Sizes are 0 based */
	src_w--;
	src_h--;
	crtc_w--;
	crtc_h--;

	dvsscale = 0;
	if (crtc_w != src_w || crtc_h != src_h)
		dvsscale = DVS_SCALE_ENABLE | (src_w << 16) | src_h;

	linear_offset = y * fb->pitches[0] + x * pixel_size;
	dvssurf_offset =
		intel_gen4_compute_page_offset(&x, &y, obj->tiling_mode,
					       pixel_size, fb->pitches[0]);
	linear_offset -= dvssurf_offset;

	local_irq_disable();
	intel_pipe_vblank_evade(crtc);

	intel_program_watermarks(crtc, &pipe_wm);

	intel_update_primary(crtc);

	I915_WRITE(DVSSTRIDE(pipe), fb->pitches[0]);
	I915_WRITE(DVSPOS(pipe), (crtc_y << 16) | crtc_x);

	if (obj->tiling_mode != I915_TILING_NONE)
		I915_WRITE(DVSTILEOFF(pipe), (y << 16) | x);
	else
		I915_WRITE(DVSLINOFF(pipe), linear_offset);

	I915_WRITE(DVSSIZE(pipe), (crtc_h << 16) | crtc_w);
	I915_WRITE(DVSSCALE(pipe), dvsscale);
	I915_WRITE(DVSCNTR(pipe), dvscntr);
	I915_MODIFY_DISPBASE(DVSSURF(pipe),
			     i915_gem_obj_ggtt_offset(obj) + dvssurf_offset);
	POSTING_READ(DVSSURF(pipe));

	local_irq_enable();
}

static void
ilk_disable_plane(struct drm_plane *plane, struct drm_crtc *crtc)
{
	struct drm_device *dev = plane->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_plane *intel_plane = to_intel_plane(plane);
	int pipe = intel_plane->pipe;
	struct intel_pipe_wm pipe_wm = {};

	to_intel_crtc(crtc)->primary_enabled = true;
	intel_update_sprite_watermarks(plane, crtc, 0, 0, false, false, &pipe_wm);

	local_irq_disable();
	intel_pipe_vblank_evade(crtc);

	intel_program_watermarks(crtc, &pipe_wm);

	intel_update_primary(crtc);

	I915_WRITE(DVSCNTR(pipe), I915_READ(DVSCNTR(pipe)) & ~DVS_ENABLE);
	/* Disable the scaler */
	I915_WRITE(DVSSCALE(pipe), 0);
	/* Flush double buffered register updates */
	I915_MODIFY_DISPBASE(DVSSURF(pipe), 0);
	POSTING_READ(DVSSURF(pipe));

	local_irq_enable();
}

static int
ilk_update_colorkey(struct drm_plane *plane,
		    struct drm_intel_sprite_colorkey *key)
{
	struct drm_device *dev = plane->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_plane *intel_plane;
	u32 dvscntr;
	int ret = 0;

	intel_plane = to_intel_plane(plane);

	I915_WRITE(DVSKEYVAL(intel_plane->pipe), key->min_value);
	I915_WRITE(DVSKEYMAX(intel_plane->pipe), key->max_value);
	I915_WRITE(DVSKEYMSK(intel_plane->pipe), key->channel_mask);

	dvscntr = I915_READ(DVSCNTR(intel_plane->pipe));
	dvscntr &= ~(DVS_SOURCE_KEY | DVS_DEST_KEY);
	if (key->flags & I915_SET_COLORKEY_DESTINATION)
		dvscntr |= DVS_DEST_KEY;
	else if (key->flags & I915_SET_COLORKEY_SOURCE)
		dvscntr |= DVS_SOURCE_KEY;
	I915_WRITE(DVSCNTR(intel_plane->pipe), dvscntr);

	POSTING_READ(DVSKEYMSK(intel_plane->pipe));

	return ret;
}

static void
ilk_get_colorkey(struct drm_plane *plane, struct drm_intel_sprite_colorkey *key)
{
	struct drm_device *dev = plane->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_plane *intel_plane;
	u32 dvscntr;

	intel_plane = to_intel_plane(plane);

	key->min_value = I915_READ(DVSKEYVAL(intel_plane->pipe));
	key->max_value = I915_READ(DVSKEYMAX(intel_plane->pipe));
	key->channel_mask = I915_READ(DVSKEYMSK(intel_plane->pipe));
	key->flags = 0;

	dvscntr = I915_READ(DVSCNTR(intel_plane->pipe));

	if (dvscntr & DVS_DEST_KEY)
		key->flags = I915_SET_COLORKEY_DESTINATION;
	else if (dvscntr & DVS_SOURCE_KEY)
		key->flags = I915_SET_COLORKEY_SOURCE;
	else
		key->flags = I915_SET_COLORKEY_NONE;
}

static bool
format_is_yuv(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_YUYV:
	case DRM_FORMAT_UYVY:
	case DRM_FORMAT_VYUY:
	case DRM_FORMAT_YVYU:
		return true;
	default:
		return false;
	}
}

static u32
ilk_current_surface(struct drm_plane *plane)
{
	struct intel_plane *intel_plane;

	intel_plane = to_intel_plane(plane);

	return DVSSURFLIVE(intel_plane->pipe);
}

static void
intel_plane_queue_unpin(struct intel_plane *plane,
			struct drm_i915_gem_object *obj)
{
	/*
	 * If the surface is currently being scanned out, we need to
	 * wait until the next vblank event latches in the new base address
	 * before we unpin it, or we may end up displaying the wrong data.
	 * However, if the old object isn't currently 'live', we can just
	 * unpin right away.
	 */
	if (plane->current_surface)
		if (plane->current_surface(&plane->base) !=
					i915_gem_obj_ggtt_offset(obj)) {
			intel_unpin_fb_obj(obj);
			return;
		}

	intel_crtc_queue_unpin(to_intel_crtc(plane->base.crtc), obj);
}

static int
intel_update_plane(struct drm_plane *plane, struct drm_crtc *crtc,
		   struct drm_framebuffer *fb, int crtc_x, int crtc_y,
		   unsigned int crtc_w, unsigned int crtc_h,
		   uint32_t src_x, uint32_t src_y,
		   uint32_t src_w, uint32_t src_h)
{
	struct drm_device *dev = plane->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	struct intel_plane *intel_plane = to_intel_plane(plane);
	struct intel_framebuffer *intel_fb = to_intel_framebuffer(fb);
	struct drm_i915_gem_object *obj = intel_fb->obj;
	struct drm_i915_gem_object *old_obj = intel_plane->obj;
	int ret;
	bool disable_primary = false;
	bool visible;
	int hscale, vscale;
	int max_scale, min_scale;
	int pixel_size = drm_format_plane_cpp(fb->pixel_format, 0);
	struct drm_rect src = {
		/* sample coordinates in 16.16 fixed point */
		.x1 = src_x,
		.x2 = src_x + src_w,
		.y1 = src_y,
		.y2 = src_y + src_h,
	};
	struct drm_rect dst = {
		/* integer pixels */
		.x1 = crtc_x,
		.x2 = crtc_x + crtc_w,
		.y1 = crtc_y,
		.y2 = crtc_y + crtc_h,
	};
	const struct drm_rect clip = {
		.x2 = intel_crtc->active ? intel_crtc->config.pipe_src_w : 0,
		.y2 = intel_crtc->active ? intel_crtc->config.pipe_src_h : 0,
	};
	const struct {
		int crtc_x, crtc_y;
		unsigned int crtc_w, crtc_h;
		uint32_t src_x, src_y, src_w, src_h;
	} orig = {
		.crtc_x = crtc_x,
		.crtc_y = crtc_y,
		.crtc_w = crtc_w,
		.crtc_h = crtc_h,
		.src_x = src_x,
		.src_y = src_y,
		.src_w = src_w,
		.src_h = src_h,
	};

	/* Don't modify another pipe's plane */
	if (intel_plane->pipe != intel_crtc->pipe) {
		DRM_DEBUG_KMS("Wrong plane <-> crtc mapping\n");
		return -EINVAL;
	}

	/* FIXME check all gen limits */
	if (fb->width < 3 || fb->height < 3 || fb->pitches[0] > 16384) {
		DRM_DEBUG_KMS("Unsuitable framebuffer for plane\n");
		return -EINVAL;
	}

	/* Sprite planes can be linear or x-tiled surfaces */
	switch (obj->tiling_mode) {
		case I915_TILING_NONE:
		case I915_TILING_X:
			break;
		default:
			DRM_DEBUG_KMS("Unsupported tiling mode\n");
			return -EINVAL;
	}

	/*
	 * FIXME the following code does a bunch of fuzzy adjustments to the
	 * coordinates and sizes. We probably need some way to decide whether
	 * more strict checking should be done instead.
	 */
	max_scale = intel_plane->max_downscale << 16;
	min_scale = intel_plane->can_scale ? 1 : (1 << 16);

	hscale = drm_rect_calc_hscale_relaxed(&src, &dst, min_scale, max_scale);
	BUG_ON(hscale < 0);

	vscale = drm_rect_calc_vscale_relaxed(&src, &dst, min_scale, max_scale);
	BUG_ON(vscale < 0);

	visible = drm_rect_clip_scaled(&src, &dst, &clip, hscale, vscale);

	crtc_x = dst.x1;
	crtc_y = dst.y1;
	crtc_w = drm_rect_width(&dst);
	crtc_h = drm_rect_height(&dst);

	if (visible) {
		/* check again in case clipping clamped the results */
		hscale = drm_rect_calc_hscale(&src, &dst, min_scale, max_scale);
		if (hscale < 0) {
			DRM_DEBUG_KMS("Horizontal scaling factor out of limits\n");
			drm_rect_debug_print(&src, true);
			drm_rect_debug_print(&dst, false);

			return hscale;
		}

		vscale = drm_rect_calc_vscale(&src, &dst, min_scale, max_scale);
		if (vscale < 0) {
			DRM_DEBUG_KMS("Vertical scaling factor out of limits\n");
			drm_rect_debug_print(&src, true);
			drm_rect_debug_print(&dst, false);

			return vscale;
		}

		/* Make the source viewport size an exact multiple of the scaling factors. */
		drm_rect_adjust_size(&src,
				     drm_rect_width(&dst) * hscale - drm_rect_width(&src),
				     drm_rect_height(&dst) * vscale - drm_rect_height(&src));

		/* sanity check to make sure the src viewport wasn't enlarged */
		WARN_ON(src.x1 < (int) src_x ||
			src.y1 < (int) src_y ||
			src.x2 > (int) (src_x + src_w) ||
			src.y2 > (int) (src_y + src_h));

		/*
		 * Hardware doesn't handle subpixel coordinates.
		 * Adjust to (macro)pixel boundary, but be careful not to
		 * increase the source viewport size, because that could
		 * push the downscaling factor out of bounds.
		 */
		src_x = src.x1 >> 16;
		src_w = drm_rect_width(&src) >> 16;
		src_y = src.y1 >> 16;
		src_h = drm_rect_height(&src) >> 16;

		if (format_is_yuv(fb->pixel_format)) {
			src_x &= ~1;
			src_w &= ~1;

			/*
			 * Must keep src and dst the
			 * same if we can't scale.
			 */
			if (!intel_plane->can_scale)
				crtc_w &= ~1;

			if (crtc_w == 0)
				visible = false;
		}
	}

	/* Check size restrictions when scaling */
	if (visible && (src_w != crtc_w || src_h != crtc_h)) {
		unsigned int width_bytes;

		WARN_ON(!intel_plane->can_scale);

		/* FIXME interlacing min height is 6 */

		if (crtc_w < 3 || crtc_h < 3)
			visible = false;

		if (src_w < 3 || src_h < 3)
			visible = false;

		width_bytes = ((src_x * pixel_size) & 63) + src_w * pixel_size;

		if (src_w > 2048 || src_h > 2048 ||
		    width_bytes > 4096 || fb->pitches[0] > 4096) {
			DRM_DEBUG_KMS("Source dimensions exceed hardware limits\n");
			return -EINVAL;
		}
	}

	dst.x1 = crtc_x;
	dst.x2 = crtc_x + crtc_w;
	dst.y1 = crtc_y;
	dst.y2 = crtc_y + crtc_h;

	/*
	 * If the sprite is completely covering the primary plane,
	 * we can disable the primary and save power.
	 */
	disable_primary = drm_rect_equals(&dst, &clip);
	WARN_ON(disable_primary && !visible && intel_crtc->active);

	mutex_lock(&dev->struct_mutex);

	/* Note that this will apply the VT-d workaround for scanouts,
	 * which is more restrictive than required for sprites. (The
	 * primary plane requires 256KiB alignment with 64 PTE padding,
	 * the sprite planes only require 128KiB alignment and 32 PTE padding.
	 */
	ret = intel_pin_and_fence_fb_obj(dev, obj, NULL);

	mutex_unlock(&dev->struct_mutex);

	if (ret)
		return ret;

	intel_plane->crtc_x = orig.crtc_x;
	intel_plane->crtc_y = orig.crtc_y;
	intel_plane->crtc_w = orig.crtc_w;
	intel_plane->crtc_h = orig.crtc_h;
	intel_plane->src_x = orig.src_x;
	intel_plane->src_y = orig.src_y;
	intel_plane->src_w = orig.src_w;
	intel_plane->src_h = orig.src_h;
	intel_plane->obj = obj;

	if (intel_crtc->active) {
		if (visible)
			intel_plane->update_plane(plane, crtc, fb, obj,
						  crtc_x, crtc_y, crtc_w, crtc_h,
						  src_x, src_y, src_w, src_h,
						  disable_primary);
		else
			intel_plane->disable_plane(plane, crtc);
	}

	/* Unpin old obj after new one is active to avoid ugliness */
	if (old_obj)
		intel_plane_queue_unpin(intel_plane, old_obj);

	return 0;
}

static int
intel_disable_plane(struct drm_plane *plane)
{
	struct drm_device *dev = plane->dev;
	struct intel_plane *intel_plane = to_intel_plane(plane);
	struct intel_crtc *intel_crtc;

	if (!plane->fb)
		return 0;

	if (WARN_ON(!plane->crtc))
		return -EINVAL;

	intel_crtc = to_intel_crtc(plane->crtc);

	if (intel_crtc->active)
		intel_plane->disable_plane(plane, plane->crtc);

	mutex_lock(&dev->struct_mutex);
	if (intel_plane->obj) {
		intel_plane_queue_unpin(intel_plane, intel_plane->obj);
		intel_plane->obj = NULL;
	}
	mutex_unlock(&dev->struct_mutex);

	return 0;
}

static void intel_destroy_plane(struct drm_plane *plane)
{
	struct intel_plane *intel_plane = to_intel_plane(plane);
	intel_disable_plane(plane);
	drm_plane_cleanup(plane);
	kfree(intel_plane);
}

int intel_sprite_set_colorkey(struct drm_device *dev, void *data,
			      struct drm_file *file_priv)
{
	struct drm_intel_sprite_colorkey *set = data;
	struct drm_mode_object *obj;
	struct drm_plane *plane;
	struct intel_plane *intel_plane;
	int ret = 0;

	if (!drm_core_check_feature(dev, DRIVER_MODESET))
		return -ENODEV;

	/* Make sure we don't try to enable both src & dest simultaneously */
	if ((set->flags & (I915_SET_COLORKEY_DESTINATION | I915_SET_COLORKEY_SOURCE)) == (I915_SET_COLORKEY_DESTINATION | I915_SET_COLORKEY_SOURCE))
		return -EINVAL;

	drm_modeset_lock_all(dev);

	obj = drm_mode_object_find(dev, set->plane_id, DRM_MODE_OBJECT_PLANE);
	if (!obj) {
		ret = -ENOENT;
		goto out_unlock;
	}

	plane = obj_to_plane(obj);
	intel_plane = to_intel_plane(plane);
	ret = intel_plane->update_colorkey(plane, set);

out_unlock:
	drm_modeset_unlock_all(dev);
	return ret;
}

int intel_sprite_get_colorkey(struct drm_device *dev, void *data,
			      struct drm_file *file_priv)
{
	struct drm_intel_sprite_colorkey *get = data;
	struct drm_mode_object *obj;
	struct drm_plane *plane;
	struct intel_plane *intel_plane;
	int ret = 0;

	if (!drm_core_check_feature(dev, DRIVER_MODESET))
		return -ENODEV;

	drm_modeset_lock_all(dev);

	obj = drm_mode_object_find(dev, get->plane_id, DRM_MODE_OBJECT_PLANE);
	if (!obj) {
		ret = -ENOENT;
		goto out_unlock;
	}

	plane = obj_to_plane(obj);
	intel_plane = to_intel_plane(plane);
	intel_plane->get_colorkey(plane, get);

out_unlock:
	drm_modeset_unlock_all(dev);
	return ret;
}

void intel_plane_restore(struct drm_plane *plane)
{
	struct intel_plane *intel_plane = to_intel_plane(plane);

	if (!plane->crtc || !plane->fb)
		return;

	intel_update_plane(plane, plane->crtc, plane->fb,
			   intel_plane->crtc_x, intel_plane->crtc_y,
			   intel_plane->crtc_w, intel_plane->crtc_h,
			   intel_plane->src_x, intel_plane->src_y,
			   intel_plane->src_w, intel_plane->src_h);
}

void intel_plane_disable(struct drm_plane *plane)
{
	if (!plane->crtc || !plane->fb)
		return;

	intel_disable_plane(plane);
}

static const struct drm_plane_funcs intel_plane_funcs = {
	.update_plane = intel_update_plane,
	.disable_plane = intel_disable_plane,
	.destroy = intel_destroy_plane,
};

static uint32_t ilk_plane_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_YUYV,
	DRM_FORMAT_YVYU,
	DRM_FORMAT_UYVY,
	DRM_FORMAT_VYUY,
};

static uint32_t snb_plane_formats[] = {
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_YUYV,
	DRM_FORMAT_YVYU,
	DRM_FORMAT_UYVY,
	DRM_FORMAT_VYUY,
};

static uint32_t vlv_plane_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_ABGR8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_XBGR2101010,
	DRM_FORMAT_ABGR2101010,
	DRM_FORMAT_YUYV,
	DRM_FORMAT_YVYU,
	DRM_FORMAT_UYVY,
	DRM_FORMAT_VYUY,
};

int
intel_plane_init(struct drm_device *dev, enum pipe pipe, int plane)
{
	struct intel_plane *intel_plane;
	unsigned long possible_crtcs;
	const uint32_t *plane_formats;
	int num_plane_formats;
	int ret;

	if (INTEL_INFO(dev)->gen < 5)
		return -ENODEV;

	intel_plane = kzalloc(sizeof(*intel_plane), GFP_KERNEL);
	if (!intel_plane)
		return -ENOMEM;

	switch (INTEL_INFO(dev)->gen) {
	case 5:
	case 6:
		intel_plane->can_scale = true;
		intel_plane->max_downscale = 16;
		intel_plane->update_plane = ilk_update_plane;
		intel_plane->disable_plane = ilk_disable_plane;
		intel_plane->update_colorkey = ilk_update_colorkey;
		intel_plane->get_colorkey = ilk_get_colorkey;
		intel_plane->current_surface = ilk_current_surface;

		if (IS_GEN6(dev)) {
			plane_formats = snb_plane_formats;
			num_plane_formats = ARRAY_SIZE(snb_plane_formats);
		} else {
			plane_formats = ilk_plane_formats;
			num_plane_formats = ARRAY_SIZE(ilk_plane_formats);
		}
		break;

	case 7:
	case 8:
		if (IS_IVYBRIDGE(dev)) {
			intel_plane->can_scale = true;
			intel_plane->max_downscale = 2;
		} else {
			intel_plane->can_scale = false;
			intel_plane->max_downscale = 1;
		}

		if (IS_VALLEYVIEW(dev)) {
			intel_plane->update_plane = vlv_update_plane;
			intel_plane->disable_plane = vlv_disable_plane;
			intel_plane->update_colorkey = vlv_update_colorkey;
			intel_plane->get_colorkey = vlv_get_colorkey;

			plane_formats = vlv_plane_formats;
			num_plane_formats = ARRAY_SIZE(vlv_plane_formats);
		} else {
			intel_plane->update_plane = ivb_update_plane;
			intel_plane->disable_plane = ivb_disable_plane;
			intel_plane->update_colorkey = ivb_update_colorkey;
			intel_plane->get_colorkey = ivb_get_colorkey;
			intel_plane->current_surface = ivb_current_surface;

			plane_formats = snb_plane_formats;
			num_plane_formats = ARRAY_SIZE(snb_plane_formats);
		}
		break;

	default:
		kfree(intel_plane);
		return -ENODEV;
	}

	intel_plane->pipe = pipe;
	intel_plane->plane = plane;
	possible_crtcs = (1 << pipe);
	ret = drm_plane_init(dev, &intel_plane->base, possible_crtcs,
			     &intel_plane_funcs,
			     plane_formats, num_plane_formats,
			     false);
	if (ret)
		kfree(intel_plane);

	return ret;
}
