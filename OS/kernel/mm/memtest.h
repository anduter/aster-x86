#ifndef MM_MEMTEST_H
#define MM_MEMTEST_H

/* 内存子系统自检
 * 覆盖：单页分配/释放、连续多页、堆 malloc/free、
 *       vmalloc 相邻块释放、页数账目与异常计数。
 * 返回失败项数（0 表示全部通过），结果同时打印到串口。 */
int mm_selftest(void);

#endif
