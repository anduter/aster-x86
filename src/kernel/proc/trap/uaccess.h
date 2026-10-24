#ifndef INTR_UACCESS_H
#define INTR_UACCESS_H

#include "../../types.h"

/* 安全的内核 <-> 用户内存拷贝。
 * 遇到未映射/越界地址时不 panic，而是回到调用点返回错误码。 */
int copy_from_user(void *dst, const void *src, uint32_t len);
int copy_to_user(void *dst, const void *src, uint32_t len);

/* 内核直接访问用户内存时的 SMAP 访问窗口；未启用 SMAP 时为空操作。 */
void user_access_begin(void);
void user_access_end(void);

#endif
