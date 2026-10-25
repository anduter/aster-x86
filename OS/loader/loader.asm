; =============================================================================
; loader.asm - 第二阶段加载器（第 1~32 号扇区，共 32 个扇区 / 16384 字节）
;
; 由 boot 读入 0x8000 并跳转执行，完成以下工作：
;   1. 重新设置段寄存器，保存引导驱动器号
;   2. check_cpu：确认 CPU 支持 386+（Unreal Mode 的最低要求）
;   3. collect_sysinfo：采集内存/CPU/SMBIOS/显卡/磁盘信息到 0x5000
;   4. load_kernel：把内核读进低端缓冲区，再在 Unreal Mode 下复制到 1MB
;   5. set_vesa_mode：切换到 VBE 目标图形模式
;   6. enter_pm：进入 32 位保护模式并跳转到内核
;
; 约定：整个文件固定 16384 字节，最后 6 字节是 "LOADER" 签名，供 boot 校验。
; 构建：nasm -f bin -i src/loader/ src/loader/loader.asm -o obj/loader.bin
;
; 屏幕输出只保留对使用者有意义的阶段提示和错误原因，方便一眼看出卡在哪一步；
; 由于 BIOS 文本模式没有中文字库，这些提示使用英文。
; =============================================================================
org 0x8000
[bits 16]

start:
    ; 不依赖 boot 留下的段寄存器，全部重新置 0
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov [BOOT_DRIVE], dl        ; 先保存引导驱动器号（必须在任何输出之前）

    mov si, msg_stage2
    call print_string

    ; ---- 1. 检查 CPU ----
    call check_cpu
    jc  cpu_error
    mov si, msg_cpu_ok
    call print_string

    ; ---- 2. 采集系统信息到 0x5000 ----
    mov si, msg_sysinfo
    call print_string
    call collect_sysinfo

    ; ---- 3. 加载内核到 1MB ----
    mov si, msg_load_kernel
    call print_string
    call load_kernel
    jc  kernel_error
    mov si, msg_kernel_ok
    call print_string

    ; ---- 4. 切换 VBE 图形模式 ----
    ;    模式设置失败会导致内核往未映射的物理地址画图，所以这里直接报错停下
    mov si, msg_set_vbe
    call print_string
    call set_vesa_mode
    jc  vesa_error
    mov si, msg_vbe_ok
    call print_string

    ; ---- 5. 进入保护模式，先验证内核签名，再跳内核 ----
    mov si, msg_verify
    call print_string
    mov si, msg_enter_pm
    call print_string
    call enter_pm

Done:
    hlt
    jmp Done

; ----------------------------- 错误处理 -----------------------------
cpu_error:
    mov si, msg_cpu_err
    call print_string
    jmp Done

vesa_error:
    mov si, msg_vbe_err
    call print_string
    jmp Done

kernel_error:
    ; AL 是 load_kernel 返回的错误码：0=成功 1=读盘失败 2=内核头非法 3=内核过大
    mov si, msg_kernel_err
    call print_string
    cmp al, 1
    je  .read_err
    cmp al, 3
    je  .too_big
    mov si, msg_bad_kernel
    call print_string
    jmp Done
.too_big:
    mov si, msg_too_big
    call print_string
    jmp Done
.read_err:
    mov si, msg_disk_err
    call print_string
    mov al, [disk_status]
    call print_hex_al
    mov si, msg_lf
    call print_string
    jmp Done

; =============================================================================
; check_cpu - 检查 CPU 是否支持 386+
; 返回：CF=0 支持（386 及以上）/ CF=1 不支持（286 及更早）
; 原理：尝试翻转 EFLAGS 的 AC 位（bit18）；386+ 可以翻转，286 做不到
; =============================================================================
check_cpu:
    pushfd
    pushad
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 0x40000
    push eax
    popfd
    pushfd
    pop eax
    xor eax, ecx
    and eax, 0x40000
    setnz al
    mov [cpu_ok], al
    popad
    popfd                       ; 恢复调用前的 EFLAGS（AC 位不能泄漏）
    cmp byte [cpu_ok], 0
    je  .no
    clc
    ret
.no:
    stc
    ret

; =============================================================================
; 例程与数据（按依赖顺序包含）
; =============================================================================
%include "BOOT_DRIVE.asm"
%include "kernel_loader.asm"
%include "print.asm"
%include "sysinfo.asm"
%include "vesa_mode.asm"
%include "pm_entry.asm"

; ----------------------------- 屏幕提示信息 -----------------------------
msg_stage2      db "Stage 2: loader running", LF, 0
msg_cpu_ok      db "  [1/5] CPU check: 386+ OK, unreal mode available", LF, 0
msg_sysinfo     db "  [2/5] collecting system info (memory/CPU/SMBIOS/VBE/disk)", LF, 0
msg_load_kernel db "  [3/5] loading kernel to 0x00100000", LF, 0
msg_kernel_ok   db "        kernel header OK, image copied", LF, 0
msg_set_vbe     db "  [4/5] setting VBE mode 0x11B (1280x1024x24)", LF, 0
msg_vbe_ok      db "        VBE mode set OK", LF, 0
msg_enter_pm    db "  [5/5] entering protected mode", LF, 0
msg_verify      db "  [5/5] verifying kernel signature", LF, 0

msg_cpu_err     db "ERROR: CPU too old, needs 386+ for unreal mode", LF, 0
msg_kernel_err  db "ERROR: kernel load failed", LF, 0
msg_bad_kernel  db "  invalid kernel header (bad magic or zero size)", LF, 0
msg_too_big     db "  kernel image too large (over 1.4MB floppy payload)", LF, 0
msg_disk_err    db "  disk read failed, int13 status="
msg_lf          db LF, 0
msg_vbe_err     db "ERROR: VBE mode 0x11B not available", LF, 0
cpu_ok          db 0

; 补齐到 0x1800，再把 32 位内核签名验证器嵌到 0x9800。
; 验证器通过后跳到 0x100008；失败只停住并写 COM1。
; 路径可由 build.ps1 用 -DBOOT_VERIFY_BIN="..." 覆盖（并行会话各有 obj 目录）。
%ifndef BOOT_VERIFY_BIN
%define BOOT_VERIFY_BIN "obj/boot_verify.bin"
%endif
times 0x1800-($-$$) db 0
incbin BOOT_VERIFY_BIN

; 补齐到 16384 字节（32 个扇区），末尾 6 字节留给签名
times 16384-($-$$)-6 db 0
db 'LOADER'             ; 末尾签名（boot 在 0xBFFA 处校验）
