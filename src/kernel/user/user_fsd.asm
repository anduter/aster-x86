; =============================================================================
; user_fsd.asm - ring3 内存文件服务
; 协议：请求首字节 'r' 读，'w' 后跟数据写；文件缓冲在用户栈。
; =============================================================================
BITS 32

global user_fsd_start
global user_fsd_end

user_fsd_start:
    cld
    call .get_base
.get_base:
    pop ebp
    sub ebp, .get_base - user_fsd_start
    sub esp, 256
    ; [esp..63] request, [esp+64..191] file, [esp+192] len, [esp+196] server handle
    lea esi, [ebp + fsd_init - user_fsd_start]
    lea edi, [esp + 64]
    mov ecx, fsd_init_len
    rep movsb
    mov dword [esp + 192], fsd_init_len

    mov eax, 0x33               ; PORT_CREATE
    int 0x80
    test eax, eax
    js .done
    mov [esp + 200], eax        ; port handle

    lea ebx, [ebp + fsd_name - user_fsd_start]
    mov ecx, 3
    mov edx, eax
    mov eax, 0x38               ; SERVICE_REGISTER("fsd", port)
    int 0x80
    test eax, eax
    js .done

.fsd_loop:
    mov ebx, [esp + 200]
    mov eax, 0x37               ; PORT_ACCEPT_BLOCK
    int 0x80
    test eax, eax
    js .done
    mov [esp + 196], eax

    mov ebx, eax
    mov ecx, esp
    mov edx, 64
    mov eax, 0x36               ; CHANNEL_RECV_BLOCK
    int 0x80
    cmp eax, 2
    jl .fsd_loop

    movzx ecx, byte [esp]       ; op
    cmp cl, 'w'
    jne .fsd_read

    ; write: 把 request[1..n-1] 复制到文件缓冲
    lea esi, [esp + 1]
    lea edi, [esp + 64]
    dec eax
    mov [esp + 192], eax
    mov ecx, eax
    rep movsb
    mov ebx, [esp + 196]
    lea ecx, [ebp + fsd_ok - user_fsd_start]
    mov edx, 2
    mov eax, 0x31
    int 0x80
    jmp .fsd_loop

.fsd_read:
    mov ebx, [esp + 196]
    lea ecx, [esp + 64]
    mov edx, [esp + 192]
    mov eax, 0x31               ; CHANNEL_SEND file
    int 0x80
    jmp .fsd_loop

.done:
    mov eax, 1                  ; NATIVE_SYS_EXIT
    xor ebx, ebx
    int 0x80

fsd_name:
    db "fsd"

fsd_ok:
    db "ok"

fsd_init:
    db "hello from userspace fs", 10
fsd_init_len equ $ - fsd_init

user_fsd_end:
