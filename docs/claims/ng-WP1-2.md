# ng-WP1-2：4 级页表 + W^X/NX + SMEP/SMAP（PASS）

## 结论

**PASS** —— ng 内核在长模式下建好 4 级页表（PML4/PDPT/PD/PT，4 KiB 页 + 2 MiB
大页混合），按段给 `.text` R-X、`.rodata` RO+NX、`.data/.bss` RW+NX；`EFER.NXE`
置位、数据页 `PTE.NX=1`；CPUID 检测后置 `CR4.SMEP/SMAP`（不支持则打印 off，不
panic）。自检不依赖 trap：读回 CR0/CR4/EFER/CR3 + 逐页走表断言 + helper 行为
测试，全部通过后打印 `ng: WP1-2 selftest PASS`。`tools/build-ng.ps1`、
`tools/run-ng.ps1` 均返回 0，旧 32 位 `build.ps1 -Test` 保持 305/305。

## 交付物

| 路径 | 内容 |
|---|---|
| `src/ng/entry.S` | 引导页表（1 GiB / 2 MiB）、`EFER.LME+NXE` 在开分页前置位、64 位入口（**改**） |
| `src/ng/paging.c` | 4 级页表建立、段权限、NX、SMEP/SMAP、`pg_map/pg_unmap/pg_protect/pg_walk`、全部自检（**新**） |
| `src/ng/paging.h` | PTE 标志位、错误码、helper 原型（**新**） |
| `src/ng/io.h` | 端口 IO、CR0/CR3/CR4、MSR、CPUID、`invlpg`、`stac/clac`（**新**） |
| `src/ng/serial.c` / `serial.h` | 从 WP1-1 的 main.c 拆出的 COM1 输出 + hex/dec 打印（**新**） |
| `src/ng/types.h` | 定宽类型（**新**） |
| `src/ng/main.c` | `kmain`：串口 → `pg_init()` → PASS/FAIL 判定（**改**） |
| `src/ng/link.ld` | `.text/.rodata/.data/.bss` 4 KiB 对齐、段边界符号、`__kernel_end <= 2 MiB` 断言（**改**） |
| `tools/build-ng.ps1` | 多源编译、PT_LOAD 4 KiB 对齐校验，保留 ELF64/启动头校验（**改**） |
| `tools/run-ng.ps1` | 多期望行匹配、`-Cpu` 参数（默认 `max`，可测 qemu64 的 off 路径）（**改**） |
| `docs/design/paging-ng.md` | 页表布局、PTE 标志、初始化顺序、helper 语义、stac/clac 约定（**新**） |
| `docs/design/decisions.md` | ADR-014（**追加**） |

## 环境

- 分支 `ng`，基线 HEAD `4834434`（WP1-1）；仓库 `D:\OS_Project`。
- clang 23.1.4 / ld.lld（`D:\msys64\mingw64\bin`），`--target=x86_64-elf
  -ffreestanding -nostdlib -mno-red-zone -fno-stack-protector`。
- QEMU 11.0.0（`v11.0.0-12122-ga4bb4b10c9`），`qemu-system-x86_64 -kernel`。
- `-cpu max`（默认，含 SMEP/SMAP）；`-cpu qemu64` 用于验证"CPU 不支持"分支。

## 证据（2026-10-31）

### 构建

```text
powershell -File tools/build-ng.ps1
```

```text
=== build-ng (WP1-2) ===
clang: D:\msys64\mingw64\bin\clang.exe
lld:   D:\msys64\mingw64\bin\ld.lld.exe
  [1/5] entry.S
  [2/5] serial.c
  [3/5] paging.c
  [4/5] main.c
  [5/5] link -> dist-ng/aster-ng.elf

ELF64: PASS (27096 bytes)
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

### ELF 段（`readelf -l`）

```text
  LOAD  0x1000 0x00100000 0x00100000 0x002c 0x002c R     0x1000   .multiboot .note.Xen
  LOAD  0x2000 0x00101000 0x00101000 0x20c3 0x20c3 R E   0x1000   .text
  LOAD  0x5000 0x00104000 0x00104000 0x0581 0x0581 R     0x1000   .rodata
  LOAD  0x6000 0x00105000 0x00105000 0x0008 0x22000 RW   0x1000   .data .bss
  GNU_STACK                                       RW    0x0      （栈不执行）
  NOTE  0x1018 0x00100018 0x00100018 0x0014 0x0014 R     0x4      PVH 入口
```

段边界符号（`nm -n`，页对齐即权限分界）：

```text
__image_start = 0x100000
__text_start  = 0x101000   __text_end   = 0x104000
__rodata_start= 0x104000   __rodata_end = 0x105000
__data_start  = 0x105000   __data_end   = 0x106000
__bss_start   = 0x106000   __bss_end    = 0x127000
__kernel_end  = 0x127000   （< 0x200000，link.ld ASSERT 兜底）
```

### 串口（`powershell -File tools/run-ng.ps1`，`-cpu max`）

```text
=== run-ng (WP1-2) ===
elf:  D:\OS_Project\dist-ng\aster-ng.elf (27096 bytes)
qemu: D:\Program Files\qemu\qemu-system-x86_64.exe
      QEMU emulator version 11.0.0 (v11.0.0-12122-ga4bb4b10c9)
args: -kernel <elf> -serial file:<log> -display none -no-reboot -m 256 -cpu max (timeout 20s)
run-ng: serial output:
------------------------------------------------
ng: hello 64-bit
ng: cpu nx=1 smep=1 smap=1 (cpuid max=0xd ext=0x80000021)
ng: pml4=0x10e000 tables=4 pool=24 (4-level, .bss)
ng: tree pml4[0]=0x10f023 pdpt[0]=0x110023 pd[0]=0x111023
ng: seg note 0x100000..0x101000 pages=1 pte=0x8000000000100001 W=0 NX=1
ng: seg text 0x101000..0x104000 pages=3 pte=0x101021 W=0 NX=0
ng: seg rodata 0x104000..0x105000 pages=1 pte=0x8000000000104021 W=0 NX=1
ng: seg data 0x105000..0x106000 pages=1 pte=0x8000000000105003 W=1 NX=1
ng: seg bss 0x106000..0x127000 pages=33 pte=0x8000000000106003 W=1 NX=1
ng: 2m  0x200000..0x40000000 entries=511 P=1 W=1 NX=1
ng: cr0=0x80010011 cr4=0x300020 efer=0xd00 cr3=0x10e000
ng: paging 4-level ok
ng: wx layout ok (text rx, rodata ro, data rw-nx)
ng: nx on
ng: smep=on smap=on
ng: helper map/unmap/protect ok
ng: WP1-2 selftest PASS
------------------------------------------------
run-ng: matched 5/5 expected lines + smep/smap line
run-ng: PASS (all expected lines in 0.2s, cpu max)
```

`run-ng.ps1` 的期望行（全部命中才 PASS，缺一行即打印 `missing line:` 并 exit 1）：

```text
ng: hello 64-bit
ng: paging 4-level ok
ng: wx layout ok (text rx, rodata ro, data rw-nx)
ng: nx on
ng: smep=<on|off> smap=<on|off>
ng: WP1-2 selftest PASS
```

### 页表实体（本次运行，串口 `ng: tree` 行读回）

| 对象 | 物理/虚拟地址 | 读回的表项 |
|---|---|---|
| PML4 | `0x10e000`（`pt_pool[0]`） | `[0] = 0x10f023` → PDPT（P\|W\|A）；`[256..511] = 0` |
| PDPT | `0x10f000`（`pt_pool[1]`） | `[0] = 0x110023` → PD；`[1..511] = 0` |
| PD | `0x110000`（`pt_pool[2]`） | `[0] = 0x111023` → PT（PS=0）；`[1..511]` = 2 MiB 大页 `|PS|RW|NX` |
| PT | `0x111000`（`pt_pool[3]`） | 512 × 4 KiB 页，按段覆盖权限（见下表） |

页表池 `pt_pool` 在 `.bss`、恒等映射（虚拟 == 物理）；`tables=4` 是本次建表
消耗的池页数（PML4/PDPT/PD/PT）。helper 自测会再按需分配 2 页（PDPT[1] 的
PD/PT），发生在这些断言之后。

PTE 值解码（`0x8000000000100001` = NX | 0x100000 | P；`0x...003` = NX|RW|P）：

| 段 | 页数 | 首 PTE | 位 | 权限 |
|---|---|---|---|---|
| note（`.multiboot`+`.note.Xen`） | 1 | `0x8000000000100001` | P=1 W=0 NX=1 | RO |
| text（`.text`） | 3 | `0x0000000000101021` | P=1 W=0 NX=0 | R-X |
| rodata | 1 | `0x8000000000104021` | P=1 W=0 NX=1 | RO |
| data | 1 | `0x8000000000105003` | P=1 W=1 NX=1 | RW |
| bss（页表池/栈） | 33 | `0x8000000000106003` | P=1 W=1 NX=1 | RW |
| 其余 RAM 0..1 MiB | 256 | RW+NX | | RW |
| 2 MiB 大页 2 MiB..1 GiB | 511 | `i*2MiB | P|W|PS|NX` | | RW |

### 控制寄存器 / EFER（读回值）

| 寄存器 | 值 | 置位 |
|---|---|---|
| CR0 | `0x80010011` | PG(31) + WP(16) + ET(4) + PE(0) |
| CR4 | `0x300020` | SMAP(21) + SMEP(20) + PAE(5) |
| EFER | `0xd00` | NXE(11) + LMA(10) + LME(8) |
| CR3 | `0x10e000` | == 新 PML4 物理地址，4 KiB 对齐 |

### SMEP/SMAP 的"不支持"分支（`-cpu qemu64`）

```text
powershell -File tools/run-ng.ps1 -Cpu qemu64
```

```text
ng: cpu nx=1 smep=0 smap=0 (cpuid max=0xd ext=0x8000000a)
ng: pml4=0x10e000 tables=4 pool=24 (4-level, .bss)
ng: tree pml4[0]=0x10f023 pdpt[0]=0x110023 pd[0]=0x111023
...
ng: cr0=0x80010011 cr4=0x20 efer=0xd00 cr3=0x10e000
ng: paging 4-level ok
ng: wx layout ok (text rx, rodata ro, data rw-nx)
ng: nx on
ng: smep=off smap=off
ng: helper map/unmap/protect ok
ng: WP1-2 selftest PASS
...
run-ng: PASS (all expected lines in 0.2s, cpu qemu64)
```

`cr4=0x20` 只剩 PAE：CPU 不报 SMEP/SMAP 时**不置位、不 panic**，打印 off，
其余自检照常通过。

### 负向验证（自检不是空跑）

把 `.text` 的权限临时改成 `PG_WRITE`（只改一行、不提交）后重新构建运行：

```text
ng: CHECK FAIL text
ng: seg text 0x101000..0x104000 pages=3 pte=0x101023 W=1 NX=0
...
ng: wx layout FAIL (text rx, rodata ro, data rw-nx)
...
ng: WP1-2 selftest FAIL (1)
------------------------------------------------
run-ng: missing line: ng: wx layout ok (text rx, rodata ro, data rw-nx)
run-ng: missing line: ng: WP1-2 selftest PASS
run-ng: FAIL (kernel reported ng: WP1-2 selftest FAIL)
```

`run-ng.ps1` 返回 1；恢复后重新 PASS。

## 自检项清单（不依赖 trap）

1. `CR0.PE/PG/WP=1`、`CR4.PAE=1`、`EFER.LMA=1`、`CR3 == 新 PML4 物理地址`、
   `PML4[0]` present、`PML4[256..511]` 全空。
2. 4 级结构：`PML4[0]`/`PDPT[0]` 是表指针（PS=0），`PD[0]` 是 PT（4 KiB）。
3. 每个段逐页：present、非 2 MiB 叶项、W/NX 与期望一致（段边界页对齐）。
4. `PD[1..511]`：511 个 2 MiB 恒等大页，RW+NX；`PD[0]` 不是大页。
5. helper：在 1 GiB（恒等映射之外，PDPT[1]）`pg_map` → 写读模式 → 重复 map 返回
   `PG_EEXIST` → `pg_protect` 清 W（读回 W=0，仍可读）→ `pg_unmap` → PTE 归零 →
   再 unmap 返回 `PG_ENOENT`。
6. `.data` 的 `volatile` 已初始化变量读回 + 改写（`PG_BOOT_MARKER`）。

**不做的**：写只读页/执行数据页的故障注入——那需要 #PF 处理器，属 WP1-3。

## CPU 特性

| 特性 | CPUID | `-cpu max` | `-cpu qemu64` |
|---|---|---|---|
| NX | `80000001h:EDX[20]` | 1 → `EFER.NXE=1` | 1 → `EFER.NXE=1` |
| SMEP | `(7,0):EBX[7]` | 1 → `CR4.SMEP=1` | 0 → off |
| SMAP | `(7,0):EBX[20]` | 1 → `CR4.SMAP=1` | 0 → off |

SMAP 下 ring0 访问用户内存的约定（`stac`/`clac`、范围校验、异常入口 `clac`、
syscall `SFMASK` 清 AC）写在 `docs/design/paging-ng.md` 第 7 节与 ADR-014；
`src/ng/io.h` 已提供 `stac()`/`clac()` inline helper，本 WP 没有用户页，未执行。

## 旧树回归与边界检查

```text
powershell -File build.ps1 -Test
=== compile: 1 compiled, 103 skipped ===
=== elapsed: 39.4s ===
=== TEST PASS (305/305) ===
[status: completed, exit code: 0]
```

```text
powershell -File tools/check-domains.ps1
checked: 205 files, 726 quoted includes
domain include violations: 0

powershell -File tools/check-deps.ps1
OK: no cross-module include violations.
```

未修改 `src/kernel/**`、`build.ps1`、`tools/ci.ps1`、`abi.h`；`build.ps1` 只扫描
`src/kernel`，`src/ng` 的改动不进入旧对象列表。

## 未做 / 遗留（按 WP 边界）

- #PF/#GP/IDT/trap、故意触发保护异常的测试：WP1-3。当前没有异常处理，任何
  保护违规都会三重故障。
- PMM/heap、页表页回收、2 MiB 大页拆分（`pg_map` 落在 2 MiB 大页里返回
  `PG_EBIGPAGE`）、`.bss` 静态池（24 页）扩容：WP1-4。
- 用户页（U/S=1）、ring3、ELF64 加载器：WP1-7；SMEP 要等用户页出现才真正生效。
- 1 GiB 大页（PDPT.PS）、PWT/PCD/MMIO 缓存属性、higher-half 映射：暂无需求。
- multiboot2/PVH 内存图未解析；恒等映射固定 1 GiB，实际 RAM 大小未使用。
- 引导页表（`boot_pml4/boot_pdpt/boot_pd`，3 页）切换后不再使用，留在 `.bss`
  未回收（WP1-4 一并处理）。

## 出口

```text
powershell -File tools/build-ng.ps1   # PASS，产出 dist-ng/aster-ng.elf（27096 bytes）
powershell -File tools/run-ng.ps1     # PASS，6 类期望行全部命中
```
