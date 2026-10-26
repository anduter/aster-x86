#ifndef MM_BRK_RANGE_H
#define MM_BRK_RANGE_H

/* =============================================================================
 * mm/brk_range.h - SYS_BRK 增量校验（纯函数，syscall 与自检共用）
 *
 * inc 来自用户（legacy int 0x81 的 ebx），按有符号 32 位增量解释：
 *   - 正增量：old + inc 不得越过 ceil（且不得 32 位回绕）；
 *   - 负增量：old + inc 不得低于 floor（堆的 ASLR 基址）。
 *
 * 只做算术，不碰页表，因此可以在 memtest 阶段用固定参数自检。
 * ============================================================================= */
#include "../include/types.h"

static inline bool brk_range_ok(uint32_t old, uint32_t inc,
                                uint32_t floor, uint32_t ceil)
{
    if (old < floor || old > ceil) return false;

    if (inc <= 0x7FFFFFFFU) {
        /* 正向：ceil - old 先算，避免 old + inc 回绕 */
        if (inc > ceil - old) return false;
    } else {
        /* 负向：0 - inc 是二进制补码的绝对值，INT_MIN 也安全 */
        if ((0U - inc) > (old - floor)) return false;
    }
    return true;
}

#endif /* MM_BRK_RANGE_H */
