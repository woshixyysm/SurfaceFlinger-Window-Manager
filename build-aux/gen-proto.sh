#!/bin/sh
# Generate Wayland protocol code
#   $1 = xml, $2 = output directory, $3 = base name, $4 = namespace (renamed when non-empty)
set -e
xml=$1; outdir=$2; base=$3; ns=$4
wayland-scanner server-header "$xml" "$outdir/$base-server-protocol.h"
if [ -n "$ns" ]; then
    # The generated wlr-layer-shell header names a parameter "namespace", which is a C++ keyword
    sed -i -E 's/\bnamespace\b/ns_name/g' "$outdir/$base-server-protocol.h"
fi
wayland-scanner private-code "$xml" "$outdir/$base-protocol.c"
