; =============================================================================
; kernel_loader.asm - 内核加载与复制
;
; 职责：
;   1. 用实模式 int 0x13(AH=0x02) 按 CHS 把内核分块读入低端缓冲区
;   2. 校验内核头部魔数 'KRNL'，并从头部取内核字节数、换算成扇区数
;   3. 进入 Unreal Mode，把每个分块复制到 1MB（0x100000）
;   4. 把 ES/FS/GS 恢复成实模式段，避免后面的 BIOS 调用出错
;
; 内存预算（这正是以前会踩坏的地方）：
;   LOAD_BUF = 0x1000，CHUNK_SECTORS*512 = 14336 字节 -> 0x1000..0x47FF
;   0x0000..0x04FF ：中断向量表 + BIOS 数据区   （不能碰）
;   0x5000..0x58FF ：系统信息表（E820 / VBE）  （不能碰）
;   0x7C00         ：boot.asm 留下的栈          （不能碰！）
;   0x8000..0xBFFF ：加载器自身的代码           （不能碰！）
;   早期版本把整个内核读到 0x6000，第 14 个扇区刚好落在 0x7C00，
;   会覆盖正在执行的那层调用的返回地址。改成分块读取后，
;   “内核大了 512 字节系统就挂”这一类问题就消除了。
;
; ★ 关键点：16 位模式下 rep movsd 只会自动加上操作数大小前缀(66)，
;   缺少地址大小前缀(67)时 DI 按 16 位解释，0x100000 会被截断成 0。
;   因此必须在 rep movsd 前手动加 db 0x67，强制使用 32 位 ESI/EDI。
; =============================================================================
KERNEL_OFFSET  equ 0x100000    ; 内核最终加载的物理地址
LOAD_BUF       equ 0x1000      ; 分块读缓冲区起始地址
CHUNK_SECTORS  equ 28          ; 28*512 = 14336 -> 缓冲区结尾 0x47FF
KERNEL_LBA     equ 33          ; 内核起始 LBA（LBA0=boot，LBA1~32=loader）
KERNEL_MAGIC   equ 0x4C4E524B  ; 'K','R','N','L' 的小端序
MAX_KERNEL_SECTORS equ 2847    ; 1.44MB 软盘 LBA33 之后的实际容量（1,423.5KB）
SPT            equ 18          ; 每磁道扇区数（1.44MB 软盘）
HEADS          equ 2           ; 磁头数

cnt        dw 0
read_cnt   dw 0
cur_cyl    dw 0
cur_head   db 0
cur_sector db 0
target_buf dw LOAD_BUF
disk_status db 0               ; int 0x13 失败时的错误码（供 loader 显示）
chunk_cnt  dd 0                ; 本次分块要读的扇区数
copy_cnt   dd 0                ; 本次分块要复制的扇区数
copy_dst   dd KERNEL_OFFSET    ; 下一个复制目标地址（每块递增）
remaining  dd 0                ; 还没读的扇区数

; =============================================================================
; Unreal Mode：让 16 位代码也能访问 1MB 以上的内存
; 步骤：打开 A20 -> 临时置 PE=1 -> 把 ES/FS/GS 装入平坦段
;       -> 清 PE 回到实模式。ES/FS/GS 会保留平坦段的基址和界限，
;       之后即可寻址 0~4GB。
; =============================================================================
ALIGN 4
gdt_start:
    dq 0
gdt_code:                       ; 0x08：平坦 32 位代码段
    dw 0xFFFF
    dw 0
    db 0
    db 0x9A
    db 0xCF
    db 0
gdt_data:                       ; 0x10：平坦 32 位数据段
    dw 0xFFFF
    dw 0
    db 0
    db 0x92
    db 0xCF
    db 0
gdt_end:
gdt_desc:
    dw gdt_end - gdt_start - 1
    dd gdt_start

enable_a20:                     ; 通过 I/O 端口 0x92 快速打开 A20
    push ax
    in al, 0x92
    or al, 0x02
    and al, 0xFE                ; bit0 是 fast reset，不能原样写回
    out 0x92, al
    pop ax
    ret

enter_unreal:
    cli
    call enable_a20
    lgdt [gdt_desc]
    mov eax, cr0
    or eax, 1
    mov cr0, eax                ; PE=1，进入保护模式
    mov ax, 0x10
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov eax, cr0
    and eax, 0xFFFFFFFE
    mov cr0, eax                ; PE=0，回到实模式（段寄存器仍保留平坦属性）
    ret                         ; 由调用者保持 cli，复制完成后再恢复 IF

; =============================================================================
; 读盘状态
; =============================================================================
kernel_sectors dd 0
lba            dd 0
kernel_err     db 0

; =============================================================================
; disk_read_chs - 实模式 CHS 读盘
; 输入：CX = 要读的扇区数，[lba] = 起始 LBA，[target_buf] = 目标地址
; 输出：CF=0 成功 / CF=1 失败（错误码存进 disk_status）
; 说明：内部自带 LBA->CHS 换算，并会在轨道/磁头边界自动续读。
;       int 0x13 之前 ES:BX 必须指向低 1MB 内；必须先设好 ES
;       （xor ax,ax; mov es,ax）再设置 AH/AL/CH/CL/DH/DL，
;       否则 AH=0x02 会被覆盖，int 0x13 就变成了“复位磁盘”并读到全 0。
; =============================================================================
disk_read_chs:
    pushad
    mov [cnt], cx
    ; LBA -> CHS：柱面 = lba/(磁头数*每道扇区数)，磁头 = (lba/每道扇区数)%磁头数，
    ;              扇区 = lba%每道扇区数 + 1
    xor edx, edx
    mov eax, [lba]
    mov ebx, HEADS * SPT
    div ebx
    mov [cur_cyl], ax
    mov eax, edx
    xor edx, edx
    mov ebx, SPT
    div ebx
    mov [cur_head], al
    inc edx
    mov [cur_sector], dl
.read_loop:
    mov cx, [cnt]
    test cx, cx
    jz  .done
    ; 一次最多读到当前磁道末尾
    movzx bx, byte [cur_sector]
    mov ax, SPT
    sub ax, bx
    inc ax                      ; 本磁道剩余扇区数
    cmp cx, ax
    jbe .set
    mov cx, ax
.set:
    mov [read_cnt], cx
    ; ---- 先设好 ES:BX，再设其余寄存器（顺序很关键！） ----
    xor ax, ax
    mov es, ax
    mov bx, [target_buf]
    mov ah, 0x02                ; 读扇区
    mov al, cl                  ; 本次读取扇区数
    mov ch, byte [cur_cyl]
    mov cl, byte [cur_sector]
    mov dh, byte [cur_head]
    mov dl, [BOOT_DRIVE]
    int 0x13
    jc  .fail
    ; ---- 更新缓冲区指针与 CHS ----
    mov ax, [read_cnt]
    sub [cnt], ax
    movzx eax, ax               ; eax = 本次扇区数
    shl eax, 9                  ; 换算成字节数
    add [target_buf], ax        ; 目标指针前移
    movzx ax, byte [cur_sector]
    add ax, [read_cnt]
    cmp ax, SPT
    ja  .wrap                   ; 越过当前磁道末尾
    mov [cur_sector], al
    jmp .read_loop
.wrap:
    sub ax, SPT
    mov [cur_sector], al
    inc byte [cur_head]
    cmp byte [cur_head], HEADS
    jb  .read_loop
    mov byte [cur_head], 0      ; 磁头回到 0
    inc word [cur_cyl]          ; 柱面 +1
    jmp .read_loop
.done:
    clc
    popad
    ret
.fail:
    mov [disk_status], ah       ; 记下 BIOS 返回的错误码
    stc
    popad
    ret

; =============================================================================
; copy_chunk - 把 [copy_cnt] 个扇区从 LOAD_BUF 复制到平坦段 ES:[copy_dst]
; 输入：[copy_cnt] = 扇区数（1..CHUNK_SECTORS），[copy_dst] = 目标地址
; 输出：[copy_dst] 前进 copy_cnt*512
; 说明：复制期间关中断，避免定时器 IRQ 在 ES 持有 4GB 平坦段时进来
; =============================================================================
copy_chunk:
    pushad
    pushfd
    cli                         ; 平坦 ES 存在期间禁止中断
    call enter_unreal
    mov esi, LOAD_BUF
    mov edi, [copy_dst]
    mov ecx, [copy_cnt]
    shl ecx, 7                  ; 扇区数 * 128 = 双字数
    cld                         ; 方向标志置 0（正向复制）
    db 0x67                     ; 强制 32 位地址大小（ESI/EDI）
    rep movsd
    mov eax, [copy_cnt]
    shl eax, 9                  ; 已复制的字节数
    add [copy_dst], eax
    ; ---- 把 ES/FS/GS 恢复成实模式段（平坦段会让后续 BIOS 调用出错） ----
    xor ax, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    popfd                       ; 恢复调用前的 IF
    popad
    ret

; =============================================================================
; load_kernel - 把内核加载到 1MB
; 输出：AL=0 成功 / 1 读盘失败 / 2 内核头非法 / 3 内核超出大小上限
;       （失败时同时置 CF）
; 流程：读第一个扇区 -> 校验头部 -> 计算扇区数 -> 复制第一个扇区
;       -> 其余扇区按 CHUNK_SECTORS 分块“读一块、复制一块”
; =============================================================================
load_kernel:
    pushad
    ; ---- 1. 读取含内核头的第一个扇区 ----
    mov word [target_buf], LOAD_BUF
    mov eax, KERNEL_LBA
    mov [lba], eax
    mov ecx, 1
    call disk_read_chs
    jc  .err_read

    ; ---- 2. 校验魔数 'KRNL' ----
    mov eax, [LOAD_BUF]
    cmp eax, KERNEL_MAGIC
    jne .err_bad

    ; ---- 3. 内核扇区数 = ceil(字节数/512)，并做上限检查 ----
    mov eax, [LOAD_BUF+4]       ; 内核字节数（由 linker.ld 写进头部）
    test eax, eax
    jz  .err_bad
    add eax, 511
    shr eax, 9
    mov [kernel_sectors], eax
    cmp eax, MAX_KERNEL_SECTORS
    ja  .err_big

    ; ---- 4. 先把已经读到的头部扇区复制过去 ----
    mov dword [copy_dst], KERNEL_OFFSET
    mov dword [copy_cnt], 1
    call copy_chunk

    ; ---- 5. 其余扇区逐块读取并复制 ----
    mov eax, [kernel_sectors]
    dec eax
    mov [remaining], eax
    mov eax, KERNEL_LBA + 1
    mov [lba], eax
.next_chunk:
    mov eax, [remaining]
    test eax, eax
    jz  .done
    cmp eax, CHUNK_SECTORS
    jbe .size_ok
    mov eax, CHUNK_SECTORS
.size_ok:
    mov [chunk_cnt], eax
    mov word [target_buf], LOAD_BUF
    mov ecx, [chunk_cnt]
    mov eax, [lba]
    call disk_read_chs
    jc  .err_read

    mov eax, [chunk_cnt]
    mov [copy_cnt], eax
    call copy_chunk

    mov eax, [chunk_cnt]
    add [lba], eax
    sub [remaining], eax
    jmp .next_chunk

.done:
    mov byte [kernel_err], 0
    clc
    jmp .ret
.err_read:
    mov byte [kernel_err], 1
    jmp .fail
.err_bad:
    mov byte [kernel_err], 2
    jmp .fail
.err_big:
    mov byte [kernel_err], 3
.fail:
    stc
.ret:
    popad
    mov al, [kernel_err]
    ret
