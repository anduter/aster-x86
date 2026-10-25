/* =============================================================================
 * core/splash.c - framebuffer 启动画面
 *
 * 在 fb_init() 之后、console_init() 之前绘制：深色竖向渐变 + 点阵网格 +
 * 居中徽标 + 字标 + 动画进度条。约 0.8 秒后交给控制台，正常内核日志仍从
 * console 开始输出。
 * ============================================================================= */
#include "splash.h"
#include "include/dev.h"
#include "include/kernel.h"

/* ---- 调色板 ---- */
#define SPLASH_BG_TOP    0x0A0E14U
#define SPLASH_BG_BOT    0x131C28U
#define SPLASH_DOT       0x16202CU
#define SPLASH_ACCENT    0x4C8DFFU
#define SPLASH_ACCENT2   0x22D3EEU
#define SPLASH_TEXT      0xE6EDF3U
#define SPLASH_SUB       0x8B949EU
#define SPLASH_BAR_BG    0x1C2430U
#define SPLASH_LINE      0x243040U

static void fill(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t c)
{
    fb_fill_rect(x0, y0, x1, y1, c);
}

static uint32_t lerp_color(uint32_t a, uint32_t b, uint32_t t, uint32_t tmax)
{
    uint32_t r = (((a >> 16) & 0xFFU) * (tmax - t) + ((b >> 16) & 0xFFU) * t) / tmax;
    uint32_t g = (((a >> 8) & 0xFFU) * (tmax - t) + ((b >> 8) & 0xFFU) * t) / tmax;
    uint32_t bl = ((a & 0xFFU) * (tmax - t) + (b & 0xFFU) * t) / tmax;
    return (r << 16) | (g << 8) | bl;
}

static uint32_t text_width(const char *s, uint32_t scale, uint32_t gap)
{
    uint32_t n = (uint32_t)strlen(s);
    if (n == 0U) return 0U;
    return n * 8U * scale + (n - 1U) * gap;
}

/* 居中文本：scale 放大，gap 为字符间距（像素） */
static void text_center(uint32_t y, const char *s, uint32_t scale, uint32_t gap, uint32_t fg)
{
    uint32_t w = text_width(s, scale, gap);
    uint32_t x = (fb_w > w) ? (fb_w - w) / 2U : 0U;
    uint32_t cw = 8U * scale;

    for (uint32_t i = 0; s[i] != '\0'; i++) {
        fb_char_scaled(x + i * (cw + gap), y, s[i], fg, SPLASH_BG_BOT, scale);
    }
}

/* 徽标：细边框方块 + 大号 "A" + 四角装饰 */
static void badge(uint32_t cx, uint32_t cy, uint32_t size)
{
    uint32_t half = size / 2U;
    uint32_t x0 = cx - half, y0 = cy - half;
    uint32_t x1 = cx + half, y1 = cy + half;
    uint32_t t = 2U;
    uint32_t gs = (size >= 120U) ? 12U : 8U;

    fill(x0, y0, x1, y0 + t, SPLASH_ACCENT);
    fill(x0, y1 - t, x1, y1, SPLASH_ACCENT);
    fill(x0, y0, x0 + t, y1, SPLASH_ACCENT);
    fill(x1 - t, y0, x1, y1, SPLASH_ACCENT);

    fill(x0 - 8U, y0 - 8U, x0, y0, SPLASH_ACCENT2);
    fill(x1, y0 - 8U, x1 + 8U, y0, SPLASH_ACCENT2);
    fill(x0 - 8U, y1, x0, y1 + 8U, SPLASH_ACCENT2);
    fill(x1, y1, x1 + 8U, y1 + 8U, SPLASH_ACCENT2);

    {
        uint32_t gw = 8U * gs;
        uint32_t gh = 8U * gs;
        uint32_t gx = cx - gw / 2U;
        uint32_t gy = cy - gh / 2U;
        fb_char_scaled(gx, gy, 'A', SPLASH_ACCENT, SPLASH_BG_BOT, gs);
    }
}

static void progress_bar(uint32_t pct)
{
    uint32_t bar_w = (fb_w > 640U) ? 560U : (fb_w - 80U);
    uint32_t bar_h = 6U;
    uint32_t x = (fb_w - bar_w) / 2U;
    uint32_t y = (fb_h * 3U) / 4U + 24U;
    char pbuf[8];

    fill(x, y, x + bar_w, y + bar_h, SPLASH_BAR_BG);
    if (pct > 0U) {
        fill(x, y, x + (bar_w * pct) / 100U, y + bar_h, SPLASH_ACCENT2);
    }

    if (pct >= 100U) {
        pbuf[0] = '1'; pbuf[1] = '0'; pbuf[2] = '0'; pbuf[3] = '%'; pbuf[4] = '\0';
    } else if (pct >= 10U) {
        pbuf[0] = (char)('0' + (pct / 10U) % 10U);
        pbuf[1] = (char)('0' + pct % 10U);
        pbuf[2] = '%'; pbuf[3] = '\0';
    } else {
        pbuf[0] = (char)('0' + pct);
        pbuf[1] = '%'; pbuf[2] = '\0';
    }

    {
        uint32_t tw = 3U * 8U + 2U * 2U;
        uint32_t tx = (fb_w - tw) / 2U;
        uint32_t ty = y + 16U;
        fill(tx - 4U, ty, tx + tw + 4U, ty + 8U, SPLASH_BG_BOT);
        text_center(ty, pbuf, 1U, 2U, SPLASH_SUB);
    }
}

static void delay_ticks(uint64_t cycles)
{
    uint32_t lo, hi;
    uint64_t start, now;

    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    start = ((uint64_t)hi << 32) | lo;
    for (uint32_t guard = 0; guard < 200000000U; guard++) {
        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
        now = ((uint64_t)hi << 32) | lo;
        if (now - start >= cycles) break;
        __asm__ volatile ("pause");
    }
}

static void splash_serial(const char *s)
{
    console_serial_line(s);
}

void boot_splash(void)
{
    uint32_t w = fb_w != 0U ? fb_w : 1024U;
    uint32_t h = fb_h != 0U ? fb_h : 768U;
    uint32_t cx = w / 2U;
    uint32_t cy = h / 2U;
    uint32_t bands = 32U;

    /* 1) 竖向渐变背景 */
    for (uint32_t i = 0; i < bands; i++) {
        uint32_t y0 = (h * i) / bands;
        uint32_t y1 = (h * (i + 1U)) / bands;
        fill(0U, y0, w, y1, lerp_color(SPLASH_BG_TOP, SPLASH_BG_BOT, i, bands - 1U));
    }

    /* 2) 点阵网格（低对比，增加质感） */
    for (uint32_t y = 24U; y < h; y += 48U) {
        for (uint32_t x = 24U; x < w; x += 48U) {
            fill(x, y, x + 2U, y + 2U, SPLASH_DOT);
        }
    }

    /* 3) 顶部品牌条与底部细线 */
    fill(0U, 0U, w, 2U, SPLASH_ACCENT);
    fill(0U, h - 2U, w, h, SPLASH_LINE);

    /* 4) 徽标 + 字标 */
    badge(cx, cy - 150U, 132U);
    text_center(cy - 44U, "ASTER", 6U, 6U, SPLASH_TEXT);
    text_center(cy + 24U, OS_NAME "  v" OS_VERSION, 2U, 3U, SPLASH_SUB);
    text_center(cy + 52U, "a small native operating system", 1U, 2U, SPLASH_SUB);

    /* 底部信息 */
    fb_text(24U, h - 22U, "32-bit protected mode", SPLASH_SUB, SPLASH_BG_BOT);
    {
        const char *r = "ASTER  x86 OS";
        uint32_t rw = text_width(r, 1U, 1U);
        fb_text(w - rw - 24U, h - 22U, r, SPLASH_SUB, SPLASH_BG_BOT);
    }

    /* 5) 分隔线 */
    fill(cx - 280U, (h * 3U) / 4U, cx + 280U, (h * 3U) / 4U + 1U, SPLASH_LINE);

    /* 6) 进度条动画 0 -> 100 */
    splash_serial("boot: splash drawn\n");
    for (uint32_t pct = 0U; pct <= 100U; pct += 10U) {
        progress_bar(pct);
        delay_ticks(9000000ULL);
    }

    splash_serial("boot: splash end\n");
}
