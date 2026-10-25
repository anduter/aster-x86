/* =============================================================================
 * tests/unit/test_string.c - 内核 lib/string.c 的 host 单元测试
 *
 * 编译时用 -Dmemcpy=k_memcpy 等把内核符号改名，避免与 host libc 冲突，
 * 测的仍是 src/kernel/lib/string.c 的真实实现（-fno-builtin 防止 gcc
 * 把循环改写成 libc 调用）。
 * ========================================================================== */
#include "unit.h"
#include "../../src/kernel/lib/string.h"

int main(void)
{
    char buf[64];
    char dst[16];

    UNIT_BEGIN("string");

    /* memcpy / memcmp */
    memset(buf, 0xAA, sizeof(buf));
    CHECK(memcpy(buf, "hello", 6) == buf);
    CHECK(memcmp(buf, "hello", 6) == 0);
    CHECK(memcmp(buf, "hellp", 6) < 0);
    CHECK(memcmp(buf, "helln", 6) > 0);
    CHECK(memcmp(buf, "hello", 0) == 0);

    /* memset */
    memset(buf, 0xAA, 8);
    memset(buf, 0x7F, 4);
    CHECK_U32((unsigned char)buf[0], 0x7F);
    CHECK_U32((unsigned char)buf[3], 0x7F);
    CHECK_U32((unsigned char)buf[4], 0xAA);

    /* memmove：重叠区间两个方向 */
    memcpy(buf, "0123456789", 11);
    CHECK(memmove(buf + 2, buf, 5) == buf + 2);
    CHECK(memcmp(buf, "0101234789", 11) == 0);
    memcpy(buf, "0123456789", 11);
    CHECK(memmove(buf, buf + 2, 5) == buf);
    CHECK(memcmp(buf, "2345656789", 11) == 0);
    CHECK(memmove(buf, buf, 3) == buf);

    /* strlen / strcmp / strncmp */
    CHECK_U32(strlen(""), 0);
    CHECK_U32(strlen("aster"), 5);
    CHECK(strcmp("abc", "abc") == 0);
    CHECK(strcmp("abc", "abd") < 0);
    CHECK(strcmp("abd", "abc") > 0);
    CHECK(strncmp("abcdef", "abcxyz", 3) == 0);
    CHECK(strncmp("abcdef", "abcxyz", 4) < 0);
    CHECK(strncmp("abc", "abc", 10) == 0);

    /* strcpy / strncpy（含补零与截断） */
    memset(dst, 0x55, sizeof(dst));
    CHECK(strcpy(dst, "ok") == dst);
    CHECK(strcmp(dst, "ok") == 0);
    memset(dst, 0x55, sizeof(dst));
    CHECK(strncpy(dst, "abcdef", 8) == dst);
    CHECK(strcmp(dst, "abcdef") == 0);
    CHECK_U32((unsigned char)dst[7], 0);
    memset(dst, 0x55, sizeof(dst));
    strncpy(dst, "abcdef", 3);
    CHECK(memcmp(dst, "abc", 3) == 0);
    CHECK_U32((unsigned char)dst[3], 0x55);   /* 截断时不补 NUL */

    /* strchr */
    CHECK(strchr("a/b/c", '/') != 0);
    CHECK_U32((unsigned int)(strchr("a/b/c", '/') - "a/b/c"), 1);
    CHECK(strchr("a/b/c", 'z') == 0);
    CHECK(strchr("abc", '\0') != 0);

    /* utoa：0 / 10 / 16 / 2 / 非法 base */
    CHECK_U32(utoa(0, buf, 10), 1);
    CHECK(strcmp(buf, "0") == 0);
    CHECK_U32(utoa(255, buf, 16), 2);
    CHECK(strcmp(buf, "FF") == 0);
    CHECK_U32(utoa(10, buf, 2), 4);
    CHECK(strcmp(buf, "1010") == 0);
    CHECK_U32(utoa(4294967295u, buf, 10), 10);
    CHECK(strcmp(buf, "4294967295") == 0);
    CHECK_U32(utoa(5, buf, 1), 0);
    CHECK(strcmp(buf, "") == 0);
    CHECK_U32(utoa(5, buf, 17), 0);

    /* itoa：正/负/零/INT32_MIN（取负溢出路径） */
    CHECK_U32(itoa(0, buf), 1);
    CHECK(strcmp(buf, "0") == 0);
    CHECK_U32(itoa(12345, buf), 5);
    CHECK(strcmp(buf, "12345") == 0);
    CHECK_U32(itoa(-42, buf), 3);
    CHECK(strcmp(buf, "-42") == 0);
    CHECK_U32(itoa((int32_t)(-2147483647 - 1), buf), 11);
    CHECK(strcmp(buf, "-2147483648") == 0);

    UNIT_END();
}
