# scripts/run/ —— 启动脚本（按构建目录分）

> 两个脚本**只差一行** `BUILD_DIR`：`out/start.sh` 用 `out`，`build/start.sh` 用 `build`。
> 分成两个目录是为了「同名、可互换、一眼看出在跑哪份产物」。

## 1. 为什么不用一条命令直接起二进制

裸跑 `./out/bin/EdgeVisionService` 会漏三件事，出问题时都很难补：

| 漏掉的东西 | 后果 | 脚本做的事 |
| --- | --- | --- |
| `ulimit -c` | 默认 `0` → 崩了没有 core | `ulimit -c unlimited`（失败只告警，提示需要 root / `LimitCORE`） |
| `LD_LIBRARY_PATH` | 找不到 `libcommon_api.so` / `libmk_api.so` | 设为 `<构建目录>/lib:install/rk3588/lib` |
| `EVS_CRASH_DIR` | 报告落到 cwd（随启动位置漂移，事后找不到） | 固定为 `<仓库根>/crash`（已 `.gitignore`） |

另外脚本会**打印当前环境**（`core_pattern` / `ulimit -c` / 构建目录 / 崩溃报告目录），并在最后 `exec` 替换自身 —— 信号直达进程，`Ctrl+C` 不会留在 shell 层。

## 2. 用法

```bash
scripts/run/out/start.sh                        # 用 config/node.json（4 路：1 摄像头 + 3 mp4）
scripts/run/out/start.sh config/push.json       # 指定配置（相对仓库根或绝对路径）
scripts/run/out/start.sh /abs/path/node.json    # 绝对路径
EVS_CRASH_DIR=/data/crash scripts/run/out/start.sh   # 换报告目录
scripts/run/build/start.sh                      # 跑历史构建目录 build/（见 §4）
```

停止：`Ctrl+C`（SIGINT，正常有序退出，退出码 0）。
**取卡死现场**：另开一个终端 `kill -QUIT <pid>` —— handler 会写报告并按默认动作重抛落 core。

## 3. 正常输出的前几行（自检用）

```text
[start] 构建目录 : /mnt/nvme/workspace/EdgeVisionService/out
[start] 二进制   : .../out/bin/EdgeVisionService
[start] LD_LIBRARY_PATH = .../out/lib:.../install/rk3588/lib
[start] core_pattern    = core            ← 相对名 = 落 cwd（out/）
[start] ulimit -c       = unlimited       ← 若为 0，检查 hard limit / LimitCORE
[start] 崩溃报告目录    = .../crash
[crash] 崩溃处理已安装 ... rlimit_core=enabled
```

看到 `[crash]` 这行才说明**这份产物带崩溃采集代码**。

## 4. 两个目录的唯一差别（也是唯一的坑）

| | `out/start.sh` | `build/start.sh` |
| --- | --- | --- |
| `BUILD_DIR` | `out` | `build` |
| 代码新旧 | ✅ 当前源码（`make` 的默认输出） | ⚠️ 2026-11-12 的旧产物，**不含崩溃采集**（日志无 `[crash]` 行） |
| 用于 | 日常开发、抓现场 | 跑旧版本对比行为 |

`build/start.sh` 启动时会额外打印一行警告；要抓崩溃现场请用 `out/` 版。

## 5. 与 systemd 的关系

`deploy/evs-edgevision.service` 的 `ExecStart` 直接指向 `run/out/start.sh`。服务模式下 `ulimit -c` 的**硬上限**通常为 0，脚本抬不上去 —— 必须在 unit 里写 `LimitCORE=infinity`（见 [../deploy/README.md](../deploy/README.md) 与 [../../docs/ops/COMMANDS.md §12](../../docs/ops/COMMANDS.md)）。
