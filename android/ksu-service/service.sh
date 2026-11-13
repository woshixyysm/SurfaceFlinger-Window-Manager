#!/system/bin/sh
# late_start service: bring the compositor up once the shell is actually alive.
#
# Starting it earlier means SurfaceComposerClient is not ready yet and the first layer creation
# fails, which used to leave a dead process behind that looked like a crash.

MODDIR=${0%/*}
[ -f "$MODDIR/config.sh" ] && . "$MODDIR/config.sh"

# Wait for the boot to settle, but never block forever.
i=0
while [ "$(getprop sys.boot_completed)" != "1" ] && [ $i -lt 120 ]; do
    sleep 2
    i=$((i + 1))
done
sleep 5

"$MODDIR/sfwm-service" start >>"${SFWM_RUN:-/dev/sfwm}/service.log" 2>&1
