# ng-WP1-4：PVH 内存图 + PMM + heap + 页表池动态化（PASS）

## 结论

**PASS** —— ng 内核在 `kmain` 里解析 QEMU `-kernel` 的 **PVH `hvm_start_info`**
（magic `0x336EC578`，本次 7 条 memmap，可用 **65407 页**），据此初始化
bitmap PMM（管理 1 GiB 窗口，保留低 1 MiB / 内核镜像 / 引导结构，剩余
**65197 页**空闲）；单页/连续页分配、清零、double-free/保留页/越界/未对齐
拒绝全部生效。heap 实现 `kmalloc/kfree/krealloc/kmalloc_aligned`
（first-fit + 同 region 相邻空闲块合并 + 泄漏/野指针/重复释放自检）；
WP1-2 的 24 页 `.bss` 页表静态池已删除，`pg_alloc_table()` 每页向 PMM 申请
（本次运行页表页计数 7）。host 单测 64 位化为 `tests/unit-ng/` +
`tools/unit-ng.ps1`，本机 clang 编译成 PE 直接跑，输出
**`166 checks, 0 failures`**。

`tools/build-ng.ps1`、`tools/run-ng.ps1`（`-cpu max` 与 `-cpu qemu64`）、
`tools/unit-ng.ps1` 均返回 0；旧 32 位 `build.ps1 -Test` 保持 **305/305**；
`check-domains` / `check-deps` 0 违规。

## 交付物

| 路径 | 内容 |
|---|---|
| `src/ng/memmap.c` / `memmap.h` | PVH start_info + multiboot2 mmap 解析、统一 `ng_memrange`、可用页统计（**新**） |
| `src/ng/pmm.c` / `pmm.h` | bitmap PMM（used/reserved 双位图、rover first-fit、单页/连续页、清零、错误拒绝与计数）（**新**） |
| `src/ng/heap.c` / `heap.h` | region + 48 字节块头、first-fit、合并、对齐分配、realloc、野指针/double-free 检测、`heap_check`（**新**） |
| `src/ng/mem_selftest.c` / `mem_selftest.h` | 内核侧 glue：内存图→PMM→自检、heap 自检、页表池证据行（**新**） |
| `src/ng/phys.h` | 页大小 / 1 GiB 物理窗口 / 对齐宏（**新**） |
| `src/ng/paging.c` / `paging.h` | 删除 `pt_pool[24]`，新增 `pg_alloc_table()` / `pg_table_count()`；页表证据行改 `tables=N (4-level, pmm)`（**改**） |
| `src/ng/main.c` | 启动顺序改为 内存图/PMM → 页表 → GDT/IDT/syscall → heap/pgtbl；`WP1-4 selftest` 行（**改**） |
| `tests/unit-ng/test_util.h` / `unit_main.c` | 极简检查框架 + `N checks, M failures` 输出（**新**） |
| `tests/unit-ng/test_pmm.c` | 8 MiB fake memory 上的 PMM 测试（**新**） |
| `tests/unit-ng/test_heap.c` | fake 页分配器上的 heap 测试（**新**） |
| `tests/unit-ng/test_memmap.c` | 合成 PVH / multiboot2 结构解析测试（**新**） |
| `tools/unit-ng.ps1` | host 单测编译/运行/解析 `N checks, M failures`，<20 checks 或 failure 非 0 即失败（**新**） |
| `tools/build-ng.ps1` | 新增 4 个 C 源（memmap/pmm/heap/mem_selftest），13 步构建（**改**） |
| `tools/run-ng.ps1` | 新增 6 条固定期望行 + memmap 正则 + WP1-4 失败模式（**改**） |
| `docs/design/memory-ng.md` | 内存图/PMM/heap/页表池设计、数字、边界（**新**） |
| `docs/design/paging-ng.md` | 页表池改为 PMM 动态分配、启动顺序更新（**改**） |
| `docs/design/decisions.md` | ADR-016（**追加**） |

未修改 `src/kernel/**`、`build.ps1`、`tools/ci.ps1`、`abi.h`。

## 环境

- 分支 `ng`，基线 HEAD `2b632a5`（WP1-3）；仓库 `D:\OS_Project`。
- clang 23.1.4 / ld.lld 23.1.4（`D:\msys64\mingw64\bin`），内核
  `--target=x86_64-elf -ffreestanding -nostdlib -mno-red-zone
  -fno-stack-protector`；host 单测用同一 clang 的默认
  `x86_64-w64-windows-gnu`（PE，可直接运行）。
- QEMU 11.0.0（`v11.0.0-12122-ga4bb4b10c9`），
  `qemu-system-x86_64 -kernel -m 256 -cpu max`。

## 证据（2026-10-31）

### 构建

```text
powershell -File tools/build-ng.ps1
```

```text
=== build-ng (WP1-4) ===
clang: D:\msys64\mingw64\bin\clang.exe
lld:   D:\msys64\mingw64\bin\ld.lld.exe
  [1/13] entry.S
  [2/13] isr.S
  [3/13] serial.c
  [4/13] memmap.c
  [5/13] pmm.c
  [6/13] heap.c
  [7/13] paging.c
  [8/13] gdt.c
  [9/13] trap.c
  [10/13] syscall.c
  [11/13] mem_selftest.c
  [12/13] main.c
  [13/13] link -> dist-ng/aster-ng.elf

ELF64: PASS (44616 bytes)
  magic    7f 45 4c 46
  class    2 (ELF64)
  machine  62 (EM_X86_64)
  type     2 (ET_EXEC)
  entry    0x0000000000101000
  phnum    6
segments: PASS
  4 PT_LOAD, 4 KiB aligned (per-section pages)
multiboot2: PASS
  header   file offset 0x1000, length 24, checksum 0, 8-byte aligned
pvh-note: PASS
  entry    0x0000000000101000 (PT_NOTE, Xen type 18; QEMU -kernel)

build-ng: PASS -> D:\OS_Project\dist-ng\aster-ng.elf
```

### 串口（`powershell -File tools/run-ng.ps1`，`-cpu max`）

```text
=== run-ng (WP1-4) ===
elf:  D:\OS_Project\dist-ng\aster-ng.elf (44616 bytes)
qemu: D:\Program Files\qemu\qemu-system-x86_64.exe
      QEMU emulator version 11.0.0 (v11.0.0-12122-ga4bb4b10c9)
args: -kernel <elf> -serial file:<log> -display none -no-reboot -m 256 -cpu max (timeout 20s)
run-ng: serial output:
------------------------------------------------
ng: hello 64-bit
ng: boot info=0x1580
ng: memmap src=pvh entries=7 usable=65407 pages boot=0x1580..0x1670
ng: memmap ok (usable=65407 pages)
ng: pmm window=0x0..0x40000000 total=262144 usable=65407 free=65197
ng: pmm alloc/free ok
ng: pmm double-free rejected
ng: cpu nx=1 smep=1 smap=1 (cpuid max=0xd ext=0x80000021)
ng: pml4=0x139000 tables=4 (4-level, pmm)
ng: tree pml4[0]=0x13a023 pdpt[0]=0x13b023 pd[0]=0x13c023
ng: seg note 0x100000..0x101000 pages=1 pte=0x8000000000100001 W=0 NX=1
ng: seg text 0x101000..0x107000 pages=6 pte=0x101021 W=0 NX=0
ng: seg rodata 0x107000..0x108000 pages=1 pte=0x8000000000107021 W=0 NX=1
ng: seg data 0x108000..0x109000 pages=1 pte=0x8000000000108003 W=1 NX=1
ng: seg bss 0x109000..0x133000 pages=42 pte=0x8000000000109003 W=1 NX=1
ng: 2m  0x200000..0x40000000 entries=511 P=1 W=1 NX=1
ng: cr0=0x80010011 cr4=0x300020 efer=0xd00 cr3=0x139000
ng: paging 4-level ok
ng: wx layout ok (text rx, rodata ro, data rw-nx)
ng: nx on
ng: smep=on smap=on
ng: helper map/unmap/protect ok
ng: WP1-2 selftest PASS
ng: gdt 7 entries (kcode/kdata/udata/ucode/tss), tr=0x28 gdt=0x127000 tss=0x127040
ng: idt 256 entries, 32 exception stubs base=0x12d0d0
ng: tss rsp0=0x12b0b0 ist1=0x12d0b0 idt8.ist=1
ng: syscall msr star=0x13000800000000 lstar=0x1012b3 fmask=0x40700 efer.sce=1
ng: #pf cr2=0x107000 error=0x3 rip=0x10127b
ng: #pf ro write handled ok
ng: #ud handled ok
ng: #gp handled ok
ng: trap recovery pf=1 ud=1 gp=1 gp_err=0x40
ng: syscall write (ring0)
ng: syscall ring0 ok
ng: ring3 map code=0x40000000 pte=0x130005 stack=0x40100000 pte=0x8000000000131007
ng: syscall write (ring3)
ng: syscall exit test
ng: ring3 syscalls=2 user_rsp=1 exit=1
ng: ring3 syscall ok
ng: WP1-3 selftest PASS
ng: heap kmalloc ok
ng: heap coalesce ok
ng: pgtbl from pmm ok (tables=7)
ng: WP1-4 selftest PASS
------------------------------------------------
run-ng: matched 18/18 expected lines + 4 patterns
run-ng: PASS (all expected lines in 0.2s, cpu max)
```

`run-ng.ps1` 新增的期望（全部命中才 PASS）：

```text
6 条固定行：
  ng: pmm alloc/free ok
  ng: pmm double-free rejected
  ng: heap kmalloc ok
  ng: heap coalesce ok
  ng: pgtbl from pmm ok
  ng: WP1-4 selftest PASS
1 条正则：
  ng: memmap ok \(usable=[0-9]+ pages\)
失败早停模式新增：
  ng: memmap FAIL / ng: pmm .* FAIL / ng: heap .* FAIL / ng: WP1-4 selftest FAIL
```

### 内存图来源与可用页

QEMU `-kernel` 走 Xen PVH：`entry.S` 把 EBX 原样转交 `kmain`，本次
`boot info=0x1580`，结构首 u32 = `0x336ec578`，`version=1`，
`memmap_paddr=0x15c8`，`memmap_entries=7`。start_info 与 memmap 都在 1 MiB
以下，恒等映射可直接读。

PVH 表（运行中调试打印，未提交）：

```text
[0] 0x0000000000000000 + 0x000000000009FC00  type=1 RAM
[1] 0x000000000009FC00 + 0x0000000000000400  type=2 reserved
[2] 0x00000000000F0000 + 0x0000000000010000  type=2 reserved
[3] 0x0000000000100000 + 0x000000000FEE0000  type=1 RAM
[4] 0x000000000FFE0000 + 0x0000000000020000  type=2 reserved
[5] 0x000000000FFFC0000 + 0x0000000000040000 type=2 reserved
[6] 0x00000FD00000000 + 0x000000300000000  type=2 reserved（MMIO 高空洞）
```

可用页 = `[0, 0x9f000)` 的 159 页 + `[0x100000, 0x10000000)` 的 65248 页
= **65407**。PMM 窗口固定 `[0, 1 GiB)`（`total=262144`），只把可用 RAM
放开；低 1 MiB 与内核镜像 `0x100000..0x133000`（51 页）保留后
`free=65197`（65407 - 低 1 MiB 中的 159 个可用页 - 内核 51 页）。

multiboot2 作为 fallback 实现（tag 6 mmap，结构校验；QEMU 不走该路径），
host 单测用合成结构覆盖。

### PMM 设计

- 两份静态位图（used / reserved）各 32 KiB，放 `.bss`；位图不进 PMM 自己
  管理的内存，避免 bootstrap 环。
- 页号相对窗口 base；1 GiB 窗口与恒等映射一致，不会发出“无映射的物理页”。
- 单页 / 连续页（`pmm_alloc_page` / `pmm_alloc_pages(n)`），rover first-fit。
- 分配默认清零（`PMM_ZERO_ON_ALLOC=1`）。
- 释放 **先整体校验再整体归还**：未对齐 → `PMM_EINVAL`；窗口外/跨末端 →
  `PMM_ERANGE`；保留页 → `PMM_ERESERVED`；`used=0` → `PMM_EDOUBLE`；
  全部计入 `pmm_stats` 的拒绝计数。
- 内核自检：单页 + 4 连续页分配/写读/归还后账目复原；同一页连 free 两次
  第二次必须是 `PMM_EDOUBLE` 且计数 +1；free(0)（低 1 MiB）必须
  `PMM_ERESERVED`；free(1 GiB) 必须 `PMM_ERANGE`。

对应串口行：`ng: pmm window=... free=65197`、`ng: pmm alloc/free ok`、
`ng: pmm double-free rejected`。

### heap 设计

- region（连续 PMM 页，至少 16 页 = 64 KiB，最多 16 个）+ 48 字节块头
  （magic/size/flags/prev/next），所有 region 的块按地址串成双向链表。
- first-fit；同一 region 内相邻空闲块合并（跨 region 即使地址连续也不合并）；
  尾部剩余 < 48+16 时不切。
- `kmalloc` / `kmalloc_aligned`（≥16 的 2 的幂，块头前移做对齐）/
  `kfree`（NULL 安全、野指针/未对齐/已合并块头拒绝）/ `krealloc`
  （扩大 copy+free，缩小可切尾）。
- 记账：`heap_live_allocs` / `heap_live_bytes`；自检结束要求
  `live_allocs=0`、`live_bytes=0`、每 region 只剩一个空闲块、
  `heap_check()=0`、`free_bytes + 48 == 65536`（本次首次扩堆正好 16 页）。
- 内核自检顺序：先做 double-free（首块释放后与右侧空闲块合并、块头仍是
  自己，第二次 free 稳定 `HEAP_EDOUBLE`），再跑 64/128/64 块、
  realloc 256→32、64 对齐分配，最后全部释放检查合并与泄漏。

对应串口行：`ng: heap kmalloc ok`、`ng: heap coalesce ok`。

### 页表池来自 PMM

`paging.c` 不再有 `pt_pool`/`PT_POOL_PAGES`；`pg_alloc_table()` 调
`pmm_alloc_page()`、清零 512 个 u64、`pg_tables_allocated++`。`pg_init()`
用 4 页（PML4/PDPT/PD/PT），WP1-3 的 helper 自测按需再建 2 页，WP1-4 的
`mem_pgtbl_selftest()` 再申请 1 页并验证非空/4 KiB 对齐/512 项全零/计数 +1，
最终 `tables=7`。证据行：

```text
ng: pml4=0x139000 tables=4 (4-level, pmm)
ng: pgtbl from pmm ok (tables=7)
```

### host 单测（64 位化）

```text
powershell -File tools/unit-ng.ps1
```

```text
=== unit-ng (WP1-4 host tests) ===
clang: D:\msys64\mingw64\bin\clang.exe
166 checks, 0 failures
unit-ng: PASS (166 checks, 0 failures)
```

- 一个本机 PE 可执行文件（无内核/无 QEMU），源：`pmm.c`、`memmap.c`、
  `heap.c` + 4 个测试文件；`-DNG_HOST_TEST=1` 让 heap 走 fake 页分配器。
- PMM：8 MiB 静态缓冲区当物理内存（窗口 base 为宿主指针），覆盖保留页、
  单页/8 连续页、double-free、保留页释放、越界/未对齐、窗口末端、
  `alloc_pages(0)`、超额分配失败计数、清零。
- heap：fake 4 MiB（1024 页），覆盖首次适配落点、乱序释放合并、
  double-free、野指针、对齐 64/4096、realloc 扩/缩、两个 region 扩堆、
  8 MiB 分配失败回滚、`heap_check`。
- memmap：合成 PVH（含 1 GiB 外与 4 GiB 外条目、坏 version/entries/paddr/
  magic）、合成 multiboot2（2 条 RAM、坏 total_size、越界 tag）。
- `unit-ng.ps1` 要求输出行 `N checks, M failures`、M=0、N≥20，否则 exit 1。

### multiboot2 / PVH 的边界

- PVH 是实测路径；`memmap_paddr=0`、`entries=0`、`version=0` 均返回
  `MEMMAP_EBAD`，magic 不匹配返回 `MEMMAP_ENOENT`。
- multiboot2 的 magic 在 EAX、不在 info 结构里；`entry.S` 目前只转交 EBX，
  所以该路径靠 `total_size` + tag 链结构校验识别，**未在 GRUB 实机验证**。
  QEMU `-kernel` 不会走这条路径。

### `-cpu qemu64`（SMEP/SMAP off 路径）

```text
ng: memmap ok (usable=65407 pages)
ng: pmm alloc/free ok
ng: pmm double-free rejected
...
ng: smep=off smap=off
...
ng: heap kmalloc ok
ng: heap coalesce ok
ng: pgtbl from pmm ok (tables=7)
ng: WP1-4 selftest PASS
run-ng: matched 18/18 expected lines + 4 patterns
run-ng: PASS (all expected lines in 0.2s, cpu qemu64)
```

PMM/heap/页表池不依赖 SMEP/SMAP，qemu64 与 max 的数字一致。

### panic 路径回归

`tools/build-ng.ps1 -PanicTest` + `tools/run-ng.ps1 -Panic` 仍 PASS：

```text
ng: panic selftest: triggering unrecoverable #UD
PANIC: vec=0x6 err=0x0 rip=0x106175 cr2=0x107000
run-ng: PASS (panic path observed in 0.2s, cpu max)
```

随后重新执行普通 `build-ng.ps1`，产物回到正常镜像。

## 旧树回归与边界检查

```text
powershell -File build.ps1 -Test
...
=== compile: 1 compiled, 103 skipped ===
=== elapsed: 41.2s ===
=== TEST PASS (305/305) ===
```

```text
powershell -File tools/check-domains.ps1
checked: 205 files, 726 quoted includes
domain include violations: 0

powershell -File tools/check-deps.ps1
OK: no cross-module include violations.
```

未修改 `src/kernel/**`、`build.ps1`、`tools/ci.ps1`、`abi.h`；`src/ng` 的
新增文件不进旧对象列表。

## 自检项清单

1. PVH magic/version/memmap 解析；可用页 65407；引导结构被保留。
2. PMM 窗口 1 GiB、used/reserved 双位图；低 1 MiB + 内核镜像 + 引导结构保留。
3. 单页与 4 连续页分配/写读/释放后 free 计数复原。
4. double-free → `PMM_EDOUBLE` + 计数 +1；保留页 → `PMM_ERESERVED`；
   1 GiB 越界 → `PMM_ERANGE`；未对齐 → `PMM_EINVAL`。
5. heap 首次适配、64/128 对齐、realloc 扩/缩保留数据、对齐分配。
6. 全部释放后每 region 一个空闲块、无泄漏、`heap_check=0`。
7. 页表页来自 PMM：`pg_alloc_table` 计数 +1、页清零、4 KiB 对齐。
8. host 单测 166 checks / 0 failures（PMM + heap + memmap）。

## 未做 / 遗留（按 WP 边界）

- **调度/线程**：PMM/heap 无锁，假定单核 + IF=0；SMP/抢占同步留给 WP1-5。
- **页表回收**：`pmm_free_page` 可用，但没有“拆地址空间→释放页表子树”的
  路径；用户页/页表页回收与 COW 引用计数留给 WP1-7 之后。
- **heap 上限**：最多 16 个 region、单次分配 ≤1 GiB；没有
  `heap_shrink`/碎片整理；region 只增不减。
- **PMM 窗口**：只管理 1 GiB（与恒等映射一致）；RAM 更大时超出部分不可用，
  扩 4 GiB 需同步改 `phys.h`、位图大小和页表建表范围。
- **multiboot2**：解析代码与 host 单测有，但 QEMU 实机只走 PVH；GRUB/UEFI
  实机未验，且 magic（EAX）尚未由 `entry.S` 转交。
- **内存图条目上限**：统一表 64 项（PVH 超过会 `MEMMAP_EFULL`）；真机
  e820 通常 <32 项，够用。
- 不实现 swap/NUMA/内存 cgroup/内存热插拔。

## 出口

```text
powershell -File tools/build-ng.ps1    # PASS，dist-ng/aster-ng.elf（44616 bytes）
powershell -File tools/run-ng.ps1      # PASS，18 条期望行 + 4 条正则
powershell -File tools/unit-ng.ps1     # PASS，166 checks, 0 failures
powershell -File build.ps1 -Test       # PASS，305/305（旧 32 位）
```
