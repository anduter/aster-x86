/*
 * ng WP2-6: 用户态 shell 的内核接线（L0 -> L3）。
 *
 * 内核只做三件事：
 *   1. 把 build-ng.ps1 嵌进 .rodata 的 shell ELF（ng_shell_elf_start..end）
 *      用 WP1-7 的 ELF64 加载器装进一个新的用户地址空间；
 *   2. 映射 16 KiB 用户栈，切 CR3，用 syscall_run_user() 进 ring3 跑 shell；
 *   3. shell 退出（syscall 1）后切回内核地址空间，检查退出码/用户栈/写次数，
 *      并读取 shell 会话留下的 /tmp/s.txt 与 pkg 统计作为端到端证据。
 *
 * shell 自己通过 syscall 0/20..27/30..35 访问串口、VFS、auth/log/pkg；本文件
 * 不做任何命令解析。默认启动顺序是：所有 WP 自检 -> WP1-7 ring3 hello ->
 * WP2-6 shell（交互）；`build-ng.ps1 -Smoke` 编出的内核跳过 shell，只保留
 * hello 冒烟路径（run-ng -Smoke）。
 */
#ifndef NG_WP26_H
#define NG_WP26_H

#include "types.h"

/*
 * 加载并运行 shell，等它退出；返回失败项数（0 = PASS）。
 * 证据行：
 *   ng: shell elf ok (size=... entry=0x... segs=...)
 *   ng: shell load ok (base=0x... end=0x... pages=...)
 *   ng: shell launch ok
 *   <用户态> ng: shell ready / ng: shell <cmd> ok ...
 *   ng: shell exit (code=0 syscalls=... writes=... fs=...)
 *   ng: shell session file ok (s.txt=3)        （脚本会话才有）
 *   ng: shell session pkg ok (lists=+1)        （脚本会话才有）
 *   ng: WP2-6 selftest PASS                    （main.c 补）
 */
int wp26_selftest(void);

#endif /* NG_WP26_H */
