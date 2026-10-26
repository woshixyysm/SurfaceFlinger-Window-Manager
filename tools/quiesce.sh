#!/system/bin/sh
# quiesce.sh —— 让指定包「能手动用、但不会自己活过来」
#
#   sh quiesce.sh                  # 默认清单
#   sh quiesce.sh com.foo.bar ...
#   sh quiesce.sh --undo           # 把清单恢复成正常
#
# 手段（都可逆，不 disable 包）：
#   1. am set-standby-bucket restricted      -> 归入罕见使用桶
#   2. appops RUN_ANY_IN_BACKGROUND ignore   -> 禁止后台拉起（含 push 服务自启）
#   3. am force-stop                          -> 立刻清掉现有进程
#
# 不放进清单的东西（会破坏功能）：
#   com.touchtype.swiftkey  sfwm 的注入 socket 在它目录里，杀了打不了字
#   com.android.systemui    状态栏/通知
#   surfaceflinger / system 别碰

DEFAULT="com.tencent.mm com.tencent.mobileqq com.oplus.aiunit"
LIST=$DEFAULT
UNDO=0
case "$1" in
--undo) UNDO=1 ;;
"") ;;
*) LIST=$* ;;
esac

mem() { awk '/MemAvailable/{printf "%.0f MB", $2/1024}' /proc/meminfo; }
cmdok() { "$@" >/dev/null 2>&1 || echo "    (失败: $*)"; }

echo "=== 当前 $(mem) 可用 ==="
for p in $LIST; do
    if [ "$UNDO" = "1" ]; then
        echo "-- 恢复 $p"
        cmdok am set-standby-bucket "$p" active
        cmdok cmd appops set "$p" RUN_ANY_IN_BACKGROUND allow
        cmdok cmd appops set "$p" RUN_IN_BACKGROUND allow
    else
        echo "-- 挂起 $p"
        cmdok am set-standby-bucket "$p" restricted
        cmdok cmd appops set "$p" RUN_ANY_IN_BACKGROUND ignore
        cmdok cmd appops set "$p" RUN_IN_BACKGROUND ignore
        cmdok am force-stop "$p"
    fi
done

sleep 4
echo
echo "=== 之后 $(mem) 可用 ==="
for p in $LIST; do printf '  %-28s %s\n' "$p" "$(pidof $p || echo 未运行)"; done
printf '  %-28s %s\n' sfwm "$(pidof sfwm || echo 不在了！)"
echo
echo "--- 最吃内存的 8 个 ---"
dumpsys meminfo 2>/dev/null | sed -n '/Total PSS by process/,/Total PSS by OOM/p' | head -11
