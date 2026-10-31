/*
 * ng WP3-4: Aster SDK alpha 的宿主单测（纯函数/线格式，不跑 QEMU）。
 *
 * 覆盖：
 *   - sdk/include/aster/display.h：协议 v3 尺寸表、CREATE/LIST/FOCUS 的 token
 *     编解码、v1/v2 兼容解码、SESSION_STATUS、拒绝路径；
 *   - sdk/include/aster/session.h：会话协议尺寸/编解码、role_token、argv
 *     `session=`/`token=` 解析、path/arg 打包、ENTRY/LIST_REPLY 解析。
 *
 * 为什么不在 tests/unit-ng/unit_main.c 里挂：WP3-5（S1 故障注入）并行会话
 * 拥有 unit_main.c / tools/unit-ng.ps1；本文件由 tools/sdk-check.ps1 用宿主
 * clang 单独编译运行（输出 "sdk-test: N checks, 0 failures"），避免同文件
 * 冲突。它不 include 任何内核头，只 include SDK 头 + 宿主 stdio。
 *
 * 运行：powershell -NoProfile -File tools/sdk-check.ps1
 */
#include <stdio.h>

#define ASTER_NO_DEFAULT_ENTRY /* 宿主测试有 main()，不要 app.h 的 _start */
#include <aster/app.h>
#include <aster/display.h>
#include <aster/session.h>

static int g_checks;
static int g_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                    \
    } while (0)

#define CHECK_EQ_U32(a, b)                                                   \
    do {                                                                     \
        u32 check_a = (u32)(a);                                              \
        u32 check_b = (u32)(b);                                              \
        g_checks++;                                                          \
        if (check_a != check_b) {                                            \
            g_failures++;                                                    \
            printf("FAIL %s:%d: %s=%u != %s=%u\n", __FILE__, __LINE__, #a,   \
                   check_a, #b, check_b);                                    \
        }                                                                    \
    } while (0)

/* 写线格式头（测试手工构造 v1/v2 消息用；SDK 自身只发 v3）。 */
static void test_put32(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFFu);
    p[1] = (u8)((v >> 8) & 0xFFu);
    p[2] = (u8)((v >> 16) & 0xFFu);
    p[3] = (u8)((v >> 24) & 0xFFu);
}

static u32 test_get32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}

static void test_put_header(u8 *buf, u32 magic, u16 version, u16 type,
                            u32 total, u32 seq)
{
    test_put32(buf + 0, magic);
    buf[4] = (u8)(version & 0xFFu);
    buf[5] = (u8)(version >> 8);
    buf[6] = (u8)(type & 0xFFu);
    buf[7] = (u8)(type >> 8);
    test_put32(buf + 8, total);
    test_put32(buf + 12, 0);
    test_put32(buf + 16, seq);
}

/* ------------------------------------------------------------------------- */
/* display v3                                                                 */
/* ------------------------------------------------------------------------- */

static void test_display_sizes(void)
{
    CHECK_EQ_U32(ASTER_DISPLAY_VERSION_SEND, 3);
    CHECK_EQ_U32(ASTER_DISPLAY_VERSION_MIN, 1);
    CHECK_EQ_U32(ASTER_DISPLAY_VERSION_MAX, 3);
    CHECK_EQ_U32(ASTER_DISPLAY_HDR_SIZE, 20);
    CHECK_EQ_U32(sizeof(struct aster_display_msg), 512);
    CHECK_EQ_U32(sizeof(struct aster_display_create_body), 44);
    CHECK_EQ_U32(sizeof(struct aster_display_list_entry), 48);
    CHECK_EQ_U32(sizeof(struct aster_display_list_reply_body), 392);
    CHECK_EQ_U32(sizeof(struct aster_display_session_status_body), 68);

    CHECK_EQ_U32(aster_display__body_size(ASTER_DISPLAY_MSG_CREATE, 1), 16);
    CHECK_EQ_U32(aster_display__body_size(ASTER_DISPLAY_MSG_CREATE, 2), 40);
    CHECK_EQ_U32(aster_display__body_size(ASTER_DISPLAY_MSG_CREATE, 3), 44);
    CHECK_EQ_U32(aster_display__body_size(ASTER_DISPLAY_MSG_CREATE, 0), 0);
    CHECK_EQ_U32(aster_display__body_size(ASTER_DISPLAY_MSG_CREATE, 4), 0);
    CHECK_EQ_U32(aster_display__body_size(ASTER_DISPLAY_MSG_LIST, 1), 0);
    CHECK_EQ_U32(aster_display__body_size(ASTER_DISPLAY_MSG_LIST, 2), 4);
    CHECK_EQ_U32(aster_display__body_size(ASTER_DISPLAY_MSG_LIST, 3), 4);
    CHECK_EQ_U32(aster_display__body_size(ASTER_DISPLAY_MSG_LIST_REPLY, 2),
                 360);
    CHECK_EQ_U32(aster_display__body_size(ASTER_DISPLAY_MSG_LIST_REPLY, 3),
                 392);
    CHECK_EQ_U32(aster_display__body_size(ASTER_DISPLAY_MSG_FOCUS, 1), 8);
    CHECK_EQ_U32(aster_display__body_size(ASTER_DISPLAY_MSG_FOCUS, 3), 8);
    CHECK_EQ_U32(
        aster_display__body_size(ASTER_DISPLAY_MSG_SESSION_STATUS, 2), 0);
    CHECK_EQ_U32(
        aster_display__body_size(ASTER_DISPLAY_MSG_SESSION_STATUS, 3), 68);
    CHECK_EQ_U32(aster_display__body_size(0xFFFFu, 3), 0);
}

static void test_display_create_v3(void)
{
    struct aster_display_msg m;
    struct aster_display_msg out;
    struct aster_display_create_req req;
    u8 buf[ASTER_DISPLAY_MSG_MAX];
    i32 n;

    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_DISPLAY_MSG_CREATE;
    m.seq = 7;
    m.u.create.w = 320;
    m.u.create.h = 200;
    m.u.create.flags = ASTER_DISPLAY_CREATE_F_POS;
    m.u.create.x = 10;
    m.u.create.y = 20;
    m.u.create.owner_pid = 42;
    m.u.create.token = 0x553B0203u;
    aster_display_title_set(m.u.create.title, "Aster SDK");

    n = aster_display_encode(&m, buf, (u32)sizeof(buf));
    CHECK_EQ_U32(n, 64);
    CHECK_EQ_U32(test_get32(buf + 0), ASTER_DISPLAY_MAGIC);
    CHECK_EQ_U32(test_get32(buf + 8), 64);
    CHECK_EQ_U32(buf[4], 3);
    CHECK_EQ_U32(buf[7], 0);
    CHECK_EQ_U32(test_get32(buf + 60), 0x553B0203u);
    CHECK_EQ_U32(test_get32(buf + 20), 320);

    CHECK(aster_display_decode(buf, (u32)n, &out) == 0);
    CHECK_EQ_U32(out.type, ASTER_DISPLAY_MSG_CREATE);
    CHECK_EQ_U32(out.u.create.token, 0x553B0203u);
    CHECK_EQ_U32(out.u.create.owner_pid, 42);
    CHECK_EQ_U32(out.u.create.x, 10);
    CHECK_EQ_U32(out.u.create.y, 20);

    /* 结构体路径：create_req.token 必须落到线格式 +40。 */
    aster_memzero(&req, (u64)sizeof(req));
    req.width = 64;
    req.height = 48;
    req.title = "t";
    req.token = 0x11223344u;
    CHECK_EQ_U32(req.token, 0x11223344u);
    CHECK_EQ_U32((u32)sizeof(req), (u32)sizeof(struct aster_display_create_req));
}

static void test_display_v1_v2_decode(void)
{
    struct aster_display_msg out;
    u8 buf[ASTER_DISPLAY_MSG_MAX];

    /* v2 CREATE：40 字节 body（无 token），title 仍然解码。 */
    aster_memzero(buf, (u64)sizeof(buf));
    test_put_header(buf, ASTER_DISPLAY_MAGIC, 2, ASTER_DISPLAY_MSG_CREATE, 60,
                    3);
    test_put32(buf + 20, 100); /* w */
    test_put32(buf + 24, 50);  /* h */
    test_put32(buf + 28, 0);   /* flags */
    test_put32(buf + 32, 0);   /* x */
    test_put32(buf + 36, 0);   /* y */
    test_put32(buf + 40, 9);   /* owner_pid */
    test_put32(buf + 44, 0x44434241u); /* "ABCD" */
    CHECK(aster_display_decode(buf, 60, &out) == 0);
    CHECK_EQ_U32(out.version, 2);
    CHECK_EQ_U32(out.u.create.w, 100);
    CHECK_EQ_U32(out.u.create.owner_pid, 9);
    CHECK_EQ_U32(out.u.create.token, 0);
    {
        char title[ASTER_DISPLAY_TITLE_MAX];

        aster_display_title_get(out.u.create.title, title,
                                (u32)sizeof(title));
        CHECK(title[0] == 'A' && title[1] == 'B' && title[2] == 'C' &&
              title[3] == 'D');
    }

    /* v1 CREATE：16 字节 body（w,h,flags,reserved）。 */
    aster_memzero(buf, (u64)sizeof(buf));
    test_put_header(buf, ASTER_DISPLAY_MAGIC, 1, ASTER_DISPLAY_MSG_CREATE, 36,
                    4);
    test_put32(buf + 20, 16);
    test_put32(buf + 24, 12);
    test_put32(buf + 28, 0);
    test_put32(buf + 32, 0);
    CHECK(aster_display_decode(buf, 36, &out) == 0);
    CHECK_EQ_U32(out.version, 1);
    CHECK_EQ_U32(out.u.create.w, 16);
    CHECK_EQ_U32(out.u.create.h, 12);
    CHECK_EQ_U32(out.u.create.token, 0);
}

static void test_display_focus(void)
{
    struct aster_display_msg m;
    struct aster_display_msg out;
    u8 buf[ASTER_DISPLAY_MSG_MAX];
    i32 n;

    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_DISPLAY_MSG_FOCUS;
    m.u.focus.surface_id = 2;
    m.u.focus.token = 0x21950102u;
    n = aster_display_encode(&m, buf, (u32)sizeof(buf));
    CHECK_EQ_U32(n, 28);
    CHECK_EQ_U32(test_get32(buf + 20), 2);
    CHECK_EQ_U32(test_get32(buf + 24), 0x21950102u);
    CHECK(aster_display_decode(buf, (u32)n, &out) == 0);
    CHECK_EQ_U32(out.u.focus.token, 0x21950102u);
}

static void test_display_list_reply(void)
{
    struct aster_display_msg m;
    struct aster_display_msg out;
    struct aster_display_list list;
    u8 buf[ASTER_DISPLAY_MSG_MAX];
    i32 n;

    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_DISPLAY_MSG_LIST_REPLY;
    m.u.list_reply.count = 2;
    m.u.list_reply.entries[0].id = 1;
    m.u.list_reply.entries[0].owner_pid = 17;
    m.u.list_reply.entries[0].focused = 1;
    m.u.list_reply.entries[0].token = 0x21950102u;
    aster_display_title_set(m.u.list_reply.entries[0].title, "Panel");
    m.u.list_reply.entries[1].id = 2;
    m.u.list_reply.entries[1].owner_pid = 18;
    m.u.list_reply.entries[1].token = 0x553B0203u;
    aster_display_title_set(m.u.list_reply.entries[1].title, "Demo");

    n = aster_display_encode(&m, buf, (u32)sizeof(buf));
    CHECK_EQ_U32(n, 412);
    CHECK_EQ_U32(test_get32(buf + 20), 2);
    CHECK_EQ_U32(test_get32(buf + 28), 1);          /* entry0 id */
    CHECK_EQ_U32(test_get32(buf + 28 + 28), 0x21950102u); /* entry0 token */
    CHECK_EQ_U32(test_get32(buf + 28 + 48 + 28), 0x553B0203u);
    CHECK(aster_display_decode(buf, (u32)n, &out) == 0);
    CHECK(aster_display_parse_list_reply(&out, &list) == 0);
    CHECK_EQ_U32(list.count, 2);
    CHECK_EQ_U32(list.entries[0].token, 0x21950102u);
    CHECK_EQ_U32(list.entries[1].token, 0x553B0203u);
    CHECK(list.entries[1].title[0] == 'D');

    /* v2 LIST_REPLY：entry 44、无 token。手工构造 1 条。 */
    aster_memzero(buf, (u64)sizeof(buf));
    test_put_header(buf, ASTER_DISPLAY_MAGIC, 2,
                    ASTER_DISPLAY_MSG_LIST_REPLY, 380, 5);
    test_put32(buf + 20, 1); /* count */
    test_put32(buf + 24, 0);
    test_put32(buf + 28, 6);  /* id */
    test_put32(buf + 32, 19); /* owner */
    test_put32(buf + 36, 0);
    test_put32(buf + 40, 0);
    test_put32(buf + 44, 10);
    test_put32(buf + 48, 20);
    test_put32(buf + 52, 0); /* focused */
    test_put32(buf + 56, 0x44434241u); /* title "ABCD" */
    CHECK(aster_display_decode(buf, 380, &out) == 0);
    CHECK(aster_display_parse_list_reply(&out, &list) == 0);
    CHECK_EQ_U32(list.count, 1);
    CHECK_EQ_U32(list.entries[0].id, 6);
    CHECK_EQ_U32(list.entries[0].token, 0);
    CHECK(list.entries[0].title[0] == 'A');
}

static void test_display_session_status(void)
{
    struct aster_display_msg m;
    struct aster_display_msg out;
    u8 buf[ASTER_DISPLAY_MSG_MAX];
    i32 n;

    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_DISPLAY_MSG_SESSION_STATUS;
    m.u.session_status.pid = 15;
    m.u.session_status.dpid = 16;
    m.u.session_status.procs = 2;
    m.u.session_status.apps = 1;
    m.u.session_status.windows = 2;
    m.u.session_status.tokens = 2;
    m.u.session_status.launches = 1;
    m.u.session_status.restarts = 1;
    m.u.session_status.wmpid = 19;
    m.u.session_status.wmstate = 1;
    m.u.session_status.apppid = 18;
    m.u.session_status.registry = 1;

    n = aster_display_encode(&m, buf, (u32)sizeof(buf));
    CHECK_EQ_U32(n, 88);
    CHECK_EQ_U32(test_get32(buf + 20), 15);
    CHECK_EQ_U32(test_get32(buf + 80), 1); /* registry at body+60 */
    CHECK(aster_display_decode(buf, (u32)n, &out) == 0);
    CHECK_EQ_U32(out.u.session_status.dpid, 16);
    CHECK_EQ_U32(out.u.session_status.wmpid, 19);
    CHECK_EQ_U32(out.u.session_status.apppid, 18);
}

static void test_display_reject(void)
{
    struct aster_display_msg m;
    struct aster_display_msg out;
    u8 buf[ASTER_DISPLAY_MSG_MAX];
    i32 n;

    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_DISPLAY_MSG_CREATE;
    m.u.create.w = 1;
    m.u.create.h = 1;
    n = aster_display_encode(&m, buf, (u32)sizeof(buf));
    CHECK_EQ_U32(n, 64);

    CHECK(aster_display_decode(buf, 63, &out) != 0);  /* 截断 */
    CHECK(aster_display_decode(buf, 65, &out) != 0);  /* 多一字节 */
    buf[0] = 0;
    CHECK(aster_display_decode(buf, 64, &out) != 0);  /* 坏 magic */
    buf[0] = (u8)(ASTER_DISPLAY_MAGIC & 0xFFu);
    buf[4] = 4;
    CHECK(aster_display_decode(buf, 64, &out) != 0);  /* 坏版本 */
    buf[4] = 3;
    test_put32(buf + 8, 60);
    CHECK(aster_display_decode(buf, 64, &out) != 0);  /* header len 不符 */
    test_put32(buf + 8, 64);
    test_put32(buf + 6, 0xFFFFu);
    CHECK(aster_display_decode(buf, 64, &out) != 0);  /* 未知类型 */

    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_DISPLAY_MSG_CREATE;
    m.u.create.w = 1;
    m.u.create.h = 1;
    CHECK(aster_display_encode(&m, buf, 63) != 0);    /* cap 不够 */
    CHECK(aster_display_encode(NULL, buf, 64) != 0);
    CHECK(aster_display_encode(&m, NULL, 64) != 0);
    CHECK(aster_display_decode(NULL, 64, &out) != 0);
    CHECK(aster_display_decode(buf, 64, NULL) != 0);
}

static void test_display_title(void)
{
    u32 words[ASTER_DISPLAY_TITLE_WORDS];
    char out[ASTER_DISPLAY_TITLE_MAX];
    char tiny[2];

    aster_display_title_set(words, "Aster SDK");
    aster_display_title_get(words, out, (u32)sizeof(out));
    CHECK(out[0] == 'A' && out[8] == 'K' && out[9] == '\0');

    aster_display_title_set(words, "0123456789abcdef"); /* 恰好 16 字符 */
    aster_display_title_get(words, out, (u32)sizeof(out));
    CHECK(out[14] == 'e' && out[15] == '\0'); /* 15 字符 + NUL */

    aster_display_title_set(words, NULL);
    aster_display_title_get(words, out, (u32)sizeof(out));
    CHECK(out[0] == '\0');

    aster_display_title_set(words, "abc");
    aster_display_title_get(words, tiny, (u32)sizeof(tiny));
    CHECK(tiny[0] == 'a' && tiny[1] == '\0');
    aster_display_title_get(words, out, 0); /* 不写、不崩 */
}

/* ------------------------------------------------------------------------- */
/* session                                                                    */
/* ------------------------------------------------------------------------- */

static void test_session_sizes(void)
{
    CHECK_EQ_U32(ASTER_SESSION_VERSION, 1);
    CHECK_EQ_U32(sizeof(struct aster_session_msg), 512);
    CHECK_EQ_U32(sizeof(struct aster_session_register_body), 28);
    CHECK_EQ_U32(sizeof(struct aster_session_heartbeat_body), 8);
    CHECK_EQ_U32(sizeof(struct aster_session_launch_body), 84);
    CHECK_EQ_U32(sizeof(struct aster_session_exit_body), 8);
    CHECK_EQ_U32(sizeof(struct aster_session_kill_body), 12);
    CHECK_EQ_U32(sizeof(struct aster_session_lookup_body), 8);
    CHECK_EQ_U32(sizeof(struct aster_session_entry_info), 40);
    CHECK_EQ_U32(sizeof(struct aster_session_entry_body), 48);
    CHECK_EQ_U32(sizeof(struct aster_session_list_reply_body), 336);

    CHECK_EQ_U32(aster_session_msg_size(ASTER_SESSION_MSG_REGISTER), 48);
    CHECK_EQ_U32(aster_session_msg_size(ASTER_SESSION_MSG_HEARTBEAT), 28);
    CHECK_EQ_U32(aster_session_msg_size(ASTER_SESSION_MSG_LAUNCH), 104);
    CHECK_EQ_U32(aster_session_msg_size(ASTER_SESSION_MSG_EXIT), 28);
    CHECK_EQ_U32(aster_session_msg_size(ASTER_SESSION_MSG_KILL), 32);
    CHECK_EQ_U32(aster_session_msg_size(ASTER_SESSION_MSG_LOOKUP), 28);
    CHECK_EQ_U32(aster_session_msg_size(ASTER_SESSION_MSG_ENTRY), 68);
    CHECK_EQ_U32(aster_session_msg_size(ASTER_SESSION_MSG_LIST), 24);
    CHECK_EQ_U32(aster_session_msg_size(ASTER_SESSION_MSG_LIST_REPLY), 356);
    CHECK_EQ_U32(aster_session_msg_size(ASTER_SESSION_MSG_SESSION_STATUS), 24);
    CHECK_EQ_U32(aster_session_msg_size(0xFFFFu), 0);
}

static void test_session_roundtrip(void)
{
    struct aster_session_msg m;
    struct aster_session_msg out;
    u8 buf[ASTER_SESSION_MSG_MAX];
    i32 n;

    /* REGISTER */
    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_SESSION_MSG_REGISTER;
    m.seq = 3;
    m.u.reg.token = 0x553B0203u;
    m.u.reg.port = 1048576;
    m.u.reg.surface_id = 2;
    aster_session_title_set(m.u.reg.title, "Aster SDK");
    n = aster_session_encode(&m, buf, (u32)sizeof(buf));
    CHECK_EQ_U32(n, 48);
    CHECK_EQ_U32(test_get32(buf + 0), ASTER_SESSION_MAGIC);
    CHECK_EQ_U32(test_get32(buf + 20), 0x553B0203u);
    CHECK(aster_session_decode(buf, (u32)n, &out) == 0);
    CHECK_EQ_U32(out.u.reg.surface_id, 2);
    CHECK_EQ_U32(out.u.reg.port, 1048576);

    /* HEARTBEAT */
    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_SESSION_MSG_HEARTBEAT;
    m.u.hb.token = 7;
    m.u.hb.rounds = 10;
    n = aster_session_encode(&m, buf, (u32)sizeof(buf));
    CHECK_EQ_U32(n, 28);
    CHECK(aster_session_decode(buf, (u32)n, &out) == 0);
    CHECK_EQ_U32(out.u.hb.rounds, 10);

    /* LAUNCH：path/arg 打包 + 回读 */
    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_SESSION_MSG_LAUNCH;
    m.u.launch.token = 0x21950102u;
    aster_session_path_set(m.u.launch.path, "/bin/demo");
    aster_session_arg_set(m.u.launch.arg, "window");
    n = aster_session_encode(&m, buf, (u32)sizeof(buf));
    CHECK_EQ_U32(n, 104);
    CHECK(aster_session_decode(buf, (u32)n, &out) == 0);
    {
        char path[ASTER_SESSION_PATH_MAX];
        char arg[ASTER_SESSION_ARG_MAX];

        aster_session_path_get(out.u.launch.path, path, (u32)sizeof(path));
        aster_session_arg_get(out.u.launch.arg, arg, (u32)sizeof(arg));
        CHECK(aster_streq(path, "/bin/demo"));
        CHECK(aster_streq(arg, "window"));
    }

    /* EXIT / KILL / LOOKUP / LIST / STATUS */
    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_SESSION_MSG_EXIT;
    m.u.exit.token = 5;
    m.u.exit.code = 9;
    n = aster_session_encode(&m, buf, (u32)sizeof(buf));
    CHECK_EQ_U32(n, 28);
    CHECK(aster_session_decode(buf, (u32)n, &out) == 0);
    CHECK_EQ_U32(out.u.exit.code, 9);

    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_SESSION_MSG_KILL;
    m.u.kill.token = 1;
    m.u.kill.pid = 18;
    m.u.kill.surface_id = 2;
    n = aster_session_encode(&m, buf, (u32)sizeof(buf));
    CHECK_EQ_U32(n, 32);
    CHECK(aster_session_decode(buf, (u32)n, &out) == 0);
    CHECK_EQ_U32(out.u.kill.pid, 18);

    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_SESSION_MSG_LOOKUP;
    m.u.lookup.surface_id = 2;
    m.u.lookup.pid = 18;
    n = aster_session_encode(&m, buf, (u32)sizeof(buf));
    CHECK_EQ_U32(n, 28);
    CHECK(aster_session_decode(buf, (u32)n, &out) == 0);
    CHECK_EQ_U32(out.u.lookup.pid, 18);

    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_SESSION_MSG_LIST;
    n = aster_session_encode(&m, buf, (u32)sizeof(buf));
    CHECK_EQ_U32(n, 24);
    CHECK(aster_session_decode(buf, (u32)n, &out) == 0);

    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_SESSION_MSG_SESSION_STATUS;
    n = aster_session_encode(&m, buf, (u32)sizeof(buf));
    CHECK_EQ_U32(n, 24);
    CHECK(aster_session_decode(buf, (u32)n, &out) == 0);
}

static void test_session_entry_and_list(void)
{
    struct aster_session_msg m;
    struct aster_session_msg out;
    struct aster_session_entry ent;
    struct aster_session_list list;
    u8 buf[ASTER_SESSION_MSG_MAX];
    i32 n;

    /* ENTRY found=1 */
    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_SESSION_MSG_ENTRY;
    m.u.entry.found = 1;
    m.u.entry.info.pid = 18;
    m.u.entry.info.token = 0x553B0203u;
    m.u.entry.info.surface_id = 2;
    m.u.entry.info.state = ASTER_SESSION_STATE_RUNNING;
    m.u.entry.info.role = ASTER_SESSION_ROLE_APP;
    aster_session_title_set(m.u.entry.info.title, "Demo");
    n = aster_session_encode(&m, buf, (u32)sizeof(buf));
    CHECK_EQ_U32(n, 68);
    CHECK(aster_session_decode(buf, (u32)n, &out) == 0);
    CHECK(aster_session_parse_entry(&out, &ent) == 0);
    CHECK_EQ_U32(ent.pid, 18);
    CHECK_EQ_U32(ent.token, 0x553B0203u);
    CHECK_EQ_U32(ent.role, ASTER_SESSION_ROLE_APP);
    CHECK(ent.title[0] == 'D');

    /* ENTRY found=0 -> -ENOENT；status 透传 */
    out.u.entry.found = 0;
    CHECK(aster_session_parse_entry(&out, &ent) == ASTER_SESSION_ENOENT);
    out.u.entry.found = 1;
    out.status = ASTER_SESSION_EPERM;
    CHECK(aster_session_parse_entry(&out, &ent) == ASTER_SESSION_EPERM);
    CHECK(aster_session_parse_entry(NULL, &ent) != 0);

    /* LIST_REPLY：2 条 + 计数 */
    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_SESSION_MSG_LIST_REPLY;
    m.u.list_reply.count = 2;
    m.u.list_reply.procs = 2;
    m.u.list_reply.apps = 1;
    m.u.list_reply.windows = 2;
    m.u.list_reply.entries[0].pid = 17;
    m.u.list_reply.entries[0].role = ASTER_SESSION_ROLE_WM;
    m.u.list_reply.entries[0].surface_id = 1;
    aster_session_title_set(m.u.list_reply.entries[0].title, "Panel");
    m.u.list_reply.entries[1].pid = 18;
    m.u.list_reply.entries[1].role = ASTER_SESSION_ROLE_APP;
    m.u.list_reply.entries[1].surface_id = 2;
    m.u.list_reply.entries[1].token = 0x553B0203u;
    aster_session_title_set(m.u.list_reply.entries[1].title, "Aster SDK");
    n = aster_session_encode(&m, buf, (u32)sizeof(buf));
    CHECK_EQ_U32(n, 356);
    CHECK(aster_session_decode(buf, (u32)n, &out) == 0);
    CHECK(aster_session_parse_list_reply(&out, &list) == 0);
    CHECK_EQ_U32(list.count, 2);
    CHECK_EQ_U32(list.procs, 2);
    CHECK_EQ_U32(list.apps, 1);
    CHECK_EQ_U32(list.windows, 2);
    CHECK(aster_streq(list.entries[0].title, "Panel"));
    CHECK(aster_streq(list.entries[1].title, "Aster SDK"));

    /* count 超上限时截断到 8，不越界。 */
    out.u.list_reply.count = 99;
    CHECK(aster_session_parse_list_reply(&out, &list) == 0);
    CHECK_EQ_U32(list.count, ASTER_SESSION_LIST_MAX);
    CHECK(aster_session_parse_list_reply(NULL, &list) != 0);
}

static void test_session_reject(void)
{
    struct aster_session_msg m;
    struct aster_session_msg out;
    u8 buf[ASTER_SESSION_MSG_MAX];
    i32 n;

    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_SESSION_MSG_HEARTBEAT;
    n = aster_session_encode(&m, buf, (u32)sizeof(buf));
    CHECK_EQ_U32(n, 28);

    CHECK(aster_session_decode(buf, 27, &out) != 0);
    CHECK(aster_session_decode(buf, 29, &out) != 0);
    buf[0] = 0;
    CHECK(aster_session_decode(buf, 28, &out) != 0);
    buf[0] = (u8)(ASTER_SESSION_MAGIC & 0xFFu);
    buf[4] = 2;
    CHECK(aster_session_decode(buf, 28, &out) != 0);
    buf[4] = 1;
    test_put32(buf + 8, 24);
    CHECK(aster_session_decode(buf, 28, &out) != 0);
    test_put32(buf + 8, 28);
    test_put32(buf + 6, 0xFFFFu);
    CHECK(aster_session_decode(buf, 28, &out) != 0);
    CHECK(aster_session_encode(&m, buf, 27) != 0);
    CHECK(aster_session_encode(NULL, buf, 28) != 0);
    CHECK(aster_session_decode(NULL, 28, &out) != 0);
    CHECK(aster_session_decode(buf, 28, NULL) != 0);
}

static void test_session_token(void)
{
    u32 t = aster_session_token_make(ASTER_SESSION_ROLE_APP, 3, 0x1234);

    CHECK_EQ_U32(ASTER_SESSION_TOKEN_ROLE(t), ASTER_SESSION_ROLE_APP);
    CHECK_EQ_U32(ASTER_SESSION_TOKEN_SEQ(t), 3);
    CHECK_EQ_U32(ASTER_SESSION_TOKEN_NONCE(t), 0x1234);
    CHECK(aster_session_token_valid(t) == 1);
    CHECK(aster_session_token_is(t, ASTER_SESSION_ROLE_APP) == 1);
    CHECK(aster_session_token_is(t, ASTER_SESSION_ROLE_WM) == 0);

    /* 0 永远非法；非法位型归一化。 */
    CHECK(aster_session_token_valid(0) == 0);
    t = aster_session_token_make(0, 0, 0);
    CHECK_EQ_U32(ASTER_SESSION_TOKEN_ROLE(t), ASTER_SESSION_ROLE_APP);
    CHECK(aster_session_token_valid(t) == 1);
    CHECK(aster_session_token_valid(aster_session_token_make(5, 1, 1)) == 0);
    CHECK(aster_session_token_valid(aster_session_token_make(1, 1, 1)) == 1);
    CHECK(aster_session_token_is(0, ASTER_SESSION_ROLE_WM) == 0);

    CHECK(aster_session_token_make(1, 1, 1) != aster_session_token_make(1, 2, 1));
    CHECK(aster_session_token_make(1, 1, 1) != aster_session_token_make(2, 1, 1));
    CHECK(aster_session_token_make(1, 1, 1) != aster_session_token_make(1, 1, 2));
}

static void test_session_parse_u32(void)
{
    u32 v = 0;

    CHECK(aster_session_parse_u32("session=1048576", "session=", &v) == 1);
    CHECK_EQ_U32(v, 1048576);
    CHECK(aster_session_parse_u32("token=0x553b0203", "token=", &v) == 1);
    CHECK_EQ_U32(v, 0x553B0203u);
    CHECK(aster_session_parse_u32("token=0XABCDEF", "token=", &v) == 1);
    CHECK_EQ_U32(v, 0xABCDEFu);
    CHECK(aster_session_parse_u32("token=0007", "token=", &v) == 1);
    CHECK_EQ_U32(v, 7);
    CHECK(aster_session_parse_u32("token=0", "token=", &v) == 1);
    CHECK_EQ_U32(v, 0);

    CHECK(aster_session_parse_u32("token=", "token=", &v) == 0);
    CHECK(aster_session_parse_u32("token=0x", "token=", &v) == 0);
    CHECK(aster_session_parse_u32("token=12x", "token=", &v) == 0);
    CHECK(aster_session_parse_u32("token=-1", "token=", &v) == 0);
    CHECK(aster_session_parse_u32("session=1", "token=", &v) == 0);
    CHECK(aster_session_parse_u32("token=0x10000000", "token=", &v) == 1);
    CHECK_EQ_U32(v, 0x10000000u);
    CHECK(aster_session_parse_u32("token=0x100000000", "token=", &v) == 0);
    CHECK(aster_session_parse_u32("token=4294967296", "token=", &v) == 0);
    CHECK(aster_session_parse_u32("token=4294967295", "token=", &v) == 1);
    CHECK_EQ_U32(v, 0xFFFFFFFFu);
    CHECK(aster_session_parse_u32(NULL, "token=", &v) == 0);
    CHECK(aster_session_parse_u32("token=1", NULL, &v) == 0);
    CHECK(aster_session_parse_u32("token=1", "token=", NULL) == 0);
}

static void test_session_parse_args(void)
{
    struct aster_session s;
    char *argv1[] = { (char *)"/bin/demo", (char *)"window",
                      (char *)"session=1048576", (char *)"token=0x553b0203",
                      NULL };
    char *argv2[] = { (char *)"/bin/demo", (char *)"window", NULL };
    char *argv3[] = { (char *)"/bin/demo", (char *)"session=", NULL };
    char *argv4[] = { (char *)"/bin/demo", (char *)"token=0x10", NULL };

    CHECK(aster_session_parse_args(&s, 4, argv1) == 1);
    CHECK_EQ_U32(s.server_ticket, 1048576);
    CHECK_EQ_U32(s.token, 0x553B0203u);

    CHECK(aster_session_parse_args(&s, 2, argv2) == 0);
    CHECK_EQ_U32(s.server_ticket, 0);
    CHECK_EQ_U32(s.token, 0);
    CHECK_EQ_U32(s.connected, 0);

    CHECK(aster_session_parse_args(&s, 2, argv3) == ASTER_SESSION_EINVAL);
    CHECK(aster_session_parse_args(&s, 2, argv4) == 0);
    CHECK_EQ_U32(s.token, 0x10u);
    CHECK_EQ_U32(s.server_ticket, 0);

    CHECK(aster_session_parse_args(&s, 0, NULL) == 0);
    CHECK(aster_session_parse_args(NULL, 0, NULL) == ASTER_SESSION_EINVAL);
}

static void test_session_path_arg(void)
{
    u32 path[ASTER_SESSION_PATH_MAX / 4u];
    u32 arg[ASTER_SESSION_ARG_MAX / 4u];
    char out[ASTER_SESSION_PATH_MAX];
    char out2[ASTER_SESSION_ARG_MAX];

    aster_session_path_set(path, NULL);
    aster_session_path_get(path, out, (u32)sizeof(out));
    CHECK(out[0] == '\0');

    aster_session_path_set(path, "/bin/demo");
    aster_session_path_get(path, out, (u32)sizeof(out));
    CHECK(aster_streq(out, "/bin/demo"));

    /* 超长截断到 63 字符 + NUL。 */
    {
        char long_path[90];
        u32 i;

        for (i = 0; i < 80u; i++) {
            long_path[i] = 'a';
        }
        long_path[80] = '\0';
        aster_session_path_set(path, long_path);
        aster_session_path_get(path, out, (u32)sizeof(out));
        CHECK_EQ_U32(aster_strlen(out), ASTER_SESSION_PATH_MAX - 1u);
    }

    /* cap 截断 / cap=0 不写。 */
    aster_session_path_set(path, "/bin/demo");
    aster_session_path_get(path, out, 4);
    CHECK(out[0] == '/' && out[3] == '\0');
    aster_session_path_get(path, out, 0);

    aster_session_arg_set(arg, "window");
    aster_session_arg_get(arg, out2, (u32)sizeof(out2));
    CHECK(aster_streq(out2, "window"));
    aster_session_arg_set(arg, NULL);
    aster_session_arg_get(arg, out2, (u32)sizeof(out2));
    CHECK(out2[0] == '\0');
}

static void test_session_connect_unavailable(void)
{
    struct aster_session s;
    i32 rc = aster_session_connect(&s, 0, NULL);

    /* 宿主上没有会话端口：connect 必须干净返回 -ENOENT 且不执行 syscall。 */
    CHECK_EQ_U32(rc, ASTER_SESSION_ENOENT);
    CHECK_EQ_U32(s.connected, 0);
    CHECK_EQ_U32(s.server_ticket, 0);
}

int main(void)
{
    test_display_sizes();
    test_display_create_v3();
    test_display_v1_v2_decode();
    test_display_focus();
    test_display_list_reply();
    test_display_session_status();
    test_display_reject();
    test_display_title();

    test_session_sizes();
    test_session_roundtrip();
    test_session_entry_and_list();
    test_session_reject();
    test_session_token();
    test_session_parse_u32();
    test_session_parse_args();
    test_session_path_arg();
    test_session_connect_unavailable();

    printf("sdk-test: %d checks, %d failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
