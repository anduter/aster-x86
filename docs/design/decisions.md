# 架构决策记录（ADR）

> 格式：决策 / 理由 / 影响 / 替代方案。状态默认 accepted；变更需追加新 ADR，不直接改旧结论。

## ADR-001：采用 7 大类（5 运行时 + 2 横切）

- **决策**：L0 内核、L1 系统服务、L1D 设备服务、L2 桌面会话、L3 应用；横切 A 平台契约、B 生命周期与运维。
- **理由**：按信任边界、故障半径、变更节奏分类，避免把“壁纸”和“文件系统”混成一个服务层。
- **影响**：目录、CI 依赖检查、manifest、测试矩阵都按域组织。
- **替代**：三层（内核/服务/程序）——保留为故障等级视图。

## ADR-002：演进，不整体重写

- **决策**：main 冻结，开 ng 分支；重写底层、移植中层、新写顶层。
- **理由**：39k 行里 FS/网络/驱动/测试是资产；整体重写要数月到一年才回到 today，并丢掉 305 项行为契约。
- **影响**：旧树做 parity oracle；每个 WP 有出口测试。
- **替代**：greenfield 重写——仅在换语言/换 ABI 模型时重新评估。

## ADR-003：64 位先行，ABI 1.0 暂缓冻结

- **决策**：D0 第一优先是 64 位内核；ABI 1.0 在 64 位 + SDK 之后再冻结。
- **理由**：桌面应用默认 64 位；先冻 ABI 再改位数会冻两次。
- **影响**：32 位树只做参考；新工具链、ELF64、syscall 指令。
- **替代**：保持 32 位——只能做复古/教育定位，不做通用桌面。

## ADR-004：native ABI 1.0 + 0x81 冻结为 compat

- **决策**：内核只讲 native（句柄/cap/64 位）；0x81 旧表冻结，只修 bug。
- **理由**：保留现有 POSIX 形状的 syscall 作为兼容面，但不再扩展，避免三套 ABI。
- **影响**：POSIX Profile 可先建在 0x81 上；新服务用 native。
- **替代**：把 POSIX 塞进内核——污染 TCB，否决。

## ADR-005：POSIX 在 D1，用户态实现

- **决策**：POSIX Profile 1.0 在 D1；libc + shim 在用户态，内核不实现 POSIX。
- **理由**：源码兼容是 80% 的生态收益；内核保持中性。
- **影响**：D0 只用 native SDK 验收；POSIX 不拖 64 位。
- **替代**：内核 POSIX（Linux 式）——否决；二进制兼容——暂不规划。

## ADR-006：L2 拆三个重启域，S1 先做

- **决策**：display server / shell-WM / session manager 分开；D0 只做 S1（杀 shell 应用不死）。
- **理由**：L2 是单点；应用不能是 shell 的子进程。
- **影响**：surface/焦点/输入路由归 display server；应用生命周期归 session manager。
- **替代**：Wayland 式单体合成器——合成器崩会话死，否决。

## ADR-007：P5/P6 驱动迁移冻结到 D1

- **决策**：blkd/inputd/netd ring3 迁移暂停；hwcap（89–92）保留。
- **理由**：驱动隔离不是 D0 关键路径；先 64 位 + 桌面会话 + SDK。
- **影响**：驱动先 ring0；D1 有硬件目标后再迁。
- **替代**：现在做——会推迟 D0 6–12 个月，否决。

## ADR-008：浏览器 B0 长线，不作为 D0 出口

- **决策**：自有引擎按 NetSurf/Ladybird 尺度；B0 先做无 JS 的 HTML/CSS 子集，D0 通过前不做 B1。
- **理由**：浏览器是 D1 的生死门，但不是 D0 的 blocker。
- **影响**：浏览器并行；依赖 display server/字体/网络。
- **替代**：移植 WebKit——D1 再评估；不自研 JIT。

## ADR-009：D0 启动路径用 QEMU multiboot2，不迁 16 位 bootloader

- **决策**：ng 用 multiboot2（或 UEFI stub）直接进 64 位；旧 MBR/二阶段加载器不移植。
- **理由**：16 位 bootloader 是历史资产，不是桌面关键路径。
- **影响**：真实硬件/UEFI 到 D1；旧启动链保留在 main。
- **替代**：移植旧 bootloader——耗时且无桌面收益，否决。

## ADR-010：域依赖检查 + 扩展 manifest

- **决策**：check-domains 进 CI；manifest 增加 plane/criticality/fallback/health。
- **理由**：架构边界必须机械化，不能靠自觉；故障策略必须可声明。
- **影响**：新增组件必须带五元组；optional 不得被 core 同步依赖。
- **替代**：文档约定——会腐烂，否决。

## ADR-011：控制面消息，数据面共享内存

- **决策**：IPC 消息只用于控制；帧/音频/大文件走共享内存或批量通道。
- **理由**：4KB 消息传不了桌面数据；合成器和浏览器都需要零拷贝。
- **影响**：IPC 底座要加共享内存/句柄传递；display server 协议从第一天按此设计。
- **替代**：全部走消息——性能和实现都不可行，否决。

## ADR-012：SDK 是一等公民

- **决策**：D0 出口包含“外部开发者一天内写出 GUI 应用”；SDK/文档/示例与内核同优先级。
- **理由**：通用桌面的瓶颈是生态，不是 syscall 数量。
- **影响**：ABI 文档、工具链、示例、调试器进 D0 范围。
- **替代**：先做内核再补 SDK——会做出“只有作者能开发”的系统，否决。

## ADR-013：QEMU 上 ELF64 内核用 Xen PVH note 启动，镜像保留 multiboot2 头

- **决策**：ng 内核仍是带 multiboot2 头的 ELF64；QEMU 的 `-kernel` 路径额外加一个 Xen PVH note（`XEN_ELFNOTE_PHYS32_ENTRY` = 18，指向同一个 32 位 `_start`）。
- **理由**：QEMU 11 的 multiboot 加载器只接受 32 位 ELF。WP1-1 实测：带 MB1 头的 ELF64 报 `Cannot load x86-64 image, give a 32bit one.`，带 MB2 头的 ELF64 走 Linux 加载器并报 `Error loading uncompressed kernel without PVH ELF Note`。PVH 是 QEMU 加载 ELF64 的既有路径，进入状态与 multiboot 相同：32 位保护模式、分页关闭、平坦段；镜像仍保留 MB2 头给 GRUB。
- **影响**：`src/ng/entry.S` 同时带两种头，`tools/build-ng.ps1` 校验两者；`tools/run-ng.ps1` 仍用 `qemu-system-x86_64 -kernel ...`。GRUB/multiboot2 实机路径 WP1-1 未实测。
- **替代**：改用 GRUB + `-cdrom` 启动——偏离 WP1-1 的 `-kernel` 出口；改用 ELF32 包装器——与“产出 ELF64”出口冲突；均否决。

## ADR-014：内核页表用 4 KiB 段页 + 2 MiB 大页混合；SMAP 走 stac/clac 窗口

- **决策**：ng 内核恒等映射 1 GiB；`[0, 2 MiB)` 用 4 KiB 页，按链接脚本的段边界分别给 `.text` R-X、`.rodata` RO+NX、`.data/.bss` RW+NX；`[2 MiB, 1 GiB)` 用 2 MiB 大页 RW+NX。页表页从 `.bss` 静态池分配（WP1-4 前无 PMM）。`EFER.NXE` 在开分页前置位，`CR0.WP=1`，`CR4.SMEP/SMAP` 由 CPUID 决定。ring0 访问用户内存统一走 `stac/clac` 窗口（范围校验 + 成对 + 异常入口 `clac` + syscall `SFMASK` 清 AC）。
- **理由**：段权限必须先有页级粒度，否则 `.rodata` 可写、`.text` 可写就谈不上 W^X；而全 1 GiB 都拆 4 KiB 既浪费页表又没必要。`.bss` 静态池让 WP1-2 不依赖 PMM，边界干净。SMAP 下不按约定就会随机 #PF，必须把约定前置到 ABI/文档。
- **影响**：`src/ng/paging.c` 是唯一建表/改权限的地方；`link.ld` 增加段边界符号与 `__kernel_end <= 2 MiB` 断言；`docs/design/paging-ng.md` 记录布局、标志位与 stac/clac 约定；WP1-3 的 `copyin/copyout`、#PF 入口与 syscall `SFMASK` 按该约定实现。
- **替代**：全 2 MiB 大页——无法按段设权限（W^X 不成立）；页表页走 PMM——把 WP1-4 依赖提前；SMAP 关闭——放弃保护，且与出口冲突；均否决。

## ADR-015：trap/syscall 用 IDT + TSS/IST；ring3 用 iret/sysret，ring0 测试返回用直接跳转

- **决策**：IDT 固定 256 项 64 位中断门（0x8E，selector 0x08）；异常 0–31 各一个 stub，有 error code 的 8 个占 10 个向量不补假 error，其余补 0；`#DF` 的 IDT 项 `IST=1`，TSS.IST1 单独 8 KiB 栈，TSS.RSP0 给 ring3→ring0 入口。GDT 7 槽：`0x08` 内核代码、`0x10` 内核数据、`0x18` 用户数据、`0x20` 用户代码、`0x28` TSS；用户数据必须在用户代码前一个槽（SYSRET 的 `CS=base+16`、`SS=base+8` 硬约束）。syscall 用 `EFER.SCE` + STAR/LSTAR/FMASK，ABI 草案：`rax` 调用号、`rdi/rsi/rdx` 参数、返回值 `rax`，本 WP 只实现 0=write serial、1=exit test。ring3 最小往返：用户页 U/S=1（代码 R-X、栈 RW+NX）、`iretq` 进 CPL3、`syscall` 进内核、`sysretq` 回用户、第二次 syscall 经内核恢复点退出。ring0 往返测试的返回用 `popfq + jmp`，不用同 CPL `iretq`。
- **理由**：可恢复异常是证明 W^X 真的生效的唯一硬证据（WP1-2 只能读回页表）；#DF 没有独立栈就一定三重故障，TSS/IST 是 64 位内核的底线配置。SYSRET 的架构语义只允许返回 CPL3，ring0 测试不该依赖它；本机 QEMU 对同 CPL `iretq` 实测也弹 5 项，绕开这条非必要路径更稳。SYSCALL 是 64 位桌面 ABI 的主入口，先把 MSR/栈切换/返回路径打通，参数与错误码留给 WP1-7/WP4-1 定形。
- **影响**：`src/ng/isr.S`/`gdt.c`/`trap.c`/`syscall.c` 是新的 trap/syscall 域；`pg_map` 增加 `PG_USER` 逐级 U 位传播；异常入口按 `CR4.SMAP` 决定是否 `clac`（SMAP=off 时 CLAC 是 #UD），syscall 用 `FMASK` 清 AC；`run-ng.ps1` 增加 12 条期望行 + 3 条正则与 `-Panic` 负向模式，`build-ng.ps1` 增加 `-PanicTest`。
- **替代**：`int 0x80` 软中断入口——慢且与旧 32 位 ABI 纠缠，否决；ring0 返回也用 SYSRET——只能回 CPL3，不可行；ring3 延后到 WP1-7——本 WP 已用静态用户页 + 机器码 blob 打通，不做 ELF 即可，故不延后；#DF 不做 IST——任何栈故障都三重故障，无法定位，否决。

## ADR-016：PMM 用双位图 + 1 GiB 窗口；heap first-fit 合并；页表池改从 PMM 分配

- **决策**：WP1-4 起 `src/ng` 自带内存管理。内存图优先解析 QEMU `-kernel` 的
  PVH `hvm_start_info`（magic `0x336EC578`，memmap 24 字节项，type 1=RAM），
  保留 multiboot2 mmap 解析作为结构校验 fallback。PMM 管理 `[0, 1 GiB)`，
  两份 32 KiB 静态位图（used / reserved）放 `.bss`；页号相对窗口 base，分配
  first-fit + rover，默认清零。保留区固定为低 1 MiB、内核镜像
  `[__image_start, __kernel_end)`、PVH start_info/memmap。释放路径先校验
  对齐/窗口/reserved/used，double-free、保留页、越界、未对齐分别返回
  `PMM_EDOUBLE/ERESERVED/ERANGE/EINVAL` 并记账。heap 由连续 PMM 页组成
  region（≥16 页，最多 16 个），region 内 48 字节块头 + first-fit + 同 region
  相邻空闲块合并；`kmalloc/kfree/krealloc/kmalloc_aligned`，野指针/重复释放
  拒绝并记账。WP1-2 的 `pt_pool[24]` 删除，`pg_alloc_table()` 每页调用
  `pmm_alloc_page()`。
- **理由**：启动到 `kmain` 时 EBX 就是 PVH start_info（WP1-1 已实测），
  `0x336EC578` 是唯一可靠识别；QEMU 的 memmap 与 multiboot2 只差字段布局，
  统一成 `ng_memrange` 后 PMM 不需要知道来源。位图放静态 `.bss` 避免“位图页
  由 PMM 自己分配再清零”的 bootstrap 环；1 GiB 窗口与现有恒等映射一致，
  超过窗口的 RAM 即使标记可用也无映射，强行分配只会 #PF。heap 用 region
  而不是单个连续 arena：PMM 连续页在碎片化后不一定够，多 region 让扩堆更稳；
  只在 region 内合并使 `kfree()` 的边界校验简单、不会被“跨 region 块”绕过。
  页表池动态化是 WP1-2 遗留项，继续用静态池会让 PMM 自检与页表自检互相
  掩盖“到底谁在管物理页”。
- **影响**：`src/ng/{memmap,pmm,heap,mem_selftest}.c` 新增；`paging.c` 的
  `pt_alloc` 改为 `pg_alloc_table`，证据行从 `pool=24 (4-level, .bss)` 改为
  `tables=N (4-level, pmm)`；`main.c` 顺序变为内存图/PMM → 页表 → trap/syscall
  → heap/pgtbl；`build-ng.ps1` 增加 4 个 C 源；`run-ng.ps1` 增加 6 条固定行 +
  1 条 memmap 正则；新增 `tests/unit-ng/` 与 `tools/unit-ng.ps1`（166 checks）。
  `docs/design/memory-ng.md` 记录布局/语义；旧 32 位树完全不改。
- **替代**：位图放第一个可用页（省 64 KiB `.bss`）——需要 bootstrap 特判，
  且坏位图会直接毁掉整个 PMM，否决；空闲链表 PMM——单页/连续页都要额外
  元数据，位图更简单；heap 用一个固定大 arena——扩堆受连续物理页限制，
  碎片化时失败，否决；页表池继续静态——WP1-2 已明确列为 WP1-4 遗留，且无法
  回答“PMM 到底管不管页表页”，否决。

## ADR-017：线程用 TCB 静态池 + heap 16 KiB 栈；`switch_context` 保存 rsp/rip 用 `jmp` 恢复；协作式 round-robin + TSC 睡眠

- **决策**：WP1-5 的线程模型是 `thread_t`（tid/状态/上下文/16 KiB 栈/
  wake_tick/链指针），TCB 放 16 槽静态池（0 号槽是 boot 线程，不回收），
  栈从 heap `kmalloc`。`switch.S` 的 `switch_context(old,new)` 只保存
  callee-saved（rbx/rbp/r12–r15）+ rsp + rip：rsp 存 `leaq 8(%rsp)`（ret 之后），
  rip 存返回地址，恢复时装载 rsp 后 `jmp *rip`，不伪造栈帧。新线程
  `ctx.rip = thread_entry`、`ctx.rsp = stack_top`，trampoline `cld` +
  `andq $-16,%rsp` + `call thread_bootstrap`（跑 `start(arg)` 后
  `thread_exit`）。调度器是单核协作式：FIFO ready 队列，pick 队头 / yield
  放队尾 = round-robin；`sched_start()` 建 idle 线程并第一次让出；`thread_exit`
  只标 DEAD 挂 zombie 链，由下一次别人调度时 `sched_core_reap(skip=current)`
  回收栈（不能在自己正在用的栈上 `kfree`）；`thread_sleep(ticks)` 用
  `rdtsc + ticks` 作 deadline，`sched_core_poll(now)` 在每次调度点/未来的
  `sched_tick()` 里把到期线程搬回 ready；`thread_wake(tid)` 显式唤醒并校验
  返回 `THREAD_EBAD/ENOENT`。栈底 16 字节 canary，退出时校验。
- **理由**：协作式是没有任何 IRQ 基础设施（PIC/APIC/PIT 都还没接）时的
  正确最小步，接口用 `sched_tick()` 为 WP1-6 的 tick 抢占留口。rsp 存
  “ret 之后”的值 + `jmp` 恢复，比“push 返回地址再 iretq”少一层伪造，
  对齐在数学上就是 SysV 的原状态，首次上下文不需要额外调整。TCB 静态池
  避免“线程退出时释放 TCB 再用到它”的窗口，16 槽对当前自检/服务启动足够；
  栈用 heap 是因为 WP1-4 已经把连续页/合并/记账都做好，且每次 16 KiB
  不超过单个 region（≥64 KiB）。延迟回收把“正在用的栈”排除在 `kfree`
  之外，是最简单且可证明安全的顺序。守护页需要把 2 MiB 恒等大页拆成
  4 KiB 页，属于页表工作，本 WP 用 canary 降到最小。
- **影响**：`src/ng/thread.h`、`sched.c`、`sched_arch.[ch]`、`switch.S`、
  `sched_selftest.[ch]` 新增；`main.c` 在 WP1-4 之后跑 WP1-5 自检；
  `build-ng.ps1` 变 17 步；`run-ng.ps1` 增加 6 条固定行 + 1 条线程交错正则 +
  sched 失败模式；`tools/unit-ng.ps1` 增加 `sched.c`/`test_sched.c`，
  host 单测 166 → 390 checks；`docs/design/sched-ng.md` 记录设计。
  旧 32 位树完全不改。
- **替代**：抢占式 IRQ 调度提前到本 WP——没有 PIT/PIC/APIC 的 ng 树里无法
  验证 tick，且会与 WP1-6 的 IPC 等待队列纠缠，否决；每次 `thread_create`
  都 `kmalloc` TCB——需要线程安全回收和“退出后不能引用 TCB”的约束，静态池
  更稳；exit 时立刻 `kfree` 自己的栈——在正在使用的栈上释放，必崩，否决；
  用 `iretq` 构造首帧——要伪造 SS/RSP/RFLAGS 三件套且容易对错齐，`jmp +
  call` trampoline 只依赖 C ABI，否决；守护页——需拆 2 MiB 映射，超出
  WP1-5 范围，先 canary。

## ADR-018：IPC 用「句柄表(ticket) + 有界端口队列 + 静态等待者池」；阻塞走 `thread_sleep/thread_wake`，close 延迟回收

- **决策**：WP1-6 的控制面 IPC 由三层组成：`handle.[ch]` 的句柄表（64 槽，
  `handle = (generation<<16)|slot`，类型 PORT/THREAD/FILE，close 递增
  generation 跳过 0）、`ipc_core.c` 的端口状态机（16 个端口，`depth` 默认 16
  上限 64，消息头固定 16B `{type,len,from,flags}` + payload ≤4KiB，FIFO 环形
  队列）、`ipc.c` 的内核胶水（`struct ipc_env` 6 个钩子：TSC/`thread_sleep`/
  `thread_wake`/kmalloc/kfree/tid，加上用户指针页级 U/S 校验与 SMAP
  `stac/clac` copyin/out）。**ticket 与端口句柄同构**：`(port_generation<<16)|
  slot` 就是 `port_create` 的返回值，客户端把它放进消息头 `from` 交给服务端，
  `reply(ticket)` 解析后再交叉校验 `port->slot/gen`。阻塞用静态等待者池
  （12 条，recv/send 两条 FIFO 链，`notified` 位保证分醒不同等待者），
  `thread_sleep` 阻塞、`thread_wake` 显式唤醒；超时单位是 TSC tick（0=非阻塞
  `EAGAIN`，~0=永久，其余 `ETIMEDOUT`）。close 先失效句柄、清队列、唤醒全部
  等待者（`ECANCELED`）；有等待者时端口延迟到等待者退出后的下一次 IPC 操作
  才回收。syscall 号 10–14 定义在 `src/ng/syscall.h`，用户指针先 `pg_walk`
  校验 P/U（recv 还要 W）再拷贝。
- **理由**：单进程 + 单句柄表下，ticket=句柄是最少系统调用、最少状态的方案，
  generation 已经能挡住 close 后旧 ticket；`reply` 的 slot/gen 交叉校验为将来
  跨进程 cap 表留了挂点。静态等待者池让阻塞路径零分配（分配失败可回退），
  池满返回 `EAGAIN` 而不是死等；延迟回收避免“唤醒别人后立刻 free 别人还在
  读的端口对象”。超时复用 WP1-5 的 TSC deadline，不引入频率校准。单核、
  IF=0、显式调度点下无锁是安全的，与调度器/堆现状一致。
- **影响**：`src/ng/handle.[ch]`、`ipc.[ch]`、`ipc_core.c`、`ipc_selftest.[ch]`
  新增；`syscall.[ch]` 加 10–14；`main.c` 加 `ipc_init()` 与 WP1-6 自检；
  `build-ng.ps1` 17→21 步；`run-ng.ps1` 加 9 条固定行 + ipc 统计正则 + 失败
  模式；`tools/unit-ng.ps1` 加 `handle.c`/`ipc_core.c`/`test_ipc.c`，host 单测
  390→810 checks；`docs/design/ipc-ng.md` 记录设计。旧 32 位树完全不改。
- **替代**：让内核自动填 `from`（每线程默认回复端口）——新增进程级概念，
  超出 WP1-6，先由发送方填、内核校验存活；ticket 用独立命名空间
  `(port_slot<<16)|port_gen`——客户端拿到 ticket 还需要额外系统调用，
  单进程下没有收益；同时保留句柄表 slot 与端口槽两套编号——多一层映射且
  自检/单测更难构造，否决；close 立即 `free(ring)`——被唤醒的等待者还要读
  `closed`，会 use-after-free，否决；等待者动态分配——阻塞失败路径无法
  干净回退，否决；超时用毫秒——没有 TSC 频率校准，先 tick。

## ADR-019：ELF64 用「纯校验 + env 回调加载」两层；用户地址空间独立 PML4；copyin/copyout 统一；per-thread TSS.RSP0

- **决策**：WP1-7 的加载器拆成两层。`elf.c` 的 `elf64_check()` 只读镜像字节，
  产出 `struct elf64_image`（entry/base/end/段表/pages），不 include 页表/PMM/
  串口；`elf64_load()` 用 `struct elf_env` 的 5 个回调（alloc_page / free_page /
  page_ptr / map_page / unmap_page）逐段映射。内核对 env 的实现绑到 PMM +
  用户地址空间；host 单测绑到 fake 页池，因此同一份校验/回滚代码在两边跑。
  校验是拒绝式的：ELF64/小端/ET_EXEC/EM_X86_64、e_phoff 8 字节对齐且 phdr 表
  在文件内、1≤phnum≤16、每个 PT_LOAD 的 filesz≤memsz 且文件区间在镜像内、
  vaddr 4 KiB 对齐、p_align 是 ≥4096 的 2 的幂、段在
  `[0x40000000, 0x7FFFFFFFFF]` 内、PF_W 与 PF_X 不同时置位（W^X）、段两两不
  重叠、entry 落在可执行段内。加载时整页清零再拷文件字节（同时覆盖 .bss 与
  页尾空洞）。用户地址空间用**新建 PML4**：PML4[0] 指向新建 PDPT，PDPT[0] 复用
  内核 PD（0..1 GiB 恒等映射，U=0），PDPT[1..511] 留给用户页，PML4[1..255]
  复制内核同名项；切 CR3 用 `pg_switch_pml4()`（同时记录 current_pml4），
  copyin/copyout 用**当前 CR3** 的 PML4 逐页校验 P/U/W，SMAP=on 时
  stac/clac 包住拷贝，坏指针返回 `EFAULT`（绝不让内核吃 #PF）。IPC 的用户
  指针路径改为调用同一份 uaccess。syscall 入口的内核栈从固定静态栈改成全局
  `syscall_kernel_rsp0`：调度器每次 `switch_context` 前用
  `syscall_set_thread_stack(next)` 把它和 TSS.RSP0 一起指向 next 的 TCB 栈
  （boot 线程没有 TCB 栈，回退到 isr.S 静态栈）。syscall 0 扩展为
  `write(buf,len)`（rdi=0 时保留 WP1-3 诊断语义），syscall 1 记录 rdi 退出码。
- **理由**：校验与加载分层后，host 单测能覆盖全部拒绝路径（不需要 QEMU），
  而内核侧只保留"分配/拷贝/映射"这三种必然可能失败的 I/O；env 回调解耦也让
  retry/回滚可证明（失败时把已映射页 unmap+free，页池回到全空，单测断言）。
  独立 PML4 而不是共享内核 PDPT：WP1-3 已在 0x40000000 映射过静态用户页，
  共享 PDPT 会让 ring3 段与旧页表项撞车；新建 PDPT 同时把用户页完全隔离在
  用户 AS 里。用户窗口限定 <512 GiB（PML4[0]）使 AS 销毁只走自己的子树，
  绝不会 free 到内核共享表。copyin/copyout 收敛成一份：WP1-6 的 IPC 用户
  指针校验与 WP1-7 的 syscall write 若各自实现，迟早出现"一处查 U/S、一处
  漏查"的差异。per-thread RSP0 是 WP1-3/1-6 遗留的硬约束：阻塞式 syscall
  切走后回到同一栈的前提是"只有一个用户线程"；把 RSP0 跟 TCB 走之后，
  syscall 入口与 ring3 异常入口天然使用当前线程的内核栈，为进程模型铺路。
  加载器限制段 ≤256 页（1 MiB）是因为回滚路径按页记录，属于本 WP 的明确
  上限（hello 只用 3 页）。
- **影响**：`src/ng/elf.[ch]`、`useras.[ch]`、`uaccess.[ch]`、`elf_selftest.[ch]`、
  `src/ng/user/hello.S` + `user.ld`、`src/ng/mem.c`（freestanding memcpy/memset）
  新增；`paging.[ch]` 增加 `pg_*_in()`/`pg_switch_pml4()`/`pg_current_pml4()`；
  `gdt.[ch]` 增加 `gdt_set_rsp0()`；`syscall.[ch]` 增加 `syscall_run_user()`/
  `syscall_set_thread_stack()` 与 syscall 0 用户 write；`isr.S` 的 syscall 入口
  改为装载 `syscall_kernel_rsp0`，ring0/ring3 旧 stub 显式清 rdi/rsi；`sched_arch.c`
  在切换前同步栈；`ipc.c` 的用户指针改走 `uaccess`；`main.c` 加 WP1-7 自检；
  `build-ng.ps1` 先构建/校验 `obj-ng/hello.elf` 再 `.incbin` 嵌进内核，C 源
  21→26 步；`run-ng.ps1` 加 6 条固定行 + 5 条正则；`tools/unit-ng.ps1` 加
  `elf.c`/`test_elf.c`，host 单测 810→901 checks；`docs/design/elf-ng.md` 记录
  设计。旧 32 位树完全不改。
- **替代**：用户程序放 `dist-ng/` 由加载器读文件——先要 FAT/ramfs，超范围，
  否决（内嵌 ELF 仍是完整 ELF64 加载路径，只是"文件"在内存里）；共享内核
  PDPT——WP1-3 用户页冲突且隔离性差，否决；每段按 p_offset 直接映射文件页
  （零拷贝）——内嵌镜像在 .rodata（只读），用户段要可写，做不到；且需求明确
  要 .bss 清零；把校验写在加载循环里——host 无法脱离页表驱动，拒绝路径也
  测不全，否决；per-CPU 系统调用栈（syscall 时切到 CPU 栈）——仍是单栈，
  阻塞式 syscall 并发时一样坏，且 TSS.RSP0 与 syscall 入口要维护两份栈，
  否决。
## ADR-020：进程用「1 AS + 1 主线程 + 1 张句柄表」；ticket 是端口的全局身份，句柄是进程私有引用

- **决策**：WP3-0 起每个用户进程有一个独立 PML4（复用 WP1-7 的 `user_as`）、
  一个内核主线程（`thread_t.pml4/proc/syscall_top`）、一张 64 槽句柄表
  （`struct handle_table`）。进程表静态 32 槽，pid 从 1 单调递增（槽位复用、
  pid 不复用），pid 1 是内核 init（boot 线程，无用户 AS），孤儿重挂到它。
  syscall 追加 40 spawn / 41 wait / 42 getpid / 43 kill / 44 getppid；退出状态
  正常 = 0..255，被 kill = `0x100|sig`。端口对象在**全局端口池**里，有两个名字：
  - **handle** = `(generation<<16)|slot`：进程私有引用（PORT/THREAD/FILE），
    spawn 不继承，退出/被 kill 时按它回收自己拥有的端口；
  - **ticket** = `(port_generation<<16)|port_slot`：端口的**全局身份**，唯一
    允许跨进程传递的名字。`send/reply` 凭 ticket，`recv/close` 还要求
    `caller pid == owner pid`。
- **理由**：跨进程命名必须有一个"不依赖某张进程表"的名字，否则 B 拿到 A 的
  句柄数值无法解析；而进程私有回收又必须知道"哪些端口是我的"，所以句柄表
  并不能省——两者是两个正交的命名空间。选择 `ticket` 同时是 capability
  （知道即可 send），是因为本 WP 不做 cap 表；把 recv/close 绑到 owner pid，
  让 ticket 泄露的后果限制在"能往你端口发消息"，不会丢消息/关端口。进程表
  pid 不复用是为了让 stale pid（wait/kill 之后）一定 ESRCH，而不是误伤新进程。
- **影响**：`src/ng/proc.[ch]`、`proc_table.c`、`proc_selftest.[ch]` 新增；
  `thread.[ch]` 增加 `struct thread_uctx`/`pml4`/`proc`/syscall 栈与
  `sched_kill_thread()`；`sched.c` 增加 `sched_core_remove()`；`sched_arch.c`
  切换前写 CR3；`syscall.[ch]`/`isr.S` 把 ring3 状态从全局搬进 TCB 并新增
  `ring3_enter_args`；`ipc.[ch]`/`ipc_core.c` 增加 `owner_pid`/ticket 解析/
  `ipc_core_cancel_thread`；`handle.[ch]` 增加 `handle_find_obj()`；
  `fs_syscall.[ch]` 增加内核侧 seed/read；`wp26_selftest.c` 把 shell 改成进程；
  `main.c` 加 `proc_init()` 与 WP3-0 自检；`src/apps/proc/proc.c` 新增多角色
  用户程序；`src/apps/shell/shell.c` 加 `spawn`/`wait`/`pid`；`build-ng.ps1`
  新增 `proc.elf` 嵌入与 3 个内核源（55 步）；`run-ng.ps1` 加 8 条期望行；
  `tests/unit-ng/test_proc.c` 新增（host 单测 3203→4043）。
- **替代**：`fork` 复制 AS——需要写时复制/页引用计数，属于 D1，否决；把所有
  进程共用一个句柄表（WP1-6 原样）——A 退出会关掉 B 的端口，且无法做
  "每进程命名空间"，否决；ticket 直接用 `(pid<<16)|slot`——pid 复用/端口槽
  复用都会撞车，且要求全局 pid→端口表，否决；wait 用忙等——破坏协作式语义且
  烧 CPU，否决；kill 只置标志等目标自己退出——阻塞在 recv 的目标永远不会退出，
  否决。