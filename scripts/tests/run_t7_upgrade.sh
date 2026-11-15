#!/usr/bin/env bash
# ============================================================================
# T7 验收脚本：升级库（upgrade_api）+ evs_upgrade 一次性动作
#
# 用法：./scripts/tests/run_t7_upgrade.sh
#   BUILD 可改构建目录（默认 out，与仓库当前默认一致）。
#
# 特点：**全本机**。临时目录里造 releases/数据/密钥，不碰 /opt、不联网、
#       不需要 root、不需要 MQTT broker（配置里 protocol=none）。
#       会真实生成 Ed25519 密钥对并签名一份清单，因此验签是"真验"。
#
# 覆盖：
#   1. test_upgrade 的 9 组单元断言（状态码/清单/兼容性/哈希/验签/槽位/取数/RTSP）
#   2. evs_upgrade 的一次性动作：--status / --check / --verify / --apply(--dry-run)
#      / --rollback / --on-media-started / 未知选项 / 配置缺失（退出码 + 输出关键字）
#   3. 骨架诚实性：--apply 必须停在解包桩并以 E_NOTIMPL 收尾（不许假装成功）
#
# 记录见 docs/history/TESTLOG.md（L2 · T7 升级库）。
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

BUILD="${BUILD:-out}"
OPENSSL="${OPENSSL:-openssl}"
PLATFORM="${PLATFORM:-rk3588}"

echo "== 1/6 generate（构建目录：${BUILD}）=="
cmake -S . -B "${BUILD}" > /dev/null

echo "== 2/6 编译 upgrade_api + test_upgrade + evs_upgrade =="
cmake --build "${BUILD}" --target test_upgrade evs_upgrade -j"$(nproc)"

export LD_LIBRARY_PATH="${BUILD}/lib:${ROOT}/install/${PLATFORM}/lib:${LD_LIBRARY_PATH:-}"

TMP="$(mktemp -d /tmp/evs_t7_script_XXXXXX)"
cleanup() { rm -rf "${TMP}"; }
trap cleanup EXIT

echo "== 3/6 生成临时 Ed25519 密钥对 + 签名清单（临时目录 ${TMP}）=="
"${OPENSSL}" version
"${OPENSSL}" genpkey -algorithm ED25519 -out "${TMP}/ota.key" 2>/dev/null
"${OPENSSL}" pkey -in "${TMP}/ota.key" -pubout -out "${TMP}/ota.pub" 2>/dev/null
"${OPENSSL}" pkey -in "${TMP}/ota.key" -pubout -outform DER 2>/dev/null \
    | tail -c 32 > "${TMP}/ota.pub.raw"

cat > "${TMP}/manifest.json" <<'JSON'
{
  "schema": 1,
  "version": "1.4.2",
  "git_commit": "422b2c4",
  "built_at": "2026-11-20T02:10:00Z",
  "target": { "hw": "rk3588", "abi": "glibc-2.35", "min_from": "1.3.0" },
  "payload_sha256": "9f2c8e1d0b3a4c5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6",
  "size_bytes": 1024,
  "files": [
    { "path": "bin/evs_media", "sha256": "1111111111111111111111111111111111111111111111111111111111111111", "mode": "0755" }
  ],
  "rollback": { "keep_versions": 2, "boot_count_max": 3 }
}
JSON

"${OPENSSL}" pkeyutl -sign -inkey "${TMP}/ota.key" -rawin \
    -in "${TMP}/manifest.json" -out "${TMP}/manifest.sig"
cp "${TMP}/manifest.sig" "${TMP}/manifest.json.sig"   # --verify 的默认签名名
SIG_BYTES="$(stat -c %s "${TMP}/manifest.sig")"
if [ "${SIG_BYTES}" != "64" ]; then
    echo "[FAIL] 签名应为 64 字节，实际 ${SIG_BYTES}"
    exit 1
fi
echo "[PASS] 生成密钥对与 64 字节签名"

# ---------- 临时配置：全部路径落在 TMP 下，protocol=none 不连 broker ----------
SRC="${TMP}/src"
mkdir -p "${SRC}"
cp "${TMP}/manifest.json" "${SRC}/manifest.json"
cp "${TMP}/manifest.sig"  "${SRC}/manifest.sig"

cat > "${TMP}/upgrade.json" <<JSON
{
  "install_root": "${TMP}/opt",
  "data_root": "${TMP}/data",
  "release": { "keep_versions": 2, "free_space_min_mb": 1 },
  "source": {
    "type": "file",
    "base_url": "${SRC}",
    "manifest_name": "manifest.json",
    "signature_name": "manifest.sig",
    "timeout_sec": 5,
    "retry": 1
  },
  "verify": { "public_key": "${TMP}/ota.pub", "allow_downgrade": false },
  "target": { "hw": "rk3588", "abi": "glibc-2.35" },
  "health": {
    "rtsp_url": "rtsp://127.0.0.1:1/live/ch0",
    "ffprobe_path": "",
    "probe_timeout_sec": 2,
    "min_fps": 15,
    "stable_sec": 1
  },
  "rollback": { "boot_count_max": 3 },
  "services": { "media": "evs-media.service" },
  "comm": {
    "protocol": "none",
    "channel_name": "upgrade",
    "host": "127.0.0.1",
    "port": 1883,
    "client_id": "evs-upgrade-t7",
    "sub_topic": "evs/v1/upgrade/cmd/#",
    "pub_topic": "evs/v1/upgrade/evt/"
  }
}
JSON

echo "== 4/6 单元用例（test_upgrade）=="
"${BUILD}/bin/test_upgrade" "${TMP}/ota.pub" "${TMP}/ota.pub.raw" \
    "${TMP}/manifest.json" "${TMP}/manifest.sig"


echo "== 5/6 evs_upgrade 一次性动作 =="
BIN="${BUILD}/bin/evs_upgrade"
CFG="${TMP}/upgrade.json"
FAIL=0

# run_case <名字> <期望退出码> <输出里必须出现的字符串（空=不查）> <参数...>
run_case() {
    local name="$1" want="$2" pattern="$3"
    shift 3
    local out rc
    out="$("${BIN}" "${CFG}" "$@" 2>&1)" && rc=0 || rc=$?
    if [ "${rc}" != "${want}" ]; then
        echo "[FAIL] ${name}: exit=${rc}，期望 ${want}"
        echo "${out}"
        FAIL=1
        return
    fi
    if [ -n "${pattern}" ] && ! printf '%s' "${out}" | grep -q -- "${pattern}"; then
        echo "[FAIL] ${name}: 输出里没有 '${pattern}'"
        echo "${out}"
        FAIL=1
        return
    fi
    echo "[PASS] ${name}（exit=${rc}）"
}

# --help 不读配置，单独跑
if "${BIN}" --help > /dev/null 2>&1; then
    echo "[PASS] --help（exit=0）"
else
    echo "[FAIL] --help 退出码非 0"; FAIL=1
fi

run_case "--status（空槽位）"          0 "install_root"     --status
run_case "--check 首装发现新版本"      0 "发现新版本 1.4.2" --check
run_case "--on-media-started 无状态"   0 "忽略"            --on-media-started
run_case "--rollback 没有 previous"    2 "没有 previous"    --rollback
run_case "--verify 正确签名"           0 "签名 OK"          --verify="${TMP}/manifest.json"
run_case "--verify 签名文件缺失"        2 "验签失败"         --verify="${TMP}/manifest.json" --sig="${TMP}/nope.sig"
run_case "--apply --dry-run 停在解包桩" 2 "E_NOTIMPL"       --apply --dry-run
run_case "未知选项被拒"                1 "不认识的选项"      --bogus

# 硬件不符：改清单 → 必须用私钥重签（否则先被 E_SIG 拦下，验不到 E_HW）
sed 's/"hw": "rk3588"/"hw": "rk3566"/' "${TMP}/manifest.json" > "${SRC}/manifest.json"
"${OPENSSL}" pkeyutl -sign -inkey "${TMP}/ota.key" -rawin \
    -in "${SRC}/manifest.json" -out "${SRC}/manifest.sig"
run_case "target.hw 不符 → E_HW"       2 "E_HW"            --check

# 签名与内容不匹配：只改内容、不重签
sed 's/"version": "1.4.2"/"version": "1.4.3"/' "${SRC}/manifest.json" > "${TMP}/forged.json"
cp "${TMP}/forged.json" "${SRC}/manifest.json"
run_case "清单被篡改 → E_SIG"          2 "E_SIG"           --check

# 源站没有清单 → E_NET
rm -f "${SRC}/manifest.json" "${SRC}/manifest.sig"
run_case "源站缺清单 → E_NET"          2 "E_NET"           --check

# 配置路径不存在 → 退出码 1（不使用内置默认值）
if "${BIN}" "${TMP}/no-such-config.json" --status > /dev/null 2>&1; then
    echo "[FAIL] 配置文件不存在时竟然返回 0"; FAIL=1
else
    echo "[PASS] 配置文件不存在 → exit=1（不静默用默认值）"
fi

echo "== 6/6 落盘痕迹检查 =="
STATE="${TMP}/data/state/upgrade.json"
if [ -f "${STATE}" ]; then
    echo "[PASS] 状态文件已落盘: ${STATE}"
    if grep -q '"phase"' "${STATE}"; then
        echo "[PASS] 状态文件含 phase 字段"
    else
        echo "[FAIL] 状态文件缺 phase"; FAIL=1
    fi
else
    echo "[FAIL] 状态文件没生成（${STATE}）"; FAIL=1
fi
if [ -d "${TMP}/opt/releases" ] && [ -d "${TMP}/opt/.staging" ]; then
    echo "[PASS] 目录布局: opt/releases + opt/.staging"
else
    echo "[FAIL] 目录布局不完整"; FAIL=1
fi

if [ "${FAIL}" != "0" ]; then
    echo "==== T7：存在 FAIL ===="
    exit 1
fi
echo "==== T7：全部 PASS ===="
