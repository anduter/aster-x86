/*
 * ng WP2-5: auth/log/pkg 自检入口（L0 接线）。
 */
#ifndef NG_WP25_H
#define NG_WP25_H

/* 跑 auth 用户/登录/拒绝、log 环、31..35 用户指针、pkg 列表/校验自检。
 * 返回失败项数（0 = 全通过）；证据行由本模块打印。 */
int wp25_selftest(void);

#endif /* NG_WP25_H */
