/*
 * ng WP1-2: COM1 串口输出（QEMU 无显示环境下唯一的证据通道）。
 */
#ifndef NG_SERIAL_H
#define NG_SERIAL_H

#include "types.h"

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
void serial_puthex(u64 value); /* "0x" + 最少位数的十六进制 */
void serial_putdec(u64 value); /* 十进制，无符号 */

#endif /* NG_SERIAL_H */
