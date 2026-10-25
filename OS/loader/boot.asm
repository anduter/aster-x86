; =============================================================================
; boot.asm - 主引导记录（历史副本，当前构建不会用到本文件）
;
; 说明：真正被 Makefile / build.bat 汇编的是 src/boot/boot.asm。
;       本文件是早期放在 loader 目录下的同一份引导扇区代码，保留仅作参考，
;       逻辑与 src/boot/boot.asm 一致（已去掉调试标记）。
; =============================================================================
BITS 16
org 0x7C00

CR equ 0x0D
LF equ 0x0A

%include "BOOT_DRIVE.asm"

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [BOOT_DRIVE], dl        ; 保存引导驱动器号
    sti

    ; 切换到 80x25 彩色文本模式（同时清屏）
    mov ah, 0x00
    mov al, 0x03
    int 0x10

    mov si, msgboot
    call print_string

    call read_loader
    jc  loader_fail

    mov dl, [BOOT_DRIVE]
    jmp 0x0000:0x8000

; ----------------------------- 加载失败处理 -----------------------------
loader_fail:
    mov si, fail_msg
    call print_string
Done:
    hlt
    jmp Done

; =============================================================================
; read_loader - 两批写死的读盘（不使用 pusha/popa，尽量省字节）
; 返回：CF=0 成功 / CF=1 失败
; =============================================================================
read_loader:
    ; ---- 第 1 批：柱面0 磁头0 扇区2 起，读 17 个扇区到 0x8000 ----
    mov ah, 0x02
    mov al, 17
    mov ch, 0
    mov cl, 2
    mov dh, 0
    mov dl, [BOOT_DRIVE]
    mov bx, 0x8000
    int 0x13
    jc  .fail

    ; ---- 第 2 批：柱面0 磁头1 扇区1 起，读 15 个扇区到 0xA200 ----
    mov ah, 0x02
    mov al, 15
    mov ch, 0
    mov cl, 1
    mov dh, 1
    mov dl, [BOOT_DRIVE]
    mov bx, 0xA200
    int 0x13
    jc  .fail

    ; ---- 校验 0xBFFA 处的 "LOADER" 签名 ----
    mov si, 0xBFFA
    mov di, loader_sig
    mov cx, 6
.check:
    mov al, [si]
    cmp al, [di]
    jne .fail
    inc si
    inc di
    loop .check

    clc
    ret
.fail:
    stc
    ret

%include "print.asm"

loader_sig db 'LOADER'
msgboot    db "Booting...", LF, 0
fail_msg   db "ERROR: load failed", LF, 0

times 510 - ($ - $$) db 0
dw 0xAA55
