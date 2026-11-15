# 应用级升级设计（`evs_upgrade`：A/B 目录 + 签名清单 + 自动回滚）

> **状态：骨架已落地（2026-11-15）** —— `src/upgrade/` → `libupgrade_api.so` + `src/main/upgrade/` → `evs_upgrade`，编译/运行/验收见 [TESTLOG §5.25](../history/TESTLOG.md)。检查（取清单/验签/兼容性）、下载、校验、槽位原子切换、状态持久化、事件上报已实现；**解包 / `systemctl` / ffprobe 帧率 / 旧版本清理仍是桩**（一律 `E_NOTIMPL`，不假装成功）。
> 最后更新：2026-11-15；适用分支：`rk3588`；代码校准点：`422b2c4`（ZLM 日志级别默认 error）。
> 相关文档：[ROADMAP.md](ROADMAP.md) §7.6（OTA 概览）· [DESIGN_PROCESS_SPLIT.md](DESIGN_PROCESS_SPLIT.md) §4/§5.1/§7 第 4 步（三进程拆分）· [HANDOFF.md](../HANDOFF.md) §2（红线）· [ARCHITECTURE.md](../ARCHITECTURE.md) · [TESTLOG.md](../history/TESTLOG.md)

## 1. 目标与非目标

### 1.1 目标（每条都可验收）

| # | 目标 | 验收口径 |
| --- | --- | --- |
| G1 | 远程下发、本地验签后升级**应用层**（`bin/` + 自有 `lib/*.so` + 默认配置） | 篡改任意 1 字节 → 100% 拒绝 |
| G2 | 升级失败/新版本不健康时**自动回滚**，不需要人到现场 | 回滚 < 2 分钟（ROADMAP §8） |
| G3 | **断电安全**：任意阶段掉电不变砖，重启后能自愈到可用版本 | 10 次随机断电后设备仍可出帧 |
| G4 | 升级过程可观测：进度、结果、原因码通过 MQTT 上报 | `evs/v1/upgrade/evt/*` 有进度与终态 |
| G5 | 不引入新第三方库、不改内核/glibc/驱动、不破坏现有两进程运行方式 | `make` 全绿；`make run-media` 行为不变 |

### 1.2 非目标（明确不做，避免范围失控）

1. **不升级内核 / 驱动 / 设备树 / BSP / U-Boot**（L3，见 §3）；需要时交 SWUpdate/RAUC。
2. 不做差分包（`bsdiff`/`xdelta3`）——首期全量包，差分列为后续增强。
3. 不做灰度/分组放量（云侧职责，设备端只认"给我哪个版本"）。
4. 不做容器镜像管理（Docker 见 §14，结论是暂不作为主方案）。
5. 不做配置热加载——升级只替换默认配置，**不覆盖现场配置**（§4.2）。
6. 不新增"远程执行任意命令"的能力（安全红线，见 §8）。

### 1.3 为什么首期只做应用层（三条硬约束）

1. **ABI 锁死**：`libavcodec.so` 最高要求 `GLIBC_2.35`，`libmk_api.so` 依赖 OpenSSL 3。换 rootfs 等于换 glibc，现有二进制直接加载不了（HANDOFF §2「系统层」红线）。
2. **系统可控性未知**：板端是否掌握 U-Boot `bootcmd` / 分区表，仓库里至今没有结论（ROADMAP §10 待确认项 2）。没有它，A/B rootfs 无法安全切换。
3. **收益/成本比**：本项目 90% 的变更在应用层（媒体链路、通信、参数）。应用级 A/B 用 13 人日就能拿到"可回滚 + 断电安全"，系统级 A/B 要重做镜像制作与引导链。

## 2. 现状事实（方案的前提，全部实测）

| 事实 | 实测值 / 位置 | 对方案的影响 |
| --- | --- | --- |
| 板端内核 / 架构 | `6.1.99-rockchip-rk3588`、aarch64、8 核、16 GB | 系统级升级风险高，首期不做 |
| 系统层 ABI | `libavcodec` 需 `GLIBC_2.35`；`libmk_api` 依赖 OpenSSL 3 | 清单必须带 `abi` 字段，不匹配**拒装**而不是装完起不来 |
| FFmpeg 是否支持 HTTPS | `install/rk3588/lib/libavformat.so` 内含 `https protocol not found, recompile FFmpeg with openssl, gnutls or securetransport enabled` | **不能**用 `avio_open("https://…")` 下包 |
| 第三方库清单 | `install/rk3588/lib` 有 `libssl.so.3` / `libcrypto.so.3`（`install/rk3588/include/openssl/opensslv.h` = **3.5.4**），**无 libcurl** | 验签用现成 OpenSSL；下载走系统 `curl` 或自研最小 HTTPS（§5.6） |
| OpenSSL 双版本 | 自带 = **3.5.4**；系统 `/usr/bin/openssl` = **3.0.2** | 构建必须显式指向 `install/`，否则头/库与运行时错配（§11.2 头号风险） |
| 板端工具 | `curl`、`gzip`、`zstd 1.4.8`、`ffmpeg/ffprobe 5.0.3` 均存在；但 **`ffprobe` 只在 `3rdparty/rk3588/ffmpeg/bin/` 下**，不在系统 `PATH` | 下载方案 A 可行；健康检查依赖 `ffprobe` 时必须指定绝对路径或自带 |
| Docker | 29.5.3 / overlayfs / cgroup v2 / 当前 0 镜像 / `registry-1.docker.io` **不可达** | 容器路线只能离线导入（§14） |
| CMake 预留位 | `src/main/CMakeLists.txt:44-47`：`EVS_PROCESS=upgrade` 目前抛 `FATAL_ERROR "尚未实现"` | 落地时改成正常分支，不新建机制 |
| 通信协议预留 | `DESIGN_PROCESS_SPLIT` §5.1 已定 `evs/v1/upgrade/cmd/{check,download,apply}` 与 `evt/{progress,done,failed}` | 直接沿用，不另造 topic |
| 通信库 API | `CommManager::getInstance()` / `Create(CommConfig)` / `SetReceiveCallback(name, frame)` / `StartAll` / `StopAll` / `Clear` | 升级进程照 `main_comm.cpp` 的骨架写，零新依赖 |
| MQTT 角色语义 | `CommRole::kServer` = 订阅通配主题的**订阅端**，broker 是独立进程 | 升级进程当 client（发布 evt、订阅 cmd） |
| 崩溃采集 | `InstallCrashHandler(CrashConfig)` + `SetCrashTag` + `EVS_CRASH_DIR` + systemd `LimitCORE=infinity` | 升级进程必须同样接入，现场崩了才留得下证 |
| 现有安装路径 | `scripts/deploy/evs-edgevision.service` 用 `/opt/EdgeVisionService`；ROADMAP §7.6 用 `/opt/edgevision` | **两处不一致**，本文按 ROADMAP 写，§16 列为待确认项 |
| 崩溃时的 core 落点 | `core_pattern` 是**宿主内核级**配置 | 决定了容器化会削弱崩溃取证（§14） |

## 3. 升级分层与方案选型

### 3.1 三层边界

| 层 | 升级对象 | 手段 | 回滚粒度 | 本项目 |
| --- | --- | --- | --- | --- |
| **L1 应用层** | `bin/`、自有 `lib/*.so`、默认配置、模型文件 | 目录 A/B + 符号链接 + systemd 重启 | 整个版本 | ✅ **首期（本文）** |
| **L2 容器层** | 镜像（应用 + 依赖） | 换 tag / 蓝绿容器 | 整个镜像 | ⚠️ 可选，§14 |
| **L3 系统层** | rootfs / 内核 / 驱动 / BSP | A/B rootfs + 引导器切换 | 整个系统 | ⏳ 交 SWUpdate / RAUC |

### 3.2 市场主流方案对比（选型依据）

| 方案 | 层级 | 升级对象 | 回滚机制 | 断电容错 | 依赖 / 门槛 | 适用场景 | 本项目结论 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| **自研目录 A/B + systemd** | L1 | bin/libs/config | 换回 `current` 链接 | 好（tmp + rename） | 零新依赖 | 中小设备、应用迭代快 | ✅ **首期选它** |
| **Docker 换 tag** | L2 | 镜像 | 回滚到旧 tag | 好（镜像层） | dockerd + 本地仓库/离线包 | 已容器化产品 | ⚠️ P2，见 §14 |
| **balena / balenaOS** | L2+L3 | 容器 + OS | supervisor 自动回滚 | 好 | 强绑定 balenaCloud | 无人值守商用设备 | ❌ 云绑定，不符内网部署 |
| **SWUpdate** | L3/组件 | rootfs·单文件·差分 | 双分区 + handler | 好（U-Boot 集成） | 分区/U-Boot 可控 | 工业网关 | ✅ **L3 首选**（后期） |
| **RAUC** | L3 | rootfs bundle | A/B slot + bootchooser | 好（bootchooser） | U-Boot + D-Bus | Yocto/Debian 系统 | ✅ L3 备选 |
| **Mender** | L3 + 设备管理 | rootfs/应用 | A/B + 服务端 rollout | 好 | 需服务端 | 大规模车队 | 💰 有服务端成本 |
| **OSTree / rpm-ostree** | L3 | 只读 rootfs | 原子部署回滚 | 好 | 必须是 ostree 系统 | Fedora IoT | ❌ 不适配现有系统 |
| **Android A/B（`bootctl`）** | L3 | boot/rootfs | slot 切换 | 好 | bootloader HAL | Android | ❌ 不适用 |
| **云 IoT（AWS IoT Jobs / Azure Device Update / 阿里云 OTA）** | 编排层 | 任意（配合 L1~L3） | 靠设备端 | 靠设备端 | 云账号 | 大规模分发/灰度 | 🔌 后接，不下沉设备端 |
| **差分升级（bsdiff / xdelta3）** | 任意 | 包 | 视上层 | 视上层 | 无 | 弱网、流量敏感 | 🎯 二期增强 |

### 3.3 选型结论

**L1 自研（本文）→ L3 交 SWUpdate/RAUC（需要时）→ 云侧只做分发编排。**

理由：市面方案没有"一个包打天下"。L1 自研成本最低（13 人日）、零新依赖、不触碰系统层红线；L3 涉及引导链，成熟开源件比自研可靠得多，但没有 U-Boot/分区控制权就绝不能做。

## 4. 总体设计

### 4.1 进程拓扑

```text
                ┌──────────────── 云端 / 内网 OTA 源（HTTPS 或共享目录）────────────────┐
                │  manifest.json + manifest.sig + evs-app-<ver>-rk3588.tar.gz          │
                └───────────────────────────────┬──────────────────────────────────────┘
                                                │ ① 拉清单 / 拉包
   ┌──────────────────────┐   MQTT   ┌──────────▼──────────────────┐
   │ evs_comm（进程 2）    │◀────────▶│ evs_upgrade（进程 3，新增） │
   │ 命令路由 / 状态汇聚   │  cmd/evt │ 状态机 / 验签 / 切换 / 健康 │
   └──────────┬───────────┘          └──────────┬──────────────────┘
              │ broker（独立进程）               │ ② systemctl stop/start（D-Bus）
              │                                  ▼
   ┌──────────▼───────────┐          ┌──────────────────────────────┐
   │ evs_media（进程 1）   │◀─────────│ /opt/edgevision/current      │
   │ 采集/编码/推流        │  符号链接 │  → releases/<version>/        │
   └──────────────────────┘          └──────────────────────────────┘
```

三条硬约束：
1. `evs_upgrade` **必须在宿主 systemd 里**，不能放进被升级目录/容器（否则自己把自己换掉，断电即失联）。
2. 三进程之间**只通过 MQTT topic 与 systemd** 交互，不共享内存、不互相 `#include`。
3. 升级只改 `/opt/edgevision`，`/data/edgevision` 跨版本保留。

### 4.2 目录布局

```text
/opt/edgevision/                     # install_root：升级进程独占写权
├── releases/
│   ├── 1.4.1/
│   │   ├── bin/{evs_media,evs_comm,evs_upgrade}
│   │   ├── lib/*.so                 # 只放自研库（common/comm/pipeline/platform/upgrade）
│   │   ├── config/*.json            # 出厂默认配置
│   │   ├── manifest.json            # 安装时留档（回查"这一版是谁签的"）
│   │   └── version.json             # {version, git_commit, built_at, abi}
│   └── 1.4.2/…
├── current  -> releases/1.4.2       # 唯一指针，原子 rename 更新
├── previous -> releases/1.4.1       # 回滚目标，原子 rename 更新
└── .staging/                        # 解包暂存区（与 releases 同分区！）

/data/edgevision/                    # data_root：升级不覆盖
├── config/                          # 现场配置（node.json/comm.json/upgrade.json 现场值）
├── ota/download/                    # 下载中的包（*.part）
├── state/upgrade.json               # 状态机持久化：phase / from / to / boot_count
└── logs/
```

**两个必须写进代码的约束**：

1. **`/opt/edgevision` 与 `/data/edgevision` 必须同一文件系统**，否则 `rename()` 跨设备会退化成"拷贝 + 删除"，失去原子性。落到不同分区时，`.staging` 必须放在 `/opt/edgevision` 下（本文默认如此）。
2. **现场配置与出厂配置分离**：升级包只带 `config/` 默认值；启动时优先读 `/data/edgevision/config/`，缺失才回落到 release 内默认值。理由：现场改过分辨率/RTSP 端口，升级一覆盖就等于设备失联。

### 4.3 组件与职责（文件级）

| 文件（计划） | 职责 | 明确不做 |
| --- | --- | --- |
| `src/main/upgrade/main_upgrade.cpp` | 入口：`InstallCrashHandler` → 读 `config/upgrade.json`（候选路径同 `main_comm.cpp`）→ `UpgradeService::Run()` → 信号 → 有序停止 | 不写业务状态机 |
| `src/main/upgrade/upgrade_service.{h,cpp}` | 编排：状态机推进、事件上报、systemd 停起、健康检查调度 | 不解析清单、不验签 |
| `src/upgrade/manifest.{h,cpp}` | 解析 `manifest.json`、字段校验，产出 `Manifest` 结构（复用 `common/json.hpp`） | 不做网络 |
| `src/upgrade/verifier.{h,cpp}` | SHA-256 + Ed25519 验签（OpenSSL 3.5.4 `EVP_PKEY_ED25519`） | 不管文件从哪来 |
| `src/upgrade/fetcher.{h,cpp}` | 取清单/取包：`curl` 子进程（方案 A）或 OpenSSL HTTPS（方案 B），带超时/重试/断点 | 不校验内容 |
| `src/upgrade/slot_manager.{h,cpp}` | `releases/` 管理、`.staging` 解包、原子切换 `current`/`previous`、版本保留、磁盘水位 | 不判断"健康不健康" |
| `src/upgrade/health_check.{h,cpp}` | 进程存活 + RTSP 首帧 + 帧率 + 稳定时长 | 不重启服务（交给 systemd） |
| `config/upgrade.json` | 全部可调参数（§10），改配置不重编译 | 不放密钥正文 |
| `/etc/evs/ota_ed25519.pub` | 验签公钥（**不在** `/opt/edgevision` 内，防止被自己升级替换） | — |

### 4.4 与现有代码的接入点（照着用，不新造轮子）

| 能力 | 现成实现 | 用法 |
| --- | --- | --- |
| 崩溃采集 | `common/crash/crash_handler.h` | `main` 第一行 `InstallCrashHandler(cfg)`；`SetCrashTag("phase", …)` 把升级阶段带进报告 |
| JSON | `common/json.hpp`（`common_api` 已 PUBLIC 暴露） | 清单、配置、上报统一用它 |
| 通信 | `src/comm/comm_manager.h` | `CommManager::getInstance()` → `Create(CommConfig)` → `SetReceiveCallback` → `StartAll`；退出 `StopAll` + `Clear` |
| 配置读取套路 | `src/main/comm/main_comm.cpp` | 候选路径 `config/xxx.json` → `../config/xxx.json`；`ReadString/ReadInt/ReadBool` 类型不符当没写；语法错误 `return 1` |
| 进程骨架 | `src/main/comm/main_comm.cpp` | 200 ms `nanosleep` 轮询 `g_run` 收信号 |
| 构建开关 | `src/main/CMakeLists.txt` 的 `EVS_PROCESS` | 加 `upgrade` 分支（§11.1） |
| 部署 | `scripts/deploy/evs-edgevision.service` | 新增 `evs-upgrade.service`（§11.4），沿用 `LimitCORE=infinity` |

## 5. 升级包与清单

### 5.1 包结构

```text
evs-app-1.4.2-rk3588.tar.gz          # 首期 gzip（tar 内建）；板端有 zstd 1.4.8，P2 可切
├── manifest.json                    # 清单（字段见 5.2）
├── manifest.sig                     # Ed25519 对 manifest.json 原始字节签名（不预哈希）
└── payload/
    ├── bin/{evs_media,evs_comm,evs_upgrade}
    ├── lib/*.so
    └── config/*.json                # 仅默认值
```

> 关于压缩格式：板端实测**有** `zstd 1.4.8`（`/usr/bin/zstd`），但首期仍选 gzip —— `tar` 内建 gzip 支持，不需要额外子进程或 `libzstd` 链接；zstd 可在 P2 实测解包耗时后切换（体积约小 15~25%）。

### 5.2 `manifest.json` 字段

| 字段 | 类型 | 必填 | 语义 / 校验规则 |
| --- | --- | --- | --- |
| `schema` | int | ✅ | 清单格式版本，首期 `1`；不认识就拒 |
| `version` | string | ✅ | 语义化版本 `MAJOR.MINOR.PATCH`，必须**严格大于**当前版本（除非 `allow_downgrade`） |
| `git_commit` | string | ✅ | 构建来源，写进上报，便于回溯 |
| `built_at` | string | ✅ | ISO-8601 UTC |
| `target.hw` | string | ✅ | 必须等于 `rk3588` |
| `target.abi` | string | ✅ | 如 `glibc-2.35`；与本机不符直接拒（防止换 rootfs 的包误装） |
| `target.min_from` | string | ✅ | 允许升级的最小源版本；低于它拒（跨大版本配置不兼容） |
| `payload_sha256` | string | ✅ | `payload/` 打包流的 SHA-256 |
| `size_bytes` | int | ✅ | 下载前先查磁盘，不足直接失败，不浪费流量 |
| `files` | array | ⭕ | 逐文件清单（`path`/`sha256`/`mode`），用于精细校验与审计 |
| `rollback.keep_versions` | int | ⭕ | 默认 2：保留几个历史版本 |
| `rollback.boot_count_max` | int | ⭕ | 默认 3：连续启动失败多少次自动回滚 |

### 5.3 示例

```json
{
  "schema": 1,
  "version": "1.4.2",
  "git_commit": "422b2c4",
  "built_at": "2026-11-20T02:10:00Z",
  "target": { "hw": "rk3588", "abi": "glibc-2.35", "min_from": "1.3.0" },
  "payload_sha256": "9f2c...（64 hex）",
  "size_bytes": 48219136,
  "files": [
    { "path": "bin/evs_media", "sha256": "...", "mode": "0755" },
    { "path": "lib/libcomm_api.so", "sha256": "...", "mode": "0644" }
  ],
  "rollback": { "keep_versions": 2, "boot_count_max": 3 }
}
```

### 5.4 签名与验签

```text
发布侧（CI / 打包脚本）：
  1. tar czf payload.tar.gz payload/
  2. payload_sha256 = sha256(payload.tar.gz)   → 写入 manifest.json
  3. openssl pkeyutl -sign -inkey ota_ed25519.pem -rawin \
                 -in manifest.json -out manifest.sig      # 私钥只在发布机
  4. tar czf evs-app-<ver>-rk3588.tar.gz manifest.json manifest.sig payload.tar.gz

设备侧（evs_upgrade）：
  1. 读 verify.public_key（PEM SubjectPublicKeyInfo，或 32 字节裸公钥）
  2. 先验 manifest.sig（对 manifest.json **原始字节**）—— 没通过签名，连解析都不做
  3. 再校验 payload_sha256       —— 防止包体被替换
  4. 最后逐文件校验 files[]      —— 若清单提供
```

⚠️ **签名口径（务必照抄，这是「验签永远失败」的头号原因）**：
Ed25519 内部已经对消息做 SHA-512，所以第 3 步**直接用 `pkeyutl -rawin` 签整份
`manifest.json`**，不要再自己套一层 `sha256`。若发布侧先 `sha256` 再签，设备侧也必须
照做，否则两边消息不同、验签必然不通过。实现见 `src/upgrade/verifier.cpp` 的
`VerifyEd25519()`（注释里写死了这条口径）。

三条规则：
- **先验签、后解包**：没通过签名的包不进 `.staging`，避免解包器成为攻击面。
- **公钥不可被升级替换**：放 `/etc/evs/`，与 `/opt/edgevision` 分离。
- **只用 Ed25519**：OpenSSL 3.5.4 已内置；不要自造 RSA + 自写填充。

### 5.5 兼容性与防降级

| 场景 | 判定 | 动作 |
| --- | --- | --- |
| `target.hw` 不符 | 硬件不匹配 | 拒，`E_HW` |
| `target.abi` 不符 | 系统层 ABI 不匹配（换过 rootfs） | 拒，`E_ABI` |
| `version` ≤ 当前版本 | 疑似降级攻击 / 重放 | 拒，`E_DOWNGRADE`（除非显式 `allow_downgrade=true`） |
| `version` < `target.min_from` | 源版本过老，配置结构不兼容 | 拒，`E_MIN_FROM` |
| 同版本且 `payload_sha256` 相同 | 重复下发 | 直接回 `done`（幂等），不重复切换 |

### 5.6 下载通道的三条路线（**待决策**，见 §16）

| 方案 | 实现成本 | 是否违反「不引新库」 | 说明 |
| --- | --- | --- | --- |
| **A. 系统 `curl` 子进程** | 最低（`fork/exec` + 超时 + `-C -` 断点续传） | 不违反（外部工具，不加链接依赖） | ✅ 首期推荐；板上 `curl` 已实测存在 |
| **B. 自研最小 HTTPS（OpenSSL）** | 中（TLS 握手 + 证书校验 + HTTP/1.1，约 300 行） | 不违反（`libssl`/`libcrypto` 已在 `install/`） | 无 `curl` 的精简镜像，或要求证书 pinning |
| **C. 引入 libcurl** | 低 | ❌ **违反**（`install/` 无 `libcurl`） | 需用户批准改红线，否则不选 |

安全兜底：**无论走哪条，最终信任来自本地 Ed25519 验签**。TLS 只解决窃听，签名解决投毒——所以离线 U 盘导入（`source.type=file`）也必须走同一套验签。

## 6. 状态机与原子切换

### 6.1 状态机

```text
IDLE
 │ cmd: check
 ▼
CHECKING ──无新版本──────────────▶ IDLE（上报 done / result=up-to-date）
 │ 有新版本
 ▼
DOWNLOADING ──网络/磁盘失败──────▶ FAILED（E_NET / E_SPACE）
 │ 校验 size_bytes + sha256
 ▼
VERIFYING ──签名/哈希/清单不符───▶ FAILED（E_SIG / E_HASH / E_SCHEMA / E_HW / E_ABI / E_DOWNGRADE）
 │ 通过
 ▼
STAGING（解包到 .staging，逐文件校验、写盘 fsync）
 │ 解包/权限失败
 ├────────────────────────────────▶ FAILED（E_APPLY）
 ▼
ACTIVATING（stop media → 切换 current/previous → start media）
 │ 启动失败
 ├────────────────────────────────▶ ROLLING_BACK
 ▼
HEALTH_CHECKING（进程存活 → RTSP 首帧 → 帧率 → 稳定 stable_sec）
 │ 不健康 / 超时
 ├────────────────────────────────▶ ROLLING_BACK
 │ 通过
 ▼
COMMITTED（boot_count 清零、清理旧版本、上报 done）──▶ IDLE

任意状态 ──进程重启/掉电──▶ 读 /data/edgevision/state/upgrade.json 恢复到 phase 对应的安全动作
```

### 6.2 各阶段的动作与失败出口

| 阶段 | 做什么 | 失败出口 | 是否动线上业务 |
| --- | --- | --- | --- |
| CHECKING | 拉 `manifest.json` + `manifest.sig`，验签，比对版本/硬件/ABI | `FAILED` | 否 |
| DOWNLOADING | 下包到 `ota/download/*.part`，支持断点续传；进度每 5% 上报一次 | `FAILED` | 否 |
| VERIFYING | 校验 `size_bytes`、`payload_sha256` | `FAILED` | 否 |
| STAGING | 解包到 `/opt/edgevision/.staging/<ver>/`，逐文件 `fsync` | `FAILED`（磁盘满/权限） | 否 |
| ACTIVATING | `systemctl stop evs-media`（≤5 s）→ `rename` 切 `current`/`previous` → `systemctl start evs-media` | `ROLLING_BACK` | **是（秒级中断）** |
| HEALTH_CHECKING | 进程存活 + RTSP 首帧 + 最低帧率 + 稳定时长 | `ROLLING_BACK` | 是（新版本在跑） |
| COMMITTED | `boot_count=0`，删多余旧版本，上报 `done` | — | 否 |
| ROLLING_BACK | 停服务 → `current` 指回 `previous` → 起服务 → 上报 `failed(reason=rollback)` | `FAILED`（回滚也失败 → 恢复模式） | 是 |

### 6.3 原子写序列（每个持久化点都照这个写）

```text
写文件：   open(tmp, O_WRONLY|O_CREAT|O_TRUNC) → write → fsync(fd) → close
          → rename(tmp, final) → open(dir, O_RDONLY) → fsync(dirfd) → close

切符号链接：symlink("releases/1.4.2", "current.tmp") → rename("current.tmp", "current") → fsync(dirfd)
            （rename 覆盖已存在的符号链接是原子的；不要先 rm 再 ln —— 中间窗口设备不可用）
```

`fsync(dirfd)` 这步最容易被省掉：**只 `fsync` 文件不 `fsync` 目录**，掉电后目录项仍可能丢，`rename` 等于没做。

### 6.4 断电保护矩阵（掉电时刻 → 重启后行为）

| 掉电发生在 | 盘上状态 | 重启后动作 |
| --- | --- | --- |
| 下载中 | `*.part` 残留，`state.phase=DOWNLOADING` | 校验断点，续传或重下；不影响现网 |
| 解包中 | `.staging/` 半成品，`current` 未动 | 删除 `.staging`，回 `IDLE`；现网版本照常运行 |
| 切换符号链接时 | `rename` 原子 → 要么旧指针要么新指针 | 按 `current` 实际指向继续；`state` 与盘上不一致时**以盘上为准** |
| 新版本启动中 | `state.phase=HEALTH_CHECKING`，`boot_count=1` | 继续健康检查；不健康则回滚 |
| 回滚中 | `state.phase=ROLLING_BACK` | 重试回滚（幂等：目标 `previous` 已记录） |
| COMMITTED 之前 | `boot_count>0` | 计入 `boot_count`，达到上限即回滚 |

**幂等原则**：每个阶段进入时先读盘上真实状态，再决定动作——不复用内存里的"我以为"。

## 7. 健康检查、回滚与预算

### 7.1 健康判据（按顺序短路）

| # | 判据 | 实现 | 阈值（`upgrade.json`） |
| --- | --- | --- | --- |
| 1 | 服务在跑 | `systemctl is-active evs-media` 或 `ActiveState=active` | 立即判 |
| 2 | 没在反复重启 | 读 `NRestarts`，60 s 内必须为 0 | `stable_sec` |
| 3 | RTSP 出流 | `ffprobe -v error -rtsp_transport tcp -i <rtsp_url> -show_streams` 子进程，超时即失败。⚠️ 板端 `ffprobe` 在 `3rdparty/rk3588/ffmpeg/bin/`，**不在系统 `PATH`** → 配置里给绝对路径，部署时随包带或改用自研 `RTSP OPTIONS/DESCRIBE`（约 30 行 socket） | `probe_timeout_sec=10` |
| 4 | 帧率达标 | `ffprobe` 取不到就用短时计数（`-read_intervals`），或后续由媒体进程经 MQTT 上报 | `min_fps≥15` |
| 5 | 持续稳定 | 以上 1~4 连续成立 `stable_sec` 秒才算健康 | `stable_sec=60` |
| 6 | 崩溃未新增 | 对比 `/data/edgevision/logs/crash/` 目录文件数 | 出现新 core / 新报告 → 不健康 |

### 7.2 `boot_count` 机制（防"起不来就永久砖"）

```text
切到新版本后：state.json 写 { phase: HEALTH_CHECKING, from: 1.4.1, to: 1.4.2, boot_count: 0 }

evs-media.service 每次启动成功执行：
    ExecStartPost=/opt/edgevision/current/bin/evs_upgrade --on-media-started
    → 该模式只做一件事：boot_count += 1（原子写盘）后立即 exit 0

判定：
    boot_count > boot_count_max(3)  → 强制回滚到 previous，上报 failed(code=E_HEALTH)
    COMMITTED 时 boot_count 清零，phase=IDLE
```

为什么用 `ExecStartPost` 而不是让 `evs_upgrade` 自己数：**媒体进程起不来时，常驻的升级进程可能已被 systemd 一起停掉**；`ExecStartPost` 由 systemd 每次真的启动成功才调用，是唯一不会漏的计数点。

### 7.3 回滚时序与预算

| 步骤 | 预算 | 说明 |
| --- | --- | --- |
| `systemctl stop evs-media` | ≤5 s | 媒体进程已有信号 → 有序停止逻辑，正常 <1 s |
| 原子切回 `previous` | <50 ms | 一次 `rename` + `fsync(dirfd)` |
| `systemctl start evs-media` | ≤3 s | 冷启动到首帧 |
| 健康检查（回滚路径放宽） | ≤30 s | 只要求"进程在跑 + 首帧出来"，不等 `stable_sec` |
| 上报 `failed(code=E_HEALTH, rolled_back=true)` | <1 s | MQTT 已连则立即发，未连则落盘待补发 |
| **合计** | **< 60 s（远低于 ROADMAP §8 的 2 min）** | — |

### 7.4 systemd 侧兜底（`evs_upgrade` 自己也起不来时）

```ini
# evs-media.service 里加
OnFailure=evs-rollback.service
StartLimitIntervalSec=300
StartLimitBurst=3
```

`evs-media` 在 300 s 内失败 3 次 → systemd 判定 unit failed → 触发 `OnFailure` → `evs-rollback.service`（`Type=oneshot`，执行 `evs_upgrade --rollback --reason=start-limit`）。这是**最后一道防线**：即使常驻升级进程挂掉，回滚仍会发生。

### 7.5 恢复模式（回滚也失败）

连续回滚失败 2 次 → 写 `state/upgrade.json: { mode: "recovery" }`：

1. 不再尝试任何升级（`check` 命令直接回 `E_RECOVERY`）。
2. `current` 指向"能起的最老可用 release"（若全挂则保留现状不动）。
3. 通过 `evt/failed` 每小时重发一次，直到收到 `cmd/apply {force:true}` 或人工介入。
4. 日志里打印明确的处置指引（含 `install_root`、`data_root`、可用版本列表）。

## 8. 安全设计

### 8.1 威胁模型与对策

| 威胁 | 攻击面 | 对策 |
| --- | --- | --- |
| 包体篡改 | 传输链路 / 存储 | Ed25519 验签 + `payload_sha256` + 逐文件哈希（§5.4） |
| 恶意清单（指向任意 URL/路径） | `manifest.json` | 清单必须验签；`files[].path` **拒绝绝对路径与 `..`**（解包 tar 时同样校验，防穿越） |
| 降级 / 重放旧包 | `cmd` 或源站 | 版本单调递增校验 + `min_from`（§5.5） |
| 命令注入 | 下载 URL、版本号拼进 shell | **禁止拼 shell 字符串**：`curl` 用 `fork/execv` 传 argv，不经过 `system()`/`popen()`；版本号先正则 `^[0-9]+\.[0-9]+\.[0-9]+$` |
| 中间人 | HTTPS | 走系统 CA 校验；内网可加证书 pinning（方案 B 时可实现） |
| 越权写系统 | 升级进程是 root | 只允许写 `install_root` + `data_root` + `state`；systemctl 只允许白名单三个动作（`start`/`stop`/`is-active`）与两个 unit |
| 磁盘塞满 | 下载/解包 | 水位检查（`free_space_min_mb`）；保留版本数上限；`.staging` 失败即清 |
| 并发升级 | 多次 `apply` 同时到 | 文件锁 `flock(/data/edgevision/state/upgrade.lock)`；非 `IDLE` 时新命令回 `E_BUSY` |
| 自己把自己换掉 | 升级 `evs_upgrade` 本体 | 允许，但**新二进制只在下次启动生效**；当前进程继续跑完本次流程（`execve` 不做） |

### 8.2 权限与沙箱建议

- 进程以 **root** 运行（需要写 `/opt`、调 systemd）。用 systemd 侧收窄，而不是在代码里做：
  `ProtectSystem=strict` + `ReadWritePaths=/opt/edgevision /data/edgevision` + `ProtectHome=yes` + `PrivateTmp=yes`。
- **不要**给 `NoNewPrivileges=yes` 之外的过度限制（会挡住 `systemctl`）；也不要为了省事 `systemctl` 走 shell。
- 公钥 `/etc/evs/ota_ed25519.pub` 权限 `0644 root:root`；私钥**永不入仓库、不上设备**。

### 8.3 审计留痕

每次升级在 `/data/edgevision/logs/upgrade.log` 追加一行 JSON：`{ts, action, from, to, phase, result, code, size, duration_ms, operator}`。`last_error()` 之类临时信息不进审计日志——审计日志只记**结果**。

## 9. 通信接口（沿用 `DESIGN_PROCESS_SPLIT` §5.1 的 topic）

### 9.1 Topic 一览

| Topic | 方向 | 谁发 | 谁收 | 用途 |
| --- | --- | --- | --- | --- |
| `evs/v1/upgrade/cmd/check` | 命令 | 云 / `evs_comm` 路由 | `evs_upgrade` | 查新（可带临时 `url`） |
| `evs/v1/upgrade/cmd/download` | 命令 | 同上 | `evs_upgrade` | 只下载不切换（预取） |
| `evs/v1/upgrade/cmd/apply` | 命令 | 同上 | `evs_upgrade` | 下载 + 校验 + 切换 + 健康检查 |
| `evs/v1/upgrade/evt/progress` | 事件 | `evs_upgrade` | 云 / 本地状态汇聚 | 阶段与百分比 |
| `evs/v1/upgrade/evt/done` | 事件 | `evs_upgrade` | 同上 | 终态成功（含已是最新） |
| `evs/v1/upgrade/evt/failed` | 事件 | `evs_upgrade` | 同上 | 终态失败（含原因码） |

QoS：cmd 用 **QoS 1**（至少一次，命令幂等）；evt 用 **QoS 1**（终态必须到），progress 可降为 QoS 0 省流量。

### 9.2 Payload（统一带 `v` / `ts` / `src`，便于多版本共存）

```json
// cmd/apply
{ "v": 1, "ts": 1763000000000, "src": "cloud",
  "action": "apply", "version": "1.4.2",
  "url": "https://ota.example.com/rk3588/evs-app-1.4.2-rk3588.tar.gz",
  "force": false }

// evt/progress
{ "v": 1, "ts": 1763000012000, "src": "upgrade",
  "phase": "downloading", "percent": 42, "from": "1.4.1", "to": "1.4.2" }

// evt/done
{ "v": 1, "ts": 1763000300000, "src": "upgrade",
  "result": "done", "from": "1.4.1", "to": "1.4.2", "duration_ms": 288000 }

// evt/failed
{ "v": 1, "ts": 1763000300000, "src": "upgrade",
  "result": "failed", "phase": "verifying", "code": "E_SIG",
  "rolled_back": false, "message": "signature mismatch" }
```

字段规则：
- `ts` 是毫秒 Unix 时间；`src` 固定 `"upgrade"`（evt）/ `"cloud"` 或 `"comm"`（cmd），用于 `evs_comm` 侧统一回调里区分来源。
- `percent` 只在 `downloading` / `staging` 出现；`from`/`to` 在所有事件里都带（便于云侧对账）。
- **不认识的多余字段必须忽略**（向前兼容），缺必填字段视为坏包丢弃并记日志。

### 9.3 原因码表（`code`）

| code | 含义 | 典型触发 |
| --- | --- | --- |
| `E_NET` | 网络不可达 / 超时 | 源站关停、DNS 失败、无外网 |
| `E_HTTP` | HTTP 非 200 / 重定向异常 | 404、500、源站鉴权失败 |
| `E_HASH` | `payload_sha256` 不符 | 传输损坏、包被替换 |
| `E_SIG` | 签名不符 / 公钥缺失 | 篡改、公钥装错 |
| `E_SCHEMA` | 清单字段缺失或类型错 | 版本不兼容、打包脚本 bug |
| `E_HW` | `target.hw` 不符 | 下发了别的板型包 |
| `E_ABI` | `target.abi` 不符 | 系统层被换过 |
| `E_DOWNGRADE` | 版本未递增 | 降级攻击 / 重放 |
| `E_MIN_FROM` | 源版本低于 `min_from` | 跨大版本跳跃 |
| `E_SPACE` | 磁盘不足 | 水位检查失败 |
| `E_APPLY` | 解包/权限/切换失败 | 文件系统只读、路径穿越被拦 |
| `E_HEALTH` | 新版本不健康 | 不出帧、进程崩溃、崩溃日志新增 |
| `E_ROLLBACK` | 回滚失败 | `previous` 也起不来 → 恢复模式 |
| `E_BUSY` | 已有升级在进行 | 并发 `apply` |
| `E_RECOVERY` | 处于恢复模式 | 需人工介入或 `force:true` |
| `E_CONFIG` | `upgrade.json` 非法 | 启动期即失败 |

### 9.4 与 `evs_comm` 的边界

- `evs_upgrade` **自带**一个 MQTT 通道（照 `main_comm.cpp` 的写法建 `CommConfig`），不依赖 `evs_comm` 存活——否则通信进程一挂，升级与回滚上报全断。
- 云侧命令默认发到 broker，`evs_upgrade` 直接订阅 `evs/v1/upgrade/cmd/#`；`evs_comm` 只做**状态汇聚与转发**，不参与升级决策。
- 两条链路的重复上报可接受（幂等），但**不要**让 `evs_comm` 代为发 `apply`——多一跳就多一个失败点。

## 10. 配置 `config/upgrade.json`

### 10.1 示例（首期默认值）

```json
{
  "_comment": "evs_upgrade 配置；字段语义见 docs/design/DESIGN_UPGRADE.md §10.2",
  "install_root": "/opt/edgevision",
  "data_root": "/data/edgevision",
  "release": { "keep_versions": 2, "free_space_min_mb": 256 },

  "source": {
    "type": "http",
    "base_url": "https://ota.example.com/rk3588/",
    "manifest_name": "manifest.json",
    "signature_name": "manifest.sig",
    "timeout_sec": 30,
    "retry": 3
  },

  "verify": { "public_key": "/etc/evs/ota_ed25519.pub", "allow_downgrade": false },

  "health": {
    "rtsp_url": "rtsp://127.0.0.1:8550/live/ch0",
    "probe_timeout_sec": 10,
    "min_fps": 15,
    "stable_sec": 60
  },

  "rollback": { "boot_count_max": 3 },
  "services": { "media": "evs-media.service" },

  "comm": {
    "channel_name": "upgrade",
    "protocol": "mqtt",
    "host": "127.0.0.1",
    "port": 1883,
    "client_id": "evs-upgrade",
    "sub_topic": "evs/v1/upgrade/cmd/#",
    "pub_topic": "evs/v1/upgrade/evt/"
  }
}
```

### 10.2 字段表

| 字段 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `install_root` | string | `/opt/edgevision` | 版本目录根（`releases/`、`current`、`previous`、`.staging`） |
| `data_root` | string | `/data/edgevision` | 现场配置 / 下载缓存 / 状态 / 日志（升级不覆盖） |
| `release.keep_versions` | int | 2 | 除当前版外保留几个历史版本（含 `previous`） |
| `release.free_space_min_mb` | int | 256 | 下载前最小剩余空间，低于即 `E_SPACE` |
| `source.type` | string | `http` | `http`（用 `curl`）· `file`（本地/U 盘导入，仍验签） |
| `source.base_url` | string | — | 目录型源站；清单与包都在其下 |
| `source.timeout_sec` / `retry` | int | 30 / 3 | 单次超时与重试次数（指数退避 1s/2s/4s） |
| `verify.public_key` | string | `/etc/evs/ota_ed25519.pub` | 公钥路径；缺失 → 拒绝一切升级（`E_SIG`），不降级为"跳过验签" |
| `verify.allow_downgrade` | bool | `false` | 仅调试用；生产必须 `false` |
| `health.rtsp_url` | string | — | 健康检查用的取流地址（走本地回环，避免占外网带宽） |
| `health.probe_timeout_sec` | int | 10 | `ffprobe` 子进程超时 |
| `health.min_fps` | int | 15 | 低于即判不健康 |
| `health.stable_sec` | int | 60 | 连续健康多久才 COMMIT |
| `rollback.boot_count_max` | int | 3 | 启动失败阈值（与 §7.2 一致） |
| `services.media` | string | `evs-media.service` | 被升级的 unit 名（停止/启动/查询都用它） |
| `target.hw` | string | `rk3588` | **本机期望值**：清单 `target.hw` 与它不等即 `E_HW`（防止下发别的板型包） |
| `target.abi` | string | `glibc-2.35` | 本机期望 ABI；**配空字符串 = 不校验**（只在开发机调试时这么用） |
| `source.curl_bin` | string | `curl` | 方案 A 用的可执行名/绝对路径（精简镜像里可能不在 `PATH`） |
| `comm.protocol` | string | `mqtt` | `mqtt`（建通道收发命令/事件）· `none`（**只打印不联网**，离线调试与 T7 冒烟用；evt 子主题依赖 MQTT，所以 UDP 不支持） |
| `comm.*` | — | 见示例 | 与 `config/comm.json` 的字段风格保持一致，便于共用解析代码 |

> 配置读取失败（文件不存在/JSON 语法错）→ 进程 `return 1` 并打印候选路径，**不要**用内置默认值静默启动：升级进程参数错了比不启动更危险。

## 11. 构建与运行集成

### 11.1 `EVS_PROCESS` 增加 `upgrade`（✅ 已落地，2026-11-15）

`src/main/CMakeLists.txt` 里原来对 `upgrade` 直接抛错的分支已换成正常选择：

| 位置 | 实际改动（已完成） |
| --- | --- |
| 变量区 | 增加 `set(EVS_WITH_UPGRADE FALSE)`，`EVS_PROCESS` 的 help 文案补 `upgrade` |
| `all` 分支 | 增加 `set(EVS_WITH_UPGRADE TRUE)`（默认 all 现在编三个进程） |
| 原 `FATAL_ERROR` 分支 | 换成 `set(EVS_WITH_UPGRADE TRUE)` |
| 错误提示 / `没有选中任何进程` / `STATUS` | 都带上 `upgrade` |
| 文件末尾 | `add_subdirectory(upgrade)`（与 `media`/`comm` 同位置） |

实测：`cmake -S . -B out -DEVS_PROCESS=upgrade` → `media=FALSE comm=FALSE upgrade=TRUE`，
产物 `out/bin/evs_upgrade`（`media`/`comm` 不编）。

### 11.2 两个 `CMakeLists.txt`（✅ 已落地）

`src/main/upgrade/CMakeLists.txt` —— 照 `src/main/comm/CMakeLists.txt` 的骨架：

```cmake
add_executable(evs_upgrade main_upgrade.cpp upgrade_service.cpp)
target_link_libraries(evs_upgrade PRIVATE common_api comm_api upgrade_api)
target_link_options(evs_upgrade PRIVATE -rdynamic)   # 崩溃栈要能显示函数名
if(NOT EVS_WITH_UPGRADE)
    set_target_properties(evs_upgrade PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()
```

`src/upgrade/CMakeLists.txt` —— 新库 `upgrade_api`，**唯一新增的外部依赖是 OpenSSL 的 crypto**：

```cmake
# 默认把 OPENSSL_ROOT_DIR 指到 install/<平台>（板端 3.5.4），避免命中系统 3.0.2
if(NOT DEFINED OPENSSL_ROOT_DIR AND EXISTS "${PROJECT_SOURCE_DIR}/install/${TARGET_PLATFORM}/include/openssl/opensslv.h")
    set(OPENSSL_ROOT_DIR "${PROJECT_SOURCE_DIR}/install/${TARGET_PLATFORM}" CACHE PATH "...")
endif()
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
add_library(upgrade_api SHARED upgrade_types.cpp manifest.cpp verifier.cpp
                            fetcher.cpp slot_manager.cpp health_check.cpp)
target_link_libraries(upgrade_api PUBLIC common_api OpenSSL::Crypto)
# configure 时打印命中的头/库与版本；头不在 install/ 下会 message(WARNING)
```

✅ **头号落地风险已消除（2026-11-15 实测）**：`make` 时 configure 输出
`-- upgrade_api: OpenSSL 3.5.4 (include=…/install/rk3588/include, crypto=…/lib/libcrypto.so)`；
进程启动与 `--status` 再打印 `OpenSSL 编译期 / 运行期` 两行（`verifier.h` 暴露），
两者不一致即「头库错配」——不用等到联调才炸。

### 11.3 Makefile 增量（✅ 已落地，薄包装不改语义）

| 改动 | 实际内容 |
| --- | --- |
| 变量 | `UPGRADE ?= evs_upgrade`、`UPGRADE_CFG ?= ../config/upgrade.json` |
| 目标 | `upgrade`（只编）、`run-upgrade`（`cd $(BUILD) && $(RUN_ENV) exec ./bin/$(UPGRADE) $(UPGRADE_CFG) $(ARGS)`） |
| `.PHONY` | 已加 `upgrade run-upgrade` |
| 帮助/信息 | `help` 与头部注释补齐；`proc` 注释的可选值补 `upgrade` |

一次性动作走 `ARGS`：`make run-upgrade ARGS="--status"` / `ARGS="--check --dry-run"` /
`ARGS="--verify=/tmp/manifest.json"`。

### 11.4 systemd unit（草案：两个新文件 + 一个既有文件改动；✅ 实际落地为 5 个，见本节末「已落地差异」）

`scripts/deploy/evs-upgrade.service`：

```ini
[Unit]
Description=EdgeVisionService Upgrade Agent
After=network-online.target evs-media.service
Wants=network-online.target

[Service]
Type=simple
User=root
WorkingDirectory=/opt/edgevision/current
ExecStart=/opt/edgevision/current/bin/evs_upgrade /data/edgevision/config/upgrade.json
Restart=on-failure
RestartSec=5
LimitCORE=infinity
Environment=EVS_CRASH_DIR=/data/edgevision/logs/crash
# 写权限收窄到两个目录；其余文件系统只读
ProtectSystem=strict
ReadWritePaths=/opt/edgevision /data/edgevision
ProtectHome=yes
PrivateTmp=yes

[Install]
WantedBy=multi-user.target
```

`scripts/deploy/evs-rollback.service`（兜底，`Type=oneshot`）：

```ini
[Unit]
Description=EdgeVisionService Auto Rollback (triggered by evs-media failure)

[Service]
Type=oneshot
ExecStart=/opt/edgevision/current/bin/evs_upgrade --rollback --reason=start-limit
LimitCORE=infinity
```

既有 `evs-edgevision.service` 需加三行（只加不动原有内容）：`OnFailure=evs-rollback.service`、`StartLimitIntervalSec=300`、`StartLimitBurst=3`，以及 `ExecStartPost=/opt/edgevision/current/bin/evs_upgrade --on-media-started`。

> ⚠️ `ProtectSystem=strict` 与 `systemctl` 的 D-Bus 访问在某些 systemd 版本上会互相干扰。P3 联调第一步就要验证；若 `systemctl` 报 `Access denied`，先去掉该行定位，再决定用 `ReadWritePaths=/run/dbus` 还是放宽。

> ✅ **已落地（2026-11-15，`scripts/deploy/`）与上面草案的差异**（以文件为准，本节保留草案便于看推理）：
> 1. 新增聚合 target **`evs.target`**（`Wants=evs-media evs-comm evs-upgrade` + `After=` 同三者，`WantedBy=multi-user.target`）；三个 service 的 `[Install]` 因此改成 `WantedBy=evs.target`，开机只 `enable evs.target`；
> 2. 补充 **`evs-comm.service`**（草案里只有媒体与升级）；`evs-upgrade.service` 保留 `After=evs-media.service` 但**不硬依赖**（媒体挂了升级仍要能上报）；
> 3. `StartLimitIntervalSec` / `StartLimitBurst` 实测**必须放 `[Unit]`**（systemd 249 下放 `[Service]` 被静默忽略并打 `Unknown key`）；
> 4. `ProtectSystem` 首期用 **`full`**，`strict` 留到 P3 验证过 `systemctl` 再换（理由见上面 ⚠️ 与本文件 `evs-upgrade.service` 末尾注释）；
> 5. `OnFailure=evs-rollback.service` **已写但注释**：骨架期 `--rollback` 一定以 `E_NOTIMPL` 失败，P4 打开；`evs-rollback.service` 刻意不写 `[Install]`（防开机自动回滚）；
> 6. `ExecStartPost=… --on-media-started` **未落**（`evs_upgrade` 已自带该子命令，接线放 P4 与 `boot_count` 一起做）；
> 7. 占位符 `@@EVS_ROOT@@` / `@@EVS_DATA@@` / `@@EVS_USER@@` / `@@EVS_PLATFORM@@` 由 `scripts/deploy/install.sh` 渲染（`Environment=LD_LIBRARY_PATH=$ROOT/...` 在 unit 里**不展开** `$ROOT`，必须渲染成绝对路径）。

## 12. 实施计划（P0–P5，合计 13 人日）

| 阶段 | 内容 | 人日 | 产出 | 验收（可执行） |
| --- | --- | --- | --- | --- |
| **P0 骨架** | `src/main/upgrade/` + `src/upgrade/` 空实现、CMake/Makefile、`config/upgrade.json`、`InstallCrashHandler` 接入、MQTT 通道建立、状态机空跑（`--dry-run` 打印迁移） | 2 | `evs_upgrade` 可执行 | `make upgrade && make run-upgrade` 起得来；发一条 `cmd/check` 能收到 `evt/progress` |
| **P1 清单与验签** | `manifest` + `verifier`（SHA-256 + Ed25519）、`--verify <pkg>` 离线模式、`scripts/tests/t7_upgrade.cpp`（仿 `t6_comm` 风格） | 2.5 | 可离线验证任意包 | 篡改 1 字节 → `E_HASH`；坏签名 → `E_SIG`；缺字段 → `E_SCHEMA`；t7 全 PASS |
| **P2 下载与暂存** | `fetcher`（`curl` 子进程 + 超时 + 断点）、`slot_manager` 解包/水位/幂等 | 2.5 | 包能落到 `.staging` | 断网 → `E_NET`；404 → `E_HTTP`；空间不足 → `E_SPACE`；重复 apply 幂等 |
| **P3 切换与回滚** | ACTIVATING / ROLLING_BACK、systemd 调用白名单、`boot_count`、`flock` | 3 | 能切能回 | 人为让新版本启动失败 → 60 s 内自动回滚；并发 apply → `E_BUSY` |
| **P4 健康检查与自愈** | `health_check`、恢复模式、`evs-rollback.service` | 2 | 不健康自动回滚 | 换一个"能起但不出帧"的假版本 → `E_HEALTH` 回滚 |
| **P5 打包与联调** | `scripts/ota/make_package.sh`（打包+签名）、部署脚本、文档回写、TESTLOG L3 | 1 | 端到端可交付 | 20 次连续升级 + 10 次随机断电（ROADMAP §8） |

依赖关系：`P0 → P1 → P2 → P3 → P4 → P5` 严格串行（每阶段都依赖前一阶段的产物）。若人力 >1，可把 P1 的单测与 P2 的 fetcher 并行。

## 13. 测试计划

### 13.1 用例矩阵

| ID | 场景 | 操作 | 期望结果 | 留痕 |
| --- | --- | --- | --- | --- |
| U1 | 正常升级 | 源站放 1.4.2 合法包，`cmd/apply` | `progress` 五阶段 → `done`，RTSP 正常，`current` 指向 1.4.2 | L2 |
| U2 | 已是最新 | 重复 apply 同版本 | 立即 `done`（幂等），不重启服务 | L1 |
| U3 | 包体被篡改 | 翻转包内 1 字节 | `E_HASH`，`current` 不变 | L2 |
| U4 | 清单被篡改 | 改 `manifest.json` 里 `version` | `E_SIG`，连解包都不做 | L2 |
| U5 | 公钥不匹配 | 换一把公钥 | `E_SIG` | L1 |
| U6 | 降级包 | 下发 1.3.0 | `E_DOWNGRADE` | L1 |
| U7 | 硬件/ABI 不符 | 清单写 `hw: rk3568` / `abi: glibc-2.31` | `E_HW` / `E_ABI` | L1 |
| U8 | 断网 | 断开外网后 apply | `E_NET`，进度停在 downloading，现网继续出帧 | L2 |
| U9 | 源站 404 | 清单在、包不在 | `E_HTTP` | L1 |
| U10 | 磁盘满 | 填满 `/data` 后 apply | `E_SPACE`（下载前就拦），不留残包 | L2 |
| U11 | 路径穿越 | 包里塞 `../../etc/passwd` | 解包拒绝 → `E_APPLY`，`/etc` 未变 | L2 |
| U12 | 激活时断电 | 切换瞬间拔电 ×10 | 重启后要么旧版要么新版，**都能出帧** | L3 |
| U13 | 新版本不出帧 | 假版本（编码器关闭） | `E_HEALTH` + 自动回滚，`failed.rolled_back=true` | L3 |
| U14 | 新版本起不来 | 假版本 `main` 立即 `return 1` | `StartLimitBurst` 触发 → `evs-rollback` → 回滚 | L3 |
| U15 | 并发命令 | 同时发两条 apply | 第一条执行，第二条 `E_BUSY` | L2 |
| U16 | 回滚也失败 | 删掉 `previous` 指向 | `E_ROLLBACK` + 进恢复模式，日志有处置指引 | L3 |
| U17 | 连续 20 次升级 | 脚本循环 apply 1.4.1↔1.4.2 | 20 次全成功；`releases/` 里只留 `keep_versions` 个 | L3 |
| U18 | 升级升级器自身 | 包内 `evs_upgrade` 版本更高 | 本次流程用旧进程跑完，下次启动生效 | L2 |

### 13.2 断电测试的诚实做法

- **纯软件注入**（`scripts/ota/fault_inject.sh`）：在指定阶段给 `evs_upgrade` 发 `SIGKILL`，再拉起——能验证"状态落盘 + 幂等恢复"，**但不等于断电**（页缓存没丢）。
- **真断电**：P5 阶段用 GPIO 电源或直接拔电，随机时刻 ×10。只有这一项能验证 §6.3 的 `fsync(dirfd)` 是否真的写对了。
- 两类结果都要记，别用软件注入冒充断电测试——这是最容易自欺的一环。

### 13.3 TESTLOG 留痕字段（缺一不可）

每次 L2/L3 记录：`构建点`（git commit）· `硬件/系统`（`rk3588` + 内核版本）· `起始版本 → 目标版本` · `注入方式`（正常/杀进程/拔电）· `耗时` · `实际 code` · `结论`。

## 14. Docker 容器化可行性（实测评估）

### 14.1 实测事实

| 项 | 实测结果 |
| --- | --- |
| Docker 版本 | `29.5.3`（aarch64 原生） |
| 存储驱动 | `overlayfs` |
| cgroup | **v2**（`/sys/fs/cgroup/cgroup.controllers` 存在） |
| 镜像 / 容器 | 当前 **0 个**（未使用过） |
| 外网 | `registry-1.docker.io` **不可达**（无法 `docker pull`） |
| 内存 | 16 GB |

### 14.2 容器化能解决什么、不能解决什么

| 能做 | 做不到 |
| --- | --- |
| 依赖打包（省掉 `LD_LIBRARY_PATH` 与 `install/` 版本漂移） | **不能升级内核 / 驱动 / BSP**（L3 与容器无关） |
| 快速回滚（换 tag，秒级） | 不能替代 boot 阶段的回滚（容器化后还得靠 systemd 拉起） |
| 环境一致性（开发机 = 设备） | 无法在无外网、无本地 registry 的现场拉镜像 |
| 进程隔离（只到 namespace 级） | **不能降低崩溃取证能力**（`core_pattern` 是宿主全局的，容器内看不到） |

### 14.3 搬到本项目的 12 个具体坑

| # | 坑 | 后果 | 规避 |
| --- | --- | --- | --- |
| 1 | MPP 硬件编码需要 `/dev/mpp_service` | 容器内编码直接失败 | `--device /dev/mpp_service` |
| 2 | DRM / DMA 需要 `/dev/dri/*`、`/dev/dma_heap/*` | `mmap` 失败、出绿屏 | 逐个 `--device` 透传 |
| 3 | V4L2 采集需要 `/dev/video*` | 无采集 | 透传，且 `--group-add video` |
| 4 | RTSP/RTP 端口范围随机 | 只映射 554 收不到流 | **`--network host`**（唯一省事方案） |
| 5 | `core_pattern` 宿主全局 | 容器内 core 落到宿主任意路径或丢 | `--ulimit core=infinity` + `EVS_CRASH_DIR` 挂卷 + 宿主 `core_pattern` 指向可写目录 |
| 6 | DMA/CMA 内存不受 `-m` 限制 | 限了内存也照样 OOM 宿主 | 不用 `-m` 做保护，靠宿主监控 |
| 7 | 镜像 250–400 MB（含 FFmpeg + ZLM + 自研库） | 离线 `save/load` 包再大 3–5 倍 | 若必须离线，用 `docker save` 分卷 |
| 8 | systemd 与 `docker run` 的 restart 策略冲突 | 双重拉起 / 起两次 | 只留一侧：systemd `Restart=always` **或** docker `--restart`，不能都开 |
| 9 | 容器日志默认 `json-file` 无上限 | 磁盘被写满 | `--log-opt max-size=50m --log-opt max-file=3` |
| 10 | 时区 / `localtime` 不继承 | 日志时间戳错 8 小时 | 挂 `-v /etc/localtime:/etc/localtime:ro` |
| 11 | **升级 agent 不能进容器** | 升级时容器自杀，断电即失联 | agent 留宿主，容器只承载业务 |
| 12 | overlayfs 写时复制 | 高频写日志性能下降 | 日志/录像目录用 volume 挂到 `/data` |

### 14.4 结论

| 问题 | 结论 |
| --- | --- |
| 首期用 Docker 做升级载体？ | **不。** 无外网、无本地 registry → 只能离线导入 250–400 MB 镜像且无法灰度；比目录 A/B 慢一个数量级 |
| Docker 能否替代 A/B？ | **不能。** 内核/驱动仍要靠系统级方案，容器只覆盖应用层，而应用层用目录 A/B 已经够用且更轻 |
| 什么时候值得上容器？ | 出现"多版本并存 / 多租户 / 依赖冲突"需求，且现场有本地 registry（或至少稳定内网）时 |
| 若将来容器化，形态是什么？ | **混合**：agent（`evs_upgrade`）留宿主管镜像 tag 与回滚；业务进程在容器内，`--network host` + 设备全透传；崩溃取证与 core 仍归宿主 |

## 15. 风险与对策

| 风险 | 影响 | 对策 |
| --- | --- | --- |
| 现场没有外网 / 源站不可达 | 升级完全不可用 | `source.type=file`（U 盘/内网共享）同一套验签；文档写明离线流程 |
| `/opt` 与 `/data` 不同分区 | `rename` 失去原子性 → 断电可能两边都不完整 | 部署时检查 `df`，或把 `.staging` 固定放 `/opt` 下（§4.2） |
| `find_package(OpenSSL)` 命中系统 3.0.2 而非 `install/` 的 3.5.4 | 头/库与运行时错配，`EVP_PKEY_ED25519` 行为异常或链接失败 | P1 首日验证；显式 `-DOPENSSL_ROOT_DIR=install/rk3588` 或自定义查找（§11.2） |
| systemd `ProtectSystem=strict` 挡住 `systemctl` | 升级无法停/起媒体服务 | P3 首日验证；失败则改 `ReadWritePaths=/run/dbus` 或放宽 |
| 媒体进程退出慢（编码队列未清） | `stop` 超 5 s，切换窗口拉长 | 复用现有有序停止逻辑；必要时 `TimeoutStopSec=10` + `SIGKILL` 兜底 |
| 健康检查误判（首帧偶发慢） | 好版本被回滚，升级成功率虚低 | `stable_sec` 与 `probe_timeout_sec` 可调；失败重试 1 次再判 |
| 现场配置与新版默认值结构不兼容 | 升级后配置解析失败 → 起不来 | `manifest.target.min_from` 拦住跨大版本；配置解析失败时**回落到包内默认值**并告警（而不是拒绝启动） |
| 升级器自身有 bug 导致连坏两版 | 设备变砖 | `boot_count_max` + `OnFailure` 兜底 + 恢复模式（§7.5）；`previous` 永不被自动删除 |

## 16. 待确认项（需要决策，决定后本文转「已定稿」）

| # | 待确认 | 备选 | 本文默认 | 影响面 |
| --- | --- | --- | --- | --- |
| 1 | 首期范围 | 只做 L1 / 直接上 L3 | **只做 L1** | L3 需 U-Boot 与分区控制权，工作量翻 3 倍 |
| 2 | U-Boot / 分区是否可控 | 可控 / 不可控 / 未知 | 未知 → 不做 L3 | 决定 L3 是否可行、要交 SWUpdate 还是 RAUC |
| 3 | 包源通道 | 自建 HTTPS / 内网共享目录 / U 盘 | HTTPS（内置 `curl`），`file` 作为兜底 | 决定 `fetcher` 的实现量（方案 A 还是 B） |
| 4 | 是否强制容器化 | 必须 / 不必须 / 以后再说 | **不必须** | 容器化会把工期从 13 人日拉到 30+，且要先解决离线镜像分发 |
| 5 | RTSP 中断预算 | 秒级（<5 s）/ 分钟级 / 不允许 | 秒级（`systemctl stop` + `start`） | 不允许中断就只能"双实例蓝绿"，端口/设备会冲突，需另设计 |
| 6 | 安装根目录统一 | `/opt/edgevision`（ROADMAP）/ `/opt/EdgeVisionService`（现有 unit） | `/opt/edgevision` | 影响所有 systemd unit 与部署脚本，越早定越好 |
| 7 | 现场配置迁移策略 | `/data` 优先（本文）/ 包内覆盖 / 版本化迁移脚本 | `/data` 优先 + 回落默认 | 决定"升级是否可能改掉现场参数" |

> **2026-11-15 骨架落地后的状态**：#1（只做 L1）、#3（方案 A：系统 `curl` 子进程，`source.type=http`）、#6（`/opt/edgevision`）已按「本文默认」实现；#2（U-Boot/分区）、#4（容器化）、#5（RTSP 中断预算 → 决定 ACTIVATING 能不能停服务）、#7（现场配置 `/data` 优先的回落逻辑）**仍需决策**，后两项直接影响 P3 的实现细节。

## 17. 变更记录

| 日期 | 变更 | 说明 |
| --- | --- | --- |
| 2026-11-15 | 初稿 | L1 应用级 A/B + Ed25519 签名清单 + `boot_count` 自动回滚；Docker 评估结论为"首期不用"；13 人日 P0–P5 排期；7 项待确认 |
| 2026-11-15 | 事实校准 | 板端实测更正三处：① `zstd 1.4.8` **存在**（原写"未确认"）；② OpenSSL 是**双版本**（`install/` 3.5.4 vs 系统 3.0.2）→ 升为头号构建风险；③ `ffprobe` 只在 `3rdparty/rk3588/ffmpeg/bin/`、不在 `PATH` |
| 2026-11-15 | **骨架落地** | 新增 `src/upgrade/`（6 组源文件 → `libupgrade_api.so`）+ `src/main/upgrade/`（`evs_upgrade`）+ `config/upgrade.json` + `scripts/tests/{t7_upgrade.cpp,run_t7_upgrade.sh}`；`EVS_PROCESS` 支持 `upgrade`；**签名口径改为 `pkeyutl -rawin`（不预哈希）**并写进 §5.4；§10.2 补 `target.*` / `source.curl_bin` / `comm.protocol`；§11 改为「已落地」；验收 146 单元 + 13 CLI 全 PASS（TESTLOG §5.25） |
| 2026-11-15 | **§11.4 systemd 落地（5 个 unit）** | `scripts/deploy/` 新增 `evs.target`（聚合）+ `evs-media` / `evs-comm` / `evs-upgrade` / `evs-rollback`.service + `install.sh`（渲染 `@@…@@`、`install`/`status`/`uninstall`、`--dry-run`、`systemd-analyze verify`）；`StartLimit*` 移入 `[Unit]`；`ProtectSystem=full` 首期；`OnFailure` 注释待 P4。§11.4 末尾追加「已落地差异」7 条；实测 `--dry-run` 5/5 渲染 + verify 0 告警、**未实装**（TESTLOG §5.26） |

> 骨架已落地（2026-11-15），剩余工作见 §12 的 P2–P5：解包/逐文件校验、`systemctl` 停起、ffprobe 帧率、旧版本清理、掉电轮转恢复、systemd unit 与 `OnFailure` 联调。

[返回设计索引](README.md) · [返回 ROADMAP](ROADMAP.md) · [返回文档总索引](../README.md) · [接手规范](../HANDOFF.md)








