#!/system/bin/sh
# late_start service: bring the compositor up once the shell is actually alive.
#
# Two things have to be true before starting, and sys.boot_completed covers neither:
#
#   1. SurfaceComposerClient has to be ready, or the first layer creation fails and what is
#      left behind looks exactly like a crash.
#   2. The user has to have unlocked at least once. Termux's data directory is credential
#      encrypted, so before unlock the binary and the whole Mesa/EGL stack (they live under
#      /data/data/com.termux/files/usr) simply do not exist. Starting earlier fails with
#      "missing binary".
#
# So wait for both, then start, retrying a few times: the first attempt can land in the
# window where the data directory exists but its contents are still being mounted.

MODDIR=${0%/*}
[ -f "$MODDIR/config.sh" ] && . "$MODDIR/config.sh"

BIN=${SFWM_BIN:-/data/data/com.termux/files/home/lg/sfwm/sfwm}
PREFIX=${SFWM_PREFIX:-/data/data/com.termux/files/usr}
RUN=${SFWM_RUN:-/dev/sfwm}

mkdir -p "$RUN" 2>/dev/null

# 120 x 5 s = 10 minutes, generous and still bounded.
i=0
while [ $i -lt 120 ]; do
    booted=$(getprop sys.boot_completed)
    # The real signal is the data being there: the encrypted store only gives up its contents
    # after an unlock, so these two tests cannot both pass before then.
    if [ "$booted" = "1" ] && [ -x "$BIN" ] && [ -d "$PREFIX/lib" ]; then
        break
    fi
    sleep 5
    i=$((i + 1))
done

sleep 3

i=0
while [ $i -lt 6 ]; do
    if "$MODDIR/sfwm-service" start >>"$RUN/service.log" 2>&1; then
        echo "started on attempt $i" >>"$RUN/service.log"
        exit 0
    fi
    sleep 5
    i=$((i + 1))
done
echo "gave up after $i attempts" >>"$RUN/service.log"
