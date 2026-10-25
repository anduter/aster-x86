; =============================================================================
; ap_trampoline.asm - AP 实模式 -> 保护模式 trampoline
;
; BSP 把本 blob 复制到物理 0x8000，然后 INIT/SIPI(vector=0x08)。
; AP 从 0x8000 开始执行，加载临时 GDT、打开 PAE/NXE、装入 BSP 的 CR3，
; 最后跳到内核里的 ap_entry（地址由 BSP 写在 0x7FF8）。
;
; 数据契约（BSP 在发 SIPI 前写入）：
;   0x7FF0 : AP 栈顶
;   0x7FF4 : BSP 的 PDPT/CR3
;   0x7FF8 : ap_entry 线性地址
; =============================================================================
[BITS 16]
[ORG 0x8000]

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    lgdt [gdtr]

    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:pm_entry

[BITS 32]
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, [0x7FF0]

    mov eax, cr4
    or eax, (1 << 5)              ; CR4.PAE
    mov cr4, eax

    mov ecx, 0xC0000080           ; IA32_EFER
    rdmsr
    or eax, (1 << 11)             ; EFER.NXE
    wrmsr

    mov eax, [0x7FF4]
    mov cr3, eax

    mov eax, cr0
    or eax, 0x80010000            ; CR0.PG | CR0.WP
    mov cr0, eax

    jmp [0x7FF8]

align 8
gdt:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF         ; 0x08: flat kernel code
    dq 0x00CF92000000FFFF         ; 0x10: flat kernel data
gdt_end:

gdtr:
    dw gdt_end - gdt - 1
    dd gdt
