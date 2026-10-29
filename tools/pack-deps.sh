#!/system/bin/sh
# Pack the headers and libraries the CI cross build needs from the Termux prefix on
# this device. Only wayland, xkbcommon and libdrm are included.
#
# A shell started by su searches /system/bin, where xz and tar are busybox builds and
# the busybox xz cannot compress with -9, so the Termux bin directory comes first.

rm -f "$OUT" "$OUT.xz"

# Headers: wayland-*.h, xkbcommon/, xf86drm*.h, libdrm/ and drm/
INCS=""
for f in include/wayland*.h include/xf86drm*.h; do [ -e "$f" ] && INCS="$INCS $f"; done
for d in include/xkbcommon include/libdrm include/drm; do [ -d "$d" ] && INCS="$INCS $d"; done

# Libraries: the link needs .so, the runtime needs .so.N, so both are packed
LIBS=""
for f in lib/libwayland-server.so* lib/libxkbcommon.so* lib/libdrm.so* lib/libwayland-util* ; do
    [ -e "$f" ] && LIBS="$LIBS $f"
done

echo "headers: $INCS" | tr ' ' '\n' | head -20
echo "libs   : $LIBS" | tr ' ' '\n' | head -20

tar -cf "$OUT" -h $INCS $LIBS 2>/dev/null
echo "archive holds $(tar -tf "$OUT" | wc -l) entries"
tar -tvf "$OUT"
xz -9 -T0 "$OUT"
ls -l "$OUT.xz"
