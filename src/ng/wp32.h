/*
 * ng WP3-2: shell/WM（L2b）自检入口。
 *
 * 依赖 WP3-1 的 display server（/bin/displayd）、WP3-0 的进程/spawn/IPC、
 * WP2-3 的 VBE/LFB/PS2；没有 LFB（-vga none 降级）时打印 skipped 并跳过。
 */
#ifndef NG_WP32_H
#define NG_WP32_H

/* 返回失败数（0 = PASS）；没有显示设备时返回 0（skipped）。 */
int wp32_selftest(void);

#endif /* NG_WP32_H */
