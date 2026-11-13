// SPDX-License-Identifier: AGPL-3.0-or-later
// sfwmctl - one small controller for the on-device sfwm / Minecraft workflow.
//
// Every path, environment variable and socket name this project needs was originally spread
// across a dozen throwaway shell scripts under /data/local/tmp. Those are unreproducible and
// each new session had to rediscover them, so they live here instead.
//
// Run on the device (aarch64), as root or as the Termux uid.
//
//   sfwmctl up [--scale N] [--fps N] [--sync] [--no-zc]
//   sfwmctl mc [version]
//   sfwmctl key F3
//   sfwmctl status
//   sfwmctl shot /data/local/tmp/out.png
//   sfwmctl stop
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace {

// --- the environment, in one place -------------------------------------------------------
const char *kHome = "/data/data/com.termux/files/home";
const char *kPrefix = "/data/data/com.termux/files/usr";
const char *kBinary = "/data/data/com.termux/files/home/lg/sfwm/sfwm";
const char *kBuild = "/data/data/com.termux/files/home/sfwm-repo/build/sfwm";
const char *kRuntime = "/data/local/tmp/sfwm-xdg";
const char *kKeys = "/data/local/tmp/sfwm-keys.sock";
const char *kLog = "/data/data/com.termux/files/home/sfwm-test.log";
const char *kMcLog = "/data/local/tmp/mc-run.log";
const char *kTermuxUid = "10391";

// The shim chain is what makes an unprivileged Termux process able to talk to SurfaceFlinger
// and to allocate through the vendor gralloc. Order matters: eglcompat first.
const char *kPreload =
    "/data/local/tmp/libeglcompat.so "
    "/data/local/tmp/libfdshim.so "
    "/system/lib64/libnativewindow.so "
    "/system/lib64/libbinder_ndk.so "
    "/system/lib64/liblzma.so";

const char *kMcPreload = "/data/local/tmp/libfdshim.so";
const char *kMcLibPath = "/data/local/tmp/mesa446";
const char *kMcDrivers = "/data/local/tmp/mesa-new";
const char *kDefaultVersion = "1.21.11-Fabric 0.19.1";

void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

std::string run(const std::string &cmd) {
    std::string out;
    FILE *p = popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[512];
    while (fgets(buf, sizeof buf, p)) out += buf;
    pclose(p);
    return out;
}

bool exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

// --- process helpers ---------------------------------------------------------------------
std::vector<pid_t> pids_of(const char *name) {
    std::vector<pid_t> v;
    const std::string out = run(std::string("pidof ") + name);
    size_t i = 0;
    while (i < out.size()) {
        while (i < out.size() && (out[i] == ' ' || out[i] == '\n')) ++i;
        if (i >= out.size()) break;
        v.push_back((pid_t)strtol(out.c_str() + i, nullptr, 10));
        while (i < out.size() && out[i] != ' ' && out[i] != '\n') ++i;
    }
    return v;
}

void kill_all(const char *name) {
    for (pid_t p : pids_of(name)) {
        say("  killing %s pid %d", name, (int)p);
        kill(p, SIGKILL);
    }
}

void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, nullptr);
}

// --- commands ----------------------------------------------------------------------------
int cmd_status() {
    say("sfwm:");
    for (pid_t p : pids_of("sfwm")) {
        const std::string cmdline = run("tr '\\0' ' ' < /proc/" + std::to_string(p) + "/cmdline");
        say("  pid %-7d up %s", (int)p,
            run("ps -o etime= -p " + std::to_string(p)).c_str());
        say("    %s", cmdline.c_str());
    }
    if (pids_of("sfwm").empty()) say("  (not running)");

    say("surfaceflinger: %s", run("pidof surfaceflinger").c_str());
    say("selinux:        %s", run("getenforce").c_str());

    const std::string mode =
        run("dumpsys SurfaceFlinger 2>/dev/null | grep -m1 -o 'vsyncRate[= ][0-9.]*'");
    say("refresh:        %s", mode.empty() ? "(unknown)" : mode.c_str());

    say("socket:         %s",
        exists(kRuntime) ? run("ls " + std::string(kRuntime) + " 2>/dev/null").c_str() : "absent");

    if (exists(kLog)) {
        say("--- last sfwm perf lines");
        const std::string tail = run("grep -a '\\[perf\\]' " + std::string(kLog) + " | tail -3");
        fputs(tail.empty() ? "  (none yet)\n" : tail.c_str(), stdout);
    }
    return 0;
}

int cmd_up(int argc, char **argv) {
    int scale = 1;
    int fps = 165;
    bool zc = true;
    bool async = true;
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-zc")) zc = false;
        // --sync asks for the vsync-paced path, which is what the compositor did before the
        // swap interval was in play. Useful as an A/B reference, not for playing.
        else if (!strcmp(argv[i], "--sync")) async = false;
    }

    kill_all("sfwm");
    kill_all("java");
    sleep_ms(3000);

    if (exists(kBuild)) {
        run(std::string("cp ") + kBuild + " " + kBinary);
        run(std::string("chown ") + kTermuxUid + ":" + kTermuxUid + " " + kBinary);
        run(std::string("chmod 755 ") + kBinary);
        run(std::string(kPrefix) + "/bin/ctxfix -u " + kTermuxUid + " --mode 755 " + kBinary +
            " > /dev/null");
    }

    run(std::string("rm -f ") + kRuntime + "/sfwm-0* " + kKeys);
    run(std::string("mkdir -p ") + kRuntime);
    run(std::string(": > ") + kLog);

    char env[4096];
    snprintf(env, sizeof env,
             "HOME=%s XDG_RUNTIME_DIR=%s LD_LIBRARY_PATH=%s/lib "
             "LD_PRELOAD=\"%s\" "
             "SFWM_ZC=%d SFWM_FPS=%d SFWM_FRAMETIME=1 SFWM_REFRESH=1 SFWM_ASYNC=%d "
             "SFWM_LIBEGL=%s/lib/libEGL.so.1 SFWM_DRM_DEVICE=/dev/dri/renderD128 "
             "MESA_LOADER_DRIVER_OVERRIDE=msm LIBGL_DRIVERS_PATH=%s/lib/dri",
             kHome, kRuntime, kPrefix, kPreload, zc ? 1 : 0, fps, async ? 1 : 0, kPrefix,
             kPrefix);

    char cmd[8192];
    snprintf(cmd, sizeof cmd,
             "setsid env %s \"%s\" --z=1000000 --scale=%d --grab >> %s 2>/dev/null "
             "< /dev/null &",
             env, kBinary, scale, kLog);

    say("starting sfwm (scale=%d fps=%d zc=%d async=%d)", scale, fps, zc ? 1 : 0,
        async ? 1 : 0);
    run(cmd);

    for (int i = 0; i < 24; ++i) {
        if (exists(std::string(kRuntime) + "/sfwm-0")) break;
        sleep_ms(500);
    }
    run(std::string("ln -sf sfwm-0 ") + kRuntime + "/wayland-0");
    sleep_ms(2000);

    const std::string head = run(std::string("head -3 ") + kLog);
    fputs(head.c_str(), stdout);
    // Surface::setSwapInterval prints nothing on success; a failure is loud.
    const std::string bad = run(std::string("grep -a -i 'swapinterval\\|window magic' ") + kLog);
    if (!bad.empty()) {
        say("!! swap interval was not applied:");
        fputs(bad.c_str(), stdout);
    } else {
        say("swap interval: async (no complaint from setSwapInterval)");
    }
    return 0;
}

int cmd_mc(int argc, char **argv) {
    const char *version = argc > 0 ? argv[0] : kDefaultVersion;
    kill_all("java");
    sleep_ms(3000);
    run(std::string(": > ") + kMcLog);

    char env[4096];
    snprintf(env, sizeof env,
             "HOME=%s PREFIX=%s PATH=%s/bin:/system/bin TMPDIR=%s/tmp "
             "XDG_RUNTIME_DIR=%s WAYLAND_DISPLAY=wayland-0 "
             "LD_PRELOAD=%s "
             "LD_LIBRARY_PATH=%s:%s/lib LIBGL_DRIVERS_PATH=%s "
             "MESA_LOADER_DRIVER_OVERRIDE=msm "
             "MC_DIR=%s/.minecraft MC_NATIVES=/data/local/tmp/mc-native "
             "MC_JVM_EXTRA=-Dorg.lwjgl.opengl.libname=libEGL.so.1",
             kHome, kPrefix, kPrefix, kPrefix, kRuntime, kMcPreload, kMcLibPath, kPrefix,
             kMcDrivers, kHome);

    char cmd[8192];
    snprintf(cmd, sizeof cmd,
             "setsid env %s %s/bin/python /data/local/tmp/mcrun.py \"%s\" < /dev/null >> %s "
             "2>/dev/null &",
             env, kPrefix, version, kMcLog);
    say("launching Minecraft %s", version);
    run(cmd);

    // The loading screen is long and there is no signal to wait on, so poll the log.
    for (int i = 0; i < 60; ++i) {
        sleep_ms(2000);
        const std::string t = run(std::string("tail -c 4000 ") + kMcLog);
        if (t.find("Preparing spawn") != std::string::npos ||
            t.find("Loaded") != std::string::npos) {
            say("world is up");
            sleep_ms(3000);
            run(std::string("printf 's F11\\n' | nc -U ") + kKeys + " >/dev/null 2>&1");
            return 0;
        }
    }
    say("still loading; tail of %s:", kMcLog);
    fputs(run(std::string("tail -3 ") + kMcLog).c_str(), stdout);
    return 1;
}

int cmd_key(int argc, char **argv) {
    if (argc < 1) {
        say("usage: sfwmctl key <KEYNAME>   e.g. F3, F11, A, ESCAPE");
        return 2;
    }
    const int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", kKeys);
    if (connect(s, (struct sockaddr *)&addr, sizeof addr) != 0) {
        perror("connect (is sfwm up?)");
        close(s);
        return 1;
    }
    char line[128];
    const int n = snprintf(line, sizeof line, "s %s\n", argv[0]);
    if (write(s, line, (size_t)n) != n) { perror("write"); close(s); return 1; }
    close(s);
    say("sent %s", argv[0]);
    return 0;
}

int cmd_shot(int argc, char **argv) {
    const char *out = argc > 0 ? argv[0] : "/data/local/tmp/sfwm-shot.png";
    // A separate process on purpose: an exported LD_PRELOAD in this shell would make
    // screencap fail to resolve eglDestroySyncKHR.
    const std::string cmd = std::string("screencap -p ") + out + " 2>/dev/null";
    run(cmd);
    say("%s%s", out, exists(out) ? " written" : " NOT written");
    return exists(out) ? 0 : 1;
}

int cmd_cursor(int argc, char **argv) {
    if (argc < 1 || (strcmp(argv[0], "on") && strcmp(argv[0], "off"))) {
        say("usage: sfwmctl cursor {on|off}");
        return 2;
    }
    // The compositor polls a marker file, so the toggle needs nothing but a touch or unlink.
    // Find whichever runtime directory the running compositor is actually using.
    std::string dir;
    const char *env = getenv("SFWM_RUN");
    if (env && *env) dir = env;
    if (dir.empty() || !exists(dir + "/sfwm-0")) {
        const char *cands[] = {"/dev/sfwm", "/data/local/tmp/sfwm-xdg", nullptr};
        for (int i = 0; cands[i]; ++i) {
            if (exists(std::string(cands[i]) + "/sfwm-0")) {
                dir = cands[i];
                break;
            }
        }
    }
    if (dir.empty()) {
        say("no running compositor found");
        return 1;
    }
    const std::string marker = dir + "/cursor-off";
    if (!strcmp(argv[0], "off")) {
        const int fd = open(marker.c_str(), O_CREAT | O_WRONLY, 0644);
        if (fd < 0) { perror("touch"); return 1; }
        close(fd);
        say("cursor hidden (%s)", marker.c_str());
    } else {
        unlink(marker.c_str());
        say("cursor visible");
    }
    return 0;
}

int cmd_stop() {
    kill_all("sfwm");
    kill_all("java");
    return 0;
}

}   // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        say("usage: sfwmctl <up|mc|key|status|shot|stop> [args]");
        say("");
        say("  up [--scale N] [--fps N] [--no-zc] [--sync]   start the compositor");
        say("  mc [version]                                   start Minecraft");
        say("  key <KEYNAME>                                  inject one key, e.g. F3");
        say("  status                                         processes, panel, perf tail");
        say("  shot [path]                                    screencap");
        say("  cursor {on|off}                                runtime cursor toggle");
        say("  stop                                           kill sfwm and java");
        return 2;
    }
    const char *c = argv[1];
    const int rest = argc - 2;
    char **args = argv + 2;
    if (!strcmp(c, "up")) return cmd_up(rest, args);
    if (!strcmp(c, "mc")) return cmd_mc(rest, args);
    if (!strcmp(c, "key")) return cmd_key(rest, args);
    if (!strcmp(c, "status")) return cmd_status();
    if (!strcmp(c, "shot")) return cmd_shot(rest, args);
    if (!strcmp(c, "cursor")) return cmd_cursor(rest, args);
    if (!strcmp(c, "stop")) return cmd_stop();
    say("unknown command: %s", c);
    return 2;
}
