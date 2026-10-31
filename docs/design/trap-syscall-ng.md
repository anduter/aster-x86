# ng trap / syscall（WP1-3）

> 实现：`src/ng/isr.S`（异常 stub、受控探测、syscall 入口、ring3 往返）、
> `src/ng/gdt.c`（GDT/TSS/IST）、`src/ng/trap.c`（IDT/异常分发/panic）、
> `src/ng/syscall.c`（MSR、ring0/ring3 测试）。
> 出口证据见 `docs/claims/ng-WP1-3.md`；决策见 `decisions.md` ADR-015。

## 1. 文件分工

| 文件 | 内容 |
|---|---|
| `isr.S` | 32 个异常入口 stub（有/无 error code）、公共保存/恢复、`iretq`；`probe_*` 受控故障函数；`syscall_entry`（LSTAR）；`ring3_enter`（iretq 到 CPL3）；`gdt_load`（刷新 CS/段寄存器/`ltr`） |
| `gdt.c` | 7 槽 GDT（内核/用户代码数据段 + TSS）、TSS64、RSP0/IST1 静态栈 |
| `trap.c` | 256 项 IDT、`trap_dispatch`、#PF 诊断、恢复判定、panic、受控自检 |
| `syscall.c` | `EFER.SCE` + STAR/LSTAR/FMASK、调用号分发、ring0/ring3 往返测试 |

## 2. GDT / TSS / IST

GDT 7 个 8 字节槽（TSS 描述符占 16 字节，用第 5、6 槽），运行期基址
`0x12c000`（`.bss`，RW+NX 即可，CPU 只读）：

| 选择子 | 内容 | 关键位 |
|---|---|---|
| `0x00` | null | |
| `0x08` | 内核代码 | L=1, DPL=0 |
| `0x10` | 内核数据 | DPL=0 |
| `0x18` | 用户数据 | DPL=3 |
| `0x20` | 用户代码 | L=1, DPL=3 |
| `0x28` | TSS（16 字节） | type=9, DPL=0 |

顺序不是随意的：`SYSRET` 在 64 位模式下取 `CS = STAR[63:48] + 16`、
`SS = STAR[63:48] + 8`，所以**用户数据段必须紧挨在用户代码段前一个槽**
（`STAR[63:48] = 0x13` → `SS = 0x1b`、`CS = 0x23`）。

TSS64（104 字节，`packed`）：

| 偏移 | 字段 | 值/用途 |
|---|---|---|
| `0x00` | reserved | 0 |
| `0x04` | RSP0 | ring3 → ring0 异常/中断入口栈顶 |
| `0x0c/0x14` | RSP1/2 | 0（未用） |
| `0x24` | IST1 | double fault 专用栈顶 |
| `0x2c..0x54` | IST2..7 | 0 |
| `0x66` | I/O map base | `0x68`（= TSS 长度，**无 I/O 位图** → ring3 `in/out` 一律 #GP） |

静态栈：RSP0 16 KiB、IST1 8 KiB；实际地址见串口 `ng: tss rsp0=... ist1=...`。
`ltr 0x28` 后 `str` 读回 `0x28`。

`gdt_load` 的顺序：`lgdt` → `push CS/新 RIP + lretq` 刷新 CS（长模式必须）
→ 重载 DS/ES/SS/FS/GS → `ltr`。CS 不刷新的话缓存描述符仍是引导 GDT 的。

## 3. IDT

256 项 × 16 字节，4 KiB 对齐，`lidt` 后基址落在 `.bss`（运行期 `0x1320d0`）。
门格式（64 位中断门）：

```text
offset[15:0] | selector=0x08 | IST(3bit) | type=0x8E | offset[31:16] | offset[63:32] | 0
```

- 0–31：独立 stub。**有 error code 的异常**（8 #DF、10 #TS、11 #NP、12 #SS、
  13 #GP、14 #PF、17 #AC、21 #CP、29 #VC、30 #SX）由 CPU 压 error code，
  stub 只压向量号；**其余**先压 0 再压向量号。`isr_common` 统一把栈整理成
  `struct trap_frame`。
- 8 #DF：`IST=1`，即使当前内核栈坏了仍能进 handler（然后 panic）。
- 32–255：指向同一个默认 stub（本 WP 不接 IRQ、不开 IF；正常不会进来）。

入口栈帧（`isr.S` 与 `trap.c` 的 `struct trap_frame` 一一对应）：

```text
[vector][error][rip][cs][rflags]            <- CPU/ash 压好
[vector][error][rip][cs][rflags][rsp][ss]   <- 若来自 CPL3（有权限切换）
再压 15 个 GPR：rax rcx rdx rbx rbp rsi rdi r8..r15（r15 先压、rax 最后）
```

`iretq` 前 `addq $16` 丢掉 vector/error，再反序弹回 15 个 GPR。入口执行
`cld`；只有 `CR4.SMAP=1` 才执行 `clac`（SMAP=off 时 CLAC 是 #UD，qemu64
路径必须跳过）。

## 4. 可恢复异常（受控探测）

`probe_ro_write` / `probe_ud2` / `probe_gp` 是故意触发异常的汇编小函数，
每个都有 `*_after` 恢复点。`trap_dispatch` 只在 `probe_active` 期间接受恢复：

| 探测 | 判定 | 恢复动作 |
|---|---|---|
| 写 `.rodata` | `vec=14` + 故障 RIP == `probe_ro_write` + `CR2` == 目标地址 | `frame->rip = probe_ro_write_after`，同时校验 `error == 0x3`（P=1/W=1/U=0） |
| `ud2` | `vec=6` + RIP == `probe_ud2` | RIP = `probe_ud2_after` |
| 加载坏段 `0x40` | `vec=13` + RIP == `probe_gp_load` | RIP = `probe_gp_after`（DS 没被改） |

未命中恢复条件的任何异常都进 panic——恢复逻辑不对普通故障开放。`#PF`
每次都会打印 `ng: #pf cr2=... error=... rip=...`，既是诊断也是证据。

## 5. panic

```text
PANIC: vec=<hex> err=<hex> rip=<hex> cr2=<hex>
```

随后 `cli; hlt` 死循环。`CR2` 对非 #PF 是上次缺页的残留值（架构如此），
仅作诊断。负向入口：`build-ng.ps1 -PanicTest` 在正常自检后执行一条
`ud2`，`run-ng.ps1 -Panic` 期望 `PANIC: vec=0x6 ...`。

## 6. syscall

MSR（`syscall_init`）：

| MSR | 值 | 含义 |
|---|---|---|
| `EFER` | `SCE=1`（bit 0） | 允许 64 位 `syscall` |
| `STAR` | `0x0013000800000000` | 高 16 位 `0x13`（SYSRET 基址），`[47:32]=0x08`（SYSCALL 的 CS） |
| `LSTAR` | `syscall_entry` | 入口地址 |
| `FMASK` | `0x40700` | 进入时清 TF(8)/IF(9)/DF(10)/AC(18) |

`syscall` 进入时：`RCX` = 返回 RIP、`R11` = 返回 RFLAGS；**不换栈**。入口
先把调用者 RSP 存全局，再切到 16 KiB 静态 `syscall_stack`，保存 15 个 GPR，
调 `syscall_dispatch(frame)`，恢复寄存器后按模式返回：

- **ring3**：`sysretq`（`RCX`/`R11` 已由弹栈恢复）→ CS=0x23、SS=0x1b；
- **ring0 往返测试**：SYSRET 的架构语义只能回 CPL3，这里用
  `popfq` 恢复 RFLAGS + `jmp *saved_rip` 直接回到 `syscall` 后的指令；
- **ring3 退出测试**（`rax=1`）：handler 置 `syscall_want_exit`，入口跳回
  `kernel_resume_rsp/rip`（`ring3_enter` 保存的内核协程点），不回用户态。

调用号（WP1-3 最小 ABI 草案）：

| rax | 语义 | 返回 |
|---|---|---|
| 0 | write serial：内核串口打一行 | 0 |
| 1 | exit test：打印；ring3 下额外请求回内核 | 0 |
| 其它 | 未实现 | -1 |

参数寄存器约定为 `rdi/rsi/rdx`，本 WP 还没用（WP1-7 的 ELF/ABI 再定形）。

## 7. ring3 最小往返

1. 把 `user_prog_start..user_prog_end` 的机器码拷进 `.bss` 的
   `user_code_page`（4 KiB，页对齐）；
2. `pg_map(0x40000000, 代码页, PG_USER)`：U/S=1、W=0、NX=0；
   `pg_map(0x40100000, user_stack, PG_USER|PG_WRITE|PG_NX)`：U/S=1、RW、NX。
   `pg_map` 在带 `PG_USER` 时会把**每一级**表项的 U 位补上（否则 ring3
   走表就被拒），并重写 CR3 刷 TLB；
3. `ring3_enter(user_rip, user_rsp)`：保存内核 RSP 与恢复标签，压
   `SS=0x1b / RSP / RFLAGS=0x2 / CS=0x23 / RIP`，`iretq` 进 CPL3；
4. 用户程序：`syscall`（rax=0）→ handler 打印 → `sysretq` 回用户 →
   `syscall`（rax=1）→ handler 置退出标志 → 入口跳回内核恢复点；
5. 内核打印 `ng: ring3 syscall ok`。

在 `-cpu max`（SMEP=on）下能执行 0x40000000 的 U/S=1 页，本身就是
“确实在 CPL3”的硬证据；`ring3_user_rsp_ok` 还校验了 syscall 进入时的
RSP 落在 0x40100000–0x40100fff。

## 8. SMAP / stac-clac 约定（承接 ADR-014）

- 异常入口第一条路径按 `CR4.SMAP` 决定是否 `clac`；syscall 用 `FMASK`
  清 AC，内核始终在 AC=0 下运行。
- 本 WP 还没有 `copyin/copyout`：ring3 测试不把用户指针传进内核，内核只
  通过 `.bss` 别名访问用户页物理内存，所以没有 stac 窗口。
- 用户页的物理页在 `.bss` 里也有一个 supervisor 别名（RW+NX），两个映射
  指向同一物理页：内核用别名写代码，用户用 0x40000000 执行。

## 9. 边界 / 未做

- 不做 PMM/heap（WP1-4）：页表页仍来自静态池，ring3 用户页是静态页。
- 不做调度（WP1-5）：syscall 内核栈是单线程静态栈，不可重入。
- 不做 ELF64/用户态异常处理（WP1-7）：用户程序是内核里的机器码 blob；
  ring3 里的 #PF/#GP 会走 panic，不投递信号。
- 不做 IRQ：IDT[32..255] 是默认 stub，IF=0。
- #DF 只验证了 IST1 配置（IDT[8].IST=1、TSS.IST1 非 0），没有主动制造
  双重故障；主动触发会破坏当前上下文且无法恢复。
- 本机 QEMU 对同 CPL 的 `iretq` 实测也弹 5 项（SDM 语义是 3 项），所以
  ring0 测试返回不依赖同 CPL `iretq`；ring3 的 iret 本来就是完整 5 项帧。
