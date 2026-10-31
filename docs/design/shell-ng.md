# 用户态 shell（ng WP2-6）

> E2 / WP2-6 的设计与实现边界。配套：`vfs-ng.md`（20..27）、`auth-log-pkg-ng.md`
> （31..35）、`display-input-ng.md`（readkey 30）、`elf-ng.md`（ELF64 加载器）。

## 1. 目标与边界

做一个**独立的 64 位用户态 shell**：L3 应用（`src/apps/shell/shell.c`），单独编译
成 ELF64、链接到 0x40000000，由内核 ELF64 加载器装进用户地址空间后在 ring3 运行。
它只通过 syscall 访问系统，不链接任何内核代码、不用 libc。

做：提示符/行编辑、15 个内建命令、`>` 重定向、友好错误 + 非零退出码、可自动
注入的命令序列、host 单测。

不做（按 WP 边界）：GUI/桌面（E3）、POSIX、作业控制、管道、通配符、引号/转义、
历史记录、补全、脚本语言、多会话/TTY 抽象。

## 2. 分层与文件

| 路径 | 层 | 内容 |
|---|---|---|
| `src/apps/shell/shell.c` | L3 | shell 全部逻辑（无 include） |
| `src/ng/wp26_selftest.c` / `wp26.h` | L0 | 加载/运行 shell，收集会话证据 |
| `src/ng/user/user.ld` | — | 用户程序链接脚本（基址 0x40000000，段 4 KiB 对齐） |
| `src/ng/isr.S` | L0 | ring3 往返（本 WP 修正 callee-saved 恢复，见 §7） |
| `tools/build-ng.ps1` | — | 编译/校验/嵌入 `shell.elf`（`ng_shell_elf_start..end`） |
| `tools/run-ng.ps1` | — | QMP 注入命令序列 + 期望行 |
| `tests/unit-ng/test_shell.c` | — | host 单测（317 checks） |

启动顺序：所有内核自检 → WP1-7 ring3 hello（`hello.S`，保留为 smoke）→
WP2-6 shell（交互直到 `exit` / Ctrl-D）。`tools/build-ng.ps1 -Smoke` 编出的内核
跳过 shell（打印 `ng: shell skipped (smoke mode)`），用于只验证 hello 路径。

## 3. 用户程序构建

```
clang --target=x86_64-elf -ffreestanding -nostdlib -mno-red-zone \
      -fno-stack-protector -fcf-protection=none -mno-sse -mgeneral-regs-only \
      -fno-builtin -fno-pic -fno-pie -std=c11 -O2 -Wall -Wextra -Werror -c
clang --target=x86_64-elf -fuse-ld=lld -nostdlib -Wl,-T,src/ng/user/user.ld \
      -Wl,--build-id=none -Wl,--fatal-warnings -o shell.elf shell.o
```

- 入口 `_start` 是 naked asm：`andq $-16,%rsp` + `call shell_main` + 死循环，
  这样 SysV ABI 的栈对齐在 `shell_main` 入口成立（内核给的 user RSP 是 16 对齐）；
- 无 libc：字符串/十进制/八进制格式化都在文件里（各 ~20 行）；
- 用户 ELF 校验（build-ng）：ELF64/ET_EXEC/EM_X86_64、`e_entry ∈
  [0x40000000,0x40010000)`、≥3 个 PT_LOAD、段 4 KiB 对齐、`p_align ≥ 4096`、
  `filesz ≤ memsz`、无 W+X、段尾 ≤ 0x40010000；
- 当前镜像：**22760 字节，3 PT_LOAD，entry 0x400007a0，end 0x40004dd0**；
- 嵌入方式与 hello 相同：`obj-ng/shell_elf_blob.o` 的
  `ng_shell_elf_start..end` 在 `.rodata`。

## 4. syscall 使用

| 调用 | 用途 | 备注 |
|---|---|---|
| 0 `write` | 控制台输出 | 单次 ≤256 字节，`sh_putn` 自动分块；`\n` 由 shell 输出成 `\r\n` |
| 1 `exit` | `exit [code]` / Ctrl-D | 无参数时用上一条命令的状态（0/1） |
| 20..27 | open/read/write/close/stat/listdir/mkdir/unlink | 全部先用 shell 自己的 cwd 解析成绝对路径 |
| 30 `readkey` | 行编辑 | 非阻塞，`-EAGAIN` 时 `pause` 忙等（ring3 没有 sleep） |
| 31 `login` | `login <user> <pass>` | 失败计数/锁定由 auth 服务负责 |
| 32 `dmesg` | `dmesg [-n N]` | 1024 字节缓冲，打印最后 N 行 |
| 33 `log_write` | `log <tag> <msg>` | tag 1..15、msg 1..126，可打印 ASCII |
| 34/35 | `pkg list` / `pkg verify <name>` | 512 字节列表缓冲 |

用户指针（路径/缓冲/`vfs_stat`/`vfs_dirent`）全部由内核 `uaccess` 校验；shell
只保证缓冲区在 `.bss`（U=1、可写）且长度不超过内核上限。

内核侧接口限制（沿用 WP2-1/2-5）：

- `write` ≤ 256 字节/次；`read`/`write(fd)` ≤ 512 字节/次；
- `listdir` 单次最多 8 条（`FS_SYS_LISTDIR_MAX`）——所以 `ls` 只显示前 8 条，
  第 8 条时会打印 `ls: (showing the first 8 entries)`；
- `dmesg` ≤ 4096；`pkg_list` ≤ 512。

## 5. 交互与命令

### 行编辑

- 提示符 `ng> `；可打印 ASCII 0x20..0x7E 入行（上限 127 字符）并回显；
- Backspace (8/127)：删一个字符，回显 `\b \b`；
- Enter (13/10)：执行；空行不改 `$?`；
- Ctrl-L：输出 `0x0C`（换页 = 清屏）后重画提示符和当前行；
- Ctrl-C：放弃当前行（`^C`），状态置 1；
- Ctrl-U：删整行；
- Ctrl-D：退出（退出码 0，打印 `ng: shell exit ok`）。

`readkey` 返回 `(flags<<24)|(ascii<<16)|scancode`；Ctrl 组合由 `flags` 判定
（PS/2 驱动不把 Ctrl 折成 0x01..0x1A，所以 shell 看 `ascii=='d'` 且带
`INPUT_F_CTRL`）。

### tokenizer / 重定向

- 空白（空格/Tab）分隔，最多 8 个参数；
- `>` 单独成 token 或紧贴路径（`>out`）都识别；只允许一个 `>`，不支持 `>>`、
  `<`、管道；`>` 只有 `echo` 支持（其它命令报错）；
- 不做引号/转义：`echo a>b` 是一个参数 `a>b`（不是重定向）。

### 命令表

| 命令 | 行为 | 证据行 |
|---|---|---|
| `help` | 命令/按键帮助 | — |
| `ls [path]` | 列目录（最多 8 条；目录带 `/`，文件带大小） | `ng: shell ls ok` |
| `cat <file>` | 按 256 字节分块读并输出（`\n`→`\r\n`，上限 64 KiB） | `ng: shell cat ok` |
| `echo <text> [> path]` | 无重定向：打印；有：写 `text\n`（O_CREAT\|O_WRONLY\|O_TRUNC, 0644） | 写文件成功时 `ng: shell write ok` |
| `mkdir <dir>` | 建目录 0755 | — |
| `rm <path>` | unlink 文件（目录 → `Is a directory`） | — |
| `cd [dir]` | 默认 `/`；stat 确认是目录后改 shell cwd | — |
| `pwd` | 打印 shell cwd | — |
| `stat <path>` | type/mode/size/ino | — |
| `dmesg [-n N]` | 内核 log 的最近 N 行（N 1..64） | `ng: shell dmesg ok` |
| `log <tag> <msg...>` | 写内核 log 环 | — |
| `pkg list` / `pkg verify <name>` | 包列表 / 清单+SHA-256 校验 | `ng: shell pkg ok` |
| `login <user> <pass>` | 认证并打印 uid | — |
| `clear` | 输出 `0x0C` | — |
| `exit [code]` | 退出；无参数用上一条命令状态 | `ng: shell exit ok` |

### cwd 与错误

VFS 的 `cwd` 属于 per-AS `vfs_ctx`，但 20..27 没有 chdir syscall。因此 shell 自己
维护 cwd 字符串（初始 `/`），每次文件操作先用 `sh_resolve()` 把 `cwd + 输入`
规范化成绝对路径（折叠 `//`、`.`、`..`，根之上仍是根），再交给内核。`pwd` 打印
的是 shell 的 cwd；内核 `vfs_ctx` 的 cwd 保持 `/` 且不参与。

错误统一走 `sh_error("cmd","arg",errno)`：`cmd: arg: <errno 文本>`（18 个 errno
映射，未知 → `Unknown error`），并把状态置 1；参数错误走 `sh: <usage>`。任何
错误都不 panic、不重启；fd 在错误路径上全部 close（host 单测断言
`vfs_fd_live()==0`）。

## 6. 内核接线与会话证据

`wp26_selftest()`：

1. `elf64_check()` 校验嵌入镜像，打印 `ng: shell elf ok (size=... entry=... segs=...)`；
2. `as_create()` + `elf64_load()`（PMM 页；`.text` RX、`.rodata` R+NX、
   `.data/.bss` RW+NX），打印 `ng: shell load ok (...)`；
3. 映射 16 KiB 用户栈（4 页 RW+NX，守护页不映射）；
4. `syscall_kernel_stack_sync()` → `as_switch()` → `syscall_run_user()`，
   打印 `ng: shell launch ok`；
5. shell 用 syscall 1 退出后 `as_switch_kernel()`，打印
   `ng: shell exit (code=0 syscalls=... writes=... fs=...)`；
6. PASS 条件：rc==0、exited、exit_code==0、writes>0、syscalls>0、
   `user_rsp_ok`、`bad_writes==0`；
7. 会话证据（只打印、不参与 PASS，因为人手交互不一定执行这两条命令）：
   - `ng: shell session file ok (s.txt=3)`：内核直接读 VFS 的 `/tmp/s.txt`，
     内容必须是 `hi\n`（`echo hi > /tmp/s.txt` 的端到端证据）；
   - `ng: shell session pkg ok (lists=+1)`：`pkg_stat()->lists` 的增量
     （`pkg list` 的证据）；
8. main.c 在用户程序退出后打印 `ng: WP2-6 selftest PASS/FAIL` 并写 log。

qemu 端由 run-ng 注入（见 §8），串口同时是 shell 的控制台（syscall 0 写串口）。
帧缓冲控制台仍只服务 WP2-3 启动自检：**ring3 的地址空间只共享内核 0..1 GiB
映射，LFB 在 0xFD000000（PDPT[3]），内核在用户 AS 下写 LFB 会 #PF**；把 LFB
映射进每个用户 AS 需要 MMIO 页的 AS 生命周期管理，留给 E3 的 display/tty
（`docs/design/display-input-ng.md` 的 E3 计划）。所以 `clear`/Ctrl-L 目前是向
控制台流输出 `0x0C`（换页）：串口终端可自行解释；真正的"清屏"要等 TTY。

## 7. isr.S 修正：ring3 返回必须恢复 callee-saved

WP1-7 的 `ring3_enter` 只保存 `kernel_resume_rsp/rip`，没有保存 rbx/rbp/r12..r15。
syscall 返回路径会把**用户**寄存器弹回，于是 `ring3_resume` 直接 `ret` 后，
`syscall_run_user()` 的 C 代码看到的是 shell 退出时的用户寄存器：shell 的 rbx
恰好指向用户栈（0x40103f78），`*out` 写入触发 `#PF` → PANIC。hello 之所以没
暴露，是它退出时 rbx 为 0（`out==NULL` 分支跳过）。

修正：`ring3_enter` 在保存 `kernel_resume_rsp` 之前把 rbx/rbp/r12..r15 压内核栈，
`ring3_resume` 按反序弹回再 `ret`。ABI 上这是 `ring3_enter` 本来就必须做的事；
对 hello 路径无行为变化。

## 8. 自动化（run-ng）

看到用户态 `ng: shell ready` 后，经 QMP `input-send-event` 逐字符注入：

```
help -> ls / -> cat /etc/passwd -> echo hi > /tmp/s.txt ->
cat /tmp/s.txt -> dmesg -n 3 -> pkg list -> exit
```

- 每个字符 25ms；`>` 用 `shift` down + `dot` down/up + `shift` up；QMP qcode
  里空格是 `spc`、`/` 是 `slash`、`.` 是 `dot`、`-` 是 `minus`；
- 每一步等到对应 `ng: shell <cmd> ok`（`cat` 第二次等到第 2 次出现）再发下一条，
  避免按键在 128 项 PS/2 环里堆积；`help` 没有证据行，固定等 250ms；
- 期望行见 `tools/run-ng.ps1` 头部注释；`-NoQmp` 不注入（不要求 shell 交互行），
  `-Smoke` 期望 `ng: shell skipped (smoke mode)`。

## 9. host 单测

`tests/unit-ng/test_shell.c` 把 `shell.c` 用 `-DNG_HOST_TEST` 编译，`sh_sys()`
换成 `sh_host_syscall()`：

- syscall 20..27 → 真实 VFS/ramfs（fake 页池，8 页）；
- 31..35 → 真实 auth/log/pkg（`auth_init` 种 root/alice，`pkg_init` 种 hello）；
- 0 → 捕获到内存输出缓冲；1 → 记退出码；30 → 脚本化按键（可预置 `-EAGAIN`，
  脚本结束给一次 Ctrl-D 兜底）。

覆盖：tokenizer（空白/8 参数上限/`>` 三种位置/`>>`/多重重定向/空行）、
cwd+路径解析（绝对/相对/`.`/`..`/超长/缓冲不足）、18 个 errno 文本、15 个命令
的成功/错误路径、行编辑（Backspace/Ctrl-L/Ctrl-C/Ctrl-U/Ctrl-D）、退出码传播、
`-EAGAIN`、`ls` 8 条截断、fd 泄漏。**317 checks**（unit-ng 总 3108）。

## 10. 已知限制 / 后续

- **shell 显示**：输出走 syscall 0 到串口；帧缓冲/VGA 文本控制台只有 WP2-3
  启动自检。TTY/display server 在 E3（WP3-1/3-2）。
- **无 sleep/时钟 syscall**：readkey 空转用 `pause` 降频（run-ng 会话约
  19 万 syscall），真正的阻塞读键等调度器/IRQ 输入服务。
- **单会话/单用户**：`login` 只验证口令，不建立会话/权限（`vfs_ctx.uid` 仍
  是 0=root）；多用户/审计在 D1。
- **命令集**：不做管道/重定向追加/通配符/引号/环境变量/历史；`ls` 只 8 条。
- **路径**：shell 不做符号链接解析（交给内核），但 `pwd` 显示的是词法 cwd。
- **`ls` 的 8 条上限**来自内核 `FS_SYS_LISTDIR_MAX`；要翻页需要 listdir
  cookie/offset，属于 ABI 变更（WP4-1 之后再动）。
