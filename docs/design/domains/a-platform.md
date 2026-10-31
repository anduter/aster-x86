# A 平台契约（横切）

> 状态：WP0-2 域文档补全，2026-10-31；描述 ng HEAD `c4efabc` 的已提交实现。
> 上位：`../architecture.md` §2.6/§8、ADR-004/005/012。
> 产物：`../../abi/abi-1.0-draft.md`（草案，E4 冻结）。
> 证据：`../../claims/ng-WP1-8.md`、`ng-WP2-6.md`、`ng-WP2-7.md`。

## 职责

A 域是**开发/构建面**，不是运行时组件：

| 内容 | 当前产物 | 状态 |
|---|---|---|
| native ABI 1.0 | `../../abi/abi-1.0-draft.md`：syscall 表、句柄/ticket、错误码、用户指针/长度、版本策略 | **草案**；无机器可读头（D12） |
| SDK / 工具链 | 只有 `tools/build-ng.ps1` 内部命令；`sdk/` 不存在 | **未做**（WP3-4） |
| libc / POSIX Profile | — | D1（ADR-005）；当前应用无 libc |
| 包格式 | 目录 + 5 字段 manifest（name/version/type/entry/sha256） | 最小可用（WP2-5）；无签名/依赖/事务 |
| 调试 / trace | 串口日志、`log` 环、panic 行、QMP 截图/按键注入、host 单测 | 内部可用；无调试器/ABI trace |
| 文档 / 示例 | `docs/design/*-ng.md`、`docs/claims/*`、`src/apps/*` | 有；缺“外部开发者一天写出 GUI app”的教程 |
| 兼容矩阵 | — | 未建立 |

## 不做什么

1. **不做运行时服务**：不拥有设备/文件/网络/桌面；A 的失败只影响开发/构建。
2. **不在内核实现 POSIX**：D1 的用户态 libc + syscall shim；源码兼容，不做 Linux/Windows
   二进制兼容（architecture §8）。
3. **不做动态链接**：D0 静态链接；dlopen/ld.so D1 再评估。
4. **不定义 display 协议**：L2 的 wire format 是内部契约；SDK 应包装它，不把 v2 当 ABI 1.0。
5. **不发签名包**：签名/更新通道归 B 域（D1）；A 只定义 manifest 字段。
6. **不承诺 ABI 1.0 已冻结**：E4/WP4-1 之前只追加；当前有已知缺口（版本查询、cap、超时单位）。

## 接口（syscall / IPC / ABI）

### ABI 1.0（草案）

- 入口：64 位 `syscall` 指令；`rax` 调用号，`rdi/rsi/rdx/r10` 参数，`rax` 返回；
  负值 = `-errno`。ABI 只承诺 `rax`；`rcx/r11` 被指令破坏，其余寄存器当前保存/恢复
  但调用方不应依赖。
- 调用号分组：0/1、10–14、20–27、30–35、40–47；2–9、15–19、28–29、36–39、48+ 未分配。
- 结构体：小端、固定宽度、自然对齐、`_Static_assert` 钉住；只允许尾部追加字段。
- 句柄：`(generation<<16)|slot`，0 非法；fd/ticket/shm handle 是不同命名空间。
- 用户指针：低半区、逐页 P/U（写还要 W）、SMAP 窗口、坏指针 `EFAULT`；单次
  read/write ≤512、write(console) ≤256、IPC ≤4112、spawn ELF ≤256 KiB。
- 完整表与错误码见 [`../../abi/abi-1.0-draft.md`](../../abi/abi-1.0-draft.md)。

### 包格式

```text
/System/Packages/<name>/<version>/manifest
/System/Packages/<name>/<version>/<entry>     # entry 是 manifest 里的相对路径
```

manifest 严格 5 字段（见 `../auth-log-pkg-ng.md` §5.2）；`pkg list` / `pkg verify`
通过 syscall 34/35 暴露。没有 `signature=`/公钥链（D1/B）。

### 工具链

```text
内核: clang --target=x86_64-elf -fuse-ld=lld -ffreestanding -nostdlib \
      -mno-red-zone -fno-stack-protector -mgeneral-regs-only
用户: 同一 target + -mno-sse -fno-builtin -fno-pic -fno-pie -std=c11 -O2/-Os \
      -Wl,-T,src/ng/user/user.ld  (基址 0x40000000)
```

`tools/build-ng.ps1` 是唯一构建入口，校验 ELF64/段/W^X/嵌入 blob；`tools/run-ng.ps1`
是 QEMU 门禁；`tools/ci-ng.ps1` 串起 7 步（见 B 域）。

### 版本与兼容

- **只追加，不破坏**：已有调用号/结构体字段偏移/大小/错误码值不改；删除的调用号保留，
  不得复用；新调用号从下一个未分配号开始，冻结后 bump minor。
- **0x81 legacy**：旧树 `abi.h` 的 1..95 + `int 0x81` 冻结为 compat，只修 bug，不加语义；
  与 ABI 1.0 **编号/寄存器/返回约定都不同**，不能混用（D11）。
- **0x80 旧 native**：同样属于旧树，不进入 1.0；ng 只有 `syscall` 指令入口。
- **ABI 版本查询**：ng 还没有；WP4-1 决定是否新增（当前未分配号）。在它存在之前，
  SDK 只能按编译期头文件版本假设。
- **冻结门槛**：E4（WP4-1）在 64 位 + SDK alpha + 外部开发者测试之后；冻结前允许
  不兼容调整，但必须同步改 `src/ng/syscall.h`、本文、ABI 草案。

## 失败等级与恢复

| 场景 | 影响 | 恢复 |
|---|---|---|
| ABI 草案/头文件错误 | 开发者编译失败或运行时错位 | 以 `src/ng/syscall.h` 为准，修文档/头；不影响已部署系统 |
| SDK/工具链不可用 | 无法构建应用 | 固定 CI 工具链版本；`check-toolchain64.ps1` 探针 |
| 包格式/校验失败 | 装包/启动失败 | `pkg_verify` 返回 `EINVAL/EIO/ENOENT`；不 panic |
| 兼容性回归 | 旧应用不工作 | E2 parity + `run-ng` 回归；旧树 oracle；冻结后只追加 |
| 文档漂移 | 开发者按错契约写 | 本套域文档 + ABI 草案 + claims 交叉检查（见 `_index.md` §5） |

A 域故障**不影响运行时**；但 ABI 错误会以“应用崩溃/系统调用返回怪值”的形式出现在
L3，所以冻结前的每次 syscall 变更都要求同一 WP 更新 ABI 草案。

## manifest 五元组

A 是横切域，**没有运行时组件**，因此没有 plane/criticality/fallback/health 的运行时取值；
`plane` 只用于 L0–L3 组件。若未来提供可部署的 SDK 辅助服务，建议：

| 组件 | plane | criticality | fallback | health | restart |
|---|---|---|---|---|---|
| `sdk-helper`（假设） | 无（A） | `optional` | `host-toolchain` | `none` | `never` |
| `pkg-signer`（假设，B） | 无（B） | `optional` | `manual-sign` | `none` | `never` |

当前**没有**这些组件；不要把它们写进 manifest 或 CI 依赖。包的五个字段是 A 域
唯一已实现的“契约数据”，但它还不是组件 manifest（D04）。

## 当前实现状态（ng WP）

| WP | 内容 | 证据 |
|---|---|---|
| WP1-8 | ng CI 一条命令、host 单测、GitHub Actions、工具链探针 | `../../claims/ng-WP1-8.md` |
| WP2-5 | pkg manifest 解析/列表/SHA-256 校验、syscall 34/35 | `../../claims/ng-WP2-5.md` |
| WP2-6 | 用户态 shell（无 libc 的 L3 参考实现） | `../../claims/ng-WP2-6.md` |
| WP2-7 | parity 100 项（ABI 行为的 kernel/host 同源断言） | `../../claims/ng-WP2-7.md` |
| WP3-0/3-1/3-2 | 新 syscall 40–47 与用户程序入口 ABI 的实际使用 | `../../claims/ng-WP3-0.md` … |

当前**没有** `sdk/`、没有 `abi-1.0.h`、没有 libc/POSIX、没有调试器、没有签名包、
没有兼容矩阵。ABI 草案是本次 WP0-2 的新增文档，不是冻结版。

## 待办

1. **WP3-4 SDK alpha**：`sdk/include/aster/` 头文件、syscall 包装、一个 GUI 示例；
   与 `docs/abi/abi-1.0-draft.md` 同源生成。
2. **WP4-1 ABI 1.0 冻结**：定稿调用号/结构体/错误码/超时单位；补版本查询；
   生成 `_Static_assert` 头；写兼容性矩阵。
3. **libc/POSIX Profile（D1）**：用户态 shim，不在内核实现 POSIX。
4. **包签名/依赖/事务（D1/B）**：manifest `signature=`、公钥链、安装/回滚。
5. **调试/trace**：符号、syscall trace、应用崩溃转储、QEMU 之外的调试路径。
6. **文档/示例**：外部开发者教程、ABI 变更日志、迁移指南。
7. **兼容矩阵**：CPU 特性（SMEP/SMAP/NX）、QEMU 机型、显示/块设备组合。
