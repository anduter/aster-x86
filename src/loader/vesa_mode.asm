; =============================================================================
; vesa_mode.asm - 切换到 VBE 目标图形模式
;
; 调用 VBE 功能 0x4F02，把 VBE_MODE_NUM 中记录的模式号设为当前模式。
; 返回：CF=0 成功 / CF=1 失败
; 说明：失败原因由调用者统一提示，这里不再往屏幕打标记字符
; =============================================================================

set_vesa_mode:
    pushad
    cmp word [VBE_MODE_NUM], 0      ; 没探测到目标模式，直接失败
    je  .fail
    mov bx, [VBE_MODE_NUM]
    or  bx, 0x4000                  ; bit14=1：保留当前显存窗口内容
    mov ax, 0x4F02                  ; VBE 设置模式
    int 0x10
    cmp ax, 0x004F                  ; 0x004F = 成功
    jne .fail
    clc
    popad
    ret
.fail:
    stc
    popad
    ret
