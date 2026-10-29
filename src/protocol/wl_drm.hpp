// SPDX-License-Identifier: AGPL-3.0-or-later
// wl_drm.hpp
//
// wl_drm, kept for older clients and the NVIDIA path.
//
// Extracted from the former single-file implementation; behaviour is unchanged.
// Depends on include order: everything used here is provided by the modules
// included before it in src/main.cpp.
#pragma once

// ---------------------------------------------------------------- wl_drm
// Mesa's Wayland EGL, in drm_handle_device():
//   device_name = the string from the wl_drm device event
//   fd_render_gpu = open(device_name), which should be /dev/kgsl-3d0 here
// freedreno then calls fd_device_new(fd) and looks at drmGetVersion()->name:
//   "msm" selects the DRM backend, "kgsl" the KGSL backend used on this device
// This is what makes hardware acceleration work; by default only the render node
static const char *g_drm_device = "/dev/kgsl-3d0";

static void drm_authenticate_cb(wl_client *, wl_resource *r, uint32_t id) {
    printf("[sfwm] wl_drm authenticate id=%u, accepted\n", id);
    wl_drm_send_authenticated(r);   // the authenticated event carries no arguments in this version
}

static void drm_create_buffer_cb(wl_client *c, wl_resource *r, uint32_t id, uint32_t name,
                                 int32_t width, int32_t height, uint32_t stride,
                                 uint32_t format) {
    printf("[sfwm] wl_drm.create_buffer %dx%d name=%u is unsupported, returning an error\n",
           width, height, name);
    wl_resource_post_error(r, WL_DRM_ERROR_INVALID_NAME, "name buffers are unsupported, use dmabuf");
    (void)c; (void)id; (void)stride; (void)format;
}

static void drm_create_planar_buffer_cb(wl_client *, wl_resource *r, uint32_t id, uint32_t name,
                                        int32_t width, int32_t height, uint32_t format,
                                        int32_t offset0, int32_t stride0,
                                        int32_t offset1, int32_t stride1,
                                        int32_t offset2, int32_t stride2) {
    wl_resource_post_error(r, WL_DRM_ERROR_INVALID_NAME, "planar name buffers are unsupported");
    (void)id; (void)name; (void)width; (void)height; (void)format;
    (void)offset0; (void)stride0; (void)offset1; (void)stride1; (void)offset2; (void)stride2;
}

// create_prime_buffer (opcode 3) is the first call Xwayland makes once glamor and
// DRI3 are enabled.
// A NULL entry in drm_impl made libwayland abort the whole compositor, which took
//   `listener function for opcode 3 of wl_drm is NULL`
// the Wayland connection down with it.

// The DmabufBuf and wl_buffer resources of zwp_linux_dmabuf_v1 are reused, since
// View::blit() only looks for DMABUF_MAGIC.
static void drm_create_prime_buffer_cb(wl_client *c, wl_resource *r, uint32_t id, int32_t fd,
                                       int32_t width, int32_t height, uint32_t format,
                                       int32_t offset0, int32_t stride0,
                                       int32_t offset1, int32_t stride1,
                                       int32_t offset2, int32_t stride2) {
    if (fd < 0 || width <= 0 || height <= 0 || stride0 <= 0) {
        if (fd >= 0) close(fd);
        wl_resource_post_error(r, WL_DRM_ERROR_INVALID_NAME,
                               "invalid create_prime_buffer arguments %dx%d stride=%d fd=%d",
                               width, height, stride0, fd);
        return;
    }
    wl_resource *br = wl_resource_create(c, &wl_buffer_interface, 1, id);
    if (!br) { wl_client_post_no_memory(c); close(fd); return; }

    DmabufBuf *b = new DmabufBuf();
    for (int i = 0; i < 4; i++) { b->fds[i] = -1; b->offset[i] = 0; b->stride[i] = 0; }
    b->fds[0] = fd;
    b->offset[0] = (uint32_t)offset0;
    b->stride[0] = (uint32_t)stride0;
    b->nplanes = 1;
    // Multi-plane prime buffers are unused by Xwayland but accepted anyway.
    if (offset1 >= 0 && stride1 > 0) { b->offset[1] = (uint32_t)offset1; b->stride[1] = (uint32_t)stride1; b->nplanes = 2; }
    if (offset2 >= 0 && stride2 > 0) { b->offset[2] = (uint32_t)offset2; b->stride[2] = (uint32_t)stride2; b->nplanes = 3; }
    b->w = width;
    b->h = height;
    b->drm_format = format;
    b->modifier = 0;                      // wl_drm has no modifiers, so the buffer is linear

    wl_resource_set_implementation(br, &dmabuf_buffer_impl, b, [](wl_resource *res) {
        DmabufBuf *bb = (DmabufBuf *)wl_resource_get_user_data(res);
        if (!bb) return;
        for (int i = 0; i < 4; i++) if (bb->fds[i] >= 0) close(bb->fds[i]);
        delete bb;
    });
    printf("[sfwm] wl_drm.create_prime_buffer %dx%d fmt=0x%08x stride=%d off=%d fd=%d planes=%d\n",
           width, height, format, stride0, offset0, fd, b->nplanes);
}

static const struct wl_drm_interface drm_impl = {
    .authenticate = drm_authenticate_cb,
    .create_buffer = drm_create_buffer_cb,
    .create_planar_buffer = drm_create_planar_buffer_cb,
    .create_prime_buffer = drm_create_prime_buffer_cb,
};

static void bind_drm(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 2) version = 2;
    wl_resource *r = wl_resource_create(c, &wl_drm_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &drm_impl, nullptr, nullptr);

    const char *dev = getenv("SFWM_DRM_DEVICE");
    if (dev && *dev) g_drm_device = dev;
    printf("[sfwm] wl_drm bound, advertising device %s\n", g_drm_device);
    wl_drm_send_device(r, g_drm_device);
    wl_drm_send_format(r, WL_DRM_FORMAT_ARGB8888);
    wl_drm_send_format(r, WL_DRM_FORMAT_XRGB8888);
    wl_drm_send_capabilities(r, WL_DRM_CAPABILITY_PRIME);
}

