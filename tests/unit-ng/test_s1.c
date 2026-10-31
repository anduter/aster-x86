/*
 * ng WP3-5 host 单测：S1 故障注入的纯逻辑（tests/s1/s1_logic.c）。
 *
 * 这些函数同时跑在 ring3 的故障注入代理（tests/s1/s1fault.c）里，所以这里
 * 覆盖的是真实客户机断言，而不是"另写一份 host 版"：
 *   1. /run/display.status 的 k=v 解析：完整行、缺字段、0 与缺失、十六进制、
 *      坏数字、重复键、前缀诱饵（pid/apps/windows/restarts 不是 s* 字段）；
 *   2. 场景模式/步骤表/动作表（a=杀 WM、b=杀输入应用、c=杀 sessiond、
 *      d=WM 死循环）；
 *   3. 每步断言：session-up / wm-restart / demo-alive / input-launch /
 *      app-reap / input-recovery / sessiond-dead，含反例；
 *   4. 证据行格式化与无符号十进制。
 */
#include <string.h>

#include "s1_logic.h"
#include "test_util.h"

/* 一条"会话已起来 + 第一个应用在跑"的完整状态行（displayd 汇总）。 */
#define S_UP                                                                  \
    " up=1 pid=1 sess=1 spid=22 sdpid=23 sprocs=2 sapps=1 swindows=2 "       \
    "stokens=2 slaunches=1 skills=0 sexits=0 sreaps=0 srestarts=0 "          \
    "swmpid=24 swmstate=1 sapppid=25 stokenfail=0 sregistry=1\n"

static void parse_into(struct s1_status *st, const char *text)
{
    memset(st, 0, sizeof(*st));
    (void)s1_status_parse(text, (u32)strlen(text), st);
}

/* ------------------------------------------------------------------------- */
/* A. 状态解析                                                               */
/* ------------------------------------------------------------------------- */

static void t_status_parse_full(void)
{
    struct s1_status st;
    u32 n = s1_status_parse(S_UP, (u32)strlen(S_UP), &st);

    CHECK(n == 12u);
    CHECK(st.found == 0x0FFFu);
    CHECK(st.sess == 1u);
    CHECK(st.spid == 22u);
    CHECK(st.sdpid == 23u);
    CHECK(st.swmpid == 24u);
    CHECK(st.swmstate == 1u);
    CHECK(st.sapps == 1u);
    CHECK(st.swindows == 2u);
    CHECK(st.slaunches == 1u);
    CHECK(st.sreaps == 0u);
    CHECK(st.srestarts == 0u);
    CHECK(st.sapppid == 25u);
    CHECK(st.stokenfail == 0u);
    CHECK((st.found & S1_F_UP) == S1_F_UP);
}

static void t_status_parse_decoys(void)
{
    struct s1_status st;
    static const char *text =
        "pid=7 apps=9 windows=8 restarts=5 sessions=2 spid=22 px=0x202838 "
        "sapps=1 swindows=2 sess=1 sdpid=23 swmpid=24 swmstate=1 sapppid=25\n";
    u32 n = s1_status_parse(text, (u32)strlen(text), &st);

    /* pid/apps/windows/restarts/sessions 都不是 s* 字段，不能被误认。 */
    CHECK(n == 8u);
    CHECK(st.spid == 22u);
    CHECK(st.sapps == 1u);
    CHECK(st.swindows == 2u);
    CHECK(st.sess == 1u);
    CHECK(st.sdpid == 23u);
    CHECK(st.swmpid == 24u);
    CHECK(st.swmstate == 1u);
    CHECK(st.sapppid == 25u);
    CHECK(st.sreaps == 0u);
    CHECK((st.found & S1_F_SREAPS) == 0u);
    CHECK((st.found & S1_F_SESS) != 0u);
}

static void t_status_parse_boundaries(void)
{
    struct s1_status st;
    u32 n;

    /* 前缀诱饵：xsess / sesses / sapppidx 都不算。 */
    n = s1_status_parse("xsess=1 sesses=2 sapppidx=3 sess=1", (u32)strlen("xsess=1 sesses=2 sapppidx=3 sess=1"), &st);
    CHECK(n == 1u);
    CHECK(st.sess == 1u);
    CHECK((st.found & S1_F_SESS) != 0u);

    /* 缺少 key 的边界：只有 sess/spid 两个字段。 */
    parse_into(&st, "sess=1 spid=3");
    CHECK(st.found == (S1_F_SESS | S1_F_SPID));
    CHECK((st.found & S1_F_UP) != S1_F_UP);

    /* 出现且为 0 与"缺失"必须分开。 */
    parse_into(&st, "swmstate=0 sapppid=0");
    CHECK((st.found & S1_F_SWMSTATE) != 0u);
    CHECK((st.found & S1_F_SAPPPID) != 0u);
    CHECK(st.swmstate == 0u);
    CHECK(st.sapppid == 0u);

    /* 十六进制 / 前导 0。 */
    parse_into(&st, "srestarts=0x2 sdpid=015 slaunches=0X1f");
    CHECK(st.srestarts == 2u);
    CHECK(st.sdpid == 15u);
    CHECK(st.slaunches == 31u);

    /* 坏数字：整段无效，字段视为缺失。 */
    parse_into(&st, "sapps=abc swindows= sapps2=1");
    CHECK((st.found & S1_F_SAPPS) == 0u);
    CHECK((st.found & S1_F_SWINDOWS) == 0u);

    /* 多一字节/负号/十六进制无数字。 */
    parse_into(&st, "sapps=12x swindows=-1 sapps=0x");
    CHECK((st.found & S1_F_SAPPS) == 0u);
    CHECK((st.found & S1_F_SWINDOWS) == 0u);

    /* 重复 key：最后一次生效，计数只算一次。 */
    n = s1_status_parse("slaunches=3 slaunches=7", (u32)strlen("slaunches=3 slaunches=7"), &st);
    CHECK(n == 1u);
    CHECK(st.slaunches == 7u);

    /* 制表符/回车/换行分隔。 */
    parse_into(&st, "sess=1\tspid=2\r\nswmpid=3\n");
    CHECK(st.found == (S1_F_SESS | S1_F_SPID | S1_F_SWMPID));

    /* 没有 '=' 的 token、空 key、空文本、NULL。 */
    parse_into(&st, "sess 1 =2 = =3");
    CHECK(st.found == 0u);
    CHECK(s1_status_parse(NULL, 0, &st) == 0u);
    CHECK(s1_status_parse("sess=1", 6, NULL) == 0u);
    CHECK(s1_status_parse("", 0, &st) == 0u);
    CHECK(st.found == 0u);

    /* 值里再出现 '='：整段不是纯数字，丢弃。 */
    parse_into(&st, "sapps=1=2");
    CHECK((st.found & S1_F_SAPPS) == 0u);
}

/* ------------------------------------------------------------------------- */
/* B. 模式 / 步骤 / 动作表                                                   */
/* ------------------------------------------------------------------------- */

static void t_mode_table(void)
{
    CHECK(s1_mode_parse("a") == S1_MODE_WM);
    CHECK(s1_mode_parse("b") == S1_MODE_INPUT);
    CHECK(s1_mode_parse("c") == S1_MODE_SESSIOND);
    CHECK(s1_mode_parse("d") == S1_MODE_SPIN);
    CHECK(s1_mode_parse("") == -1);
    CHECK(s1_mode_parse("A") == -1);
    CHECK(s1_mode_parse("e") == -1);
    CHECK(s1_mode_parse("ab") == -1);
    CHECK(s1_mode_parse(NULL) == -1);
    CHECK(strcmp(s1_mode_name(S1_MODE_WM), "a") == 0);
    CHECK(strcmp(s1_mode_name(S1_MODE_INPUT), "b") == 0);
    CHECK(strcmp(s1_mode_name(S1_MODE_SESSIOND), "c") == 0);
    CHECK(strcmp(s1_mode_name(S1_MODE_SPIN), "d") == 0);
    CHECK(strcmp(s1_mode_name(99), "?") == 0);

    CHECK(s1_step_count(S1_MODE_WM) == 4u);
    CHECK(s1_step_count(S1_MODE_INPUT) == 3u);
    CHECK(s1_step_count(S1_MODE_SESSIOND) == 2u);
    CHECK(s1_step_count(S1_MODE_SPIN) == 1u);
    CHECK(s1_step_count(99) == 0u);

    CHECK(strcmp(s1_step_name(S1_MODE_WM, 0), "session-up") == 0);
    CHECK(strcmp(s1_step_name(S1_MODE_WM, 1), "wm-restart") == 0);
    CHECK(strcmp(s1_step_name(S1_MODE_WM, 2), "demo-alive") == 0);
    CHECK(strcmp(s1_step_name(S1_MODE_WM, 3), "input-launch") == 0);
    CHECK(s1_step_name(S1_MODE_WM, 4) == NULL);
    CHECK(strcmp(s1_step_name(S1_MODE_INPUT, 1), "app-reap") == 0);
    CHECK(strcmp(s1_step_name(S1_MODE_INPUT, 2), "input-recovery") == 0);
    CHECK(strcmp(s1_step_name(S1_MODE_SESSIOND, 1), "sessiond-dead") == 0);
    CHECK(strcmp(s1_step_name(S1_MODE_SPIN, 0), "session-up") == 0);
    CHECK(s1_step_name(99, 0) == NULL);

    CHECK(s1_step_action(S1_MODE_WM, 0) == S1_ACT_KILL_WM);
    CHECK(s1_step_action(S1_MODE_WM, 1) == S1_ACT_NONE);
    CHECK(s1_step_action(S1_MODE_WM, 2) == S1_ACT_INPUT_REQ);
    CHECK(s1_step_action(S1_MODE_WM, 3) == S1_ACT_DONE);
    CHECK(s1_step_action(S1_MODE_INPUT, 0) == S1_ACT_KILL_APP);
    CHECK(s1_step_action(S1_MODE_INPUT, 1) == S1_ACT_INPUT_REQ);
    CHECK(s1_step_action(S1_MODE_INPUT, 2) == S1_ACT_DONE);
    CHECK(s1_step_action(S1_MODE_SESSIOND, 0) == S1_ACT_KILL_SESSIOND);
    CHECK(s1_step_action(S1_MODE_SESSIOND, 1) == S1_ACT_DONE);
    CHECK(s1_step_action(S1_MODE_SPIN, 0) == S1_ACT_ARM_SPIN);
    CHECK(s1_step_action(S1_MODE_WM, 9) == S1_ACT_NONE);
    CHECK(s1_step_action(99, 0) == S1_ACT_NONE);
}

/* ------------------------------------------------------------------------- */
/* C. 基线与每步断言                                                         */
/* ------------------------------------------------------------------------- */

static void t_ctx_capture(void)
{
    struct s1_status st;
    struct s1_ctx ctx;

    parse_into(&st, S_UP);
    memset(&ctx, 0xAA, sizeof(ctx));
    s1_ctx_capture(&ctx, &st);
    CHECK(ctx.wm_pid == 24u);
    CHECK(ctx.app_pid == 25u);
    CHECK(ctx.sessiond_pid == 22u);
    CHECK(ctx.launches0 == 1u);
    CHECK(ctx.reaps0 == 0u);
    CHECK(ctx.restarts0 == 0u);
    CHECK(ctx.kill_ok == 0u);

    memset(&ctx, 0xAA, sizeof(ctx));
    s1_ctx_capture(&ctx, NULL);
    CHECK(ctx.wm_pid == 0u && ctx.app_pid == 0u && ctx.kill_ok == 0u);
    s1_ctx_capture(NULL, &st); /* 不能崩 */
}

static void t_eval_wm(void)
{
    struct s1_status st;
    struct s1_ctx ctx;

    parse_into(&st, S_UP);
    s1_ctx_capture(&ctx, &st);
    CHECK(s1_step_eval(S1_MODE_WM, 0, &st, &ctx) == S1_OK);

    /* 缺 sapppid / 状态不是 running / 缺字段都还不能算 up。 */
    parse_into(&st, "sess=1 spid=22 sdpid=23 swmpid=24 swmstate=1 sapps=1 "
                    "swindows=2 slaunches=1\n");
    CHECK(s1_step_eval(S1_MODE_WM, 0, &st, &ctx) == S1_WAIT);
    parse_into(&st, "sess=1 spid=22 sdpid=23 swmpid=24 swmstate=2 sapps=1 "
                    "swindows=2 sapppid=25\n");
    CHECK(s1_step_eval(S1_MODE_WM, 0, &st, &ctx) == S1_WAIT);
    parse_into(&st, "sess=1 spid=22 swmpid=24 sapps=1 swindows=2 "
                    "sapppid=25\n");
    CHECK(s1_step_eval(S1_MODE_WM, 0, &st, &ctx) == S1_WAIT);

    /* step 1：restarts 增加 + wm pid 换了 + running。 */
    parse_into(&st, S_UP);
    st.srestarts = 1u;
    st.swmpid = 26u;
    CHECK(s1_step_eval(S1_MODE_WM, 1, &st, &ctx) == S1_OK);
    st.swmpid = 24u;
    CHECK(s1_step_eval(S1_MODE_WM, 1, &st, &ctx) == S1_WAIT);
    st.swmpid = 26u;
    st.srestarts = 0u;
    CHECK(s1_step_eval(S1_MODE_WM, 1, &st, &ctx) == S1_WAIT);
    st.srestarts = 1u;
    st.swmstate = 2u;
    CHECK(s1_step_eval(S1_MODE_WM, 1, &st, &ctx) == S1_WAIT);

    /* step 2：应用 pid/窗口还在、没被回收。 */
    parse_into(&st, S_UP);
    CHECK(s1_step_eval(S1_MODE_WM, 2, &st, &ctx) == S1_OK);
    st.sreaps = 1u;
    CHECK(s1_step_eval(S1_MODE_WM, 2, &st, &ctx) == S1_WAIT);
    st.sreaps = 0u;
    st.sapppid = 27u;
    CHECK(s1_step_eval(S1_MODE_WM, 2, &st, &ctx) == S1_WAIT);
    st.sapppid = 25u;
    st.swindows = 1u;
    CHECK(s1_step_eval(S1_MODE_WM, 2, &st, &ctx) == S1_WAIT);

    /* step 3：launches 至少 +2（自检 fallback 可能 auto-launch 一次）且 app 变了。 */
    parse_into(&st, S_UP);
    st.slaunches = 3u;
    st.sapppid = 27u;
    CHECK(s1_step_eval(S1_MODE_WM, 3, &st, &ctx) == S1_OK);
    st.slaunches = 2u;
    CHECK(s1_step_eval(S1_MODE_WM, 3, &st, &ctx) == S1_WAIT);
    st.slaunches = 3u;
    st.sapppid = 25u;
    CHECK(s1_step_eval(S1_MODE_WM, 3, &st, &ctx) == S1_WAIT);
    st.sapppid = 27u;
    st.slaunches = 1u;
    CHECK(s1_step_eval(S1_MODE_WM, 3, &st, &ctx) == S1_WAIT);
}

static void t_eval_input(void)
{
    struct s1_status st;
    struct s1_ctx ctx;

    parse_into(&st, S_UP);
    s1_ctx_capture(&ctx, &st);

    /* step 1：reaps 增加，WM 不重启。 */
    st.sreaps = 1u;
    CHECK(s1_step_eval(S1_MODE_INPUT, 1, &st, &ctx) == S1_OK);
    st.srestarts = 1u;
    CHECK(s1_step_eval(S1_MODE_INPUT, 1, &st, &ctx) == S1_WAIT);
    st.srestarts = 0u;
    st.sreaps = 0u;
    CHECK(s1_step_eval(S1_MODE_INPUT, 1, &st, &ctx) == S1_WAIT);

    /* step 2：重新 LAUNCH。 */
    st.slaunches = 2u;
    st.sapppid = 26u;
    CHECK(s1_step_eval(S1_MODE_INPUT, 2, &st, &ctx) == S1_OK);
    st.sapppid = 25u;
    CHECK(s1_step_eval(S1_MODE_INPUT, 2, &st, &ctx) == S1_WAIT);
}

static void t_eval_sessiond(void)
{
    struct s1_status st;
    struct s1_ctx ctx;

    parse_into(&st, S_UP);
    s1_ctx_capture(&ctx, &st);
    ctx.kill_ok = 1u;

    /* sessiond 死后状态停在最后一次 SESSION_STATUS：spid 还是它、没重启。 */
    CHECK(s1_step_eval(S1_MODE_SESSIOND, 1, &st, &ctx) == S1_OK);
    ctx.kill_ok = 0u;
    CHECK(s1_step_eval(S1_MODE_SESSIOND, 1, &st, &ctx) == S1_WAIT);
    ctx.kill_ok = 1u;
    st.spid = 99u;
    CHECK(s1_step_eval(S1_MODE_SESSIOND, 1, &st, &ctx) == S1_WAIT);
    st.spid = 22u;
    st.sess = 0u;
    CHECK(s1_step_eval(S1_MODE_SESSIOND, 1, &st, &ctx) == S1_WAIT);
    st.sess = 1u;
    st.srestarts = 2u;
    CHECK(s1_step_eval(S1_MODE_SESSIOND, 1, &st, &ctx) == S1_WAIT);
    st.srestarts = 0u;
    st.swmstate = 0u;
    CHECK(s1_step_eval(S1_MODE_SESSIOND, 1, &st, &ctx) == S1_WAIT);
}

static void t_eval_guards(void)
{
    struct s1_status st;
    struct s1_ctx ctx;

    parse_into(&st, S_UP);
    s1_ctx_capture(&ctx, &st);
    CHECK(s1_step_eval(S1_MODE_SPIN, 0, &st, &ctx) == S1_OK);
    CHECK(s1_step_eval(S1_MODE_SPIN, 1, &st, &ctx) == S1_FAIL);
    CHECK(s1_step_eval(S1_MODE_WM, 4, &st, &ctx) == S1_FAIL);
    CHECK(s1_step_eval(99, 0, &st, &ctx) == S1_FAIL);
    CHECK(s1_step_eval(S1_MODE_WM, 0, NULL, &ctx) == S1_FAIL);
    CHECK(s1_step_eval(S1_MODE_WM, 0, &st, NULL) == S1_FAIL);
}

/* ------------------------------------------------------------------------- */
/* D. 证据行格式化                                                           */
/* ------------------------------------------------------------------------- */

static void t_format(void)
{
    char buf[64];
    u32 n;

    n = s1_u32_dec(buf, (u32)sizeof(buf), 0u);
    CHECK(n == 1u && strcmp(buf, "0") == 0);
    n = s1_u32_dec(buf, (u32)sizeof(buf), 42u);
    CHECK(n == 2u && strcmp(buf, "42") == 0);
    n = s1_u32_dec(buf, (u32)sizeof(buf), 4294967295u);
    CHECK(n == 10u && strcmp(buf, "4294967295") == 0);
    n = s1_u32_dec(buf, 2u, 12345u);
    CHECK(n == 1u && buf[0] == '1' && buf[1] == '\0');
    CHECK(s1_u32_dec(NULL, 16u, 1u) == 0u);
    CHECK(s1_u32_dec(buf, 0u, 1u) == 0u);

    n = s1_format_result(buf, (u32)sizeof(buf), S1_MODE_WM, 1, 4u);
    CHECK(n == 31u);
    CHECK(strcmp(buf, "ng: s1 scenario a PASS (step=4)") == 0);
    n = s1_format_result(buf, (u32)sizeof(buf), S1_MODE_SESSIOND, 0, 1u);
    CHECK(n == 31u);
    CHECK(strcmp(buf, "ng: s1 scenario c FAIL (step=1)") == 0);
    n = s1_format_result(buf, (u32)sizeof(buf), S1_MODE_SPIN, 0, 0u);
    CHECK(strcmp(buf, "ng: s1 scenario d FAIL (step=0)") == 0);
    CHECK(n == 31u);
    n = s1_format_result(buf, 12u, S1_MODE_WM, 1, 4u);
    CHECK(n == 11u);
    CHECK(strcmp(buf, "ng: s1 scen") == 0);
    CHECK(s1_format_result(NULL, 16u, S1_MODE_WM, 1, 0u) == 0u);
}

int test_s1(void)
{
    t_status_parse_full();
    t_status_parse_decoys();
    t_status_parse_boundaries();
    t_mode_table();
    t_ctx_capture();
    t_eval_wm();
    t_eval_input();
    t_eval_sessiond();
    t_eval_guards();
    t_format();
    return 0;
}
