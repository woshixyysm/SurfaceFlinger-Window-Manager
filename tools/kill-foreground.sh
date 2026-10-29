#!/system/bin/sh
# kill-foreground.sh - force-stop whatever activity is in the foreground
#
# The goal is to leave no Android application in front, so that only the sfwm layers
# are on screen.
#
#   sh kill-foreground.sh             five rounds, for observation
#   sh kill-foreground.sh --once      a single pass
#   sh kill-foreground.sh --daemon    keep running, with a log and a pidfile
#   sh kill-foreground.sh --stop      stop the daemon
#
# Allow list, since killing these breaks the session:
#   com.termux            the build and runtime environment
#   org.sfwm.ime          the input method host, which is an activity itself
#   com.android.systemui  status bar and notifications
#   android               the framework package, where force-stop has no meaning
#
# am force-stop is used rather than disable, so an explicit launch still works.
# Current foreground package: mResumedActivity, falling back to mFocusedApp
fg_pkg() {
    out=$(dumpsys activity activities 2>/dev/null)
    pkg=$(echo "$out" | grep -m1 'mResumedActivity' | sed -n 's/.*[ {]\([a-zA-Z0-9_.]*\)\/.*/\1/p')
    [ -z "$pkg" ] && pkg=$(echo "$out" | grep -m1 'mFocusedApp' | sed -n 's/.*[ {]\([a-zA-Z0-9_.]*\)\/.*/\1/p')
    echo "$pkg"
}

whitelisted() {
    for w in $WHITELIST; do [ "$1" = "$w" ] && return 0; done
    return 1
}

step() {
    pkg=$(fg_pkg)
    if [ -z "$pkg" ]; then
        echo "$(date +%H:%M:%S) no foreground activity"
        return 0
    fi
    if whitelisted "$pkg"; then
        echo "$(date +%H:%M:%S) foreground $pkg is allow-listed, skipped"
        return 0
    fi
    pid=$(pidof "$pkg")
    am force-stop "$pkg" 2>/dev/null
    echo "$(date +%H:%M:%S) stopping foreground $pkg (pid=${pid:-?})"
}

case "${1:-}" in
--stop)
    if [ -f "$PIDFILE" ] && kill -0 "$(cat $PIDFILE)" 2>/dev/null; then
        kill "$(cat $PIDFILE)"; rm -f "$PIDFILE"; echo "stopped"
    else
        echo "not running"
    fi
    ;;
--once)
    step
    ;;
--daemon)
    if [ -f "$PIDFILE" ] && kill -0 "$(cat $PIDFILE)" 2>/dev/null; then
        echo "already running (pid=$(cat $PIDFILE))"; exit 0
    fi
    echo $$ > "$PIDFILE"
    echo "=== daemon started $(date) ===" > "$LOG"
    while :; do
        step >> "$LOG" 2>&1
        sleep 1
    done
    ;;
*)
    # five rounds by default, for observation
    i=0
    while [ $i -lt 5 ]; do
        step
        i=$((i+1))
        sleep 1
    done
    echo "--- now ---"
    echo "MemAvailable: $(awk '/MemAvailable/{printf "%.0f MB", $2/1024}' /proc/meminfo)"
    echo "foreground: $(fg_pkg)"
    echo "sfwm: $(pidof sfwm || echo not running)"
    ;;
esac
