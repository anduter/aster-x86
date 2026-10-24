#ifndef ULIB_H
#define ULIB_H

/* 用户态最小 C 库：迁移期只依赖 int 0x81，不链接内核。
 * 系统调用编号/结构体/错误码的唯一来源是内核侧 include/abi.h；
 * 这里用相对路径引入，user 程序不依赖 -Isrc/kernel。 */
#include "../kernel/include/abi.h"

typedef unsigned int usize;

int syscall3(int n, int a, int b, int c);

/* SYS_ABI_VERSION 的标量封装：高 16 位 ABI major，低 16 位 feature bits */
int abi_version(void);

/* 改名/移动（同卷内）。newpath 是完整目标路径，不做 `mv` 的"移入目录"语义。
 * 成功返回 0，失败返回 -1（跨卷/目标非空目录/权限不足都算失败）。 */
static inline int fs_rename(const char *oldpath, const char *newpath)
{
    return syscall3(SYS_RENAME, (int)(unsigned int)oldpath, (int)(unsigned int)newpath, 0);
}

/* 基于 fd 的读写（0=stdin，1=stdout，其余是文件/管道） */
int fd_read(int fd, void *buf, int len);
int fd_write(int fd, const void *buf, int len);

int putchar(int c);
int puts(const char *s);
int printf(const char *fmt, ...);

void *malloc(usize n);
void free(void *p);

usize strlen(const char *s);
int strcmp(const char *a, const char *b);
char *strcpy(char *d, const char *s);
void *memcpy(void *d, const void *s, usize n);
void *memset(void *d, int c, usize n);

#endif
