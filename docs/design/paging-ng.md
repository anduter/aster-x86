# ng 分页与内存保护（WP1-2）

> 实现：`src/ng/entry.S`（引导页表、EFER）、`src/ng/paging.c`（最终 4 级页表、
> W^X/NX、SMEP/SMAP、自检）、`src/ng/link.ld`（段边界符号）。
> 决策记录见 `docs/design/decisions.md` ADR-014；出口证据见 `docs/claims/ng-WP1-2.md`。

## 1. 布局（恒等映射，虚拟 == 物理）

```text
PML4[0] ──> PDPT
             ├─ PDPT[0] ──> PD
             │               ├─ PD[0]      ──> PT   [0x000000, 0x200000) 4 KiB 页
             │               │                      ├─ [0x000000,0x100000) RAM/VGA/空洞  rw-nx
             │               │                      ├─ [0x100000,__text_start) 启动头    ro-nx
             │               │                      ├─ [__text_start,__text_end) .text    r-x
             │               │                      ├─ [__rodata_start,__rodata_end) .rodata ro-nx
             │               │                      └─ [__data_start,__kernel_end) .data/.bss rw-nx
             │               └─ PD[1..511]           [0x200000, 0x40000000) 2 MiB 大页 rw-nx
             └─ PDPT[1..511] 空（1 GiB 以上不映射）

PML4[256..511] 空：不做 higher-half 映射，用户态地址空间留给 WP1-3/1-7。
```

- 恒等映射 1 GiB 与 WP1-1 的行为一致；不同点是 0..2 MiB 换成 4 KiB 页，才能按段
  给 `.text`/`.rodata`/`.data`/`.bss` 不同权限。
- 内核镜像必须整个落在第一个 2 MiB 内（`link.ld` 的
  `ASSERT(__kernel_end <= 0x200000)` 兜底）；WP1-4 镜像约 0x133000，余量充足。
- 页表页从 PMM 动态申请（WP1-4 起，`pg_alloc_table()` → `pmm_alloc_page()`），
  每页清零；不再有 `.bss` 静态池。PMM 位图本身在 `.bss`，由内核镜像保留区
  覆盖。
- 引导页表（`boot_pml4/boot_pdpt/boot_pd`，1 GiB 2 MiB）只为把 CPU 带进长模式；
  C 建好最终表后写 CR3 切换。两张表对当前代码/栈/表池的映射一致，所以**不需要**
  再 far jump 一次。

## 2. 段权限（W^X）

| 段 | 页大小 | P | W | NX | 含义 |
|---|---|---|---|---|---|
| `.multiboot` + `.note.Xen` | 4 KiB | 1 | 0 | 1 | 只读、不可执行 |
| `.text` | 4 KiB | 1 | 0 | 0 | 只读、可执行（R-X） |
| `.rodata`（含 GDT） | 4 KiB | 1 | 0 | 1 | 只读、不可执行 |
| `.data` | 4 KiB | 1 | 1 | 1 | 可写、不可执行 |
| `.bss`（PMM 位图、栈、静态页面） | 4 KiB | 1 | 1 | 1 | 可写、不可执行 |
| 其余 RAM/空洞 0..1 MiB | 4 KiB | 1 | 1 | 1 | 可写、不可执行 |
| RAM 2 MiB..1 GiB | 2 MiB | 1 | 1 | 1 | 可写、不可执行 |

- 段之间用 `ALIGN(4096)` 隔开：两个段共享一个 4 KiB 页就没法分开保护。
- `CR0.WP=1`：只读页在 ring0 也真正只读，否则 W^X 只是"用户态才生效"。
- 所有内核页 `U/S=0`（`PG_USER` 未使用）；用户页留给 WP1-3/1-7。

## 3. PTE 标志位

| 位 | 名称 | 本 WP 用法 |
|---|---|---|
| 0 | P | 所有映射页/表项置 1 |
| 1 | R/W | `.text`/`.rodata`/启动头 = 0；`.data`/`.bss`/大页 = 1 |
| 2 | U/S | 恒 0（内核页） |
| 3/4 | PWT/PCD | 恒 0（默认 WB；MMIO 的缓存属性等 WP1-4/驱动再定） |
| 5 | A | CPU 置位，代码不清 |
| 6 | D | CPU 置位，代码不清 |
| 7 | PS | 仅 PD[1..511] 的 2 MiB 大页置 1；PD[0] 是 PT 指针，必须为 0 |
| 8 | G | 恒 0（切 CR3 会刷 TLB，不依赖全局页） |
| 63 | NX | 数据/只读页 = 1；`.text` = 0 |

地址位 12..51 用 `PG_ADDR_MASK = 0x000FFFFFFFFFF000` 取。

## 4. 初始化顺序（顺序错就会 #PF / 三重故障）

1. `entry.S`：CPUID 查 LM（必须）与 NX（可选）→ 建 1 GiB 2 MiB 引导表 →
   `CR4.PAE=1` → `CR3=boot_pml4` → `EFER.LME=1`（NX 支持时同时 `NXE=1`）→ `CR0.PG=1`
   → far jump 到 64 位代码段 → `call kmain`。
2. `mem_boot_init()`（WP1-4）：解析 PVH/multiboot2 内存图 → `pmm_init()` →
   标记可用 RAM → 保留低 1 MiB/内核镜像/引导结构；页表页从这里开始可分配。
3. `pg_init()`：CPUID 查 NX/SMEP/SMAP → 确认/兜底 `EFER.NXE` → 从 PMM 申请
   页表页建最终表（含 NX 位）→ 写 CR3 → `CR0.WP=1` → `CR4.SMEP/SMAP` →
   再写一次 CR3 刷 TLB。
4. 自检：读 CR0/CR4/EFER/CR3 + 逐页走表断言 → 打印证据行。

要点：

- **NXE 必须在带 NX 位的表被 CR3 装载前置位**，否则 NX=1 是保留位 → #PF。
- 最终表自己的页（WP1-4 起由 PMM 分配）必须映射成 RW+NX，否则建表过程会在
  只读/不可写的映射上写表项；PMM 只发 1 GiB 恒等映射范围内的 RAM 页。
- 写 CR3 前先确认新表覆盖当前 RIP/RSP 所在页（这里都是恒等映射）。

## 5. helper 语义

```c
int pg_map(u64 va, u64 phys, u64 flags);   /* 建 4 KiB 映射；已映射返回 PG_EEXIST */
int pg_unmap(u64 va);                       /* 清 PTE；未映射返回 PG_ENOENT */
int pg_protect(u64 va, u64 flags);          /* 改权限，保留物理地址 */
u64 pg_walk(u64 va);                        /* 叶项原值；未映射返回 0 */
```

- `flags` 只传权限位（`PG_WRITE`/`PG_NX`/…），`PG_PRESENT` 由 helper 加；物理地址
  必须 4 KiB 对齐。
- 缺页表时按需分配；遇到 2 MiB 大页返回 `PG_EBIGPAGE`，**不自动拆分**（拆分 +
  回收留给 WP1-4 的 PMM）。
- 成功/失败都做 `invlpg`，不依赖调用方刷 TLB。
- 当前只支持 4 级（48 位）地址；`va >> 48 != 0` 直接 `PG_EINVAL`。

## 6. 自检（不依赖 trap）

WP1-2 不做 IDT/#PF（WP1-3），所以"保护生效"用**读回 + 走表断言**证明，而不是靠
故意触发异常：

1. `CR0.PE/PG/WP=1`、`CR4.PAE=1`、`EFER.LMA=1`、`CR3 == 新 PML4 物理地址`；
2. PML4 上半区（256..511）为空；PML4[0]/PDPT[0] 是表指针（PS=0），PD[0] 是 PT；
3. 每个段逐页检查：存在、4 KiB、W/NX 与期望一致（段边界必须页对齐）；
4. PD[1..511]：511 个 2 MiB 大页、恒等、RW、NX；
5. helper 行为：在 1 GiB（恒等映射之外、PDPT[1]）map 一页 → 写读模式 →
   重复 map 被拒 → protect 成只读（W=0）→ 再读 → unmap → PTE 归零；
6. `.data` 里一个 `volatile` 已初始化变量读回 + 改写，覆盖"可写数据页"。

**不能**在自检里写只读页/执行数据页：那会触发 #PF，而 #PF 处理器要等 WP1-3。
负向验证（临时把 `.text` 改成可写）会打印 `ng: CHECK FAIL text`、
`ng: wx layout FAIL`、`ng: WP1-2 selftest FAIL (1)`，`run-ng.ps1` 返回 1——说明
这些断言不是空跑。

## 7. SMEP / SMAP 与 stac/clac 约定

- 检测：`CPUID.(7,0):EBX[7]` = SMEP、`EBX[20]` = SMAP；支持才置
  `CR4.SMEP/SMAP`，不支持就打印 `smep=off smap=off`，**不 panic**。
- SMEP：ring0 不能执行 U/S=1 的页。当前没有用户页，等 WP1-7 的 ring3 才真正生效。
- SMAP：ring0 访问 U/S=1 的页会 #PF，除非 `RFLAGS.AC=1`（`stac` 置位、`clac` 清除）。

**内核访问用户内存的约定（WP1-3/1-7 必须遵守）：**

1. 所有用户内存访问都走 `copyin`/`copyout` 这类封装（WP1-3 实现），
   内部是 `stac(); ...; clac();`，调用方不得自己散写 `stac`。
2. 开窗前做范围校验：用户指针 + 长度必须整体落在当前进程用户地址空间内，
   且逐页确认 PTE 的 `U/S=1`；**不把内核地址当用户地址传**（否则 SMAP 之外
   还会绕过内核保护）。
3. `stac`/`clac` 必须严格成对，窗口内只碰用户缓冲：不调用可能睡眠、调度或
   重入的代码，避免 `AC=1` 泄漏到别的执行流。
4. 异常/中断入口的第一条指令 `clac`（WP1-3 的 #PF/#GP 处理器），
   `syscall` 入口用 `SFMASK` 清 `AC`，保证内核总在 `AC=0` 下运行。
5. `stac/clac` 只在 CPUID 报 SMAP 时保证可用（否则 #UD）；SMAP=off 时执行它们
   也合法，所以封装里不必按 CPUID 分支，但范围校验始终要做。
6. `src/ng/io.h` 提供 `stac()`/`clac()` 两个 inline helper；WP1-7 起
   `src/ng/uaccess.c` 是唯一的用户内存访问封装（IPC 也改走它）。

### 7.1 WP1-7 多地址空间补充

WP1-7 在 paging 之上加了"按 PML4 操作"的一组接口，内核 PML4 的语义不变：

```text
pg_kernel_pml4() / pg_current_pml4()     当前 CR3 对应的表
pg_switch_pml4(pml4)                     写 CR3 + 更新 current_pml4（NULL=内核表）
pg_walk_in / pg_map_in / pg_unmap_in / pg_protect_in(pml4, ...)
pg_walk / pg_map / ...                   等价于 *_in(pg_kernel_pml4(), ...)
```

用户地址空间由 `useras.c` 新建 PML4：PML4[0] → 新 PDPT，PDPT[0] 复用内核 PD
（0..1 GiB 恒等映射，U=0），PDPT[1..511] 归用户页，PML4[1..255] 复制内核项。
切 CR3 后内核代码/栈/页表页仍走同一份恒等映射；用户页 U=1 且只在用户 AS 里。
详见 `docs/design/elf-ng.md` §5。

## 8. 未做（严格按 WP1-2 边界）

- #PF/#GP/IDT/trap、故意触发保护异常的负向测试：WP1-3（已完成，见
  `docs/design/trap-syscall-ng.md` 与 `docs/claims/ng-WP1-3.md`）。
- PMM/heap、页表页回收、2 MiB 大页拆分：WP1-4。
- 用户页（U/S=1）、ring3、ELF64 加载器：WP1-7。
- 1 GiB 大页、PWT/PCD/MMIO 缓存属性、higher-half 映射：暂不需要。
- multiboot2/PVH 内存图未解析：恒等映射固定 1 GiB，实际 RAM 大小未用。
