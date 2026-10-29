// SPDX-License-Identifier: AGPL-3.0-or-later
// display_ctl.hpp - backends for panel brightness, colour temperature and display power.
//
// Android and Linux already provide these interfaces, so no data model is invented here:
//   brightness  /sys/class/backlight/<dev>/brightness (writable as root, standard Linux)
//   colour      cmd display dwb-set-cct <K>, with -1 removing the override
//   power       cmd display power-off / power-reset <id> (DisplayManager shell commands)
//
// These are the backends behind zwlr_gamma_control_v1 (gammastep, wlsunset) and
// zwlr_output_power_manager_v1 (wlopm), which previously only logged their requests.
//
// Measured on PLQ110 (Android 16, root): panel0-backlight reports max_brightness 4094 and
// accepts writes immediately; dwb-set-cct returns 0 for 4500 and for -1; power-off 0
// followed by power-reset 0 blanks the panel and restores it completely.
#pragma once

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

struct DispCtl {
    char bl_path[192] = {0};   // /sys/class/backlight/xxx/brightness
    int  bl_max  = 0;
    int  bl_orig = -1;         // brightness before the first change, used to restore
    int  bl_now  = -1;
    int  cct_now = -1;         // -1 means no override is set
    int  disp_id = -1;         // logical display id, 0 is the built-in panel
    bool screen_on = true;
};
static DispCtl g_dc;

// ------------------------------------------------------------- helpers
static int disp_run(char *const argv[]) {
    pid_t p = fork();
    if (p < 0) { printf("[disp] fork failed\n"); return -1; }
    if (p == 0) { execv(argv[0], argv); _exit(127); }
    int st = 0;
    if (waitpid(p, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int disp_read_int(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char b[32] = {0};
    ssize_t n = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0) return -1;
    return atoi(b);
}

static bool disp_write_int(const char *path, int v) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) { printf("[disp] cannot open %s: %s\n", path, strerror(errno)); return false; }
    char b[32];
    int n = snprintf(b, sizeof(b), "%d\n", v);
    ssize_t w = write(fd, b, (size_t)n);
    close(fd);
    if (w != n) { printf("[disp] cannot write %s: %s\n", path, strerror(errno)); return false; }
    return true;
}

// ---------------------------------------------------------- brightness
// The backlight device is found by scanning the class directory rather than with glob(),
// which keeps the lookup to opendir and readdir. The first entry that exposes a writable
// brightness file is used.
static bool disp_bl_init() {
    if (g_dc.bl_path[0]) return g_dc.bl_max > 0;
    DIR *d = opendir("/sys/class/backlight");
    if (!d) {
        printf("[disp] cannot open /sys/class/backlight, brightness is not controllable\n");
        return false;
    }
    bool found = false;
    while (struct dirent *e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        char path[sizeof(g_dc.bl_path)];
        snprintf(path, sizeof(path), "/sys/class/backlight/%s/brightness", e->d_name);
        if (access(path, W_OK) != 0) continue;
        snprintf(g_dc.bl_path, sizeof(g_dc.bl_path), "%s", path);
        found = true;
        break;
    }
    closedir(d);
    if (!found) {
        printf("[disp] no writable backlight under /sys/class/backlight\n");
        return false;
    }
    char maxp[sizeof(g_dc.bl_path) + 16];
    snprintf(maxp, sizeof(maxp), "%s", g_dc.bl_path);
    char *slash = strrchr(maxp, '/');
    if (slash) snprintf(slash, sizeof(maxp) - (size_t)(slash - maxp), "/max_brightness");
    g_dc.bl_max = disp_read_int(maxp);
    if (g_dc.bl_max <= 0) g_dc.bl_max = 4095;   // fall back to a conservative maximum
    g_dc.bl_now = disp_read_int(g_dc.bl_path);
    printf("[disp] backlight %s max=%d current=%d\n", g_dc.bl_path, g_dc.bl_max, g_dc.bl_now);
    return true;
}

static int disp_brightness_pct() {
    if (!disp_bl_init() || g_dc.bl_max <= 0) return -1;
    int v = disp_read_int(g_dc.bl_path);
    if (v < 0) return -1;
    return (int)((int64_t)v * 100 / g_dc.bl_max);
}

// pct: 0..100; 0 is clamped to 1, full black belongs to the power switch
static bool disp_set_brightness_pct(int pct) {
    if (!disp_bl_init()) return false;
    if (pct < 1) pct = 1;
    if (pct > 100) pct = 100;
    int cur = disp_read_int(g_dc.bl_path);
    if (cur > 0 && g_dc.bl_orig < 0) g_dc.bl_orig = cur;   // remember the value before the first change
    int v = (int)((int64_t)g_dc.bl_max * pct / 100);
    if (v < 1) v = 1;
    if (!disp_write_int(g_dc.bl_path, v)) return false;
    g_dc.bl_now = v;
    printf("[disp] brightness -> %d%% (%d/%d)\n", pct, v, g_dc.bl_max);
    return true;
}

// Restore the brightness in use before the first change.
static void disp_brightness_restore() {
    if (g_dc.bl_orig < 0) return;
    disp_write_int(g_dc.bl_path, g_dc.bl_orig);
    printf("[disp] brightness restored to %d\n", g_dc.bl_orig);
    g_dc.bl_now = g_dc.bl_orig;
}

// ------------------------------------------------------- colour temperature
// k <= 0 or k >= 6500 means no override: keep the native white point.
static void disp_set_cct(int k) {
    if (k >= 6500 || k <= 0) {
        if (g_dc.cct_now < 0) return;          // no override was set
        char *argv[] = { (char *)"/system/bin/cmd", (char *)"display",
                         (char *)"dwb-set-cct", (char *)"-1", nullptr };
        int rc = disp_run(argv);
        g_dc.cct_now = -1;
        printf("[disp] colour override disabled, rc=%d\n", rc);
        return;
    }
    if (k < 1000) k = 1000;
    if (k > 10000) k = 10000;
    if (g_dc.cct_now == k) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", k);
    char *argv[] = { (char *)"/system/bin/cmd", (char *)"display",
                     (char *)"dwb-set-cct", buf, nullptr };
    int rc = disp_run(argv);
    g_dc.cct_now = k;
    printf("[disp] colour temperature -> %dK, rc=%d\n", k, rc);
}

// -------------------------------------------------------- display power
static void disp_set_screen(bool on) {
    char id[16];
    snprintf(id, sizeof(id), "%d", g_dc.disp_id < 0 ? 0 : g_dc.disp_id);
    char *off[] = { (char *)"/system/bin/cmd", (char *)"display",
                    (char *)"power-off", id, nullptr };
    char *on_[] = { (char *)"/system/bin/cmd", (char *)"display",
                    (char *)"power-reset", id, nullptr };
    int rc = disp_run(on ? on_ : off);
    g_dc.screen_on = on;
    printf("[disp] display %s (%s) rc=%d\n", on ? "on" : "off", id, rc);
}

// ----------------------------------------------------------- gamma ramp
// A wlroots gamma ramp is three increasing uint16 ramps, r[size] g[size] b[size].
// It encodes two properties at once:
//   overall level < 1  dimming, that is brightness
//   b/r ratio < 1      warm tint, that is colour temperature
// Both are translated into real backend calls here.
// Brightness is applied as a factor on the original value, not overwritten, so a
// colour-only ramp (level 1) does not reset a brightness set by the user.
static void disp_apply_ramp(const uint16_t *ramp, uint32_t size) {
    if (!ramp || size == 0) return;
    double ar = 0, ag = 0, ab = 0;
    for (uint32_t i = 0; i < size; i++) {
        ar += ramp[i];
        ag += ramp[size + i];
        ab += ramp[2u * size + i];
    }
    ar /= size; ag /= size; ab /= size;
    // An identity ramp averages 32767.5.
    const double mx = fmax(ar, fmax(ag, ab));
    if (mx <= 1.0) return;                       // all-black ramp: leave brightness alone
    const double level = mx / 32767.5;           // 1.0 means no dimming

    if (level < 0.97) {
        // Record the original brightness before the first dim, so that an identity
        // ramp applied later restores it.
        if (g_dc.bl_orig < 0 && disp_bl_init()) {
            disp_bl_init();
            g_dc.bl_orig = disp_read_int(g_dc.bl_path);
        }
        if (g_dc.bl_orig > 0) {
            int v = (int)((double)g_dc.bl_orig * level);
            if (v < 1) v = 1;
            if (disp_write_int(g_dc.bl_path, v)) {
                g_dc.bl_now = v;
                printf("[disp] ramp dim x%.2f -> backlight %d/%d\n", level, v, g_dc.bl_max);
            }
        }
    } else {
        disp_brightness_restore();
    }

    // Colour temperature: normalise to the brightest channel and compare b/r.
    const double rn = ar / mx, bn = ab / mx;
    double ratio = (rn > 1e-6) ? (bn / rn) : 1.0;
    // Measured points on the blackbody locus, b/r against colour temperature.
    static const double tbl[][2] = {
        {1.00, 6500}, {0.92, 5700}, {0.85, 5000}, {0.70, 4100},
        {0.55, 3400}, {0.47, 3000}, {0.40, 2700}, {0.33, 2200},
    };
    const int n = (int)(sizeof(tbl) / sizeof(tbl[0]));
    if (ratio >= 0.97) {
        disp_set_cct(-1);                        // neutral: drop the override
    } else {
        int k = 2200;
        for (int i = 0; i + 1 < n; i++) {
            if (ratio <= tbl[i][0] && ratio >= tbl[i + 1][0]) {
                const double t = (tbl[i][0] - ratio) / (tbl[i][0] - tbl[i + 1][0]);
                k = (int)(tbl[i][1] + t * (tbl[i + 1][1] - tbl[i][1]));
                break;
            }
        }
        if (ratio > tbl[0][0]) k = 6500;
        disp_set_cct(k);
    }
}

static void disp_status() {
    printf("[disp] brightness=%d%% (%d/%d)  cct=%d%s  screen=%s\n",
           disp_brightness_pct(), g_dc.bl_now, g_dc.bl_max,
           g_dc.cct_now, g_dc.cct_now < 0 ? " (native)" : "K",
           g_dc.screen_on ? "on" : "off");
}
