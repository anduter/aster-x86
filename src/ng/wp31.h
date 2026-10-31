/*
 * ng WP3-1: display server（L2a）内核自检编排（见 wp31.h）。
 *
 * 本文件只做"父进程"该做的事：spawn displayd -> 等发现文件 -> spawn demo ->
 * 等 demo 退出 -> 读 displayd 的状态文件 -> 杀 displayd 验证崩溃隔离。
 * 显示协议/合成/输入转发全在 ring3 的 displayd 与 demo 里，内核不知道
 * surface 长什么样（除了共享内存对象表和 LFB 映射这两条机制）。
 */
#ifndef NG_WP31_H
#define NG_WP31_H

#include "types.h"

/*
 * 跑 WP3-1 自检；返回失败项数（0 = 通过）。
 * 没有 LFB（-vga none 的 VGA 文本降级）时打印 skipped 并返回 0。
 */
int wp31_selftest(void);

#endif /* NG_WP31_H */
