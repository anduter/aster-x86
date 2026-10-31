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
#
# WP2-4 demo（内核已把 ATA 真实磁盘的 FAT16 挂到 /mnt/disk）：
#   read("/mnt/disk/HELLO.TXT") 校验 18 字节 ->
#   create/truncate("/mnt/disk/DISK.TXT") + write("disk24") -> 重开读回校验 ->
#   stat(size=6)。
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

    # ---- 27 unlink("/tmp/a")：demo 自己清理 ------------------------------
    # ramfs 节点池只有 32 个；WP3-2 把 /bin/wm 也种进 /bin 之后，这里留下的
    # /tmp/a 会挤掉后面用户态 shell 的 /tmp/s.txt。demo 跑完就删掉自己的
    # scratch（27 unlink），给会话留一个节点。
    leaq fs_path(%rip), %rdi
    movl $27, %eax
    syscall
    testq %rax, %rax
    jne .Lfail

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

    # ---- WP2-4：真实磁盘镜像挂载点 /mnt/disk ------------------------------
    # 20 open("/mnt/disk/HELLO.TXT", O_RDONLY) -> 21 read 18 字节并校验
    leaq disk_hello_path(%rip), %rdi
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
    leaq fat_hello(%rip), %rsi        # 同一份 "hello from fat16\r\n"
    movl $18, %ecx
    cld
    repe cmpsb
    jne .Lfail
    movq fs_fd(%rip), %rdi
    movl $23, %eax
    syscall
    testq %rax, %rax
    jne .Lfail
    PRINT_MSG disk_msg_read, disk_msg_read_len

    # 20 open("/mnt/disk/DISK.TXT", O_CREAT|O_RDWR|O_TRUNC, 0644) -> 22 write
    leaq disk_file_path(%rip), %rdi
    movl $0x242, %esi
    movl $0644, %edx
    movl $20, %eax
    syscall
    testq %rax, %rax
    jle .Lfail
    movq %rax, fs_fd(%rip)
    movq fs_fd(%rip), %rdi
    leaq disk_data(%rip), %rsi
    movl $6, %edx
    movl $22, %eax
    syscall
    cmpq $6, %rax
    jne .Lfail
    movq fs_fd(%rip), %rdi
    movl $23, %eax
    syscall
    testq %rax, %rax
    jne .Lfail

    # 重新打开只读 -> 21 read 6 字节读回校验（offset 从 0 开始）
    leaq disk_file_path(%rip), %rdi
    xorl %esi, %esi
    xorl %edx, %edx
    movl $20, %eax
    syscall
    testq %rax, %rax
    jle .Lfail
    movq %rax, fs_fd(%rip)
    movq fs_fd(%rip), %rdi
    leaq fs_readbuf(%rip), %rsi
    movl $16, %edx
    movl $21, %eax
    syscall
    cmpq $6, %rax
    jne .Lfail
    leaq fs_readbuf(%rip), %rdi
    leaq disk_data(%rip), %rsi
    movl $6, %ecx
    cld
    repe cmpsb
    jne .Lfail
    movq fs_fd(%rip), %rdi
    movl $23, %eax
    syscall
    testq %rax, %rax
    jne .Lfail
    PRINT_MSG disk_msg_write, disk_msg_write_len

    # 24 stat("/mnt/disk/DISK.TXT")：type=FILE(1)、size=6
    leaq disk_file_path(%rip), %rdi
    leaq fs_statbuf(%rip), %rsi
    movl $24, %eax
    syscall
    testq %rax, %rax
    jne .Lfail
    cmpl $1, fs_statbuf(%rip)
    jne .Lfail
    cmpq $6, fs_statbuf+8(%rip)
    jne .Lfail
    PRINT_MSG disk_msg_stat, disk_msg_stat_len

    # ---- WP2-5：auth/login（31）、log（32/33）、pkg（34/35） --------------
    # 31 login("root","root") -> uid 0
    leaq svc_root(%rip), %rdi
    leaq svc_root(%rip), %rsi
    movl $31, %eax
    syscall
    testq %rax, %rax
    jne .Lfail
    # 31 login("alice","alice") -> uid 1000
    leaq svc_alice(%rip), %rdi
    leaq svc_alice(%rip), %rsi
    movl $31, %eax
    syscall
    cmpq $1000, %rax
    jne .Lfail
    # 31 login("root","wrong") -> -EACCES(-13)
    leaq svc_root(%rip), %rdi
    leaq svc_wrong(%rip), %rsi
    movl $31, %eax
    syscall
    cmpq $-13, %rax
    jne .Lfail
    PRINT_MSG svc_msg_login, svc_msg_login_len

    # 33 log_write("ring", "hello from ring3") -> 0
    leaq svc_tag(%rip), %rdi
    leaq svc_logmsg(%rip), %rsi
    movl $33, %eax
    syscall
    testq %rax, %rax
    jne .Lfail

    # 32 dmesg(buf, 256) -> >0；dmesg 返回最近的行，最后一行必须是刚写的。
    leaq svc_buf(%rip), %rdi
    movl $256, %esi
    movl $32, %eax
    syscall
    testq %rax, %rax
    jle .Lfail
    movq %rax, %rdx
    leaq svc_buf(%rip), %rdi
    addq %rdx, %rdi
    subq $svc_logline_len, %rdi
    leaq svc_logline(%rip), %rsi
    movl $svc_logline_len, %ecx
    cld
    repe cmpsb
    jne .Lfail

    # 34 pkg_list(buf, 256) -> 1，内容 "hello 1.0.0\n"
    leaq svc_buf(%rip), %rdi
    movl $256, %esi
    movl $34, %eax
    syscall
    cmpq $1, %rax
    jne .Lfail
    leaq svc_buf(%rip), %rdi
    leaq svc_pkgline(%rip), %rsi
    movl $svc_pkgline_len, %ecx
    cld
    repe cmpsb
    jne .Lfail

    # 35 pkg_verify("hello") -> 0；35 pkg_verify("nope") -> -ENOENT(-2)
    leaq svc_hello(%rip), %rdi
    movl $35, %eax
    syscall
    testq %rax, %rax
    jne .Lfail
    leaq svc_nope(%rip), %rdi
    movl $35, %eax
    syscall
    cmpq $-2, %rax
    jne .Lfail
    PRINT_MSG svc_msg_pkg, svc_msg_pkg_len
    PRINT_MSG svc_msg_ok, svc_msg_ok_len

    # ---- WP2-3：readkey（30）非阻塞轮询 -----------------------------------
    # 先打印 ready，run-ng 用 QMP input-send-event 注入 'a'；没有注入时走超时分支。
    # 返回 (flags<<24)|(ascii<<16)|scancode；-EAGAIN(-11) 继续重试。
    PRINT_MSG input_ready_msg, input_ready_msg_len
    movl $5000000, %r8d
.Lkey_wait:
    movl $30, %eax
    syscall
    cmpq $-11, %rax
    jne .Lkey_got
    decl %r8d
    jnz .Lkey_wait
    PRINT_MSG input_timeout_msg, input_timeout_msg_len
    jmp .Linput_done
.Lkey_got:
    movq %rax, %rdx
    shrq $16, %rdx
    andl $0xFF, %edx
    cmpl $97, %edx              # 'a'
    jne .Lkey_other
    PRINT_MSG input_key_msg, input_key_msg_len
    jmp .Linput_done
.Lkey_other:
    PRINT_MSG input_other_msg, input_other_msg_len
.Linput_done:

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
disk_msg_read:
    .ascii "ng: fs disk read ok\r\n"
    .set disk_msg_read_len, . - disk_msg_read
disk_msg_write:
    .ascii "ng: fs disk write ok\r\n"
    .set disk_msg_write_len, . - disk_msg_write
disk_msg_stat:
    .ascii "ng: fs disk stat ok (size=6)\r\n"
    .set disk_msg_stat_len, . - disk_msg_stat
svc_msg_login:
    .ascii "ng: ring3 auth ok\r\n"
    .set svc_msg_login_len, . - svc_msg_login
svc_msg_pkg:
    .ascii "ng: ring3 pkg ok\r\n"
    .set svc_msg_pkg_len, . - svc_msg_pkg
svc_msg_ok:
    .ascii "ng: ring3 svc ok\r\n"
    .set svc_msg_ok_len, . - svc_msg_ok
svc_root:
    .asciz "root"
svc_alice:
    .asciz "alice"
svc_wrong:
    .asciz "wrong"
svc_tag:
    .asciz "ring"
svc_logmsg:
    .asciz "hello from ring3"
svc_logline:
    .ascii "ring: hello from ring3\n"
    .set svc_logline_len, . - svc_logline
svc_hello:
    .asciz "hello"
svc_nope:
    .asciz "nope"
svc_pkgline:
    .ascii "hello 1.0.0\n"
    .set svc_pkgline_len, . - svc_pkgline
input_ready_msg:
    .ascii "ng: ring3 input ready\r\n"
    .set input_ready_msg_len, . - input_ready_msg
input_key_msg:
    .ascii "ng: ring3 input ok (key=a)\r\n"
    .set input_key_msg_len, . - input_key_msg
input_other_msg:
    .ascii "ng: ring3 input ok (key=other)\r\n"
    .set input_other_msg_len, . - input_other_msg
input_timeout_msg:
    .ascii "ng: ring3 input ok (timeout)\r\n"
    .set input_timeout_msg_len, . - input_timeout_msg
fs_hello:
    .ascii "hello"
    .set fs_hello_len, . - fs_hello
fat_hello:
    .ascii "hello from fat16\r\n"
    .set fat_hello_len, . - fat_hello
fat_data:
    .ascii "fat16"
    .set fat_data_len, . - fat_data
disk_data:
    .ascii "disk24"
    .set disk_data_len, . - disk_data
fs_path:
    .asciz "/tmp/a"
fs_tmp:
    .asciz "/tmp"
fat_hello_path:
    .asciz "/mnt/fat/HELLO.TXT"
fat_new_path:
    .asciz "/mnt/fat/NEW.TXT"
disk_hello_path:
    .asciz "/mnt/disk/HELLO.TXT"
disk_file_path:
    .asciz "/mnt/disk/DISK.TXT"

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
svc_buf:
    .zero 512
