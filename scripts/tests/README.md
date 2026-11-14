# scripts/tests/ —— 可重跑的回归 / 验收脚本

> 定位：**不需要摄像头、不需要 RTSP 服务、不需要推流**就能验证逻辑正确性的脚本 + 一个 C++ 用例。
> 结果必须写进 [../../docs/history/TESTLOG.md](../../docs/history/TESTLOG.md)（L2 小节），脚本本身不保存历史。

## 1. 清单

| 脚本 | 覆盖 | 断言数 | 硬件依赖 | 记录 |
| --- | --- | --- | --- | --- |
| `run_t1_zlm_rtsp_key.sh` | T1：`rtsp://` URL 生成与 ZLMediaKit `MediaInfo::key` 一致性（`vhost/app/stream` 拼接规则） | 18 | 需 ZLM 库（`3rdparty`） | TESTLOG §5.5 |
| `run_t2_node_config.sh` | T2：`node.json` 解析 + 校验（缺字段 / 类型错 / 重复 channel / 端口冲突 / fps=0） | 53 | 无 | TESTLOG §5.6 |
| `run_t3_node_json.sh` | T3：`config/node.json` v1 全量自检（JSON 语法 + `ffprobe` 探测素材分辨率/帧率/编码，缺失则跳过实测比对） | 104 | `ffprobe`（可选） | TESTLOG §5.7 |
| `run_t5_builder.sh` | T5：`NodeFactory` 按 `type` 造节点 + `PipelineBuilder` 按配置建链路（**只 build 不 start**） | 64 | 无 | TESTLOG §5.8 |

配套文件：

```text
scripts/tests/
├── t1_zlm_rtsp_key.cpp  t2_node_config.cpp  t3_node_json.cpp  t5_builder.cpp   ← 断言用 C++（各自链接被测库）
├── run_t1_zlm_rtsp_key.sh  run_t2_node_config.sh  run_t3_node_json.sh  run_t5_builder.sh
└── fixtures/            ← T2/T3 的输入样例（正向 1 个 + 反向 6 个）
    ├── node_v1_ok_ref.json           正向基线
    ├── node_v1_missing_type.json     缺 type → 应报错
    ├── node_v1_broken.json           语法/结构破坏 → 应报错
    ├── node_v1_dup_channel.json      重复 channel → 应报错
    ├── node_v1_dup_node_name.json    重复 node name → 应报错
    ├── node_v1_fps_zero.json         fps=0 → 应报错
    └── node_v1_port_conflict.json    两个 pipeline 抢同一 rtsp 端口 → 应报错
```

## 2. 跑法

```bash
# 全部（逐个跑，任一失败即停）
for t in scripts/tests/run_t*.sh; do echo "== $t"; bash "$t" || break; done

# 单个；输出落盘再读（否则终端截断，且拿不到退出码）
bash scripts/tests/run_t5_builder.sh > /tmp/evs_t5.txt 2>&1; echo rc=$?; tail -30 /tmp/evs_t5.txt

# T5 换配置
bash scripts/tests/run_t5_builder.sh config/node.json

# T3 指定中间产物目录
OUT_DIR=/tmp/t3 bash scripts/tests/run_t3_node_json.sh
```

判据：**退出码 0 且末行 `ALL ... PASS`**；`rc!=0` 时不看输出先看第一个 `FAIL` 行。

## 3. 已知不一致（必须知道，别被误导）

T1/T2/T3/T5 四个脚本都**硬编码历史构建目录 `build/`**（`cmake --build build`、`-L build/lib`、`-rpath "$ROOT/build/lib"`），而仓库默认构建目录已改为 `out/`。

- 后果：全新 clone 后直接跑会失败（`build/lib` 不存在）。
- 临时办法：先 `cmake -S . -B build && cmake --build build -j"$(nproc)"` 造出 `build/`，再跑脚本。
- 正确做法（待办）：脚本内统一成 `BUILD_DIR="${BUILD_DIR:-out}"` 并替换 `build` 字样，属独立小任务，改完需重跑四套并更新 TESTLOG。

## 4. 新增一个验收脚本的模板

```bash
#!/usr/bin/env bash
# T<N> 验收脚本：<被测对象>
# 用法：./scripts/tests/run_t<N>_<topic>.sh
# 记录见 docs/history/TESTLOG.md §5.<x>（L2）
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

OUT_DIR="${OUT_DIR:-$(mktemp -d)}"; mkdir -p "$OUT_DIR"
# 1) 构建被测目标  2) 编译用例  3) 跑正向 + 反向  4) 打印 PASS/FAIL 汇总

echo "ALL T<N> CHECKS PASS"
```

三条要求：① `set -euo pipefail`；② 反向用例（坏输入必须报错）至少和正向一样多；③ 末行固定 `ALL ... PASS`（便于 grep）。
