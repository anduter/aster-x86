# ng-WP1-3：IDT / 可恢复异常 / TSS+IST / syscall + ring3（PASS）

## 结论

**PASS** —— ng 内核在 64 位长模式下装好 256 项 IDT（异常 0–31 全部有独立
handler，带/不带 error code 分开；#DF 用 TSS.IST1 独立栈），实现了可恢复的
受控异常探测：

- 写只读 `.rodata` → `#PF` 捕获，handler 打印 `cr2/error/rip` 并跳过故障指令，
  随后 `ng: #pf ro write handled ok`；
- `ud2` → `#UD` 捕获并恢复，`ng: #ud handled ok`；
- 加载 GDT 之外的段选择子 → `#GP` 捕获并恢复，`ng: #gp handled ok`；
- 未命中恢复条件的异常走 `PANIC: vec=... err=... rip=... cr2=...` 后 `hlt`
  （负向验证见下文）。

syscall 侧置好 `EFER.SCE`/`STAR`/`LSTAR`/`FMASK`：ring0 往返
`ng: syscall ring0 ok`；**ring3 stretch 完成**——用户页（U/S=1、R-X）、
4 KiB 用户栈、`TSS.RSP0`、`iretq` 到 CPL3，`syscall` 进内核打印后 `sysretq`
回 ring3，第二次 `syscall` 再回内核，打印 `ng: ring3 syscall ok`。

`tools/build-ng.ps1`（9 步多源编译）与 `tools/run-ng.ps1` 均返回 0；旧 32 位
`build.ps1 -Test` 保持 **305/305**。

## 交付物

| 路径 | 内容 |
|---|---|
| `src/ng/isr.S` | 32 个异常 stub（有/无 error code）、公共栈帧、`iretq`；受控探测 stub；`syscall_entry`（LSTAR）、`ring3_enter`、`gdt_load`（**新**） |
| `src/ng/gdt.c` / `gdt.h` | 7 槽 GDT（内核/用户段 + TSS）、TSS64、RSP0 16 KiB / IST1 8 KiB、`ltr`（**新**） |
| `src/ng/trap.c` / `trap.h` | 256 项 IDT、异常分发、`#PF` 诊断、恢复判定、panic、受控自检（**新**） |
| `src/ng/syscall.c` / `syscall.h` | STAR/LSTAR/FMASK/EFER.SCE、调用号 0/1、ring0 往返、用户页映射 + ring3 往返（**新**） |
| `src/ng/io.h` | `read_cr2()`、`cpu_cli()`、`read_tr()`（**改**） |
| `src/ng/paging.c` / `paging.h` | `pg_map` 对 `PG_USER` 逐级补 U 位；暴露 `pg_nx/smep/smap_on()`（**改**） |
| `src/ng/main.c` | 顺序改为 页表 → GDT/TSS → IDT → syscall → 自检；`-DNG_PANIC_TEST` 负向入口（**改**） |
| `tools/build-ng.ps1` | 汇编源列表 + 6 个 C 源；`-PanicTest`（**改**） |
| `tools/run-ng.ps1` | 12 条期望行 + 3 条正则；`-Panic` 负向模式（**改**） |
| `docs/design/trap-syscall-ng.md` | IDT/TSS/IST 布局、异常帧、syscall ABI、ring3 流程（**新**） |
| `docs/design/decisions.md` | ADR-015（**追加**） |
| `docs/design/paging-ng.md` | 第 8 节交叉引用 WP1-3 已实现（**改**） |

## 环境

- 分支 `ng`，基线 HEAD `dfe3a2c`（WP1-2）；仓库 `D:\OS_Project`。
- clang 23.1.4 / ld.lld（`D:\msys64\mingw64\bin`），`--target=x86_64-elf
  -ffreestanding -nostdlib -mno-red-zone -fno-stack-protector`。
- QEMU 11.0.0（`v11.0.0-12122-ga4bb4b10c9`），`qemu-system-x86_64 -kernel`，
  `-cpu max`（默认）与 `-cpu qemu64`（SMEP/SMAP=off 路径）。

## 证据（2026-10-31）

### 构建

```text
powershell -File tools/build-ng.ps1
```

```text
=== build-ng (WP1-3) ===
clang: D:\msys64\mingw64\bin\clang.exe
lld:   D:\msys64\mingw64\bin\ld.lld.exe
  [1/9] entry.S
  [2/9] isr.S
  [3/9] serial.c
  [4/9] paging.c
  [5/9] gdt.c
  [6/9] trap.c
  [7/9] syscall.c
  [8/9] main.c
  [9/9] link -> dist-ng/aster-ng.elf

ELF64: PASS (34512 bytes)
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
=== run-ng (WP1-3) ===
elf:  D:\OS_Project\dist-ng\aster-ng.elf (34512 bytes)
qemu: D:\Program Files\qemu\qemu-system-x86_64.exe
      QEMU emulator version 11.0.0 (v11.0.0-12122-ga4bb4b10c9)
args: -kernel <elf> -serial file:<log> -display none -no-reboot -m 256 -cpu max (timeout 20s)
run-ng: serial output:
------------------------------------------------
ng: hello 64-bit
ng: cpu nx=1 smep=1 smap=1 (cpuid max=0xd ext=0x80000021)
ng: pml4=0x113000 tables=4 pool=24 (4-level, .bss)
ng: tree pml4[0]=0x114023 pdpt[0]=0x115023 pd[0]=0x116023
ng: seg note 0x100000..0x101000 pages=1 pte=0x8000000000100001 W=0 NX=1
ng: seg text 0x101000..0x105000 pages=4 pte=0x101021 W=0 NX=0
ng: seg rodata 0x105000..0x106000 pages=1 pte=0x8000000000105021 W=0 NX=1
ng: seg data 0x106000..0x107000 pages=1 pte=0x8000000000106003 W=1 NX=1
ng: seg bss 0x107000..0x137000 pages=48 pte=0x8000000000107003 W=1 NX=1
ng: 2m  0x200000..0x40000000 entries=511 P=1 W=1 NX=1
ng: cr0=0x80010011 cr4=0x300020 efer=0xd00 cr3=0x113000
ng: paging 4-level ok
ng: wx layout ok (text rx, rodata ro, data rw-nx)
ng: nx on
ng: smep=on smap=on
ng: helper map/unmap/protect ok
ng: WP1-2 selftest PASS
ng: gdt 7 entries (kcode/kdata/udata/ucode/tss), tr=0x28 gdt=0x12c000 tss=0x12c040
ng: idt 256 entries, 32 exception stubs base=0x1320d0
ng: tss rsp0=0x1300b0 ist1=0x1320b0 idt8.ist=1
ng: syscall msr star=0x13000800000000 lstar=0x1012b3 fmask=0x40700 efer.sce=1
ng: #pf cr2=0x105000 error=0x3 rip=0x10127b
ng: #pf ro write handled ok
ng: #ud handled ok
ng: #gp handled ok
ng: trap recovery pf=1 ud=1 gp=1 gp_err=0x40
ng: syscall write (ring0)
ng: syscall ring0 ok
ng: ring3 map code=0x40000000 pte=0x135005 stack=0x40100000 pte=0x8000000000136007
ng: syscall write (ring3)
ng: syscall exit test
ng: ring3 syscalls=2 user_rsp=1 exit=1
ng: ring3 syscall ok
ng: WP1-3 selftest PASS
------------------------------------------------
run-ng: matched 12/12 expected lines + 3 patterns
run-ng: PASS (all expected lines in 0.2s, cpu max)
```

`run-ng.ps1` 的期望（全部命中才 PASS）：

```text
12 条固定行：
  ng: hello 64-bit
  ng: paging 4-level ok
  ng: wx layout ok (text rx, rodata ro, data rw-nx)
  ng: nx on
  ng: WP1-2 selftest PASS
  ng: idt 256 entries, 32 exception stubs
  ng: #pf ro write handled ok
  ng: #ud handled ok
  ng: #gp handled ok
  ng: syscall ring0 ok
  ng: ring3 syscall ok
  ng: WP1-3 selftest PASS
3 条正则：
  ng: smep=<on|off> smap=<on|off>
  ng: tss rsp0=0x... ist1=0x... idt8\.ist=1
  ng: syscall msr star=0x... lstar=0x... fmask=0x...
```

### IDT / TSS / IST 布局（本次运行读回）

| 对象 | 地址/值 | 证据 |
|---|---|---|
| GDT | `0x12c000`，7 槽，limit `0x37` | `gdt=0x12c000` |
| TSS | `0x12c040`，104 字节，type=9，`tr=0x28` | `tss=0x12c040` / `tr=0x28` |
| TSS.RSP0 | `0x1300b0`（16 KiB 栈顶） | `rsp0=0x1300b0` |
| TSS.IST1 | `0x1320b0`（8 KiB 栈顶，给 #DF） | `ist1=0x1320b0` |
| IDT | `0x1320d0`，256 × 16 B，limit `0xfff` | `base=0x1320d0` |
| IDT[8].IST | `1` | `idt8.ist=1` |
| syscall 栈 | `0x10e000..0x112000`（16 KiB，静态） | `nm -n` / `isr.S` |
| 用户代码页 | VA `0x40000000` → PA `0x135000`，PTE `0x135005`（P|U，W=0，NX=0） | `ring3 map` |
| 用户栈页 | VA `0x40100000` → PA `0x136000`，PTE `0x8000000000136007`（P|W|U|NX） | `ring3 map` |

GDT 选择子：`0x08` 内核代码（L=1,DPL0）、`0x10` 内核数据、`0x18` 用户数据
（DPL3）、`0x20` 用户代码（DPL3,L=1）、`0x28` TSS。用户数据放在用户代码
前一个槽，是 `SYSRET`（CS=base+16、SS=base+8）的硬约束。

IDT 异常 0–31：`#DF/#TS/#NP/#SS/#GP/#PF/#AC/#CP/#VC/#SX` 不补 error code，
其余 stub 先压 0；`isr_common` 统一成
`[GPR×15][vector][error][rip][cs][rflags][rsp][ss]` 后分发。

### syscall MSR（读回）

| MSR | 值 | 说明 |
|---|---|---|
| `EFER` | `0xd01` | `SCE(0)=1` + `LMA(10)` + `LME(8)` + `NXE(11)` |
| `STAR` | `0x13000800000000` | SYSCALL CS=0x08/SS=0x10；SYSRET CS=0x23/SS=0x1b |
| `LSTAR` | `0x1012b3` | `syscall_entry` |
| `FMASK` | `0x40700` | 清 TF/IF/DF/AC |

### 可恢复异常细节

本次运行的 `#PF` 诊断行：

```text
ng: #pf cr2=0x105000 error=0x3 rip=0x10127b
```

- `cr2=0x105000` = `.rodata` 页首（`__rodata_start`），`error=0x3`
  = P=1（页存在）+ W=1（写访问）+ U=0（ring0），说明是“ring0 写只读页”；
- `rip=0x10127b` = `probe_ro_write` 的 `movq $1,(%rdi)`；
- handler 把 `frame->rip` 改成 `probe_ro_write_after`，`iretq` 后从 `ret` 继续，
  内存未被改写（`.rodata` 仍只读，后续页表自检照常 PASS）。

`#UD`（`probe_ud2`）与 `#GP`（`mov %eax,%ds`，选择子 `0x40`）同样按
“向量 + 故障 RIP”命中恢复；`gp_err=0x40` 是 #GP 的 error code（选择子
索引）。三次探测后 `trap recovery pf=1 ud=1 gp=1`。

### panic 负向验证

`tools/build-ng.ps1 -PanicTest` 在全部正常自检后执行一条不可恢复的 `ud2`；
`tools/run-ng.ps1 -Panic` 期望 panic 行：

```text
ng: panic selftest: triggering unrecoverable #UD
PANIC: vec=0x6 err=0x0 rip=0x103ff3 cr2=0x105000
------------------------------------------------
run-ng: matched 1/1 expected lines + 1 patterns
run-ng: PASS (panic path observed in 0.2s, cpu max)
```

panic 后 `cli; hlt`。`cr2` 对非 #PF 是上次缺页的残留值（架构行为），仅作诊断。
负向跑完重新执行普通 `build-ng.ps1`，`dist-ng/aster-ng.elf` 回到正常镜像。

### SMEP/SMAP=off 路径（`-cpu qemu64`）

```text
powershell -File tools/run-ng.ps1 -Cpu qemu64
```

```text
ng: cpu nx=1 smep=0 smap=0 (cpuid max=0xd ext=0x8000000a)
ng: cr0=0x80010011 cr4=0x20 efer=0xd00 cr3=0x113000
ng: smep=off smap=off
...
ng: #pf ro write handled ok
ng: #ud handled ok
ng: #gp handled ok
ng: syscall ring0 ok
ng: ring3 syscall ok
ng: WP1-3 selftest PASS
run-ng: PASS (all expected lines in 0.2s, cpu qemu64)
```

SMAP=off 时 `ng_clac_enabled=0`，异常入口跳过 `clac`（否则 CLAC 在
qemu64 上是 #UD）；ring3 照常完成。SMEP=on 的 `-cpu max` 下仍能执行
`0x40000000` 的 U/S=1 页，证明 ring3 确实在 CPL3（SMEP 只允许 CPL3 执行
用户页）。

### ring3 往返证据

```text
ng: ring3 map code=0x40000000 pte=0x135005 stack=0x40100000 pte=0x8000000000136007
ng: syscall write (ring3)
ng: syscall exit test
ng: ring3 syscalls=2 user_rsp=1 exit=1
```

- 代码页 PTE `0x135005` = P|U（W=0、NX=0）→ 用户可读可执行；
- 栈页 PTE `0x8000000000136007` = NX|P|W|U → 用户可写、不可执行；
- 两次 syscall 的进入 RSP 都落在用户栈区间（`user_rsp=1`）；
- 第一次 `syscall` 用 `sysretq` 回到 CPL3 的 `mov $1,%eax`，第二次
  （`rax=1`）由 handler 置退出标志、入口跳回内核协程；`ring3 syscall ok`。

## 自检项清单

1. GDT 7 槽、`tr=0x28`；TSS.RSP0/IST1 非 0；`IDT[8].IST=1`。
2. IDT 256 项全部 present（0x8E 中断门、selector 0x08），0–31 独立 stub，
   有/无 error code 分开。
3. `#PF`：写 `.rodata` 被捕获，`CR2`/`error=0x3`/`RIP` 正确，恢复后继续。
4. `#UD`：`ud2` 被捕获并跳过。
5. `#GP`：坏段选择子 `0x40` 被捕获（error code 0x40），DS 未被破坏。
6. 未命中恢复条件的异常进 `PANIC` 并 `hlt`（`-PanicTest` 负向）。
7. syscall MSR 读回与预期一致；ring0 `syscall` 往返一次。
8. ring3：用户页 U/S=1、`iretq` 进 CPL3、两次 `syscall`、
   第一次 `sysretq` 回 ring3、第二次回内核。

## 旧树回归与边界检查

```text
powershell -File build.ps1 -Test
=== compile: 1 compiled, 103 skipped ===
=== elapsed: 49.8s ===
=== TEST PASS (305/305) ===
```

```text
powershell -File tools/check-domains.ps1
checked: 205 files, 726 quoted includes
domain include violations: 0

powershell -File tools/check-deps.ps1
OK: no cross-module include violations.
```

未修改 `src/kernel/**`、`build.ps1`、`tools/ci.ps1`、`abi.h`。

## 未做 / 遗留（按 WP 边界）

- **PMM/heap**：页表页仍来自 `.bss` 静态池（24 页，WP1-3 用 6 页 + 用户页
  映射复用 PDPT[1] 的既有表），用户页是静态 4 KiB 页：WP1-4。
- **调度/线程**：syscall 内核栈、RSP0、IST 都是单线程静态栈，不可重入/不可
  并发：WP1-5。
- **用户态异常投递**：ring3 的 #PF/#GP 目前进 panic，不投递信号，也不做
  `copyin/copyout`（没有用户指针进内核）：WP1-7。
- **ELF64 加载器**：ring3 程序是内核 `.rodata` 里的机器码 blob（`mov/syscall/
  hlt`），不是从文件加载的 ELF：WP1-7。
- **IRQ**：IDT[32..255] 是默认 stub，IF=0，没有 PIC/APIC 初始化。
- **#DF 主动触发**：只验证 IST1 配置（IDT[8].IST=1、TSS.IST1 非 0）；
  主动制造双重故障会破坏当前上下文且不可恢复，留给故障注入 WP3-5/E4。
- **同 CPL `iretq`**：本机 QEMU 实测同 CPL iret 也会弹 5 项（SDM 语义是 3 项），
  所以 ring0 测试返回不依赖它；ring3 的 iret 本来就是完整 5 项帧，不受影响。
- syscall 参数寄存器 `rdi/rsi/rdx` 尚未使用；调用号只有 0/1，ABI 1.0 在
  WP4-1 冻结。

## 出口

```text
powershell -File tools/build-ng.ps1   # PASS，产出 dist-ng/aster-ng.elf（34512 bytes）
powershell -File tools/run-ng.ps1     # PASS，12 条期望行 + 3 条正则全部命中
powershell -File build.ps1 -Test      # PASS，305/305（旧 32 位）
```
