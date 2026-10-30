/* =============================================================================
 * core/klog.c - 内核日志环（P2 dmesg）
 *
 * 64KB 固定环，按行保存；写入路径只做 memcpy + 关中断短自旋，绝无阻塞/睡眠，
 * 因而可以从 IRQ 上下文和 kprintf 里直接调用。
 *
 * 读侧（SYS_DMESG / shell dmesg）拿的是“只读快照”：先在锁内把环里的字节拷到
 * 调用方缓冲，锁外再 copy_to_user / 打印；用户缓冲缺页不会影响环本身。
 *
 * 行语义：只有 '\n' 才算一行结束；溢出时从最旧的一端“整行”丢弃，因此快照
 * 总是以行首开始（末尾可能是尚未换行的半行，这是 kprintf 的既有行为）。
 * ============================================================================= */
#include "../include/types.h"
#include "include/klog.h"
#include "include/kernel.h"
#include "include/dev.h"
#include "include/abi.h"
#include "include/proc.h"      /* syscall_count()/timer_ticks()：自检就绪判断 */
#include "spinlock.h"

#define KLOG_CANARY   0x4B4C4F47U       /* "KLOG"：环前后金丝雀 */
#define KLOG_LINE_MAX 640U              /* "<level><tag>: <msg>\n" 上限 */
#define KLOG_SAVE_MAX 4096U             /* 自检前保存的启动日志尾部 */

/* 环本体 + 前后金丝雀：越界写会先踩到 canary（自检里检查）。 */
static struct {
    uint32_t pre;
    char     ring[KLOG_RING_SIZE];
    uint32_t post;
} klog_area = { KLOG_CANARY, { 0 }, KLOG_CANARY };

static spinlock_t klog_lock = SPINLOCK_INIT;
static uint32_t   ring_head;            /* 最旧字节在 ring[] 里的下标 */
static uint32_t   ring_len;             /* 有效字节数（<= KLOG_RING_SIZE） */
static uint32_t   ring_lines;           /* 有效完整行数 */
static uint32_t   ring_drops;           /* 因溢出丢掉的旧行数 */
static uint32_t   klog_ready;           /* 惰性初始化：静态环本身零初始化即可用 */
static uint32_t   klog_self_active;     /* 自检互斥（原子） */
static uint32_t   klog_self_done;

/* 静态 BSS 环不依赖分配器，因此“未调用 klog_init”并不代表不可用：第一次
 * 写入时惰性置 ready 即可。保留显式 init 入口，给后续 bootseq 注册用。 */
void klog_init(void)
{
    klog_ready = 1U;
}

static void klog_ensure_ready(void)
{
    if (!klog_ready) klog_init();
}

/* ------------------------------------------------------------------------- */
/* 环内部：调用者必须已持 klog_lock                                        */
/* ------------------------------------------------------------------------- */

static char ring_byte(uint32_t off)
{
    return klog_area.ring[(ring_head + off) % KLOG_RING_SIZE];
}

/* 丢掉最旧的一整行（含 '\n'）。环里没有换行（单行过长）时整体清空。 */
static bool ring_drop_oldest(void)
{
    uint32_t i;

    if (ring_len == 0U) return false;

    for (i = 0; i < ring_len; i++) {
        if (ring_byte(i) == '\n') {
            ring_head = (ring_head + i + 1U) % KLOG_RING_SIZE;
            ring_len -= i + 1U;
            if (ring_lines > 0U) ring_lines--;
            ring_drops++;
            return true;
        }
    }

    /* 整个环是一条没有换行的超长行：只能整体丢弃，保证不会越界。 */
    ring_head = 0U;
    ring_len = 0U;
    ring_lines = 0U;
    ring_drops++;
    return true;
}

static void ring_append(const char *s, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (ring_len >= KLOG_RING_SIZE) {
            if (!ring_drop_oldest()) break;
        }
        klog_area.ring[(ring_head + ring_len) % KLOG_RING_SIZE] = s[i];
        ring_len++;
        if (s[i] == '\n') ring_lines++;
    }
}

static void ring_reset(void)
{
    ring_head = 0U;
    ring_len = 0U;
    ring_lines = 0U;
    ring_drops = 0U;
}

static void ring_copy(char *out, uint32_t begin, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) out[i] = ring_byte(begin + i);
}

/* 把 "<level><tag>: <msg>\n" 组进 line，返回字节数（不含结尾 NUL）。 */
static uint32_t build_line(char *out, uint32_t cap, int level,
                           const char *tag, const char *msg)
{
    const char *prefix = "";
    const char *p;
    uint32_t n = 0;

    switch (level) {
    case KLOG_WARN:  prefix = "[warn] "; break;
    case KLOG_ERR:   prefix = "[err] ";  break;
    case KLOG_DEBUG: prefix = "[dbg] ";  break;
    default:         break;
    }

    if (tag == 0) tag = "kern";
    if (msg == 0) msg = "";

    for (p = prefix; *p != '\0' && n + 1U < cap; p++) out[n++] = *p;
    for (p = tag; *p != '\0' && n + 1U < cap; p++) out[n++] = *p;
    if (n + 1U < cap) out[n++] = ':';
    if (n + 1U < cap) out[n++] = ' ';
    for (p = msg; *p != '\0' && n + 1U < cap; p++) out[n++] = *p;

    if (n == 0U || out[n - 1U] != '\n') {
        if (n + 1U < cap) out[n++] = '\n';
    }
    out[n] = '\0';
    return n;
}

/* 子串查找（锁外使用；只读环）。 */
static bool ring_has(const char *needle)
{
    uint32_t nl, i, j, flags;
    bool found = false;

    if (needle == 0) return false;
    nl = (uint32_t)strlen(needle);
    if (nl == 0U) return false;

    flags = spin_lock_irqsave(&klog_lock);
    if (ring_len >= nl) {
        for (i = 0; i + nl <= ring_len; i++) {
            for (j = 0; j < nl; j++) {
                if (ring_byte(i + j) != needle[j]) break;
            }
            if (j == nl) { found = true; break; }
        }
    }
    spin_unlock_irqrestore(&klog_lock, flags);
    return found;
}

/* ------------------------------------------------------------------------- */
/* 公开接口                                                                  */
/* ------------------------------------------------------------------------- */

void klog_write(int level, const char *tag, const char *msg)
{
    char line[KLOG_LINE_MAX];
    uint32_t n, flags;

    klog_ensure_ready();
    n = build_line(line, sizeof(line), level, tag, msg);
    if (n == 0U) return;

    flags = spin_lock_irqsave(&klog_lock);
    ring_append(line, n);
    spin_unlock_irqrestore(&klog_lock, flags);
}

void klog_printf(int level, const char *tag, const char *fmt, ...)
{
    char msg[KLOG_MSG_MAX];
    char line[KLOG_LINE_MAX];
    va_list ap;
    uint32_t n, flags;

    if (fmt == 0) return;
    klog_ensure_ready();

    va_start(ap, fmt);
    kvsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    n = build_line(line, sizeof(line), level, tag, msg);
    if (n == 0U) return;

    flags = spin_lock_irqsave(&klog_lock);
    ring_append(line, n);
    spin_unlock_irqrestore(&klog_lock, flags);

    /* 直接写 console；不走 kprintf，避免同一条日志被 hook 再记一次。 */
    console_write_atomic(line);
}

/* 自检就绪条件：只在 -Test 构建跑；int 0x81 门已被验证过（boot 的
 * syscall_selftest 已经调用过 SYS_WRITE）、时钟中断已在推进。用状态判断
 * 而不是匹配日志文本，避免隐式触发。 */
static bool klog_selftest_ready(void)
{
    if (!boot_test_build()) return false;
    if (syscall_count() == 0U) return false;
    if (timer_ticks() == 0U) return false;
    return true;
}

/* kprintf/kprintln 的同步 hook：原样收进环，之后做一次性的自检触发。 */
void klog_capture(const char *buf, uint32_t len)
{
    uint32_t flags;

    if (buf == 0 || len == 0U) return;
    klog_ensure_ready();

    flags = spin_lock_irqsave(&klog_lock);
    ring_append(buf, len);
    spin_unlock_irqrestore(&klog_lock, flags);

    if (klog_self_done) return;
    if (klog_selftest_ready()) klog_selftest();
}

uint32_t klog_read(char *out, uint32_t maxlen)
{
    uint32_t flags, n, begin, k;

    if (out == 0 || maxlen == 0U) return 0U;

    flags = spin_lock_irqsave(&klog_lock);
    if (ring_len == 0U) {
        spin_unlock_irqrestore(&klog_lock, flags);
        return 0U;
    }

    if (maxlen >= ring_len) {
        begin = 0U;
        n = ring_len;
    } else {
        begin = ring_len - maxlen;
        n = maxlen;
        /* 裁掉开头半行：跳到第一个换行之后（只剩一个换行时保留原样）。 */
        for (k = 0; k < n; k++) {
            if (ring_byte(begin + k) == '\n') break;
        }
        if (k + 1U < n) {
            begin += k + 1U;
            n -= k + 1U;
        }
    }

    ring_copy(out, begin, n);
    spin_unlock_irqrestore(&klog_lock, flags);
    return n;
}

uint32_t klog_read_tail_lines(char *out, uint32_t maxlen, uint32_t lines)
{
    uint32_t flags, n, begin, i, last;

    if (out == 0 || maxlen == 0U || lines == 0U) return 0U;

    flags = spin_lock_irqsave(&klog_lock);
    if (ring_len == 0U) {
        spin_unlock_irqrestore(&klog_lock, flags);
        return 0U;
    }

    /* 从尾部往回数 lines 行；环尾的换行算作上一行的结束。 */
    i = ring_len;
    begin = 0U;
    while (i > 0U) {
        while (i > 0U && ring_byte(i - 1U) == '\n') i--;
        if (i == 0U) { begin = 0U; break; }

        begin = i;
        while (begin > 0U && ring_byte(begin - 1U) != '\n') begin--;
        i = begin;
        if (--lines == 0U) break;
    }

    n = ring_len - begin;
    if (n > maxlen) {
        begin = ring_len - maxlen;
        n = maxlen;
        for (i = 0; i < n; i++) {
            if (ring_byte(begin + i) == '\n') break;
        }
        if (i + 1U < n) {
            begin += i + 1U;
            n -= i + 1U;
        }
    }

    /* 只返回完整行：末尾半行（如果有）丢掉，避免 dmesg 打印半行。 */
    last = n;
    while (last > 0U && ring_byte(begin + last - 1U) != '\n') last--;
    if (last > 0U) n = last;

    ring_copy(out, begin, n);
    spin_unlock_irqrestore(&klog_lock, flags);
    return n;
}

uint32_t klog_used(void)
{
    return ring_len;
}

uint32_t klog_lines(void)
{
    return ring_lines;
}

uint32_t klog_dropped(void)
{
    return ring_drops;
}

bool klog_contains(const char *needle)
{
    return ring_has(needle);
}

/* 把最近 lines 行打到 console（同时镜像串口）。dmesg 命令与自检共用。 */
uint32_t klog_print_tail(uint32_t lines)
{
    static char snap[4096];
    char head[96];
    char line[KLOG_LINE_MAX];
    uint32_t n, start, i, printed = 0;

    n = klog_read_tail_lines(snap, sizeof(snap) - 1U, lines);

    (void)ksnprintf(head, sizeof(head),
                    "dmesg: last %u line(s) (%u/%u bytes, %u dropped)\n",
                    (unsigned int)lines, (unsigned int)klog_used(),
                    (unsigned int)KLOG_RING_SIZE, (unsigned int)klog_dropped());
    console_write_atomic(head);

    if (n == 0U) {
        console_write_atomic("dmesg: ring empty\n");
        return 0U;
    }

    start = 0U;
    for (i = 0; i < n; i++) {
        if (snap[i] != '\n') continue;
        {
            uint32_t len = i - start;
            if (len >= sizeof(line)) len = (uint32_t)sizeof(line) - 1U;
            memcpy(line, snap + start, len);
            line[len] = '\0';
            if (len > 0U) console_write_atomic(line);
            console_write_atomic("\n");
            printed++;
        }
        start = i + 1U;
    }
    if (start < n) {
        uint32_t len = n - start;
        if (len >= sizeof(line)) len = (uint32_t)sizeof(line) - 1U;
        memcpy(line, snap + start, len);
        line[len] = '\0';
        console_write_atomic(line);
        console_write_atomic("\n");
        printed++;
    }
    return printed;
}

/* ------------------------------------------------------------------------- */
/* 自检                                                                      */
/* ------------------------------------------------------------------------- */

static int klog_syscall_dmesg(uint32_t buf, uint32_t maxlen)
{
    int ret;

    __asm__ volatile ("int $0x81"
                      : "=a"(ret)
                      : "a"(SYS_DMESG), "b"(buf), "c"(maxlen)
                      : "memory");
    return ret;
}

static bool buf_has(const char *buf, uint32_t len, const char *needle)
{
    uint32_t nl = (uint32_t)strlen(needle), i, j;

    if (nl == 0U || len < nl) return false;
    for (i = 0; i + nl <= len; i++) {
        for (j = 0; j < nl; j++) {
            if (buf[i + j] != needle[j]) break;
        }
        if (j == nl) return true;
    }
    return false;
}

static void klog_selftest_body(void)
{
    static char keep[KLOG_SAVE_MAX];
    char text[96];
    char small[192];
    uint32_t i, keep_n, used, lines, drops, n;
    int rc_small, rc_full, rc_bad_hi, rc_bad_map;
    bool ok = true;

    /* 1) 先保存启动日志尾部：溢出测试会顶掉整环，测完再放回去。 */
    keep_n = klog_read(keep, sizeof(keep));

    /* 2) 写 > 64KB（1400 * 48B ≈ 67KB），验证只保留最近行且不越界。 */
    for (i = 0; i < 1400U; i++) {
        (void)ksnprintf(text, sizeof(text),
                        "fill line %04u ................................",
                        (unsigned int)i);
        klog_write(KLOG_INFO, "klog", text);
    }

    used  = klog_used();
    lines = klog_lines();
    drops = klog_dropped();

    if (klog_area.pre != KLOG_CANARY || klog_area.post != KLOG_CANARY) ok = false;
    if (used > KLOG_RING_SIZE) ok = false;
    if (used < KLOG_RING_SIZE - 256U) ok = false;   /* 应当基本写满 */
    if (lines == 0U || drops == 0U) ok = false;
    if (!klog_contains("fill line 1399")) ok = false;  /* 最近行还在 */
    if (klog_contains("fill line 0000")) ok = false;   /* 最旧行已丢 */

    if (!ok) {
        kprintln("klog: ring 64KB FAIL (used=%u lines=%u drops=%u pre=%x post=%x)",
                 (unsigned int)used, (unsigned int)lines, (unsigned int)drops,
                 (unsigned int)klog_area.pre, (unsigned int)klog_area.post);
    } else {
        kprintln("klog: ring detail used=%u lines=%u dropped=%u",
                 (unsigned int)used, (unsigned int)lines, (unsigned int)drops);
        kprintln("klog: ring 64KB ok");
    }

    /* 3) 小缓冲截断：只返回最新的一段，且不超过请求长度。 */
    n = klog_read(small, 128U);
    if (n > 0U && n <= 128U && small[n - 1U] == '\n') {
        kprintln("klog: truncate detail asked=128 got=%u", (unsigned int)n);
        kprintln("klog: truncate ok");
    } else {
        ok = false;
        kprintln("klog: truncate FAIL (n=%u last=%d)",
                 (unsigned int)n, (n > 0U) ? (int)small[n - 1U] : -1);
    }

    /* 4) 恢复启动日志尾部（自检不把真实启动日志顶掉）。 */
    {
        uint32_t flags = spin_lock_irqsave(&klog_lock);
        ring_reset();
        if (keep_n > 0U) ring_append(keep, keep_n);
        spin_unlock_irqrestore(&klog_lock, flags);
    }

    /* 5) SYS_DMESG：小缓冲 / 正常缓冲 / 坏指针（窗口拒绝 + 未映射页）。 */
    klog_write(KLOG_INFO, "dmesg", "syscall probe");
    memset(small, 0, sizeof(small));

    rc_small    = klog_syscall_dmesg((uint32_t)(uintptr_t)small, 64U);
    rc_full     = klog_syscall_dmesg((uint32_t)(uintptr_t)small, sizeof(small));
    rc_bad_hi   = klog_syscall_dmesg(0xC0000000U, 16U);   /* >= 3GB：窗口拒绝 */
    rc_bad_map  = klog_syscall_dmesg(0xBFFFF000U, 16U);   /* 未映射：copy 失败 */

    if (rc_small <= 0 || rc_small > 64) ok = false;
    if (rc_full <= 0 || rc_full > (int)sizeof(small)) ok = false;
    if (!buf_has(small, (uint32_t)rc_full, "dmesg: syscall probe")) ok = false;
    if (rc_bad_hi != -1 || rc_bad_map != -1) ok = false;

    if (ok) {
        kprintln("dmesg: syscall detail small=%d full=%d bad_hi=%d bad_map=%d",
                 rc_small, rc_full, rc_bad_hi, rc_bad_map);
        kprintln("dmesg: syscall ok");
    } else {
        kprintln("dmesg: syscall FAIL (small=%d full=%d bad_hi=%d bad_map=%d)",
                 rc_small, rc_full, rc_bad_hi, rc_bad_map);
    }

    /* 6) `dmesg -n N` 的打印路径：演示最近 3 行（与 shell 命令同一实现）。 */
    (void)klog_print_tail(3U);

    if (ok) kprintln("dmesg: selftest PASS");
    else    kprintln("dmesg: selftest FAIL");
}

void klog_selftest(void)
{
    if (klog_self_done) return;
    if (atomic_xchg(&klog_self_active, 1U) != 0U) return;

    klog_selftest_body();
    klog_self_done = 1U;
}
