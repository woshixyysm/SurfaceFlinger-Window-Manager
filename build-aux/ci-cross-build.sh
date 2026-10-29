#!/bin/sh
# ci-cross-build.sh - cross build sfwm for aarch64-android on a non-Android machine.
#
# This is not the local build script; on Termux meson is used directly. It exists so
# that CI compiles the code instead of only running static checks.
#
# Three inputs are required:
#   NDK   the Android NDK root, official r27 or android-ndk-custom, as long as the
#         toolchains/llvm/prebuilt/<host> layout is present
#   DEPS  a prefix holding include/ and lib/ for wayland-server, xkbcommon and
#         libdrm, built for aarch64 bionic
#   WPDIR the wayland-protocols XML root, /usr/share/wayland-protocols by default
#
# meson is not used here: meson.build carries Termux specific -L paths and a
# prefix. The compiler is called directly with the equivalent arguments.
#
# Usage:
#   NDK=/path/to/ndk DEPS=/path/to/prefix sh build-aux/ci-cross-build.sh
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
NDK=${NDK:-}
DEPS=${DEPS:-}
WPDIR=${WPDIR:-/usr/share/wayland-protocols}
API=${API:-24}
OUT=${OUT:-$ROOT/ci-build}

# ---- toolchain discovery ----
first_glob() { for f in $1; do [ -e "$f" ] && { echo "$f"; return 0; }; done; return 1; }

# Discovery and checks live in functions, so PROTO_ONLY works without an NDK.
setup_toolchain() {
[ -n "$NDK" ] || { echo "NDK=<NDK root> is required" >&2; exit 1; }
[ -n "$DEPS" ] || { echo "DEPS=<prefix with wayland, xkbcommon and libdrm> is required" >&2; exit 1; }

# Tolerate an extra directory level after extraction.
if ! first_glob "$NDK/toolchains/llvm/prebuilt/*/bin/clang" >/dev/null 2>&1; then
    inner=$(first_glob "$NDK"/android-ndk-*/toolchains || true)
    if [ -n "${inner:-}" ]; then NDK=$(dirname "$inner"); fi
fi

CLANG=$(first_glob "$NDK/toolchains/llvm/prebuilt/*/bin/clang") \
    || { echo "no toolchains/llvm/prebuilt/*/bin/clang under $NDK" >&2; exit 1; }
CLANGXX=$(first_glob "$NDK/toolchains/llvm/prebuilt/*/bin/clang++") \
    || { echo "clang++ not found" >&2; exit 1; }
SYSROOT=$(first_glob "$NDK/toolchains/llvm/prebuilt/*/sysroot") \
    || { echo "sysroot not found" >&2; exit 1; }

TARGET="--target=aarch64-linux-android$API --sysroot=$SYSROOT"
echo "clang   : $CLANG"
echo "sysroot : $SYSROOT"
echo "target  : aarch64-linux-android$API"
echo "deps    : $DEPS"

# ---- preflight, to separate a broken environment from broken code ----
$CLANG --version | head -1
for f in "$SYSROOT/usr/include/android/native_window.h" \
         "$DEPS/include/wayland-server-core.h" \
         "$DEPS/include/xkbcommon/xkbcommon.h" \
         "$DEPS/include/xf86drm.h" \
         "$DEPS/lib/libwayland-server.so"; do
    [ -e "$f" ] || { echo "preflight failed, missing: $f" >&2; exit 1; }
done

# libandroid.so and liblog.so are not necessarily at the root of
# sysroot/usr/lib/<triple>; the official NDK puts them in an API level directory.
# Only aarch64 may be used: this NDK ships x86_64 stubs as well, and taking the
# first find hit would link x86_64 libraries into an aarch64 binary.
find_stub() {
    _lib=$1
    for _p in "$SYSROOT/usr/lib/aarch64-linux-android/$API/$_lib" \
              "$SYSROOT/usr/lib/aarch64-linux-android/$_lib" \
              "$SYSROOT/usr/lib/aarch64-linux-android"/*/"$_lib"; do
        [ -e "$_p" ] && { echo "$_p"; return 0; }
    done
    return 1
}

ANDROID_SO=$(find_stub libandroid.so || true)
LOG_SO=$(find_stub liblog.so || true)
LENIENT=""
ANDROID_LIBS=""
if [ -n "$ANDROID_SO" ]; then
    echo "libandroid.so : $ANDROID_SO"
    SYSLIB=$(dirname "$ANDROID_SO")
    ANDROID_LIBS="-landroid"
else
    echo "libandroid.so : no aarch64 build found"
    other=$(find "$SYSROOT/usr/lib" -name 'libandroid.so' 2>/dev/null | head -1 || true)
    [ -n "$other" ] && echo "                (only $other, which cannot be used for aarch64)"
fi
if [ -n "$LOG_SO" ]; then
    echo "liblog.so     : $LOG_SO"
    [ -n "${SYSLIB:-}" ] || SYSLIB=$(dirname "$LOG_SO")
    ANDROID_LIBS="$ANDROID_LIBS -llog"
else
    echo "liblog.so     : no aarch64 build found"
fi
if [ -z "$ANDROID_LIBS" ]; then
    echo "                this NDK has no aarch64 bionic stub, so the link allows"
    echo "                unresolved symbols; it proves compilation, not a runnable binary"
    LENIENT="-Wl,--unresolved-symbols=ignore-all"
fi
if [ -n "${SYSLIB:-}" ] && [ -d "$SYSLIB" ]; then
    echo "syslib dir    : $SYSLIB"
fi
echo "preflight passed"
}
# ---- protocol generation ----
# The same script as meson, so the namespace rename is exercised as well.
mkdir -p "$OUT/inc" "$OUT/obj"
echo "---- generating protocols ----"
python3 - "$ROOT/meson.build" > "$OUT/protos.txt" <<'PY'
import re, sys
s = open(sys.argv[1], encoding="utf-8").read()
pat = re.compile(r"\[\s*'([^']+)'\s*,\s*'([^']+)'\s*,\s*'([^']*)'\s*,\s*(true|false)\s*\]")
for xml, base, ns, is_wp in pat.findall(s):
    # The separator is a pipe: ns is often empty, and spaces would be folded by
    # IFS, shifting every field.
    print("|".join([xml, base, ns, is_wp]))
PY
[ "$(wc -l < "$OUT/protos.txt")" -ge 20 ] || {
    echo "cannot parse the protocol list, $(wc -l < "$OUT/protos.txt") entries" >&2; exit 1; }

while IFS='|' read -r xml base ns is_wp; do
    # true means a path inside wayland-protocols, false means this repository.
    if [ "$is_wp" = "true" ]; then src="$WPDIR/$xml"; else src="$ROOT/$xml"; fi
    [ -f "$src" ] || { echo "missing XML $src (WPDIR=$WPDIR)" >&2; exit 1; }
    sh "$ROOT/build-aux/gen-proto.sh" "$src" "$OUT/inc" "$base" "$ns"
done < "$OUT/protos.txt"
echo "generated $(ls "$OUT/inc"/*-protocol.c | wc -l) protocols"

# Stop after parsing and generating the protocols, which needs no NDK.
if [ -n "${PROTO_ONLY:-}" ]; then
    echo "PROTO_ONLY: protocols done, skipping compilation"
    exit 0
fi

setup_toolchain

# ---- compilation ----
# The protocol .c files must be compiled as C; in C++ wl_interface becomes internal
# linkage and the link fails.
CFLAGS="$TARGET -std=gnu11 -O2 -w -I$OUT/inc -isystem $DEPS/include"
# Modules include each other relative to src/, the same way meson is configured with
# include_directories('src'), so that directory has to be on the include path here too.
CXXFLAGS="$TARGET -std=c++17 -O2 -w -g -I$ROOT/src -I$OUT/inc -isystem $DEPS/include"

echo "---- compiling protocol .c files as C ----"
for c in "$OUT/inc"/*-protocol.c; do
    b=$(basename "$c" .c)
    $CLANG $CFLAGS -c "$c" -o "$OUT/obj/$b.o"
done

echo "---- compiling and linking sfwm as C++ ----"
LFLAGS="-L$DEPS/lib"
if [ -n "${SYSLIB:-}" ] && [ -d "${SYSLIB:-}" ]; then
    LFLAGS="-L$SYSLIB $LFLAGS"
fi
# shellcheck disable=SC2086  # word splitting is intended here
$CLANGXX $CXXFLAGS -o "$OUT/sfwm" \
    "$ROOT/src/main.cpp" "$OUT"/obj/*.o \
    $LFLAGS \
    -lwayland-server -lxkbcommon -ldrm $ANDROID_LIBS \
    $LENIENT

echo "---- result ----"
ls -l "$OUT/sfwm"
# file(1) may be absent; when present it prints the architecture.
command -v file >/dev/null 2>&1 && file "$OUT/sfwm" || true
echo "cross build succeeded"
