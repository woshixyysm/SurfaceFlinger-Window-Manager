// SPDX-License-Identifier: AGPL-3.0-or-later
// security_context.hpp
//
// wp_security_context_manager_v1, the identity channel for sandboxed clients.
// （wayland-protocols staging/security-context）。
//
// It works by passing sockets, not by declaring identity:
//   1. the sandbox host binds a unix listening socket and passes two fds with
//      create_listener: the listening fd the compositor accepts on, and a close fd
//      it closes once the metadata is handled, which tells the host it worked;
//        listen_fd  accepted for the sandboxed application
//        close_fd    closed when the metadata is handled
//   2. the host sets the engine, app id and instance id;
//   3. after commit the application connects to listen_fd and the accepted
//      connection becomes its Wayland client.
//
// The commit therefore has to accept on listen_fd and create a wl_client,
// otherwise the sandboxed application can never connect.
// The metadata is only logged; there is no per-application isolation here.
//
// Requires g_srv.dpy and g_srv.loop, so it comes after core/config.hpp.
#pragma once

#include "security-context-v1-server-protocol.h"
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

struct SecCtx {
    wl_resource *res = nullptr;
    char engine[64]   = {0};
    char app_id[128]  = {0};
    char inst_id[128] = {0};
    int listen_fd = -1;
    int close_fd  = -1;
    bool committed = false;
    wl_event_source *src = nullptr;
    int accepted = 0;
};
static std::vector<SecCtx *> g_seccx;

static SecCtx *sc_of(wl_resource *r) {
    return r ? (SecCtx *)wl_resource_get_user_data(r) : nullptr;
}

// A sandboxed application connected; each connection is a new wl_client.
static int sec_on_conn(int fd, uint32_t, void *data) {
    SecCtx *sc = (SecCtx *)data;
    for (;;) {
        int c = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (c < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            if (errno == EINTR) continue;
            fprintf(stderr, "[secctx] accept failed: %s\n", strerror(errno));
            return 0;
        }
        wl_client *cli = wl_client_create(g_srv.dpy, c);
        if (!cli) {
            fprintf(stderr, "[secctx] wl_client_create failed: %s\n", strerror(errno));
            close(c);
            continue;
        }
        sc->accepted++;
        printf("[secctx] sandbox client connected (engine=%s app_id=%s instance=%s), number %d\n",
               sc->engine[0] ? sc->engine : "?",
               sc->app_id[0] ? sc->app_id : "?",
               sc->inst_id[0] ? sc->inst_id : "?", sc->accepted);
    }
}

// ------------------------------------------------------- wp_security_context_v1
static void sc_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static void sc_set_engine(wl_client *, wl_resource *r, const char *name) {
    SecCtx *sc = sc_of(r);
    if (!sc || !name) return;
    if (sc->engine[0]) { wl_resource_post_error(r, WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_SET,
                                                "sandbox_engine already set"); return; }
    snprintf(sc->engine, sizeof(sc->engine), "%s", name);
}

static void sc_set_app_id(wl_client *, wl_resource *r, const char *v) {
    SecCtx *sc = sc_of(r);
    if (!sc || !v) return;
    if (sc->app_id[0]) { wl_resource_post_error(r, WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_SET,
                                                "app_id already set"); return; }
    snprintf(sc->app_id, sizeof(sc->app_id), "%s", v);
}

static void sc_set_instance_id(wl_client *, wl_resource *r, const char *v) {
    SecCtx *sc = sc_of(r);
    if (!sc || !v) return;
    if (sc->inst_id[0]) { wl_resource_post_error(r, WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_SET,
                                                 "instance_id already set"); return; }
    snprintf(sc->inst_id, sizeof(sc->inst_id), "%s", v);
}

static void sc_commit(wl_client *, wl_resource *r) {
    SecCtx *sc = sc_of(r);
    if (!sc) return;
    if (sc->committed) {
        wl_resource_post_error(r, WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_USED,
                               "security context already committed");
        return;
    }
    sc->committed = true;
    // close_fd is the host's completion sentinel: closing it confirms the registration.
    if (sc->close_fd >= 0) { close(sc->close_fd); sc->close_fd = -1; }
    if (sc->listen_fd < 0) {
        // Without a listening socket there is nothing to register, so the host sent
        wl_resource_post_error(r, WP_SECURITY_CONTEXT_V1_ERROR_INVALID_METADATA,
                               "commit without create_listener");
        return;
    }
    int fl = fcntl(sc->listen_fd, F_GETFL, 0);
    if (fl >= 0) fcntl(sc->listen_fd, F_SETFL, fl | O_NONBLOCK);
    sc->src = wl_event_loop_add_fd(g_srv.loop, sc->listen_fd, WL_EVENT_READABLE,
                                   sec_on_conn, sc);
    if (!sc->src)
        fprintf(stderr, "[secctx] cannot add listen_fd to the event loop\n");
    printf("[secctx] commit: engine=%s app_id=%s instance=%s, accepting\n",
           sc->engine[0] ? sc->engine : "?",
           sc->app_id[0] ? sc->app_id : "?",
           sc->inst_id[0] ? sc->inst_id : "?");
}

static const struct wp_security_context_v1_interface sc_impl = {
    .destroy            = sc_destroy,
    .set_sandbox_engine = sc_set_engine,
    .set_app_id         = sc_set_app_id,
    .set_instance_id    = sc_set_instance_id,
    .commit             = sc_commit,
};

static void sc_res_gone(wl_resource *r) {
    SecCtx *sc = sc_of(r);
    if (!sc) return;
    if (sc->src) { wl_event_source_remove(sc->src); sc->src = nullptr; }
    if (sc->listen_fd >= 0) { close(sc->listen_fd); sc->listen_fd = -1; }
    if (sc->close_fd >= 0)  { close(sc->close_fd);  sc->close_fd = -1; }
    for (size_t i = 0; i < g_seccx.size(); i++)
        if (g_seccx[i] == sc) { g_seccx.erase(g_seccx.begin() + i); break; }
    printf("[secctx] context destroyed after %d accepted clients\n", sc->accepted);
    delete sc;
}

// ------------------------------------------------- wp_security_context_manager_v1
static void scm_create_listener(wl_client *c, wl_resource *mgr, uint32_t id,
                                int32_t listen_fd, int32_t close_fd) {
    wl_resource *r = wl_resource_create(c, &wp_security_context_v1_interface,
                                        wl_resource_get_version(mgr), id);
    if (!r) {
        if (listen_fd >= 0) close(listen_fd);
        if (close_fd >= 0)  close(close_fd);
        wl_client_post_no_memory(c);
        return;
    }
    SecCtx *sc = new SecCtx();
    sc->res = r;
    sc->listen_fd = listen_fd;
    sc->close_fd = close_fd;
    g_seccx.push_back(sc);
    wl_resource_set_implementation(r, &sc_impl, sc, sc_res_gone);
    printf("[secctx] create_listener listen_fd=%d close_fd=%d\n", listen_fd, close_fd);
}

static void scm_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wp_security_context_manager_v1_interface scm_impl = {
    .destroy         = scm_destroy,
    .create_listener = scm_create_listener,
};

static void bind_security_context(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &wp_security_context_manager_v1_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &scm_impl, nullptr, nullptr);
    printf("[secctx] wp_security_context_manager_v1 bound\n");
}
