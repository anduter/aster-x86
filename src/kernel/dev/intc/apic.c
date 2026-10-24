/* =============================================================================
 * core/apic.c - 本地 APIC：探测 + LAPIC 定时器 + NMI 看门狗
 *
 * 探测：CPUID.1:EDX[9] + IA32_APIC_BASE(MSR 0x1B)。
 * 看门狗：
 *   1. 映射 LAPIC 页（0xFEE00000，禁止缓存），打开 APIC（SVR）；
 *   2. 用 PIT 的 100Hz tick 校准 LAPIC 定时器频率（100ms 内计数值差）；
 *   3. LVT Timer 配成“周期 + NMI 投递”，约每 250ms 打一次 NMI；
 *   4. NMI 处理函数比较 (timer_ticks + syscall_count) 是否前进：
 *      连续 4 次没变化（约 1 秒）说明系统卡死 → panic 并打印 eip。
 * 因为 NMI 不受 IF 影响，即使内核把中断关掉卡死，也能被发现。
 * ============================================================================= */
#include "apic.h"
#include "../../proc/smp.h"
#include "../../core/error.h"
#include "../../mm/vmm.h"
#include "../../intr/interrupt.h"
#include "../../intr/syscall.h"
#include "../../panic.h"
#include "../../lib/kprintf.h"

#define MSR_APIC_BASE    0x1BU

#define LAPIC_TPR        0x80U
#define LAPIC_SVR        0xF0U
#define LAPIC_LVT_TIMER  0x320U
#define LAPIC_TIMER_INIT 0x380U
#define LAPIC_TIMER_CUR  0x390U

static uint32_t lapic_base;
static int      lapic_present;
static volatile uint32_t *lapic;

static int      wd_state;            /* 0=关闭 1=校准中 2=运行 */
static uint32_t calib_count0;
static uint32_t calib_tick0;
static uint32_t final_period;
static uint32_t last_progress;
static uint32_t stuck_count;
static int      in_panic;

uint32_t rdmsr_low(uint32_t msr)
{
    uint32_t lo, hi;

    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    (void)hi;
    return lo;
}

void wrmsr_low(uint32_t msr, uint32_t value)
{
    __asm__ volatile("wrmsr" :: "c"(msr), "a"(value), "d"(0U));
}

static int cpuid_has_apic(void)
{
    uint32_t eax = 1, ebx = 0, ecx = 0, edx = 0;

    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    return (edx & (1U << 9)) != 0U;
}

uint32_t apic_base(void) { return lapic_base; }
int apic_present(void) { return lapic_present; }

uint32_t apic_id_from_cpuid(void)
{
    uint32_t eax = 1, ebx = 0, ecx = 0, edx = 0;

    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    return (ebx >> 24) & 0xFFU;
}

int apic_init(void)
{
    uint32_t base;

    if (!cpuid_has_apic()) {
        kprintln("apic: not present (CPUID.1:EDX[9]=0)");
        return 0;
    }

    base = rdmsr_low(MSR_APIC_BASE) & 0xFFFFF000U;
    lapic_base = base;
    lapic_present = 1;

    kprintf("apic: LAPIC present, base=0x%x (PIC mode retained)\n",
            (unsigned int)base);
    return 1;
}

/* ---- LAPIC 寄存器访问 ---- */
static uint32_t lapic_rd(uint32_t off)
{
    return lapic[off / 4U];
}

static void lapic_wr(uint32_t off, uint32_t v)
{
    lapic[off / 4U] = v;
}

int apic_prepare(void)
{
    if (!lapic_present) return 0;

    if (lapic == 0) {
        if (vmm_map_mmio(lapic_base, 0x1000U) != 0) {
            kprintln("apic: cannot map LAPIC page for AP startup");
            return 0;
        }
        lapic = (volatile uint32_t *)(uintptr_t)lapic_base;
    }

    lapic_wr(LAPIC_TPR, 0U);
    lapic_wr(LAPIC_SVR, 0x1FFU);          /* 打开 APIC，伪向量 0xFF */
    return 1;
}

int apic_prepare_ap(void)
{
    if (!apic_prepare()) return 0;

    /* AP 不接管 8259 PIC；屏蔽 LINT0/LINT1，只保留本地 timer。 */
    lapic_wr(0x350U, 0x00010000U);
    lapic_wr(0x360U, 0x00010000U);
    return 1;
}

int apic_start_timer_ap(uint32_t vector, uint32_t count){
    if (lapic == 0) return -1;

    lapic_wr(0x3E0U, 0x3U);                                   /* divide by 16 */
    lapic_wr(LAPIC_LVT_TIMER, (vector & 0xFFU) | (1U << 17)); /* periodic */
    lapic_wr(LAPIC_TIMER_INIT, count);
    return 0;
}

void apic_eoi(void)
{
    if (lapic != 0) lapic_wr(0xB0U, 0U);
}

/* ---- IPI：INIT / SIPI（SMP AP 启动用） ---- */
#define LAPIC_ICR_LOW   0x300U
#define LAPIC_ICR_HIGH  0x310U

uint32_t apic_id(void)
{
    if (lapic == 0) return apic_id_from_cpuid();
    return (lapic_rd(0x20U) >> 24) & 0xFFU;
}

int apic_send_ipi(uint32_t dest_apic_id, uint32_t low)
{
    if (lapic == 0) return -1;

    /* 等上一条 IPI 发送完成（bit12 = delivery status） */
    for (uint32_t i = 0; i < 1000000U; i++) {
        if ((lapic_rd(LAPIC_ICR_LOW) & (1U << 12)) == 0U) break;
    }

    lapic_wr(LAPIC_ICR_HIGH, (dest_apic_id & 0xFFU) << 24);
    lapic_wr(LAPIC_ICR_LOW, low);
    return 0;
}

/* “系统还活着”的度量：时钟 tick + 系统调用次数 */
static uint32_t progress_now(void)
{
    return timer_ticks() + syscall_count();
}

static int nmi_watchdog(interrupt_frame_t *f)
{
    uint32_t p;

    if (in_panic) return EXC_HANDLED;      /* 已经 panic，别递归 */

    p = progress_now();
    if (p == last_progress) {
        stuck_count++;
        if (stuck_count >= 4U) {
            in_panic = 1;
            panic("NMI watchdog: no progress for ~1s (eip=%p ticks=%u)",
                  (void *)(uintptr_t)f->eip, timer_ticks());
        }
    } else {
        last_progress = p;
        stuck_count = 0;
    }
    return EXC_HANDLED;
}

/* 启动看门狗：映射 LAPIC、打开 APIC、开始用 PIT 校准 */
int apic_watchdog_start(void)
{
    if (!lapic_present) return 0;

    if (vmm_map_mmio(lapic_base, 0x1000U) != 0) {
        kprintln("apic: cannot map LAPIC page");
        return 0;
    }
    lapic = (volatile uint32_t *)(uintptr_t)lapic_base;

    lapic_wr(LAPIC_TPR, 0U);               /* 允许所有优先级 */
    lapic_wr(LAPIC_SVR, 0x1FFU);           /* 打开 APIC，伪向量 0xFF */

    /* 先屏蔽 LVT，用最大计数做 100ms 校准 */
    lapic_wr(LAPIC_LVT_TIMER, 0x00010000U);
    lapic_wr(LAPIC_TIMER_INIT, 0xFFFFFFFFU);
    calib_tick0 = timer_ticks();
    calib_count0 = lapic_rd(LAPIC_TIMER_CUR);
    wd_state = 1;

    exception_register(2U, nmi_watchdog);  /* NMI 向量 */

    kprintln("apic: NMI watchdog calibrating ...");
    return 1;
}

/* 由 PIT 中断每个 tick 调用：完成校准，之后由 NMI 负责检查 */
void apic_tick(void)
{
    if (wd_state != 1) return;

    if (timer_ticks() - calib_tick0 >= 10U) {         /* 100ms */
        uint32_t now_count = lapic_rd(LAPIC_TIMER_CUR);
        uint32_t elapsed = calib_count0 - now_count;  /* 100ms 内的计数 */
        uint32_t period;

        if (elapsed < 1000U) {                        /* 读不到计数，放弃 */
            wd_state = 0;
            kprintln("apic: timer calibration failed, watchdog disabled");
            return;
        }

        period = elapsed * 10U / 4U;                  /* 250ms */
        final_period = period;

        /* 周期 + NMI 投递（delivery mode = 4），不屏蔽 */
        lapic_wr(LAPIC_TIMER_INIT, 0U);
        lapic_wr(LAPIC_LVT_TIMER, (4U << 8) | (1U << 17));
        lapic_wr(LAPIC_TIMER_INIT, period);

        last_progress = progress_now();
        stuck_count = 0;
        wd_state = 2;

        kprintf("apic: NMI watchdog armed (period=%u counts, ~250ms)\n",
                (unsigned int)final_period);

        /* AP 定时器用 divide-by-16：10ms 计数 = (250ms 计数)/25/16 ≈ period/400 */
        if (period >= 4000U) {
            smp_ap_timer_reload(period / 400U);
        }
    }
}

uint32_t apic_timer_count_10ms(void)
{
    /* 未校准或数值离谱时返回 0，由调用方决定兜底 */
    if (final_period < 4000U) return 0U;
    return final_period / 400U;
}
