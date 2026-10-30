/* =============================================================================
 * lib/kprintf.c - 内核格式化输出实现
 *
 * 先把内容格式化进栈上缓冲区，再一次性交给 console；这样屏幕输出不会
 * 因为格式化过程中插入的字符而撕裂。
 * ============================================================================= */
#include "kprintf.h"
#include "string.h"
#include "include/dev.h"
#include "include/klog.h"      /* P2：kprintf 同步写入内核日志环（dmesg） */

#define KPRINTF_BUF_SIZE 512

/* 向缓冲区追加一个字符，带边界保护；返回 1 表示成功 */
static int emit(char *buf, size_t size, size_t *pos, char c)
{
    if (*pos + 1U < size) {
        buf[*pos] = c;
        (*pos)++;
        buf[*pos] = '\0';
        return 1;
    }
    return 0;
}

/* 输出整数，支持最小宽度和 '0' 填充 */
static void emit_number(char *buf, size_t size, size_t *pos,
                        uint32_t value, unsigned base, int upper,
                        int width, char pad, int negative)
{
    static const char lower_digits[] = "0123456789abcdef";
    static const char upper_digits[] = "0123456789ABCDEF";
    const char *digits = upper ? upper_digits : lower_digits;
    char tmp[64];
    int n = 0;
    int pad_count;

    if (value == 0U) {
        tmp[n++] = '0';
    } else {
        while (value > 0U && n < (int)sizeof(tmp)) {
            tmp[n++] = digits[value % base];
            value /= base;
        }
    }

    /* 负号计入宽度 */
    if (negative && pad == '0') {
        emit(buf, size, pos, '-');
        width--;
        negative = 0;
    }

    pad_count = width - n - (negative ? 1 : 0);
    if (negative) {
        emit(buf, size, pos, '-');
    }
    while (pad_count-- > 0) {
        emit(buf, size, pos, pad);
    }
    while (n > 0) {
        emit(buf, size, pos, tmp[--n]);
    }
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    size_t pos = 0;

    if (size == 0U) {
        return 0;
    }
    buf[0] = '\0';

    while (*fmt != '\0') {
        char pad = ' ';
        int width = 0;

        if (*fmt != '%') {
            emit(buf, size, &pos, *fmt++);
            continue;
        }

        fmt++; /* 跳过 '%' */
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        switch (*fmt) {
        case 'c':
            emit(buf, size, &pos, (char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            int len;
            if (s == NULL) {
                s = "(null)";
            }
            len = (int)strlen(s);
            for (int i = 0; i < width - len; i++) {
                emit(buf, size, &pos, ' ');
            }
            while (*s != '\0') {
                emit(buf, size, &pos, *s++);
            }
            break;
        }
        case 'd':
        case 'i': {
            int32_t v = va_arg(ap, int32_t);
            if (v < 0) {
                emit_number(buf, size, &pos, (uint32_t)(-(int64_t)v), 10U, 0,
                            width, pad, 1);
            } else {
                emit_number(buf, size, &pos, (uint32_t)v, 10U, 0, width, pad, 0);
            }
            break;
        }
        case 'u':
            emit_number(buf, size, &pos, va_arg(ap, uint32_t), 10U, 0, width, pad, 0);
            break;
        case 'x':
            emit_number(buf, size, &pos, va_arg(ap, uint32_t), 16U, 0, width, pad, 0);
            break;
        case 'X':
            emit_number(buf, size, &pos, va_arg(ap, uint32_t), 16U, 1, width, pad, 0);
            break;
        case 'p':
            emit(buf, size, &pos, '0');
            emit(buf, size, &pos, 'x');
            emit_number(buf, size, &pos, (uint32_t)(uintptr_t)va_arg(ap, void *),
                        16U, 0, 8, '0', 0);
            break;
        case '%':
            emit(buf, size, &pos, '%');
            break;
        case '\0':
            /* 结尾的孤立 '%' */
            emit(buf, size, &pos, '%');
            goto done;
        default:
            emit(buf, size, &pos, '%');
            emit(buf, size, &pos, *fmt);
            break;
        }
        fmt++;
    }

done:
    return (int)pos;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int len;

    va_start(ap, fmt);
    len = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return len;
}

int kprintf(const char *fmt, ...)
{
    char buf[KPRINTF_BUF_SIZE];
    va_list ap;
    int len;

    va_start(ap, fmt);
    len = kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    /* 先入环再输出：即使 console/串口卡住或随后 panic，最后一行也已可 dmesg 读到。
     * klog_capture 只做短自旋 + memcpy，不打印/不分配，IRQ 上下文安全。 */
    klog_capture(buf, (uint32_t)len);
    console_write_atomic(buf);
    return len;
}

int kprintln(const char *fmt, ...)
{
    char buf[KPRINTF_BUF_SIZE];
    va_list ap;
    int len;

    va_start(ap, fmt);
    len = kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if ((size_t)len + 2U < sizeof(buf)) {
        buf[len++] = '\r';
        buf[len++] = '\n';
        buf[len] = '\0';
    }
    klog_capture(buf, (uint32_t)len);   /* kprintln 不经过 kprintf，这里单独入环 */
    console_write_atomic(buf);
    return len;
}
