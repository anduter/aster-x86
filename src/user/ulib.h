#ifndef ULIB_H
#define ULIB_H

/* 用户态最小 C 库：迁移期只依赖 int 0x81，不链接内核 */
typedef unsigned int usize;

#define SYS_WRITE 1
#define SYS_EXIT  5
#define SYS_FREAD 7
#define SYS_FWRITE 8
#define SYS_BRK   22

int syscall3(int n, int a, int b, int c);

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
