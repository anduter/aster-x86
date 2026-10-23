/* =============================================================================
 * panic.c - 内核致命错误处理
 *
 * 显示：屏幕中央一块白底面板 + 红色边框，标题红色、正文深色；
 *       同时把纯文本原因写到 COM1，保证自动测试能抓到。
 * 之后关中断并永久停机。
 * ============================================================================= */
#include "panic.h"
#include "fb.h"
#include "io.h"
#include "lib/kprintf.h"
#include "lib/string.h"

/* 面板配色与尺寸 */
#define PANIC_BORDER_COLOR 0xD00000U    /* 红色边框 */
#define PANIC_TITLE_COLOR  0xD00000U    /* 红色标题 */
#define PANIC_TEXT_COLOR   0x202020U    /* 深灰正文 */
#define PANIC_BG_COLOR     0xFFFFFFU    /* 白色底 */
#define PANIC_BORDER       4U           /* 边框粗细 */
#define PANIC_PAD          16U          /* 内边距 */
#define PANIC_LINE_H       16U          /* 行高 */
#define PANIC_TEXT_MAX     640U         /* 原因文本最大长度（要放得下完整寄存器现场） */

/* 画白底红框面板，并把多行文本排进去 */
static void draw_panic_panel(const char *text)
{
    uint32_t panel_w = 760U;
    uint32_t panel_h = 200U;
    uint32_t x0, y0, x1, y1;
    uint32_t y;
    char     line[128];
    uint32_t n = 0U;

    /* 没有可用的 framebuffer 时只保留串口输出 */
    if (fb_addr == 0U || fb_w == 0U || fb_h == 0U) {
        return;
    }

    if (panel_w + 40U > (uint32_t)fb_w) panel_w = (uint32_t)fb_w - 40U;
    if (panel_h + 40U > (uint32_t)fb_h) panel_h = (uint32_t)fb_h - 40U;

    x0 = ((uint32_t)fb_w - panel_w) / 2U;
    y0 = ((uint32_t)fb_h - panel_h) / 2U;
    x1 = x0 + panel_w;
    y1 = y0 + panel_h;

    /* 白底 */
    fb_fill_rect(x0, y0, x1, y1, PANIC_BG_COLOR);

    /* 红色边框 */
    fb_fill_rect(x0, y0, x1, y0 + PANIC_BORDER, PANIC_BORDER_COLOR);
    fb_fill_rect(x0, y1 - PANIC_BORDER, x1, y1, PANIC_BORDER_COLOR);
    fb_fill_rect(x0, y0, x0 + PANIC_BORDER, y1, PANIC_BORDER_COLOR);
    fb_fill_rect(x1 - PANIC_BORDER, y0, x1, y1, PANIC_BORDER_COLOR);

    /* 标题 */
    fb_text(x0 + PANIC_PAD, y0 + PANIC_PAD, "*** KERNEL PANIC ***",
            PANIC_TITLE_COLOR, PANIC_BG_COLOR);

    /* 正文：按 '\n' 逐行排版 */
    y = y0 + PANIC_PAD + PANIC_LINE_H + 8U;
    for (const char *p = text; ; p++) {
        if (*p == '\n' || *p == '\0') {
            line[n] = '\0';
            if (n > 0U) {
                fb_text(x0 + PANIC_PAD, y, line, PANIC_TEXT_COLOR, PANIC_BG_COLOR);
                y += PANIC_LINE_H;
            }
            n = 0U;
            if (*p == '\0' || y + PANIC_LINE_H > y1 - PANIC_PAD) {
                break;
            }
        } else if (n + 1U < sizeof(line)) {
            line[n++] = *p;
        }
    }
}

void panic(const char *fmt, ...)
{
    char   buf[PANIC_TEXT_MAX];
    uint32_t esp = 0;
    uint32_t n;
    va_list ap;

    __asm__ volatile("cli");
    __asm__ volatile("mov %%esp, %0" : "=r"(esp));

    /* 1. 格式化原因（屏幕和串口共用同一份文本） */
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    /* 2. 串口：纯文本 + 栈内存转储，便于离线分析 */
    serial_puts("\r\n*** KERNEL PANIC ***\r\n");
    serial_puts(buf);
    serial_puts("\r\nstack:\r\n");
    for (uint32_t i = 0; i < 16U; i++) {
        uint32_t w = ((const uint32_t *)(uintptr_t)esp)[i];
        serial_puthex32(w);
        serial_putc((i % 8U == 7U) ? '\n' : ' ');
    }
    serial_puts("*** system halted ***\r\n");

    /* 屏幕面板：追加两行栈摘要 */
    n = strlen(buf);
    if (n + 40U < sizeof(buf)) {
        char extra[64];
        ksnprintf(extra, sizeof(extra),
                  "\nstack: %08x %08x %08x %08x",
                  ((const uint32_t *)(uintptr_t)esp)[0],
                  ((const uint32_t *)(uintptr_t)esp)[1],
                  ((const uint32_t *)(uintptr_t)esp)[2],
                  ((const uint32_t *)(uintptr_t)esp)[3]);
        strncpy(buf + n, extra, sizeof(buf) - n - 1U);
        buf[sizeof(buf) - 1U] = '\0';
    }
    draw_panic_panel(buf);

    /* 3. 永久停机（要自动重启请用 SYS_REBOOT 或 panic_set_reboot） */
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

/* 机器复位：写 8042 的 0xFE 命令（QEMU/大多数 PC 都支持） */
void machine_reboot(void)
{
    __asm__ volatile("cli");

    while ((inb(0x64) & 0x02U) != 0U) {
        /* 等键盘控制器输入缓冲空 */
    }
    outb(0x64, 0xFEU);

    for (;;) {
        __asm__ volatile("hlt");
    }
}
