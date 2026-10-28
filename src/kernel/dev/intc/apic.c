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
#include "include/proc.h"
#include "include/mm.h"
#include "include/kernel.h"
#include "../../lib/kprintf.h"

#define MSR_APIC_BASE    0x1BU

#define LAPIC_TPR        0x80U
#define LAPIC_SVR        0xF0U
#define LAPIC_LVT_TIMER  0x320U
#define LAPIC_TIMER_INIT 0x380U
#define LAPIC_TIMER_CUR  0x390U
#define LAPIC_TIMER_DCR  0x3E0U

/* DCR 编码：0xB=/1，0x3=/16。校准必须用 /1，否则 period 与
 * “10ms = period/400（/16 计数）”的换算差一个分频因子。 */
#define LAPIC_DCR_DIV1   0xBU
#define LAPIC_DCR_DIV16  0x3U

static uint32_t lapic_base;
static int      lapic_present;
static volatile uint32_t *lapic;

static int      wd_state;            /* 0=关闭 1=校准中 2=运行 */
static uint32_t calib_count0;
static uint32_t calib_tick0;
static uint32_t final_period;
static uint32_t last_progress;
static uint32_t stuck_count;
static uint32_t bsp_last_ticks;      /* BSP PIT tick 快照（BSP 时钟停摆检测） */
static uint32_t bsp_stall_count;
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

    lapic_wr(LAPIC_TIMER_DCR, LAPIC_DCR_DIV16);               /* divide by 16 */
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

    /* BSP 时钟停摆检测（audit-smp/audit-net 线索）：
     *
     * progress_now() = 全局 timer_ticks + syscall_count。timer_ticks 只由
     * BSP 的 PIT 推进；SMP 下 AP 上的 syscall 也会让 syscall_count 增长，
     * 于是“BSP 关中断/锁死、PIT 停摆”时 progress 仍然在变，旧看门狗永远不会
     * 报警——表现为 tcptest 等依赖 timer_ticks 的线程卡 60s+ 而系统看似活着。
     *
     * 这里单独盯 BSP 的 per-CPU tick：NMI 由 BSP 自己的 LAPIC 定时器投递，
     * 不受 IF 影响，连续 8 次（~2s）没有推进就 panic 留现场。正常路径只多
     * 一次 32 位比较；AP 上的活动不会掩盖它。 */
    if (wd_state == 2) {
        uint32_t bsp_ticks = per_cpu[0].ticks;

        if (bsp_ticks != bsp_last_ticks) {
            bsp_last_ticks = bsp_ticks;
            bsp_stall_count = 0;
        } else if (++bsp_stall_count >= 8U) {
            in_panic = 1;
            panic("NMI watchdog: BSP ticks stalled ~2s (bsp_ticks=%u eip=%p "
                  "global_ticks=%u syscalls=%u)",
                  (unsigned int)bsp_ticks, (void *)(uintptr_t)f->eip,
                  timer_ticks(), syscall_count());
        }
    }

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

/* ---- LAPIC 校准 ----
 *
 * 校准目标不是“1 秒墙钟”，而是 BSP 的 PIT tick 频率：per-CPU ticks 要能和
 * BSP tick 直接比较（2s 自检、AP 看门狗都按 tick 计数）。QEMU/TCG 下 PIT
 * 边沿在 IF=0 或投递延迟时会被合并，BSP 实际 tick 间隔大于 10ms，所以用
 * “N 个 PIT tick 之间 LAPIC 走了多少 count”来定 AP 的 10ms 计数，AP 的
 * tick 频率才会跟 BSP 一致。
 *
 * 校准窗口 100 个 tick，tick 100 先武装 NMI；tick 200 用 100→200 的窗口再校准一次：QEMU/TCG 下启动早期的
 * PIT 延迟随负载变化（实测 10-tick 窗口能把 AP 装慢 40%），一次性 100-tick
 * 校准仍会偶发 >10% 偏斜。第二次校准让 AP 定时器跟踪 BSP 近期有效 tick
 * 频率，并把 AP 的 per-CPU tick 基准对齐 BSP；tick 200→400 的增量自检
 * 因此不会被中途重装打断（tick 300/400 不再校准）。 */
#define APIC_CAL_TICKS 150U

static uint32_t apic_period_from(uint32_t elapsed)
{
    uint32_t q = elapsed / APIC_CAL_TICKS;
    uint32_t r = elapsed % APIC_CAL_TICKS;
    uint32_t period = q * 25U + (r * 25U) / APIC_CAL_TICKS;

    /* 250ms = elapsed * 25 / APIC_CAL_TICKS；商/余数拆开避免 32 位溢出。 */
    return (period >= 4000U) ? period : 0U;
}

static void smp_resync_ap_ticks(void)
{
    uint32_t base = per_cpu[0].ticks;

    for (uint32_t c = 1U; c < SMP_MAX_CPUS; c++) {
        if (!per_cpu[c].online) continue;
        __atomic_store_n(&per_cpu[c].ticks, base, __ATOMIC_RELAXED);
    }
}

/* 由 PIT 中断每个 tick 调用：周期校准 + AP tick 对齐 */
void apic_tick(void)
{
    uint32_t now = timer_ticks();
    uint32_t elapsed, period;

    if (wd_state != 1) return;

    if (now - calib_tick0 < APIC_CAL_TICKS) return;

    elapsed = calib_count0 - lapic_rd(LAPIC_TIMER_CUR);
    period = apic_period_from(elapsed);
    if (period == 0U) {
        if (wd_state == 1) {
            wd_state = 0;
            kprintln("apic: timer calibration failed, watchdog disabled");
        }
        return;
    }

    calib_tick0 = now;
    calib_count0 = lapic_rd(LAPIC_TIMER_CUR);
    final_period = period;

    if (wd_state == 1) {
        /* 首次校准：武装 NMI 定时器（周期 + delivery mode 4，不屏蔽） */
        lapic_wr(LAPIC_TIMER_INIT, 0U);
        lapic_wr(LAPIC_LVT_TIMER, (4U << 8) | (1U << 17));
        lapic_wr(LAPIC_TIMER_INIT, period);

        last_progress = progress_now();
        stuck_count = 0;
        bsp_last_ticks = per_cpu[0].ticks;
        bsp_stall_count = 0;
        wd_state = 2;

        kprintf("apic: NMI watchdog armed (period=%u counts, ~250ms)\n",
                (unsigned int)final_period);
    }

    /* AP 定时器 divide-by-16：10ms 计数 = (250ms 计数)/25/16 ≈ period/400。 */
    smp_ap_timer_reload(period / 400U);

    /* per-CPU tick 是“全局时钟”的副本：每次校准对齐 BSP，防累计漂移。
     * tick 300 不对齐：irq.c 的 skew 自检用 200→400 的增量，300 的跳变
     * 会让 2s 窗口失真。AP 侧并发 +1 最多丢一次更新，下个窗口再对齐。 */
    smp_resync_ap_ticks();
}

/* 启动看门狗：映射 LAPIC、打开 APIC、开始用 PIT tick 校准 */
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

    /* 先屏蔽 LVT，用最大计数开始校准。
     * DCR 必须显式设成 divide-by-1：period 是 /1 计数，NMI 定时器沿用
     * 同一分频；AP 的 10ms 计数 = period/400（/16），公式只对 /1 成立。
     * 旧实现没写 DCR，真实硬件复位值是 /2，会让 AP 定时器快一倍。 */
    lapic_wr(LAPIC_TIMER_DCR, LAPIC_DCR_DIV1);
    lapic_wr(LAPIC_LVT_TIMER, 0x00010000U);
    lapic_wr(LAPIC_TIMER_INIT, 0xFFFFFFFFU);
    calib_tick0 = timer_ticks();
    calib_count0 = lapic_rd(LAPIC_TIMER_CUR);
    wd_state = 1;

    exception_register(2U, nmi_watchdog);  /* NMI 向量 */

    kprintln("apic: NMI watchdog calibrating ...");
    return 1;
}

uint32_t apic_timer_count_10ms(void)
{
    /* 未校准或数值离谱时返回 0，由调用方决定兜底 */
    if (final_period < 4000U) return 0U;
    return final_period / 400U;
}
