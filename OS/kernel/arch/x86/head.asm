; =============================================================================
; head.asm - 32 位内核入口（内核镜像的第 8 个字节处）
;
; 职责：
;   1. 装载平坦数据段（0x10）和内核栈（0x90000）
;   2. 调用 C 语言内核入口 kernel_main
;
; 与 linker.ld 配合：
;   内核镜像最前面是 8 字节的内核头（魔数 'KRNL' + 内核字节数），
;   紧接着就是本文件所在的 .entry 段。该段不要求对齐，
;   因此 kernel_entry 的地址固定为 0x100000 + 8 = 0x100008，
;   与 loader/pm_entry.asm 里写死的跳转地址一致。
;
; 注意：不要把入口并回 .text。.text 按 16 字节对齐，会被前推到
; 0x100010，入口就和 0x100008 差了 8 字节，CPU 会先去执行那 8 字节填充。
;
; 构建：nasm -f elf32 -o head.o head.asm
; =============================================================================
[bits 32]
global kernel_entry
extern kernel_main

; 入口段：不要求对齐（align=1），链接脚本把它放在内核头之后
section .entry progbits alloc exec nowrite align=1
kernel_entry:
    ; ---- 把段寄存器都设成平坦数据段 ----
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000            ; 内核栈顶
    cld                         ; C ABI 要求 DF=0

    ; ---- 清零 BSS（镜像文件不含 BSS，必须自己清） ----
    extern __bss_start
    extern __bss_end
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb

    call kernel_main            ; 进入 C 内核
.halt:
    hlt
    jmp .halt
