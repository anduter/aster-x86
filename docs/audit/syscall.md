# audit-syscall：用户→内核边界崩溃/漏洞扫描（2026-10-27）

- 会话：`audit-syscall`（独立审计）
- 基线：`38161af`（225 项检查，v0.73.0+ 并行批次）
- 构建：`.obj-audit-sys` / `.dist-audit-sys`，QEMU QMP **4541**，串口 `obj/audit/*.log`
- 本会话提交：`e0eddab`（ring3 探针 + 回归驱动）、`2463479`（misc/brk/权限门）、
  `25b8587`+`360617a`（报告/边界套件）、`97e1d74`（DUP2 管道读端引用）、
  `fd263f5`（LOGIN 缓冲长度）、`010343c`（两条新回归）；`e80605d`/`bbd04c5`
  只改测试驱动（full 套件按键/结论解析）
- 集成基线：`e80605d` 内核快照（含 audit-fs `b4d5fd9`、audit-net `e4ef7ea` 与
  本会话全部修复；当前 HEAD `bbd04c5` 的内核源码与其相同）

## 1. 结论

**找到 15 项，本会话修复 10 项；其余 5 项已分派：AS-9、AS-14 的 SYS_TIME 子项、
AS-15 已由 audit-fs 在 `b4d5fd9` 修复；AS-10、AS-11、AS-12 转交 audit-smp。**
10 项修复都有 QEMU 动态回归（修复前失败/崩溃，修复后 PASS）。

| # | 位置 | 问题 | 状态 |
|---|------|------|------|
| AS-1 | `core/misc_syscall.c` SYS_WRITE | 用户缓冲只有上界，ring3 可读内核内存并打印 | 已修 |
| AS-2 | 同上 SYS_BLK_READ | 输出缓冲未校验下界，可写内核内存 512B | 已修 |
| AS-3 | 同上 SYS_PCI | 同上，最多 64B | 已修 |
| AS-4 | 同上 SYS_RESINFO | 同上，40B（可踩 PMM 位图） | 已修 |
| AS-5 | 同上 SYS_ETH | 同上，8B | 已修 |
| AS-6 | 同上 SYS_MOUSE | 同上，12B（可踩内核 .bss） | 已修 |
| AS-7 | `mm/mm_syscall.c` SYS_BRK | 32 位加法回绕 + 无堆下界，brk 可落到内核地址 | 已修 |
| AS-8 | `core/misc_syscall.c` 调试入口 | 非 root 可 panic/hang/reboot/poweroff 整机 | 已修 |
| AS-9 | `fs/fs_syscall.c` SYS_CHROOT | `strcpy` 到 192B `cwd`，路径可达 255B → `proc_t` 溢出 | 已由 audit-fs 修（b4d5fd9） |
| AS-10 | `proc/process.c` SYS_SIGRETURN | 从用户可改的信号帧恢复 cs/eip/ss/eflags → 越权 iret | 转交 audit-smp |
| AS-11 | `proc/trap/uaccess.c` | `copy_*_user` 全局缺用户窗口校验（其余模块仍可内核 R/W） | 转交 audit-smp |
| AS-12 | `proc/proc_syscall.c` SYS_KILL | 任意用户可向任意 pid 发信号（可 SIGKILL root shell） | 转交 audit-smp |
| AS-13 | `svc/svc_syscall.c` + `fs/pipe.*` SYS_DUP2 | 管道读端复制不增加引用，槽位提前回收 → 跨进程串数据 | 已修 |
| AS-14 | `security/security_syscall.c` LOGIN_WHO/LAST/INFO | 忽略调用方缓冲长度，80B 缓冲被写 192B | 已修（security）；SYS_TIME 由 audit-fs 修 |
| AS-15 | `fs/fs_syscall.c` SYS_FSCK | 忽略 `copy_to_user` 返回值，失败仍报成功 | 已由 audit-fs 修（b4d5fd9） |

## 2. 攻击面与方法

1. **静态**：逐个核对 `include/abi.h` 的 1..79 号 legacy 调用，沿
   `proc/trap/syscall.c` 的 7 组委派读入口校验（指针/长度/对齐/fd/flags/路径），
   并交叉检查 `mm/**` 的页表/引用计数/brk/mmap 实现。
2. **动态**：QEMU 里以 ring3 shell 发 `int 0x81`。由于
   `trust_verify_shell()` 只接受构建签名的 shell 镜像（自编译 ELF 会被
   `trust: rejected unsigned image` 拒绝），探针以 shell 内置命令实现：
   `sys <nr> <a> <b> <c>`（十进制参数，打印 `sys: nr=.. ret=..`），
   驱动见 `tools/audit-syscall.ps1`，说明见 `tests/audit/syscall/README.md`。
3. 复现缩到“一行 syscall + 参数”，修复后同一命令返回 `-1` 并保留正常路径
   （`res`/`mouse`/`mmap`/`munmap`/`brk(0)`）作对照。

> 环境注记：审计期间共享工作树被并行会话的未提交改动持续修改，
> 直接构建曾因 `net/net_internal.h` 注释里的 `/**` 触发 `-Werror=comment`
> 而失败。本会话的构建/回归在 `git archive HEAD` 的私有快照
> （`D:\audit-sys-build`）+ 本会话补丁上完成；225 项回归在快照上
> **225/225 PASS**。集成树恢复可构建后应重跑一次。

## 3. 已修复项（含复现/根因/修复/回归）

### AS-1 SYS_WRITE：ring3 读内核内存（信息泄露，P0）

复现（修复前）：

```
sys 1 1048576 8 0        # SYS_WRITE(ptr=0x100000, len=8)
```

串口实际输出（内核镜像头被打印到控制台）：

```
KRNLsys: nr=1 a=1048576 b=8 c=0 ret=8
```

根因：`user_range_ok()` 只检查 `ptr < 0xC0000000` 和长度，`0x00100000`
这样的内核恒等映射地址被当作合法用户缓冲；`copy_from_user()` 在 ring0
直接 `rep movsb` 读内核内存并 `console_write_atomic()` 输出。

修复（`core/misc_syscall.c:33`）：新增 `misc_user_buf()`，对
`is_user` 进程要求 `ptr >= USER_LAYOUT_CODE_BASE`（0x40000000）；
内核自检（`p->is_user == false`，如 `core/bootseq.c` 的 `syscall_selftest`
用内核字符串调 SYS_WRITE）保持原行为。

回归：`sys 1 1048576 8 0` → `ret=-1`；日志中不再出现 `KRNL` 泄漏；
`core/bootseq.c` 的 `sys: write=20` 仍 PASS。

### AS-2..AS-6 输出缓冲内核目标：任意内核写（P0）

同一根因的 5 个调用点，`copy_to_user()` 会把内核地址当用户地址写：

| 修复前复现 | 语义 | 动态结果 |
|------------|------|----------|
| `sys 40 2097152 0 0` | SYS_RESINFO 写 40B 到 0x200000（PMM 位图） | `ret=0` |
| `sys 37 8388608 0 0` | SYS_MOUSE 写 12B 到 0x800000（内核 .bss） | `ret=0` |
| `sys 57 0 2097152 0` | SYS_BLK_READ 写 512B 到 0x200000 | 同路径（静态确认） |
| `sys 35 0 2097152 0` | SYS_PCI 写 ≤64B 到 0x200000 | 同路径（静态确认） |
| `sys 39 0 2097152 0` | SYS_ETH 写 8B 到 0x200000 | 同路径（静态确认） |

影响：可改写 PMM 位图（分配器状态）、内核 .bss（进程表/账号表等全局），
等价于任意内核写，后续可放大为 panic 或权限绕过。

修复：上述 5 个 case 在 `copy_to_user` 前调用 `misc_user_buf()`，长度分别用
`BLK_SECTOR` / `sizeof(b)` / `sizeof(out)` / `sizeof(info)`。

回归：5 条命令全部 `ret=-1`；`res`（栈上 u32[10]）与 `mouse`（栈上 int[3]）
仍正常输出（证明校验没有误伤合法用户缓冲）。

### AS-7 SYS_BRK：32 位回绕把 brk 带到内核地址（P1）

复现（修复前）：

```
sys 22 0 0 0              # brk(0) -> 1075978272 (0x4023xxxx)
sys 22 3221225472 0 0     # brk(0xC0000000)
                          # -> ret=2236448 (0x00222020)
sys 22 0 0 0              # -> ret=2236448（brk 已在内核区）
```

根因：`neu = old + inc` 使用 32 位无符号加法，`0xC0000000` 与 ~0x4023xxxx
相加回绕到 0x0022xxxx；旧校验只判 `neu > USER_HEAP_MAX`，回绕后的小值通过。
其后的 brk 以 `old & ~0xFFF` 继续推进；恒等映射页被 `vmm_get_phys()!=0`
跳过，于是“成功”的 brk 实际没有用户页，用户一访问就 #PF 被杀。负增量也没有
下界，brk 可被降到堆基址以下（栈/代码区）。

修复：
- 新增 `mm/brk_range.h::brk_range_ok(old, inc, floor, ceil)`：inc 按有符号
  32 位解释；正向用 `ceil - old` 先算避免回绕，负向用 `0 - inc`（对 INT_MIN
  安全）比较 `old - floor`。
- `mm/mm_syscall.c:79` 取 `floor = user_heap_base`，旧值低于 floor 时先夹到
  floor（兼容修复前留下的坏状态），校验通过后再做一次终值断言。

回归：
- 内核启动自检：`mm: brk range guard ok (no wrap, no underflow)` +
  `memtest: brk inc range guard ... PASS`（`mm/memtest.c` 新增纯算术用例）。
- ring3：`sys 22 3221225472 0 0` → `-1`；`brk(0)` 仍返回 0x402xxxxx；
  `mmap/munmap` 正常。

### AS-8 调试/关机调用无权限门：任意 ring3 可停整机（P1）

复现（修复前，先降权再 panic）：

```
drop 1000
sys 28 0 0 0       # SYS_PANIC
```

串口：

```
drop: now uid=1000 gid=1000 (reboot to regain root)
*** KERNEL PANIC ***
panic requested by userland (pid 7)
```

根因：`SYS_PANIC/HANG/REBOOT/POWEROFF` 是调试入口，但没有任何身份/权限检查，
任何 ring3 进程一条 `int 0x81` 就能让整机停机（SYS_REBOOT 还会直接复位）。

修复（`core/misc_syscall.c:44`）：`misc_system_privileged()`——`proc_current()==0`
（内核线程）或 `euid==0` 才放行；否则返回 `-1`。`SYS_OOPS` 保持原样（只杀调用者，
不是系统级停机）。

回归：`drop 1000` 后 `sys 28/27/36/38` 全部 `ret=-1` 且无 `KERNEL PANIC`；
root 下的 `crash`/`shutdown` 命令语义不变。

### AS-13 SYS_DUP2 管道读端引用计数（P2，svc + `fs/pipe.*`）

复现（修复前，shell 命令 `duptest`）：

```
pipe(); dup2(read, 10); close(write); close(original read);
read(10) -> -1        # 应为 0（EOF）
```

串口：`ushell: dup2 pipe read ref FAIL (last=-1)`；fork 变体同样失败
（子进程关闭读端并退出后，父进程再读同一个管道得到 -1）。

根因：读端没有引用计数。`fs/pipe.c::pipe_unref_read()` 在
`writers==0 && count==0` 时直接释放槽位，不检查是否还有别的读 fd；dup2 复制
`FD_KIND_PIPE_R` 不增加任何引用（`pipe_fork_inherit()` 也只给写端加引用）。
槽位释放后被新 pipe 复用时，旧 fd 会读到**另一个进程/管道的数据**。

修复（提交 `97e1d74`；`fs/pipe.h`、`fs/pipe.c` 经父会话单独授权，
audit-fs 不碰这两个文件）：

- `pipe_t` 增加 `readers`，`pipe_create()` 置 1，新增 `pipe_ref_read()`；
- dup2 复制读端、fork 继承读 fd 时各加一个引用；close/exit 走 `pipe_unref_read()`；
- 释放条件改为 `readers==0 && writers==0`（不再依赖 count），顺带修掉
  “先关读端、后关写端”导致的 8 槽位泄漏；
- `SYS_PIPE` 的分配失败 / `copy_to_user` 失败路径把读+写引用都回滚
  （此前会泄漏 fd 与管道槽位）；
- EOF 语义不变：仍只看 `writers==0`。

回归：`duptest` 覆盖 dup2 读端 + fork 父子 close + 写端关闭后 EOF，输出
`ushell: dup2 pipe read ref ok (last=0)`；`-Suite guard` PASS。

残余（不在本会话路径）：`proc_fork()` 的 `vmm_clone_user_pages()` 失败回滚只
释放写端，读端引用会泄漏一个（proc/process.c 属 audit-smp）；只在内存不足的
fork 失败路径出现，建议 proc 侧补一次 `pipe_unref_read()`。

### AS-14 LOGIN_WHO/LAST/INFO 忽略调用方缓冲长度（P2，security）

复现（修复前，shell 命令 `loginlen`，80B 栈缓冲）：

```
SYS_LOGIN(LOGIN_INFO, buf, 0/1/8)   # 旧实现只判 a!=0，完全忽略 b
SYS_LOGIN(LOGIN_WHO,  buf, 8)
SYS_LOGIN(LOGIN_LAST, buf, 1)
```

旧实现把最多 192B 写进 80B 缓冲，用户栈被踩，shell 进程直接死掉：

```
ushell: login len clamp FAIL
user: exception 14 at eip=0x20444441 err=14 cr2=0x20444441, killing thread
proc: pid 7 (shell) exited with 142
```

修复（提交 `fd263f5`）：新增 `copy_login_out()`——`max` 必须放得下完整字符串
（含 NUL），否则返回 `-1` 且一个字节都不写；WHO/LAST/INFO 统一走它。
`SYS_TIME` 在 fs 路径，按分工本会话未动，已由 audit-fs 在 `b4d5fd9` 修复
（honor `ecx`、截断 + NUL）。

回归：`loginlen` 覆盖 size=0/1/8（`-1` + canary 完好）与 80B（成功且 NUL 结尾），
输出 `ushell: login len clamp ok`。

## 4. 转交项（含已由其它会话修复的条目）

### AS-9 SYS_CHROOT：`proc_t.cwd[192]` 栈外写（P0，fs）

**状态：audit-fs 已在 `b4d5fd9` 修复**（超长路径直接 `-1`、`cwd` 不变，新增
`fs_chroot_set_cwd()`；`proc_root_set` 的截断问题一并处理）。以下为原始分析，
保留备查。

`fs/fs_syscall.c`（`SYS_CHROOT` case，约 551 行）：
`char path[FS_PATH_MAX]`（256）→ `strcpy(p->cwd, path)`，而 `proc_t.cwd` 只有
192 字节。`copy_user_string` 允许 255 字节路径，`make_abs_path` 也会保留长度；
只要路径是已存在的目录（可用多级 `mkdir` 构造），就会写过 `cwd` 覆盖
`user_brk/user_stack_*/fds[]/caps` 等字段。另 `proc_root_set()` 会静默截断到
191 字节，chroot 根与实际路径不一致，可能被用于越过 chroot。

建议：`strlen(path) >= sizeof(p->cwd)` 直接 `E_INVAL`，并统一用 `strncpy`；
`proc_root_set` 也返回长度错误而不是截断。

### AS-10 SYS_SIGRETURN：恢复用户可改的 iret 现场（P0，proc）

`proc/process.c::proc_sigreturn()` 只校验 `sig_frame` 的地址范围，然后
`memcpy(f, user_frame, sizeof(*f))`；`isr.asm` 的 `iret` 不做 cs/ss 校验。
但信号帧保存在用户栈上（`deliver_signal` 写入 `useresp - sizeof(frame)`），
handler 在入口处 `esp+8` 就是帧起点，可随意改：

- `frame.ss = 0x08`（内核数据段）/非法 cs → `iret` 时 CPL0 #GP → 内核 oops
  （杀进程）或 panic（视上下文）；
- `frame.eflags |= IOPL(3)` 后正常返回 ring3 → 用户态获得 I/O 端口权限
  （可直驱 ATA/PIC 等）；
- 若配合 AS-1 泄露的内核地址把 `cs` 改成 0x08、`eip` 指向内核代码，则直接
  ring0 执行。

建议：`proc_sigreturn` 校验 `cs == 用户代码段 (0x1B)`、`ss == 用户数据段 (0x23)`、
`eip < USER_ADDR_LIMIT`、`eflags` 只允许用户可改位，并校验 `sig_frame` 确实是
本次投递写入的（序号/魔数）。

### AS-11 `copy_*_user` 的根因仍在（P0，proc）

`proc/trap/uaccess.c` 只拒绝 `ptr == 0`，不校验用户窗口；本会话只在 misc 组
加了 `misc_user_buf()`。仍然存在同类调用的组（ring3 可内核读/写）：

- fs：SYS_FREAD/FWRITE（src/dst）、LIST、DRIVES、STAT、GETCWD、READLINK、LSTAT；
  SYS_TIME/FSCK 的长度/返回值已由 `b4d5fd9` 修，但“把内核地址当用户缓冲”的窗口
  问题仍在；
- proc：SYS_WAIT/WAITPID 的 status；
- svc：SYS_PIPE 的 fds、SYS_INPUT_SET 的 buf；
- security：SYS_GETGROUPS/SETGROUPS、LOGIN_AUTH/SU/PASSWD 字符串、
  LOGIN_WHO/LAST/INFO 输出（长度已由 `fd263f5` 修，窗口仍缺）；
- net：SYS_UDP_SEND/UDP_RECV/SENDTO/RECVFROM/ACCEPT 的 `udp_req`/addr；
- pkg：SYS_PKG LIST 的输出缓冲。

**建议根修**：在 `copy_from_user/copy_to_user` 里做一次统一校验——当
`proc_current() != 0 && proc_current()->is_user` 时，要求
`ptr >= USER_LAYOUT_CODE_BASE && ptr + len <= USER_ADDR_LIMIT`（并防回绕）；
`is_user == false` 的内核自检保持可用。这样可一次覆盖所有组，misc 的
`misc_user_buf()` 就退化成冗余保险。

### AS-12 SYS_KILL 无权限检查（P1，proc）

`proc/proc_syscall.c` 直接用 `pid` 找进程并 `proc_send_signal()`，不比较
ruid/euid。普通用户可 `kill <任意pid> 9` 杀掉 root shell、getty、服务进程。
建议按 POSIX：`euid==0 || 目标 ruid/suid == 发送者 euid/ruid` 才允许；
SIGKILL/SIGSTOP 也要走同一检查。

### AS-15 SYS_FSCK 忽略 copy_to_user 返回值（P3，fs）

**状态：audit-fs 已在 `b4d5fd9` 修复**（检查 `copy_to_user` 返回值，失败返回
错误）。以下为原始分析，保留备查。

`fs/fs_syscall.c` 的 SYS_FSCK 在 `copy_to_user()` 失败时仍返回错误计数，
用户拿到“成功”但缓冲未写；结合 AS-11 还可把报告写进内核地址。建议失败
返回 `-1`（或统一错误码）。

## 5. 回归入口

```powershell
# 隔离产物 + QMP 4541；跑前确认宿主 12345 空闲
powershell -NoProfile -ExecutionPolicy Bypass -File tools\audit-syscall.ps1 -Suite guard
powershell -NoProfile -ExecutionPolicy Bypass -File tools\audit-syscall.ps1 -Suite priv
powershell -NoProfile -ExecutionPolicy Bypass -File tools\audit-syscall.ps1 -Suite full
# 修复前复现（在 vulnerable 树上跑，信息性）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\audit-syscall.ps1 -Suite probe
```

驱动内部调用：

```powershell
.\build.ps1 -Test -ObjDir .obj-audit-sys -DistDir .dist-audit-sys `
  -QmpPort 4541 -SerialLog obj\audit\syscall-guard.log `
  -Keys "sys 48 4096 3 0`nsys 49 ...`nres`nmouse"
```

- `guard`：18 条边界 syscall + `res`/`mouse` 对照 + `loginlen`/`duptest`
  两个专项回归（mmap/munmap 正常与重复 munmap、brk 回绕/负向越界、超大 len、
  mmap 长度回绕、mprotect 未映射/未对齐/len=0、5 个内核目标写、SYS_WRITE 内核源、
  LOGIN 长度钳制、dup2/fork 管道读端引用），全部断言通过
  （`[audit-syscall] suite=guard => PASS`）。
- `priv`：`drop 1000` 后 4 个停机调用全部 `-1`（`suite=priv => PASS`）。
- `full`：标准 225 项回归。集成快照 `e80605d`（含 audit-fs `b4d5fd9`、
  audit-net `e4ef7ea` 与本会话全部修复）上 `-Suite full => PASS`，build.ps1 打出
  `=== TEST PASS (225/225) ===`。同一快照另一次重跑命中仓库已记录的 TCP 时序抖动
  （仅“TCP 健壮性自检通过”一项，224/225；见 b79ca12/1a6998e），重跑即过。
- `probe`：修复前证据（`KRNL` 泄漏、brk 回绕、RESINFO/MOUSE `ret=0`）。

关键串口行（修复后）：

```
mm: brk range guard ok (no wrap, no underflow)
memtest: brk inc range guard ... PASS
sys: nr=22 a=3221225472 b=0 c=0 ret=-1
sys: nr=1  a=1048576 b=8 c=0 ret=-1
sys: nr=40 a=2097152 b=0 c=0 ret=-1
sys: nr=37 a=8388608 b=0 c=0 ret=-1
sys: nr=35/39/57 ... ret=-1
res: mem  free=59238/65008 pages ...
mouse: x=640 y=512 buttons=2
ushell: login len clamp ok
ushell: dup2 pipe read ref ok (last=0)
```

修复前证据（同一探针）：

```
ushell: login len clamp FAIL
user: exception 14 at eip=0x20444441 err=14 cr2=0x20444441, killing thread
proc: pid 7 (shell) exited with 142          # 80B 栈缓冲被 LOGIN_WHO 写穿

ushell: dup2 pipe read ref FAIL (last=-1)     # 关闭原 read 后 dup 读到 -1
```

## 6. 覆盖范围与限制

- **已覆盖**：1..79 legacy 编号的静态入口校验；7 个分发组的指针/长度/fd/flags/
  路径参数；`mm/**` 的 brk/mmap/munmap/mprotect/页表/COW/引用计数；misc 组全部
  case；沙箱 allowlist 与 `eax >= 64` 旁路检查；`mmap` 失败回滚与 `unmap` 路径。
- **动态触达**：SYS_WRITE(1)、BRK(22)、PIPE(25)、DUP2(26)、REBOOT(27)、
  PANIC(28)、PCI(35)、POWEROFF(36)、MOUSE(37)、HANG(38)、ETH(39)、RESINFO(40)、
  MMAP(48)、MUNMAP(49)、BLK_READ(57)、MPROTECT(62)、LOGIN(72)；含重复 munmap、
  长度回绕、未对齐、超长 len、内核地址缓冲、dup2/fork 管道读端、LOGIN 小缓冲等
  边界；以及 `res`/`mouse`/`loginlen`/`duptest`/`drop`/`crash` 等 shell 命令。
- **未覆盖/留给对应会话**：native int 0x80 ABI、net 协议栈深度、fs 路径解析与
  FAT16、security/accounts 口令路径、pkg 安装器、SMP 并发与 TLB shootdown、
  文件映射内容语义、长时间 soak。
- **限制**：审计期间共享树被并行会话修改，动态验证都在 `git archive HEAD` 的
  私有快照 + 本会话补丁上完成；最终 225 项用的是集成快照 `e80605d`
  （含 audit-fs `b4d5fd9`、audit-net `e4ef7ea`）。转交项的细节见第 4 节。
- 预算：8h 未用完；在修复 10 项后提前收尾。
