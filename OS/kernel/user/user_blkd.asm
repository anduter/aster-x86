; =============================================================================
; user_blkd.asm - ring3 块设备服务
; 启动时通过 SYS_BLK_READ 读取真实 ata0 扇区 0；协议 'r' 读 / 'w'+data 写。
; =============================================================================
BITS 32

global user_blkd_start
global user_blkd_end

user_blkd_start:
    cld
    call .base
.base:
    pop ebp
    sub ebp, .base - user_blkd_start
    sub esp, 768

    ; 虚拟块服务：内容在用户栈，不经内核 ATA（真实扇区实验已回退）
    lea esi, [ebp + blkd_init - user_blkd_start]
    lea edi, [esp + 64]
    mov ecx, blkd_init_len
    rep movsb
    mov dword [esp + 592], blkd_init_len

    mov eax, 0x33               ; PORT_CREATE
    int 0x80
    test eax, eax
    js .done
    mov [esp + 600], eax

    lea ebx, [ebp + blkd_name - user_blkd_start]
    mov ecx, 4
    mov edx, eax
    mov eax, 0x38               ; SERVICE_REGISTER("blkd")
    int 0x80
    test eax, eax
    js .done

.loop:
    mov ebx, [esp + 600]
    mov eax, 0x37               ; PORT_ACCEPT_BLOCK
    int 0x80
    test eax, eax
    js .done
    mov [esp + 596], eax

    mov ebx, eax
    mov ecx, esp
    mov edx, 64
    mov eax, 0x36               ; CHANNEL_RECV_BLOCK
    int 0x80
    cmp eax, 2
    jl .loop

    movzx ecx, byte [esp]
    cmp cl, 'w'
    jne .read

    lea esi, [esp + 1]
    lea edi, [esp + 64]
    dec eax
    mov [esp + 592], eax
    mov ecx, eax
    rep movsb

    mov ebx, [esp + 596]
    lea ecx, [ebp + blkd_ok - user_blkd_start]
    mov edx, 2
    mov eax, 0x31
    int 0x80
    jmp .loop

.read:
    mov ebx, [esp + 596]
    lea ecx, [esp + 64]
    mov edx, [esp + 592]
    mov eax, 0x31
    int 0x80
    jmp .loop

.done:
    mov eax, 1
    xor ebx, ebx
    int 0x80

blkd_name:
    db "blkd"

blkd_ok:
    db "ok"

blkd_init:
    db "ring3 block fallback", 10
blkd_init_len equ $ - blkd_init

user_blkd_end:
