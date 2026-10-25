; =============================================================================
; user_entry.asm - GDT 加载与进入 ring3
;
; 文件名刻意不叫 gdt.asm：Makefile 里 .c 和 .asm 会生成同名 .o，
; 同目录下重名会互相覆盖，导致 gdt_init 之类的符号丢失。
; =============================================================================
[bits 32]
section .text

; ---- void gdt_flush(void *gdtr) ----
global gdt_flush
gdt_flush:
    mov eax, [esp + 4]
    lgdt [eax]

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:.reload        ; 远跳刷新 CS
.reload:
    ret

; ---- void tss_flush(void) ----
global tss_flush
tss_flush:
    mov ax, 0x28
    ltr ax
    ret

; =============================================================================
; void user_enter(uint32_t entry, uint32_t stack_top)
;
; 手工构造 iret 现场进入 ring3：
;   压入 SS3、ESP3、EFLAGS（IF=1、IOPL=0）、CS3、EIP3
; iret 时 CPU 检查目标 CS 的 DPL=3，做特权级切换并清空段寄存器缓存。
; =============================================================================
global user_enter
user_enter:
    mov edi, [esp + 4]      ; entry
    mov esi, [esp + 8]      ; stack_top

    cli

    mov ax, 0x23            ; 用户数据段
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push dword 0x23         ; SS
    push esi                ; ESP
    pushfd
    pop ecx
    or ecx, 0x200           ; IF=1
    and ecx, 0xFFFFCFFF     ; IOPL=0
    push ecx                ; EFLAGS
    push dword 0x1B         ; CS
    push edi                ; EIP
    iretd

; =============================================================================
; void user_enter_regs(uint32_t entry, uint32_t stack_top,
;                     uint32_t retval, const uint32_t *regs)
; regs = { ebx, ebp, esi, edi }
; fork 的子进程要“从父进程中断的那条指令继续”，所以除了 eip/esp，
; 还必须恢复 callee-saved 寄存器（尤其 ebp，否则局部变量会写到内核栈地址）。
; =============================================================================
global user_enter_regs
user_enter_regs:
    mov eax, [esp + 4]      ; entry
    mov ecx, [esp + 8]      ; stack_top
    mov edx, [esp + 12]     ; retval
    mov esi, [esp + 16]     ; regs

    push dword 0x23
    push ecx
    pushfd
    pop ecx
    or ecx, 0x200
    and ecx, 0xFFFFCFFF
    push ecx
    push dword 0x1B
    push eax

    mov eax, edx            ; 返回值
    mov ebx, [esi + 0]
    mov ebp, [esi + 4]
    mov ecx, [esi + 8]      ; 父进程 esi
    mov edx, [esi + 12]     ; 父进程 edi
    xchg ecx, edx
    mov esi, edx
    mov edi, ecx
    iretd
global user_enter_ret
user_enter_ret:
    mov edi, [esp + 4]      ; entry
    mov esi, [esp + 8]      ; stack_top
    mov eax, [esp + 12]     ; retval

    cli

    mov dx, 0x23
    mov ds, dx
    mov es, dx
    mov fs, dx
    mov gs, dx

    push dword 0x23
    push esi
    pushfd
    pop ecx
    or ecx, 0x200
    and ecx, 0xFFFFCFFF
    push ecx
    push dword 0x1B
    push edi
    iretd
