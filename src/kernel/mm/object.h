#ifndef CORE_OBJECT_H
#define CORE_OBJECT_H

#include "../types.h"

/* 对象类型：句柄的高 8 位就是它，所以“拿程序句柄当文件用”会在入口被拒绝。
 * 这是本设计区别于 Linux“一切皆文件”的关键：五类对象并列，互不冒充。 */
typedef enum {
    OBJ_FILE    = 1,
    OBJ_PROGRAM = 2,
    OBJ_DEVICE  = 3,
    OBJ_CHANNEL = 4     /* 预留给 P1 的消息通道 */
} obj_type_t;

/* 对象槽位上限（每个对象占一个槽，句柄 = 类型 << 24 | 槽序号 + 1） */
#define OBJ_MAX 64

/* 创建一个对象，返回句柄；失败返回 0（0 永远不是合法句柄） */
uint32_t obj_create(obj_type_t type, void *data);

/* 校验句柄类型并取出数据指针；成功返回 E_OK */
int obj_deref(uint32_t handle, obj_type_t expect, void **data);

/* 增加对象引用；句柄类型/标签无效时返回错误 */
int obj_ref(uint32_t handle, obj_type_t expect);

/* 释放一次引用；类型不符或句柄非法返回负错误码 */
int obj_close(uint32_t handle, obj_type_t expect);

/* 当前存活对象数 / 类型名（用于 shell 与日志） */
uint32_t obj_live_count(void);
const char *obj_type_name(obj_type_t type);

#endif
