#include "verifier.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <vector>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/opensslv.h>
#include <openssl/pem.h>

#include "manifest.h"  // ReadFileBytes

namespace evs {
namespace upgrade {
namespace {

void AppendSslError(std::string *err) {
  unsigned long e = ERR_get_error();
  if (e == 0 || err == nullptr) return;
  char buf[256];
  ERR_error_string_n(e, buf, sizeof(buf));
  *err += "（openssl: ";
  *err += buf;
  *err += "）";
}

std::string ToHex(const unsigned char *data, size_t size) {
  static const char *kDigits = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    out.push_back(kDigits[(data[i] >> 4) & 0x0f]);
    out.push_back(kDigits[data[i] & 0x0f]);
  }
  return out;
}

// 读公钥：先按 PEM(SubjectPublicKeyInfo) 读，失败再退到 32 字节裸公钥。
EVP_PKEY *LoadPublicKey(const std::string &path, std::string *err) {
  FILE *fp = std::fopen(path.c_str(), "rb");
  if (fp == nullptr) {
    if (err != nullptr) *err = "公钥打不开: " + path;
    return nullptr;
  }
  ERR_clear_error();
  EVP_PKEY *pkey = PEM_read_PUBKEY(fp, nullptr, nullptr, nullptr);
  std::fclose(fp);
  if (pkey != nullptr) return pkey;

  // PEM 失败：可能是裸 32 字节公钥
  ERR_clear_error();
  std::string raw;
  std::string rerr;
  if (ReadFileBytes(path, &raw, &rerr) && raw.size() == 32) {
    pkey = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char *>(raw.data()), raw.size());
    if (pkey != nullptr) return pkey;
  }

  if (err != nullptr) {
    *err = "公钥解析失败（既不是 PEM SubjectPublicKeyInfo，也不是 32 字节裸 "
           "Ed25519 公钥）: " + path;
    AppendSslError(err);
  }
  return nullptr;
}

}  // namespace

const char *OpenSslCompileTimeVersion() { return OPENSSL_VERSION_TEXT; }

const char *OpenSslRuntimeVersion() { return OpenSSL_version(OPENSSL_VERSION); }

bool Sha256Bytes(const void *data, size_t size, std::string *hex) {
  if (hex == nullptr) return false;
  hex->clear();
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int md_len = 0;
  bool ok = false;
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  if (ctx != nullptr) {
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
        (size == 0 || EVP_DigestUpdate(ctx, data, size) == 1) &&
        EVP_DigestFinal_ex(ctx, md, &md_len) == 1 && md_len == 32) {
      *hex = ToHex(md, md_len);
      ok = true;
    }
    EVP_MD_CTX_free(ctx);
  }
  return ok;
}

bool Sha256File(const std::string &path, std::string *hex, std::string *err) {
  if (hex == nullptr) return false;
  std::ifstream ifs(path.c_str(), std::ios::in | std::ios::binary);
  if (!ifs.is_open()) {
    if (err != nullptr) *err = "打不开文件: " + path;
    return false;
  }

  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int md_len = 0;
  bool ok = false;
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  if (ctx == nullptr) {
    if (err != nullptr) *err = "EVP_MD_CTX_new 失败（内存不足？）";
    return false;
  }
  if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
    if (err != nullptr) {
      *err = "EVP_DigestInit_ex 失败";
      AppendSslError(err);
    }
  } else {
    std::vector<char> buf(64 * 1024);
    bool read_ok = true;
    while (ifs.good()) {
      ifs.read(&buf[0], static_cast<std::streamsize>(buf.size()));
      const std::streamsize got = ifs.gcount();
      if (got > 0 && EVP_DigestUpdate(ctx, &buf[0], static_cast<size_t>(got)) != 1) {
        read_ok = false;
        if (err != nullptr) {
          *err = "EVP_DigestUpdate 失败";
          AppendSslError(err);
        }
        break;
      }
    }
    if (read_ok && ifs.bad()) {
      read_ok = false;
      if (err != nullptr) *err = "读文件出错: " + path;
    }
    if (read_ok && EVP_DigestFinal_ex(ctx, md, &md_len) == 1 && md_len == 32) {
      *hex = ToHex(md, md_len);
      ok = true;
    } else if (read_ok && err != nullptr) {
      *err = "EVP_DigestFinal_ex 失败";
      AppendSslError(err);
    }
  }
  EVP_MD_CTX_free(ctx);
  return ok;
}

bool HexEqual(const std::string &a, const std::string &b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const char ca =
        static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
    const char cb =
        static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
    if (ca != cb) return false;
  }
  return true;
}

bool VerifyEd25519(const std::string &pubkey_path, const std::string &message,
                   const std::string &signature, std::string *err) {
  if (signature.size() != 64) {
    if (err != nullptr)
      *err = "签名长度 " + std::to_string(signature.size()) +
             " 不是 Ed25519 要求的 64 字节（文件可能被截断，或存成了 base64）";
    return false;
  }

  ERR_clear_error();
  EVP_PKEY *pkey = LoadPublicKey(pubkey_path, err);
  if (pkey == nullptr) return false;

  if (EVP_PKEY_base_id(pkey) != EVP_PKEY_ED25519) {
    if (err != nullptr) *err = "公钥不是 Ed25519（本程序只支持 Ed25519，见 §5.4）";
    EVP_PKEY_free(pkey);
    return false;
  }

  bool ok = false;
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  if (ctx == nullptr) {
    if (err != nullptr) *err = "EVP_MD_CTX_new 失败（内存不足？）";
  } else {
    // Ed25519 是「一次性签名」算法：md 必须传 NULL，也不允许分多次 Update
    if (EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey) != 1) {
      if (err != nullptr) {
        *err = "EVP_DigestVerifyInit 失败";
        AppendSslError(err);
      }
    } else {
      const int rc = EVP_DigestVerify(
          ctx, reinterpret_cast<const unsigned char *>(signature.data()),
          signature.size(),
          reinterpret_cast<const unsigned char *>(message.data()), message.size());
      if (rc == 1) {
        ok = true;
      } else if (err != nullptr) {
        *err = (rc == 0) ? "签名不匹配（清单被改过，或不是这把私钥签的）"
                         : "EVP_DigestVerify 调用失败";
        if (rc != 0) AppendSslError(err);
      }
    }
    EVP_MD_CTX_free(ctx);
  }
  EVP_PKEY_free(pkey);
  return ok;
}

bool VerifyManifestSignature(const std::string &pubkey_path,
                             const std::string &manifest_path,
                             const std::string &sig_path, std::string *err) {
  std::string manifest;
  if (!ReadFileBytes(manifest_path, &manifest, err)) return false;
  std::string sig;
  if (!ReadFileBytes(sig_path, &sig, err)) return false;

  // 只认裸 64 字节签名（发布脚本 §5.4 已固定为 pkeyutl -rawin 的裸输出）。
  // base64 文本不支持：那会让「长度对不上」变成一个含糊的错误，
  // 不如直接说清楚该传什么。
  return VerifyEd25519(pubkey_path, manifest, sig, err);
}

}  // namespace upgrade
}  // namespace evs
