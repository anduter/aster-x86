#include "program.h"
#include "include/mm.h"
#include "include/kernel.h"
#include "include/svc.h"
#include "include/kernel.h"
#include "include/kernel.h"
#include "include/mm.h"
#include "include/apps.h"
#include "include/apps.h"
#include "include/apps.h"
#include "include/apps.h"
#include "include/apps.h"
#include "include/userland.h"
#include "include/pkg.h"
#include "include/net.h"

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

/* 最小 ring3 探针：只 write/exit，用于 AP ring3 回归 */
static void prog_ring3probe_entry(void)
{
    if (user_probe_run() != 0) {
        kprintln("user: probe setup failed");
    }
}

static void prog_fsd_entry(void)
{
    if (user_fsd_run() != 0) {
        kprintln("user: fsd setup failed");
    }
}

static void prog_blkd_entry(void)
{
    if (user_blkd_run() != 0) {
        kprintln("user: blkd setup failed");
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

static void prog_tcptest_entry(void)
{
    /* 必须在 proc 线程里跑：syscall 上下文可能是关中断的，timer_ticks 不走，
     * RTO/重传会永远等不到超时（曾经把 NMI watchdog 直接触发）。 */
    if (net_tcp_robust_selftest() != 0) {
        kprintln("net: tcp robust selftest failed");
    }
}


void prog_init(void)
{
    count = 0;
    prog_register("shell",   1, "ring3 shell loaded from /bin/shell", prog_shell_entry);
    prog_register("getty",   1, "console getty: mark tty login, exec /bin/shell", getty_entry);
    prog_register("hello",   1, "print a greeting",    prog_hello_entry);
    prog_register("memtest", 1, "run the memory tests", prog_memtest_entry);
    prog_register("heartbeat", 1, "print a heartbeat every second", heartbeat_entry);
    prog_register("userdemo",  1, "run a tiny ring3 program", prog_userdemo_entry);
    prog_register("supervisor", 1, "service registry supervisor", supervisor_entry);
    prog_register("ring3probe", 1, "tiny ring3 AP probe", prog_ring3probe_entry);
    prog_register("echosvc",   1, "echo service for supervisor", echosvc_entry);
    prog_register("svcwatch",  1, "periodic echo service watchdog", svcwatch_entry);
    prog_register("fsd",       1, "ring3 userspace file service", prog_fsd_entry);
    prog_register("blkd",      1, "ring3 virtual block service", prog_blkd_entry);
    prog_register("spin",      1, "busy-wait 2s (scheduler load test)", spin_entry);
    /* 组件模型：_pkgd 安装服务；pkgtest 是它的回归驱动（启动后自动跑一次） */
    prog_register("pkgd",      1, "component package service (_pkgd)", pkgd_entry);
    prog_register("pkgtest",   1, "component model regression (pkg)", pkg_selftest_entry);
    /* P0：组件 ELF 的 ring3 启动线程（由 user_component_exec 唤醒） */
    prog_register("comprun",   1, "component ELF loader/exec (pkg)", user_component_entry);
    prog_register("tcptest",   1, "TCP robustness regression (4c)", prog_tcptest_entry);

    /* heartbeat 是验证 AP 调度/计时器的探针，亲和到 AP；其余程序按负载放置。 */
    (void)prog_set_flags("heartbeat", PROG_F_PIN_AP);
    (void)prog_set_flags("ring3probe", PROG_F_PIN_AP);
}

int prog_set_flags(const char *name, uint32_t flags)
{
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(programs[i].name, name) == 0) {
            programs[i].flags |= flags;
            return E_OK;
        }
    }
    return E_NOENT;
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
