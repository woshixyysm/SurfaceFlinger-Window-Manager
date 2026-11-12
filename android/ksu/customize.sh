#!/system/bin/sh
# Installer for the optional sfwm-sfhook module.
#
# The only thing that has to happen here is stashing the stock surfaceflinger binary: once
# the module is mounted, /system/bin/surfaceflinger is our wrapper, so the real one has to
# be saved before that. Everything else is a plain file copy.

SKIPUNZIP=1

ui_print "- sfwm SurfaceFlinger hook (optional)"
ui_print "  Only needed to skip sfwm's last composite step."
ui_print "  If the system UI misbehaves, disable this module."

if [ ! -x /system/bin/surfaceflinger ]; then
    abort "! /system/bin/surfaceflinger not found, this does not look like Android"
fi

# Unpack the payload.
unzip -o "$ZIPFILE" 'system/*' 'lib/*' 'module.prop' 'sepolicy.rule' -d "$MODPATH" >/dev/null 2>&1

cp -f /system/bin/surfaceflinger "$MODPATH/surfaceflinger.real" || abort "! cannot back up surfaceflinger"
chmod 0755 "$MODPATH/surfaceflinger.real"

if [ ! -f "$MODPATH/lib/libsfwmsfhook.so" ]; then
    abort "! lib/libsfwmsfhook.so missing from the zip"
fi
if [ ! -f "$MODPATH/system/bin/surfaceflinger" ]; then
    abort "! system/bin/surfaceflinger wrapper missing from the zip"
fi

mkdir -p /data/adb/sfwm-sf
chmod 0700 /data/adb/sfwm-sf

set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/system/bin/surfaceflinger" 0 0 0755
set_perm "$MODPATH/surfaceflinger.real" 0 0 0755
set_perm "$MODPATH/lib/libsfwmsfhook.so" 0 0 0644

ui_print "- Reboot to activate. Socket: /data/adb/sfwm-sf/sf.sock"
ui_print "- Remove with: touch /data/adb/modules/sfwm-sfhook/disable"
