#ifndef PROC_CAP_H
#define PROC_CAP_H

#include "../include/types.h"
#include "include/mm.h"

#define CAP_TABLE_MAX 16U

typedef struct {
    bool       used;
    obj_type_t type;
    uint32_t   object;     /* 全局 obj 句柄 */
    uint32_t   rights;
    uint16_t   generation;
} cap_slot_t;

typedef struct {
    cap_slot_t slot[CAP_TABLE_MAX];
} cap_table_t;

void cap_init(cap_table_t *t);
uint32_t cap_install(cap_table_t *t, uint32_t object, obj_type_t type,
                     uint32_t rights);
int cap_lookup(cap_table_t *t, uint32_t handle, obj_type_t type,
               uint32_t need_rights, uint32_t *object);
int cap_close(cap_table_t *t, uint32_t handle);
int cap_transfer(cap_table_t *src, cap_table_t *dst, uint32_t handle,
                 uint32_t rights);
void cap_fork(cap_table_t *dst, const cap_table_t *src);
void cap_release_all(cap_table_t *t);
int cap_selftest(void);

#endif
