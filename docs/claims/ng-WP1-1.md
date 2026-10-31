# ng-WP1-1：64 位 boot + long mode + 串口 hello（PASS）

## 结论

**PASS** —— `src/ng` 的新内核从 32 位入口启动，建 1 GiB 恒等页表、开 PAE/LME/PG、far jump 进 64 位，64 位 C 入口从 COM1 打印 `ng: hello 64-bit` 后 `hlt` 停住；`tools/run-ng.ps1` 返回 0。

## 交付物

| 路径 | 内容 |
|---|---|
| `src/ng/entry.S` | multiboot2 header + Xen PVH note、32 位入口、GDT、页表、PAE/LME/PG、far jump、64 位段/栈 |
| `src/ng/main.c` | 64 位 C 入口：COM1 115200 8N1，打印 hello，`hlt` 循环 |
| `src/ng/link.ld` | 链接到 `0x100000`；MB2/note 在前，`.bss` 放页表与启动栈 |
| `tools/build-ng.ps1` | 编译/链接 + ELF64/启动头校验，产出 `dist-ng/aster-ng.elf` |
| `tools/run-ng.ps1` | QEMU `-kernel` 启动，轮询串口，PASS/FAIL |
| `docs/design/decisions.md` | ADR-013：QEMU 上 ELF64 用 PVH note 的理由 |

## 启动流程

1. 引导器进入 32 位保护模式 `_start`（QEMU 走 PVH note；GRUB 走 MB2 header，二者同址）。
2. `cli/cld`；保存 EBX（multiboot2 info / PVH hvm_start_info，本 WP 不解析）。
3. CPUID `80000001h:EDX[29]` 检查长模式；不支持则 `hlt`。
4. `lgdt` 后用 far jump 到 `0x08` 刷新 CS，再加载 32 位数据段 `0x10`。
5. 清零并填写 PML4/PDPT/PD：`PML4[0] -> PDPT[0] -> PD`，`PD[0..511]` 为 2 MiB 页，恒等映射 `0..1 GiB`。
6. `CR4.PAE=1`；`CR3=PML4`；`EFER.LME=1`；`CR0.PG=1`。
7. `ljmp $0x18` 进入 64 位代码段；加载数据段 `0x20`、`RSP=boot_stack_top`。
8. `call kmain`（RDI = 引导器信息指针，暂未使用）。
9. COM1 输出 `ng: hello 64-bit`，进入 `hlt` 循环。

## 环境

- 分支 `ng`，基线 HEAD `41b8769`；仓库 `D:\OS_Project`。
- clang 23.1.4（`D:\msys64\mingw64\bin\clang.exe`），LLD 23.1.4。
- QEMU：`qemu-system-x86_64.exe` 11.0.0（`v11.0.0-12122-ga4bb4b10c9`，`D:\Program Files\qemu`）。
- 编译参数：`--target=x86_64-elf -ffreestanding -nostdlib -mno-red-zone -fno-stack-protector`
  （C 另加 `-mno-sse -mgeneral-regs-only -fno-builtin -O2 -Wall -Wextra -Werror`）。

## 证据（2026-10-31）

### 构建

```text
powershell -File tools/build-ng.ps1
```

```text
=== build-ng (WP1-1) ===
clang: D:\msys64\mingw64\bin\clang.exe
lld:   D:\msys64\mingw64\bin\ld.lld.exe
  [1/3] entry.S
  [2/3] main.c
  [3/3] link -> dist-ng/aster-ng.elf

ELF64: PASS (9296 bytes)
  magic    7f 45 4c 46
  class    2 (ELF64)
  machine  62 (EM_X86_64)
  type     2 (ET_EXEC)
  entry    0x0000000000100030
  phnum    6
multiboot2: PASS
  header   file offset 0x1000, length 24, checksum 0, 8-byte aligned
pvh-note: PASS
  entry    0x0000000000100030 (PT_NOTE, Xen type 18; QEMU -kernel)

build-ng: PASS -> D:\OS_Project\dist-ng\aster-ng.elf
```

### ELF 头（`readelf -h -l -n dist-ng/aster-ng.elf`）

```text
Class:                             ELF64
Data:                              2's complement, little endian
Type:                              EXEC (Executable file)
Machine:                           Advanced Micro Devices X86-64
Entry point address:               0x100030
Number of program headers:         6

  LOAD  0x1000 0x100000 0x100000 0x002c 0x002c R      <- .multiboot + .note.Xen
  LOAD  0x1030 0x100030 0x100030 0x0153 0x0153 R E    <- .text
  LOAD  0x1188 0x100188 0x100188 0x0045 0x0045 R      <- .rodata (GDT)
  LOAD  0x2000 0x101000 0x101000 0x0000 0x7000 RW     <- .bss (页表 + 16 KiB 栈)
  NOTE  0x1018 0x100018 0x100018 0x0014 0x0014 R

Displaying notes found in: .note.Xen
  Owner    Data size  Description
  Xen      0x00000004 Unknown note type: (0x00000012)   <- 18 = XEN_ELFNOTE_PHYS32_ENTRY
  description data: 30 00 10 00                           <- 0x100030 = _start
```

校验点：ELF magic/`ELFCLASS64`/`EM_X86_64`/`ET_EXEC`；入口 `0x100030`；multiboot2 header 在文件偏移 `0x1000`、长度 24、checksum 为 0、8 字节对齐（前 32 KiB 内）；PT_NOTE 中的 PVH 入口与 ELF `e_entry` 一致。

### QEMU / 串口

```text
powershell -File tools/run-ng.ps1
```

```text
=== run-ng (WP1-1) ===
elf:  D:\OS_Project\dist-ng\aster-ng.elf (9296 bytes)
qemu: D:\Program Files\qemu\qemu-system-x86_64.exe
      QEMU emulator version 11.0.0 (v11.0.0-12122-ga4bb4b10c9)
args: -kernel <elf> -serial file:<log> -display none -no-reboot -m 256 (timeout 20s)
run-ng: serial output:
------------------------------------------------
ng: hello 64-bit
------------------------------------------------
run-ng: PASS (found 'ng: hello 64-bit' in 0.1s)
```

`run-ng.ps1` 返回 0；失败路径实测返回 1：非 ELF 输入会打印 QEMU stderr（`qemu: invalid kernel header`），`-TimeoutSec 0` 会走超时分支并打印 `timeout 0s, marker ... not found`。

## 关键取舍：QEMU 上的 ELF64 启动路径

WP1-1 要求“multiboot2 头 + ELF64”且用 `qemu-system-x86_64 -kernel` 启动。实测 QEMU 11 的 multiboot 加载器只接受 32 位 ELF：

- 只带 multiboot2 头的 ELF64：QEMU 走 Linux 加载器，报 `Error loading uncompressed kernel without PVH ELF Note`；
- 只带 multiboot1 头的 ELF64：QEMU 报 `Cannot load x86-64 image, give a 32bit one.`。

因此镜像同时携带：

1. **multiboot2 header**（GRUB 等 multiboot2 引导器）；
2. **Xen PVH note**（`XEN_ELFNOTE_PHYS32_ENTRY` = 18，QEMU `-kernel` 的 ELF64 入口）。

两者都指向同一个 32 位 `_start`；进入状态一致（32 位保护模式、分页关闭、平坦段）。`src/ng` 仍是“multiboot2 头 + 32 位入口 → long mode”的结构，QEMU 只是一个不读 MB2 的加载器。取舍记录见 `docs/design/decisions.md` ADR-013。

## 未做（严格按 WP1-1 边界）

- trap/IDT、syscall、PMM/heap、调度、ELF64 加载器：留给 WP1-2..WP1-7。
- 4 级页表保护（W^X/NX/SMEP/SMAP）：WP1-2；当前只有 1 GiB 2 MiB 页恒等映射。
- multiboot2 info / PVH start_info 未解析（`kmain` 收到指针但忽略）。
- CPUID 不支持长模式时只 `hlt`，没有早期串口诊断。
- GRUB/multiboot2 实机路径未实测（ADR-013）。
- `-serial file:` 轮询依赖 QEMU 及时写盘；当前实测启动后 0.1 s 内可见。

## 对旧树影响

- 未修改 `build.ps1`、`src/kernel/**`、`tools/ci.ps1`、`abi.h`；旧 32 位构建路径不变（`build.ps1` 只扫描 `src/kernel`，不会编译 `src/ng`）。
- `tools/check-domains.ps1`：0 违规（205 文件、726 quoted include）。
- `tools/check-deps.ps1`：0 违规（201 文件、650 quoted include）。
- 旧 32 位 `build.ps1 -Test` 回归结果见下方“旧树回归”。

## 出口

```text
powershell -File tools/build-ng.ps1   # PASS，产出 dist-ng/aster-ng.elf
powershell -File tools/run-ng.ps1     # PASS，串口出现 ng: hello 64-bit
```

## 旧树回归

同一会话里跑了旧 32 位构建与回归（ng 工作区，未改旧路径）：

```text
powershell -File build.ps1 -Test
...
=== compile: 104 compiled, 0 skipped ===
=== TEST PASS (305/305) ===
[status: completed, exit code: 0]
```

耗时 55.3 s；`src/ng` 不在 `build.ps1` 的扫描路径（它只递归 `src/kernel`），因此新增目录不会进入旧对象列表。
