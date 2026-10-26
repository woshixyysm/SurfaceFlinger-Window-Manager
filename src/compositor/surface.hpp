// surface.hpp
//
// struct View + wl_region/wl_surface/wl_shm
//
// 从 sfwm.cpp 原样搬出（只搬不改，逻辑零差异）。
// 依赖包含顺序：本文件用到的东西必须由**前面的**模块提供，
// 因此它在 sfwm.cpp 里的 include 位置就是它原来在文件里的位置。
#pragma once

#include "view.hpp"

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

// set_opaque_region / set_input_region 不转移所有权！客户端会自己 destroy，
// 服务端提前销毁会导致客户端后续请求 → wl_display invalid_object。
static void surface_set_region(wl_client *, wl_resource *, wl_resource *region, bool input) {
    (void)region;
    (void)input;
}

// ---------------------------------------------------------------- wl_surface
static void surf_destroy(wl_client *, wl_resource *r) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (v && v->read_next) {          // 还攥着的帧必须还给客户端
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

// wl_surface.offset (v5)：用于光标表面等需要“只改位置不改内容”的场景
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

// ---- wl_shm 服务端实现
static void shm_buffer_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct wl_buffer_interface buffer_impl = {
    .destroy = shm_buffer_destroy,
};

static void shm_pool_create_buffer(wl_client *c, wl_resource *r, uint32_t id, int32_t offset,
                                   int32_t width, int32_t height, int32_t stride, uint32_t format) {
    ShmPool *pool = (ShmPool *)wl_resource_get_user_data(r);
    if (!pool || !pool->base) { wl_client_post_implementation_error(c, "pool 未映射"); return; }

    if (width <= 0 || height <= 0 || stride < width * 4) {
        wl_resource_post_error(r, WL_SHM_ERROR_INVALID_STRIDE, "参数非法");
        return;
    }
    int64_t need = (int64_t)offset + (int64_t)stride * height;
    if (offset < 0 || need > (int64_t)pool->size) {
        wl_resource_post_error(r, WL_SHM_ERROR_INVALID_FD, "缓冲越界 %lld > %d",
                               (long long)need, pool->size);
        return;
    }

    wl_resource *br = wl_resource_create(c, &wl_buffer_interface, 1, id);
    if (!br) { wl_client_post_no_memory(c); return; }
    ShmBuf *b = new ShmBuf();
    b->pool = pool;
    b->offset = offset;
    pool->refs++;              // buffer 活多久，映射就留多久
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
    void *p = mmap(nullptr, size, PROT_READ, MAP_SHARED, pool->fd, 0);
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
    void *p = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        wl_resource_post_error(r, WL_SHM_ERROR_INVALID_FD, "mmap 失败: %s", strerror(errno));
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

