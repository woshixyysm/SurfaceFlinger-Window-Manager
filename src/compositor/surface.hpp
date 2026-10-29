// SPDX-License-Identifier: AGPL-3.0-or-later
// surface.hpp
//
// struct View + wl_region/wl_surface/wl_shm
//
// Extracted from the former single-file implementation; behaviour is unchanged.
// Depends on include order: everything used here is provided by the modules
// included before it in src/main.cpp.
#pragma once

#include "compositor/view.hpp"

// ---------------------------------------------------------------- wl_region
static void region_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void region_add(wl_client *, wl_resource *, int32_t, int32_t, int32_t, int32_t) {}
static void region_subtract(wl_client *, wl_resource *, int32_t, int32_t, int32_t, int32_t) {}
static const struct wl_region_interface region_impl = {
    .destroy = region_destroy,
    .add = region_add,
    .subtract = region_subtract,
};

static void region_handle_destroyed(wl_resource *r) { (void)r; }

// set_opaque_region and set_input_region do not transfer ownership: the client destroys
// the region itself, and destroying it here breaks later requests with invalid_object.
static void surface_set_region(wl_client *, wl_resource *, wl_resource *region, bool input) {
    (void)region;
    (void)input;
}

// ---------------------------------------------------------------- wl_surface
static void surf_destroy(wl_client *, wl_resource *r) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (v && v->read_next) {          // the held frame has to go back to the client
        if (v->hold) { wl_list_remove(&v->hold->lis.link); delete v->hold; v->hold = nullptr; }
        wl_buffer_send_release(v->read_next);
        v->read_next = nullptr;
    }
    wl_resource_destroy(r);
}

static void surf_attach(wl_client *, wl_resource *r, wl_resource *buffer, int32_t, int32_t) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (v) v->pending_buffer = buffer;
}

static void surf_damage(wl_client *, wl_resource *, int32_t, int32_t, int32_t, int32_t) {}

static void surf_frame(wl_client *, wl_resource *r, uint32_t id) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    wl_resource *cb = wl_resource_create(wl_resource_get_client(r), &wl_callback_interface,
                                         wl_resource_get_version(r), id);
    if (!cb) { wl_client_post_no_memory(wl_resource_get_client(r)); return; }
    if (v->frame_cb) wl_resource_destroy(v->frame_cb);
    v->frame_cb = cb;
}

static void surf_set_opaque_region(wl_client *c, wl_resource *r, wl_resource *region) {
    surface_set_region(c, r, region, false);
}
static void surf_set_input_region(wl_client *c, wl_resource *r, wl_resource *region) {
    surface_set_region(c, r, region, true);
}

static void surf_commit(wl_client *, wl_resource *r) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (v && getenv("SFWM_TRACE_COMMIT"))
        printf("[trace] commit v=%p title='%s' pend=%p next=%p mapped=%d ud=%p\n",
               (void *)v, v->title, (void *)v->pending_buffer, (void *)v->read_next,
               (int)v->mapped,
               v->pending_buffer ? wl_resource_get_user_data(v->pending_buffer) : nullptr);
    if (v) v->commit();
}

static void surf_set_buffer_transform(wl_client *, wl_resource *, int32_t) {}
static void surf_set_buffer_scale(wl_client *, wl_resource *r, int32_t scale) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    v->buf_scale = scale > 1 ? scale : 1;
    printf("[input] buffer_scale=%d\n", v->buf_scale);
}
static void surf_damage_buffer(wl_client *, wl_resource *, int32_t, int32_t, int32_t, int32_t) {}

// wl_surface.offset (v5): move a surface without changing its content.
static void surf_offset(wl_client *, wl_resource *r, int32_t dx, int32_t dy) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v || (dx == 0 && dy == 0)) return;
    v->x += dx;
    v->y += dy;
    if (v->mapped) v->place();
}

static const struct wl_surface_interface surface_impl = {
    .destroy = surf_destroy,
    .attach = surf_attach,
    .damage = surf_damage,
    .frame = surf_frame,
    .set_opaque_region = surf_set_opaque_region,
    .set_input_region = surf_set_input_region,
    .commit = surf_commit,
    .set_buffer_transform = surf_set_buffer_transform,
    .set_buffer_scale = surf_set_buffer_scale,
    .damage_buffer = surf_damage_buffer,
    .offset = surf_offset,
};

// ---- wl_shm server implementation ----
static void shm_buffer_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct wl_buffer_interface buffer_impl = {
    .destroy = shm_buffer_destroy,
};

static void shm_pool_create_buffer(wl_client *c, wl_resource *r, uint32_t id, int32_t offset,
                                   int32_t width, int32_t height, int32_t stride, uint32_t format) {
    ShmPool *pool = (ShmPool *)wl_resource_get_user_data(r);
    if (!pool || !pool->base) { wl_client_post_implementation_error(c, "pool is not mapped"); return; }

    if (width <= 0 || height <= 0 || stride < width * 4) {
        wl_resource_post_error(r, WL_SHM_ERROR_INVALID_STRIDE, "invalid arguments");
        return;
    }
    int64_t need = (int64_t)offset + (int64_t)stride * height;
    if (offset < 0 || need > (int64_t)pool->size) {
        wl_resource_post_error(r, WL_SHM_ERROR_INVALID_FD, "buffer out of range %lld > %d",
                               (long long)need, pool->size);
        return;
    }

    wl_resource *br = wl_resource_create(c, &wl_buffer_interface, 1, id);
    if (!br) { wl_client_post_no_memory(c); return; }
    ShmBuf *b = new ShmBuf();
    b->pool = pool;
    b->offset = offset;
    pool->refs++;              // the mapping lives as long as a buffer refers to it
    b->w = width; b->h = height; b->stride = stride; b->format = format;
    wl_resource_set_implementation(br, &buffer_impl, b, [](wl_resource *res) {
        ShmBuf *bb = (ShmBuf *)wl_resource_get_user_data(res);
        if (!bb) return;
        shm_pool_unref(bb->pool);
        delete bb;
    });
}

static void shm_pool_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static void shm_pool_resize(wl_client *, wl_resource *r, int32_t size) {
    ShmPool *pool = (ShmPool *)wl_resource_get_user_data(r);
    if (!pool || size <= pool->size) return;
    if (pool->base) munmap(pool->base, pool->size);
    // The mapping must be writable: screencopy and image-copy-capture write captured
    // pixels into the client shm buffer, and a read-only mapping faults.
    void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, pool->fd, 0);
    if (p == MAP_FAILED) { pool->base = nullptr; wl_resource_post_no_memory(r); return; }
    pool->base = (uint8_t *)p;
    pool->size = size;
}

static const struct wl_shm_pool_interface shm_pool_impl = {
    .create_buffer = shm_pool_create_buffer,
    .destroy = shm_pool_destroy,
    .resize = shm_pool_resize,
};

static void shm_create_pool(wl_client *c, wl_resource *r, uint32_t id, int32_t fd, int32_t size) {
    // PROT_WRITE is required by screencopy, which writes into the client buffer.
    void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        wl_resource_post_error(r, WL_SHM_ERROR_INVALID_FD, "mmap failed: %s", strerror(errno));
        close(fd);
        return;
    }
    wl_resource *pres = wl_resource_create(c, &wl_shm_pool_interface, 1, id);
    if (!pres) { munmap(p, size); close(fd); wl_client_post_no_memory(c); return; }

    ShmPool *pool = new ShmPool();
    pool->fd = fd; pool->size = size; pool->base = (uint8_t *)p;
    wl_resource_set_implementation(pres, &shm_pool_impl, pool, [](wl_resource *res) {
        shm_pool_unref((ShmPool *)wl_resource_get_user_data(res));
    });
}

static void shm_release(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wl_shm_interface shm_impl = {
    .create_pool = shm_create_pool,
    .release = shm_release,
};

static void bind_shm(wl_client *c, void *, uint32_t version, uint32_t id) {
    wl_resource *r = wl_resource_create(c, &wl_shm_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &shm_impl, nullptr, nullptr);
    wl_shm_send_format(r, WL_SHM_FORMAT_ARGB8888);
    wl_shm_send_format(r, WL_SHM_FORMAT_XRGB8888);
}

