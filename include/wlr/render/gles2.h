/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_RENDER_GLES2_H
#define WLR_RENDER_GLES2_H

#include <GLES2/gl2.h>
#include <wlr/backend.h>
#include <wlr/render/wlr_renderer.h>

struct wlr_egl;

struct wlr_renderer *wlr_gles2_renderer_create_with_drm_fd(int drm_fd);
struct wlr_renderer *wlr_gles2_renderer_create(struct wlr_egl *egl);

bool wlr_gles2_renderer_check_ext(struct wlr_renderer *renderer,
	const char *ext);

struct wlr_gles2_texture_attribs {
	GLenum target; /* either GL_TEXTURE_2D or GL_TEXTURE_EXTERNAL_OES */
	GLuint tex;

	bool inverted_y;
	bool has_alpha;
};

bool wlr_texture_is_gles2(struct wlr_texture *texture);
void wlr_gles2_texture_get_attribs(struct wlr_texture *texture,
	struct wlr_gles2_texture_attribs *attribs);

struct wlr_egl *gles2_renderer_get_egl(struct wlr_renderer *renderer);

#endif
