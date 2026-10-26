// activation.hpp
//
// xdg_activation_v1
//
// 从 sfwm.cpp 原样搬出（只搬不改，逻辑零差异）。
// 依赖包含顺序：本文件用到的东西必须由**前面的**模块提供，
// 因此它在 sfwm.cpp 里的 include 位置就是它原来在文件里的位置。
#pragma once

// ---------------------------------------------------------------- xdg_activation_v1
// 应用启动时用它抢焦点（没有这个，点启动器起来的窗口经常不聚焦）。
static void act_token_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void act_token_set_serial(wl_client *, wl_resource *, uint32_t, wl_resource *) {}
static void act_token_set_app_id(wl_client *, wl_resource *, const char *) {}
static void act_token_set_surface(wl_client *, wl_resource *, wl_resource *) {}

static void act_token_commit(wl_client *, wl_resource *r) {
    static uint32_t seq = 0;
    char tok[32];
    snprintf(tok, sizeof(tok), "sfwm-token-%u", ++seq);
    xdg_activation_token_v1_send_done(r, tok);
}

static const struct xdg_activation_token_v1_interface act_token_impl = {
    .set_serial   = act_token_set_serial,
    .set_app_id   = act_token_set_app_id,
    .set_surface  = act_token_set_surface,
    .commit       = act_token_commit,
    .destroy      = act_token_destroy,
};

static void act_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static void act_activate(wl_client *, wl_resource *, const char *token, wl_resource *surface) {
    View *v = surface ? view_from_surface(surface) : nullptr;
    if (!v || !v->mapped) return;

    int32_t top = 0;
    for (View *o : g_srv.views)
        if (o->mapped && !o->is_layer && !o->is_cursor && o->z > top) top = o->z;
    v->z = top + 1;
    v->place();
    kb_set_focus(v);
    printf("[activation] 激活 '%s'\n", v->title[0] ? v->title : "view");
}

static void act_get_token(wl_client *c, wl_resource *res, uint32_t id) {
    wl_resource *r = wl_resource_create(c, &xdg_activation_token_v1_interface, wl_resource_get_version(res), id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &act_token_impl, nullptr, nullptr);
}

static const struct xdg_activation_v1_interface act_impl = {
    .destroy              = act_destroy,
    .get_activation_token = act_get_token,
    .activate             = act_activate,
};

static void bind_activation(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &xdg_activation_v1_interface, version, id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &act_impl, nullptr, nullptr);
}

