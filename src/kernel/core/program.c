#include "program.h"
#include "object.h"
#include "error.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include "../mm/memtest.h"
#include "../apps/shell.h"
#include "../apps/heartbeat.h"
#include "../user/user_mode.h"

static program_t programs[PROG_MAX];
static uint32_t  count;

/* 内建示例程序：只证明“程序是注册出来的对象”，不是某个文件路径 */
static void prog_hello_entry(void)
{
    kprintln("hello: running program hello@1");
}

static void prog_memtest_entry(void)
{
    int failures = mm_selftest();
    kprintf("memtest: program finished, failures=%d\n", failures);
}

/* ring3 示例程序：准备用户页并 iret 进用户态 */
static void prog_userdemo_entry(void)
{
    if (user_mode_run() != 0) {
        kprintln("user: setup failed (no page or mapping)");
    }
}

/* shell 现在是一个 ring3 程序：从 /bin/shell 载入执行。
   载入失败才退回内核内建 shell（保证系统仍可用）。 */
static void prog_shell_entry(void)
{
    if (user_exec("system:/bin/shell") != 0) {
        kprintln("shell: cannot load /bin/shell, using kernel shell");
        shell_entry();
    }
}

void prog_init(void)
{
    count = 0;
    prog_register("shell",   1, "ring3 shell loaded from /bin/shell", prog_shell_entry);
    prog_register("hello",   1, "print a greeting",    prog_hello_entry);
    prog_register("memtest", 1, "run the memory tests", prog_memtest_entry);
    prog_register("heartbeat", 1, "print a heartbeat every second", heartbeat_entry);
    prog_register("userdemo",  1, "run a tiny ring3 program", prog_userdemo_entry);
}

int prog_register(const char *name, uint32_t version, const char *desc,
                  void (*entry)(void))
{
    if (name == 0 || name[0] == '\0') return E_INVAL;
    if (strlen(name) >= PROG_NAME_MAX) return E_INVAL;
    if (count >= PROG_MAX) return E_FULL;
    if (prog_lookup(name) != 0) return E_EXIST;

    program_t *p = &programs[count];
    strncpy(p->name, name, PROG_NAME_MAX - 1U);
    p->name[PROG_NAME_MAX - 1U] = '\0';
    p->version = version;
    p->desc = desc;
    p->entry = entry;
    count++;
    return E_OK;
}

const program_t *prog_lookup(const char *name)
{
    if (name == 0) return 0;

    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(programs[i].name, name) == 0) {
            return &programs[i];
        }
    }
    return 0;
}

const program_t *prog_at(uint32_t index)
{
    return (index < count) ? &programs[index] : 0;
}

uint32_t prog_count(void)
{
    return count;
}

uint32_t prog_open(const char *name)
{
    const program_t *p = prog_lookup(name);
    if (p == 0) return 0;

    return obj_create(OBJ_PROGRAM, (void *)(uintptr_t)p);
}

int prog_run(const char *name)
{
    const program_t *p = prog_lookup(name);
    if (p == 0) return E_NOENT;
    if (p->entry == 0) return E_INVAL;

    p->entry();
    return E_OK;
}
