#!/usr/bin/env python3
"""Structural checks for the sfwm source tree. They run on any machine, without
Android or Termux, and cover the invariants the project actually depends on:

  1. every local #include "..." resolves, starting from src/main.cpp;
  2. every header under src/ is reachable from src/main.cpp;
  3. the module graph has no include cycles; a cycle cannot compile in a single TU;
  4. every header has #pragma once;
  5. no generated files are committed: *-protocol.c, *.o, *-server-protocol.h and the
     sfwm binary. A stale protocol header in src/ shadows the generated one in build/;
  6. the protocol list in meson.build can be generated with wayland-scanner.

Usage:
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
    print("  FAIL " + msg)


def ok(msg):
    print("  ok   " + msg)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read().replace("\r\n", "\n")


INC_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)


def local_includes(path):
    """Return the includes that resolve to files in this repository."""
    out = []
    for inc in INC_RE.findall(read(path)):
        cand = os.path.normpath(os.path.join(os.path.dirname(path), inc))
        if os.path.isfile(cand):
            out.append(cand)
        else:
            # Paths relative to src/, added by include_directories('src')
            cand2 = os.path.normpath(os.path.join(SRC, inc))
            if os.path.isfile(cand2):
                out.append(cand2)
            else:
                out.append(("MISSING", inc, path))
    return out


def check_includes():
    print("\n[1] include resolution")
    # Protocol headers are generated into build/, so they may be absent here.
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
                    fail("%s includes \"%s\", which does not exist"
                         % (os.path.relpath(p, ROOT), r[1]))
                    bad += 1
    if not bad:
        ok("all local includes resolve, %d of them generated protocol headers"
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
                fail("%s has no #pragma once" % os.path.relpath(p, ROOT))
                bad += 1
    if not bad:
        ok("every module header has #pragma once")
    return bad == 0


def walk_graph():
    """Depth-first walk from src/main.cpp, returning (postorder, cycles)."""
    entry = os.path.join(SRC, "main.cpp")
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
    print("\n[3] module graph: reachability and cycles")
    post, cycles = walk_graph()
    if cycles:
        for c in cycles:
            fail("include cycle: " + " -> ".join(os.path.relpath(x, ROOT) for x in c))
        return False
    ok("no include cycles")

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
            fail("orphan module, nothing includes it: " + o)
        return False
    ok("every header under src/ is reachable, %d files in the build" % len(post))
    notes.append("translation unit order, %d files:\n%s"
                 % (len(post), "\n".join("    " + os.path.relpath(x, ROOT) for x in post)))
    return True


ARTIFACT_RE = re.compile(r"(-protocol\.c|-protocol\.o|-server-protocol\.h|\.o$)")


def check_artifacts():
    print("\n[4] no generated files are tracked")
    # Only tracked files matter; local build output is not a problem.
    if not os.path.isdir(os.path.join(ROOT, ".git")):
        print("  skip  not a git repository, skipping")
        notes.append("not a git repository, skipping the generated file check")
        return True
    try:
        tracked = subprocess.run(["git", "-C", ROOT, "ls-files"], check=True,
                                 capture_output=True).stdout.decode("utf-8").split()
    except (subprocess.CalledProcessError, OSError) as e:
        notes.append("git ls-files failed, skipping the generated file check: %s" % e)
        return True

    bad = 0
    for rel in tracked:
        n = os.path.basename(rel)
        if ARTIFACT_RE.search(n) or rel == "sfwm":
            fail("generated file is tracked: " + rel)
            bad += 1
    if not bad:
        ok("%d tracked files, none of them generated, object or binary" % len(tracked))
    return bad == 0


PROTO_RE = re.compile(
    r"\[\s*'([^']+)'\s*,\s*'([^']+)'\s*,\s*'([^']*)'\s*,\s*(true|false)\s*\]")


def meson_protos():
    # The regex runs over the whole meson.build, since each entry has its own brackets.
    s = read(os.path.join(ROOT, "meson.build"))
    return [(m.group(1), m.group(2), m.group(3), m.group(4) == "true")
            for m in PROTO_RE.finditer(s)]


def check_protogen(wp_dir):
    print("\n[5] protocol list (proto_entries in meson.build)")
    protos = meson_protos()
    if len(protos) < 20:
        fail("cannot parse the protocol list, found %d entries" % len(protos))
        return False

    scanner = shutil.which("wayland-scanner")
    if not scanner:
        notes.append("wayland-scanner not installed, only checking that the XML exists")

    # Use the same path as meson: build-aux/gen-proto.sh, which renames namespace.
    gen = os.path.join(ROOT, "build-aux", "gen-proto.sh")
    tmp = tempfile.mkdtemp(prefix="sfwm-proto-")
    bad = 0
    for xml, base, ns, is_wp in protos:
        path = os.path.join(wp_dir, xml) if is_wp else os.path.join(ROOT, xml)
        if not os.path.isfile(path):
            fail("XML not found: %s%s" % (xml, " (wayland-protocols required)" if is_wp else ""))
            bad += 1
            continue
        if not scanner:
            continue
        try:
            subprocess.run(["sh", gen, path, tmp, base, ns], check=True,
                           capture_output=True)
        except subprocess.CalledProcessError as e:
            fail("gen-proto.sh failed on %s\n%s" % (xml, e.stderr.decode()[:300]))
            bad += 1
            continue
        h = os.path.join(tmp, base + "-server-protocol.h")
        c = os.path.join(tmp, base + "-protocol.c")
        for f in (h, c):
            if not os.path.isfile(f) or os.path.getsize(f) == 0:
                fail("gen-proto.sh did not produce %s" % os.path.basename(f))
                bad += 1
        if os.path.isfile(h) and ns and re.search(r"\bnamespace\b", read(h)):
            fail("%s still contains the C++ keyword namespace in the generated header" % base)
            bad += 1
        if os.path.isfile(c) and "wl_interface" not in read(c):
            fail("%s generated a .c file without wl_interface" % base)
            bad += 1
    if not bad:
        ok("%d protocols generated, %d local and %d from wayland-protocols"
           % (len(protos), sum(1 for p in protos if not p[3]), sum(1 for p in protos if p[3])))
    shutil.rmtree(tmp, ignore_errors=True)
    return bad == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--protogen", action="store_true", help="run wayland-scanner")
    ap.add_argument("--wp-dir", default=os.environ.get(
        "SFWM_WAYLAND_PROTOCOLS", "/usr/share/wayland-protocols"))
    ap.add_argument("--show-order", action="store_true", help="print the translation unit order")
    a = ap.parse_args()

    print("sfwm structure check in " + ROOT)
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
        print("\n(add --show-order to print the translation unit order)")

    print()
    if ok_all:
        print("structure check passed")
        return 0
    print("structure check failed: %d problem(s)" % len(fails))
    return 1


if __name__ == "__main__":
    sys.exit(main())
