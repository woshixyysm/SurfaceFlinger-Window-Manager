#!/system/bin/sh
# Runs at on post-fs-data, which is before class core starts, so before surfaceflinger is
# exec'd.
#
# Module managers mount the module tree over /system without relabelling anything, so the
# files keep the data label of /data/adb. That breaks both halves of the hook:
#   - /system/bin/surfaceflinger needs surfaceflinger_exec, otherwise init execs it without
#     switching into the surfaceflinger domain and the system waits for SurfaceFlingerAIDL
#     forever
#   - the stock binary and the helper need labels the surfaceflinger domain may map
#
# chcon acts on the module's own files, which are the upper layer of the mount, so the
# mounted paths get the same label. Failure is not fatal: sepolicy.rule carries fallback
# rules for the data label.
M=/data/adb/modules/sfwm-sfhook

# The wrapper must carry surfaceflinger_exec: init uses that label to switch into the
# surfaceflinger domain. The stock binary it execs afterwards is an ordinary executable.
chcon u:object_r:surfaceflinger_exec:s0 "$M/system/bin/surfaceflinger" 2>/dev/null
chcon u:object_r:system_file:s0 "$M/system/bin/surfaceflinger.real" 2>/dev/null

[ -f "$M/system/lib64/libsfwmsfhook.so" ] &&
    chcon u:object_r:system_lib_file:s0 "$M/system/lib64/libsfwmsfhook.so" 2>/dev/null

mkdir -p /data/adb/sfwm-sf
chmod 0700 /data/adb/sfwm-sf
chcon u:object_r:system_data_file:s0 /data/adb/sfwm-sf 2>/dev/null

exit 0
