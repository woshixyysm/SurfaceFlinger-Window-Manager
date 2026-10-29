// SPDX-License-Identifier: AGPL-3.0-or-later
// misc_ext3.hpp
//
// Small but useful protocols that wlroots and Hyprland clients probe for.
// This file covers:
//   - zwlr_output_power_manager_v1, wlopm, backed by cmd display power-off and reset
//   - zwlr_gamma_control_manager_v1, gammastep and wlsunset, backed by the
//     backlight sysfs file and cmd display dwb-set-cct
//
// Both are wlroots protocols, vendored into protocols/.
// The hardware backends live in protocol/display_ctl.hpp.
#pragma once

#include "wlr-output-power-management-unstable-v1-server-protocol.h"
#include "wlr-gamma-control-unstable-v1-server-protocol.h"
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <vector>

// ======================================================= zwlr_output_power_v1
static const char *opm_name(int mode) { return mode == 1 ? "on" : "off"; }

static void op_destroy_req(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

// mode 0 is off and 1 on; the panel is really switched through cmd display
static void op_set_mode(wl_client *, wl_resource *r, uint32_t mode) {
    printf("[opm] output power request mode=%s(%u)\n", opm_name((int)mode), mode);
    disp_set_screen(mode == 1);
    zwlr_output_power_v1_send_mode(r, mode);
}

static const struct zwlr_output_power_v1_interface op_impl = {
    .set_mode = op_set_mode,
    .destroy  = op_destroy_req,
};

static void opm_get_output_power(wl_client *c, wl_resource *res, uint32_t id, wl_resource *output) {
    (void)output;
    wl_resource *r = wl_resource_create(c, &zwlr_output_power_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &op_impl, nullptr, nullptr);
    // initial state is on
    zwlr_output_power_v1_send_mode(r, 1);
    printf("[opm] new output_power object\n");
}

static void opm_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct zwlr_output_power_manager_v1_interface opm_impl = {
    .destroy          = opm_destroy,
    .get_output_power = opm_get_output_power,
};

static void bind_output_power(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &zwlr_output_power_manager_v1_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &opm_impl, nullptr, nullptr);
    printf("[opm] zwlr_output_power_manager_v1 bound\n");
}

// =================================================== zwlr_gamma_control_v1
static void gc_destroy_req(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static void gc_set_gamma(wl_client *, wl_resource *r, int32_t fd) {
    // ramp is r, g and b of size uint16 each, passed as a memfd or a file
    if (fd < 0) return;
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0) { close(fd); return; }
    lseek(fd, 0, SEEK_SET);
    const uint32_t size = (uint32_t)(sz / 6);        // three channels of two bytes
    if (size == 0 || size > 65536) { close(fd); return; }
    const size_t want = (size_t)size * 6;
    std::vector<uint16_t> ramp((size_t)size * 3);
    uint8_t *p = (uint8_t *)ramp.data();
    size_t got = 0;
    while (got < want) {
        ssize_t n = read(fd, p + got, want - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);                                       // the fd must be closed or it leaks
    if (got < want) { printf("[gc] short gamma ramp read %zu/%zu\n", got, want); return; }
    printf("[gc] received gamma ramp size=%u\n", size);
    disp_apply_ramp(ramp.data(), size);
}

static const struct zwlr_gamma_control_v1_interface gc_impl = {
    .set_gamma = gc_set_gamma,
    .destroy   = gc_destroy_req,
};

static void gcm_get_gamma_control(wl_client *c, wl_resource *res, uint32_t id, wl_resource *output) {
    (void)output;
    wl_resource *r = wl_resource_create(c, &zwlr_gamma_control_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &gc_impl, nullptr, nullptr);
    // Report the ramp size, that is the number of entries per channel.
    zwlr_gamma_control_v1_send_gamma_size(r, 256);
    printf("[gc] new gamma_control, ramp size reported as 256\n");
}

static void gcm_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct zwlr_gamma_control_manager_v1_interface gcm_impl = {
    .get_gamma_control = gcm_get_gamma_control,
    .destroy           = gcm_destroy,
};

static void bind_gamma_control(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &zwlr_gamma_control_manager_v1_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &gcm_impl, nullptr, nullptr);
    printf("[gc] zwlr_gamma_control_manager_v1 bound\n");
}
