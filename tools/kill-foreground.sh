#!/system/bin/sh
# kill-foreground.sh —— 谁当前台 Activity 就杀谁
#
# 目的：让安卓自己不再跑任何前台应用，屏幕只留 sfwm 的图层。
#
#   sh kill-foreground.sh             跑 5 轮（观察用）
#   sh kill-foreground.sh --once      只做一次判定
#   sh kill-foreground.sh --daemon    常驻循环（落日志 + pidfile）
#   sh kill-foreground.sh --stop      停掉常驻循环
#
# 白名单（杀了会坏事）：
#   com.termux      我们的构建/运行环境
#   org.sfwm.ime    sfwm 的输入法宿主（它自己是个 Activity）
#   com.android.systemui  状态栏/通知，不是 Activity 但保险起见
#   其余一律杀。
#
# ⚠️ 用 am force-stop（不 disable）：它们下次被你/系统显式启动照常能用。

WHITELIST="com.termux org.sfwm.ime com.android.systemui com.android.permissioncontroller android"
# 注："android" 是框架自身的包名，前台变成它就是「已经没有真 Activity 了」；
#     对它 force-stop 没意义，还可能惹到 system_server，所以白名单。
LOG=/data/local/tmp/kill-fg.log
PIDFILE=/data/local/tmp/kill-fg.pid

# 取当前前台包名：优先 mResumedActivity，退回 mFocusedApp
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
        echo "$(date +%H:%M:%S) 没有前台 Activity"
        return 0
    fi
    if whitelisted "$pkg"; then
        echo "$(date +%H:%M:%S) 前台 $pkg —— 白名单，跳过"
        return 0
    fi
    pid=$(pidof "$pkg")
    am force-stop "$pkg" 2>/dev/null
    echo "$(date +%H:%M:%S) 杀掉前台 $pkg (pid=${pid:-?})"
}

case "${1:-}" in
--stop)
    if [ -f "$PIDFILE" ] && kill -0 "$(cat $PIDFILE)" 2>/dev/null; then
        kill "$(cat $PIDFILE)"; rm -f "$PIDFILE"; echo "已停"
    else
        echo "没在跑"
    fi
    ;;
--once)
    step
    ;;
--daemon)
    if [ -f "$PIDFILE" ] && kill -0 "$(cat $PIDFILE)" 2>/dev/null; then
        echo "已经在跑了 (pid=$(cat $PIDFILE))"; exit 0
    fi
    echo $$ > "$PIDFILE"
    echo "=== 常驻开始 $(date) ===" > "$LOG"
    while :; do
        step >> "$LOG" 2>&1
        sleep 1
    done
    ;;
*)
    # 默认跑 5 轮，人眼观察
    i=0
    while [ $i -lt 5 ]; do
        step
        i=$((i+1))
        sleep 1
    done
    echo "--- 现在 ---"
    echo "MemAvailable: $(awk '/MemAvailable/{printf "%.0f MB", $2/1024}' /proc/meminfo)"
    echo "前台: $(fg_pkg)"
    echo "sfwm: $(pidof sfwm || echo 不在了！)"
    ;;
esac
