// SPDX-License-Identifier: AGPL-3.0-or-later
static void idle_note_activity();   // ext-idle-notify, defined in idle_notify.hpp
// pointer.hpp
//
// Pointer, mouse buttons, hit testing and raising.
//
// Extracted from the former single-file implementation; behaviour is unchanged.
// Depends on include order: everything used here comes from the modules included
// before it in src/main.cpp.
#pragma once

// ---- wl_keyboard resources ----
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
    // Send enter when focus already exists, otherwise the client never sees keys.
    View *v = g_srv.kb_focus ? g_srv.kb_focus : front_view();
    if (v) {
        if (!g_srv.kb_focus) g_srv.kb_focus = v;
        kb_send_enter_to(r, v);
    }
    printf("[kbd] new client bound the keyboard (v%u)\n", ver);
}

static void pointer_motion(int32_t sx, int32_t sy, bool send) {
    idle_note_activity();
    g_srv.pointer_x = sx;
    g_srv.pointer_y = sy;
    g_cur_x = sx;
    g_cur_y = sy;
    // The cursor layer follows the pointer: use the client cursor surface when there is one.
    if (g_srv.cursor_view && g_srv.cursor_view->layer.win) {
        g_srv.cursor_view->x = sx - g_srv.cursor_hot_x;
        g_srv.cursor_view->y = sy - g_srv.cursor_hot_y;
        g_srv.cursor_view->place();
        cursor_hide();
    } else {
        // Present the cursor only while a finger is down. setTrustedOverlay and
        // setDropInputMode succeed but leave touchOcclusionMode unchanged for non-system
        // layers, so an idle cursor would swallow the next tap.
        if (g_in.down) cursor_present();
    }
    if (g_srv.pointers.empty()) return;
    View *v = hit_test(sx, sy);
    // Implicit pointer grab: the window pressed at holds the pointer until release,
    // otherwise sliding out sends leave and the client drops the button.
    if (g_in.down && g_btn_view && g_btn_view->mapped) v = g_btn_view;
    uint32_t serial = wl_display_next_serial(g_srv.dpy);
    uint32_t t = now_ms();

    if (v != g_srv.pointer_focus) {
        printf("[input] pointer focus: %s -> %s\n",
               g_srv.pointer_focus ? (g_srv.pointer_focus->title[0] ? g_srv.pointer_focus->title : "view") : "(none)",
               v ? (v->title[0] ? v->title : "view") : "(none)");
        if (g_srv.pointer_focus && g_srv.pointer_focus->surface) {
            for (wl_resource *p : g_srv.pointers)
                if (ptr_same_client(p, g_srv.pointer_focus))
                    wl_pointer_send_leave(p, serial, g_srv.pointer_focus->surface);
        }
        g_srv.pointer_focus = v;
        if (v && v->surface) {
            for (wl_resource *p : g_srv.pointers) {
                if (!ptr_same_client(p, v)) continue;
                // wl_fixed_t is 24.8 fixed point; surface coordinates are logical pixels.
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

// Synthetic mouse events, used by the injection socket.
static void mouse_button(int32_t x, int32_t y, int pressed) {
    pointer_motion(x, y, true);
    View *v = g_srv.pointer_focus;
    if (!v) { printf("[mouse] no window at (%d,%d)\n", x, y); return; }
    if (pressed) kb_set_focus(v);
    uint32_t serial = wl_display_next_serial(g_srv.dpy);
    // Qt and similar clients expect a motion event before they accept buttons.
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
    printf("[mouse] %s (%d,%d) -> %s local=(%d,%d) pointers=%zu matched=%d\n", pressed ? "down" : "up", x, y,
           v->title[0] ? v->title : "view", v->to_local_x(x), v->to_local_y(y),
           g_srv.pointers.size(), nmatched);
}

static void mouse_click(int32_t x, int32_t y) {
    mouse_button(x, y, 1);
    mouse_button(x, y, 0);
}

// Raise the clicked window above the others, using max(z) + 1 rather than a growing
// counter, so repeated clicks do not inflate the z values.
static void raise_view(View *v) {
    // Popups have a parent but no parent layer to attach to, so they may raise themselves.
    if (!v || v->is_cursor || (v->parent && !v->is_popup)) return;
    // A window covering the output is a background and does not raise.
    if (v->bw * 100 >= g_out_w * 95 && v->bh * 100 >= g_out_h * 95) {
        printf("[wm] '%s' is a background layer, not raised\n",
               v->title[0] ? v->title : (v->app_id[0] ? v->app_id : "view"));
        return;
    }
    int32_t top = -1;
    for (View *o : g_srv.views)
        if (o->mapped && !o->is_cursor && !o->parent && o->z > top) top = o->z;
    if (v->z >= top) return;
    v->z = top + 1;
    v->place();
    printf("[wm] raise -> '%s' to the front, z=%d\n",
           v->title[0] ? v->title : (v->app_id[0] ? v->app_id : "view"), v->z);
}

// Whether the finger is on the title area at the top, which the compositor owns.
static int32_t title_h(View *v) {
    const int32_t s = v && v->buf_scale > 0 ? v->buf_scale : 1;
    int32_t h = 44 * s;
    // The band must cover the corner area, otherwise a drag on the top edge turns into a
    // resize of the top edge.
    const int32_t band = 32 * (g_scale > 0 ? g_scale : 1);
    if (h < band) h = band;
    if (v && h > v->bh / 3) h = v->bh / 3;
    return h;
}
static bool in_title(View *v, int32_t sx, int32_t sy) {
    if (!v) return false;
    return sy >= v->y && sy < v->y + title_h(v);
}

// Which resize zone the finger is in:
//   1 bottom right, 2 bottom left, 3 top right, 4 top left, 5 right, 6 left, 7 bottom,
//   0 none. The top edge has no resize zone: it is reserved for moving the window.
static int corner_hit(View *v, int32_t sx, int32_t sy) {
    if (!v || v->bw <= 0 || v->bh <= 0) return 0;
    int32_t m = 32 * (g_scale > 0 ? g_scale : 1);
    if (m > v->bw / 3) m = v->bw / 3;
    if (m > v->bh / 3) m = v->bh / 3;
    if (m < 12) m = 12;
    // The whole top edge moves the window and does not resize.
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

// Whether the client behind this view has a focused text field.
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
    idle_note_activity();
    View *v = g_srv.pointer_focus;
    uint32_t serial = wl_display_next_serial(g_srv.dpy);
    uint32_t t = now_ms();
    if (pressed) {
        pointer_motion(g_in.screen_x, g_in.screen_y, true);   // resolve focus and position first
        v = g_srv.pointer_focus;
        if (v) {
            g_btn_view = v;           // remember the pressed window; release goes to it
            kb_set_focus(v);          // keyboard focus follows the click
            raise_view(v);            // raise the clicked window
        }
        // Clicking a client with an active text field opens the keyboard.
        if (!g_ime_on && ti_enabled_for(v)) ime_send(true);
    } else if (!v && g_btn_view) {
        // The focus was already lost on release; send it to the remembered window anyway.
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
    // wl_pointer buttons
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

// Do not send a configure on every frame while dragging an edge or pinching: each one
// forces the client to relayout, redraw and swap buffers.
static uint32_t g_cfg_ms = 0;
static int32_t g_cfg_w = -1, g_cfg_h = -1;
// Two gates: time (120 ms, 250 ms while pinching) and size (24 logical pixels).
// Configure events rebuild the client swapchain, so sending them per frame flickers.
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

// Snap to the left and right edges, the centre line and the top when released.
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
    if (moved) { v->place(); printf("[wm] snap -> (%d,%d)\n", v->x, v->y); }
}

