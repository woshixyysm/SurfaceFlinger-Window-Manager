#!/usr/bin/env python3
# 把 src/sfwm.cpp 按顺序切成多个模块头文件。
#
# 只做两件事：整块切出 + 在原位置留一行 #include。
# 因为切点全部是「顺序边界」（前一块不用后一块的东西，C++ 本来就保证），
# 所以作用域、名字查找、初始化顺序都和原来逐字等价 —— 行为零差异。
#
# 切分依据：文件自身的 `// ---- 章节` 分隔线 + 顶层定义起始行。
import os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC  = os.path.join(ROOT, "src", "sfwm.cpp")

# (起始行 1-based, 相对 src/ 的模块路径, 一句话说明)
CHUNKS = [
    (   1, "core/prelude.hpp",            "标准头 + 协议头 + wl_shm 的 fd 帮手"),
    ( 113, "core/config.hpp",             "配置全局 + 缓冲/连接/Server 结构体"),
    ( 215, "core/trace.hpp",              "结构化跟踪（SFWM_TRACE）"),
    ( 306, "render/cursor.hpp",           "光标几何绘制 + Xcursor 主题解析 + 上屏"),
    ( 797, "render/testpat.hpp",          "自检图案（SFWM_TESTPAT，逐项验证不闪）"),
    ( 842, "compositor/surface.hpp",      "struct View + wl_region/wl_surface/wl_shm"),
    (1022, "protocol/linux_dmabuf.hpp",   "zwp_linux_dmabuf_v1（+ feedback）"),
    (1217, "protocol/wl_drm.hpp",         "wl_drm（老客户端/nvidia 兼容路径）"),
    (1271, "protocol/compositor.hpp",     "wl_compositor（建 surface/region）"),
    (1332, "protocol/subcompositor.hpp",  "wl_subcompositor（Qt/OBS 的内容面在这里）"),
    (1417, "protocol/output.hpp",         "wl_output + xdg_output"),
    (1459, "input/seat.hpp",              "wl_seat / wl_pointer / wl_touch 绑定"),
    (1594, "input/keyboard.hpp",          "wl_keyboard + text_input_v3 + IME + 控制命令"),
    (2295, "input/inject.hpp",            "按键注入 socket（sfwmc 走这里）"),
    (2471, "input/pointer.hpp",           "指针/鼠标按钮/命中测试/抬升"),
    (2771, "input/touch.hpp",             "触摸状态机 + 手势 + handle_frame + /dev/input"),
    (3384, "protocol/misc_ext.hpp",       "content_type / alpha_modifier / fifo / 手势 / warp"),
    (3621, "protocol/syncobj.hpp",        "wp_linux_drm_syncobj_v1（显式同步）"),
    (3793, "protocol/presentation.hpp",   "wp_presentation_feedback"),
    (3872, "protocol/activation.hpp",     "xdg_activation_v1"),
    (3934, "shell/layer_shell.hpp",       "zwlr_layer_shell_v1（面板/托盘）"),
    (4121, "shell/xdg_shell.hpp",         "xdg_wm_base / toplevel / popup / configure"),
    (4428, "protocol/misc_ext2.hpp",      "viewporter / cursor-shape / tearing / 剪贴板 等"),
    (4814, "protocol/xwayland.hpp",       "xwayland-shell-v1"),
]

run = "--run" in sys.argv

data = open(SRC, "rb").read()
lines = data.split(b"\n")
eol = b"\r\n" if b"\r" in lines[0] else b"\n"
lines = [l.rstrip(b"\r") for l in lines]
print("当前 sfwm.cpp: %d 行, eol=%s" % (len(lines), "CRLF" if eol == b"\r\n" else "LF"))

# main 之前必须切完；main 的行号从切分表后自动探测
main_i = None
for i, l in enumerate(lines):
    if l.startswith(b"int main("):
        main_i = i
        break
if main_i is None:
    sys.exit("找不到 int main(")
print("int main 在第 %d 行" % (main_i + 1))

bounds = [c[0] - 1 for c in CHUNKS] + [main_i]
for i in range(len(bounds) - 1):
    if bounds[i] >= bounds[i + 1]:
        sys.exit("切点顺序错误: %r" % (CHUNKS[i],))

if not run:
    # 只报告，不动文件
    for k, (start, path, desc) in enumerate(CHUNKS):
        s = start - 1
        e = bounds[k + 1] - 1        # 含
        print("  %-34s %5d..%-5d (%4d 行)  %s" % (path, s + 1, e + 1, e - s + 1, desc))
    n = main_i + 1
    print("  %-34s %5d..%-5d (%4d 行)  %s" % ("src/sfwm.cpp (main)",
          n, len(lines), len(lines) - n + 1, "参数解析 + 全局注册 + 事件循环"))
    print("\n加 --run 才真正落盘")
    sys.exit(0)

newmain = []
for k, (start, path, desc) in enumerate(CHUNKS):
    s = start - 1
    e = bounds[k + 1] - 1
    # ⚠️ 不 rstrip：模块尾部的空行也是原文件的一部分，去掉会让拼回来时行号漂移
    body = b"\n".join(lines[s:e + 1])

    preamble = (
        "// %s\n"
        "//\n"
        "// %s\n"
        "//\n"
        "// 从 sfwm.cpp 原样搬出（只搬不改，逻辑零差异）。\n"
        "// 依赖包含顺序：本文件用到的东西必须由**前面的**模块提供，\n"
        "// 因此它在 sfwm.cpp 里的 include 位置就是它原来在文件里的位置。\n"
        "#pragma once\n\n" % (path.split("/")[-1], desc)
    ).encode("utf-8")

    dst = os.path.join(ROOT, "src", path)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, "wb").write(preamble + body + b"\n")
    newmain.append('#include "%s"' % path)
    print("写出 %-34s %4d 行" % (path, body.count(b"\n") + 1))

head = (
    "// sfwm.cpp —— 入口与装配\n"
    "//\n"
    "// 单 TU 的组合方式：下面这一串 include 的顺序 = 代码在原始单文件里的顺序。\n"
    "// 所以它等价于原来那个 5631 行的大文件，只是被切成了可读的模块。\n"
    "// 每个模块的文件头都写了它依赖什么；挪动 include 顺序会改变语义，别乱动。\n"
    "\n"
    + "\n".join(newmain)
    + "\n\n"
    + "// ---------------------------------------------------------------- main\n"
).encode("utf-8")

tail = b"\n".join(lines[main_i:])
open(SRC, "wb").write(head + tail)
print("\n新 sfwm.cpp: %d 行（%d 个 include + main）"
      % (head.count(b"\n") + tail.count(b"\n") + 1, len(newmain)))
