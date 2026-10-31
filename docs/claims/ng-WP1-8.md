# ng-WP1-8：E1 收口（host 单测补齐 + ng CI 门禁）（PASS）

## 结论

**PASS —— E1 出口达成，待打 tag `ng-e1`。**

- **ring3 hello 仍然跑通**：`tools/run-ng.ps1` 39/39 期望行 + 11 条正则命中，
  用户态 `ng: ring3 hello` → `ng: ring3 exit ok` → `ng: WP1-7 selftest PASS`。
- **host 单测从 901 涨到 1085 checks, 0 failures**（+184）：新增
  `test_uaccess.c`（55 checks）、`test_syscall.c`（81 checks）、
  `test_ipc.c` 的 `t_extremes()`（+48 checks）。
- **一条命令的门禁**：`tools/ci-ng.ps1` 顺序跑
  check-domains → check-deps → unit-ng → build-ng → run-ng → 旧 32 位
  `build.ps1 -Test`，全绿输出 `=== NG CI PASS ===`。
- **GitHub Actions**：新增 `.github/workflows/ng.yml`（ubuntu-latest，clang/lld/
  qemu-system-x86，`tools/ci-ng.ps1 -Quick` + 非阻塞 QEMU 冒烟）。
- **无回归**：旧 32 位 **305/305**；check-domains / check-deps 0 违规；
  内核产物仍是 97184 字节（WP1-7 基线大小）。

本 WP **没有修改 `src/ng/**`**：新增单测全部通过 `tests/unit-ng/fake_ng_env.c`
提供的 fake 环境（页表/串口/GDT/IPC 胶水/ring3 入口）链接真实
`uaccess.c` + `syscall.c`，所以测的是生产代码，而不是复制品。

## E1 出口清单

| 出口（evolution.md） | 证据 |
|---|---|
| 64 位内核可构建、串口 64 位 hello | `build-ng` PASS（97184 bytes，26 步）；`ng: hello 64-bit` |
| 4 级页表 / W^X / NX / SMEP/SMAP | `run-ng` 的 WP1-2 行 + 正则（`smep/smap`） |
| trap/syscall：IDT、`syscall` 指令、ABI 草案 | `ng: syscall ring0 ok` / `ng: ring3 syscall ok` / `ng: WP1-3 selftest PASS` |
| PMM/heap 迁移 + host 单测 | `test_pmm` / `test_heap` / `test_memmap` 全部在 1085 checks 内 |
| 调度/线程/上下文切换 | WP1-5 行 + `test_sched` |
| IPC/cap：端口、句柄、close | WP1-6 行 + `test_ipc`（含新极端用例） |
| **ring3 hello** | `ng: ring3 exec ok` / `ng: ring3 hello` / `ng: ring3 exit ok` |
| **host 单测 64 位化** | `unit-ng: PASS (1085 checks, 0 failures)` |
| **CI 门禁** | `ci-ng.ps1` 全量 PASS + `.github/workflows/ng.yml` |
| **回归** | 旧 32 位 305/305；check-domains / check-deps 0 违规 |

## 交付物

| 路径 | 内容 |
|---|---|
| `tools/ci-ng.ps1` | E1 门禁：6 步顺序执行、每步 PASS/FAIL/SKIP、失败尾日志、`=== NG CI PASS ===`（**新**） |
| `tests/unit-ng/fake_ng_env.h` / `.c` | host 替身：fake 4 KiB 页表、串口捕获、GDT RSP0 记录、fake ring3 入口、`ipc_sys_*` 记录（**新**） |
| `tests/unit-ng/test_uaccess.c` | copyin/copyout/copyin_str 参数/权限/页边界（55 checks，**新**） |
| `tests/unit-ng/test_syscall.c` | 调用号分发、write 边界、IPC 参数搬运、ring3 统计、per-thread 内核栈（81 checks，**新**） |
| `tests/unit-ng/test_ipc.c` | `t_extremes()`：最大消息 / max=0 / depth=64 / 空 payload / stale 句柄（+48 checks，**改**） |
| `tests/unit-ng/unit_main.c` | 注册 `test_uaccess` / `test_syscall`（**改**） |
| `tools/unit-ng.ps1` | 10 个编译单元（加 `uaccess.c`、`syscall.c`、fake/env + 两个 test），标题改 WP1-8（**改**） |
| `tools/run-ng.ps1` | 跨平台：`Start-Process` 不再硬传 `-WindowStyle`（Linux）、QEMU 默认路径仅 Windows、缺产物时用当前 PowerShell 宿主重构建（**改**） |
| `.github/workflows/ng.yml` | ubuntu-latest：apt 装 clang/lld/qemu-system-x86 → `ci-ng.ps1 -Quick` → 非阻塞 `run-ng` → 上传产物（**新**） |
| `docs/design/README.md` | 当前阶段 = E1 完成（待 tag）、下一步 E2（**改**） |
| `docs/design/evolution.md` | E1 状态与 WP1-8 出口（**改**） |

未修改 `src/ng/**`、`src/kernel/**`、`build.ps1`、`tools/ci.ps1`、`abi.h`。

## 新增 host 单测（+184 checks）

### `test_uaccess.c`（55 checks）

`uaccess.c` 在 host 上直接链接，页表换成 fake：测试把宿主静态缓冲区的页
登记成 U=1/W=1 的“用户页”，拷贝真的读写那块内存。

- 参数：`uva=0`、`len=0`、未映射、`uva+len-1` 64 位回绕、
  末地址超过 `0x00007FFF_FFFF_FFFF`、`pml4=NULL`；
- 单页/整页/页尾单字节；末字节跨入未映射页 → EFAULT；
- copyin/copyout 字节内容；空 dst/src；坏指针时 dst 原样（校验先于拷贝）；
- 跨两个映射页的 8 字节拷贝内容正确（页边界不是“按页粗判”）；
- 权限：W=0 页可读不可写（copyout 拒绝且目标不变）、U=0 内核页读写都拒绝、
  第一页可写第二页只读的跨页写整体拒绝；
- `copyin_str`：正常串、空串、无 NUL 截断（返回 `max-1` 且 NUL 结尾）、
  `max=1`、跨页坏指针（已拷入的字节保留、失败点写 NUL）、`dst=NULL`、
  `max=0`、`uva=0`。

### `test_syscall.c`（81 checks）

`syscall.c` 在 host 上直接链接：串口/页表/GDT/IPC 胶水是 fake，
`ring3_enter()` 换成“执行测试注册的 hook”，hook 往 `syscall_dispatch()`
灌 frame —— 等价于用户态连发 syscall，而 `syscall_run_user()` 的统计/退出
路径全部是真代码。

- 分发：未知调用号 = `-1`；write(rdi=0) 的 ring0/ring3 诊断行；exit 的 ring0 行；
- write 边界：`len=0` / `len=257` → EINVAL(-22)、`len=256` 成功且串口正好 256
  字节、未映射指针与跨页指针 → EFAULT(-14)、好指针返回长度并输出字节；
- IPC 调用号 10..14：`rdi/rsi/rdx/r10` 原样搬到 `ipc_sys_*`（含 timeout 在
  r10），返回值透传（含负错误码两补码 `-110 == 0xFFFF_FFFF_FFFF_FF92`）；
- `syscall_run_user`：entry=0 / rsp<lo / rsp>=hi / hi<=lo → `-1`；
  正常 write+exit → `syscalls=2, writes=1, bytes=5, exit_code=0x2A, exited=1,
  user_rsp_ok=1, bad_writes=0`；exit_code 全 1 位保留；无 exit → `-1`；
  RSP 不在区间 → `user_rsp_ok=0`；ring3 坏指针 → `bad_writes=1`；
  rdi=0 打 ring3 诊断行；`out=NULL` 可用；
- per-thread 栈：`syscall_set_kernel_stack(0)` 不清 RSP0、
  非 0 值同步 `syscall_kernel_rsp0` + TSS.RSP0、`set_thread_stack(NULL)` /
  `stack_top==NULL` 回退 `syscall_stack_top`、有 TCB 栈顶时用 TCB 值。

### `test_ipc.c` 的 `t_extremes()`（+48 checks）

- 最大消息：16 字节头 + 4096 payload = `IPC_MSG_MAX` 整条 send/recv，
  内容与 `hdr.len` 正确；
- `hdr.len = PAYLOAD_MAX+1` → EMSGSIZE（总长仍在 `IPC_MSG_MAX` 内，走的是
  hdr 校验分支而不是长度早退）；
- `recv` 的 `max=0` / `buf=NULL`：只返回总长，一个字节都不写；
- `depth=64` 上限：填满 64 条后第 65 条非阻塞 EAGAIN，随后 64 条全部排空；
- 零 payload 的 reply：16 字节头消息走完 ticket 往返；
- 句柄表：句柄 0、slot=64（越界）、错误 generation 都 EBADF 且
  `bad_lookups` 计数正确；close 后同槽复用，旧句柄失效、新句柄可用。

## 证据（2026-10-31，Windows PS 5.1，QEMU 11.0.0）

### `tools/ci-ng.ps1 -Quick`

```text
=== ci-ng (E1 gate) ===
repo:   D:\OS_Project
host:   C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe
mode:   quick
legacy: skip

--- 1/6 domain boundaries ---
  [PASS] check-domains  (0 violations; exit=0, 0.5s)
--- 2/6 module boundaries ---
  [PASS] check-deps  (0 violations; exit=0, 0.5s)
--- 3/6 host unit tests (unit-ng) ---
  [PASS] unit-ng  (1085 checks; exit=0, 2.0s)
--- 4/6 build kernel (build-ng) ---
  [PASS] build-ng  (97184 bytes, user-elf 13152 bytes; exit=0, 2.1s)
--- 5/6 QEMU ring3 regression (run-ng) ---
  [SKIP] run-ng  (-Quick 跳过 QEMU)
--- 6/6 legacy 32-bit parity (build.ps1 -Test) ---
  [SKIP] legacy-32bit  (-Quick 跳过 legacy)

=== ci-ng summary ===
  PASS check-domains    0 violations; exit=0, 0.5s
  PASS check-deps       0 violations; exit=0, 0.5s
  PASS unit-ng          1085 checks; exit=0, 2.0s
  PASS build-ng         97184 bytes, user-elf 13152 bytes; exit=0, 2.1s
  SKIP run-ng           -Quick 跳过 QEMU
  SKIP legacy-32bit     -Quick 跳过 legacy
  4 passed, 0 failed, 2 skipped, 5.0s

=== NG CI PASS ===
```

### `tools/ci-ng.ps1`（全量，含 QEMU + legacy）

```text
--- 5/6 QEMU ring3 regression (run-ng) ---
  [PASS] run-ng  (39/39 lines + 11 patterns; exit=0, 0.5s)
--- 6/6 legacy 32-bit parity (build.ps1 -Test) ---
  [PASS] legacy-32bit  (305/305 parity checks; exit=0, 54.5s)

  PASS check-domains    0 violations; exit=0, 0.5s
  PASS check-deps       0 violations; exit=0, 0.5s
  PASS unit-ng          1085 checks; exit=0, 1.9s
  PASS build-ng         97184 bytes, user-elf 13152 bytes; exit=0, 2.1s
  PASS run-ng           39/39 lines + 11 patterns; exit=0, 0.5s
  PASS legacy-32bit     305/305 parity checks; exit=0, 54.5s
  6 passed, 0 failed, 0 skipped, 60.0s

=== NG CI PASS ===
```

### 其它门禁

```text
=== unit-ng (WP1-8 host tests) ===
1085 checks, 0 failures
unit-ng: PASS (1085 checks, 0 failures)

checked: 205 files, 726 quoted includes
domain include violations: 0

checked: 201 files, 650 quoted includes
module include violations: 0
OK: no cross-module include violations.
```

`-NoLegacy` 也验证过（5 passed / 1 skipped，`run-ng` 仍跑，`=== NG CI PASS ===`）；
`-Quick` 与 `-Legacy` 同时给出时 `-Quick` 优先并打印提示。

## GitHub Actions（`.github/workflows/ng.yml`）

- 触发：`push`/`pull_request` 到 `ng` 分支 + `workflow_dispatch`；
  `permissions: contents: read`，`concurrency` 按 ref 取消旧任务；
- `runs-on: ubuntu-latest`，`timeout-minutes: 15`；
- 步骤：
  1. `actions/checkout@v4`；
  2. `sudo apt-get update && sudo apt-get install -y clang lld qemu-system-x86`
     （并打印 clang / ld.lld / qemu 版本）；
  3. 确保 `pwsh`（runner 自带则跳过，否则 `apt-get install -y powershell`）；
  4. `& ./tools/ci-ng.ps1 -Quick`（静态 + unit-ng + build-ng，真门禁）；
  5. `& ./tools/run-ng.ps1 -TimeoutSec 90 -SerialLog serial-ng.log`
     （QEMU 冒烟；托管 runner 无 KVM 走 TCG，`continue-on-error: true`
     只记录日志，不作为门禁）；
  6. `actions/upload-artifact@v4` 上传 `dist-ng/**`、`obj-ng/**`、
     `serial-ng.log`（`if: always()`）。
- YAML 用 PyYAML 6.0.3 `safe_load` 验证通过（1 个 job，6 个 step）；
  命令与本地 `tools/ci-ng.ps1` 完全一致，可直接复制到终端。
- **未在 GitHub 上实际执行**（按任务约定）；旧 32 位套件依赖 Windows
  vendored `i686-elf`，托管 Linux runner 不跑（`ci-ng.ps1` 在非 Windows
  自动 SKIP 并给出原因）。

## 自检项清单

1. `ci-ng.ps1` 6 步全绿；`-Quick` 跳 QEMU/legacy；`-NoLegacy` 单跳 legacy；
   每步失败打印该步日志尾部并汇总 `=== NG CI FAIL (n) ===`。
2. `unit-ng` 1085/0（901 → +184）；新增 uaccess 55、syscall 81、ipc +48。
3. `build-ng` 97184 字节 / 26 步 / user-elf 13152 字节，与 WP1-7 基线一致。
4. `run-ng` 39/39 + 11 patterns（含 `ng: ring3 hello`）。
5. 旧 32 位 305/305。
6. check-domains 0 / check-deps 0。
7. `.github/workflows/ng.yml` YAML 语法正确、step 可复制。
8. 所有被改的 `*.ps1` 带 `EF BB BF`（PS 5.1 中文注释不乱码）。
9. 路径所有权：只改 tools/tests/ci/docs/.github；`src/ng/**` 零改动。

## 未做 / 遗留（按 WP 边界）

- **tag `ng-e1` 待打 / 未 push**：本会话只提交，不 push、不打 tag（按约定）。
- **GitHub Actions 未实际跑**：YAML 与命令已验证，托管 runner 的 QEMU 冒烟
  是非阻塞步骤；若要在 CI 里把 ring3 变成硬门禁，需要 runner 有 KVM 或接受
  TCG 的稳定性评估。
- **legacy 32 位仅 Windows**：vendored `i686-elf` 不在仓库里，Linux 上
  `ci-ng.ps1` 会 SKIP（不是 PASS，汇总里单独标 SKIP）。
- **SMAP=on 的 copyin 路径**仍只由 QEMU 覆盖：host 用户态不能执行 `stac/clac`，
  host 单测只走 SMAP=off 分支（与 WP1-7 的 `-cpu qemu64` 证据互补）。
- **ring3 真实指令往返**（`sysretq`/`iretq`/CR3 切换）仍由 `run-ng` 覆盖；
  host 的 syscall 单测替换了 `ring3_enter`，不执行特权指令。
- **IPC 等待者池耗尽**（`IPC_WAITER_MAX`）没有 host 用例：占满 12 个等待者需要
  嵌套 fake sleep，容易把测试写成死循环，价值低；现有 close/超时/唤醒用例
  已覆盖等待队列生命周期。
- WP1-7 的遗留照旧：用户态异常投递、进程模型、从 FS 加载 ELF、无 ASLR/
  mmap/mprotect、write ≤256 字节、无用户 SDK、仍协作式调度。

## 出口

```text
powershell -File tools/ci-ng.ps1             # PASS，6/6 步（60.0s）
powershell -File tools/ci-ng.ps1 -Quick      # PASS，4/6 步（QEMU/legacy SKIP）
powershell -File tools/unit-ng.ps1           # PASS，1085 checks, 0 failures
powershell -File tools/build-ng.ps1          # PASS，97184 bytes
powershell -File tools/run-ng.ps1            # PASS，39/39 + 11 patterns
powershell -File build.ps1 -Test             # PASS，305/305
pwsh -File tools/ci-ng.ps1 -Quick            # ubuntu runner 入口（ng.yml 使用）
```
