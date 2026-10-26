// inject.hpp
//
// 按键注入 socket（sfwmc 走这里）
//
// 从 sfwm.cpp 原样搬出（只搬不改，逻辑零差异）。
// 依赖包含顺序：本文件用到的东西必须由**前面的**模块提供，
// 因此它在 sfwm.cpp 里的 include 位置就是它原来在文件里的位置。
#pragma once

// ---- 注入 socket
static void key_conn_close(KeyConn *kc) {
    if (!kc) return;
    if (kc->fd == g_ime_fd) {
        g_ime_fd = -1;
        printf("[ime] IME 客户端连接断开\n");
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
    // 先读数据：客户端往往「发完就 close」，此时 mask 里 READABLE 和 HANGUP 同时置位，
    // 必须先取走数据，否则整条命令就丢了。
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
    // 对端已关闭：把残留的最后一行（没带换行的）也处理掉再收工
    if (!kc->buf.empty()) { std::string line = kc->buf; kc->buf.clear(); kb_command(line, kc->fd); }
    key_conn_close(kc);
    return 0;
}

static int on_key_accept(int fd, uint32_t, void *) {
    int c = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
    if (c < 0) return 0;
    printf("[kbd] 注入连接已接受 fd=%d\n", c);
    KeyConn *kc = new KeyConn();
    kc->fd = c;
    g_srv.key_conns.push_back(kc);
    kc->src = wl_event_loop_add_fd(g_srv.loop, c, WL_EVENT_READABLE, on_key_conn, kc);
    return 0;
}

// 在指定路径开一个注入 socket 并挂上 accept 处理器。
// 注意 chmod 0666 只解决「同 uid 之外的进程能不能连」——DAC 还要能穿过父目录。
// Termux 的 $HOME/.run 是 0700 u0_a391，别的 app（SwiftKey 等）根本进不去；
// 所以另外在 /data/local/tmp 开一个全 uid 可达的。
static int key_listen_on(const std::string &path, bool is_primary) {
    unlink(path.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) { fprintf(stderr, "[kbd] socket 创建失败(%s): %s\n", path.c_str(), strerror(errno)); return -1; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 8) < 0) {
        fprintf(stderr, "[kbd] bind/listen 失败(%s): %s\n", path.c_str(), strerror(errno));
        close(fd);
        return -1;
    }
    chmod(path.c_str(), 0666);
    if (is_primary) { g_srv.key_listen_fd = fd; g_srv.key_sock_path = path; }
    wl_event_loop_add_fd(g_srv.loop, fd, WL_EVENT_READABLE, on_key_accept, nullptr);
    printf("[kbd] 注入 socket: %s\n", path.c_str());
    return fd;
}

// 抽象命名空间 unix socket（@name）。没有文件系统节点，不受目录 DAC 限制，
// 也不受 /data/local/tmp 的 SELinux 类型牵连 —— 普通 app 最容易连的就是它。
static int key_listen_abstract(const char *name) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) { fprintf(stderr, "[kbd] 抽象 socket 创建失败: %s\n", strerror(errno)); return -1; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t n = strlen(name);
    if (n + 1 > sizeof(addr.sun_path)) { close(fd); return -1; }
    addr.sun_path[0] = '\0';
    memcpy(addr.sun_path + 1, name, n);
    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);
    if (bind(fd, (struct sockaddr *)&addr, len) < 0 || listen(fd, 8) < 0) {
        fprintf(stderr, "[kbd] 抽象 bind 失败(@%s): %s\n", name, strerror(errno));
        close(fd);
        return -1;
    }
    wl_event_loop_add_fd(g_srv.loop, fd, WL_EVENT_READABLE, on_key_accept, nullptr);
    printf("[kbd] 注入 socket: @%s\n", name);
    return fd;
}

// 127.0.0.1 上的 TCP 端口 —— 保底方案：有 INTERNET 权限的 app 连 loopback 一定被允许
static int key_listen_tcp(int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) { fprintf(stderr, "[kbd] tcp socket 创建失败: %s\n", strerror(errno)); return -1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 8) < 0) {
        fprintf(stderr, "[kbd] tcp bind 失败(:%d): %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }
    wl_event_loop_add_fd(g_srv.loop, fd, WL_EVENT_READABLE, on_key_accept, nullptr);
    printf("[kbd] 注入 socket: 127.0.0.1:%d\n", port);
    return fd;
}

// 把 socket 开在目标 app 自己的数据目录里：DAC 同 uid、SELinux 同 app_data_file label，
// 是唯一「普通 app 一定连得上」的文件系统位置。（/data/local/tmp 会被 SELinux 拦）
static int key_listen_on_appdir(const char *pkg) {
    std::string dir = std::string("/data/data/") + pkg;
    struct stat st;
    if (stat(dir.c_str(), &st) < 0) { printf("[kbd] %s 不存在，跳过 appdir socket\n", dir.c_str()); return -1; }
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

// 看门狗：攥住的帧超过 150ms 还没被下一帧顶掉，就立刻读掉。
//   两个作用：
//     1) 客户端如果只提交一次（单缓冲/静止画面），我们不会把它一直攥着不放；
//     2) 万一哪次 wl_buffer 生命周期没兜住，也不会无限期留着一个悬空指针。
static wl_event_source *g_defer_timer = nullptr;
static int on_defer_tick(void *) {
    const uint32_t nowm = now_ms();
    for (View *v : g_srv.views) {
        if (!v->read_next || nowm - v->read_next_ms < 150) continue;
        if (v->hold) { wl_list_remove(&v->hold->lis.link); delete v->hold; v->hold = nullptr; }
        wl_resource *b = v->read_next;
        v->read_next = nullptr;
        v->blit(b);
        printf("[wm] 看门狗：把攥了 %ums 的帧读掉\n", nowm - v->read_next_ms);
    }
    if (g_defer_timer) wl_event_source_timer_update(g_defer_timer, 150);
    return 0;
}

static void sfwm_keys_init() {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    key_listen_on(std::string(rt ? rt : "/data/local/tmp") + "/sfwm-keys.sock", true);
    // 跨 app 通道：/data/local/tmp 本身是 0771 shell:shell，others 有 x（可穿过），
    // 文件 0666 就能连。但 SELinux 会拦 untrusted_app → shell_data_file，实测 Permission denied。
    if (!rt || std::string(rt) != "/data/local/tmp") key_listen_on("/data/local/tmp/sfwm-keys.sock", false);
    // 真正好用的那条：把 socket 开在输入法自己的数据目录里（同 uid、同 app_data_file label）
    key_listen_on_appdir("com.touchtype.swiftkey");
    // IME 宿主（我们自己那个 APK）也算一个客户端：它连进来后保持长连接，
    // sfwm 往下发 activate/deactivate，它往上发 height。
    key_listen_on_appdir("org.sfwm.ime");
    key_listen_abstract("sfwm-keys");
    key_listen_tcp(5599);
    kb_xkb_init();
    if (g_srv.loop) {
        g_defer_timer = wl_event_loop_add_timer(g_srv.loop, on_defer_tick, nullptr);
        if (g_defer_timer) wl_event_source_timer_update(g_defer_timer, 150);
    }
}

