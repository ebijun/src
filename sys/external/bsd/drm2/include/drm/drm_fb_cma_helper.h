/*-
 * Copyright (c) 2026 Jun Ebihara <jun@NetBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _DRM_DRM_FB_CMA_HELPER_H_
#define _DRM_DRM_FB_CMA_HELPER_H_

#include <sys/types.h>

/* Forward declarations for Linux framebuffer structures */
struct fb_info;
struct fb_var_screeninfo;
struct fb_cmap;
struct fb_fillrect;
struct fb_copyarea;
struct fb_image;

#include <linux/fb.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/wait.h>

#include <drm/drm_framebuffer.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem_cma_helper.h>

static inline struct drm_gem_cma_object *
drm_fb_cma_get_gem_obj(struct drm_framebuffer *fb, unsigned int plane)
{
	if (!fb || plane >= 4)
		return NULL;

	return (struct drm_gem_cma_object *)fb->obj[plane];
}

#endif /* _DRM_DRM_FB_CMA_HELPER_H_ */

