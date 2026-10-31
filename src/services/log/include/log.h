/*
 * ng WP2-5: log 服务接口（L1，src/services/log）。
 *
 * 64 KiB 环形缓冲，**按整行**保存（行 = "tag: msg\n"）。缓冲满时从最旧
 * 的整行开始丢弃；log_read() 返回最近的、能放进调用方缓冲的若干整行
 * （若最新一行本身就超过 max，则返回该行的末尾 max 字节）。
 *
 * 依赖只有 types.h：不 include 内核串口/控制台，内核关键路径（启动、自检、
 * panic）由 L0 显式调用 log_write()。
 */
#ifndef LOG_H
#define LOG_H

#include "types.h"

#define LOG_CAPACITY 65536u /* 64 KiB（按字节） */
#define LOG_TAG_MAX 16u     /* 含 NUL；tag 1..15 字符 */
#define LOG_MSG_MAX 128u    /* 含 NUL；msg 1..127 字符 */
#define LOG_LINE_MAX (LOG_TAG_MAX - 1u + 2u + LOG_MSG_MAX - 1u + 1u)
#define LOG_SYS_READ_MAX 4096u /* syscall 32 单次读取上限 */

#define LOG_EINVAL (-22)
#define LOG_ENAMETOOLONG (-36)
#define LOG_ENOSPC (-28)

/* 清空环形缓冲与统计；调用后 log_write 才可用。 */
int log_init(void);

/*
 * 追加一行：tag 1..15、msg 1..127，只接受可打印 ASCII（0x20..0x7E）；
 * tag 不允许 ':' 与换行。成功返回写入字节数（含 "\n"），失败返回负 errno。
 */
int log_write(const char *tag, const char *msg);

/*
 * 取最近的行：从最旧到最新拷进 buf；返回字节数（0 = 空/无内容）。
 * max 为 0 或 buf 为 NULL 时返回 0。不保证 NUL 结尾（返回长度是权威）。
 */
u32 log_read(char *buf, u32 max);

u32 log_bytes_used(void);
u32 log_line_count(void);
u32 log_dropped_lines(void);
u64 log_written_lines(void);
u32 log_capacity(void);

#endif /* LOG_H */
