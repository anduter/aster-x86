# 演进计划（ng 分支）

> 配套 architecture.md。执行会话按工作包（WP）推进；每个 WP 独立提交、独立门禁。
> 原则：**重写底层，移植中层，新写顶层**；不整体重写。

## 0. 总原则

1. main 冻结在 69ab006；打 tag v0.75-32bit-final。
2. 新分支 ng；旧树只做参考和 parity oracle，不再加功能。
3. 一个 WP 一个会话；按路径所有权，不跨域改文件。
4. 每个 WP 必须有出口测试和 claims；没有出口不算完成。
5. D0 之前不加“顺手功能”；范围变更先改 architecture.md / decisions.md。
6. 每天至少一个可演示产物（截图/串口/命令）。

## 1. 阶段与时间盒

| 阶段 | 内容 | 时间盒 | 出口 |
|---|---|---|---|
| E0 准备 | tag/branch/docs/CI/域检查/工具链 | 1 周 | ng 可构建；check-domains 生效 |
| E1 64 位核心 | boot、long mode、页表、trap、syscall、PMM/heap、调度、IPC、ELF64 | **6 周（硬上限）** | ring3 hello + host 单测 |
| E2 服务/驱动移植 | VFS/ramfs、FAT16、fb/input/blk、auth/log/pkg、shell | 8 周 | 64 位 shell + 100 项 parity |
| E3 桌面会话 S1 | display server、shell/WM、session manager、SDK alpha、demo | 8 周 | 杀 shell 应用不死 |
| E4 D0 收口 | 故障注入、外部开发者测试、ABI 冻结、文档/发布 | 4 周 | D0 出口（见 architecture.md） |
| E5 D1 | POSIX Profile、TLS/HTTP、浏览器 B0、真实硬件、8h 浸泡 | 按 D1 计划 | D1 出口 |

E1 是硬时间盒：6 周内没跑到 ring3，停下来重新评估（增加移植、缩范围或调整架构），不要无限期调页表。

## 2. 工作包（WP）

### E0

| ID | 内容 | 主要路径 | 出口 |
|---|---|---|---|
| WP0-1 | main 打 tag、开 ng、冻结旧树 | git | tag 存在；ng 可构建 |
| WP0-2 | 域目录 + 文档 + 所有权 | docs/design、src/* | architecture/evolution/decisions 入库 |
| WP0-3 | check-domains + CI 骨架 | tools/ci | 向上依赖会失败 |
| WP0-4 | 64 位工具链探针 | tools/ | x86_64 编译器可用；hello 能链 |

### E1

| ID | 内容 | 主要路径 | 出口 |
|---|---|---|---|
| WP1-1 | boot 入口：multiboot2/UEFI stub、long mode、串口 | src/kernel/boot | 串口打印 64 位 hello |
| WP1-2 | 4 级页表、W^X/NX、SMEP/SMAP | src/kernel/mm | 页表单测 + 保护生效 |
| WP1-3 | trap/syscall：IDT、syscall 指令、ABI 1.0 草案 | src/kernel/proc | 用户态可进内核再返回 |
| WP1-4 | PMM/heap 迁移 | src/kernel/mm | host 单测通过 |
| WP1-5 | 调度/线程/上下文切换 | src/kernel/proc | 两个线程可切换 |
| WP1-6 | IPC/cap：端口、句柄、cap、close | src/kernel/svc | ping/reply 单测 |
| WP1-7 | ELF64 加载器 + ring3 hello | src/kernel/user、src/apps | ring3 打印并退出 |
| WP1-8 | host 单测 64 位化 + ng CI 门禁 | tests/、tools/ci-ng.ps1 | 单测全 PASS；一条命令门禁 |

E1 出口状态（2026-10-31）：**E1 完成，待打 tag `ng-e1`**。WP1-1..WP1-8 全部
完成：

- `tools/unit-ng.ps1` host 单测 **1085 checks, 0 failures**（WP1-7 为 901；
  WP1-8 补 uaccess 页边界 55、syscall 分发 81、handle/ipc 极端用例 48）；
- `tools/run-ng.ps1` 39 条期望行 + 11 条正则全绿：ring3 由 ELF64 加载器加载
  `hello` 并打印 `ng: ring3 hello`、正常退出（见 `docs/claims/ng-WP1-7.md`）；
- `tools/ci-ng.ps1` 一条命令跑 check-domains / check-deps / unit-ng / build-ng /
  run-ng / 旧 32 位 `build.ps1 -Test`，全绿输出 `=== NG CI PASS ===`
  （`-Quick` 跳 QEMU/legacy）；`.github/workflows/ng.yml` 在 ubuntu-latest 上
  跑 `-Quick` + 非阻塞 QEMU 冒烟；
- 旧 32 位 **305/305**；check-domains / check-deps 0 违规
  （见 `docs/claims/ng-WP1-8.md`）。

### E2

| ID | 内容 | 主要路径 | 出口 |
|---|---|---|---|
| WP2-1 | VFS/ramfs 移植 | src/services/fs | 文件读写/链接/权限测试 |
| WP2-2 | FAT16 移植 | src/services/fs | 宿主交换文件 |
| WP2-3 | fb/virtio-gpu 2D + 键鼠 | src/drivers | 显示字符/图形；输入事件 |
| WP2-4 | 块设备（ATA/virtio-blk） | src/drivers | 读扇区/文件系统挂载 |
| WP2-5 | auth/log/pkg 移植 | src/services | 登录/日志/装包 |
| WP2-6 | shell 移植 | src/apps | 64 位 shell 可交互 |
| WP2-7 | parity 套件 100 项 | tests/ | 100 项通过 |

### E3

| ID | 内容 | 主要路径 | 出口 |
|---|---|---|---|
| WP3-1 | display server：surface/buffer/焦点/扫描输出 | src/desktop/display | 一个窗口显示 |
| WP3-2 | shell/WM：面板、启动器、任务切换 | src/desktop/shell | 可启动/切换应用 |
| WP3-3 | session manager：应用生命周期/窗口表/角色令牌 | src/desktop/session | 应用不归 shell 管 |
| WP3-4 | SDK alpha + demo app | sdk/、src/apps | 外部可编译 GUI app |
| WP3-5 | S1 故障注入：杀 shell/输入 | tests/ | 应用存活、面板回来 |

### E4

| ID | 内容 | 主要路径 | 出口 |
|---|---|---|---|
| WP4-1 | ABI 1.0 冻结文档 | docs/abi | 版本/句柄/错误码定稿 |
| WP4-2 | 外部开发者测试 | docs/、sdk/ | 一天内写出 GUI app |
| WP4-3 | D0 文档 + 发布 | docs/、dist/ | D0 可交付 |
| WP4-4 | 1h smoke（D1 再 8h） | tools/ | 无 panic/停滞 |

## 3. 第一周 checklist

1. git tag v0.75-32bit-final 69ab006；main 只修致命 bug。
2. git switch -c ng。
3. 建目录：src/kernel、src/services、src/drivers、src/desktop、src/apps、sdk、tools/ci。
4. 写 docs/design/domains/*.md（每域一页：职责/接口/失败/manifest）。
5. check-domains 骨架 + CI 接入。
6. 64 位工具链探针（x86_64-elf-gcc 或等价）。
7. boot spike：multiboot2 + long mode + 串口 hello。
8. 每天一个 demo；周五评审 E1 进度。

## 4. 门禁

- 每个 WP：build + 相关 host 单测 + claims；没有 claims 不算完成。
- ng 门禁（E1 起）：`tools/ci-ng.ps1` = check-domains + check-deps + unit-ng +
  build-ng + run-ng + 旧 32 位 `build.ps1 -Test`（`-Quick` 跳 QEMU/legacy）；
  CI 侧 `.github/workflows/ng.yml`。
- check-domains：向上依赖必须失败。
- parity：E2 起 50 项，每个 WP +10，E2 结束 100 项。
- 故障注入：E3 起杀 shell/compositor/inputd，按 architecture.md 第 3 节断言。
- 8h 浸泡：E4 之前 1h smoke；D1 出口 8h。
- 文档：每个 WP 必须更新对应 domain 文档或 decisions.md。

## 5. 分支与提交

- main：冻结；只接受致命 bugfix。
- ng：唯一开发分支；每阶段打 tag（ng-e1、ng-e2…）。
- 提交按路径限定，不用 git add -A。
- claims 放 docs/claims/ng-<WP>.md，写清期望的串口/测试行。

## 6. 风险与回退

| 风险 | 应对 |
|---|---|
| E1 超时（页表/启动调不通） | 6 周硬停；评估改用现成 boot 协议/减少范围 |
| FS/驱动移植踩 32 位假设 | parity 测试先行；保留旧树 oracle |
| L2 范围膨胀（S2/S3 提前） | D0 只做 S1；S2/S3 进 D1 |
| SDK/POSIX 分叉 | ABI 1.0 在 64 位后冻结；POSIX 用户态 |
| 多会话冲突 | 路径所有权；一个 WP 一个会话 |
| 动力/演示缺失 | 每天 demo；阶段 tag 可回退 |

## 7. 与旧树的关系

- 旧树（69ab006）是**行为 oracle**：305 项检查、FS/网络/驱动语义都可对照。
- 旧树不删、不更新功能；只修致命 bug。
- 新树每移植一个模块，从旧测试里挑对应用例进 parity 套件。
