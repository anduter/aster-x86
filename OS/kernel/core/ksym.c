/* =============================================================================
 * core/ksym.c - 内核符号查找
 *
 * 符号表由 tools/gensyms.ps1 在链接后生成（obj/symbols.c），
 * 只保留函数符号（nm 的 T/t），按地址排序，二分查找。
 * ============================================================================= */
#include "ksym.h"

/* 第一遍链接（生成符号表之前）用的弱默认：空表。
 * 最终链接时 obj/symbols.o 里的强定义会覆盖它。 */
__attribute__((weak)) const ksym_t ksym_table[] = { { 0U, "" } };
__attribute__((weak)) const unsigned int ksym_count = 0U;

const char *ksym_lookup(unsigned int addr, unsigned int *offset)
{
    unsigned int lo = 0;
    unsigned int hi = ksym_count;
    unsigned int found = 0;
    int have = 0;

    while (lo < hi) {
        unsigned int mid = (lo + hi) / 2U;

        if (ksym_table[mid].addr <= addr) {
            found = mid;
            have = 1;
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }

    if (!have) return 0;
    if (offset != 0) *offset = addr - ksym_table[found].addr;
    return ksym_table[found].name;
}
