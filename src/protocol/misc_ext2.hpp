// misc_ext2.hpp
//
// viewporter / cursor-shape / tearing / 剪贴板 等
//
// 从 sfwm.cpp 原样搬出（只搬不改，逻辑零差异）。
// 依赖包含顺序：本文件用到的东西必须由**前面的**模块提供，
// 因此它在 sfwm.cpp 里的 include 位置就是它原来在文件里的位置。
#pragma once

// ---------------------------------------------------------------- 更多协议
//
// 目标：把常见客户端 / 嵌套窗管会 bind 的全局对象都提供出来。
// 大多数实现是"接受并忽略"，但只要对象存在，客户端就不会在启动时直接失败。
// 真正影响能否跑起来的是 wl_compositor / wl_shm / wl_seat / xdg-shell / wl_output，
// 这几个前面已经实现了。

// ---- wp_viewporter（缩放/裁剪；我们只记住，不改实际合成）
static void viewport_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void viewport_set_source(wl_client *, wl_resource *, wl_fixed_t, wl_fixed_t, wl_fixed_t, wl_fixed_t) {}
static void viewport_set_destination(wl_client *, wl_resource *, int32_t, int32_t) {}
static const struct wp_viewport_interface viewport_impl = {
    .destroy = viewport_destroy,
    .set_source = viewport_set_source,
    .set_destination = viewport_set_destination,
};
static void viewporter_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void viewporter_get_viewport(wl_client *c, wl_resource *res, uint32_t id, wl_resource *) {
    wl_resource *v = wl_resource_create(c, &wp_viewport_interface, wl_resource_get_version(res), id);
    if (!v) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(v, &viewport_impl, nullptr, nullptr);
}
static const struct wp_viewporter_interface viewporter_impl = {
    .destroy = viewporter_destroy,
    .get_viewport = viewporter_get_viewport,
};

// ---- wp_presentation 没接：Termux 这份生成的 server-header 里缺少
// wp_presentation_feedback_interface 的结构定义，不强行用（对客户端不是必需的）。

// ---- wp_fractional_scale_v1
static void frac_scale_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct wp_fractional_scale_v1_interface frac_scale_impl = {
    .destroy = frac_scale_destroy,
};
static void frac_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void frac_mgr_get(wl_client *c, wl_resource *res, uint32_t id, wl_resource *) {
    wl_resource *f = wl_resource_create(c, &wp_fractional_scale_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!f) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(f, &frac_scale_impl, nullptr, nullptr);
    wp_fractional_scale_v1_send_preferred_scale(f, (uint32_t)(120 * (g_scale > 0 ? g_scale : 1)));
}
static const struct wp_fractional_scale_manager_v1_interface frac_mgr_impl = {
    .destroy = frac_mgr_destroy,
    .get_fractional_scale = frac_mgr_get,
};

// ---- wp_cursor_shape_v1
static void cs_dev_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
// shape 只是个枚举（不带像素），合成器得自己按编号找图 —— 交给 cursor_build()。
static void cs_dev_set_shape(wl_client *, wl_resource *, uint32_t serial, uint32_t shape) {
    (void)serial;
    if (shape != g_cur_shape) {
        g_cur_shape = shape;
        cursor_build(shape);
        printf("[cursor] 形状 -> %u\n", shape);
    }
    if (!(g_srv.cursor_view && g_srv.cursor_view->layer.win)) cursor_present();
}
static const struct wp_cursor_shape_device_v1_interface cs_dev_impl = {
    .destroy = cs_dev_destroy,
    .set_shape = cs_dev_set_shape,
};
static void cs_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void cs_mgr_get_pointer(wl_client *c, wl_resource *res, uint32_t id, wl_resource *) {
    wl_resource *d = wl_resource_create(c, &wp_cursor_shape_device_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!d) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(d, &cs_dev_impl, nullptr, nullptr);
}
static void cs_mgr_get_tablet_tool(wl_client *c, wl_resource *res, uint32_t id, wl_resource *) {
    wl_resource *d = wl_resource_create(c, &wp_cursor_shape_device_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!d) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(d, &cs_dev_impl, nullptr, nullptr);
}
static const struct wp_cursor_shape_manager_v1_interface cs_mgr_impl = {
    .destroy = cs_mgr_destroy,
    .get_pointer = cs_mgr_get_pointer,
    .get_tablet_tool_v2 = cs_mgr_get_tablet_tool,
};

// ---- wp_single_pixel_buffer_v1（我们自己造一块 1x1 的 shm 缓冲）
static void spb_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void spb_create_u32_rgba(wl_client *c, wl_resource *res, uint32_t id,
                                uint32_t r_, uint32_t g_, uint32_t b_, uint32_t a_) {
    int fd = sfwm_rt_file("sfwm-1px", 4);
    if (fd < 0) {
        wl_client_post_no_memory(c);
        return;
    }
    void *p = mmap(nullptr, 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { close(fd); wl_client_post_no_memory(c); return; }
    // ARGB8888 小端：byte0=B, 1=G, 2=R, 3=A
    uint8_t *px = (uint8_t *)p;
    px[0] = (uint8_t)(b_ >> 24 ? b_ >> 24 : b_);
    px[1] = (uint8_t)g_;
    px[2] = (uint8_t)r_;
    px[3] = (uint8_t)a_;

    ShmPool *pool = new ShmPool();
    pool->fd = fd;
    pool->size = 4;
    pool->base = (uint8_t *)p;

    wl_resource *br = wl_resource_create(c, &wl_buffer_interface, wl_resource_get_version(res), id);
    if (!br) { shm_pool_unref(pool); wl_client_post_no_memory(c); return; }
    ShmBuf *sb = new ShmBuf();
    sb->pool = pool;
    sb->offset = 0;
    pool->refs++;
    sb->w = 1; sb->h = 1; sb->stride = 4; sb->format = WL_SHM_FORMAT_ARGB8888;
    wl_resource_set_implementation(br, &buffer_impl, sb, [](wl_resource *rr) {
        ShmBuf *bb = (ShmBuf *)wl_resource_get_user_data(rr);
        if (!bb) return;
        shm_pool_unref(bb->pool);
        delete bb;
    });
}
static const struct wp_single_pixel_buffer_manager_v1_interface spb_impl = {
    .destroy = spb_destroy,
    .create_u32_rgba_buffer = spb_create_u32_rgba,
};

// ---- wp_tearing_control_v1
static void tc_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void tc_set_hint(wl_client *, wl_resource *, uint32_t) {}
static const struct wp_tearing_control_v1_interface tc_impl = {
    .set_presentation_hint = tc_set_hint,
    .destroy = tc_destroy,
};
static void tc_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void tc_mgr_get(wl_client *c, wl_resource *res, uint32_t id, wl_resource *) {
    wl_resource *o = wl_resource_create(c, &wp_tearing_control_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!o) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(o, &tc_impl, nullptr, nullptr);
}
static const struct wp_tearing_control_manager_v1_interface tc_mgr_impl = {
    .destroy = tc_mgr_destroy,
    .get_tearing_control = tc_mgr_get,
};

// ---- zwp_relative_pointer_v1
static std::vector<wl_resource *> g_relative_ptrs;
static void rp_destroy(wl_client *, wl_resource *r) {
    for (size_t i = 0; i < g_relative_ptrs.size(); i++)
        if (g_relative_ptrs[i] == r) { g_relative_ptrs.erase(g_relative_ptrs.begin() + i); break; }
    wl_resource_destroy(r);
}
static const struct zwp_relative_pointer_v1_interface rp_impl = { .destroy = rp_destroy };
static void rp_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void rp_mgr_get(wl_client *c, wl_resource *res, uint32_t id, wl_resource *) {
    wl_resource *o = wl_resource_create(c, &zwp_relative_pointer_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!o) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(o, &rp_impl, nullptr, nullptr);
    g_relative_ptrs.push_back(o);
}
static const struct zwp_relative_pointer_manager_v1_interface rp_mgr_impl = {
    .destroy = rp_mgr_destroy,
    .get_relative_pointer = rp_mgr_get,
};

// ---- zwp_pointer_constraints_v1（锁/限制指针：直接告诉客户端"已生效"）
static void lp_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void lp_set_hint(wl_client *, wl_resource *, wl_fixed_t, wl_fixed_t) {}
static void lp_set_region(wl_client *, wl_resource *, wl_resource *) {}
static const struct zwp_locked_pointer_v1_interface lp_impl = {
    .set_cursor_position_hint = lp_set_hint,
    .set_region = lp_set_region,
    .destroy = lp_destroy,
};
static void cp_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void cp_set_region(wl_client *, wl_resource *, wl_resource *) {}
static const struct zwp_confined_pointer_v1_interface cp_impl = {
    .set_region = cp_set_region,
    .destroy = cp_destroy,
};
static void pc_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void pc_mgr_lock(wl_client *c, wl_resource *res, uint32_t id, wl_resource *,
                        wl_resource *, wl_resource *, uint32_t) {
    wl_resource *o = wl_resource_create(c, &zwp_locked_pointer_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!o) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(o, &lp_impl, nullptr, nullptr);
    zwp_locked_pointer_v1_send_locked(o);
}
static void pc_mgr_confine(wl_client *c, wl_resource *res, uint32_t id, wl_resource *,
                           wl_resource *, wl_resource *, uint32_t) {
    wl_resource *o = wl_resource_create(c, &zwp_confined_pointer_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!o) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(o, &cp_impl, nullptr, nullptr);
    zwp_confined_pointer_v1_send_confined(o);
}
static const struct zwp_pointer_constraints_v1_interface pc_mgr_impl = {
    .destroy = pc_mgr_destroy,
    .lock_pointer = pc_mgr_lock,
    .confine_pointer = pc_mgr_confine,
};

// ---- zwp_idle_inhibit_v1
static void ii_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct zwp_idle_inhibitor_v1_interface ii_impl = { .destroy = ii_destroy };
static void ii_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void ii_mgr_create(wl_client *c, wl_resource *res, uint32_t id, wl_resource *) {
    wl_resource *o = wl_resource_create(c, &zwp_idle_inhibitor_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!o) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(o, &ii_impl, nullptr, nullptr);
}
static const struct zwp_idle_inhibit_manager_v1_interface ii_mgr_impl = {
    .destroy = ii_mgr_destroy,
    .create_inhibitor = ii_mgr_create,
};

// ---- zwp_keyboard_shortcuts_inhibit_v1（告诉客户端快捷键归它）
static void ksi_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct zwp_keyboard_shortcuts_inhibitor_v1_interface ksi_impl = { .destroy = ksi_destroy };
static void ksi_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void ksi_mgr_inhibit(wl_client *c, wl_resource *res, uint32_t id, wl_resource *, wl_resource *) {
    wl_resource *o = wl_resource_create(c, &zwp_keyboard_shortcuts_inhibitor_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!o) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(o, &ksi_impl, nullptr, nullptr);
    zwp_keyboard_shortcuts_inhibitor_v1_send_active(o);
}
static const struct zwp_keyboard_shortcuts_inhibit_manager_v1_interface ksi_mgr_impl = {
    .destroy = ksi_mgr_destroy,
    .inhibit_shortcuts = ksi_mgr_inhibit,
};

// ---- zxdg_output_manager_v1
static void xo_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct zxdg_output_v1_interface xo_impl = { .destroy = xo_destroy };
static void xo_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void xo_mgr_get(wl_client *c, wl_resource *res, uint32_t id, wl_resource *output) {
    uint32_t ver = wl_resource_get_version(res);
    wl_resource *o = wl_resource_create(c, &zxdg_output_v1_interface, ver, id);
    if (!o) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(o, &xo_impl, output, nullptr);
    int32_t lw = g_out_w / (g_scale > 0 ? g_scale : 1);
    int32_t lh = g_out_h / (g_scale > 0 ? g_scale : 1);
    zxdg_output_v1_send_logical_position(o, 0, 0);
    zxdg_output_v1_send_logical_size(o, lw, lh);
    if (ver >= 2) {
        zxdg_output_v1_send_name(o, "SFWM-1");
        zxdg_output_v1_send_description(o, "sfwm output");
    }
    if (ver < 3) {
        zxdg_output_v1_send_done(o);
    } else if (output && wl_resource_get_version(output) >= 2) {
        // xdg-output v3 起 `zxdg_output_v1.done` 被删除，客户端改用 `wl_output.done`
        // 来给这一批信息收尾。（v3 规范原话：“From version 3 on, the protocol uses
        // wl_output.done”。）
        // 麻烦在于：bind_output() 里已经发过一次 done，但那是在客户端请求
        // get_xdg_output **之前**，此时它还不知道 logical_size。Qt 的 QWaylandOutput
        // 会因此认为信息不完整、屏幕建不出来，只打印
        //     qt.qpa.wayland: There are no outputs - creating placeholder screen
        // 然后套一个占位屏（尺寸错乱、UI 不绘制）。
        // ⇒ xdg 数据发完之后必须**再补一次** wl_output.done。规范允许多次 done。
        wl_output_send_done(output);
    }
}
static const struct zxdg_output_manager_v1_interface xo_mgr_impl = {
    .destroy = xo_mgr_destroy,
    .get_xdg_output = xo_mgr_get,
};

// ---- zxdg_decoration_manager_v1（我们不做服务端装饰）
static void deco_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void deco_send_mode(wl_resource *r) {
    zxdg_toplevel_decoration_v1_send_configure(r, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
}
static void deco_set_mode(wl_client *, wl_resource *r, uint32_t) { deco_send_mode(r); }
static void deco_unset_mode(wl_client *, wl_resource *r) { deco_send_mode(r); }
static const struct zxdg_toplevel_decoration_v1_interface deco_impl = {
    .destroy = deco_destroy,
    .set_mode = deco_set_mode,
    .unset_mode = deco_unset_mode,
};
static void deco_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void deco_mgr_get(wl_client *c, wl_resource *res, uint32_t id, wl_resource *) {
    wl_resource *o = wl_resource_create(c, &zxdg_toplevel_decoration_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!o) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(o, &deco_impl, nullptr, nullptr);
    deco_send_mode(o);
}
static const struct zxdg_decoration_manager_v1_interface deco_mgr_impl = {
    .destroy = deco_mgr_destroy,
    .get_toplevel_decoration = deco_mgr_get,
};

// wl_data_device_manager / zwp_primary_selection_device_manager_v1
// 已经搬到 protocol/data_device.hpp（真实现，不再是空桩）。
// 这里只留一行提醒：别把旧空桩加回来 —— 那会盖掉 bind_datadev/bind_primsel 的左右。

// 没状态的全局对象：用宏生成 bind 函数，省得每个协议手写一遍
#define DEFINE_BIND(NAME, IFACE, IMPL)                                            \
    static void bind_##NAME(wl_client *c, void *, uint32_t version, uint32_t id) { \
        if (version > IFACE.version) version = IFACE.version;                      \
        wl_resource *r = wl_resource_create(c, &IFACE, version, id);               \
        if (!r) { wl_client_post_no_memory(c); return; }                           \
        wl_resource_set_implementation(r, &IMPL, nullptr, nullptr);                \
    }

DEFINE_BIND(viewporter, wp_viewporter_interface, viewporter_impl)
DEFINE_BIND(fracescale, wp_fractional_scale_manager_v1_interface, frac_mgr_impl)
DEFINE_BIND(cursorshape, wp_cursor_shape_manager_v1_interface, cs_mgr_impl)
DEFINE_BIND(spbuffer, wp_single_pixel_buffer_manager_v1_interface, spb_impl)
DEFINE_BIND(tearing, wp_tearing_control_manager_v1_interface, tc_mgr_impl)
DEFINE_BIND(relptr, zwp_relative_pointer_manager_v1_interface, rp_mgr_impl)
DEFINE_BIND(ptrcon, zwp_pointer_constraints_v1_interface, pc_mgr_impl)
DEFINE_BIND(idleinhibit, zwp_idle_inhibit_manager_v1_interface, ii_mgr_impl)
DEFINE_BIND(ksinhibit, zwp_keyboard_shortcuts_inhibit_manager_v1_interface, ksi_mgr_impl)
DEFINE_BIND(xdgoutput, zxdg_output_manager_v1_interface, xo_mgr_impl)
DEFINE_BIND(decoration, zxdg_decoration_manager_v1_interface, deco_mgr_impl)

