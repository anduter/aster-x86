/* =============================================================================
 * tests/unit/unit.h - 极简 host 单元测试断言（不依赖 libc 头）
 *
 * 为什么手工声明 printf：内核头 include/types.h 把 size_t typedef 成
 * unsigned int（32 位 ABI），而 host libc 的 <stdio.h>/<stdlib.h> 把 size_t
 * 定义成 64 位，同一个 TU 里会冲突。单元测试只需要输出，所以只声明用到的符号。
 *
 * 用法：
 *   UNIT_BEGIN("string");
 *   CHECK(cond);
 *   UNIT_END();
 * 退出码：0 = 全 PASS，1 = 有失败（tools/unit.ps1 以退出码为门禁）。
 * ========================================================================== */
#ifndef UNIT_H
#define UNIT_H

int printf(const char *fmt, ...);

static int unit_checks;
static int unit_failures;

#define UNIT_BEGIN(name)                                                     \
    do {                                                                     \
        printf("== %s ==\n", (name));                                        \
        unit_checks = 0;                                                     \
        unit_failures = 0;                                                   \
    } while (0)

#define CHECK(cond)                                                          \
    do {                                                                     \
        unit_checks++;                                                       \
        if (!(cond)) {                                                       \
            unit_failures++;                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                    \
    } while (0)

#define CHECK_U32(actual, expected)                                          \
    do {                                                                     \
        unsigned int a_ = (unsigned int)(actual);                            \
        unsigned int e_ = (unsigned int)(expected);                          \
        unit_checks++;                                                       \
        if (a_ != e_) {                                                      \
            unit_failures++;                                                 \
            printf("FAIL %s:%d: %s = %u, expected %u\n",                     \
                   __FILE__, __LINE__, #actual, a_, e_);                     \
        }                                                                    \
    } while (0)

#define CHECK_INT(actual, expected)                                          \
    do {                                                                     \
        int a_ = (int)(actual);                                              \
        int e_ = (int)(expected);                                            \
        unit_checks++;                                                       \
        if (a_ != e_) {                                                      \
            unit_failures++;                                                 \
            printf("FAIL %s:%d: %s = %d, expected %d\n",                     \
                   __FILE__, __LINE__, #actual, a_, e_);                     \
        }                                                                    \
    } while (0)

#define CHECK_MEM(actual, expected, len)                                     \
    do {                                                                     \
        const unsigned char *a_ = (const unsigned char *)(actual);           \
        const unsigned char *e_ = (const unsigned char *)(expected);         \
        unsigned int n_ = (unsigned int)(len);                               \
        unsigned int i_;                                                     \
        int bad_ = 0;                                                        \
        for (i_ = 0; i_ < n_; i_++) {                                        \
            if (a_[i_] != e_[i_]) { bad_ = 1; break; }                       \
        }                                                                    \
        unit_checks++;                                                       \
        if (bad_) {                                                          \
            unit_failures++;                                                 \
            printf("FAIL %s:%d: %s differs at byte %u\n",                    \
                   __FILE__, __LINE__, #actual, i_);                         \
        }                                                                    \
    } while (0)

#define UNIT_END()                                                           \
    do {                                                                     \
        printf("%d checks, %d failures\n", unit_checks, unit_failures);      \
        return (unit_failures == 0) ? 0 : 1;                                 \
    } while (0)

#endif /* UNIT_H */
