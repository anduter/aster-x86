; =============================================================================
; boot.asm - 主引导记录（MBR，第 0 号扇区，共 512 字节）
;
; 职责：
;   1. 建立实模式运行环境，保存 BIOS 传入的引导驱动器号（DL）
;   2. 分两批把第二阶段加载器（第 1~32 号扇区，共 16384 字节）读入 0x8000
;   3. 校验加载器末尾 6 字节的 "LOADER" 签名
;   4. 跳转到 0x0000:0x8000 执行加载器
;
; 磁盘布局：
;   LBA1..17  = 加载器前半部分（柱面0 磁头0 扇区2..18，共 17 个扇区）
;   LBA18..32 = 加载器后半部分（柱面0 磁头1 扇区1..15，共 15 个扇区）
;
; 注意：本文件的第一条指令必须位于最前面，所以 BOOT_DRIVE 变量放到文件
;       末尾的数据区引入。若把 BOOT_DRIVE 放在偏移 0，CPU 会先执行
;       `00 FA`（add dl,bh），把 DL 里的引导驱动器号悄悄改坏。
;
; 屏幕提示只能用英文：BIOS 文本模式没有中文字库。
; =============================================================================
BITS 16
org 0x7C00

CR equ 0x0D
LF equ 0x0A

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [BOOT_DRIVE], dl        ; 保存引导驱动器号（在任何操作之前）
    sti

    ; 切换到 80x25 彩色文本模式（同时清屏）
    mov ah, 0x00
    mov al, 0x03
    int 0x10

    mov si, msg_boot
    call print_string

    call read_loader
    jc  loader_fail

    mov dl, [BOOT_DRIVE]        ; 加载器还要用驱动器号继续读内核
    jmp 0x0000:0x8000

; ----------------------------- 加载失败处理 -----------------------------
loader_fail:
    mov si, msg_fail
    call print_string

    cmp al, 2
    je  .bad_sig

    ; AL=1：读盘失败，把 BIOS 返回的 int 0x13 状态码显示出来
    mov si, msg_read_err
    call print_string
    mov al, [io_status]
    call print_hex_al
    mov si, msg_lf
    call print_string
    jmp Done

.bad_sig:
    ; AL=2：签名不匹配，说明读到的内容不是加载器
    mov si, msg_bad_sig
    call print_string

Done:
    hlt
    jmp Done

; =============================================================================
; read_loader - 把加载器读入 0x8000
; 返回：CF=0 成功；CF=1 失败（AL=1 读盘失败 / AL=2 签名不匹配）
;
; 两批读取的落点：
;   第 1 批 柱面0 磁头0 扇区2 起 17 个扇区 -> 0x8000（0x8000+0x2200 = 0xA200）
;   第 2 批 柱面0 磁头1 扇区1 起 15 个扇区 -> 0xA200（0xA200+0x1E00 = 0xC000）
; 合计 32 个扇区 = 16384 字节，正好覆盖 0x8000~0xBFFF。
;
; 为什么写死：MBR 只有 512 字节，放不下通用的 LBA->CHS 循环。
; 每批读取前都显式重设 ES，不依赖上一次调用留下的段值。
; =============================================================================
read_loader:
    push es
    xor ax, ax
    mov es, ax

    ; ---- 第 1 批：17 个扇区 ----
    mov al, 17
    mov ch, 0
    mov cl, 2
    mov dh, 0
    mov bx, 0x8000
    call disk_read
    jc  .fail

    ; ---- 第 2 批：15 个扇区 ----
    xor ax, ax
    mov es, ax
    mov al, 15
    mov ch, 0
    mov cl, 1
    mov dh, 1
    mov bx, 0xA200
    call disk_read
    jc  .fail

    ; ---- 校验 0xBFFA 处的 "LOADER" 签名 ----
    mov si, 0xBFFA
    mov di, loader_sig
    mov cx, 6
.check:
    mov al, [si]
    cmp al, [di]
    jne .bad_sig
    inc si
    inc di
    loop .check

    pop es
    xor al, al
    clc
    ret

.fail:
    pop es
    mov al, 1
    stc
    ret

.bad_sig:
    pop es
    mov al, 2
    stc
    ret

; =============================================================================
; disk_read - 调用 BIOS int 0x13(AH=0x02) 读取扇区
; 输入：ES:BX = 缓冲区，AL = 扇区数，CH = 柱面号，CL = 起始扇区号，DH = 磁头号
; 输出：CF=0 成功 / CF=1 失败（直接沿用 int 0x13 的结果）
;       失败时把 BIOS 返回的状态码存入 io_status，供上层显示
; =============================================================================
disk_read:
    mov ah, 0x02
    mov dl, [BOOT_DRIVE]
    int 0x13
    jnc .ok
    mov [io_status], ah         ; int 0x13 失败时 AH = 错误码
.ok:
    ret

%include "print.asm"

; -----------------------------------------------------------------------------
; 变量与字符串放在代码之后（原因见文件头的说明）
; -----------------------------------------------------------------------------
%include "BOOT_DRIVE.asm"
io_status    db 0
loader_sig   db 'LOADER'
msg_boot     db "Booting...", LF, 0
msg_fail     db "ERROR: load failed", LF, 0
msg_read_err db "  int13 status="
msg_lf       db LF, 0
msg_bad_sig  db "  loader signature wrong", LF, 0

times 510 - ($ - $$) db 0
dw 0xAA55
