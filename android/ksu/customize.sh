#!/system/bin/sh
# Installer for the optional sfwm-sfhook module.
#
# The stock binary has to be captured now, while /system/bin/surfaceflinger still points at
# it: the module itself then mounts the wrapper over that path and the stock binary as
# surfaceflinger.real. Everything the hook needs ends up under /system, with the labels the
# surfaceflinger domain is already allowed to use - under /data/adb the domain can neither
# exec nor read, which is what broke the first attempt.

SKIPUNZIP=1

ui_print "- sfwm SurfaceFlinger hook (optional)"
ui_print "  Only needed to skip sfwm's last composite step."
ui_print "  If the system UI misbehaves, disable this module."

if [ ! -x /system/bin/surfaceflinger ]; then
    abort "! /system/bin/surfaceflinger not found, this does not look like Android"
fi

unzip -o "$ZIPFILE" 'system/*' 'module.prop' 'sepolicy.rule' 'README.md' 'hot.sh' -d "$MODPATH" >/dev/null 2>&1

# Capture the stock binary into the module's own /system tree, next to the wrapper.
mkdir -p "$MODPATH/system/bin"
cp -f /system/bin/surfaceflinger "$MODPATH/system/bin/surfaceflinger.real" ||
    abort "! cannot capture the stock surfaceflinger"
chmod 0755 "$MODPATH/system/bin/surfaceflinger.real"

if [ ! -f "$MODPATH/system/lib64/libsfwmsfhook.so" ]; then
    abort "! system/lib64/libsfwmsfhook.so missing from the zip"
fi
if [ ! -f "$MODPATH/system/bin/surfaceflinger" ]; then
    abort "! system/bin/surfaceflinger wrapper missing from the zip"
fi

mkdir -p /data/adb/sfwm-sf
chmod 0700 /data/adb/sfwm-sf

set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/system/bin/surfaceflinger" 0 0 0755
set_perm "$MODPATH/system/bin/surfaceflinger.real" 0 0 0755
set_perm "$MODPATH/system/lib64/libsfwmsfhook.so" 0 0 0644

ui_print "- Reboot to activate. Socket: /data/adb/sfwm-sf/sf.sock"
ui_print "- Disable without losing the stock binary: touch /data/adb/modules/sfwm-sfhook/disable"
