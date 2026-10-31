/*
 * ng WP2-5 host 单测：log 64 KiB 按行环形缓冲。
 *
 * 覆盖：行格式/顺序、读取的“最近行”语义（含 max 小于最新行时的尾部）、
 * tag/msg 长度与字符校验、整行溢出丢弃、精确填满 64 KiB、重置统计。
 */
#include <string.h>

#include "log.h"
#include "test_util.h"

static char g_all[LOG_CAPACITY + 1u];

static void make_line_msg(char *msg, u32 index)
{
    u32 i;

    msg[0] = (char)('0' + (int)((index / 1000u) % 10u));
    msg[1] = (char)('0' + (int)((index / 100u) % 10u));
    msg[2] = (char)('0' + (int)((index / 10u) % 10u));
    msg[3] = (char)('0' + (int)(index % 10u));
    for (i = 4; i < 60u; i++) {
        msg[i] = 'a';
    }
    msg[60] = '\0';
}

/* 1. 基本写入/读取顺序。 */
static void t_log_basic(void)
{
    char buf[64];

    CHECK(log_init() == 0);
    CHECK(log_capacity() == 65536);
    CHECK(log_bytes_used() == 0);
    CHECK(log_line_count() == 0);
    CHECK(log_read(buf, sizeof(buf)) == 0);

    CHECK(log_write("t", "m") == 5);
    CHECK(log_bytes_used() == 5);
    CHECK(log_line_count() == 1);
    CHECK(log_written_lines() == 1);
    memset(buf, 0, sizeof(buf));
    CHECK(log_read(buf, sizeof(buf)) == 5);
    CHECK(strcmp(buf, "t: m\n") == 0);

    CHECK(log_write("a", "1") == 5);
    CHECK(log_write("b", "2") == 5);
    CHECK(log_bytes_used() == 15);
    CHECK(log_line_count() == 3);

    /* max=10 只放得下最近两行。 */
    memset(buf, 0, sizeof(buf));
    CHECK(log_read(buf, 10) == 10);
    CHECK(memcmp(buf, "a: 1\nb: 2\n", 10) == 0);

    /* max=5 正好一行（最新行）；max=4 是最新行的末尾 4 字节。 */
    memset(buf, 0, sizeof(buf));
    CHECK(log_read(buf, 5) == 5);
    CHECK(memcmp(buf, "b: 2\n", 5) == 0);
    memset(buf, 0, sizeof(buf));
    CHECK(log_read(buf, 4) == 4);
    CHECK(memcmp(buf, ": 2\n", 4) == 0);
    CHECK(log_read(buf, 1) == 1);
    CHECK(buf[0] == '\n');
    CHECK(log_read(NULL, 10) == 0);
    CHECK(log_read(buf, 0) == 0);
}

/* 2. tag/msg 校验。 */
static void t_log_validation(void)
{
    static char big_msg[LOG_MSG_MAX + 8u];
    static char big_tag[LOG_TAG_MAX + 8u];
    u32 i;

    CHECK(log_init() == 0);
    CHECK(log_write(NULL, "m") == LOG_EINVAL);
    CHECK(log_write("t", NULL) == LOG_EINVAL);
    CHECK(log_write("", "m") == LOG_EINVAL);
    CHECK(log_write("t", "") == LOG_EINVAL);
    CHECK(log_write("t:", "m") == LOG_EINVAL);
    CHECK(log_write("t", "bad\nline") == LOG_EINVAL);
    CHECK(log_write("t", "bad\rline") == LOG_EINVAL);
    CHECK(log_write("t", "tab\there") == LOG_EINVAL);
    CHECK(log_write("t", "del\x7f") == LOG_EINVAL);
    CHECK(log_write("t", "ctrl\x1f") == LOG_EINVAL);
    CHECK(log_write("t", "with:colon ok") > 0);

    /* 15 字符 tag + 127 字符 msg 正好 LOG_LINE_MAX(145)。 */
    for (i = 0; i < 15u; i++) {
        big_tag[i] = (char)('a' + (int)(i % 26u));
    }
    big_tag[15] = '\0';
    for (i = 0; i < 127u; i++) {
        big_msg[i] = 'x';
    }
    big_msg[127] = '\0';
    CHECK(log_write(big_tag, big_msg) == 145);

    big_tag[15] = 'z';
    big_tag[16] = '\0';
    CHECK(log_write(big_tag, "m") == LOG_EINVAL);
    big_msg[127] = 'y';
    big_msg[128] = '\0';
    CHECK(log_write("t", big_msg) == LOG_EINVAL);
    big_msg[127] = '\0';
    CHECK(log_write("t", big_msg) == 127 + 3 + 1);
}

/* 3. 溢出：精确填满 64 KiB，再多写一行丢最旧一行。 */
static void t_log_overflow(void)
{
    char msg[64];
    u32 i;
    int ok = 1;

    CHECK(log_init() == 0);
    for (i = 0; i < 1024u; i++) {
        make_line_msg(msg, i);
        if (log_write("t", msg) != 64) {
            ok = 0;
        }
    }
    CHECK(ok == 1);
    CHECK(log_bytes_used() == LOG_CAPACITY);
    CHECK(log_line_count() == 1024);
    CHECK(log_dropped_lines() == 0);

    make_line_msg(msg, 1024u);
    CHECK(log_write("t", msg) == 64);
    CHECK(log_bytes_used() == LOG_CAPACITY);
    CHECK(log_line_count() == 1024);
    CHECK(log_dropped_lines() == 1);
    CHECK(log_written_lines() == 1025);

    memset(g_all, 0, sizeof(g_all));
    CHECK(log_read(g_all, LOG_CAPACITY) == LOG_CAPACITY);
    CHECK(memcmp(g_all, "t: 0001", 7) == 0);
    CHECK(memcmp(g_all + LOG_CAPACITY - 64u, "t: 1024", 7) == 0);
    CHECK(strstr(g_all, "t: 0000") == NULL);
    CHECK(strstr(g_all, "t: 1024") != NULL);
    CHECK(g_all[LOG_CAPACITY - 1u] == '\n');
}

/* 4. 重置。 */
static void t_log_reset(void)
{
    CHECK(log_init() == 0);
    CHECK(log_bytes_used() == 0);
    CHECK(log_line_count() == 0);
    CHECK(log_dropped_lines() == 0);
    CHECK(log_written_lines() == 0);
    CHECK(log_write("x", "y") == 5);
    CHECK(log_init() == 0);
    CHECK(log_bytes_used() == 0);
}

int test_log(void)
{
    t_log_basic();
    t_log_validation();
    t_log_overflow();
    t_log_reset();
    return 0;
}
