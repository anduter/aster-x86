# B 生命周期与运维（横切）

> 状态：WP0-2 域文档补全，2026-10-31；描述 ng HEAD `c4efabc` 的已提交实现。
> 上位：`../architecture.md` §2.7、§3、§10（D0/D1/D2）、ADR-010。
> 证据：`../../claims/ng-WP1-8.md`、`ng-WP2-5.md`、`ng-WP2-7.md`。
> B 域是**策略 + 流程**，不是运行时组件；它的失败降低安全/更新能力，不直接拖垮内核。

## 职责

| 内容 | 当前产物 | 状态 |
|---|---|---|
| 身份/权限策略 | auth：root(0)/alice(1000)，salt+SHA-256 4096 轮，5 次失败锁定，`/etc/passwd` | 部分：只验证口令，不建立会话/不切 uid（D14） |
| 沙箱 | — | **未实现**；所有进程 uid=0、共享全局 VFS ctx |
| 签名更新/回滚 | — | **未实现**；pkg 只有 SHA-256 清单校验，无发布者签名 |
| 审计/CVE | log 环 + panic/启动/WP 汇总 | 部分：无持久化/优先级/审计查询；无 CVE 流程 |
| CI/测试矩阵 | `tools/ci-ng.ps1` 7 步、`.github/workflows/ng.yml`、host 4684 checks、parity 100 | 已建立（WP1-8/WP2-7/WP3-2）；托管 runner 的 QEMU 冒烟非阻塞 |
| 遥测 | — | 未实现；`log` 是本机环，不外发 |
| 发布 | 阶段 tag `ng-e1`/`ng-e2`；D0 发布未做 | 部分 |
| 硬件矩阵 | QEMU 11 + stdvga/PIIX3/PS2 + `-cpu max`/`qemu64` | 仅 QEMU；真机 D1 |
| 故障注入矩阵 | `-Panic`/`-Fallback`/`-NoQmp`/`-Smoke`、ATA 超时、坏 ELF/坏 BPB/坏指针 | 部分（WP2-7/WP3-1/3-2）；S1 故障注入未做（WP3-5） |

## 不做什么

1. **不实现运行时机制**：cap、MMU、IPC 属于 L0；B 只定义策略/流程（角色、签名、审计、
   发布门禁）。
2. **不把 POSIX/账户策略塞进内核**：auth 是 L1 库；D1 的会话/PAM 在用户态。
3. **不承诺“签名包已可用”**：D0 只有哈希自洽；签名/公钥链/回滚是 D1。
4. **不做真实硬件认证/遥测上报**：D1 锁定硬件后再建矩阵。
5. **不把 CI 当安全边界**：CI 通过≠没有漏洞；CVE/审计流程仍待建立。

## 接口（syscall / IPC / ABI）

B 没有自己的 syscall；它约束/使用以下面：

| 面 | 当前接口 | 说明 |
|---|---|---|
| 身份 | `SYS_LOGIN`（31）→ uid / `ENOENT` / `EACCES` | 只验证；`vfs_ctx.uid` 仍 0，登录不切会话（D14） |
| 日志/审计 | `SYS_LOG_WRITE`（33）、`SYS_DMESG`（32） | 64 KiB 行环；panic/启动/WP 汇总写入 |
| 包校验 | `SYS_PKG_LIST`（34）、`SYS_PKG_VERIFY`（35） | 清单 + entry SHA-256；无签名 |
| manifest | `name/version/type/entry/sha256` | 五元组字段未支持（D04） |
| 更新 | — | 无 update syscall/通道 |
| 进程权限 | `proc->uid`、`kill` 的 uid 检查 | 所有进程 uid=0，检查实际不生效 |
| CI | `tools/ci-ng.ps1` | check-domains / check-deps / unit-ng / build-ng / run-ng / parity-ng / legacy |
| ABI 兼容 | `docs/abi/abi-1.0-draft.md` + 旧 `abi.h` | 0x81 冻结 compat；1.0 只追加 |

### CI 门禁

```text
tools/ci-ng.ps1           7 步全绿：check-domains / check-deps / unit-ng /
                          build-ng / run-ng / parity-ng / legacy build.ps1 -Test
tools/ci-ng.ps1 -Quick    跳过 QEMU/legacy（静态 + host 单测）
.github/workflows/ng.yml  ubuntu-latest 跑 -Quick + 非阻塞 QEMU 冒烟
```

最近证据（WP3-2）：host **4684 checks, 0 failures**、parity **100/100**（host 95/95）、
旧 32 位 **305/305**、`ci-ng` **7/7 PASS**。CI 不是发布签名，也不证明真机稳定性。

## 失败等级与恢复

| 场景 | 影响 | 当前恢复 |
|---|---|---|
| auth 服务崩溃/锁定 | 无法登录/降权 | 库链接进内核；重启=系统重启；锁定只能重新 `auth_init`（无超时解锁） |
| 日志环满 | 丢最旧整行 | 环形覆盖；不阻塞 |
| 包校验失败 | 拒绝安装/启动 | 返回 `EINVAL/EIO/ENOENT`；无事务/回滚 |
| 签名缺失（当前） | 无法证明发布者 | **D0 已知缺口**；D1 补签名通道 |
| CI 失败 | 阻止合并/发布 | 人工看门禁；GitHub Actions 未做硬性 branch protection |
| 更新失败 | 无回滚机制 | D1 做 A/B 或快照回滚；当前无更新器 |
| 安全事件 | 无 CVE/审计流程 | 未建立；先做日志持久化/审计查询 |

B 的失败**不直接 panic 内核**；但“身份未生效/无沙箱/无签名”会让运行时暴露在
权限提升与供应链风险下，这是 D0 的安全债。

## manifest 五元组

B 是横切域，没有运行时 plane/criticality/fallback/health；它**约束**所有组件的
manifest 与发布流程：

| 约束 | 规则 |
|---|---|
| `criticality` | 发布门禁必须覆盖所有 `core`/`fatal`/`session` 组件；`optional` 允许降级 |
| `fallback` | 发布前验证 fallback 目标存在；L2 组件缺 fallback 拒绝发布（architecture §4） |
| `health` | 有 `health` 的组件必须有超时/退避策略；当前只有状态文件，无真实探活 |
| `restart` | `restart_limit` 与故障注入矩阵对应；`never` 的 L0 要单独评审 |
| 签名 | 发布包必须签名（D1）；当前只有 SHA-256，不能替代签名 |
| 回滚 | 每个 `core` 组件更新必须有回滚路径（D1） |

B 域自身的“组件”是工具/流程，不进 manifest。若未来有 `updater`/`auditd` 服务，
按 L1 `core` 处理，但它的信任根/签名验证属于 B。

## 当前实现状态（ng WP）

| WP | 内容 | 证据 |
|---|---|---|
| WP1-8 | ng CI 一条命令、GitHub Actions、E1 门禁 | `../../claims/ng-WP1-8.md` |
| WP2-5 | auth/log/pkg、SHA-256、锁定、清单校验 | `../../claims/ng-WP2-5.md` |
| WP2-7 | parity 100 项、`-Panic`/`-Fallback`/`-NoQmp`/`-Smoke` 负向/降级 | `../../claims/ng-WP2-7.md` |
| WP3-1/3-2 | display/WM 的 kill 隔离与 QMP 像素断言（故障注入的一部分） | `../../claims/ng-WP3-1.md`、`ng-WP3-2.md` |

## 待办

1. **身份生效**：login 建立会话、`proc->uid` 进 VFS ctx、每进程 fd/命名空间（D14）；
   WP3-3 session manager 是落点。
2. **沙箱**：每应用根、picker/broker、网络 broker；先做 capability 再谈策略。
3. **签名包**：manifest `signature=`/公钥链、签名更新、A/B 或快照回滚（D1）。
4. **审计/CVE**：日志持久化、审计查询、CVE 跟踪/披露流程。
5. **CI 硬化**：branch protection、QEMU 硬门禁（KVM/TCG 稳定性）、锁定工具链版本。
6. **故障注入矩阵**：S1（杀 shell/WM）、S2（displayd）、S3（GPU）、OOM、坏盘、坏 ELF；
   与 architecture §3 的等级逐项对应。
7. **发布**：D0 文档/镜像/校验和/签名；版本号与 ABI 版本解耦。
8. **遥测/硬件矩阵**：D1 真实硬件、8h 浸泡、指标采集（默认关闭/可审计）。
