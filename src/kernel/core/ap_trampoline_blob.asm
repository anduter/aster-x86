; 把构建期生成的 AP trampoline 二进制嵌入内核 rodata，由 BSP 复制到 0x8000。
section .rodata
global ap_trampoline_blob_start
global ap_trampoline_blob_end

ap_trampoline_blob_start:
    incbin "obj/ap_trampoline.bin"
ap_trampoline_blob_end:
