/*
 * ng WP3-5: S1 故障注入的纯逻辑（tests/s1）。
 *
 * 主机（tests/unit-ng/test_s1.c）和客户机代理（tests/s1/s1fault.c）共用这一份：
 *   1. /run/display.status 的 k=v 解析（只认会话的 s* 字段，0 与"缺失"分开）；
 *   2. 四个场景的步骤表 + 每步的断言求值（WAIT/OK/FAIL），agent 只负责
 *      读状态、执行动作（kill/打印）、按步骤推进；
 *   3. 证据行格式化（`ng: s1 scenario a PASS (step=4)`）。
 *
 * 约束：不 include libc/内核头；只用 src/ng/types.h 的定宽类型，宿主与
 * x86_64-elf 目标都能编译（-ffreestanding -nostdlib）。
 */
#ifndef S1_LOGIC_H
#define S1_LOGIC_H

#include "types.h"

/* /run/display.status 的缓冲上限（displayd 一行约 300 字节，留余量）。 */
#define S1_STATUS_MAX 1024u

/*
 * 会话状态快照：只取 S1 断言需要的 s* 字段（displayd 把 sessiond 的
 * SESSION_STATUS 汇总进已有状态文件，不新建 ramfs 节点）。
 */
struct s1_status {
    u32 sess;
    u32 spid;
    u32 sdpid;
    u32 swmpid;
    u32 swmstate;
    u32 sapps;
    u32 swindows;
    u32 slaunches;
    u32 sreaps;
    u32 srestarts;
    u32 sapppid;
    u32 stokenfail;
    u32 found; /* S1_F_* 位：字段真的出现过（区别于出现且为 0） */
};

#define S1_F_SESS 0x0001u
#define S1_F_SPID 0x0002u
#define S1_F_SDPID 0x0004u
#define S1_F_SWMPID 0x0008u
#define S1_F_SWMSTATE 0x0010u
#define S1_F_SAPPS 0x0020u
#define S1_F_SWINDOWS 0x0040u
#define S1_F_SLAUNCHES 0x0080u
#define S1_F_SREAPS 0x0100u
#define S1_F_SRESTARTS 0x0200u
#define S1_F_SAPPPID 0x0400u
#define S1_F_STOKENFAIL 0x0800u
/* 一条"会话已经起来并跑着第一个应用"的状态至少要有这些字段。 */
#define S1_F_UP                                                              \
    (S1_F_SESS | S1_F_SPID | S1_F_SWMPID | S1_F_SWMSTATE | S1_F_SAPPS |      \
     S1_F_SWINDOWS | S1_F_SAPPPID)

/* 场景模式：a=杀 WM，b=杀输入应用，c=杀 sessiond，d=WM 死循环。 */
enum s1_mode {
    S1_MODE_NONE = 0,
    S1_MODE_WM = 1,
    S1_MODE_INPUT = 2,
    S1_MODE_SESSIOND = 3,
    S1_MODE_SPIN = 4
};

#define S1_STEPS_MAX 4u

/* 步骤求值结果。 */
enum s1_verdict {
    S1_WAIT = 0,
    S1_OK = 1,
    S1_FAIL = 2
};

/* step 成功后 agent 要执行的动作（打印/杀进程/等输入）。 */
enum s1_action {
    S1_ACT_NONE = 0,
    S1_ACT_KILL_WM = 1,
    S1_ACT_KILL_APP = 2,
    S1_ACT_KILL_SESSIOND = 3,
    S1_ACT_ARM_SPIN = 4,
    S1_ACT_INPUT_REQ = 5,
    S1_ACT_DONE = 6
};

/* 步骤开始时从状态里抓的基线（pid/计数），后续步骤与它比较。 */
struct s1_ctx {
    u32 wm_pid;
    u32 app_pid;
    u32 sessiond_pid;
    u32 launches0;
    u32 reaps0;
    u32 restarts0;
    u32 kill_ok; /* 杀 sessiond 的 kill syscall 成功（mode c 用） */
};

/*
 * 解析状态文本。返回找到的字段数（0..12），同时写 out->found。
 * NULL/len=0/out=NULL 一律返回 0（out 为 NULL 时不写）。
 */
u32 s1_status_parse(const char *text, u32 len, struct s1_status *out);

/* "a".."d" -> S1_MODE_*；其它返回 -1。 */
int s1_mode_parse(const char *name);

/* 模式的单字母名；非法模式返回 "?"。 */
const char *s1_mode_name(int mode);

/* 该模式的步骤数；非法模式返回 0。 */
u32 s1_step_count(int mode);

/* 步骤名（证据/失败定位用）；越界返回 NULL。 */
const char *s1_step_name(int mode, u32 step);

/* 该步成功后 agent 的动作；越界/非法返回 S1_ACT_NONE。 */
int s1_step_action(int mode, u32 step);

/* step 0 成功时抓基线。 */
void s1_ctx_capture(struct s1_ctx *ctx, const struct s1_status *st);

/*
 * 断言求值：
 *   S1_WAIT - 条件还不满足（agent 继续轮询）；
 *   S1_OK   - 该步完成（agent 执行 s1_step_action 并进入下一步）；
 *   S1_FAIL - 状态自相矛盾/参数非法（不必再等）。
 * st/ctx 为 NULL 或步骤越界返回 S1_FAIL。
 */
int s1_step_eval(int mode, u32 step, const struct s1_status *st,
                 const struct s1_ctx *ctx);

/* 无符号十进制格式化（不写 NUL 之外的东西）；返回写入长度。 */
u32 s1_u32_dec(char *out, u32 cap, u32 v);

/*
 * 汇总证据行：`ng: s1 scenario <mode> PASS (step=N)` / FAIL。
 * cap 不足时截断；返回实际写入长度（不含 NUL）。
 */
u32 s1_format_result(char *out, u32 cap, int mode, int pass, u32 step);

#endif /* S1_LOGIC_H */
