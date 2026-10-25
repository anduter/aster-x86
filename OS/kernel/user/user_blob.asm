; =============================================================================
; user_blob.asm - 把用户程序 ELF 嵌进内核镜像
;
; 构建顺序：user_shell.c -> user_shell.elf -> 本文件 incbin（ELF 本身）。
; 内核的 user_exec 会解析 ELF 程序头，把 PT_LOAD 段映射进用户地址空间。
; =============================================================================
[bits 32]
section .rodata

global user_shell_blob_start
global user_shell_blob_end

user_shell_blob_start:
    incbin "user_shell.elf"
user_shell_blob_end:
