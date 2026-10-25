/* =============================================================================
 * core/splash.c - framebuffer 启动画面
 *
 * 深色渐变背景 + 点阵网格 + 铁框木板招牌（四颗钉子）+ 彩色系统名 + 动画进度条。
 * 约 1.2 秒后交给控制台。
 * ============================================================================= */
#include "splash.h"
#include "include/dev.h"
#include "include/kernel.h"

/* ---- 背景 ---- */
#define BG_TOP      0x0A0E14U
#define BG_BOT      0x131C28U
#define BG_DOT      0x16202CU

/* ---- 铁框 ---- */
#define IRON        0x3B4048U
#define IRON_HI     0x777F8AU
#define IRON_LO     0x1B1F24U
#define IRON_LIP    0x15181CU

/* ---- 木板 ---- */
#define WOOD_L      0x6B4728U
#define WOOD_D      0x5A3B22U
#define WOOD_SEAM   0x3E2814U
#define WOOD_GRAIN  0x4A2F1AU

/* ---- 文字 / 进度条 ---- */
#define SUB         0x8B949EU
#define TEXT        0xE6EDF3U
#define BAR_BG      0x1C2430U
#define BAR_FG      0x22D3EEU

static const uint32_t palette[5] = {
    0xFF5C5CU,  /* A */
    0xFFB020U,  /* S */
    0xFFE066U,  /* T */
    0x4ADE80U,  /* E */
    0x4C8DFFU   /* R */
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

static uint32_t bg_at(uint32_t y)
{
    uint32_t bands = 32U;
    uint32_t hh = (fb_h != 0U) ? (uint32_t)fb_h : 1024U;
    uint32_t i = (y * bands) / hh;
    if (i >= bands) i = bands - 1U;
    return lerp_color(BG_TOP, BG_BOT, i, bands - 1U);
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

    (void)bg;
    for (uint32_t i = 0; s[i] != '\0'; i++) {
        fb_glyph(x + i * (cw + gap), y, s[i], fg, scale);
    }
}

/* 木纹：交替色板条 + 接缝 + 竖向纹理 */
static void wood_panel(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    uint32_t plank = 32U;

    for (uint32_t y = y0; y < y1; y += plank) {
        uint32_t y2 = (y + plank < y1) ? y + plank : y1;
        uint32_t shade = (((y - y0) / plank) & 1U) ? WOOD_D : WOOD_L;
        fill(x0, y, x1, y2, shade);
        /* 板缝 */
        fill(x0, y2 - 2U < y1 ? y2 - 2U : y, x1, y2, WOOD_SEAM);
    }

    /* 竖向木纹（确定性分布，避免随机数） */
    for (uint32_t x = x0 + 12U; x < x1 - 6U; x += 37U) {
        uint32_t gy = y0 + 6U + ((x * 13U) % (y1 - y0 > 12U ? (y1 - y0 - 12U) : 1U));
        fill(x, gy, x + 2U, gy + 14U, WOOD_GRAIN);
        fill(x + 19U, y0 + 4U, x + 20U, y1 - 4U, WOOD_GRAIN);
    }
}

/* 铁钉：方头 + 高光 + 阴影 */
static void nail(uint32_t x, uint32_t y)
{
    fill(x, y, x + 10U, y + 10U, 0x59616BU);
    fill(x + 1U, y + 1U, x + 5U, y + 5U, 0x9AA3AEU);   /* 左上高光 */
    fill(x + 5U, y + 5U, x + 10U, y + 10U, 0x2A2F35U); /* 右下阴影 */
}

/* 铁框 + 木板 + 钉子 */
static void signboard(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    const uint32_t iron = 10U;

    /* 铁框主体 */
    fill(x0, y0, x1, y1, IRON);
    /* 上/左高光，下/右阴影 */
    fill(x0, y0, x1, y0 + 2U, IRON_HI);
    fill(x0, y0, x0 + 2U, y1, IRON_HI);
    fill(x0, y1 - 2U, x1, y1, IRON_LO);
    fill(x1 - 2U, y0, x1, y1, IRON_LO);

    /* 铁框内唇 */
    fill(x0 + iron - 2U, y0 + iron - 2U, x1 - iron + 2U, y0 + iron, IRON_LIP);
    fill(x0 + iron - 2U, y1 - iron, x1 - iron + 2U, y1 - iron + 2U, IRON_LIP);
    fill(x0 + iron - 2U, y0 + iron, x0 + iron, y1 - iron, IRON_LIP);
    fill(x1 - iron, y0 + iron, x1 - iron + 2U, y1 - iron, IRON_LIP);

    /* 木板 */
    wood_panel(x0 + iron, y0 + iron, x1 - iron, y1 - iron);

    /* 四颗钉子 */
    nail(x0 + 16U, y0 + 16U);
    nail(x1 - 26U, y0 + 16U);
    nail(x0 + 16U, y1 - 26U);
    nail(x1 - 26U, y1 - 26U);
}

/* 彩虹系统名（只画前景，木纹透出，无阴影块） */
static void rainbow_title(uint32_t cx, uint32_t cy, uint32_t scale, uint32_t gap)
{
    const char *s = "ASTER";
    uint32_t w = text_width(s, scale, gap);
    uint32_t x = cx - w / 2U;
    uint32_t y = cy - (8U * scale) / 2U;
    uint32_t cw = 8U * scale;

    for (uint32_t i = 0; s[i] != '\0'; i++) {
        fb_glyph(x + i * (cw + gap), y, s[i],
                 palette[i % (uint32_t)(sizeof(palette) / sizeof(palette[0]))], scale);
    }
}

static void progress_bar(uint32_t pct)
{
    uint32_t bar_w = (fb_w > 640U) ? 560U : (fb_w - 80U);
    uint32_t bar_h = 8U;
    uint32_t x = (fb_w - bar_w) / 2U;
    uint32_t y = (fb_h * 3U) / 4U + 24U;
    char pbuf[8];

    fill(x, y, x + bar_w, y + bar_h, BAR_BG);
    if (pct > 0U) {
        fill(x, y, x + (bar_w * pct) / 100U, y + bar_h, BAR_FG);
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
        fill(tx - 6U, ty - 2U, tx + tw + 6U, ty + 12U, bg_at(ty));
        text_center(ty, pbuf, 1U, 2U, SUB, BG_BOT);
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

    /* 招牌尺寸（比上一版小一圈） */
    uint32_t sign_w = (w > 660U) ? 560U : (w - 100U);
    uint32_t sign_h = 170U;
    uint32_t sign_x0 = (w - sign_w) / 2U;
    uint32_t sign_y0 = (h / 2U) - 210U;
    uint32_t sign_x1 = sign_x0 + sign_w;
    uint32_t sign_y1 = sign_y0 + sign_h;

    /* 1) 竖向渐变背景 */
    for (uint32_t i = 0; i < bands; i++) {
        uint32_t y0 = (h * i) / bands;
        uint32_t y1 = (h * (i + 1U)) / bands;
        fill(0U, y0, w, y1, lerp_color(BG_TOP, BG_BOT, i, bands - 1U));
    }

    /* 2) 点阵网格 */
    for (uint32_t y = 24U; y < h; y += 48U) {
        for (uint32_t x = 24U; x < w; x += 48U) {
            fill(x, y, x + 2U, y + 2U, BG_DOT);
        }
    }

    /* 3) 顶部品牌条 / 底部细线 */
    fill(0U, 0U, w, 2U, 0x4C8DFFU);
    fill(0U, h - 2U, w, h, 0x243040U);

    /* 4) 招牌：铁框 + 木板 + 钉子，名字居中 */
    signboard(sign_x0, sign_y0, sign_x1, sign_y1);
    rainbow_title((sign_x0 + sign_x1) / 2U, (sign_y0 + sign_y1) / 2U, 5U, 8U);

    /* 5) 招牌下方版本与标语 */
    text_center(sign_y1 + 28U, OS_NAME "  v" OS_VERSION, 2U, 3U, TEXT, BG_BOT);
    text_center(sign_y1 + 58U, "a small native operating system", 1U, 2U, SUB, BG_BOT);

    /* 6) 页脚 */
    {
        const char *l = "32-bit protected mode";
        for (uint32_t i = 0; l[i] != '\0'; i++) {
            fb_glyph(24U + i * 8U, h - 22U, l[i], SUB, 1U);
        }
    }
    {
        const char *r = "ASTER  x86 OS";
        uint32_t rw = text_width(r, 1U, 1U);
        for (uint32_t i = 0; r[i] != '\0'; i++) {
            fb_glyph(w - rw - 24U + i * 9U, h - 22U, r[i], SUB, 1U);
        }
    }

    /* 7) 分隔线 + 进度条动画（每步约 20-30ms，整体约 1.2-1.6s） */
    fill((w - 560U) / 2U, (h * 3U) / 4U, (w + 560U) / 2U, (h * 3U) / 4U + 1U, 0x243040U);
    splash_serial("boot: splash drawn\n");
    for (uint32_t pct = 0U; pct <= 100U; pct += 2U) {
        progress_bar(pct);
        delay_ticks(130000000ULL);
    }

    splash_serial("boot: splash end\n");
}
