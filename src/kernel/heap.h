#ifndef HEAP_H
#define HEAP_H

/* 堆起始地址（8MB，位于 VMM 恒等映射的物理内存范围内，可直接使用） */
#define HEAP_BASE   0x00800000
/* 堆总大小（16MB） */
#define HEAP_SIZE   (16 * 1024 * 1024)

/* 初始化堆分配器 */
void heap_init(void);

/* 申请内存（4 字节对齐） */
void* malloc(unsigned int size);

/* 释放内存 */
void free(void* ptr);

/* 堆使用统计（诊断）：已用字节 / 总字节 */
void heap_stats(unsigned int *used, unsigned int *total);

#endif
