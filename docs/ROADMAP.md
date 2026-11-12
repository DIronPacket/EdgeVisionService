# EdgeVisionService 路线图

> 本文档描述 EdgeVisionService 的未来扩展计划：H.264 编码推流、RGA 预处理、OSD、录像、Ubuntu 交叉编译、Docker 容器化与 OTA。
> 当前架构与扩展方式见 [ARCHITECTURE.md](ARCHITECTURE.md)；模块完成度与已知问题见 [STATUS.md](STATUS.md)；项目入口见 [README](../README.md)。本文档侧重目标、方案与排期。
> **进程形态演进（单进程 → 媒体 / 通信 / 升级 三进程 + 共享通信对象）单独记录在 [DESIGN_PROCESS_SPLIT.md](DESIGN_PROCESS_SPLIT.md)**（当前状态：规划，尚未落地）。
> 最后更新：2026-11-04；适用分支：`rk3588`。

## 1. 背景与目标

本文 6.1 的 6 周计划面向“稳定性 MVP”：修构建、收敛内存模型、补多通道与 P0 问题。本路线图是在其之上的功能扩展与交付计划，建议在 MVP 收敛后串行执行；若人力允许，交叉编译与 H.264 可提前并行。

目标：

1. **编码可选**：H.264 / H.265 按通道配置，H.264 优先落地。
2. **预处理硬件化**：用 RGA 完成缩放、裁剪、NV12/RGB 转换，替代 `sws_scale`。
3. **OSD 可配置**：时间、通道名、分辨率/帧率/码率，预留 AI 检测框。
4. **录像可检索**：基于编码裸流封装 MP4/fMP4，按关键帧分段、按天/容量保留、掉电可恢复。
5. **构建可交付**：在 Ubuntu 本机交叉编译 aarch64 产物，打包部署到 RK3588S，不再依赖板端编译。
6. **部署容器化**：Docker + Compose 管理服务、设备、网络、数据卷与日志。
7. **升级可回滚**：远程签名升级、健康检查、自动回滚、灰度发布。

## 2. 范围与边界

| 类别 | 内容 |
| --- | --- |
| 本期范围 | H.264/H.265 参数化、RGA 预处理、OSD、录像、交叉编译、Docker、应用级/容器级 OTA、测试与交付 |
| 暂不包含 | AI 模型训练、Web 管理界面、云端平台、内核/驱动开发、音频编码（待确认） |
| 待确认后可纳入 | 系统级 A/B（RAUC/SWUpdate）、H.264+H.265 双流、RTSP 鉴权、NFS/对象存储录像 |
| 前置条件 | 板端 OS/glibc/分区/U-Boot 信息；扇出拓扑 + 多分支用例（`MemBlock` 引用计数本身已完整，P0-5 假阳性）；`install/rk3588` 依赖可用 |

## 3. 现状与差距

| 能力 | 当前事实 | 需要补的内容 |
| --- | --- | --- |
| H.264 | `MppEncoder` 已支持 `MPP_VIDEO_CodingAVC`；`config/push.json` 的 `codec` 已可切到 h264（编码器与 ZLM 声明同步，提交 `283bab5`），但未做实机 SPS/PPS 与播放器兼容性验证 | h264 板端出流验证，约 1–2 天 |
| RGA | `install/rk3588` 已有 `librga.so` 与 `rockchip/im2d*.h`；`FindRGA.cmake` 存在但从未被引用 | 新建 `processor` 模块，扩展 `ProcessorInterface` |
| OSD | 无 OSD 代码；`install` 中没有 FreeType/OpenCV | 引入 `stb_truetype` 渲染字体，RGA 叠加，先做板端 PoC |
| 录像 | FFmpeg `libavformat` 已就绪；ZLM 头文件含 `mk_recorder.h`；项目内无录像节点 | 增加 `EncodedPacket` 分发与 MP4 封装节点 |
| 交叉编译 | 工具链指向不存在的 `/usr/aarch64-linux-gnu/bin/...`；未设置 `CMAKE_SYSROOT` | 修工具链、准备板端 sysroot、补 x86 模拟与 CI |
| 依赖 ABI | `libavcodec.so` 最高要求 GLIBC_2.35；`libmk_api.so` 依赖 OpenSSL 3 | 确认板端 glibc ≥2.35，运行环境带 `libssl3` |
| Docker | 仓库无 Dockerfile/compose，本机也未安装 Docker | 多阶段镜像、设备映射、数据卷、健康检查 |
| OTA | 无升级代理、无签名清单、无回滚机制 | 应用 A/B 或容器镜像 OTA，后续再评估系统 A/B |

## 4. 目标数据流

```text
Source（V4L2/FFmpeg）
   │ NV12
   ▼
Preprocess（RGA 缩放/裁剪/格式转换）
   │ NV12
   ▼
OsdNode（时间/通道名/AI 框，RGA 叠加）
   │ NV12
   ▼
Fanout（引用计数分发）
   ├──────────────┐
   ▼              ▼
Encoder H.264   Encoder H.265（可选）
   │ EncodedPacket
   ▼
Encoded Fanout
   ├──────────────┐
   ▼              ▼
RTSP Sink       Record Sink（MP4/fMP4）
```

设计约束：

1. **编码包只产生一次**：RTSP 与录像共享 `EncodedPacket`，不做二次编码。
2. **OSD 必须在编码前**：只叠加一次，直播与录像看到同一画面。
3. **多下游依赖扇出拓扑**：`MemBlock` 的 `user_cnt` 计数链路是完整的（P0-5 于 2026-11-10 核实为假阳性），但 `PipelineBuilder` 只连 `i→i+1`，多分支从未被触发——启用前先补多分支用例 + "下游只读 / 不就地改写"约定（[STATUS.md](STATUS.md) §4.2 第 24 条）。
4. **OSD 输出独立块**：不能原地修改与 AI/录像共享的源块。
5. **AI 节点后置**：AI 输出检测结果，OSD 只读取最新结果，二者不直接耦合。

## 5. 工作分解（WBS）

| ID | 工作流 | 关键任务 | 产出 | 人日 | 依赖 |
| --- | --- | --- | --- | --- | --- |
| A | 交叉编译与构建 | 修正 toolchain 路径；准备板端 sysroot；配置 `CMAKE_SYSROOT` / `CMAKE_FIND_ROOT_PATH`；显式化 `TARGET_PLATFORM`；x86 模拟构建；rpath 与打包；CI | 可用的 `rk3588.toolchain.cmake`、`scripts/build.sh`、`dist/` 发布包 | 8–10 | 板端信息 |
| B | 媒体框架收敛 | 统一到 `MemPool/MemBlock`；`refcnt` 多下游；`BlockQueue`（超时/关闭/丢弃策略）；节点状态机；配置加载 | 一套内存/队列/节点基类 | 10–12 | A |
| C | H.264 编码推流 | codec/码率/GOP/码控配置化；`PushNode` 去硬编码；ZLM 按 codec 初始化；SPS/PPS 验证；多通道 URL | H.264/H.265 可选，RTSP 可播 | 4–6 | B |
| D | RGA 预处理 | `RgaProcessor`：NV12 裁剪/缩放/格式转换；stride 与 dma-buf 处理；x86 fallback | `processor` 模块 + 单测 | 5–7 | A/B |
| E | OSD 叠加 | 字体渲染（`stb_truetype`）；文字/矩形/时间戳/通道名/AI 框；RGA blend；缓存与刷新策略 | `OsdNode` + 配置 | 6–8 | D |
| F | 录像 | `EncodedPacket` 分发；MP4/fMP4 封装；关键帧分段；文件命名；保留策略；磁盘水位；掉电恢复 | `RecordSinkNode` + 清理策略 | 7–9 | C |
| G | 服务化 + Docker | 多通道配置；日志/指标/健康检查；优雅退出；Dockerfile/compose；设备映射；host 网络；数据卷 | 容器镜像 + compose | 5–7 | B/C |
| H | OTA | 升级代理；签名清单；HTTPS 下载；应用 A/B 槽或容器镜像；健康检查/回滚；断电保护；状态上报 | `ota-agent` + 清单规范 | 10–14 | G 或系统 A/B |
| I | 测试与交付 | 单测；集成脚本；4 路 24h；72h soak；性能/温度；安全审查；文档 | 测试报告 + 交付包 | 8–10 | 全部 |

合计约 **63～83 人日**。按 2 人并行并预留联调、板端占用与返工，建议按 **12 周** 规划；3 人可压缩到 **8～9 周**。

## 6. 排期

### 6.1 近期稳定性 MVP（6 周）

> 编号说明：本节的 `MVP-x` 是稳定性阶段；6.2 的 `M0–M5` 是功能扩展阶段，两套编号相互独立。
> 假设：1～2 名熟悉 C++/RK 平台的开发，按 6 周一个迭代排；若只有 1 人，整体顺延至 10～12 周；若 3 人并行，可压缩到 5 周。

| 阶段 | 周期 | 目标 | 关键任务 | 交付/验收 |
| --- | --- | --- | --- | --- |
| MVP-1 基础收敛 | 第 1 周 | 能稳定构建、能跑通单链路 | 修 x86/交叉构建；定义 `BUILD_*` 开关；接入 `rtsp`/`common_api`；删除重复头与重复 ZLM 实现；确定唯一内存模型 | x86 与 RK 均 `cmake && build` 成功；`testnode` 连续 30 分钟不崩、无 ASan 报错 |
| MVP-2 内存与流水线加固 | 第 2 周 | 资源不泄漏、支持多下游 | 扇出拓扑 + 多分支用例（`user_cnt` 计数已完整）；所有错误路径归还；`BlockQueue` + 背压策略；节点状态机/优雅停止；每通道池 | 24 小时循环，池块数稳定；1 源 2 下游（编码 + 算法模拟）无脏读；停止 1s 内退出 |
| MVP-3 采集与编解码完善 | 第 3 周 | 1080p30 稳定采集编码 | V4L2 `poll`/参数回写；FFmpeg options/重连；MPP EOS/flush/码控；MPP 解码接回池；RGA 预处理 PoC | 1080p30 H.265 连续 2 小时；CPU/内存基线报告；解码输出正确 NV12 |
| MVP-4 推流与多通道 | 第 4 周 | 多路 RTSP 可看、可停 | 修 ZLM stop/key/返回值；每流锁；多通道配置；SPS/PPS 缓存；断流恢复 | 4 路 1080p30 同时推流 2 小时；`ffplay`/VLC 均可播放；停止后无残留 `mk_media`/线程 |
| MVP-5 服务化与可观测 | 第 5 周 | 从 demo 变成服务 | `main` 装配 + 配置文件 + 信号处理；统一日志；指标/健康检查；看门狗；错误恢复 | 配置文件启动多通道；`/metrics` 可查；kill -TERM 优雅退出；异常源可自恢复 |
| MVP-6 测试与交付 | 第 6 周 | 可交付基线 | CTest 单测；集成/压测脚本；72h soak；性能与稳定性报告；部署包与文档 | 核心模块单测通过；72h 无崩溃/无泄漏；交叉构建可复现；交付包含二进制、配置、脚本、说明 |

**第一周“立即行动”清单：**

1. 补齐 `install/x86_64`（或加 `pkg-config` fallback），让 x86 能配置构建。
2. 修复 `ViNode`/`PushNode` 错误路径不归还 `MemBlock` 的问题（P0 #3/#4）。
3. 修复 `zlm_rtsp_stop()` key 与 `out_code` 空指针（P0 #6/#7）。
4. 修复 `pipeline_api` 的 `rtsp/common_api` 链接关系，统一一份 `zlm_push_server`。
5. 确定内存模型并删除另一套同名头文件，避免后续开发继续分叉。

### 6.2 扩展计划（12 周）

> 在 6.1 完成后启动；交叉编译与 H.264 可在 MVP 期间并行预研。

| 阶段 | 时间 | 目标 | 主要任务 | 退出标准 |
| --- | --- | --- | --- | --- |
| **M0 事实确认** | 第 0 周（2–3 天） | 摸清板端约束 | 收集 OS/glibc/分区/U-Boot/Docker/设备节点；确认 OTA 范围；冻结配置 schema | 产出《板端基线表》，无未知项 |
| **M1 构建与框架** | 第 1–2 周 | 本机能交叉编译，H.264 先跑起来 | 修 toolchain + sysroot；x86 模拟；`MemPool/refcnt/BlockQueue`；H.264 参数化；RTSP 验证 | Ubuntu 一条命令出 aarch64 包；板端 H.264 1080p30 连续 2 小时；x86 文件源 → RTSP 可播 |
| **M2 RGA/OSD/录像** | 第 3–5 周 | 多下游链路成型 | `RgaProcessor`；OSD 字体与叠加；`EncodedPacket` 分发；MP4 分段与保留；多通道配置 | 2～4 路 1080p30，OSD 正确；RTSP 与录像同时工作；10 分钟分段可播；24 小时无泄漏 |
| **M3 服务化 + Docker** | 第 6–8 周 | 从程序变成可部署服务 | 配置/日志/指标/健康检查/优雅退出；Dockerfile + compose；设备映射；数据卷；镜像瘦身 | 容器内 4 路 24 小时；重启自恢复；`/healthz`、`/metrics` 可查；镜像 <300MB |
| **M4 OTA + 安全** | 第 9–11 周 | 可远程升级、可回滚 | 签名清单；下载/校验/切换/健康检查/回滚；应用 A/B 或容器镜像方案；断电测试 | 连续 20 次升级成功；10 次随机断电不变砖；回滚 <2 分钟；篡改包被拒 |
| **M5 压测与交付** | 第 12 周 | 可交付基线 | 72 小时 soak；4 路性能/温度；安全审查；文档；发布包 | 无崩溃/无泄漏；CPU/内存/温度达标；交付物齐全 |

依赖关系：

- **B 是所有新节点的前置**：没有扇出拓扑与多分支用例，RGA/OSD/录像/AI 并行会踩"就地改写同一块"的脏读（引用计数本身已完整，P0-5 假阳性）。
- **D 是 E 的前置**：OSD 复用 RGA 的缩放/格式转换能力。
- **C 是 F 的前置**：录像直接消费编码包，不重新编码。
- **G 是容器化 OTA 的前置**；若选择系统级 A/B，则 H 可与 G 并行，但必须先确认 U-Boot/分区可控。

## 7. 关键技术方案

### 7.1 H.264 编码推流

H.264 的主体能力已经存在，改动集中在“参数化”：

- `MppEncoder`：已支持 AVC/HEVC 切换；补齐 profile/level、GOP、码控模式（CBR/VBR/FIXQP）、SPS/PPS 重复发送，低延迟场景关闭 B 帧。
- `PushNode`：
  - 删除 `VIDEO_CodingH265`、帧率 `30`、码率 `500` 的硬编码，改为通道配置；
  - 每个编码器实例使用独立 PTS 基准（当前 `frame_index_` 是文件级全局，多通道会串）；
  - 编码器打开失败、采集失败必须归还 `MemBlock`（现有 P0 问题）。
- `zlm_rtsp_onekey_start()`：
  - 编码类型由配置传入；
  - `mk_media_init_video` 内部码率当前写死 2000000，需与编码器保持一致；
  - H.264 的 SPS/PPS 由 `mk_media_input_h264` 解析；H.265 已有 `zlm_hevc_send_param_set`，需要验证“播放器中途接入”时参数集是否完整。
- 同时输出 H.264 + H.265：使用两个编码器实例 + NV12 引用计数分发，**不要转码**；先确认芯片编码吞吐和业务是否真的需要。

> 建议把 H.264 作为 M1 的快速交付：先加一个临时配置项跑通，再纳入正式配置体系。

### 7.2 RGA 预处理与 OSD

- 使用 `im2d` API（`rockchip/im2d.h` 等头文件已齐全）：`imcvtcolor`、`imresize`、`imcrop`、`imblend`、`imfill`；旧 `rga.h` 仅作兼容。
- 需要处理：宽高偶数对齐、stride、NV12 UV 偏移、虚拟地址与 dma-buf、不同 RGA 版本的差异。
- 建议把 `ProcessorInterface` 从单一 `process(src,dst)` 扩展为：
  - `resize/crop/convert`（预处理）；
  - `blend(overlay, dst)`（OSD）；
  - `fill(rect, color)`（遮罩/背景）。
- **字体方案**：`install` 中没有 FreeType/OpenCV，推荐引入 `stb_truetype.h`（单头、无额外依赖），把文字栅格化为 RGBA 位图，再用 RGA 叠加。
- **必须先做板端 PoC**：验证 RGA 能否直接把 RGBA 叠加到 NV12。若当前版本不支持，降级为“NV12→RGB→叠加→NV12”或“仅对 OSD 小区域做 CPU 叠加”，并测量单帧耗时。
- OSD 分两层：静态层（通道名/位置）+ 动态层（时间、帧率、码率、AI 框）；动态层 1Hz 刷新，避免每帧重新栅格化。
- 无 OSD 需求的通道直接 pass-through，保持零拷贝。

### 7.3 录像

两条路线，建议以自研为主、ZLM 录像做兜底：

| 方案 | 优点 | 缺点 | 建议 |
| --- | --- | --- | --- |
| 自研 FFmpeg muxer | 完全可控；与 `EncodedPacket` 零拷贝；易做分段/保留/索引 | 需处理 extradata、关键帧、掉电 | **主方案** |
| ZLM `mk_recorder` | 头文件已具备，接入快 | 多一次流复制；控制力弱；依赖 ZLM 配置 | MVP/兜底 |

自研要点：

- 从 `EncodedPacket` 取数据，`avformat_write_header` 后调用 `av_interleaved_write_frame`；
- **按关键帧切段**，文件名形如 `ch0_20261104_221500.mp4`；
- 先写 `.tmp`，`fsync` 后原子 rename；
- 掉电安全优先 **fMP4** 或“分段 + 启动修复”，不要长时间不落盘的普通 MP4；
- 保留策略：按天数 + 磁盘水位双阈值，删除最旧文件，并监控 inode；
- 容量估算：4 路 × 2Mbps ≈ 1MB/s ≈ **86GB/天**，7 天约 **600GB**，需先确认存储介质与保留周期。

### 7.4 Ubuntu 交叉编译

当前工具链文件有两个硬伤：

1. `TOOLCHAIN_ROOT=/usr/aarch64-linux-gnu`，但本机编译器实际在 `/usr/bin/aarch64-linux-gnu-g++-11`；
2. 未设置 `CMAKE_SYSROOT`，系统头文件/库可能混用主机或交叉包版本。

建议方案：

- **sysroot 取自板端**（最稳）：把板子的 `/lib`、`/usr/lib`、`/usr/include` 同步到 `sysroot/rk3588`（排除文档/locale），配置 `CMAKE_SYSROOT` 与 `CMAKE_FIND_ROOT_PATH`。
- 或使用 RK 官方 SDK/Buildroot 的 sysroot（若板端镜像由它构建）。
- 校验：`readelf -V` 确认没有引入高于板端 glibc 的符号；`file`/`readelf -h` 确认 aarch64；上板 `ldd` 检查依赖。
- 运行库：`libmk_api.so` 需要 OpenSSL 3，部署环境必须带 `libssl.so.3`/`libcrypto.so.3`。
- 打包：二进制 + 必要的 `install/rk3588/lib/*.so` + 配置，设置 `RPATH=$ORIGIN/../lib`，不依赖 `run.sh` 的 `LD_LIBRARY_PATH`。
- x86 侧：准备 `install/x86_64` 或增加 `pkg-config` fallback，让开发机可以跑“文件源 → 软编 → RTSP”的回归链路。
- CI：x86 构建 + 单测；aarch64 交叉编译（只编译）；板端 nightly 跑集成。

### 7.5 Docker 容器化

- 基础镜像：`ubuntu:22.04`（glibc 2.35，与 `libavcodec` 匹配），或 `debian:bookworm`（2.36）。
- 多阶段构建：Ubuntu 上交叉编译 → 运行阶段只拷贝二进制、必要 `.so`、配置，避免把 369MB 的 `libzlmediakit.a` 带进镜像。
- 设备与权限：
  - `/dev/video*`（V4L2）
  - `/dev/mpp_service`（MPP）
  - `/dev/rga`（RGA）
  - `/dev/dma_heap/*`（DMA-BUF，若使用）
  - 可选 `/dev/dri/*`（本地显示）
  - `--group-add video --group-add render`；非 root 用户；`no-new-privileges`。
- 网络：RTSP 建议 `network_mode: host`，避免 RTP UDP 端口映射问题；否则需映射 8550/tcp + UDP 端口段。
- 数据卷：`/opt/edgevision/config`（只读）、`/data/recordings`、`/var/log/edgevision`；根文件系统只读 + `/tmp` tmpfs。
- 健康检查：HTTP `/healthz` 或心跳文件；`restart: unless-stopped`；`stop_grace_period` 给编码器 flush 时间。
- 日志：json-file 限制大小/数量或 journald，避免写满 eMMC；Docker 数据目录不要放在小 eMMC 上。
- 构建：x86 上用 `buildx --platform linux/arm64` 时，尽量把 `RUN` 放在 amd64 builder 阶段，运行阶段只 `COPY`，避免 QEMU 拖慢。

最小运行示例：

```bash
docker run -d --name edgevision \
  --network host --restart unless-stopped \
  --device /dev/video0 --device /dev/mpp_service --device /dev/rga \
  --group-add video --group-add render \
  -v /data/edgevision:/data -v /opt/edgevision/config:/opt/edgevision/config:ro \
  edgevision:1.0.0
```

### 7.6 OTA 远程升级

先明确升级范围，再选型：

| 方案 | 升级范围 | 优点 | 风险/成本 | 适用 |
| --- | --- | --- | --- | --- |
| 应用目录 A/B + systemd | 应用/配置 | 最简单、可回滚、无需改分区 | 不动内核/驱动 | **推荐第一阶段** |
| 容器镜像 OTA | 应用+依赖 | 与 Docker 天然配合；回滚换 tag | 需要镜像仓库/离线包；不动系统 | 已确定 Docker 时**推荐** |
| RAUC A/B rootfs | 整系统 | 成熟、签名、U-Boot 集成、回滚 | 需要分区/U-Boot 可控；镜像制作复杂 | 需要升级内核/驱动时 |
| SWUpdate | 整系统/组件 | 灵活、支持多种 handler | 配置复杂 | 中大型项目 |
| Mender | 整系统+设备管理 | 平台完整 | 服务端/商业成本 | 有设备管理需求 |

应用级 A/B 建议布局：

```text
/opt/edgevision/releases/<version>/   # 每个版本一个目录
/opt/edgevision/current -> releases/<version>
/data/edgevision/{config,recordings,logs}
```

升级流程：

1. OTA 代理通过 HTTPS 拉取清单，校验硬件型号、版本范围、sha256、签名；
2. 下载到 `/data/ota/` 临时目录，`fsync`；
3. 停止服务，备份当前版本；
4. 原子切换 `current` 符号链接（或切换容器镜像 tag）；
5. 启动服务并做健康检查（进程 + RTSP 拉流 + 持续 60 秒）；
6. 成功则标记 `good`；失败自动回滚到上一版本；
7. 上报升级结果与当前版本。

安全与可靠性：

- 清单签名（Ed25519/RSA）+ HTTPS + 证书校验/pinning；设备证书可选；
- 防重放：版本号单调递增；拒绝降级到低于最小兼容版本；
- 断电保护：临时文件 + 原子 rename；启动计数 + 看门狗；连续失败进入恢复模式；
- **OTA 代理运行在宿主机 systemd**，不要放在被升级的容器里；
- 灰度发布：按设备分组/百分比放量，保留最近 2 个版本；
- 系统级 A/B（RAUC）只有在掌握 U-Boot、分区表和 rootfs 制作流程时才建议实施，否则优先应用级/容器级 OTA。

## 8. 验收指标

| 项目 | 指标 |
| --- | --- |
| H.264 推流 | 1080p30 连续 2 小时；`ffplay`/VLC 可播；码率偏差 ±10%；局域网端到端延迟 <200ms |
| 多通道 | 4 路 1080p30 连续 24 小时；CPU <70%；RSS 波动 <5%；板温 <80℃ |
| RGA/OSD | 缩放/裁剪无花屏、无色偏；OSD 叠加单帧 <5ms；4 路不成为瓶颈 |
| 录像 | 10 分钟分段、关键帧对齐、可播放；7 天保留；磁盘满自动清理；断电后已写分段可播 |
| Docker | 容器启动 <10 秒；设备可用；`restart` 自恢复；镜像 <300MB；日志不撑爆磁盘 |
| OTA | 连续 20 次升级成功；10 次随机断电不变砖；回滚 <2 分钟；篡改包 100% 拒绝 |
| 构建 | Ubuntu 一条命令产出 aarch64 包；可重复构建；产物带版本/commit；上板 `ldd` 无缺失 |

## 9. 风险与对策

| 风险 | 影响 | 对策 |
| --- | --- | --- |
| 板端 glibc <2.35 | `libavcodec.so` 无法加载 | 第一天确认 `ldd --version`；不匹配则重编 FFmpeg 或更换 sysroot |
| sysroot/库版本混杂 | 本机能链接、板上跑不起来 | 统一使用板端 sysroot；上板 `ldd` + 冒烟测试 |
| 扇出拓扑未补就用多下游 | 加录像/AI 后随机脏数据 | M1 先补扇出拓扑 + 多分支用例（引用计数已完整，P0-5 假阳性） |
| RGA 不支持 NV12 叠加 | OSD 方案返工 | 第 3 周先做最小 PoC；准备 RGB 中转或局部 CPU 叠加降级 |
| 录像掉电 | MP4 损坏 | fMP4 或分段 + 原子 rename + 启动修复 |
| 多通道 MPP 实例 | 编码吞吐/内存不足 | 先做 1/2/4 路阶梯压测；限制单通道分辨率 |
| Docker 设备权限 | 容器内 MPP/RGA 不可用 | 明确 `--device`/cgroup 规则；非 root + 组权限；早期上板验证 |
| OTA 变砖 | 现场设备失联 | 应用 A/B + 看门狗 + 回滚；系统级 A/B 需 U-Boot 支持 |
| 安全暴露 | RTSP 无认证、OTA 被篡改 | RTSP 鉴权/白名单；OTA 签名 + TLS；容器降权 |
| 存储磨损/容量 | 录像写满、eMMC 损坏 | 独立数据分区；保留策略；监控水位与介质健康 |

## 10. 待确认输入

1. **板端系统**：`/etc/os-release`、`ldd --version`、`uname -a`；是否 Ubuntu 22.04？glibc 是否 ≥2.35？
2. **系统可控性**：是否有 U-Boot/分区表控制权？能否做 rootfs A/B？还是只能动应用层？
3. **升级范围**：只升级应用/容器，还是也要升级内核/驱动？
4. **录像需求**：存储介质（eMMC/SD/NFS）、保留天数、分段时长、是否需要音频？
5. **OSD 内容**：时间/通道名/分辨率/码率/AI 框？是否需要中文字体、多语言？
6. **编码目标**：只要 H.264，还是 H.264+H.265 同时；目标通道数、分辨率、帧率？
7. **Docker 约束**：是否允许 `privileged`/`host` 网络？板端是否已装 Docker？镜像仓库自建还是离线包？
8. **CI 资源**：是否有 ARM 板子做 nightly，还是只做交叉编译？

在未确认前，建议按以下默认值推进：**Ubuntu 22.04 / 应用级 A/B OTA / fMP4 录像保留 7 天 / OSD 显示时间+通道名 / 每通道单编码 H.264 / Docker host 网络 / 交叉编译 + 板端 nightly**。

## 11. 第一周行动清单

1. 在板端收集：`cat /etc/os-release`、`ldd --version`、`ls -l /dev/mpp_service /dev/rga /dev/video* /dev/dma_heap`、`mount`、`cat /proc/cmdline`。
2. 在 Ubuntu 上确认 `aarch64-linux-gnu-g++-11` 可用，修正工具链路径，准备板端 sysroot。
3. 用现有 `install/rk3588` 做一次最小交叉编译验证（只验证工具链与链接）。
4. 把 `PushNode` 的 H.265/码率硬编码临时改为参数，做 H.264 冒烟，确认可行后再正式配置化。
5. 写一个最小 RGA PoC：NV12 缩放 + RGBA 文字叠加，确认板端 RGA 支持矩阵与耗时。
6. 确认 OTA 边界：板端是否有 U-Boot/分区控制权，决定走“应用 A/B”还是“RAUC 系统 A/B”。

## 12. 变更记录

| 日期 | 变更 |
| --- | --- |
| 2026-11-04 | 初版：从原 README 第 11 章拆出，补充 WBS、排期、技术方案、验收、风险与待确认项 |
| 2026-11-04 | 文档重构：README 精简为入口页，架构与扩展约定迁至 `ARCHITECTURE.md`，现状与问题迁至 `STATUS.md`；6 周稳定性 MVP 排期并入本文 6.1 |
| 2026-11-12 | 新增 [DESIGN_PROCESS_SPLIT.md](DESIGN_PROCESS_SPLIT.md)：三进程（媒体 / 通信 / 升级）+ 共享通信对象 `libcomm_api` 的拆分规划；本文头部补充指引 |

> 维护约定：本路线图随优先级与板端约束调整同步更新；完成一个里程碑后，在“变更记录”中登记，并回写 [STATUS.md](STATUS.md) 的现状章节。

[返回 README](../README.md)
