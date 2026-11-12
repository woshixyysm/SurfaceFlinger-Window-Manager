// SPDX-License-Identifier: AGPL-3.0-or-later
// main.cpp - entry point and module assembly.
//
// The project builds as a single translation unit: the include block below follows the
// order of the original single-file source, and the modules share static globals, so
// that order must be preserved when moving includes.
//
// The behaviour of individual protocols follows the compositors named in the "Prior art"
// section of README.md; the comment where an approach was taken from one of them says so.

#include "core/prelude.hpp"
#include "core/config.hpp"
#include "core/trace.hpp"
#include "render/cursor.hpp"
#include "render/testpat.hpp"
// GPU composition backend: client dmabuf to EGLImage, GL texture, FBO, and a read-back
// into the SurfaceFlinger buffer. Must precede compositor/surface.hpp, which calls it.
#include "render/gl_comp.hpp"
// Direct scanout of client dmabufs through ASurfaceControl, ahead of the GL path in
// compositor/surface.hpp. Must come after config.hpp for DmabufBuf.
#include "render/direct.hpp"
#include "compositor/surface.hpp"
#include "protocol/linux_dmabuf.hpp"
#include "protocol/wl_drm.hpp"
#include "protocol/compositor.hpp"
#include "protocol/subcompositor.hpp"
#include "protocol/output.hpp"
// Real backends for brightness, colour temperature and display power. Included before
// keyboard.hpp, whose injection socket exposes the bright/cct/screen commands.
#include "protocol/display_ctl.hpp"
// Workspaces (ext_workspace_manager_v1); the data comes from g_srv.views and View::ws.
#include "protocol/workspace.hpp"
// Android windows hosted by the optional bridge module. Included before tiling.hpp, which
// places them in the same layout as the Wayland windows.
#include "compositor/appwin.hpp"
// Tiling layouts. Included after the workspace state it reads and before the input modules
// that expose the tile commands and the three-finger gesture.
#include "compositor/tiling.hpp"
// Session lock (ext_session_lock_manager_v1), implemented by hiding SF layers.
#include "protocol/session_lock.hpp"
// ext_image_copy_capture_v1, reusing the screencopy backend for capture.
#include "protocol/image_capture.hpp"
#include "input/seat.hpp"
#include "input/keyboard.hpp"
#include "input/inject.hpp"
#include "input/pointer.hpp"
#include "input/touch.hpp"
#include "protocol/data_device.hpp"
#include "protocol/misc_ext.hpp"
#include "protocol/syncobj.hpp"
#include "protocol/presentation.hpp"
#include "protocol/activation.hpp"
#include "shell/layer_shell.hpp"
#include "shell/xdg_shell.hpp"
#include "protocol/misc_ext2.hpp"
#include "protocol/xwayland.hpp"
#include "protocol/foreign_toplevel.hpp"
#include "protocol/idle_notify.hpp"
#include "protocol/toplevel_icon.hpp"
#include "protocol/misc_ext3.hpp"
// Implemented for client compatibility:
//   wp_commit_timing_manager_v1       frame timing constraints
//   wp_security_context_manager_v1    sandbox identity and a real accept path
//   zwlr_virtual_pointer_manager_v1   virtual pointer for automation
//   ext/zwlr_data_control_manager_v1  clipboard managers, independent of focus
//   zwlr_foreign_toplevel_manager_v1  taskbars and window lists
#include "protocol/commit_timing.hpp"
#include "protocol/security_context.hpp"
#include "protocol/virtual_pointer.hpp"
#include "protocol/data_control.hpp"
#include "protocol/foreign_toplevel_mgr.hpp"

// ---------------------------------------------------------------- main
// ---------------------------------------------------------------- display size
// screencap prints the display size in its current rotation as the first two uint32
// values of its output, before the pixels. It is the only size query that follows the
// rotation: `wm size` reports the panel, not the rotated display.
static bool detect_display_size(int32_t *w, int32_t *h) {
    // LD_PRELOAD holds libfdshim and libeglcompat for the compositor; a child that inherits
    // them crashes (screencap segfaults before writing its header), so clear it first.
    FILE *p = popen("/system/bin/env -u LD_PRELOAD -u LD_LIBRARY_PATH "
                    "/system/bin/screencap 2>/dev/null", "r");
    if (!p) {
        printf("[sfwm] screencap: %s\n", strerror(errno));
        return false;
    }
    uint32_t hdr[2] = {0, 0};
    const size_t n = fread(hdr, sizeof(uint32_t), 2, p);
    pclose(p);
    if (n != 2) {
        printf("[sfwm] screencap header gave %zu words\n", n);
        return false;
    }
    const int32_t dw = (int32_t)hdr[0], dh = (int32_t)hdr[1];
    if (dw < 200 || dh < 200 || dw > 20000 || dh > 20000) return false;
    *w = dw;
    *h = dh;
    return true;
}

int main(int argc, char **argv) {
    // Line buffering, so a crash does not cut the tail of the log when stdout is a file.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    bool size_given = false, rotate_given = false;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--size=", 7)) { sscanf(argv[i] + 7, "%dx%d", &g_out_w, &g_out_h); size_given = true; }
        else if (!strncmp(argv[i], "--z=", 4)) g_base_z = atoi(argv[i] + 4);
        else if (!strncmp(argv[i], "--socket=", 9)) g_socket = argv[i] + 9;
        else if (!strncmp(argv[i], "--scale=", 8)) g_scale = atoi(argv[i] + 8);
        else if (!strncmp(argv[i], "--rotate=", 9)) { g_rotate = ((atoi(argv[i] + 9) % 360) + 360) % 360; rotate_given = true; }
        else if (!strcmp(argv[i], "--grab")) g_grab = true;
        else if (!strcmp(argv[i], "--clickthrough")) g_clickthrough = true;
        else if (!strcmp(argv[i], "--touch")) g_touch_enabled = true;
    }
    if (!size_given) {
        int32_t dw = g_out_w, dh = g_out_h;
        if (detect_display_size(&dw, &dh)) {
            g_out_w = dw;
            g_out_h = dh;
            printf("[sfwm] detected display %dx%d\n", dw, dh);
        } else {
            printf("[sfwm] display size detection failed, keeping %dx%d\n", g_out_w, g_out_h);
        }
    }
    // A landscape output over a portrait panel needs the touch panel's axes rotated. 90
    // is the rotation the display uses when the device is turned counter-clockwise;
    // --rotate=270 covers the other direction.
    if (!rotate_given && g_out_w > g_out_h) {
        g_rotate = 90;
        printf("[sfwm] landscape output, assuming touch rotation 90 (override with --rotate=270)\n");
    }
    printf("[sfwm] output %dx%d (logical %dx%d), scale=%d, base_z=%d, rotate=%d, socket=%s\n",
           g_out_w, g_out_h, g_out_w / g_scale, g_out_h / g_scale, g_scale, g_base_z, g_rotate, g_socket);

    sf::Lib &L = sf::Lib::get();
    if (!L.ok) { fprintf(stderr, "[sfwm] libgui initialisation failed\n"); return 1; }

    g_srv.dpy = wl_display_create();
    if (!g_srv.dpy) { fprintf(stderr, "[sfwm] wl_display_create failed\n"); return 1; }
    g_srv.loop = wl_display_get_event_loop(g_srv.dpy);

    // Poll the input method state file every 300 ms for keyboard height and visibility.
    g_ime_timer = wl_event_loop_add_timer(g_srv.loop, on_ime_tick, nullptr);
    if (g_ime_timer) wl_event_source_timer_update(g_ime_timer, 300);

    // A long press of 400 ms moves a window. The top edge band alone is too narrow to
    // hit on a touchscreen, so the timer carries the main move gesture.
    g_lp_timer = wl_event_loop_add_timer(g_srv.loop, on_longpress, nullptr);

    // wl_surface.frame throttling: the timer is armed on demand and releases callbacks in
    // 16 ms batches. Sending them straight from commit() gives clients no rate limit.
    g_frame_timer = wl_event_loop_add_timer(g_srv.loop, on_frame_tick, nullptr);
    trace_init();

    wl_global_create(g_srv.dpy, &wl_compositor_interface, 6, nullptr, bind_compositor);
    wl_global_create(g_srv.dpy, &wl_subcompositor_interface, 1, nullptr, bind_subcompositor);
    wl_global_create(g_srv.dpy, &zwp_linux_dmabuf_v1_interface, 4, nullptr, bind_dmabuf);
    wl_global_create(g_srv.dpy, &wl_drm_interface, 2, nullptr, bind_drm);
    wl_global_create(g_srv.dpy, &wl_shm_interface, 1, nullptr, bind_shm);
    wl_global_create(g_srv.dpy, &wl_output_interface, 4, nullptr, bind_output);
    sc_screencopy_init(g_srv.dpy, g_out_w, g_out_h);   // zwlr_screencopy_manager_v1, see protocol/screencopy.hpp
    // wl_seat is advertised at version 9: clients such as aquamarine bind version 9
    // unconditionally, and libwayland-server rejects a lower advertised version.
    wl_global_create(g_srv.dpy, &wl_seat_interface, 9, nullptr, bind_seat);
    wl_global_create(g_srv.dpy, &xdg_wm_base_interface, 6, nullptr, bind_wm_base);

    // Protocols that most clients and shells bind at startup.
    wl_global_create(g_srv.dpy, &wp_viewporter_interface, 1, nullptr, bind_viewporter);
    wl_global_create(g_srv.dpy, &wp_fractional_scale_manager_v1_interface, 1, nullptr, bind_fracescale);
    wl_global_create(g_srv.dpy, &wp_cursor_shape_manager_v1_interface, 1, nullptr, bind_cursorshape);
    wl_global_create(g_srv.dpy, &wp_single_pixel_buffer_manager_v1_interface, 1, nullptr, bind_spbuffer);
    wl_global_create(g_srv.dpy, &wp_tearing_control_manager_v1_interface, 1, nullptr, bind_tearing);
    wl_global_create(g_srv.dpy, &zwp_relative_pointer_manager_v1_interface, 1, nullptr, bind_relptr);
    wl_global_create(g_srv.dpy, &zwp_pointer_constraints_v1_interface, 1, nullptr, bind_ptrcon);
    wl_global_create(g_srv.dpy, &zwp_idle_inhibit_manager_v1_interface, 1, nullptr, bind_idleinhibit);
    wl_global_create(g_srv.dpy, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1, nullptr, bind_ksinhibit);
    wl_global_create(g_srv.dpy, &zxdg_output_manager_v1_interface, 3, nullptr, bind_xdgoutput);
    wl_global_create(g_srv.dpy, &zxdg_decoration_manager_v1_interface, 1, nullptr, bind_decoration);
    wl_global_create(g_srv.dpy, &wl_data_device_manager_interface, 3, nullptr, bind_datadev);
    // Toplevel lists used by quickshell, niri and waybar.
    wl_global_create(g_srv.dpy, &ext_foreign_toplevel_list_v1_interface, 1, nullptr, bind_foreign_toplevel_list);
    // Idle notification, used by swayidle, hypridle and automatic locking.
    wl_global_create(g_srv.dpy, &ext_idle_notifier_v1_interface, 1, nullptr, bind_idle_notify);
    // Toplevel icons, together with ext_foreign_toplevel_list_v1.
    wl_global_create(g_srv.dpy, &xdg_toplevel_icon_manager_v1_interface, 1, nullptr, bind_toplevel_icon);
    // Small wlroots protocols.
    wl_global_create(g_srv.dpy, &zwlr_output_power_manager_v1_interface, 1, nullptr, bind_output_power);
    wl_global_create(g_srv.dpy, &zwlr_gamma_control_manager_v1_interface, 1, nullptr, bind_gamma_control);
    // Frame timing, sandbox identity, virtual pointer, clipboard managers, toplevel lists.
    wl_global_create(g_srv.dpy, &wp_commit_timing_manager_v1_interface, 1, nullptr, bind_commit_timing);
    wl_global_create(g_srv.dpy, &wp_security_context_manager_v1_interface, 1, nullptr, bind_security_context);
    wl_global_create(g_srv.dpy, &zwlr_virtual_pointer_manager_v1_interface, 2, nullptr, bind_virtual_pointer);
    wl_global_create(g_srv.dpy, &ext_data_control_manager_v1_interface, 1, nullptr, bind_ext_data_control);
    wl_global_create(g_srv.dpy, &zwlr_data_control_manager_v1_interface, 2, nullptr, bind_wlr_data_control);
    wl_global_create(g_srv.dpy, &zwlr_foreign_toplevel_manager_v1_interface, 3, nullptr, bind_foreign_toplevel_mgr);
    // Workspaces for bars and desktop indicators.
    wl_global_create(g_srv.dpy, &ext_workspace_manager_v1_interface, 1, nullptr, bind_workspace);
    // Session lock for swaylock, hyprlock and quickshell.
    wl_global_create(g_srv.dpy, &ext_session_lock_manager_v1_interface, 1, nullptr, bind_session_lock);
    // Screen capture for grim and wl-screenrec.
    wl_global_create(g_srv.dpy, &ext_output_image_capture_source_manager_v1_interface, 1, nullptr, bind_output_capture_source);
    wl_global_create(g_srv.dpy, &ext_image_copy_capture_manager_v1_interface, 1, nullptr, bind_image_copy_capture);
    wl_global_create(g_srv.dpy, &zwp_primary_selection_device_manager_v1_interface, 1, nullptr, bind_primsel);
    wl_global_create(g_srv.dpy, &xwayland_shell_v1_interface, 1, nullptr, bind_xwlshell);
    wl_global_create(g_srv.dpy, &zwp_text_input_manager_v3_interface, 2, nullptr, bind_ti_mgr);
    // Layer shell, the basis for panels, trays and launchers.
    wl_global_create(g_srv.dpy, &zwlr_layer_shell_v1_interface, 4, nullptr, bind_lshell);
    wl_global_create(g_srv.dpy, &xdg_activation_v1_interface, 1, nullptr, bind_activation);
    wl_global_create(g_srv.dpy, &wp_presentation_interface, 2, nullptr, bind_presentation);
    // Small extensions, bound mainly to probe for their presence.
    wl_global_create(g_srv.dpy, &wp_content_type_manager_v1_interface, 1, nullptr, bind_content_type);
    wl_global_create(g_srv.dpy, &wp_alpha_modifier_v1_interface, 1, nullptr, bind_alpha_modifier);
    wl_global_create(g_srv.dpy, &wp_fifo_manager_v1_interface, 1, nullptr, bind_fifo);
    wl_global_create(g_srv.dpy, &zwp_pointer_gestures_v1_interface, 3, nullptr, bind_pointer_gestures);
    wl_global_create(g_srv.dpy, &wp_pointer_warp_v1_interface, 1, nullptr, bind_pointer_warp);
    if (syncobj_supported())
        wl_global_create(g_srv.dpy, &wp_linux_drm_syncobj_manager_v1_interface, 1, nullptr, bind_syncobj);
    else
        printf("[syncobj] driver has no DRM_CAP_SYNCOBJ_TIMELINE, protocol not advertised; clients use implicit sync\n");

    if (wl_display_add_socket(g_srv.dpy, g_socket) != 0) {
        fprintf(stderr, "[sfwm] add_socket failed: %s\n", strerror(errno));
        return 1;
    }
    const char *rt = getenv("XDG_RUNTIME_DIR");
    // Clients running as another user connect through this socket. The runtime directory
    // is created by the session script with mode 0700; only the socket is made writable.
    if (rt) {
        mkdir(rt, 0700);
        std::string sock = std::string(rt) + "/" + g_socket;
        chmod(sock.c_str(), 0666);
    }
    printf("[sfwm] WAYLAND_DISPLAY=%s  (socket at %s/%s)\n",
           g_socket, rt ? rt : "?", g_socket);

    int evfd = open_touch_device(g_grab);
    if (evfd >= 0) {
        g_have_input = true;
        wl_event_loop_add_fd(g_srv.loop, evfd, WL_EVENT_READABLE, on_input_fd, nullptr);
    } else {
        printf("[input] no touch device found; reading /dev/input requires root\n");
    }

    // A physical keyboard and mouse are two more input sources, next to the injection socket.
    // They are read here and, while a Wayland window has the focus, also grabbed, so the shell
    // neither consumes the keys nor draws a cursor of its own.
    g_kbd_fd = open_keyboard_device();
    if (g_kbd_fd >= 0)
        wl_event_loop_add_fd(g_srv.loop, g_kbd_fd, WL_EVENT_READABLE, on_key_fd, nullptr);
    g_ptr_fd = open_pointer_device();
    if (g_ptr_fd >= 0) {
        g_mouse_x = g_out_w / 2;
        g_mouse_y = g_out_h / 2;
        wl_event_loop_add_fd(g_srv.loop, g_ptr_fd, WL_EVENT_READABLE, on_pointer_fd, nullptr);
    }

    sfwm_keys_init();   // keyboard injection socket

    // Poll the bridge's published state for Android windows, so a window opened by the
    // module joins the layout even when sfwm did not ask for it.
    g_app_timer = wl_event_loop_add_timer(g_srv.loop, on_appwin_tick, nullptr);
    if (g_app_timer) wl_event_source_timer_update(g_app_timer, 300);
    appwin_poll();

    wl_display_run(g_srv.dpy);
    // The pointer picture may be the system's sprite at this moment, and nothing else will
    // take it away: the module keeps it up until it is told otherwise.
    if (g_sprite_shown) appwin_sprite("hide", 0, 0);
    return 0;
}
