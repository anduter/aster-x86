# =============================================================================
# ng WP1-5: 协作式上下文切换（switch_context）与新线程入口（thread_entry）
#
# 上下文布局（与 src/ng/thread.h 的 struct thread_context 严格一致，8 个 u64）：
#
#   offset  0   rbx
#           8   rbp
#           16  r12
#           24  r13
#           32  r14
#           40  r15
#           48  rsp      <- 恢复后直接使用的栈指针（等价于 switch_context 返回后）
#           56  rip      <- 恢复后跳转的地址（等价于 switch_context 的返回地址）
#
# switch_context(old, new)：
#   - 保存 6 个 callee-saved 寄存器；
#   - 保存 rsp：leaq 8(%rsp) —— 即“ret 之后”的栈指针，于是恢复时不需要
#     再伪造返回地址，jmp rip 即可让调用者从 call 之后继续；
#   - 保存 rip：(%rsp) 就是 return address；
#   - 恢复 new 的寄存器/rsp，然后 jmp rip。
#
# 对齐：调用点遵守 SysV（call 前 rsp 16 字节对齐，函数入口 rsp%16==8）；
# 保存的 rsp 是 ret 之后的值（%16==0），恢复后用 jmp 进入调用者，栈状态与
# 正常返回完全一致。新线程的 ctx.rsp = 栈顶（16 字节对齐），thread_entry
# 用 `andq $-16` 再做一次保险，call 之前的 rsp%16==0。
# =============================================================================

    .section .text
    .align 16
    .global switch_context
    .type switch_context, @function
switch_context:
    movq %rbx, 0(%rdi)
    movq %rbp, 8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)

    leaq 8(%rsp), %rax                    # 恢复点 = 本函数 ret 之后
    movq %rax, 48(%rdi)
    movq (%rsp), %rax                     # 返回地址 = 恢复后的 RIP
    movq %rax, 56(%rdi)

    movq 0(%rsi), %rbx
    movq 8(%rsi), %rbp
    movq 16(%rsi), %r12
    movq 24(%rsi), %r13
    movq 32(%rsi), %r14
    movq 40(%rsi), %r15
    movq 56(%rsi), %rax
    movq 48(%rsi), %rsp
    jmp *%rax
    .size switch_context, . - switch_context

# ---- 新线程第一次进入 -------------------------------------------------------
# ctx.rsp 是 16 KiB 栈顶；这里不是被 call 进来的，先对齐再用 call 进 C。
# thread_bootstrap() 不返回（跑完 start(arg) 后调 thread_exit）；真回来了就 ud2。
    .global thread_entry
    .type thread_entry, @function
thread_entry:
    cld                                   # SysV 要求 DF=0
    andq $-16, %rsp
    call thread_bootstrap
    ud2
    .size thread_entry, . - thread_entry
