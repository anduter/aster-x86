# ng-WP1-7：ELF64 加载器 + ring3 hello（PASS）

## 结论

**PASS** —— ng 内核新增了真正的 ELF64 加载路径：`elf.c` 校验
ELF64/ET_EXEC/EM_X86_64、e_phoff/e_phnum/PT_LOAD 边界、W^X、段重叠与入口，
产出段规划；`elf64_load()` 通过 `elf_env` 回调从 PMM 逐页分配、把文件字节
拷进用户页、把 `.bss`/页尾清零，再映射进**新建的 PML4 用户地址空间**
（U/S=1；PF_W→RW、PF_X→RX、其余 R+NX）；失败时已映射页全部 unmap+free。

用户程序 `src/ng/user/hello.S` 由 `user.ld` 链到 `0x40000000`，`build-ng.ps1`
先校验再 `.incbin` 嵌进内核 `.rodata`（不依赖 FS）。内核切 CR3 进用户 AS、
`iretq` 到 CPL3 执行 hello：用户态调 syscall 0 打印 **`ng: ring3 hello`**、
syscall 1 退出；内核随后打印 `ng: ring3 exec ok` / `ng: ring3 exit ok`。

`copyin/copyout`（`uaccess.c`）按当前 CR3 的 PML4 逐页校验 P/U/W，SMAP=on
时 `stac/clac` 包住拷贝，坏指针返回 `EFAULT`（null/内核地址/只读用户页三种
都验证过，不触发 #PF）；IPC 的用户指针路径已改为同一份实现。

per-thread 内核栈完成：调度器每次 `switch_context` 前把
`syscall_kernel_rsp0` 与 `TSS.RSP0` 同步到 next 的 TCB 栈（boot 线程回退
isr.S 静态栈），syscall 入口从该全局装载 RSP；自检用真实线程证明两个线程
的栈顶不同且 RSP0 跟着切换。

`tools/build-ng.ps1`（26 步）与 `tools/run-ng.ps1`（39 条期望行 + 11 条正则）
均 PASS；host 单测 **901 checks, 0 failures**（WP1-6 为 810，本 WP +91）；
旧 32 位 `build.ps1 -Test` 保持 **305/305**；`check-domains` / `check-deps`
0 违规。

## 交付物

| 路径 | 内容 |
|---|---|
| `src/ng/elf.h` / `elf.c` | ELF64 校验、段规划、env 回调加载、失败回滚（**新**） |
| `src/ng/useras.h` / `useras.c` | 独立 PML4、用户页/窗口、CR3 切换、AS 销毁（**新**） |
| `src/ng/uaccess.h` / `uaccess.c` | copyin/copyout/copyin_str、页级 P/U/W 校验 + SMAP（**新**） |
| `src/ng/elf_selftest.h` / `.c` | 内核自检、23 份坏镜像、段/PTE/栈/uaccess 证据行（**新**） |
| `src/ng/user/hello.S` / `user.ld` | ring3 示例（write+exit；.data 魔数与 .bss 清零自检）、0x40000000 链接脚本（**新**） |
| `src/ng/mem.c` | freestanding memcpy/memset/memmove（结构体赋值会发 memcpy 调用）（**新**） |
| `src/ng/paging.h` / `paging.c` | `pg_*_in()`、`pg_switch_pml4()`、`pg_current_pml4()`、`pg_free_table()`（**改**） |
| `src/ng/gdt.h` / `gdt.c` | `gdt_set_rsp0()`（**改**） |
| `src/ng/syscall.h` / `syscall.c` | write(buf,len)、exit code、`syscall_run_user`、per-thread 栈同步（**改**） |
| `src/ng/isr.S` | syscall 入口装载 `syscall_kernel_rsp0`；ring0/ring3 旧 stub 清 rdi/rsi（**改**） |
| `src/ng/sched_arch.c` | 切换前 `syscall_set_thread_stack(next)`（**改**） |
| `src/ng/ipc.c` | 用户指针改走 `uaccess.c`（**改**） |
| `src/ng/main.c` | WP1-7 自检与 PASS 行（**改**） |
| `tests/unit-ng/test_elf.c` | 校验/加载/回滚 fake 页池单测（**新**） |
| `tests/unit-ng/unit_main.c` | 加 `test_elf()`（**改**） |
| `tools/unit-ng.ps1` | 加 `elf.c`/`test_elf.c`（**改**，810→901 checks） |
| `tools/build-ng.ps1` | 先构建/校验 `obj-ng/hello.elf` 再 `.incbin`；C 源 21→26（**改**） |
| `tools/run-ng.ps1` | 6 条固定行 + 5 条正则 + WP1-7 失败模式（**改**） |
| `docs/design/elf-ng.md` | 加载器/AS/uaccess/per-thread 栈/自检/遗留（**新**） |
| `docs/design/decisions.md` | ADR-019（**追加**） |
| `docs/design/paging-ng.md` | §7.1 多地址空间补充（**改**） |
| `docs/design/evolution.md` | E1 出口状态（**改**） |

未修改 `src/kernel/**`、`build.ps1`、`tools/ci.ps1`、`abi.h`；新文件不在旧
32 位对象列表里。

## 环境

- 分支 `ng`，基线 HEAD `e12dd42`（WP1-6）；仓库 `D:\OS_Project`。
- clang 23.1.4 / ld.lld 23.1.4（`D:\msys64\mingw64\bin`）；内核
  `--target=x86_64-elf -ffreestanding -nostdlib -mno-red-zone
  -fno-stack-protector`；host 单测同一 clang 的默认 PE 目标。
- QEMU 11.0.0（`v11.0.0-12122-ga4bb4b10c9`），
  `qemu-system-x86_64 -kernel -m 256 -cpu max`（另跑 `-cpu qemu64` 验证
  SMAP=off 路径）。

## 证据（2026-10-31）

### 构建

```text
powershell -File tools/build-ng.ps1
```

```text
=== build-ng (WP1-7) ===
clang: D:\msys64\mingw64\bin\clang.exe
lld:   D:\msys64\mingw64\bin\ld.lld.exe

=== user-elf (WP1-7) ===
  hello.S -> obj-ng/user_hello.o
  link    -> obj-ng/hello.elf (base 0x40000000)
user-elf: PASS (13152 bytes, 3 PT_LOAD, entry 0x40000000, end 0x40002010)

  [1/26] entry.S
  [2/26] isr.S
  [3/26] switch.S
  [4/26] serial.c
  [5/26] memmap.c
  [6/26] pmm.c
  [7/26] heap.c
  [8/26] mem.c
  [9/26] paging.c
  [10/26] gdt.c
  [11/26] trap.c
  [12/26] syscall.c
  [13/26] mem_selftest.c
  [14/26] sched_arch.c
  [15/26] sched.c
  [16/26] sched_selftest.c
  [17/26] handle.c
  [18/26] ipc_core.c
  [19/26] ipc.c
  [20/26] ipc_selftest.c
  [21/26] elf.c
  [22/26] useras.c
  [23/26] uaccess.c
  [24/26] elf_selftest.c
  [25/26] main.c
  [26/26] link -> dist-ng/aster-ng.elf

ELF64: PASS (97184 bytes)
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

WP1-2/3/4/5/6 全部保持 PASS。WP1-7 段（完整串口见运行日志）：

```text
ng: WP1-6 selftest PASS
ng: elf64 header ok
ng: elf64 header entry=0x40000000 phnum=4 type=2 machine=62
ng: elf seg0 vaddr=0x40000000 filesz=0x63 memsz=0x63 pages=1 flags=0x5 (R-X)
ng: elf seg1 vaddr=0x40001000 filesz=0x11 memsz=0x11 pages=1 flags=0x4 (R)
ng: elf seg2 vaddr=0x40002000 filesz=0x8 memsz=0x10 pages=1 flags=0x6 (RW)
ng: elf64 bad image rejected (n=23)
ng: syscall stack per-thread ok (probe=0x157030 rsp0=0x157030 tss.rsp0=0x157030 boot=0x11f000)
ng: elf64 map ok (segs=3)
ng: elf64 load base=0x40000000 end=0x40002010 entry=0x40000000 pages=3 bss=0x8
ng: elf seg0 vaddr=0x40000000 filesz=0x63 memsz=0x63 pages=1 flags=0x5 (R-X) pte=0x176005
ng: elf seg1 vaddr=0x40001000 filesz=0x11 memsz=0x11 pages=1 flags=0x4 (R) pte=0x8000000000179005
ng: elf seg2 vaddr=0x40002000 filesz=0x8 memsz=0x10 pages=1 flags=0x6 (RW) pte=0x800000000017a007
ng: user stack 16KiB ok (base=0x40100000 top=0x40104000 guard=unmapped)
ng: uaccess ok (bad ptr EFAULT)
ng: elf64 data/bss ok
ng: ring3 exec ok
ng: ring3 cr3=0x174000 (as pml4=0x174000)
ng: ring3 hello
ng: syscall exit test
ng: ring3 exit ok
ng: WP1-7 selftest PASS
------------------------------------------------
run-ng: matched 39/39 expected lines + 11 patterns
run-ng: PASS (all expected lines in 0.2s, cpu max)
```

`run-ng.ps1` 新增的期望（全部命中才 PASS）：

```text
6 条固定行：
  ng: elf64 header ok
  ng: elf64 map ok (segs=3)
  ng: ring3 exec ok
  ng: ring3 hello                      <- 用户态输出（syscall 0）
  ng: ring3 exit ok
  ng: WP1-7 selftest PASS
5 条正则：
  ng: elf64 header entry=0x[0-9a-f]+ phnum=[0-9]+ type=2 machine=62
  ng: elf64 bad image rejected \(n=[0-9]+\)
  ng: syscall stack per-thread ok \(probe=0x[0-9a-f]+ rsp0=0x[0-9a-f]+
                                    tss\.rsp0=0x[0-9a-f]+ boot=0x[0-9a-f]+\)
  ng: uaccess ok \(bad ptr EFAULT\)
  ng: elf64 data/bss ok
失败早停模式新增：
  ng: elf64 .* FAIL / ng: ring3 .* FAIL / ng: WP1-7 selftest FAIL
```

### ELF 段证据（本次运行读回）

用户 ELF（`obj-ng/hello.elf`，13152 字节，4 个程序头里 3 个 PT_LOAD）：

| 段 | VA | filesz | memsz | 权限 | PTE（加载后） |
|---|---|---|---|---|---|
| `.text` | `0x40000000` | `0x63` | `0x63` | R-X | `0x176005` = P\|U，W=0、NX=0 |
| `.rodata` | `0x40001000` | `0x11` | `0x11` | R+NX | `0x8000000000179005` = NX\|P\|U，W=0 |
| `.data`+`.bss` | `0x40002000` | `0x8` | `0x10` | RW+NX | `0x800000000017a007` = NX\|P\|W\|U |

- 加载后 `bss=0x8`：`.data` 的 8 字节文件数据之后是 8 字节 `.bss`，加载器
  **先整页清零再拷文件字节**，所以 `.bss` 为 0；用户程序启动时读
  `user_bss_word == 0` 并写入 `0x5A5A5A5A` 成功，才继续打印。
- `.data` 魔数 `0x1122334455667788` 由用户程序读回校验，证明文件字节真的
  进了用户页（`ng: elf64 data/bss ok`）。
- 用户地址空间：`as pml4=0x174000`，`ring3 cr3=0x174000`，切 CR3 后内核
  恒等映射（0..1 GiB）仍可用，用户页只在用户 AS 的 PDPT[1] 子树里。

### 用户栈 / 守护页

```text
ng: user stack 16KiB ok (base=0x40100000 top=0x40104000 guard=unmapped)
```

4 页（16 KiB）RW+NX、U=1；下方 `0x400FF000` 不建映射（守护页）。ring3 入口
RSP = `0x40103FF0`，syscall 进入内核时 RSP 全部落在 `[0x40100000,0x40104000)`。

### per-thread 内核栈 / TSS.RSP0

```text
ng: syscall stack per-thread ok (probe=0x157030 rsp0=0x157030 tss.rsp0=0x157030 boot=0x11f000)
```

- 一个真实线程（heap 16 KiB 栈，栈顶 `0x157030`）里调
  `syscall_kernel_stack_sync()`：syscall 入口栈顶与 `TSS.RSP0` 都等于它的
  TCB `stack_top`；
- 回到 boot 线程（isr.S 静态栈，栈顶 `0x11f000`）再同步：两个值都变成 boot
  栈顶，且与 probe 的栈顶**不同**——证明 RSP0 是 per-thread 的，不是固定值。

### 坏镜像拒绝（23 例）

```
ident.magic / ident.class / ident.data / e_type / e_machine / e_version /
e_ehsize / e_phentsize / e_phnum=0 / e_phnum>16 / e_phoff 未对齐 /
e_phoff 越界 / p_filesz>p_memsz / p_offset+filesz 越界 / W^X /
保留 p_flags 位 / p_align<4 KiB / p_vaddr 未对齐 / p_vaddr<user_min /
p_vaddr>user_max / entry 不在可执行段 / 段重叠 / 无 PT_LOAD
```

每条都断言**具体错误码**（`ELF_EHDR/EPHDR/ESEG/EOFF/EVA/EOVERLAP/EENTRY/ENOSEG`），
不是"只要是负值就算过"。

### host 单测

```text
powershell -File tools/unit-ng.ps1
=== unit-ng (WP1-7 host tests) ===
clang: D:\msys64\mingw64\bin\clang.exe
901 checks, 0 failures
unit-ng: PASS (901 checks, 0 failures)
```

新增 `test_elf.c`（+91 checks）覆盖：

- 合法镜像的头/段规划（entry/base/end/phnum/seg_count/load_flags/pages）；
- 22 条头/段变异拒绝 + `PT_NOTE` 忽略 + 无 PT_LOAD + 0 长度段；
- 加载：3 页、逐段 flags（U / U+NX / U+W+NX）、文件字节、`.bss` 与页尾清零、
  `bss_bytes`、out 参数可空；
- 回滚：第 2 页 alloc 失败（ENOMEM）、第 2 页 map 失败（EMAP）、第 3 页
  alloc 失败；每次断言页池全空（unmap+free 没漏）。

### `-cpu qemu64`（SMAP=off）

```text
powershell -File tools/run-ng.ps1 -Cpu qemu64
```

```text
ng: smep=off smap=off
...
ng: uaccess ok (bad ptr EFAULT)
ng: ring3 exec ok
ng: ring3 hello
ng: ring3 exit ok
ng: WP1-7 selftest PASS
run-ng: matched 39/39 expected lines + 11 patterns
run-ng: PASS (all expected lines in 0.2s, cpu qemu64)
```

SMAP=off 时 `copyin/copyout` 跳过 `stac/clac`（U 页访问合法）；其余数字与
`-cpu max` 一致，说明加载/copyin/ring3 不依赖 SMAP。

### panic 路径回归

```text
tools/build-ng.ps1 -PanicTest  +  tools/run-ng.ps1 -Panic
ng: panic selftest: triggering unrecoverable #UD
PANIC: vec=0x6 err=0x0 rip=0x10dbcd cr2=0x10e000
run-ng: PASS (panic path observed in 0.2s, cpu max)
```

随后重新执行普通 `build-ng.ps1`，产物回到正常镜像（97184 bytes）。

### 旧树回归与边界检查

```text
powershell -File build.ps1 -Test
=== compile: 1 compiled, 103 skipped ===
=== elapsed: 42.4s ===
=== TEST PASS (305/305) ===
```

```text
powershell -File tools/check-domains.ps1
checked: 205 files, 726 quoted includes
domain include violations: 0

powershell -File tools/check-deps.ps1
checked: 201 files, 650 quoted includes
module include violations: 0
OK: no cross-module include violations.
```

## 自检项清单

1. ELF64 头：magic/class/data/version/type/machine/ehsize/phentsize/phnum/
   phoff 全部校验；hello ELF 的 entry=`0x40000000`、3 个 PT_LOAD。
2. 段规划：vaddr 4 KiB 对齐、filesz≤memsz、文件区间在镜像内、W^X、段不
   重叠、入口在可执行段。
3. 映射：逐段 PTE 读回（RX / R+NX / RW+NX，全部 U=1）；`.bss` 与页尾清零；
   `.data` 魔数正确。
4. 用户地址空间：新 PML4、切 CR3（`0x174000`）、用户页与内核恒等映射共存。
5. 用户栈 16 KiB + 守护页未映射；ring3 入口 RSP 在栈区间内。
6. per-thread 内核栈：probe/boot 两线程的 TCB 栈顶分别等于 syscall 入口
   RSP0 与 TSS.RSP0。
7. copyin/copyout：好指针成功；null/内核地址/只读用户页 `EFAULT`；SMAP=off
   路径一致。
8. ring3：用户态 `ng: ring3 hello`（17 字节，syscall 0）、exit_code=0、
   bad_writes=0，`ng: ring3 exec ok` / `ng: ring3 exit ok`。
9. host 单测 901/0；旧 32 位 305/305；check-domains/check-deps 0；panic
   负向与 `-cpu qemu64` 路径 PASS。

## 未做 / 遗留（按 WP 边界）

- **用户态异常投递**：ring3 的 #PF/#GP 仍进 panic，没有信号/退出码投递；
  守护页越界目前会 panic。属进程模型 WP。
- **进程模型**：只有一个用户 AS / 一个用户线程；无 fork/exec/wait、无每进程
  地址空间所有权，`as_destroy` 只在自检里调用。
- **加载源**：用户 ELF 嵌在内核 `.rodata`，不从 FS 读；VFS 就绪后替换
  "取文件"即可，`elf64_check/load` 不变。
- **上限**：phdr ≤16、段 ≤16、每段 ≤256 页（1 MiB）；用户窗口 <512 GiB
  （PML4[0]）；栈固定 16 KiB/固定地址；无 ASLR/mmap/mprotect。
- **syscall write**：单缓冲 ≤256 字节；无 iovec。
- **用户 SDK**：hello 是汇编 + 裸 syscall，没有 CRT/ABI 头（WP4-1 冻结时做）。
- **抢占/tick IRQ**：与 WP1-5/1-6 相同，仍协作式。

## 出口

```text
powershell -File tools/build-ng.ps1    # PASS，dist-ng/aster-ng.elf（97184 bytes，26 步）
powershell -File tools/run-ng.ps1      # PASS，39 条期望行 + 11 条正则（含 ng: ring3 hello）
powershell -File tools/unit-ng.ps1     # PASS，901 checks, 0 failures
powershell -File build.ps1 -Test       # PASS，305/305（旧 32 位）
powershell -File tools/check-domains.ps1   # 0 违规
powershell -File tools/check-deps.ps1      # 0 违规
```
