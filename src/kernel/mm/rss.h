#ifndef MM_RSS_H
#define MM_RSS_H

#include "../include/types.h"

/* =============================================================================
 * rss.h - per-process RSS（常驻用户页）统计
 *
 * 口径：RSS = 进程地址空间“用户布局区” [USER_LAYOUT_CODE_BASE,
 * USER_LAYOUT_CLONE_END) 内 PRESENT|USER 的 4KB 页数 × 4KB。包含代码/数据、
 * 栈、堆、信号蹦床、匿名 mmap 与文件映射页。
 *
 *   - fork 之后父子共享的 COW 页在每个进程各计一次（与 Linux RSS 一致，
 *     共享页不会被“只算一份”）；fork 本身不改变父进程页数。
 *   - 不计内核栈、页表页、内核高半区映射。
 *   - 计数直接扫描页表，不做增量记账：进程退出（proc_reap → vmm_destroy_dir）、
 *     munmap、COW 复制（旧页换新页，页数不变）都会自动反映到下一次查询，
 *     不存在“忘了扣减”的悬挂计数。
 *   - 只读代码页/只读文件映射页也都算常驻（在页表里就是常驻）。
 *
 * 这是诊断接口、非原子：若另一个 CPU 正在 reap，扫描可能看到过渡状态。
 * vmm_user_page_count() 对页目录/页表物理地址做 RAM 范围校验，坏指针按 0 页
 * 处理，不会因为读毒化内存而内核 #PF。
 * ============================================================================= */

struct process;                 /* proc_t；避免 mm 公共头反向依赖 proc.h */

/* 单个进程的 RSS（用户页数 / 字节数）；p==0、PROC_FREE 或无页目录返回 0。 */
uint32_t mm_rss_pages(const struct process *p);
uint32_t mm_rss_bytes(const struct process *p);

/* 所有活动进程 RSS 之和（用户页数）。 */
uint32_t mm_rss_total_pages(void);

/* 打印一行 ps 风格：mm: ps pid=.. name=.. state=.. rss_pages=.. rss_kb=.. */
void mm_rss_print(const struct process *p);

/* 遍历进程表打印所有活动进程；返回打印的行数（没有活动进程时打印 0 行）。 */
uint32_t mm_rss_dump(void);

/* 自检：页表计数、fork 共享页各计一次、munmap 扣减、退出归零、PMM 账目复原。
 * 成功打印 "mm: rss accounting ok ..." 返回 1；失败打印 FAIL 返回 0。 */
int mm_rss_selftest(void);

#endif
