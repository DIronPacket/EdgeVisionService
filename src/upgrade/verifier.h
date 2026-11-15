#ifndef EVS_UPGRADE_VERIFIER_H
#define EVS_UPGRADE_VERIFIER_H

#include <cstddef>
#include <string>

namespace evs {
namespace upgrade {

// 编译期 / 运行期实际使用的 OpenSSL 版本（`OPENSSL_VERSION_TEXT` 与
// `OpenSSL_version(OPENSSL_VERSION)`）。
//
// 为什么单独暴露：install/<平台> 里是 3.5.4，系统自带是 3.0.2
// （见 docs/design/DESIGN_UPGRADE.md §11.2 的头号风险）。启动时把两行都打出来，
// 一眼就能发现「用 3.0.2 的头编、运行期却加载 3.5.4」这种错配。
const char *OpenSslCompileTimeVersion();
const char *OpenSslRuntimeVersion();

// SHA-256 → 小写十六进制（长度固定 64）。data 可以为空（size==0 时按空消息处理）。
bool Sha256Bytes(const void *data, size_t size, std::string *hex);
bool Sha256File(const std::string &path, std::string *hex, std::string *err);

// 十六进制比较（大小写不敏感；长度不同直接 false）
bool HexEqual(const std::string &a, const std::string &b);

// Ed25519 验签（§5.4）。message 是**原始字节**：
//
//   发布侧：openssl pkeyutl -sign -inkey ota_ed25519.pem -rawin \
//                      -in manifest.json -out manifest.sig
//   设备侧：本函数
//
// 口径说明：Ed25519 内部已对消息做 SHA-512，这里**不再自己套一层 sha256**
// （先 sha256 再签属于「预哈希」方案，需要签名侧也照做；用标准 pkeyutl 打包时
// 两边口径会对不上，是最常见的「验签永远失败」根因）。
//
// 公钥支持两种格式：
//   1) PEM SubjectPublicKeyInfo —— `openssl pkey -pubout` 的产物（推荐）
//   2) 32 字节裸 Ed25519 公钥 —— `openssl pkey -pubout -outform DER | tail -c 32`
bool VerifyEd25519(const std::string &pubkey_path, const std::string &message,
                   const std::string &signature, std::string *err);

// 便捷封装：验 manifest.sig 对 manifest.json **原始文件字节**的签名。
// 任何一步失败（文件读不到 / 公钥缺失 / 签名不符）都返回 false —— 公钥缺失时
// 绝不降级为「跳过验签」（§10.2）。
bool VerifyManifestSignature(const std::string &pubkey_path,
                             const std::string &manifest_path,
                             const std::string &sig_path, std::string *err);

}  // namespace upgrade
}  // namespace evs

#endif  // EVS_UPGRADE_VERIFIER_H
