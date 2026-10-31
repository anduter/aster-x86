# ng ELF64 加载器 / 用户地址空间 / copyin-copyout（WP1-7）

> 状态：已实现（2026-10-31，ng `e12dd42` 之后）。
> 代码：`src/ng/elf.[ch]`、`useras.[ch]`、`uaccess.[ch]`、`elf_selftest.[ch]`、
> `src/ng/user/hello.S` + `user.ld`、`src/ng/mem.c`；
> host 单测：`tests/unit-ng/test_elf.c`；出口：`ng: WP1-7 selftest PASS`。
> 相关：`docs/design/trap-syscall-ng.md`（syscall 入口）、`paging-ng.md`
> （4 级表/W^X/SMAP）、`ipc-ng.md`（用户指针约定）、ADR-019。

## 1. 范围与边界

WP1-7 只做"把一个静态链接的 ELF64 程序加载进用户地址空间并跑到 exit"：

- ELF64 校验 + 逐段映射 + `.bss` 清零；
- 独立用户 PML4、CR3 切换、16 KiB 用户栈 + 守护页；
- copyin/copyout（页级校验 + SMAP 窗口 + `EFAULT`）；
- per-thread 内核栈（syscall 入口 + TSS.RSP0）；
- 一个 syscall 0/1 的示例用户程序（嵌入内核镜像，不依赖 FS）。

不做：进程模型/多用户进程、文件系统加载、动态链接、POSIX、fork/exec、
抢占、用户态异常投递（ring3 的 #PF/#GP 仍进 panic）。

## 2. 加载器分层

```text
        build-ng.ps1
          hello.S --(user.ld, . = 0x40000000)--> obj-ng/hello.elf
                                                    |
                                        .incbin 进内核 .rodata
                                                    v
                                   ng_user_elf_start .. ng_user_elf_end
                                                    |
      elf64_check(纯校验)  <------------------ elf_selftest.c
          |  产出 struct elf64_image（entry/base/end/seg[]/pages）
          v
      elf64_load(env 回调) ---- alloc/free/page_ptr/map/unmap ----> PMM + useras
          |
          +--> host 单测：同一份 elf.c + fake 页池（test_elf.c）
```

`elf.c` 只 include `elf.h`（而 `elf.h` 只 include `types.h`），所以：

- host 单测能覆盖头/段校验的每一条拒绝路径与加载回滚；
- 内核侧只剩"页分配、字节拷贝、页表映射"三种可能失败的 I/O。

## 3. 校验规则（全部拒绝式）

| 检查 | 错误码 |
|---|---|
| `image != NULL` / `size >= 64` | `ELF_ENULL` / `ELF_ESIZE` |
| magic / EI_CLASS=2 / EI_DATA=1 / EI_VERSION=1 | `ELF_EHDR` |
| e_version=1、e_type=ET_EXEC、e_machine=EM_X86_64、e_ehsize=64 | `ELF_EHDR` |
| e_phoff≠0 且 8 字节对齐、e_phentsize=56、1≤e_phnum≤16、phdr 表在文件内 | `ELF_EPHDR` |
| 至少一个 PT_LOAD | `ELF_ENOSEG` |
| `p_filesz ≤ p_memsz`、flags 只用 PF_R/W/X | `ELF_ESEG` |
| `p_offset + p_filesz ≤ size`（无溢出） | `ELF_EOFF` |
| `vaddr` 在 `[0x40000000, 0x7FFFFFFFFF]` 且 `vaddr+memsz` 不溢出 | `ELF_EVA` |
| `vaddr` 4 KiB 对齐、`p_align` 是 ≥4096 的 2 的幂、段 ≤256 页 | `ELF_ESEG` |
| **W^X**：PF_W 与 PF_X 不同时置位 | `ELF_ESEG` |
| 段两两不重叠（`[vaddr, vaddr+memsz)`） | `ELF_EOVERLAP` |
| `e_entry` 落在某个 PF_X 的非空 PT_LOAD | `ELF_EENTRY` |

非 PT_LOAD 的程序头（PT_NOTE、PT_GNU_STACK 等）直接忽略；0 长度 PT_LOAD 合法
但不映射。`p_align` 按"页级映射"的强约束处理：本加载器不做段内偏移对齐修正。

## 4. 加载与回滚

每个 PT_LOAD：

```text
pages = ceil(memsz / 4096)
对每页 p:
    phys = env->alloc_page()                  ; 失败 -> ELF_ENOMEM
    dst  = env->page_ptr(phys)                ; 恒等映射下就是 phys
    memset(dst, 0, 4096)                      ; 同时覆盖 .bss 与页尾空洞
    if (p*4096 < filesz)
        copy(dst, image + offset + p*4096, min(4096, filesz - p*4096))
    map_flags = USER | (PF_W ? WRITE : 0) | (PF_X ? 0 : NX)
    env->map_page(va, phys, map_flags)        ; 失败 -> ELF_EMAP
```

失败回滚：对**已完整映射的段**逐页 `env->unmap_page(va)`，对当前段已完成的页
同样 unmap；刚分配但未映射的页用 `env->free_page(phys)`。内核实现里
`unmap_page` = `pg_unmap_in + pmm_free_page`，所以回滚后 PMM 页数与进入前一致
（host 单测断言页池全空）。

## 5. 用户地址空间（`useras.c`）

```text
新 PML4
  [0]   -> 新 PDPT（本 AS 私有）
             [0]    -> 内核 PD（共享 0..1 GiB 恒等映射，U=0）
             [1..]  -> 按需分配（用户页；本 WP 都落在 PDPT[1] = 1..2 GiB）
  [1..255] -> 复制内核 PML4 同名项（当前内核只用 [0]）
  [256..511] 清零（不做 higher-half）
```

为什么新建 PDPT 而不是共享内核 PDPT：WP1-3 在 `0x40000000` 映射过静态用户
页，共享 PDPT 会让 ELF 段和旧 PTE 撞车（`PG_EEXIST`）；新建后用户页只存在
于用户 AS，PDPT[0] 仍指向内核 PD，切 CR3 后内核代码/栈/页表页照常可访问。

用户窗口限定 `<512 GiB`（PML4[0]）：`as_destroy()` 只遍历 PDPT[1..511] 回收
页表页与叶页，绝不 free 内核共享表。销毁前若 AS 正在 CR3 里，先
`as_switch_kernel()`。

用户栈（`useras.h`）：

```text
NG_USER_STACK_BASE 0x40100000   （= 1 GiB + 1 MiB，PDPT[1]）
4 页 = 16 KiB，RW+NX，U=1
守护页 0x400FF000 不建映射 -> 越界访问 #PF（当前进 panic，见 §10）
entry RSP = NG_USER_STACK_TOP - 16
```

## 6. copyin / copyout（`uaccess.c`）

```text
uaccess_range_ok(va, len, write):
    va != 0, len != 0, 无 64 位溢出, 末地址 <= 0x00007FFFFFFFFFFF
    对范围里每一页：pg_walk_in(pg_current_pml4(), page)
        必须 P=1 且 U=1（write 还要 W=1）
copyin / copyout:
    先整段校验（任何一页失败 -> UACCESS_EFAULT，不触碰内存）
    再在 SMAP=on 时 stac/clac 包住逐字节拷贝
```

`pg_current_pml4()` 就是 CR3 对应的表：内核态时是内核 PML4（内核地址 U=0
自然被拒），ring3 syscall 时是用户 AS（用户页通过）。`copyin_str` 逐字节
读用户字符串（max 字节、总是 NUL 结尾），供将来的 exec/参数传递使用。

IPC 的 `ipc_sys_*` 不再自带一套校验，改为调用 `copyin/copyout`
（`user_copy_in/out` 只做错误码翻译），保证"一处校验、两处使用"。

## 7. per-thread 内核栈与 syscall 入口

```text
sched_arch_switch(prev, next):
    syscall_set_thread_stack(next)   -> syscall_kernel_rsp0 = TSS.RSP0 = next->stack_top
                                        （next 是 boot 线程时回退 isr.S 静态栈）
    switch_context(...)

syscall_entry:
    保存用户 RSP/RCX/R11 -> 从 syscall_kernel_rsp0 装载内核 RSP -> 压 15 个 GPR
```

`gdt_set_rsp0()` 同时改 TSS.RSP0；`syscall_set_kernel_stack()` 同时改
`syscall_kernel_rsp0`，两者永远一致。自检用一个真实线程验证：

```text
probe=0x157030 rsp0=0x157030 tss.rsp0=0x157030 boot=0x11f000
```

即线程 A 的 TCB 栈顶 == syscall 入口栈顶 == TSS.RSP0，回到 boot 线程后
RSP0 又变回 boot 栈顶（两值不同）。

## 8. syscall ABI 增量（WP1-7）

| rax | 调用 | 参数 | 返回 |
|---|---|---|---|
| 0 | write(buf,len) | rdi=用户缓冲、rsi=长度（≤256） | 写入字节数 / `-14 EFAULT` / `-22 EINVAL` |
| 0 | write（rdi=0） | — | 0（保留 WP1-3 诊断行） |
| 1 | exit(code) | rdi=退出码 | 不返回（ring3 回内核恢复点） |

`syscall_run_user(entry, rsp, stack_lo, stack_hi, &res)` 是唯一的 ring3 执行
入口：置 ring3 状态与统计、`iretq` 进 CPL3、syscall 1 时由入口跳回
`kernel_resume`。WP1-3 的静态用户页往返与 WP1-7 的 ELF 程序都走它，统计见
`struct ring3_result`（syscalls/writes/bytes/exit_code/exited/user_rsp_ok/
bad_writes）。

## 9. 自检（`elf_selftest.c`）

| 子项 | 做法 | 断言 |
|---|---|---|
| 头/段 | 校验内嵌 hello ELF | entry/base/end/phnum、段数 3 |
| 坏镜像 | 23 份变异副本（magic/class/data/type/machine/version/ehsize/phentsize/phnum/phoff/filesz/offset/W^X/p_align/vaddr/entry/overlap/无 LOAD…） | 每条都返回**期望的错误码** |
| 映射 | 逐段 map 后读回 PTE | text `U|P|W=0|NX=0`、rodata `U|P|NX`、data `U|P|W|NX` |
| 数据 | AS 激活后 `copyin` 用户 `.data/.bss` | 魔数 `0x1122334455667788` 正确、`.bss` 为 0 |
| 用户栈 | 4 页 + 守护页 | `guard` 未映射、base/top 已映射 |
| per-thread 栈 | 真线程 + 两次 sync | probe/boot 两个栈顶不同，RSP0 跟着变 |
| uaccess | 好指针 copyin/out + null/内核地址/只读页 | 前者成功、后者全部 `EFAULT` |
| ring3 | 切 CR3、iretq、用户 write+exit | `ng: ring3 hello`、exit_code=0、writes=1、bad_writes=0 |
| 回收 | `as_switch_kernel()` + `as_destroy()` | 页表页与叶页全部归还 PMM |

## 10. 未做 / 遗留（按 WP 边界）

- **用户态异常**：ring3 的 #PF/#GP 仍走 panic，不投递信号；守护页越界会
  打出 `PANIC`。用户态 fault 与信号/退出码投递属于进程模型 WP。
- **进程**：只有一个用户 AS/一个用户线程；没有 fork/exec/wait、没有每进程
  页表所有权，`as_destroy` 只在自检里调用。多进程调度是后续 WP。
- **加载源**：用户 ELF 用 `.incbin` 嵌在内核 `.rodata`，不是从 FS 读；VFS/
  ramfs 就绪后把"取文件"换成 `copyin` 即可，`elf64_check/load` 不用改。
- **段数/页数上限**：`ELF_MAX_SEGS=16`、每段 `256` 页（1 MiB）、phdr ≤16。
  回滚按页记录，上限是显式的；更大程序先扩上限或改成 unmap 回调即时回收。
- **用户地址窗口**：<512 GiB（PML4[0]）；不支持高半区用户地址、ASLR、
  mmap。栈固定 16 KiB/固定地址。
- **W^X**：拒绝 RWX 段；text 是 R-X，rodata R+NX，data/bss RW+NX。没有
  `mprotect`，也没有按页细分的 COW。
- **SMAP=off**：qemu64 路径跳过 stac/clac，行为一致（见 claims）。
- **syscall 参数**：write 上限 256 字节（内核栈缓冲）；没有 iovec/scatter。
- **用户 libc/SDK**：hello 是汇编 + 裸 syscall，没有 `_start` CRT、没有 ABI
  头文件（WP4-1 冻结时做）。

## 11. 文件与工具链变更

| 路径 | 内容 |
|---|---|
| `src/ng/elf.h` / `elf.c` | ELF64 校验/段规划/env 加载/回滚（**新**） |
| `src/ng/useras.h` / `useras.c` | 独立 PML4、用户页映射/窗口、CR3 切换/销毁（**新**） |
| `src/ng/uaccess.h` / `uaccess.c` | copyin/copyout/copyin_str + 页级校验 + SMAP（**新**） |
| `src/ng/elf_selftest.h` / `.c` | 内核自检、23 份坏镜像、证据行（**新**） |
| `src/ng/user/hello.S` / `user.ld` | ring3 示例程序与 0x40000000 链接脚本（**新**） |
| `src/ng/mem.c` | freestanding memcpy/memset/memmove（**新**） |
| `src/ng/paging.h` / `paging.c` | `pg_*_in()`、`pg_switch_pml4()`、`pg_current_pml4()`、`pg_free_table()`（**改**） |
| `src/ng/gdt.h` / `gdt.c` | `gdt_set_rsp0()`（**改**） |
| `src/ng/syscall.h` / `syscall.c` | write(buf,len)、exit code、`syscall_run_user`、per-thread 栈（**改**） |
| `src/ng/isr.S` | syscall 入口用 `syscall_kernel_rsp0`；旧 stub 清 rdi/rsi（**改**） |
| `src/ng/sched_arch.c` | 切换前同步 syscall 栈/TSS.RSP0（**改**） |
| `src/ng/ipc.c` | 用户指针改走 uaccess（**改**） |
| `src/ng/main.c` | WP1-7 自检与 PASS 行（**改**） |
| `tests/unit-ng/test_elf.c` | 校验/加载/回滚 host 单测（**新**） |
| `tests/unit-ng/unit_main.c` | 加 `test_elf()`（**改**） |
| `tools/unit-ng.ps1` | 加 `elf.c`/`test_elf.c`（**改**） |
| `tools/build-ng.ps1` | 先构建/校验用户 ELF 再 `.incbin`；C 源 21→26（**改**） |
| `tools/run-ng.ps1` | 6 条固定行 + 5 条正则 + WP1-7 失败模式（**改**） |

未修改 `src/kernel/**`、`build.ps1`、`tools/ci.ps1`、`abi.h`。
