#ifndef INTR_SYSCALL_H
#define INTR_SYSCALL_H

#include "../../types.h"

/* 旧原型调用号（迁移期走 int 0x81；新代码请用 Native ABI 的 int 0x80） */
#define SYS_WRITE   1   /* ebx=buf, ecx=len            -> 写入字节数 */
#define SYS_GETPID  2   /*                              -> 当前 pid */
#define SYS_UPTIME  3   /*                              -> 运行秒数 */
#define SYS_YIELD   4   /*                              -> 0 */
#define SYS_EXIT    5   /* ebx=退出码                   -> 不返回 */
#define SYS_OPEN    6   /* ebx=路径, ecx=标志           -> 文件句柄 */
#define SYS_FREAD   7   /* ebx=句柄, ecx=buf, edx=len   -> 读到字节数 */
#define SYS_FWRITE  8   /* ebx=句柄, ecx=buf, edx=len   -> 写入字节数 */
#define SYS_CLOSE   9   /* ebx=句柄                     -> 0 */
#define SYS_GETCHAR 10  /*                              -> 字符或 -1 */
#define SYS_LIST    11  /* ebx=路径, ecx=序号, edx=buf     -> 0 / -1 */
#define SYS_DRIVES  12  /* ebx=序号, ecx=buf, edx=max      -> 0 / -1 */
#define SYS_MKDIR   13  /* ebx=路径                       -> 0 / -1 */
#define SYS_UNLINK  14  /* ebx=路径                       -> 0 / -1 */
#define SYS_STAT    15  /* ebx=路径, ecx=buf(8字节)       -> 0 / -1 */
#define SYS_CHDIR   16  /* ebx=路径                       -> 0 / -1 */
#define SYS_GETCWD  17  /* ebx=buf, ecx=max               -> 长度 / -1 */
#define SYS_FORK    18  /*                                -> 子 pid / 0（子进程） */
#define SYS_EXEC    19  /* ebx=路径                       -> 不返回（成功） */
#define SYS_WAIT    20  /* ebx=pid, ecx=&status           -> 子 pid / -1 */
#define SYS_INPUT_SET 21 /* ebx=buf, ecx=len              -> 0 / -1（画到输入框） */
#define SYS_BRK     22  /* ebx=增量                       -> 新的堆顶 / -1 */
#define SYS_KILL    23  /* ebx=pid, ecx=sig                -> 0 / -1 */
#define SYS_SLEEP   24  /* ebx=毫秒                        -> 0 */
#define SYS_PIPE    25  /* ebx=用户 int[2]                 -> 0 / -1 */
#define SYS_DUP2    26  /* ebx=旧 fd, ecx=新 fd            -> 新 fd / -1 */
#define SYS_REBOOT  27  /*                                -> 不返回（机器复位） */
#define SYS_PANIC   28  /*                                -> 不返回（调试用：触发内核 panic） */
#define SYS_OOPS    29  /*                                -> 不返回（调试用：触发内核 oops） */
#define SYS_SIGACTION  30 /* ebx=sig, ecx=handler           -> 旧 handler / -1 */
#define SYS_SIGRETURN  31 /*                                -> 恢复信号前的现场 */
#define SYS_SYNC       32 /*                                -> 0 / -1（落盘到持久化设备） */
#define SYS_TIME       33 /* ebx=缓冲, ecx=长度             -> 0（格式化时间字符串） */
#define SYS_FSCK       34 /* ebx=缓冲, ecx=长度             -> 错误数（缓冲里是摘要） */
#define SYS_PCI        35 /* ebx=index, ecx=缓冲            -> 0（PCI 设备信息）/ -1 */
#define SYS_POWEROFF   36 /*                                -> 不返回（ACPI S5 关机） */
#define SYS_MOUSE      37 /* ebx=用户 int[3]                -> 0（{x,y,buttons}） */
#define SYS_HANG       38 /* 调试：关中断死循环（验证 NMI 看门狗） */
#define SYS_ETH        39 /* ebx=0 取信息 / 1 发测试帧, ecx=缓冲 */
#define SYS_RESINFO    40 /* ebx=用户 u32[10]               -> 0（资源统计） */
#define SYS_GETUID     41 /*                                -> uid */
#define SYS_GETGID     42 /*                                -> gid */
#define SYS_CHMOD      43 /* ebx=路径, ecx=mode             -> 0 / -1 */
#define SYS_CHOWN      44 /* ebx=路径, ecx=uid, edx=gid     -> 0 / -1（root） */
#define SYS_SETUID     45 /* ebx=uid, ecx=gid               -> 0 / -1（root 才能降权） */
#define SYS_CAP_DROP   46 /* ebx=fd, ecx=要撤销的 rights    -> 0 / -1 */
#define SYS_CAP_SEND   47 /* ebx=pid, ecx=fd, edx=rights   -> 目标 fd / -1 */
#define SYS_MMAP       48 /* ebx=len, ecx=prot             -> 用户 VA / -1 */
#define SYS_MUNMAP     49 /* ebx=addr, ecx=len             -> 0 / -1 */
#define SYS_UDP_SEND   50 /* ebx=&udp_req                 -> 0 / -1 */
#define SYS_UDP_RECV   51 /* ebx=&udp_req                 -> payload len / -1 */
#define SYS_SOCKET     52 /* ebx=domain, ecx=type, edx=proto -> fd / -1 */
#define SYS_SENDTO     53 /* ebx=fd, ecx=&udp_req          -> 0 / -1 */
#define SYS_RECVFROM   54 /* ebx=fd, ecx=&udp_req          -> len / -1 */
#define SYS_CHROOT     55 /* ebx=path                      -> 0 / -1 */
#define SYS_TCP_TEST   56 /* 运行一次 TCP echo 自检           -> 0 / -1 */
#define SYS_BLK_READ   57 /* ebx=lba, ecx=user buf           -> 0 / -1 */

/* 用户/内核代码统一用这个宏发起系统调用 */
static inline int sys_call(int num, int a, int b, int c)
{
    int ret;

    __asm__ volatile ("int $0x81"
                      : "=a"(ret)
                      : "a"(num), "b"(a), "c"(b), "d"(c)
                      : "memory");
    return ret;
}

/* 已发生的系统调用次数（供日志/统计） */
unsigned int syscall_count(void);

#endif
