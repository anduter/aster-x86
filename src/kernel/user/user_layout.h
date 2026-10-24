#ifndef USER_LAYOUT_H
#define USER_LAYOUT_H

/* =============================================================================
 * user_layout.h - 用户地址空间唯一布局来源
 *
 * 0x40000000 .. 0x40100000   代码/数据（ELF，最大 1MB）
 * 0x40100000 .. 0x40200000   用户栈（向下增长，底部 4KB 为 guard）
 * 0x40200000 .. 0x40300000   用户堆（brk 向上增长）
 * 0x40300000 .. 0x40400000   信号蹦床 / 预留页
 * 0x40400000 .. 0x40500000   匿名 mmap 区（每进程独立）
 *
 * fork 的 COW 范围覆盖到 0x40500000，确保栈、堆、蹦床、mmap 页都被复制。
 * ============================================================================= */

#define USER_LAYOUT_CODE_BASE     0x40000000U
#define USER_LAYOUT_CODE_END      0x40100000U
#define USER_LAYOUT_STACK_LIMIT   0x40100000U
#define USER_LAYOUT_STACK_TOP     0x40200000U
#define USER_LAYOUT_STACK_SIZE    4096U
#define USER_LAYOUT_STACK_INIT    (USER_LAYOUT_STACK_TOP - USER_LAYOUT_STACK_SIZE)
#define USER_LAYOUT_STACK_ASLR_SLOTS 128U /* 栈顶可在顶部 512KB 内随机 */
#define USER_LAYOUT_HEAP_BASE     0x40200000U
#define USER_LAYOUT_HEAP_END      0x40300000U
#define USER_LAYOUT_HEAP_ASLR_SLOTS 64U  /* 堆基址在 256KB 范围内随机 */
#define USER_LAYOUT_TRAMPOLINE    0x40300000U
#define USER_LAYOUT_TRAMPOLINE_ASLR_SLOTS 64U
#define USER_LAYOUT_MMAP_BASE     0x40400000U
#define USER_LAYOUT_MMAP_END      0x40500000U
#define USER_LAYOUT_CLONE_END     0x40500000U

/* 栈底 guard：低 4KB 永不映射，栈增长到这里就拒绝。 */
#define USER_LAYOUT_STACK_GUARD_END (USER_LAYOUT_STACK_LIMIT + 0x1000U)

#endif
