# ng-WP2-7：parity 套件 100 项 + E2 出口（PASS）

## 结论

**PASS** —— E2 出口达成，**待打 tag `ng-e2`**（tag 由负责人打，本会话不 push）。

- **parity 100/100**：内核启动自检逐项打印 `parity: NNN/100 [域] 描述 ... ok`，
  汇总 `ng: parity 100/100 PASS`；host 单测跑其中可 host 化的 **95/95**
  （显示/输入 5 项需要 QEMU 的真实 VBE/PS2 状态）。
- **一套 case 表、一套断言体**：`tests/parity/parity_cases.h`（编号/域/描述）
  + `tests/parity/parity_body.inc`（90 项共享断言，只用 L1 公开 API）。
  内核 harness 是 `src/ng/parity_selftest.c`，host harness 是
  `tests/unit-ng/test_parity.c`；ATA 两侧各走真实端口 / fake 控制器，
  显示/输入只在 QEMU 里跑。**不存在“host 一套、内核另一套”的语义漂移**。
- **执行方式**：
  - 内核自检：`tools/run-ng.ps1` 一次启动就覆盖（parity 排在所有服务/驱动
    自检之后、ring3 demo 之前）；
  - 独立门禁：`tools/parity-ng.ps1` 跑（或复用）`run-ng` 日志，断言
    `ng: parity 100/100 PASS`、逐项恰好 100 条且编号 001..100 连续、无
    `parity FAIL`；
  - `tools/ci-ng.ps1` 增加第 6 步 `parity-ng`（复用第 5 步 run-ng 的日志，
    不重复启动 QEMU）。
- **未新增功能**：100 项全部是把旧树 305 项 + WP2-1..6 claims 的既有语义
  钉成回归断言。**没有发现需要修复的产品 bug**（100 项在 host 与内核均为
  一次通过；见“观察/遗留”里的 log errno 文档级不一致，未改代码）。
- **无回归**：`ci-ng` 7/7 PASS（check-domains / check-deps / unit-ng /
  build-ng / run-ng / parity-ng / legacy-32bit）；host 单测 3108 → **3203**
  checks, 0 failures；旧 32 位 **305/305**。

## 交付物

| 路径 | 内容 |
|---|---|
| `tests/parity/parity_cases.h` | 100 项 case 表（X-macro：编号/域/描述）+ 计数静态断言（**新**） |
| `tests/parity/parity_body.inc` | 90 项共享断言体 + 失败原因格式化 + 分发（**新**） |
| `src/ng/parity_selftest.[ch]` | 内核 harness：case 表打印、真实 ATA、真实显示/输入、shell 钩子（**新**） |
| `tests/unit-ng/test_parity.c` | host harness：自建 ramfs 根 + fake ATA + 95 项（**新**） |
| `src/ng/main.c` | WP2-7 接线：WP2-3 之后、ring3 demo 之前；PASS/FAIL + log（**改**） |
| `src/ng/wp23.h` / `wp23_selftest.c` | 只读诊断接口 `ng_display_fb/text`、`ng_input_state`（**改**，仅暴露指针） |
| `tools/parity-ng.ps1` | parity 门禁：独立跑 run-ng 或 `-LogFile` 复用日志（**新**，BOM） |
| `tools/run-ng.ps1` | +2 期望行（`ng: parity 100/100 PASS` / `ng: WP2-7 selftest PASS`）+2 正则（001/100）与失败模式（**改**，BOM） |
| `tools/build-ng.ps1` | 编 `parity_selftest.c` + `-I tests/parity` + 一份 `-DNG_HOST_TEST` 的 shell.c（tokenizer 回归）（**改**，BOM） |
| `tools/unit-ng.ps1` | 加 `test_parity.c` 与 `-I tests/parity`（**改**，BOM） |
| `tools/ci-ng.ps1` | 第 6 步 parity-ng（复用 run-ng 日志）；步骤编号 7 步（**改**，BOM） |
| `docs/claims/ng-WP2-7.md` | 本文件（100 项表 + E2 出口清单 + 遗留）（**新**） |
| `docs/design/evolution.md` | E2 完成状态、parity 门禁说明（**改**） |
| `docs/design/README.md` | 当前阶段 = E2 完成待 tag（**改**） |

## 100 项（编号 / 域 / 描述 / 结果）

结果来自 2026-10-31 `tools/run-ng.ps1`（QEMU 11.0.0，`-cpu max`，stdvga）与
`tools/unit-ng.ps1`（host）；`-Fallback`（`-vga none`）同样 100/100。

| 编号 | 域 | 描述 | 结果 |
|---|---|---|---|
| 001 | vfs | normalize: "/" -> "/" | PASS |
| 002 | vfs | normalize: "//a//b/" -> "/a/b" | PASS |
| 003 | vfs | normalize: "." with cwd /x -> /x | PASS |
| 004 | vfs | normalize: ".." with cwd /x/y -> /x | PASS |
| 005 | vfs | normalize: ".." above root stays / | PASS |
| 006 | vfs | normalize: empty/NULL path -> EINVAL | PASS |
| 007 | vfs | normalize: overlong path -> ENAMETOOLONG | PASS |
| 008 | vfs | open: empty and overlong path rejected | PASS |
| 009 | vfs | open: missing path -> ENOENT | PASS |
| 010 | vfs | open: dir RDWR -> EISDIR, file O_DIRECTORY -> ENOTDIR | PASS |
| 011 | vfs | open: file as middle component -> ENOTDIR | PASS |
| 012 | vfs | unlink: dir -> EISDIR, missing -> ENOENT | PASS |
| 013 | vfs | mkdir: existing -> EEXIST, missing parent -> ENOENT | PASS |
| 014 | vfs | fd: bad fd read/write/close -> EBADF | PASS |
| 015 | vfs | fd: stale generation after close -> EBADF | PASS |
| 016 | vfs | fd: table full -> EMFILE, then all closable | PASS |
| 017 | vfs | symlink: relative target resolves, readlink ok | PASS |
| 018 | vfs | symlink: loop -> ELOOP | PASS |
| 019 | vfs | perm: uid!=0 owner bits (read/write) -> EPERM | PASS |
| 020 | vfs | chdir: relative open and ".." normalization | PASS |
| 021 | ramfs | create/write/stat/read small file | PASS |
| 022 | ramfs | 5000-byte cross-page write/read | PASS |
| 023 | ramfs | O_APPEND writes at end | PASS |
| 024 | ramfs | O_TRUNC clears file | PASS |
| 025 | ramfs | O_CREAT|O_EXCL on existing -> EEXIST | PASS |
| 026 | ramfs | truncate shrink -> EOF at new size | PASS |
| 027 | ramfs | truncate grow -> zero fill | PASS |
| 028 | ramfs | unlink hides file, new ino on recreate | PASS |
| 029 | ramfs | nested mkdir + listdir entries | PASS |
| 030 | ramfs | symlink/readlink/stat through link | PASS |
| 031 | ramfs | rename file, old path gone | PASS |
| 032 | ramfs | rename onto existing -> EEXIST | PASS |
| 033 | ramfs | unlink while open keeps data readable | PASS |
| 034 | ramfs | single-file 1 MiB limit -> ENOSPC | PASS |
| 035 | ramfs | empty file read EOF then write | PASS |
| 036 | fat16 | mount synthetic image geometry | PASS |
| 037 | fat16 | bad boot signature -> EINVAL | PASS |
| 038 | fat16 | bad bytes-per-sector -> EINVAL | PASS |
| 039 | fat16 | bad sectors-per-cluster -> EINVAL | PASS |
| 040 | fat16 | bad FAT count -> EINVAL | PASS |
| 041 | fat16 | bad total sectors -> EINVAL | PASS |
| 042 | fat16 | create/write/read file | PASS |
| 043 | fat16 | 1500-byte cross-cluster write/read | PASS |
| 044 | fat16 | truncate shrink and grow (zero fill) | PASS |
| 045 | fat16 | unlink frees clusters and entry | PASS |
| 046 | fat16 | rename file preserves data | PASS |
| 047 | fat16 | rename onto existing -> EEXIST | PASS |
| 048 | fat16 | mkdir + subdir readdir | PASS |
| 049 | fat16 | unlink directory -> EISDIR | PASS |
| 050 | fat16 | 8.3 name limits and bad chars | PASS |
| 051 | fat16 | cluster chain loop -> EIO | PASS |
| 052 | fat16 | truncated chain -> EIO | PASS |
| 053 | fat16 | bad cluster marker -> EIO | PASS |
| 054 | fat16 | out-of-range cluster -> EIO | PASS |
| 055 | fat16 | duplicate create EEXIST, readdir end 0 | PASS |
| 056 | ata | IDENTIFY present/sectors/model | PASS |
| 057 | ata | read sector 0 stable, BPB signature | PASS |
| 058 | ata | LBA/count/NULL parameter rejection | PASS |
| 059 | ata | last-sector write/readback/restore | PASS |
| 060 | ata | pure lba range/devsel/status class | PASS |
| 061 | auth | init seeds 2 users | PASS |
| 062 | auth | root login -> uid 0 | PASS |
| 063 | auth | alice login -> uid 1000 | PASS |
| 064 | auth | wrong password -> EACCES | PASS |
| 065 | auth | unknown user -> ENOENT | PASS |
| 066 | auth | empty name/pass -> EINVAL | PASS |
| 067 | auth | /etc/passwd has two well-formed records | PASS |
| 068 | auth | hash deterministic; iterations 4096 | PASS |
| 069 | auth | salt changes digest; iterations 0 EINVAL | PASS |
| 070 | auth | 5 failures lock user, correct pass rejected | PASS |
| 071 | log | write/read roundtrip line format | PASS |
| 072 | log | tag/msg validation boundaries | PASS |
| 073 | log | log_read truncates to max bytes | PASS |
| 074 | log | 64 KiB ring drops oldest lines | PASS |
| 075 | log | log_init resets state | PASS |
| 076 | pkg | init seeds one package | PASS |
| 077 | pkg | list returns hello 1.0.0 | PASS |
| 078 | pkg | verify hello OK | PASS |
| 079 | pkg | verify unknown -> ENOENT | PASS |
| 080 | pkg | manifest parse valid -> all fields | PASS |
| 081 | pkg | manifest missing field -> EINVAL | PASS |
| 082 | pkg | manifest unknown key -> EINVAL | PASS |
| 083 | pkg | manifest duplicate key -> EINVAL | PASS |
| 084 | pkg | manifest bad sha256 hex -> EINVAL | PASS |
| 085 | pkg | tampered entry -> EIO, restore -> OK | PASS |
| 086 | shell | tokenize "echo hi" | PASS |
| 087 | shell | tokenize redirect separated | PASS |
| 088 | shell | tokenize redirect attached | PASS |
| 089 | shell | tokenize whitespace/collapse | PASS |
| 090 | shell | tokenize single command | PASS |
| 091 | shell | tokenize duplicate redir -> -4 | PASS |
| 092 | shell | tokenize missing redir path -> -3 | PASS |
| 093 | shell | tokenize empty line -> -2, too many args -> -1 | PASS |
| 094 | shell | resolve relative path against cwd | PASS |
| 095 | shell | resolve absolute/.. and empty -> EINVAL | PASS |
| 096 | disp | display: VBE framebuffer or VGA text ready | PASS |
| 097 | disp | display: framebuffer pixel readback (or text cell) | PASS |
| 098 | disp | display: console grid/cursor geometry | PASS |
| 099 | input | PS/2 queue empty -> readkey EAGAIN | PASS |
| 100 | input | scancode decode shift/caps/ctrl/release | PASS |

域分布：vfs 20、ramfs 15、fat16 20、ata 5、auth 10、log 5、pkg 10、shell 10、
disp 3 + input 2（显示/输入合计 5）。

## E2 出口清单

| 出口项（evolution.md） | 状态 | 证据 |
|---|---|---|
| 64 位 shell 可交互 | ✅ | WP2-6：`ng: shell ready` + QMP 注入 8 条命令 + `ng: shell exit (code=0 ...)` |
| VFS/ramfs | ✅ | WP2-1 + parity 001..035 |
| FAT16 | ✅ | WP2-2/2-4 + parity 036..055 |
| fb/virtio-gpu 2D + 键鼠 | ✅（VBE 2D + PS/2 键盘） | WP2-3 + parity 096..100 |
| 块设备 | ✅ | WP2-4（ATA PIO）+ parity 056..060 |
| auth/log/pkg | ✅ | WP2-5 + parity 061..085 |
| **100 项 parity** | ✅ | 本会话：内核 100/100、host 95/95 |
| 无回归 | ✅ | unit-ng 3203/0、run-ng 108/108+42、legacy 305/305、ci-ng 7/7 |

## 执行证据（2026-10-31，`D:\OS_Project`，分支 `ng`）

```
# 内核 parity（run-ng 日志）
parity: 001/100 [vfs] normalize: "/" -> "/" ... ok
...
parity: 100/100 [input] scancode decode shift/caps/ctrl/release ... ok
ng: parity 100/100 PASS
ng: WP2-7 selftest PASS
run-ng: matched 108/108 expected lines + 42 patterns
run-ng: PASS (all expected lines in 4.6s, cpu max)

# 独立门禁
parity-ng: PASS (100/100 items, 0 FAIL, 10 domains)

# host
parity(host): 95/95 PASS (5 display/input cases are kernel-only)
unit-ng: PASS (3203 checks, 0 failures)

# 一条命令门禁
7 passed, 0 failed, 0 skipped ... === NG CI PASS ===
```

其他模式（同一次会话验证）：

| 模式 | 结果 |
|---|---|
| `run-ng`（stdvga，默认） | 108/108 + 42，parity 100/100 |
| `run-ng -Fallback`（`-vga none`） | 100/100（disp 走 VGA 文本分支） |
| `run-ng -NoQmp` | 94/94 + 38，parity 100/100 |
| `build-ng -Smoke` + `run-ng -Smoke` | 95/95 + 38，parity 100/100 |
| `unit-ng`（host） | 3203 checks / 0 failures（其中 parity 95） |
| 旧 32 位 `build.ps1 -Test` | 305/305 |
| `ci-ng`（全量） | 7/7 PASS，60.5s |
| `ci-ng -Quick` | 4 pass / 3 skip（QEMU 与 legacy 跳过） |

## 边界 / 明确不做

- parity 是**回归**，不是新功能：没有新增 syscall、文件系统特性或 shell 命令。
- 100 项只覆盖 WP2-1..6 已交付的语义；旧树 305 项里依赖旧 32 位内核结构
  （inode v6 审计镜像、socket/pipe、SMP 等）的检查不属于 E2 服务层，未搬。
- 显示/输入 5 项只能在 QEMU 里证明（真实 LFB / PS/2 控制器状态）；host 的
  `test_drivers.c` 继续覆盖纯逻辑，二者互补。
- parity 用例依赖 live 内核状态，因此 harness 会重建 auth/log/pkg（幂等）
  并在 case 070 末尾重新 `auth_init` 解锁 alice（ring3 svc demo 还要登录）。
  这本身也是回归点，写在 case 070 的断言里。
- 不改 `src/kernel/**`、`build.ps1`、`tools/ci.ps1`、`abi.h`；旧树未动。

## 观察 / 遗留

1. **未发现产品 bug**：100 项首次实现即全绿；`src/services/**` 没有为 parity
   改动任何一行。
2. **文档级不一致（未改，避免扩大范围）**：`log.h` 定义了 `LOG_ENAMETOOLONG`，
   但 `log_write()` 对超长 tag/msg 实际返回 `LOG_EINVAL`（`text_ok()` 统一
   拒绝）。parity 按**实际语义**断言 EINVAL；若后续要对齐 POSIX 风格，需在
   L1 改代码并同步 claims。
3. **shell 回归的编译方式**：内核为了跑 tokenizer 用例，额外把
   `src/apps/shell/shell.c` 以 `-DNG_HOST_TEST` 编成 `shell_parity.o`（只取
   `sh_test_tokenize/sh_test_resolve` 钩子，不执行 `shell_main`）。这是 L0
   自检对 L3 解析器的“测试用编译”，不改变 ring3 的加载路径；`sh_host_syscall`
   由 parity harness 提供 `-ENOSYS` 兜底。
4. **parity 顺序**：WP2-7 排在 WP2-3 之后、WP1-7 之前；如果后续 WP 插在中间，
   需保持“所有被 parity 依赖的服务已初始化、ring3 demo 尚未运行”的约束。
5. **E3 起**：parity 应随 display server / WM / session manager 增加新项
   （当前 100 项是 E2 基线，不再是上限）。

## 环境

- 分支 `ng`，基线 HEAD `7fc36f2`（WP2-6）；仓库 `D:\OS_Project`。
- clang / ld.lld 23.1.4（`D:\msys64\mingw64\bin`）；
  内核 `clang --target=x86_64-elf -fuse-ld=lld -ffreestanding -nostdlib
  -mno-red-zone -fno-stack-protector -mgeneral-regs-only`。
- QEMU 11.0.0：`-kernel dist-ng/aster-ng.elf -m 256 -cpu max -vga std
  -drive file=dist-ng/disk.img,format=raw,if=ide -display none`，QMP
  `input-send-event` 注入按键；`-Fallback` 用 `-vga none`。
- 提交：`[ng][WP2-7] ...`，不 push；`.ps1` 文件均带 UTF-8 BOM。
