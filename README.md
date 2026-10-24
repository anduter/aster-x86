# x86 OS

从零实现的 32 位 x86 教学操作系统：自写引导扇区 + 二阶段加载器 + 32 位 C 内核，
在 QEMU（`-vga std`，VBE 0x118 1024x768x24）上启动，带 framebuffer 文字控制台、
物理/虚拟内存管理、堆、内存自检、IDT 与时钟/键盘中断。

## 系统结构模型（目标架构）

[`docs/system-model.md`](docs/system-model.md) 定义了 Aster 的目标结构：能力（capability）
驱动的强类型对象、每进程私有命名空间、ring3 服务化文件系统/网络/驱动，以及与 Unix/Linux、
Windows NT、seL4、Mach、Plan 9、Fuchsia、Minix 3/QNX 的逐项差异。

原生程序接口规范见 [`docs/native-abi.md`](docs/native-abi.md)：`int 0x80`、类型化句柄、
Port/Channel IPC、`process_create` 与 ELF32 启动约定。POSIX/Linux 兼容层后置。

P3 安全边界与工程约定见 [`docs/security-model.md`](docs/security-model.md)：
ELF 段权限分离、代码页只读、COW 只作用于可写页、明确限制与安全测试矩阵。

> 注意：该文档描述的是迁移目标，不代表当前实现已经具备全部能力；当前代码仍是原型。

## 特性

| 模块 | 状态 |
|---|---|
| 引导扇区（MBR） | 读加载器、校验 `LOADER` 签名、错误码可读 |
| 二阶段加载器 | 386+ 检查、E820/CPUID/SMBIOS/VBE/磁盘信息采集、Unreal Mode 分块载入内核、VBE 模式切换、进入保护模式 |
| 内核 | 平坦段、framebuffer、console、kprintf、panic、PMM、VMM（PAE：2MB 恒等映射 + 4KB NX 用户页）、heap、memtest、IDT/PIC/PIT/键盘 |
| 对象模型 | 程序 / 文件 / 设备 / 通道 五类对象并列，句柄带类型标签（不是“一切皆文件”） |
| 启动自检 | `demo_run()` 验证三类对象独立、句柄类型不能被混用 |
| shell | **ring3 程序**：内核从 `/system/bin/shell` 载入，迁移期通过 `int 0x81` 读键盘、读写文件 |
| 进程/调度 | 进程表、内核线程、上下文切换、时间片轮转；**阻塞等待、睡眠超时、COW fork、信号（可注册用户处理函数）、管道/dup2** |
| Native ABI | **`int 0x80`（DPL3）**：ABI 版本、exit/yield、类型化句柄 close/dup/info、调试输出；ring3 demo 已验证 |
| 旧系统调用 | `int 0x81` 过渡门：write / getpid / uptime / yield / exit 等原有调用表 |
| P3 安全边界 | ELF 代码页只读/数据页可写、拒绝 RWX 段、COW 只作用于可写页；编译器 `-Werror`；完整限制见安全模型 |
| CPU 安全特性 | SMEP/SMAP 按 CPUID 启用；SMAP 下用户拷贝和信号帧走 STAC/CLAC；QEMU `-cpu max` 验证 |
| 用户栈/堆/蹦床 ASLR | 栈顶在顶部 512KB、堆基址和蹦床页在各自 256KB 范围内每进程随机 |
| 沙箱 | legacy + native syscall allowlist；`SYS_OPEN` 和 `HANDLE_DUP` 均被拒绝，`userdemo` 已验证 |
| 镜像签名 | RSA-2048/SHA-256 PKCS#1 v1.5；启动与 `user_exec` 验签，篡改镜像被拒绝 |
| 地址空间回收 | 进程退出/exec 释放用户物理页、PT/PD/PDPT；fork COW 共享页按引用计数回收 |
| 用户态 | 每进程页目录 + ELF32 加载器 + argc/argv 启动栈 + mini-libc；用户栈按需增长；信号蹦床页支持 `sigreturn` |
| 屏幕小部件 | 右下角心跳 `hb N *`、左侧 `uptime N s`；周期输出只更新状态栏，内容区保持静止 |
| 文件系统 | **驱动器为根**（Windows 风格 `system:/bin`、`data:/a.txt`）：顶层是卷表本身，没有虚拟 `/` |
| 块设备 / 持久化 | `DEV_BLOCK` + **ATA PIO（`ata0` XFS1 系统盘 + `ata1` FAT16 交换盘）+ `ram0` 内存盘**；`XFS1` v3；**重启后文件还在**；`fat:/` 可直接和宿主交换文件 |
| 平台 / 外设 | **串口输入（IRQ4）、PCI 枚举、ACPI S5 关机、LAPIC 探测 + NMI 看门狗、PS/2 鼠标（IRQ12，`mouse`）、e1000 网卡（MAC/链路、ARP、ICMP echo 收发）** |

## 快速开始

需要：`nasm`、`i686-elf-gcc/ld/objcopy`、`qemu-system-i386`、`openssl`。
仓库自带交叉工具链在 `tools/i686-elf/bin`（`build.ps1` 会自动加入 PATH）。
构建时用 `keys/shell_private.pem`（测试密钥）给 shell ELF 签名。

**Windows（推荐，无需 POSIX shell）**

```powershell
.\build.ps1            # 构建 dist\floppy.img
.\build.ps1 -Run       # 构建并带窗口启动（串口打到控制台）
.\build.ps1 -Test      # 构建 + 无界面启动 + 自动断言（失败返回非 0）
.\build.ps1 -Shot a.ppm  # 构建并截屏
```

默认 `-Cpu max`，测试会硬断言 SMEP/SMAP；`-Cpu qemu32` 只用于观察不支持路径，此时对应的安全断言不适用。

**GNU Make（Git Bash / MSYS2 / WSL）**

```sh
make                 # 构建
make run             # 带窗口启动
make run-headless    # 无界面，仅串口
make debug           # 启动后暂停，等 GDB 连 1234 端口
make test            # 自动验证（内部调用 build.ps1 -Test）
make clean
```

## 目录结构

```
docs/architecture.md  架构设计：五类对象、命名空间分离、系统调用 ABI、落地顺序
src/user/      用户态程序：user_shell.c（ring3 shell）+ user.ld，链接到 0x40000000
src/boot/      引导扇区（512 字节）
  boot.asm         读加载器并校验签名
  print.asm        实模式字符串/十六进制输出
src/loader/    二阶段加载器（32 扇区 = 16KB）
  loader.asm       主流程与阶段提示
  kernel_loader.asm 内核分块读取 + Unreal Mode 复制到 1MB
  sysinfo.asm      E820 / CPUID / SMBIOS / VBE / 磁盘信息采集
  vesa_mode.asm    切换到 VBE 0x118
  pm_entry.asm     GDT/IDT、进入 32 位保护模式
src/kernel/
  head.asm         内核入口（0x100008）
  kernel.c         内核主流程与 idle 循环
  console.c/h      文字控制台（framebuffer + 串口镜像）
  fb.c/h           显存驱动与字体（BIOS VGA 8x8 字模 + 整数倍缩放）
  intr/            中断子系统（IDT / 入口桩 / IRQ）
    interrupt.h      对外接口与 interrupt_frame_t
    idt.c            IDT、异常分发（交给 panic）
    isr.asm          异常/IRQ 入口桩与入口地址表
    irq.c            8259 重映射、PIT 时钟、键盘、IRQ 注册与伪中断处理
    syscall.c/h      系统调用分发与 sys_call()（int 0x80）
    uaccess.c/h      安全访问用户内存（缺页可恢复）
  core/            对象模型（P0）
    object.c/h       对象表与类型化句柄（低 24 位槽号，高 8 位类型）
    program.c/h      程序注册表：程序是注册项，不是文件路径
    device.c/h       设备注册表：按类别暴露强类型函数，无 /dev
    demo.c/h         启动自检（对象模型自证）
  apps/            程序层（不是内核模块）
    shell.c/h        命令解释器：读 kbd0、用 prog_run 执行程序
    heartbeat.c/h    心跳线程：每秒打印一行，验证调度
  proc/            进程与调度（P1）
    proc.h           进程接口
    proc_internal.h  进程表与切换函数（process.c / sched.c 共享）
    process.c        进程表、线程创建、退出、上下文切换封装
    sched.c          环形轮转、时间片、抢占
    switch.asm       context_switch 与 thread_trampoline
  user/            用户态（P2）
    gdt.c/h          GDT（内核/用户段）+ TSS
    user_entry.asm   gdt_flush / tss_flush / user_enter（iret 进 ring3）
    user_demo.asm    极小的 ring3 程序（位置无关，复制到用户页执行）
    user_mode.c/h    分配用户页、加 U/S 映射、进 ring3
  fs/fs.h          文件系统公开接口：路径、目录、文件句柄
  fs/ramfs.c       内存后端：节点树、路径解析、句柄与目录遍历
  panic.c/h        致命错误处理
  lib/kprintf.c    printf/格式化实现
  lib/string.c     memmove/memset/strlen 等
  mm/pmm.c         物理页位图（按 32 位字扫描 + rover）
  mm/vmm.c         PAE 页表、2MB 恒等映射、4KB NX 用户页、COW、vmalloc
  mm/memtest.c     内存子系统自检（8 项）
  heap.c           固定区堆分配器
  version.h        系统名称/版本/构建时间
```

## 启动流程

```
BIOS -> LBA0 boot.bin (0x7C00)
       └─ 读 LBA1..32 到 0x8000，校验末尾 "LOADER"
          └─ loader：CPU 检查 -> 采集系统信息到 0x5000
                     -> 内核按 28 扇区分块读到 0x1000，再复制到 0x100000
                     -> 切换到 VBE 0x118
                     -> 保护模式，跳到 0x100008
                        └─ kernel_main()
```

## 物理内存布局

| 地址 | 用途 |
|---|---|
| `0x0000`–`0x04FF` | 中断向量表 / BIOS 数据区（不可碰） |
| `0x1000`–`0x47FF` | 加载器分块读缓冲 |
| `0x5000`–`0x58FF` | 系统信息表（E820 / VBE / CPU / 磁盘） |
| `0x7C00` | 引导扇区加载位置 / 临时栈 |
| `0x8000`–`0xBFFF` | 加载器自身 |
| `0x100000` | 内核镜像（前 8 字节为 `KRNL` 头） |
| `0x200000` | PMM 位图 |
| `0x800000`–`0xC00000` | 堆（16MB，已向 PMM 声明保留） |
| `0xFD000000` | VBE 线性显存（按不缓存映射） |

虚拟地址方面：PAE 恒等映射覆盖全部物理内存（2MB 页），
`vmalloc` 区固定在 `0xC0000000`（3GB）起 32MB，避开恒等映射大页。
用户页使用 4KB PTE：text R+X、rodata R+NX、data/stack/heap RW+NX。

## 崩溃处理

- **双重故障（vec 8）有专用栈**：`isr8` 先把旧栈上的 err/eip/cs/eflags 读出来，切到 4KB 静态栈
  `df_stack` 再进通用入口，避免坏栈直接三故障复位；
- **panic 转储**：串口打印原因 + `stack:` 16 个 32 位字；屏幕面板追加前 4 个字；
  串口日志即持久化现场（`-serial file:`）；
- **ring0 oops**：内核异常打印异常名/eip/err/cr2 + **符号化 EBP 回溯**（`函数名+偏移`），
  杀当前线程而不是停机；符号表由构建时两遍链接生成（`nm` → `obj/symbols.c`，367 个函数符号）；
- **ring3 异常**：先尝试用户栈增长，失败才杀进程；
- **`SYS_REBOOT`（27）**：用户态可复位机器（8042 `0xFE`），shell 命令 `reboot`；
- **`SYS_PANIC`（28）**：调试入口，shell 命令 `crash` 可人为触发 panic 验证转储；
- **`SYS_OOPS`（29）**：调试入口，shell 命令 `oops` 触发内核缺页，验证符号化回溯；
- **NMI 看门狗**：`apic_watchdog_start()` 映射 LAPIC 页、打开 APIC，用 PIT 的 100Hz tick
  校准 LAPIC 定时器（100ms 计数差），再把 LVT Timer 配成“周期 + NMI 投递”（约 250ms）。
  NMI 处理函数比较 `timer_ticks + syscall_count`：连续 4 次没前进（约 1 秒）→ panic。
  **NMI 不受 IF 影响**，所以 `cli; hlt` 卡死也能被发现；`hang` 命令可验证：
  `NMI watchdog: no progress for ~1s (eip=...)`；
- **e1000 网卡**：PCI 找 8086:100e → **打开 CMD 的 MEMORY+BUS MASTER**
  （不打开总线主控时 TX 的 `TDH` 照样前进，但帧根本不上线——这是之前 RX/TX 都"看起来成功"的根因）
  → 映射 BAR0 → 读 MAC/链路 → 初始化 TX/RX 描述符环 →
  **ARP 解析网关 MAC + IPv4/ICMP echo 收发**（启动日志 `net: ARP reply gw=…`、
  `net: ICMP echo reply seq=1`，两条都进了自动回归）；
  UDP 与协议栈分层仍未实现；
  （浸泡测试 180 秒无串口输出判定挂死）。

## 中断

- 8259 重映射到 `0x20` / `0x28`；PIT 通道 0 工作在模式 3、100 Hz；
- **IRQ 可注册**：`irq_register(irq, handler, arg, name)`，定时器和键盘由 `interrupt_init()` 自己注册，
  `irq.c` 里不再硬编码设备逻辑；
- **PIC 屏蔽按线管理**：`irq_mask/irq_unmask` 读改写屏蔽字，放行从片 IRQ 时自动放行主片 IRQ2；
- **伪中断处理**：IRQ7/IRQ15 先读 ISR 判断真假，伪中断不发 EOI（从片伪中断只给主片发 EOI）；
  没有处理函数的 IRQ 会被自动屏蔽，避免反复触发；
- **异常分级处理**：`exception_register(vec, handler)` 的处理函数返回
  `EXC_HANDLED`（已修复，重执行）/ `EXC_KILL`（杀当前线程）/ `EXC_PANIC`（打印现场停机）；
  未注册的向量按默认策略：**ring3 异常杀线程，ring0 异常才 panic**；
- **缺页可恢复（通用异常表）**：`intr/fault.c` 维护 `(start, end, fixup)` 表，最多 8 条；
  `copy_from_user/copy_to_user` 走汇编的 `uaccess_copy`，其 `[start,end)` 已登记，
  缺页时 #PF 钩子把 eip 改成 fixup（失败返回），内核继续运行
  （启动日志里 `sys: unmapped user pointer -> -1` 就是这条路径）；
- **键盘增强**：Shift（左右）、CapsLock、上档符号表，0xE0 扩展键前缀会被正确吞掉；
- 统计接口 `irq_count/irq_name/irq_spurious_count`，shell 里用 `irq` 命令查看：

```
shell> irq
irq: ticks=301 spurious=0
  irq0 timer count=301
  irq1 keyboard count=7
```

## 启动自检与 shell

### 启动自检（内核行为）

内核启动时跑一次 `demo_run()`，验证对象模型的三条约束：

```
demo: programs = 3
  prog shell@1
  prog hello@1
  prog memtest@1
demo: fs write/read ok (16 bytes)
demo: handle type check ok (prog-as-file=-4, file-as-device=-4)
demo: devices = 3
  dev char:kbd0
  dev video:fb0
  dev timer:pit0
```

最后一行是关键：把程序句柄当文件用、把文件句柄当设备用，都会被 `E_TYPE(-4)` 拒绝。

### shell（一个 ring3 程序）

shell 不再是内核模块，而是**用户态程序**：

- 源码在 [src/user/user_shell.c](src/user/user_shell.c)，链接到 `0x40000000`；
- 构建时转成裸二进制并嵌入内核，启动时由内核写入文件系统 `/system/bin/shell`；
- `shell@1` 的入口只是「exec `/system/bin/shell`」：`user_exec()` 从文件系统读出映像、
  装进用户页、`iret` 进 ring3（失败才退回内核内建 shell）；
- ring3 shell 的全部能力都来自系统调用：`SYS_WRITE` 输出、`SYS_GETCHAR` 读键盘、
  `SYS_OPEN/FREAD/FWRITE/CLOSE` 操作文件、`SYS_LIST` 列目录、`SYS_DRIVES` 列盘符、`SYS_EXIT` 退出。

```
ushell: ready (ring3, int 0x80)
ushell> help
ushell: commands
  help [cmd]             this help / help for one command
  drives                 list drives (system, data, fat, ...)
  ls [-l] [path]         no path = list drives; -l = long format
  cat [path]             show a file, e.g. system:/docs/a.txt
  write <path> <text>    create/overwrite a file (quotes allowed)
  mkdir <path>           create a directory
  rm <path>              remove a file/dir
  cd <path>              change directory; cd - returns
  pwd                    show current directory
  stat <path>            show type/size/mtime
  echo <args...>         print arguments
  sleep <ms>             sleep milliseconds
  fork                   create a child process, wait for it
  exec <path> [args]     replace this shell with a program
  <cmd> > <file>         redirect stdout
  <cmd> < <file>         redirect stdin
  <cmd1> | <cmd2>        pipe
  history                show recent commands
  !!  / !n               repeat last / n-th command
  reboot / crash / trap / kill / sync / fsck / date / exit
```

shell 交互细节：

- **引号与转义**：`write system:/a.txt 'hello world'`、`"..."`、`\ ` 都支持；
- **`ls -l`**：`-  16  2026-10-22 14:30:04  hello.txt`（类型/大小/mtime）；
- **历史**：`history` 列出最近 8 条，`!!` 重放上一条，`!3` 重放第 3 条；
- **`cd -`**：回到上一个目录；
- **`help <cmd>`**：单命令用法；
- **错误走 fd 2**：`cat nope > out.txt` 不会把错误信息写进 out.txt；
- **退格**：输入框里直接用 Backspace 编辑，回车后整行进输出区。

`prog run memtest` 会真的执行注册的 `memtest` 程序并打印结果。

### 界面：输出框 + 输入框

- 字体默认加载 **BIOS VGA ROM（F000:FA6E）的标准 8×8 字模**，比手写字库规整；
  ROM 不可用时回退到内核内置的手写 8×8 字模；
- `FONT_SCALE` 控制整数倍放大：默认 **2×**（清晰、易读），`make FONT_SCALE=1` 回到紧凑模式；
  标题栏、状态栏、输入框、输出框都按同一比例缩放；
- 每个字符的每个像素按 `scale × scale` 方块绘制，不做插值，边缘保持锐利。

内容区被拆成上下两个带边框的框：

```
┌──────────── 输出框（可滚动）────────────┐
│ 日志、命令历史、命令结果                 │
└──────────────────────────────────────────┘
┌──────────── 输入框（单行）──────────────┐
│ shell> 正在输入的内容                    │
└──────────────────────────────────────────┘
```

- 输入只画在下方输入框里（不换行、不进串口、不影响输出区）；
- 回车后才把整行写进输出框，形成命令历史；
- 退格只重画输入框那一行；
- 状态栏左侧仍是 uptime，右侧是心跳小部件。

shell 本身已经是 ring3 程序（见「用户态」一节）：它通过 `SYS_WRITE` 输出、
`SYS_GETCHAR` 读键盘，不再直接调用 `kprintf`。控制台这里只负责"把内核和用户
写进来的字节画到框里"，不区分调用者。

## 文件系统

`fs/` 是一个独立模块，公开接口在 [fs.h](src/kernel/fs/fs.h)，后端实现是内存卷：

```
fs_init()                      建默认驱动器 "system"（顶层是卷表本身）
fs_mount("data")               再挂一个驱动器（名字最长 31 字节，最多 4 个）
fs_volume_count() / fs_volume_name(i, buf, max)
fs_open(path, flags)           打开/创建文件 -> OBJ_FILE 句柄
fs_read / fs_write / fs_close  基于句柄读写（会校验句柄类型）
fs_stat / fs_mkdir / fs_unlink 元数据与目录操作
fs_listdir(path, index, ...)   目录遍历（路径为空时列出驱动器）
```

路径规则（**驱动器各自为根**，Windows 语义）：

| 写法 | 含义 |
|---|---|
| `system:/` | 驱动器 system 的根 |
| `system:/bin/shell` | 驱动器 system 下的 bin/shell |
| `data:/a.txt` | 驱动器 data 下的 a.txt |
| `/docs/a.txt` | 省略驱动器 → 默认驱动器 |
| `docs/a.txt` | 相对路径 → 拼当前 cwd（默认 `system:/`） |
| `..` / `.` | 在各驱动器树内解析，`..` 到驱动器根为止 |
| `ls`（无参数） | 顶层是卷表：列出所有驱动器 |

- 没有虚拟的 `/` 根节点——顶层就是文件系统模块的卷表；
- 驱动器根节点名字就是驱动号，`system:/` 的 `..` 停在根；
- 非空路径若带 `:` 而卷名不存在则报错；不带 `:` 走默认驱动器。

特点：

- **目录树**：节点用 `parent / first_child / next_sibling` 组成，路径逐段下钻；
- **驱动器**：每个卷一棵独立树，根节点名字即驱动号，名字最长 32 字节，最多 4 个；
- **句柄类型安全**：文件句柄是 `OBJ_FILE`，拿它当设备用会被 `E_TYPE` 拒绝；
- **容量**：64 个节点（所有卷共用）、文件名/目录名最长 **64 字符**、单文件 16KB、最多 8 个同时打开的文件；
- **持久化**：`fs_sync` / `fs_mount_disk` 把整棵树序列化到块设备（XFS1 v3）；
  启动时优先挂载 `ata0`（8MB 硬盘镜像），没有硬盘才退回 `ram0` 内存盘；
  shell 里 `write`/`mkdir`/`rm` 后会自动落盘，也可以手动 `sync`；
- **RTC 时间戳**：文件/目录记录 `mtime`（CMOS 时钟，按 UTC 解释），
  `stat` 会显示，`date` 命令打印当前时间；
- **扇区缓存**：块设备层 8 项写穿透缓存（写时失效），减少重复读盘；
- **fsck**：`fsck` 命令检查类型/大小/子节点回指/父链环，输出 `nodes=… files=… dirs=… errors=…`。

### FAT16 交换盘（`fat:/`）

- 第二块硬盘 `ata1`（`dist/fat.img`，8MB）按 FAT16 挂载，驱动器名 `fat`；
- 宿主工具 [tools/mkfat16.ps1](tools/mkfat16.ps1) 负责格式化、写文件、列目录、提取文件（不依赖 mkfs.fat）：

```
.\tools\mkfat16.ps1 -Path dist\fat.img -File HOST.TXT -Content "hello from host"
.\tools\mkfat16.ps1 -Path dist\fat.img -List
.\tools\mkfat16.ps1 -Path dist\fat.img -Extract K.TXT
```

- 内核里就是普通路径：`ls fat:/`、`cat fat:/host.txt`、`write fat:/k.txt hi`；
- 启动自检会读 `host.txt` 并打印 `fat16: host.txt = ...`；
- 限制：只支持根目录、8.3 短名（自动大写），不支持长文件名/子目录/时间戳。

shell 里的用法：

```
ushell> mkdir docs
ok
ushell> write docs/a.txt hello
ok
ushell> cat docs/a.txt
hello
```

## 块设备与持久化

`block/` 是块设备层，`ram0` 是 256KB 内存盘（512 个 512B 扇区）：

```c
typedef struct block_device {
    char name[16];
    uint32_t sector_size, sector_count;
    int (*read)(struct block_device *, uint32_t lba, void *buf);
    int (*write)(struct block_device *, uint32_t lba, const void *buf);
} block_device_t;
```

文件系统在它上面定义一个真实的磁盘格式 **XFS1**：

| 扇区 | 内容 |
|---|---|
| 0 | 超级块：magic `XFS1`、版本、扇区数、inode 数/大小、inode 表位置、数据区位置、驱动号列表 |
| 1..7 | inode 表：64 × 52 字节（used/type/parent/size/data_lba/volume/name） |
| 8..511 | 数据区（按扇区连续存放文件内容） |

- `fs_sync("ram0")`：深度优先遍历整棵树 → 写 inode 表 + 文件数据 + 超级块；
- `fs_mount_disk("ram0")`：校验超级块 → 清空内存树 → 按 inode 顺序重建（父节点编号一定在子节点之前）→ 读回文件数据；
- 启动自检会 `sync` → `fs_init()` 清空 → `mount_disk` 恢复 → 校验 `/system/bin/shell` 大小：
  `disk: remount ok (sectors=512, /system/bin/shell=13064 bytes)`。

## 系统调用

IDT 的 `0x80` 号向量是 **DPL3 中断门**（`0xEE`），用户态将来也能直接 `int 0x80`。

约定（i386 经典 ABI）：

```
eax = 调用号    ebx/ecx/edx = 参数   返回值放 eax
```

| 号 | 名称 | 参数 | 返回 |
|---|---|---|---|
| 1 | `SYS_WRITE` | ebx=buf, ecx=len | 写入字节数 |
| 2 | `SYS_GETPID` | — | 当前 pid |
| 3 | `SYS_UPTIME` | — | 运行秒数 |
| 4 | `SYS_YIELD` | — | 0（让出 CPU） |
| 5 | `SYS_EXIT` | ebx=退出码 | 不返回 |
| 6 | `SYS_OPEN` | ebx=路径, ecx=标志 | **fd**（≥3） |
| 7 | `SYS_FREAD` | ebx=fd, ecx=buf, edx=len | 读到字节数 |
| 8 | `SYS_FWRITE` | ebx=fd, ecx=buf, edx=len | 写入字节数 |
| 9 | `SYS_CLOSE` | ebx=fd | 0 |
| 10 | `SYS_GETCHAR` | — | 字符（阻塞） |
| 11 | `SYS_LIST` | ebx=路径, ecx=序号, edx=buf | 0 / -1 |
| 12 | `SYS_DRIVES` | ebx=序号, ecx=buf, edx=max | 0 / -1 |
| 13 | `SYS_MKDIR` | ebx=路径 | 0 / -1 |
| 14 | `SYS_UNLINK` | ebx=路径 | 0 / -1 |
| 15 | `SYS_STAT` | ebx=路径, ecx=buf(8B) | 0 / -1 |
| 16 | `SYS_CHDIR` | ebx=路径 | 0 / -1 |
| 17 | `SYS_GETCWD` | ebx=buf, ecx=max | 长度 / -1 |
| 18 | `SYS_FORK` | — | 子 pid；子进程返回 0 |
| 19 | `SYS_EXEC` | ebx=路径, ecx=参数串 | 不返回（成功） |
| 20 | `SYS_WAIT` | ebx=pid, ecx=&status | 子 pid / -1 |
| 21 | `SYS_INPUT_SET` | ebx=buf, ecx=len | 0 / -1（画到输入框） |
| 22 | `SYS_BRK` | ebx=增量 | 新的堆顶 / -1 |
| 23 | `SYS_KILL` | ebx=pid, ecx=sig | 0 / -1 |
| 24 | `SYS_SLEEP` | ebx=毫秒 | 0 |
| 25 | `SYS_PIPE` | ebx=&fds[2] | 0 / -1 |
| 26 | `SYS_DUP2` | ebx=旧 fd, ecx=新 fd | 新 fd / -1 |
| 27 | `SYS_REBOOT` | — | 不返回（8042 复位） |
| 28 | `SYS_PANIC` | — | 不返回（调试：触发 panic） |
| 29 | `SYS_OOPS` | — | 不返回（调试：触发内核 oops） |
| 30 | `SYS_SIGACTION` | ebx=sig, ecx=handler | 旧 handler / -1 |
| 31 | `SYS_SIGRETURN` | — | 恢复信号前现场 |
| 32 | `SYS_SYNC` | — | 0 / -1（落盘到持久化设备） |
| 33 | `SYS_TIME` | ebx=缓冲, ecx=长度 | 0（格式化 RTC 时间） |
| 34 | `SYS_FSCK` | ebx=缓冲, ecx=长度 | 错误数（缓冲里是摘要） |
| 35 | `SYS_PCI` | ebx=index, ecx=缓冲 | 0（设备信息）/ -1 |
| 36 | `SYS_POWEROFF` | — | 不返回（ACPI S5） |
| 37 | `SYS_MOUSE` | ebx=int[3] | 0（{x,y,buttons}） |
| 38 | `SYS_HANG` | — | 不返回（调试：关中断死循环） |
| 39 | `SYS_ETH` | ebx=0 信息 / 1 发测试帧 | 0 / -1 |

每个进程有自己的 fd 表（0=stdin、1=stdout、2=stderr，其余是文件），
`SYS_OPEN` 返回的是 fd 而不是全局对象句柄。

入口 `isr128`/`isr129` 与异常共用 `isr_common` 压栈布局：向量 128 转给
`native_syscall_dispatch()`，向量 129 转给旧 `syscall_dispatch()`，返回值写回现场里的
`eax` 槽位，`iret` 后就在 `eax` 中。

自检在启动时真的走两条门：

```
sys: int 0x80 native + int 0x81 legacy gates installed (DPL3)
sys: int 0x81 works
sys: write=20 uptime=0 s
native: ABI 0.1 gate ok (features=0)
native: handle table ok
```

ring3 demo 还会实际执行一次 Native ABI：

```
native: ring3 ABI 0.1 ok
```

shell 的 `sys` 命令会再调用一轮：

```
shell> sys
sys: hello via int 0x80
sys: write=24 pid=1 uptime=3 s (total calls=5)
```

**注意**：现在调用都来自 ring0 内核线程，参数是内核指针，暂时可以直接解引用。
等 P2 有用户态后，`syscall_dispatch` 必须先校验指针落在用户地址区间，否则用户
可以借 `SYS_WRITE` 读内核内存——这是接下来必须补的第一件事。

## 用户态

已经能从 ring0 真正切到 ring3 并返回：

1. GDT 增加 DPL3 的用户代码段（0x18）/ 数据段（0x20），并加载 TSS（0x28）；
2. **每个进程一套页目录**：`proc_spawn` 时复制内核映射、清空用户区 PDE，
   线程切换时装入对应 CR3——用户页只属于该进程；
3. 从 PMM 分配代码页与用户栈页，用 4KB 映射挂到 `0x40000000` / `0x401FF000`，
   **PDE 和 PTE 都带 U/S 位**；
4. **ELF32 加载器**：解析程序头，把每个 `PT_LOAD` 段按页分配物理页、复制文件内容、
   清零 `.bss`，再映射到段地址；`e_entry` 作为入口；
5. **argc/argv**：内核在用户栈上构造 cdecl 调用帧
   `[ret][argc][argv 指针]`，argv 数组与字符串放在栈页里；
   `SYS_EXEC(path, args)` 会把参数串切分后压栈；
6. `TSS.esp0` 指向当前线程内核栈，`iret` 到 ring3；
7. **mini-libc**：[src/user/ulib.c](src/user/ulib.c) 提供 `printf/puts/malloc/字符串函数`，
   输出走 `SYS_WRITE`，堆走 `SYS_BRK`（0x40080000 起按页映射）。

实测日志：

```
disk: remount ok (sectors=512, /system/bin/shell=13064 bytes)
user: entering ring3 (/system/bin/shell, entry=0x40000000 pid=1)
ushell: ready (ring3, int 0x80)
ushell: argc=0
ushell: libc ok (argc=0, ptr=0x40080000, str=libc)
ushell> echo hi there
hi there
ushell> exec /system/bin/shell foo bar
user: exec /system/bin/shell (entry=0x40000000, args='foo bar')
ushell: argc=2 argv0=foo
```

`SYS_WRITE` 会校验用户缓冲区：指针必须小于 `0xC0000000`、长度不超过 64KB，
且通过 `copy_from_user` 做可恢复拷贝——防止 ring3 把内核地址或未映射地址交给内核。

**当前限制**：只有一个用户程序映像（`/system/bin/shell`）；用户 shell 用阻塞式读键盘；
还没有 `fork/exec` 语义、没有等待队列，也没有把程序运行在文件系统任意路径的通用加载器。

## 自动测试

`make test` / `.\build.ps1 -Test` 会构建镜像、无界面启动、注入命令（默认 `help` + `fork`），然后断言：

1. 串口有输出；
2. `selftest: PASS`（内存 8 项自检全过，含 PAE 2MB 恒等映射）；
3. `obj: subsystems ready` 且 `drive: system` / `drive: data`（多卷）；
4. `demo: fs write/read ok` 且 `demo: handle type check ok`；
5. `kernel: ready`；
6. `intr: handlers irq0=timer irq1=keyboard`（IRQ 处理函数已注册）；
7. `went to sleep on`（等待队列生效，线程真的睡了）；
8. `ushell: cwd=system:` 与 `ushell: fs syscalls ok`（CWD + 文件系统系统调用）；
9. `sys: int 0x81 works` 且 `sys: write=N`（旧系统调用过渡门可用）；
10. `native: ABI 0.1 gate ok`、`native: handle table ok`、`native: ring3 ABI 0.1 ok`
    （Native ABI 入口、句柄表与 ring3 调用都可用）；
11. `user: entering ring3` 且 `hello from ring3`（用户态真的跑起来了）；
12. `sys: unmapped user pointer -> -1`（缺页被恢复，没有 panic）；
13. `thread: heartbeat N pid=M` 且计数在增长（时钟中断在推进）；
14. `ushell: ready`（ring3 shell 从 `/system/bin/shell` 载入并运行）；
15. 注入的 `help` 被执行（`ushell: commands`）；
16. 注入的 `fork` 完成子进程创建与回收（`child: hello from fork child`、`parent: waited pid N, status 3`）；
17. `disk: remount ok`（块设备存盘后重新挂载，文件完好）；
18. `ushell: argc=0`（内核按 ELF 启动约定构造 argc/argv）；
19. `ushell> echo hi` 输出 `hi`（命令行参数真的传进用户程序）；
20. `ushell: libc ok (ptr=0x402xxxxx)`（malloc 走 `SYS_BRK`、printf 走 `SYS_FWRITE`）；
21. `ushell: pipe ok sleep ok`（管道 + 睡眠系统调用）；
22. `user: ELF text RO / data RW (W^X loader)`（ELF 代码页只读、数据页可写，且拒绝 RWX 段）；
23. `user: layout code=0x40000000..0x40100000 stack=0x40100000..0x40200000 heap=0x40200000..0x40300000 tramp=0x40300000`
    （用户地址空间分区、栈 guard 与蹦床页分离）；
24. `sectest: code write pid=N status=142`（写代码页触发 #PF，子进程被终止）；
25. `sectest: guard write pid=N status=142`（写栈 guard 触发 #PF，子进程被终止）；
26. `cpu: SMEP supported=1 enabled=1`（SMEP 已启用）；
27. `cpu: SMAP supported=1 enabled=1`（SMAP 已启用，完整回归在 SMAP 下通过）；
28. `ASLR 栈/堆/蹦床`（至少两个进程的 `stack_init`、`heap_base`、`tramp` 均不同）；
29. `vmm: PAE on, NX enabled`（PAE 页表与 NX 已启用）；
30. `sectest: stack exec pid=N status=142`（执行栈地址触发 NX #PF）；
31. `sectest: rodata exec pid=N status=142`（执行 rodata 地址触发 NX #PF）；
32. `sandbox: open denied`（沙箱进程的 `SYS_OPEN` 被 allowlist 拒绝）；
33. `trust: shell signature OK`（RSA-2048/SHA-256 签名验证通过）；
34. `trust: tampered image rejected`（翻转一个字节的镜像被拒绝）；
35. `vmm: address space teardown ok`（用户页/PT/PD/PDPT 回收后 PMM 账目复原）；
36. `exec 地址空间切换`（`reexec` 后再次进入 ring3 shell）；
37. `sandbox: native denied`（native `HANDLE_DUP` 被 allowlist 拒绝）；
38. `无内核 panic`（整轮回归不出现内核 panic/canary 损坏）；
39. `pmm: linear first-fit selftest ok`（保留区不可分配、256 页唯一、释放后账目复原）；
40. `vmm: kernel W^X active`（内核 text RX、rodata R+NX、data RW+NX）；
41. `boot: KERNEL SIGNATURE OK`（loader 在跳转内核前完成 RSA 验签）；
42. `demo: fs refcount ok`（同一 fs 句柄 ref 后 close 一次仍可读）；
43. `sectest: fd write denied`（只读 fd 的写 capability 被拒绝）；
44. `madt: 2 cpu(s)` / `smp: BSP online ... AP startup pending`（发现双核）；
45. `smp: 2/2 cpus online, 1 AP GDT/TSS/IDT ready, AP ticks=... ipi=...`（AP 本地 timer/IPI 工作）；
46. `thread: heartbeat ... cpu=1`（heartbeat 由 AP 调度执行）；
47. `sectest: cap drop denied (-1)`（显式撤销 fd WRITE capability 后立即拒绝写）；
48. `sectest: cap recv hello from ramfs` / `cap xfer ... status=0`（跨进程 fd capability 转移）；
49. `cap: table selftest ok`（通用 per-process capability 表 install/transfer/close）；
50. `native: process_create/wait ok`（按注册程序名创建子进程并等待，子进程可在 AP 运行）；
51. `user: entering ring3 (builtin demo ... cpu=1)`（ring3 userdemo 在 AP 执行）。
52. `sched: pid N (spin) running on cpu 1`（负载放置：不看程序名，按 runqueue 负载把线程放到 AP）；
53. `native: handle table ok`（native handle restrict/transfer/generation 自检）；
54. `sectest: mmap ok`（匿名 mmap 映射、写读、munmap）；
55. `native: channel ok`（成对 channel endpoint 的 send/recv）；
56. `native: port ok`（Port create → connect → accept → channel 数据）；
57. channel/port 阻塞 recv/accept 已接入 `proc_block` + send/connect wakeup；
58. `native: channel block ok`（fork 子进程 sleep 后 send，父进程阻塞 recv 被唤醒）；
59. `native: service ok`（service registry register → connect by name → accept）；
60. `native: supervisor ok`（supervisor 服务查询 registry 并回复）；
61. `user: entering ring3 (ring3 AP probe ... cpu=1)` + `ring3 AP probe ok`；
62. `native: manifest ok`（supervisor 检查 supervisor + echo 服务清单）；
63. `native: handle revoke ok`（revoke 后句柄失效，close 仍可释放对象引用）；
64. `net: UDP echo reply`（QEMU 用户网络 + 宿主 PowerShell UDP 回显）；
65. `sectest: udp ok`（ring3 通过 SYS_UDP_SEND/RECV 完成 UDP echo）；
66. `sectest: socket udp ok`（socket → sendto → recvfrom → close）；
67. `svcwatch: restart echo -> pid N`（周期 watchdog 检测 echo 缺失并拉起 echosvc）；
68. `sectest: chroot ok`（per-process namespace，rooted 后 `..` 越界被拒绝）；
69. `native: restart ok`（重启后重新连接 echo 并完成数据往返）；
70. `sectest: fd limit ok`（per-process fd 上限 16，同时打开 10 个文件）；
71. `net: TCP echo reply`（TCP client：SYN/SYN-ACK/ACK/data、多连接重试、data 重传）；
72. `sectest: fs service ok`（ring3 fsd 内存文件服务，经 Port/Channel 读取）；
73. `sectest: fs write ok`（fsd 写路径：'w'+data，随后 VFS svc 读回新内容）；
74. `sectest: svc vfs ok`（VFS `svc:` 后端把 open/read 路由到 ring3 fsd）。

失败时返回非 0，可直接接到 CI。

**网络端到端自检（绕过 slirp）**：

```powershell
.\tools\net_peer_test.ps1        # 20 秒，期望 NET PEER PASS
```

它用 QEMU 的 socket 网络后端，自己扮演对端：收到 ARP 请求就回 ARP 应答、
收到 ICMP echo 就回 echo reply、收到 UDP 就原样回显，然后断言串口里出现
`net: ARP reply gw=` / `net: ICMP echo reply` / `net: UDP echo reply`。

为什么不用 `-nic user`：slirp 会替网关回 ARP/ICMP，但**不会**把 guest 发往
`10.0.2.2` 的 UDP 交给宿主监听端口，UDP 回显没法稳定断言。socket 后端完全确定、
不依赖外网。

## 压力测试 / 长稳

```powershell
.\tools\stress.ps1 -Hours 8                    # 8 小时长稳（默认 smp=2）
.\tools\stress.ps1 -Hours 0.05 -InjectEverySec 15   # 3 分钟冒烟
New-Item stress-run\STOP                        # 提前停止
```

脚本行为：串口落文件并**按字节偏移 tail**（不依赖 TCP 串口），每 60 秒用 HMP
`sendkey` 输入一条命令（`ls` / `echo` / `pwd` / `fork` / `cat`），持续扫描
`KERNEL PANIC`、`CPU exception`、`user: exception`、`Triple fault`；
串口 300 秒无增长判定为挂死。故意崩溃或挂死会写 `CRASH.txt` 并以 2 退出。

> 不要用 `tools/soak.ps1` 做多小时长跑：它的串口走 TCP，在 QEMU 这版上会中途
> 静默（guest 仍在跑），看起来像挂死。

**已跑通的一次 8 小时结果（2026-10-24 01:45–09:45，smp=2）：**

| 指标 | 数值 |
|---|---|
| 时长 | 480 分钟（准点结束） |
| 注入命令 | 478 条，478 条都有 `ushell>` 回显 |
| 心跳 | 28 559 次（≈1/s，无中断） |
| `KERNEL PANIC` / 异常 / oops / 栈 canary | 0 / 0 / 0 / 0 |
| `vmm: WARN` / 内存分配失败 | 0 / 0 |
| 崩溃文件 | 无 |

覆盖：双核调度与迁移、APIC/PIT、shell 命令循环、`fork`/`exec`/COW、文件系统读写
与落盘、capability/pipe、console 锁竞争、启动期 ARP/ICMP/UDP。
未覆盖：持续网络流量、精确的内存泄漏量化（需要前后对比 `pmm` 空闲页计数）。

## 已知限制

- **SMP 是 2 核共享内存多处理（受限）**：BSP + 1 个 AP，per-CPU GDT/TSS、共享 IDT、
  LAPIC timer、IPI/TLB shootdown、**per-CPU runqueue 与按负载放置/迁移**都已落地；
  `heartbeat` 用程序标志（`PROG_F_PIN_AP`）亲和到 AP 作为探针，其余进程由
  `sched_place()` 按负载选核，`sched_balance()` 在负载差 ≥2 时迁移 READY 进程；
- **SMP 共享状态**：对象表、管道、文件系统（ramfs 一把全局锁，公开入口统一包装）
  都已串行化；仍未逐项加锁的是进程表的部分遍历（`proc_wakeup`/`proc_find`）和
  内核页表修改路径，跨核高并发下仍有理论竞态，也没有做过压力验证；
- 文件系统在内存中 + `ata0` 硬盘镜像（`dist/disk.img`，重启保留）或 `ram0` 内存盘：
  最多 4 个驱动器、64 个节点、名字 ≤64 字符、单文件 16KB、路径 ≤255 字符；
  驱动号是保留名，顶层不能建同名目录；
- **FAT16 只支持根目录 + 8.3 短名**（无 LFN、无子目录、mtime 为 0），读写都只针对根目录文件；
- RTC 时间按 **UTC** 解释（没有时区/夏令时）；
- 用户态每个进程有独立页目录，但只有 `/system/bin/shell` 一个用户程序映像；
- 用户 shell 用阻塞式 `SYS_GETCHAR`（线程睡眠在键盘通道上）；
- 恒等映射用 2MB PAE 大页；内核映像范围拆成 4KB 页并启用内核 W^X：text RX、rodata R+NX、data/bss RW+NX；用户页和内核高地址（vmalloc、MMIO）用 4KB PTE；
- 堆是固定 16MB 区，4 字节对齐首次适配，无 `realloc`；
- 内核态异常会打印 oops + 栈回溯并杀掉当前线程；只有完全没有当前线程时才 panic；
- 信号支持 `SIG_DFL`（终止）/`SIG_IGN`/用户 handler；handler 返回后经蹦床页
  （`0x40300000`，`mov eax,31; int 0x81`）调用 `SYS_SIGRETURN` 恢复现场；
  同一时刻只保留一个信号帧，不支持信号嵌套；
- 用户地址布局：代码/数据 `0x40000000..0x40100000`，栈 `0x40100000..0x40200000`
  （向下增长，底部 4KB guard），堆 `0x40200000..0x40300000`，信号蹦床页 `0x40300000`；
- fork 用 **COW**：只对可写页建立 COW，代码页保持永久只读；物理页有引用计数；
- 通用异常表只有 8 个槽位、目前只登记了 uaccess 一段；
- 键盘只处理扫描码 set 1 的可打印键（含 Shift/CapsLock/Ctrl+C），无方向键/功能键。
