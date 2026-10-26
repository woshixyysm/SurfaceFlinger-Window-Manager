// layer_shell.hpp
//
// zwlr_layer_shell_v1（面板/托盘）
//
// 从 sfwm.cpp 原样搬出（只搬不改，逻辑零差异）。
// 依赖包含顺序：本文件用到的东西必须由**前面的**模块提供，
// 因此它在 sfwm.cpp 里的 include 位置就是它原来在文件里的位置。
#pragma once

// ---------------------------------------------------------------- zwlr_layer_shell_v1
// 面板 / 托盘 / 启动器（quickshell、waybar、fuzzel、swaybg、mako）全靠它。
// 我们不做真正的「层」，只把 anchor + margin 算成屏幕坐标，然后当普通 View 上屏；
// 层序靠 z 分档：background < 普通窗口 < top < overlay。

static int32_t layer_z_for(uint32_t layer) {
    switch (layer) {
    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: return -3000;
    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:     return -2000;
    case ZWLR_LAYER_SHELL_V1_LAYER_TOP:        return 500;
    case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:    return 900;
    default:                                   return 500;
    }
}

// 按 anchor/margin 算位置。尺寸优先用已到手的 buffer，其次用客户端请求值。
static void layer_apply_anchor(View *v) {
    if (!v || !v->is_layer) return;
    g_reason = "layer";

    const int32_t  ow = g_out_w, oh = g_out_h;
    const uint32_t A = v->layer_anchor;
    const bool     a_top = (A & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) != 0;
    const bool     a_bot = (A & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) != 0;
    const bool     a_lef = (A & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) != 0;
    const bool     a_rig = (A & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) != 0;

    int32_t w = v->bw > 0 ? v->bw : v->layer_w;
    int32_t h = v->bh > 0 ? v->bh : v->layer_h;
    if (w <= 0) w = (a_lef && a_rig) ? ow - v->layer_ml - v->layer_mr : ow;
    if (h <= 0) h = (a_top && a_bot) ? oh - v->layer_mt - v->layer_mb : oh;
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;

    if (a_lef && a_rig)      v->x = v->layer_ml;
    else if (a_rig)          v->x = ow - w - v->layer_mr;
    else if (a_lef)          v->x = v->layer_ml;
    else                     v->x = (ow - w) / 2;

    if (a_top && a_bot)      v->y = v->layer_mt;
    else if (a_bot)          v->y = oh - h - v->layer_mb;
    else if (a_top)          v->y = v->layer_mt;
    else                     v->y = (oh - h) / 2;

    if (v->x < 0) v->x = 0;
    if (v->y < 0) v->y = 0;
    v->place();
}

static void layer_send_configure(View *v) {
    if (!v || !v->layer_surface) return;

    const int32_t  ow = g_out_w, oh = g_out_h;
    const uint32_t A = v->layer_anchor;
    const bool     a_top = (A & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) != 0;
    const bool     a_bot = (A & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) != 0;
    const bool     a_lef = (A & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) != 0;
    const bool     a_rig = (A & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) != 0;

    int32_t w = v->layer_w, h = v->layer_h;
    if (w <= 0) w = (a_lef && a_rig) ? ow - v->layer_ml - v->layer_mr : ow;
    if (h <= 0) h = (a_top && a_bot) ? oh - v->layer_mt - v->layer_mb : oh;
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;

    zwlr_layer_surface_v1_send_configure(v->layer_surface, wl_display_next_serial(g_srv.dpy), (uint32_t)w, (uint32_t)h);
    layer_apply_anchor(v);
}

static void lsurf_set_size(wl_client *, wl_resource *r, uint32_t w, uint32_t h) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    v->layer_w = (int32_t)w;
    v->layer_h = (int32_t)h;
    layer_send_configure(v);
}

static void lsurf_set_anchor(wl_client *, wl_resource *r, uint32_t anchor) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    v->layer_anchor = anchor;
    layer_send_configure(v);
}

static void lsurf_set_exclusive_zone(wl_client *, wl_resource *r, int32_t zone) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    v->layer_excl = zone;
}

static void lsurf_set_margin(wl_client *, wl_resource *r, int32_t t, int32_t rr, int32_t b, int32_t l) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    v->layer_mt = t;
    v->layer_mr = rr;
    v->layer_mb = b;
    v->layer_ml = l;
    layer_send_configure(v);
}

static void lsurf_set_keyboard_interactivity(wl_client *, wl_resource *r, uint32_t kb) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    v->layer_kb = kb;
    if (kb != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE && v->mapped)
        kb_set_focus(v);
}

// 层表面的 popup（下拉菜单）：目前不实现真正的 grab，客户端会自己兜底
static void lsurf_get_popup(wl_client *, wl_resource *, wl_resource *) {}

static void lsurf_ack_configure(wl_client *, wl_resource *, uint32_t) {}

static void lsurf_set_layer(wl_client *, wl_resource *r, uint32_t layer) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    v->layer_num = layer;
    v->z         = layer_z_for(layer);
    if (v->mapped) v->place();
}

static void lsurf_destroy(wl_client *, wl_resource *r) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (v) v->layer_surface = nullptr;
    wl_resource_destroy(r);
}

static const struct zwlr_layer_surface_v1_interface lsurf_impl = {
    .set_size                 = lsurf_set_size,
    .set_anchor               = lsurf_set_anchor,
    .set_exclusive_zone       = lsurf_set_exclusive_zone,
    .set_margin               = lsurf_set_margin,
    .set_keyboard_interactivity = lsurf_set_keyboard_interactivity,
    .get_popup                = lsurf_get_popup,
    .ack_configure            = lsurf_ack_configure,
    .destroy                  = lsurf_destroy,
    .set_layer                = lsurf_set_layer,
};

static void lshell_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static void lshell_get_layer_surface(wl_client *c, wl_resource *res, uint32_t id, wl_resource *surface, wl_resource *output,
                                     uint32_t layer, const char *ns) {
    View *v = view_from_surface(surface);
    if (!v) {
        wl_resource_post_error(res, ZWLR_LAYER_SHELL_V1_ERROR_ROLE, "layer_surface on unknown wl_surface");
        return;
    }
    if (v->is_layer || v->xdg_surface) {
        wl_resource_post_error(res, ZWLR_LAYER_SHELL_V1_ERROR_ROLE, "surface already has a role");
        return;
    }

    wl_resource *lr = wl_resource_create(c, &zwlr_layer_surface_v1_interface, wl_resource_get_version(res), id);
    if (!lr) {
        wl_client_post_no_memory(c);
        return;
    }

    v->is_layer      = true;
    v->layer_surface = lr;
    v->layer_num     = layer;
    v->z             = layer_z_for(layer);
    snprintf(v->layer_ns, sizeof(v->layer_ns), "%s", ns ? ns : "");
    if (!v->title[0])
        snprintf(v->title, sizeof(v->title), "layer:%s", v->layer_ns);
    wl_resource_set_implementation(lr, &lsurf_impl, v, nullptr);

    printf("[layer-shell] %s layer=%u\n", v->layer_ns, layer);
    layer_send_configure(v);
}

static const struct zwlr_layer_shell_v1_interface lshell_impl = {
    .get_layer_surface = lshell_get_layer_surface,
    .destroy           = lshell_destroy,
};

static void bind_lshell(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 4) version = 4;
    wl_resource *r = wl_resource_create(c, &zwlr_layer_shell_v1_interface, version, id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &lshell_impl, nullptr, nullptr);
}

