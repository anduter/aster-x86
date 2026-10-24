#include "object.h"
#include "../core/error.h"
#include "../core/spinlock.h"

/* 对象表：每个槽位记录类型、引用计数和指向真实数据的指针。
 * 数据本身由各子系统（ramfs / program / device）自己管理，
 * 对象层只负责“类型标签 + 生命周期”。
 *
 * SMP：对象表是全局共享的，BSP/AP 可能同时创建/关闭句柄，必须串行化。 */
typedef struct {
    obj_type_t type;    /* 0 表示空槽 */
    uint32_t   refs;
    void      *data;
} obj_slot_t;

static obj_slot_t slots[OBJ_MAX];
static uint32_t   live;
static spinlock_t obj_lock = SPINLOCK_INIT;

static uint32_t slot_index(uint32_t handle)
{
    uint32_t idx = handle & 0x00FFFFFFU;
    return (idx == 0U || idx > OBJ_MAX) ? (uint32_t)-1 : (idx - 1U);
}

uint32_t obj_create(obj_type_t type, void *data)
{
    uint32_t flags;
    uint32_t handle = 0;

    if (type == 0) return 0;

    flags = spin_lock_irqsave(&obj_lock);
    for (uint32_t i = 0; i < OBJ_MAX; i++) {
        if (slots[i].type == 0) {
            slots[i].type = type;
            slots[i].refs = 1;
            slots[i].data = data;
            live++;
            handle = ((uint32_t)type << 24) | (i + 1U);
            break;
        }
    }
    spin_unlock_irqrestore(&obj_lock, flags);
    return handle;   /* 0 = 对象表满 */
}

int obj_deref(uint32_t handle, obj_type_t expect, void **data)
{
    uint32_t idx = slot_index(handle);
    uint32_t flags;
    int r = E_OK;

    if (idx == (uint32_t)-1) return E_BADH;

    flags = spin_lock_irqsave(&obj_lock);
    if (slots[idx].type == 0) {
        r = E_BADH;
    } else if (slots[idx].type != (uint32_t)expect) {
        r = E_TYPE;
    } else if ((handle >> 24) != (uint32_t)expect) {   /* 句柄标签被伪造 */
        r = E_BADH;
    } else if (data != 0) {
        *data = slots[idx].data;
    }
    spin_unlock_irqrestore(&obj_lock, flags);
    return r;
}

int obj_ref(uint32_t handle, obj_type_t expect)
{
    uint32_t idx = slot_index(handle);
    uint32_t flags;
    int r = E_OK;

    if (idx == (uint32_t)-1) return E_BADH;

    flags = spin_lock_irqsave(&obj_lock);
    if (slots[idx].type == 0) {
        r = E_BADH;
    } else if (slots[idx].type != (uint32_t)expect) {
        r = E_TYPE;
    } else if ((handle >> 24) != (uint32_t)expect) {
        r = E_BADH;
    } else if (slots[idx].refs == 0xFFFFFFFFU) {
        r = E_NOSPC;
    } else {
        slots[idx].refs++;
    }
    spin_unlock_irqrestore(&obj_lock, flags);
    return r;
}

int obj_close(uint32_t handle, obj_type_t expect)
{
    uint32_t idx = slot_index(handle);
    uint32_t flags;
    int r = E_OK;

    if (idx == (uint32_t)-1) return E_BADH;

    flags = spin_lock_irqsave(&obj_lock);
    if (slots[idx].type == 0) {
        r = E_BADH;
    } else if (slots[idx].type != (uint32_t)expect) {
        r = E_TYPE;
    } else if (slots[idx].refs > 1U) {
        slots[idx].refs--;
    } else {
        slots[idx].type = 0;
        slots[idx].refs = 0;
        slots[idx].data = 0;
        live--;
    }
    spin_unlock_irqrestore(&obj_lock, flags);
    return r;
}

uint32_t obj_live_count(void)
{
    uint32_t flags;
    uint32_t n;

    flags = spin_lock_irqsave(&obj_lock);
    n = live;
    spin_unlock_irqrestore(&obj_lock, flags);
    return n;
}

const char *obj_type_name(obj_type_t type)
{
    switch (type) {
    case OBJ_FILE:    return "file";
    case OBJ_PROGRAM: return "program";
    case OBJ_DEVICE:  return "device";
    case OBJ_CHANNEL: return "channel";
    default:          return "none";
    }
}
