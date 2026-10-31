/*
 * ng WP2-1: VFS/ramfs 内核自检入口。
 *
 * 返回失败项数（0 = PASS）；main.c 负责打印 "ng: WP2-1 selftest PASS/FAIL"。
 * 自检会建根 ramfs（挂 "/"）并建 /tmp，ring3 demo 直接用它。
 */
#ifndef NG_FS_SELFTEST_H
#define NG_FS_SELFTEST_H

int fs_selftest(void);

#endif /* NG_FS_SELFTEST_H */
