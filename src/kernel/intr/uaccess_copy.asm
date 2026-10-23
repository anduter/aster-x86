; =============================================================================
; intr/uaccess_copy.asm - 用户内存拷贝（带异常表标签）
;
; int uaccess_copy(void *dst, const void *src, uint32_t len)
;   返回 0 = 成功，1 = 中途缺页
;
; [uaccess_copy_start, uaccess_copy_end) 这一段登记进通用异常修复表，
; 缺页时 eip 会被改成 uaccess_copy_fixup，直接走失败返回。
; 文件名不叫 uaccess.asm：Makefile 里 .c 和 .asm 会生成同名 .o，会互相覆盖。
; =============================================================================
[bits 32]
section .text

global uaccess_copy
global uaccess_copy_start
global uaccess_copy_end
global uaccess_copy_fixup

extern smap_enabled

uaccess_copy:
    push edi
    push esi
    push ecx

    mov edi, [esp + 16]         ; dst
    mov esi, [esp + 20]         ; src
    mov ecx, [esp + 24]         ; len
    cld

    ; SMAP 启用时，内核访问用户内存前必须 STAC（0F 01 CB）。
    cmp dword [smap_enabled], 0
    je .no_stac
    db 0x0F, 0x01, 0xCB
.no_stac:

uaccess_copy_start:
    rep movsb
uaccess_copy_end:

    ; 关闭访问窗口：CLAC（0F 01 CA）。
    cmp dword [smap_enabled], 0
    je .no_clac
    db 0x0F, 0x01, 0xCA
.no_clac:
    xor eax, eax                ; 成功
    pop ecx
    pop esi
    pop edi
    ret

uaccess_copy_fixup:
    cmp dword [smap_enabled], 0
    je .fixup_no_clac
    db 0x0F, 0x01, 0xCA
.fixup_no_clac:
    mov eax, 1                  ; 缺页：失败
    pop ecx
    pop esi
    pop edi
    ret
