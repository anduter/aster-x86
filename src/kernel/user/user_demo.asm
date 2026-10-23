; =============================================================================
; user_demo.asm - 极小的 ring3 程序（位置无关，可整体复制到用户页）
;
; 迁移期的双重验证：
;   1. 旧 ABI（int 0x81）打印 "hello from ring3"；
;   2. 新 Native ABI（int 0x80）查询 ABI 版本并打印一条调试信息；
;   3. 用 Native ABI 的 SYS_EXIT 退出。
;
; 放在 .rodata：链接进内核只是当“字节源”，运行时把它复制到用户页执行。
; =============================================================================
[bits 32]
section .rodata

global user_demo_start
global user_demo_end

user_demo_start:
    call .here
.here:
    pop ebp                     ; ebp = 当前 EIP（运行时用户页地址）

    ; ---- 旧 ABI：int 0x81 ----
    mov ebx, ebp
    add ebx, msg - .here
    mov eax, 1                  ; legacy SYS_WRITE
    mov ecx, msg_len
    int 0x81

    ; ---- 沙箱：userdemo 只允许 write/exit，打开文件必须被拒绝 ----
    mov ebx, ebp
    add ebx, sandbox_path - .here
    mov eax, 6                  ; legacy SYS_OPEN
    mov ecx, 1                  ; FS_READ
    int 0x81
    test eax, eax
    js .sandbox_denied

    mov ebx, ebp
    add ebx, sandbox_allowed - .here
    mov ecx, sandbox_allowed_len
    jmp .sandbox_print

.sandbox_denied:
    mov ebx, ebp
    add ebx, sandbox_denied_msg - .here
    mov ecx, sandbox_denied_len

.sandbox_print:
    mov eax, 1                  ; legacy SYS_WRITE
    int 0x81

    ; ---- native 沙箱：HANDLE_DUP 必须被拒绝 ----
    mov eax, 0x11               ; NATIVE_SYS_HANDLE_DUP
    mov ebx, 1                  ; self handle
    xor ecx, ecx                ; 默认权限
    int 0x80
    test eax, eax
    js .native_sandbox_denied

    mov ebx, ebp
    add ebx, native_sandbox_allowed - .here
    mov ecx, native_sandbox_allowed_len
    jmp .native_sandbox_print

.native_sandbox_denied:
    mov ebx, ebp
    add ebx, native_sandbox_denied_msg - .here
    mov ecx, native_sandbox_denied_len

.native_sandbox_print:
    mov eax, 1                  ; legacy SYS_WRITE
    int 0x81

    ; ---- Native ABI：int 0x80 ----
    sub esp, 16                 ; struct native_abi_version 的栈缓冲
    mov eax, 0                  ; NATIVE_SYS_ABI_VERSION
    mov ebx, esp
    int 0x80
    test eax, eax
    jnz .native_fail

    mov ebx, ebp
    add ebx, native_msg - .here
    mov eax, 0x50               ; NATIVE_SYS_DEBUG_WRITE
    mov ecx, native_msg_len
    int 0x80

    ; ---- Native process_create + wait ----
    mov ebx, ebp
    add ebx, proc_name - .here
    mov ecx, 5                  ; "hello"
    xor edx, edx
    mov eax, 0x20               ; NATIVE_SYS_PROCESS_CREATE
    int 0x80
    test eax, eax
    js .proc_fail

    mov ebx, eax                ; process handle
    sub esp, 4
    mov ecx, esp                ; &status
    mov eax, 0x21               ; NATIVE_SYS_PROCESS_WAIT
    int 0x80
    add esp, 4
    test eax, eax
    js .proc_fail

    mov ebx, ebp
    add ebx, proc_ok_msg - .here
    mov eax, 0x50
    mov ecx, proc_ok_len
    int 0x80
    jmp .native_done

.proc_fail:
    mov ebx, ebp
    add ebx, proc_fail_msg - .here
    mov eax, 0x50
    mov ecx, proc_fail_len
    int 0x80

.native_done:
    mov eax, 1                  ; NATIVE_SYS_EXIT
    xor ebx, ebx
    int 0x80

.native_fail:
    mov ebx, ebp
    add ebx, native_fail_msg - .here
    mov eax, 0x50               ; NATIVE_SYS_DEBUG_WRITE
    mov ecx, native_fail_len
    int 0x80

    mov eax, 1                  ; NATIVE_SYS_EXIT
    mov ebx, 1
    int 0x80

.hang:
    jmp .hang

msg:
    db "hello from ring3", 10
msg_len equ $ - msg

native_msg:
    db "native: ring3 ABI 0.1 ok", 10
native_msg_len equ $ - native_msg

native_fail_msg:
    db "native: ring3 ABI FAILED", 10
native_fail_len equ $ - native_fail_msg

proc_name:
    db "hello"

proc_ok_msg:
    db "native: process_create/wait ok", 10
proc_ok_len equ $ - proc_ok_msg

proc_fail_msg:
    db "native: process_create/wait FAILED", 10
proc_fail_len equ $ - proc_fail_msg

sandbox_path:
    db "system:/bin/shell", 0
sandbox_allowed:
    db "sandbox: open ALLOWED", 10
sandbox_allowed_len equ $ - sandbox_allowed
sandbox_denied_msg:
    db "sandbox: open denied", 10
sandbox_denied_len equ $ - sandbox_denied_msg

native_sandbox_allowed:
    db "sandbox: native ALLOWED", 10
native_sandbox_allowed_len equ $ - native_sandbox_allowed
native_sandbox_denied_msg:
    db "sandbox: native denied", 10
native_sandbox_denied_len equ $ - native_sandbox_denied_msg

user_demo_end:
