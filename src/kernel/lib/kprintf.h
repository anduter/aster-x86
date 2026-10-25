/* =============================================================================
 * lib/kprintf.h - 内核格式化输出
 *
 * 所有输出都走 console（同时镜像到 COM1），所以屏幕上和串口里看到的内容一致。
 * 支持：%c %s %d %i %u %x %X %p %% 以及 "0" 填充和最小宽度，例如 %08x。
 * ============================================================================= */
#ifndef LIB_KPRINTF_H
#define LIB_KPRINTF_H

#include "../include/types.h"
#include <stdarg.h>

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int ksnprintf(char *buf, size_t size, const char *fmt, ...);
/* 输出到 console（并镜像串口）；返回写入的字符数 */
int kprintf(const char *fmt, ...);
/* 输出到 console 并追加 "\r\n" */
int kprintln(const char *fmt, ...);

#endif /* LIB_KPRINTF_H */
