#!/system/bin/sh
# 从设备自己的 Termux 前缀里，把 CI 交叉编译需要的那一小撮头/库打包。
# 只挑我们用到的：wayland / xkbcommon / libdrm。
H=/data/data/com.termux/files/home
P=/data/data/com.termux/files/usr
OUT=/data/local/tmp/termux-deps-aarch64.tar
# ⚠️ su 来的 shell PATH 是 /system/bin，xz/tar 都是 busybox 阉割版
#    （busybox xz 只能解压，没有 -9）。必须把 Termux 的 bin 放最前。
export PATH="$P/bin:/system/bin"
cd "$P" || exit 1

rm -f "$OUT" "$OUT.xz"

# 头文件：wayland-*.h、xkbcommon/、xf86drm*.h、libdrm/、drm/
INCS=""
for f in include/wayland*.h include/xf86drm*.h; do [ -e "$f" ] && INCS="$INCS $f"; done
for d in include/xkbcommon include/libdrm include/drm; do [ -d "$d" ] && INCS="$INCS $d"; done

# 库：链接需要 .so，运行时才需要 .so.N，全都带上（体积很小）
LIBS=""
for f in lib/libwayland-server.so* lib/libxkbcommon.so* lib/libdrm.so* lib/libwayland-util* ; do
    [ -e "$f" ] && LIBS="$LIBS $f"
done

echo "头文件：$INCS" | tr ' ' '\n' | head -20
echo "库    ：$LIBS" | tr ' ' '\n' | head -20

tar -cf "$OUT" -h $INCS $LIBS 2>/dev/null
echo "tar 内容 $(tar -tf "$OUT" | wc -l) 项"
tar -tvf "$OUT"
xz -9 -T0 "$OUT"
ls -l "$OUT.xz"
