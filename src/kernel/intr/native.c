/* =============================================================================
 * native.c - Native ABI v0 系统调用与句柄表
 *
 * 原生程序通过 int 0x80 进入这里；旧原型的 POSIX 风格调用表暂时走 int 0x81。
 * v0 只实现最小集合：ABI 版本、退出、让出、句柄 close/dup/info、调试输出。
 * 其余编号返回 NATIVE_E_NOSYS，避免“未实现”被误认为成功。
 * ============================================================================= */
#include "native.h"
#include "interrupt.h"
#include "uaccess.h"
#include "../console.h"
#include "../core/error.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include "../proc/proc.h"
#include "../proc/proc_internal.h"

#define NATIVE_HANDLE_MAX 16
/* v0 只声明真正实现的能力；sealed image / sandbox 尚未接入，不能提前置位。 */
#define NATIVE_ABI_FEATURES 0U

typedef struct {
    bool     used;
    uint8_t  type;
    uint16_t generation;
    uint32_t rights;
    uint32_t object_id;
    uint32_t parent;
} native_slot_t;

typedef struct {
    native_slot_t slot[NATIVE_HANDLE_MAX];
} native_table_t;

/* 每个进程一张句柄表；对象本身的内核生命周期暂由各子系统管理。 */
static native_table_t proc_tables[PROC_MAX];

static uint32_t make_handle(uint32_t slot, uint16_t generation)
{
    return ((uint32_t)generation << 8) | (slot + 1U);
}

static int handle_slot(uint32_t handle, uint32_t *slot)
{
    uint32_t index;

    if (handle == 0U) return NATIVE_E_BADF;
    index = (handle & 0xFFU);
    if (index == 0U || index > NATIVE_HANDLE_MAX) return NATIVE_E_BADF;
    *slot = index - 1U;
    return NATIVE_E_OK;
}

static native_slot_t *table_lookup(native_table_t *t, uint32_t handle,
                                   uint32_t rights)
{
    uint32_t slot;
    native_slot_t *s;

    if (handle_slot(handle, &slot) != NATIVE_E_OK) return 0;
    s = &t->slot[slot];
    if (!s->used) return 0;
    if ((uint16_t)(handle >> 8) != s->generation) return 0;
    if ((rights & ~s->rights) != 0U) return 0;
    return s;
}

static uint32_t table_init(native_table_t *t, uint32_t process_id)
{
    memset(t, 0, sizeof(*t));
    for (uint32_t i = 0; i < NATIVE_HANDLE_MAX; i++) {
        t->slot[i].generation = 1;
    }

    /* v0 约定槽 0 是当前进程自身的句柄。后续 process_create 会通过
     * bootstrap 句柄块把真正的句柄集合交给子进程。 */
    t->slot[0].used = true;
    t->slot[0].type = NATIVE_OBJ_PROCESS;
    t->slot[0].rights = NATIVE_R_WAIT | NATIVE_R_SIGNAL | NATIVE_R_MANAGE;
    t->slot[0].object_id = process_id;
    t->slot[0].parent = 0;
    return make_handle(0, t->slot[0].generation);
}

static uint32_t table_add(native_table_t *t, uint8_t type, uint32_t rights,
                          uint32_t object_id, uint32_t parent)
{
    for (uint32_t i = 0; i < NATIVE_HANDLE_MAX; i++) {
        native_slot_t *s = &t->slot[i];

        if (s->used) continue;
        s->used = true;
        s->type = type;
        s->rights = rights;
        s->object_id = object_id;
        s->parent = parent;
        if (s->generation == 0U) s->generation = 1;
        return make_handle(i, s->generation);
    }
    return 0;
}

static int table_info(native_table_t *t, uint32_t handle,
                      struct native_handle_info *info)
{
    native_slot_t *s = table_lookup(t, handle, 0);

    if (s == 0 || info == 0) return NATIVE_E_BADF;
    info->type = s->type;
    info->rights = s->rights;
    info->object_id = s->object_id;
    info->flags = 0;
    return NATIVE_E_OK;
}

static int table_close(native_table_t *t, uint32_t handle)
{
    native_slot_t *s = table_lookup(t, handle, 0);

    if (s == 0) return NATIVE_E_BADF;
    s->used = false;
    s->generation++;
    if (s->generation == 0U) s->generation = 1;
    return NATIVE_E_OK;
}

static uint32_t table_dup(native_table_t *t, uint32_t handle, uint32_t rights,
                          int *err)
{
    native_slot_t *s = table_lookup(t, handle, 0);
    uint32_t out;

    if (s == 0) {
        *err = NATIVE_E_BADF;
        return 0;
    }
    if (rights == 0U) rights = s->rights;
    if ((rights & ~s->rights) != 0U) {
        *err = NATIVE_E_ACCESS;
        return 0;
    }
    out = table_add(t, s->type, rights, s->object_id, handle);
    *err = (out != 0U) ? NATIVE_E_OK : NATIVE_E_NOSPC;
    return out;
}

static int proc_table_index(proc_t *p)
{
    if (p == 0) return -1;
    if (p < proc_table || p >= proc_table + PROC_MAX) return -1;
    return (int)(p - proc_table);
}

void native_proc_init(proc_t *p)
{
    int i = proc_table_index(p);

    if (i >= 0) (void)table_init(&proc_tables[i], (uint32_t)p->pid);
}

void native_proc_copy(proc_t *parent, proc_t *child)
{
    int pi = proc_table_index(parent);
    int ci = proc_table_index(child);
    native_table_t *src;

    if (pi < 0 || ci < 0 || parent == child) return;
    src = &proc_tables[pi];
    (void)table_init(&proc_tables[ci], (uint32_t)child->pid);

    /* 槽 0 是子进程自己的 self handle；其余句柄按值复制。
     * v0 只有 self handle，等对象 refcount 接入后再补真正的引用计数。 */
    for (uint32_t i = 1; i < NATIVE_HANDLE_MAX; i++) {
        if (!src->slot[i].used) continue;
        (void)table_add(&proc_tables[ci], src->slot[i].type,
                        src->slot[i].rights, src->slot[i].object_id, 0);
    }
}

void native_proc_free(proc_t *p)
{
    int i = proc_table_index(p);

    if (i >= 0) memset(&proc_tables[i], 0, sizeof(proc_tables[i]));
}

static native_table_t *current_table(void)
{
    int i = proc_table_index(proc_current());

    return (i >= 0) ? &proc_tables[i] : 0;
}

/* native syscall -> allowlist 位；-1 表示未知调用，沙箱下默认拒绝。 */
static int native_sandbox_bit(uint32_t nr)
{
    switch (nr) {
    case NATIVE_SYS_ABI_VERSION:  return 0;
    case NATIVE_SYS_EXIT:         return 1;
    case NATIVE_SYS_YIELD:        return 2;
    case NATIVE_SYS_HANDLE_CLOSE: return 3;
    case NATIVE_SYS_HANDLE_DUP:   return 4;
    case NATIVE_SYS_HANDLE_INFO:  return 5;
    case NATIVE_SYS_PROCESS_CREATE: return 7;
    case NATIVE_SYS_PROCESS_WAIT:   return 8;
    case NATIVE_SYS_DEBUG_WRITE:  return 6;
    default:                      return -1;
    }
}

static int native_debug_write(uint32_t ptr, uint32_t len)
{
    char buf[129];
    uint32_t done = 0;

    if (len > 64U * 1024U) return NATIVE_E_INVAL;
    if (ptr >= 0xC0000000U || len > 0xC0000000U - ptr) return NATIVE_E_FAULT;

    while (done < len) {
        uint32_t n = len - done;

        if (n > 128U) n = 128U;
        if (copy_from_user(buf, (const void *)(uintptr_t)(ptr + done), n) != E_OK) {
            return NATIVE_E_FAULT;
        }
        buf[n] = '\0';
        console_write_atomic(buf);
        done += n;
    }
    return (int)done;
}

void native_syscall_dispatch(interrupt_frame_t *f)
{
    int ret;
    proc_t *np = proc_current();

    /* native ABI 与 legacy 一样受沙箱 allowlist 约束。 */
    if (np != 0 && np->sandboxed) {
        int bit = native_sandbox_bit(f->eax);
        if (bit < 0 || (np->native_mask & (1U << (uint32_t)bit)) == 0U) {
            f->eax = (uint32_t)NATIVE_E_ACCESS;
            return;
        }
    }

    switch (f->eax) {
    case NATIVE_SYS_ABI_VERSION: {
        struct native_abi_version v;

        v.major = NATIVE_ABI_MAJOR;
        v.minor = NATIVE_ABI_MINOR;
        v.patch = NATIVE_ABI_PATCH;
        v.reserved = 0;
        v.features = NATIVE_ABI_FEATURES;
        ret = (copy_to_user((void *)(uintptr_t)f->ebx, &v, sizeof(v)) == E_OK)
                  ? NATIVE_E_OK : NATIVE_E_FAULT;
        break;
    }
    case NATIVE_SYS_EXIT:
        proc_exit((int)f->ebx);
        ret = NATIVE_E_OK;              /* 不会到达 */
        break;
    case NATIVE_SYS_YIELD:
        proc_yield();
        ret = NATIVE_E_OK;
        break;
    case NATIVE_SYS_HANDLE_CLOSE: {
        native_table_t *t = current_table();

        ret = (t != 0) ? table_close(t, f->ebx) : NATIVE_E_BADF;
        break;
    }
    case NATIVE_SYS_HANDLE_DUP: {
        native_table_t *t = current_table();
        uint32_t h = 0;
        int err = NATIVE_E_BADF;

        if (t != 0) h = table_dup(t, f->ebx, f->ecx, &err);
        ret = (h != 0U) ? (int)h : err;
        break;
    }
    case NATIVE_SYS_HANDLE_INFO: {
        native_table_t *t = current_table();
        struct native_handle_info info;
        int r;

        r = (t != 0) ? table_info(t, f->ebx, &info) : NATIVE_E_BADF;
        if (r == NATIVE_E_OK &&
            copy_to_user((void *)(uintptr_t)f->ecx, &info, sizeof(info)) != E_OK) {
            r = NATIVE_E_FAULT;
        }
        ret = r;
        break;
    }
    case NATIVE_SYS_PROCESS_CREATE: {
        native_table_t *t = current_table();
        char name[32];
        uint32_t rights = (f->edx != 0U) ? f->edx :
                          (NATIVE_R_WAIT | NATIVE_R_SIGNAL | NATIVE_R_MANAGE);
        proc_t *child;
        uint32_t h;

        if (t == 0 || f->ecx == 0U || f->ecx >= sizeof(name)) {
            ret = NATIVE_E_INVAL;
            break;
        }
        for (uint32_t i = 0; i < f->ecx; i++) {
            if (copy_from_user(&name[i], (const void *)(uintptr_t)(f->ebx + i), 1) != E_OK) {
                ret = NATIVE_E_FAULT;
                break;
            }
        }
        if (ret == NATIVE_E_FAULT) break;
        name[f->ecx] = '\0';
        child = proc_spawn(name);
        if (child == 0) {
            ret = NATIVE_E_NOENT;
            break;
        }
        child->parent_pid = (np != 0) ? np->pid : 0U;
        h = table_add(t, NATIVE_OBJ_PROCESS, rights, child->pid, 0);
        ret = (h != 0U) ? (int)h : NATIVE_E_NOSPC;
        break;
    }
    case NATIVE_SYS_PROCESS_WAIT: {
        native_table_t *t = current_table();
        native_slot_t *s = (t != 0) ? table_lookup(t, f->ebx, NATIVE_R_WAIT) : 0;
        int st = 0;
        int pid;

        if (s == 0 || s->type != NATIVE_OBJ_PROCESS) {
            ret = NATIVE_E_BADF;
            break;
        }
        pid = proc_wait((int)s->object_id, &st);
        if (pid > 0 && f->ecx != 0U) {
            (void)copy_to_user((void *)(uintptr_t)f->ecx, &st, sizeof(st));
        }
        ret = pid;
        break;
    }
    case NATIVE_SYS_DEBUG_WRITE:
        ret = native_debug_write(f->ebx, f->ecx);
        break;
    default:
        ret = NATIVE_E_NOSYS;
        break;
    }

    f->eax = (uint32_t)ret;
}

/* -------------------------------------------------------------------------
 * 启动自检
 * ------------------------------------------------------------------------- */

static int native_call3(int n, int a, int b, int c)
{
    int r;

    __asm__ volatile ("int $0x80"
                      : "=a"(r)
                      : "a"(n), "b"(a), "c"(b), "d"(c)
                      : "memory");
    return r;
}

static void native_handle_selftest(void)
{
    native_table_t t;
    struct native_handle_info info;
    uint32_t self;
    uint32_t dup;
    int ok = 1;

    self = table_init(&t, 0x1234U);
    if (self == 0U) ok = 0;

    if (ok && table_info(&t, self, &info) != NATIVE_E_OK) ok = 0;
    if (ok && info.type != NATIVE_OBJ_PROCESS) ok = 0;

    dup = 0;
    if (ok) {
        int derr = NATIVE_E_OK;
        dup = table_dup(&t, self, NATIVE_R_WAIT | NATIVE_R_SIGNAL, &derr);
        if (dup == 0U || derr != NATIVE_E_OK) ok = 0;
    }
    if (ok && table_close(&t, dup) != NATIVE_E_OK) ok = 0;
    if (ok && table_info(&t, dup, &info) != NATIVE_E_BADF) ok = 0;

    /* 权限扩张必须返回 E_ACCESS，而不是含糊的 BADF */
    if (ok) {
        int derr = NATIVE_E_OK;
        uint32_t bad = table_dup(&t, self, NATIVE_R_READ, &derr);

        if (bad != 0U || derr != NATIVE_E_ACCESS) ok = 0;
    }

    if (ok) kprintln("native: handle table ok");
    else    kprintln("native: handle table FAILED");
}

void native_selftest(void)
{
    struct native_abi_version v;
    static char msg[] = "native: int 0x80 debug write ok\n";
    int r;

    v.major = v.minor = v.patch = 0;
    v.reserved = 0;
    v.features = 0;

    r = native_call3(NATIVE_SYS_ABI_VERSION, (int)(uintptr_t)&v, 0, 0);
    if (r == NATIVE_E_OK && v.major == NATIVE_ABI_MAJOR &&
        v.minor == NATIVE_ABI_MINOR) {
        kprintf("native: ABI %u.%u gate ok (features=%x)\n",
                (unsigned int)v.major, (unsigned int)v.minor,
                (unsigned int)v.features);
    } else {
        kprintf("native: ABI gate FAILED (r=%d)\n", r);
    }

    r = native_call3(NATIVE_SYS_DEBUG_WRITE, (int)(uintptr_t)msg,
                     (int)strlen(msg), 0);
    if (r != (int)strlen(msg)) {
        kprintf("native: debug write FAILED (r=%d)\n", r);
    }

    native_handle_selftest();
}
