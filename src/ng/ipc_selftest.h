/*
 * ng WP1-6: IPC/句柄/端口内核自检。
 */
#ifndef NG_IPC_SELFTEST_H
#define NG_IPC_SELFTEST_H

/* 跑完 ping/pong、队列满/超时/坏句柄/close 唤醒/用户指针等检查。
 * 返回失败项数（0 = 全过）；出口行见 docs/claims/ng-WP1-6.md。 */
int ipc_selftest(void);

#endif /* NG_IPC_SELFTEST_H */
