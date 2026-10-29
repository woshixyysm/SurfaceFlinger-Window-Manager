#!/system/bin/sh
# android-desktop.sh - enable the Android desktop mode with freeform windows
#
#   sh android-desktop.sh            enable
#   sh android-desktop.sh --restore  restore the defaults
#   sh android-desktop.sh --status   print the current state
#
# This is independent of sfwm: it lets Android host draggable, resizable
# non-fullscreen application windows, which is the window model a nested compositor
# needs.

apply() { settings put global "$1" "$2" 2>&1 | sed 's/^/  /'; }
get()   { settings get global "$1" 2>/dev/null; }

status() {
    for k in development_settings_enabled enable_freeform_support \
             force_resizable_activities enable_non_resizable_multi_window \
             force_desktop_mode_on_external_displays override_desktop_mode_features; do
        printf '  %-44s %s\n' "$k" "$(get $k)"
    done
    echo "  supportsFreeform (system capability): $(dumpsys window 2>/dev/null | grep -m1 -i 'mSupportsFreeform' || echo '(absent from dumpsys)')"
}

case "${1:-on}" in
--status) echo "=== current state ==="; status ;;
--restore)
    echo "=== restoring ==="
    apply enable_freeform_support 0
    apply force_desktop_mode_on_external_displays 0
    status
    echo "(a reboot is usually needed before freeform is fully disabled)"
    ;;
*)
    echo "=== before ==="; status
    echo "=== enabling ==="
    apply development_settings_enabled 1
    apply enable_freeform_support 1
    apply force_resizable_activities 1
    apply enable_non_resizable_multi_window 1
    apply force_desktop_mode_on_external_displays 1
    apply override_desktop_mode_features -1
    echo "=== after ==="; status
    echo
    echo "=== launching a freeform window (windowingMode=5) ==="
    am start --windowingMode 5 -n com.android.settings/.Settings 2>&1 | sed 's/^/  /'
    sleep 3
    dumpsys activity activities 2>/dev/null | grep -m4 -E 'windowingMode|mResumedActivity' | sed 's/^/  /'
    ;;
esac
