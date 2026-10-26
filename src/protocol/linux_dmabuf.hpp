// linux_dmabuf.hpp
//
// zwp_linux_dmabuf_v1（+ feedback）
//
// 从 sfwm.cpp 原样搬出（只搬不改，逻辑零差异）。
// 依赖包含顺序：本文件用到的东西必须由**前面的**模块提供，
// 因此它在 sfwm.cpp 里的 include 位置就是它原来在文件里的位置。
#pragma once

// ---------------------------------------------------------------- zwp_linux_dmabuf_v1
// Mesa/zink 在 Wayland 上要求合成器支持 wl_drm 或 zwp_linux_dmabuf_v1，否则
// egl_dri2 会断言 "software || dri2_dpy->fd_render_gpu >= 0" 而直接挂掉。
// 这里提供 dmabuf 协议；真正的 buffer 导入（送进 SF）在下一步做。

struct DmabufParams {
    DmabufBuf buf;
};

static void dmabuf_buffer_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct wl_buffer_interface dmabuf_buffer_impl = {
    .destroy = dmabuf_buffer_destroy,
};

static void dmabuf_params_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static void dmabuf_params_add(wl_client *, wl_resource *r, int32_t fd, uint32_t plane_idx,
                              uint32_t offset, uint32_t stride, uint32_t modifier_hi,
                              uint32_t modifier_lo) {
    DmabufParams *p = (DmabufParams *)wl_resource_get_user_data(r);
    if (!p) { close(fd); return; }
    if (plane_idx >= 4) {
        wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
                               "plane_idx %u 太大", plane_idx);
        close(fd);
        return;
    }
    if (p->buf.fds[plane_idx] >= 0) close(p->buf.fds[plane_idx]);
    p->buf.fds[plane_idx] = fd;
    p->buf.offset[plane_idx] = offset;
    p->buf.stride[plane_idx] = stride;
    p->buf.modifier = ((uint64_t)modifier_hi << 32) | modifier_lo;
    if ((int)plane_idx + 1 > p->buf.nplanes) p->buf.nplanes = (int)plane_idx + 1;
}

static wl_resource *dmabuf_make_buffer(wl_client *c, DmabufParams *p, int32_t width,
                                       int32_t height, uint32_t format, uint32_t buffer_id) {
    if (width <= 0 || height <= 0 || p->buf.nplanes == 0) {
        return nullptr;
    }
    wl_resource *br = wl_resource_create(c, &wl_buffer_interface, 1, buffer_id);
    if (!br) { wl_client_post_no_memory(c); return nullptr; }

    DmabufBuf *b = new DmabufBuf();
    *b = p->buf;              // fd 所有权转给 buffer
    b->w = width;
    b->h = height;
    b->drm_format = format;
    for (int i = 0; i < 4; i++) p->buf.fds[i] = -1;
    p->buf.nplanes = 0;

    wl_resource_set_implementation(br, &dmabuf_buffer_impl, b, [](wl_resource *res) {
        DmabufBuf *bb = (DmabufBuf *)wl_resource_get_user_data(res);
        if (!bb) return;
        for (int i = 0; i < 4; i++) if (bb->fds[i] >= 0) close(bb->fds[i]);
        delete bb;
    });
    return br;
}

// 异步版：buffer 的资源由服务端分配（id=0 → wayland 自动分配），通过 created 事件送出
static void dmabuf_params_create(wl_client *c, wl_resource *r, int32_t width, int32_t height,
                                 uint32_t format, uint32_t flags) {
    (void)flags;
    DmabufParams *p = (DmabufParams *)wl_resource_get_user_data(r);
    if (!p) return;
    if (width <= 0 || height <= 0 || p->buf.nplanes == 0) {
        wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
                               "参数不全 %dx%d planes=%d", width, height, p->buf.nplanes);
        return;
    }
    wl_resource *br = dmabuf_make_buffer(c, p, width, height, format, 0);
    if (!br) return;
    zwp_linux_buffer_params_v1_send_created(r, br);
}

// 同步版（v2+）：客户端自己给 buffer id（注意 create_immed 的 buffer_id 在最前面）
static void dmabuf_params_create_immed(wl_client *c, wl_resource *r, uint32_t buffer_id,
                                       int32_t width, int32_t height,
                                       uint32_t format, uint32_t flags) {
    (void)flags;
    DmabufParams *p = (DmabufParams *)wl_resource_get_user_data(r);
    if (!p) return;
    if (!dmabuf_make_buffer(c, p, width, height, format, buffer_id)) {
        wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
                               "参数不全 %dx%d planes=%d", width, height, p->buf.nplanes);
    }
}

static const struct zwp_linux_buffer_params_v1_interface dmabuf_params_impl = {
    .destroy = dmabuf_params_destroy,
    .add = dmabuf_params_add,
    .create = dmabuf_params_create,
    .create_immed = dmabuf_params_create_immed,
};

static void dmabuf_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static void dmabuf_create_params(wl_client *c, wl_resource *r, uint32_t id) {
    wl_resource *pr = wl_resource_create(c, &zwp_linux_buffer_params_v1_interface,
                                        wl_resource_get_version(r), id);
    if (!pr) { wl_client_post_no_memory(c); return; }
    DmabufParams *p = new DmabufParams();
    wl_resource_set_implementation(pr, &dmabuf_params_impl, p, [](wl_resource *res) {
        DmabufParams *pp = (DmabufParams *)wl_resource_get_user_data(res);
        if (!pp) return;
        for (int i = 0; i < 4; i++) if (pp->buf.fds[i] >= 0) close(pp->buf.fds[i]);
        delete pp;
    });
}

// v4 新增的 surface feedback：我们不做直接扫描输出，但必须把格式表/设备告诉客户端，
// 否则嵌套窗管（aquamarine）拿不到 allocator。
static void dmabuf_feedback_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct zwp_linux_dmabuf_feedback_v1_interface dmabuf_feedback_impl = {
    .destroy = dmabuf_feedback_destroy,
};

static void dmabuf_send_feedback(wl_resource *f) {
    // 格式表：每项 format(u32) + pad(u32) + modifier(u64)
    struct __attribute__((packed)) Entry { uint32_t format, pad; uint64_t mod; };
    Entry entries[2] = { { 0x34325241u, 0, 0 }, { 0x34325258u, 0, 0 } };
    const uint32_t sz = (uint32_t)sizeof(entries);
    int fd = sfwm_rt_file("sfwm-dmabuf-fmt", 0);
    if (fd < 0) return;
    if (write(fd, entries, sz) != (ssize_t)sz) { close(fd); return; }
    lseek(fd, 0, SEEK_SET);
    zwp_linux_dmabuf_feedback_v1_send_format_table(f, fd, sz);
    close(fd);

    struct stat st;
    uint64_t dev = 0;
    if (stat("/dev/dri/renderD128", &st) == 0) dev = (uint64_t)st.st_rdev;

    struct wl_array devarr;
    wl_array_init(&devarr);
    uint32_t *dv = (uint32_t *)wl_array_add(&devarr, 2 * sizeof(uint32_t));
    if (dv) { dv[0] = (uint32_t)(dev >> 32); dv[1] = (uint32_t)dev; }
    zwp_linux_dmabuf_feedback_v1_send_main_device(f, &devarr);
    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(f, &devarr);
    wl_array_release(&devarr);

    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(f, 0 /* SCANOUT 不开 */);

    struct wl_array idx;
    wl_array_init(&idx);
    uint16_t *iv = (uint16_t *)wl_array_add(&idx, 2 * sizeof(uint16_t));
    if (iv) { iv[0] = 0; iv[1] = 1; }
    zwp_linux_dmabuf_feedback_v1_send_tranche_formats(f, &idx);
    wl_array_release(&idx);

    zwp_linux_dmabuf_feedback_v1_send_tranche_done(f);
    zwp_linux_dmabuf_feedback_v1_send_done(f);
}

static void dmabuf_make_feedback(wl_client *c, wl_resource *res, uint32_t id) {
    wl_resource *f = wl_resource_create(c, &zwp_linux_dmabuf_feedback_v1_interface,
                                       wl_resource_get_version(res), id);
    if (!f) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(f, &dmabuf_feedback_impl, nullptr, nullptr);
    dmabuf_send_feedback(f);
}

static void dmabuf_get_default_feedback(wl_client *c, wl_resource *res, uint32_t id) {
    dmabuf_make_feedback(c, res, id);
}

static void dmabuf_get_surface_feedback(wl_client *c, wl_resource *res, uint32_t id, wl_resource *) {
    dmabuf_make_feedback(c, res, id);
}

static const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
    .destroy = dmabuf_destroy,
    .create_params = dmabuf_create_params,
    .get_default_feedback = dmabuf_get_default_feedback,
    .get_surface_feedback = dmabuf_get_surface_feedback,
};

static void bind_dmabuf(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 4) version = 4;
    printf("[sfwm] 客户端绑定了 zwp_linux_dmabuf_v1 (请求 v%u)\n", version);
    wl_resource *r = wl_resource_create(c, &zwp_linux_dmabuf_v1_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &dmabuf_impl, nullptr, nullptr);
    const uint32_t fmts[2] = { 0x34325241u /*ARGB8888*/, 0x34325258u /*XRGB8888*/ };
    for (int i = 0; i < 2; i++) {
        if (version >= 3) {
            // v3 起用 modifiers 事件；只广告 LINEAR，够客户端分配用
            zwp_linux_dmabuf_v1_send_modifier(r, fmts[i], 0, 0);
        } else {
            wl_resource_post_event(r, ZWP_LINUX_DMABUF_V1_FORMAT, fmts[i]);
        }
    }
}

