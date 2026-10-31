/*
 * ng WP2-5: log 服务实现（64 KiB 按行环形缓冲，L1）。
 *
 * 只有一份静态缓冲，单核协作式（与 ramfs/VFS 相同的并发假设：IF=0 + 显式
 * 调度点）。行是原子的：写入要么整行可见，要么不可见；缓冲满时按整行
 * 丢弃最旧内容，不会留下半行。
 *
 * 行格式固定 "tag: msg\n"，tag/msg 只允许 0x20..0x7E（tag 还不允许 ':'），
 * 因此日志可直接作为 ASCII 文本被 dmesg 读取。
 */
#include "log.h"

static char g_ring[LOG_CAPACITY];
static u32 g_head; /* 下一字节写入位置 */
static u32 g_used; /* 已存字节数 */
static u32 g_lines;
static u32 g_dropped;
static u64 g_written;
static int g_ready;

/* 逻辑偏移（0 = 最旧）-> 环形下标。 */
static u32 ring_at(u32 logical)
{
    u32 start = (g_head + LOG_CAPACITY - g_used) % LOG_CAPACITY;

    return (start + logical) % LOG_CAPACITY;
}

/* 去掉最旧的一整行；缓冲里只有完整行，所以一定能找到 '\n'。 */
static void evict_oldest(void)
{
    u32 n = 0;

    if (g_used == 0) {
        return;
    }
    while (n < g_used && g_ring[ring_at(n)] != '\n') {
        n++;
    }
    if (n < g_used) {
        n++; /* 连 '\n' 一起丢 */
    }
    if (n > g_used) {
        n = g_used;
    }
    g_used -= n;
    if (g_lines > 0) {
        g_lines--;
    }
    g_dropped++;
}

/*
 * 行结束于逻辑偏移 end（不包），返回该行的起始逻辑偏移。
 * 前提：ring_at(end-1) == '\n'。
 */
static u32 line_start_before(u32 end)
{
    u32 i;

    if (end == 0) {
        return 0;
    }
    for (i = end - 1; i > 0; i--) {
        if (g_ring[ring_at(i - 1)] == '\n') {
            return i;
        }
    }
    return 0;
}

static int text_ok(const char *s, u32 min, u32 max, int allow_colon)
{
    u32 n = 0;

    if (s == NULL) {
        return 0;
    }
    while (s[n] != '\0') {
        u8 c = (u8)s[n];

        if (n >= max || c < 0x20u || c > 0x7Eu) {
            return 0;
        }
        if (!allow_colon && c == ':') {
            return 0;
        }
        n++;
    }
    return n >= min;
}

int log_init(void)
{
    u32 i;

    for (i = 0; i < LOG_CAPACITY; i++) {
        g_ring[i] = 0;
    }
    g_head = 0;
    g_used = 0;
    g_lines = 0;
    g_dropped = 0;
    g_written = 0;
    g_ready = 1;
    return 0;
}

int log_write(const char *tag, const char *msg)
{
    char line[LOG_LINE_MAX + 1u];
    u32 n = 0;
    u32 tag_len;
    u32 msg_len;
    u32 i;

    if (g_ready == 0) {
        return LOG_EINVAL;
    }
    if (!text_ok(tag, 1u, LOG_TAG_MAX - 1u, 0)) {
        return LOG_EINVAL;
    }
    if (!text_ok(msg, 1u, LOG_MSG_MAX - 1u, 1)) {
        return LOG_EINVAL;
    }

    tag_len = 0;
    while (tag[tag_len] != '\0') {
        tag_len++;
    }
    msg_len = 0;
    while (msg[msg_len] != '\0') {
        msg_len++;
    }

    for (i = 0; i < tag_len; i++) {
        line[n++] = tag[i];
    }
    line[n++] = ':';
    line[n++] = ' ';
    for (i = 0; i < msg_len; i++) {
        line[n++] = msg[i];
    }
    line[n++] = '\n';

    while (g_used + n > LOG_CAPACITY) {
        u32 before = g_used;

        evict_oldest();
        if (g_used == before) {
            return LOG_ENOSPC; /* 防御：正常不会发生 */
        }
    }

    for (i = 0; i < n; i++) {
        g_ring[(g_head + i) % LOG_CAPACITY] = line[i];
    }
    g_head = (g_head + n) % LOG_CAPACITY;
    g_used += n;
    g_lines++;
    g_written++;
    return (int)n;
}

u32 log_read(char *buf, u32 max)
{
    u32 end;
    u32 chosen;
    u32 total;
    u32 i;

    if (g_ready == 0 || buf == NULL || max == 0 || g_used == 0) {
        return 0;
    }

    end = g_used;
    chosen = end;
    total = 0;
    while (end > 0) {
        u32 start = line_start_before(end);
        u32 len = end - start;

        if (len > max - total) {
            break;
        }
        total += len;
        chosen = start;
        end = start;
    }

    if (total == 0) {
        /* 最新一行本身超过 max：给它末尾 max 字节。 */
        u32 start = line_start_before(g_used);
        u32 len = g_used - start;
        u32 skip = len - max;

        for (i = 0; i < max; i++) {
            buf[i] = g_ring[ring_at(start + skip + i)];
        }
        return max;
    }

    for (i = 0; i < total; i++) {
        buf[i] = g_ring[ring_at(chosen + i)];
    }
    return total;
}

u32 log_bytes_used(void)
{
    return g_used;
}

u32 log_line_count(void)
{
    return g_lines;
}

u32 log_dropped_lines(void)
{
    return g_dropped;
}

u64 log_written_lines(void)
{
    return g_written;
}

u32 log_capacity(void)
{
    return LOG_CAPACITY;
}
