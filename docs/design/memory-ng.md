# ng 内存管理（WP1-4：内存图 / PMM / heap / 页表池）

> 配套 `paging-ng.md`（页表布局）与 `docs/claims/ng-WP1-4.md`（证据）。
> 代码：`src/ng/memmap.[ch]`、`src/ng/pmm.[ch]`、`src/ng/heap.[ch]`、
> `src/ng/mem_selftest.[ch]`；host 单测在 `tests/unit-ng/`。

## 1. 内存图

### 1.1 来源与进入状态

`entry.S` 把引导器的 EBX 原样转交给 `kmain(u64 boot_info)`：

| 引导路径 | boot_info 指向 | 识别方式 |
|---|---|---|
| QEMU `-kernel`（Xen PVH） | `hvm_start_info` 物理地址 | 结构首 u32 magic = `0x336EC578` |
| multiboot2 | multiboot2 information 物理地址 | 结构校验：`total_size` + tag 链（magic 在 EAX，entry.S 暂未转交） |

PVH 路径是当前 QEMU 的实测路径。start_info 与 memmap 表都在 1 MiB 以下
（本次运行 `0x1580` / `0x15C8`），恒等映射可直接解引用。

PVH 字段（`hvm_start_info`，56 字节，`__attribute__((packed))`）：

| 偏移 | 字段 | 本 WP 用法 |
|---|---|---|
| 0 | magic | `0x336EC578` |
| 4 | version | 必须 ≥ 1 |
| 12 | nr_modules | 忽略 |
| 40 | memmap_paddr | 指向 `hvm_memmap_table_entry[]` |
| 48 | memmap_entries | 条目数（上限 4096；统一表上限 64） |

`hvm_memmap_table_entry`（24 字节）：`u64 addr`、`u64 size`、`u32 type`、
`u32 reserved`。type 1=RAM、2=reserved、3=ACPI、4=NVS、5=unusable；只把
type 1 记为可用。multiboot2 的 type 1/3/4/5 映射到同一套编号。

### 1.2 统一后的 `ng_memmap`

- `entries[64]`：原始区间（RAM 与保留区都保留，便于打印/诊断）；
- `usable_pages/usable_bytes`：只算 type=RAM 与 `[0, 1 GiB)` 的交集，首尾
  按 4 KiB 对齐裁剪（半页不算）；
- `boot_start/boot_end`：start_info + memmap 表（PVH）或整个 mb2 info
  （multiboot2），调用方必须交给 PMM 保留；
- 解析失败返回 `MEMMAP_ENOENT/EBAD/EFULL`，不带半份结果。

本次 `-m 256` 的 PVH 表（7 项）：

```text
[0] 0x0000000000000000 + 0x000000000009FC00  RAM
[1] 0x000000000009FC00 + 0x0000000000000400  reserved
[2] 0x00000000000F0000 + 0x0000000000010000  reserved
[3] 0x0000000000100000 + 0x000000000FEE0000  RAM
[4] 0x000000000FFE0000 + 0x0000000000020000  reserved
[5] 0x000000000FFFC0000 + 0x0000000000040000 reserved
[6] 0x00000FD00000000 + 0x000000300000000  reserved（PCI/MMIO 高空洞）
```

可用页 = 159（低 640 KiB 的整页）+ 65248（1 MiB..255 MiB）= **65407 页**。

## 2. PMM

### 2.1 表示

- 管理窗口 `[0, 1 GiB)`（`NG_PHYS_LIMIT`，与 1 GiB 恒等映射一致）；
  `PMM_MAX_PAGES = 262144`。
- 两份静态位图：`used_bits`（1=已用）与 `reserved_bits`（1=永久保留），
  各 32 KiB，放 `.bss`（被内核镜像保留区覆盖，不参与分配）。
- 页号相对 `pmm_base`：`idx = (phys - base) >> 12`。内核 `base=0`；host
  单测可以把一块 8 MiB 缓冲区当地址窗口，因此 64 位宿主地址也能测。
- `rover` 记录上次分配位置，分配从 rover 开始线性 first-fit，扫到末尾回绕。

### 2.2 初始化与保留区

```c
pmm_init(0, NG_PHYS_LIMIT);              /* 全部 used+reserved */
for (RAM entries) pmm_add_usable(addr, size);
pmm_reserve(0, 1 MiB);                   /* IVT/BDA/VGA/引导结构 */
pmm_reserve(__image_start, __kernel_end - __image_start);
pmm_reserve(map.boot_start, map.boot_end - map.boot_start);
```

`pmm_add_usable()` 只放开完整落在区间内的页（起点向上、终点向下取整），
幂等；`pmm_reserve()` 把页标成 used+reserved（若原本空闲则同时减
`free_pages`）。本次数字：

```text
window = 0..0x40000000  total=262144  usable=65407  free=65197
保留 = 低 1 MiB 里的 159 个可用页 + 内核镜像 51 页（0x100000..0x133000）
```

### 2.3 分配 / 释放语义

| 操作 | 行为 |
|---|---|
| `pmm_alloc_page()` | 单页；失败返回 0（0 号页永远保留，0 不会是真页） |
| `pmm_alloc_pages(n)` | 连续 n 页；n=0 / n>free / 无连续段返回 0 |
| 清零 | 默认 `PMM_ZERO_ON_ALLOC=1`，逐 u64 清零；可编译期关闭 |
| `pmm_free_page(s)` | 先整体校验再整体归还，绝不半释放 |
| double free | 目标页 `used=0` → `PMM_EDOUBLE` + `reject_double++` |
| 保留页 | `reserved=1` → `PMM_ERESERVED` + `reject_reserved++` |
| 未对齐 | → `PMM_EINVAL` + `reject_align++` |
| 窗口外/跨末端 | → `PMM_ERANGE` + `reject_range++` |

校验顺序是 **对齐 → 窗口 → reserved → used**：释放内核镜像/低 1 MiB 的页
不会被当成普通“已用页”放回空闲池；释放从未分配过的页则稳定落到 double-free。
失败计数（`pmm_stats`）给自检/诊断读回。

`pmm_owns()`/`pmm_is_reserved()` 供页表、heap、驱动判断一块地址是否真的
归 PMM 管。

### 2.4 为什么位图是静态的

PMM 的第一个分配者不能是 PMM 自己：若位图放在被管理的 RAM 里，就要先
“分配位图页”，再把它从位图里标掉，还要防止后续清零把位图抹掉。WP1-4 选择
两份 32 KiB 的静态数组（总 64 KiB `.bss`），换取实现简单、无 bootstrap 环。
1 GiB 窗口是当前恒等映射的上界；扩到 4 GiB 时改 `NG_PHYS_LIMIT`、位图大小
和页表建表范围三处即可（`phys.h` 有注释）。

## 3. heap（kmalloc/kfree/krealloc）

### 3.1 结构

```text
region（连续物理页，至少 16 页 = 64 KiB，最多 16 个）
  └─ block（48 字节头：magic/size/flags/pad/prev/next/spare）
       ├─ payload（16 字节对齐）
       └─ block ...（所有 region 的块按地址串成双向链表）
```

- region 只从 PMM 整页申请；第一次分配不够时按需扩展，扩展页数 =
  `ceil((need + header + 16) / 4096)`，至少 16 页。region 用静态数组记录
  （16 项），用完返回 `HEAP_ENOMEM`。
- 块头 48 字节让 payload 相对块地址保持 16 字节对齐；`size` 是 payload
  字节数（可能略大于请求，尾部不足 `48+16` 时不再切）。
- **合并只在同一 region 内**、且两个空闲块地址严格相邻时进行。不同 region
  即使物理地址碰巧连续也不合并，`kfree()` 的 region 校验因此不会跨边界。
- 被合并吸收的块头写 `HEAP_MAGIC_DEAD`；迟到的指针会得到 `HEAP_EINVAL`
  而不是改坏堆。

### 3.2 API 与分配策略

| API | 说明 |
|---|---|
| `kmalloc(n)` | 首次适配；n 向上取整到 16；n=0 返回 NULL |
| `kmalloc_aligned(n, a)` | `a` 必须是 ≥16 的 2 的幂（上限 1 MiB）；块头前移，对齐前剩余切成空闲块 |
| `kfree(p)` | NULL 安全；野指针/未对齐/已合并块头 → `HEAP_EINVAL`；重复释放 → `HEAP_EDOUBLE` |
| `krealloc(p, n)` | NULL → kmalloc；n=0 → free+NULL；缩小可切尾；扩大 malloc+copy+free |
| `heap_add_region(base, size)` | 给堆增加一段内存（内核/host 单测共用） |
| `heap_check()` | 遍历链表：magic、region 边界、地址递增、prev/next、相邻空闲未合并、记账一致 |

记账：`heap_live_allocs`/`heap_live_bytes` 在分配/释放/缩放时同步；
`heap_free_bytes()`/`heap_block_count()`/`heap_region_count()` 从链表现算。
`heap_double_free_count()`/`heap_bad_free_count()` 记录被拒绝的坏释放。

### 3.3 自检

内核自检（`mem_heap_selftest()`）：

1. 首次分配 16 字节后立即释放，再释放一次 → 必须被拒（double-free）；
2. 64/128/64 字节三块：非空、16 对齐、互不重叠、写入读回一致；
3. `krealloc` 扩大到 256、缩小到 32 都保留数据；
4. `kmalloc_aligned(100, 64)` 按 64 对齐、可写可读；
5. 全部释放后：`live_allocs=0`、`live_bytes=0`、每 region 只剩一个空闲块、
   `heap_check()=0`、`free_bytes + 48 == 64 KiB`；
6. 打印 `ng: heap kmalloc ok`、`ng: heap coalesce ok`。

host 单测另外覆盖：首次适配落在 region 起点、乱序释放合并、野指针、扩堆
（两个 region）、超大分配失败不破坏记账、对齐分配 4096。

## 4. 页表池接入 PMM

`paging.c` 删除 WP1-2 的 `pt_pool[24][512]`，改为：

```c
void *pg_alloc_table(void) {
    u64 phys = pmm_alloc_page();     /* 失败返回 0 */
    if (!phys) return NULL;
    /* 清零 512 个 u64，pg_tables_allocated++ */
}
```

- `pg_init()` 的 PML4/PDPT/PD/PT、`pg_map` 按需建的中间表全部走这里；
- 页表页只可能落在 `[0, 1 GiB)` 的 RAM（PMM 窗口），已被 4 KiB PT 或
  2 MiB 大页覆盖为 RW+NX，可以直接写；
- `pg_table_count()` 给自检读回；`mem_pgtbl_selftest()` 再申请一页，验证
  非空、4 KiB 对齐、512 项全零、计数 +1，打印 `ng: pgtbl from pmm ok`；
- 本 WP 不回收页表页（地址空间拆除/用户页回收是 WP1-7 之后的事）。

## 5. 启动顺序

```text
serial_init
mem_boot_init(boot_info)     memmap -> pmm_init -> reserve -> PMM 自检
pg_init()                    页表页从 PMM 申请；W^X/NX/SMEP/SMAP
gdt_init / trap_init / syscall_init / WP1-3 自检
mem_heap_selftest()          heap（需要 PMM）
mem_pgtbl_selftest()         页表池证据
WP1-4 selftest PASS
```

`mem_boot_init()` 失败时仍会跑 `pg_init()`（它会因为申请不到页表页而报
FAIL），但 heap/pgtbl 自检跳过，避免拿 0 地址扩堆。

## 6. host 单测（64 位化）

`tools/unit-ng.ps1` 用本机 clang 编译成 Windows PE 并直接运行，不依赖 QEMU：

| 源码 | 覆盖 |
|---|---|
| `pmm.c` + `test_pmm.c` | 8 MiB fake memory、保留页、单页/连续页、double-free、越界/未对齐、清零、失败计数 |
| `heap.c` + `test_heap.c` | fake 页分配器、first-fit、合并、对齐、realloc、double-free、野指针、扩堆、失败回滚 |
| `memmap.c` + `test_memmap.c` | 合成 PVH（含 1 GiB 外条目）/坏 PVH / 合成 multiboot2 /坏 total_size/tag |

输出格式固定为 `N checks, M failures`；`unit-ng.ps1` 要求 M=0 且 N≥20，
当前 **166 checks, 0 failures**。

## 7. 边界 / 遗留

- 单核、IF=0 的早期内核：PMM/heap 没有锁（SMP/抢占在 WP1-5 之后再加）。
- PMM 只管理 1 GiB；RAM 更大时超出部分不可用（页表也只恒等映射 1 GiB）。
- 位图/堆/页表都没有回收策略：`pmm_free_page` 可用，但内核还没有“释放页表
  子树”的路径；heap region 只增不减。
- 没有 COW 引用计数、swap、NUMA、内存 cgroup；这些随用户态/调度再定。
- multiboot2 路径只有 host 单测（QEMU `-kernel` 走 PVH）；GRUB 实机未验。
- heap 上限：16 个 region、单次分配 ≤ 1 GiB；没有最大堆大小配置。
