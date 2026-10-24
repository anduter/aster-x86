/* =============================================================================
 * console.h - 屏幕文字控制台（framebuffer 后端，镜像到 COM1）
 *
 * 布局：顶部标题栏、底部状态栏、中间内容区。内容区是一个 cols x rows 的
 * 字符网格，支持换行/回车/制表/退格/上卷。所有 console 输出同时镜像到串口，
 * 所以 make run-headless 抓到的串口日志和屏幕上的内容一致。
 * ============================================================================= */
#ifndef CONSOLE_H
#define CONSOLE_H

#include "../../types.h"

/* 初始化：按当前分辨率计算网格并画出标题栏/状态栏/边框 */
void console_init(void);

/* 清空内容区并把光标归位 */
void console_clear(void);

/* 输出一个字符 / 一串字符（'\n' 会自动换行并上卷） */
void console_putc(char c);
void console_write(const char *s);
/* 关中断写完整个字符串，避免内核日志被进程切换/中断输出撕裂。 */
void console_write_atomic(const char *s);

/* 设置文字前景/背景色（0x00RRGGBB） */
void console_set_color(uint32_t fg, uint32_t bg);

/* 输入区：在底部输入框里画一行（提示符 + 正在输入的内容），不换行不滚动 */
void console_input_set(const char *text);

/* 是否把输出同时写到串口（默认开启） */
void console_set_mirror(bool enabled);

/* 标题栏 / 状态栏文字（每次调用会先清空该栏） */
void console_set_title(const char *text);
void console_set_status(const char *text);
/* 状态栏右侧文字（右对齐），用于心跳指示这类小部件 */
void console_set_status_right(const char *text);

/* 当前网格尺寸，供 shell 等上层使用 */
uint32_t console_cols(void);
uint32_t console_rows(void);

#endif /* CONSOLE_H */
