; 把构建期生成的 AP trampoline 二进制嵌入内核 rodata，由 BSP 复制到 0x8000。
; 路径可由 build.ps1 用 -DAP_TRAMPOLINE_BIN="..." 覆盖（并行会话各有 obj 目录）。
%ifndef AP_TRAMPOLINE_BIN
%define AP_TRAMPOLINE_BIN "obj/ap_trampoline.bin"
%endif
section .rodata
global ap_trampoline_blob_start
global ap_trampoline_blob_end

ap_trampoline_blob_start:
    incbin AP_TRAMPOLINE_BIN
ap_trampoline_blob_end:
