/* =============================================================================
 * intr/uaccess.c - 安全访问用户内存
 *
 * 拷贝放在 intr/uaccess.asm 里，[start,end) 指令范围登记到通用异常表。
 * 缺页时异常表把 eip 改到 fixup，拷贝函数返回错误码，内核继续运行。
 * ============================================================================= */
#include "uaccess.h"
#include "interrupt.h"
#include "fault.h"
#include "../../core/error.h"
#include "include/security.h"

/* 汇编实现：返回 0 成功 / 1 缺页 */
extern int uaccess_copy(void *dst, const void *src, uint32_t len);

/* 异常表标签（由 uaccess.asm 导出） */
extern uint32_t uaccess_copy_start;
extern uint32_t uaccess_copy_end;
extern uint32_t uaccess_copy_fixup;

/* #PF 钩子：命中修复表就改 eip，否则交给默认策略 */
static int pf_fixup(interrupt_frame_t *f)
{
    uint32_t fix = fault_lookup(f->eip);

    if (fix != 0U) {
        f->eip = fix;
        return EXC_HANDLED;
    }
    return EXC_PANIC;
}

void uaccess_init(void)
{
    fault_register((uint32_t)(uintptr_t)&uaccess_copy_start,
                   (uint32_t)(uintptr_t)&uaccess_copy_end,
                   (uint32_t)(uintptr_t)&uaccess_copy_fixup);
    exception_register(14U, pf_fixup);
}

int copy_from_user(void *dst, const void *src, uint32_t len)
{
    if (len == 0U) return E_OK;
    if (dst == 0 || src == 0) return E_INVAL;

    return (uaccess_copy(dst, src, len) != 0) ? E_INVAL : E_OK;
}

int copy_to_user(void *dst, const void *src, uint32_t len)
{
    if (len == 0U) return E_OK;
    if (dst == 0 || src == 0) return E_INVAL;

    return (uaccess_copy(dst, src, len) != 0) ? E_INVAL : E_OK;
}

/* 内核直接操作已映射用户内存（例如信号帧）时的访问窗口。
 * 未启用 SMAP 时是空操作；启用后必须成对调用。 */
void user_access_begin(void)
{
    if (smap_enabled) {
        __asm__ volatile (".byte 0x0f, 0x01, 0xcb" ::: "memory"); /* stac */
    }
}

void user_access_end(void)
{
    if (smap_enabled) {
        __asm__ volatile (".byte 0x0f, 0x01, 0xca" ::: "memory"); /* clac */
    }
}
