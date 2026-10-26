// keyboard.hpp
//
// wl_keyboard + text_input_v3 + IME + 控制命令
//
// 从 sfwm.cpp 原样搬出（只搬不改，逻辑零差异）。
// 依赖包含顺序：本文件用到的东西必须由**前面的**模块提供，
// 因此它在 sfwm.cpp 里的 include 位置就是它原来在文件里的位置。
#pragma once

// ---------------------------------------------------------------- wl_keyboard / 按键注入
//
// 手机上没有物理键盘设备（/dev/input 里只有 gpio/pwrkey/touchpanel），
// 所以键事件靠一个 unix socket 注入：$XDG_RUNTIME_DIR/sfwm-keys.sock
//   t <文本>        —— 直接打字（裸行也当这个用）
//   k <code> <0|1>  —— 原始 evdev keycode（KEY_A=30 等）
//   s <keysym名>    —— 例如 s Return / s Escape / s Left
static struct xkb_context *g_xkb_ctx = nullptr;
static struct xkb_keymap  *g_xkb_keymap = nullptr;
static struct xkb_state   *g_xkb_state = nullptr;
static std::vector<uint32_t> g_pressed_keys;   // 当前按下的 evdev keycode

static void kb_xkb_init() {
    if (g_xkb_keymap) return;
    // Termux 的 xkb 数据不在 /usr/share/X11/xkb，环境变量没设的话补一个
    if (!getenv("XKB_CONFIG_ROOT")) {
        const char *cand[] = { "/data/data/com.termux/files/usr/share/X11/xkb", nullptr };
        for (int i = 0; cand[i]; i++) {
            if (access(cand[i], R_OK) == 0) { setenv("XKB_CONFIG_ROOT", cand[i], 0); break; }
        }
    }
    g_xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!g_xkb_ctx) { fprintf(stderr, "[kbd] xkb_context 创建失败\n"); return; }
    struct xkb_rule_names names;
    memset(&names, 0, sizeof(names));
    names.rules = "evdev";
    names.model = "pc105";
    names.layout = "us";
    g_xkb_keymap = xkb_keymap_new_from_names(g_xkb_ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!g_xkb_keymap)
        g_xkb_keymap = xkb_keymap_new_from_names(g_xkb_ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!g_xkb_keymap) { fprintf(stderr, "[kbd] keymap 编译失败（XKB_CONFIG_ROOT 对不？）\n"); return; }
    g_xkb_state = xkb_state_new(g_xkb_keymap);
    printf("[kbd] xkb keymap 就绪\n");
}

// 发 keymap 给某个 wl_keyboard（wayland 要求 fd + size）。
// 用 sfwm_rt_file 而不是 memfd —— 见文件头那段说明（跨 MLS 收 fd 会被拒）。
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
    close(fd);
    printf("[kbd] 已下发 keymap (%zu 字节)\n", len);
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

// 只有「同一个客户端」的 keyboard 才能收到 enter/leave，否则 wl_resource 里的 id 对不上
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
// 客户端（GTK/Qt/foot…）靠它声明「我要输入法」。合成器这边负责：
//   enter/leave（跟随键盘焦点）、把注入器的文本用 commit_string 送进去、
//   每次客户端 commit 后回一个 done（协议要求）。
// 这是「中文能打字」的关键 —— 键盘注入走 US keymap 是没有 CJK keycode 的。
struct TiState {
    bool enabled = false;
    View *focus = nullptr;
    uint32_t hint = 0, purpose = 0;
    uint32_t serial = 0;      // 协议要求：done 的 serial = commit 计数（这里收发共用一个递增计数器）
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
        // 客户端「启用输入法」= 有输入框拿到焦点 —— 这就是要弹键盘的信号。
        // 以前这步没人管，所以只能手动喊 ime on。
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
    // P2：内置软键盘要靠它避开光标
}
static void ti_commit(wl_client *, wl_resource *r) {
    // 客户端每次 commit 之后，合成器必须回 done（serial 用递增计数）
    TiState *st = ti_state(r);
    uint32_t serial = st ? ++st->serial : 1;
    zwp_text_input_v3_send_done(r, serial);
}
static void ti_set_available_actions(wl_client *, wl_resource *, struct wl_array *) {}
static void ti_show_panel(wl_client *, wl_resource *r) {
    // 这就是「用户点了一下输入框、要弹键盘」的标准信号。以前只打印了一行日志，
    // 所以点输入框没任何反应（enable 只在客户端第一次启用输入法时发一次）。
    printf("[ti] show_input_panel -> 拉起键盘\n");
    if (!g_ime_on) ime_send(true);
}
static void ti_hide_panel(wl_client *, wl_resource *r) {
    printf("[ti] hide_input_panel -> 收起键盘\n");
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

// 有 enabled 的 text-input 就用 commit_string（支持任意 UTF-8，含中文）
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

// 拼音/组词阶段（还没上屏）走 preedit_string，客户端会画成带下划线的预编辑串。
// 传空串 = 清掉预编辑（finishComposingText）。
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
    printf("[ti] 新 text-input (v%u)\n", ver);
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
 * 强制让所有客户端「退出输入法焦点」。
 *
 * 这是「安卓那边把键盘收掉了」时该做的事：用户一个返回手势把键盘划走，
 * 桌面这边 GTK 的输入框却还挂着 IME 焦点（光标还在闪、还在等输入法），
 * 两边状态就脱节了。发 disable + leave，客户端才会真正 focus_out 掉输入法上下文。
 */
static void ti_force_disable() {
    int n = 0;
    for (wl_resource *r : g_srv.text_inputs) {
        TiState *st = ti_state(r);
        if (!st) continue;
        // text-input-v3 里 disable 是「客户端→服务端」的 request，服务端没有对应事件。
        // 服务端能让客户端 focus_out 的手段只有一个：发 leave(null) —— surface 为空即「离开当前输入目标」。
        if (st->enabled || st->focus) {
            zwp_text_input_v3_send_leave(r, nullptr);
            st->focus = nullptr;
            st->enabled = false;
            zwp_text_input_v3_send_done(r, ++st->serial);
            n++;
        }
    }
    if (n) printf("[ti] 键盘被系统收起 -> 已让 %d 个客户端退出输入法焦点\n", n);
}

// ---------------------------------------------------------------- 输入法状态通道
// 为什么不走 socket：模块是个 untrusted_app，连不进我们的 unix socket，TCP 5599 也不可靠。
// 它写自己数据目录里的一个文件（只 app 自己能写），我们用 root 把它读出来。
static const char *IME_STATE_FILE = "/data/data/org.sfwm.ime/files/ime-state";
static wl_event_source *g_ime_timer = nullptr;

static void ime_apply(int32_t h, bool on) {
    if (!on) h = 0;
    if (h == g_ime_h && on == g_ime_on) return;
    g_ime_h = h;
    g_ime_on = on;
    if (!on) ti_force_disable();
    // 注意：不能只 place() —— 裁剪写在 blit() 里，而客户端画完一帧就不再送帧，
    // 高度变化时不会有新帧来触发它。所以这里必须主动重发一次 setGeometry。
    for (View *v : g_srv.views) {
        if (!v->mapped || v->is_x_root) continue;
        v->place();
        if (v->layer.win) {
            v->layer.setGeometry(sf::Rect{0, 0, v->bw, v->bh},
                                 sf::Rect{0, 0, v->bw, v->bh});
        }
    }
    printf("[ime] 控件状态: 高=%d 键盘=%s\n", g_ime_h, on ? "显示" : "隐藏");
    // 键盘收起时必须把它的图层也藏掉 —— 否则它会以“最后一帧”（通常是一整块不透明背景）
    // 一直盖在窗口底部，而且还会吃掉点击（表现为“不该遮挡的时候遮挡”）。
    for (View *v : g_srv.views)
        if (v->is_ime && v->layer.win) {
            v->layer.setVisible(on);
            printf("[ime] 输入法图层 -> %s\n", on ? "显示" : "隐藏");
        }
}

static int on_ime_tick(void *) {
    // 键盘上沿（来自 dumpsys，一次性文件）：高度 = 屏幕高 - 上沿
    // 注意：不再读 app 写的那份状态文件 —— 它只能是 208/0（insets 相对窗口算，不可信），
    // 会和这里算出的 1024 互相覆盖，造成画面抽插。
    if (g_ime_on) {
        const char *inset = "/data/local/tmp/sfwm-ime-inset";
        FILE *g = fopen(inset, "r");
        if (g) {
            int top = -1;
            if (fscanf(g, "%d", &top) == 1 && top > 0 && top < g_out_h) {
                int32_t h = g_out_h - top;
                if (h > 0 && h != g_ime_h) {
                    printf("[ime] 从窗口几何取到高度 = %d（上沿 %d）\n", h, top);
                    ime_apply(h, true);
                }
            }
            fclose(g);
            unlink(inset);      // 一次性，避免收起后又把它读回来
        }
    }
    if (g_ime_timer) wl_event_source_timer_update(g_ime_timer, 300);
    return 0;
}

// 最上层的普通 toplevel（没有指针焦点时兜底用）
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

static void dd_focus_changed(View *v);   // protocol/data_device.hpp（剪贴板跟焦点走）

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
    printf("[kbd] 焦点 -> %s\n", v ? (v->title[0] ? v->title : "view") : "(无)");
    ti_update_focus(old_focus, v);   // text-input 的 enter/leave 跟着键盘焦点走
    // 换焦点时把按下的键全放掉，避免卡键
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
    dd_focus_changed(v);   // 新焦点客户端拿到剪贴板；老的收到 selection(NULL)
}

static void kb_key(uint32_t code, bool pressed) {
    if (g_srv.keyboards.empty()) { printf("[kbd] 没有客户端在听键盘\n"); return; }
    if (!g_srv.kb_focus) kb_set_focus(front_view());
    if (!g_srv.kb_focus) { printf("[kbd] 没有可聚焦窗口，丢弃按键 %u\n", code); return; }
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
               pressed ? "按下" : "抬起", g_srv.keyboards.size(),
               g_srv.kb_focus ? (g_srv.kb_focus->title[0] ? g_srv.kb_focus->title : "view") : "无");
}

// 找一个能产生该 keysym 的 keycode（先试 level0，再试 level1=Shift）
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
    // 注意：KEY_LEFTSHIFT 是 linux/input.h 里的宏（=42），不能拿来当变量名
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
    // 有 text-input 的客户端优先走 commit_string —— 这条能送任意 UTF-8（含中文），
    // 而 keymap 注入要找 keycode，CJK 根本找不到。
    if (ti_commit_text(s)) { printf("[ime] commit_string: %s\n", s); return; }
    if (!g_xkb_keymap) return;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t cp = 0;
        // 解 UTF-8
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
            printf("[kbd] 这个字符敲不出来: U+%04X\n", cp);
        }
    }
}

static void kb_send_symbol(const char *name) {
    kb_xkb_init();
    if (!g_xkb_keymap) return;
    xkb_keysym_t sym = xkb_keysym_from_name(name, XKB_KEYSYM_NO_FLAGS);
    if (sym == XKB_KEY_NoSymbol) { printf("[kbd] 不认识的 keysym: %s\n", name); return; }
    uint32_t code = 0;
    bool shift = false;
    if (kb_find_keycode(sym, &code, &shift)) kb_tap(code, shift);
    else printf("[kbd] keysym %s 没有对应 keycode\n", name);
}

// 拉起/收起安卓软键盘：命令写 settings，由 LSPosed 模块里的悬浮窗服务 SfwmImeService 执行。
// 为什么不由 sfwm 自己弹：输入法要求一个 Java View（ViewRootImpl + window token + InputConnection），
// SurfaceComposer 图层没有这一层，IME 永远指不到它。
static void ime_kick_geometry();
static void ime_send(bool show) {
    g_ime_on = show;
    // 首选：往下发给已登记的长连接（相当于 Wayland 服务端发事件）
    if (g_ime_fd >= 0) {
        const char *msg = show ? "activate\n" : "deactivate\n";
        if (write(g_ime_fd, msg, strlen(msg)) >= 0) {
            printf("[ime] %s（下发给 IME 客户端 fd=%d）\n", show ? "activate" : "deactivate", g_ime_fd);
            if (show) ime_kick_geometry();
            return;
        }
        printf("[ime] 下发失败，客户端连接已断，回退到 settings 通道\n");
        g_ime_fd = -1;
    }
    // 回退：没有客户端连接时，照旧用 settings + am
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
    printf("[ime] %s rc=%d seq=%ld（回退通道）\n", show ? "拉起" : "收起", rc, ms);
    if (show) ime_kick_geometry();
}

// 键盘弹出后去捡一次窗口几何（IME 窗口的 mGivenContentInsets）算真实高度。
// 这是目前唯一可靠的来源：insets 恒为 208，IMM 推荐高度恒为 0。
static void ime_kick_geometry() {
    system("( sleep 1; dumpsys window windows 2>/dev/null "
           "| sed -n '/InputMethod}/,/Window #/p' "
           "| grep -o 'mGivenContentInsets=\\[0,[0-9]*\\]' | head -1 | tr -dc '0-9' "
           "> /data/local/tmp/sfwm-ime-inset ) &");
}

static void kb_command(const std::string &line, int fd) {
    if (line.empty()) return;
    printf("[kbd] 命令: %s\n", line.c_str());
    const char *s = line.c_str();
    if (line == "ime-register") {
        // IME 宿主登记为「常驻客户端」：这条连接保持住，以后由 sfwm 往它下发 activate/deactivate
        g_ime_fd = fd;
        printf("[ime] IME 客户端已登记 fd=%d\n", fd);
        return;
    }
    if (line.compare(0, 5, "imeh ") == 0) {
        int h = atoi(s + 5);
        if (h < 0) h = 0;
        g_ime_h = h;
        for (View *v : g_srv.views) if (v->mapped) v->place();
        printf("[ime] 客户端上报高度 = %d\n", g_ime_h);
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
        // c x y      = 完整点击（按下+抬起）
        // c x y 0/1  = 只抬起/只按下
        if (n >= 3) mouse_button(x, y, st != 0);
        else if (n == 2) mouse_click(x, y);
    } else if (line.compare(0, 2, "p ") == 0) {
        int x = 0, y = 0;
        if (sscanf(s + 2, "%d %d", &x, &y) == 2) mouse_click(x, y);
    } else if (line.compare(0, 4, "pre ") == 0) {
        // 预编辑串（拼音未上屏），空内容用 "pre" 单独一行来清
        ti_preedit_text(s + 4);
    } else if (line == "pre") {
        ti_preedit_text("");
    } else if (line == "snap") {
        g_snap_on = !g_snap_on;
        printf("[wm] 吸附 = %s（阈值 %d dp）\n", g_snap_on ? "开" : "关", g_snap_band_dp);
    } else if (line.compare(0, 5, "snap ") == 0) {
        std::string arg = line.substr(5);
        if (arg == "on" || arg == "1" || arg == "true") g_snap_on = true;
        else if (arg == "off" || arg == "0" || arg == "false") g_snap_on = false;
        else { int n = atoi(arg.c_str()); if (n >= 0 && n <= 200) g_snap_band_dp = n; }
        printf("[wm] 吸附 = %s（阈值 %d dp）\n", g_snap_on ? "开" : "关", g_snap_band_dp);
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
        // 一键救回：把最大的普通窗口夹进屏幕并居中（对话框按钮通常就在右下角）
        View *v = nullptr;
        for (View *o : g_srv.views) {
            if (!o->mapped || o->is_cursor || o->is_layer || o->is_subsurface) continue;
            if (!v || (int64_t)o->bw * o->bh > (int64_t)v->bw * v->bh) v = o;
        }
        if (!v) { printf("[wm] fit: 没有窗口\n"); return; }
        const int32_t ox = v->x, oy = v->y;
        if (v->bw <= g_out_w) v->x = (g_out_w - v->bw) / 2;
        if (v->bh <= g_out_h) v->y = (g_out_h - v->bh) / 2;
        v->clamp_on_screen();
        printf("[wm] fit '%s' (%d,%d)->(%d,%d) buf=%dx%d 屏幕=%dx%d\n",
               v->title[0] ? v->title : "view", ox, oy, v->x, v->y, v->bw, v->bh, g_out_w, g_out_h);
    } else if (line == "max") {
        // 让最上层的普通窗口铺满输出：关键是**发 configure 告诉客户端尺寸**，
        // 光改 v->x/y 没用（buffer 还是客户端自己定的 1280x720）。
        // 按“面积最大”选，而不是 z 最高 —— 否则会选中 64x64 的光标 surface。
        View *v = nullptr;
        for (View *o : g_srv.views) {
            if (!o->mapped || o->is_cursor || o->is_layer || o->is_subsurface) continue;
            if (!v || (int64_t)o->bw * o->bh > (int64_t)v->bw * v->bh) v = o;
        }
        if (!v) { printf("[wm] max: 没有可最大化的窗口\n"); return; }
        // ★ 必须用客户端的 buf_scale，不能用 g_scale。
        //   g_scale 是 sfwm 自己的 UI 缩放（--scale=3），和客户端无关；
        //   用它去除会把 1272x2800 的屏幕变成 424x933，窗口缩成一小块。
        const int32_t sc = client_scale(v);
        v->x = 0;
        v->y = 0;
        v->req_w = g_out_w / sc;
        v->req_h = g_out_h / sc;
        // 只发尺寸不够：很多客户端（含 nested compositor）只在状态里有 MAXIMIZED 时才采纳。
        v->want_maximized = true;
        toplevel_send_configure(v);
        v->place();
        printf("[wm] max '%s' -> 请求 %dx%d 逻辑（屏幕 %dx%d，scale=%d）\n",
               v->title[0] ? v->title : "view", v->req_w, v->req_h, g_out_w, g_out_h, sc);
    } else if (line.rfind("zoom ", 0) == 0) {
        // 调试：手动模拟「捏合中」的合成器侧缩放（不碰客户端）
        View *v = nullptr;
        for (View *o : g_srv.views) {
            if (!o->mapped || o->is_cursor || o->is_layer || o->is_subsurface) continue;
            if (!v || (int64_t)o->bw * o->bh > (int64_t)v->bw * v->bh) v = o;
        }
        if (!v) { printf("[wm] zoom: 没有窗口\n"); return; }
        float z = (float)atof(line.c_str() + 5);
        if (z < 0.1f) z = 0.1f;
        if (z > 4.0f) z = 4.0f;
        const int32_t cx = v->x + v->bw / 2, cy = v->y + v->bh / 2;
        v->x = cx - (int32_t)(v->bw * z) / 2;
        v->y = cy - (int32_t)(v->bh * z) / 2;
        v->set_zoom(z);
        printf("[wm] zoom %.3f -> '%s' dst=%dx%d @ (%d,%d)，buffer 保持 %dx%d（没发 configure）\n",
               z, v->title[0] ? v->title : "view",
               (int)(v->bw * z), (int)(v->bh * z), v->x, v->y, v->bw, v->bh);
    } else if (line == "zoomend") {
        // 调试：模拟「松手」——发一次 configure 落实尺寸
        View *v = nullptr;
        for (View *o : g_srv.views) {
            if (!o->mapped || o->is_cursor || o->is_layer || o->is_subsurface) continue;
            if (!v || (int64_t)o->bw * o->bh > (int64_t)v->bw * v->bh) v = o;
        }
        if (!v) { printf("[wm] zoomend: 没有窗口\n"); return; }
        const int32_t sc0 = client_scale(v);
        v->req_w = (int32_t)(v->bw * v->zoom) / sc0;
        v->req_h = (int32_t)(v->bh * v->zoom) / sc0;
        if (v->req_w < 120) v->req_w = 120;
        if (v->req_h < 120) v->req_h = 120;
        toplevel_send_configure(v);
        printf("[wm] zoomend zoom=%.3f -> 请求 %dx%d 逻辑（buffer %dx%d）\n",
               v->zoom, v->req_w, v->req_h, v->bw, v->bh);
    } else if (line == "ct") {
        // 点击穿透开关：开了之后我们的图层不再抢触摸，安卓界面完全可点
        g_clickthrough = !g_clickthrough;
        if (g_clickthrough) { g_in.resizing = false; g_in.resize_view = nullptr; g_in.dragging = false; g_in.drag_view = nullptr; g_in.down = false; g_in.pending = 0; }
        for (View *v : g_srv.views) if (v->mapped) v->place();
        printf("[wm] 点击穿透 = %s\n", g_clickthrough ? "开" : "关");
    } else if (line.compare(0, 3, "ct ") == 0) {
        // 注意 "on"/"off" 首字母都是 'o'，必须整词比较
        std::string arg = line.substr(3);
        bool val = g_clickthrough;
        if (arg == "on" || arg == "1" || arg == "true" || arg == "yes") val = true;
        else if (arg == "off" || arg == "0" || arg == "false" || arg == "no") val = false;
        else val = !g_clickthrough;
        g_clickthrough = val;
        if (g_clickthrough) { g_in.resizing = false; g_in.resize_view = nullptr; g_in.dragging = false; g_in.drag_view = nullptr; g_in.down = false; g_in.pending = 0; }
        for (View *v : g_srv.views) if (v->mapped) v->place();
        printf("[wm] 点击穿透 = %s\n", g_clickthrough ? "开" : "关");
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
        printf("[ime] 高度 = %d\n", g_ime_h);
    } else if (line.compare(0, 9, "imestate ") == 0) {
        int st = atoi(s + 9);
        g_ime_on = st != 0;
        if (!g_ime_on) {
            g_ime_h = 0;
            ti_force_disable();     // 键盘被系统收掉 -> 让客户端同步退出输入法焦点
        }
        for (View *v : g_srv.views) if (v->mapped) v->place();
        printf("[ime] 状态 = %s (h=%d)\n", g_ime_on ? "显示" : "隐藏", g_ime_h);
    } else if (line == "hide") {
        // 把合成器输出藏起来（让出屏幕给安卓界面），但进程继续跑、继续收注入
        for (View *v : g_srv.views) if (v->layer.win) v->layer.hide();
        printf("[sfwm] 图层已隐藏\n");
    } else if (line == "show") {
        for (View *v : g_srv.views) if (v->mapped) v->place();
        printf("[sfwm] 图层已恢复\n");
    }
    else if (line == "help") {
        printf("[kbd] 用法: t <文本> | k <evdev码> <0|1> | s <keysym名> | m <x> <y> | c <x> <y> <0|1> | p <x> <y>\n");
    } else {
        kb_type_text(s);   // 裸行 = 直接打字
    }
}

