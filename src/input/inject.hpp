// SPDX-License-Identifier: AGPL-3.0-or-later
// inject.hpp
//
// Key injection socket, used by the control client.
//
// Extracted from the former single-file implementation; behaviour is unchanged.
// Depends on include order: everything used here is provided by the modules
// included before it in src/main.cpp.
#pragma once

// ---- injection socket
static void key_conn_close(KeyConn *kc) {
    if (!kc) return;
    if (kc->fd == g_ime_fd) {
        g_ime_fd = -1;
        printf("[ime] IME client disconnected\n");
    }
    if (kc->src) wl_event_source_remove(kc->src);
    for (size_t i = 0; i < g_srv.key_conns.size(); i++) {
        if (g_srv.key_conns[i] == kc) { g_srv.key_conns.erase(g_srv.key_conns.begin() + i); break; }
    }
    close(kc->fd);
    delete kc;
}

static int on_key_conn(int fd, uint32_t mask, void *data) {
    KeyConn *kc = (KeyConn *)data;
    // Read before handling HUP: a client often sends and closes at once, so READABLE and
    // HANGUP are set together and the command would be lost.
    bool hangup = (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0;
    if (mask & WL_EVENT_READABLE) {
        char tmp[1024];
        ssize_t n = read(fd, tmp, sizeof(tmp) - 1);
        if (n <= 0) { key_conn_close(kc); return 0; }
        tmp[n] = 0;
        kc->buf.append(tmp, (size_t)n);
        size_t pos;
        while ((pos = kc->buf.find('\n')) != std::string::npos) {
            std::string line = kc->buf.substr(0, pos);
            kc->buf.erase(0, pos + 1);
            if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
            kb_command(line, fd);
        }
        if (!hangup) return 0;
    }
    // Peer closed: process the last line even without a trailing newline.
    if (!kc->buf.empty()) { std::string line = kc->buf; kc->buf.clear(); kb_command(line, kc->fd); }
    key_conn_close(kc);
    return 0;
}

static int on_key_accept(int fd, uint32_t, void *) {
    int c = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
    if (c < 0) return 0;
    printf("[kbd] injection connection accepted, fd=%d\n", c);
    KeyConn *kc = new KeyConn();
    kc->fd = c;
    g_srv.key_conns.push_back(kc);
    kc->src = wl_event_loop_add_fd(g_srv.loop, c, WL_EVENT_READABLE, on_key_conn, kc);
    return 0;
}

// Open an injection socket at the given path and attach the accept handler.
// chmod 0666 only covers the socket itself; DAC also has to traverse the parent.
// $HOME/.run is 0700 and owned by the Termux uid, so other apps cannot reach it,
// hence the second socket under /data/local/tmp.
static int key_listen_on(const std::string &path, bool is_primary) {
    unlink(path.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) { fprintf(stderr, "[kbd] cannot create socket %s: %s\n", path.c_str(), strerror(errno)); return -1; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 8) < 0) {
        fprintf(stderr, "[kbd] bind or listen failed on %s: %s\n", path.c_str(), strerror(errno));
        close(fd);
        return -1;
    }
    chmod(path.c_str(), 0666);
    if (is_primary) { g_srv.key_listen_fd = fd; g_srv.key_sock_path = path; }
    wl_event_loop_add_fd(g_srv.loop, fd, WL_EVENT_READABLE, on_key_accept, nullptr);
    printf("[kbd] injection socket: %s\n", path.c_str());
    return fd;
}

// Abstract namespace socket: no filesystem node, so no directory DAC and no SELinux
// type on the path, which makes it the easiest one for ordinary applications.
static int key_listen_abstract(const char *name) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) { fprintf(stderr, "[kbd] cannot create abstract socket: %s\n", strerror(errno)); return -1; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t n = strlen(name);
    if (n + 1 > sizeof(addr.sun_path)) { close(fd); return -1; }
    addr.sun_path[0] = '\0';
    memcpy(addr.sun_path + 1, name, n);
    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);
    if (bind(fd, (struct sockaddr *)&addr, len) < 0 || listen(fd, 8) < 0) {
        fprintf(stderr, " [kbd] abstract bind failed(@%s): %s\n", name, strerror(errno));
        close(fd);
        return -1;
    }
    wl_event_loop_add_fd(g_srv.loop, fd, WL_EVENT_READABLE, on_key_accept, nullptr);
    printf("[kbd] injection socket: @%s\n", name);
    return fd;
}

// TCP on 127.0.0.1, the fallback: an app with the INTERNET permission may use loopback.
static int key_listen_tcp(int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) { fprintf(stderr, "[kbd] cannot create tcp socket: %s\n", strerror(errno)); return -1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 8) < 0) {
        fprintf(stderr, "[kbd] tcp bind failed on port %d: %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }
    wl_event_loop_add_fd(g_srv.loop, fd, WL_EVENT_READABLE, on_key_accept, nullptr);
    printf("[kbd] injection socket: 127.0.0.1:%d\n", port);
    return fd;
}

// A socket in the application data directory has the same uid and the same
// app_data_file label, which is the only location ordinary apps can reach.
static int key_listen_on_appdir(const char *pkg) {
    std::string dir = std::string("/data/data/") + pkg;
    struct stat st;
    if (stat(dir.c_str(), &st) < 0) { printf("[kbd] %s does not exist, skipping the app socket\n", dir.c_str()); return -1; }
    std::string files = dir + "/files";
    mkdir(files.c_str(), 0770);
    std::string path = files + "/sfwm-keys.sock";
    int fd = key_listen_on(path, false);
    if (fd < 0) return -1;
    chown(path.c_str(), st.st_uid, st.st_gid);
    chmod(path.c_str(), 0600);
    printf("[kbd] appdir socket uid=%u gid=%u\n", (unsigned)st.st_uid, (unsigned)st.st_gid);
    return fd;
}

// Watchdog: a held frame that no later frame replaced within 150 ms is read at once.
// Two reasons:
//   1) a client that commits once (single buffer, static content) is not held forever;
//   2) a wl_buffer that died unseen does not leave a stale pointer behind.
static wl_event_source *g_defer_timer = nullptr;
static int on_defer_tick(void *) {
    const uint32_t nowm = now_ms();
    for (View *v : g_srv.views) {
        if (!v->read_next || nowm - v->read_next_ms < 150) continue;
        if (v->hold) { wl_list_remove(&v->hold->lis.link); delete v->hold; v->hold = nullptr; }
        wl_resource *b = v->read_next;
        v->read_next = nullptr;
        v->blit(b);
        printf("[wm] watchdog: reading a frame held for %u ms\n", nowm - v->read_next_ms);
    }
    if (g_defer_timer) wl_event_source_timer_update(g_defer_timer, 150);
    return 0;
}

static void sfwm_keys_init() {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    key_listen_on(std::string(rt ? rt : "/data/local/tmp") + "/sfwm-keys.sock", true);
    // The cross application channel: /data/local/tmp is 0771, so other uids can traverse it,
    // but SELinux denies untrusted_app access to shell_data_file, so that path fails.
    if (!rt || std::string(rt) != "/data/local/tmp") key_listen_on("/data/local/tmp/sfwm-keys.sock", false);
    // The working path is a socket in the input method data directory, same uid and label.
    key_listen_on_appdir("com.touchtype.swiftkey");
    // The IME host is a client as well: it keeps one connection open, receives
    // activate and deactivate, and reports its height.
    key_listen_on_appdir("org.sfwm.ime");
    key_listen_abstract("sfwm-keys");
    key_listen_tcp(5599);
    kb_xkb_init();
    if (g_srv.loop) {
        g_defer_timer = wl_event_loop_add_timer(g_srv.loop, on_defer_tick, nullptr);
        if (g_defer_timer) wl_event_source_timer_update(g_defer_timer, 150);
    }
}

