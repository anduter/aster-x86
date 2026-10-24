/* =============================================================================
 * core/cpu.c - 可选 CPU 安全特性：SMEP / SMAP
 *
 * SMEP：ring0 不能执行用户页。
 * SMAP：ring0 不能读写用户页，除非用 stac/clac 打开访问窗口。
 *
 * 只有 CPUID.07H:EBX 对应位为 1 时才置 CR4 位；否则保持关闭并打印状态。
 * ============================================================================= */
#include "cpu.h"
#include "include/kernel.h"

volatile int smap_enabled;
static int smep_on;
static int smap_on;

static void cpuid(uint32_t leaf, uint32_t sub,
                  uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    uint32_t eax = leaf, ebx = 0, ecx = sub, edx = 0;

    __asm__ volatile ("cpuid"
                      : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
    *a = eax;
    *b = ebx;
    *c = ecx;
    *d = edx;
}

void cpu_security_init(void)
{
    uint32_t max_leaf, a, b, c, d;
    uint32_t cr4;
    uint32_t want = 0;
    int smep_supported = 0;
    int smap_supported = 0;

    cpuid(0U, 0U, &max_leaf, &b, &c, &d);
    if (max_leaf >= 7U) {
        cpuid(7U, 0U, &a, &b, &c, &d);
        smep_supported = (b & (1U << 7)) != 0U;
        smap_supported = (b & (1U << 20)) != 0U;
    }

    if (smep_supported) want |= (1U << 20);   /* CR4.SMEP */
    if (smap_supported) want |= (1U << 21);   /* CR4.SMAP */

    if (want != 0U) {
        __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= want;
        __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4) : "memory");
    }

    smep_on = smep_supported;
    smap_on = smap_supported;
    smap_enabled = smap_on;

    kprintf("cpu: SMEP supported=%d enabled=%d, SMAP supported=%d enabled=%d\n",
            smep_supported, smep_on, smap_supported, smap_on);
    if (smap_on) {
        kprintln("cpu: SMAP active; user copies use stac/clac");
    }
}

int cpu_smep_enabled(void) { return smep_on; }
int cpu_smap_enabled(void) { return smap_on; }
