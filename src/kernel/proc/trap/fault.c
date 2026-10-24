/* =============================================================================
 * intr/fault.c - 通用异常修复表
 *
 * 用法：
 *   fault_register(start, end, fixup);
 * 当 #PF 的 eip 落在 [start, end) 时，异常处理把它改成 fixup 继续执行，
 * 于是这段代码可以“失败后返回错误码”，而不是杀线程/panic。
 * ============================================================================= */
#include "fault.h"
#include "../../core/error.h"

#define FAULT_MAX 8

typedef struct {
    uint32_t start;
    uint32_t end;
    uint32_t fixup;
    bool     used;
} fault_entry_t;

static fault_entry_t entries[FAULT_MAX];

int fault_register(uint32_t start, uint32_t end, uint32_t fixup)
{
    if (start == 0U || end <= start || fixup == 0U) return E_INVAL;

    for (int i = 0; i < FAULT_MAX; i++) {
        if (!entries[i].used) {
            entries[i].used = true;
            entries[i].start = start;
            entries[i].end = end;
            entries[i].fixup = fixup;
            return E_OK;
        }
    }
    return E_FULL;
}

uint32_t fault_lookup(uint32_t eip)
{
    for (int i = 0; i < FAULT_MAX; i++) {
        if (!entries[i].used) continue;
        if (eip >= entries[i].start && eip < entries[i].end) {
            return entries[i].fixup;
        }
    }
    return 0U;
}
