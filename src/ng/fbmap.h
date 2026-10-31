/*
 * ng WP3-1: `SYS_FB_MAP`（47）—— root-only，把 LFB 映射进调用者用户 AS。
 *
 * 背景：WP2-3 的 LFB（`0xFD000000`，1280x800x32，pitch 5120）只映射在内核
 * PML4；display server（L2a）是 ring3 进程，必须能直写扫描输出。这里只加
 * "映射"这一条机制，显示策略/合成仍然全在 displayd。
 *
 * 语义：
 *   - 只有 uid==0（root）能调；当前所有进程 uid 都是 0，cap/角色令牌后置
 *     （WP3-3 的 role token 会收紧这一点）。
 *   - 映射到固定 VA `FBMAP_VA_BASE`（1.5 GiB，用户窗口内、SHM 窗口之上），
 *     逐页 `PG_USER|PG_WRITE|PG_PCD|PG_PWT|PG_NX`——MMIO 不经可缓存路径，
 *     用户态不可执行。
 *   - 返回 `struct ds_fb_info`（base=用户 VA、phys=物理基址、size/pitch/bpp/
 *     width/height/flags）。同一个进程重复调用返回 EBUSY（不重复映射）。
 *   - 进程退出/被 kill 前 `fbmap_release_proc()` 把这段 PTE 摘掉：LFB 页不在
 *     PMM 窗口内，`as_destroy()` 对它们调用 pmm_free_page 会 ERANGE（无害），
 *     但显式摘掉更干净，也不依赖 pmm 的范围检查。
 */
#ifndef NG_FBMAP_H
#define NG_FBMAP_H

#include "ds_protocol.h"
#include "types.h"

#define FBMAP_VA_BASE 0x0000000060000000ULL /* 1.5 GiB */

#define FBMAP_OK 0
#define FBMAP_EPERM (-1)
#define FBMAP_ENOMEM (-12)
#define FBMAP_EFAULT (-14)
#define FBMAP_EBUSY (-16)
#define FBMAP_ENODEV (-19)
#define FBMAP_EINVAL (-22)

struct proc;
struct user_as;

/* 内核侧：给 p 映射 LFB；成功写 *out_va / *out_info（可空）。 */
int fbmap_map_proc(struct proc *p, u64 *out_va, struct ds_fb_info *out_info);

/* syscall 47：uva_out 指向 u64，uinfo_out 指向 struct ds_fb_info（48 字节）。 */
u64 fbmap_sys_map(u64 uva_out, u64 uinfo_out);

/* 进程回收：把 LFB 段从 AS 里摘掉（不 free 物理页）。 */
void fbmap_release_proc(struct proc *p);

/* 诊断：当前 fb 的页数（没有 fb 返回 0）。 */
u32 fbmap_pages(void);

#endif /* NG_FBMAP_H */
