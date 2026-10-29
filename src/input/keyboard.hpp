// SPDX-License-Identifier: AGPL-3.0-or-later
// keyboard.hpp
//
// wl_keyboard, text_input_v3, input method handling and control commands.
//
// Extracted from the former single-file implementation; behaviour is unchanged.
// Depends on include order: everything used here is provided by the modules
// included before it in src/main.cpp.
#pragma once

// Forward declaration; defined in protocol/workspace.hpp, which is included later.
static void ws_activate(int idx);

// ----------------------------------------------------- wl_keyboard and key injection
//
// There is no physical keyboard on this device, so key events are injected through a
// unix socket at $XDG_RUNTIME_DIR/sfwm-keys.sock, with these commands:
//   t <text>        type text; a bare line is treated the same way
//   k <code> <0|1>  raw evdev keycode, for example KEY_A is 30
//   s <keysym>      for example s Return, s Escape, s Left
static struct xkb_context *g_xkb_ctx = nullptr;
static struct xkb_keymap  *g_xkb_keymap = nullptr;
static struct xkb_state   *g_xkb_state = nullptr;
static std::vector<uint32_t> g_pressed_keys;
static std::vector<int> g_keymap_fds;   // see kb_send_keymap: never closed early; the second part is the evdev keycode

static void kb_xkb_init() {
    if (g_xkb_keymap) return;
    // The Termux xkb data is not in /usr/share/X11/xkb, so set the variable when absent.
    if (!getenv("XKB_CONFIG_ROOT")) {
        const char *cand[] = { "/data/data/com.termux/files/usr/share/X11/xkb", nullptr };
        for (int i = 0; cand[i]; i++) {
            if (access(cand[i], R_OK) == 0) { setenv("XKB_CONFIG_ROOT", cand[i], 0); break; }
        }
    }
    g_xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!g_xkb_ctx) { fprintf(stderr, "[kbd] cannot create the xkb context\n"); return; }
    struct xkb_rule_names names;
    memset(&names, 0, sizeof(names));
    names.rules = "evdev";
    names.model = "pc105";
    names.layout = "us";
    g_xkb_keymap = xkb_keymap_new_from_names(g_xkb_ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!g_xkb_keymap)
        g_xkb_keymap = xkb_keymap_new_from_names(g_xkb_ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!g_xkb_keymap) { fprintf(stderr, "[kbd] cannot compile the keymap; check XKB_CONFIG_ROOT\n"); return; }
    g_xkb_state = xkb_state_new(g_xkb_keymap);
    printf("[kbd] xkb keymap ready\n");
}

// Send the keymap to a wl_keyboard, which takes an fd and a size.
// A runtime file is used instead of memfd; see the note in core/prelude.hpp.
static void kb_send_keymap(wl_resource *r) {
    kb_xkb_init();
    if (!g_xkb_keymap) return;
    char *text = xkb_keymap_get_as_string(g_xkb_keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (!text) return;
    size_t len = strlen(text) + 1;

    int fd = sfwm_rt_file("sfwm-keymap", 0);
    if (fd < 0) { free(text); return; }
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, text + off, len - off);
        if (n <= 0) break;
        off += n;
    }
    free(text);
    if (off != len) { close(fd); return; }
    lseek(fd, 0, SEEK_SET);
    wl_keyboard_send_keymap(r, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)len);
    // The fd must not be closed: libwayland sends it only on wl_display_flush, and closing
    // it early hands the client a dead descriptor, which maps successfully but is empty,
    // so strlen(keymap) runs off the end and raises SIGBUS.
    // One fd leaks per keymap; a session sends a handful.
    g_keymap_fds.push_back(fd);
    printf("[kbd] keymap sent (%zu bytes, fd=%d left open)\n", len, fd);
}

static void kb_send_modifiers(uint32_t serial) {
    if (!g_xkb_state) return;
    uint32_t dep = xkb_state_serialize_mods(g_xkb_state, XKB_STATE_MODS_DEPRESSED);
    uint32_t lat = xkb_state_serialize_mods(g_xkb_state, XKB_STATE_MODS_LATCHED);
    uint32_t lock = xkb_state_serialize_mods(g_xkb_state, XKB_STATE_MODS_LOCKED);
    uint32_t grp = xkb_state_serialize_layout(g_xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
    for (wl_resource *k : g_srv.keyboards)
        wl_keyboard_send_modifiers(k, serial, dep, lat, lock, grp);
}

static void kb_send_modifiers_to(wl_resource *k, uint32_t serial) {
    if (!k || !g_xkb_state) return;
    uint32_t dep = xkb_state_serialize_mods(g_xkb_state, XKB_STATE_MODS_DEPRESSED);
    uint32_t lat = xkb_state_serialize_mods(g_xkb_state, XKB_STATE_MODS_LATCHED);
    uint32_t lock = xkb_state_serialize_mods(g_xkb_state, XKB_STATE_MODS_LOCKED);
    uint32_t grp = xkb_state_serialize_layout(g_xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
    wl_keyboard_send_modifiers(k, serial, dep, lat, lock, grp);
}

// enter and leave only go to keyboards of the same client; other ids are invalid.
static bool kb_same_client(wl_resource *k, View *v) {
    if (!k || !v || !v->surface) return false;
    return wl_resource_get_client(k) == wl_resource_get_client(v->surface);
}

static void kb_send_enter_to(wl_resource *r, View *v) {
    if (!r || !v || !v->surface) return;
    if (!kb_same_client(r, v)) return;
    uint32_t serial = wl_display_next_serial(g_srv.dpy);
    wl_array keys;
    wl_array_init(&keys);
    for (uint32_t kc : g_pressed_keys) {
        uint32_t *p = (uint32_t *)wl_array_add(&keys, sizeof(uint32_t));
        if (p) *p = kc;
    }
    printf("[kbd] enter -> kb#%u(client %p) surface#%u(client %p)\n",
           wl_resource_get_id(r), (void *)wl_resource_get_client(r),
           v->surface ? wl_resource_get_id(v->surface) : 0,
           v->surface ? (void *)wl_resource_get_client(v->surface) : nullptr);
    wl_keyboard_send_enter(r, serial, v->surface, &keys);
    wl_array_release(&keys);
    kb_send_modifiers_to(r, serial);
}

// ---------------------------------------------------------------- zwp_text_input_v3
// Clients declare that they want text input through it. The compositor handles:
//   enter and leave following the keyboard focus, text from the injector through
//   commit_string, and one done after every client commit.
// This is what makes CJK input possible: the US keymap has no such keycodes.
struct TiState {
    bool enabled = false;
    View *focus = nullptr;
    uint32_t hint = 0, purpose = 0;
    uint32_t serial = 0;      // the protocol wants done with the commit counter as serial
};

static void ti_destroyed(wl_resource *r) {
    for (size_t i = 0; i < g_srv.text_inputs.size(); i++)
        if (g_srv.text_inputs[i] == r) { g_srv.text_inputs.erase(g_srv.text_inputs.begin() + i); break; }
}
static TiState *ti_state(wl_resource *r) { return (TiState *)wl_resource_get_user_data(r); }

static void ti_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void ti_enable(wl_client *, wl_resource *r) {
    if (TiState *st = ti_state(r)) {
        st->enabled = true;
        // Enabling text input means a text field took focus, which asks for the keyboard.
        // Nothing acted on it before, so the keyboard had to be requested by hand.
        if (!g_ime_on) ime_send(true);
    }
}
static void ti_disable(wl_client *, wl_resource *r) {
    if (TiState *st = ti_state(r)) {
        st->enabled = false;
        if (g_ime_on) ime_send(false);
    }
}
static void ti_set_surrounding(wl_client *, wl_resource *, const char *, int32_t, int32_t) {}
static void ti_set_cause(wl_client *, wl_resource *, uint32_t) {}
static void ti_set_content_type(wl_client *, wl_resource *r, uint32_t hint, uint32_t purpose) {
    if (TiState *st = ti_state(r)) { st->hint = hint; st->purpose = purpose; }
}
static void ti_set_cursor_rect(wl_client *, wl_resource *, int32_t, int32_t, int32_t, int32_t) {
    // Used to keep the built-in keyboard clear of the cursor.
}
static void ti_commit(wl_client *, wl_resource *r) {
    // The compositor answers every client commit with done.
    TiState *st = ti_state(r);
    uint32_t serial = st ? ++st->serial : 1;
    zwp_text_input_v3_send_done(r, serial);
}
static void ti_set_available_actions(wl_client *, wl_resource *, struct wl_array *) {}
static void ti_show_panel(wl_client *, wl_resource *r) {
    // This is the standard request to raise the keyboard, and was previously only logged,
    // so tapping a text field did nothing.
    printf("[ti] show_input_panel, raising the keyboard\n");
    if (!g_ime_on) ime_send(true);
}
static void ti_hide_panel(wl_client *, wl_resource *r) {
    printf("[ti] hide_input_panel, hiding the keyboard\n");
    if (g_ime_on) ime_send(false);
}
static const struct zwp_text_input_v3_interface ti_impl = {
    .destroy = ti_destroy,
    .enable = ti_enable,
    .disable = ti_disable,
    .set_surrounding_text = ti_set_surrounding,
    .set_text_change_cause = ti_set_cause,
    .set_content_type = ti_set_content_type,
    .set_cursor_rectangle = ti_set_cursor_rect,
    .commit = ti_commit,
    .set_available_actions = ti_set_available_actions,
    .show_input_panel = ti_show_panel,
    .hide_input_panel = ti_hide_panel,
};

static void ti_update_focus(View *oldv, View *newv) {
    if (g_srv.text_inputs.empty()) return;
    for (wl_resource *r : g_srv.text_inputs) {
        TiState *st = ti_state(r);
        if (!st) continue;
        wl_client *c = wl_resource_get_client(r);
        if (oldv && oldv->surface && wl_resource_get_client(oldv->surface) == c) {
            zwp_text_input_v3_send_leave(r, oldv->surface);
            st->focus = nullptr;
        }
        if (newv && newv->surface && wl_resource_get_client(newv->surface) == c) {
            zwp_text_input_v3_send_enter(r, newv->surface);
            st->focus = newv;
        }
    }
}

// Use commit_string when text input is enabled; it carries any UTF-8.
static bool ti_commit_text(const char *utf8) {
    View *v = g_srv.kb_focus;
    bool sent = false;
    for (wl_resource *r : g_srv.text_inputs) {
        TiState *st = ti_state(r);
        if (!st || !st->enabled) continue;
        if (v && v->surface && wl_resource_get_client(v->surface) != wl_resource_get_client(r)) continue;
        zwp_text_input_v3_send_commit_string(r, utf8);
        zwp_text_input_v3_send_done(r, ++st->serial);
        sent = true;
    }
    return sent;
}

// Preedit covers the composition stage, which clients draw as underlined text.
// An empty string clears the preedit.
static bool ti_preedit_text(const char *utf8) {    View *v = g_srv.kb_focus;
    bool sent = false;
    uint32_t len = 0;
    if (utf8 && *utf8) len = (uint32_t)strlen(utf8);
    for (wl_resource *r : g_srv.text_inputs) {
        TiState *st = ti_state(r);
        if (!st || !st->enabled) continue;
        if (v && v->surface && wl_resource_get_client(v->surface) != wl_resource_get_client(r)) continue;
        zwp_text_input_v3_send_preedit_string(r, (utf8 && *utf8) ? utf8 : "", (int32_t)len, (int32_t)len);
        zwp_text_input_v3_send_done(r, ++st->serial);
        sent = true;
    }
    return sent;
}

static void ti_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void ti_mgr_get(wl_client *c, wl_resource *mgr, uint32_t id, wl_resource *seat) {
    (void)seat;
    uint32_t ver = wl_resource_get_version(mgr);
    wl_resource *r = wl_resource_create(c, &zwp_text_input_v3_interface, ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    TiState *st = new TiState();
    wl_resource_set_implementation(r, &ti_impl, st, [](wl_resource *res) {
        ti_destroyed(res);
        delete (TiState *)wl_resource_get_user_data(res);
    });
    g_srv.text_inputs.push_back(r);
    printf("[ti] new text-input (v%u)\n", ver);
    if (g_srv.kb_focus) ti_update_focus(nullptr, g_srv.kb_focus);
}
static const struct zwp_text_input_manager_v3_interface ti_mgr_impl = {
    .destroy = ti_mgr_destroy,
    .get_text_input = ti_mgr_get,
};
static void bind_ti_mgr(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 2) version = 2;
    wl_resource *r = wl_resource_create(c, &zwp_text_input_manager_v3_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &ti_mgr_impl, nullptr, nullptr);
}

/**
  * Make every client leave its text input focus.
 *
  * This is what to do when Android hides the keyboard: the gesture removes it while the
  * focused GTK entry keeps waiting for input, so the two sides drift apart.
  * disable and leave make the client drop its input method context.
 */
static void ti_force_disable() {
    int n = 0;
    for (wl_resource *r : g_srv.text_inputs) {
        TiState *st = ti_state(r);
        if (!st) continue;
        // disable is a client request in text-input-v3, with no matching server event.
        // leave(NULL) is the only way to make a client leave its text input target.
        if (st->enabled || st->focus) {
            zwp_text_input_v3_send_leave(r, nullptr);
            st->focus = nullptr;
            st->enabled = false;
            zwp_text_input_v3_send_done(r, ++st->serial);
            n++;
        }
    }
    if (n) printf("[ti] keyboard hidden by the system, %d clients left text input focus\n", n);
}

// ------------------------------------------------- input method state channel
// The module runs as an untrusted app and cannot reach the unix socket or TCP 5599, so
// it writes a file in its own data directory that is read here as root.
static const char *IME_STATE_FILE = "/data/data/org.sfwm.ime/files/ime-state";
static wl_event_source *g_ime_timer = nullptr;

static void ime_apply(int32_t h, bool on) {
    if (!on) h = 0;
    if (h == g_ime_h && on == g_ime_on) return;
    g_ime_h = h;
    g_ime_on = on;
    if (!on) ti_force_disable();
    // place() alone is not enough: the crop is applied in blit(), and a client that finished
    // drawing sends no more frames, so setGeometry is re-sent explicitly.
    for (View *v : g_srv.views) {
        if (!v->mapped || v->is_x_root) continue;
        v->place();
        if (v->layer.win) {
            v->layer.setGeometry(sf::Rect{0, 0, v->bw, v->bh},
                                 sf::Rect{0, 0, v->bw, v->bh});
        }
    }
    printf("[ime] state: height=%d keyboard=%s\n", g_ime_h, on ? "shown" : "hidden");
    // Hide the layer when the keyboard is hidden; otherwise its last frame, usually an
    // opaque background, keeps covering the bottom of the window and eats clicks.
    for (View *v : g_srv.views)
        if (v->is_ime && v->layer.win) {
            v->layer.setVisible(on);
            printf("[ime] input method layer -> %s\n", on ? "shown" : "hidden");
        }
}

static int on_ime_tick(void *) {
    // Keyboard top edge from a one-shot dumpsys file; height is the screen height minus it.
    // The app state file is no longer read: its values are relative to the window and
    // would fight with the value computed here.
    if (g_ime_on) {
        const char *inset = "/data/local/tmp/sfwm-ime-inset";
        FILE *g = fopen(inset, "r");
        if (g) {
            int top = -1;
            if (fscanf(g, "%d", &top) == 1 && top > 0 && top < g_out_h) {
                int32_t h = g_out_h - top;
                if (h > 0 && h != g_ime_h) {
                    printf("[ime] height %d taken from the window geometry (top %d)\n", h, top);
                    ime_apply(h, true);
                }
            }
            fclose(g);
            unlink(inset);      // one shot, so it is not read back after collapsing
        }
    }
    if (g_ime_timer) wl_event_source_timer_update(g_ime_timer, 300);
    return 0;
}

// The topmost ordinary toplevel, used when the pointer has no focus.
static View *front_view() {
    View *best = nullptr;
    int32_t bestz = -0x7fffffff;
    for (View *v : g_srv.views) {
        if (!v->mapped || v->is_cursor || v->is_subsurface || !v->surface) continue;
        int32_t zz = v->stack_key();
        if (!best || zz >= bestz) { best = v; bestz = zz; }
    }
    return best;
}

static void dd_focus_changed(View *v);   // protocol/data_device.hpp: the clipboard follows focus

static void kb_set_focus(View *v) {
    if (v == g_srv.kb_focus) return;
    View *old_focus = g_srv.kb_focus;
    uint32_t serial = wl_display_next_serial(g_srv.dpy);
    if (g_srv.kb_focus && g_srv.kb_focus->surface) {
        for (wl_resource *k : g_srv.keyboards)
            if (kb_same_client(k, g_srv.kb_focus))
                wl_keyboard_send_leave(k, serial, g_srv.kb_focus->surface);
    }
    g_srv.kb_focus = v;
    printf("[kbd] focus -> %s\n", v ? (v->title[0] ? v->title : "view") : "(none)");
    ti_update_focus(old_focus, v);   // text input enter and leave follow the keyboard focus
    // Release every held key on a focus change, so none stays stuck.
    for (uint32_t kc : g_pressed_keys) {
        for (wl_resource *k : g_srv.keyboards)
            if (kb_same_client(k, v))
                wl_keyboard_send_key(k, serial, now_ms(), kc, WL_KEYBOARD_KEY_STATE_RELEASED);
    }
    if (g_xkb_state) {
        for (uint32_t kc : g_pressed_keys) xkb_state_update_key(g_xkb_state, kc + 8, XKB_KEY_UP);
    }
    g_pressed_keys.clear();
    if (v) {
        for (wl_resource *k : g_srv.keyboards) kb_send_enter_to(k, v);
    }
    dd_focus_changed(v);   // the new client receives the clipboard, the old one selection(NULL)
}

static void kb_key(uint32_t code, bool pressed) {
    if (g_srv.keyboards.empty()) { printf("[kbd] no client is listening for keys\n"); return; }
    if (!g_srv.kb_focus) kb_set_focus(front_view());
    if (!g_srv.kb_focus) { printf("[kbd] no focusable window, dropping key %u\n", code); return; }
    kb_xkb_init();
    if (g_xkb_state)
        xkb_state_update_key(g_xkb_state, code + 8, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
    if (pressed) {
        bool have = false;
        for (uint32_t kc : g_pressed_keys) if (kc == code) have = true;
        if (!have) g_pressed_keys.push_back(code);
    } else {
        for (size_t i = 0; i < g_pressed_keys.size(); i++)
            if (g_pressed_keys[i] == code) { g_pressed_keys.erase(g_pressed_keys.begin() + i); break; }
    }
    uint32_t serial = wl_display_next_serial(g_srv.dpy);
    uint32_t t = now_ms();
    for (wl_resource *k : g_srv.keyboards) {
        if (!kb_same_client(k, g_srv.kb_focus)) continue;
        wl_keyboard_send_key(k, serial, t, code,
                             pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
        kb_send_modifiers_to(k, serial);
    }
    static int key_logs = 0;
    if (key_logs++ < 12)
        printf("[kbd] -> code=%u %s (keyboards=%zu focus=%s)\n", code,
               pressed ? "down" : "up", g_srv.keyboards.size(),
               g_srv.kb_focus ? (g_srv.kb_focus->title[0] ? g_srv.kb_focus->title : "view") : "none");
}

// Find a keycode producing the keysym, trying level 0 and then level 1.
static bool kb_find_keycode(xkb_keysym_t sym, uint32_t *code, bool *need_shift) {
    if (!g_xkb_keymap) return false;
    for (xkb_keycode_t kc = 9; kc < 256; kc++) {
        const xkb_keysym_t *syms = nullptr;
        int n = xkb_keymap_key_get_syms_by_level(g_xkb_keymap, kc, 0, 0, &syms);
        if (n == 1 && syms[0] == sym) { *code = kc - 8; *need_shift = false; return true; }
        n = xkb_keymap_key_get_syms_by_level(g_xkb_keymap, kc, 0, 1, &syms);
        if (n == 1 && syms[0] == sym) { *code = kc - 8; *need_shift = true; return true; }
    }
    return false;
}

static void kb_tap(uint32_t code, bool shift) {
    // KEY_LEFTSHIFT is a macro in linux/input.h and cannot be used as a variable name.
    if (shift) kb_key(KEY_LEFTSHIFT, true);
    kb_key(code, true);
    kb_key(code, false);
    if (shift) kb_key(KEY_LEFTSHIFT, false);
}

static void pointer_motion(int32_t sx, int32_t sy, bool send);
static void mouse_button(int32_t x, int32_t y, int pressed);
static void mouse_click(int32_t x, int32_t y);

static void kb_type_text(const char *s) {
    kb_xkb_init();
    // Clients with text input get commit_string, which carries arbitrary UTF-8, while
    // keymap injection has no keycode for CJK characters.
    if (ti_commit_text(s)) { printf("[ime] commit_string: %s\n", s); return; }
    if (!g_xkb_keymap) return;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t cp = 0;
        // decode UTF-8
        if (*p < 0x80) { cp = *p++; }
        else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3;
        } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
            cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) |
                 ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); p += 4;
        } else { p++; continue; }

        if (cp == '\n') { kb_tap(KEY_ENTER, false); continue; }
        if (cp == '\t') { kb_tap(KEY_TAB, false); continue; }

        uint32_t code = 0;
        bool shift = false;
        if (kb_find_keycode((xkb_keysym_t)cp, &code, &shift)) {
            kb_tap(code, shift);
        } else {
            printf("[kbd] cannot type U+%04X\n", cp);
        }
    }
}

static void kb_send_symbol(const char *name) {
    kb_xkb_init();
    if (!g_xkb_keymap) return;
    xkb_keysym_t sym = xkb_keysym_from_name(name, XKB_KEYSYM_NO_FLAGS);
    if (sym == XKB_KEY_NoSymbol) { printf("[kbd] unknown keysym %s\n", name); return; }
    uint32_t code = 0;
    bool shift = false;
    if (kb_find_keycode(sym, &code, &shift)) kb_tap(code, shift);
    else printf("[kbd] no keycode for keysym %s\n", name);
}

// Raising and hiding the Android keyboard writes settings, which the input method
// service runs. sfwm cannot raise it itself: an input method needs a Java view with a
// window token and an InputConnection, which a SurfaceFlinger layer does not have.
static void ime_kick_geometry();
static void ime_send(bool show) {
    g_ime_on = show;
    // Preferred: write to the registered connection, as a Wayland server sends events.
    if (g_ime_fd >= 0) {
        const char *msg = show ? "activate\n" : "deactivate\n";
        if (write(g_ime_fd, msg, strlen(msg)) >= 0) {
            printf("[ime] %s sent to the IME client, fd=%d\n", show ? "activate" : "deactivate", g_ime_fd);
            if (show) ime_kick_geometry();
            return;
        }
        printf("[ime] delivery failed, client gone, falling back to settings\n");
        g_ime_fd = -1;
    }
    // Fallback when no client is connected: settings and am.
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long ms = (long)ts.tv_sec * 1000 + (long)(ts.tv_nsec / 1000000);
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "/system/bin/am start-foreground-service -n org.sfwm.ime/.SfwmImeService -f 0x20 >/dev/null 2>&1; "
             "settings put global sfwm_ime %d; "
             "settings put global sfwm_ime_seq %ld",
             show ? 1 : 2, ms);
    int rc = system(cmd);
    printf("[ime] %s rc=%d seq=%ld through the fallback\n", show ? "raise" : "hide", rc, ms);
    if (show) ime_kick_geometry();
}

// Read the window geometry once the keyboard is up to get the real height.
// It is the only reliable source here: the insets are always 208 and the hinted
static void ime_kick_geometry() {
    system("( sleep 1; dumpsys window windows 2>/dev/null "
           "| sed -n '/InputMethod}/,/Window #/p' "
           "| grep -o 'mGivenContentInsets=\\[0,[0-9]*\\]' | head -1 | tr -dc '0-9' "
           "> /data/local/tmp/sfwm-ime-inset ) &");
}

static void kb_command(const std::string &line, int fd) {
    if (line.empty()) return;
    printf("[kbd] command: %s\n", line.c_str());
    const char *s = line.c_str();
    if (line == "ime-register") {
        // The IME host registers as a persistent client; activate and deactivate are written
        g_ime_fd = fd;
        printf("[ime] IME client registered, fd=%d\n", fd);
        return;
    }
    if (line.compare(0, 5, "imeh ") == 0) {
        int h = atoi(s + 5);
        if (h < 0) h = 0;
        g_ime_h = h;
        for (View *v : g_srv.views) if (v->mapped) v->place();
        printf("[ime] client reported height %d\n", g_ime_h);
        return;
    }
    if (line.compare(0, 2, "t ") == 0) { kb_type_text(s + 2); }
    else if (line.compare(0, 5, "text ") == 0) { kb_type_text(s + 5); }
    else if (line.compare(0, 2, "k ") == 0) {
        unsigned code = 0, state = 1;
        if (sscanf(s + 2, "%u %u", &code, &state) >= 1) kb_key(code, state != 0);
    } else if (line.compare(0, 2, "s ") == 0) { kb_send_symbol(s + 2); }
    else if (line.compare(0, 2, "m ") == 0) {
        int x = 0, y = 0;
        if (sscanf(s + 2, "%d %d", &x, &y) == 2) { pointer_motion(x, y, true); }
    } else if (line.compare(0, 2, "c ") == 0) {
        int x = 0, y = 0, st = 1;
        int n = sscanf(s + 2, "%d %d %d", &x, &y, &st);
        // c x y      a full click, press and release
        // c x y 0/1  release or press only
        if (n >= 3) mouse_button(x, y, st != 0);
        else if (n == 2) mouse_click(x, y);
    } else if (line.compare(0, 2, "p ") == 0) {
        int x = 0, y = 0;
        if (sscanf(s + 2, "%d %d", &x, &y) == 2) mouse_click(x, y);
    } else if (line.compare(0, 4, "pre ") == 0) {
        // preedit string; a bare "pre" clears it
        ti_preedit_text(s + 4);
    } else if (line == "pre") {
        ti_preedit_text("");
    } else if (line == "snap") {
        g_snap_on = !g_snap_on;
        printf("[wm] snap %s (threshold %d dp)\n", g_snap_on ? "on" : "off", g_snap_band_dp);
    } else if (line.compare(0, 7, "bright ") == 0) {
        // bright <0..100> writes /sys/class/backlight/*/brightness
        disp_set_brightness_pct(atoi(s + 7));
    } else if (line == "bright") {
        disp_status();
    } else if (line.compare(0, 4, "cct ") == 0) {
        // cct <K> or cct off uses cmd display dwb-set-cct
        std::string a = line.substr(4);
        if (a == "off" || a == "0" || a == "native") disp_set_cct(-1);
        else disp_set_cct(atoi(a.c_str()));
    } else if (line.compare(0, 7, "screen ") == 0) {
        std::string a = line.substr(7);
        if (a == "off" || a == "0") disp_set_screen(false);
        else if (a == "on" || a == "1") disp_set_screen(true);
        else disp_status();
    } else if (line.compare(0, 3, "ws ") == 0) {
        // ws <1..4> switches workspace
        ws_activate(atoi(s + 3) - 1);
    } else if (line.compare(0, 5, "zoom ") == 0) {
        // zoom <f> sets the composer-side zoom without a gesture
        float f = (float)atof(s + 5);
        if (f < 0.2f) f = 0.2f;
        if (f > 3.0f) f = 3.0f;
        View *v = g_srv.pointer_focus ? g_srv.pointer_focus : g_srv.kb_focus;
        if (!v) { printf("[wm] zoom: no focused window\n"); return; }
        v->set_zoom(f);   // uses the same path as a real gesture
        printf("[wm] manual zoom=%.2f -> '%s' buf=%dx%d\n", f,
               v->title[0] ? v->title : "view", v->bw, v->bh);
    } else if (line == "zoom") {
        View *v = g_srv.pointer_focus ? g_srv.pointer_focus : g_srv.kb_focus;
        if (v) printf("[wm] '%s' zoom=%.2f buf=%dx%d pos=(%d,%d)\n",
                      v->title[0] ? v->title : "view", v->zoom, v->bw, v->bh, v->x, v->y);
    } else if (line == "ws") {
        printf("[ws] %d workspaces, current %d, window assignment:\n", g_ws_count, g_ws_active + 1);
        for (View *v : g_srv.views) {
            if (!v || !v->mapped) continue;
            printf("[ws]   ws=%d %s%s\n", v->ws + 1,
                   v->title[0] ? v->title : (v->app_id[0] ? v->app_id : "view"),
                   ws_visible(v) ? "" : "(hidden)");
        }
    } else if (line == "screen") {
        disp_status();
    } else if (line.compare(0, 5, "snap ") == 0) {
        std::string arg = line.substr(5);
        if (arg == "on" || arg == "1" || arg == "true") g_snap_on = true;
        else if (arg == "off" || arg == "0" || arg == "false") g_snap_on = false;
        else { int n = atoi(arg.c_str()); if (n >= 0 && n <= 200) g_snap_band_dp = n; }
        printf("[wm] snap %s (threshold %d dp)\n", g_snap_on ? "on" : "off", g_snap_band_dp);
    } else if (line == "dump") {
        printf("[dump] screen=%dx%d scale=%d gesture=%d owner=%d\n",
               g_out_w, g_out_h, g_scale, (int)g_in_gesture, touch_owner_value());
        for (View *v : g_srv.views) {
            if (!v) continue;
            printf("[dump] %-28s %p x=%-5d y=%-5d buf=%dx%d scale=%d z=%-5d mapped=%d cur=%d sub=%d layer=%d title='%s'\n",
                   v->app_id[0] ? v->app_id : "?", (void *)v, v->x, v->y, v->bw, v->bh,
                   v->buf_scale, g_base_z + v->stack_key(), (int)v->mapped,
                   (int)v->is_cursor, (int)v->is_subsurface, (int)v->is_layer,
                   v->title);
        }
        printf("[dump] pointer=%d,%d focus=%p\n", g_srv.pointer_x, g_srv.pointer_y,
               (void *)g_srv.pointer_focus);
    } else if (line == "fit") {
        // Recovery: clamp the largest ordinary window into the output and centre it.
        View *v = nullptr;
        for (View *o : g_srv.views) {
            if (!o->mapped || o->is_cursor || o->is_layer || o->is_subsurface) continue;
            if (!v || (int64_t)o->bw * o->bh > (int64_t)v->bw * v->bh) v = o;
        }
        if (!v) { printf("[wm] fit: no window\n"); return; }
        const int32_t ox = v->x, oy = v->y;
        if (v->bw <= g_out_w) v->x = (g_out_w - v->bw) / 2;
        if (v->bh <= g_out_h) v->y = (g_out_h - v->bh) / 2;
        v->clamp_on_screen();
        printf("[wm] fit '%s' (%d,%d)->(%d,%d) buf=%dx%d output=%dx%d\n",
               v->title[0] ? v->title : "view", ox, oy, v->x, v->y, v->bw, v->bh, g_out_w, g_out_h);
    } else if (line == "max") {
        // Maximise by sending a configure with the size; changing x and y alone leaves the
        // client buffer at its own size.
        // Choose the largest window, not the topmost one, which may be the 64x64 cursor.
        View *v = nullptr;
        for (View *o : g_srv.views) {
            if (!o->mapped || o->is_cursor || o->is_layer || o->is_subsurface) continue;
            if (!v || (int64_t)o->bw * o->bh > (int64_t)v->bw * v->bh) v = o;
        }
        if (!v) { printf("[wm] max: no maximisable window\n"); return; }
        // The client buf_scale is required here, not g_scale: g_scale is the compositor UI
        // scale from --scale and has nothing to do with the client.
        // Using it turns a 1272x2800 output into 424x933.
        const int32_t sc = client_scale(v);
        v->x = 0;
        v->y = 0;
        v->req_w = g_out_w / sc;
        v->req_h = g_out_h / sc;
        // The size alone is not enough: many clients apply it only with MAXIMIZED in the state.
        v->want_maximized = true;
        toplevel_send_configure(v);
        v->place();
        printf("[wm] max '%s' -> requesting %dx%d logical (output %dx%d, scale=%d)\n",
               v->title[0] ? v->title : "view", v->req_w, v->req_h, g_out_w, g_out_h, sc);
    } else if (line.rfind("zoom ", 0) == 0) {
        // Debug: apply composer-side zoom without touching the client.
        View *v = nullptr;
        for (View *o : g_srv.views) {
            if (!o->mapped || o->is_cursor || o->is_layer || o->is_subsurface) continue;
            if (!v || (int64_t)o->bw * o->bh > (int64_t)v->bw * v->bh) v = o;
        }
        if (!v) { printf("[wm] zoom: no window\n"); return; }
        float z = (float)atof(line.c_str() + 5);
        if (z < 0.1f) z = 0.1f;
        if (z > 4.0f) z = 4.0f;
        const int32_t cx = v->x + v->bw / 2, cy = v->y + v->bh / 2;
        v->x = cx - (int32_t)(v->bw * z) / 2;
        v->y = cy - (int32_t)(v->bh * z) / 2;
        v->set_zoom(z);
        printf("[wm] zoom %.3f -> '%s' dst=%dx%d at (%d,%d), buffer stays %dx%d, no configure sent\n",
               z, v->title[0] ? v->title : "view",
               (int)(v->bw * z), (int)(v->bh * z), v->x, v->y, v->bw, v->bh);
    } else if (line == "zoomend") {
        // Debug: simulate the release by sending one configure.
        View *v = nullptr;
        for (View *o : g_srv.views) {
            if (!o->mapped || o->is_cursor || o->is_layer || o->is_subsurface) continue;
            if (!v || (int64_t)o->bw * o->bh > (int64_t)v->bw * v->bh) v = o;
        }
        if (!v) { printf("[wm] zoomend: no window\n"); return; }
        const int32_t sc0 = client_scale(v);
        v->req_w = (int32_t)(v->bw * v->zoom) / sc0;
        v->req_h = (int32_t)(v->bh * v->zoom) / sc0;
        if (v->req_w < 120) v->req_w = 120;
        if (v->req_h < 120) v->req_h = 120;
        toplevel_send_configure(v);
        printf("[wm] zoomend zoom=%.3f -> requesting %dx%d logical (buffer %dx%d)\n",
               v->zoom, v->req_w, v->req_h, v->bw, v->bh);
    } else if (line == "ct") {
        // Click-through: our layers stop taking touches and the Android UI is reachable.
        g_clickthrough = !g_clickthrough;
        if (g_clickthrough) { g_in.resizing = false; g_in.resize_view = nullptr; g_in.dragging = false; g_in.drag_view = nullptr; g_in.down = false; g_in.pending = 0; }
        for (View *v : g_srv.views) if (v->mapped) v->place();
        printf("[wm] click-through %s\n", g_clickthrough ? "on" : "off");
    } else if (line.compare(0, 3, "ct ") == 0) {
        // "on" and "off" share their first letter, so compare whole words.
        std::string arg = line.substr(3);
        bool val = g_clickthrough;
        if (arg == "on" || arg == "1" || arg == "true" || arg == "yes") val = true;
        else if (arg == "off" || arg == "0" || arg == "false" || arg == "no") val = false;
        else val = !g_clickthrough;
        g_clickthrough = val;
        if (g_clickthrough) { g_in.resizing = false; g_in.resize_view = nullptr; g_in.dragging = false; g_in.drag_view = nullptr; g_in.down = false; g_in.pending = 0; }
        for (View *v : g_srv.views) if (v->mapped) v->place();
        printf("[wm] click-through %s\n", g_clickthrough ? "on" : "off");
    } else if (line == "ime") {
        ime_send(!g_ime_on);
    } else if (line.compare(0, 4, "ime ") == 0) {
        std::string a = line.substr(4);
        if (a == "on" || a == "1" || a == "true") ime_send(true);
        else if (a == "off" || a == "0" || a == "false") ime_send(false);
        else ime_send(!g_ime_on);
    } else if (line.compare(0, 5, "imeh ") == 0) {
        int h = atoi(s + 5);
        if (h < 0) h = 0;
        g_ime_h = h;
        for (View *v : g_srv.views) if (v->mapped) v->place();
        printf("[ime] height %d\n", g_ime_h);
    } else if (line.compare(0, 9, "imestate ") == 0) {
        int st = atoi(s + 9);
        g_ime_on = st != 0;
        if (!g_ime_on) {
            g_ime_h = 0;
            ti_force_disable();     // keyboard hidden by the system: clients leave text input focus
        }
        for (View *v : g_srv.views) if (v->mapped) v->place();
        printf("[ime] state %s (h=%d)\n", g_ime_on ? "shown" : "hidden", g_ime_h);
    } else if (line == "hide") {
        // Hide the output and give the screen back to Android; the process keeps running.
        for (View *v : g_srv.views) if (v->layer.win) v->layer.hide();
        printf("[sfwm] layers hidden\n");
    } else if (line == "show") {
        for (View *v : g_srv.views) if (v->mapped) v->place();
        printf("[sfwm] layers restored\n");
    }
    else if (line == "help") {
        printf("[kbd] usage: t <text> | k <evdev> <0|1> | s <keysym> | m <x> <y> | c <x> <y> <0|1> | p <x> <y>\n");
    } else {
        kb_type_text(s);   // a bare line types text
    }
}

