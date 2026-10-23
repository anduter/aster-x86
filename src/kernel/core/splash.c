/* =============================================================================
 * core/splash.c - framebuffer 启动画面
 *
 * 在 fb_init() 之后、console_init() 之前绘制，随后等待约 0.8 秒再交给
 * 控制台初始化。这样用户能看到完整的 ASTER 启动画面，而正常内核日志
 * 仍然从 console 开始输出。
 * ============================================================================= */
#include "splash.h"
#include "../fb.h"
#include "../io.h"
#include "../lib/string.h"
#include "../version.h"

#define SPLASH_BG       0x081018U
#define SPLASH_PANEL    0x0E1B2CU
#define SPLASH_ACCENT   0x1F6FEBU
#define SPLASH_TITLE    0x7FDBFFU
#define SPLASH_SUB      0xA9B7C6U
#define SPLASH_BAR_BG   0x1B2A3AU
#define SPLASH_BAR_FG   0x3FB950U

static void splash_text(uint32_t y, const char *s, uint32_t scale, uint32_t fg)
{
    uint32_t n = (uint32_t)strlen(s);
    uint32_t cw = 8U * scale;
    uint32_t width = n * cw;
    uint32_t x = (fb_w > width) ? (fb_w - width) / 2U : 0U;

    for (uint32_t i = 0; i < n; i++) {
        fb_char_scaled(x + i * cw, y, s[i], fg, SPLASH_BG, scale);
    }
}

static void splash_wait(void)
{
    uint32_t lo, hi, start_lo, start_hi;

    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    start_lo = lo;
    start_hi = hi;

    /* 约 0.8 秒；上限循环数防止某些平台 TSC 不前进时卡死。 */
    for (uint32_t guard = 0; guard < 400000000U; guard++) {
        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
        uint64_t now = ((uint64_t)hi << 32) | lo;
        uint64_t start = ((uint64_t)start_hi << 32) | start_lo;
        if (now - start >= 2000000000ULL) break;
        __asm__ volatile ("pause");
    }
}

static void splash_serial(const char *s)
{
    while (*s != '\0') serial_putc(*s++);
}

void boot_splash(void)
{
    uint32_t w = fb_w != 0U ? fb_w : 1024U;
    uint32_t h = fb_h != 0U ? fb_h : 768U;
    uint32_t title_scale = (w >= 800U) ? 8U : 5U;
    uint32_t bar_w = (w > 520U) ? 420U : (w > 120U ? w - 100U : w);
    uint32_t bar_x = (w - bar_w) / 2U;
    uint32_t bar_y = h / 2U + 110U;

    /* 背景与上下强调条 */
    fb_fill_rect(0, 0, w, h, SPLASH_BG);
    fb_fill_rect(0, 0, w, 4U, SPLASH_ACCENT);
    fb_fill_rect(0, h - 4U, w, h, SPLASH_ACCENT);

    /* 中间的暗色 panel，给标题一个视觉层次 */
    {
        uint32_t px0 = (w > 720U) ? (w / 2U - 360U) : 20U;
        uint32_t px1 = (w > 720U) ? (w / 2U + 360U) : (w - 20U);
        fb_fill_rect(px0, h / 2U - 190U, px1, h / 2U + 190U, SPLASH_PANEL);
    }

    splash_text((h / 2U) - 150U, "ASTER", title_scale, SPLASH_TITLE);
    splash_text((h / 2U) + 20U, "a small native operating system", 2U, SPLASH_SUB);
    splash_text((h / 2U) + 52U, OS_NAME "  " OS_VERSION, 2U, SPLASH_SUB);

    /* 进度条：静态 72%，表示“正在进入内核” */
    fb_fill_rect(bar_x, bar_y, bar_x + bar_w, bar_y + 8U, SPLASH_BAR_BG);
    fb_fill_rect(bar_x, bar_y, bar_x + (bar_w * 72U) / 100U, bar_y + 8U, SPLASH_BAR_FG);

    splash_text(bar_y + 26U, "booting kernel ...", 1U, SPLASH_SUB);

    splash_serial("boot: splash drawn\n");
    splash_wait();
    splash_serial("boot: splash end\n");
}
