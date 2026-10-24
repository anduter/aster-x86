; =============================================================================
; switch.asm - 上下文切换与线程入口
;
; 上下文 = 被调用者保存寄存器（edi/esi/ebx/ebp）+ 返回地址 + 栈指针。
; 切换只做两件事：把当前 esp 存进 *old_esp，把 esp 换成 new_esp。
;
; 新线程的栈在 process.c 里预先摆好成同样的形状：
;   [esp+0] edi   [esp+4] esi   [esp+8] ebx
;   [esp+12] ebp  [esp+16] 返回地址 = thread_trampoline
; 所以第一次切进来时，pop 完四个寄存器后 ret 就跳到 thread_trampoline。
; =============================================================================
[bits 32]
section .text

global context_switch
context_switch:
    mov eax, [esp + 4]          ; old_esp（void **）
    mov edx, [esp + 8]          ; new_esp（void *）

    push ebp
    push ebx
    push esi
    push edi                    ; 此时栈顶是 edi，与 process.c 的布局一致

    mov [eax], esp              ; 保存旧上下文
    mov esp, edx                ; 切到新栈

    pop edi
    pop esi
    pop ebx
    pop ebp
    ret                         ; 新线程：跳到 thread_trampoline；旧线程：返回 proc_switch_to

; -----------------------------------------------------------------------------
; proc_exit_on_stack - 退出线程切到本 CPU 的专用退出栈后继续收尾
;
; C 侧（proc_exit）调用前已经 cli，并把 self/next/exit_code 填进每 CPU 槽位。
; 切栈之后 proc_exit_finish 才发布 ZOMBIE，因此父进程即使立刻回收退出线程的
; 内核栈/页目录，也不会踩到正在使用的栈。正常情况下不返回。
; -----------------------------------------------------------------------------
global proc_exit_on_stack
extern proc_exit_finish
proc_exit_on_stack:
    mov eax, [esp + 4]          ; 参数：退出栈顶
    mov esp, eax
    xor ebp, ebp                ; 栈回溯到此为止
    call proc_exit_finish
.hang:
    cli
    hlt
    jmp .hang

; -----------------------------------------------------------------------------
; thread_trampoline - 新线程的入口
;
; 从时钟中断切换过来的话，此时 IF=0（中断门），必须先开中断，
; 否则新线程会一直关着中断跑，时间片也无法抢占它。
; -----------------------------------------------------------------------------
global thread_trampoline
extern thread_entry_c
thread_trampoline:
    sti
    call thread_entry_c
    ; 正常不会返回；万一返回就停在这里
    cli
.hang:
    hlt
    jmp .hang
