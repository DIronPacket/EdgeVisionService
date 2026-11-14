# config/ —— 运行时配置（改这里不用重新编译）

> 程序启动时读配置：**有 `node.json` 就用它（v1 多链路），没有才回退 `push.json`（v0 单链路）**。
> 字段校验规则在 `src/common/configmgr/node_config_manager.cpp`，改完配置必跑 `scripts/tests/run_t3_node_json.sh`。

## 1. 目录内容

| 文件 | 是否入库 | 作用 |
| --- | --- | --- |
| `node.json` | ✅ | **主配置（v1）**：`global` 进程级资源 + `pipelines[]` 多链路（当前 ch0 摄像头 + ch1~ch3 三个 mp4） |
| `push.json` | ✅ | **兼容配置（v0）**：单链路 `vi → push(rtsp) → print`，`node.json` 不存在或被显式指定时才用 |
| `comm.json` | ✅ | **通信进程（`evs_comm`）的通道配置**：`channels[]`（`type` udp/mqtt · `role` server/client · `host`/`port`/`topic` …）。媒体进程不读它（见 §11） |
| `2.mp4` (33 MB) | ❌ 未跟踪 | 本地测试素材（推流/解码验证用），**不入库** |
| `test.nv12` (556 MB) | ❌ 未跟踪 | 本地 NV12 裸帧素材，**不入库** |

两个大文件只是本机素材：`git status` 里会一直显示为 `??`，**提交时不要 `git add -A`**（会把 GB 级文件塞进仓库）。

## 2. 生效顺序（`load_with_fallback()`）

```text
命令行给了路径（argv[1]）
   → 只用该路径，失败直接退出（不静默回退）
没给路径
   → 先找 config/node.json（v1）
   → 不存在再用 config/push.json（v0：合成一条单链路）
启动日志第一行会打印「配置来源: <路径>（v1 node.json / v0 push.json → 合成单链路）」
```

## 3. `node.json` 整体结构

```json
{
  "version": 1,
  "global":   { "log": {...}, "pool": {...}, "media": {...} },
  "pipelines": [
    { "name": "ch0", "channel": 0, "enable": true,
      "nodes": [ {"type":"vi","name":"vi","config":{...}}, ... ] }
  ]
}
```

| 段 | 含义 | 谁在读 |
| --- | --- | --- |
| `version` | schema 版本，当前固定 `1` | `NodeConfigManager`（判定 v1） |
| `global` | **进程级**资源，全进程一份，与链路数量无关 | 见 §4 |
| `pipelines[]` | 每条链路 = 一个独立采集实例 + 一条流水线 | `PipelineBuilder::build_all` |
| `_comment` | 任意段的注释字段（数组），解析器忽略 | 只给人看 |

## 4. `global` 段逐字段

| 字段 | 类型 | 当前值 | 是否已生效 | 说明 |
| --- | --- | --- | --- | --- |
| `log.level` | string | `info` | ⚠️ 未接线 | 预留给日志模块 |
| `log.file` | string | `""` | ⚠️ 未接线 | 空 = 标准输出 |
| `pool.blocks` | int | 64 | ❌ **未接线（T10a）** | 目标：池块数。**现状仍用 `MemInit()` 里的代码默认值**，启动日志会明确提示 |
| `pool.queue_depth` | int | 8 | ❌ **未接线（T10a）** | 目标：节点队列深度 |
| `pool.quota_per_channel` | int | 10 | ❌ **未接线（T10a）** | 目标：每路最多同时占用块数，超了只丢该路 |
| `pool.images[]` | array | `nv12 1920x1080` | ❌ **未接线（T10b）** | 目标：池内图像规格；现状块尺寸硬编码 1080p |
| `media.rtsp.port` | int | 8550 | ✅ | RTSP 服务端口，**只在 global 写一处**，节点里不再写 port |
| `media.rtsp.bind_ip` | string | `0.0.0.0` | ✅ | 监听地址 |
| `media.rtsp.app` | string | `live` | ✅ | 播放地址 `rtsp://<ip>:<port>/<app>/<stream>` 的第二段 |

> 「配置里写了 ≠ 已生效」是这份配置最容易误判的地方。`main.cpp` 会把未接线字段直接打出来，别拿 `pool.blocks=64` 当事实用。

## 5. `pipelines[]` 与节点

| 字段 | 类型 | 必填 | 校验 / 说明 |
| --- | --- | --- | --- |
| `name` | string | ✅ | 链路名（日志与错误定位用） |
| `channel` | int | ✅ | **每条链路唯一**；队列名 = 节点名 + channel，重名会串队列 |
| `enable` | bool | 否（默认 true） | `false` 时不建这条链路 |
| `nodes[]` | array | ✅ | **顺序即连线顺序**，拓扑规则见 §7 |

每个节点对象：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `type` | string | `vi` / `vpss` / `push` / `print`；未知 type → 该链构建失败（其余链继续跑） |
| `name` | string | 节点名，缺省 = `type`；同一链路内应唯一 |
| `config` | object | 该节点的参数（下表），解析后包成 `{"<type>":{...}}` 交给节点的 `set_data()` |

### 5.1 `vi`（采集）

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `src` | string | **必填**。前缀决定后端：`dev:/dev/videoN` → V4L2；`file:` / `rtsp://` / `rtmp://` / `http://` / `udp://` → FFmpeg |
| `width` / `height` | int | **必须等于素材实际分辨率**（FFmpeg 采集当前不缩放，不一致会花屏），且要与该路 `push.width/height` 一致 |
| `fps` | int | **必须接近素材实际帧率**（mp4 实测 23.976 → 写 24），偏高会让播放端时间戳走快 |
| `loop` | bool | 文件源**必须 `true`**，否则 mp4 播完该路静默停摆；`dev:` 不需要 |

### 5.2 `vpss`（预处理，可选）

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `enable` | bool | 当前 ch0 为 `false`（不启用缩放/转换） |
| `src_slot` / `out_slot` | string | 输入/输出槽位名（对应池内图像规格） |
| `dst_fmt` | string | 目标格式；空 = 不变 |
| `align` | int | 宽高对齐（默认 16） |

### 5.3 `push`（编码 + 推流）

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `enable` | bool | `false` 只跑采集与流水线，不推流 |
| `width` / `height` | int | 偶数；须与 `vi` 一致（FFmpeg 源不缩放） |
| `codec` | string | `h264` / `h265`（也接受 `avc` / `hevc` 别名） |
| `protocol` | string | `rtsp`（推荐）/ `tcp` / `udp` / `rtmp`（`tcp`/`udp` 只是 `rtsp` + `transport` 的简写；**rtmp 后端未实现**，会明确报错并跳过推流） |
| `transport` | string | `tcp` / `udp`；只记录进日志，实际由播放端 SETUP 决定 |
| `fps` | int | 编码帧率，同时决定推流时间戳间隔（`1000000/fps`） |
| `bitrate_kbps` | int | MPP 硬编目标码率（实际输出，不是声明值） |
| `gop` | int | **预留，未生效**（`EncoderInterface` 未暴露该参数） |
| `stream` | string | 播放地址最后一段，**每路唯一**（当前 `ch0`~`ch3`） |

### 5.4 `print`（链路终点 + 统计）

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `enable` | bool | `false` 不打印，但**仍负责把 block 回收进池** |
## 6. `push.json`（v0 兼容配置）

只有 `node.json` 不存在时才用；它会被**合成为一条单链路** `vi → push(name=rtsp) → print`。

| 段 | 字段 | 说明 |
| --- | --- | --- |
| `print` | `enable` / `interval` | 与 v1 的 `print` 节点语义相同 |
| `push` | `enable` `width` `height` `codec` `protocol` `transport` `port` `app` `stream` `fps` `bitrate_kbps` `gop` | 与 v1 的 `push` 节点字段相同，**但 `port` / `app` / `stream` 也写在这里**（v0 没有 `global` 段） |
| （隐含） | 采集源 | **`vi` 段不在本文件里**：`ParseV0` 从 `push` 段复制 `width`/`height`/`fps` 合成 `vi` 节点，采集源不写 → `ViNode` 走缺省 `/dev/video0`（V4L2）。这也是要迁移到 v1 的原因（T8 后删除本文件） |

注意：`push.json` 里的 `port` 与 `node.json` 的 `global.media.rtsp.port` 是**两套来源**，不要同时改两处而只改一个。

## 7. 拓扑规则（`PipelineBuilder` 校验，违反则该链构建失败）

| 规则 | 内容 |
| --- | --- |
| R1 | 每条链路**至少 2 个节点** |
| R2 | 首节点必须是 `vi` |
| R3 | 末节点必须是 `push` 或 `print` |
| R4 | `vi` 只能出现在首位（一条链只能有一个采集源） |
| R5 | `print` 只能出现在末位 |

错误信息带字段路径，例如 `pipelines[1].nodes[2].type: 缺失或为空`，直接对着配置改。

## 8. 改完配置怎么验

```bash
# 1) 结构与素材交叉校验（JSON 语法 + 字段 + ffprobe 探测 ch1~ch3 实际分辨率/帧率）
bash scripts/tests/run_t3_node_json.sh > /tmp/t3.txt 2>&1; echo rc=$?; tail -20 /tmp/t3.txt
#    期望末行 ALL ... PASS；若素材不存在，ffprobe 段会跳过而不是失败

# 2) 解析/校验规则回归（改了 node_config_manager.cpp 才必须跑）
bash scripts/tests/run_t2_node_config.sh

# 3) 起程序看配置是否真的生效（前 3 行日志就是配置来源与链路清单）
scripts/run/out/start.sh
#    [main] 配置来源: /mnt/nvme/workspace/EdgeVisionService/config/node.json（v1 node.json） / 链路 4 条
#    [main] global: rtsp 端口 8550 / app "live" / pool.blocks 64 / queue_depth 8
#    [main] 注意: pool.blocks / queue_depth / quota_per_channel 尚未接线（T10a），当前仍用 MemInit() 默认值
#   （若走的是 v0，多一行 [main] 提示: 当前走 v0 push.json 适配（合成单链路）…）
```

## 9. 常见配置错误 → 现象对照

| 配置错误 | 看到的现象 | 原因 |
| --- | --- | --- |
| `vi.width/height` 与素材不一致 | 画面花屏 / 撕裂 | FFmpeg 采集不缩放，按源尺寸写入块，推流按配置尺寸读 |
| `vi.fps` 配得比素材高 | 播放端"加速"、时间戳走快 | `PushNode` 用 `1000000/fps` 算帧间隔 |
| 文件源漏写 `"loop": true` | 播几分钟后该路无声无息停帧 | 文件读完即 EOF，采集不再产出 |
| 两条链路 `channel` 相同 | 串流 / 一路帧跑到另一路 / 卡死 | 队列名 = 节点名 + channel，重名即共用队列 |
| 两个 `push` 写不同 `port` | 启动第二个 RTSP 服务失败，日志有 `ZLM_ERR_*` | 端口只应在 `global.media.rtsp.port` 配一处 |
| `codec` 写成 `h264x` / `protocol` 写 `rtmp` | 该链构建或推流失败（其余链仍跑） | 取值白名单：`h264|h265(avc|hevc)`、`rtsp|tcp|udp` |
| 以为 `pool.blocks=64` 已生效 | 实际内存占用与配置对不上 | T10a 未接线，仍用代码默认值（启动日志有提示） |
| 以为 `gop` 有效 | 关键帧间隔没变化 | `EncoderInterface` 未暴露该参数，属预留 |

## 10. 素材（ch1~ch3 的 mp4）

- 当前指向 `/mnt/nvme/workspace/EdgeVisionService/build/file/test1.mp4 ~ test3.mp4`（**绝对路径、`build/` 不入库**）。
- `ffprobe` 实测：`hevc 1024x576 @23.976fps`，因此配置写 `1024x576 / fps 24`。
- 换机器 / 换素材：先 `ffprobe -v error -select_streams v:0 -show_entries stream=width,height,r_frame_rate -of csv=p=0 <文件>`，
  再**同时**改该路 `vi.width/height/fps` 与 `push.width/height/fps`（四处必须一致）。
- 素材缺失时程序不会崩：该链 `open` 失败、其余链继续跑（构建阶段失败只告警）。

## 11. `comm.json`（通信进程 `evs_comm` 的通道配置）

媒体进程（`evs_media` / `EdgeVisionService`）**不读这个文件**；只有 `evs_comm` 读它（`src/main/comm/main_comm.cpp`；候选路径 `config/comm.json` → `../config/comm.json`，也可 `argv[1]` 显式指定）。

```json
{
  "channels": [
    { "name": "cmd", "type": "mqtt", "role": "server",
      "host": "127.0.0.1", "port": 1883, "topic": "evs/v1/#",
      "keepalive_sec": 30, "reconnect_delay_ms": 1000, "recv_timeout_ms": 200 }
  ]
}
```

| 字段 | 取值 | 说明 |
| --- | --- | --- |
| `name` | 字符串 | 管理器内唯一名；空 = 自动 `udp-server-1` 式命名 |
| `type` | `udp` \| `mqtt` | **必填**，其余字段缺失即取默认值 |
| `role` | `server` \| `client` | 缺省 `client`。★ MQTT 的 `server` 是**订阅端角色**（订阅 `evs/v1/#`），不是 broker —— broker 是独立进程 `install/rk3588/sbin/mosquitto`（见 `src/comm/README.md` §3） |
| `host` / `port` | 字符串 / 0–65535 | UDP `client` 必填；MQTT 空 = `127.0.0.1`，`port` 0 = 1883 |
| `topic` / `topic_pub` | 字符串 | 订阅 / 发布主题；空值语义见 `src/comm/comm_types.h::CommConfig` |
| `client_id` / `username` / `password` | 字符串 | MQTT 连接参数；空 = 自动 id + 匿名 |
| `keepalive_sec` / `qos` / `retain` / `reconnect_delay_ms` | 整数 / 布尔 | `qos` 只接受 0/1/2（订阅固定 0）；断线按 `reconnect_delay_ms` 重连并重订阅 |
| `recv_timeout_ms` | 整数 | 单次等待超时，决定 `Stop()` 的响应延迟（100–500 合适） |
| `udp_broadcast` | 布尔 | UDP `server` 打开 `SO_BROADCAST` |

行为约定（实测口径见 [../docs/history/TESTLOG.md](../docs/history/TESTLOG.md) §5.22）：

- **语法/取值错误** → 打印 `[comm] 配置错误: channels[i]: …` 后退出码 1（快速失败，避免带错配置长跑）。
- 单个通道 `Create`/`Start` 失败 → 只告警，其余通道照跑（与流水线「单链失败只告警」同一策略）。
- 找不到配置文件 / 一个通道都没有 → **不退出**，打印试过的路径并等信号（systemd 下不会因 broker 没起来反复重启）。
- 改完怎么验：`make run-comm` → 期望 `[comm] 通道就绪 …` → `[comm] 1 个通道已启动` → `[comm] 运行中`；另一侧用 `mosquitto_pub` 或 `out/bin/test_comm` 发消息，能看到 `[comm] 收包 通道=… topic=… peer=… size=… body="…"`。
