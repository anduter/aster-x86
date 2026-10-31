# =============================================================================
# ng WP1-7 / WP2-1 / WP2-2 示例用户程序：ring3 hello + VFS/FAT demo（无 libc）
#
# 加载基址 0x40000000（见 user.ld），由内核 ELF64 加载器映射成三段：
#   .text   R-X   -> syscall 0 = write(buf,len)、1 = exit(code)、20..27 = VFS
#   .rodata R+NX  -> 打印/路径字符串
#   .data/.bss RW+NX -> 已初始化魔数 + 未初始化字/缓冲
#
# 启动自检（失败 exit(1)，内核自检跟着 FAIL）：
#   1. .data 的初始化字等于魔数      -> 文件字节确实被加载器拷进用户页；
#   2. .bss 的字为 0 且可写          -> 加载器清零了 memsz-filesz 的尾巴。
#
# WP2-1 demo（全部走用户指针，内核经 uaccess 拷贝）：
#   open("/tmp/a", O_CREAT|O_RDWR, 0644) -> write("hello") -> read 校验
#   -> stat(type=FILE,size=5) -> close -> read(旧 fd) 必须是 -EBADF
#   -> listdir("/tmp") 必须 1 条、名字 "a"。
#
# WP2-2 demo（内核已把 RAM 盘 FAT16 挂到 /mnt/fat）：
#   read("/mnt/fat/HELLO.TXT") 校验 18 字节 ->
#   create("/mnt/fat/NEW.TXT") + write("fat16") -> stat(size=5) ->
#   unlink -> stat 必须 -ENOENT。
#   每步失败就 exit(1)；成功打印 "ng: fs ..." 证据行。
#
# 系统调用约定（ABI 1.0 草案）：
#   rax=调用号，rdi/rsi/rdx/r10=参数，返回 rax（负 = -errno）。
# =============================================================================

    # 打印一个字面量并检查返回值 == 长度（syscall 0 返回写出的字节数）。
    .macro PRINT_MSG sym, len
    leaq \sym(%rip), %rdi
    movl $\len, %esi
    xorl %eax, %eax
    syscall
    cmpq $\len, %rax
    jne .Lfail
    .endm

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

    PRINT_MSG user_msg, user_msg_len

    # ---- 20 open("/tmp/a", O_CREAT|O_RDWR, 0644) -------------------------
    leaq fs_path(%rip), %rdi
    movl $0x42, %esi
    movl $0644, %edx
    movl $20, %eax
    syscall
    testq %rax, %rax
    jle .Lfail
    movq %rax, fs_fd(%rip)
    PRINT_MSG fs_msg_open, fs_msg_open_len

    # ---- 22 write(fd, "hello", 5) ----------------------------------------
    movq fs_fd(%rip), %rdi
    leaq fs_hello(%rip), %rsi
    movl $5, %edx
    movl $22, %eax
    syscall
    cmpq $5, %rax
    jne .Lfail
    PRINT_MSG fs_msg_write, fs_msg_write_len

    # 关闭后重开只读：没有 lseek，新 fd 的 offset 从 0 开始，才能读回 "hello"。
    movq fs_fd(%rip), %rdi
    movl $23, %eax
    syscall
    testq %rax, %rax
    jne .Lfail
    leaq fs_path(%rip), %rdi
    movl $0, %esi
    movl $0, %edx
    movl $20, %eax
    syscall
    testq %rax, %rax
    jle .Lfail
    movq %rax, fs_fd(%rip)

    # ---- 21 read(fd, buf, 8) 必须返回 5，内容等于 "hello" ----------------
    movq fs_fd(%rip), %rdi
    leaq fs_readbuf(%rip), %rsi
    movl $8, %edx
    movl $21, %eax
    syscall
    cmpq $5, %rax
    jne .Lfail
    leaq fs_readbuf(%rip), %rdi
    leaq fs_hello(%rip), %rsi
    movl $5, %ecx
    cld
    repe cmpsb
    jne .Lfail
    PRINT_MSG fs_msg_read, fs_msg_read_len

    # ---- 24 stat("/tmp/a", &st)：type=FILE(1)、size=5 --------------------
    leaq fs_path(%rip), %rdi
    leaq fs_statbuf(%rip), %rsi
    movl $24, %eax
    syscall
    testq %rax, %rax
    jne .Lfail
    cmpl $1, fs_statbuf(%rip)
    jne .Lfail
    cmpq $5, fs_statbuf+8(%rip)
    jne .Lfail
    PRINT_MSG fs_msg_stat, fs_msg_stat_len

    # ---- 23 close(fd) ----------------------------------------------------
    movq fs_fd(%rip), %rdi
    movl $23, %eax
    syscall
    testq %rax, %rax
    jne .Lfail

    # 关闭后旧 fd 必须失效：21 read(fd) == -EBADF(-9)。
    movq fs_fd(%rip), %rdi
    leaq fs_readbuf(%rip), %rsi
    movl $1, %edx
    movl $21, %eax
    syscall
    cmpq $-9, %rax
    jne .Lfail
    PRINT_MSG fs_msg_close, fs_msg_close_len

    # ---- 25 listdir("/tmp", ents, 4)：1 条、name_len=1、name[0]=='a' -----
    leaq fs_tmp(%rip), %rdi
    leaq fs_dirents(%rip), %rsi
    movl $4, %edx
    movl $25, %eax
    syscall
    cmpq $1, %rax
    jne .Lfail
    cmpl $1, fs_dirents+4(%rip)
    jne .Lfail
    movzbl fs_dirents+16(%rip), %eax
    cmpb $97, %al
    jne .Lfail
    PRINT_MSG fs_msg_listdir, fs_msg_listdir_len

    # ---- WP2-2：FAT16 挂载点 /mnt/fat -------------------------------------
    # 20 open("/mnt/fat/HELLO.TXT", O_RDONLY) -> 21 read 18 字节并校验
    leaq fat_hello_path(%rip), %rdi
    xorl %esi, %esi
    xorl %edx, %edx
    movl $20, %eax
    syscall
    testq %rax, %rax
    jle .Lfail
    movq %rax, fs_fd(%rip)
    movq fs_fd(%rip), %rdi
    leaq fs_readbuf(%rip), %rsi
    movl $32, %edx
    movl $21, %eax
    syscall
    cmpq $18, %rax
    jne .Lfail
    leaq fs_readbuf(%rip), %rdi
    leaq fat_hello(%rip), %rsi
    movl $18, %ecx
    cld
    repe cmpsb
    jne .Lfail
    movq fs_fd(%rip), %rdi
    movl $23, %eax
    syscall
    testq %rax, %rax
    jne .Lfail
    PRINT_MSG fat_msg_read, fat_msg_read_len

    # 20 open("/mnt/fat/NEW.TXT", O_CREAT|O_RDWR, 0644) -> 22 write("fat16")
    leaq fat_new_path(%rip), %rdi
    movl $0x42, %esi
    movl $0644, %edx
    movl $20, %eax
    syscall
    testq %rax, %rax
    jle .Lfail
    movq %rax, fs_fd(%rip)
    movq fs_fd(%rip), %rdi
    leaq fat_data(%rip), %rsi
    movl $5, %edx
    movl $22, %eax
    syscall
    cmpq $5, %rax
    jne .Lfail
    movq fs_fd(%rip), %rdi
    movl $23, %eax
    syscall
    testq %rax, %rax
    jne .Lfail
    PRINT_MSG fat_msg_write, fat_msg_write_len

    # 24 stat("/mnt/fat/NEW.TXT")：type=FILE(1)、size=5
    leaq fat_new_path(%rip), %rdi
    leaq fs_statbuf(%rip), %rsi
    movl $24, %eax
    syscall
    testq %rax, %rax
    jne .Lfail
    cmpl $1, fs_statbuf(%rip)
    jne .Lfail
    cmpq $5, fs_statbuf+8(%rip)
    jne .Lfail
    PRINT_MSG fat_msg_stat, fat_msg_stat_len

    # 27 unlink + 24 stat 必须是 -ENOENT(-2)
    leaq fat_new_path(%rip), %rdi
    movl $27, %eax
    syscall
    testq %rax, %rax
    jne .Lfail
    leaq fat_new_path(%rip), %rdi
    leaq fs_statbuf(%rip), %rsi
    movl $24, %eax
    syscall
    cmpq $-2, %rax
    jne .Lfail
    PRINT_MSG fat_msg_unlink, fat_msg_unlink_len

    PRINT_MSG fs_msg_demo, fs_msg_demo_len

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
fs_msg_open:
    .ascii "ng: fs open ok\r\n"
    .set fs_msg_open_len, . - fs_msg_open
fs_msg_write:
    .ascii "ng: fs write ok\r\n"
    .set fs_msg_write_len, . - fs_msg_write
fs_msg_read:
    .ascii "ng: fs read ok\r\n"
    .set fs_msg_read_len, . - fs_msg_read
fs_msg_stat:
    .ascii "ng: fs stat ok (size=5)\r\n"
    .set fs_msg_stat_len, . - fs_msg_stat
fs_msg_close:
    .ascii "ng: fs close ok\r\n"
    .set fs_msg_close_len, . - fs_msg_close
fs_msg_listdir:
    .ascii "ng: fs listdir ok (n=1)\r\n"
    .set fs_msg_listdir_len, . - fs_msg_listdir
fs_msg_demo:
    .ascii "ng: fs user demo ok\r\n"
    .set fs_msg_demo_len, . - fs_msg_demo
fat_msg_read:
    .ascii "ng: fs fat read ok\r\n"
    .set fat_msg_read_len, . - fat_msg_read
fat_msg_write:
    .ascii "ng: fs fat write ok\r\n"
    .set fat_msg_write_len, . - fat_msg_write
fat_msg_stat:
    .ascii "ng: fs fat stat ok\r\n"
    .set fat_msg_stat_len, . - fat_msg_stat
fat_msg_unlink:
    .ascii "ng: fs fat unlink ok\r\n"
    .set fat_msg_unlink_len, . - fat_msg_unlink
fs_hello:
    .ascii "hello"
    .set fs_hello_len, . - fs_hello
fat_hello:
    .ascii "hello from fat16\r\n"
    .set fat_hello_len, . - fat_hello
fat_data:
    .ascii "fat16"
    .set fat_data_len, . - fat_data
fs_path:
    .asciz "/tmp/a"
fs_tmp:
    .asciz "/tmp"
fat_hello_path:
    .asciz "/mnt/fat/HELLO.TXT"
fat_new_path:
    .asciz "/mnt/fat/NEW.TXT"

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
fs_fd:
    .zero 8
fs_readbuf:
    .zero 64
fs_statbuf:
    .zero 24
fs_dirents:
    .zero 320
