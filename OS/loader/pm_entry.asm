; =============================================================================
; pm_entry.asm - 进入 32 位保护模式
;
; 从实模式切到 32 位保护模式：设置 GDT/IDT，初始化段寄存器和栈，
; 然后跳到内核执行。GDT 是平坦模型（0x08 代码段 / 0x10 数据段），
; IDT 目前是一张空表（还没有中断处理程序）。
; =============================================================================
PM_CODE_SEL equ 0x08
PM_DATA_SEL equ 0x10

ALIGN 4
gdt_pm_start:
    dq 0
gdt_pm_code:                   ; 0x08：平坦 32 位代码段
    dw 0xFFFF
    dw 0
    db 0
    db 0x9A
    db 0xCF
    db 0
gdt_pm_data:                   ; 0x10：平坦 32 位数据段
    dw 0xFFFF
    dw 0
    db 0
    db 0x92
    db 0xCF
    db 0
gdt_pm_end:
gdt_pm_desc:
    dw gdt_pm_end - gdt_pm_start - 1
    dd gdt_pm_start

ALIGN 4
idt_pm_start:
    times 256 dq 0              ; 空 IDT（占位，暂时用不到）
idt_pm_end:
idt_pm_desc:
    dw idt_pm_end - idt_pm_start - 1
    dd idt_pm_start

[bits 16]
enter_pm:
    cli
    lgdt [gdt_pm_desc]
    lidt [idt_pm_desc]
    mov eax, cr0
    or eax, 1
    mov cr0, eax                ; PE=1，进入保护模式
    jmp dword PM_CODE_SEL:pm_start32   ; 装载代码段选择子并跳转

[bits 32]
pm_start32:
    mov ax, PM_DATA_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000            ; 验证器栈放到 1MB 以下安全区，避免覆盖 sysinfo
    jmp dword PM_CODE_SEL:0x9800   ; 跳到嵌入 loader 的内核签名验证器
