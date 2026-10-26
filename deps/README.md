# deps/ —— 设备上的 Termux 依赖（给 CI 交叉编译用）

`termux-deps-aarch64.tar.xz`（432 KB）是从**本机 Termux 前缀**里直接打出来的：

| 库 | 版本 |
|---|---|
| wayland (server) | 1.25.0 |
| libxkbcommon | 1.13.2 |
| libdrm | 2.4.134 |
| 设备 | OnePlus PLQ110 / Android API 36 (aarch64, bionic) |

只包含 CI 需要的那一小撮：

```
include/wayland*.h        wayland-server-core.h / wayland-server-protocol.h / wayland-util.h …
include/xkbcommon/        xkbcommon.h …
include/xf86drm.h libdrm/ drm/
lib/libwayland-server.so*  lib/libxkbcommon.so*  lib/libdrm.so
```

## 为什么要提交二进制

CI 的 Linux runner 是 glibc/x86_64，编不了 bionic/aarch64 的目标，而 sfwm 要链接的
就是这三个**设备上真正在跑**的库。自己交叉编译它们可以，但那是另一个工程；
从 Termux 仓库在线取包又多一层「包名/索引会不会变」的不确定性。
所以直接把设备上那份打包进来 —— ABI 和运行时完全一致，且不依赖任何外部源。

## 怎么重新生成

设备上跑（脚本在 `tools/pack-deps.sh`）：

```sh
adb push tools/pack-deps.sh /data/local/tmp/pack-deps.sh
adb shell "su 0 sh /data/local/tmp/pack-deps.sh"
adb pull /data/local/tmp/termux-deps-aarch64.tar.xz deps/
```

> ⚠️ 脚本里必须 `export PATH=$PREFIX/bin:/system/bin`：
> `su` 起来的 shell 用的是 busybox 的 `xz`（只能解压、没有 `-9`）和 busybox `tar`。

## 备用路径

`build-aux/ci-fetch-termux-deps.sh` 可以在没有设备的情况下从 Termux 仓库在线取同样的东西
（解析 `Packages.xz` 索引、精确匹配包名）。CI 默认**不走**它，只是留着应急。
