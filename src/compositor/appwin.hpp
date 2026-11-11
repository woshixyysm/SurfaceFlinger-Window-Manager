// SPDX-License-Identifier: AGPL-3.0-or-later
// appwin.hpp - Android windows that take part in the layout.
//
// An Android application cannot be given a Wayland surface: it draws into its own display.
// The bridge module hosts that display in an overlay window, so the geometry is owned by
// whoever can move that window. This module is the sfwm side of that contract: it reads the
// published state to learn which windows exist, keeps them in the tiling order, and sends
// the tile geometry back over the control socket. Nothing here links against the module;
// without it the file simply finds no state and no window appears.
//
// Depends on include order: Server and the workspace state (protocol/workspace.hpp) must be
// visible, and compositor/tiling.hpp defines tile_sync() after this header.
#pragma once

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void tile_sync();   // compositor/tiling.hpp
static void app_lock_apply(bool locked);   // input/keyboard.hpp: hides the layers

struct AppWin {
    int32_t slot = 0;       // slot in the bridge, also the window's identity
    int32_t display = -1;
    int32_t stack = -1;
    int32_t x = 0, y = 0, w = 0, h = 0;
    int32_t dpi = 0;
    int32_t ws = 0;         // workspace the window was opened on
    char    pkg[128] = {0};
};

static std::vector<AppWin *> g_apps;
static wl_event_source *g_app_timer = nullptr;
static bool g_app_locked = false;   // the Android keyguard is up

static bool app_locked_now() { return g_app_locked; }

// Published state and control socket. Both are configurable, because the shell process
// that hosts the windows is not always SystemUI.
static const char *APPWIN_STATE_DEFAULT = "/data/data/com.android.systemui/files/sfwm-bridge.state";
static const char *APPWIN_CTL_ABSTRACT  = "sfwm-bridge-ctl";
static const char *APPWIN_CTL_PATH      = "/data/local/tmp/sfwm-bridge-ctl.sock";

// ------------------------------------------------------------------ control channel
// The module runs in another process and another SELinux domain, so the abstract namespace
// is tried first: it needs no path, no permission and no label. The filesystem socket is the
// fallback for shells where an abstract name is filtered.
static bool appwin_send(const char *line) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    struct sockaddr_un addr;
    bool connected = false;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    const size_t nameLen = strlen(APPWIN_CTL_ABSTRACT);
    memcpy(addr.sun_path + 1, APPWIN_CTL_ABSTRACT, nameLen);
    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + nameLen);
    if (connect(fd, (struct sockaddr *)&addr, len) == 0) {
        connected = true;
    } else {
        close(fd);
        fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return false;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", APPWIN_CTL_PATH);
        len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(addr.sun_path) + 1);
        connected = connect(fd, (struct sockaddr *)&addr, len) == 0;
    }
    if (!connected) {
        close(fd);
        fprintf(stderr, "[app] no bridge control socket, dropped '%s'\n", line);
        return false;
    }
    std::string msg(line);
    if (msg.empty() || msg.back() != '\n') msg += '\n';
    const ssize_t written = write(fd, msg.data(), msg.size());
    close(fd);
    return written == (ssize_t)msg.size();
}

static void appwin_place(AppWin *a, int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!a) return;
    if (a->x == x && a->y == y && a->w == w && a->h == h) return;   // nothing to send
    char line[160];
    snprintf(line, sizeof(line), "place %d %d %d %d %d", a->slot, x, y, w, h);
    if (appwin_send(line)) {
        a->x = x;
        a->y = y;
        a->w = w;
        a->h = h;
    }
}

// ------------------------------------------------------------------ published state
static AppWin *appwin_find(int32_t slot) {
    for (AppWin *a : g_apps) if (a->slot == slot) return a;
    return nullptr;
}

static AppWin *appwin_find_display(int32_t display) {
    for (AppWin *a : g_apps) if (a->display == display) return a;
    return nullptr;
}

// One state line per window:
//     display <slot> <displayId> <layerStack> <w> <h> <dpi> <x> <y> <package>
// A package of "-" marks a plain output, which is not a window and is skipped.
static void appwin_poll() {
    const char *path = getenv("SFWM_BRIDGE_STATE");
    if (!path || !*path) path = APPWIN_STATE_DEFAULT;
    FILE *f = fopen(path, "r");
    if (!f) return;
    std::vector<int32_t> seen;
    bool changed = false;
    char buf[512];
    while (fgets(buf, sizeof(buf), f)) {
        // The lock screen is the shell drawing over everything. While it is up the bridge
        // hides the hosted windows and sfwm hides its own layers, so nothing of the session
        // is visible behind the keyguard.
        int lockState = -1, panelHeight = 0;
        if (sscanf(buf, "lock %d %d", &lockState, &panelHeight) >= 1 && lockState >= 0) {
            const bool locked = lockState != 0;
            if (locked != g_app_locked) {
                g_app_locked = locked;
                app_lock_apply(locked);
            }
            continue;
        }
        int slot = -1, display = -1, stack = -1, w = 0, h = 0, dpi = 0, x = 0, y = 0;
        char pkg[128] = {0};
        const int n = sscanf(buf, "display %d %d %d %d %d %d %d %d %127s",
                             &slot, &display, &stack, &w, &h, &dpi, &x, &y, pkg);
        if (n < 9 || slot < 0) continue;
        if (!strcmp(pkg, "-")) continue;
        seen.push_back(slot);
        AppWin *a = appwin_find(slot);
        if (!a) {
            a = new AppWin();
            a->slot = slot;
            a->ws = g_ws_active;
            g_apps.push_back(a);
            changed = true;
            printf("[app] slot %d opened '%s' on display %d (%dx%d)\n", slot, pkg, display, w, h);
        }
        if (a->display != display || a->stack != stack) changed = true;
        if (strcmp(a->pkg, pkg)) {
            snprintf(a->pkg, sizeof(a->pkg), "%s", pkg);
            changed = true;
        }
        a->display = display;
        a->stack = stack;
        a->dpi = dpi;
        // The reported rectangle is what the window actually is. Trusting it is what keeps
        // the two sides in step: a placement the module could not apply shows up here as the
        // old geometry, and the next layout pass simply sends it again.
        if (a->x != x || a->y != y || a->w != w || a->h != h) {
            a->x = x;
            a->y = y;
            a->w = w;
            a->h = h;
            changed = true;
        }
    }
    fclose(f);

    for (size_t i = 0; i < g_apps.size(); ) {
        bool alive = false;
        for (int32_t s : seen) if (s == g_apps[i]->slot) alive = true;
        if (alive) { i++; continue; }
        printf("[app] slot %d closed\n", g_apps[i]->slot);
        delete g_apps[i];
        g_apps.erase(g_apps.begin() + i);
        changed = true;
    }
    if (changed) tile_sync();
}

static int on_appwin_tick(void *) {
    appwin_poll();
    if (g_app_timer) wl_event_source_timer_update(g_app_timer, 300);
    return 0;
}

// ------------------------------------------------------------------ commands
static void appwin_open(int32_t slot, const char *what) {
    if (slot < 0) return;
    char line[192];
    snprintf(line, sizeof(line), "open %d %s", slot, (what && *what) ? what : "home");
    if (appwin_send(line)) printf("[app] %s\n", line);
}

static void appwin_close(int32_t slot) {
    char line[64];
    snprintf(line, sizeof(line), "close %d", slot);
    if (appwin_send(line)) printf("[app] %s\n", line);
}

// Android windows cannot be handed the keyboard the way a Wayland client is: keys reach
// whichever window the shell thinks is focused, so focusing a tile means raising its task.
static bool appwin_focus(AppWin *a) {
    if (!a) return false;
    char line[48];
    snprintf(line, sizeof(line), "focus %d", a->slot);
    return appwin_send(line);
}

// Touch belongs to the compositor: it reads the panel and decides which tile the finger is
// on, so a contact inside an Android window is delivered from here. The coordinates are the
// window's own, which are the display's, because the window is exactly as large as it.
static bool appwin_touch(AppWin *a, const char *phase, int32_t x, int32_t y, int32_t id) {
    if (!a) return false;
    char line[96];
    snprintf(line, sizeof(line), "touch %d %s %d %d %d", a->slot, phase, x, y, id);
    return appwin_send(line);
}

// The pointer does not "do" anything to an application: what lifts an icon is the hover
// events it receives, so they travel the same way a contact does, with enter and exit marking
// the moments a window gains and loses the pointer.
static bool appwin_hover(AppWin *a, const char *phase, int32_t x, int32_t y) {
    if (!a) return false;
    char line[96];
    snprintf(line, sizeof(line), "hover %d %s %d %d", a->slot, phase, x, y);
    return appwin_send(line);
}

// The pointer's picture over an Android tile is the system's own pointer sprite, not a layer
// of ours. A layer sfwm creates is a client layer: SurfaceFlinger composites it, and it is
// given a touchable region over its pixels that swallows the touch landing under it — the
// reason a cursor used to break a swipe. The sprite is created without an input channel at
// all, so it cannot occlude anything, and it is the layer this device's composer puts on the
// hardware cursor plane. The position is in screen pixels: the sprite lives on the physical
// display, over the whole tile grid.
//
// The controller that makes the sprite lives in libinputservice.so, which only a process in
// the platform's linker namespace may open, so it is not run inside a hooked Android process
// but in one of its own: `app_process` with the module's dex, started here, as root, the
// first time the pointer reaches a tile. Commands go down its standard input, and the pipe
// closing is what tells it to take the picture away — a cursor cannot outlive the compositor.
static int   g_sprite_fd = -1;
static pid_t g_sprite_pid = -1;

/** /data/app/<tree>/org.sfwm.bridge-<hash>/base.apk: the module that carries the host. */
static bool sprite_apk(char *out, size_t size) {
    DIR *root = opendir("/data/app");
    if (!root) return false;
    bool found = false;
    while (struct dirent *tree = readdir(root)) {
        if (tree->d_name[0] == '.') continue;
        char treePath[PATH_MAX];
        snprintf(treePath, sizeof treePath, "/data/app/%s", tree->d_name);
        DIR *inner = opendir(treePath);
        if (!inner) continue;
        while (struct dirent *pkg = readdir(inner)) {
            if (strncmp(pkg->d_name, "org.sfwm.bridge-", 16) != 0) continue;
            char candidate[PATH_MAX];
            snprintf(candidate, sizeof candidate, "%s/%s/base.apk", treePath, pkg->d_name);
            struct stat st;
            if (stat(candidate, &st) == 0 && st.st_size > 0) {
                snprintf(out, size, "%s", candidate);
                found = true;
                break;
            }
        }
        closedir(inner);
        if (found) break;
    }
    closedir(root);
    return found;
}

/** The split install puts the library next to the dex; a deploy may also leave it here. */
static bool sprite_library(const char *apk, char *out, size_t size) {
    snprintf(out, size, "%s", apk);
    char *tail = strstr(out, "/base.apk");
    if (tail) snprintf(tail, size - (size_t)(tail - out), "/lib/arm64/libsfwmsprite.so");
    struct stat st;
    if (stat(out, &st) == 0 && st.st_size > 0) return true;
    snprintf(out, size, "/data/local/tmp/libsfwmsprite.so");
    return stat(out, &st) == 0 && st.st_size > 0;
}

/**
 * Preloading straight out of the APK is refused to a process that is not the module, so the
 * library is copied next to the compositor's other device-side files first. It is a few
 * hundred kilobytes, and the copy is what makes it loadable at all.
 */
static bool sprite_copy(const char *source, char *out, size_t size) {
    snprintf(out, size, "/data/local/tmp/libsfwmsprite.so");
    if (strcmp(source, out) == 0) return true;
    struct stat st;
    if (stat(source, &st) != 0) return false;
    FILE *in = fopen(source, "rb");
    if (!in) return false;
    FILE *to = fopen(out, "wb");
    if (!to) {
        fclose(in);
        return false;
    }
    char buffer[65536];
    size_t n;
    bool ok = true;
    while ((n = fread(buffer, 1, sizeof buffer, in)) > 0) {
        if (fwrite(buffer, 1, n, to) != n) {
            ok = false;
            break;
        }
    }
    fclose(in);
    fclose(to);
    if (!ok) {
        fprintf(stderr, "[sprite] could not copy %s\n", source);
        return false;
    }
    chmod(out, 0644);
    return true;
}

static bool sprite_start(void) {
    if (g_sprite_fd >= 0) return true;
    char apk[PATH_MAX];
    if (!sprite_apk(apk, sizeof apk)) {
        fprintf(stderr, "[sprite] the bridge module is not installed, no system cursor\n");
        return false;
    }
    char library[PATH_MAX];
    if (!sprite_library(apk, library, sizeof library)) {
        fprintf(stderr, "[sprite] libsfwmsprite.so is missing next to %s\n", apk);
        return false;
    }
    char preload[PATH_MAX];
    if (!sprite_copy(library, preload, sizeof preload)) return false;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
        fprintf(stderr, "[sprite] no pipe: %s\n", strerror(errno));
        return false;
    }
    // A closed pipe must not take the compositor down with it.
    signal(SIGPIPE, SIG_IGN);
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0) {
        dup2(fds[0], 0);
        close(fds[0]);
        close(fds[1]);
        char classPath[PATH_MAX];
        snprintf(classPath, sizeof classPath, "-Djava.class.path=%s", apk);
        // Preloading is what puts the library — and so the sprite controller — in the
        // platform's linker namespace; a library loaded with System.load gets a classloader
        // namespace and may not open libinputservice.so.
        //
        // The compositor's own environment has to go with it: LD_LIBRARY_PATH points at the
        // Termux tree and LD_PRELOAD at the shims that make EGL work, and an Android runtime
        // started with those binds to the wrong libc and never comes up.
        unsetenv("LD_LIBRARY_PATH");
        unsetenv("MESA_LOADER_DRIVER_OVERRIDE");
        unsetenv("LIBGL_DRIVERS_PATH");
        unsetenv("SFWM_LIBEGL");
        setenv("LD_PRELOAD", preload, 1);
        char *const argv[] = {
            (char *)"/system/bin/app_process",
            classPath,
            (char *)"/system/bin",
            (char *)"--nice-name=sfwm-sprite",
            (char *)"org.sfwm.bridge.cursor.SpriteHost",
            nullptr,
        };
        execv(argv[0], argv);
        _exit(127);
    }
    close(fds[0]);
    g_sprite_fd = fds[1];
    g_sprite_pid = pid;
    printf("[sprite] host started for '%s', pid %d\n", apk, (int)pid);
    return true;
}

static bool appwin_sprite(const char *phase, int32_t x, int32_t y) {
    const bool hide = strcmp(phase, "hide") == 0;
    if (g_sprite_fd < 0 && (hide || !sprite_start())) return false;
    char line[64];
    const int n = snprintf(line, sizeof line, "%s %d %d\n", phase, x, y);
    // One line per command; a failed write means the host is gone and the pointer is sfwm's
    // again until it is started once more. It is reaped here so a dead host does not stay a
    // zombie for the rest of the session.
    if (write(g_sprite_fd, line, (size_t)n) != n) {
        close(g_sprite_fd);
        g_sprite_fd = -1;
        if (g_sprite_pid > 0) waitpid(g_sprite_pid, nullptr, WNOHANG);
        g_sprite_pid = -1;
        return false;
    }
    return true;
}



static void appwin_list() {    if (g_apps.empty()) {
        printf("[app] no Android window; 'app open <slot> <package|home>' starts one\n");
        return;
    }
    for (AppWin *a : g_apps)
        printf("[app] slot=%d display=%d stack=%d '%s' %dx%d at %d,%d dpi=%d ws=%d\n",
               a->slot, a->display, a->stack, a->pkg, a->w, a->h, a->x, a->y, a->dpi, a->ws);
}
