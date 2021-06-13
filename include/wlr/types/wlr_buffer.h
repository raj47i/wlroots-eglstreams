/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_BUFFER_H
#define WLR_TYPES_WLR_BUFFER_H

#include <pixman.h>
#include <wayland-server-core.h>
#include <wlr/render/dmabuf.h>
#include <wlr/render/egl.h>

struct wlr_buffer;

struct wlr_shm_attributes {
	int fd;
	uint32_t format;
	int width, height, stride;
	off_t offset;
};

struct wlr_buffer_impl {
	void (*destroy)(struct wlr_buffer *buffer);
	bool (*get_dmabuf)(struct wlr_buffer *buffer,
		struct wlr_dmabuf_attributes *attribs);
	bool (*get_shm)(struct wlr_buffer *buffer,
		struct wlr_shm_attributes *attribs);
	bool (*begin_data_ptr_access)(struct wlr_buffer *buffer, void **data,
		uint32_t *format, size_t *stride);
	void (*end_data_ptr_access)(struct wlr_buffer *buffer);
};

/**
 * A buffer containing pixel data.
 *
 * A buffer has a single producer (the party who created the buffer) and
 * multiple consumers (parties reading the buffer). When all consumers are done
 * with the buffer, it gets released and can be re-used by the producer. When
 * the producer and all consumers are done with the buffer, it gets destroyed.
 */
struct wlr_buffer {
	const struct wlr_buffer_impl *impl;

	int width, height;

	bool dropped;
	size_t n_locks;
	bool accessing_data_ptr;

	struct {
		struct wl_signal destroy;
		struct wl_signal release;
	} events;

	// Non-null for EGLStreams
	struct wlr_eglstream *egl_stream;
};

/**
 * Initialize a buffer. This function should be called by producers. The
 * initialized buffer is referenced: once the producer is done with the buffer
 * they should call wlr_buffer_drop.
 */
void wlr_buffer_init(struct wlr_buffer *buffer,
	const struct wlr_buffer_impl *impl, int width, int height);
/**
 * Unreference the buffer. This function should be called by producers when
 * they are done with the buffer.
 */
void wlr_buffer_drop(struct wlr_buffer *buffer);
/**
 * Lock the buffer. This function should be called by consumers to make
 * sure the buffer can be safely read from. Once the consumer is done with the
 * buffer, they should call wlr_buffer_unlock.
 */
struct wlr_buffer *wlr_buffer_lock(struct wlr_buffer *buffer);
/**
 * Unlock the buffer. This function should be called by consumers once they are
 * done with the buffer.
 */
void wlr_buffer_unlock(struct wlr_buffer *buffer);
/**
 * Reads the DMA-BUF attributes of the buffer. If this buffer isn't a DMA-BUF,
 * returns false.
 *
 * The returned DMA-BUF attributes are valid for the lifetime of the
 * wlr_buffer. The caller isn't responsible for cleaning up the DMA-BUF
 * attributes.
 */
bool wlr_buffer_get_dmabuf(struct wlr_buffer *buffer,
	struct wlr_dmabuf_attributes *attribs);
/**
 * Read shared memory attributes of the buffer. If this buffer isn't shared
 * memory, returns false.
 *
 * The returned shared memory attributes are valid for the lifetime of the
 * wlr_buffer. The caller isn't responsible for cleaning up the shared memory
 * attributes.
 */
bool wlr_buffer_get_shm(struct wlr_buffer *buffer,
	struct wlr_shm_attributes *attribs);

/**
 * A client buffer.
 */
struct wlr_client_buffer {
	struct wlr_buffer base;

	/**
	 * The buffer resource, if any. Will be NULL if the client destroys it.
	 */
	struct wl_resource *resource;
	/**
	 * Whether a release event has been sent to the resource.
	 */
	bool resource_released;
	/**
	 * The buffer's texture, if any. A buffer will not have a texture if the
	 * client destroys the buffer before it has been released.
	 */
	struct wlr_texture *texture;

	struct wl_listener resource_destroy;
	struct wl_listener release;
};

struct wlr_renderer;

/**
 * Get a client buffer from a generic buffer. If the buffer isn't a client
 * buffer, returns NULL.
 */
struct wlr_client_buffer *wlr_client_buffer_get(struct wlr_buffer *buffer);
/**
 * Check if a resource is a wl_buffer resource.
 */
bool wlr_resource_is_buffer(struct wl_resource *resource);
/**
 * Get the size of a wl_buffer resource.
 */
bool wlr_resource_get_buffer_size(struct wl_resource *resource,
	struct wlr_renderer *renderer, int *width, int *height);
/**
 * Import a client buffer and lock it.
 *
 * Once the caller is done with the buffer, they must call wlr_buffer_unlock.
 */
struct wlr_client_buffer *wlr_client_buffer_import(
	struct wlr_renderer *renderer, struct wl_resource *resource);
/**
 * Try to update the buffer's content. On success, returns the updated buffer
 * and destroys the provided `buffer`. On error, `buffer` is intact and NULL is
 * returned.
 *
 * Fails if there's more than one reference to the buffer or if the texture
 * isn't mutable.
 */
struct wlr_client_buffer *wlr_client_buffer_apply_damage(
	struct wlr_client_buffer *buffer, struct wl_resource *resource,
	pixman_region32_t *damage);

#endif
