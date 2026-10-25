/* =============================================================================
 * core/splash.c - framebuffer 启动画面
 *
 * 在 fb_init() 之后、console_init() 之前绘制：深色渐变背景 + 点阵网格 +
 * 招牌式边框（彩色系统名）+ 动画进度条。约 1.2-1.6 秒后交给控制台。
 * ============================================================================= */
#include "splash.h"
#include "include/dev.h"
#include "include/kernel.h"

/* ---- 调色板 ---- */
#define SPLASH_BG_TOP    0x0A0E14U
#define SPLASH_BG_BOT    0x131C28U
#define SPLASH_DOT       0x16202CU
#define SPLASH_SIGN_BG   0x0D1622U
#define SPLASH_SIGN_EDGE 0x4C8DFFU
#define SPLASH_SIGN_IN   0x1E3A5FU
#define SPLASH_SUB       0x8B949EU
#define SPLASH_BAR_BG    0x1C2430U
#define SPLASH_BAR_FG    0x22D3EEU
#define SPLASH_TEXT      0xE6EDF3U

static const uint32_t sign_palette[5] = {
    0xFF5C5CU,  /* A - red    */
    0xFFB020U,  /* S - amber  */
    0xFFE066U,  /* T - yellow */
    0x4ADE80U,  /* E - green  */
    0x4C8DFFU   /* R - blue   */
};

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

static void text_center(uint32_t y, const char *s, uint32_t scale, uint32_t gap,
                        uint32_t fg, uint32_t bg)
{
    uint32_t w = text_width(s, scale, gap);
    uint32_t x = (fb_w > w) ? (fb_w - w) / 2U : 0U;
    uint32_t cw = 8U * scale;

    for (uint32_t i = 0; s[i] != '\0'; i++) {
        fb_char_scaled(x + i * (cw + gap), y, s[i], fg, bg, scale);
    }
}

/* 彩虹字：每个字母取调色板中的一种颜色 */
static void text_rainbow(uint32_t y, const char *s, uint32_t scale, uint32_t gap, uint32_t bg)
{
    uint32_t w = text_width(s, scale, gap);
    uint32_t x = (fb_w > w) ? (fb_w - w) / 2U : 0U;
    uint32_t cw = 8U * scale;

    for (uint32_t i = 0; s[i] != '\0'; i++) {
        uint32_t c = sign_palette[i % (uint32_t)(sizeof(sign_palette) / sizeof(sign_palette[0]))];
        fb_char_scaled(x + i * (cw + gap), y, s[i], c, bg, scale);
    }
}

/* 招牌：双层边框 + 面板 + 四角铆钉 */
static void signboard(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    fill(x0, y0, x1, y1, SPLASH_SIGN_BG);

    fill(x0, y0, x1, y0 + 3U, SPLASH_SIGN_EDGE);
    fill(x0, y1 - 3U, x1, y1, SPLASH_SIGN_EDGE);
    fill(x0, y0, x0 + 3U, y1, SPLASH_SIGN_EDGE);
    fill(x1 - 3U, y0, x1, y1, SPLASH_SIGN_EDGE);

    fill(x0 + 6U, y0 + 6U, x1 - 6U, y0 + 7U, SPLASH_SIGN_IN);
    fill(x0 + 6U, y1 - 7U, x1 - 6U, y1 - 6U, SPLASH_SIGN_IN);
    fill(x0 + 6U, y0 + 6U, x0 + 7U, y1 - 6U, SPLASH_SIGN_IN);
    fill(x1 - 7U, y0 + 6U, x1 - 6U, y1 - 6U, SPLASH_SIGN_IN);

    fill(x0 + 14U, y0 + 14U, x0 + 20U, y0 + 20U, SPLASH_BAR_FG);
    fill(x1 - 20U, y0 + 14U, x1 - 14U, y0 + 20U, SPLASH_BAR_FG);
    fill(x0 + 14U, y1 - 20U, x0 + 20U, y1 - 14U, SPLASH_BAR_FG);
    fill(x1 - 20U, y1 - 20U, x1 - 14U, y1 - 14U, SPLASH_BAR_FG);
}

static void progress_bar(uint32_t pct)
{
    uint32_t bar_w = (fb_w > 640U) ? 560U : (fb_w - 80U);
    uint32_t bar_h = 8U;
    uint32_t x = (fb_w - bar_w) / 2U;
    uint32_t y = (fb_h * 3U) / 4U + 24U;
    char pbuf[8];

    fill(x, y, x + bar_w, y + bar_h, SPLASH_BAR_BG);
    if (pct > 0U) {
        fill(x, y, x + (bar_w * pct) / 100U, y + bar_h, SPLASH_BAR_FG);
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
        uint32_t tw = text_width(pbuf, 1U, 2U);
        uint32_t tx = (fb_w - tw) / 2U;
        uint32_t ty = y + 20U;
        fill(tx - 6U, ty - 2U, tx + tw + 6U, ty + 12U, SPLASH_BG_BOT);
        text_center(ty, pbuf, 1U, 2U, SPLASH_SUB, SPLASH_BG_BOT);
    }
}

static void delay_ticks(uint64_t cycles)
{
    uint32_t lo, hi;
    uint64_t start, now;

    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    start = ((uint64_t)hi << 32) | lo;
    for (uint32_t guard = 0; guard < 400000000U; guard++) {
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
    uint32_t bands = 32U;
    uint32_t sign_w = (w > 840U) ? 760U : (w - 100U);
    uint32_t sign_h = 230U;
    uint32_t sign_x0 = (w - sign_w) / 2U;
    uint32_t sign_y0 = (h / 2U) - 300U;
    uint32_t sign_x1 = sign_x0 + sign_w;
    uint32_t sign_y1 = sign_y0 + sign_h;

    /* 1) 竖向渐变背景 */
    for (uint32_t i = 0; i < bands; i++) {
        uint32_t y0 = (h * i) / bands;
        uint32_t y1 = (h * (i + 1U)) / bands;
        fill(0U, y0, w, y1, lerp_color(SPLASH_BG_TOP, SPLASH_BG_BOT, i, bands - 1U));
    }

    /* 2) 点阵网格 */
    for (uint32_t y = 24U; y < h; y += 48U) {
        for (uint32_t x = 24U; x < w; x += 48U) {
            fill(x, y, x + 2U, y + 2U, SPLASH_DOT);
        }
    }

    /* 3) 顶部品牌条 / 底部细线 */
    fill(0U, 0U, w, 2U, SPLASH_SIGN_EDGE);
    fill(0U, h - 2U, w, h, 0x243040U);

    /* 4) 招牌 + 彩色系统名 */
    signboard(sign_x0, sign_y0, sign_x1, sign_y1);
    text_rainbow(sign_y0 + 70U, "ASTER", 7U, 10U, SPLASH_SIGN_BG);

    /* 5) 招牌下方的版本与标语 */
    text_center(sign_y1 + 36U, OS_NAME "  v" OS_VERSION, 2U, 3U, SPLASH_TEXT, SPLASH_BG_BOT);
    text_center(sign_y1 + 70U, "a small native operating system", 1U, 2U, SPLASH_SUB, SPLASH_BG_BOT);

    /* 6) 底部信息 */
    fb_text(24U, h - 22U, "32-bit protected mode", SPLASH_SUB, SPLASH_BG_BOT);
    {
        const char *r = "ASTER  x86 OS";
        uint32_t rw = text_width(r, 1U, 1U);
        fb_text(w - rw - 24U, h - 22U, r, SPLASH_SUB, SPLASH_BG_BOT);
    }

    /* 7) 分隔线 + 进度条动画（每步约 30-40ms，整体约 1.2-1.6s） */
    fill((w - 560U) / 2U, (h * 3U) / 4U, (w + 560U) / 2U, (h * 3U) / 4U + 1U, 0x243040U);
    splash_serial("boot: splash drawn\n");
    for (uint32_t pct = 0U; pct <= 100U; pct += 2U) {
        progress_bar(pct);
        delay_ticks(130000000ULL);
    }

    splash_serial("boot: splash end\n");
}
