# L0 内核（plane=l0）

> 状态：WP0-2 域文档补全，2026-10-31。描述 ng 分支 **HEAD `c4efabc` 的已提交实现**。
> 上位：`../architecture.md` §2.1/§3/§5、`../decisions.md` ADR-013…020。
> 接口定形：`../../abi/abi-1.0-draft.md`。证据：`../../claims/ng-WP1-*.md`、
> `ng-WP3-0.md`、`ng-WP3-1.md`。

## 职责

L0 只提供**机制**，不提供策略：

| 类别 | 内容 | 实现位置 |
|---|---|---|
| boot / HAL | multiboot2 头 + Xen PVH note、32 位保护模式 → long mode、串口、端口 IO、TSC | `src/ng/entry.S`、`io.h`、`serial.c` |
| CPU / arch | GDT/TSS/IST、256 项 IDT、#DF 独立栈、syscall MSR（STAR/LSTAR/FMASK） | `gdt.c`、`isr.S`、`trap.c`、`syscall.c` |
| 内存 | PVH/multiboot2 内存图、1 GiB PMM 双位图、first-fit heap、4 级页表、W^X/NX/SMEP/SMAP | `memmap.c`、`pmm.c`、`heap.c`、`paging.c` |
| 地址空间 | 每进程独立 PML4、用户窗口 `<512 GiB`、16 KiB 用户栈 + 守护页、CR3 切换 | `useras.c`、`elf.c`、`uaccess.c` |
| 执行 | 16 槽 TCB、16 KiB 内核栈、协作式 round-robin、TSC sleep/wake、延迟回收 | `thread.h`、`sched.c`、`switch.S` |
| 进程机制 | 32 槽进程表、pid 单调、spawn/wait/kill/exit、每进程 64 槽句柄表 | `proc.h`、`proc_table.c`、`proc.c` |
| IPC 机制 | 16 个端口、16 B 消息头、payload ≤ 4 KiB、ticket、阻塞/超时/close 唤醒 | `handle.c`、`ipc_core.c`、`ipc.c` |
| 数据面机制 | 8 个共享内存对象（≤1024 页/对象）、LFB 用户映射 | `shm_core.c`、`shm.c`、`fbmap.c` |
| 故障 | #PF/#UD/#GP 诊断、受控恢复探测、panic | `trap.c`、`isr.S` |

## 不做什么

1. **文件语义**：路径、目录项、权限位在 L1（`src/services/fs`）；VFS 暂以库链接进内核，
   但 L0 不定义“文件是什么”（architecture §5 不变量 1）。
2. **网络协议、账户策略、日志格式、包清单、桌面策略、manifest 解析**：分别归 L1/L2/A。
3. **设备驱动**：PCI/VBE/fb/PS2/ATA 在 `src/drivers`（L1D），L0 只做 PCI/MMIO 映射、
   端口 IO 注入和 syscall 胶水（`wp23/24_selftest.c`、`ata_blk.c`）。
4. **抢占/时间片/SMP/优先级**：协作式；`sched_tick()` 只做到期唤醒，IRQ 未接。
5. **用户态异常投递**：ring3 的 #PF/#GP 仍 panic，不杀进程（D06）。
6. **capability**：没有 `cap_grant/revoke`；端口 ticket 是“知道就能发”，`from` 由发送方填。
7. **按需分页/COW/mmap/mprotect、信号、fork、动态链接、POSIX**：D0 不做。
8. **IRQ/MMIO/DMA 授权 syscall**：旧树 hwcap 89–92 没有迁到 ng（D09）。
9. **watchdog**：panic 只 `cli; hlt`，没有自动重启（D07）。

## 接口（syscall / IPC / ABI）

### syscall 分组

约定：`rax` = 调用号，`rdi/rsi/rdx/r10` = 参数，`rax` 返回；负值 = `-errno`。
`syscall` 指令破坏 `RCX/R11`；内核用 FMASK 清 TF/IF/DF/AC，并切到当前线程的
内核栈（TSS.RSP0 / `syscall_kernel_rsp0`）。ABI 只承诺 `rax`，其余寄存器当前实现
会保存/恢复，但调用方不应依赖。

| 号 | 名称 | 参数 | L0 角色 |
|---|---|---|---|
| 0 | `write` | `buf, len`（`len` 1..256） | 串口控制台；`buf=0` 保留 WP1-3 诊断模式 |
| 1 | `exit` | `code` | ring3 回到内核恢复点；进程模型下置退出状态 |
| 10 | `port_create` | `depth` | 端口/句柄机制；默认 16、上限 64、池 16 |
| 11 | `port_send` | `ticket, msg, len, timeout` | 有界队列 + 阻塞/超时 |
| 12 | `port_recv` | `ticket, buf, max, timeout` | 返回消息总长；可截断 |
| 13 | `port_reply` | `ticket, msg, len, timeout` | 与 send 共用核心，交叉校验 ticket |
| 14 | `port_close` | `ticket` | 仅属主；唤醒等待者 |
| 20–27 | `open/read/write/close/stat/listdir/mkdir/unlink` | 见 ABI 草案 | **L0 分发 + L1 语义**；用户指针走 uaccess |
| 30 | `readkey` | — | L0 分发到 L1D PS/2 事件环；非阻塞 |
| 31–35 | `login/dmesg/log_write/pkg_list/pkg_verify` | 见 ABI 草案 | **L0 分发 + L1 语义** |
| 40 | `spawn` | `path, argv, &pid` | 读 VFS ELF → 新 AS → 新线程 → ring3 |
| 41 | `wait` | `pid, &status` | 只等直接子进程；阻塞 |
| 42 | `getpid` | — | 当前 pid |
| 43 | `kill` | `pid, sig` | 摘 IPC 等待者/线程、回收 AS；状态 `0x100\|sig` |
| 44 | `getppid` | — | 父 pid；init 为 0 |
| 45 | `shm_alloc` | `pages, &va, &handle` | 共享页 + 首映射；`pages` 1..1024 |
| 46 | `shm_map` | `handle, &va` | owner/root 校验；每对象最多 8 条映射 |
| 47 | `fb_map` | `&va, &info` | root-only；固定 VA `0x60000000`，PCD/PWT/NX |

调用号 2–9、15–19、28–29、36–39、48+ **未分配**，按 ABI 草案“只追加、不复用”处理。
旧树 `abi.h` 的 1..95 是另一套入口，不在此表（D11）。

### IPC（控制面）

- 消息 = 16 B 头 `{u32 type; u32 len; u32 from; u32 flags}` + payload；
  `len` 参数是**含头的总长度**，合法范围 16..4112；`hdr.len` ≤ 4096；`flags` 必须 0。
- **handle**（进程私有引用）与 **ticket**（端口全局身份）都是 `(gen<<16)|slot`，
  但命名空间不同：`send/reply` 凭 ticket，`recv/close` 还要求调用进程 == 端口 owner pid。
- timeout 单位是 **TSC tick**（0 = 非阻塞 `EAGAIN`，`~0` = 永久，其余 = 上限）；
  未校准，ABI 冻结前不能假设毫秒。
- 错误：`EPERM/ENOENT/EBADF/EAGAIN/ENOMEM/EFAULT/EINVAL/ENOSPC/EMSGSIZE/
  ETIMEDOUT/ECANCELED`，数值见 ABI 草案 §5。
- 详见 `../ipc-ng.md`、`../proc-ng.md`。

### 用户指针 / ABI

- 用户地址窗口：`[0x40000000, 0x7FFFFFFFFF]`（ELF 段），用户栈
  `0x40100000..0x40104000`（守护页 `0x400FF000` 不映射），标记页 `0x40010000`，
  SHM `0x50000000..0x5F000000`，LFB `0x60000000`。
- `uaccess.c` 逐页校验 P=1、U=1（写还要 W=1），SMAP=on 时 `stac/clac`；坏指针
  返回 `EFAULT`，绝不让内核吃 #PF。路径/缓冲先整体校验再拷贝。
- 进程入口：`rdi=argc`、`rsi=argv`（用户栈上的 `char*[]`，NULL 结尾）；退出码取低 8 位。
- 内核内部接口按“纯状态机 + 胶水”分层，host 单测直接驱动纯状态机：
  `proc_table`、`handle/ipc_core`、`sched_core`、`pmm/heap`、`elf64_check/load`、
  `shm_core`、`paging`。
- 完整 syscall 表、句柄/错误码/对齐/版本策略见
  [`../../abi/abi-1.0-draft.md`](../../abi/abi-1.0-draft.md)。

## 失败等级与恢复

| 项 | 设计等级 | 当前行为 | 证据/限制 |
|---|---|---|---|
| 内核代码/数据结构损坏 | fatal | panic：打印 `PANIC: vec/err/rip/cr2`，`cli; hlt` | `trap-syscall-ng.md` §5；**无 watchdog**（D07） |
| 内核 #PF / 坏指针 | fatal | 未命中受控探测即 panic；`uaccess` 先校验，正常路径不应发生 | `trap-syscall-ng.md` §4 |
| ring3 #PF/#GP | 设计 isolated | **panic**，不杀进程 | D06；WP3-5 处理 |
| PMM/heap 耗尽 | 可恢复错误 | 分配返回 0 / `ENOMEM`，调用方回滚；不会 panic | `memory-ng.md` §2/§3 |
| ELF 坏镜像 | 可恢复错误 | `elf64_check/load` 拒绝并回滚；`spawn` 映射成 `EINVAL` | `elf-ng.md` §3/§4 |
| 端口/共享内存耗尽 | 可恢复错误 | `ENOSPC`/`ENOMEM`；不 panic | `ipc-ng.md` §2.3、`shm.h` |
| 设备超时/错误 | 可恢复错误 | ATA 有界轮询返回 `EIO/ETIMEDOUT`；调用方降级 | `block-ng.md` §2.3 |
| 用户进程退出/被 kill | isolated | 回收 AS/句柄/端口/共享页；父进程 `wait` 收尸 | `proc-ng.md` §7 |

S1 的“杀 shell 应用不死”只在 **WM/displayd 被 kill、Demo 继续跑** 这一层验证过；
输入恢复/面板重建归 WP3-5（D16）。

## manifest 五元组

当前 L0 没有独立 manifest；内核是单一镜像，不能按组件重启。下表是**设计约定**，
不是已实现字段（见 `_index.md` §2、D04）：

| 组件 | plane | criticality | fallback | health | restart |
|---|---|---|---|---|---|
| boot / kmain | `l0` | `fatal` | `none` | `none` | `never` |
| mm（pmm/heap/paging） | `l0` | `fatal` | `none` | `panic` | `never` |
| sched / thread | `l0` | `fatal` | `none` | `panic` | `never` |
| ipc / handle / proc | `l0` | `fatal` | `none` | `panic` | `never` |
| shm / fbmap（机制） | `l0` | `fatal` | `none` | `panic` | `never` |
| trap / panic | `l0` | `fatal` | `none` | `panic` | `never` |

规则：L0 组件只能是 `fatal/none`；`fallback` 不能指向 L1/L2（依赖只能向下）。
真正落地要等组件 manifest v2 解析器（D04）。

## 当前实现状态（ng WP）

| WP | 内容 | 状态/证据 |
|---|---|---|
| WP1-1 | multiboot2 + PVH note + long mode + COM1 | `../../claims/ng-WP1-1.md` |
| WP1-2 | 4 级页表、W^X/NX、SMEP/SMAP、stac/clac 约定 | `../../claims/ng-WP1-2.md` |
| WP1-3 | IDT/TSS/IST、可恢复 #PF/#UD/#GP、syscall ring0/ring3 | `../../claims/ng-WP1-3.md` |
| WP1-4 | PVH/multiboot2 内存图、PMM、heap、页表池 | `../../claims/ng-WP1-4.md` |
| WP1-5 | TCB、switch_context、round-robin、TSC sleep/wake | `../../claims/ng-WP1-5.md` |
| WP1-6 | 句柄表、16 端口、阻塞/超时/close、ticket/reply | `../../claims/ng-WP1-6.md` |
| WP1-7 | ELF64 校验/加载、独立 PML4、uaccess、per-thread 栈 | `../../claims/ng-WP1-7.md` |
| WP1-8 | E1 host 单测/CI 收口 | `../../claims/ng-WP1-8.md` |
| WP3-0 | 32 槽进程表、spawn/wait/kill、每进程句柄、跨进程 ticket | `../../claims/ng-WP3-0.md` |
| WP3-1 | syscall 45/46/47、共享页 refcount、LFB 用户映射 | `../../claims/ng-WP3-1.md` |

已提交的 tag：`ng-e1` = `69d84ca`、`ng-e2` = `d3a5671`。ng 不修改
`src/kernel/**` 与旧 `build.ps1`；旧 32 位树作为行为 oracle。

## 待办

1. **用户态 fault → 杀进程**（#PF/#GP），这是 L3 isolated 成立的前提（D06，WP3-5）。
2. **抢占/tick IRQ**：接 PIC/APIC/PIT；让 `kill` 能终止 ring3 死循环。
3. **capability**：`cap_grant/revoke`、ticket 防伪（内核填 `from` 或 cap 校验），配合 ABI 1.0。
4. **watchdog**：panic 后自动重启或至少记录到持久介质（D07）。
5. **页表/AS 回收**：目前 `as_destroy` 会回收用户页；页表子树、COW 引用计数仍缺。
6. **hwcap**：ng 版 IRQ/MMIO/DMA 授权 syscall（旧树 89–92 不适用，D09）。
7. **SMP 与锁**：当前全局 sched/heap/ipc 假定单核 + IF=0。
8. **ABI 1.0 头文件**：与 WP4-1 一起生成 `sdk/include/aster/abi-1.0.h`（D12）。
9. **性能**：ASID/PCID、TLB 刷新策略、IPC 批量/共享内存扩容（D15）。
10. **时间**：TSC 校准/时钟源，为 ABI 提供可移植的 timeout 单位。
