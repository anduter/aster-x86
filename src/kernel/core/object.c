#include "object.h"
#include "error.h"

/* 对象表：每个槽位记录类型、引用计数和指向真实数据的指针。
 * 数据本身由各子系统（ramfs / program / device）自己管理，
 * 对象层只负责“类型标签 + 生命周期”。 */
typedef struct {
    obj_type_t type;    /* 0 表示空槽 */
    uint32_t   refs;
    void      *data;
} obj_slot_t;

static obj_slot_t slots[OBJ_MAX];
static uint32_t   live;

static uint32_t slot_index(uint32_t handle)
{
    uint32_t idx = handle & 0x00FFFFFFU;
    return (idx == 0U || idx > OBJ_MAX) ? (uint32_t)-1 : (idx - 1U);
}

uint32_t obj_create(obj_type_t type, void *data)
{
    if (type == 0) return 0;

    for (uint32_t i = 0; i < OBJ_MAX; i++) {
        if (slots[i].type == 0) {
            slots[i].type = type;
            slots[i].refs = 1;
            slots[i].data = data;
            live++;
            return ((uint32_t)type << 24) | (i + 1U);
        }
    }
    return 0;   /* 对象表满 */
}

int obj_deref(uint32_t handle, obj_type_t expect, void **data)
{
    uint32_t idx = slot_index(handle);

    if (idx == (uint32_t)-1) return E_BADH;
    if (slots[idx].type == 0) return E_BADH;

    if (slots[idx].type != (uint32_t)expect) return E_TYPE;
    if ((handle >> 24) != (uint32_t)expect) return E_BADH;   /* 句柄标签被伪造 */

    if (data != 0) *data = slots[idx].data;
    return E_OK;
}

int obj_ref(uint32_t handle, obj_type_t expect)
{
    uint32_t idx = slot_index(handle);

    if (idx == (uint32_t)-1) return E_BADH;
    if (slots[idx].type == 0) return E_BADH;
    if (slots[idx].type != (uint32_t)expect) return E_TYPE;
    if ((handle >> 24) != (uint32_t)expect) return E_BADH;
    if (slots[idx].refs == 0xFFFFFFFFU) return E_NOSPC;
    slots[idx].refs++;
    return E_OK;
}

int obj_close(uint32_t handle, obj_type_t expect)
{
    uint32_t idx = slot_index(handle);

    if (idx == (uint32_t)-1) return E_BADH;
    if (slots[idx].type == 0) return E_BADH;
    if (slots[idx].type != (uint32_t)expect) return E_TYPE;

    if (slots[idx].refs > 1U) {
        slots[idx].refs--;
        return E_OK;
    }

    slots[idx].type = 0;
    slots[idx].refs = 0;
    slots[idx].data = 0;
    live--;
    return E_OK;
}

uint32_t obj_live_count(void)
{
    return live;
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
