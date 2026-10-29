// SPDX-License-Identifier: AGPL-3.0-or-later
// prelude.hpp
//
// Standard headers, protocol headers and the wl_shm fd helper.
//
// Extracted from the former single-file implementation; behaviour is unchanged.
// Depends on include order: everything used here is provided by the modules
// included before it in src/main.cpp.
#pragma once

// sfwm - a Wayland compositor that uses SurfaceFlinger as its window manager.
//
// sfwm never composites pixels itself. Every xdg_toplevel owns a SurfaceFlinger layer,
// and position, z-order and visibility are applied through SurfaceComposerClient
// transactions, so SurfaceFlinger's hardware composer does the blending. What remains
// is the wl_shm pixel copy and the window policy.
//
// Build instructions are in README.md; wayland-scanner generates the protocol code.

#include "compositor/sf_layer.hpp"

// The wayland protocol headers have no extern "C" guard, while the generated .c files
// are compiled as C, so this block is required or the interface symbols are mangled.
extern "C" {
#include <wayland-server-core.h>
#include "protocol/screencopy.hpp"
#include <wayland-server-protocol.h>
#include "xdg-shell-server-protocol.h"
#include "linux-dmabuf-v1-server-protocol.h"
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <cerrno>
#include <cstring>
#include "wayland-drm-server-protocol.h"
#include "viewporter-server-protocol.h"
#include "presentation-time-server-protocol.h"
#include "fractional-scale-v1-server-protocol.h"
#include "cursor-shape-v1-server-protocol.h"
#include "single-pixel-buffer-v1-server-protocol.h"
#include "tearing-control-v1-server-protocol.h"
#include "relative-pointer-unstable-v1-server-protocol.h"
#include "pointer-constraints-unstable-v1-server-protocol.h"
#include "idle-inhibit-unstable-v1-server-protocol.h"
#include "keyboard-shortcuts-inhibit-unstable-v1-server-protocol.h"
#include "text-input-unstable-v3-server-protocol.h"
#include "xdg-output-unstable-v1-server-protocol.h"
#include "xdg-decoration-unstable-v1-server-protocol.h"
#include "primary-selection-unstable-v1-server-protocol.h"
#include "xwayland-shell-v1-server-protocol.h"
#include "wlr-layer-shell-unstable-v1-server-protocol.h"
#include "xdg-activation-v1-server-protocol.h"
#include "content-type-v1-server-protocol.h"
#include "alpha-modifier-v1-server-protocol.h"
#include "fifo-v1-server-protocol.h"
#include "pointer-gestures-unstable-v1-server-protocol.h"
#include "pointer-warp-v1-server-protocol.h"
#include "linux-drm-syncobj-v1-server-protocol.h"
#include <xf86drm.h>
}

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <stddef.h>
#include <time.h>
#include <unistd.h>

#include <xkbcommon/xkbcommon.h>

#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Fds handed to clients must not come from memfd_create. A memfd inherits the SELinux
// label of its creator, and sfwm runs as root while clients run as applications; an fd
// crossing MLS categories is denied by security_file_receive, and the rule is dontaudit,
// so nothing appears in dmesg. The client just loses the fd, which breaks Xwayland, OBS
// and EGL initialisation. A regular file in XDG_RUNTIME_DIR carries a label in the
// client's own category, so the transfer is allowed.
// ─────────────────────────────────────────────────────────────────────────────
static int sfwm_rt_file(const char *name, size_t size) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !*rt) return -1;
    static int seq = 0;
    size_t n = strlen(rt) + strlen(name) + 32;
    char *p = (char *)malloc(n);
    if (!p) return -1;
    snprintf(p, n, "%s/%s-%d-%d", rt, name, (int)getpid(), ++seq);
    int fd = open(p, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        fprintf(stderr, "[sf] cannot create %s: %s\n", p, strerror(errno));
        free(p);
        return -1;
    }
    if (size > 0 && ftruncate(fd, (off_t)size) < 0) {
        fprintf(stderr, "[sf] ftruncate %s failed: %s\n", p, strerror(errno));
        close(fd);
        free(p);
        return -1;
    }
    chmod(p, 0644);
    // Unlink immediately: the fd stays valid and the file goes away with the last one.
    unlink(p);
    free(p);
    return fd;
}

