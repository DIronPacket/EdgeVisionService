# EdgeVisionService

> 边缘视觉服务原型：**采集 → 统一内存池 → 节点流水线 → 硬件/软件编码 → RTSP 推流**。
> 以 RK3588（MPP / V4L2 / RGA）为主，x86_64（FFmpeg）作为开发调试目标。
> 当前阶段：原型链路已打通，尚未产品化；详细现状见 [docs/STATUS.md](docs/STATUS.md)。

## 目录

1. [现在能做什么](#1-现在能做什么)
2. [架构一图流](#2-架构一图流)
3. [快速开始](#3-快速开始)
4. [当前状态](#4-当前状态)
5. [路线图](#5-路线图)
6. [文档导航](#6-文档导航)
7. [开发约定](#7-开发约定)

## 1. 现在能做什么

- **采集**：`FfmpegCapture` 支持本地文件、摄像头、RTSP/RTMP/HTTP 输入；`V4l2Capture` 面向 RK 的 `/dev/video*`。两者统一输出 NV12 到内存池。
- **内存池与流水线**：`MemManager::MemInit(50)` 预建 50 个 1920×1080 NV12 块；`ViNode → PushNode` 通过深度 20 的 `MemQueue` 连接，队列满时丢弃最老帧。
- **硬件编码**：`MppEncoder` 支持 H.264/H.265，NV12 输入、128 字节 stride 对齐、固定 QP 33、GOP 150，回调输出裸流。
- **RTSP 推流**：通过 ZLMediaKit C API 启动 RTSP 服务、创建流并送 H.264/H.265，默认地址 `rtsp://<ip>:8550/live/ch0`。
- **节点编排**：`PipelineNode` 提供 `link_to` / `push_input_pipe` / `push_data`，`ViNode` 与 `PushNode` 的线程启停可运行。

当前限制：

- x86 本机构建会缺 `install/x86_64` 依赖；FFmpeg 软编/软解尚未完成。
- RGA 预处理、OSD、录像、配置化、统一日志/指标/健康检查均未落地。
- 内存池为 `MemManager`（`MemPool` + `MemQueue`），多下游引用计数未完成。

> 模块完成度、P0–P2 问题与优化建议见 [docs/STATUS.md](docs/STATUS.md)。

## 2. 架构一图流

```text
V4L2 / 文件 / RTSP 源
        │ NV12
        ▼
ViNode（独立线程：采集 + 30fps 节流）
        │ MemQueue（深度 20，drop-oldest）
        ▼
PushNode（独立线程：MPP H.264/H.265 编码）
        │ 编码裸流
        ▼
ZLMediaKit RTSP Server
rtsp://<ip>:8550/live/ch0
```

当前是**单消费者链路**；RGA/OSD/录像/AI 并行后需要块级引用计数支持一源多下游。分层、内存模型、线程/背压和平台接口详见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

## 3. 快速开始

### 3.1 依赖

| 依赖 | 期望位置 | 当前仓库状态 |
| --- | --- | --- |
| FFmpeg 5.x（头文件 + `.so`） | `install/<platform>/include`、`install/<platform>/lib` | 仅 `install/rk3588` |
| Rockchip MPP | 同上 | `install/rk3588/lib/librockchip_mpp.so` |
| ZLMediaKit（`libmk_api.so`） | 同上 | `install/rk3588/lib/libmk_api.so` |
| RGA | 同上 | `install/rk3588/librga.so`，代码尚未使用 |
| 交叉工具链 | `cmake/rk3588.toolchain.cmake` | 路径需按本机实际安装修改 |

### 3.2 x86_64 本机构建

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
```

> 当前会在配置阶段失败：`FindFFMPEG.cmake` 只查找 `install/x86_64`，仓库只有 `install/rk3588`。需补 x86 依赖，或增加 `pkg-config` fallback；构建系统问题见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

### 3.3 RK3588 交叉编译

```bash
cmake -S . -B build-rk \
      -DCMAKE_TOOLCHAIN_FILE=cmake/rk3588.toolchain.cmake
cmake --build build-rk -j"$(nproc)"
```

注意：

- 工具链默认写死 `/usr/aarch64-linux-gnu/bin/aarch64-linux-gnu-g++`，需改成实际编译器路径（例如 `/usr/bin/aarch64-linux-gnu-g++-11`）。
- 当前未设置 `CMAKE_SYSROOT`；板端 sysroot 与 glibc 版本需先确认。
- `build/` 里的旧缓存来自其他机器，建议清理后重建。
- 交叉编译的现状与待办见 [docs/ROADMAP.md](docs/ROADMAP.md) 的 M1。

### 3.4 运行

```bash
./build/run.sh testnode
```

`build/run.sh` 会切到 `build`、导出 `LD_LIBRARY_PATH=build/lib:install/rk3588/lib`，再执行 `build/bin/<name>`。当前 `EdgeVisionService`（`src/main/main.cpp`）只打印一行字符串，真实可跑的主链路在测试程序 `testnode`（`src/main/test/main_node.cpp`）。

## 4. 当前状态

| 模块 | 状态 | 说明 |
| --- | --- | --- |
| 构建/平台探测 | 🟡 60% | 平台探测与 `Find*.cmake` 齐全；x86 缺依赖即失败；RTSP 未接入构建；工具链路径待确认 |
| 采集 | 🟡 65–75% | FFmpeg 文件/摄像头/RTSP 输入可用；V4L2 无 `poll`、参数硬编码 |
| 编码 | 🟡 70% | MPP H.264/H.265 可跑；x86 软编未完成；每帧 `memcpy`，无 EOS flush |
| RTSP 推流 | 🟡 65% | 单路可演示；`stop` key 不匹配、空指针解引用等问题待修 |
| 内存/流水线 | 🟡 60–65% | `MemManager`（`MemPool`/`MemQueue`）在用；多下游引用计数未完成 |
| 测试/文档 | 🟡 25–30% | 仅手工测试，无 CTest/CI；README/ARCHITECTURE/STATUS/ROADMAP 已建立 |

> 完整模块表、P0/P1/P2 明细与优化建议见 [docs/STATUS.md](docs/STATUS.md)。

## 5. 路线图

| 阶段 | 周期 | 重点 | 退出标准 |
| --- | --- | --- | --- |
| 近期稳定性 MVP | 6 周 | 构建收敛、内存模型、P0、多通道、服务化 | 4 路 1080p30 连续 2 小时；72h 无崩溃/无泄漏 |
| 中期扩展 | 12 周 | H.264、RGA、OSD、录像、交叉编译、Docker、OTA | 4 路 24 小时；OTA 连续升级/回滚通过 |
| 前置确认 | 第 0 周 | 板端 OS/glibc、分区、U-Boot、录像存储、CI 资源 | 产出《板端基线表》，无未知项 |

> 详细 WBS、12 周排期、技术方案、验收指标、风险矩阵与待确认项见 [docs/ROADMAP.md](docs/ROADMAP.md)。

## 6. 文档导航

| 想了解什么 | 看哪里 |
| --- | --- |
| 当前架构、目录、数据流、内存/线程模型、平台接口 | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) |
| 模块完成度、已实现/占位、P0–P2、优化建议、测试现状 | [docs/STATUS.md](docs/STATUS.md) |
| 未来 WBS、排期、技术方案、验收指标、风险与待确认项 | [docs/ROADMAP.md](docs/ROADMAP.md) |
| 源码入口 | `src/`；真实主链路在 `src/main/test/main_node.cpp` |

## 7. 开发约定

- 新增节点继承统一的 `PipelineNode`：`push_input()` 只入队，耗时处理放 `run()`，处理完 `dispatch_to_children()`，终点 `release_block()`。
- 配置统一走 `config/channels.json`，启动时校验分辨率/池容量/端口/app/stream 唯一性，失败拒绝启动。
- 目录与命名统一小写，修正 `moudle` → `module`；头文件 guard 使用 `EVS_<MODULE>_<FILE>_H`。
- 提交信息：`<模块>: <动词> <对象>`，例如 `pipeline: fix block leak on capture error`。
- 合并前：x86 构建通过、单测通过、无新增告警；RK 改动需交叉编译通过并在板端跑 30 分钟。

> 节点扩展示例、配置 schema、命名规范与质量门禁详见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

---

最后更新：2026-11-04；适用分支：`rk3588`。
