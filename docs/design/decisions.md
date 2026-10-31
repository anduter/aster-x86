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
