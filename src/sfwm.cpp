// sfwm.cpp —— 入口与装配
//
// 单 TU 的组合方式：下面这一串 include 的顺序 = 代码在原始单文件里的顺序。
// 所以它等价于原来那个 5631 行的大文件，只是被切成了可读的模块。
// 每个模块的文件头都写了它依赖什么；挪动 include 顺序会改变语义，别乱动。

#include "core/prelude.hpp"
#include "core/config.hpp"
#include "core/trace.hpp"
#include "render/cursor.hpp"
#include "render/testpat.hpp"
#include "compositor/surface.hpp"
#include "protocol/linux_dmabuf.hpp"
#include "protocol/wl_drm.hpp"
#include "protocol/compositor.hpp"
#include "protocol/subcompositor.hpp"
#include "protocol/output.hpp"
#include "input/seat.hpp"
#include "input/keyboard.hpp"
#include "input/inject.hpp"
#include "input/pointer.hpp"
#include "input/touch.hpp"
#include "protocol/misc_ext.hpp"
#include "protocol/syncobj.hpp"
#include "protocol/presentation.hpp"
#include "protocol/activation.hpp"
#include "shell/layer_shell.hpp"
#include "shell/xdg_shell.hpp"
#include "protocol/misc_ext2.hpp"
#include "protocol/xwayland.hpp"

// ---------------------------------------------------------------- main
int main(int argc, char **argv) {
    // 重定向到文件时 stdout 默认是全缓冲，段错时日志尾巴会丢 —— 强制行缓冲
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--size=", 7)) sscanf(argv[i] + 7, "%dx%d", &g_out_w, &g_out_h);
        else if (!strncmp(argv[i], "--z=", 4)) g_base_z = atoi(argv[i] + 4);
        else if (!strncmp(argv[i], "--socket=", 9)) g_socket = argv[i] + 9;
        else if (!strncmp(argv[i], "--scale=", 8)) g_scale = atoi(argv[i] + 8);
        else if (!strcmp(argv[i], "--grab")) g_grab = true;
        else if (!strcmp(argv[i], "--clickthrough")) g_clickthrough = true;
        else if (!strcmp(argv[i], "--touch")) g_touch_enabled = true;
    }
    printf("[sfwm] 输出 %dx%d (逻辑 %dx%d), scale=%d, base_z=%d, socket=%s\n",
           g_out_w, g_out_h, g_out_w / g_scale, g_out_h / g_scale, g_scale, g_base_z, g_socket);

    sf::Lib &L = sf::Lib::get();
    if (!L.ok) { fprintf(stderr, "[sfwm] libgui 初始化失败\n"); return 1; }

    g_srv.dpy = wl_display_create();
    if (!g_srv.dpy) { fprintf(stderr, "[sfwm] wl_display_create 失败\n"); return 1; }
    g_srv.loop = wl_display_get_event_loop(g_srv.dpy);

    // 轮询模块写的输入法状态文件（300ms）：键盘高度 + 显示状态
    g_ime_timer = wl_event_loop_add_timer(g_srv.loop, on_ime_tick, nullptr);
    if (g_ime_timer) wl_event_source_timer_update(g_ime_timer, 300);

    // 长按 400ms -> 移动窗口。
    // ⚠️ 以前 on_longpress() 写好了、但 timer **从没创建也没布防**，
    //    所以移动窗口的唯一入口只剩顶部 44*scale 那条细带，手机上基本捏不到，
    //    手指一拖就落在四个角的缩放带里 —— 表现就是“只能缩放、没法移动”。
    g_lp_timer = wl_event_loop_add_timer(g_srv.loop, on_longpress, nullptr);
    trace_init();

    wl_global_create(g_srv.dpy, &wl_compositor_interface, 6, nullptr, bind_compositor);
    wl_global_create(g_srv.dpy, &wl_subcompositor_interface, 1, nullptr, bind_subcompositor);
    wl_global_create(g_srv.dpy, &zwp_linux_dmabuf_v1_interface, 4, nullptr, bind_dmabuf);
    wl_global_create(g_srv.dpy, &wl_drm_interface, 2, nullptr, bind_drm);
    wl_global_create(g_srv.dpy, &wl_shm_interface, 1, nullptr, bind_shm);
    wl_global_create(g_srv.dpy, &wl_output_interface, 4, nullptr, bind_output);
    sc_screencopy_init(g_srv.dpy, g_out_w, g_out_h);   // zwlr_screencopy_manager_v1（见 sc_screencopy.hpp）
    // seat 版本：aquamarine（Hyprland 的 wayland 后端）是硬编码 bind(wl_seat, 9) 的，
    // 宣告 7 会被我们自己的 libwayland-server 拒绝：
    //   "invalid version for global wl_seat (7): expected at most 7, got 9"
    // 所以必须宣告 9。v8/v9 对我们没有新接口，只是版本号。
    wl_global_create(g_srv.dpy, &wl_seat_interface, 9, nullptr, bind_seat);
    wl_global_create(g_srv.dpy, &xdg_wm_base_interface, 6, nullptr, bind_wm_base);

    // 常见客户端/窗管都会 bind 的协议（存在即胜利，具体行为见前面的实现）
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
    wl_global_create(g_srv.dpy, &zwp_primary_selection_device_manager_v1_interface, 1, nullptr, bind_primsel);
    wl_global_create(g_srv.dpy, &xwayland_shell_v1_interface, 1, nullptr, bind_xwlshell);
    wl_global_create(g_srv.dpy, &zwp_text_input_manager_v3_interface, 2, nullptr, bind_ti_mgr);
    // 面板/托盘/启动器（quickshell、waybar、fuzzel、swaybg 的地基）
    wl_global_create(g_srv.dpy, &zwlr_layer_shell_v1_interface, 4, nullptr, bind_lshell);
    wl_global_create(g_srv.dpy, &xdg_activation_v1_interface, 1, nullptr, bind_activation);
    wl_global_create(g_srv.dpy, &wp_presentation_interface, 2, nullptr, bind_presentation);
    // 小型扩展（客户端探测存在性）
    wl_global_create(g_srv.dpy, &wp_content_type_manager_v1_interface, 1, nullptr, bind_content_type);
    wl_global_create(g_srv.dpy, &wp_alpha_modifier_v1_interface, 1, nullptr, bind_alpha_modifier);
    wl_global_create(g_srv.dpy, &wp_fifo_manager_v1_interface, 1, nullptr, bind_fifo);
    wl_global_create(g_srv.dpy, &zwp_pointer_gestures_v1_interface, 3, nullptr, bind_pointer_gestures);
    wl_global_create(g_srv.dpy, &wp_pointer_warp_v1_interface, 1, nullptr, bind_pointer_warp);
    if (syncobj_supported())
        wl_global_create(g_srv.dpy, &wp_linux_drm_syncobj_manager_v1_interface, 1, nullptr, bind_syncobj);
    else
        printf("[syncobj] 驱动不支持 DRM_CAP_SYNCOBJ_TIMELINE，不宣告该协议（客户端自动退化为隐式同步）\n");

    if (wl_display_add_socket(g_srv.dpy, g_socket) != 0) {
        fprintf(stderr, "[sfwm] add_socket 失败: %s\n", strerror(errno));
        return 1;
    }
    const char *rt = getenv("XDG_RUNTIME_DIR");
    // 允许非 root 的 Termux 用户连进来（本进程通常是 root 起的）
    // 注意：目录本身由用户在会话脚本里 mkdir -p 700 建好（属于用户自己），这里不动它的权限，
    // 只把 socket 放开到 0666。
    if (rt) {
        mkdir(rt, 0700);
        std::string sock = std::string(rt) + "/" + g_socket;
        chmod(sock.c_str(), 0666);
    }
    printf("[sfwm] WAYLAND_DISPLAY=%s  (socket 在 %s/%s)\n",
           g_socket, rt ? rt : "?", g_socket);

    int evfd = open_touch_device(g_grab);
    if (evfd >= 0) {
        g_have_input = true;
        wl_event_loop_add_fd(g_srv.loop, evfd, WL_EVENT_READABLE, on_input_fd, nullptr);
    } else {
        printf("[input] 没找到触摸设备（读 /dev/input 需要 root）\n");
    }

    sfwm_keys_init();   // 键盘注入 socket（没有物理键盘也能打字）

    wl_display_run(g_srv.dpy);
    return 0;
}
