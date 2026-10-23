; =============================================================================
; print.asm - 实模式文本输出例程（加载器使用）
;
; 只保留加载阶段真正用到的两个函数，避免占用 16KB 的加载器空间。
; =============================================================================
CR equ 0x0D
LF equ 0x0A

; -----------------------------------------------------------------------------
; print_string - 输出以 NUL 结尾的字符串（BIOS int 0x10，AH=0x0E 逐字符）
; 输入：DS:SI = 字符串首地址
; 说明：'\n'(LF) 自动展开成 '\r'+'\n'，保证换行后光标回到行首
; -----------------------------------------------------------------------------
print_string:
    pusha
.next:
    lodsb
    cmp al, 0
    je  .done
    cmp al, LF
    je  .newline
    cmp al, CR
    je  .carriage
    mov ah, 0x0E
    int 0x10
    jmp .next
.newline:
    mov ah, 0x0E
    mov al, CR
    int 0x10
    mov al, LF
    int 0x10
    jmp .next
.carriage:
    mov ah, 0x0E
    mov al, CR
    int 0x10
    jmp .next
.done:
    popa
    ret

; -----------------------------------------------------------------------------
; print_hex_al - 以 "0xNN" 形式显示 AL 中的字节（用于打印错误码）
; 输入：AL = 要显示的字节
; 说明：显示前先压栈保存，因为 int 0x10 可能改动 AL
; -----------------------------------------------------------------------------
print_hex_al:
    mov ah, 0x0E
    mov bx, 0x0007
    push ax
    mov al, '0'
    int 0x10
    mov al, 'x'
    int 0x10
    pop ax

    push ax                     ; 先显示高 4 位
    shr al, 4
    call .digit
    pop ax
    call .digit                 ; 再显示低 4 位
    ret

.digit:
    push ax
    and al, 0x0F
    cmp al, 10
    jb  .num
    add al, 'A' - 10            ; 10~15 显示为 A~F
    jmp .emit
.num:
    add al, '0'
.emit:
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    pop ax
    ret
