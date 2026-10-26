#!/system/bin/sh
# desktop-mode.sh —— 让 sfwm 独占桌面的设备侧开关
#
# 做的事：停掉安卓自己的桌面（Launcher / 负一屏 / 桌面搜索）并回收缓存进程。
# 不碰 SurfaceFlinger、不碰 SystemUI —— SF 的合成通路是 sfwm 的基石。
#
#   sh desktop-mode.sh            停（可逆）
#   sh desktop-mode.sh --restore  恢复安卓桌面
#   sh desktop-mode.sh --status   只看状态
#
# 注意：停掉 launcher 后按 HOME 不会有反应，这是预期行为。
# 重启设备后依然是停用状态（pm disable-user 是持久的），要恢复就跑 --restore。

DESKTOP_PKGS="com.android.launcher com.coloros.assistantscreen com.heytap.quicksearchbox"

mem() { grep -E 'MemAvailable' /proc/meminfo | awk '{printf "%.0f MB 可用\n", $2/1024}'; }

status() {
    echo "内存: $(mem)"
    for p in $DESKTOP_PKGS; do
        d=$(pm list packages -d 2>/dev/null | grep -c "^package:$p$")
        printf '  %-32s disabled=%s pid=%s\n' "$p" "$d" "$(pidof $p || echo -)"
    done
    printf '  %-32s pid=%s\n' "sfwm" "$(pidof sfwm || echo 未运行)"
}

case "${1:-stop}" in
--status)
    status
    ;;
--restore)
    for p in $DESKTOP_PKGS; do
        pm enable "$p" 2>&1 | sed 's/^/  /'
    done
    echo "已恢复，按 HOME 或 am start 回桌面"
    status
    ;;
*)
    echo "=== 停之前 ==="; mem
    for p in $DESKTOP_PKGS; do
        pm disable-user --user 0 "$p" >/dev/null 2>&1
        am force-stop "$p" 2>/dev/null
    done
    sleep 2
    am kill-all 2>/dev/null      # 只清缓存/后台进程，安全
    sleep 2
    echo "=== 停之后 ==="
    status
    ;;
esac
