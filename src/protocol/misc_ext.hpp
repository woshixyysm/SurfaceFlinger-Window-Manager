// misc_ext.hpp
//
// content_type / alpha_modifier / fifo / 手势 / warp
//
// 从 sfwm.cpp 原样搬出（只搬不改，逻辑零差异）。
// 依赖包含顺序：本文件用到的东西必须由**前面的**模块提供，
// 因此它在 sfwm.cpp 里的 include 位置就是它原来在文件里的位置。
#pragma once

// ---------------------------------------------------------------- 小型扩展合集
// 这几个都是客户端「探测存在性」就决定行为的扩展：实现了就等于告诉它
// "我是正经合成器"。行为本身可以很轻。

// ============ wp_content_type_v1（视频/游戏内容类型，用来切色彩/时序策略）============
static void ct_surface_set_type(wl_client *, wl_resource *r, uint32_t type) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    v->content_type = type;
}
static void ct_surface_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wp_content_type_v1_interface ct_surface_impl = {
    .destroy         = ct_surface_destroy,
    .set_content_type = ct_surface_set_type,
};

static void ct_get_surface(wl_client *c, wl_resource *res, uint32_t id, wl_resource *surface) {
    View *v = view_from_surface(surface);
    if (!v) {
        wl_resource_post_error(res, 0, "content_type on unknown surface");
        return;
    }
    wl_resource *r = wl_resource_create(c, &wp_content_type_v1_interface, wl_resource_get_version(res), id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &ct_surface_impl, v, nullptr);
}

static void ct_manager_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wp_content_type_manager_v1_interface ct_manager_impl = {
    .destroy     = ct_manager_destroy,
    .get_surface_content_type = ct_get_surface,
};

static void bind_content_type(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &wp_content_type_manager_v1_interface, version, id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &ct_manager_impl, nullptr, nullptr);
}

// ============ wp_alpha_modifier_v1（整表面透明度乘子）============
static void am_surface_set_multiplier(wl_client *, wl_resource *r, uint32_t factor) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    // 32 位定点：0xFFFFFFFF == 1.0，我们内部用 0..256
    v->alpha_mult = (int32_t)(((uint64_t)factor * 256ull) / 0xFFFFFFFFull);
    if (v->alpha_mult < 0) v->alpha_mult = 0;
    if (v->alpha_mult > 256) v->alpha_mult = 256;
}
static void am_surface_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wp_alpha_modifier_surface_v1_interface am_surface_impl = {
    .destroy        = am_surface_destroy,
    .set_multiplier = am_surface_set_multiplier,
};

static void am_get_surface(wl_client *c, wl_resource *res, uint32_t id, wl_resource *surface) {
    View *v = view_from_surface(surface);
    if (!v) {
        wl_resource_post_error(res, 0, "alpha_modifier on unknown surface");
        return;
    }
    wl_resource *r = wl_resource_create(c, &wp_alpha_modifier_surface_v1_interface, wl_resource_get_version(res), id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &am_surface_impl, v, nullptr);
}

static void am_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wp_alpha_modifier_v1_interface am_impl = {
    .destroy     = am_destroy,
    .get_surface = am_get_surface,
};

static void bind_alpha_modifier(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &wp_alpha_modifier_v1_interface, version, id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &am_impl, nullptr, nullptr);
}

// ============ wp_fifo_v1（显式排队/等待栅栏）============
// 我们本来就是「提交即拷帧」，天然按顺序执行，所以栅栏立刻放行即可。
static void fifo_set_barrier(wl_client *, wl_resource *) {}
static void fifo_wait_barrier(wl_client *, wl_resource *) {}
static void fifo_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wp_fifo_v1_interface fifo_impl = {
    .destroy      = fifo_destroy,
    .set_barrier  = fifo_set_barrier,
    .wait_barrier = fifo_wait_barrier,
};

static void fifo_get(wl_client *c, wl_resource *res, uint32_t id, wl_resource *surface) {
    if (!view_from_surface(surface)) {
        wl_resource_post_error(res, 0, "fifo on unknown surface");
        return;
    }
    wl_resource *r = wl_resource_create(c, &wp_fifo_v1_interface, wl_resource_get_version(res), id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &fifo_impl, nullptr, nullptr);
}

static void fifo_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wp_fifo_manager_v1_interface fifo_mgr_impl = {
    .destroy  = fifo_mgr_destroy,
    .get_fifo = fifo_get,
};

static void bind_fifo(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &wp_fifo_manager_v1_interface, version, id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &fifo_mgr_impl, nullptr, nullptr);
}

// ============ zwp_pointer_gestures_v1（触控板手势对象）============
// 真发事件需要触控板多点输入，这里先把对象建出来，客户端不至于因为
// "缺扩展"而关掉手势支持。
static void gest_obj_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

// 这三个接口只有 destroy 一个请求，wayland-scanner 不给它们生成 requests 结构，
// 而且 xyz_interface 这个名字已被 wl_interface 占用 → 自己起名，按 void* 传进去。
struct gest_iface_impl {
    void (*destroy)(struct wl_client *client, struct wl_resource *resource);
};
static const struct gest_iface_impl gest_impl = { .destroy = gest_obj_destroy };

static void pg_get_swipe(wl_client *c, wl_resource *res, uint32_t id, wl_resource *pointer) {
    wl_resource *r = wl_resource_create(c, &zwp_pointer_gesture_swipe_v1_interface, wl_resource_get_version(res), id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, (const void *)&gest_impl, nullptr, nullptr);
}
static void pg_get_pinch(wl_client *c, wl_resource *res, uint32_t id, wl_resource *pointer) {
    wl_resource *r = wl_resource_create(c, &zwp_pointer_gesture_pinch_v1_interface, wl_resource_get_version(res), id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, (const void *)&gest_impl, nullptr, nullptr);
}
static void pg_get_hold(wl_client *c, wl_resource *res, uint32_t id, wl_resource *pointer) {
    wl_resource *r = wl_resource_create(c, &zwp_pointer_gesture_hold_v1_interface, wl_resource_get_version(res), id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, (const void *)&gest_impl, nullptr, nullptr);
}
static void pg_release(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void pg_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

// 同样：这个协议整个生成头里都没有 requests 结构 → 自己声明。
// 成员顺序必须严格照 XML 的 request 声明顺序（opcode 就是下标）。
struct pg_iface_impl {
    void (*get_swipe_gesture)(struct wl_client *, struct wl_resource *, uint32_t, struct wl_resource *);
    void (*get_pinch_gesture)(struct wl_client *, struct wl_resource *, uint32_t, struct wl_resource *);
    void (*get_hold_gesture)(struct wl_client *, struct wl_resource *, uint32_t, struct wl_resource *);
    void (*release)(struct wl_client *, struct wl_resource *);
    void (*destroy)(struct wl_client *, struct wl_resource *);
};
static const struct pg_iface_impl pg_impl = {
    .get_swipe_gesture = pg_get_swipe,
    .get_pinch_gesture = pg_get_pinch,
    .get_hold_gesture  = pg_get_hold,
    .release           = pg_release,
    .destroy           = pg_destroy,
};

static void bind_pointer_gestures(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 3) version = 3;
    wl_resource *r = wl_resource_create(c, &zwp_pointer_gestures_v1_interface, version, id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, (const void *)&pg_impl, nullptr, nullptr);
}

// ============ wp_pointer_warp_v1（把指针瞬移到指定 surface 坐标）============
static int32_t g_pointer_x_abs = 0, g_pointer_y_abs = 0;   // 见已有的 pointer_x/y
// 签名：warp_pointer(pointer, surface, x, y, serial)
static void warp_warp(wl_client *, wl_resource *r, wl_resource *pointer, wl_resource *surface,
                      wl_fixed_t x, wl_fixed_t y, uint32_t serial) {
    (void)pointer;
    (void)serial;
    View *v = view_from_surface(surface);
    if (!v) return;
    // 视口内坐标 → 屏幕坐标（按 buffer_scale 放大）
    const double lx = wl_fixed_to_double(x), ly = wl_fixed_to_double(y);
    const int32_t sc = v->buf_scale > 0 ? v->buf_scale : 1;
    g_cur_x = v->x + (int32_t)(lx * sc);
    g_cur_y = v->y + (int32_t)(ly * sc);
    cursor_present();
}
static void warp_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wp_pointer_warp_v1_interface warp_impl = {
    .destroy      = warp_destroy,
    .warp_pointer = warp_warp,
};

static void bind_pointer_warp(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &wp_pointer_warp_v1_interface, version, id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &warp_impl, nullptr, nullptr);
}


