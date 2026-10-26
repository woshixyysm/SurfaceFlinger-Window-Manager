#!/system/bin/sh
# kill-heavy.sh —— 把指定包连同其子进程全部 force-stop，并报告内存变化
#
#   sh kill-heavy.sh                 # 默认清单：微信 / QQ
#   sh kill-heavy.sh com.foo.bar ... # 指定包
#
# 只用 am force-stop（不 disable），下次手动点开就正常启动，不需要恢复。
# 注意：不要加 com.touchtype.swiftkey —— sfwm 的注入 socket 建在它的目录里，
#       它是 sfwm 里的输入法载体之一，杀了会打不了字。

DEFAULT="com.tencent.mm com.tencent.mobileqq"
LIST=${*:-$DEFAULT}

mem() { awk '/MemAvailable/{printf "%.0f MB", $2/1024}' /proc/meminfo; }

echo "=== 杀之前: $(mem) 可用 ==="
for p in $LIST; do
    pid=$(pidof "$p")
    am force-stop "$p" 2>&1 | sed 's/^/  /'
    printf '  %-28s %s\n' "$p" "${pid:+pid=$pid 已杀}${pid:-本来就没跑}"
done

sleep 3
echo
echo "=== 杀之后: $(mem) 可用 ==="
echo "--- 清单里还有活着的吗 ---"
for p in $LIST; do printf '  %-28s %s\n' "$p" "$(pidof $p || echo 已退出)"; done
echo "--- sfwm ---"
printf '  %-28s %s\n' sfwm "$(pidof sfwm || echo 不在了！)"
echo
echo "--- 现在最吃内存的 8 个 ---"
dumpsys meminfo 2>/dev/null | sed -n '/Total PSS by process/,/Total PSS by OOM/p' | head -11
