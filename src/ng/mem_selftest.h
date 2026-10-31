/*
 * ng WP1-4: 内核侧内存自检（memmap -> PMM -> heap -> 页表池）。
 *
 * 纯逻辑（memmap/pmm/heap）放在各自模块里，可被 host 单测直接编译；这里只做
 * 内核 glue：解析启动信息、按链接符号保留内核镜像、打印证据行、跑分配器自检。
 */
#ifndef NG_MEM_SELFTEST_H
#define NG_MEM_SELFTEST_H

#include "types.h"

/* 解析启动内存图 + 初始化 PMM + PMM 自检；返回失败项数。 */
int mem_boot_init(u64 boot_info);

/* 堆分配/合并/double-free 自检；返回失败项数。 */
int mem_heap_selftest(void);

/* 页表页确实来自 PMM 的自检；返回失败项数。 */
int mem_pgtbl_selftest(void);

#endif /* NG_MEM_SELFTEST_H */
