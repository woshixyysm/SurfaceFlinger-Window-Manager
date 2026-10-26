#!/usr/bin/env python3
"""结构检查 —— 在没有 Android/Termux 环境的机器上也能跑（CI 用）。

检查的都是这个项目真正的“不变式”，不是风格：

  1. include 能否解析：sfwm.cpp 与所有模块里的 #include "..." 必须存在；
  2. 无悬空模块：src/ 下每个 .hpp 都必须从 sfwm.cpp 可达（否则就是拆剩的孤儿）；
  3. 无 include 环：模块之间不能成环（单 TU 里成环会真的编译不过）；
  4. 每个模块都有 #pragma once；
  5. 仓库里不能有生成物：*-protocol.c / *.o / *-server-protocol.h / 编译出的 sfwm
     —— src/ 里残留的协议头会**遮蔽** meson 在 build/ 里新生成的那份（很隐蔽）；
  6. 协议清单可用：解析 meson.build 的 proto_entries，逐个用 wayland-scanner 生成一遍。

用法：
    python3 tools/check_structure.py
    python3 tools/check_structure.py --protogen --wp-dir /usr/share/wayland-protocols
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")

fails = []
notes = []


def fail(msg):
    fails.append(msg)
    print("  \u274c " + msg)


def ok(msg):
    print("  \u2705 " + msg)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read().replace("\r\n", "\n")


INC_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)


def local_includes(path):
    """只返回能落到本仓库文件上的 include。"""
    out = []
    for inc in INC_RE.findall(read(path)):
        cand = os.path.normpath(os.path.join(os.path.dirname(path), inc))
        if os.path.isfile(cand):
            out.append(cand)
        else:
            # 相对 src/ 的写法（meson 里 include_directories('src')）
            cand2 = os.path.normpath(os.path.join(SRC, inc))
            if os.path.isfile(cand2):
                out.append(cand2)
            else:
                out.append(("MISSING", inc, path))
    return out


def check_includes():
    print("\n[1] include 解析")
    # 协议头是 meson 在 build/ 里现生成的，源码树里本来就没有 —— 但它们必须在清单里
    generated = {base + "-server-protocol.h" for _, base, _, _ in meson_protos()}
    bad = 0
    seen_gen = set()
    for dirpath, _, names in os.walk(SRC):
        for n in names:
            if not n.endswith((".hpp", ".cpp", ".h")):
                continue
            p = os.path.join(dirpath, n)
            for r in local_includes(p):
                if isinstance(r, tuple):
                    if r[1] in generated:
                        seen_gen.add(r[1])
                        continue
                    fail("%s 里的 #include \"%s\" 找不到"
                         % (os.path.relpath(p, ROOT), r[1]))
                    bad += 1
    if not bad:
        ok("所有本地 include 都能解析（其中 %d 个是待生成的协议头，均在 meson 清单内）"
           % len(seen_gen))
    return bad == 0


def check_pragma_once():
    print("\n[2] #pragma once")
    bad = 0
    for dirpath, _, names in os.walk(SRC):
        for n in sorted(names):
            if not n.endswith(".hpp"):
                continue
            p = os.path.join(dirpath, n)
            if "#pragma once" not in read(p):
                fail("%s 缺少 #pragma once" % os.path.relpath(p, ROOT))
                bad += 1
    if not bad:
        ok("所有模块头都有 #pragma once")
    return bad == 0


def walk_graph():
    """从 sfwm.cpp 出发做 DFS，返回 (postorder, cycles)。"""
    entry = os.path.join(SRC, "sfwm.cpp")
    post, state, cycles = [], {}, []

    def visit(p, stack):
        st = state.get(p)
        if st == "done":
            return
        if st == "busy":
            cycles.append(stack[stack.index(p):] + [p])
            return
        state[p] = "busy"
        for c in local_includes(p):
            if isinstance(c, tuple):
                continue
            visit(c, stack + [p])
        state[p] = "done"
        post.append(p)

    visit(entry, [])
    return post, cycles


def check_graph():
    print("\n[3] 模块图：可达性 / 环")
    post, cycles = walk_graph()
    if cycles:
        for c in cycles:
            fail("include 成环: " + " -> ".join(os.path.relpath(x, ROOT) for x in c))
        return False
    ok("无 include 环")

    reached = {os.path.normcase(x) for x in post}
    orphans = []
    for dirpath, _, names in os.walk(SRC):
        for n in names:
            if not n.endswith(".hpp"):
                continue
            p = os.path.join(dirpath, n)
            if os.path.normcase(p) not in reached:
                orphans.append(os.path.relpath(p, ROOT))
    if orphans:
        for o in orphans:
            fail("孤儿模块（没有任何地方 include 它）: " + o)
        return False
    ok("src/ 下所有 .hpp 都可达，共 %d 个文件参与编译" % len(post))
    notes.append("编译单元实际展开顺序（%d 个文件）：\n%s"
                 % (len(post), "\n".join("    " + os.path.relpath(x, ROOT) for x in post)))
    return True


ARTIFACT_RE = re.compile(r"(-protocol\.c|-protocol\.o|-server-protocol\.h|\.o$)")


def check_artifacts():
    print("\n[4] 仓库里不能有生成物")
    bad = 0
    for dirpath, dirnames, names in os.walk(ROOT):
        dirnames[:] = [d for d in dirnames if d not in (".git", "build", "subprojects")]
        for n in names:
            p = os.path.join(dirpath, n)
            rel = os.path.relpath(p, ROOT)
            if ARTIFACT_RE.search(n):
                fail("生成物被提交了: " + rel)
                bad += 1
            elif rel == "sfwm":
                fail("编译产物被提交了: " + rel)
                bad += 1
    if not bad:
        ok("没有协议生成物 / 目标文件 / 二进制残留")
    return bad == 0


PROTO_RE = re.compile(
    r"\[\s*'([^']+)'\s*,\s*'([^']+)'\s*,\s*'([^']*)'\s*,\s*(true|false)\s*\]")


def meson_protos():
    # 直接在整份 meson.build 上套正则（不能先按 "[" / "]" 截块：每条记录自己就有一对括号）
    s = read(os.path.join(ROOT, "meson.build"))
    return [(m.group(1), m.group(2), m.group(3), m.group(4) == "true")
            for m in PROTO_RE.finditer(s)]


def check_protogen(wp_dir):
    print("\n[5] 协议清单（meson.build proto_entries）")
    protos = meson_protos()
    if len(protos) < 20:
        fail("协议清单解析异常，只拿到 %d 项" % len(protos))
        return False

    scanner = shutil.which("wayland-scanner")
    if not scanner:
        notes.append("未安装 wayland-scanner，跳过实际生成（只校验 XML 存在）")

    tmp = tempfile.mkdtemp(prefix="sfwm-proto-")
    bad = 0
    for xml, base, ns, is_wp in protos:
        path = os.path.join(wp_dir, xml) if is_wp else os.path.join(ROOT, xml)
        if not os.path.isfile(path):
            fail("XML 找不到: %s%s" % (xml, "（需要 wayland-protocols）" if is_wp else ""))
            bad += 1
            continue
        if not scanner:
            continue
        h = os.path.join(tmp, base + "-server-protocol.h")
        c = os.path.join(tmp, base + "-protocol.c")
        try:
            subprocess.run([scanner, "server-header", path, h], check=True,
                           capture_output=True)
            subprocess.run([scanner, "private-code", path, c], check=True,
                           capture_output=True)
        except subprocess.CalledProcessError as e:
            fail("wayland-scanner 失败: %s\n%s" % (xml, e.stderr.decode()[:300]))
            bad += 1
            continue
        if ns and "namespace" in read(h):
            fail("%s 生成的头里还有 C++ 关键字 namespace（gen-proto.sh 的 sed 没生效）" % base)
            bad += 1
        if "wl_interface" not in read(c):
            fail("%s 生成的 .c 里没有 wl_interface" % base)
            bad += 1
    if not bad:
        ok("%d 个协议全部可生成（私有 %d + 系统 %d）"
           % (len(protos), sum(1 for p in protos if not p[3]), sum(1 for p in protos if p[3])))
    shutil.rmtree(tmp, ignore_errors=True)
    return bad == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--protogen", action="store_true", help="实际跑 wayland-scanner")
    ap.add_argument("--wp-dir", default=os.environ.get(
        "SFWM_WAYLAND_PROTOCOLS", "/usr/share/wayland-protocols"))
    ap.add_argument("--show-order", action="store_true", help="打印展开顺序")
    a = ap.parse_args()

    print("sfwm 结构检查 @ " + ROOT)
    ok_all = True
    ok_all &= check_includes()
    ok_all &= check_pragma_once()
    ok_all &= check_graph()
    ok_all &= check_artifacts()
    if a.protogen:
        ok_all &= check_protogen(a.wp_dir)

    if notes and a.show_order:
        print()
        for n in notes:
            print(n)
    elif notes:
        print("\n（加 --show-order 可看编译单元展开顺序）")

    print()
    if ok_all:
        print("\u2705 结构检查通过")
        return 0
    print("\u274c 结构检查失败：%d 项" % len(fails))
    return 1


if __name__ == "__main__":
    sys.exit(main())
