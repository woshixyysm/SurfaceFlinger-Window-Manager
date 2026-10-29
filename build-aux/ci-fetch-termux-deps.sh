#!/bin/sh
# ci-fetch-termux-deps.sh - fetch aarch64 bionic wayland, libxkbcommon and libdrm
# wayland, libxkbcommon and libdrm and unpack them into a usable prefix.
#
# The libraries must match the runtime on the device, which is the Termux build, so
# ABI than the device, which runs the Termux packages. Cross compiling them is a
# separate project; CI only has to compile, so the device headers are the closest match.
#
# Usage: sh build-aux/ci-fetch-termux-deps.sh <output prefix>
set -eu

PREFIX=${1:?usage: ci-fetch-termux-deps.sh <output prefix>}
REPO=${TERMUX_REPO:-https://packages.termux.dev/apt/termux-main}
SUITE=${TERMUX_SUITE:-stable}
ARCH=${TERMUX_ARCH:-aarch64}
WANT=${TERMUX_PKGS:-"wayland libxkbcommon libdrm"}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$PREFIX/include" "$PREFIX/lib"

echo "---- fetching the package index $REPO/dists/$SUITE/main/binary-$ARCH/Packages.xz ----"
curl -fsSL "$REPO/dists/$SUITE/main/binary-$ARCH/Packages.xz" -o "$work/Packages.xz"
xz -dc "$work/Packages.xz" > "$work/Packages"
echo "index lists $(grep -c '^Package: ' "$work/Packages") packages"

# Package names are matched exactly, since grep would also hit related packages.
for pkg in $WANT; do
    fn=$(python3 - "$work/Packages" "$pkg" <<'PY'
import sys
pkg = sys.argv[2]
best = None
for stanza in open(sys.argv[1], encoding="utf-8", errors="replace").read().split("\n\n"):
    d = {}
    for line in stanza.splitlines():
        if ": " in line:
            k, v = line.split(": ", 1)
            d[k] = v
    if d.get("Package") == pkg:
        best = d.get("Filename")           # last entry wins when a package repeats
print(best or "")
PY
)
    if [ -z "$fn" ]; then
        echo "no package named '$pkg' in the index; similar names:" >&2
        grep '^Package: ' "$work/Packages" | grep -i "$pkg" | sed 's/^/    /' >&2 || true
        exit 1
    fi
    echo "---- downloading $pkg -> $fn ----"
    curl -fsSL "$REPO/$fn" -o "$work/$pkg.deb"
    dpkg-deb -x "$work/$pkg.deb" "$work/x-$pkg"
    # A Termux deb unpacks to data/data/com.termux/files/usr/...
    src=$(find "$work/x-$pkg" -type d -name usr | head -1)
    [ -n "$src" ] || { echo "cannot find the directory layout of $pkg" >&2; exit 1; }
    [ -d "$src/include" ] && cp -a "$src/include/." "$PREFIX/include/" 2>/dev/null || true
    [ -d "$src/lib" ] && cp -a "$src/lib/." "$PREFIX/lib/" 2>/dev/null || true
done

mkdir -p "$PREFIX/include" "$PREFIX/lib"
mkdir -p "$PREFIX/include" "$PREFIX/lib"
echo "---- prefix contents ----"
echo "include: $(ls "$PREFIX/include" 2>/dev/null | tr '\n' ' ' | cut -c1-200)"
echo "lib    : $(ls "$PREFIX/lib" 2>/dev/null | tr '\n' ' ' | cut -c1-200)"

for must in "$PREFIX/include/wayland-server-core.h" \
            "$PREFIX/include/xkbcommon/xkbcommon.h" \
            "$PREFIX/include/xf86drm.h"; do
    [ -f "$must" ] || { echo "missing header: $must" >&2; exit 1; }
done
echo "Termux dependencies ready"
