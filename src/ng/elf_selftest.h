/*
 * ng WP1-7: ELF64 加载器 + ring3 hello 内核自检。
 */
#ifndef NG_ELF_SELFTEST_H
#define NG_ELF_SELFTEST_H

/*
 * 依次做：
 *   1. 校验内嵌的 hello ELF（ELF64/ET_EXEC/EM_X86_64/PT_LOAD 边界）；
 *   2. 对 22 份变异镜像逐条验证拒绝；
 *   3. 建用户地址空间、逐段映射（RX / R+NX / RW+NX，.bss 清零）；
 *   4. 16 KiB 用户栈 + 守护页；
 *   5. per-thread 内核栈（TSS.RSP0 / syscall 入口）跨线程验证；
 *   6. copyin/copyout（好指针成功、null/内核地址/只读页 EFAULT）；
 *   7. 切 CR3、iretq 进 CPL3 跑 hello（write + exit），切回内核表并销毁 AS。
 * 返回失败项数；出口行 `ng: WP1-7 selftest PASS` 由 main.c 打。
 */
int elf_selftest(void);

#endif /* NG_ELF_SELFTEST_H */
