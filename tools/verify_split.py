#!/usr/bin/env python3
"""无损性证明：把模块按切分表拼回去，与**拆分前那一次提交**里的 sfwm.cpp 逐行比对。

这是「只搬不重写」这个承诺的可复现证据。用法：
    python3 tools/verify_split.py            # 默认对比 PRE_SPLIT_REF
    python3 tools/verify_split.py <git-ref>  # 指定别的基线

注意：模块一旦被有意修改（修 bug、加功能），这项检查就会失败 ——
那是正常的，它比较的是「当前模块」与「历史单文件」的文本差异，不是长期不变的约束。
"""
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 拆分前最后一次提交（此时 src/sfwm.cpp 还是 4958 行的单文件）
PRE_SPLIT_REF = "389c0f6"
REF = sys.argv[1] if len(sys.argv) > 1 else PRE_SPLIT_REF

# 从切分器里用正则抽出 CHUNKS（不执行它的文件 IO 部分）
plan = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "split_modules.py"), encoding="utf-8").read()
block = plan.split("CHUNKS = [")[1].split("]")[0]
CHUNKS = [(int(m.group(1)), m.group(2), m.group(3))
          for m in re.finditer(r'\(\s*(\d+),\s*"([^"]+)",\s*"([^"]*)"\)', block)]
print("模块数:", len(CHUNKS))

orig = subprocess.run(["git", "-C", ROOT, "show", REF + ":src/sfwm.cpp"],
                      capture_output=True, check=True).stdout.decode("utf-8").split("\n")
print("基线: %s:src/sfwm.cpp（%d 行）" % (REF, len(orig)))

rebuilt = []
for start, path, desc in CHUNKS:
    body = open(os.path.join(ROOT, "src", path), encoding="utf-8").read()
    i = body.find("#pragma once\n")
    if i < 0:
        sys.exit("模块缺少 #pragma once: " + path)
    body = body[i + len("#pragma once\n"):].lstrip("\n")
    bl = body.split("\n")
    if bl and bl[-1] == "":      # 文件末尾的换行会多切出一个空元素
        bl.pop()
    rebuilt += bl

cur = open(os.path.join(ROOT, "src", "sfwm.cpp"), encoding="utf-8").read().split("\n")
mi = next(i for i, l in enumerate(cur) if l.startswith("int main("))
main_tail = cur[mi:]

def trim(x):
    while x and x[-1].strip() == "":
        x.pop()
    return x

o = trim(list(orig))
full = trim(rebuilt + main_tail)
print("原件 %d 行 / 拼回 %d 行" % (len(o), len(full)))

n = min(len(o), len(full))
bad = 0
for i in range(n):
    if o[i].rstrip() != full[i].rstrip():
        print("第 %d 行不同:\n  原件: %r\n  拼回: %r" % (i + 1, o[i][:110], full[i][:110]))
        bad += 1
        if bad >= 6:
            break
if bad == 0:
    print("✅ 逐行完全一致（%d 行）：拆分无损，逻辑零改动" % len(o) if len(o) == len(full)
          else "⚠️ 内容一致但行数差 %d" % (len(full) - len(o)))
