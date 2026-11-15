# src/upgrade/ —— 升级对象库（`libupgrade_api.so`）

> 方案的**唯一权威**是 [../../docs/design/DESIGN_UPGRADE.md](../../docs/design/DESIGN_UPGRADE.md)。
> 本文件只讲「代码里现在有什么、哪些还是桩、怎么用」，以及踩过的坑。

## 1. 现状（2026-11-15：骨架落地）

```text
src/upgrade/                      → out/lib/libupgrade_api.so
├── upgrade_types.{h,cpp}         Phase(11) / ErrorCode(16+1) ↔ 字符串；状态持久化结构
├── manifest.{h,cpp}              manifest.json 解析 + semver + 兼容性/防降级 + 路径穿越拦截
├── verifier.{h,cpp}              SHA-256；Ed25519 验签（PEM / 32 字节裸公钥）；OpenSSL 版本自检
├── fetcher.{h,cpp}               file / http(s) 取数（curl 子进程，不走 shell）；.part + 重试退避
├── slot_manager.{h,cpp}          releases/current/previous + 原子写（fsync 文件 + fsync 目录）+ 水位
└── health_check.{h,cpp}          RTSP URL 解析 + TCP 端口探活（帧率/首帧是桩）
```

进程入口在 [`../main/upgrade/`](../main/upgrade/)（`evs_upgrade`）。

| 能力 | 状态 |
| --- | --- |
| 状态机 / 事件上报 / 命令分发 / `boot_count` / 审计日志 | ✅ `src/main/upgrade/upgrade_service.cpp` |
| 清单解析、字段校验、semver、hw/abi/降级/min_from 判定 | ✅ |
| Ed25519 验签（先验签后解析）、SHA-256 | ✅ |
| 取清单 / 取包（`file` 与 `http(s)`，断点续传、重试、`.part` 原子改名） | ✅ |
| 槽位原子切换 / 状态原子持久化 / 磁盘水位 | ✅ |
| 健康检查：端口探活 | ✅ |
| 解包（`ExtractPackage`）、逐文件校验（`VerifyStagedFiles`） | 🚧 桩 → `E_NOTIMPL` |
| `systemctl` 停/起/查（`ControlService`） | 🚧 桩 → 返回 false + 说明 |
| 旧版本清理（`PruneOldReleases`） | 🚧 桩 |
| 首帧 / 帧率（`CheckFps`） | 🚧 桩（ffprobe 路径见配置 §7.1） |
| 轮转恢复（掉电后按 `state/upgrade.json` 续做） | 🚧 未接（设计 §6.4 矩阵已写） |

**骨架的诚实性约束**：桩一律 `return false` + `E_NOTIMPL` + 说明，**绝不假装成功**。
所以 `--apply` 现在必然停在 STAGING 并以 `E_NOTIMPL` 收尾，`--rollback` 在切链前就
因「控制不了服务」而中止（不会留下「链切了但服务没起」的半死状态）。

## 2. 关键实现约束（改代码前必读）

1. **先验签、后解析**：`manifest.json` 的原始字节先过 Ed25519，再 `ParseManifest`。
   验签口径是 `openssl pkeyutl -sign -rawin`（Ed25519 内部已做 SHA-512，**不要再 prehash**）。
2. **取数不经 shell**：`fork` + `execvp` + `waitpid`，`curl -fsSL -m <timeout> -o <dst>.part [-C -]`。
   失败分类：`22 → E_HTTP`，其余（6/7/28/35/56/127…）`→ E_NET`。
3. **任何落盘都先 `.part` / `.tmp`，再 `rename`**，并 `fsync` 文件与目录；`AtomicSymlink`
   先 `symlink(tmp)` 再 `rename`，**不要先 unlink**（中间窗口设备不可用）。
4. **换链顺序**：先写 `previous`（指向老版本），再换 `current`；反序在掉电时无法回滚。
5. **公钥缺失 = 拒绝一切升级**（`E_SIG`），不允许「跳过验签」的降级路径。
6. **路径安全**：`files[].path` 拒绝绝对路径与 `..`（解包器将来照此校验，防穿越）。
7. **OpenSSL 必须用 `install/<平台>` 那份**（3.5.4，系统是 3.0.2）。`src/upgrade/CMakeLists.txt`
   默认把 `OPENSSL_ROOT_DIR` 指过去，并在 configure 时打印头/库路径与版本；
   进程启动时再打印「编译期 / 运行期」两行版本——不一致即头库错配。
8. **C++14**：不用 `std::optional` / `if constexpr` / 结构化绑定。

## 3. 公开 API 速查

```cpp
// 清单
evs::upgrade::Manifest m; evs::upgrade::ErrorCode code; std::string err;
evs::upgrade::ParseManifest(text, &m, &code, &err);          // 只校验字段，不验签
evs::upgrade::CheckManifest(m, ctx, &code, &err);            // hw/abi/降级/min_from（§5.5）
evs::upgrade::CompareVersion("1.4.10", "1.4.9");             // 1；非法版本返回 2

// 验签 / 哈希
evs::upgrade::VerifyManifestSignature(pub, "manifest.json", "manifest.sig", &err);
evs::upgrade::Sha256File(path, &hex, &err);
evs::upgrade::OpenSslCompileTimeVersion();                   // vs OpenSslRuntimeVersion()

// 取数
evs::upgrade::Fetcher f(cfg);                                // cfg.type = file|http
f.FetchToString("manifest.json", &text, &code, &err);
f.FetchToFile("evs-app-1.4.2-rk3588.tar.gz", dst, &code, &err);

// 槽位
evs::upgrade::SlotManager slots({install_root, data_root, 2, 256});
slots.EnsureLayout(&err);
slots.ReadCurrentVersion(&ver, &err);                        // 无指针 → 空串 + true
slots.SwitchCurrent("1.4.2", /*keep_previous=*/true, &err);  // 原子
slots.WriteState(st, &err); slots.ReadState(&st, &err);
```

## 4. 验证

```bash
bash scripts/tests/run_t7_upgrade.sh          # 单元 146 项 + CLI 13 个用例，全 PASS
make run-upgrade ARGS="--status"              # 看真实配置下的槽位/状态
make run-upgrade ARGS="--verify=/tmp/manifest.json"   # 发布机自检：只验签
```

记录见 `docs/history/TESTLOG.md` §5.25（L2）。

## 5. 已知限制 / 下一步

- `comm.protocol` 只支持 `mqtt`（evt 子主题依赖 MQTT）与 `none`（离线只打印）。
  evt 三个子主题各占一条发布通道 —— `MqttComm` 的发布主题是**通道级**配置，
  不能按消息指定 topic；若将来 comm_api 支持按消息指定 topic，可收敛成一条连接。
- 下载无进度百分比（骨架期 `percent` 恒 0/100）：需要 `curl -w`/`--progress-bar`
  解析或改自研下载器。
- `--apply` 目前不写 `previous`→`current` 的 manifest 留档（`version.json`），P2 补。
