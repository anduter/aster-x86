/*
 * ng WP3-3 host 单测：session manager（L2c）的纯逻辑。
 *
 *   1. session 协议：body 长度表、编解码往返（REGISTER/HEARTBEAT/LAUNCH/EXIT/
 *      KILL/LOOKUP/ENTRY/LIST/LIST_REPLY/SESSION_STATUS）、长度/版本/magic 拒绝；
 *   2. 角色令牌：make/归一化/valid/role/seq/nonce、token_is；
 *   3. path/arg 打包与 "session=<dec>"/"token=<hex>" argv 解析（含溢出/坏格式）；
 *   4. 会话表：分配（pid/role/令牌）、重复 pid、表满、指定令牌、WM 槽位；
 *   5. 窗口注册/心跳/EXIT/崩溃回收、权限（只有 WM 能 LAUNCH/KILL）；
 *   6. 计数（apps/windows/procs/last_app_pid/tokens）与作废令牌环；
 *   7. 与 displayd LIST 的同步校验（owner_pid/token 三字段、滞后快照、外来令牌）；
 *   8. 重启退避（TSC tick 指数退避、restart_limit）；
 *   9. 状态文本（/run/display.status 的 s* 字段由 displayd 汇总，这里只测格式）。
 */
#include <string.h>

#include "session_protocol.h"
#include "session_table.h"
#include "test_util.h"

/* ------------------------------------------------------------------------- */
/* A. 协议尺寸 / 编解码                                                       */
/* ------------------------------------------------------------------------- */

static void t_proto_sizes(void)
{
    CHECK(SESSION_HDR_SIZE == 20u);
    CHECK(SESSION_IPC_HDR_SIZE == 16u);
    CHECK(sizeof(struct session_ipc_hdr) == 16);
    CHECK(sizeof(struct session_msg) == SESSION_MSG_MAX);
    CHECK(SESSION_MAX_ENTRIES == 8u);
    CHECK(SESSION_PATH_MAX == 64u);
    CHECK(SESSION_ARG_MAX == 16u);
    CHECK(SESSION_TITLE_WORDS == 4u);
    CHECK(sizeof(struct session_register_body) == 28);
    CHECK(sizeof(struct session_heartbeat_body) == 8);
    CHECK(sizeof(struct session_launch_body) == 84);
    CHECK(sizeof(struct session_exit_body) == 8);
    CHECK(sizeof(struct session_kill_body) == 12);
    CHECK(sizeof(struct session_lookup_body) == 8);
    CHECK(sizeof(struct session_entry_body) == 48);
    CHECK(sizeof(struct session_entry_info) == 40);
    CHECK(sizeof(struct session_list_body) == 4);
    CHECK(sizeof(struct session_list_reply_body) == 336);
    CHECK(sizeof(struct session_status_body) == 4);
    CHECK(session_msg_body_size(SESSION_MSG_REGISTER) == 28);
    CHECK(session_msg_body_size(SESSION_MSG_HEARTBEAT) == 8);
    CHECK(session_msg_body_size(SESSION_MSG_LAUNCH) == 84);
    CHECK(session_msg_body_size(SESSION_MSG_EXIT) == 8);
    CHECK(session_msg_body_size(SESSION_MSG_KILL) == 12);
    CHECK(session_msg_body_size(SESSION_MSG_LOOKUP) == 8);
    CHECK(session_msg_body_size(SESSION_MSG_ENTRY) == 48);
    CHECK(session_msg_body_size(SESSION_MSG_LIST) == 4);
    CHECK(session_msg_body_size(SESSION_MSG_LIST_REPLY) == 336);
    CHECK(session_msg_body_size(SESSION_MSG_SESSION_STATUS) == 4);
    CHECK(session_msg_body_size(99) == 0);
    CHECK(session_msg_size(SESSION_MSG_REGISTER) == 48);
    CHECK(session_msg_size(SESSION_MSG_LIST_REPLY) == 356);
    CHECK(session_msg_size(99) == 0);
    CHECK(SESSION_HDR_SIZE + 336u <= SESSION_MSG_MAX);
}

static void t_proto_roundtrip(void)
{
    u8 buf[SESSION_MSG_MAX];
    struct session_msg m;
    struct session_msg d;
    int n;

    /* REGISTER（窗口注册 + 标题） */
    memset(&m, 0, sizeof(m));
    m.type = SESSION_MSG_REGISTER;
    m.seq = 7;
    m.u.reg.token = 0x12345678u;
    m.u.reg.port = 0x30005;
    m.u.reg.surface_id = 3;
    ds_title_set(m.u.reg.title, "Aster Panel");
    n = session_msg_encode(&m, buf, sizeof(buf));
    CHECK(n == (int)session_msg_size(SESSION_MSG_REGISTER));
    CHECK(session_msg_decode(buf, (u32)n, &d) == DS_OK);
    CHECK(d.magic == SESSION_MAGIC);
    CHECK(d.version == SESSION_VERSION);
    CHECK(d.type == SESSION_MSG_REGISTER);
    CHECK(d.len == (u32)n);
    CHECK(d.seq == 7);
    CHECK(d.u.reg.token == 0x12345678u);
    CHECK(d.u.reg.port == 0x30005u);
    CHECK(d.u.reg.surface_id == 3);
    {
        char title[DS_TITLE_MAX];

        ds_title_get(d.u.reg.title, title, (u32)sizeof(title));
        CHECK(strcmp(title, "Aster Panel") == 0);
    }

    /* HEARTBEAT */
    memset(&m, 0, sizeof(m));
    m.type = SESSION_MSG_HEARTBEAT;
    m.u.hb.token = 0x1001u;
    m.u.hb.rounds = 42;
    n = session_msg_encode(&m, buf, sizeof(buf));
    CHECK(n == (int)session_msg_size(SESSION_MSG_HEARTBEAT));
    CHECK(session_msg_decode(buf, (u32)n, &d) == DS_OK);
    CHECK(d.u.hb.token == 0x1001u);
    CHECK(d.u.hb.rounds == 42);

    /* LAUNCH（path + arg） */
    memset(&m, 0, sizeof(m));
    m.type = SESSION_MSG_LAUNCH;
    m.u.launch.token = 0x2002u;
    session_path_set(m.u.launch.path, "/bin/demo");
    session_arg_set(m.u.launch.arg, "window");
    n = session_msg_encode(&m, buf, sizeof(buf));
    CHECK(n == (int)session_msg_size(SESSION_MSG_LAUNCH));
    CHECK(session_msg_decode(buf, (u32)n, &d) == DS_OK);
    CHECK(d.u.launch.token == 0x2002u);
    {
        char path[SESSION_PATH_MAX];
        char arg[SESSION_ARG_MAX];

        session_path_get(d.u.launch.path, path, (u32)sizeof(path));
        session_arg_get(d.u.launch.arg, arg, (u32)sizeof(arg));
        CHECK(strcmp(path, "/bin/demo") == 0);
        CHECK(strcmp(arg, "window") == 0);
    }

    /* EXIT / KILL / LOOKUP */
    memset(&m, 0, sizeof(m));
    m.type = SESSION_MSG_EXIT;
    m.u.exit.token = 0x3003u;
    m.u.exit.code = 9;
    n = session_msg_encode(&m, buf, sizeof(buf));
    CHECK(session_msg_decode(buf, (u32)n, &d) == DS_OK);
    CHECK(d.u.exit.token == 0x3003u && d.u.exit.code == 9);

    memset(&m, 0, sizeof(m));
    m.type = SESSION_MSG_KILL;
    m.u.kill.token = 0x4004u;
    m.u.kill.pid = 18;
    m.u.kill.surface_id = 2;
    n = session_msg_encode(&m, buf, sizeof(buf));
    CHECK(session_msg_decode(buf, (u32)n, &d) == DS_OK);
    CHECK(d.u.kill.token == 0x4004u);
    CHECK(d.u.kill.pid == 18);
    CHECK(d.u.kill.surface_id == 2);

    memset(&m, 0, sizeof(m));
    m.type = SESSION_MSG_LOOKUP;
    m.u.lookup.surface_id = 2;
    m.u.lookup.pid = 18;
    n = session_msg_encode(&m, buf, sizeof(buf));
    CHECK(session_msg_decode(buf, (u32)n, &d) == DS_OK);
    CHECK(d.u.lookup.surface_id == 2 && d.u.lookup.pid == 18);

    /* ENTRY（LOOKUP 的回复） */
    memset(&m, 0, sizeof(m));
    m.type = SESSION_MSG_ENTRY;
    m.u.entry.found = 1;
    session_entry_fill(&m.u.entry.info, 18, 0x5005u, 2, SESSION_STATE_RUNNING,
                       SESSION_ROLE_APP, "Demo");
    n = session_msg_encode(&m, buf, sizeof(buf));
    CHECK(n == (int)session_msg_size(SESSION_MSG_ENTRY));
    CHECK(session_msg_decode(buf, (u32)n, &d) == DS_OK);
    CHECK(d.u.entry.found == 1);
    CHECK(d.u.entry.info.pid == 18);
    CHECK(d.u.entry.info.token == 0x5005u);
    CHECK(d.u.entry.info.surface_id == 2);
    CHECK(d.u.entry.info.state == SESSION_STATE_RUNNING);
    CHECK(d.u.entry.info.role == SESSION_ROLE_APP);

    /* LIST / LIST_REPLY */
    memset(&m, 0, sizeof(m));
    m.type = SESSION_MSG_LIST;
    n = session_msg_encode(&m, buf, sizeof(buf));
    CHECK(n == (int)session_msg_size(SESSION_MSG_LIST));
    CHECK(session_msg_decode(buf, (u32)n, &d) == DS_OK);
    CHECK(d.type == SESSION_MSG_LIST);

    memset(&m, 0, sizeof(m));
    m.type = SESSION_MSG_LIST_REPLY;
    m.u.list_reply.count = 2;
    m.u.list_reply.procs = 2;
    m.u.list_reply.apps = 1;
    m.u.list_reply.windows = 2;
    session_entry_fill(&m.u.list_reply.entries[0], 17, 0x6006u, 1,
                       SESSION_STATE_RUNNING, SESSION_ROLE_WM, "Aster Panel");
    session_entry_fill(&m.u.list_reply.entries[1], 18, 0x7007u, 2,
                       SESSION_STATE_RUNNING, SESSION_ROLE_APP, "Demo");
    n = session_msg_encode(&m, buf, sizeof(buf));
    CHECK(n == (int)session_msg_size(SESSION_MSG_LIST_REPLY));
    CHECK(session_msg_decode(buf, (u32)n, &d) == DS_OK);
    CHECK(d.u.list_reply.count == 2);
    CHECK(d.u.list_reply.procs == 2);
    CHECK(d.u.list_reply.apps == 1);
    CHECK(d.u.list_reply.windows == 2);
    CHECK(d.u.list_reply.entries[0].role == SESSION_ROLE_WM);
    CHECK(d.u.list_reply.entries[1].title[0] != 0);

    /* SESSION_STATUS（保留消息，长度 4） */
    memset(&m, 0, sizeof(m));
    m.type = SESSION_MSG_SESSION_STATUS;
    n = session_msg_encode(&m, buf, sizeof(buf));
    CHECK(n == (int)session_msg_size(SESSION_MSG_SESSION_STATUS));
    CHECK(session_msg_decode(buf, (u32)n, &d) == DS_OK);
}

static void t_proto_reject(void)
{
    u8 buf[SESSION_MSG_MAX];
    struct session_msg m;
    struct session_msg d;
    int n;

    memset(&m, 0, sizeof(m));
    m.type = SESSION_MSG_HEARTBEAT;
    m.u.hb.token = 1;
    n = session_msg_encode(&m, buf, sizeof(buf));
    CHECK(n == (int)session_msg_size(SESSION_MSG_HEARTBEAT));

    /* 截断 / 多一个字节 */
    CHECK(session_msg_decode(buf, (u32)n - 1u, &d) == DS_EINVAL);
    CHECK(session_msg_decode(buf, (u32)n + 1u, &d) == DS_EINVAL);
    /* header 里的 len 与传入不一致 */
    {
        u32 bad = (u32)n + 4u;

        memcpy(buf + 8, &bad, 4);
        CHECK(session_msg_decode(buf, (u32)n, &d) == DS_EINVAL);
    }
    /* 坏 magic */
    memset(&m, 0, sizeof(m));
    m.type = SESSION_MSG_HEARTBEAT;
    n = session_msg_encode(&m, buf, sizeof(buf));
    buf[0] = 0;
    CHECK(session_msg_decode(buf, (u32)n, &d) == DS_EINVAL);
    /* 坏版本 */
    n = session_msg_encode(&m, buf, sizeof(buf));
    buf[4] = 2;
    CHECK(session_msg_decode(buf, (u32)n, &d) == DS_EINVAL);
    /* 未知类型 */
    memset(&m, 0, sizeof(m));
    m.type = 99;
    CHECK(session_msg_encode(&m, buf, sizeof(buf)) == DS_EINVAL);
    CHECK(session_msg_decode(buf, 20, &d) == DS_EINVAL);
    /* cap 不够 / NULL */
    memset(&m, 0, sizeof(m));
    m.type = SESSION_MSG_REGISTER;
    CHECK(session_msg_encode(&m, buf, 20) == DS_EINVAL);
    CHECK(session_msg_encode(NULL, buf, sizeof(buf)) == DS_EINVAL);
    CHECK(session_msg_encode(&m, NULL, sizeof(buf)) == DS_EINVAL);
    CHECK(session_msg_decode(NULL, 20, &d) == DS_EINVAL);
    CHECK(session_msg_decode(buf, 20, NULL) == DS_EINVAL);
    CHECK(session_msg_decode(buf, 19, &d) == DS_EINVAL);
}

/* ------------------------------------------------------------------------- */
/* B. 令牌                                                                    */
/* ------------------------------------------------------------------------- */

static void t_tokens(void)
{
    u32 t = session_token_make(SESSION_ROLE_WM, 3, 0xBEEFu);

    CHECK(t != 0);
    CHECK(SESSION_TOKEN_ROLE(t) == SESSION_ROLE_WM);
    CHECK(SESSION_TOKEN_SEQ(t) == 3u);
    CHECK(SESSION_TOKEN_NONCE(t) == 0xBEEFu);
    CHECK(session_token_valid(t) == 1);
    CHECK(session_token_is(t, SESSION_ROLE_WM) == 1);
    CHECK(session_token_is(t, SESSION_ROLE_APP) == 0);
    CHECK(session_token_is(0, SESSION_ROLE_WM) == 0);
    CHECK(session_token_valid(0) == 0);
    CHECK(session_token_valid(0x00000100u) == 0); /* nonce=0 */
    CHECK(session_token_valid(0x12340000u) == 0); /* seq=0 */
    CHECK(session_token_valid(0x12340500u) == 0); /* role=5 未知 */
    /* 归一化：role=0 -> APP、seq=0 -> 1、nonce=0 -> 1 */
    t = session_token_make(0, 0, 0);
    CHECK(SESSION_TOKEN_ROLE(t) == SESSION_ROLE_APP);
    CHECK(SESSION_TOKEN_SEQ(t) == 1u);
    CHECK(SESSION_TOKEN_NONCE(t) == 1u);
    CHECK(session_token_valid(t) == 1);
    /* 角色全集 */
    CHECK(session_token_valid(session_token_make(SESSION_ROLE_APP, 1, 1)) == 1);
    CHECK(session_token_valid(session_token_make(SESSION_ROLE_SERVICE, 1, 1)) ==
          1);
    CHECK(session_token_valid(session_token_make(SESSION_ROLE_SESSION, 1, 1)) ==
          1);
    /* 不同 nonce/seq -> 不同令牌 */
    CHECK(session_token_make(SESSION_ROLE_APP, 1, 1) !=
          session_token_make(SESSION_ROLE_APP, 1, 2));
    CHECK(session_token_make(SESSION_ROLE_APP, 1, 1) !=
          session_token_make(SESSION_ROLE_APP, 2, 1));
    CHECK(session_token_make(SESSION_ROLE_APP, 1, 1) !=
          session_token_make(SESSION_ROLE_WM, 1, 1));
}

/* ------------------------------------------------------------------------- */
/* C. path / arg / argv                                                       */
/* ------------------------------------------------------------------------- */

static void t_path_arg(void)
{
    u32 words[SESSION_PATH_MAX / 4u];
    u32 argw[SESSION_ARG_MAX / 4u];
    char buf[SESSION_PATH_MAX + 8];
    char s[200];
    u32 i;
    u32 v = 0;

    session_path_set(words, "/bin/demo");
    session_path_get(words, buf, (u32)sizeof(buf));
    CHECK(strcmp(buf, "/bin/demo") == 0);
    session_path_set(words, NULL);
    session_path_get(words, buf, (u32)sizeof(buf));
    CHECK(buf[0] == '\0');
    session_path_set(words, "");
    session_path_get(words, buf, (u32)sizeof(buf));
    CHECK(buf[0] == '\0');
    /* 超长截断到 63 字符 + NUL */
    for (i = 0; i < 150u; i++) {
        s[i] = 'a';
    }
    s[150] = '\0';
    session_path_set(words, s);
    session_path_get(words, buf, (u32)sizeof(buf));
    CHECK(strlen(buf) == (size_t)(SESSION_PATH_MAX - 1u));
    /* cap 截断 */
    session_path_set(words, "/bin/demo");
    session_path_get(words, buf, 4);
    CHECK(strcmp(buf, "/bi") == 0);
    buf[0] = '\0';
    session_path_get(words, buf, 0);
    CHECK(buf[0] == '\0'); /* cap=0 不写 */

    session_arg_set(argw, "window");
    session_arg_get(argw, buf, (u32)sizeof(buf));
    CHECK(strcmp(buf, "window") == 0);
    session_arg_set(argw, "0123456789abcdef"); /* 15 字符上限 */
    session_arg_get(argw, buf, (u32)sizeof(buf));
    CHECK(strlen(buf) == (size_t)(SESSION_ARG_MAX - 1u));
    session_arg_get(argw, NULL, 10);
    session_arg_set(argw, NULL);
    session_arg_get(argw, buf, (u32)sizeof(buf));
    CHECK(buf[0] == '\0');

    /* argv 项解析 */
    CHECK(session_arg_parse_u32("session=1048576", "session=", &v) == 1);
    CHECK(v == 1048576u);
    CHECK(session_arg_parse_u32("token=0xdeadbeef", "token=", &v) == 1);
    CHECK(v == 0xDEADBEEFu);
    CHECK(session_arg_parse_u32("token=0X2A", "token=", &v) == 1);
    CHECK(v == 42u);
    CHECK(session_arg_parse_u32("token=42", "token=", &v) == 1);
    CHECK(v == 42u);
    CHECK(session_arg_parse_u32("token=0x", "token=", &v) == 0);
    CHECK(session_arg_parse_u32("token=", "token=", &v) == 0);
    CHECK(session_arg_parse_u32("token=12z", "token=", &v) == 0);
    CHECK(session_arg_parse_u32("session=1", "token=", &v) == 0);
    CHECK(session_arg_parse_u32("", "token=", &v) == 0);
    CHECK(session_arg_parse_u32(NULL, "token=", &v) == 0);
    CHECK(session_arg_parse_u32("token=1", NULL, &v) == 0);
    CHECK(session_arg_parse_u32("token=1", "token=", NULL) == 0);
    CHECK(session_arg_parse_u32("token=0xFFFFFFFF", "token=", &v) == 1);
    CHECK(v == 0xFFFFFFFFu);
    CHECK(session_arg_parse_u32("token=0x100000000", "token=", &v) == 0);
    CHECK(session_arg_parse_u32("token=99999999999", "token=", &v) == 0);
    CHECK(session_arg_parse_u32("session=007", "session=", &v) == 1);
    CHECK(v == 7u);
}

/* ------------------------------------------------------------------------- */
/* D. 会话表                                                                  */
/* ------------------------------------------------------------------------- */

static void t_table_alloc(void)
{
    struct session_table t;
    u32 tok = 0;
    u32 tok2 = 0;
    int slot;
    u32 i;

    session_table_init(&t);
    CHECK(t.count == 0);
    CHECK(t.wm_slot == SESSION_SLOT_NONE);
    CHECK(session_wm(&t) == NULL);
    CHECK(session_by_pid(&t, 17) == NULL);
    CHECK(session_by_token(&t, 0) == NULL);
    CHECK(session_at(&t, 0) == NULL);

    slot = session_alloc(&t, 17, SESSION_ROLE_WM, 100, &tok);
    CHECK(slot == 0);
    CHECK(tok != 0);
    CHECK(SESSION_TOKEN_ROLE(tok) == SESSION_ROLE_WM);
    CHECK(session_by_pid(&t, 17) != NULL);
    CHECK(session_by_token(&t, tok) != NULL);
    CHECK(session_wm(&t) != NULL && session_wm(&t)->pid == 17);
    CHECK(session_wm(&t)->state == SESSION_STATE_RUNNING);
    CHECK(session_wm(&t)->last_seen == 100);
    CHECK(t.count == 1);
    CHECK(session_procs(&t) == 1);
    CHECK(session_apps(&t) == 0);
    CHECK(session_tokens(&t) == 1);

    /* 重复 pid / 非法参数 */
    CHECK(session_alloc(&t, 17, SESSION_ROLE_APP, 1, &tok2) == DS_EBUSY);
    CHECK(session_alloc(&t, 0, SESSION_ROLE_APP, 1, &tok2) == DS_EINVAL);
    CHECK(session_alloc(&t, 18, SESSION_ROLE_NONE, 1, &tok2) == DS_EINVAL);
    CHECK(session_alloc(&t, 18, SESSION_ROLE_SESSION, 1, &tok2) == DS_EINVAL);
    CHECK(session_alloc(NULL, 18, SESSION_ROLE_APP, 1, &tok2) == DS_EINVAL);

    /* 应用 */
    slot = session_alloc(&t, 18, SESSION_ROLE_APP, 200, &tok2);
    CHECK(slot == 1);
    CHECK(session_token_is(tok2, SESSION_ROLE_APP) == 1);
    CHECK(tok != tok2);
    CHECK(t.count == 2);
    CHECK(session_apps(&t) == 1);
    CHECK(session_last_app_pid(&t) == 18);
    CHECK(session_tokens(&t) == 2);

    /* 表满 */
    for (i = 2; i < SESSION_MAX_ENTRIES; i++) {
        u32 tmp = 0;

        CHECK(session_alloc(&t, 20 + i, SESSION_ROLE_APP, 1, &tmp) >= 0);
    }
    CHECK(t.count == SESSION_MAX_ENTRIES);
    CHECK(session_alloc(&t, 99, SESSION_ROLE_APP, 1, &tok2) == DS_ENOSPC);

    /* 指定令牌登记 */
    {
        struct session_table t2;
        u32 fixed = session_token_make(SESSION_ROLE_APP, 9, 0x4242u);

        session_table_init(&t2);
        CHECK(session_alloc_with_token(&t2, 30, SESSION_ROLE_APP, 1, fixed) ==
              0);
        CHECK(session_by_pid(&t2, 30) != NULL);
        CHECK(session_by_pid(&t2, 30)->token == fixed);
        CHECK(session_alloc_with_token(&t2, 31, SESSION_ROLE_APP, 1, fixed) ==
              DS_EBUSY);
        CHECK(session_alloc_with_token(&t2, 31, SESSION_ROLE_WM, 1, fixed) ==
              DS_EINVAL); /* 角色不匹配 */
        CHECK(session_alloc_with_token(&t2, 31, SESSION_ROLE_APP, 1, 0) ==
              DS_EINVAL);
        CHECK(session_alloc_with_token(NULL, 31, SESSION_ROLE_APP, 1, fixed) ==
              DS_EINVAL);
    }
}

static void t_table_lifecycle(void)
{
    struct session_table t;
    u32 wm_tok = 0;
    u32 app_tok = 0;

    session_table_init(&t);
    CHECK(session_alloc(&t, 17, SESSION_ROLE_WM, 10, &wm_tok) == 0);
    CHECK(session_alloc(&t, 18, SESSION_ROLE_APP, 10, &app_tok) == 1);

    /* 窗口注册 */
    CHECK(session_register_window(&t, wm_tok, 0x30001, 1, "Aster Panel", 15) ==
          DS_OK);
    CHECK(session_by_token(&t, wm_tok)->surface_id == 1);
    CHECK(session_by_token(&t, wm_tok)->port == 0x30001u);
    CHECK(strcmp(session_by_token(&t, wm_tok)->title, "Aster Panel") == 0);
    CHECK(session_by_token(&t, wm_tok)->last_seen == 15);
    CHECK(session_windows(&t) == 1);
    CHECK(session_by_surface(&t, 1) != NULL);
    CHECK(session_by_surface(&t, 0) == NULL);

    CHECK(session_register_window(&t, app_tok, 0x30002, 2, "Demo", 16) ==
          DS_OK);
    CHECK(session_windows(&t) == 2);
    /* 同一个 surface 不能属于两个会话项 */
    CHECK(session_register_window(&t, app_tok, 0x30003, 1, "Dup", 17) ==
          DS_EBUSY);
    /* 未知令牌 / NULL 标题 */
    CHECK(session_register_window(&t, 0xDEADu, 0, 9, "x", 1) == DS_ENOENT);
    CHECK(session_register_window(&t, app_tok, 0, 0, NULL, 1) == DS_OK);
    CHECK(session_by_token(&t, app_tok)->surface_id == 0);

    /* 心跳 */
    CHECK(session_heartbeat(&t, app_tok, 99) == DS_OK);
    CHECK(session_by_token(&t, app_tok)->heartbeats == 1);
    CHECK(session_by_token(&t, app_tok)->last_seen == 99);
    CHECK(session_heartbeat(&t, 0xDEADu, 1) == DS_ENOENT);
    CHECK(session_heartbeat(NULL, 1, 1) == DS_ENOENT);

    /* 权限：只有 WM 能 LAUNCH/KILL */
    CHECK(session_launch_allowed(&t, wm_tok) == DS_OK);
    CHECK(session_launch_allowed(&t, app_tok) == DS_EPERM);
    CHECK(session_launch_allowed(&t, 0xDEADu) == DS_ENOENT);
    CHECK(session_launch_allowed(&t, 0) == DS_ENOENT);
    CHECK(session_launch_allowed(NULL, wm_tok) == DS_ENOENT);
    CHECK(session_kill_allowed(&t, wm_tok) == DS_OK);
    CHECK(session_kill_allowed(&t, app_tok) == DS_EPERM);
    CHECK(t.rejects >= 2);

    /* EXIT */
    CHECK(session_mark_exit(&t, app_tok, 0) == DS_OK);
    CHECK(session_by_token(&t, app_tok)->state == SESSION_STATE_EXITED);
    CHECK(session_by_token(&t, app_tok)->exit_code == 0);
    CHECK(t.exits == 1);
    CHECK(session_apps(&t) == 0);
    CHECK(session_heartbeat(&t, app_tok, 1) == DS_EPERM);
    CHECK(session_register_window(&t, app_tok, 0, 5, "x", 1) == DS_EPERM);
    CHECK(session_launch_allowed(&t, app_tok) == DS_EPERM);
    /* 重复 EXIT 不重复计数 */
    CHECK(session_mark_exit(&t, app_tok, 1) == DS_OK);
    CHECK(t.exits == 1);

    /* 崩溃：mark_dead 按 pid */
    CHECK(session_mark_dead(&t, 17) == DS_OK);
    CHECK(session_by_pid(&t, 17)->state == SESSION_STATE_DEAD);
    CHECK(session_mark_dead(&t, 17) == DS_OK);
    CHECK(session_mark_dead(&t, 77) == DS_ENOENT);
    CHECK(session_launch_allowed(&t, wm_tok) == DS_EPERM); /* 已死 */
    CHECK(t.reaps == 0); /* WM 不计入应用回收数 */

    /* 应用崩溃回收计数 */
    {
        struct session_table t2;
        u32 a = 0;

        session_table_init(&t2);
        CHECK(session_alloc(&t2, 40, SESSION_ROLE_APP, 1, &a) == 0);
        CHECK(session_mark_dead(&t2, 40) == DS_OK);
        CHECK(t2.reaps == 1);
        CHECK(session_apps(&t2) == 0);
    }
}

/* ------------------------------------------------------------------------- */
/* E. 注册表 <-> displayd LIST 同步                                           */
/* ------------------------------------------------------------------------- */

static void build_list(struct ds_list_entry *l, u32 n, u32 id, u32 owner,
                       u32 token)
{
    memset(l, 0, sizeof(*l));
    l->id = id;
    l->owner_pid = owner;
    l->token = token;
    (void)n;
}

static void t_sync(void)
{
    struct session_table t;
    struct ds_list_entry list[DS_LIST_MAX];
    u32 wm_tok = 0;
    u32 app_tok = 0;
    u32 bound = 0;
    u32 mismatch;

    session_table_init(&t);
    (void)session_alloc(&t, 17, SESSION_ROLE_WM, 1, &wm_tok);
    (void)session_alloc(&t, 18, SESSION_ROLE_APP, 1, &app_tok);
    (void)session_register_window(&t, wm_tok, 0, 1, "Aster Panel", 1);
    (void)session_register_window(&t, app_tok, 0, 2, "Demo", 1);

    /* 完全一致 */
    build_list(&list[0], 2, 1, 17, wm_tok);
    build_list(&list[1], 2, 2, 18, app_tok);
    mismatch = session_sync_list(&t, list, 2, &bound);
    CHECK(mismatch == 0);
    CHECK(bound == 2);
    CHECK(t.registry_ok == 1);
    CHECK(t.registry_syncs == 1);
    CHECK(t.token_fail == 0);

    /* owner_pid 不一致（绑定检查与反向令牌检查各计一次） */
    build_list(&list[1], 2, 2, 99, app_tok);
    bound = 0;
    CHECK(session_sync_list(&t, list, 2, &bound) == 2);
    CHECK(bound == 1);
    CHECK(t.token_fail == 2);
    CHECK(t.registry_ok == 0);

    /* token 不一致（绑定 + 外来令牌） */
    build_list(&list[1], 2, 2, 18, 0xBEEFu);
    CHECK(session_sync_list(&t, list, 2, &bound) == 2);
    CHECK(t.token_fail == 4);

    /* 外来令牌（会话表里没有） */
    build_list(&list[0], 2, 1, 17, wm_tok);
    build_list(&list[1], 2, 2, 18, 0xCAFEu);
    CHECK(session_sync_list(&t, list, 2, &bound) == 2);
    CHECK(t.token_fail == 6);

    /* 窗口不在 LIST 里（滞后快照/未创建）：不清引用、不算 mismatch */
    build_list(&list[0], 1, 1, 17, wm_tok);
    bound = 0;
    CHECK(session_sync_list(&t, list, 1, &bound) == 0);
    CHECK(bound == 1);
    CHECK(session_by_token(&t, app_tok)->surface_id == 2);
    CHECK(session_windows(&t) == 2);
    CHECK(t.registry_ok == 1);

    /* token=0 的旧客户端窗口不参与令牌校验 */
    build_list(&list[0], 2, 1, 17, 0);
    build_list(&list[1], 2, 2, 18, 0);
    bound = 0;
    CHECK(session_sync_list(&t, list, 2, &bound) == 2); /* 两个绑定都不符 */
    CHECK(t.registry_ok == 0);

    /* 作废令牌：WM 重启后旧面板的滞后快照不算外来令牌 */
    build_list(&list[0], 2, 1, 17, wm_tok);
    (void)session_mark_dead(&t, 17); /* WM 死：它的会话项不再参与绑定校验 */
    session_retire_token(&t, wm_tok);
    CHECK(session_token_retired(&t, wm_tok) == 1);
    CHECK(session_token_retired(&t, 0) == 0);
    CHECK(session_token_retired(&t, app_tok) == 0);
    build_list(&list[1], 2, 2, 18, app_tok);
    bound = 0;
    CHECK(session_sync_list(&t, list, 2, &bound) == 0);
    CHECK(bound == 1); /* 只剩应用窗口参与绑定校验 */
    CHECK(t.registry_ok == 1);

    /* 空 LIST / NULL LIST */
    CHECK(session_sync_list(&t, NULL, 0, &bound) == 0);
    CHECK(session_sync_list(&t, list, 0, &bound) == 0);
    CHECK(bound == 0);
    CHECK(session_sync_list(NULL, list, 2, &bound) == 0);
    CHECK(session_sync_list(&t, list, 2, NULL) == 0);
}

static void t_sync_count_clamp(void)
{
    struct session_table t;
    struct ds_list_entry many[99];
    u32 wm_tok = 0;
    u32 app_tok = 0;
    u32 bound = 0;
    u32 i;

    session_table_init(&t);
    (void)session_alloc(&t, 17, SESSION_ROLE_WM, 1, &wm_tok);
    (void)session_alloc(&t, 18, SESSION_ROLE_APP, 1, &app_tok);
    (void)session_register_window(&t, wm_tok, 0, 1, "Aster Panel", 1);
    (void)session_register_window(&t, app_tok, 0, 2, "Demo", 1);

    memset(many, 0, sizeof(many));
    build_list(&many[0], 99, 1, 17, wm_tok);
    build_list(&many[1], 99, 2, 18, app_tok);
    /* 尾部放一堆"外来令牌"：如果 count 没被钳到 DS_LIST_MAX，就会越界读
     * 这些 entry 并把 token_fail 抬上去（审计发现：IPC body 的 count 是
     * 攻击者可控的 u32）。 */
    for (i = DS_LIST_MAX; i < 99u; i++) {
        build_list(&many[i], 99, 100u + i, 999u, 0xDEAD0000u | i);
    }
    CHECK(session_sync_list(&t, many, 99, &bound) == 0);
    CHECK(bound == 2);
    CHECK(t.token_fail == 0);
    CHECK(t.registry_ok == 1);

    /* 边界再确认：恰好 8 条、0 条都正常。 */
    CHECK(session_sync_list(&t, many, DS_LIST_MAX, &bound) == 0);
    CHECK(bound == 2);
    CHECK(session_sync_list(&t, many, 0, &bound) == 0);
    CHECK(bound == 0);
}

static void t_retire_ring(void)
{
    struct session_table t;
    u32 i;
    u32 toks[6];

    session_table_init(&t);
    for (i = 0; i < 6u; i++) {
        toks[i] = session_token_make(SESSION_ROLE_APP, (u32)(i + 1u),
                                     (u32)(0x10u + i));
        session_retire_token(&t, toks[i]);
    }
    /* 环形缓冲保留最近 4 个（toks[2..5]），更早的被挤掉 */
    CHECK(session_token_retired(&t, toks[0]) == 0);
    CHECK(session_token_retired(&t, toks[1]) == 0);
    CHECK(session_token_retired(&t, toks[2]) == 1);
    CHECK(session_token_retired(&t, toks[3]) == 1);
    CHECK(session_token_retired(&t, toks[4]) == 1);
    CHECK(session_token_retired(&t, toks[5]) == 1);
    session_retire_token(&t, 0); /* 0 忽略 */
    session_retire_token(NULL, toks[0]);
    CHECK(session_token_retired(NULL, toks[3]) == 0);
}

/* ------------------------------------------------------------------------- */
/* F. 重启退避 / 状态文本                                                     */
/* ------------------------------------------------------------------------- */

static void t_backoff(void)
{
    u64 d = 0;

    CHECK(session_backoff_ticks(0) == SESSION_BACKOFF_BASE);
    CHECK(session_backoff_ticks(1) == SESSION_BACKOFF_BASE);
    CHECK(session_backoff_ticks(2) == SESSION_BACKOFF_BASE * 2u);
    CHECK(session_backoff_ticks(3) == SESSION_BACKOFF_BASE * 4u);
    CHECK(session_backoff_ticks(4) == SESSION_BACKOFF_CAP);
    CHECK(session_backoff_ticks(9) == SESSION_BACKOFF_CAP);
    CHECK(session_backoff_ticks(100) == SESSION_BACKOFF_CAP);

    CHECK(session_should_restart(0, 3, &d) == 1);
    CHECK(d == SESSION_BACKOFF_BASE);
    CHECK(session_should_restart(1, 3, &d) == 1);
    CHECK(d == SESSION_BACKOFF_BASE * 2u);
    CHECK(session_should_restart(2, 3, &d) == 1);
    CHECK(d == SESSION_BACKOFF_BASE * 4u);
    CHECK(session_should_restart(3, 3, &d) == 0);
    CHECK(d == 0);
    CHECK(session_should_restart(4, 3, NULL) == 0);
    CHECK(session_should_restart(0, 3, NULL) == 1);
    CHECK(SESSION_RESTART_LIMIT == 3u);
}

static void t_status_text(void)
{
    struct session_table t;
    char buf[320];
    u32 wm_tok = 0;
    u32 app_tok = 0;
    u32 len;

    session_table_init(&t);
    (void)session_alloc(&t, 17, SESSION_ROLE_WM, 1, &wm_tok);
    (void)session_alloc(&t, 18, SESSION_ROLE_APP, 1, &app_tok);
    (void)session_register_window(&t, wm_tok, 0, 1, "Aster Panel", 1);
    (void)session_register_window(&t, app_tok, 0, 2, "Demo", 1);
    t.launches = 1;

    len = session_status_format(&t, 15, 16, 17, 1, 0, buf, (u32)sizeof(buf));
    CHECK(len > 0);
    CHECK(strstr(buf, "up=1") != NULL);
    CHECK(strstr(buf, " pid=15") != NULL);
    CHECK(strstr(buf, " dpid=16") != NULL);
    CHECK(strstr(buf, " procs=2") != NULL);
    CHECK(strstr(buf, " apps=1") != NULL);
    CHECK(strstr(buf, " windows=2") != NULL);
    CHECK(strstr(buf, " tokens=2") != NULL);
    CHECK(strstr(buf, " launches=1") != NULL);
    CHECK(strstr(buf, " restarts=0") != NULL);
    CHECK(strstr(buf, " wmpid=17") != NULL);
    CHECK(strstr(buf, " wmstate=1") != NULL);
    CHECK(strstr(buf, " apppid=18") != NULL);
    CHECK(strstr(buf, " tokenfail=0") != NULL);
    CHECK(buf[len - 1] == '\n');
    CHECK(buf[len] == '\0');

    /* 截断：不越界、仍有 NUL */
    len = session_status_format(&t, 1, 2, 3, 2, 1, buf, 24);
    CHECK(len <= 23);
    CHECK(buf[len] == '\0');
    CHECK(session_status_format(NULL, 1, 2, 3, 0, 0, buf, (u32)sizeof(buf)) >
          0);
    CHECK(session_status_format(&t, 1, 2, 3, 0, 0, NULL, 10) == 0);
    CHECK(session_status_format(&t, 1, 2, 3, 0, 0, buf, 0) == 0);
}

/* ------------------------------------------------------------------------- */
/* 入口                                                                       */
/* ------------------------------------------------------------------------- */

int test_session(void)
{
    t_proto_sizes();
    t_proto_roundtrip();
    t_proto_reject();
    t_tokens();
    t_path_arg();
    t_table_alloc();
    t_table_lifecycle();
    t_sync();
    t_sync_count_clamp();
    t_retire_ring();
    t_backoff();
    t_status_text();
    return 0;
}
