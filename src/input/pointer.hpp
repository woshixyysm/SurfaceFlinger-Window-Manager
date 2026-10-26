// pointer.hpp
//
// 指针/鼠标按钮/命中测试/抬升
//
// 从 sfwm.cpp 原样搬出（只搬不改，逻辑零差异）。
// 依赖包含顺序：本文件用到的东西必须由**前面的**模块提供，
// 因此它在 sfwm.cpp 里的 include 位置就是它原来在文件里的位置。
#pragma once

// ---- wl_keyboard 资源
static void kb_destroyed(wl_resource *r) {
    for (size_t i = 0; i < g_srv.keyboards.size(); i++)
        if (g_srv.keyboards[i] == r) { g_srv.keyboards.erase(g_srv.keyboards.begin() + i); break; }
}
static void kb_release(wl_client *, wl_resource *r) {
    kb_destroyed(r);
    wl_resource_destroy(r);
}
static const struct wl_keyboard_interface keyboard_impl = {
    .release = kb_release,
};

static void seat_get_keyboard(wl_client *c, wl_resource *seat, uint32_t id) {
    uint32_t ver = wl_resource_get_version(seat);
    wl_resource *r = wl_resource_create(c, &wl_keyboard_interface, ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &keyboard_impl, nullptr, kb_destroyed);
    g_srv.keyboards.push_back(r);
    kb_send_keymap(r);
    if (ver >= 4) wl_keyboard_send_repeat_info(r, 25, 600);
    // 已经有焦点就补一个 enter，否则客户端永远收不到按键
    View *v = g_srv.kb_focus ? g_srv.kb_focus : front_view();
    if (v) {
        if (!g_srv.kb_focus) g_srv.kb_focus = v;
        kb_send_enter_to(r, v);
    }
    printf("[kbd] 新客户端拿到键盘 (v%u)\n", ver);
}

static void pointer_motion(int32_t sx, int32_t sy, bool send) {
    g_srv.pointer_x = sx;
    g_srv.pointer_y = sy;
    g_cur_x = sx;
    g_cur_y = sy;
    // 光标层跟着触点走：客户端自己给了 cursor surface 就画它的，否则画内置的
    if (g_srv.cursor_view && g_srv.cursor_view->layer.win) {
        g_srv.cursor_view->x = sx - g_srv.cursor_hot_x;
        g_srv.cursor_view->y = sy - g_srv.cursor_hot_y;
        g_srv.cursor_view->place();
        cursor_hide();
    } else {
        // 只在手指按下期间画光标。
        // 原因：SurfaceFlinger 不允许非系统 uid 的图层改 touchOcclusionMode
        // （实测 setTrustedOverlay/setDropInputMode 调用成功，但 dumpsys input
        // 依旧是 touchOcclusionMode=BLOCK_UNTRUSTED）→ 光标那块 72x72 像素
        // 会吞掉落在它上面的下一次点击。把“空闲时不可见”就能彻底避开。
        if (g_in.down) cursor_present();
    }
    if (g_srv.pointers.empty()) return;
    View *v = hit_test(sx, sy);
    // 隐式指针 grab：按住期间锁定按下时的窗口。
    // 否则手指滑出窗口就会发 leave，客户端把 button 一丢，后续点击全失效。
    if (g_in.down && g_btn_view && g_btn_view->mapped) v = g_btn_view;
    uint32_t serial = wl_display_next_serial(g_srv.dpy);
    uint32_t t = now_ms();

    if (v != g_srv.pointer_focus) {
        printf("[input] 焦点切换: %s -> %s\n",
               g_srv.pointer_focus ? (g_srv.pointer_focus->title[0] ? g_srv.pointer_focus->title : "view") : "(无)",
               v ? (v->title[0] ? v->title : "view") : "(无)");
        if (g_srv.pointer_focus && g_srv.pointer_focus->surface) {
            for (wl_resource *p : g_srv.pointers)
                if (ptr_same_client(p, g_srv.pointer_focus))
                    wl_pointer_send_leave(p, serial, g_srv.pointer_focus->surface);
        }
        g_srv.pointer_focus = v;
        if (v && v->surface) {
            for (wl_resource *p : g_srv.pointers) {
                if (!ptr_same_client(p, v)) continue;
                // wl_fixed_t 是 24.8 定点数；且 surface 局部坐标是逻辑像素（= 物理 / buffer_scale）
                wl_pointer_send_enter(p, serial, v->surface,
                                      wl_fixed_from_int(v->to_local_x(sx)),
                                      wl_fixed_from_int(v->to_local_y(sy)));
                if (wl_resource_get_version(p) >= 5) wl_pointer_send_frame(p);
            }
        }
    } else if (v && send) {
        for (wl_resource *p : g_srv.pointers) {
            if (!ptr_same_client(p, v)) continue;
            wl_pointer_send_motion(p, t,
                                   wl_fixed_from_int(v->to_local_x(sx)),
                                   wl_fixed_from_int(v->to_local_y(sy)));
            if (wl_resource_get_version(p) >= 5) wl_pointer_send_frame(p);
        }
    }
}

// 合成鼠标：给注入器用（手机上不想伸手点时，也能驱动任何客户端）
static void mouse_button(int32_t x, int32_t y, int pressed) {
    pointer_motion(x, y, true);
    View *v = g_srv.pointer_focus;
    if (!v) { printf("[mouse] (%d,%d) 处没有窗口\n", x, y); return; }
    if (pressed) kb_set_focus(v);
    uint32_t serial = wl_display_next_serial(g_srv.dpy);
    // Qt 这类客户端要先拿到一次 motion（确认光标位置）才认后续的 button
    for (wl_resource *p : g_srv.pointers) {
        if (!ptr_same_client(p, v)) continue;
        wl_pointer_send_motion(p, now_ms(),
                               wl_fixed_from_int(v->to_local_x(x)),
                               wl_fixed_from_int(v->to_local_y(y)));
        if (wl_resource_get_version(p) >= 5) wl_pointer_send_frame(p);
    }
    for (wl_resource *p : g_srv.pointers) {
        if (!ptr_same_client(p, v)) continue;
        wl_pointer_send_button(p, serial, now_ms(), BTN_LEFT, pressed ? 1 : 0);
        if (wl_resource_get_version(p) >= 5) wl_pointer_send_frame(p);
    }
    int nmatched = 0;
    for (wl_resource *p : g_srv.pointers) if (ptr_same_client(p, v)) nmatched++;
    printf("[mouse] %s (%d,%d) -> %s local=(%d,%d) pointers=%zu matched=%d\n", pressed ? "按下" : "抬起", x, y,
           v->title[0] ? v->title : "view", v->to_local_x(x), v->to_local_y(y),
           g_srv.pointers.size(), nmatched);
}

static void mouse_click(int32_t x, int32_t y) {
    mouse_button(x, y, 1);
    mouse_button(x, y, 0);
}

// 抢占：把点中的窗口提到所有窗口之上。
// 用“当前最大 z + 1”而不是 ++next_z，避免反复点击后 z 无限膨胀。
static void raise_view(View *v) {
    // 弹窗（xdg_popup）也有 parent，但没有父亲图层可依附，必须允许它自己提升
    if (!v || v->is_cursor || (v->parent && !v->is_popup)) return;
    // 整屏窗口是「底图」，不参与点击提升，否则点一下就把桌面盖成黑板
    if (v->bw * 100 >= g_out_w * 95 && v->bh * 100 >= g_out_h * 95) {
        printf("[wm] '%s' 是底图，不参与提升\n",
               v->title[0] ? v->title : (v->app_id[0] ? v->app_id : "view"));
        return;
    }
    int32_t top = -1;
    for (View *o : g_srv.views)
        if (o->mapped && !o->is_cursor && !o->parent && o->z > top) top = o->z;
    if (v->z >= top) return;
    v->z = top + 1;
    v->place();
    printf("[wm] 抢占 -> '%s' 到最前 z=%d\n",
           v->title[0] ? v->title : (v->app_id[0] ? v->app_id : "view"), v->z);
}

// 手指是否落在“标题栏”条上（顶部 title_h 像素）：这里是合成器管辖，可以拖窗口
static int32_t title_h(View *v) {
    const int32_t s = v && v->buf_scale > 0 ? v->buf_scale : 1;
    int32_t h = 44 * s;
    // 至少要盖满角带（32*g_scale），否则顶部那一小条会落进“只能缩上边”的区里，
    // 表现就是“想拖窗口，结果一拖就变高/变矮”。
    const int32_t band = 32 * (g_scale > 0 ? g_scale : 1);
    if (h < band) h = band;
    if (v && h > v->bh / 3) h = v->bh / 3;
    return h;
}
static bool in_title(View *v, int32_t sx, int32_t sy) {
    if (!v) return false;
    return sy >= v->y && sy < v->y + title_h(v);
}

// 手指是否落在窗口的缩放区：
//   1=右下 2=左下 3=右上 4=左上 5=右边 6=左边 7=下边，0=不是
// 以前只认四个角，所以「拖左/右边框改宽度」完全没反应。
// 上边（原 8）故意不做：手机上顶部是用来拖窗口的，再做缩放会互相抢，
// 结果就是“想移窗口却变成放大”（这个 bug 已经踩过）。
static int corner_hit(View *v, int32_t sx, int32_t sy) {
    if (!v || v->bw <= 0 || v->bh <= 0) return 0;
    int32_t m = 32 * (g_scale > 0 ? g_scale : 1);
    if (m > v->bw / 3) m = v->bw / 3;
    if (m > v->bh / 3) m = v->bh / 3;
    if (m < 12) m = 12;
    // 顶部整条留给“移动”，不参与缩放
    if (sy <= v->y + m) return 0;
    bool nearR = sx >= v->x + v->bw - m, nearL = sx <= v->x + m;
    bool nearB = sy >= v->y + v->bh - m;
    if (nearR && nearB) return 1;
    if (nearL && nearB) return 2;
    if (nearR) return 5;
    if (nearL) return 6;
    if (nearB) return 7;
    return 0;
}

// 这个 view 背后的客户端是不是「正开着输入法」（有输入框拿到焦点）
static bool ti_enabled_for(View *v) {
    if (!v || !v->surface) return false;
    wl_client *c = wl_resource_get_client(v->surface);
    for (wl_resource *r : g_srv.text_inputs) {
        TiState *st = ti_state(r);
        if (st && st->enabled && wl_resource_get_client(r) == c) return true;
    }
    return false;
}

static void input_button(bool pressed) {
    View *v = g_srv.pointer_focus;
    uint32_t serial = wl_display_next_serial(g_srv.dpy);
    uint32_t t = now_ms();
    if (pressed) {
        pointer_motion(g_in.screen_x, g_in.screen_y, true);   // 先确认焦点/位置
        v = g_srv.pointer_focus;
        if (v) {
            g_btn_view = v;           // 记住按下的窗口，UP 时必须还给它
            kb_set_focus(v);          // 点哪个窗口，键盘就给哪个
            raise_view(v);            // 抢占：点谁谁到最前
        }
        // 点到「已经启用输入法」的客户端就弹键盘（兜底：某些客户端不会再发 show_input_panel）
        if (!g_ime_on && ti_enabled_for(v)) ime_send(true);
    } else if (!v && g_btn_view) {
        // 抬起时 focus 已经丢了（手指滑出窗口）：仍然要把 release 补发出去
        v = g_btn_view;
    }
    if (!v) { g_btn_view = nullptr; return; }

    // wl_touch
    for (wl_resource *tr : g_srv.touches) {
        if (!ptr_same_client(tr, v)) continue;
        if (pressed)
            wl_touch_send_down(tr, serial, t, v->surface, 0,
                               wl_fixed_from_int((g_in.screen_x - v->x) / v->buf_scale),
                               wl_fixed_from_int((g_in.screen_y - v->y) / v->buf_scale));
        else
            wl_touch_send_up(tr, serial, t, 0);
        if (wl_resource_get_version(tr) >= 5) wl_touch_send_frame(tr);
    }
    // wl_pointer 按钮
    for (wl_resource *p : g_srv.pointers) {
        if (!ptr_same_client(p, v)) continue;
        wl_pointer_send_button(p, serial, t, BTN_LEFT, pressed ? 1 : 0);
        if (wl_resource_get_version(p) >= 5) wl_pointer_send_frame(p);
    }
    if (!pressed) g_btn_view = nullptr;
    printf("[input] %s screen=(%d,%d) surface=(%d,%d) scale=%d target=%s z=%d rect=(%d,%d %dx%d)\n",
           pressed ? "DOWN" : "UP",
           g_in.screen_x, g_in.screen_y,
           v->to_local_x(g_in.screen_x),
           v->to_local_y(g_in.screen_y),
           v->buf_scale,
           v->title[0] ? v->title : "view", v->z, v->x, v->y, v->bw, v->bh);
}

static void input_touch_motion() {
    View *v = g_srv.pointer_focus;
    if (!v) return;
    uint32_t t = now_ms();
    for (wl_resource *tr : g_srv.touches) {
        wl_touch_send_motion(tr, t, 0,
                            wl_fixed_from_int((g_in.screen_x - v->x) / v->buf_scale),
                            wl_fixed_from_int((g_in.screen_y - v->y) / v->buf_scale));
        if (wl_resource_get_version(tr) >= 5) wl_touch_send_frame(tr);
    }
}

static void raw_to_screen(int32_t rx, int32_t ry, int32_t *sx, int32_t *sy) {
    int32_t w = g_in.xmax - g_in.xmin, h = g_in.ymax - g_in.ymin;
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    int32_t x = (int32_t)((int64_t)(rx - g_in.xmin) * g_out_w / w);
    int32_t y = (int32_t)((int64_t)(ry - g_in.ymin) * g_out_h / h);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= g_out_w) x = g_out_w - 1;
    if (y >= g_out_h) y = g_out_h - 1;
    *sx = x; *sy = y;
}

// 拖边角/双指缩放时不要每帧都发 configure：
// 客户端每次都要重新算布局、重画、换 buffer —— 120Hz 发等于一直在闪。
// 规则：尺寸至少变 6 逻辑像素，或者距上次至少 60ms。
static uint32_t g_cfg_ms = 0;
static int32_t g_cfg_w = -1, g_cfg_h = -1;
// 捏合过程中客户端每次收到 configure 都要重建 swapchain → 发太勤就是整屏闪。
// 所以这里两道闸门：时间闸门（捏合中 250ms，其余 120ms）+ 变化闸门（<24 逻辑像素）。
// 老代码是「dw<6 && dh<6 && <60ms 才 return」，等于尺寸一变就每帧都发。
static uint32_t g_cfg_min_interval = 120;

static void maybe_configure(View *v) {
    const uint32_t now = now_ms();
    const int32_t  dw  = abs(v->req_w - g_cfg_w), dh = abs(v->req_h - g_cfg_h);
    if (g_cfg_w >= 0 && now - g_cfg_ms < g_cfg_min_interval) return;
    if (g_cfg_w >= 0 && dw < 24 && dh < 24 && now - g_cfg_ms < 400) return;
    g_cfg_ms = now;
    g_cfg_w  = v->req_w;
    g_cfg_h  = v->req_h;
    toplevel_send_configure(v);
}

// 松手吸附：贴近屏幕左右边/中线/顶部时对齐，避免每次都要手调到像素级
static void snap_view(View *v) {
    if (!v) return;
    const int32_t M = 40 * (g_scale > 0 ? g_scale : 1);
    bool moved = false;
    int32_t w = v->bw;
    if (v->x > -M && v->x < M) { v->x = 0; moved = true; }
    else if (v->x + w > g_out_w - M && v->x + w < g_out_w + M) { v->x = g_out_w - w; moved = true; }
    else {
        int32_t c = v->x + w / 2, m = g_out_w / 2;
        if (c > m - M && c < m + M) { v->x = m - w / 2; moved = true; }
    }
    if (v->y > -M && v->y < M) { v->y = 0; moved = true; }
    if (moved) { v->place(); printf("[wm] 吸附 -> (%d,%d)\n", v->x, v->y); }
}

