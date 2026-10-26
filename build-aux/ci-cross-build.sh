#!/bin/sh
# ci-cross-build.sh —— 在非 Android 机器上把 sfwm 交叉编译成 aarch64-android 二进制。
#
# 这不是本机构建脚本（Termux 上直接用 meson，见 README）。它的存在是为了让 CI
# 能真正编译一遍代码，而不是只做静态检查。
#
# 需要的三样东西：
#   NDK   Android NDK 根目录（官方 r27+ 或 android-ndk-custom 都可以，只要
#         toolchains/llvm/prebuilt/<host>/ 这套布局在）
#   DEPS  一个前缀目录，里面要有 wayland-server / xkbcommon / libdrm 的
#         include/ 和 lib/（aarch64 bionic 版本；CI 里从 Termux 包解出来）
#   WPDIR wayland-protocols 的 XML 根（默认 /usr/share/wayland-protocols）
#
# 为什么不用 meson：meson.build 里的 -L.../opt/gcc-17/... 和 --prefix 都是
# Termux 的路径，交叉编译时那些全都不成立。这里直接调编译器，等价参数一一对应。
#
# 用法：
#   NDK=/path/to/ndk DEPS=/path/to/prefix sh build-aux/ci-cross-build.sh
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
: "${NDK:?需要 NDK=<NDK 根目录>}"
: "${DEPS:?需要 DEPS=<wayland/xkbcommon/libdrm 的安装前缀>}"
WPDIR=${WPDIR:-/usr/share/wayland-protocols}
API=${API:-24}
OUT=${OUT:-$ROOT/ci-build}

# ---- 定位工具链 ---------------------------------------------------------
first_glob() { for f in $1; do [ -e "$f" ] && { echo "$f"; return 0; }; done; return 1; }

# 兼容「解压出来外面还套了一层目录」的情况
if ! first_glob "$NDK/toolchains/llvm/prebuilt/*/bin/clang" >/dev/null 2>&1; then
    inner=$(first_glob "$NDK"/android-ndk-*/toolchains || true)
    if [ -n "${inner:-}" ]; then NDK=$(dirname "$inner"); fi
fi

CLANG=$(first_glob "$NDK/toolchains/llvm/prebuilt/*/bin/clang") \
    || { echo "在 $NDK 里找不到 toolchains/llvm/prebuilt/*/bin/clang" >&2; exit 1; }
CLANGXX=$(first_glob "$NDK/toolchains/llvm/prebuilt/*/bin/clang++") \
    || { echo "找不到 clang++" >&2; exit 1; }
SYSROOT=$(first_glob "$NDK/toolchains/llvm/prebuilt/*/sysroot") \
    || { echo "找不到 sysroot" >&2; exit 1; }

TARGET="--target=aarch64-linux-android$API --sysroot=$SYSROOT"
echo "clang   : $CLANG"
echo "sysroot : $SYSROOT"
echo "target  : aarch64-linux-android$API"
echo "deps    : $DEPS"

# ---- 生成协议 -----------------------------------------------------------
# 和 meson 用同一个脚本，保证 namespace 改名那条 sed 也一起验证到
mkdir -p "$OUT/inc" "$OUT/obj"
echo "---- 生成协议 ----"
python3 - "$ROOT/meson.build" > "$OUT/protos.txt" <<'PY'
import re, sys
s = open(sys.argv[1], encoding="utf-8").read()
pat = re.compile(r"\[\s*'([^']+)'\s*,\s*'([^']+)'\s*,\s*'([^']*)'\s*,\s*(true|false)\s*\]")
for xml, base, ns, is_wp in pat.findall(s):
    print(xml, base, ns, is_wp)
PY
[ "$(wc -l < "$OUT/protos.txt")" -ge 20 ] || {
    echo "协议清单解析异常（只有 $(wc -l < "$OUT/protos.txt") 条）" >&2; exit 1; }

while read -r xml base ns is_wp; do
    # true = 系统 wayland-protocols 里的相对路径，false = 本仓库 protocols/ 下
    if [ "$is_wp" = "true" ]; then src="$WPDIR/$xml"; else src="$ROOT/$xml"; fi
    [ -f "$src" ] || { echo "缺 XML: $src" >&2; exit 1; }
    sh "$ROOT/build-aux/gen-proto.sh" "$src" "$OUT/inc" "$base" "$ns"
done < "$OUT/protos.txt"
echo "生成 $(ls "$OUT/inc"/*-protocol.c | wc -l) 个协议"

# ---- 编译 ---------------------------------------------------------------
# ⚠️ 协议 .c 必须用 C 编译：用 C++ 编会让 wl_interface 变成内部链接，
#    最后一定 undefined symbol（README 里记过这条）。
CFLAGS="$TARGET -std=gnu11 -O2 -w -I$OUT/inc -isystem $DEPS/include"
CXXFLAGS="$TARGET -std=c++17 -O2 -w -g -I$OUT/inc -isystem $DEPS/include"

echo "---- 编译协议 .c（C） ----"
for c in "$OUT/inc"/*-protocol.c; do
    b=$(basename "$c" .c)
    $CLANG $CFLAGS -c "$c" -o "$OUT/obj/$b.o"
done

echo "---- 编译链接 sfwm（C++） ----"
$CLANGXX $CXXFLAGS -o "$OUT/sfwm" \
    "$ROOT/src/sfwm.cpp" "$OUT"/obj/*.o \
    -L"$DEPS/lib" \
    -lwayland-server -lxkbcommon -ldrm -landroid -llog

echo "---- 结果 ----"
ls -l "$OUT/sfwm"
# file(1) 可能没装；有就打个架构，方便人眼确认
command -v file >/dev/null 2>&1 && file "$OUT/sfwm" || true
echo "✅ 交叉编译通过"
