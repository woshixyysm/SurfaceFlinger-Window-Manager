// SPDX-License-Identifier: AGPL-3.0-or-later
// subcompositor.hpp
//
// wl_subcompositor, which carries the content surface of Qt and OBS.
//
// Extracted from the former single-file implementation; behaviour is unchanged.
// Depends on include order: everything used here is provided by the modules
// included before it in src/main.cpp.
#pragma once

// ---------------------------------------------------------------- wl_subcompositor
// The OBS preview is not a widget but its own subsurface: the client renders into it with
// GL through a wl_egl_window and attaches it to a parent surface. Without this global OBS
// reports that subsurfaces are unsupported and the preview stays black.
struct SubsurfData { View *v = nullptr; };

static void sub_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static void sub_set_position(wl_client *, wl_resource *r, int32_t x, int32_t y) {
    SubsurfData *d = (SubsurfData *)wl_resource_get_user_data(r);
    if (!d || !d->v) return;
    d->v->sub_x = x;
    d->v->sub_y = y;
    if (d->v->mapped) d->v->place();
}

// Everything is placed above the parent; OBS has a single preview subsurface.
static void sub_place_above(wl_client *, wl_resource *, wl_resource *) {}
static void sub_place_below(wl_client *, wl_resource *, wl_resource *) {}

// The sync mode would apply changes on the parent commit; changes apply immediately
// here, which removes a state machine without tearing the single preview surface.
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
        wl_resource_post_error(res, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE, "invalid subsurface arguments");
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
    printf("[sfwm] created subsurface (preview surface)\n");
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

