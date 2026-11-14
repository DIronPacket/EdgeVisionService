# EdgeVisionService 常用命令手册（分类 · 作用 · 实测输出）

> 本文只回答一件事：**想干某件事，该敲哪条命令**。每条命令都写明「作用 + 为什么这么写 + 实测输出」。
> 分工：**规矩**在 [HANDOFF.md](../HANDOFF.md) §2 · **测试证据**在 [TESTLOG.md](../history/TESTLOG.md) · **bug 原理**在 [BUG_CASES.md](../history/BUG_CASES.md) · **命令**在本文。
>
> 适用：分支 `rk3588`、板端 aarch64 本机编译（cmake 3.22 / g++ 11.4 / 8 核）。
> **所有命令默认从仓库根执行**：`cd /mnt/nvme/workspace/EdgeVisionService`。
> 标注「✅ 实测」的条目于 **2026-11-10** 在本机真跑过，输出为当时原文摘录；其余为按仓库约定给出的等价写法。

## 目录

- [0. 三条通用套路（先读这段，后面的命令都靠它读结果）](#0-三条通用套路先读这段后面的命令都靠它读结果)
- [1. 构建](#1-构建)
- [2. 运行程序与优雅退出](#2-运行程序与优雅退出)
- [3. 看内存（进程级 / 堆级 / 泄漏）](#3-看内存进程级--堆级--泄漏)
- [4. 临时探针：写文件、编译、运行（不污染仓库）](#4-临时探针写文件编译运行不污染仓库)
- [5. 进程 / 端口 / 线程 / 设备](#5-进程--端口--线程--设备)
- [6. 自检脚本（无硬件依赖）](#6-自检脚本无硬件依赖)
- [7. Git（提交 / 校验 / 推送）](#7-git提交--校验--推送)
- [8. 文档一致性体检](#8-文档一致性体检)
- [9. 踩过的坑速查表](#9-踩过的坑速查表)
- [10. 一屏自检（复制粘贴）](#10-一屏自检复制粘贴)
- [11. 崩溃现场：报告 / core / 符号化](#11-崩溃现场报告--core--符号化)
- [12. 部署 / 开机自启（systemd）](#12-部署--开机自启systemd)

## 0. 三条通用套路（先读这段，后面的命令都靠它读结果）

### 0.1 输出落盘再读（对付超时 / 输出被截断）

```bash
{ echo '=== 段1 ==='; cmd1; echo "RC1=$?"; echo '=== 段2 ==='; cmd2; echo "RC2=$?"; } > /tmp/out.txt 2>&1; cat /tmp/out.txt
```

**作用**：一次跑多条命令、把 stdout+stderr 一起写盘，避免终端只回显一半；`cat` 出来即完整证据。
**为什么**：构建/运行动辄几十秒到几分钟，直接看终端容易只看到尾部；落盘后可反复 `grep` 定位。
⚠️ 别写成 `{ ...; cat /tmp/out.txt; } > /tmp/out.txt` —— 会报 `cat: /tmp/out.txt: input file is output file`（今晚已踩）。

### 0.2 先拿行号，再定点读（不要整篇读）

```bash
grep -nE '^#{1,3} ' docs/HANDOFF.md            # 拿全部章节行号（~150 token）
grep -n 'P0-5' docs/STATUS.md | cut -c1-160    # 只打印命中行前 160 列，避免刷屏
sed -n '93,145p' docs/HANDOFF.md               # 读指定行区间
awk '/^### 4\.1/,/^### 4\.2/' docs/STATUS.md   # 抓两个标题之间的一整节
grep -c 'slot_free<FrameBuffer' src/common/MemManager/MemMeta.h   # 只数出现次数（0 = 已删干净）
```

**作用**：把「读文档」成本从全文 token 降到几百。
**为什么**：`core.handoff` 明确要求「不要全仓扫描」；`cut -c1-N` 是防刷屏的关键，中文一行能到 300+ 列。

### 0.3 每条命令后面立刻验退出码

```bash
cmd; echo "RC=$?"
```

**作用**：`RC=0` 才算通过，不靠「看起来没报错」判断。
**为什么**：管道最后一环决定退出码 —— `cmd | tail -2` 的 RC 是 `tail` 的，永远 0；要看真实结果用 `PIPESTATUS[0]` 或先落盘。

---

## 1. 构建

```bash
make                                   # ✅ 首选：= cmake -S . -B out + cmake --build out -j"$(nproc)"
make -j8 2>&1 | tail -2                # 只看结果尾（✅ 实测 2026-11-12：[100%] Built target EdgeVisionService）
make configure                         # 只配置（首次 / 改了 CMakeLists 后）
make reconfigure                       # 强制重跑 configure（新增 option / 清了缓存后）
make rebuild                           # clean 后全量重编（保留手写 run.sh）
cmake --build out --target EdgeVisionService   # 只编某个目标（规则仍由 CMake 定义）
```

**作用**：`make` → configure（首次自动）→ 编译。
**为什么**：顶层 `Makefile` 是**薄包装**，源码/依赖规则全在 CMake；默认构建目录 **`out/`**（`BUILD=` 可覆盖）。等价手写 `cmake -S . -B out && cmake --build out -j"$(nproc)"`；产物在 `out/bin`、`out/lib`。

**清干净重建（慎用）**

```bash
make clean                             # 只清编译目标（CMake --target clean），保留手写 run.sh
make distclean FORCE=1                 # 删整个 out/（未加 FORCE=1 时直接拒绝，防误删）
```

⚠️ `clean` 之所以不 `rm -rf`：构建目录里的 `run.sh` 是手写、不入库的脚本（HANDOFF §1.5），`rm -rf` 会连它一起删。彻底重来才用 `distclean FORCE=1`。

**编译期反证（验证「该编不过的确实编不过」）**

```bash
g++ -std=c++14 -I src/common /tmp/fb_copy_forbidden.cpp -o /tmp/neg; echo "RC=$?"   # ✅ 实测 RC=1
```

✅ 实测输出（P0-10 禁拷贝的证据）：

```text
error: use of deleted function 'FrameBuffer::FrameBuffer(const FrameBuffer&)'
src/common/frame_buffer.h:27:3: note: declared here
error: use of deleted function 'FrameBuffer& FrameBuffer::operator=(const FrameBuffer&)'
```

---

## 2. 运行程序与优雅退出

### 2.1 冒烟（最快的一条）

```bash
make run                                # ✅ 实测：EdgeVisionService start
```

**作用**：验证「能跑起来」，不碰硬件、不起流水线。
**为什么**：`make run` 会 `cd out/` + 导出 `LD_LIBRARY_PATH`（`out/lib` + `install/rk3588/lib`）+ 运行 `bin/EdgeVisionService`，不依赖未入库的 `run.sh`。

### 2.2 真实主链路（4 路 / 单路）

```bash
make run-node                                          # 默认 CFG=../config/node.json（4 路）
make run-node CFG=/abs/path/config/push.json           # 显式绝对路径（1 路，避免 cwd 歧义）
```

⚠️ `testnode` **当前未参与构建**（`src/main/CMakeLists.txt` 的 `# add_subdirectory(test)` 为注释）；`make run-node` 会提示这一点。需要时取消注释 → `make reconfigure && make`。

**为什么用绝对路径**：构建目录被设为 cwd（`out/`），相对路径会相对 `out/` 解析 —— 这是「配置读不到」的头号原因。

### 2.3 定时跑 + 模拟 Ctrl+C（自动化验证用这个）

```bash
timeout --preserve-status -s INT 10 make run-node CFG=/abs/path/config/push.json; echo "EXIT=$?"
```

✅ 实测：`EXIT=0`，日志尾部有 `[main] FreeMem 完成，进程正常退出`，`pgrep testnode` 无残留。
⚠️ 不加 `--preserve-status` 时 `timeout` 自身返回 **124**，别当成崩溃（TESTLOG §5.15 有此备注）。

### 2.4 不用 make 的等价写法（排「是不是 Makefile / 脚本的问题」）

```bash
export LD_LIBRARY_PATH="$PWD/out/lib:$PWD/install/rk3588/lib:$LD_LIBRARY_PATH"
./out/bin/testnode "$PWD/config/node.json"
```

### 2.5 两个一键启动脚本（`out/` 与 `build/` 各一个，同名 `start.sh`）

```bash
scripts/run/out/start.sh                      # out/bin/EdgeVisionService + config/node.json
scripts/run/out/start.sh config/push.json     # 指定配置（相对仓库根或绝对路径）
scripts/run/build/start.sh                    # build/bin/EdgeVisionService（11-12 旧产物）

timeout --preserve-status -s INT 5 scripts/run/out/start.sh; echo "RC=$?"   # 自动化验证
```

脚本做的事（每次启动都可复查，已打印出来）：

| 项 | 值 | 为什么 |
| --- | --- | --- |
| `ulimit -c` | `unlimited`（失败只告警） | 本机 soft=0，不抬就没有 core |
| `LD_LIBRARY_PATH` | `<构建目录>/lib` + `install/rk3588/lib` | 与 `make run` 完全一致 |
| `EVS_CRASH_DIR` | 默认 `<仓库根>/crash` | 报告集中一处，不随 cwd 漂移 |
| cwd | `<构建目录>` | 与历史实测行为一致（相对资源路径按 `out/` 解析）；`core_pattern=core` 时 core 也落这里 |
| 前台 `exec` | — | `Ctrl+C` 直接送到程序；`kill -QUIT <pid>` 能命中 handler |

✅ 实测（2026-11-14）：`out` 版 4 路出帧、`ulimit -c=unlimited`、`[crash] 崩溃处理已安装 ... rlimit_core=enabled`，`timeout -s INT` 后 `RC=0`（优雅退出）。
⚠️ `build/` 是 11-12 的旧产物：能跑（实测 4 路），但**里面没有崩溃采集代码**（日志无 `[crash]` 行）。要抓现场请用 `out` 版。


## 3. 看内存（进程级 / 堆级 / 泄漏）

内存问题分三层看：**进程占多少** → **堆里是谁在占** → **有没有泄漏**。三层命令不通用，混用会得出错误结论。

### 3.1 进程级：`/proc/<pid>/status`（最准的单点快照）

```bash
PID=$(pgrep -n testnode) && grep -E 'VmPeak|VmSize|VmRSS|Threads' /proc/$PID/status
```

✅ 实测（`push.json` 单路跑 4 秒时）：

```text
VmPeak:	 1094768 kB      ← 历史峰值（含 MPP/ZLMediaKit 映射过的大虚拟区）
VmSize:	 1091728 kB      ← 当前虚拟地址空间
VmRSS:	  172708 kB      ← 实际驻留物理内存（判断"吃多少内存"看这个）
Threads:	11
```

**怎么读**：`VmRSS` 才是物理占用；`VmSize/VmPeak` 在多媒体程序里天然巨大（MPP 会 mmap 设备与 DMA 缓冲），**不能拿它判断泄漏**。

### 3.2 系统级：`free` / `top` / `ps`

```bash
free -m                                                 # 整机内存与 buff/cache
top -b -n1 | head -15                                   # 批处理取一屏
ps -o pid,rss,vsz,pcpu,comm -p "$(pgrep -n testnode)"   # 单进程 RSS(kB)/VSZ/CPU
```

**为什么必须 `-b -n1`**：`top` 默认全屏交互，脚本里不加参数会卡住。

### 3.3 堆级：`mallinfo2()` —— 区分 mmap 与主堆（池泄漏靠这个定位）

```cpp
struct mallinfo2 mi = mallinfo2();
// 在用总量 = hblkhd（仍在用的 mmap 块） + uordblks（主堆在用）
```

✅ 实测（`MemInit(2)` + `FreeMem()` 循环）：

```text
baseline             in_use=   73600 B (mmap=0        + heap=73600)
cycle 0 after init   in_use=12525728 B (mmap=12443648 + heap=82080)
[MemManager] FreeMem: 释放 2 块 / 4 个槽位
cycle 0 after free   in_use=   82080 B (mmap=0        + heap=82080)   ← 完全回落 = 无泄漏
```

**为什么不能只看 `hblkhd`**：glibc 对 ≥128 KB 的分配走 mmap，`free` 后同尺寸再分配可能改走 brk 主堆 ——「mmap 归零但主堆涨了」，只看一项会误判；必须看 `hblkhd + uordblks` 之和。
**为什么不用 RSS 判断堆**：没被写过的页不进 RSS，`new[]` 后不 memset 时 RSS 几乎不涨。

### 3.4 泄漏检测：ASAN 优先，valgrind 兜底

```bash
# ASAN：编进探针（或整个工程）
g++ -std=c++14 -g -fsanitize=address -I src/common /tmp/fb_real_probe.cpp -o /tmp/probe_fb
/tmp/probe_fb; echo "RC=$?"        # RC=0 且无 AddressSanitizer 输出 = 干净

# valgrind（本机未安装：需要时先 sudo apt install valgrind；被调程序要带 -g 编）
valgrind --leak-check=full --error-exitcode=1 ./out/bin/testnode "$PWD/config/push.json"
```

**为什么 ASAN 优先**：零配置，直接给「double-free / use-after-free / leak」的调用栈；板端也**不需要额外安装**（g++ 自带）。
⚠️ 本机实测 `command -v valgrind` → 未安装，走这条前先装。
⚠️ ASAN 下 `mallinfo2` 偏大（redzone + 隔离区）—— **ASAN 判「有没有错」，非 ASAN 版判「占多少内存」**，别混用。

### 3.5 池内视角：程序自己打的账

```text
[MemManager] FreeMem: 释放 50 块 / 100 个槽位
```

**作用**：`FreeMem()` 报「释放多少块 / 多少槽位」。数字与 `MemInit()` 建的数量不符 = 有块或槽位丢在别处（P0-9 / P0-11 就是靠这行 + `mallinfo2` 双向确认的）。

### 3.6 查某个地址是谁映射的

```bash
cat /proc/$PID/maps | grep -E 'mpp|dma|heap' | head
grep -A6 -E 'mpp|heap' /proc/$PID/smaps | head -40      # 带 Rss/Pss 的明细
```

## 4. 临时探针：写文件、编译、运行（不污染仓库）

**原则**：一次性验证代码放在 `/tmp`，**不进 `src/`、不进 `out/`、不进 git**。文件名要说明它证明什么，验证完把结论写进 `docs/history/TESTLOG.md` 并附探针路径。

### 4.1 写一个临时源文件（heredoc，防变量展开）

```bash
cat > /tmp/fb_real_probe.cpp <<'EOF'
#include "frame_buffer.h"
#include <cstdio>
int main() {
  FrameBuffer *f = new FrameBuffer;
  if (!f->Allocate(1920, 1080, PixelFormat::FMT_NV12, 16)) return 1;
  printf("owned_mem=%p\n", (void *)f->owned_mem);
  delete f;                       // ~FrameBuffer → Release() → delete[]
  return 0;
}
EOF
```

**为什么用 `<<'EOF'`（带引号）**：带引号才**不做** `$VAR` / 反引号展开，C++ 里的 `$` 和反引号不会被 shell 吃掉；不加引号是新手最常见的「明明复制对了却编不过」。
**其它写文件姿势**：

```bash
printf '%s\n' 'line1' 'line2' > /tmp/x.txt      # 少量行，不用 heredoc
echo something >> /tmp/x.txt                    # 追加
cat > /tmp/run_probe.sh <<'EOF'                 # 临时脚本
#!/bin/bash
set -e
EOF
bash /tmp/run_probe.sh                          # 用 bash 显式跑，别忘 chmod +x
```

### 4.2 纯头文件探针：`-I src/common` 就够，无需链接

```bash
g++ -std=c++14 -g -fsanitize=address -I src/common /tmp/fb_real_probe.cpp -o /tmp/probe_fb
echo "CC_RC=$?" ; /tmp/probe_fb; echo "RUN_RC=$?"
```

✅ 实测输出：

```text
CC_RC=0
heap: owned_mem=0x7fb1a10800 data1=0x7fb1c0ac00
heap: delete 完成（未显式 Release）
view: owned_mem=(nil) data0=0x7fb2107800     ← 栈视图 owned_mem 为空 → 析构 no-op
view 出作用域后外部缓冲仍可写: 0x5A
RUN_RC=0
```

**为什么这样能编**：`frame_buffer.h` 是纯头文件（只依赖 `<cstdint>/<iostream>/<cstddef>`）；`MemMeta.h` 也是纯头（额外 `-I src/common/MemManager`）。

### 4.3 需要实现文件的探针：`-L out/lib -l<库>`

```bash
g++ -std=c++14 -g -I src/common -I src/common/MemManager \
    /tmp/mem_probe.cpp -o /tmp/probe_mem -L out/lib -lcommon_api
echo "CC_RC=$?"
LD_LIBRARY_PATH="$PWD/out/lib:$PWD/install/rk3588/lib" /tmp/probe_mem 2 3; echo "RUN_RC=$?"
```

✅ 实测输出（节选，证明 `MemInit/FreeMem` 无泄漏）：

```text
[MemManager] FreeMem: 释放 2 块 / 4 个槽位
cycle 2 after free       in_use=   82080 B (mmap=0 + heap=82080)  rss=3888 kB
RUN_RC=0
```

**三条要点**：
1. `-L out/lib -lcommon_api` 是**编译期**找库；`LD_LIBRARY_PATH` 是**运行期**找 `.so` —— 只做前者会 `error while loading shared libraries: libcommon_api.so`。
2. 链接顺序：源文件在前、`-l` 在后（`--as-needed` 下反了会 `undefined reference`）。
3. 用 `$PWD` 拼绝对路径，避免 `LD_LIBRARY_PATH` 里的相对路径被解释成相对 `out/`。

### 4.4 反例探针（证明「不加某约束就会崩」）

```bash
g++ -std=c++14 -g -fsanitize=address -I /tmp /tmp/fb_raii_copy_probe.cpp -o /tmp/probe_bad
/tmp/probe_bad; echo "RC=$?"      # 期望 RC=1（ASAN 主动 abort），不是 0
```

✅ 实测输出（节选）：

```text
CC_RC=0
RUN_RC=1
==26317==ERROR: AddressSanitizer: attempting double-free on 0x007fa5800100 in thread T0:
    #1 ... in FrameBuffer::Release() /tmp/frame_buffer_raii.h:61
```

**作用**：P0-10 就是靠「只加析构、不禁拷贝」的反例探针，证明「加析构必须连拷贝一起禁」，否则泄漏升级成必崩（TESTLOG §5.15）。
**怎么判读**：这类探针**期望非 0 退出码**——`RC=1` + `double-free` 才是验证成功，`RC=0` 反而说明反例没生效。

### 4.5 收尾

```bash
ls -la /tmp/*probe*            # 看今晚留下了什么
rm -f /tmp/probe_* /tmp/doc_probe_*   # 删二进制，保留 .cpp 便于复现（.cpp 也可删）
```

### 4.6 RGA 探针（验证 DMA 缓冲直传硬件加速器）

```bash
g++ -std=c++14 -O2 -I/usr/local/include \
    -I/mnt/nvme/workspace/EdgeVisionService/3rdparty/rk3588/librga-1.9.1/include \
    /tmp/rga_probe.cpp -o /tmp/rga_probe \
    -L/usr/local/lib -lrockchip_mpp -lrga -lpthread
LD_LIBRARY_PATH=/usr/local/lib:/usr/lib timeout 30 /tmp/rga_probe; echo "EXIT=$?"
```

**作用**：验证 `mpp_buffer_get` 拿到的 DRM 缓冲能否**不经 CPU 拷贝**直接交给 RGA —— `mpp_buffer_get_fd(native)` → `importbuffer_fd()` → `wrapbuffer_handle()`，之后就能 `imcopy` / `imresize` / `imcvtcolor` / `imcrop` / `improcess`。
**为什么这么写**：

- **RGA 头必须显式用 `3rdparty/rk3588/librga-1.9.1/include`**：`/usr/local/include/rockchip/` 下 17 个 RGA 头全是 **0 字节空壳**（`rga.h` / `im2d*.h` / `Rga*.h`），只有 MPP 头正常。写 `#include "im2d.h"` 时命中的是 3rdparty 那份（空壳在 `rockchip/` 子目录里，不会被顶层 `im2d.h` 查找命中），但一旦写成 `#include "rockchip/im2d.h"` 就会命中空壳，报一堆看不到的符号。
- `-lrockchip_mpp -lrga` 放源文件之后（`--as-needed`）。
- 运行期要 `LD_LIBRARY_PATH=/usr/local/lib`，否则 `error while loading shared libraries: librga.so`。
**判读**：`imStrError()` 打印 `Run successfully` / `No errors during operation` 即成功。失败时先看 `rect[x,y,w,h]` 与格式，三种高频原因（都已实测）：
1. `RGA_BLIT fail: Invalid argument` + 矩形宽 < 68 → **RGA3 的宽度下限**，不是内存/cache 问题（宽 64、66 失败，68 起成功；高度无此下限）
2. `Unsupported to scaling more than 1/16 ~ 16 times` → 用了无 rect 参数的 `imblend`，它按**整块 dst** 算缩放比（64×32 → 1280×720 = 20 倍）；改用 `improcess(..., src_rect, dst_rect, ...)`
3. `src, Error yuv not aligned` → 矩形宽为奇数（NV12 要求偶数宽）

**注意**：`mpp_buffer_get_fd` 的 fd **不要 `close()`**（归 MPP 管），但 RGA 侧的 `releasebuffer_handle()` 必须调；`mpp_buffer_put` / `FrameMemDeinit()` 之前所有 RGA job 必须已完成（**`sync=1`，即 `IM_SYNC` 或不显式 `imasync`**），否则驱动的 job 会握着已 `munmap` 的缓冲。

---

## 5. 进程 / 端口 / 线程 / 设备

本机工具在位情况（✅ 实测 2026-11-10）：`ss` / `pgrep` / `v4l2-ctl` / `lsof` / `top` / `free` 都在 PATH；`ffmpeg`、`ffprobe` 只在 `3rdparty/rk3588/ffmpeg/bin/`；**`ffplay` 与 `valgrind` 未安装**（下文相应条目已注明替代方案）。

### 5.1 找进程、看有几个

```bash
pgrep -a testnode          # 列出 pid + 完整命令行（-a 必加，否则只有数字）
pgrep -n testnode          # 只取最新的一个（脚本里取 PID 最方便）
pgrep -c testnode          # 数个数：跑 4 路流水线时确认只有一个进程
```

✅ 实测：`PID=$(pgrep -n testnode)` → `25130`。

### 5.2 用信号退出（模拟 Ctrl+C，走优雅释放）

```bash
kill -INT "$PID"           # 等价 Ctrl+C → 触发 SIGINT 处理 → Pipeline 停 → MemManager::FreeMem
pkill -INT testnode        # 按名字发（有多个 testnode 时慎用）
```

⛔ **绝不 `kill -9`**：跳过 SIGINT 处理，`FreeMem()` 不执行，池块与其槽位全部留在操作系统里 —— 这时候再去「查泄漏」得到的是假象。
✅ 实测：`kill -INT` 后日志出现 `[main] FreeMem 完成，进程正常退出`，退出码 0。

### 5.3 端口：RTSP 是否真的起来了

```bash
ss -ltnp | grep 8550
```

✅ 实测：

```text
LISTEN 0  0  *:8550  *:*  users:(("testnode",pid=25130,fd=20))
```

**作用**：一眼看出「谁在监听、pid 多少、fd 多少」；端口没起来先查配置里的 `rtsp.port`，别急着怀疑 MPP。
`lsof -i :8550` 是等价手段（板端不一定装了 lsof，`ss` 一定在）。

### 5.4 线程数（判断线程泄漏 / 起没起工作线程）

```bash
ls /proc/$PID/task | wc -l          # 实测 11（与 status 里的 Threads 一致）
ls -l /proc/$PID/task | awk '{print $9}' | sort | head   # 线程 TID 列表
```

**用法**：跑一段时间后再数一次，线程数持续上涨 = 有线程没回收。

### 5.5 摄像头设备

```bash
ls -l /dev/video*                       # 有没有设备节点
v4l2-ctl --list-devices                 # 设备名与对应的 /dev/videoN
v4l2-ctl -d /dev/video0 --list-formats-ext   # 支持的分辨率/像素格式（NV12 是否可用）
```

**为什么先查这个**：无 `/dev/video0` 时按 P0-2 会直接失败，属已知，不要误判成新 bug。

### 5.6 拉流验证（端到端最后一环）

本机 ffmpeg 工具在 `3rdparty/rk3588/ffmpeg/bin/`（✅ 实测：只有 `ffmpeg` 与 `ffprobe`，**没有 `ffplay`**，且不在 PATH 上）：

```bash
FF=3rdparty/rk3588/ffmpeg/bin
"$FF/ffprobe" -version | head -1        # ✅ 实测：ffprobe version 5.0.3
"$FF/ffprobe" -v error -rtsp_transport tcp \
    -show_entries stream=codec_name,width,height,avg_frame_rate \
    rtsp://127.0.0.1:8550/live/ch0
"$FF/ffmpeg" -rtsp_transport tcp -i rtsp://127.0.0.1:8550/live/ch0 -t 5 -f null - 2>&1 | tail -5
```

**作用**：不看日志，直接确认「流能不能解出来、分辨率/帧率对不对」。
**为什么用 `-f null -`**：本机无 `ffplay`，`ffmpeg -f null` 只解码不显示，尾部会打印 `frame= N fps= ...`；有没有绿斑/花屏则拉到 PC 上用 VLC / `ffplay` 看（P0-13 就是这样肉眼确认的）。
**为什么加 `-rtsp_transport tcp`**：UDP 在板端本机回环上偶发丢包，会让「花屏」结论失真。

---

## 6. 自检脚本（无硬件依赖）

```bash
bash scripts/tests/run_t5_builder.sh     | tail -3   # 拓扑/装配校验，期望 passes=64 fails=0 skips=2
bash scripts/tests/run_t3_node_json.sh   | tail -3   # config/node.json 结构，期望 passes=104 fails=0 skips=0
bash scripts/tests/run_t2_node_config.sh | tail -3   # v1 解析 / v0 适配，期望 ALL PASS (fails=0)
bash scripts/tests/run_t1_zlm_rtsp_key.sh | tail -3  # ZLM RTSP key 校验（涉及外部服务，按需跑）
```

**目录里实际只有 t1 / t2 / t3 / t5 四套**（`ls scripts/tests/`，无 `t4`，编号是历史跳号）。
**作用**：改完配置/构建器后**先跑这几套**——纯 CPU、不需要摄像头与 NPU，1 秒出结果，能在跑真链路前挡掉大部分回归。
**为什么看 `tail -3`**：脚本结尾固定打印 `passes=N fails=N skips=N`，刷屏的中间行没有价值。

---

## 7. Git（提交 / 校验 / 推送）

今晚整套走下来就这几条，按顺序用：

### 7.1 看待办与分支同步状态

```bash
git status -sb                 # 第一行 ## rk3588...origin/rk3588（不带 [ahead/behind] 才算同步）
git --no-pager log --oneline -5
git --no-pager diff --stat     # 改了哪些文件、各几行
git --no-pager diff --cached --name-only   # 暂存区里到底有什么
```

**为什么 `--no-pager`**：默认会调 `less` 进交互分页，非交互环境里直接卡死。

### 7.2 暂存 + 提交（多行中文消息用文件）

```bash
git add README.md docs/ops/COMMANDS.md docs/HANDOFF.md
git commit -F /tmp/commit_msg.txt
```

**为什么 `-F <file>`**：多行中文消息 + 反引号/引号很多，写在文件里可逐条核对，避免 shell 转义破坏格式。
**踩过的坑**：文件已被 `git rm` 或本来就不存在时 `git add <path>` 会 `fatal: pathspec ... did not match any files` —— 只 add 真实存在的路径，删除另用 `git rm`。

提交消息文件长这样（本仓库约定：`type: 一句话` + 正文写「改了什么/为什么/验证」）：

```text
docs: 新增 docs/ops/COMMANDS.md 常用命令手册

- 归档今晚实测通过的命令：构建 / 运行 / 看内存 / 临时探针 / 进程端口 / git
- 每条给「作用 + 为什么这么写 + 实测输出」，标注 ✅ 实测项
- HANDOFF §0.1 跳读表、§1.4 文档清单、README §6 增加入口
```

### 7.3 推送 + 校验（远端到底有没有这笔）

```bash
git push origin rk3588
git rev-list --left-right --count HEAD...origin/rk3588   # 期望输出 "0	0"
git --no-pager show --stat --oneline HEAD
git ls-remote --heads origin rk3588                      # 绕过本地缓存，看远端真实指针
git rev-parse --short HEAD
```

**为什么还要 `ls-remote`**：`origin/rk3588` 是本地缓存的远端引用，`git fetch` 不及时会给你过时结论；`ls-remote` 直接问服务器。
**踩过的坑**：`git rev-parse --short origin/rk3588` 偶尔报 `Needed a single revision` —— 用全名 `refs/remotes/origin/rk3588` 即可。

### 7.4 确认一笔提交「有没有动代码」

```bash
git --no-pager show --name-only --format='' HEAD | grep -E '^(src|cmake|config|scripts)/' || echo '纯文档提交'
```

**作用**：决定「要不要重新编译 / 要不要补 TESTLOG」。
**为什么重要**：纯 `docs:` 提交不需要重编，但**必须**在回复里说清「最后一次构建仍对应哪个代码提交」，否则「已通过」失去意义（HANDOFF 硬约束 6 / 8）。

### 7.5 红线（来自 HANDOFF §2.1，命令层面）

```bash
# ✗ git push --force   ✗ git reset --hard   ✗ 提交 out/ build/ install/ 3rdparty/ .vscode/
git check-ignore -v out/bin/testnode     # 确认忽略规则生效
```

---

## 8. 文档一致性体检

改完文档/代码后，确认「全仓还有没有旧说法」：

```bash
# 1) 某条结论在哪些文件出现（限宽防刷屏）
grep -rn 'P0-5' README.md docs/*.md .clinerules/*.md AGENTS.md | cut -c1-165

# 2) 只数次数（0 = 已经清干净）
grep -rc '14 项' README.md docs/*.md | grep -v ':0'

# 3) 抓某一小节全文
awk '/^### 4\.1/,/^### 4\.2/' docs/STATUS.md

# 4) 找「旧校准点」残留（换基线后必查）
grep -rn '114c94d' README.md docs/*.md .clinerules/*.md AGENTS.md | cut -c1-165
```

**结论优先级（冲突时以左为真）**：代码 > `docs/STATUS.md` > `docs/ARCHITECTURE.md` > `docs/HANDOFF.md` > `README.md`。
**为什么重要**：今晚 P0-5 就是典型 —— 代码里引用计数链完整（`acquire→addUser` / `release→subUser`），README 却写着「未实现」，属于「文档落后于代码」，不改会让下一个人白干一天。

---

## 9. 踩过的坑速查表

| 现象 | 真正原因 | 正确做法 |
| --- | --- | --- |
| `cat: /tmp/x.txt: input file is output file` | 同一个文件既当 `>` 重定向目标又当 `cat` 参数 | 先 `{ ... } > /tmp/x.txt` 落盘，**另一条命令**再 `cat` |
| 命令退出码 1，但明明成功了 | 管道最后一环（`grep`/`tail`）的退出码 | 看 `${PIPESTATUS[0]}`，或先落盘再判 RC |
| `timeout` 返回 124 | 没加 `--preserve-status`，`timeout` 自己超时退出 | `timeout --preserve-status -s INT 10 ...`，看程序自身 RC |
| 配置文件读不到 | 构建目录被设为 cwd（`make run*` 与 `run.sh` 都是 `cd out/`） | 传**绝对路径**：`make run-node CFG="$PWD/config/node.json"` |
| `error while loading shared libraries` | 只有 `-L`（编译期），没设运行期搜索路径 | `LD_LIBRARY_PATH="$PWD/out/lib:$PWD/install/rk3588/lib"` |
| `undefined reference to ...` | `-l` 写在源文件前面（`--as-needed`） | 源文件在前、`-lcommon_api` 在后 |
| heredoc 里的 `$` 被吃掉 | `<<EOF` 没加引号 | 用 `<<'EOF'` |
| `git add` 报 `pathspec ... did not match` | 路径不存在 / 已删除 | 只 add 存在的路径；删除用 `git rm` |
| `git rev-parse origin/rk3588` 报 `Needed a single revision` | 短名歧义 | 用全名 `refs/remotes/origin/rk3588` |
| `top` 卡住不动 | 默认交互界面 | `top -b -n1` |
| ASAN 版内存数字比正常版大 | redzone / 隔离区开销 | ASAN 判错误，非 ASAN 判占用 |
| `use of deleted function` 编译失败 | 这是**预期**（P0-10 禁拷贝） | 反证成功，不是环境坏了 |
| 反例探针退出码 1 | ASAN 检测到 double-free 后主动 abort | 反例探针**就要**非 0；`RC=0` 说明反例没生效 |
| 查「泄漏」查到假象 | 之前用过 `kill -9`，`FreeMem()` 没跑 | 只用 `kill -INT` / `Ctrl+C` |
| 找不到 `t4` 测试 | 编号历史跳号，本就没有 | 实际四套：t1 / t2 / t3 / t5 |
| `/usr/local/include/rockchip/im2d.h` 存在，但 `cat` 出来是空的 | 该目录下 **17 个 RGA 头全是 0 字节空壳**（`rga.h` / `im2d*.h` / `Rga*.h`），只有 MPP 头正常；`find_path` 只要文件存在就成功，于是 `FindRGA.cmake` 误判 `RGA_FOUND` | 显式用 `-I 3rdparty/rk3588/librga-1.9.1/include`；CMake 里补内容校验（`file(SIZE)` > 0 或试编一个 `#include "im2d.h"`） |
| RGA 报 `Unsupported to scaling more than 1/16 ~ 16 times, src[w,h]=[64,32], dst[w,h]=[1280,720]` | `imblend()` **没有 rect 参数**，按整块 dst 算缩放比（64×32 → 1280×720 = 20 倍） | 改用 `improcess(src, dst, pat, srect, drect, prect, -1, nullptr, nullptr, IM_SYNC\|IM_ALPHA_BLEND_SRC_OVER)` 显式指定区域 |
| RGA 只回 `RGA_BLIT fail: Invalid argument`，dump 里参数看着都对 | RGA3 不接受**矩形宽 < 68**（实测宽 64 / 66 失败、68 成功；高度无此下限，16 高可过；面积无关） | 窄条 / 小图标走 CPU 实现；别去查 cache / 物理地址 / 格式，方向错了 |
| RGA 报 `src, Error yuv not aligned` | 矩形宽为奇数（实测 127×64），NV12 要求偶数宽 | 宽高取偶数（4 对齐更稳） |
| `imblend` / `improcess` 返回 success，但画面颜色明显不对 | 混合的通道顺序 / alpha 语义与"ARGB8888 = A<<24\|R<<16"的假设不一致（实测 `Y(0,0)=205` 期望 ~70、`UV=(110,25)` 期望 ~(109,184)） | 先用单通道纯色逐通道标定再上 blend；未标定就不要用，叠字走 CPU |

---

## 10. 一屏自检（复制粘贴）

```bash
cd /mnt/nvme/workspace/EdgeVisionService && \
  git status -sb && \
  make -j"$(nproc)" 2>&1 | tail -2 && \
  make run && \
  bash scripts/tests/run_t5_builder.sh | tail -1 && \
  echo "=== 全部自检结束 ==="
```

期望：分支行无 `[ahead/behind]`、`[100%] Built target ...`、`EdgeVisionService start`、`passes=64 fails=0`。

带硬件的主链路（有 `/dev/video0` 时；`testnode` 需先取消 `src/main/CMakeLists.txt` 的 `# add_subdirectory(test)`）：

```bash
timeout --preserve-status -s INT 20 make run-node CFG="$PWD/config/node.json"; echo "EXIT=$?"
pgrep testnode || echo '无残留'
```

---

## 11. 崩溃现场：报告 / core / 符号化

> 代码：`src/common/crash/crash_handler.{h,cpp}`（信号 handler + `std::terminate` handler）
> 脚本：`scripts/coredump/`（体检 / 开 core / 符号化 / 解析 core / 归档改名 / 抓活进程 / systemd 单元）
> **完整流程、core 命名机制、抓不到的原因（按链路 7 段 27 条）见 [CRASH_DUMP.md](CRASH_DUMP.md)**；踩过的坑见 [BUG_CASES.md](../history/BUG_CASES.md) §2.12

### 11.1 一次性打开系统侧 core（root，两台机器都要做）

```bash
sudo scripts/coredump/enable_coredump.sh
cat /proc/sys/kernel/core_pattern      # 期望 /var/lib/evs/coredump/core.%e.%p.%t.sig%s
ulimit -c                              # 期望 unlimited
scripts/coredump/check_coredump_env.sh     # 逐项 PASS/WARN/FAIL 体检（只读，不需要 root）
```
- `core_pattern` 必须是**绝对路径**（或以 `|` 开头的管道），相对路径会落到进程 cwd —— 服务化后 cwd 常是 `/`，写不进去就等于没抓。
- **systemd 服务不读 `/etc/security/limits.d`**，必须 `LimitCORE=infinity`（见 `scripts/deploy/evs-edgevision.service`），否则 `RLIMIT_CORE=0`，`core_pattern` 设了也不落盘。
- 容器里改 `core_pattern` 无效：它是 host 级参数。

### 11.2 崩了先看什么

```bash
ls -lt out/crash | head                # 报告：crash_<app>_<pid>_<epoch>_sig<N>.log
ls -lt /var/lib/evs/coredump | head    # core：core.<app>.<pid>.<epoch>（本机未装该脚本时是 ./core.<pid>）
```
报告已经给的：程序 / exe / pid / tid / UTC 时间 / 信号+描述 / `si_code`+故障地址 / `pc sp lr fp` / 业务 tag / 调用栈。**先看 tag**（例如 `config=../config/node.json`）确认是哪份配置的现场。

### 11.3 报告地址 → 函数 + 文件:行号

```bash
scripts/coredump/symbolize_report.sh out/crash/crash_EdgeVisionService_75879_1794659287_sig3.log
# 报告里是运行时相对路径（./bin/xxx、./lib/xxx.so），不在同一 cwd 时给部署根：
EVS_MODROOT=/opt/EdgeVisionService scripts/coredump/symbolize_report.sh <报告>
```
输出形如：

```
./bin/EdgeVisionService(main+0x574)[0x558f2c2574]
    -> main /mnt/.../src/main/main.cpp:129
```
**语义要点**：`backtrace_symbols` 的「模块(符号+0x偏移)」里偏移是**符号内偏移**，脚本先用 `nm` 查符号 vaddr 再加；「模块(+0x偏移)」才是模块内偏移（PIE 下等于文件 vaddr）。搞错这两种会得到 `?? ??:0`。

### 11.4 上 core 看变量（**只有 `-g` 编译才有**）

```bash
scripts/coredump/analyze_core.sh out/core.75879 out/bin/EdgeVisionService
# → out/core.75879.gdb.txt：info threads + thread apply all bt full + 寄存器 + 已加载 .so
```
交互式：

```bash
gdb -q out/bin/EdgeVisionService out/core.75879
(gdb) info threads                 # 哪个线程崩的
(gdb) thread apply all bt          # 全线程栈（卡死类问题靠这个）
(gdb) frame 3                      # 切到崩溃帧（前 2 帧是 handler 与 sigreturn）
(gdb) info args                    # 函数入参
(gdb) info locals                  # 局部变量
(gdb) p *this                      # 对象成员
(gdb) p frame->width               # 具体字段
```
- `bt full` 能打印变量，靠的是 DWARF（`-g`），所以**没 `-g` 就只有 `??`**；本项目默认 `RelWithDebInfo` 已带 `-g`。
- 变量显示 `<optimized out>` = 被 `-O2` 优化掉：`make BUILD=out-dbg CMAKE_FLAGS="-DCMAKE_BUILD_TYPE=Debug"` 重编复现。
- core 必须配**当时那份二进制与 `.so`**；重编后地址错位，用 `readelf -n` 记 build-id 归档。

### 11.5 进程没崩但卡住 / 无输出 / CPU 100%

```bash
scripts/coredump/gcore_now.sh EdgeVisionService     # 按名字找 pid → /proc 快照 + gcore + 解析
scripts/coredump/gcore_now.sh 12345 crash           # 按 pid，输出到 crash/
kill -QUIT <pid>                                # 也走同一 handler（装了 SIGQUIT）
```
- `wchan` 判据：`futex` / `do_wait` = 卡锁；`poll` / `epoll` = 等 IO；`nanosleep` = 在自己的循环里。
- `gcore` 用 `ptrace` attach，**不杀进程**，但 attach 期间目标进程短暂暂停。

### 11.6 主动触发一次（自动化验证用）

```bash
cd out && LD_LIBRARY_PATH="lib:../install/rk3588/lib" \
  timeout --preserve-status -s QUIT 6 ./bin/EdgeVisionService ../config/node.json
echo "RC=$?"    # 131 = 128+3(SIGQUIT)：handler 生效 + core 落盘
```

### 11.7 先体检「这台机器现在能不能抓到 core」（只读，不需要 root）

```bash
scripts/coredump/check_coredump_env.sh
echo "RC=$?"      # 0 = 没有 FAIL；1 = 有 FAIL（当前一定抓不到）
```

覆盖 13 项：`core_pattern`（绝对/相对/管道、`%p`/`%t`/`%e`）、目录存在与可写、盘类型（tmpfs 会吃内存）、
`ulimit -c` 软/硬、`ulimit -f`、`suid_dumpable`、AppArmor/SELinux、`coredump_filter`、`coredumpctl` 接管、
容器、`sysctl.d`/`limits.d` 持久化、已装 unit 的 `LimitCORE`。每项都带「怎么修」。

✅ 实测（2026-11-14 本机）：`PASS=5 WARN=9 FAIL=0`，结论是"现在能抓，但 systemd 上线前要跑 `enable_coredump.sh`"。

### 11.8 把 core 归档成「名字里带函数名 / 文件 / 行号」的目录

内核只能给 core 起 `core.<app>.<pid>.<epoch>.sig<N>` 这类名字（它不认识用户态符号，
详见 [CRASH_DUMP.md §2](CRASH_DUMP.md)）。要看得到函数名的归档，用这条：

```bash
scripts/coredump/collect_core.sh out/core.75879 out/bin/EdgeVisionService
# 不给 exe 也行：脚本会按 core 名里的程序段（%e，≤16 字节）去 out/bin、build/bin 里找

scripts/coredump/collect_core.sh /var/lib/evs/coredump/core.* out/bin/EdgeVisionService -o /data/archive
```

产物（✅ 实测 2026-11-14）：

```
out/archive/core.EdgeVisionService.75879.1794659287.sig3.std::this_thread::sleep_for_...@this_thread_sleep.h:82/
    core                       硬链接（同 inode，不占双倍空间；Link count=2）
    meta.txt                   exe + build-id + pid/epoch/信号 + core_pattern + 内核 + ulimit
    crash_thread_bt.txt        崩溃线程栈（tgkill 保证是崩溃现场）
    backtrace_all_threads.txt  24 线程全栈（20 KB）
    sharedlibs.txt             .so 加载情况（排查 ?? 帧）
    report.log                 同 pid 的 crash_*.log（自动配对）
```

- 报告配对规则：按 `crash_*_<pid>_*_sig*.log` 找最新一份（pid 相同即同一次崩溃）。
- 信号号缺失时（`core_pattern=core` 的 `core.75255` 没 `sig` 段）从报告名 `_sigN.log` 反推。
- `-o` 指定归档根；默认 `<core 所在目录>/archive`。

## 12. 部署 / 开机自启（systemd）

> 结论：**只改 systemd unit 一处**。不要用 `rc.local` / `crontab @reboot`：
> 它们不设 `LimitCORE`（服务模式 hard=0 → core 永远抓不到）、cwd 不可控、没有重启策略、日志也不进 journal。

### 12.1 安装单元（一次性，需要 root）

```bash
sudo cp scripts/deploy/evs-edgevision.service /etc/systemd/system/
sudo systemctl daemon-reload
systemctl cat evs-edgevision | head -30      # 复核路径 / User / LimitCORE
```

单元里必须对的六行（其余见文件内注释）：

```ini
[Service]
WorkingDirectory=/mnt/nvme/workspace/EdgeVisionService      # 直接跑二进制时 core 相对名的落点（start.sh 会 cd 到 out/）
ExecStart=/mnt/nvme/workspace/EdgeVisionService/scripts/run/out/start.sh
Restart=on-failure
RestartSec=3
LimitCORE=infinity                                         # 不写这行 = 服务模式永远没有 core
# User=orangepi                                            # 需要时改成实际用户
```

- `LimitCORE=infinity` 是**唯一**能让服务模式抓到 core 的方式：`/etc/security/limits.d` 对 systemd 无效，
  PID1 的 hard limit 通常是 0，进程自己无权抬高（`setrlimit` 只能抬到 hard）。
- 用 `start.sh` 当 `ExecStart`：`ulimit -c` / `LD_LIBRARY_PATH` / `EVS_CRASH_DIR` 都在脚本里，
  单元不用重复写（`Environment=` 也不做变量展开，写 `$PATH` 之类不会生效）。

### 12.2 开机自启 / 立即启动 / 状态

```bash
sudo systemctl enable --now evs-edgevision     # 开机自启 + 现在就起
systemctl status evs-edgevision --no-pager
systemctl is-enabled evs-edgevision            # 期望 enabled
systemctl show -p LimitCORE -p WorkingDirectory evs-edgevision
```

### 12.3 日志与「core 到底能不能落」

```bash
journalctl -u evs-edgevision -f                 # 实时日志（crash 报告同时打到 stderr → journal）
PID=$(systemctl show -p MainPID --value evs-edgevision)
grep -i 'core file' "/proc/$PID/limits"         # 期望 hard = unlimited
cat /proc/sys/kernel/core_pattern               # 期望 /var/lib/evs/coredump/core.%e.%p.%t.sig%s
ls -lt /var/lib/evs/coredump | head             # core 产物
```

### 12.4 替代方案（不推荐，只说明差别）

| 方式 | 缺点 |
| --- | --- |
| `rc.local` | 不设 `LimitCORE`（core 抓不到）、cwd=`/`、无重启、日志混进 syslog |
| `crontab @reboot` | 同上，且 crontab 环境极简（`PATH`/`HOME` 可能缺），失败无感 |
| `~/.bashrc` / `profile` | 只对登录 shell 生效，无人登录就不启动 |

临时手工启动用 `scripts/run/out/start.sh`；要服务化就按 §12.1。

---

## 维护规则

1. **只收验证过的命令**：新加一条，先在本机跑通，再连同输出一起写进来并标 ✅ 实测 + 日期。
2. **必须写「作用」和「为什么这么写」**：单纯贴命令，下次还是要查 man。
3. **本文只放操作手法**：结论放 `STATUS.md`，证据放 `TESTLOG.md`，规矩放 `HANDOFF.md`，原理放 `BUG_CASES.md`。
4. 命令的执行环境变了（换板子 / 换工具链），先改 [HANDOFF.md](../HANDOFF.md) §1.3，再回来更新本文的路径与版本相关部分。




