# =============================================================================
# ng WP1-7 示例用户程序：ring3 hello（无 libc，只用 syscall 0/1）
#
# 加载基址 0x40000000（见 user.ld），由内核 ELF64 加载器映射成三段：
#   .text   R-X   -> syscall 0 = write(buf,len)、syscall 1 = exit(code)
#   .rodata R+NX  -> "ng: ring3 hello" 字符串
#   .data/.bss RW+NX -> 已初始化魔数 + 未初始化字
#
# 程序在打印前先自检两件事（失败则 exit(1)，内核自检会跟着 FAIL）：
#   1. .data 的初始化字等于魔数  -> 文件字节确实被加载器拷进用户页；
#   2. .bss 的字为 0 且可写      -> 加载器把 memsz-filesz 的尾巴清零了。
# =============================================================================

    .section .text
    .align 16
    .global _start
    .type _start, @function
_start:
    # .data 段：加载器必须把文件里的魔数拷进来
    leaq user_data_word(%rip), %rax
    movabsq $0x1122334455667788, %rcx
    cmpq %rcx, (%rax)
    jne .Lfail

    # .bss 段：加载器必须清零；写一下证明页是可写的
    leaq user_bss_word(%rip), %rax
    cmpq $0, (%rax)
    jne .Lfail
    movq $0x5A5A5A5A, (%rax)
    cmpq $0x5A5A5A5A, (%rax)
    jne .Lfail

    # write(2)：syscall 0 = write(buf, len)
    leaq user_msg(%rip), %rdi
    movl $user_msg_len, %esi
    movl $0, %eax
    syscall
    cmpq $user_msg_len, %rax
    jne .Lfail

    # exit(0)
    xorl %edi, %edi
    movl $1, %eax
    syscall
    hlt

.Lfail:
    movl $1, %edi
    movl $1, %eax
    syscall
    hlt
    .size _start, . - _start

    .section .rodata
    .align 8
user_msg:
    .ascii "ng: ring3 hello\r\n"
    .set user_msg_len, . - user_msg

    .section .data
    .align 8
    .global user_data_word
user_data_word:
    .quad 0x1122334455667788

    .section .bss
    .align 8
    .global user_bss_word
user_bss_word:
    .zero 8
