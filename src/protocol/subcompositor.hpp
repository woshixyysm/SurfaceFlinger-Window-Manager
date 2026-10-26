// subcompositor.hpp
//
// wl_subcompositor（Qt/OBS 的内容面在这里）
//
// 从 sfwm.cpp 原样搬出（只搬不改，逻辑零差异）。
// 依赖包含顺序：本文件用到的东西必须由**前面的**模块提供，
// 因此它在 sfwm.cpp 里的 include 位置就是它原来在文件里的位置。
#pragma once

// ---------------------------------------------------------------- wl_subcompositor
// OBS 的预览区（program/preview monitor）不是普通 Qt 控件，而是一条独立子系统表面：
// 客户端在它上面建 wl_egl_window 自己用 GL 渲染，再附到某个 wl_surface 的 sub-surface 上。
// 没有这个全局对象，OBS 会报 "Can't create subsurface, not supported by the compositor"，
// 然后整个预览区永远是黑的。
struct SubsurfData { View *v = nullptr; };

static void sub_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static void sub_set_position(wl_client *, wl_resource *r, int32_t x, int32_t y) {
    SubsurfData *d = (SubsurfData *)wl_resource_get_user_data(r);
    if (!d || !d->v) return;
    d->v->sub_x = x;
    d->v->sub_y = y;
    if (d->v->mapped) d->v->place();
}

// 一律当成“在父之上”，OBS 只有一条预览子系统，不需要真正的兄弟排序
static void sub_place_above(wl_client *, wl_resource *, wl_resource *) {}
static void sub_place_below(wl_client *, wl_resource *, wl_resource *) {}

// sync 模式需要父提交时才生效；我们直接立即生效（desync 语义），
// 对单个预览表面没有撕裂风险，但少一层状态机。
static void sub_set_sync(wl_client *, wl_resource *) {}
static void sub_set_desync(wl_client *, wl_resource *) {}

static const struct wl_subsurface_interface subsurface_impl = {
    .destroy = sub_destroy,
    .set_position = sub_set_position,
    .place_above = sub_place_above,
    .place_below = sub_place_below,
    .set_sync = sub_set_sync,
    .set_desync = sub_set_desync,
};

static void subcompositor_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static void subcompositor_get_subsurface(wl_client *c, wl_resource *res, uint32_t id,
                                         wl_resource *surface, wl_resource *parent) {
    View *v = (View *)wl_resource_get_user_data(surface);
    View *p = (View *)wl_resource_get_user_data(parent);
    if (!v || !p || v == p) {
        wl_resource_post_error(res, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE, "子表面参数非法");
        return;
    }
    wl_resource *r = wl_resource_create(c, &wl_subsurface_interface,
                                        wl_resource_get_version(res), id);
    if (!r) { wl_client_post_no_memory(c); return; }

    v->is_subsurface = true;
    v->sub_parent = p;
    v->z = p->z;
    p->subs.push_back(v);

    SubsurfData *d = new SubsurfData();
    d->v = v;
    wl_resource_set_implementation(r, &subsurface_impl, d, [](wl_resource *res2) {
        SubsurfData *dd = (SubsurfData *)wl_resource_get_user_data(res2);
        if (dd && dd->v) {
            View *vv = dd->v;
            vv->is_subsurface = false;
            if (vv->sub_parent) {
                auto &s = vv->sub_parent->subs;
                for (size_t i = 0; i < s.size(); i++)
                    if (s[i] == vv) { s.erase(s.begin() + i); break; }
                vv->sub_parent = nullptr;
            }
        }
        delete dd;
    });
    printf("[sfwm] 建立 subsurface（预览表面）\n");
}

static const struct wl_subcompositor_interface subcompositor_impl = {
    .destroy = subcompositor_destroy,
    .get_subsurface = subcompositor_get_subsurface,
};

static void bind_subcompositor(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &wl_subcompositor_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &subcompositor_impl, nullptr, nullptr);
}

