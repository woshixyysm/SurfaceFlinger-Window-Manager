// seat.hpp
//
// wl_seat / wl_pointer / wl_touch 绑定
//
// 从 sfwm.cpp 原样搬出（只搬不改，逻辑零差异）。
// 依赖包含顺序：本文件用到的东西必须由**前面的**模块提供，
// 因此它在 sfwm.cpp 里的 include 位置就是它原来在文件里的位置。
#pragma once

// ---------------------------------------------------------------- wl_seat / 输入
static void ptr_set_cursor(wl_client *c, wl_resource *, uint32_t, wl_resource *surface, int32_t hx, int32_t hy) {
    (void)c;
    if (!surface) { g_srv.cursor_view = nullptr; return; }
    View *v = (View *)wl_resource_get_user_data(surface);
    g_srv.cursor_view = v;
    g_srv.cursor_hot_x = hx;
    g_srv.cursor_hot_y = hy;
    if (v) v->is_cursor = true;
}
static void ptr_destroyed(wl_resource *r) {
    auto &v = g_srv.pointers;
    for (size_t i = 0; i < v.size(); i++)
        if (v[i] == r) { v.erase(v.begin() + i); printf("[input] pointer 销毁，剩 %zu 个\n", v.size()); break; }
}
// 注意：resource 的销毁路径有两条 —— 客户端发 release 请求，或者客户端直接断连。
// 只管 release 不够，断连时 vector 里会留下悬空指针，之后往它发事件就是
// “interface 'xdg_surface' has no event 5” 那类协议错误（对方直接断开）。
static void ptr_release(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct wl_pointer_interface pointer_impl = {
    .set_cursor = ptr_set_cursor,
    .release = ptr_release,
};

static void touch_destroyed(wl_resource *r) {
    auto &v = g_srv.touches;
    for (size_t i = 0; i < v.size(); i++) if (v[i] == r) { v.erase(v.begin() + i); break; }
}
static void touch_release(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct wl_touch_interface touch_impl = {
    .release = touch_release,
};

static bool ptr_same_client(wl_resource *p, View *v);

static void seat_get_pointer(wl_client *c, wl_resource *seat, uint32_t id) {
    wl_resource *r = wl_resource_create(c, &wl_pointer_interface, wl_resource_get_version(seat), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &pointer_impl, nullptr, ptr_destroyed);
    g_srv.pointers.push_back(r);
    printf("[input] 新 pointer (v%u)，共 %zu 个\n", wl_resource_get_version(r), g_srv.pointers.size());

    // 如果焦点已经存在，必须给新 pointer 补一个 enter，否则客户端永远不认这个指针
    View *v = g_srv.pointer_focus;
    if (v && v->surface && ptr_same_client(r, v)) {
        uint32_t serial = wl_display_next_serial(g_srv.dpy);
        wl_pointer_send_enter(r, serial, v->surface,
                              wl_fixed_from_int((g_srv.pointer_x - v->x) / v->buf_scale),
                              wl_fixed_from_int((g_srv.pointer_y - v->y) / v->buf_scale));
        if (wl_resource_get_version(r) >= 5) wl_pointer_send_frame(r);
        printf("[input] 新 pointer 补发 enter -> (%d,%d)\n",
               g_srv.pointer_x - v->x, g_srv.pointer_y - v->y);
    }
}

static void seat_get_keyboard(wl_client *, wl_resource *, uint32_t);
static void seat_get_touch(wl_client *c, wl_resource *seat, uint32_t id) {
    wl_resource *r = wl_resource_create(c, &wl_touch_interface, wl_resource_get_version(seat), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &touch_impl, nullptr, touch_destroyed);
    g_srv.touches.push_back(r);
}
static void seat_release(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wl_seat_interface seat_impl = {
    .get_pointer = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch,
    .release = seat_release,
};

static bool g_have_input = false;
static bool g_touch_enabled = false;   // 默认只发 pointer：Qt Widgets 不处理 QTouchEvent，
                                       // 收到 wl_touch 反而会把点击吃掉。--touch 才开。

static void bind_seat(wl_client *c, void *, uint32_t version, uint32_t id) {
    wl_resource *r = wl_resource_create(c, &wl_seat_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &seat_impl, nullptr, nullptr);
    // 键盘不依赖触摸设备：走注入 socket，所以总是宣告
    uint32_t caps = WL_SEAT_CAPABILITY_KEYBOARD;
    if (g_have_input) {
        caps |= WL_SEAT_CAPABILITY_POINTER;
        if (g_touch_enabled) caps |= WL_SEAT_CAPABILITY_TOUCH;
    }
    wl_seat_send_capabilities(r, caps);
    if (version >= 2) wl_seat_send_name(r, "seat0");
}

// ---------------------------------------------------------------- evdev 触摸
struct MtSlot { int32_t id = -1; int32_t x = 0, y = 0; };

struct InputDev {
    int fd = -1;
    int32_t xmin = 0, xmax = 0, ymin = 0, ymax = 0;
    MtSlot slots[10];
    int32_t cur_slot = 0;
    bool down = false;
    int pending = 0;      // +1 待按下, -1 待抬起, 0 无
    int32_t screen_x = 0, screen_y = 0;
    // 双指拖拽窗口
    bool dragging = false;
    bool drag_started = false;         // 过了起手阈值才算真的拖
    int32_t grab_off_x = 0, grab_off_y = 0;
    int32_t last_cx = 0, last_cy = 0;
    View *drag_view = nullptr;
    // 单指拖边角改尺寸
    bool resizing = false;
    View *resize_view = nullptr;
    int32_t resize_x0 = 0, resize_y0 = 0;   // 手指起点
    int32_t resize_w0 = 0, resize_h0 = 0;   // 窗口起始尺寸（逻辑像素）
    int32_t resize_left = 0, resize_top = 0; // 窗口起始位置
    int resize_corner = 0;                   // 1=右下 2=左下 3=右上 4=左上
};
static InputDev g_in;

static View *hit_test(int32_t sx, int32_t sy) {
    View *best = nullptr;
    int32_t bestz = -0x7fffffff;
    for (View *v : g_srv.views) {
        if (!v->mapped || v->is_cursor || v->is_subsurface || v->bw <= 0 || v->bh <= 0) continue;
        if (sx < v->x || sy < v->y || sx >= v->x + v->ew() || sy >= v->y + v->eh()) continue;
        int32_t zz = g_base_z + v->stack_key();
        if (!best || zz > bestz) { best = v; bestz = zz; }
    }
    return best;
}

// 只有「同一个客户端」的 pointer 才能收到针对某视图的 enter/motion/button，
// 否则客户端会收到别的客户端的 surface 对象 id → "invalid object" → 整个连接 Invalid argument 断开。
static bool ptr_same_client(wl_resource *p, View *v) {
    if (!p || !v || !v->surface) return false;
    return wl_resource_get_client(p) == wl_resource_get_client(v->surface);
}

