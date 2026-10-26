// touch.hpp
//
// 触摸状态机 + 手势 + handle_frame + /dev/input
//
// 从 sfwm.cpp 原样搬出（只搬不改，逻辑零差异）。
// 依赖包含顺序：本文件用到的东西必须由**前面的**模块提供，
// 因此它在 sfwm.cpp 里的 include 位置就是它原来在文件里的位置。
#pragma once

// ---------------------------------------------------------------- 触摸状态机
// 参考 Hyprland 的做法：owner 一旦确定，到抬起为止不再改变。
//   OWNER_CLIENT : 全部事件转给客户端（含多指 pinch）
//   OWNER_MOVE   : 长按 400ms 转过来的窗口移动（锚定 + 阈值 + 过程吸附）
//   OWNER_RESIZE : 起手落在角带里，改尺寸（对角固定）
//   OWNER_NONE   : 空闲
enum TouchOwner { OWNER_NONE = 0, OWNER_CLIENT, OWNER_MOVE, OWNER_RESIZE, OWNER_PINCH };

static TouchOwner g_owner = OWNER_NONE;
static int touch_owner_value(void) { return (int)g_owner; }
static View *g_gesture_view = nullptr;      // MOVE / RESIZE 的作用对象
static View *g_client_view = nullptr;       // CLIENT 模式的目标（次指 up 要用）
static int32_t g_press_x = 0, g_press_y = 0;
static bool g_move_started = false;
// 长按已布防、但还没真正拖动：抬起时仍然要把这次按压当成一次点击
static bool g_lp_pending_click = false;
static wl_event_source *g_lp_timer = nullptr;
static int32_t g_sec_id[10] = {0};           // 次指 → wl_touch id
static int32_t g_next_touch_id = 1;
static int32_t g_primary_slot = -1;
static int32_t g_prev_primary = -1;
static int32_t g_pinch_d0 = 0;               // 双指起始距离
static int32_t g_pinch_w0 = 0, g_pinch_h0 = 0;   // 起始逻辑尺寸
// 双指中点 + 起始窗口原点：用来做“以两指中点为中心缩放”
static int32_t g_pinch_mx = 0, g_pinch_my = 0;
static int32_t g_pinch_px = 0, g_pinch_py = 0;
// 手势开始时就把渲染倍率“冻结”：blit 里会自校准 buf_scale，
// 如果每帧都去读活值，锚定算式算出的原点会在两个值之间来回翻 —— 就是“瞬移”。
static int32_t g_pinch_sc = 1;
static float   g_pinch_zoom0 = 1.f;   // 手势开始时的 zoom，连续捏合不会跳

static void touch_ev(View *v, int32_t id, int32_t sx, int32_t sy, int kind) {
    if (!v || !v->surface) return;
    uint32_t serial = wl_display_next_serial(g_srv.dpy);
    uint32_t t = now_ms();
    for (wl_resource *tr : g_srv.touches) {
        if (!ptr_same_client(tr, v)) continue;
        if (kind == 0)
            wl_touch_send_down(tr, serial, t, v->surface, id,
                               wl_fixed_from_int(v->to_local_x(sx)),
                               wl_fixed_from_int(v->to_local_y(sy)));
        else if (kind == 1)
            wl_touch_send_motion(tr, t, id,
                                 wl_fixed_from_int(v->to_local_x(sx)),
                                 wl_fixed_from_int(v->to_local_y(sy)));
        else
            wl_touch_send_up(tr, serial, t, id);
        if (wl_resource_get_version(tr) >= 5) wl_touch_send_frame(tr);
    }
}

// 次指同步：第 2 根以后的每根手指各拿一个独立的 wl_touch id，
// 这样客户端的 pinch / 双指滚动才真的能用（以前合成器自己吃掉了）
static void sec_sync(View *v) {
    if (!v) return;
    // 主指换了（原来那根抬起来、另一根升格为主指）→ 旧的主指得先把它的次指 id 抬掉
    if (g_primary_slot >= 0 && g_primary_slot < 10 && g_sec_id[g_primary_slot]) {
        touch_ev(v, g_sec_id[g_primary_slot], 0, 0, 2);
        g_sec_id[g_primary_slot] = 0;
    }
    for (int i = 0; i < 10; i++) {
        if (i == g_primary_slot) continue;
        int32_t sx, sy;
        if (g_in.slots[i].id >= 0) {
            raw_to_screen(g_in.slots[i].x, g_in.slots[i].y, &sx, &sy);
            if (!g_sec_id[i]) {
                g_sec_id[i] = g_next_touch_id++;
                touch_ev(v, g_sec_id[i], sx, sy, 0);
            } else {
                touch_ev(v, g_sec_id[i], sx, sy, 1);
            }
        } else if (g_sec_id[i]) {
            touch_ev(v, g_sec_id[i], 0, 0, 2);
            g_sec_id[i] = 0;
        }
    }
}

static void sec_release_all() {
    View *v = g_client_view;
    for (int i = 0; i < 10; i++) {
        if (g_sec_id[i]) {
            if (v) touch_ev(v, g_sec_id[i], 0, 0, 2);
            g_sec_id[i] = 0;
        }
    }
}

// 长按 400ms：把已经发给客户端的那根手指“收回来”，转成窗口移动
static int on_longpress(void *) {
    if (g_lp_timer) wl_event_source_timer_update(g_lp_timer, 0);
    if (g_clickthrough || g_owner != OWNER_CLIENT) return 0;
    View *v = g_client_view ? g_client_view : hit_test(g_press_x, g_press_y);
    if (!v || v->is_subsurface || v->is_cursor) return 0;

    // ★ 这里**不能**给客户端发 up：那会把用户正在点的按钮/滑块直接吞掉。
    //   改成：按压保持住，进入“预备移动”。抬手没移动 -> 照常算点击；
    //   真的拖动超过阈值 -> 那时才发 up 取消点击（见 OWNER_MOVE 分支）。
    g_lp_pending_click = true;

    g_owner = OWNER_MOVE;
    g_gesture_view = v;
    g_move_started = false;
    g_in.grab_off_x = v->x - g_in.screen_x;
    g_in.grab_off_y = v->y - g_in.screen_y;
    g_in.last_cx = g_in.screen_x;
    g_in.last_cy = g_in.screen_y;
    raise_view(v);
    printf("[wm] 长按 -> 预备移动 '%s'（未拖动则仍算点击）\n", v->title[0] ? v->title : "view");
    return 0;
}

// 过程吸附：只改“将要应用的位置”，不改窗口本身
// 必须有滞回（hysteresis）：否则手指停在吸附边界上，窗口会在两个位置之间反复跳 —— 就是闪烁。
static void snap_pos(View *v, int32_t &px, int32_t &py) {
    if (!g_snap_on || g_snap_band_dp <= 0) return;
    // 窗口比屏幕还大时吸附毫无意义，只会把它拉开屏幕（“乱飞”的帮凶）
    if (!v || v->bw > g_out_w || v->bh > g_out_h) return;
    const int32_t BAND = g_snap_band_dp * (g_scale > 0 ? g_scale : 1);   // 进入阈值
    const int32_t REL  = BAND * 2;                            // 退出阈值（更大 = 滞回）
    const int32_t w    = v->bw;
    const int32_t m    = g_out_w / 2;

    if (g_snap_bits & 1) { if (px > REL) g_snap_bits &= ~1; else px = 0; }
    else if (g_snap_bits & 4) { if (px + w < g_out_w - REL) g_snap_bits &= ~4; else px = g_out_w - w; }
    else if (g_snap_bits & 2) { if (abs(px + w / 2 - m) > REL) g_snap_bits &= ~2; else px = m - w / 2; }
    else if (abs(px) < BAND) { g_snap_bits |= 1; px = 0; }
    else if (abs(px + w - g_out_w) < BAND) { g_snap_bits |= 4; px = g_out_w - w; }
    else if (abs(px + w / 2 - m) < BAND) { g_snap_bits |= 2; px = m - w / 2; }

    if (g_snap_bits & 8) { if (py > REL) g_snap_bits &= ~8; else py = 0; }
    else if (abs(py) < BAND) { g_snap_bits |= 8; py = 0; }
}

static void handle_frame() {
    // 点击穿透模式下 sfwm 完全不碰触摸：不然 Android 和合成器会同时收到同一根手指
    if (g_clickthrough) return;

    // 键盘弹着：落在键盘区域的手指全给安卓输入法（候选词条、按键都属于它），
    // 但只要已经有拖动/缩放在进行中，就让它做完，免得手势半途被冻住。
    if (g_ime_h > 0 && (g_owner == OWNER_NONE || g_owner == OWNER_CLIENT)) {
        for (int i = 0; i < 10; i++) {
            if (g_in.slots[i].id < 0) continue;
            int32_t bsx, bsy;
            raw_to_screen(g_in.slots[i].x, g_in.slots[i].y, &bsx, &bsy);
            if (bsy >= g_out_h - g_ime_h) return;
        }
    }
    g_in_gesture = (g_owner == OWNER_MOVE || g_owner == OWNER_RESIZE || g_owner == OWNER_PINCH);
    int nf = 0;
    int64_t sumx = 0, sumy = 0;
    int32_t fx = 0, fy = 0;
    g_primary_slot = -1;
    for (int i = 0; i < 10; i++) {
        if (g_in.slots[i].id < 0) continue;
        int32_t sx, sy;
        raw_to_screen(g_in.slots[i].x, g_in.slots[i].y, &sx, &sy);
        if (nf == 0) { fx = sx; fy = sy; g_primary_slot = i; }
        sumx += sx; sumy += sy;
        nf++;
    }

    // ---------- 全部抬起 ----------
    if (nf == 0) {
        if (g_lp_timer) wl_event_source_timer_update(g_lp_timer, 0);
        if (g_owner == OWNER_RESIZE) {
            View *v = g_gesture_view;
            printf("[wm] 拖边角结束 -> '%s' 逻辑尺寸 %dx%d\n",
                   v ? (v->title[0] ? v->title : "view") : "无",
                   v ? (v->bw / (v->buf_scale > 0 ? v->buf_scale : 1)) : 0,
                   v ? (v->bh / (v->buf_scale > 0 ? v->buf_scale : 1)) : 0);
        } else if (g_owner == OWNER_MOVE) {
            View *v = g_gesture_view;
            if (v && g_move_started)
                printf("[input] 移动结束 -> '%s' @ (%d,%d)\n",
                       v->title[0] ? v->title : "view", v->x, v->y);
        } else if (g_owner == OWNER_CLIENT) {
            sec_release_all();
        } else if (g_owner == OWNER_PINCH) {
            View *v = g_gesture_view;
            printf("[wm] 缩放结束 -> '%s' 逻辑尺寸 %dx%d\n",
                   v ? (v->title[0] ? v->title : "view") : "无",
                   v ? (v->bw / (v->buf_scale > 0 ? v->buf_scale : 1)) : 0,
                   v ? (v->bh / (v->buf_scale > 0 ? v->buf_scale : 1)) : 0);
        }
        // ── 手势结束才“收编”：操控过程中完全不干预（不重建图层、不夹位置、不发 configure）
        // 抬起就把光标藏起来：避免它空闲时压住下一次点击（见 pointer_motion 里的说明）
        cursor_hide();
        // ── 捏合结束：把合成器侧的 zoom 落实成客户端的新尺寸 ──────────────
        //   手势中 buffer 一直没变（放大是糊的）。这里只发**一次** configure，
        //   客户端按新尺寸重绘后糊就没了。zoom 先保留，等新 buffer 到达再归 1，
        //   否则会先“弹回”原尺寸再变大，很难看。
        if (g_gesture_view && g_owner == OWNER_PINCH && g_gesture_view->zoom != 1.f) {
            View *gv = g_gesture_view;
            const int32_t sc0 = client_scale(gv);
            // 目标尺寸夹到屏幕以内：configure_bounds 报的就是屏幕，
            // 超出的请求客户端直接无视（白跑一趟，还会重建 swapchain = 闪）。
            const int32_t maxw = g_out_w / sc0, maxh = g_out_h / sc0;
            int32_t fw = (int32_t)(gv->bw * gv->zoom) / sc0;
            int32_t fh = (int32_t)(gv->bh * gv->zoom) / sc0;
            if (fw > maxw) fw = maxw;
            if (fh > maxh) fh = maxh;
            if (fw < 120) fw = 120;
            if (fh < 120) fh = 120;
            const int32_t curw = gv->bw / (sc0 > 0 ? sc0 : 1);
            const int32_t curh = gv->bh / (sc0 > 0 ? sc0 : 1);
            const int32_t ddx = fw > curw ? fw - curw : curw - fw;
            const int32_t ddy = fh > curh ? fh - curh : curh - fh;
            if (ddx <= 4 && ddy <= 4) {
                // 已经是屏幕尺寸：这次 configure 什么也变不了，只会让客户端
                // 重建一次 swapchain（视觉上就是“松手时闪一下”）。
                // 保留 zoom 当放大镜，等下次捏小才会重新变清晰。
                printf("[wm] 缩放结束：已到屏幕上限，保持视觉缩放 %.3f（模糊属正常）\n", gv->zoom);
            } else {
                gv->req_w = fw;
                gv->req_h = fh;
                toplevel_send_configure(gv);
                printf("[wm] 缩放结束：buffer %dx%d zoom=%.3f -> 请求 %dx%d 逻辑\n",
                       gv->bw, gv->bh, gv->zoom, gv->req_w, gv->req_h);
            }
        }
        if (g_gesture_view && g_owner == OWNER_RESIZE) {
            View *gv = g_gesture_view;
            if (gv->bw > g_out_w || gv->bh > g_out_h) {
                // 用「实际 buffer ÷ 上次请求的逻辑尺寸」反推真实倍率
                //（Qt 经常不按 set_buffer_scale 来，直接用 buf_scale 会算错一圈）
                int32_t eff = gv->buf_scale > 0 ? gv->buf_scale : 1;
                if (gv->req_w > 0 && gv->bw > 0) {
                    int32_t e2 = (gv->bw + gv->req_w / 2) / gv->req_w;
                    if (e2 >= 1 && e2 <= 8) eff = e2;
                }
                gv->req_w = (g_out_w - 60) / eff;
                gv->req_h = (g_out_h - 60) / eff;
                if (gv->req_w < 200) gv->req_w = 200;
                if (gv->req_h < 200) gv->req_h = 200;
                toplevel_send_configure(gv);
                printf("[wm] 手势结束收编：%dx%d 超出屏幕 -> 请求 %dx%d 逻辑（实测倍率 %d）\n",
                       gv->bw, gv->bh, gv->req_w, gv->req_h, eff);
            }
        }
        // ★ 手势结束后必须把窗口拉回屏幕：否则捏合/拖拽能把它推到屏幕外，
        //   图层还在渲染（SurfaceFlinger 不裁），但 hit_test 永远命中不到 → "点不动"。
        if (g_gesture_view && g_gesture_view->mapped) {
            View *v = g_gesture_view;
            const int32_t ox = v->x, oy = v->y;
            v->clamp_on_screen();
            if (ox != v->x || oy != v->y)
                printf("[wm] 手势结束拉回屏幕: (%d,%d)->(%d,%d) buf=%dx%d\n",
                       ox, oy, v->x, v->y, v->bw, v->bh);
        }

        // 捏合/拖角结束时补发一次最终尺寸（过程中被限流跳过的那些帧最后都要落地）
        g_in_gesture = false;
        if (g_cfg_min_interval != 120) g_cfg_min_interval = 120;
        if (g_gesture_view && g_gesture_view->mapped && g_gesture_view->req_w > 0)
            toplevel_send_configure(g_gesture_view);

        if (g_in.down) { g_in.down = false; input_button(false); }
        g_lp_pending_click = false;
        g_owner = OWNER_NONE;
        g_gesture_view = nullptr;
        g_client_view = nullptr;
        g_move_started = false;
        g_snap_bits = 0;
        g_in_drag = false;
        g_in.pending = 0;
        g_arm_corner = 0;
        return;
    }

    // ---------- 客户端想让某个窗口跟着走（xdg_toplevel.move）----------
    if (g_srv.move_view && g_owner != OWNER_MOVE && g_owner != OWNER_RESIZE) {
        View *v = g_srv.move_view;
        g_srv.move_view = nullptr;
        if (g_owner == OWNER_CLIENT && g_in.down) { g_in.down = false; input_button(false); }
        sec_release_all();
        g_owner = OWNER_MOVE;
        g_gesture_view = v;
        g_move_started = false;
        g_snap_bits = 0;
        g_in_drag = true;
        g_in.grab_off_x = v->x - fx;
        g_in.grab_off_y = v->y - fy;
        g_in.last_cx = fx; g_in.last_cy = fy;
        raise_view(v);
        printf("[wm] 客户端请求移动 '%s'\n", v->title[0] ? v->title : "view");
    }

    g_in.screen_x = fx;
    g_in.screen_y = fy;

    // ---------- owner: PINCH（双指改窗口大小）----------
    if (g_owner == OWNER_PINCH) {
        View *v = g_gesture_view;
        g_in.pending = 0;
        if (!v || !v->mapped) { g_owner = OWNER_NONE; g_gesture_view = nullptr; return; }
        if (nf < 2) return;                       // 松到只剩一根：等全抬起才结束
        int32_t ax = 0, ay = 0, bx = 0, by = 0, got = 0;
        for (int i = 0; i < 10 && got < 2; i++) {
            if (g_in.slots[i].id < 0) continue;
            int32_t sx, sy;
            raw_to_screen(g_in.slots[i].x, g_in.slots[i].y, &sx, &sy);
            if (got == 0) { ax = sx; ay = sy; got = 1; } else { bx = sx; by = sy; got = 2; }
        }
        if (got < 2) return;
        int32_t ddx = bx - ax, ddy = by - ay;
        int32_t d = (int32_t)sqrt((double)(ddx * ddx + ddy * ddy));
        double scale = g_pinch_d0 > 0 ? (double)d / (double)g_pinch_d0 : 1.0;
        if (scale < 0.25) scale = 0.25;
        if (scale > 4.0) scale = 4.0;
        // ★ 手势期间**完全不动客户端**：只记 zoom，图层 dst 尺寸跟着变。
        //   没有 configure → 客户端不重建 swapchain → 不闪、不抖、不打架。
        //   代价是放大时糊（原 buffer 分辨率不够），抬手后发一次 configure 就清晰了。
        float nz = g_pinch_zoom0 * (float)scale;
        if (nz < 0.1f) nz = 0.1f;
        // 允许放大到超过屏幕：那是“放大镜”效果（content 会糊，但至少看得见、点得准）。
        //   真正的上限交给手势结束时处理 —— 那里只在尺寸确实会变时才发 configure。
        if (nz > 4.0f) nz = 4.0f;
        // 以两指中点为中心：手指下面那块内容始终留在手指下面
        v->x = g_pinch_mx - (int32_t)((g_pinch_mx - g_pinch_px) * scale);
        v->y = g_pinch_my - (int32_t)((g_pinch_my - g_pinch_py) * scale);
        v->set_zoom(nz);      // 父层 + 所有子表面一起缩放
        {
            static int dbgn = 0;
            if ((dbgn++ & 15) == 0)
                printf("[wm] 双指缩放 zoom=%.3f (指距 %d->%d)\n", scale, g_pinch_d0, d);
        }
        return;
    }

    // ---------- owner: RESIZE ----------
    if (g_owner == OWNER_RESIZE) {
        View *v = g_gesture_view;
        if (v && v->mapped) {
            const int32_t sc = v->buf_scale > 0 ? v->buf_scale : 1;
            int32_t dx = fx - g_in.resize_x0, dy = fy - g_in.resize_y0;
            int32_t w = g_in.resize_w0, h = g_in.resize_h0;
            int32_t nx = g_in.resize_left, ny = g_in.resize_top;
            const int c = g_in.resize_corner;
            // 逐轴判断：纯左边/右边只改宽，纯上边/下边只改高（以前用 if/else 判，
            // 边缘带根本不会被识别，所以只能拖四个角）
            const bool eR = (c == 1 || c == 3 || c == 5);
            const bool eL = (c == 2 || c == 4 || c == 6);
            const bool eB = (c == 1 || c == 2 || c == 7);
            const bool eT = (c == 3 || c == 4 || c == 8);
            if (eR) w += dx;
            else if (eL) { w -= dx; nx += dx; }
            if (eB) h += dy;
            else if (eT) { h -= dy; ny += dy; }
            const int32_t minp = 120 * sc;
            if (w < minp) { if (eL) nx = g_in.resize_left + g_in.resize_w0 - minp; w = minp; }
            if (h < minp) { if (eT) ny = g_in.resize_top + g_in.resize_h0 - minp; h = minp; }
            // 尺寸不设上限：桌面隐喻里窗口本来就允许比屏幕大，用户自己能缩回去。
            // 之前这里和 blit 里的“夹回屏幕”一起把位置硬改了，反而显得“乱飞”。
            if (nx != v->x || ny != v->y) { v->x = nx; v->y = ny; v->place(); }
            int32_t lw = w / sc, lh = h / sc;
            if (lw != v->req_w || lh != v->req_h) {
                v->req_w = lw;
                v->req_h = lh;
                maybe_configure(v);
                printf("[wm] 拖边角 -> 请求 %dx%d 逻辑像素\n", lw, lh);
            }
        }
        g_in.pending = 0;
        return;
    }

    // ---------- owner: MOVE ----------
    if (g_owner == OWNER_MOVE) {
        g_reason = "move";
        View *v = g_gesture_view;
        if (v) {
            // ★ 阈值 24dp（以前是 8dp）。触屏手指抖动就有 1–2mm（scale=2 时 8dp 只有 16 物理像素），
            //   于是每次“轻点小按钮”都会越过阈值 → 进入 MOVE → 窗口跟着手指跑
            //   → 按钮从手指底下溜走（“点不了”），同时每帧重新 place 造成闪烁。
            const int32_t TH = 24 * (g_scale > 0 ? g_scale : 1);
            if (!g_move_started) {
                int32_t ddx = fx - g_in.last_cx, ddy = fy - g_in.last_cy;
                if (ddx * ddx + ddy * ddy < TH * TH) { g_in.pending = 0; return; }
                g_move_started = true;
                // 这一刻才算真的拖窗口：撤销那根手指的按压（点击作废），
                // 并用“当前手指位置”重算锚点 —— 用长按时刻的旧锚点会让窗口瞬移。
                if (g_lp_pending_click) {
                    g_lp_pending_click = false;
                    if (g_in.down) { g_in.down = false; input_button(false); }
                    printf("[wm] 长按后拖动 -> 取消点击，开始移动窗口\n");
                }
                g_in.grab_off_x = v->x - fx;
                g_in.grab_off_y = v->y - fy;
            }
            int32_t nx = fx + g_in.grab_off_x, ny = fy + g_in.grab_off_y;
            const int32_t keep = 60 * (g_scale > 0 ? g_scale : 1);
            if (nx > g_out_w - keep) nx = g_out_w - keep;
            if (ny > g_out_h - keep) ny = g_out_h - keep;
            if (nx < -(v->bw - keep)) nx = -(v->bw - keep);
            if (ny < 0) ny = 0;
            snap_pos(v, nx, ny);      // 过程吸附：每帧都对齐
            if (nx != v->x || ny != v->y) {
                v->x = nx; v->y = ny;
                v->layer.move(nx, ny);           // 只发 setPosition
                for (View *c : v->subs) if (c->mapped) c->place();
                static int mvdbg = 0;
                if ((mvdbg++ % 20) == 0) printf("[move] %d,%d (finger %d,%d)\n", nx, ny, fx, fy);
            }
        }
        g_in.pending = 0;
        return;
    }

    // ---------- 起手：决定 owner ----------
    if (g_owner == OWNER_NONE && g_in.pending > 0) {
        {   // 跟踪：这一帧要按下了，把命中结果和窗口几何一起记下来
            View *thv = hit_test(fx, fy);
            trace("I press screen=(%d,%d) hit=%s %p rect=(%d,%d %dx%d) nf=%d owner=%d snap=%d",
                  fx, fy,
                  thv ? (thv->title[0] ? thv->title : "view") : "(none)", (void *)thv,
                  thv ? thv->x : 0, thv ? thv->y : 0, thv ? thv->bw : 0, thv ? thv->bh : 0,
                  nf, (int)g_owner, (int)g_snap_on);
        }
        g_in.pending = 0;
        // 坐标标定用：原始值 vs 换算后的屏幕值
        printf("[tap] raw=(%d,%d) -> screen=(%d,%d)  raw范围 x[%d,%d] y[%d,%d]\n",
               g_in.slots[g_primary_slot].x, g_in.slots[g_primary_slot].y, fx, fy,
               g_in.xmin, g_in.xmax, g_in.ymin, g_in.ymax);
        View *hv = hit_test(fx, fy);
        int corner = corner_hit(hv, fx, fy);
        // 角带*不再*一按就 resize：先布防，移动超阈值才真开始拖。
        // 否则窗口底部每 96px 内都是“拖拽区”，那里的按钮永远点不动。
        g_arm_corner = 0;
        if (corner) {
            g_arm_corner = corner;
            g_arm_x = fx;
            g_arm_y = fy;
            printf("[wm] 角带布防 corner=%d '%s' (%dx%d)，移动超 %dpx 才算拖拽\n",
                   corner, hv->title[0] ? hv->title : "view", hv->bw, hv->bh,
                   24 * (g_scale > 0 ? g_scale : 1));
        }
        g_owner = OWNER_CLIENT;
        g_client_view = hv;
        g_press_x = fx; g_press_y = fy;
        g_prev_primary = g_primary_slot;

        // 顶部标题条：直接拖窗口（传统桌面隐喻），内容区完全还给客户端
        if (hv && !hv->is_subsurface && !hv->is_cursor && in_title(hv, fx, fy)) {
            g_owner = OWNER_MOVE;
            g_gesture_view = hv;
            g_move_started = false;
            g_snap_bits = 0;
            g_in_drag = true;
            g_in.grab_off_x = hv->x - fx;
            g_in.grab_off_y = hv->y - fy;
            g_in.last_cx = fx; g_in.last_cy = fy;
            raise_view(hv);
            kb_set_focus(hv);
            printf("[wm] 拖标题条 -> 移动 '%s'\n", hv->title[0] ? hv->title : "view");
            return;
        }

        g_in.down = true;
        input_button(true);
        // 布防长按：按住 650ms 不动 -> 进入“预备移动”。
        // 原来 400ms 太短：小按钮要按稳，很容易在点中之前就变成移动窗口。
        if (g_lp_timer && !g_arm_corner) wl_event_source_timer_update(g_lp_timer, 650);
        return;
    }

    // ---------- owner: CLIENT ----------
    g_in.pending = 0;

    // 手指已经开始移动 -> 撤销长按，否则 400ms 后会把正在进行的拖拽/滚动劫持成移动窗口
    if (g_lp_timer && (abs(fx - g_press_x) > 8 * (g_scale > 0 ? g_scale : 1) ||
                       abs(fy - g_press_y) > 8 * (g_scale > 0 ? g_scale : 1)))
        wl_event_source_timer_update(g_lp_timer, 0);

    // 角带已布防：手指移动超过阈值 -> 把刚才发给客户端的按压撤掉，转成 resize
    if (g_arm_corner && g_client_view && g_client_view->mapped) {
        View *v = g_client_view;
        const int32_t thr = 24 * (g_scale > 0 ? g_scale : 1);
        const int32_t adx = fx - g_arm_x, ady = fy - g_arm_y;
        if (adx * adx + ady * ady >= thr * thr) {
            if (g_in.down) { g_in.down = false; input_button(false); }  // 撤销点击
            g_owner = OWNER_RESIZE;
            g_gesture_view = v;
            g_in.resize_corner = g_arm_corner;
            g_in.resize_x0 = g_arm_x;  g_in.resize_y0 = g_arm_y;   // ← 手指起点（删了它就会乱飞）
            g_in.resize_w0 = v->bw;    g_in.resize_h0 = v->bh;
            g_in.resize_left = v->x;   g_in.resize_top = v->y;
            g_snap_bits = 0;           // 缩放期间不吸附
            printf("[wm] 角带拖拽开始 -> resize '%s' corner=%d\n",
                   v->title[0] ? v->title : "view", g_arm_corner);
            g_arm_corner = 0;
            return;
        }
    }

    // 双指进来 → 转成「改窗口大小」（传统桌面隐喻：捏窗口 = 调整它，不是给客户端）
    if (nf >= 2 && g_owner == OWNER_CLIENT && g_client_view && g_client_view->mapped) {
        View *v = g_client_view;
        int32_t ax = 0, ay = 0, bx = 0, by = 0, got = 0;
        for (int i = 0; i < 10 && got < 2; i++) {
            if (g_in.slots[i].id < 0) continue;
            int32_t sx, sy;
            raw_to_screen(g_in.slots[i].x, g_in.slots[i].y, &sx, &sy);
            if (got == 0) { ax = sx; ay = sy; got = 1; }
            else { bx = sx; by = sy; got = 2; }
        }
        int32_t dx = bx - ax, dy = by - ay;
        int32_t d0 = (int32_t)sqrt((double)(dx * dx + dy * dy));
        if (d0 > 40) {
            if (g_in.down) { g_in.down = false; input_button(false); }
            sec_release_all();
            g_owner = OWNER_PINCH;
        g_cfg_min_interval = 250;   // 捏合期间别每帧 reconfigure，否则整屏闪
            g_gesture_view = v;
            g_pinch_d0 = d0;
            g_snap_bits = 0;      // 缩放期间不吸附，否则两者互相拉扯 = 乱飞
            // 记**逻辑**尺寸做基准（物理 / buf_scale），否则 scale=3 下会被放大 3 倍
            const int32_t psc = v->buf_scale > 0 ? v->buf_scale : 1;
            g_pinch_sc = psc;                     // 冻结，整个手势期间都用它
            // 基准用「当前视觉尺寸」（含还没落实的 zoom）：
            //   用 req_w 会在“上一轮 configure 客户端没照做”时算错基准 → 一捏就跳。
            g_pinch_zoom0 = v->zoom > 0.f ? v->zoom : 1.f;
            g_pinch_w0 = (int32_t)(v->bw * g_pinch_zoom0) / psc;
            g_pinch_h0 = (int32_t)(v->bh * g_pinch_zoom0) / psc;
            // 记下两指中点与窗口原点：缩放时让中点下的内容留在原地
            g_pinch_mx = (ax + bx) / 2;
            g_pinch_my = (ay + by) / 2;
            g_pinch_px = v->x;
            g_pinch_py = v->y;
            printf("[wm] 双指 -> 缩放 '%s' 起始 %dx%d 逻辑 指距 %d\n",
                   v->title[0] ? v->title : "view", g_pinch_w0, g_pinch_h0, d0);
            return;
        }
    }

    if (nf >= 2 && g_lp_timer) wl_event_source_timer_update(g_lp_timer, 0);   // 双指 = 一定是客户端
    // 主指换了人：把 id 0 这根“逻辑主指”先抬起再重新按下，客户端的单指状态才不会卡住
    if (g_in.down && g_prev_primary >= 0 && g_primary_slot != g_prev_primary) {
        g_in.down = false; input_button(false);
        g_in.down = true;  input_button(true);
    }
    g_prev_primary = g_primary_slot;

    pointer_motion(fx, fy, g_in.down);
    if (g_in.down) input_touch_motion();
    sec_sync(g_client_view ? g_client_view : g_srv.pointer_focus);
}


static int on_input_fd(int fd, uint32_t, void *) {
    struct input_event ev[64];
    int n = (int)read(fd, ev, sizeof(ev));
    if (n <= 0) return 0;
    n /= (int)sizeof(struct input_event);

    for (int i = 0; i < n; i++) {
        if (ev[i].type == EV_ABS) {
            if (ev[i].code == ABS_MT_SLOT) {
                int32_t s = ev[i].value;
                g_in.cur_slot = (s < 0) ? 0 : (s > 9 ? 9 : s);
            } else if (ev[i].code == ABS_MT_TRACKING_ID) {
                g_in.slots[g_in.cur_slot].id = ev[i].value;
                g_in.pending = (ev[i].value < 0) ? -1 : 1;
            } else if (ev[i].code == ABS_MT_POSITION_X) {
                g_in.slots[g_in.cur_slot].x = ev[i].value;
            } else if (ev[i].code == ABS_MT_POSITION_Y) {
                g_in.slots[g_in.cur_slot].y = ev[i].value;
            }
        } else if (ev[i].type == EV_SYN && ev[i].code == SYN_REPORT) {
            handle_frame();
        }
    }
    return 0;
}

static int open_touch_device(bool grab) {
    for (int i = 0; i < 32; i++) {
        char path[64], name[96] = {0};
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;

        unsigned long abits[(ABS_MAX / (8 * sizeof(unsigned long))) + 2] = {0};
        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abits)), abits) < 0) { close(fd); continue; }
        auto has = [&](int code) {
            return (abits[code / (8 * sizeof(unsigned long))] >> (code % (8 * sizeof(unsigned long)))) & 1ul;
        };
        if (!has(ABS_MT_POSITION_X) || !has(ABS_MT_POSITION_Y)) { close(fd); continue; }

        struct input_absinfo ai;
        if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ai) < 0) { close(fd); continue; }
        g_in.xmin = ai.minimum; g_in.xmax = ai.maximum;
        if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ai) < 0) { close(fd); continue; }
        g_in.ymin = ai.minimum; g_in.ymax = ai.maximum;
        for (int k = 0; k < 10; k++) {
            g_in.slots[k].id = -1;
            g_in.slots[k].x = g_in.xmin;
            g_in.slots[k].y = g_in.ymin;
        }
        ioctl(fd, EVIOCGNAME(sizeof name), name);

        if (grab && ioctl(fd, EVIOCGRAB, 1) == 0) printf("[input] 已独占 (EVIOCGRAB)\n");
        g_in.fd = fd;
        printf("[input] %s '%s' x[%d,%d] y[%d,%d]\n", path, name,
               g_in.xmin, g_in.xmax, g_in.ymin, g_in.ymax);
        return fd;
    }
    return -1;
}




