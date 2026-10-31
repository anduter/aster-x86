#ifndef NG_PROC_STR_H
#define NG_PROC_STR_H

#include "types.h"

/*
 * copyin_str() 的返回值同时表示“长度”和“被截断”：
 *   - 正常串：长度 <= cap-2（NUL 落在 cap-2 或更早）；
 *   - 无 NUL：长度 == cap-1（NUL 被强制写在 cap-1，最后一个字符已丢）。
 * 因此调用方必须用 "< cap-1" 判定，才能把“刚好填满”和“截断”都拒绝。
 */
static inline int proc_user_str_len_ok(int n, u32 cap)
{
    return (n >= 0 && cap >= 2u && (u32)n < cap - 1u) ? 1 : 0;
}

#endif /* NG_PROC_STR_H */