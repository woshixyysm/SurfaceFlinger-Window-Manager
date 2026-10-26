#!/system/bin/sh
# android-desktop.sh —— 打开安卓自己的「桌面模式 / 自由窗口」能力
#
#   sh android-desktop.sh            打开
#   sh android-desktop.sh --restore  关回去
#   sh android-desktop.sh --status   看状态
#
# 这一步和 sfwm 无关：它只是让 Android 允许非全屏的、可拖拽缩放的应用窗口
# （freeform），这样才有「桌面」这个概念可用；Hyprland / sway 这种嵌套合成器
# 才有地方糊。

apply() { settings put global "$1" "$2" 2>&1 | sed 's/^/  /'; }
get()   { settings get global "$1" 2>/dev/null; }

status() {
    for k in development_settings_enabled enable_freeform_support \
             force_resizable_activities enable_non_resizable_multi_window \
             force_desktop_mode_on_external_displays override_desktop_mode_features; do
        printf '  %-44s %s\n' "$k" "$(get $k)"
    done
    echo "  supportsFreeform(系统能力): $(dumpsys window 2>/dev/null | grep -m1 -i 'mSupportsFreeform' || echo '(dumpsys 没这项)')"
}

case "${1:-on}" in
--status) echo "=== 现状 ==="; status ;;
--restore)
    echo "=== 关回去 ==="
    apply enable_freeform_support 0
    apply force_desktop_mode_on_external_displays 0
    status
    echo "（freeform 关掉通常要重启才彻底生效）"
    ;;
*)
    echo "=== 打开之前 ==="; status
    echo "=== 打开 ==="
    apply development_settings_enabled 1
    apply enable_freeform_support 1
    apply force_resizable_activities 1
    apply enable_non_resizable_multi_window 1
    apply force_desktop_mode_on_external_displays 1
    apply override_desktop_mode_features -1
    echo "=== 打开之后 ==="; status
    echo
    echo "=== 用设置页试一个自由窗口（windowingMode=5 freeform）==="
    am start --windowingMode 5 -n com.android.settings/.Settings 2>&1 | sed 's/^/  /'
    sleep 3
    dumpsys activity activities 2>/dev/null | grep -m4 -E 'windowingMode|mResumedActivity' | sed 's/^/  /'
    ;;
esac
