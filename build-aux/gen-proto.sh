#!/bin/sh
# 生成 Wayland 协议代码
#   $1 = xml, $2 = 输出目录, $3 = base 名, $4 = ns（非空则把 namespace 改名）
set -e
xml=$1; outdir=$2; base=$3; ns=$4
wayland-scanner server-header "$xml" "$outdir/$base-server-protocol.h"
if [ -n "$ns" ]; then
    # wlr-layer-shell 生成的 header 里参数名叫 namespace，是 C++ 关键字
    sed -i -E 's/\bnamespace\b/ns_name/g' "$outdir/$base-server-protocol.h"
fi
wayland-scanner private-code "$xml" "$outdir/$base-protocol.c"
