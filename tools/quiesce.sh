#!/system/bin/sh
# quiesce.sh - keep the listed packages usable by hand but stop them starting themselves
#
#   sh quiesce.sh                  default list
#   sh quiesce.sh --undo           restore the list
#
# Three reversible steps are applied and no package is disabled:
#   1. am set-standby-bucket restricted     move to the rare bucket
#   2. appops RUN_ANY_IN_BACKGROUND ignore  block background starts, including push
#   3. am force-stop                        clear the running processes
#
# Never add these:
#   com.touchtype.swiftkey  the sfwm injection socket lives in its directory
#   com.android.systemui    status bar and notifications
#   surfaceflinger, system  leave them alone
DEFAULT="com.tencent.mm com.tencent.mobileqq com.oplus.aiunit"
LIST=$DEFAULT
UNDO=0
case "$1" in
--undo) UNDO=1 ;;
"") ;;
*) LIST=$* ;;
esac

mem() { awk '/MemAvailable/{printf "%.0f MB", $2/1024}' /proc/meminfo; }
cmdok() { "$@" >/dev/null 2>&1 || echo "    (failed: $*)"; }

echo "=== currently $(mem) available ==="
for p in $LIST; do
    if [ "$UNDO" = "1" ]; then
        echo "-- restore $p"
        cmdok am set-standby-bucket "$p" active
        cmdok cmd appops set "$p" RUN_ANY_IN_BACKGROUND allow
        cmdok cmd appops set "$p" RUN_IN_BACKGROUND allow
    else
        echo "-- quiesce $p"
        cmdok am set-standby-bucket "$p" restricted
        cmdok cmd appops set "$p" RUN_ANY_IN_BACKGROUND ignore
        cmdok cmd appops set "$p" RUN_IN_BACKGROUND ignore
        cmdok am force-stop "$p"
    fi
done

sleep 4
echo
echo "=== 之后 $(mem) 可用 ==="
for p in $LIST; do printf '  %-28s %s\n' "$p" "$(pidof $p || echo not running)"; done
printf '  %-28s %s\n' sfwm "$(pidof sfwm || echo not running)"
echo
echo "--- eight largest consumers ---"
dumpsys meminfo 2>/dev/null | sed -n '/Total PSS by process/,/Total PSS by OOM/p' | head -11
