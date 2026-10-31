# =============================================================================
# ng WP1-3: 异常入口 stub / 受控探测 stub / syscall 入口 / ring3 往返
#
# 栈帧约定（C 侧 struct trap_frame 必须与此一致）：
#
#   isr_common 先把 CPU 压的 [vector][error][rip][cs][rflags] 补成
#   [vector][error]，再依次 push 15 个通用寄存器（r15 先、rax 最后），
#   于是 rsp 指向的 15 个槽位顺序为：
#
#       rax rcx rdx rbx rbp rsi rdi r8 r9 r10 r11 r12 r13 r14 r15
#       vector error rip cs rflags [rsp ss]      <- rsp/ss 仅特权级切换时有
#
#   iretq 之前 addq $16 丢掉 vector/error，再把 15 个寄存器按反序弹回。
#
# 受控探测：probe_* 是故意触发异常的小函数，C handler 识别“故障 RIP +
# probe_active”后把 frame->rip 改到 *_after 标签，实现“跳过故障指令”。
#
# syscall 入口：SYSCALL 不换栈，所以先保存用户 RSP 到全局，再切到静态内核栈；
# 返回路径有两种：
#   - SYSRETQ（ring3 回归，STAR 描述符 DPL=3）；
#   - IRETQ（ring0 往返测试：SYSRET 在架构上只用于返回 CPL3，不能回 ring0）。
# =============================================================================

# ---- 异常入口 stub（0..31，带/不带 error code 分开）-----------------------
    .section .text
    .align 16

.macro ISR_NOERR num
    .global isr\num
isr\num:
    pushq $0                              # 没有 error code 的异常补 0
    pushq $\num
    jmp isr_common
.endm

.macro ISR_ERR num
    .global isr\num
isr\num:
    pushq $\num                           # error code 已由 CPU 压好
    jmp isr_common
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

# 32..255 的 IDT 项共用一个 stub（本 WP 不接 IRQ，正常不会进来）。
    .global isr_default
isr_default:
    pushq $0
    pushq $0xff
    jmp isr_common

    .global isr_common
isr_common:
    cld
    cmpl $0, ng_clac_enabled(%rip)        # SMAP=off 时 CLAC 会 #UD，先判断
    je 1f
    clac
1:
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %r11
    pushq %r10
    pushq %r9
    pushq %r8
    pushq %rdi
    pushq %rsi
    pushq %rbp
    pushq %rbx
    pushq %rdx
    pushq %rcx
    pushq %rax

    movq %rsp, %rdi
    movq %rsp, %rbx                       # rbx 原值已压进帧；C 会保留 rbx
    andq $-16, %rsp                       # SysV：call 前 16 字节对齐
    call trap_dispatch
    movq %rbx, %rsp                       # 回到帧基址（pop 会恢复 rbx）

    popq %rax
    popq %rcx
    popq %rdx
    popq %rbx
    popq %rbp
    popq %rsi
    popq %rdi
    popq %r8
    popq %r9
    popq %r10
    popq %r11
    popq %r12
    popq %r13
    popq %r14
    popq %r15
    addq $16, %rsp                        # 丢掉 vector/error
    iretq

# ---- 受控探测 stub ---------------------------------------------------------
# 写只读页：故障 RIP 就是函数第一条指令；恢复点在同函数下一条指令。
    .global probe_ro_write
    .type probe_ro_write, @function
probe_ro_write:
    movq $1, (%rdi)
    .global probe_ro_write_after
probe_ro_write_after:
    ret
    .size probe_ro_write, . - probe_ro_write

    .global probe_ud2
    .type probe_ud2, @function
probe_ud2:
    ud2
    .global probe_ud2_after
probe_ud2_after:
    ret
    .size probe_ud2, . - probe_ud2

# 加载坏段选择子：故障点是 mov %eax,%ds（probe_gp_load），跳过它即恢复。
    .global probe_gp
    .type probe_gp, @function
probe_gp:
    movl $0x40, %eax                      # 0x40 = GDT 之外的 selector
    .global probe_gp_load
probe_gp_load:
    movl %eax, %ds
    .global probe_gp_after
probe_gp_after:
    ret
    .size probe_gp, . - probe_gp

# ---- GDT 装载（C 建好描述符后调用）----------------------------------------
#   rdi = &gdtr。lgdt 后必须 far return 刷新 CS，再重载数据段并 ltr。
    .global gdt_load
    .type gdt_load, @function
gdt_load:
    lgdt (%rdi)
    pushq $0x08                           # 内核代码段
    leaq 1f(%rip), %rax
    pushq %rax
    lretq
1:
    movw $0x10, %ax                       # 内核数据段
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    movw $0x28, %ax                       # TSS（GDT 第 5/6 项）
    ltr %ax
    ret
    .size gdt_load, . - gdt_load

# ---- syscall 入口（LSTAR 指向这里）----------------------------------------
# 进入时：RCX = 返回 RIP，R11 = 返回 RFLAGS；CPL 已是 0，RSP 仍是调用者栈。
#
# WP3-0：入口的四个值（用户 RSP / 返回 RIP / 返回 RFLAGS / CS）先写进全局，
# 换到内核栈之后由 syscall_dispatch() 的入口代码一次性搬进**当前线程**的
# TCB（syscall.c: syscall_enter_ctx）。入口到 dispatch 之间没有调度点，所以
# 这几个全局是安全的；dispatch 返回前会把本线程的状态写回来（prepare_return），
# 于是多进程交替执行时不会互相覆盖。
    .global syscall_entry
    .type syscall_entry, @function
syscall_entry:
    movq %rsp, syscall_user_rsp(%rip)
    movq %rcx, syscall_saved_rip(%rip)
    movq %r11, syscall_saved_flags(%rip)
    # WP1-7：换成**当前线程**的内核栈（调度器在切换前同步这个全局；它与
    # TSS.RSP0 总是一致）。还没初始化时退回 isr.S 的静态栈。
    cmpq $0, syscall_kernel_rsp0(%rip)
    je 2f
    movq syscall_kernel_rsp0(%rip), %rsp
    jmp 3f
2:
    movq $syscall_stack_top, %rsp
3:

    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %r11
    pushq %r10
    pushq %r9
    pushq %r8
    pushq %rdi
    pushq %rsi
    pushq %rbp
    pushq %rbx
    pushq %rdx
    pushq %rcx
    pushq %rax
    # WP3-0：把入口 CS 记进全局（0x23 = CPL3）。用户寄存器已经全部压栈，
    # 这里可以安全借用 rax。dispatch 的入口代码会把它搬进当前线程的 TCB。
    xorl %eax, %eax
    movw %cs, %ax
    movq %rax, syscall_entry_cs(%rip)

    movq %rsp, %rdi
    movq %rsp, %rbx
    andq $-16, %rsp
    call syscall_dispatch
    movq %rbx, %rsp

    popq %rax
    popq %rcx
    popq %rdx
    popq %rbx
    popq %rbp
    popq %rsi
    popq %rdi
    popq %r8
    popq %r9
    popq %r10
    popq %r11
    popq %r12
    popq %r13
    popq %r14
    popq %r15

    movq syscall_user_rsp(%rip), %rsp
    cmpq $0, syscall_return_mode(%rip)
    jne .Lsys_ring0
    cmpl $0, syscall_want_exit(%rip)
    jne .Lsys_kernel_exit
    sysretq                               # ring3：CS/SS 取自 STAR 高 16 位

.Lsys_ring0:
    # ring0 往返测试的返回：SYSRET 只能回 CPL3，这里直接恢复 RFLAGS 并跳回
    # syscall 之后的指令（调用者栈上就是返回地址）。
    pushq syscall_saved_flags(%rip)
    popfq
    movq syscall_saved_rip(%rip), %rax
    jmp *%rax

.Lsys_kernel_exit:
    movq kernel_resume_rsp(%rip), %rsp    # ring3 测试结束：回内核协程
    movq kernel_resume_rip(%rip), %rax
    jmp *%rax

# ---- ring0 syscall 往返 stub ----------------------------------------------
    .global syscall_ring0_stub
    .type syscall_ring0_stub, @function
syscall_ring0_stub:
    movl $0, %eax                         # 调用号 0：write serial
    xorl %edi, %edi                       # rdi=0 => WP1-3 诊断模式
    xorl %esi, %esi
    syscall
    ret
    .size syscall_ring0_stub, . - syscall_ring0_stub

# ---- 进 ring3：保存内核恢复点，构造 iretq 帧 ------------------------------
#   rdi = 用户 RIP，rsi = 用户 RSP
#
# WP3-0：恢复点保存到**当前线程**的 TCB（syscall.c 的 ring3_save_resume），
# 不再用全局 —— 进程 A 在 syscall 里阻塞、进程 B 进 ring3 时，各有各的恢复点。
# WP2-6：ring3 期间内核的 callee-saved 寄存器（rbx/rbp/r12..r15）必须保持
# 不变 —— syscall 返回路径会把**用户**的寄存器值弹回来，所以这里先把它们压到
# 内核栈，ring3_resume 再弹回去；r12/r13 借来在调用 C helper 时暂存用户
# rip/rsp（这两个值本身已压在栈上）。
    .global ring3_enter
    .type ring3_enter, @function
ring3_enter:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rdi, %r12                       # user rip
    movq %rsi, %r13                       # user rsp
    movq %rsp, %rdi
    leaq ring3_resume(%rip), %rsi
    call ring3_save_resume
    pushq $0x1b                           # SS：用户数据（RPL3）
    pushq %r13                            # RSP
    pushq $0x2                            # RFLAGS（IF=0）
    pushq $0x23                           # CS：用户代码（RPL3）
    pushq %r12                            # RIP
    iretq
ring3_resume:
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret
    .size ring3_enter, . - ring3_enter

# ---- WP3-0：带 argc/argv 的 ring3 入口 ------------------------------------
#   rdi = 用户 RIP，rsi = 用户 RSP，rdx = argc，rcx = argv
# 进程初始寄存器 ABI：rdi=argc、rsi=argv（用户程序 _start 直接当参数用）。
# ring3_enter 保持两参数版本（WP1-7 的 hello / WP1-3 的静态页 / host 单测）。
    .global ring3_enter_args
    .type ring3_enter_args, @function
ring3_enter_args:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rdi, %r12                       # user rip
    movq %rsi, %r13                       # user rsp
    movq %rdx, %r14                       # argc
    movq %rcx, %r15                       # argv
    movq %rsp, %rdi
    leaq ring3_resume(%rip), %rsi
    call ring3_save_resume
    pushq $0x1b
    pushq %r13
    pushq $0x2
    pushq $0x23
    pushq %r12
    movq %r14, %rdi                       # iretq 之后用户态看到的 rdi/rsi
    movq %r15, %rsi
    iretq
    .size ring3_enter_args, . - ring3_enter_args

# ---- 用户态测试程序（拷进用户页后执行）------------------------------------
    .section .rodata
    .align 16
    .global user_prog_start
user_prog_start:
    xorl %edi, %edi                       # rdi=0 => syscall 0 诊断模式
    xorl %esi, %esi
    movl $0, %eax                         # syscall 0：write serial
    syscall
    movl $1, %eax                         # syscall 1：exit test
    syscall
    hlt                                   # 不应到达
    .global user_prog_end
user_prog_end:

# 异常 stub 地址表（trap.c 填 IDT[0..31] 用）。
    .align 8
    .global isr_stub_table
isr_stub_table:
    .quad isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7
    .quad isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15
    .quad isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    .quad isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31

# ---- syscall 专用内核栈（静态，WP1-5 调度前只做单线程）---------------------
    .section .bss
    .align 16
syscall_stack:
    .skip 16384
    .global syscall_stack_top
syscall_stack_top:
