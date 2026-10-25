/* =============================================================================
 * types.h - 兼容转发头（根目录）
 *
 * 实际内容已拆分到接口层：
 *   include/types.h   基础类型（定宽整数、size_t/uintptr_t、地址类型、NULL）
 *   include/macros.h  工具宏（KiB/MiB/GiB、ARRAY_SIZE、ALIGN_*、MIN/MAX、UNUSED）
 *
 * 保留本文件是为了让现有 66 处 #include "types.h" 不改动；新代码可以直接
 * 包含 "include/types.h" / "include/macros.h"。
 * ============================================================================= */
#ifndef TYPES_ROOT_H
#define TYPES_ROOT_H

#include "include/types.h"
#include "include/macros.h"

#endif /* TYPES_ROOT_H */
