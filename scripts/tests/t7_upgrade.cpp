// ============================================================
// T7 验收：升级库（upgrade_api）
//
// 覆盖（全部本机、临时目录，不碰 /opt、不联网、不需要 root）：
//   1. 状态码映射：Phase / ErrorCode ↔ 字符串、枚举值冻结、IsTerminal
//   2. semver 比较与非法版本拒绝
//   3. manifest 解析：正常路径 + 每一条错误路径（schema/version/target/sha256/
//      size_bytes/路径穿越/绝对路径/files 类型/rollback 范围）
//   4. 兼容性判定：hw / abi / 降级 / 同版本 / min_from / allow_downgrade
//   5. SHA-256 已知向量（空串、"abc"）+ 文件摘要 + 大小写不敏感比较
//   6. Ed25519 验签：缺公钥 / 长度不对 → 失败；给了密钥对（argv）→ 正负例都验
//   7. 槽位：目录布局、原子换链（current/previous）、状态文件往返、原子写文件
//   8. 取数：file 源拷贝 + .part 落盘 + 缺文件时的重试与失败分类
//   9. RTSP URL 解析 + 端口探活（真起一个 listen socket）
//
// 运行：./scripts/tests/run_t7_upgrade.sh
//   （脚本会生成临时 Ed25519 密钥对并签名一份清单，再以 argv 传进来）
// 记录见 docs/history/TESTLOG.md（L2 · T7 升级库）。
// ============================================================

#include <arpa/inet.h>
#include <ftw.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "fetcher.h"
#include "health_check.h"
#include "manifest.h"
#include "slot_manager.h"
#include "upgrade_types.h"
#include "verifier.h"

namespace {

int g_fail = 0;

void Check(bool ok, const std::string &what) {
  std::cout << (ok ? "[PASS] " : "[FAIL] ") << what << std::endl;
  if (!ok) ++g_fail;
}

std::string TempRoot() {
  char buf[256];
  std::snprintf(buf, sizeof(buf), "/tmp/evs_t7_%ld",
                static_cast<long>(::getpid()));
  return std::string(buf);
}

int RemoveEntry(const char *path, const struct stat *, int, struct FTW *) {
  ::remove(path);
  return 0;
}

void RemoveTree(const std::string &path) {
  ::nftw(path.c_str(), RemoveEntry, 16, FTW_DEPTH | FTW_PHYS);
}

bool WriteText(const std::string &path, const std::string &text) {
  std::ofstream ofs(path.c_str(), std::ios::out | std::ios::binary);
  if (!ofs.is_open()) return false;
  ofs << text;
  return ofs.good();
}

bool Exists(const std::string &path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
}

// 一份合法清单（下面用字符串替换构造各种错误用例）
std::string GoodManifestText() {
  return "{\n"
         "  \"schema\": 1,\n"
         "  \"version\": \"1.4.2\",\n"
         "  \"git_commit\": \"422b2c4\",\n"
         "  \"built_at\": \"2026-11-20T02:10:00Z\",\n"
         "  \"target\": { \"hw\": \"rk3588\", \"abi\": \"glibc-2.35\", "
         "\"min_from\": \"1.3.0\" },\n"
         "  \"payload_sha256\": "
         "\"9f2c8e1d0b3a4c5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6\",\n"
         "  \"size_bytes\": 48219136,\n"
         "  \"files\": [\n"
         "    { \"path\": \"bin/evs_media\", \"sha256\": "
         "\"1111111111111111111111111111111111111111111111111111111111111111\", "
         "\"mode\": \"0755\" },\n"
         "    { \"path\": \"lib/libcomm_api.so\", \"sha256\": "
         "\"2222222222222222222222222222222222222222222222222222222222222222\" }\n"
         "  ],\n"
         "  \"rollback\": { \"keep_versions\": 3, \"boot_count_max\": 5 }\n"
         "}\n";
}


void TestPhaseAndErrorCodes() {
  std::cout << "---- 1. 状态码映射 ----" << std::endl;
  Check(std::string(evs::upgrade::ToString(evs::upgrade::Phase::kHealthChecking)) ==
            "HEALTH_CHECKING",
        "ToString(kHealthChecking) == HEALTH_CHECKING");
  Check(std::string(evs::upgrade::PhaseToPayload(
            evs::upgrade::Phase::kHealthChecking)) == "health_checking",
        "PhaseToPayload 是小写（上报 JSON 用）");

  // 状态文件里存的是数字：这些值**不能再变**，否则老状态文件会被解释成别的阶段
  Check(static_cast<int>(evs::upgrade::Phase::kIdle) == 0 &&
            static_cast<int>(evs::upgrade::Phase::kChecking) == 1 &&
            static_cast<int>(evs::upgrade::Phase::kDownloading) == 2 &&
            static_cast<int>(evs::upgrade::Phase::kVerifying) == 3 &&
            static_cast<int>(evs::upgrade::Phase::kStaging) == 4 &&
            static_cast<int>(evs::upgrade::Phase::kActivating) == 5 &&
            static_cast<int>(evs::upgrade::Phase::kHealthChecking) == 6 &&
            static_cast<int>(evs::upgrade::Phase::kCommitted) == 7 &&
            static_cast<int>(evs::upgrade::Phase::kRollingBack) == 8 &&
            static_cast<int>(evs::upgrade::Phase::kFailed) == 9 &&
            static_cast<int>(evs::upgrade::Phase::kRecovery) == 10,
        "Phase 枚举值按设计冻结（0..10，只能追加不能重排）");

  evs::upgrade::Phase parsed = evs::upgrade::Phase::kIdle;
  Check(evs::upgrade::ParsePhase("rolling_back", &parsed) &&
            parsed == evs::upgrade::Phase::kRollingBack,
        "ParsePhase 认得小写 rolling_back");
  Check(evs::upgrade::ParsePhase("DOWNLOADING", &parsed) &&
            parsed == evs::upgrade::Phase::kDownloading,
        "ParsePhase 认得大写 DOWNLOADING");
  Check(!evs::upgrade::ParsePhase("bogus", &parsed), "ParsePhase 拒绝未知字符串");

  Check(evs::upgrade::IsTerminal(evs::upgrade::Phase::kCommitted) &&
            evs::upgrade::IsTerminal(evs::upgrade::Phase::kFailed) &&
            evs::upgrade::IsTerminal(evs::upgrade::Phase::kRecovery) &&
            !evs::upgrade::IsTerminal(evs::upgrade::Phase::kDownloading),
        "IsTerminal 只认 COMMITTED / FAILED / RECOVERY");

  // 原因码必须与 §9.3 表逐字一致（云侧按字符串归类，拼错等于丢告警）
  Check(std::string(evs::upgrade::ToString(evs::upgrade::ErrorCode::kNet)) == "E_NET" &&
            std::string(evs::upgrade::ToString(evs::upgrade::ErrorCode::kHttp)) ==
                "E_HTTP" &&
            std::string(evs::upgrade::ToString(evs::upgrade::ErrorCode::kHash)) ==
                "E_HASH" &&
            std::string(evs::upgrade::ToString(evs::upgrade::ErrorCode::kSig)) ==
                "E_SIG" &&
            std::string(evs::upgrade::ToString(evs::upgrade::ErrorCode::kSchema)) ==
                "E_SCHEMA" &&
            std::string(evs::upgrade::ToString(evs::upgrade::ErrorCode::kHw)) == "E_HW" &&
            std::string(evs::upgrade::ToString(evs::upgrade::ErrorCode::kAbi)) ==
                "E_ABI" &&
            std::string(evs::upgrade::ToString(
                evs::upgrade::ErrorCode::kDowngrade)) == "E_DOWNGRADE" &&
            std::string(evs::upgrade::ToString(evs::upgrade::ErrorCode::kMinFrom)) ==
                "E_MIN_FROM" &&
            std::string(evs::upgrade::ToString(evs::upgrade::ErrorCode::kSpace)) ==
                "E_SPACE" &&
            std::string(evs::upgrade::ToString(evs::upgrade::ErrorCode::kApply)) ==
                "E_APPLY" &&
            std::string(evs::upgrade::ToString(evs::upgrade::ErrorCode::kHealth)) ==
                "E_HEALTH" &&
            std::string(evs::upgrade::ToString(evs::upgrade::ErrorCode::kRollback)) ==
                "E_ROLLBACK" &&
            std::string(evs::upgrade::ToString(evs::upgrade::ErrorCode::kBusy)) ==
                "E_BUSY" &&
            std::string(evs::upgrade::ToString(evs::upgrade::ErrorCode::kRecovery)) ==
                "E_RECOVERY" &&
            std::string(evs::upgrade::ToString(evs::upgrade::ErrorCode::kConfig)) ==
                "E_CONFIG",
        "ErrorCode 16 个正式码与 §9.3 表逐字一致");
  Check(std::string(evs::upgrade::ToString(evs::upgrade::ErrorCode::kNotImpl)) ==
            "E_NOTIMPL",
        "骨架期临时码 E_NOTIMPL 存在（P2 补齐后必须删除）");

  Check(evs::upgrade::NowMs() > 1600000000000LL, "NowMs 返回毫秒时间戳");
  Check(!evs::upgrade::FormatTimeMs(evs::upgrade::NowMs()).empty(),
        "FormatTimeMs 能格式化");
}

void TestVersionCompare() {
  std::cout << "---- 2. semver 比较 ----" << std::endl;
  Check(evs::upgrade::CompareVersion("1.4.2", "1.4.1") == 1, "1.4.2 > 1.4.1");
  Check(evs::upgrade::CompareVersion("1.4.10", "1.4.9") == 1,
        "1.4.10 > 1.4.9（按数字比，不是字符串比）");
  Check(evs::upgrade::CompareVersion("2.0.0", "1.99.99") == 1, "2.0.0 > 1.99.99");
  Check(evs::upgrade::CompareVersion("1.4.2", "1.4.2") == 0, "同版本 == 0");
  Check(evs::upgrade::CompareVersion("1.3.0", "1.4.1") == -1, "1.3.0 < 1.4.1");
  Check(evs::upgrade::CompareVersion("1.4", "1.4.1") == 2,
        "两段版本号被判非法（返回 2）");
  Check(evs::upgrade::CompareVersion("1.4.2-rc1", "1.4.2") == 2,
        "带后缀的版本被拒（首期只认 MAJOR.MINOR.PATCH）");
  Check(evs::upgrade::CompareVersion("a.b.c", "1.0.0") == 2, "非数字版本被拒");
}


// 无 files / rollback 的最小合法清单：结构类错误用例都在它后面拼
std::string BaseNoFiles() {
  return "{ \"schema\": 1, \"version\": \"1.4.2\", \"git_commit\": \"abc\", "
         "\"built_at\": \"2026-11-20T02:10:00Z\", "
         "\"target\": {\"hw\": \"rk3588\", \"abi\": \"glibc-2.35\", "
         "\"min_from\": \"1.3.0\"}, "
         "\"payload_sha256\": "
         "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\", "
         "\"size_bytes\": 1024";
}

std::string ReplaceOnce(const std::string &text, const std::string &from,
                        const std::string &to) {
  std::string out = text;
  const size_t pos = out.find(from);
  if (pos != std::string::npos) out.replace(pos, from.size(), to);
  return out;
}

void TestManifestParse() {
  std::cout << "---- 3. manifest 解析 ----" << std::endl;
  using evs::upgrade::ErrorCode;

  evs::upgrade::Manifest m;
  ErrorCode code = ErrorCode::kNone;
  std::string err;
  Check(evs::upgrade::ParseManifest(GoodManifestText(), &m, &code, &err),
        "合法清单解析成功");
  Check(m.version == "1.4.2" && m.git_commit == "422b2c4" &&
            m.hw == "rk3588" && m.abi == "glibc-2.35" &&
            m.min_from == "1.3.0" && m.size_bytes == 48219136,
        "字段取值正确");
  Check(m.files.size() == 2 && m.files[0].path == "bin/evs_media" &&
            m.files[0].mode == "0755" && m.files[1].mode.empty(),
        "files[] 解析（缺 mode 时为空）");
  Check(m.keep_versions == 3 && m.boot_count_max == 5, "rollback 段覆盖默认值");

  const std::string round = evs::upgrade::ManifestToJson(m);
  evs::upgrade::Manifest m2;
  Check(evs::upgrade::ParseManifest(round, &m2, &code, &err) &&
            m2.version == m.version && m2.files.size() == m.files.size() &&
            m2.keep_versions == m.keep_versions,
        "ManifestToJson → ParseManifest 往返一致（release 内留档用）");

  // ---- 字段类错误：每一条都必须被拒，且错误串能指到具体字段 ----
  const std::string good = GoodManifestText();
  Check(!evs::upgrade::ParseManifest(
            ReplaceOnce(good, "\"schema\": 1", "\"schema\": 2"), &m2, &code, &err) &&
            code == ErrorCode::kSchema && err.find("schema=2") != std::string::npos,
        "schema=2 → E_SCHEMA，提示不认识该格式版本");
  Check(!evs::upgrade::ParseManifest(
            ReplaceOnce(good, "\"version\": \"1.4.2\"", "\"version\": \"1.4\""), &m2,
            &code, &err) &&
            code == ErrorCode::kSchema,
        "version 两段 → E_SCHEMA");
  Check(!evs::upgrade::ParseManifest(
            ReplaceOnce(good, "  \"git_commit\": \"422b2c4\",\n", ""), &m2, &code,
            &err) &&
            code == ErrorCode::kSchema && err.find("git_commit") != std::string::npos,
        "缺 git_commit → E_SCHEMA（错误串带字段名）");
  Check(!evs::upgrade::ParseManifest(
            ReplaceOnce(good, "\"abi\": \"glibc-2.35\", ", ""), &m2, &code, &err) &&
            code == ErrorCode::kSchema && err.find("target.abi") != std::string::npos,
        "缺 target.abi → E_SCHEMA（错误串带 target.abi）");
  Check(!evs::upgrade::ParseManifest(
            ReplaceOnce(good,
                        "9f2c8e1d0b3a4c5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6",
                        "zz2c8e1d0b3a4c5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6"),
            &m2, &code, &err) &&
            code == ErrorCode::kSchema &&
            err.find("payload_sha256") != std::string::npos,
        "payload_sha256 非十六进制 → E_SCHEMA");
  Check(!evs::upgrade::ParseManifest(
            ReplaceOnce(good, "\"size_bytes\": 48219136", "\"size_bytes\": 0"), &m2,
            &code, &err) &&
            code == ErrorCode::kSchema && err.find("size_bytes") != std::string::npos,
        "size_bytes=0 → E_SCHEMA（下载前就要能判）");
}


void TestManifestStructure() {
  std::cout << "---- 3b. manifest 结构与路径安全 ----" << std::endl;
  using evs::upgrade::ErrorCode;
  evs::upgrade::Manifest m;
  ErrorCode code = ErrorCode::kNone;
  std::string err;

  Check(evs::upgrade::ParseManifest(BaseNoFiles() + "}", &m, &code, &err) &&
            m.files.empty() && m.keep_versions == 2 && m.boot_count_max == 3,
        "files / rollback 都是选填；缺省 keep_versions=2 boot_count_max=3");
  Check(!evs::upgrade::ParseManifest(BaseNoFiles() + ", \"files\": 7}", &m, &code,
                                     &err) &&
            code == ErrorCode::kSchema && err.find("files") != std::string::npos,
        "files 不是数组 → E_SCHEMA");
  Check(!evs::upgrade::ParseManifest(
            BaseNoFiles() +
                ", \"files\": [{\"path\": \"../etc/passwd\", \"sha256\": "
                "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}]}",
            &m, &code, &err) &&
            code == ErrorCode::kSchema && err.find("'..'") != std::string::npos,
        "files[].path 含 '..' → 拒绝（防解包穿越，§8.1）");
  Check(!evs::upgrade::ParseManifest(
            BaseNoFiles() +
                ", \"files\": [{\"path\": \"/etc/passwd\", \"sha256\": "
                "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}]}",
            &m, &code, &err) &&
            code == ErrorCode::kSchema && err.find("绝对路径") != std::string::npos,
        "files[].path 是绝对路径 → 拒绝");
  Check(!evs::upgrade::ParseManifest(
            BaseNoFiles() +
                ", \"files\": [{\"path\": \"bin/x\", \"sha256\": \"abc\"}]}", &m, &code,
            &err) &&
            code == ErrorCode::kSchema,
        "files[].sha256 长度不对 → E_SCHEMA");
  Check(!evs::upgrade::ParseManifest(BaseNoFiles() + ", \"rollback\": 5}", &m, &code,
                                     &err) &&
            code == ErrorCode::kSchema,
        "rollback 不是对象 → E_SCHEMA");
  Check(!evs::upgrade::ParseManifest("not json", &m, &code, &err) &&
            code == ErrorCode::kSchema,
        "非 JSON → E_SCHEMA（不抛异常）");
  Check(!evs::upgrade::ParseManifest("[]", &m, &code, &err) &&
            code == ErrorCode::kSchema,
        "根节点不是对象 → E_SCHEMA");
  Check(evs::upgrade::ParseManifest(
            BaseNoFiles() + ", \"rollback\": {\"keep_versions\": 0}}", &m, &code,
            &err) &&
            m.keep_versions == 2,
        "rollback.keep_versions=0 越界 → 回落默认 2（不为配置细节整包作废）");
}

void TestCompatibility() {
  std::cout << "---- 4. 兼容性 / 防降级 ----" << std::endl;
  using evs::upgrade::ErrorCode;

  evs::upgrade::Manifest m;
  ErrorCode code = ErrorCode::kNone;
  std::string err;
  if (!evs::upgrade::ParseManifest(GoodManifestText(), &m, &code, &err)) {
    Check(false, "兼容性用例前置：合法清单解析失败");
    return;
  }

  evs::upgrade::ManifestContext ctx;
  ctx.hw = "rk3588";
  ctx.abi = "glibc-2.35";
  ctx.current_version = "1.4.1";
  Check(evs::upgrade::CheckManifest(m, ctx, &code, &err) && code == ErrorCode::kNone,
        "1.4.1 → 1.4.2 放行");

  ctx.hw = "rk3568";
  Check(!evs::upgrade::CheckManifest(m, ctx, &code, &err) && code == ErrorCode::kHw,
        "target.hw 不符 → E_HW");
  ctx.hw = "rk3588";

  ctx.abi = "glibc-2.31";
  Check(!evs::upgrade::CheckManifest(m, ctx, &code, &err) && code == ErrorCode::kAbi,
        "target.abi 不符 → E_ABI（换过 rootfs 的包）");
  ctx.abi = "glibc-2.35";

  ctx.current_version = "1.4.2";
  Check(!evs::upgrade::CheckManifest(m, ctx, &code, &err) &&
            code == ErrorCode::kDowngrade && err.find("相同") != std::string::npos,
        "同版本 → E_DOWNGRADE（上层按重复下发处理）");

  ctx.current_version = "1.5.0";
  Check(!evs::upgrade::CheckManifest(m, ctx, &code, &err) &&
            code == ErrorCode::kDowngrade,
        "降级 → E_DOWNGRADE");
  ctx.allow_downgrade = true;
  Check(evs::upgrade::CheckManifest(m, ctx, &code, &err),
        "allow_downgrade=true 时降级放行（仅调试用）");
  ctx.allow_downgrade = false;

  ctx.current_version = "1.2.0";
  Check(!evs::upgrade::CheckManifest(m, ctx, &code, &err) &&
            code == ErrorCode::kMinFrom,
        "源版本低于 min_from=1.3.0 → E_MIN_FROM");

  ctx.current_version = "";
  Check(evs::upgrade::CheckManifest(m, ctx, &code, &err),
        "首装（current 为空）不做版本比较，只校验 hw/abi");

  ctx.current_version = "1.4.1";
  ctx.abi = "";
  Check(evs::upgrade::CheckManifest(m, ctx, &code, &err),
        "abi 配空 = 不校验（开发机调试用）");
}


void TestCrypto(const std::string &root) {
  std::cout << "---- 5. SHA-256 ----" << std::endl;
  std::string hex_abc;
  std::string hex_empty;
  Check(evs::upgrade::Sha256Bytes("abc", 3, &hex_abc) &&
            hex_abc ==
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "sha256(\"abc\") 命中已知向量");
  Check(evs::upgrade::Sha256Bytes("", 0, &hex_empty) &&
            hex_empty ==
                "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "sha256(\"\") 命中已知向量");

  const std::string f = root + "/abc.txt";
  std::string err;
  Check(WriteText(f, "abc"), "写临时文件 abc.txt");
  std::string fhex;
  Check(evs::upgrade::Sha256File(f, &fhex, &err) && fhex == hex_abc,
        "Sha256File 与 Sha256Bytes(\"abc\") 一致");
  Check(!evs::upgrade::Sha256File(root + "/missing.txt", &fhex, &err),
        "文件不存在 → 返回 false（不崩、不抛）");
  Check(evs::upgrade::HexEqual("ABCDEF", "abcdef"), "HexEqual 大小写不敏感");
  Check(!evs::upgrade::HexEqual("abcd", "abce"), "HexEqual 内容不同 → false");
  Check(!evs::upgrade::HexEqual("ab", "abc"), "HexEqual 长度不同 → false");
}

// argv 传入的密钥/清单；缺省则只跑反例
void TestSignature(const std::string &pub_pem, const std::string &pub_raw,
                   const std::string &manifest_path, const std::string &sig_path) {
  std::cout << "---- 6. Ed25519 验签 ----" << std::endl;
  std::string err;

  if (!pub_pem.empty()) {
    Check(evs::upgrade::VerifyManifestSignature(pub_pem, manifest_path, sig_path,
                                                &err),
          "正例（PEM 公钥）：签名与清单匹配 → 通过");
    if (!pub_raw.empty())
      Check(evs::upgrade::VerifyManifestSignature(pub_raw, manifest_path, sig_path,
                                                  &err),
            "正例（32 字节裸公钥）：同一条签名 → 通过");

    std::string text;
    Check(evs::upgrade::ReadFileBytes(manifest_path, &text, &err), "读回清单原文");
    std::string sig;
    evs::upgrade::ReadFileBytes(sig_path, &sig, &err);

    std::string tampered = text;
    if (!tampered.empty())
      tampered[0] = (tampered[0] == 'x') ? 'y' : 'x';
    Check(!evs::upgrade::VerifyEd25519(pub_pem, tampered, sig, &err),
          "反例：清单被改一个字节 → 验签失败（整条信任链的根）");
    Check(!evs::upgrade::VerifyEd25519(pub_pem, text, std::string(64, 'x'), &err),
          "反例：签名内容不对 → 失败");
    Check(!evs::upgrade::VerifyEd25519(pub_pem, text, sig.substr(0, 32), &err) &&
              err.find("64") != std::string::npos,
          "反例：签名只有 32 字节 → 明确报「不是 64 字节」");
    Check(!evs::upgrade::VerifyManifestSignature(pub_pem, manifest_path,
                                                 manifest_path + ".nope", &err),
          "反例：签名文件不存在 → 失败");
  } else {
    std::cout << "[SKIP] 没给密钥/清单参数（run_t7_upgrade.sh 会自动生成）"
              << std::endl;
  }

  Check(!evs::upgrade::VerifyEd25519("/nonexistent/ota_ed25519.pub", "x",
                                     std::string(64, 'a'), &err),
        "公钥不存在 → 失败（绝不降级为「跳过验签」，§10.2）");
}


void TestSlots(const std::string &root) {
  std::cout << "---- 7. 槽位 / 原子写 ----" << std::endl;
  evs::upgrade::SlotConfig cfg;
  cfg.install_root = root + "/opt";
  cfg.data_root = root + "/data";
  evs::upgrade::SlotManager slots(cfg);
  std::string err;

  Check(slots.EnsureLayout(&err), "EnsureLayout 建出 releases/.staging/ota/state/logs");
  Check(Exists(slots.releases_dir()) && Exists(slots.staging_dir()) &&
            Exists(slots.download_dir()) && Exists(slots.logs_dir()),
        "四个关键目录都在");
  Check(slots.state_file() == root + "/data/state/upgrade.json",
        "状态文件路径 = data_root/state/upgrade.json（§4.2）");

  Check(evs::upgrade::MakeDirs(slots.release_dir("1.4.1"), &err) &&
            evs::upgrade::MakeDirs(slots.release_dir("1.4.2"), &err),
        "建两个 release 目录（模拟已安装 + 新解包）");

  std::string ver;
  Check(slots.ReadCurrentVersion(&ver, &err) && ver.empty(),
        "没有 current 指针 → 返回空串而不是报错（首装状态）");

  Check(slots.SwitchCurrent("1.4.1", true, &err), "首次切 current → 1.4.1");
  Check(slots.ReadCurrentVersion(&ver, &err) && ver == "1.4.1", "current = 1.4.1");
  std::string prev;
  Check(slots.ReadPreviousVersion(&prev, &err) && prev.empty(),
        "首次切换不写 previous（没有旧版本可指）");

  Check(slots.SwitchCurrent("1.4.2", true, &err), "切 current → 1.4.2（保留 previous）");
  Check(slots.ReadCurrentVersion(&ver, &err) && ver == "1.4.2", "current = 1.4.2");
  Check(slots.ReadPreviousVersion(&prev, &err) && prev == "1.4.1", "previous = 1.4.1");
  Check(!Exists(slots.current_link() + ".tmp"), "换链后不留 .tmp（rename 是原子的）");

  Check(!slots.SwitchCurrent("9.9.9", true, &err),
        "切到不存在的 release → 失败（不会留下坏指针）");
  Check(slots.ReadCurrentVersion(&ver, &err) && ver == "1.4.2",
        "失败后 current 仍是 1.4.2（原子性）");

  // ---- 状态文件往返（掉电恢复的唯一依据）----
  evs::upgrade::UpgradeState st;
  Check(slots.ReadState(&st, &err) && st.phase == evs::upgrade::Phase::kIdle,
        "没有状态文件 → 按 IDLE 处理（不是错误）");
  st.phase = evs::upgrade::Phase::kHealthChecking;
  st.from = "1.4.1";
  st.to = "1.4.2";
  st.boot_count = 2;
  st.percent = 42;
  Check(slots.WriteState(st, &err), "写状态（原子：tmp → fsync → rename → fsync dir）");
  evs::upgrade::UpgradeState st2;
  Check(slots.ReadState(&st2, &err) &&
            st2.phase == evs::upgrade::Phase::kHealthChecking &&
            st2.boot_count == 2 && st2.from == "1.4.1" && st2.to == "1.4.2" &&
            st2.percent == 42,
        "状态往返一致（phase/from/to/boot_count/percent）");
  Check(st2.updated_at_ms > 0, "状态里带 updated_at_ms");

  // ---- 原子写文件 ----
  const std::string f = root + "/data/logs/atomic.txt";
  Check(evs::upgrade::WriteFileAtomic(f, "hello", &err), "WriteFileAtomic 写文件");
  std::string text;
  Check(evs::upgrade::ReadFileBytes(f, &text, &err) && text == "hello", "内容正确");
  Check(evs::upgrade::WriteFileAtomic(f, "hello2", &err) &&
            evs::upgrade::ReadFileBytes(f, &text, &err) && text == "hello2",
        "覆盖写也走同一条原子路径");
  Check(!Exists(f + ".tmp"), "成功后不留 .tmp");

  Check(evs::upgrade::FreeSpaceMbOf(root) > 0, "FreeSpaceMbOf 能读到剩余空间");
  Check(evs::upgrade::DirName("/a/b/c") == "/a/b" &&
            evs::upgrade::DirName("a.txt") == "." &&
            evs::upgrade::DirName("/a") == "/",
        "DirName 边界（绝对 / 相对 / 单段）");

  // ---- 骨架桩：必须失败并给出 E_NOTIMPL，而不是假装成功 ----
  evs::upgrade::ErrorCode code = evs::upgrade::ErrorCode::kNone;
  Check(!slots.ExtractPackage(root + "/x.tar.gz", "1.4.2", &code, &err) &&
            code == evs::upgrade::ErrorCode::kNotImpl,
        "ExtractPackage 是桩：E_NOTIMPL（骨架期不许假装成功）");
  Check(!slots.VerifyStagedFiles("1.4.2", std::vector<evs::upgrade::ManifestFile>(),
                                 &code, &err) &&
            code == evs::upgrade::ErrorCode::kNotImpl,
        "VerifyStagedFiles 是桩：E_NOTIMPL");
  Check(!slots.PruneOldReleases(&err), "PruneOldReleases 是桩：返回 false + 说明");
  Check(!slots.ControlService("evs-media.service", "is-active", 5, &err),
        "ControlService 是桩：返回 false + 说明");
}


void TestFetcher(const std::string &root) {
  std::cout << "---- 8. 取数（file 源）----" << std::endl;
  const std::string src = root + "/src";
  const std::string dst = root + "/dst";
  std::string err;
  Check(evs::upgrade::MakeDirs(src, &err) && evs::upgrade::MakeDirs(dst, &err),
        "建源 / 目标目录");
  Check(WriteText(src + "/manifest.json", GoodManifestText()), "准备源文件");

  evs::upgrade::FetchConfig cfg;
  cfg.type = "file";
  cfg.base_url = src;
  cfg.retry = 1;  // 测试里不等退避
  evs::upgrade::Fetcher fetcher(cfg);

  std::string loc;
  Check(fetcher.Resolve("manifest.json", &loc, &err) &&
            loc == src + "/manifest.json",
        "Resolve 拼出 file 位置");
  Check(fetcher.Resolve("/abs/x", &loc, &err) && loc == "/abs/x",
        "绝对路径原样返回（cmd 里带显式 url）");

  std::string text;
  evs::upgrade::ErrorCode code = evs::upgrade::ErrorCode::kNone;
  Check(fetcher.FetchToString("manifest.json", &text, &code, &err) &&
            text.find("\"version\": \"1.4.2\"") != std::string::npos,
        "FetchToString 取到清单原文");
  Check(!Exists(src + "/manifest.json.part"), "取数后不留 .part");

  const std::string local_dst = dst + "/pkg.tar.gz";
  Check(fetcher.FetchToFile("manifest.json", local_dst, &code, &err) &&
            Exists(local_dst),
        "FetchToFile 拷贝到目标路径");
  Check(!Exists(local_dst + ".part"), "成功后 .part 已 rename 掉（无半成品）");

  const std::string missing_dst = dst + "/missing.bin";
  Check(!fetcher.FetchToFile("nope.bin", missing_dst, &code, &err) &&
            code == evs::upgrade::ErrorCode::kNet,
        "源文件不存在 → E_NET");
  Check(!Exists(missing_dst), "失败时不留下目标文件");

  evs::upgrade::FetchConfig empty_cfg;
  evs::upgrade::Fetcher f2(empty_cfg);
  Check(!f2.Resolve("a.json", &loc, &err),
        "base_url 为空 → Resolve 拒绝（不拼出相对垃圾路径）");
}

void TestRtsp() {
  std::cout << "---- 9. RTSP URL 与端口探活 ----" << std::endl;
  std::string host;
  std::string path;
  int port = 0;
  Check(evs::upgrade::ParseRtspUrl("rtsp://127.0.0.1:8550/live/ch0", &host, &port,
                                   &path) &&
            host == "127.0.0.1" && port == 8550 && path == "/live/ch0",
        "解析 rtsp://host:port/path");
  Check(evs::upgrade::ParseRtspUrl("rtsp://cam.local/live", &host, &port, &path) &&
            host == "cam.local" && port == 554,
        "省略端口 → 默认 554");
  Check(!evs::upgrade::ParseRtspUrl("http://127.0.0.1/x", &host, &port, &path),
        "非 rtsp:// → 拒绝");
  Check(!evs::upgrade::ParseRtspUrl("rtsp:///live", &host, &port, &path),
        "没有主机名 → 拒绝");

  // 真起一个 listen socket，验证探活能连通 / 断开后能判不可达
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  Check(fd >= 0, "创建监听 socket");
  if (fd < 0) return;
  sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;  // 让内核挑一个空闲端口
  bool ok = ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0 &&
            ::listen(fd, 1) == 0;
  socklen_t len = sizeof(addr);
  ok = ok && ::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) == 0;
  Check(ok, "监听 127.0.0.1 的临时端口");

  if (ok) {
    evs::upgrade::HealthConfig hc;
    hc.rtsp_url = "rtsp://127.0.0.1:" +
                  std::to_string(static_cast<int>(ntohs(addr.sin_port))) + "/live/ch0";
    evs::upgrade::HealthChecker checker(hc);
    const evs::upgrade::HealthResult r = checker.CheckPort(2);
    Check(r.port_alive && r.ok,
          "CheckPort：有监听 → 可达（" + r.detail + "）");

    ::close(fd);
    const evs::upgrade::HealthResult r2 = checker.CheckPort(2);
    Check(!r2.port_alive && !r2.ok,
          "CheckPort：端口关掉后 → 不可达（" + r2.detail + "）");
  } else {
    ::close(fd);
  }

  evs::upgrade::HealthConfig bad;
  bad.rtsp_url = "rtsp://127.0.0.1:1/live";
  evs::upgrade::HealthChecker c2(bad);
  Check(!c2.CheckPort(1).ok, "端口没人听 → 判不健康");
  Check(!c2.CheckFps().ok,
        "CheckFps 是桩：明确返回不健康 + 说明（不假装通过）");

  evs::upgrade::HealthConfig no_url;
  evs::upgrade::HealthChecker c3(no_url);
  const evs::upgrade::HealthResult r3 = c3.CheckPort(1);
  Check(!r3.ok && r3.detail.find("rtsp_url") != std::string::npos,
        "rtsp_url 为空/非法 → 明确指出是配置问题");
}

}  // namespace

int main(int argc, char **argv) {
  const std::string root = TempRoot();
  RemoveTree(root);  // 清掉上次崩溃残留
  std::string err;
  if (!evs::upgrade::MakeDirs(root, &err)) {
    std::cout << "[FAIL] 无法创建临时目录 " << root << ": " << err << std::endl;
    return 1;
  }

  std::cout << "临时目录: " << root << std::endl;
  std::cout << "OpenSSL 编译期: " << evs::upgrade::OpenSslCompileTimeVersion()
            << std::endl;
  std::cout << "OpenSSL 运行期: " << evs::upgrade::OpenSslRuntimeVersion()
            << std::endl;
  if (std::string(evs::upgrade::OpenSslCompileTimeVersion()) !=
      std::string(evs::upgrade::OpenSslRuntimeVersion())) {
    std::cout << "[WARN] OpenSSL 头与运行库版本不一致（DESIGN_UPGRADE §11.2）"
              << std::endl;
  }

  TestPhaseAndErrorCodes();
  TestVersionCompare();
  TestManifestParse();
  TestManifestStructure();
  TestCompatibility();
  TestCrypto(root);
  // argv: <pub.pem> <pub_raw.bin> <manifest.json> <manifest.sig>（缺省 → SKIP）
  TestSignature(argc > 1 ? argv[1] : "", argc > 2 ? argv[2] : "",
                argc > 3 ? argv[3] : "", argc > 4 ? argv[4] : "");
  TestSlots(root);
  TestFetcher(root);
  TestRtsp();

  std::cout << "==== 结果："
            << (g_fail == 0 ? "全部 PASS" : "存在 FAIL") << "（失败 " << g_fail
            << " 项）====" << std::endl;
  RemoveTree(root);
  return g_fail == 0 ? 0 : 1;
}
