#!/system/bin/sh
# Creates the runtime directory before the rest of the system is up, so the socket path and
# its labels are stable by the time service.sh starts the daemon.
#
# The directory is on tmpfs. Creating it here rather than in the Termux data dir is the whole
# point: PackageManagerService clearing an app's data cannot touch /dev, and /dev/sfwm is not
# a mount point, so it never shows up in mountinfo either.

MODDIR=${0%/*}
[ -f "$MODDIR/config.sh" ] && . "$MODDIR/config.sh"

RUN=${SFWM_RUN:-/dev/sfwm}
UID_=${SFWM_UID:-10391}

mkdir -p "$RUN" 2>/dev/null

# Termux has to be able to search the directory and connect to the socket, but not list it.
chown "$UID_:$UID_" "$RUN" 2>/dev/null
chmod 0777 "$RUN" 2>/dev/null

# tmpfs entries inherit a device label. Anything that has to be reachable by an untrusted app
# domain needs a type that app domains may search; shell_data_file is the one this device
# already uses for that purpose. If avc denials appear for connectto, this is the line to fix.
chcon u:object_r:shell_data_file:s0 "$RUN" 2>/dev/null

# Same treatment for the launchers we expose in /system/bin. The KSU overlay puts them there,
# but the label follows the module directory unless it is corrected here.
for f in sfwm sfwm-service sfwmctl; do
    [ -f "$MODDIR/system/bin/$f" ] && chcon u:object_r:system_file:s0 "$MODDIR/system/bin/$f" 2>/dev/null
done
