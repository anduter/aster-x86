/* =============================================================================
 * core/rtc.c - CMOS 实时时钟 + epoch 换算
 *
 * 读 CMOS 的秒/分/时/日/月/年（BCD 或二进制由寄存器 B 决定），
 * 再换算成 1970 起的秒数，给文件时间戳和 shell 的 date 命令用。
 * ============================================================================= */
#include "rtc.h"
#include "../io.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static int bcd_to_bin(uint8_t v)
{
    return (int)(v & 0x0FU) + ((int)(v >> 4) * 10);
}

/* 民用日期 -> 1970 起的天数（Howard Hinnant 的 days_from_civil） */
static int days_from_civil(int y, int m, int d)
{
    y -= (m <= 2) ? 1 : 0;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;                    /* [0, 399] */
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

    return era * 146097 + doe - 719468;
}

uint32_t rtc_now(void)
{
    int sec, min, hour, day, month, year;
    uint8_t regb;
    int binary, is_pm;
    int guard = 200000;

    /* 等更新周期结束，避免读到一半的时间 */
    while ((cmos_read(0x0A) & 0x80U) != 0U && guard-- > 0) {
        /* busy wait */
    }

    sec   = cmos_read(0x00);
    min   = cmos_read(0x02);
    hour  = cmos_read(0x04);
    day   = cmos_read(0x07);
    month = cmos_read(0x08);
    year  = cmos_read(0x09);
    regb  = cmos_read(0x0B);

    binary = (regb & 0x04U) != 0U;      /* 1 = 二进制，0 = BCD */
    if (!binary) {
        sec   = bcd_to_bin((uint8_t)sec);
        min   = bcd_to_bin((uint8_t)min);
        hour  = bcd_to_bin((uint8_t)hour);
        day   = bcd_to_bin((uint8_t)day);
        month = bcd_to_bin((uint8_t)month);
        year  = bcd_to_bin((uint8_t)year);
    }

    if ((regb & 0x02U) == 0U) {         /* 12 小时制 */
        is_pm = (hour & 0x80) != 0;
        hour &= 0x7F;
        if (is_pm && hour < 12) hour += 12;
        if (!is_pm && hour == 12) hour = 0;
    }

    year += (year < 70) ? 2000 : 1900;

    if (month < 1 || month > 12 || day < 1 || day > 31) {
        return 0;                       /* CMOS 不可信 */
    }

    int days = days_from_civil(year, month, day);
    return (uint32_t)(days * 86400 + hour * 3600 + min * 60 + sec);
}

int rtc_format(uint32_t epoch, char *buf, uint32_t max)
{
    uint32_t days = epoch / 86400U;
    uint32_t rem = epoch % 86400U;
    int hour = (int)(rem / 3600U);
    int min = (int)((rem % 3600U) / 60U);
    int sec = (int)(rem % 60U);
    int z, era, doe, yoe, y, doy, mp, d, m;
    int n = 0;

    if (buf == 0 || max < 21U) return 0;

    /* civil_from_days */
    z = (int)days + 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2) ? 1 : 0;

    /* 手写格式化，避免依赖 printf */
    for (int sh = 1000; sh >= 1; sh /= 10) buf[n++] = (char)('0' + (y / sh) % 10);
    buf[n++] = '-';
    buf[n++] = (char)('0' + m / 10); buf[n++] = (char)('0' + m % 10);
    buf[n++] = '-';
    buf[n++] = (char)('0' + d / 10); buf[n++] = (char)('0' + d % 10);
    buf[n++] = ' ';
    buf[n++] = (char)('0' + hour / 10); buf[n++] = (char)('0' + hour % 10);
    buf[n++] = ':';
    buf[n++] = (char)('0' + min / 10); buf[n++] = (char)('0' + min % 10);
    buf[n++] = ':';
    buf[n++] = (char)('0' + sec / 10); buf[n++] = (char)('0' + sec % 10);
    buf[n] = '\0';
    return n;
}
