#include <assert.h>
#include <stdlib.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/util/log.h>
#include "render/pixel_format.h"
#include "render/wlr_texture.h"
#include "types/wlr_buffer.h"
#include "util/signal.h"

void wlr_buffer_init(struct wlr_buffer *buffer,
		const struct wlr_buffer_impl *impl, int width, int height) {
	assert(impl->destroy);
	if (impl->begin_data_ptr_access || impl->end_data_ptr_access) {
		assert(impl->begin_data_ptr_access && impl->end_data_ptr_access);
	}
	buffer->impl = impl;
	buffer->width = width;
	buffer->height = height;
	buffer->egl_stream = NULL;
	wl_signal_init(&buffer->events.destroy);
	wl_signal_init(&buffer->events.release);
}

static void buffer_consider_destroy(struct wlr_buffer *buffer) {
	if (!buffer->dropped || buffer->n_locks > 0) {
		return;
	}

	assert(!buffer->accessing_data_ptr);

	wlr_signal_emit_safe(&buffer->events.destroy, NULL);

	buffer->impl->destroy(buffer);
}

void wlr_buffer_drop(struct wlr_buffer *buffer) {
	if (buffer == NULL) {
		return;
	}

	assert(!buffer->dropped);
	buffer->dropped = true;
	buffer_consider_destroy(buffer);
}

struct wlr_buffer *wlr_buffer_lock(struct wlr_buffer *buffer) {
	buffer->n_locks++;
	return buffer;
}

void wlr_buffer_unlock(struct wlr_buffer *buffer) {
	if (buffer == NULL) {
		return;
	}

	assert(buffer->n_locks > 0);
	buffer->n_locks--;

	if (buffer->n_locks == 0) {
		wl_signal_emit(&buffer->events.release, NULL);
	}

	buffer_consider_destroy(buffer);
}

bool wlr_buffer_get_dmabuf(struct wlr_buffer *buffer,
		struct wlr_dmabuf_attributes *attribs) {
	if (!buffer->impl->get_dmabuf) {
		return false;
	}
	return buffer->impl->get_dmabuf(buffer, attribs);
}

bool buffer_begin_data_ptr_access(struct wlr_buffer *buffer, void **data,
		uint32_t *format, size_t *stride) {
	assert(!buffer->accessing_data_ptr);
	if (!buffer->impl->begin_data_ptr_access) {
		return false;
	}
	if (!buffer->impl->begin_data_ptr_access(buffer, data, format, stride)) {
		return false;
	}
	buffer->accessing_data_ptr = true;
	return true;
}

void buffer_end_data_ptr_access(struct wlr_buffer *buffer) {
	assert(buffer->accessing_data_ptr);
	buffer->impl->end_data_ptr_access(buffer);
	buffer->accessing_data_ptr = false;
}

bool wlr_buffer_get_shm(struct wlr_buffer *buffer,
		struct wlr_shm_attributes *attribs) {
	if (!buffer->impl->get_shm) {
		return false;
	}
	return buffer->impl->get_shm(buffer, attribs);
}

bool wlr_resource_is_buffer(struct wl_resource *resource) {
	return strcmp(wl_resource_get_class(resource), wl_buffer_interface.name) == 0;
}

bool wlr_resource_get_buffer_size(struct wl_resource *resource,
		struct wlr_renderer *renderer, int *width, int *height) {
	assert(wlr_resource_is_buffer(resource));

	struct wl_shm_buffer *shm_buf = wl_shm_buffer_get(resource);
	if (shm_buf != NULL) {
		*width = wl_shm_buffer_get_width(shm_buf);
		*height = wl_shm_buffer_get_height(shm_buf);
	} else if (wlr_renderer_resource_is_wl_drm_buffer(renderer,
			resource)) {
		wlr_renderer_wl_drm_buffer_get_size(renderer, resource,
			width, height);
	} else if (wlr_dmabuf_v1_resource_is_buffer(resource)) {
		struct wlr_dmabuf_v1_buffer *dmabuf =
			wlr_dmabuf_v1_buffer_from_buffer_resource(resource);
		*width = dmabuf->attributes.width;
		*height = dmabuf->attributes.height;
	} else if (wlr_renderer_wl_buffer_get_params(renderer,
			resource, width, height, NULL)) {
		(void)0;
	} else {
		*width = *height = 0;
		return false;
	}

	return true;
}

static const struct wlr_buffer_impl client_buffer_impl;

struct wlr_client_buffer *wlr_client_buffer_get(struct wlr_buffer *buffer) {
	if (buffer->impl != &client_buffer_impl) {
		return NULL;
	}
	return (struct wlr_client_buffer *)buffer;
}

static struct wlr_client_buffer *client_buffer_from_buffer(
		struct wlr_buffer *buffer) {
	struct wlr_client_buffer *client_buffer = wlr_client_buffer_get(buffer);
	assert(client_buffer != NULL);
	return client_buffer;
}

static void client_buffer_destroy(struct wlr_buffer *_buffer) {
	struct wlr_client_buffer *buffer = client_buffer_from_buffer(_buffer);

	if (!buffer->resource_released && buffer->resource != NULL) {
		wl_buffer_send_release(buffer->resource);
	}

	wl_list_remove(&buffer->resource_destroy.link);
	wlr_texture_destroy(buffer->texture);
	free(buffer);
}

static bool client_buffer_get_dmabuf(struct wlr_buffer *_buffer,
		struct wlr_dmabuf_attributes *attribs) {
	struct wlr_client_buffer *buffer = client_buffer_from_buffer(_buffer);

	if (buffer->resource == NULL) {
		return false;
	}

	struct wl_resource *buffer_resource = buffer->resource;
	if (!wlr_dmabuf_v1_resource_is_buffer(buffer_resource)) {
		return false;
	}

	struct wlr_dmabuf_v1_buffer *dmabuf_buffer =
		wlr_dmabuf_v1_buffer_from_buffer_resource(buffer_resource);
	memcpy(attribs, &dmabuf_buffer->attributes,
		sizeof(struct wlr_dmabuf_attributes));
	return true;
}

static const struct wlr_buffer_impl client_buffer_impl = {
	.destroy = client_buffer_destroy,
	.get_dmabuf = client_buffer_get_dmabuf,
};

static void client_buffer_resource_handle_destroy(struct wl_listener *listener,
		void *data) {
	struct wlr_client_buffer *buffer =
		wl_container_of(listener, buffer, resource_destroy);
	wl_list_remove(&buffer->resource_destroy.link);
	wl_list_init(&buffer->resource_destroy.link);
	buffer->resource = NULL;

	// At this point, if the wl_buffer comes from linux-dmabuf or wl_drm, we
	// still haven't released it (ie. we'll read it in the future) but the
	// client destroyed it. Reading the texture itself should be fine because
	// we still hold a reference to the DMA-BUF via the texture. However the
	// client could decide to re-use the same DMA-BUF for something else, in
	// which case we'll read garbage. We decide to accept this risk.
}

static void client_buffer_handle_release(struct wl_listener *listener,
		void *data) {
	struct wlr_client_buffer *buffer =
		wl_container_of(listener, buffer, release);
	if (!buffer->resource_released && buffer->resource != NULL) {
		wl_buffer_send_release(buffer->resource);
		buffer->resource_released = true;
	}
}

struct wlr_client_buffer *wlr_client_buffer_import(
		struct wlr_renderer *renderer, struct wl_resource *resource) {
	assert(wlr_resource_is_buffer(resource));

	struct wlr_texture *texture = NULL;
	bool resource_released = false;

	if (wl_shm_buffer_get(resource) != NULL) {
		struct wlr_shm_client_buffer *shm_client_buffer =
			shm_client_buffer_create(resource);
		if (shm_client_buffer == NULL) {
			wlr_log(WLR_ERROR, "Failed to create shm client buffer");
			return NULL;
		}

		// Ensure the buffer will be released before being destroyed
		wlr_buffer_lock(&shm_client_buffer->base);
		wlr_buffer_drop(&shm_client_buffer->base);

		texture = wlr_texture_from_buffer(renderer, &shm_client_buffer->base);

		// The renderer should've locked the buffer by now if necessary
		wlr_buffer_unlock(&shm_client_buffer->base);

		// The renderer is responsible for releasing the buffer when
		// appropriate
		resource_released = true;
	} else if (wlr_renderer_resource_is_wl_drm_buffer(renderer, resource)) {
		texture = wlr_texture_from_wl_drm(renderer, resource);
	} else if (wlr_dmabuf_v1_resource_is_buffer(resource)) {
		struct wlr_dmabuf_v1_buffer *dmabuf =
			wlr_dmabuf_v1_buffer_from_buffer_resource(resource);
		texture = wlr_texture_from_buffer(renderer, &dmabuf->base);

		// The renderer is responsible for releasing the buffer when
		// appropriate
		resource_released = true;
	} else if ((texture = wlr_texture_from_wl_eglstream(renderer, resource))) {
		(void)0; // Nothing special is needed for EGLStream texture here
	} else {
		wlr_log(WLR_ERROR, "Cannot upload texture: unknown buffer type");

		// Instead of just logging the error, also disconnect the client with a
		// fatal protocol error so that it's clear something went wrong.
		wl_resource_post_error(resource, 0, "unknown buffer type");
		return NULL;
	}

	if (texture == NULL) {
		wlr_log(WLR_ERROR, "Failed to upload texture");
		wl_buffer_send_release(resource);
		return NULL;
	}

	struct wlr_client_buffer *buffer =
		calloc(1, sizeof(struct wlr_client_buffer));
	if (buffer == NULL) {
		wlr_texture_destroy(texture);
		wl_resource_post_no_memory(resource);
		return NULL;
	}
	wlr_buffer_init(&buffer->base, &client_buffer_impl,
		texture->width, texture->height);
	buffer->resource = resource;
	buffer->texture = texture;
	buffer->resource_released = resource_released;

	wl_resource_add_destroy_listener(resource, &buffer->resource_destroy);
	buffer->resource_destroy.notify = client_buffer_resource_handle_destroy;

	buffer->release.notify = client_buffer_handle_release;
	wl_signal_add(&buffer->base.events.release, &buffer->release);

	// Ensure the buffer will be released before being destroyed
	wlr_buffer_lock(&buffer->base);
	wlr_buffer_drop(&buffer->base);

	return buffer;
}

struct wlr_client_buffer *wlr_client_buffer_apply_damage(
		struct wlr_client_buffer *buffer, struct wl_resource *resource,
		pixman_region32_t *damage) {
	assert(wlr_resource_is_buffer(resource));

	if (buffer->base.n_locks > 1) {
		// Someone else still has a reference to the buffer
		return NULL;
	}

	struct wl_shm_buffer *shm_buf = wl_shm_buffer_get(resource);
	struct wl_shm_buffer *old_shm_buf = wl_shm_buffer_get(buffer->resource);
	if (shm_buf == NULL || old_shm_buf == NULL) {
		// Uploading only damaged regions only works for wl_shm buffers and
		// mutable textures (created from wl_shm buffer)
		return NULL;
	}

	enum wl_shm_format new_fmt = wl_shm_buffer_get_format(shm_buf);
	enum wl_shm_format old_fmt = wl_shm_buffer_get_format(old_shm_buf);
	if (new_fmt != old_fmt) {
		// Uploading to textures can't change the format
		return NULL;
	}

	int32_t stride = wl_shm_buffer_get_stride(shm_buf);
	int32_t width = wl_shm_buffer_get_width(shm_buf);
	int32_t height = wl_shm_buffer_get_height(shm_buf);

	if ((uint32_t)width != buffer->texture->width ||
			(uint32_t)height != buffer->texture->height) {
		return NULL;
	}

	wl_shm_buffer_begin_access(shm_buf);
	void *data = wl_shm_buffer_get_data(shm_buf);

	int n;
	pixman_box32_t *rects = pixman_region32_rectangles(damage, &n);
	for (int i = 0; i < n; ++i) {
		pixman_box32_t *r = &rects[i];
		if (!wlr_texture_write_pixels(buffer->texture, stride,
				r->x2 - r->x1, r->y2 - r->y1, r->x1, r->y1,
				r->x1, r->y1, data)) {
			wl_shm_buffer_end_access(shm_buf);
			return NULL;
		}
	}

	wl_shm_buffer_end_access(shm_buf);

	// We have uploaded the data, we don't need to access the wl_buffer
	// anymore
	wl_buffer_send_release(resource);

	wl_list_remove(&buffer->resource_destroy.link);
	wl_resource_add_destroy_listener(resource, &buffer->resource_destroy);
	buffer->resource_destroy.notify = client_buffer_resource_handle_destroy;

	buffer->resource = resource;
	buffer->resource_released = true;
	return buffer;
}

static const struct wlr_buffer_impl shm_client_buffer_impl;

static struct wlr_shm_client_buffer *shm_client_buffer_from_buffer(
		struct wlr_buffer *buffer) {
	assert(buffer->impl == &shm_client_buffer_impl);
	return (struct wlr_shm_client_buffer *)buffer;
}

static void shm_client_buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct wlr_shm_client_buffer *buffer =
		shm_client_buffer_from_buffer(wlr_buffer);
	wl_list_remove(&buffer->resource_destroy.link);
	wl_list_remove(&buffer->release.link);
	if (buffer->saved_shm_pool != NULL) {
		wl_shm_pool_unref(buffer->saved_shm_pool);
	}
	free(buffer);
}

static bool shm_client_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
		void **data, uint32_t *format, size_t *stride) {
	struct wlr_shm_client_buffer *buffer =
		shm_client_buffer_from_buffer(wlr_buffer);
	*format = buffer->format;
	*stride = buffer->stride;
	if (buffer->shm_buffer != NULL) {
		*data = wl_shm_buffer_get_data(buffer->shm_buffer);
		wl_shm_buffer_begin_access(buffer->shm_buffer);
	} else {
		*data = buffer->saved_data;
	}
	return true;
}

static void shm_client_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
	struct wlr_shm_client_buffer *buffer =
		shm_client_buffer_from_buffer(wlr_buffer);
	if (buffer->shm_buffer != NULL) {
		wl_shm_buffer_end_access(buffer->shm_buffer);
	}
}

static const struct wlr_buffer_impl shm_client_buffer_impl = {
	.destroy = shm_client_buffer_destroy,
	.begin_data_ptr_access = shm_client_buffer_begin_data_ptr_access,
	.end_data_ptr_access = shm_client_buffer_end_data_ptr_access,
};

static void shm_client_buffer_resource_handle_destroy(
		struct wl_listener *listener, void *data) {
	struct wlr_shm_client_buffer *buffer =
		wl_container_of(listener, buffer, resource_destroy);

	// In order to still be able to access the shared memory region, we need to
	// keep a reference to the wl_shm_pool
	buffer->saved_shm_pool = wl_shm_buffer_ref_pool(buffer->shm_buffer);
	buffer->saved_data = wl_shm_buffer_get_data(buffer->shm_buffer);

	// The wl_shm_buffer destroys itself with the wl_resource
	buffer->resource = NULL;
	buffer->shm_buffer = NULL;
	wl_list_remove(&buffer->resource_destroy.link);
	wl_list_init(&buffer->resource_destroy.link);
}

static void shm_client_buffer_handle_release(struct wl_listener *listener,
		void *data) {
	struct wlr_shm_client_buffer *buffer =
		wl_container_of(listener, buffer, release);
	if (buffer->resource != NULL) {
		wl_buffer_send_release(buffer->resource);
	}
}

struct wlr_shm_client_buffer *shm_client_buffer_create(
		struct wl_resource *resource) {
	struct wl_shm_buffer *shm_buffer = wl_shm_buffer_get(resource);
	assert(shm_buffer != NULL);

	int32_t width = wl_shm_buffer_get_width(shm_buffer);
	int32_t height = wl_shm_buffer_get_height(shm_buffer);

	struct wlr_shm_client_buffer *buffer = calloc(1, sizeof(*buffer));
	if (buffer == NULL) {
		return NULL;
	}
	wlr_buffer_init(&buffer->base, &shm_client_buffer_impl, width, height);
	buffer->resource = resource;
	buffer->shm_buffer = shm_buffer;

	enum wl_shm_format wl_shm_format = wl_shm_buffer_get_format(shm_buffer);
	buffer->format = convert_wl_shm_format_to_drm(wl_shm_format);
	buffer->stride = wl_shm_buffer_get_stride(shm_buffer);

	buffer->resource_destroy.notify = shm_client_buffer_resource_handle_destroy;
	wl_resource_add_destroy_listener(resource, &buffer->resource_destroy);

	buffer->release.notify = shm_client_buffer_handle_release;
	wl_signal_add(&buffer->base.events.release, &buffer->release);

	return buffer;
}
