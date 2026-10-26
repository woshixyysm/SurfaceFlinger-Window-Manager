#!/bin/sh
# ci-fetch-termux-deps.sh —— 从 Termux 仓库取 aarch64(bionic) 的
# wayland / libxkbcommon / libdrm，解出一个可用的前缀（include/ + lib/）。
#
# 为什么从 Termux 取而不是自己交叉编译：这三个库跟设备上的运行时必须是同一份
# ABI（设备跑的就是 Termux 的包）。自己编可以，但那是另一个工程；CI 只需要
# 「能编过」，所以直接拿设备上那份一模一样的头/库最贴近真实。
#
# 用法：sh build-aux/ci-fetch-termux-deps.sh <输出前缀目录>
set -eu

PREFIX=${1:?用法: ci-fetch-termux-deps.sh <输出前缀>}
REPO=${TERMUX_REPO:-https://packages.termux.dev/apt/termux-main}
SUITE=${TERMUX_SUITE:-stable}
ARCH=${TERMUX_ARCH:-aarch64}
WANT=${TERMUX_PKGS:-"wayland libxkbcommon libdrm"}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$PREFIX/include" "$PREFIX/lib"

echo "---- 取包索引 $REPO/dists/$SUITE/main/binary-$ARCH/Packages.xz ----"
curl -fsSL "$REPO/dists/$SUITE/main/binary-$ARCH/Packages.xz" -o "$work/Packages.xz"
xz -dc "$work/Packages.xz" > "$work/Packages"
echo "索引 $(grep -c '^Package: ' "$work/Packages") 个包"

# 用 python 精确匹配包名（别用 grep，wayland-protocols / libxkbcommon-x11 会被误伤）
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
        best = d.get("Filename")           # 同一包出现多次时取最后一个（最新）
print(best or "")
PY
)
    if [ -z "$fn" ]; then
        echo "❌ 索引里没有名为 '$pkg' 的包。相近的：" >&2
        grep '^Package: ' "$work/Packages" | grep -i "$pkg" | sed 's/^/    /' >&2 || true
        exit 1
    fi
    echo "---- 下载 $pkg -> $fn ----"
    curl -fsSL "$REPO/$fn" -o "$work/$pkg.deb"
    dpkg-deb -x "$work/$pkg.deb" "$work/x-$pkg"
    # Termux 的 deb 展开后是 data/data/com.termux/files/usr/...
    src=$(find "$work/x-$pkg" -type d -name usr | head -1)
    [ -n "$src" ] || { echo "解不开 $pkg 的目录结构" >&2; exit 1; }
    [ -d "$src/include" ] && cp -a "$src/include/." "$PREFIX/include/" 2>/dev/null || true
    [ -d "$src/lib" ] && cp -a "$src/lib/." "$PREFIX/lib/" 2>/dev/null || true
done

mkdir -p "$PREFIX/include" "$PREFIX/lib"
mkdir -p "$PREFIX/include" "$PREFIX/lib"
echo "---- 前缀内容 ----"
echo "include: $(ls "$PREFIX/include" 2>/dev/null | tr '\n' ' ' | cut -c1-200)"
echo "lib    : $(ls "$PREFIX/lib" 2>/dev/null | tr '\n' ' ' | cut -c1-200)"

for must in "$PREFIX/include/wayland-server-core.h" \
            "$PREFIX/include/xkbcommon/xkbcommon.h" \
            "$PREFIX/include/xf86drm.h"; do
    [ -f "$must" ] || { echo "❌ 缺头文件: $must" >&2; exit 1; }
done
echo "✅ Termux 依赖就绪"
