#ifndef MM_FILEMAP_H
#define MM_FILEMAP_H

#include "../include/types.h"

/* =============================================================================
 * filemap.h - 文件映射 mmap 的“读页 + 映射”核心（mm 内实现）
 *
 * fs 层没有 lseek，也没有把“路径/偏移”暴露给 mm 的接口；mm 只能用
 * fs_read_ex() 顺序读。因此文件映射采用“一次性读入”（非按需缺页）：
 * 调用方给一个 reader，mm 先丢弃 offset 字节，再把 len 字节读进新分配的
 * 物理页并映射到当前地址空间。
 *
 * MAP_PRIVATE 语义：
 *   - prot 不带 VMM_PROT_WRITE：页真正只读（无 PTE_WRITE/PTE_COW），用户写
 *     触发 #PF → 进程按 SIGSEGV 终止（“拒绝写”）。
 *   - prot 带 VMM_PROT_WRITE：页标 PTE_COW。首次写走 vmm_cow_fault()：
 *     引用计数为 1 时直接把这块私有页改成可写（文件本身已读进内存，写不到
 *     文件），等价 MAP_PRIVATE 的“私有副本”。文件内容不会被修改。
 *
 * fs 层顺序读的限制：
 *   - offset 必须是页大小整数倍（映射按页）；
 *   - mmap 会消费 fd 的读偏移：映射完成后 fd 位于 offset+实际读到的字节
 *     之后（RAMFS/FAT16 句柄都是顺序读）。调用方若还要顺序读该 fd，应另开
 *     一个句柄或先把偏移读回去（ABI 目前没有 lseek）。
 * ============================================================================= */

/* 数据源：从“当前位置”顺序读最多 len 字节到 buf。
 * 返回实际读到的字节数；0 = EOF；<0 = 错误。 */
typedef int (*mm_file_reader_t)(void *ctx, void *buf, uint32_t len);

/* 把 [offset, offset+len) 读进用户页并映射到当前地址空间 base 起。
 * base/len/offset 都必须页对齐，len > 0，reader 非空。
 * 成功返回 0；失败返回 -1，并回滚本次已建立的映射与物理页。 */
int mm_file_map(uint32_t base, uint32_t len, uint32_t offset,
                mm_file_reader_t reader, void *ctx, uint32_t prot);

/* 取消 [base, base+len) 的页映射并释放物理页（munmap / 失败回滚共用）。
 * 对 COW 共享页只递减 PMM 引用计数（最后一个引用才真正回收）。 */
void mm_unmap_range(uint32_t base, uint32_t len);

/* 自检：只读文件映射内容/零填充、offset 定位、MAP_PRIVATE COW 私有副本、
 * munmap 释放与 PMM 账目复原。成功打印 "mm: file mmap ok ..." 返回 1；
 * 失败打印 FAIL 返回 0。 */
int mm_filemap_selftest(void);

#endif
