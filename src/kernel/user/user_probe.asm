; =============================================================================
; user_probe.asm - 最小 ring3 AP 探针
; 只做一次 write + exit，不 fork、不打开文件，专门验证 ring3 能落在 AP。
; =============================================================================
BITS 32

global user_probe_start
global user_probe_end

user_probe_start:
    mov eax, 1                  ; SYS_WRITE
    mov ebx, probe_msg
    mov ecx, probe_msg_len
    int 0x81

    mov eax, 5                  ; SYS_EXIT
    xor ebx, ebx
    int 0x81

probe_msg:
    db "ring3 AP probe ok", 10
probe_msg_len equ $ - probe_msg

user_probe_end:
