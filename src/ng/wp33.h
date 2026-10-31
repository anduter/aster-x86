/*
 * ng WP3-3: session manager 自检编排（见 wp33_selftest.c）。
 */
#ifndef NG_WP33_H
#define NG_WP33_H

/*
 * 跑 WP3-3 自检：spawn sessiond（sessiond 再 spawn displayd + wm），
 * 经 WM 的 LAUNCH 拉起应用、校验注册表/令牌，kill WM 验证应用存活 + WM
 * 重启，最后回收。返回失败项数（0 = PASS；没有 LFB 时打印 skipped 返回 0）。
 */
int wp33_selftest(void);

#endif /* NG_WP33_H */
