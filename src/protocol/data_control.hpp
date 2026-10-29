// SPDX-License-Identifier: AGPL-3.0-or-later
// data_control.hpp
//
// The clipboard manager channel:
//   ext_data_control_manager_v1   the current standard, in wayland-protocols staging
//   zwlr_data_control_manager_v1  the wlroots protocol, version 2 adds primary
//
// They differ from wl_data_device in three ways, each of which matters:
//   1. no keyboard focus is required, so history managers such as cliphist see every
//      change;

//   2. the clipboard can be set from the manager side;
//   3. primary selection has its own events and requests.
//
// There is no second clipboard state: g_dd_clip, g_dd_prim and DdSrc from
// data_device.hpp are reused, otherwise a manager and an application would see
// different clipboards.
//   - a manager source is a DdSrc whose send and cancel point at the zwlr or ext code
//   - dd_broadcast() also calls dc_broadcast() for the managers
//
// Requires DdSrc, g_dd_clip, g_dd_prim, g_dd_srcs, dd_src_of, dd_broadcast and
// dd_src_res_destroyed, all from data_device.hpp.
#pragma once

#include "ext-data-control-v1-server-protocol.h"
#include "wlr-data-control-unstable-v1-server-protocol.h"
#include <cstdio>
#include <unistd.h>
#include <vector>

struct DcDev;

struct DcOffer {
    wl_resource *res = nullptr;
    DdSrc *src = nullptr;
    bool zwlr = false;
    int idx = 0;                 // 0 clipboard, 1 primary
    DcDev *dev = nullptr;
};

struct DcDev {
    wl_resource *res = nullptr;
    bool zwlr = false;
    DcOffer *cur[2] = {nullptr, nullptr};
};

static std::vector<DcDev *> g_dc_devs;

static DcDev *dc_dev_of(wl_resource *r) {
    return r ? (DcDev *)wl_resource_get_user_data(r) : nullptr;
}

// ------------------------------------------------------------ offer
static void dc_offer_receive(wl_client *, wl_resource *r, const char *mime, int32_t fd) {
    DcOffer *o = r ? (DcOffer *)wl_resource_get_user_data(r) : nullptr;
    if (o && o->src && o->src->res && mime) {
        if (o->src->send_fn) o->src->send_fn(o->src->res, mime, fd);
        else wl_data_source_send_send(o->src->res, mime, fd);
    } else {
        printf("[dc] receive with no source, dropping\n");
    }
    close(fd);   // libwayland duplicated the fd into the connection
}
static void dc_offer_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct ext_data_control_offer_v1_interface dc_ext_offer_impl = {
    .receive = dc_offer_receive,
    .destroy = dc_offer_destroy,
};
static const struct zwlr_data_control_offer_v1_interface dc_zwlr_offer_impl = {
    .receive = dc_offer_receive,
    .destroy = dc_offer_destroy,
};

static void dc_offer_res_gone(wl_resource *r) {
    DcOffer *o = r ? (DcOffer *)wl_resource_get_user_data(r) : nullptr;
    if (!o) return;
    if (o->dev && o->dev->cur[o->idx] == o) o->dev->cur[o->idx] = nullptr;
    delete o;
}

static void dc_drop_offer(DcDev *d, int idx) {
    if (!d) return;
    DcOffer *o = d->cur[idx];
    if (!o) return;
    d->cur[idx] = nullptr;
    o->dev = nullptr;
    if (o->res) wl_resource_destroy(o->res);
    else delete o;
}

// ------------------------------------------------------------ source
static void dc_src_offer(wl_client *, wl_resource *r, const char *mime) {
    DdSrc *s = dd_src_of(r);
    if (s && mime) s->mimes.push_back(mime);
}
static void dc_src_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct ext_data_control_source_v1_interface dc_ext_src_impl = {
    .offer   = dc_src_offer,
    .destroy = dc_src_destroy,
};
static const struct zwlr_data_control_source_v1_interface dc_zwlr_src_impl = {
    .offer   = dc_src_offer,
    .destroy = dc_src_destroy,
};

static void dc_make_source(wl_client *c, wl_resource *mgr, uint32_t id, bool zwlr) {
    wl_resource *r = wl_resource_create(c,
        zwlr ? &zwlr_data_control_source_v1_interface
             : &ext_data_control_source_v1_interface,
        wl_resource_get_version(mgr), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    DdSrc *s = new DdSrc();
    s->res = r;
    s->primary = false;      // clipboard or primary is decided by set_selection
    s->client = c;
    if (zwlr) {
        s->send_fn   = zwlr_data_control_source_v1_send_send;
        s->cancel_fn = zwlr_data_control_source_v1_send_cancelled;
    } else {
        s->send_fn   = ext_data_control_source_v1_send_send;
        s->cancel_fn = ext_data_control_source_v1_send_cancelled;
    }
    g_dd_srcs.push_back(s);
    wl_resource_set_implementation(r,
        zwlr ? (const void *)&dc_zwlr_src_impl : (const void *)&dc_ext_src_impl,
        s, dd_src_res_destroyed);
}

// ------------------------------------------------------- push an offer to a device
static void dc_push_to(DcDev *d, int idx, DdSrc *src) {
    if (!d || !d->res) return;
    wl_client *cli = wl_resource_get_client(d->res);
    const uint32_t ver = wl_resource_get_version(d->res);
    dc_drop_offer(d, idx);

    // The source client does not receive its own offer; NULL clears the rest.
    bool give = src && !src->mimes.empty() && src->client != cli;
    if (!give) {
        if (idx == 1) {
            if (d->zwlr) { if (ver >= 2) zwlr_data_control_device_v1_send_primary_selection(d->res, nullptr); }
            else ext_data_control_device_v1_send_primary_selection(d->res, nullptr);
        } else {
            if (d->zwlr) zwlr_data_control_device_v1_send_selection(d->res, nullptr);
            else ext_data_control_device_v1_send_selection(d->res, nullptr);
        }
        return;
    }

    DcOffer *o = new DcOffer();
    o->src = src;
    o->zwlr = d->zwlr;
    o->idx = idx;
    o->dev = d;
    // id 0 lets the server allocate the object id.
    o->res = wl_resource_create(cli,
        d->zwlr ? &zwlr_data_control_offer_v1_interface
                : &ext_data_control_offer_v1_interface,
        ver, 0);
    if (!o->res) { delete o; wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(o->res,
        d->zwlr ? (const void *)&dc_zwlr_offer_impl : (const void *)&dc_ext_offer_impl,
        o, dc_offer_res_gone);
    d->cur[idx] = o;

    if (d->zwlr) zwlr_data_control_device_v1_send_data_offer(d->res, o->res);
    else ext_data_control_device_v1_send_data_offer(d->res, o->res);

    for (const std::string &m : src->mimes) {
        if (d->zwlr) zwlr_data_control_offer_v1_send_offer(o->res, m.c_str());
        else ext_data_control_offer_v1_send_offer(o->res, m.c_str());
    }

    if (idx == 1) {
        if (d->zwlr) { if (ver >= 2) zwlr_data_control_device_v1_send_primary_selection(d->res, o->res); }
        else ext_data_control_device_v1_send_primary_selection(d->res, o->res);
    } else {
        if (d->zwlr) zwlr_data_control_device_v1_send_selection(d->res, o->res);
        else ext_data_control_device_v1_send_selection(d->res, o->res);
    }
    printf("[dc] push clipboard to manager %p (%zu MIME types, %s)\n",
           (void *)cli, src->mimes.size(), idx ? "primary" : "clipboard");
}

static void dc_broadcast(bool primary) {
    DdSrc *src = primary ? g_dd_prim : g_dd_clip;
    std::vector<DcDev *> snap = g_dc_devs;
    for (DcDev *d : snap) if (d) dc_push_to(d, primary ? 1 : 0, src);
}

// The source is gone: manager offers are invalidated and cleared with NULL.
static void dc_src_gone(DdSrc *s) {
    if (!s) return;
    for (DcDev *d : g_dc_devs) {
        if (!d) continue;
        for (int i = 0; i < 2; i++)
            if (d->cur[i] && d->cur[i]->src == s) dc_drop_offer(d, i);
    }
}

// ------------------------------------------------------------ device
static void dc_dev_set_sel(DcDev *d, wl_resource *source, bool primary) {
    if (!d) return;
    DdSrc *s = dd_src_of(source);
    DdSrc *old = primary ? g_dd_prim : g_dd_clip;
    if (old && old != s && old->cancel_fn) old->cancel_fn(old->res);
    if (primary) g_dd_prim = s; else g_dd_clip = s;
    printf("[dc] %s <- %s (%zu MIME types)\n", primary ? "primary" : "clipboard",
           s ? "manager" : "(cleared)", s ? s->mimes.size() : 0);
    dd_broadcast(primary);      // calls dc_broadcast internally
}

static void dc_dev_res_gone(wl_resource *r) {
    DcDev *d = dc_dev_of(r);
    if (!d) return;
    for (size_t i = 0; i < g_dc_devs.size(); i++)
        if (g_dc_devs[i] == d) { g_dc_devs.erase(g_dc_devs.begin() + i); break; }
    dc_drop_offer(d, 0);
    dc_drop_offer(d, 1);
    delete d;
}

// ext variant
static void dce_set_selection(wl_client *, wl_resource *r, wl_resource *source) {
    dc_dev_set_sel(dc_dev_of(r), source, false);
}
static void dce_set_primary(wl_client *, wl_resource *r, wl_resource *source) {
    dc_dev_set_sel(dc_dev_of(r), source, true);
}
static void dce_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct ext_data_control_device_v1_interface dce_impl = {
    .set_selection         = dce_set_selection,
    .destroy               = dce_destroy,
    .set_primary_selection = dce_set_primary,
};

// zwlr variant, same semantics with different names
static void dcz_set_selection(wl_client *, wl_resource *r, wl_resource *source) {
    dc_dev_set_sel(dc_dev_of(r), source, false);
}
static void dcz_set_primary(wl_client *, wl_resource *r, wl_resource *source) {
    dc_dev_set_sel(dc_dev_of(r), source, true);
}
static void dcz_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct zwlr_data_control_device_v1_interface dcz_impl = {
    .set_selection         = dcz_set_selection,
    .destroy               = dcz_destroy,
    .set_primary_selection = dcz_set_primary,
};

static void dc_make_device(wl_client *c, wl_resource *mgr, uint32_t id, bool zwlr) {
    wl_resource *r = wl_resource_create(c,
        zwlr ? &zwlr_data_control_device_v1_interface
             : &ext_data_control_device_v1_interface,
        wl_resource_get_version(mgr), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    DcDev *d = new DcDev();
    d->res = r;
    d->zwlr = zwlr;
    g_dc_devs.push_back(d);
    wl_resource_set_implementation(r,
        zwlr ? (const void *)&dcz_impl : (const void *)&dce_impl,
        d, dc_dev_res_gone);
    // Send the current selection right after the bind.
    dc_push_to(d, 0, g_dd_clip);
    dc_push_to(d, 1, g_dd_prim);
    printf("[dc] new data control device (%s)\n", zwlr ? "zwlr" : "ext");
}

// ------------------------------------------------------------ manager
static void dce_mgr_create_source(wl_client *c, wl_resource *m, uint32_t id) {
    dc_make_source(c, m, id, false);
}
static void dce_mgr_get_device(wl_client *c, wl_resource *m, uint32_t id, wl_resource *) {
    dc_make_device(c, m, id, false);
}
static void dce_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct ext_data_control_manager_v1_interface dce_mgr_impl = {
    .create_data_source = dce_mgr_create_source,
    .get_data_device    = dce_mgr_get_device,
    .destroy            = dce_mgr_destroy,
};

static void dcz_mgr_create_source(wl_client *c, wl_resource *m, uint32_t id) {
    dc_make_source(c, m, id, true);
}
static void dcz_mgr_get_device(wl_client *c, wl_resource *m, uint32_t id, wl_resource *) {
    dc_make_device(c, m, id, true);
}
static void dcz_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct zwlr_data_control_manager_v1_interface dcz_mgr_impl = {
    .create_data_source = dcz_mgr_create_source,
    .get_data_device    = dcz_mgr_get_device,
    .destroy            = dcz_mgr_destroy,
};

static void bind_ext_data_control(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &ext_data_control_manager_v1_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &dce_mgr_impl, nullptr, nullptr);
    printf("[dc] ext_data_control_manager_v1 bound\n");
}
static void bind_wlr_data_control(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 2) version = 2;
    wl_resource *r = wl_resource_create(c, &zwlr_data_control_manager_v1_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &dcz_mgr_impl, nullptr, nullptr);
    printf("[dc] zwlr_data_control_manager_v1 bound\n");
}
