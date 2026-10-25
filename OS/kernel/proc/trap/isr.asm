; =============================================================================
; isr.asm - 中断与异常的入口桩
;
; 每个向量一个很短的入口，做两件事：
;   1. 把“向量号”和“错误码”压成统一的栈布局
;      （无错误码的异常补一个 0，保证所有向量布局一致）
;   2. 跳到通用入口：保存现场 -> 调 C 分发函数 -> 恢复现场 -> iret
;
; 统一之后的栈布局（从低地址到高地址）：
;   vec, err, eip, cs, eflags[, useresp, ss]
; 前面再加上 pushad 和段寄存器，正好对应 intr/interrupt.h 的 interrupt_frame_t。
; =============================================================================
[bits 32]
section .text

extern isr_dispatch
extern irq_dispatch

; ---- 无错误码的异常：先补一个 0 占位，再压向量号 ----
%macro ISR_NOERR 1
global isr%1
isr%1:
    cli
    push dword 0
    push dword %1
    jmp isr_common
%endmacro

; ---- 有错误码的异常：CPU 已经压了错误码，这里只压向量号 ----
%macro ISR_ERR 1
global isr%1
isr%1:
    cli
    push dword %1
    jmp isr_common
%endmacro

; 0 ~ 31 号异常
ISR_NOERR 0      ; 除法错误
ISR_NOERR 1      ; 调试
ISR_NOERR 2      ; 不可屏蔽中断
ISR_NOERR 3      ; 断点
ISR_NOERR 4      ; 溢出
ISR_NOERR 5      ; 越界
ISR_NOERR 6      ; 非法指令
ISR_NOERR 7      ; 设备不可用
; 8 号（双重故障）不用宏：先切到专用栈，避免坏栈导致三故障
ISR_NOERR 9      ; 协处理器段越界（保留）
ISR_ERR   10     ; 非法 TSS
ISR_ERR   11     ; 段不存在
ISR_ERR   12     ; 栈错误
ISR_ERR   13     ; 通用保护错误
ISR_ERR   14     ; 缺页
ISR_NOERR 15     ; 保留
ISR_NOERR 16     ; x87 浮点错误
ISR_ERR   17     ; 对齐检查
ISR_NOERR 18     ; 机器检查
ISR_NOERR 19     ; SIMD 浮点错误
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; ---- 系统调用门：向量 0x80（Native ABI） ----
global isr128
isr128:
    cli
    push dword 0
    push dword 128
    jmp isr_common

; ---- AP LAPIC timer：向量 0x30 ----
global isr_ap_timer
isr_ap_timer:
    cli
    push dword 0
    push dword 0x30
    jmp isr_common

; ---- AP IPI：向量 0x31 ----
global isr_ap_ipi
isr_ap_ipi:
    cli
    push dword 0
    push dword 0x31
    jmp isr_common

; ---- LAPIC spurious：向量 0xFF，只 iret，不 EOI ----
global isr_spurious
isr_spurious:
    cli
    push dword 0
    push dword 0xFF
    jmp isr_common

; ---- 过渡门：向量 0x81（旧原型 POSIX 风格 ABI） ----
global isr129
isr129:
    cli
    push dword 0
    push dword 129
    jmp isr_common

; ---- 通用异常入口 ----
isr_common:
    pushad                          ; 保存通用寄存器
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10                    ; 切到内核平坦数据段
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp                        ; 把现场指针作为参数传给 C
    call isr_dispatch
    add esp, 4

    pop gs
    pop fs
    pop es
    pop ds
    popad
    add esp, 8                      ; 丢掉 vec 和 err
    iret

; ---- IRQ 桩：向量 32 ~ 47 ----
%macro IRQ_STUB 2
global irq%1
irq%1:
    cli
    push dword 0
    push dword %2
    jmp irq_common
%endmacro

IRQ_STUB 0,  32
IRQ_STUB 1,  33
IRQ_STUB 2,  34
IRQ_STUB 3,  35
IRQ_STUB 4,  36
IRQ_STUB 5,  37
IRQ_STUB 6,  38
IRQ_STUB 7,  39
IRQ_STUB 8,  40
IRQ_STUB 9,  41
IRQ_STUB 10, 42
IRQ_STUB 11, 43
IRQ_STUB 12, 44
IRQ_STUB 13, 45
IRQ_STUB 14, 46
IRQ_STUB 15, 47

; ---- 通用 IRQ 入口 ----
irq_common:
    pushad
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp
    call irq_dispatch
    add esp, 4

    pop gs
    pop fs
    pop es
    pop ds
    popad
    add esp, 8
    iret

; =============================================================================
; 入口地址表：C 侧按向量号取地址来填 IDT，不必逐个写 extern 声明
; =============================================================================
section .rodata

; =============================================================================
; 8 号：双重故障。当前栈可能已经损坏，先切到静态专用栈再进通用入口。
; 先把 CPU 压在旧栈上的 err/eip/cs/eflags 读出来，重建到新栈。
; =============================================================================
section .text
global isr8
isr8:
    cli
    mov eax, [esp]          ; err
    mov ebx, [esp + 4]      ; eip
    mov ecx, [esp + 8]      ; cs
    mov edx, [esp + 12]     ; eflags

    mov esp, df_stack_top
    push edx
    push ecx
    push ebx
    push eax
    push dword 8
    jmp isr_common

section .bss
align 16
df_stack_bottom:
    resb 4096
df_stack_top:

section .text

global isr_stub_table
isr_stub_table:
%assign i 0
%rep 32
    dd isr %+ i
%assign i i+1
%endrep

global irq_stub_table
irq_stub_table:
%assign i 0
%rep 16
    dd irq %+ i
%assign i i+1
%endrep
