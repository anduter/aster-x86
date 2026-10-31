# Aster native ABI 1.0（冻结）

> 状态：**冻结（FROZEN）**，2026-10-31，E4 / WP4-1。
> 版本：ABI **1.0.0**（major 1，minor 0，patch 0；`ASTER_ABI_VERSION=0x00010000`）。
> 适用：ng 分支的 64 位内核 + 静态 ELF64 用户程序。
>
> **规范源（按优先级）**：
> 1. `docs/abi/abi-1.0.h` —— 机器可读的冻结编号/错误码/常量；
> 2. 本文 —— 语义、生命周期、错误条件、版本策略；
> 3. `src/ng/syscall.h` 与 `sdk/include/aster/syscall.h` —— 实现/消费镜像。
>    二者必须与第 1 项逐号一致，由 `tools/abi-check.ps1` 强制。
>
> 冻结后**只追加，不破坏**：已有调用号、参数、返回值、错误码数值、结构体
> 字段偏移/大小不变；新能力只能新增调用号或结构体尾部字段（§9）。
>
> 证据：`../claims/ng-WP1-8.md` … `ng-WP3-5.md`、`../claims/ng-WP4-1.md`；
> 门禁：`tools/abi-check.ps1`。

## 1. 范围

ABI 1.0 定义：

- 64 位 long mode、`syscall` 指令入口、静态链接 ELF64（`ET_EXEC`/`EM_X86_64`）；
- 调用号、参数寄存器、返回值、错误码；
- 四类句柄命名空间（fd / handle / ticket / shm）的数值语义与生命周期；
- 用户指针、长度、对齐限制；
- 结构体布局、版本与兼容策略。

不包含：

- POSIX/libc（D1 的用户态 Profile，ADR-005）、Linux/Windows 二进制兼容；
- 动态链接、`dlopen`、`ld.so`（D0 静态，D1 再评估）；
- display server / session manager 的 wire protocol（L2 内部契约，见
  `../design/display-server-ng.md`、`../design/session-ng.md`）；其中
  role_token 不是 syscall 句柄命名空间；
- 包 manifest 的完整格式（A 域包格式，见 `../design/auth-log-pkg-ng.md` §5）；
- 硬件/驱动接口（L1D 只通过 L0 的 syscall 胶水暴露）。

ABI 1.0 **不是**旧树的 ABI 续集；旧入口见 §2。

## 2. 与旧 32 位 ABI 的区分（必读）

旧树有两套入口，均**冻结在 main `69ab006`**，只修致命 bug，不加语义：

| | 旧 legacy | 旧 native | **ABI 1.0（本文）** |
|---|---|---|---|
| 入口 | `int 0x81` | `int 0x80` | `syscall` 指令（LSTAR） |
| 位数 | 32 位 | 32 位 | 64 位 |
| 编号源 | `src/kernel/include/abi.h` | 旧 `proc/trap/native.h` | `src/ng/syscall.h` |
| 参数 | `ebx/ecx/edx` | 旧 native 约定 | `rdi/rsi/rdx/r10` |
| 返回 | `eax`，多数失败 `-1` | `eax` | `rax`，负值 = `-errno` |
| 编号范围 | 1..95（`SYS_NR_MAX=95`） | 旧 native 编号 | 0/1、10–14、20–27、30–35、40–47 |
| 状态 | compat，冻结 | 旧树，冻结 | **冻结（1.0.0）** |

**同名不同号**的例子（不要混用）：

| 名字 | 旧 legacy | ABI 1.0 |
|---|---|---|
| `write` | `SYS_WRITE=1`（写 fd/缓冲） | `0`（串口/控制台）；fd 写是 `22` |
| `exit` | `SYS_EXIT=5` | `1` |
| `getpid` | `SYS_GETPID=2` | `42` |
| `port_create` | `SYS_PORT_CREATE=80` | `10` |
| `port_send` | `SYS_PORT_SEND=81` | `11` |
| `dmesg` | `SYS_DMESG=85` | `32` |
| `log_write` | `SYS_LOG_WRITE=93` | `33` |

约定：

1. SDK 只发 ABI 1.0 头；**不要** `#include "abi.h"`（旧 32 位）。
2. 旧 `0x81` 的编号空间与 1.0 完全独立；一个进程不能同时按两套编号发 `syscall`。
3. 旧 `abi.h` 的 `SYS_NR_MAX=95` 不约束 1.0；1.0 的未分配号见 §5.6。
4. 旧文档 `docs/api.md`、`docs/native-abi.md` 描述的是旧树，不是本规范。

## 3. 调用约定

```text
rax = 调用号
rdi = 参数 1
rsi = 参数 2
rdx = 参数 3
r10 = 参数 4
返回值 = rax（有符号 64 位；负值 = -errno）
```

- `syscall` 指令自身破坏 `RCX`（返回 RIP）与 `R11`（返回 RFLAGS）；内核用
  `STAR/LSTAR/FMASK` 进入，`FMASK=0x40700` 清 TF/IF/DF/AC。
- 内核在入口切换到当前线程的内核栈（`TSS.RSP0` / `syscall_kernel_rsp0`）；
  用户 RSP 只用于返回，不用于内核栈。
- ABI **只承诺 `rax`**。当前实现会保存/恢复 15 个 GPR，但调用方不得依赖
  `rdi/rsi/rdx/r10/r8/r9` 等寄存器跨 syscall 保持不变。
- 调用点按 SysV 约定保持用户栈 16 字节对齐；内核当前只统计/校验 RSP 是否落在
  用户栈区间，不强制对齐。
- 用户程序只用通用寄存器；D0 的构建参数是 `-mno-sse -mgeneral-regs-only`，
  没有 FPU/SIMD 上下文保存。不要依赖 SSE/AVX 跨 syscall 保留。
- **未分配调用号不属于 ABI 1.0**：调用它们的行为未定义；当前
  `syscall_dispatch()` 的 `default` 返回 `-EPERM(-1)`，但这不是承诺，SDK
  不得依赖或主动调用。`-ENOSYS(-38)` 的数值已保留（§10 第 1 项）。
- `timeout` 参数是**不透明 TSC tick**：`0` = 非阻塞（`EAGAIN`），
  `~0ULL` = 永久，其余 = 最多 N tick（`ETIMEDOUT`）。1.0 **不承诺**
  tick 与毫秒/秒的换算；应用应只用 0/`~0` 或轮询。

## 4. 类型、结构体与对齐

- 固定宽度类型：`u8/u16/u32/u64`，`i32/i64`；**小端**。
- 结构体按自然对齐、固定布局；不允许编译器 packing 差异。ABI 1.0 结构体：
  - `struct vfs_stat`：**24 B** — `{u32 type; u32 mode; u64 size; u64 ino}`；
  - `struct vfs_dirent`：**80 B** — `{u32 type; u32 name_len; u64 size; char name[64]}`；
  - `struct ipc_msg_hdr`：**16 B** — `{u32 type; u32 len; u32 from; u32 flags}`；
  - `struct ds_fb_info`：**48 B** — `{u64 base; u64 phys; u64 size; u32 pitch; u32 bpp;
    u32 width; u32 height; u32 flags; u32 reserved}`（仅 syscall 47）。
- 句柄/调用号/错误码是标量；句柄放进 `u64` 时高 32 位必须为 0。
- 用户指针是 64 位；只接受用户地址，内核地址/非规范地址 → `EFAULT`。
- **对齐是调用方责任**：结构体/整数输出参数必须自然对齐。内核按字节
  `copyin/copyout`，当前**不校验**对齐；未对齐访问是未定义行为（不是
  `EINVAL`）。ABI 1.0 只保证已对齐调用的结果。
- 结构体/错误码/编号冻结后只能**尾部追加**；已有字段的偏移/大小不可变。

## 5. 调用号表

> 与 `docs/abi/abi-1.0.h`、`sdk/include/aster/syscall.h`、`src/ng/syscall.h`
> 逐号一致（`tools/abi-check.ps1`）。用户指针一律指当前进程 AS 的 U=1 页。

### 5.1 基础（0/1）

| rax | 名称 | 参数 | 返回 | 说明 / 主要错误 |
|---|---|---|---|---|
| 0 | `write` | `rdi=buf, rsi=len` | 写入字节数 | 串口/控制台；`len` 1..256；`len=0/>256` → `EINVAL`；坏指针 → `EFAULT`；`rdi=0` 是 WP1-3 诊断模式，返回 0（**不是** fd 写） |
| 1 | `exit` | `rdi=code` | 不返回 | ring3 回到内核恢复点；进程退出码取低 8 位；内核上下文调用返回 0 |

### 5.2 IPC / 端口（10–14）

消息格式：16 B 头 + payload；`send/recv/reply` 的 `len` 是**含头总长**。

| rax | 名称 | 参数 | 返回 | 说明 / 主要错误 |
|---|---|---|---|---|
| 10 | `port_create` | `rdi=depth` | ticket（u32） | `depth=0` → 默认 16；上限 64；端口池 16；同时在本进程句柄表登记；`EINVAL/ENOSPC/ENOMEM` |
| 11 | `port_send` | `rdi=ticket, rsi=msg, rdx=len, r10=timeout` | 0 | `len` 16..4112；`hdr.len` ≤4096；`hdr.flags=0`；`hdr.from` 为 0 或存活 PORT；`EPERM/EBADF/EINVAL/EMSGSIZE/EFAULT/ENOMEM/EAGAIN/ETIMEDOUT/ECANCELED` |
| 12 | `port_recv` | `rdi=ticket, rsi=buf, rdx=max, r10=timeout` | 消息总长（可截断） | 仅属主进程；`max` 1..4112（>4112 截到 4112）；只拷贝 `min(总长,max)`；`EPERM/EBADF/EINVAL/EFAULT/ENOMEM/EAGAIN/ETIMEDOUT/ECANCELED` |
| 13 | `port_reply` | `rdi=ticket, rsi=msg, rdx=len, r10=timeout` | 0 | 与 send 共用核心；ticket 必须解析为存活端口；`EPERM/EBADF/EINVAL/EMSGSIZE/EFAULT/ENOMEM/EAGAIN/ETIMEDOUT/ECANCELED` |
| 14 | `port_close` | `rdi=ticket` | 0 | 仅属主进程；`EPERM/EBADF`；close 唤醒等待者（`ECANCELED`） |

### 5.3 文件系统（20–27）

| rax | 名称 | 参数 | 返回 | 说明 / 主要错误 |
|---|---|---|---|---|
| 20 | `open` | `rdi=path, rsi=flags, rdx=mode` | fd | 路径缓冲 256 B（含 NUL；wrapper 拒绝长度 ≥255，有效 ≤254 字符；组件 ≤63 B）；flags 见下；`EPERM/ENOENT/EIO/EEXIST/ENOTDIR/EISDIR/EINVAL/ENFILE/EMFILE/ENOSPC/EROFS/ENAMETOOLONG/ELOOP/EFAULT` |
| 21 | `read` | `rdi=fd, rsi=buf, rdx=len` | 字节数 | `len` 1..512；目录 `EISDIR`；`EBADF/EINVAL/EFAULT/EIO` |
| 22 | `write` | `rdi=fd, rsi=buf, rdx=len` | 字节数 | `len` 1..512；`O_APPEND` 写前定位尾；`EBADF/EINVAL/EFAULT/ENOSPC/EROFS/EIO` |
| 23 | `close` | `rdi=fd` | 0 | 坏 fd/旧 generation → `EBADF` |
| 24 | `stat` | `rdi=path, rsi=&vfs_stat` | 0 | 输出 24 B；`EFAULT/ENOENT/ENOTDIR/ENAMETOOLONG/ELOOP` |
| 25 | `listdir` | `rdi=path, rsi=ents, rdx=max` | 条数 | `max` 1..8（>8 截到 8；0/null → `EFAULT`）；每条 80 B；`EFAULT/ENOENT/ENOTDIR/ENAMETOOLONG/ELOOP` |
| 26 | `mkdir` | `rdi=path, rsi=mode` | 0 | `EEXIST/ENOENT/ENOTDIR/EPERM/ENOSPC/ENAMETOOLONG/EINVAL` |
| 27 | `unlink` | `rdi=path` | 0 | 目录 `EISDIR`；FAT 打开中 `EBUSY`；`ENOENT/EPERM/EBUSY/EISDIR/ENAMETOOLONG/ELOOP` |

`open` flags（`VFS_O_*`，只认这些位；其他位 → `EINVAL`）：

```text
O_RDONLY=0x000  O_WRONLY=0x001  O_RDWR=0x002  O_ACCMODE=0x003
O_CREAT =0x040  O_EXCL  =0x080  O_TRUNC=0x200 O_APPEND=0x400
O_DIRECTORY=0x800
O_FLAGS_MASK=0xEC3
```

`mode`：`VFS_S_IFREG=0x8000`、`VFS_S_IFDIR=0x4000`、`VFS_S_IFLNK=0xA000`，
低 9 位权限；ramfs/FAT16 当前只检查 owner 位，uid=0 绕过（§10 第 4 项）。

路径：先词法规范化（`//`、`.`、`..`、尾 `/`），再按最长挂载前缀选挂载点；
路径缓冲 256 B（含 NUL；有效 ≤254 字符）、组件 ≤63 B；符号链接深度 >8 → `ELOOP`。

### 5.4 输入 / L1 服务（30–35）

| rax | 名称 | 参数 | 返回 | 说明 / 主要错误 |
|---|---|---|---|---|
| 30 | `readkey` | — | `(flags<<24)\|(ascii<<16)\|scancode` / `-EAGAIN` | 非阻塞；无按下事件 → `EAGAIN(-11)` |
| 31 | `login` | `rdi=name, rsi=pass` | uid | 名字缓冲 32 B、口令 64 B（含 NUL；wrapper 拒绝长度 ≥31/63，实际 ≤30/62）；`ENOENT`（未知用户）、`EACCES(-13)`（口令错/锁定）、`EINVAL`、`EFAULT`；超长 → `ENAMETOOLONG` |
| 32 | `dmesg` | `rdi=buf, rsi=max` | 字节数 | `max` 1..4096；`EINVAL/EFAULT` |
| 33 | `log_write` | `rdi=tag, rsi=msg` | 0 | 服务层 tag 1..15、msg 1..127 可打印 ASCII；syscall wrapper 拒绝长度 ≥15/127（实际 ≤14/126）；内容非法/空 → `EINVAL`；超长 → `ENAMETOOLONG`；`EFAULT` |
| 34 | `pkg_list` | `rdi=buf, rsi=max` | 包数 | `max` 1..512；放不下 → `ENOSPC`；`EINVAL/EFAULT` |
| 35 | `pkg_verify` | `rdi=name` | 0 | 名字缓冲 32 B（wrapper 实际 ≤30）；包不存在 `ENOENT`、清单/路径非法 `EINVAL`、哈希不符 `EIO`、超长 `ENAMETOOLONG`、`EFAULT` |

`readkey` 返回：

```text
bits 23..16 = ascii（不可打印为 0）
bits 15..8  = scancode
bits  7..0  = flags
flags: 0x01 SHIFT, 0x02 CAPS, 0x04 CTRL, 0x08 EXT
```

返回值非负；没有按键时是 `-EAGAIN`。`ascii=0` 表示方向键等不可打印键，
scancode 仍有效。

### 5.5 进程 / 共享内存 / LFB（40–47）

| rax | 名称 | 参数 | 返回 | 说明 / 主要错误 |
|---|---|---|---|---|
| 40 | `spawn` | `rdi=path, rsi=argv, rdx=&pid` | 0 | path 缓冲 64 B（有效 ≤62 字符）；argv ≤8 项、单项 <128 B（有效 ≤126）、所有字符串+NUL ≤1 KiB；ELF ≤256 KiB；**超长 path/argv → `ENAMETOOLONG`**（不是静默截断）；`ENOENT/EINVAL/EFAULT/ENOMEM/ENOSPC/ENAMETOOLONG` |
| 41 | `wait` | `rdi=pid, rsi=&status` | 0 | 只等直接子进程；`status` 可为 0；阻塞；`ECHILD/EPERM/EINVAL/EFAULT` |
| 42 | `getpid` | — | pid | 内核上下文为 0；用户进程 ≥1 |
| 43 | `kill` | `rdi=pid, rsi=sig` | 0 | `sig` 0..255；状态 = `0x100\|sig`；`ESRCH/EPERM/EINVAL`；自杀不返回 |
| 44 | `getppid` | — | ppid | init 为 0 |
| 45 | `shm_alloc` | `rdi=pages, rsi=&va, rdx=&handle` | 0 | `pages` 1..1024；`&va` 8 B、`&handle` 4 B（内核当前按 8 B 可写校验，见 §10 第 14 项）；`EINVAL/EPERM/EFAULT/ENOMEM/ENOSPC` |
| 46 | `shm_map` | `rdi=handle, rsi=&va` | 0 | handle 是独立命名空间；owner 或 root；每对象 ≤8 条映射；`EBADF/EPERM/EFAULT/ENOMEM/EBUSY/ENOSPC` |
| 47 | `fb_map` | `rdi=&va, rsi=&info` | 0 | root-only；`&va` 8 B + `ds_fb_info` 48 B；固定 VA `0x60000000`；`EPERM/ENODEV/EBUSY/ENOMEM/EFAULT/EINVAL` |

`spawn` 的用户入口 ABI：`rdi=argc`、`rsi=argv`（用户栈上的 `char*[]`，NULL 结尾）；
用户栈 16 KiB @ `0x40100000`，守护页 `0x400FF000` 不映射；段 W^X。

### 5.6 未分配号

`2–9`、`15–19`、`28–29`、`36–39`、`48–...` 未分配。新调用只能从下一个未分配号
开始；冻结后**不得复用**已删除/保留的号。1.0 没有 ABI 版本查询调用（§10 第 5 项）。

## 6. 句柄命名空间（四类）

所有“句柄类”值都是 `u32 = (generation << 16) | slot`，`0` 永远非法；
`generation` 16 位，close 时 +1（回绕跳过 0）；slot 复用后旧值因 generation
不匹配而失效。不同命名空间**数值可能相同**，由 syscall 决定如何解释：

| 名称 | 值 | 作用域 | 生命周期 | 能跨进程？ |
|---|---|---|---|---|
| fd | `(gen<<16)\|slot`，slot 0..15 | 全局一份 VFS ctx（1.0） | `open` → `close`；close 后旧 fd `EBADF` | 否 |
| handle | `(gen<<16)\|slot`，slot 0..63 | 每进程句柄表（PORT/THREAD/FILE） | 进程退出/被 kill 按表回收；`spawn` 不继承 | 否 |
| ticket | `(gen<<16)\|slot`，slot 0..15 | 全局端口池（16 个） | `port_create` → `port_close`；close 后旧 ticket `EBADF` | **是**（唯一跨进程名字） |
| shm handle | `(gen<<16)\|slot`，slot 0..7 | 全局共享内存表 | `shm_alloc` → 最后一个映射解除/进程退出；无 `SHM_FREE` | **是**（经 IPC 传递） |

规则：

1. **fd 与 handle 不通用**：fd 走 20–27，句柄走 10–14/41 等；类型不匹配 → `EBADF`。
2. **ticket 是端口的全局身份**：`send/reply` 凭 ticket；`recv/close` 还要求
   调用进程 == `owner_pid`，否则 `EPERM`。`from` 字段由发送方填自己的 ticket，
   内核校验其存活；当前没有防伪（§10 第 3 项）。
3. **handle 是进程私有引用**：`port_create` 同时返回 ticket（作为句柄值）并在
   进程句柄表记一条；进程退出只回收自己的端口。
4. **shm handle 独立命名空间**：不能当端口 ticket 用；`shm_map` 校验 owner/root，
   当前所有进程 uid=0，所以实际是“知道 handle 就能映射”（§10 第 4 项）。
5. **close 语义**：close 后旧值立即失效；重复 close → `EBADF`；端口 close 会
   唤醒 recv/send 等待者（`ECANCELED`），有等待者时对象延迟回收。
6. **能力（cap）尚未实现**：ticket 是 bearer token，不是可撤销 capability；
   `cap_grant/revoke` 不在 ABI 1.0（§10 第 3 项）。role_token 是 L2 会话协议
   的 32 位值（`seq|role|nonce`，0 非法），不属于本 ABI 的四类命名空间。

## 7. 用户指针 / 长度 / 对齐限制

### 7.1 指针规则（`src/ng/uaccess.c`）

```text
va != 0, len != 0, va + len 不溢出 64 位
末地址 <= 0x00007FFF_FFFF_FFFF
对范围内每一页：P=1 且 U=1（写还要 W=1），按当前 CR3 的 PML4 查
SMAP=on 时 stac/clac 包住逐字节拷贝
任何一页失败 -> EFAULT，整段不触碰（先校验后拷贝）
```

实际用户窗口：ELF 段 `[0x40000000, 0x7FFFFFFFFF]`；固定 VA：

| 区域 | 地址 | 说明 |
|---|---|---|
| 用户代码/数据 | `0x40000000` 起 | ELF64 段，W^X |
| 进程标记页 | `0x40010000` | 每进程一页，AS 隔离证据 |
| 用户栈 | `0x40100000..0x40104000` | 16 KiB，RW+NX |
| 栈守护页 | `0x400FF000` | 不映射，越界 `EFAULT/#PF` |
| 共享内存 | `0x50000000..0x5F000000` | 每对象间留一页守护空洞 |
| LFB | `0x60000000` | root-only，PCD/PWT/NX |

`copyin_str`：最多 `max` 字节、总是 NUL 结尾；截断时返回 `max-1`。路径/名字
包装层把 `>= max-1` 转成 `ENAMETOOLONG`（20–27/31/33/35/40）；`spawn` 的
path/argv 超长也转成 `ENAMETOOLONG`（不再静默截断）。

### 7.2 长度上限

| 项 | 上限 | 超出 |
|---|---|---|
| `write(0)` 控制台 | 256 B | `EINVAL` |
| fd `read/write` | 512 B/次 | `EINVAL` |
| `listdir` | 8 条 × 80 B | 截到 8 |
| IPC 消息 | 16 B 头 + 4096 B payload = 4112 B | `EMSGSIZE`（>4112）/`EINVAL`（<16 或头不自洽） |
| `port_create` depth | 64（0=默认 16） | `EINVAL` |
| `dmesg` | 4096 B | `EINVAL` |
| `pkg_list` | 512 B | `EINVAL`（max 超限）/`ENOSPC`（放不下） |
| 路径 | 缓冲 256 B（含 NUL；有效 ≤254 字符；组件 ≤63 B） | `ENAMETOOLONG` |
| `spawn` path | 缓冲 64 B（有效 ≤62 字符） | `ENAMETOOLONG` |
| `spawn` ELF | 256 KiB | `EINVAL` |
| `spawn` argv | 8 项、单项 <128 B（有效 ≤126）、总 ≤1 KiB | `ENAMETOOLONG`（字符串超长）/`EINVAL`（总长超限） |
| `shm_alloc` | 1024 页（4 MiB）/对象；8 对象 | `EINVAL`/`ENOSPC` |
| `shm_map` | 8 条映射/对象 | `ENOSPC`/`EBUSY` |
| LFB | 1280×800×32 = 1000 页 | 由 fb 几何决定 |

## 8. 错误码表

所有错误以**有符号 64 位二补码负数**返回（`rax = (u64)(i64)-errno`）。
数值不是 Linux 的正式 errno 兼容承诺，只是 ABI 1.0 的冻结值；冻结后不得改值。

| 值 | 名称 | 引入/典型调用 |
|---|---|---|
| 0 | `OK` | 成功 |
| -1 | `EPERM` | 权限/属主；**未分配调用号的当前返回值（不属于 ABI 契约）** |
| -2 | `ENOENT` | 路径/用户/包不存在 |
| -3 | `ESRCH` | `kill` 目标 pid 不存在 |
| -4 | `EINTR` | 保留（当前未主动返回） |
| -5 | `EIO` | 块设备/簇链/哈希不符 |
| -9 | `EBADF` | 坏 fd/handle/ticket/generation |
| -10 | `ECHILD` | `wait` 非直接子进程 |
| -11 | `EAGAIN` | 非阻塞 IPC 空/满；`readkey` 无键 |
| -12 | `ENOMEM` | 页/堆/内核缓冲不足 |
| -13 | `EACCES` | auth 口令错/锁定（注意与 `EPERM=-1` 区分） |
| -14 | `EFAULT` | 用户指针/长度非法 |
| -16 | `EBUSY` | COMMIT 前未 MAP；FAT 打开中 unlink |
| -17 | `EEXIST` | `O_CREAT\|O_EXCL` / mkdir 已存在 |
| -19 | `ENODEV` | 无 LFB / 非 ATA 设备 |
| -20 | `ENOTDIR` | 路径组件不是目录 |
| -21 | `EISDIR` | 对目录 read/unlink |
| -22 | `EINVAL` | 参数/长度/flags/协议非法 |
| -23 | `ENFILE` | VFS 全局打开文件上限 |
| -24 | `EMFILE` | 每 ctx fd 上限（16） |
| -28 | `ENOSPC` | 磁盘/节点/句柄/端口/包列表缓冲不足 |
| -30 | `EROFS` | 只读文件系统 |
| -36 | `ENAMETOOLONG` | 路径/字符串超长 |
| -38 | `ENOSYS` | 保留（1.0 内核不返回；1.1 用于未分配号） |
| -39 | `ENOTEMPTY` | 保留（1.0 没有 rmdir） |
| -40 | `ELOOP` | 符号链接深度 >8 |
| -90 | `EMSGSIZE` | IPC 消息 >4112 B |
| -110 | `ETIMEDOUT` | IPC/ATA 超时 |
| -125 | `ECANCELED` | 端口 close 唤醒等待者 |

注意：

- `EACCES(-13)` 只由 auth 使用；VFS 权限不足用 `EPERM(-1)`。不要按 POSIX 习惯
  把两者混为一谈。
- `-1` 在 ABI 1.0 有歧义（权限 vs 未分配号），但**未分配号行为不属于契约**；
  应用只应把 `-1` 当作 `EPERM` 处理（§10 第 1 项）。
- 旧 `0x81` 的 `E_*` 值是另一套（`E_INVAL=-1`、`E_NOENT=-2`…），不能与上表混用。
- SDK alpha 目前未导出 `ESRCH(-3)` / `EINTR(-4)` / `ECHILD(-10)` 的 `ASTER_E*`
  宏（ABI 头已冻结数值）；SDK 后续只追加这三个宏，不改变数值（§10 第 15 项）。

## 9. 版本 / 冻结 / 兼容策略

1. **只追加，不破坏**：冻结后，已有调用号、参数个数/含义、返回类型、结构体字段
   偏移/大小、错误码数值都不变；新能力只能新增调用号或结构体尾部字段。
2. **删除的调用号保留**：不得复用；未分配号按 §5.6 保留。
3. **结构体版本**：只能尾部追加字段，并 bump minor；调用方用固定大小或未来
   新增的 `size` 字段协商。1.0 的四个结构体没有版本字段。
4. **错误码**：数值冻结；新增错误码只能追加新值，不复用旧值。
5. **ABI 版本**：1.0 **没有运行时版本查询 syscall**；版本在
   `abi-1.0.h` / `syscall.h` 的编译期宏（`ASTER_ABI_VERSION_*`）。SDK 按头文件
   版本假设；运行时查询留到后续 major（§10 第 5 项）。
6. **冻结门槛（已满足）**：64 位内核 + SDK alpha + 外部示例可编译；机器可读头
   `docs/abi/abi-1.0.h` 与三方编号由 `tools/abi-check.ps1` 钉住；旧 `0x81`
   只作为 compat 保留，不并入 1.0。
7. **兼容性测试**：E2 parity 100 项覆盖 VFS/ramfs/FAT16/ATA/auth/log/pkg/shell；
   E3 的 display/session/进程由 kernel/host 单测 + run-ng 覆盖；1.0 冻结后新增
   “ABI 回归”用例集（`abi-check` + SDK host 单测）。

## 10. 冻结前待决项的处理结论

下表是草案 §10 的 13 项，加冻结审计新发现的 2 项。**结论只有三类**：
“保留”（写进 1.0 契约）、“延后”（不进入 1.0，登记后续）、“已解决”。

| # | 待决项 | 结论 | 1.0 契约 / 处理 |
|---|---|---|---|
| 1 | 未分配调用号返回 `-1`（EPERM）而非 `ENOSYS` | **延后** | 1.0 不定义未分配号行为；SDK 不得调用。`ENOSYS(-38)` 数值保留，1.1 改返回不构成对合法调用的破坏。 |
| 2 | IPC timeout 是未校准 TSC tick | **保留** | 1.0 冻结为不透明 tick：只保证 0 与 `~0`；其它值不承诺毫秒。应用用 0/`~0` 或轮询；带时间单位的 API 留到 1.1。 |
| 3 | ticket 是 bearer token，无 cap/防伪 | **保留** | 1.0 明确 ticket 是 bearer token；`recv/close` 校验属主，`send/reply` 不校验调用者，`from` 可伪造。cap/revoke 留到 1.1；D0 安全边界依赖“不泄露 ticket”。 |
| 4 | 所有进程 uid=0，owner/root 校验宽松 | **保留** | 1.0 权限模型 = root-only；所有进程 uid=0，FBMAP root-only 对所有进程成立，VFS owner 检查实际不区分用户。每进程 uid/凭据留到 1.1（fsd）。 |
| 5 | 没有 ABI 版本查询 | **延后** | 1.0 不新增调用号；版本是编译期宏。运行时查询若需要，在后续 major 分配新号，不复用未分配号。 |
| 6 | 没有机器可读 ABI 头 | **已解决** | `docs/abi/abi-1.0.h`（本冻结）逐号/逐常量镜像；`tools/abi-check.ps1` 与 SDK/内核三方比对。 |
| 7 | ring3 #PF/#GP 仍 panic | **延后（已知限制）** | 1.0 只承诺 syscall 经 uaccess 的坏指针返回 `EFAULT`；用户态直接解引用坏地址/执行特权指令不在 ABI 内，D0 内核会 panic。用户 fault → kill 进程 + 状态码留到 D1 前。 |
| 8 | `write` 命名冲突（0=控制台，22=fd） | **保留** | 0 命名为 `write` / 机器头 `ASTER_ABI_SYS_WRITE`；22 命名为 `write`(fd) / `ASTER_ABI_SYS_WRITE_FD`；SDK 用 `aster_console_write` 与 fd API 区分。不重命名。 |
| 9 | 结构体对齐未强制 | **保留（调用方责任）** | 调用方必须自然对齐；内核不校验，未对齐是未定义行为（不是 `EINVAL`）。1.0 只保证已对齐调用。 |
| 10 | 没有 `mmap/mprotect/lseek/dup/pipe/socket/sleep` | **延后** | 1.0 不提供、不分配号；D1 的 POSIX Profile 在用户态 shim 实现，不改变 1.0 编号。 |
| 11 | `shm` 无 `SHM_FREE` | **保留** | 1.0 生命周期：对象在最后一个映射解除或进程退出时回收；没有显式 free。`shm_alloc` 后进程退出即释放；`shm_map` 可重复映射（≤8）。`SHM_FREE` 若加，用新调用号（1.1）。 |
| 12 | fd 表仍是全局一份 | **保留** | 1.0 的 fd 是全局 VFS ctx 的命名空间（`VFS_FD_MAX=16`），不是每进程；`spawn` 的新进程看不到父进程 fd，但 fd 数值空间是全局的。每进程 fd 表留到 1.1（fsd）。 |
| 13 | `spawn` 的 path/argv 超长被静默截断 | **已解决** | `proc_user_str_len_ok` 拒绝 `len >= cap-1`；path/argv 超长统一 `ENAMETOOLONG`。path 有效 ≤62、arg 有效 ≤126；不再接受截断后的路径。 |
| 14 | `shm_alloc` 对 `&handle` 按 8 B 而非 4 B 做可写校验 | **保留（记录实现细节）** | ABI 语义是 4 B 输出；当前实现多校验 4 B。调用方应把 `u32` 放在至少 8 B 可写区域内（SDK 的局部变量满足）；1.1 修正校验长度，不改变参数含义。 |
| 15 | SDK 未导出 `ESRCH`/`EINTR`/`ECHILD` 宏 | **保留** | ABI 头冻结 `-3/-4/-10`；SDK alpha 只有 `ASTER_E*` 子集。SDK 后续只追加这三个宏（值不变），不构成破坏。 |

## 11. 兼容矩阵（D0 证据范围）

| 维度 | 1.0 覆盖 | 证据 / 限制 |
|---|---|---|
| CPU / 模式 | x86_64 long mode；SMEP/SMAP 开启或关闭均可 | `-cpu max` 与 `-cpu qemu64` 的 run-ng 均 PASS（WP3-3/WP3-5） |
| 固件/引导 | QEMU `-kernel`（multiboot2 + PVH note） | D0 不支持 UEFI stub / 真机安装器 |
| 显示 | QEMU stdvga Bochs VBE 1280×800×32；回退 1024×768；`-vga none` → VGA 文本 | 像素断言只在 stdvga 1280×800 + QMP screendump 下成立 |
| 块设备 | QEMU PIIX3 IDE + ATA PIO；8 MB FAT16 `disk.img` | 其它控制器/真盘不在 D0 矩阵 |
| 输入 | PS/2 set 1 键盘 | 鼠标/IME 未实现 |
| 用户程序 | 静态 ELF64，`ET_EXEC`/`EM_X86_64`，基址 `0x40000000` | 无动态链接、无 PIE |
| 工具链 | clang 23.1.4 / ld.lld 23.1.4 | 构建参数见 `build-ng.ps1` |
| SDK | `sdk/include/aster/*.h`（v3 display + session v1） | SDK alpha；不承诺 POSIX |

## 12. 最小调用示例（伪代码）

```c
/* 打开 /etc/passwd 并读 16 字节；错误统一按 -errno 处理。 */
static inline long sys_open(const char *path, unsigned flags, unsigned mode) {
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(20L), "D"(path), "S"(flags), "d"(mode)
                     : "rcx", "r11", "memory");
    return r;
}
```

注意：`path` 必须是用户地址；返回值 `<0` 时取 `-r` 作为 errno。实际 SDK 会提供
包装和常量头，应用不应直接写裸 `syscall`。

## 13. 变更记录

| 日期 | 版本 | 变更 |
|---|---|---|
| 2026-10-31 | v0.1 | 从 `src/ng/syscall.h` + WP1-8/2-x/3-x 的实际实现提取草案；标注与旧 0x81 的区别；列出冻结前缺口 |
| 2026-10-31 | **v1.0.0** | **冻结**：补机器可读头、`spawn` 超长 `ENAMETOOLONG`、四类命名空间、15 项冻结结论、兼容矩阵；编号/常量由 `tools/abi-check.ps1` 三方比对 |

## 14. 冻结证据 / 校验

```text
powershell -File tools/abi-check.ps1
  [ok] 29 syscalls agree (ABI/SDK/kernel): 0/1, 10-14, 20-27, 30-35, 40-47
  [ok] 28 error codes frozen; SDK mirrors 26; <N> kernel aliases agree
  [ok] <M> ABI constants match <K> kernel-source checks
  [ok] struct layouts: vfs_stat=24, vfs_dirent=80, ipc_msg_hdr=16, ds_fb_info=48
  [ok] docs/abi/abi-1.0.md is marked frozen
  [ok] ABI version 1.0.0 (0x00010000)
  abi-check: PASS
```

（`N/M/K` 的具体数字以实际运行输出为准；见 `../claims/ng-WP4-1.md`。）

`docs/abi/abi-1.0-draft.md` 保留为冻结前的历史草案；其“未冻结”表述不再有效。
