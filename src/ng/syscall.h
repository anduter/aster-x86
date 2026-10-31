/*
 * ng WP1-3: syscall（STAR/LSTAR/FMASK）与 ring0/ring3 往返测试。
 */
#ifndef NG_SYSCALL_H
#define NG_SYSCALL_H

/* 置 EFER.SCE 与 STAR/LSTAR/FMASK，打印读回值。 */
void syscall_init(void);

/* ring0 syscall 往返 + ring3 syscall 往返；返回失败项数。 */
int syscall_selftest(void);

#endif /* NG_SYSCALL_H */
