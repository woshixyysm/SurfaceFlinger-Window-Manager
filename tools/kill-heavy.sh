#!/system/bin/sh
# kill-heavy.sh - force-stop the listed packages with their children, reporting memory
#
#   sh kill-heavy.sh                 # default list
#   sh kill-heavy.sh com.foo.bar ... # explicit packages
#
# Only am force-stop is used, never disable, so the next launch works normally.
# Do not add com.touchtype.swiftkey: the sfwm injection socket lives in its directory
# and it carries the input method, so killing it breaks typing.

DEFAULT="com.tencent.mm com.tencent.mobileqq"
LIST=${*:-$DEFAULT}

mem() { awk '/MemAvailable/{printf "%.0f MB", $2/1024}' /proc/meminfo; }

echo "=== before: $(mem) available ==="
for p in $LIST; do
    pid=$(pidof "$p")
    am force-stop "$p" 2>&1 | sed 's/^/  /'
    printf '  %-28s %s\n' "$p" "${pid:+killed pid=$pid}${pid:-was not running}"
done

sleep 3
echo
echo "=== after: $(mem) available ==="
echo "--- still running from the list ---"
for p in $LIST; do printf '  %-28s %s\n' "$p" "$(pidof $p || echo exited)"; done
echo "--- sfwm ---"
printf '  %-28s %s\n' sfwm "$(pidof sfwm || echo not running)"
echo
echo "--- eight largest consumers now ---"
dumpsys meminfo 2>/dev/null | sed -n '/Total PSS by process/,/Total PSS by OOM/p' | head -11
