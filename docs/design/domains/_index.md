# 域文档索引（WP0-2 补全）

> 状态：WP0-2 文档补全，2026-10-31。描述 ng 分支 **HEAD `c4efabc` 的已提交实现**；
> WP3-3（session manager）正在另一会话进行，工作区里的未提交文件不计入本文口径。
> 上位：`../architecture.md`（7 大类）、`../evolution.md`（E0–E5 / WP）、
> `../decisions.md`（ADR-001…020）。接口定形见 `../../abi/abi-1.0-draft.md`。
> 证据：`../../claims/ng-WP1-*.md` … `ng-WP3-2.md`。**没有 claims 的能力一律按未实现写。**

## 1. 域与所有权

| 域 | 页面 | 当前代码路径（ng） | 目标路径（architecture §7） | 当前状态 |
|---|---|---|---|---|
| L0 内核 | [l0-kernel.md](l0-kernel.md) | `src/ng/**`、`src/kernel/**`（旧树参考） | `src/kernel/` | 已落地 E1 全部 + WP3-0 进程 + WP3-1 共享内存/LFB 机制 |
| L1 系统服务 | [l1-services.md](l1-services.md) | `src/services/{fs,auth,log,pkg}/**` + `src/ng/*_syscall.c` 胶水 | `src/services/` | 部分：VFS/FAT16/auth/log/pkg 已移植，但**以 L1 库链接进内核**，无 fsd/netd 进程 |
| L1D 设备服务 | [l1d-drivers.md](l1d-drivers.md) | `src/drivers/**` + `src/ng/wp23/24_selftest.c`、`ata_blk.c` | `src/drivers/` | 部分：PCI/VBE/fb/PS2/ATA 在 ring0；无 ring3 驱动、无 IRQ/DMA |
| L2 桌面会话 | [l2-desktop.md](l2-desktop.md) | `src/desktop/**` | `src/desktop/` | 部分：L2a displayd + L2b wm 已落地；L2c session manager 进行中（WP3-3），S1 未验收（WP3-5） |
| L3 应用 | [l3-apps.md](l3-apps.md) | `src/apps/{shell,demo,proc}/**`、`src/ng/user/hello.S` | `src/apps/` | 部分：shell / demo / proc / hello；无 SDK、编辑器、GUI 工具包、沙箱 |
| A 平台契约 | [a-platform.md](a-platform.md) | `docs/abi/**`；`sdk/` **不存在** | `sdk/` | 草案：ABI 1.0 文档成形；SDK/POSIX/签名包均未做 |
| B 生命周期与运维 | [b-lifecycle.md](b-lifecycle.md) | `tools/ci-ng.ps1`、`.github/workflows/ng.yml`、`src/services/pkg` | `tools/ci/` 等 | 部分：CI/哈希校验/登录验证；无签名、更新/回滚、沙箱、审计 |

依赖规则（`tools/check-domains.ps1`）：L3 → L2 → L1 → L0；**L1D 只允许依赖 L0/A/B**；
A/B 为横切域，可被任何层依赖；同层允许互相依赖。`src/kernel/**` 在 E0 映射里整体视为 L0，
`src/ng/**` 不在检查器的域映射表里（属于 L0 实现目录，见 D01/D05）。

## 2. manifest 五元组（词汇表）

architecture.md §4 在既有 manifest 字段（name/version/type/entry/requires/capabilities/
restart/restart_limit/uid）上追加 `plane`、`criticality`、`fallback`、`health`。本套域文档
按任务口径把 **五元组** 落成：

```text
(plane, criticality, fallback, health, restart)
  前四项  = architecture.md §4 的新增字段
  restart = 既有 restart/restart_limit，作为第五项恢复策略（不放任“只声明等级、不声明重启”）
```

| 字段 | 取值 | 语义 |
|---|---|---|
| `plane` | `l0` / `l1` / `l1d` / `l2a` / `l2b` / `l2c` / `l3` | 组件所属故障域；`l2` 只作为未拆分时的别名。A/B 横切域没有 plane |
| `criticality` | `fatal` / `core` / `session` / `optional` / `isolated` | 与 architecture.md §3 的五级故障矩阵一一对应 |
| `fallback` | 组件名 / `none` | 崩溃或缺失时的降级目标；**L2 组件不得为 `none`**（architecture §4 规则 4）；L0 只能 `none` |
| `health` | `heartbeat:<N>s` / `status:<path>` / `none` | 存活探测；当前实现用 `/run/display.status` 之类状态文件代替真正的 health 通道 |
| `restart` | `never` / `on-failure` / `always` | 既有字段；`restart_limit` 仍按现有语义 |

规则：

1. `optional` 不得被 `core` 同步依赖（architecture §4 规则 2）。
2. shell/WM 不拥有应用；应用生命周期归 `l2c` session manager（ADR-006）。
3. 没有 fallback 的组件不许进 L2；但 L3 应用允许没有 fallback（只死自己）。
4. **当前没有组件 manifest 解析器支持这五个字段。** `src/services/pkg` 的
   `pkg_manifest_parse()` 只接受 `name/version/type/entry/sha256` 五个 key，
   多一个 `plane=` 就返回 `-EINVAL`（见 `../auth-log-pkg-ng.md` §5.2、D04）。
   本节的五元组是**设计契约**，不是已实现能力。

## 3. 跨域调用

```text
L3 应用（ring3）──syscall 0/1/10..14/20..27/30..35/40..47──────┐
L2 displayd/wm（ring3）──syscall + IPC(ticket) + SHM/LFB────────┤
                                                              v
L1D 驱动（ring0，由 L0 注入端口 IO/MMIO） <── L0 分发/胶水 <── L0 内核
                                                              │
L1 服务（当前以库形式链接进 L0） <─────────────────────────────┘
   VFS/ramfs/FAT16/auth/log/pkg 的用户指针只在 src/ng/*_syscall.c 处理
A 平台契约：ABI 草案 / 包格式 / 工具链（无运行时组件）
B 生命周期：CI / 身份 / 更新 / 发布（无运行时组件）
```

- **控制面**走 IPC（消息 ≤ 4 KiB + 16 B 头）；**数据面**走共享内存（surface 像素）或
  LFB（扫描输出）。参见 ADR-011 与 `../display-server-ng.md`。
- L3 不直接碰硬件；L2 的 displayd 通过 `SYS_FB_MAP` 才拿到 LFB（root-only，机制在 L0）。
- L1D 不认识 VFS/页表/PMM；`blk_env` 适配层放在 `src/ng`，避免 L1D → L1 的向上依赖
  （`../block-ng.md` §1）。

## 4. 工作包与证据

| WP | 域 | claim | 结论 |
|---|---|---|---|
| WP1-1 | L0 boot/long mode | [ng-WP1-1.md](../../claims/ng-WP1-1.md) | PASS |
| WP1-2 | L0 页表/W^X | [ng-WP1-2.md](../../claims/ng-WP1-2.md) | PASS |
| WP1-3 | L0 trap/syscall | [ng-WP1-3.md](../../claims/ng-WP1-3.md) | PASS |
| WP1-4 | L0 PMM/heap | [ng-WP1-4.md](../../claims/ng-WP1-4.md) | PASS |
| WP1-5 | L0 线程/调度 | [ng-WP1-5.md](../../claims/ng-WP1-5.md) | PASS |
| WP1-6 | L0 IPC/句柄 | [ng-WP1-6.md](../../claims/ng-WP1-6.md) | PASS |
| WP1-7 | L0 ELF64/用户 AS/uaccess | [ng-WP1-7.md](../../claims/ng-WP1-7.md) | PASS |
| WP1-8 | L0/E1 门禁 | [ng-WP1-8.md](../../claims/ng-WP1-8.md) | PASS（tag `ng-e1` = `69d84ca`） |
| WP2-1 | L1 VFS/ramfs | [ng-WP2-1.md](../../claims/ng-WP2-1.md) | PASS |
| WP2-2 | L1 FAT16 | [ng-WP2-2.md](../../claims/ng-WP2-2.md) | PASS |
| WP2-3 | L1D fb/input | [ng-WP2-3.md](../../claims/ng-WP2-3.md) | PASS |
| WP2-4 | L1D ATA PIO | [ng-WP2-4.md](../../claims/ng-WP2-4.md) | PASS |
| WP2-5 | L1 auth/log/pkg | [ng-WP2-5.md](../../claims/ng-WP2-5.md) | PASS |
| WP2-6 | L3 shell | [ng-WP2-6.md](../../claims/ng-WP2-6.md) | PASS |
| WP2-7 | 测试/parity | [ng-WP2-7.md](../../claims/ng-WP2-7.md) | PASS（tag `ng-e2` = `d3a5671`） |
| WP3-0 | L0 进程 + L3 /bin/proc | [ng-WP3-0.md](../../claims/ng-WP3-0.md) | PASS |
| WP3-1 | L0 机制 + L2a + L3 demo | [ng-WP3-1.md](../../claims/ng-WP3-1.md) | PASS |
| WP3-2 | L2b WM + 协议 v2 | [ng-WP3-2.md](../../claims/ng-WP3-2.md) | PASS |
| WP3-3 | L2c session manager | — | **进行中（另一会话，未提交）** |

## 5. 已知漂移

### 5.1 未解决（本次只记录，不改基线/代码）

> D03 已并入 §5.2 的 F02（编号保留，避免跨文档引用错位）；D18 是本次核对代码时新发现。

| ID | 位置 | 现象与证据 | 影响 | 建议 |
|---|---|---|---|---|
| D01 | `../architecture.md` §6 | “现状映射（HEAD 69ab006）”仍写 boot/loader/arch 32 位、64 位未开始、VFS 内核内；实际 ng 已到 E3 | 新会话可能误判基线，重复已完成的 WP | 由架构负责人把 §6 更新到 `c4efabc`，或明确标注“历史快照（69ab006）” |
| D02 | `../evolution.md` E1/E2 出口段 | 写“待打 tag `ng-e1`/`ng-e2`”；实际 tag 已存在：`ng-e1=69d84ca`、`ng-e2=d3a5671` | 发布状态误读 | 改成“已打 tag”；tag 由负责人维护 |
| D04 | `../architecture.md` §4 / `src/services/pkg/pkg.c` | 五元组未实现：`pkg_manifest_parse()` 只认 5 个 key，遇到 `plane=` 直接 `-EINVAL` | 架构示例 manifest 当前无法被任何解析器接受；组件模型只停留在文档 | E4/WP4-1 前定义“服务 manifest v2”并实现解析；之前不要把五元组写成已实现 |
| D05 | `../architecture.md` §2.2/§3 / `src/ng/*_syscall.c` | L1 服务以库链接进内核，无独立端口/重启域；VFS/auth 的一个 bug 可 panic 整个内核 | “L1 core = 快速重启”不成立，故障半径实际为 fatal | architecture §2.2 已允许 D0 “VFS 留内核”，但应把“core 隔离自 fsd 起”写成 D0 例外，别在文档里承诺 core |
| D06 | `../architecture.md` §3 / `../elf-ng.md` §10、`../proc-ng.md` §10 | ring3 的 #PF/#GP 仍进 panic；只有 exit/kill 是 isolated | L3 “只死自己”对**异常**不成立，坏应用可拖垮系统 | WP3-5 故障注入时把用户态 fault 改成“杀进程 + 状态码”，再更新本表 |
| D07 | `../architecture.md` §2.1/§3 | fatal 写“panic + watchdog 重启”；`src/ng/trap.c` 只有 `cli; hlt`，无 watchdog | fatal 实际是挂死，不是自动恢复 | E4 增加 watchdog 或把架构措辞改成“panic + 人工/测试重启” |
| D08 | `../architecture.md` §2.3/§10 D0 | D0 写“fb 或 virtio-gpu 2D + 键鼠”；`../display-input-ng.md` §11 明确只做键盘、鼠标可选 | “D0 键鼠”验收口径不一致 | 负责人二选一：D0 明确键盘-only，或把鼠标列进 WP3-5/E4 |
| D09 | `../architecture.md` §6 | hwcap 89–92（MMIO/IRQ/DMA）写成“已提交，305/305”，但那是旧 32 位树；ng 的 syscall 表没有 89–92，也没有 IRQ 路由 | 读者会以为 ng 已有驱动隔离地基 | 在 §6 标明“旧树能力，ng 未迁”；ng 的 hwcap 另立 WP |
| D10 | `src/services/log/log.h` vs `log_write()` / `SYS_LOG_WRITE` | `LOG_ENAMETOOLONG(-36)` 已定义；`log_write()` 自身对超长/非法 tag/msg 返回 `LOG_EINVAL`；但 syscall 33 的超长字符串先被 `read_user_string()` 判成 `ENAMETOOLONG`。同一“超长”在 API 与 syscall 两层不一致（`ng-WP2-7` 观察 2 只记录了 API 层）；`read_user_string()` 的 off-by-one 也让 auth 名字/口令、pkg 名字在 syscall 层比服务层少 1 字节 | 头文件/行为不一致，ABI 文档若只照一层写会错 | ABI 草案按 syscall 层写（见 §5.4：log tag≤14/msg≤126、login 名字≤30/口令≤62、pkg 名字≤30）；后续统一 API 或改头 |
| D11 | `src/kernel/include/abi.h`、`docs/api.md`、`docs/native-abi.md` | 旧树是 int 0x80/0x81 ABI，编号 1..95（`SYS_WRITE=1`、`SYS_PORT_CREATE=80`…）；ng 是 `syscall` 指令 + `SYS_WRITE=0`、`PORT_CREATE=10` | 同一个名字两套编号，开发者极易混用 | 见 `../../abi/abi-1.0-draft.md` §2：旧 ABI 冻结为 compat，不并入 1.0；SDK 只发 1.0 头 |
| D12 | `docs/abi/` | 只有草案文档，没有机器可读的 `abi-1.0.h`；`src/ng/syscall.h` 是当前唯一调用号源 | SDK 只能手抄常量；E4 冻结缺一个可比对的头 | WP4-1 生成 `sdk/include/aster/abi-1.0.h`，用 `_Static_assert` 钉住结构体 |
| D13 | `src/desktop/include/ds_protocol.h` 工作区 | 未提交的 WP3-3 改动把协议升到 v3（CREATE `token`）；HEAD `c4efabc` 是 v2 | 照工作区读会误以为 v3/角色令牌已实现 | 本文与域文档一律按 v2 写；WP3-3 提交后再补 v3 差异 |
| D14 | `../auth-log-pkg-ng.md` §3.3 / `../shell-ng.md` §10 / `../proc-ng.md` §8 | auth 只验证口令并返回 uid，不建立会话；`proc->uid` 默认 0，VFS `ctx->uid` 仍是 0；所有进程实际 root | B 域“身份/权限策略”没有运行时落点，文件权限检查被 root 绕过 | WP3-3 session manager 接 uid/角色令牌；fsd 前至少把 `proc->uid` 传进 VFS ctx |
| D15 | `../display-server-ng.md` §12 / `../../claims/ng-WP3-1.md` | 没有 `SYS_SHM_FREE`，surface DESTROY 后 displayd 的映射要等进程退出才释放 | 长跑会话会耗尽 8 个 SHM 对象/映射槽 | WP3-3 surface 生命周期或 E4 前补 `SHM_FREE`/引用释放 |
| D16 | `../architecture.md` §2.4 / `../../claims/ng-WP3-2.md` | D0 写 S1“杀 shell，L3 应用不死、输入恢复、面板回来”；WP3-2 只证明“kill WM 后 Demo 仍活、displayd 继续服务” | D0 出口未达成，属进度差不是矛盾 | WP3-5 做输入恢复/面板重建断言，再更新 architecture 的 D0 出口状态 |
| D17 | `src/ng/syscall.c` default | 未分配调用号返回 `-1`（`-EPERM`），不是 `-ENOSYS`；`-1` 同时又是合法权限错误 | 无法区分“内核不认识这个调用”和“权限不足”；SDK 探测能力会误判 | E4 前改为 `-ENOSYS(-38)`；1.0 冻结时把该行为写死 |
| D18 | `src/ng/proc.c` `proc_sys_spawn` / `proc_copy_argv` | `copyin_str()` 在缓冲满时返回 `max-1`（63/127），但 spawn 用 `len >= PROC_PATH_MAX(64)`、argv 用 `len >= PROC_ARG_MAX(128)` 判定，永远不命中；64+ 字符 path/128+ 字符 argv 被**静默截断后接受**，不是 `EINVAL/ENAMETOOLONG` | 可能打开/传递错误路径；ABI 1.0 的 spawn 长度语义与代码不一致 | 改判定为 `>= max-1` 并返回 `ENAMETOOLONG`（path）或 `EINVAL`（argv）；冻结前必须修 |

### 5.2 本次已修正（仅文档）

| ID | 位置 | 修正 |
|---|---|---|
| F01 | `../../design/README.md` | 文件清单补 `domains/`、`docs/abi/`；当前阶段补“WP3-3 进行中” |
| F02 | `../../design/README.md` | main 分支描述改为“代码冻结 69ab006 / main 顶端 d62bccb 仅设计文档” |
| F03 | `docs/claims/ng-WP0-2.md` | 新建：记录本次文档补全的范围、证据与“未改代码/未跑 QEMU” |

### 5.3 进度差（设计已写、实现未做，不是文档矛盾）

| 项 | 设计位置 | 现状 |
|---|---|---|
| S1（杀 shell 应用不死 + 输入恢复 + 面板回来） | architecture §2.4/§10 | 只验证 kill WM 后 Demo 存活；WP3-3/3-5 未完成 |
| session manager / 角色令牌 | architecture §2.4、ADR-006 | WP3-3 进行中，未提交 |
| SDK alpha / POSIX Profile | architecture §2.6、ADR-005/012 | `sdk/` 不存在；POSIX 在 D1 |
| cap（grant/revoke）与 ticket 防伪 | architecture §8、ADR-011 | 只有 owner/root 校验；ticket“知道就能发” |
| 驱动 ring3 化（blkd/inputd/netd） | architecture §2.3、ADR-007 | 冻结到 D1；当前 ring0 |
| 签名包/更新/回滚 | architecture §2.7、D0 出口 | 只有 SHA-256 清单校验，无签名 |
| 沙箱/每应用命名空间 | architecture §2.5/§5 不变量 3 | 无；所有进程 uid=0、共享全局 VFS ctx |
| 抢占/tick IRQ | sched-ng §7 | 协作式；ring3 死循环杀不掉 |
| i386 旧树 | architecture §8 | 冻结在 main 69ab006（305/305），ng 不合并 |

## 6. 维护规则

1. **一个 WP 一个会话**；按路径所有权提交。域文档与本域 claim 在同一个 WP 更新。
2. 文档里的“已实现”必须能指到 `../../claims/` 的具体 claim；没有 claim 的写“设计/待办”。
3. 新 syscall / 结构体变化同时改三处：`src/ng/syscall.h`（或对应头）、本页接口表、
   `../../abi/abi-1.0-draft.md`。E4 冻结后只允许**追加**。
4. manifest 五元组只有解析器真正接受后才可从“设计”改成“已实现”（D04）。
5. 不引用工作区未提交文件作为“已实现”证据；WP3-3 落地前只认 `c4efabc`。
6. 目录/依赖变化同步 `tools/check-domains.ps1` 的映射与本文 §1。

## 7. 文件清单

| 文件 | 域 | 内容 |
|---|---|---|
| [_index.md](_index.md) | — | 本页：词汇、状态、漂移 |
| [l0-kernel.md](l0-kernel.md) | L0 | 内核机制、syscall 0/1/10–14/40–47、失败 fatal |
| [l1-services.md](l1-services.md) | L1 | VFS/FAT16/auth/log/pkg，syscall 20–27/31–35 |
| [l1d-drivers.md](l1d-drivers.md) | L1D | PCI/VBE/fb/PS2/ATA，ring0 现状 |
| [l2-desktop.md](l2-desktop.md) | L2 | displayd/wm/session manager、协议 v2、S1 |
| [l3-apps.md](l3-apps.md) | L3 | shell/demo/proc/hello、进程入口 ABI |
| [a-platform.md](a-platform.md) | A | ABI 1.0 草案、SDK/POSIX/包格式 |
| [b-lifecycle.md](b-lifecycle.md) | B | 身份/沙箱/更新/CI/发布 |
| [../../abi/abi-1.0-draft.md](../../abi/abi-1.0-draft.md) | A | ABI 1.0 草案（E4 冻结） |
