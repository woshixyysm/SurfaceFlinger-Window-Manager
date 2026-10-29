#!/system/bin/sh
# desktop-mode.sh - device side switch that gives sfwm the desktop
#
# It stops the Android launcher, the feed page and desktop search, and reclaims cached
# processes. SurfaceFlinger and SystemUI are left alone, since the SF composition path
# is what sfwm is built on.
#
#   sh desktop-mode.sh            stop (reversible)
#   sh desktop-mode.sh --restore  restore the Android desktop
#   sh desktop-mode.sh --status   print the current state
#
# HOME does nothing while the launcher is stopped, which is intended. The state
# survives a reboot because pm disable-user is persistent; use --restore to undo it.

DESKTOP_PKGS="com.android.launcher com.coloros.assistantscreen com.heytap.quicksearchbox"

mem() { grep -E 'MemAvailable' /proc/meminfo | awk '{printf "%.0f MB available\n", $2/1024}'; }

status() {
    echo "memory: $(mem)"
    for p in $DESKTOP_PKGS; do
        d=$(pm list packages -d 2>/dev/null | grep -c "^package:$p$")
        printf '  %-32s disabled=%s pid=%s\n' "$p" "$d" "$(pidof $p || echo -)"
    done
    printf '  %-32s pid=%s\n' "sfwm" "$(pidof sfwm || echo not running)"
}

case "${1:-stop}" in
--status)
    status
    ;;
--restore)
    for p in $DESKTOP_PKGS; do
        pm enable "$p" 2>&1 | sed 's/^/  /'
    done
    echo "restored, press HOME or use am start to return to the desktop"
    status
    ;;
*)
    echo "=== before ==="; mem
    for p in $DESKTOP_PKGS; do
        pm disable-user --user 0 "$p" >/dev/null 2>&1
        am force-stop "$p" 2>/dev/null
    done
    sleep 2
    am kill-all 2>/dev/null      # cached and background processes only, which is safe
    sleep 2
    echo "=== after ==="
    status
    ;;
esac
