/*
 * Aster SDK alpha —— 应用入口 / 窗口 / 事件循环骨架（ABI 1.0 未冻结）
 *
 * 最小应用只做三件事：
 *   1. 实现 int aster_app_main(long argc, char **argv)；
 *   2. 用 aster_app_open() 建窗口，拿 aster_app.surface 画像素；
 *   3. 用 aster_app_poll() 收键盘事件，aster_app_commit() 提交画面。
 *
 * 入口 ABI（与 ng 内核的进程入口一致）：
 *   内核 iretq 到 _start 时 rdi = argc、rsi = argv，用户栈已按 16 字节对齐；
 *   _start（本头文件提供，weak 符号）对齐栈后调用 aster_app_main，返回后
 *   syscall 1 exit(code)。想自己写入口时，定义 ASTER_NO_DEFAULT_ENTRY 并在
 *   自己的 _start 里调用 aster_app_main。
 *
 * 事件循环建议（示例见 sdk/examples/hello_gui）：
 *   while (aster_app_running(&app)) {
 *       rc = aster_app_poll(&app, &ev, ASTER_DISPLAY_POLL_TICKS);
 *       if (rc < 0) break;                 // 端口/协议错误
 *       if (rc == 0) continue;             // 20ms 内无输入
 *       // 处理 ev（KEY_DOWN/KEY_UP）后重画 + commit
 *   }
 *   aster_app_close(&app);
 *
 * alpha 边界：一个进程一个窗口；没有装饰/拖动/缩放（WM 的职责，WP3-3）；
 * 没有 alpha 合成；没有定时器（用轮询超时近似）。
 */
#ifndef ASTER_APP_H
#define ASTER_APP_H

#include <aster/types.h>
#include <aster/syscall.h>
#include <aster/fs.h>
#include <aster/ipc.h>
#include <aster/display.h>
#include <aster/draw.h>

/* 应用入口符号：_start 会 call 它。 */
int aster_app_main(long argc, char **argv);

/* 方便声明：ASTER_APP_MAIN { ... } */
#define ASTER_APP_MAIN int aster_app_main(long argc, char **argv)

struct aster_app_config {
    u32 width;
    u32 height;
    const char *title; /* 可为 NULL */
    u32 flags;         /* ASTER_DISPLAY_CREATE_F_POS 或 0 */
    i32 x;             /* flags & POS 时有效 */
    i32 y;
};

struct aster_app {
    struct aster_display display;
    struct aster_surface surface;
    u32 running; /* 0 = 退出循环 */
};

/* 事件类型 */
#define ASTER_EVENT_NONE 0u
#define ASTER_EVENT_KEY_DOWN 1u
#define ASTER_EVENT_KEY_UP 2u

struct aster_event {
    u32 type; /* ASTER_EVENT_* */
    u32 surface_id;
    u32 key;      /* ASCII，不可打印为 0 */
    u32 scancode; /* PS/2 scancode */
    u32 flags;    /* ASTER_INPUT_F_*（含 GLOBAL 位） */
};

/* 连 displayd + CREATE + MAP；成功返回 0，失败返回负 errno。 */
static inline i32 aster_app_open(struct aster_app *app,
                                 const struct aster_app_config *cfg)
{
    struct aster_display_create_req req;
    i32 rc;

    if (app == NULL || cfg == NULL) {
        return ASTER_EINVAL;
    }
    aster_memzero(app, (u64)sizeof(*app));
    aster_memzero(&req, (u64)sizeof(req));
    req.width = cfg->width;
    req.height = cfg->height;
    req.flags = cfg->flags;
    req.x = cfg->x;
    req.y = cfg->y;
    req.title = cfg->title;
    req.owner_pid = 0; /* 让 SDK 用 getpid() */

    rc = aster_display_connect(&app->display);
    if (rc < 0) {
        return rc;
    }
    rc = aster_display_create_mapped(&app->display, &req, &app->surface);
    if (rc < 0) {
        aster_display_close(&app->display);
        return rc;
    }
    app->running = 1;
    return 0;
}

/* 提交整个窗口。 */
static inline i32 aster_app_commit(struct aster_app *app)
{
    if (app == NULL) {
        return ASTER_EINVAL;
    }
    return aster_surface_commit_all(&app->display, &app->surface);
}

/*
 * 轮询一个事件：
 *   1  = 有事件，ev 填好
 *   0  = timeout 内没有事件
 *   <0 = 错误（端口关闭 / 协议错）
 */
static inline i32 aster_app_poll(struct aster_app *app, struct aster_event *ev,
                                 u64 timeout)
{
    struct aster_display_input in;
    i32 rc;

    if (app == NULL || ev == NULL) {
        return ASTER_EINVAL;
    }
    aster_memzero(ev, (u64)sizeof(*ev));
    rc = aster_display_poll_input(&app->display, &in, timeout);
    if (rc <= 0) {
        return rc;
    }
    ev->surface_id = in.surface_id;
    ev->key = in.key;
    ev->scancode = in.scancode;
    ev->flags = in.flags;
    if (in.kind == ASTER_INPUT_KEY_DOWN) {
        ev->type = ASTER_EVENT_KEY_DOWN;
    } else if (in.kind == ASTER_INPUT_KEY_UP) {
        ev->type = ASTER_EVENT_KEY_UP;
    } else {
        ev->type = ASTER_EVENT_NONE;
    }
    return 1;
}

static inline int aster_app_running(const struct aster_app *app)
{
    return (app != NULL && app->running != 0) ? 1 : 0;
}

static inline void aster_app_quit(struct aster_app *app)
{
    if (app != NULL) {
        app->running = 0;
    }
}

/* DESTROY + BYE + close port；可重复调用。 */
static inline void aster_app_close(struct aster_app *app)
{
    if (app == NULL) {
        return;
    }
    if (app->surface.id != 0) {
        (void)aster_display_destroy(&app->display, &app->surface);
    }
    aster_display_close(&app->display);
    app->running = 0;
}

/*
 * 默认入口：weak，所以应用可以自己定义 _start 覆盖；多文件应用即使每个 TU
 * 都 include 这个头也不会重复符号（链接器任选一个相同的 weak 定义）。
 * 编译时加 ASTER_NO_DEFAULT_ENTRY 可完全不生成它。
 */
#ifndef ASTER_NO_DEFAULT_ENTRY
__attribute__((weak, naked, noreturn)) void _start(void)
{
    __asm__ volatile("andq $-16, %rsp\n\t"
                     "xorq %rbp, %rbp\n\t"
                     "call aster_app_main\n\t"
                     "movl %eax, %edi\n\t"
                     "movl $1, %eax\n\t"
                     "syscall\n\t"
                     "1: jmp 1b\n\t");
}
#endif /* ASTER_NO_DEFAULT_ENTRY */

#endif /* ASTER_APP_H */
