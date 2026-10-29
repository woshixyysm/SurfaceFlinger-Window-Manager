// SPDX-License-Identifier: AGPL-3.0-or-later
// touch.hpp
//
// Touch state machine, gestures, frame handling and /dev/input.
//
// Extracted from the former single-file implementation; behaviour is unchanged.
// Depends on include order: everything used here is provided by the modules
// included before it in src/main.cpp.
#pragma once

// -------------------------------------------------------- touch state machine
// As in Hyprland, the owner is decided once and kept until release.
//   OWNER_CLIENT all events go to the client, including multi-finger pinch
//   OWNER_MOVE   window move after a 400 ms press, with anchoring and snapping
//   OWNER_RESIZE resize started in a corner band, with the opposite corner fixed
//   OWNER_NONE   idle
enum TouchOwner { OWNER_NONE = 0, OWNER_CLIENT, OWNER_MOVE, OWNER_RESIZE, OWNER_PINCH };

static TouchOwner g_owner = OWNER_NONE;
static int touch_owner_value(void) { return (int)g_owner; }
static View *g_gesture_view = nullptr;      // target of MOVE and RESIZE
static View *g_client_view = nullptr;       // target in CLIENT mode, needed for the secondary finger release
static int32_t g_press_x = 0, g_press_y = 0;
static bool g_move_started = false;
// The long press is armed but has not moved: the release still counts as a click.
static bool g_lp_pending_click = false;
static wl_event_source *g_lp_timer = nullptr;
static int32_t g_sec_id[10] = {0};           // secondary finger to wl_touch id
static int32_t g_next_touch_id = 1;
static int32_t g_primary_slot = -1;
static int32_t g_prev_primary = -1;
static int32_t g_pinch_d0 = 0;               // initial two-finger distance
static int32_t g_pinch_w0 = 0, g_pinch_h0 = 0;   // initial logical size
// Two-finger midpoint and initial window origin, so scaling keeps the midpoint.
static int32_t g_pinch_mx = 0, g_pinch_my = 0;
static int32_t g_pinch_px = 0, g_pinch_py = 0;
// The scale factor is frozen when the gesture starts: blit() calibrates buf_scale,
// and reading a live value makes the computed origin jump between two results.
static int32_t g_pinch_sc = 1;
static float   g_pinch_zoom0 = 1.f;   // zoom at gesture start, so successive pinches do not jump

static void idle_note_activity();   // ext-idle-notify, defined in idle_notify.hpp

static void touch_ev(View *v, int32_t id, int32_t sx, int32_t sy, int kind) {
    idle_note_activity();
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

// Every finger after the first gets its own wl_touch id, so pinch and two-finger
// scrolling work in clients instead of being consumed here.
static void sec_sync(View *v) {
    if (!v) return;
    // the primary finger changed, so release the old secondary id first
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

// After a 400 ms press the finger is taken back from the client and becomes a move.
static int on_longpress(void *) {
    if (g_lp_timer) wl_event_source_timer_update(g_lp_timer, 0);
    if (g_clickthrough || g_owner != OWNER_CLIENT) return 0;
    View *v = g_client_view ? g_client_view : hit_test(g_press_x, g_press_y);
    if (!v || v->is_subsurface || v->is_cursor) return 0;

    // No up event may be sent here: it would cancel the button or slider being pressed.
    // The press is kept and enters a pending move: releasing without motion is a click,
    // and moving past the threshold sends up and starts the move.
    g_lp_pending_click = true;

    g_owner = OWNER_MOVE;
    g_gesture_view = v;
    g_move_started = false;
    g_in.grab_off_x = v->x - g_in.screen_x;
    g_in.grab_off_y = v->y - g_in.screen_y;
    g_in.last_cx = g_in.screen_x;
    g_in.last_cy = g_in.screen_y;
    raise_view(v);
    printf("[wm] long press -> pending move '%s', still a click if it does not move\n", v->title[0] ? v->title : "view");
    return 0;
}

// Snapping during the gesture only changes the position about to be applied.
// Hysteresis is required, or a finger resting on the boundary makes the window jump.
static void snap_pos(View *v, int32_t &px, int32_t &py) {
    if (!g_snap_on || g_snap_band_dp <= 0) return;
    // Snapping is meaningless for a window larger than the output.
    if (!v || v->bw > g_out_w || v->bh > g_out_h) return;
    const int32_t BAND = g_snap_band_dp * (g_scale > 0 ? g_scale : 1);   // entry threshold
    const int32_t REL  = BAND * 2;                            // exit threshold, larger for hysteresis
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
    // In click-through mode the compositor ignores touches entirely, or Android and the
    if (g_clickthrough) return;

    // With the keyboard up, fingers over it belong to the input method, but a drag or
    // resize already in progress is allowed to finish.
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

    // ---------- all fingers released ----------
    if (nf == 0) {
        if (g_lp_timer) wl_event_source_timer_update(g_lp_timer, 0);
        if (g_owner == OWNER_RESIZE) {
            View *v = g_gesture_view;
            printf("[wm] resize finished -> '%s' logical size %dx%d\n",
                   v ? (v->title[0] ? v->title : "view") : "none",
                   v ? (v->bw / (v->buf_scale > 0 ? v->buf_scale : 1)) : 0,
                   v ? (v->bh / (v->buf_scale > 0 ? v->buf_scale : 1)) : 0);
        } else if (g_owner == OWNER_MOVE) {
            View *v = g_gesture_view;
            if (v && g_move_started)
                printf("[input] move finished -> '%s' at (%d,%d)\n",
                       v->title[0] ? v->title : "view", v->x, v->y);
        } else if (g_owner == OWNER_CLIENT) {
            sec_release_all();
        } else if (g_owner == OWNER_PINCH) {
            View *v = g_gesture_view;
            printf("[wm] zoom finished -> '%s' logical size %dx%d\n",
                   v ? (v->title[0] ? v->title : "view") : "none",
                   v ? (v->bw / (v->buf_scale > 0 ? v->buf_scale : 1)) : 0,
                   v ? (v->bh / (v->buf_scale > 0 ? v->buf_scale : 1)) : 0);
        }
        // The client is only updated when the gesture ends; during it nothing is touched.
        // Hide the cursor on release so it cannot cover the next click.
        cursor_hide();
        // Pinch end: turn the composer-side zoom into a new client size.
        // The buffer never changed during the gesture, so it looked blurry. A single
        // configure makes the client redraw at the new size. The zoom is kept until the
        // new buffer arrives, or the window would snap back before growing again.
        if (g_gesture_view && g_owner == OWNER_PINCH && g_gesture_view->zoom != 1.f) {
            View *gv = g_gesture_view;
            const int32_t sc0 = client_scale(gv);
            // Clamp the target to the output: configure_bounds reports the output size and
            // clients ignore larger requests, which only rebuilds their swapchain.
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
                // Already at the output size, so a configure would change nothing and only make
                // the client rebuild its swapchain.
                // The zoom stays as a magnifier until the next pinch out.
                printf("[wm] zoom end: output size reached, keeping visual zoom %.3f\n", gv->zoom);
            } else {
                gv->req_w = fw;
                gv->req_h = fh;
                toplevel_send_configure(gv);
                printf("[wm] zoom end: buffer %dx%d zoom=%.3f -> requesting %dx%d logical\n",
                       gv->bw, gv->bh, gv->zoom, gv->req_w, gv->req_h);
            }
        }
        if (g_gesture_view && g_owner == OWNER_RESIZE) {
            View *gv = g_gesture_view;
            if (gv->bw > g_out_w || gv->bh > g_out_h) {
                // Derive the real scale from the buffer size over the last requested size.
                // Qt often ignores set_buffer_scale, so buf_scale alone is misleading.
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
                printf("[wm] gesture end: %dx%d exceeds the output -> requesting %dx%d logical (measured scale %d)\n",
                       gv->bw, gv->bh, gv->req_w, gv->req_h, eff);
            }
        }
        // The window has to be pulled back after a gesture: the layer keeps rendering while
        // off screen, but hit testing never reaches it and it cannot be clicked.
        if (g_gesture_view && g_gesture_view->mapped) {
            View *v = g_gesture_view;
            const int32_t ox = v->x, oy = v->y;
            v->clamp_on_screen();
            if (ox != v->x || oy != v->y)
                printf("[wm] gesture end, pulled back: (%d,%d)->(%d,%d) buf=%dx%d\n",
                       ox, oy, v->x, v->y, v->bw, v->bh);
        }

        // Send the final size once at the end of a pinch or corner drag.
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

    // ---------- client-requested move (xdg_toplevel.move) ----------
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
        printf("[wm] client requested a move of '%s'\n", v->title[0] ? v->title : "view");
    }

    g_in.screen_x = fx;
    g_in.screen_y = fy;

    // ---------- owner PINCH: two fingers resize the window ----------
    if (g_owner == OWNER_PINCH) {
        View *v = g_gesture_view;
        g_in.pending = 0;
        if (!v || !v->mapped) { g_owner = OWNER_NONE; g_gesture_view = nullptr; return; }
        if (nf < 2) return;                       // one finger left; wait for a full release
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
        // The client is untouched during the gesture: only the zoom changes, and the layer
        // destination size follows it, so no configure is needed and nothing flickers.
        // The cost is blur while enlarged, which one configure at the end fixes.
        float nz = g_pinch_zoom0 * (float)scale;
        if (nz < 0.1f) nz = 0.1f;
        // Enlarging past the output is allowed: it acts as a magnifier, blurry but usable.
        // The real limit is handled at the end of the gesture.
        if (nz > 4.0f) nz = 4.0f;
        // Scale around the midpoint, so the content under the fingers stays put.
        v->x = g_pinch_mx - (int32_t)((g_pinch_mx - g_pinch_px) * scale);
        v->y = g_pinch_my - (int32_t)((g_pinch_my - g_pinch_py) * scale);
        v->set_zoom(nz);      // the parent and every subsurface scale together
        {
            static int dbgn = 0;
            if ((dbgn++ & 15) == 0)
                printf("[wm] pinch zoom=%.3f (distance %d to %d)\n", scale, g_pinch_d0, d);
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
            // Decide per axis: a pure left or right edge changes the width only, a pure top or
            // bottom edge the height only.
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
            // The size is not capped: a window may be larger than the output, as on a desktop.
            // Combined with the clamp in blit(), this used to move windows around visibly.
            if (nx != v->x || ny != v->y) { v->x = nx; v->y = ny; v->place(); }
            int32_t lw = w / sc, lh = h / sc;
            if (lw != v->req_w || lh != v->req_h) {
                v->req_w = lw;
                v->req_h = lh;
                maybe_configure(v);
                printf("[wm] corner drag -> requesting %dx%d logical pixels\n", lw, lh);
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
            // The threshold is 24 dp because a finger on a touchscreen wobbles by 1 to 2 mm,
            // so small buttons were always dragged and the window moved instead of the button
            // being pressed.
            const int32_t TH = 24 * (g_scale > 0 ? g_scale : 1);
            if (!g_move_started) {
                int32_t ddx = fx - g_in.last_cx, ddy = fy - g_in.last_cy;
                if (ddx * ddx + ddy * ddy < TH * TH) { g_in.pending = 0; return; }
                g_move_started = true;
                // Only now does the press become a drag: the click is cancelled and the anchor is
                // recomputed from the current position, or the window would jump.
                if (g_lp_pending_click) {
                    g_lp_pending_click = false;
                    if (g_in.down) { g_in.down = false; input_button(false); }
                    printf("[wm] long press then drag -> click cancelled, moving the window\n");
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
            snap_pos(v, nx, ny);      // snapping is applied every frame
            if (nx != v->x || ny != v->y) {
                v->x = nx; v->y = ny;
                v->layer.move(nx, ny);           // only setPosition is sent
                for (View *c : v->subs) if (c->mapped) c->place();
                static int mvdbg = 0;
                if ((mvdbg++ % 20) == 0) printf("[move] %d,%d (finger %d,%d)\n", nx, ny, fx, fy);
            }
        }
        g_in.pending = 0;
        return;
    }

    // ---------- start of a touch: choosing the owner ----------
    if (g_owner == OWNER_NONE && g_in.pending > 0) {
        {   // trace: record the hit test result and the window geometry
            View *thv = hit_test(fx, fy);
            trace("I press screen=(%d,%d) hit=%s %p rect=(%d,%d %dx%d) nf=%d owner=%d snap=%d",
                  fx, fy,
                  thv ? (thv->title[0] ? thv->title : "view") : "(none)", (void *)thv,
                  thv ? thv->x : 0, thv ? thv->y : 0, thv ? thv->bw : 0, thv ? thv->bh : 0,
                  nf, (int)g_owner, (int)g_snap_on);
        }
        g_in.pending = 0;
        // For calibration: raw values against converted screen values.
        printf("[tap] raw=(%d,%d) -> screen=(%d,%d) raw range x[%d,%d] y[%d,%d]\n",
               g_in.slots[g_primary_slot].x, g_in.slots[g_primary_slot].y, fx, fy,
               g_in.xmin, g_in.xmax, g_in.ymin, g_in.ymax);
        View *hv = hit_test(fx, fy);
        int corner = corner_hit(hv, fx, fy);
        // A corner press no longer resizes immediately: it is armed first and only starts
        // once the finger moves past the threshold.
        g_arm_corner = 0;
        if (corner) {
            g_arm_corner = corner;
            g_arm_x = fx;
            g_arm_y = fy;
            printf("[wm] corner armed %d '%s' (%dx%d), a drag needs %d px of movement\n",
                   corner, hv->title[0] ? hv->title : "view", hv->bw, hv->bh,
                   24 * (g_scale > 0 ? g_scale : 1));
        }
        g_owner = OWNER_CLIENT;
        g_client_view = hv;
        g_press_x = fx; g_press_y = fy;
        g_prev_primary = g_primary_slot;

        // The title strip moves the window; the content area belongs to the client.
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
            printf("[wm] title strip drag -> moving '%s'\n", hv->title[0] ? hv->title : "view");
            return;
        }

        g_in.down = true;
        input_button(true);
        // Arm the long press: 650 ms without motion enters a pending move.
        // The previous 400 ms was short enough to turn presses on small buttons into moves.
        if (g_lp_timer && !g_arm_corner) wl_event_source_timer_update(g_lp_timer, 650);
        return;
    }

    // ---------- owner: CLIENT ----------
    g_in.pending = 0;

    // Once the finger moves the long press is cancelled, or it would hijack a scroll.
    if (g_lp_timer && (abs(fx - g_press_x) > 8 * (g_scale > 0 ? g_scale : 1) ||
                       abs(fy - g_press_y) > 8 * (g_scale > 0 ? g_scale : 1)))
        wl_event_source_timer_update(g_lp_timer, 0);

    // The corner is armed and the finger passed the threshold: cancel the press and resize.
    if (g_arm_corner && g_client_view && g_client_view->mapped) {
        View *v = g_client_view;
        const int32_t thr = 24 * (g_scale > 0 ? g_scale : 1);
        const int32_t adx = fx - g_arm_x, ady = fy - g_arm_y;
        if (adx * adx + ady * ady >= thr * thr) {
            if (g_in.down) { g_in.down = false; input_button(false); }  // cancel the click
            g_owner = OWNER_RESIZE;
            g_gesture_view = v;
            g_in.resize_corner = g_arm_corner;
            g_in.resize_x0 = g_arm_x;  g_in.resize_y0 = g_arm_y;   // finger origin, required by the resize
            g_in.resize_w0 = v->bw;    g_in.resize_h0 = v->bh;
            g_in.resize_left = v->x;   g_in.resize_top = v->y;
            g_snap_bits = 0;           // no snapping while resizing
            printf("[wm] corner drag started -> resize '%s' corner=%d\n",
                   v->title[0] ? v->title : "view", g_arm_corner);
            g_arm_corner = 0;
            return;
        }
    }

    // Two fingers resize the window instead of going to the client.
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
        g_cfg_min_interval = 250;   // no configure per frame while pinching
            g_gesture_view = v;
            g_pinch_d0 = d0;
            g_snap_bits = 0;      // no snapping while resizing
            // Record the logical size as the base, or it is scaled three times over.
            const int32_t psc = v->buf_scale > 0 ? v->buf_scale : 1;
            g_pinch_sc = psc;                     // frozen for the whole gesture
            // The base is the current visual size, including a zoom not yet applied; the
            // requested size is wrong when the client ignored the last configure.
            g_pinch_zoom0 = v->zoom > 0.f ? v->zoom : 1.f;
            g_pinch_w0 = (int32_t)(v->bw * g_pinch_zoom0) / psc;
            g_pinch_h0 = (int32_t)(v->bh * g_pinch_zoom0) / psc;
            // Record the midpoint and the window origin so the midpoint stays fixed.
            g_pinch_mx = (ax + bx) / 2;
            g_pinch_my = (ay + by) / 2;
            g_pinch_px = v->x;
            g_pinch_py = v->y;
            printf("[wm] two fingers -> scaling '%s', start %dx%d logical, distance %d\n",
                   v->title[0] ? v->title : "view", g_pinch_w0, g_pinch_h0, d0);
            return;
        }
    }

    if (nf >= 2 && g_lp_timer) wl_event_source_timer_update(g_lp_timer, 0);   // two fingers always go to the client
    // The primary finger changed: release logical finger 0 and press it again.
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

        if (grab && ioctl(fd, EVIOCGRAB, 1) == 0) printf("[input] device grabbed with EVIOCGRAB\n");
        g_in.fd = fd;
        printf("[input] %s '%s' x[%d,%d] y[%d,%d]\n", path, name,
               g_in.xmin, g_in.xmax, g_in.ymin, g_in.ymax);
        return fd;
    }
    return -1;
}




