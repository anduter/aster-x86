# ng-WP2-6：用户态 shell 移植（PASS）

## 结论

**PASS** —— E2 / WP2-6 出口达成，`src/apps/shell` 成为 ng 分支第一个独立编译、
独立加载的 **L3 应用**：

- **64 位用户态 shell**（`src/apps/shell/shell.c`，无 libc，单独编译成 ELF64，
  链接到 0x40000000）：`ng> ` 提示符；`readkey` 行编辑（可打印字符/Backspace/
  Enter/Ctrl-L 清屏/Ctrl-C 丢弃当前行/Ctrl-U 删整行/Ctrl-D 退出）；
- **15 个命令**：`help`、`ls [path]`、`cat <file>`、`echo <text> [> path]`、
  `mkdir <dir>`、`rm <path>`、`cd [dir]`、`pwd`、`stat <path>`、`dmesg [-n N]`、
  `log <tag> <msg>`、`pkg list|verify <name>`、`login <user> <pass>`、`clear`、
  `exit [code]`；shell 自己维护 cwd（20..27 没有 chdir），每个文件操作先解析成
  绝对路径；错误打印 `cmd: arg: <errno 文本>` 并把状态置 1，`exit` 无参数时
  用该状态退出（非零），任何错误都不 panic；
- 所有文件操作走 **syscall 20–27**（open/read/write/close/stat/listdir/mkdir/
  unlink），auth **31**、log **32/33**、pkg **34/35**、输入 **30**、输出 **0**、
  退出 **1**；用户指针全部由内核 uaccess 校验；
- **构建/装载**：`tools/build-ng.ps1` 编译 `shell.c` 为 ELF64（22760 字节、
  3 PT_LOAD、entry 0x400007a0、end 0x40004dd0），校验 W^X/4 KiB 对齐/64 KiB
  窗口后以 `ng_shell_elf_start..end` 嵌入 `.rodata`；内核在 WP1-7 ring3 hello
  之后加载 shell 进新用户地址空间（16 KiB 栈）并进 ring3 交互。`-Smoke` 编出
  的内核跳过 shell（`ng: shell skipped (smoke mode)`），保留 hello 冒烟路径；
- **run-ng 自动化**：QEMU 起来后经 QMP `input-send-event` 逐字符注入
  `help` → `ls /` → `cat /etc/passwd` → `echo hi > /tmp/s.txt` →
  `cat /tmp/s.txt` → `dmesg -n 3` → `pkg list` → `exit`（`>` 用 shift+dot），
  每步等对应证据行；校验 `ng: shell ready`、
  `ng: shell ls/cat/write/dmesg/pkg/exit ok`、
  `ng: shell exit (code=0 ...)`、`ng: shell session file ok (s.txt=3)`、
  `ng: shell session pkg ok (lists=+1)`、`ng: WP2-6 selftest PASS`；
- **门禁**：unit-ng **3108 checks, 0 failures**（+317）；run-ng
  **106/106 期望行 + 40 条正则**（Fallback 105/105 + 34；NoQmp 92/92 + 36；
  qemu64 106/106 + 40；Smoke 93/93 + 36）；旧 32 位 **305/305**；
  check-domains / check-deps 0 违规；ci-ng **6/6 PASS**。

顺带修掉一个 WP1-7 遗留的 ring3 返回缺陷：`ring3_enter` 没有保存/恢复
callee-saved 寄存器（rbx/rbp/r12..r15），syscall 1 退出后 `syscall_run_user()`
拿到的是用户寄存器的值 —— shell 的 rbx 指向用户栈，写 `*out` 触发 #PF → PANIC。
`src/ng/isr.S` 现在在恢复点保存/恢复这些寄存器（对 hello 路径无行为变化）。

`src/kernel/**`、`build.ps1`、`tools/ci.ps1`、`abi.h` 未修改；不做 GUI/桌面
（E3）、不做 POSIX、不做作业控制/管道。

## 交付物

| 路径 | 内容 |
|---|---|
| `src/apps/shell/shell.c` | 64 位用户态 shell：行编辑、tokenizer/重定向、cwd 解析、15 个命令、错误分支、证据行（**新**） |
| `src/ng/wp26.h`、`src/ng/wp26_selftest.c` | 内核接线：ELF64 校验/加载、用户栈、ring3 运行、退出/会话证据（**新**） |
| `src/ng/isr.S` | ring3 返回恢复 callee-saved 寄存器（**改**，WP1-7 缺陷修正） |
| `src/ng/main.c` | 在 WP1-7 之后启动 WP2-6 shell；`-DNG_SMOKE_MODE=1` 跳过；PASS/FAIL 与 log（**改**） |
| `tools/build-ng.ps1` | 编译/校验/嵌入 `shell.elf`（`ng_shell_elf_start..end`）；`-Smoke`；wp26 源（**改**） |
| `tools/run-ng.ps1` | QMP 逐字符命令注入 + WP2-6 期望行/正则/失败模式 + `-Smoke`/`-NoQmp` 分支（**改**） |
| `tests/unit-ng/test_shell.c` | tokenizer/cwd/重定向/15 命令/错误/行编辑/`-EAGAIN` 单测（**新**，317 checks） |
| `tests/unit-ng/unit_main.c`、`tools/unit-ng.ps1` | 注册 shell 测试 + 源码（**改**） |
| `docs/design/shell-ng.md` | 设计/边界/证据行/run-ng 注入/限制（**新**） |
| `docs/design/evolution.md`、`docs/design/README.md` | E2 进度/索引（**改**） |

## 环境

- 分支 `ng`，基线 HEAD `439c33d`（WP2-5），仓库 `D:\OS_Project`。
- clang 23.1.4 / ld.lld 23.1.4（`D:\msys64\mingw64\bin`）。
  内核：`clang --target=x86_64-elf -fuse-ld=lld -ffreestanding -nostdlib
  -mno-red-zone -fno-stack-protector -mgeneral-regs-only`。
  用户 shell：同一 target，加 `-mno-sse -mgeneral-regs-only -fno-builtin
  -fno-pic -fno-pie -std=c11 -O2`，用 `src/ng/user/user.ld` 链到 0x40000000。
- QEMU 11.0.0：`-kernel dist-ng/aster-ng.elf -m 256 -cpu max -vga std
  -drive file=dist-ng/disk.img,format=raw,if=ide -display none`，QMP TCP
  `input-send-event` 注入按键。

## 关键参数

| 项 | 值 |
|---|---|
| shell ELF | 22760 字节，3 PT_LOAD，base 0x40000000，entry 0x400007a0，end 0x40004dd0 |
| 用户栈 | 16 KiB（4 页 RW+NX），`NG_USER_STACK_BASE=0x40100000`，下方守护页不映射 |
| 行缓冲 | 127 字符；最多 8 个参数；`>` 只允许一个、只 `echo` 支持 |
| 单次写 | syscall 0 ≤256 字节；fd read/write ≤512 字节（shell 按 256 分块） |
| `ls` | 单次最多 8 条（`FS_SYS_LISTDIR_MAX`），第 8 条时提示截断 |
| `cat` | 上限 64 KiB 输出（超出打印 truncated） |
| `dmesg` | 1024 字节缓冲；`-n` 取 1..64 行 |
| `log` | tag 1..15（可打印、无 `:`），msg 1..126 |
| 退出码 | 任一命令失败把状态置 1；`exit` 无参用该状态，`exit N` 用 N（0..255） |
| 证据行 | `ng: shell ready`；`ng: shell ls/cat/write/dmesg/pkg/exit ok`；内核补 `ng: WP2-6 selftest PASS` |

## 证据（2026-10-31）

### 构建

```text
powershell -File tools/build-ng.ps1
=== build-ng (WP2-6) ===
...
=== user-elf (WP1-7) ===
user-elf: PASS (15872 bytes, 3 PT_LOAD, entry 0x40000000, end 0x400023b0)
=== user-shell (WP2-6) ===
  shell.c -> obj-ng/user_shell.o
  link    -> obj-ng/shell.elf (base 0x40000000)
user-shell: PASS (22760 bytes, 3 PT_LOAD, entry 0x400007a0, end 0x40004dd0)
...
  [50/50] link -> dist-ng/aster-ng.elf
ELF64: PASS (1707016 bytes)
  class    2 (ELF64)
  machine  62 (EM_X86_64)
  type     2 (ET_EXEC)
  entry    0x0000000000101000
  phnum    7
segments: PASS
  5 PT_LOAD, 4 KiB aligned (per-section pages)
multiboot2: PASS
pvh-note: PASS
build-ng: PASS -> D:\OS_Project\dist-ng\aster-ng.elf
```

`-Smoke`：

```text
build-ng.ps1 -Smoke -> build-ng: PASS（mode: smoke build -DNG_SMOKE_MODE=1）
run-ng.ps1   -Smoke -> 93/93 lines + 36 patterns PASS（ng: shell skipped (smoke mode)）
```

### host 单测

```text
powershell -File tools/unit-ng.ps1
=== unit-ng (WP1-8 host tests) ===
clang: D:\msys64\mingw64\clang.exe
3108 checks, 0 failures
unit-ng: PASS (3108 checks, 0 failures)
```

WP2-5 为 2791，本 WP **+317**（`tests/unit-ng/test_shell.c`）：

- tokenizer：空白/多空格/Tab、空行、8 参数上限与第 9 个拒绝、`> path`、
  `>path`、缺路径、多重重定向、`>>`、`> /only`、`a>b` 不拆；
- 路径解析：绝对/相对/`.`/`..`/重复 `/`/根之上、超长、缓冲不足（-36）、空（-22）；
- errno 文本：ENOENT/EACCES/EISDIR/ENOTDIR/ENAMETOOLONG/EEXIST/EINVAL/未知；
- 每个命令的成功与错误路径（ls/cat/echo/重定向/mkdir/rm/cd/pwd/stat/dmesg/
  log/pkg/login/clear/exit），`$?` 传播（`ls /nope` 后 exit=1，成功命令清零），
  `exit 7` 的退出码，`exit 256`/`exit abc`/多参数拒绝；
- 行编辑：Backspace 修正后执行、Ctrl-L（`\f` + 重画）、Ctrl-C（`^C`、不执行、
  状态 1）、Ctrl-U（回退两次）、Ctrl-D 退出；
- `readkey` `-EAGAIN` 7 次后照常工作；`ls` 9 个文件的 8 条截断；
- fd 回收：cat/重定向后 `vfs_fd_live()==0`。

### QEMU（主路径）

```text
powershell -File tools/run-ng.ps1
=== run-ng (WP2-6) ===
...
ng: WP1-7 selftest PASS
ng: shell elf ok (size=22760 entry=0x400007a0 segs=3)
ng: shell load ok (base=0x40000000 end=0x40004dd0 pages=5)
ng: shell launch ok
Aster ng shell (WP2-6, ring3) - type 'help'
ng: shell ready
ng> help
Aster ng shell (WP2-6) - 64-bit ring3 shell
commands:
  help                 this help
  ls [path]            list a directory (first 8 entries)
  cat <file>           print a file
  echo <text> [> path] print text, or write it to a file
  ...
ng> ls /
tmp/
mnt/
selftest/
edge/
etc/
System/
ng: shell ls ok
ng> cat /etc/passwd
root:0:bab4ecd99b3bcb573de4ae62dc22bf60:9f870bf5b76dbf90d100a5f6d58f962cfff806c371916f236b2fab3e402bf095:4096
alice:1000:5024659b54f52d1bda15c6bfe6a7a499:a84d897ad8a60b21c40e9f18460ec676a07ff7b91c00ad62db2b1add8e66c359:4096
ng: shell cat ok
ng> echo hi > /tmp/s.txt
ng: shell write ok
ng> cat /tmp/s.txt
hi
ng: shell cat ok
ng> dmesg -n 3
selftest: WP2-3 PASS
ring: hello from ring3
selftest: WP1-7 PASS
ng: shell dmesg ok
ng> pkg list
hello 1.0.0
ng: shell pkg ok
ng> exit
ng: shell exit ok
ng: syscall exit test
ng: shell exit (code=0 syscalls=189606 writes=153 fs=12)
ng: shell session ok (exit=0 rsp=ok)
ng: shell session file ok (s.txt=3)
ng: shell session pkg ok (lists=+1)
ng: WP2-6 selftest PASS
------------------------------------------------
run-ng: matched 106/106 expected lines + 40 patterns
run-ng: PASS (all expected lines in 3.8s, cpu max)
```

`run-ng.ps1` 新增期望（14 条固定行 + 4 条正则）：

```text
14 条固定行：
  ng: shell elf ok / ng: shell load ok / ng: shell launch ok / ng: shell ready
  ng: shell ls ok / ng: shell cat ok / ng: shell write ok /
  ng: shell dmesg ok / ng: shell pkg ok / ng: shell exit ok
  ng: shell session ok (exit=0 rsp=ok) /
  ng: shell session file ok (s.txt=3) / ng: shell session pkg ok
  ng: WP2-6 selftest PASS
4 条正则：
  ng: shell elf ok \(size=... entry=0x... segs=...\)
  ng: shell load ok \(base=0x... end=0x... pages=...\)
  ng: shell exit \(code=0 syscalls=... writes=... fs=...\)
  ng: shell session pkg ok \(lists=\+[0-9]+\)
失败早停模式新增：
  ng: shell .* FAIL / ng: WP2-6 selftest FAIL
```

### 负向 / 降级 / 其他 CPU

```text
run-ng -Fallback    -> 105/105 lines + 34 patterns PASS（-vga none；shell 仍可用）
run-ng -NoQmp       -> 92/92 lines + 36 patterns PASS（不注入，不要求 shell 交互行）
run-ng -Cpu qemu64  -> 106/106 lines + 40 patterns PASS（SMEP/SMAP off）
build-ng -Smoke + run-ng -Smoke -> 93/93 lines + 36 patterns PASS
```

### 门禁

```text
tools/check-domains.ps1
checked: 235 files, 763 quoted includes
domain include violations: 0

tools/check-deps.ps1
checked: 201 files, 650 quoted includes
interface layer violations (include/*.h -> *_internal.h): 0
module include violations: 0
OK: no cross-module include violations.

tools/ci-ng.ps1
  PASS check-domains    0 violations
  PASS check-deps       0 violations
  PASS unit-ng          3108 checks
  PASS build-ng         1707016 bytes, user-elf 15872 bytes
  PASS run-ng           106/106 lines + 40 patterns
  PASS legacy-32bit     305/305 parity checks
  === NG CI PASS ===
```

旧 32 位独立/门禁内均 **305/305**。

## syscall ABI（摘要）

```c
 0 write(buf,len)          -> len / -EINVAL / -EFAULT     （单次 <= 256）
 1 exit(code)              -> 不返回（回内核恢复点）
20 open(path,flags,mode) 21 read(fd,buf,len) 22 write(fd,buf,len) 23 close(fd)
24 stat(path,&st) 25 listdir(path,ents,max<=8) 26 mkdir(path,mode) 27 unlink(path)
30 readkey()               -> (flags<<24)|(ascii<<16)|scancode / -EAGAIN
31 login(name,pass) 32 dmesg(buf,max<=4096) 33 log_write(tag,msg)
34 pkg_list(buf,max<=512) 35 pkg_verify(name)
```

`struct vfs_stat` = `{u32 type; u32 mode; u64 size; u64 ino;}`；
`struct vfs_dirent` = `{u32 type; u32 name_len; u64 size; char name[64];}`。
shell 侧结构逐字节一致（host 单测在 LLP64 上发现 `unsigned long` 只有 4 字节，
改用 `unsigned long long` 后布局才正确 —— 这是 `.bss` 越界写坏 shell 全局变量的
真实 bug，已在实现里修正）。

## 自检项清单

1. 构建：`user-shell: PASS`（22760 字节/3 PT_LOAD/entry/end）、内核 1707016 字节；
   `-Smoke` 模式 PASS。
2. host：tokenizer/解析/重定向/15 命令/错误/行编辑/`-EAGAIN`/ls 截断/fd 回收，
   3108 checks 0 failures（本 WP +317）。
3. QEMU 主路径：shell 加载/启动/`ng: shell ready`/8 条注入命令/退出码 0/
   会话文件与 pkg 证据/`ng: WP2-6 selftest PASS`，106 行 + 40 正则。
4. 降级/其他 CPU：Fallback 105+34、NoQmp 92+36、qemu64 106+40、Smoke 93+36。
5. 门禁：check-domains/check-deps 0、旧 32 位 305/305、ci-ng 6/6。
6. 无 panic：shell 退出路径曾触发 #PF（callee-saved 未恢复），修正后主/降级/
   其他 CPU 模式均无 PANIC 行。

## 未做 / 遗留（按 WP 边界）

- **shell 的屏幕输出/TTY**：ring3 `write` 只到串口；帧缓冲/VGA 文本控制台仍是
  WP2-3 启动自检。原因是用户 AS 只共享内核 0..1 GiB，LFB 在 0xFD000000
  （PDPT[3]），内核在用户 AS 下写 LFB 会 #PF；把 MMIO 映射进每个用户 AS 需要
  AS 生命周期管理，留给 E3（WP3-1 display server / WP3-2 shell/WM）。
  `clear`/Ctrl-L 目前输出 `0x0C`（换页），等 TTY 解释。
- **无 sleep/时钟 syscall**：readkey 空转用 `pause` 降频（脚本会话约 19 万
  syscall）；真正的阻塞读键等 IRQ 输入服务/调度器。
- **登录不建会话**：`login` 只验证口令并打印 uid，`vfs_ctx.uid` 仍是 0（root）；
  会话/权限/审计在 D1。
- **命令集**：无管道/`>>`/`<`/通配符/引号/环境变量/历史/补全；`ls` 只前 8 条；
  `cd` 不支持 `cd -`。
- **8 条 listdir 上限**来自 WP2-1 的 `FS_SYS_LISTDIR_MAX`；要翻页需要 listdir
  cookie/offset（ABI 变更，WP4-1 之后）。
- **单会话**：一个用户 AS、一个 shell；多应用/作业控制/会话管理是 E3。
- **shell 静态链接**：无动态库/共享对象（D0 允许）。

## 出口

```text
powershell -File tools/build-ng.ps1          # PASS，1707016 bytes，50 步
powershell -File tools/build-ng.ps1 -Smoke   # PASS（跳过 shell）
powershell -File tools/run-ng.ps1            # PASS，106 行 + 40 正则
powershell -File tools/run-ng.ps1 -Smoke     # PASS，93 行 + 36 正则
powershell -File tools/unit-ng.ps1           # PASS，3108 checks, 0 failures
powershell -File build.ps1 -Test             # PASS，305/305（旧 32 位）
powershell -File tools/check-domains.ps1     # 0 违规（235 files）
powershell -File tools/check-deps.ps1        # 0 违规
powershell -File tools/ci-ng.ps1             # 6/6 PASS
```
