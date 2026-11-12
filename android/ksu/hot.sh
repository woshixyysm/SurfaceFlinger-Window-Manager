#!/system/bin/sh
# hot.sh - rebuild and hot-load the SurfaceFlinger helper without rebooting.
#
# Run from the repo checkout on the device (or after pushing the built files):
#     sh android/ksu/hot.sh
#
# It pushes libsfwmsfhook.so, creates the sentinel file the helper checks, injects the
# library into the running surfaceflinger and reports whether the socket came up. Repeat as
# often as you like: no reboot, and stopping is just deleting the sentinel and restarting
# surfaceflinger.

set -u
MODDIR=/data/adb/modules/sfwm-sfhook
RUNDIR=/data/adb/sfwm-sf
SOCK=$RUNDIR/sf.sock

SO_SRC=${SO_SRC:-$MODDIR/system/lib64/libsfwmsfhook.so}
SO_DST=/data/adb/sfwm-sf/libsfwmsfhook.so
INJECT=${INJECT:-/data/local/tmp/sfinject}

echo "== prepare"
mkdir -p "$RUNDIR"
chmod 0700 "$RUNDIR"

if [ ! -f "$SO_SRC" ]; then
    echo "!! $SO_SRC missing; push the built module first" >&2
    exit 1
fi
if [ ! -x "$INJECT" ]; then
    echo "!! $INJECT missing; build and push sfinject first" >&2
    exit 1
fi

# A fresh copy every time: the running copy stays mapped, the next injection picks up the
# new file.
cp -f "$SO_SRC" "$SO_DST"
chmod 0644 "$SO_DST"

PID=$(pidof surfaceflinger)
if [ -z "$PID" ]; then
    echo "!! surfaceflinger is not running" >&2
    exit 1
fi
echo "== surfaceflinger pid=$PID"

if [ -S "$SOCK" ]; then
    echo "-- socket already present, restarting surfaceflinger to clear the old copy"
    # Removing the sentinel first means the restarted process does not re-arm itself.
    rm -f "$RUNDIR/enable"
    stop surfaceflinger >/dev/null 2>&1
    sleep 1
    start surfaceflinger >/dev/null 2>&1
    for i in $(seq 1 40); do
        PID=$(pidof surfaceflinger) && [ -n "$PID" ] && break
        sleep 0.25
    done
    echo "== restarted, pid=$PID"
fi

# The injector cannot set the environment of the process it attaches to, so the helper also
# accepts this sentinel.
: > "$RUNDIR/enable"

echo "== inject"
"$INJECT" "$PID" "$SO_DST" || exit 1

for i in $(seq 1 40); do
    if [ -S "$SOCK" ]; then
        echo "== socket up: $SOCK"
        exit 0
    fi
    sleep 0.25
done
echo "!! socket did not appear, check: logcat -d | grep -i sfwm" >&2
exit 1
