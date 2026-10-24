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

    ; ---- Channel：fork 子进程 send，父进程 blocking recv ----
    sub esp, 32                 ; [0]=h0 [4]=h1 [8..15]=msg [16]=pid [20]=status
    mov ebx, esp
    mov eax, 0x30               ; CHANNEL_CREATE
    int 0x80
    test eax, eax
    js .chan_fail

    mov eax, 18                 ; legacy SYS_FORK
    int 0x81
    test eax, eax
    js .chan_fail
    jz .chan_child

    mov [esp + 16], eax         ; parent: 保存 child pid
    mov ebx, [esp + 4]          ; endpoint 1
    lea ecx, [esp + 8]
    mov edx, 16
    mov eax, 0x36               ; CHANNEL_RECV_BLOCK
    int 0x80
    cmp eax, 4
    jne .chan_fail
    cmp dword [esp + 8], 0x676E6970  ; "ping"
    jne .chan_fail

    mov ebx, [esp + 16]         ; wait child
    lea ecx, [esp + 20]
    mov eax, 20                 ; legacy SYS_WAIT
    int 0x81
    add esp, 32

    mov ebx, ebp
    add ebx, chan_ok_msg - .here
    mov eax, 0x50
    mov ecx, chan_ok_len
    int 0x80

    mov ebx, ebp
    add ebx, chan_block_msg - .here
    mov eax, 0x50
    mov ecx, chan_block_len
    int 0x80
    jmp .chan_done

.chan_child:
    mov eax, 24                 ; legacy SYS_SLEEP
    mov ebx, 50
    int 0x81
    mov ebx, [esp]              ; child: endpoint 0
    mov ecx, ebp
    add ecx, chan_msg - .here
    mov edx, 4
    mov eax, 0x31               ; CHANNEL_SEND
    int 0x80
    mov eax, 1                  ; NATIVE_SYS_EXIT
    xor ebx, ebx
    int 0x80

.chan_fail:
    add esp, 32
    jmp .proc_fail

.chan_done:

    ; ---- Port create/connect/accept ----
    sub esp, 24                 ; [0]=port [4]=client [8]=server [12..]=buf
    mov eax, 0x33               ; PORT_CREATE
    int 0x80
    test eax, eax
    js .port_fail
    mov [esp], eax

    mov ebx, eax
    mov eax, 0x34               ; PORT_CONNECT
    int 0x80
    test eax, eax
    js .port_fail
    mov [esp + 4], eax

    mov ebx, [esp]
    mov eax, 0x37               ; PORT_ACCEPT_BLOCK
    int 0x80
    test eax, eax
    js .port_fail
    mov [esp + 8], eax

    mov ebx, [esp + 4]
    mov ecx, ebp
    add ecx, port_msg - .here
    mov edx, 4
    mov eax, 0x31               ; CHANNEL_SEND
    int 0x80
    test eax, eax
    js .port_fail

    mov ebx, [esp + 8]
    lea ecx, [esp + 12]
    mov edx, 16
    mov eax, 0x36               ; CHANNEL_RECV_BLOCK
    int 0x80
    cmp eax, 4
    jne .port_fail
    cmp dword [esp + 12], 0x74726F70  ; "port"
    jne .port_fail
    add esp, 24

    mov ebx, ebp
    add ebx, port_ok_msg - .here
    mov eax, 0x50
    mov ecx, port_ok_len
    int 0x80

    ; ---- Service registry: connect echo by name（echosvc 由 supervisor 自动拉起） ----
    sub esp, 24                 ; [0]=client [4..]=buf
    mov ebx, ebp
    add ebx, svc_name - .here
    mov ecx, 4
    mov eax, 0x39               ; SERVICE_CONNECT "echo"
    int 0x80
    test eax, eax
    js .svc_fail
    mov [esp], eax

    mov ebx, [esp]
    mov ecx, ebp
    add ecx, svc_msg - .here
    mov edx, 4
    mov eax, 0x31               ; CHANNEL_SEND
    int 0x80
    test eax, eax
    js .svc_fail

    mov ebx, [esp]
    lea ecx, [esp + 4]
    mov edx, 16
    mov eax, 0x36               ; CHANNEL_RECV_BLOCK
    int 0x80
    cmp eax, 4
    jne .svc_fail
    cmp dword [esp + 4], 0x76726573  ; "serv"
    jne .svc_fail
    add esp, 24

    mov ebx, ebp
    add ebx, svc_ok_msg - .here
    mov eax, 0x50
    mov ecx, svc_ok_len
    int 0x80

    mov ebx, ebp
    add ebx, proc_ok_msg - .here
    mov eax, 0x50
    mov ecx, proc_ok_len
    int 0x80
    jmp .supervisor_test

.svc_fail:
    add esp, 24
    jmp .proc_fail

    ; ---- Supervisor service: connect "supervisor", ask whether "echo" exists ----
.supervisor_test:
    sub esp, 24
    mov ebx, ebp
    add ebx, sup_name - .here
    mov ecx, 10
    mov eax, 0x39               ; SERVICE_CONNECT
    int 0x80
    test eax, eax
    js .sup_fail
    mov [esp], eax

    mov ebx, [esp]
    mov ecx, ebp
    add ecx, svc_name - .here
    mov edx, 4
    mov eax, 0x31               ; CHANNEL_SEND
    int 0x80
    test eax, eax
    js .sup_fail

    mov ebx, [esp]
    lea ecx, [esp + 4]
    mov dword [esp + 4], 0
    mov edx, 8
    mov eax, 0x36               ; CHANNEL_RECV_BLOCK
    int 0x80
    cmp eax, 3
    jne .sup_fail
    cmp dword [esp + 4], 0x00736579  ; "yes"
    jne .sup_fail
    add esp, 24

    mov ebx, ebp
    add ebx, sup_ok_msg - .here
    mov eax, 0x50
    mov ecx, sup_ok_len
    int 0x80
    jmp .manifest_test

.manifest_test:
    sub esp, 24
    mov ebx, ebp
    add ebx, sup_name - .here
    mov ecx, 10
    mov eax, 0x39               ; SERVICE_CONNECT supervisor
    int 0x80
    test eax, eax
    js .man_fail
    mov [esp], eax

    mov ebx, [esp]
    mov ecx, ebp
    add ecx, manifest_name - .here
    mov edx, 8
    mov eax, 0x31               ; CHANNEL_SEND "manifest"
    int 0x80
    test eax, eax
    js .man_fail

    mov ebx, [esp]
    lea ecx, [esp + 4]
    mov dword [esp + 4], 0
    mov edx, 8
    mov eax, 0x36               ; CHANNEL_RECV_BLOCK
    int 0x80
    cmp eax, 2
    jne .man_fail
    cmp word [esp + 4], 0x6B6F   ; "ok"
    jne .man_fail
    add esp, 24

    mov ebx, ebp
    add ebx, manifest_ok_msg - .here
    mov eax, 0x50
    mov ecx, manifest_ok_len
    int 0x80
    jmp .restart_test

.restart_test:
    sub esp, 24
    mov ebx, ebp
    add ebx, sup_name - .here
    mov ecx, 10
    mov eax, 0x39               ; SERVICE_CONNECT supervisor
    int 0x80
    test eax, eax
    js .restart_fail
    mov [esp], eax

    mov ebx, [esp]
    mov ecx, ebp
    add ecx, restart_name - .here
    mov edx, 12
    mov eax, 0x31               ; CHANNEL_SEND "restart-echo"
    int 0x80
    test eax, eax
    js .restart_fail

    mov ebx, [esp]
    lea ecx, [esp + 4]
    mov dword [esp + 4], 0
    mov edx, 8
    mov eax, 0x36               ; CHANNEL_RECV_BLOCK
    int 0x80
    cmp eax, 2
    jne .restart_fail
    cmp word [esp + 4], 0x6B6F   ; "ok"
    jne .restart_fail

    ; 重启后重新按名字连接 echo，确认新服务可用
    mov ebx, ebp
    add ebx, svc_name - .here
    mov ecx, 4
    mov eax, 0x39               ; SERVICE_CONNECT echo
    int 0x80
    test eax, eax
    js .restart_fail
    mov [esp], eax

    mov ebx, [esp]
    mov ecx, ebp
    add ecx, svc_msg - .here
    mov edx, 4
    mov eax, 0x31
    int 0x80
    test eax, eax
    js .restart_fail

    mov ebx, [esp]
    lea ecx, [esp + 4]
    mov edx, 16
    mov eax, 0x36
    int 0x80
    cmp eax, 4
    jne .restart_fail
    cmp dword [esp + 4], 0x76726573  ; "serv"
    jne .restart_fail
    add esp, 24

    mov ebx, ebp
    add ebx, restart_ok_msg - .here
    mov eax, 0x50
    mov ecx, restart_ok_len
    int 0x80
    jmp .native_done

.restart_fail:
    add esp, 24
    jmp .proc_fail

.man_fail:
    add esp, 24
    jmp .proc_fail

.sup_fail:
    add esp, 24
    jmp .proc_fail

.port_fail:
    add esp, 24
    jmp .proc_fail

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

chan_msg:
    db "ping"

chan_ok_msg:
    db "native: channel ok", 10
chan_ok_len equ $ - chan_ok_msg

chan_block_msg:
    db "native: channel block ok", 10
chan_block_len equ $ - chan_block_msg

port_msg:
    db "port"

port_ok_msg:
    db "native: port ok", 10
port_ok_len equ $ - port_ok_msg

svc_name:
    db "echo"

svc_msg:
    db "serv"

svc_ok_msg:
    db "native: service ok", 10
svc_ok_len equ $ - svc_ok_msg

sup_name:
    db "supervisor"

sup_ok_msg:
    db "native: supervisor ok", 10
sup_ok_len equ $ - sup_ok_msg

manifest_name:
    db "manifest"

manifest_ok_msg:
    db "native: manifest ok", 10
manifest_ok_len equ $ - manifest_ok_msg

restart_name:
    db "restart-echo"

restart_ok_msg:
    db "native: restart ok", 10
restart_ok_len equ $ - restart_ok_msg

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
