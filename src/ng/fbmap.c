/*
 * ng WP3-1: SYS_FB_MAP 实现（见 fbmap.h）。
 */
#include "fbmap.h"

#include "fb.h"
#include "paging.h"
#include "proc.h"
#include "uaccess.h"
#include "useras.h"
#include "wp23.h"

static u32 fbmap_page_count(const struct fb_info *fb)
{
    u64 size;

    if (fb == NULL || fb->pitch == 0 || fb->height == 0) {
        return 0;
    }
    size = (u64)fb->pitch * (u64)fb->height;
    return (u32)((size + 0xFFFULL) >> 12);
}

u32 fbmap_pages(void)
{
    return fbmap_page_count(ng_display_fb());
}

static u64 fbmap_flags(void)
{
    u64 flags = PG_USER | PG_WRITE | PG_PCD | PG_PWT;

    if (pg_nx_on()) {
        flags |= PG_NX;
    }
    return flags;
}

int fbmap_map_proc(struct proc *p, u64 *out_va, struct ds_fb_info *out_info)
{
    const struct fb_info *fb;
    struct user_as *as;
    u32 pages;
    u32 i;
    u64 flags;

    if (out_va != NULL) {
        *out_va = 0;
    }
    if (p == NULL || p->as == NULL) {
        return FBMAP_EPERM;
    }
    if (p->uid != 0) {
        return FBMAP_EPERM; /* root-only；cap/role token 后置 */
    }
    fb = ng_display_fb();
    if (fb == NULL) {
        return FBMAP_ENODEV; /* VGA 文本降级路径没有 LFB */
    }
    pages = fbmap_page_count(fb);
    if (pages == 0) {
        return FBMAP_EINVAL;
    }
    if (FBMAP_VA_BASE + (u64)pages * 0x1000ULL > NG_USER_VA_MAX) {
        return FBMAP_EINVAL;
    }
    as = (struct user_as *)p->as;
    if ((as_walk(as, FBMAP_VA_BASE) & PG_PRESENT) != 0) {
        return FBMAP_EBUSY;
    }
    flags = fbmap_flags();
    for (i = 0; i < pages; i++) {
        u64 va = FBMAP_VA_BASE + (u64)i * 0x1000ULL;
        u64 phys = (u64)(usize)fb->base + (u64)i * 0x1000ULL;

        if (as_map(as, va, phys, flags) != AS_OK) {
            u32 k;

            for (k = 0; k < i; k++) {
                (void)pg_unmap_in(as->pml4,
                                  FBMAP_VA_BASE + (u64)k * 0x1000ULL);
            }
            return FBMAP_ENOMEM;
        }
    }
    if (out_info != NULL) {
        out_info->base = FBMAP_VA_BASE;
        out_info->phys = (u64)(usize)fb->base;
        out_info->size = (u64)fb->pitch * (u64)fb->height;
        out_info->pitch = fb->pitch;
        out_info->bpp = fb->bpp;
        out_info->width = fb->width;
        out_info->height = fb->height;
        out_info->flags = DS_FB_FLAG_PCD | DS_FB_FLAG_PWT;
        if (pg_nx_on()) {
            out_info->flags |= DS_FB_FLAG_NX;
        }
        out_info->reserved = 0;
    }
    if (out_va != NULL) {
        *out_va = FBMAP_VA_BASE;
    }
    return FBMAP_OK;
}

u64 fbmap_sys_map(u64 uva_out, u64 uinfo_out)
{
    struct proc *p = proc_current();
    struct ds_fb_info info;
    u64 va = 0;
    int rc;

    if (uaccess_range_ok(uva_out, 8, 1) == 0 ||
        uaccess_range_ok(uinfo_out, (u64)sizeof(info), 1) == 0) {
        return (u64)(i64)FBMAP_EFAULT;
    }
    rc = fbmap_map_proc(p, &va, &info);
    if (rc != FBMAP_OK) {
        return (u64)(i64)rc;
    }
    if (copyout(uva_out, &va, 8) != UACCESS_OK ||
        copyout(uinfo_out, &info, (u64)sizeof(info)) != UACCESS_OK) {
        fbmap_release_proc(p);
        return (u64)(i64)FBMAP_EFAULT;
    }
    return 0;
}

void fbmap_release_proc(struct proc *p)
{
    const struct fb_info *fb;
    struct user_as *as;
    u32 pages;
    u32 i;

    if (p == NULL || p->as == NULL) {
        return;
    }
    as = (struct user_as *)p->as;
    fb = ng_display_fb();
    pages = fbmap_page_count(fb);
    if (pages == 0) {
        return;
    }
    for (i = 0; i < pages; i++) {
        u64 va = FBMAP_VA_BASE + (u64)i * 0x1000ULL;

        if ((as_walk(as, va) & PG_PRESENT) != 0) {
            (void)pg_unmap_in(as->pml4, va);
        }
    }
}
