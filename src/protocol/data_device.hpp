// SPDX-License-Identifier: AGPL-3.0-or-later
// data_device.hpp - clipboard and primary selection.
//
// Implements:
//   wl_data_device_manager (v3)               copy and paste
//   zwp_primary_selection_device_manager_v1   middle-click paste
//
// Wayland clipboard semantics: the source offers formats and the target pulls the data.
//   1. the copying client creates a data source and advertises its MIME types;
//   2. this compositor keeps the source and sends data_offer plus selection to the data
//      devices of the other clients;
//   3. the pasting client calls receive() with the write end of a pipe, which is passed
//      straight to the source. The data flows between the two clients and never enters
//      compositor memory, so any size and any format works.
//
// Drag and drop is not implemented: start_drag is ignored, which leaves clipboard-only
// clients unaffected.
#pragma once

#include <string>
#include <vector>

// ---- state ----
struct DdSrc {                       // a content provider: wl_data_source or its primary variant
    wl_resource *res = nullptr;
    bool primary = false;
    wl_client *client = nullptr;
    std::vector<std::string> mimes;
    // One clipboard model serves four protocols: wl_data_source,
    // zwp_primary_selection_source_v1, ext_data_control_source_v1 and
    // zwlr_data_control_source_v1. send and cancelled are dispatched through
    // function pointers, so no header included later is needed here.
    void (*send_fn)(wl_resource *, const char *, int32_t) = nullptr;
    void (*cancel_fn)(wl_resource *) = nullptr;
};

struct DdOffer {                     // the wl_data_offer sent to one client
    wl_resource *res = nullptr;
    DdSrc *src = nullptr;
    bool primary = false;
    std::string accepted;            // MIME type chosen in accept(), recorded only
};

struct DdDev {                       // the data device of one client
    bool primary = false;
    bool focused = false;               // selection is only sent to focused clients, as Weston does
    wl_resource *cur_offer = nullptr;   // offer currently held; destroyed when replaced
};

static std::vector<DdSrc *> g_dd_srcs;
static std::vector<DdOffer *> g_dd_offers;
static std::vector<std::pair<wl_resource *, DdDev *>> g_dd_devs;
static DdSrc *g_dd_clip = nullptr;    // current clipboard source
static DdSrc *g_dd_prim = nullptr;    // current primary selection

static DdSrc *dd_src_of(wl_resource *r) {
    return r ? (DdSrc *)wl_resource_get_user_data(r) : nullptr;
}
static DdOffer *dd_offer_of(wl_resource *r) {
    return r ? (DdOffer *)wl_resource_get_user_data(r) : nullptr;
}

static void dd_broadcast(bool primary);
// Provided by data_control.hpp: clipboard managers also receive every change,
// and their offers are invalidated when the source is destroyed.
static void dc_broadcast(bool primary);
static void dc_src_gone(DdSrc *s);

// ---- wl_data_offer / zwp_primary_selection_offer_v1 ----------------------
static void dd_offer_accept(wl_client *, wl_resource *r, uint32_t, const char *mime) {
    DdOffer *o = dd_offer_of(r);
    if (o) o->accepted = mime ? mime : "";
}
static void dd_offer_receive(wl_client *, wl_resource *r, const char *mime, int32_t fd) {
    DdOffer *o = dd_offer_of(r);
    // Pass the pipe to the source: the data moves between the two clients.
    if (o && o->src && o->src->res && mime) {
        if (o->src->send_fn) o->src->send_fn(o->src->res, mime, fd);
        else if (o->src->primary) zwp_primary_selection_source_v1_send_send(o->src->res, mime, fd);
        else wl_data_source_send_send(o->src->res, mime, fd);
    } else {
        printf("[clip] receive with no available source, dropping\n");
    }
    close(fd);   // libwayland duplicated the fd into the connection, so close this copy
}
static void dd_offer_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void dd_offer_set_actions(wl_client *, wl_resource *r, uint32_t, uint32_t) {}
static void dd_offer_res_destroyed(wl_resource *r) {
    DdOffer *o = dd_offer_of(r);
    if (!o) return;
    for (size_t i = 0; i < g_dd_offers.size(); i++)
        if (g_dd_offers[i] == o) { g_dd_offers.erase(g_dd_offers.begin() + i); break; }
    // clear the device pointer if it still refers to this offer
    for (auto &d : g_dd_devs) {
        DdDev *st = d.second;
        if (st && st->cur_offer == r) st->cur_offer = nullptr;
    }
    delete o;
}

static const struct wl_data_offer_interface dd_doffer_impl = {
    .accept = dd_offer_accept,
    .receive = dd_offer_receive,
    .destroy = dd_offer_destroy,
    .finish = dd_offer_destroy,
    .set_actions = dd_offer_set_actions,
};
static const struct zwp_primary_selection_offer_v1_interface dd_poffer_impl = {
    .receive = dd_offer_receive,
    .destroy = dd_offer_destroy,
};

// ---- wl_data_source / zwp_primary_selection_source_v1 -------------------
static void dd_src_offer(wl_client *, wl_resource *r, const char *mime) {
    DdSrc *s = dd_src_of(r);
    if (s && mime) s->mimes.push_back(mime);
}
static void dd_src_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void dd_src_set_actions(wl_client *, wl_resource *, uint32_t) {}
static void dd_src_res_destroyed(wl_resource *r) {
    DdSrc *s = dd_src_of(r);
    if (!s) return;
    bool was_clip = (g_dd_clip == s);
    bool was_prim = (g_dd_prim == s);
    for (size_t i = 0; i < g_dd_srcs.size(); i++)
        if (g_dd_srcs[i] == s) { g_dd_srcs.erase(g_dd_srcs.begin() + i); break; }
    if (g_dd_clip == s) g_dd_clip = nullptr;
    if (g_dd_prim == s) g_dd_prim = nullptr;
    dc_src_gone(s);   // invalidate the offers held by clipboard managers
    delete s;
    // Without a source every target is cleared, otherwise they paste stale content.
    if (was_clip) dd_broadcast(false);
    if (was_prim) dd_broadcast(true);
}

static const struct wl_data_source_interface dd_dsrc_impl = {
    .offer = dd_src_offer,
    .destroy = dd_src_destroy,
    .set_actions = dd_src_set_actions,
};
static const struct zwp_primary_selection_source_v1_interface dd_psrc_impl = {
    .offer = dd_src_offer,
    .destroy = dd_src_destroy,
};

// ---- push the current selection to one client ----
static void dd_push_to(wl_resource *dev, DdDev *st, DdSrc *src) {
    if (!st) return;
    wl_client *cli = wl_resource_get_client(dev);
    // Selection is only sent to clients with keyboard focus, as Weston does.
    // Sending selection(NULL) at bind time, before enter, drives some clients
    // such as Qt into an uninitialised path.
    if (!st->focused) return;
    uint32_t ver = wl_resource_get_version(dev);

    if (st->cur_offer) {                       // withdraw the previous offer
        wl_resource *old = st->cur_offer;
        st->cur_offer = nullptr;
        wl_resource_destroy(old);
    }

    // The source client does not receive its own offer; selection(NULL) clears the rest.
    bool give = src && !src->mimes.empty() && src->client != cli;
    if (!give) {
            if (st->primary) zwp_primary_selection_device_v1_send_selection(dev, nullptr);
                else wl_data_device_send_selection(dev, nullptr);
        return;
    }

    DdOffer *o = new DdOffer();
    o->src = src;
    o->primary = st->primary;
    // id 0 lets the server allocate the object id
    o->res = wl_resource_create(cli,
                                st->primary ? &zwp_primary_selection_offer_v1_interface
                                            : &wl_data_offer_interface,
                                ver, 0);
    if (!o->res) { delete o; wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(o->res,
                                   st->primary ? (const void *)&dd_poffer_impl
                                               : (const void *)&dd_doffer_impl,
                                   o, dd_offer_res_destroyed);
    g_dd_offers.push_back(o);

    if (st->primary) {
        for (const std::string &m : src->mimes)
            zwp_primary_selection_offer_v1_send_offer(o->res, m.c_str());
        zwp_primary_selection_device_v1_send_selection(dev, o->res);
    } else {
        for (const std::string &m : src->mimes)
            wl_data_offer_send_offer(o->res, m.c_str());
        wl_data_device_send_selection(dev, o->res);
    }
    st->cur_offer = o->res;
    printf("[clip] push to client %p: %zu MIME types (%s)\n", (void *)cli, src->mimes.size(),
           src->mimes[0].c_str());
}

static void dd_broadcast(bool primary) {
    DdSrc *src = primary ? g_dd_prim : g_dd_clip;
    std::vector<std::pair<wl_resource *, DdDev *>> snapshot = g_dd_devs;   // callbacks may modify the container
    for (auto &d : snapshot)
        if (d.second && d.second->primary == primary) dd_push_to(d.first, d.second, src);
    // Clipboard managers take the other path: they receive everything, focused or not.
    dc_broadcast(primary);
}

// ---- wl_data_device / zwp_primary_selection_device_v1 -------------------
static void dd_dev_set_selection(wl_client *, wl_resource *r, wl_resource *source, uint32_t) {
    DdDev *st = (DdDev *)wl_resource_get_user_data(r);
    bool primary = st ? st->primary : false;
    DdSrc *s = dd_src_of(source);
    DdSrc *old = primary ? g_dd_prim : g_dd_clip;
    if (old && old != s) {
        // Tell the replaced source that it no longer owns the selection.
        if (old->cancel_fn) old->cancel_fn(old->res);
        else if (old->primary) zwp_primary_selection_source_v1_send_cancelled(old->res);
        else wl_data_source_send_cancelled(old->res);
    }
    if (primary) g_dd_prim = s; else g_dd_clip = s;
    printf("[clip] %s selection <- %s (%zu MIME types)\n",
           primary ? "primary" : "clipboard",
           s ? "new source" : "(cleared)", s ? s->mimes.size() : 0);
    dd_broadcast(primary);
}
static void dd_dev_start_drag(wl_client *, wl_resource *, wl_resource *, wl_resource *,
                              wl_resource *, uint32_t) {
    // No drag and drop: cancelling the source is the safest response.
}
static void dd_dev_release(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void dd_dev_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wl_data_device_interface dd_ddev_impl = {
    .start_drag = dd_dev_start_drag,
    .set_selection = dd_dev_set_selection,
    .release = dd_dev_release,
};
static const struct zwp_primary_selection_device_v1_interface dd_pdev_impl = {
    .set_selection = dd_dev_set_selection,
    .destroy = dd_dev_destroy,
};

static void dd_dev_res_destroyed(wl_resource *r) {
    for (size_t i = 0; i < g_dd_devs.size(); i++) {
        if (g_dd_devs[i].first == r) {
            delete g_dd_devs[i].second;
            g_dd_devs.erase(g_dd_devs.begin() + i);
            break;
        }
    }
}

// ---- factories ----
static void dd_create_source(bool primary, wl_client *c, wl_resource *mgr, uint32_t id) {
    uint32_t ver = wl_resource_get_version(mgr);
    wl_resource *r = wl_resource_create(
        c, primary ? &zwp_primary_selection_source_v1_interface : &wl_data_source_interface,
        ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    DdSrc *s = new DdSrc();
    s->res = r;
    s->primary = primary;
    s->client = c;
    if (primary) {
        s->send_fn   = zwp_primary_selection_source_v1_send_send;
        s->cancel_fn = zwp_primary_selection_source_v1_send_cancelled;
    } else {
        s->send_fn   = wl_data_source_send_send;
        s->cancel_fn = wl_data_source_send_cancelled;
    }
    g_dd_srcs.push_back(s);
    wl_resource_set_implementation(r, primary ? (const void *)&dd_psrc_impl
                                              : (const void *)&dd_dsrc_impl,
                                   s, dd_src_res_destroyed);
}

static void dd_get_device(bool primary, wl_client *c, wl_resource *mgr, uint32_t id) {
    uint32_t ver = wl_resource_get_version(mgr);
    wl_resource *r = wl_resource_create(
        c, primary ? &zwp_primary_selection_device_v1_interface : &wl_data_device_interface,
        ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    DdDev *st = new DdDev();
    st->primary = primary;
    g_dd_devs.push_back({r, st});
    wl_resource_set_implementation(r, primary ? (const void *)&dd_pdev_impl
                                              : (const void *)&dd_ddev_impl,
                                   st, dd_dev_res_destroyed);
    // Send the current selection to the new device immediately.
    dd_push_to(r, st, primary ? g_dd_prim : g_dd_clip);
}

static void ddm_create_source(wl_client *c, wl_resource *m, uint32_t id) {
    dd_create_source(false, c, m, id);
}
static void ddm_get_device(wl_client *c, wl_resource *m, uint32_t id, wl_resource *) {
    dd_get_device(false, c, m, id);
}
static const struct wl_data_device_manager_interface ddm_impl = {
    .create_data_source = ddm_create_source,
    .get_data_device = ddm_get_device,
};
static void psel_mgr_create_source(wl_client *c, wl_resource *m, uint32_t id) {
    dd_create_source(true, c, m, id);
}
static void psel_mgr_get_device(wl_client *c, wl_resource *m, uint32_t id, wl_resource *) {
    dd_get_device(true, c, m, id);
}
static void psel_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct zwp_primary_selection_device_manager_v1_interface psel_mgr_impl = {
    .create_source = psel_mgr_create_source,
    .get_device = psel_mgr_get_device,
    .destroy = psel_mgr_destroy,
};

static void bind_datadev(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 3) version = 3;
    wl_resource *r = wl_resource_create(c, &wl_data_device_manager_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &ddm_impl, nullptr, nullptr);
}
static void bind_primsel(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > zwp_primary_selection_device_manager_v1_interface.version)
        version = zwp_primary_selection_device_manager_v1_interface.version;
    wl_resource *r = wl_resource_create(c, &zwp_primary_selection_device_manager_v1_interface,
                                        version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &psel_mgr_impl, nullptr, nullptr);
}

// ---- focus change: push the clipboard to the newly focused client ----
// Primary selection does not follow focus, so only the clipboard is pushed.
static void dd_focus_changed(View *v) {
    wl_client *fc = (v && v->surface) ? wl_resource_get_client(v->surface) : nullptr;
    std::vector<std::pair<wl_resource *, DdDev *>> snapshot = g_dd_devs;
    for (auto &d : snapshot) {
        DdDev *st = d.second;
        if (!st || st->primary) continue;
        bool focused = fc && wl_resource_get_client(d.first) == fc;
        st->focused = focused;
        // Clients losing focus get selection(NULL) so they cannot paste stale content.
        dd_push_to(d.first, st, focused ? g_dd_clip : nullptr);
    }
}
