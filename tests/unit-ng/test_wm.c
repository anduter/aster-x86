/*
 * ng WP3-2 host 单测：shell/WM（L2b）纯逻辑 + display 协议 v2。
 *
 * 覆盖：
 *   - 协议 v2（ds_protocol.c）：CREATE title/owner_pid/位置、LIST/LIST_REPLY、
 *     REGISTER_HOTKEY 的长度与往返；v1 CREATE（16 字节 body）兼容；版本拒绝；
 *     ds_title_set/get、ds_list_entry_set；
 *   - surface 表（ds_surface.c）：ds_table_create_ex 的标题/指定位置/贴边、
 *     ds_table_fill_list 的顺序与 focused 位；
 *   - 面板布局（ds_wm.c）：1280x32 / 640 / 320 的 task_max、按钮矩形、
 *     文本截断、面板绘制（背景/强调线/启动器/任务按钮/通知点/标题/时钟）；
 *   - 启动器：/bin 过滤 shell/displayd/wm、/System/Packages 只列目录、
 *     去重、排序（键盘 1..9 的确定性）、表满、可启动下标；
 *   - 任务列表：LIST_REPLY 解析、exclude self、next 回绕/焦点不在表里；
 *   - 热键状态机：Tab / 1..9 / 其它、计数、GLOBAL 位。
 */
#include <string.h>

#include "ds_draw.h"
#include "ds_protocol.h"
#include "ds_surface.h"
#include "ds_wm.h"
#include "test_util.h"

/* ------------------------------------------------------------------------- */
/* A. 协议 v2                                                                 */
/* ------------------------------------------------------------------------- */

static void t_proto_sizes(void)
{
    CHECK(ds_msg_body_size(DS_MSG_CREATE) == 40);
    CHECK(ds_msg_body_size_v(DS_MSG_CREATE, 1) == 16);
    CHECK(ds_msg_body_size_v(DS_MSG_CREATE, 2) == 40);
    CHECK(ds_msg_body_size(DS_MSG_CREATE_REPLY) == 28);
    CHECK(ds_msg_body_size(DS_MSG_LIST) == 4);
    CHECK(ds_msg_body_size_v(DS_MSG_LIST, 1) == 0);
    CHECK(ds_msg_body_size(DS_MSG_LIST_REPLY) == 360);
    CHECK(ds_msg_body_size_v(DS_MSG_LIST_REPLY, 1) == 0);
    CHECK(ds_msg_body_size(DS_MSG_REGISTER_HOTKEY) == 16);
    CHECK(ds_msg_body_size_v(DS_MSG_REGISTER_HOTKEY, 1) == 0);
    CHECK(ds_msg_body_size(DS_MSG_WM_STATUS) == 28);
    CHECK(ds_msg_body_size_v(DS_MSG_WM_STATUS, 1) == 0);
    CHECK(ds_msg_size(DS_MSG_WM_STATUS) == DS_MSG_HDR_SIZE + 28);
    CHECK(ds_msg_body_size_v(DS_MSG_FOCUS, 1) == 8);
    CHECK(ds_msg_body_size_v(DS_MSG_FOCUS, 2) == 8);
    CHECK(ds_msg_body_size_v(DS_MSG_FOCUS, 0) == 0);
    CHECK(ds_msg_body_size_v(DS_MSG_FOCUS, 3) == 0);
    CHECK(ds_msg_body_size(99) == 0);
    CHECK(ds_msg_size(DS_MSG_CREATE) == DS_MSG_HDR_SIZE + 40);
    CHECK(ds_msg_size(DS_MSG_LIST_REPLY) == DS_MSG_HDR_SIZE + 360);
    CHECK(ds_msg_size_v(DS_MSG_CREATE, 1) == DS_MSG_HDR_SIZE + 16);
    CHECK(ds_msg_size_v(DS_MSG_LIST, 1) == 0);
    CHECK(sizeof(struct ds_msg) <= DS_MSG_MAX);
    CHECK(sizeof(struct ds_list_entry) == 44);
    CHECK(sizeof(struct ds_register_hotkey_body) == 16);
    CHECK(DS_MSG_HDR_SIZE + 360u <= DS_MSG_MAX);
    CHECK(DS_PROTO_VERSION == 2u);
    CHECK(DS_PROTO_VERSION_MIN == 1u);
    CHECK(DS_TITLE_MAX == 16u);
}

static void t_proto_create_v2(void)
{
    u8 buf[DS_MSG_MAX];
    struct ds_msg m;
    struct ds_msg d;
    int n;

    memset(&m, 0, sizeof(m));
    m.type = DS_MSG_CREATE;
    m.seq = 9;
    m.u.create.w = DS_WM_PANEL_W;
    m.u.create.h = DS_WM_PANEL_H;
    m.u.create.flags = DS_CREATE_F_POS;
    m.u.create.x = 0;
    m.u.create.y = 0;
    m.u.create.owner_pid = 42;
    ds_title_set(m.u.create.title, "Aster Panel");
    n = ds_msg_encode(&m, buf, sizeof(buf));
    CHECK(n == (int)ds_msg_size(DS_MSG_CREATE));
    CHECK(ds_msg_decode(buf, (u32)n, &d) == DS_OK);
    CHECK(d.version == DS_PROTO_VERSION);
    CHECK(d.type == DS_MSG_CREATE);
    CHECK(d.len == (u32)n);
    CHECK(d.seq == 9);
    CHECK(d.u.create.w == DS_WM_PANEL_W);
    CHECK(d.u.create.h == DS_WM_PANEL_H);
    CHECK(d.u.create.flags == DS_CREATE_F_POS);
    CHECK(d.u.create.owner_pid == 42);
    {
        char title[DS_TITLE_MAX];

        ds_title_get(d.u.create.title, title, (u32)sizeof(title));
        CHECK(strcmp(title, "Aster Panel") == 0);
    }
}

static void t_proto_create_v1(void)
{
    u8 buf[DS_MSG_MAX];
    struct ds_msg m;
    struct ds_msg d;
    int n;

    /* 先按 v2 编码，再把 header 改成 v1 并把长度截到 16 字节 body。 */
    memset(&m, 0, sizeof(m));
    m.type = DS_MSG_CREATE;
    m.u.create.w = 400;
    m.u.create.h = 300;
    m.u.create.flags = 0;
    ds_title_set(m.u.create.title, "ignored");
    n = ds_msg_encode(&m, buf, sizeof(buf));
    CHECK(n == (int)ds_msg_size(DS_MSG_CREATE));
    buf[4] = 1;  /* version = 1 */
    buf[5] = 0;
    {
        u32 v1len = DS_MSG_HDR_SIZE + DS_CREATE_BODY_V1;

        buf[8] = (u8)(v1len & 0xFFu);
        buf[9] = (u8)((v1len >> 8) & 0xFFu);
        buf[10] = (u8)((v1len >> 16) & 0xFFu);
        buf[11] = (u8)((v1len >> 24) & 0xFFu);
        CHECK(ds_msg_decode(buf, v1len, &d) == DS_OK);
    }
    CHECK(d.version == 1);
    CHECK(d.type == DS_MSG_CREATE);
    CHECK(d.u.create.w == 400 && d.u.create.h == 300);
    CHECK(d.u.create.flags == 0);
    CHECK(d.u.create.owner_pid == 0); /* v1 没有这个字段 */
    {
        char title[DS_TITLE_MAX];

        ds_title_get(d.u.create.title, title, (u32)sizeof(title));
        CHECK(title[0] == '\0'); /* v1 没有 title */
    }
    /* 版本 1 但用了 v2 长度 -> 拒绝（长度必须与版本匹配）。 */
    buf[4] = 1;
    buf[8] = (u8)((DS_MSG_HDR_SIZE + 40u) & 0xFFu);
    CHECK(ds_msg_decode(buf, DS_MSG_HDR_SIZE + 40u, &d) == DS_EINVAL);
    /* 版本 3 -> 拒绝。 */
    buf[4] = 3;
    CHECK(ds_msg_decode(buf, DS_MSG_HDR_SIZE + 40u, &d) == DS_EINVAL);
}

static void t_proto_list(void)
{
    u8 buf[DS_MSG_MAX];
    struct ds_msg m;
    struct ds_msg d;
    int n;

    memset(&m, 0, sizeof(m));
    m.type = DS_MSG_LIST;
    m.seq = 3;
    n = ds_msg_encode(&m, buf, sizeof(buf));
    CHECK(n == (int)ds_msg_size(DS_MSG_LIST));
    CHECK(ds_msg_decode(buf, (u32)n, &d) == DS_OK);
    CHECK(d.type == DS_MSG_LIST);

    memset(&m, 0, sizeof(m));
    m.type = DS_MSG_LIST_REPLY;
    m.seq = 4;
    m.u.list_reply.count = 2;
    ds_list_entry_set(&m.u.list_reply.entries[0], 1, 7, 0, 0, 1280, 32, 1,
                      "Aster Panel");
    ds_list_entry_set(&m.u.list_reply.entries[1], 2, 9, 192, 152, 400, 300, 0,
                      "Demo");
    n = ds_msg_encode(&m, buf, sizeof(buf));
    CHECK(n == (int)ds_msg_size(DS_MSG_LIST_REPLY));
    CHECK(ds_msg_decode(buf, (u32)n, &d) == DS_OK);
    CHECK(d.type == DS_MSG_LIST_REPLY);
    CHECK(d.u.list_reply.count == 2);
    CHECK(d.u.list_reply.entries[0].id == 1);
    CHECK(d.u.list_reply.entries[0].owner_pid == 7);
    CHECK(d.u.list_reply.entries[0].focused == 1);
    CHECK(d.u.list_reply.entries[1].x == 192);
    CHECK(d.u.list_reply.entries[1].w == 400);
    {
        char title[DS_TITLE_MAX];

        ds_title_get(d.u.list_reply.entries[1].title, title,
                     (u32)sizeof(title));
        CHECK(strcmp(title, "Demo") == 0);
    }

    /* focused 归一化：非 0 -> 1。 */
    memset(&m, 0, sizeof(m));
    m.type = DS_MSG_LIST_REPLY;
    ds_list_entry_set(&m.u.list_reply.entries[0], 5, 0, 1, 2, 3, 4, 9, "");
    CHECK(m.u.list_reply.entries[0].focused == 1);
    CHECK(m.u.list_reply.entries[0].title[0] == 0);
}

static void t_proto_hotkey(void)
{
    u8 buf[DS_MSG_MAX];
    struct ds_msg m;
    struct ds_msg d;
    int n;

    memset(&m, 0, sizeof(m));
    m.type = DS_MSG_REGISTER_HOTKEY;
    m.u.register_hotkey.surface_id = 1;
    m.u.register_hotkey.key = DS_HOTKEY_ANY;
    n = ds_msg_encode(&m, buf, sizeof(buf));
    CHECK(n == (int)ds_msg_size(DS_MSG_REGISTER_HOTKEY));
    CHECK(ds_msg_decode(buf, (u32)n, &d) == DS_OK);
    CHECK(d.type == DS_MSG_REGISTER_HOTKEY);
    CHECK(d.u.register_hotkey.surface_id == 1);
    CHECK(d.u.register_hotkey.key == DS_HOTKEY_ANY);
    CHECK(DS_INPUT_F_GLOBAL == 0x100u);

    /* WP3-2：WM_STATUS 往返（displayd 汇总进 /run/display.status）。 */
    memset(&m, 0, sizeof(m));
    m.type = DS_MSG_WM_STATUS;
    m.seq = 12;
    m.u.wm_status.surface_id = 1;
    m.u.wm_status.tasks = 1;
    m.u.wm_status.focus = 2;
    m.u.wm_status.switches = 3;
    m.u.wm_status.lastswitch = 2;
    m.u.wm_status.launches = 4;
    m.u.wm_status.lastpid = 14;
    n = ds_msg_encode(&m, buf, sizeof(buf));
    CHECK(n == (int)ds_msg_size(DS_MSG_WM_STATUS));
    CHECK(ds_msg_decode(buf, (u32)n, &d) == DS_OK);
    CHECK(d.type == DS_MSG_WM_STATUS);
    CHECK(d.seq == 12);
    CHECK(d.u.wm_status.surface_id == 1);
    CHECK(d.u.wm_status.tasks == 1);
    CHECK(d.u.wm_status.focus == 2);
    CHECK(d.u.wm_status.switches == 3);
    CHECK(d.u.wm_status.lastswitch == 2);
    CHECK(d.u.wm_status.launches == 4);
    CHECK(d.u.wm_status.lastpid == 14);
}

static void t_proto_title(void)
{
    u32 words[DS_TITLE_WORDS];
    char out[DS_TITLE_MAX];

    ds_title_set(words, NULL);
    CHECK(words[0] == 0 && words[1] == 0 && words[2] == 0 && words[3] == 0);

    ds_title_set(words, "Demo");
    ds_title_get(words, out, (u32)sizeof(out));
    CHECK(strcmp(out, "Demo") == 0);
    CHECK((words[0] & 0xFFu) == (u32)'D');

    ds_title_set(words, "");
    ds_title_get(words, out, (u32)sizeof(out));
    CHECK(out[0] == '\0');

    /* 15 字符正好放满；20 字符截到 15。 */
    ds_title_set(words, "123456789012345");
    ds_title_get(words, out, (u32)sizeof(out));
    CHECK(strcmp(out, "123456789012345") == 0);
    ds_title_set(words, "12345678901234567890");
    ds_title_get(words, out, (u32)sizeof(out));
    CHECK(strlen(out) == 15);
    CHECK(out[15] == '\0');

    /* cap 比标题短时截断并 NUL。 */
    ds_title_set(words, "Demo");
    ds_title_get(words, out, 3);
    CHECK(out[0] == 'D' && out[1] == 'e' && out[2] == '\0');
    /* NULL / 0 cap 不崩。 */
    ds_title_get(NULL, out, (u32)sizeof(out));
    CHECK(out[0] == '\0');
    ds_title_get(words, NULL, 4);
}

/* ------------------------------------------------------------------------- */
/* B. surface 表（v2 扩展）                                                  */
/* ------------------------------------------------------------------------- */

#define TBL_W 1280u
#define TBL_H 800u

static u32 spx[8 * 8];

static void t_table_ext(void)
{
    struct ds_table t;
    struct ds_list_entry list[DS_LIST_MAX];
    struct ds_surface *s;
    u32 id = 0;
    u32 n;

    ds_table_init(&t, TBL_W, TBL_H);
    CHECK(ds_table_create_ex(&t, DS_WM_PANEL_W, DS_WM_PANEL_H, 11, 0x30001,
                             0x40001, 40, (u64)(usize)spx, "Aster Panel", 0, 0,
                             &id) == DS_OK);
    CHECK(id == 1);
    s = ds_table_find(&t, 1);
    CHECK(s != NULL && s->x == 0 && s->y == 0);
    CHECK(s != NULL && strcmp(s->title, "Aster Panel") == 0);
    CHECK(s != NULL && s->owner_pid == 11);

    /* 不指定位置 -> 级联（面板算第 1 个，demo 落到 192,152）。 */
    CHECK(ds_table_create_ex(&t, 400, 300, 12, 0x30002, 0x40002, 118,
                             (u64)(usize)spx, "Demo", -1, -1, &id) == DS_OK);
    CHECK(id == 2);
    s = ds_table_find(&t, 2);
    CHECK(s != NULL && s->x == DS_FIRST_X + DS_CASCADE);
    CHECK(s != NULL && s->y == DS_FIRST_Y + DS_CASCADE);
    CHECK(s != NULL && strcmp(s->title, "Demo") == 0);

    /* 显式位置越界 -> 贴边（1200 + 400 > 1280 -> x = 880）。 */
    CHECK(ds_table_create_ex(&t, 400, 300, 13, 0x30003, 0x40003, 118, 0, "Big",
                             1200, 0, &id) == DS_OK);
    s = ds_table_find(&t, id);
    CHECK(s != NULL && s->x == 880);

    /* 无标题 -> 空串（v1 客户端等价路径）。 */
    CHECK(ds_table_create(&t, 8, 8, 14, 0x30004, 0x40004, 1,
                          (u64)(usize)spx, &id) == DS_OK);
    s = ds_table_find(&t, id);
    CHECK(s != NULL && s->title[0] == '\0');

    /* LIST 填充：z-order 后->前；FOCUS(2) 之后 surface 2 到最前。 */
    CHECK(ds_table_focus(&t, 2) == DS_OK);
    n = ds_table_fill_list(&t, list, DS_LIST_MAX);
    CHECK(n == 4);
    CHECK(list[0].id == 1);
    CHECK(list[0].focused == 0);
    CHECK(list[2].id == 4);
    CHECK(list[3].id == 2);
    CHECK(list[3].focused == 1);
    CHECK(list[3].owner_pid == 12);
    CHECK(list[3].x == 192 && list[3].y == 152);
    {
        char title[DS_TITLE_MAX];

        ds_title_get(list[3].title, title, (u32)sizeof(title));
        CHECK(strcmp(title, "Demo") == 0);
    }
    CHECK(ds_table_fill_list(&t, list, 2) == 2);
    CHECK(ds_table_fill_list(&t, NULL, 2) == 0);
    CHECK(ds_table_fill_list(NULL, list, 2) == 0);
}

/* ------------------------------------------------------------------------- */
/* C. 面板布局 / 绘制                                                         */
/* ------------------------------------------------------------------------- */

static u32 panel_px[DS_WM_PANEL_W * DS_WM_PANEL_H];

static void t_layout(void)
{
    struct ds_wm_layout l;
    struct ds_wm_rect r;

    ds_wm_layout_init(&l, DS_WM_PANEL_W);
    CHECK(l.panel_w == 1280 && l.panel_h == 32);
    CHECK(l.task_max == DS_LIST_MAX);
    CHECK(l.task_x == 136);
    CHECK(l.task_w == 96 && l.task_h == 24 && l.task_gap == 6);
    CHECK(l.clock.x == 1112 && l.clock.w == 64);
    CHECK(l.notif.x == 1184 && l.notif.w == 88);
    CHECK(l.clock.x + l.clock.w <= l.notif.x);
    CHECK(l.task_x + l.task_max * (l.task_w + l.task_gap) <= l.clock.x);
    CHECK(ds_wm_task_rect(&l, 0, &r) == 1);
    CHECK(r.x == 136 && r.y == 4 && r.w == 96 && r.h == 24);
    CHECK(ds_wm_task_rect(&l, 7, &r) == 1);
    CHECK(r.x == 136 + 7 * 102);
    CHECK(ds_wm_task_rect(&l, 8, &r) == 0);
    CHECK(ds_wm_task_rect(NULL, 0, &r) == 0);
    CHECK(ds_wm_task_rect(&l, 0, NULL) == 0);

    ds_wm_layout_init(&l, 640);
    CHECK(l.clock.x == 472);
    CHECK(l.task_max > 0 && l.task_max < DS_LIST_MAX);
    CHECK(ds_wm_task_rect(&l, l.task_max - 1u, &r) == 1);

    ds_wm_layout_init(&l, 320);
    CHECK(l.task_max == 0);
    CHECK(ds_wm_task_rect(&l, 0, &r) == 0);

    ds_wm_layout_init(NULL, 1280); /* 不崩 */
}

static void t_text_fit(void)
{
    char buf[8];

    ds_wm_text_fit(buf, (u32)sizeof(buf), "Demo");
    CHECK(strcmp(buf, "Demo") == 0);
    ds_wm_text_fit(buf, (u32)sizeof(buf), "1234567890");
    CHECK(strlen(buf) == 7);
    ds_wm_text_fit(buf, (u32)sizeof(buf), NULL);
    CHECK(buf[0] == '\0');
    ds_wm_text_fit(NULL, 8, "x");
}

static void t_draw_panel(void)
{
    struct ds_buffer b;
    struct ds_wm_layout l;
    struct ds_wm_tasks tasks;
    struct ds_wm_launcher apps;
    u32 i;
    u32 title_px = 0;
    u32 clock_px = 0;

    for (i = 0; i < (u32)(DS_WM_PANEL_W * DS_WM_PANEL_H); i++) {
        panel_px[i] = 0;
    }
    ds_wm_layout_init(&l, DS_WM_PANEL_W);
    memset(&tasks, 0, sizeof(tasks));
    tasks.count = 3; /* 面板自己 + 两个窗口 */
    tasks.focus_id = 2;
    tasks.task[0].id = 1;
    tasks.task[0].focused = 0;
    strcpy(tasks.task[0].title, "Aster Panel");
    tasks.task[1].id = 2;
    tasks.task[1].focused = 1;
    strcpy(tasks.task[1].title, "Demo");
    tasks.task[2].id = 3;
    tasks.task[2].focused = 0;
    strcpy(tasks.task[2].title, "Notes");
    ds_wm_launcher_init(&apps);
    ds_buffer_init(&b, panel_px, DS_WM_PANEL_W, DS_WM_PANEL_H);

    ds_wm_draw_panel(&b, &l, &tasks, 1, &apps, 0);

    /* 探针像素 = 纯背景（QMP 断言 (4,4) == #202838）。 */
    CHECK(panel_px[DS_WM_PROBE_Y * DS_WM_PANEL_W + DS_WM_PROBE_X] ==
          0xFF202838u);
    /* 底部强调线。 */
    CHECK(panel_px[30 * DS_WM_PANEL_W + 10] == 0xFF30507Au);
    /* 启动器按钮：外框 accent、内部按钮底色。 */
    CHECK(panel_px[4 * DS_WM_PANEL_W + 64] == 0xFF30507Au);
    CHECK(panel_px[6 * DS_WM_PANEL_W + 66] == 0xFF304460u);
    /* 任务按钮 0（Demo，焦点）用高亮底色；按钮 1（Notes）用普通底色。 */
    CHECK(panel_px[6 * DS_WM_PANEL_W + 140] == 0xFF4A7AC0u);
    CHECK(panel_px[4 * DS_WM_PANEL_W + 136] == 0xFF30507Au);
    CHECK(panel_px[6 * DS_WM_PANEL_W + 242] == 0xFF304460u);
    CHECK(panel_px[4 * DS_WM_PANEL_W + 238] == 0xFF30507Au);
    /* 通知区圆点。 */
    CHECK(panel_px[15 * DS_WM_PANEL_W + 1194] == 0xFFFFC040u);
    /* 标题/时钟确实画了字（有非背景像素），但探针区保持干净。 */
    for (i = 12; i < 20; i++) {
        u32 x;

        for (x = 8; x < 60; x++) {
            if (panel_px[i * DS_WM_PANEL_W + x] != 0xFF202838u) {
                title_px++;
            }
        }
    }
    CHECK(title_px > 0);
    for (i = 12; i < 20; i++) {
        u32 x;

        for (x = l.clock.x + 8; x < l.clock.x + 48; x++) {
            if (panel_px[i * DS_WM_PANEL_W + x] != 0xFF202838u) {
                clock_px++;
            }
        }
    }
    CHECK(clock_px > 0);

    /* 面板以外不写（缓冲正好 1280x32，全部像素 A=255）。 */
    CHECK((panel_px[0] >> 24) == 0xFFu);
    CHECK((panel_px[DS_WM_PANEL_W * DS_WM_PANEL_H - 1u] >> 24) == 0xFFu);

    /* pixel helper。 */
    CHECK(ds_wm_panel_pixel(panel_px, DS_WM_PANEL_W, DS_WM_PROBE_X,
                            DS_WM_PROBE_Y) == 0xFF202838u);
    CHECK(ds_wm_panel_pixel(panel_px, DS_WM_PANEL_W, DS_WM_PANEL_W, 0) == 0);
    CHECK(ds_wm_panel_pixel(NULL, DS_WM_PANEL_W, 0, 0) == 0);

    /* 空任务表 + 打开启动器：不崩，启动器按钮变高亮。 */
    memset(&tasks, 0, sizeof(tasks));
    ds_wm_draw_panel(&b, &l, &tasks, 1, &apps, 1);
    CHECK(panel_px[6 * DS_WM_PANEL_W + 66] == 0xFF4A7AC0u);
    ds_wm_draw_panel(NULL, &l, &tasks, 1, &apps, 0);
    ds_wm_draw_panel(&b, NULL, &tasks, 1, &apps, 0);
}

/* ------------------------------------------------------------------------- */
/* D. 启动器                                                                  */
/* ------------------------------------------------------------------------- */

static void t_launcher(void)
{
    struct ds_wm_launcher l;
    const struct ds_wm_app *a;
    char long_name[24];
    char name_buf[4];
    u32 i;

    ds_wm_launcher_init(&l);
    CHECK(l.count == 0 && l.bins == 0 && l.pkgs == 0 && l.launchable == 0);
    CHECK(ds_wm_launcher_at(&l, 0) == NULL);

    CHECK(ds_wm_launcher_add(&l, "/bin", "shell", 5, 0) == 1);
    CHECK(ds_wm_launcher_add(&l, "/bin", "proc", 4, 0) == 1);
    CHECK(ds_wm_launcher_add(&l, "/bin", "hello", 5, 0) == 1);
    CHECK(ds_wm_launcher_add(&l, "/bin", "displayd", 8, 0) == 1);
    CHECK(ds_wm_launcher_add(&l, "/bin", "demo", 4, 0) == 1);
    CHECK(ds_wm_launcher_add(&l, "/bin", "wm", 2, 0) == 1);
    /* /bin 里的目录、/System/Packages 里的文件都不进启动器。 */
    CHECK(ds_wm_launcher_add(&l, "/bin", "sub", 3, 1) == 0);
    CHECK(ds_wm_launcher_add(&l, "/System/Packages", "hello", 5, 1) == 1);
    CHECK(ds_wm_launcher_add(&l, "/System/Packages", "notes.txt", 9, 0) == 0);
    /* 重复 + 空名 + 表满前的边界。 */
    CHECK(ds_wm_launcher_add(&l, "/bin", "demo", 4, 0) == 0);
    CHECK(ds_wm_launcher_add(&l, "/bin", "", 0, 0) == 0);
    CHECK(l.count == 7);

    ds_wm_launcher_finalize(&l);
    CHECK(l.count == 7);
    CHECK(l.bins == 6);
    CHECK(l.pkgs == 1);
    CHECK(l.launchable == 3); /* demo / hello / proc */
    a = ds_wm_launcher_at(&l, 0);
    CHECK(a != NULL && strcmp(a->path, "/bin/demo") == 0);
    CHECK(a != NULL && a->kind == DS_WM_APP_BIN);
    a = ds_wm_launcher_at(&l, 1);
    CHECK(a != NULL && strcmp(a->path, "/bin/hello") == 0);
    a = ds_wm_launcher_at(&l, 2);
    CHECK(a != NULL && strcmp(a->path, "/bin/proc") == 0);
    CHECK(ds_wm_launcher_at(&l, 3) == NULL);

    /* 排序确定性：kind 先 BIN 后 PKG，同 kind 按 name。 */
    CHECK(strcmp(l.app[0].name, "demo") == 0);
    CHECK(strcmp(l.app[1].name, "displayd") == 0);
    CHECK(strcmp(l.app[5].name, "wm") == 0);
    CHECK(l.app[6].kind == DS_WM_APP_PKG);
    CHECK(strcmp(l.app[6].path, "/System/Packages/hello") == 0);
    CHECK(l.app[6].launchable == 0);

    /* 超长名字截断（NAME_MAX 16）。 */
    for (i = 0; i < (u32)sizeof(long_name) - 1u; i++) {
        long_name[i] = (char)('a' + (int)(i % 26u));
    }
    long_name[sizeof(long_name) - 1u] = '\0';
    ds_wm_launcher_init(&l);
    CHECK(ds_wm_launcher_add(&l, "/bin", long_name,
                             (u32)strlen(long_name), 0) == 1);
    CHECK(strlen(l.app[0].name) == (size_t)(DS_WM_NAME_MAX - 1u));

    /* 表满：DS_WM_APPS_MAX 条之后拒绝。 */
    ds_wm_launcher_init(&l);
    for (i = 0; i < DS_WM_APPS_MAX; i++) {
        name_buf[0] = 'a';
        name_buf[1] = (char)('a' + (int)(i % 26u));
        name_buf[2] = (char)('a' + (int)((i / 26u) % 26u));
        name_buf[3] = '\0';
        CHECK(ds_wm_launcher_add(&l, "/bin", name_buf, 3, 0) == 1);
    }
    CHECK(ds_wm_launcher_add(&l, "/bin", "zz", 2, 0) == 0);
    ds_wm_launcher_finalize(&l);
    CHECK(l.count == DS_WM_APPS_MAX);
    CHECK(l.launchable == DS_WM_APPS_MAX);
}

/* ------------------------------------------------------------------------- */
/* E. 任务列表                                                                */
/* ------------------------------------------------------------------------- */

static void build_tasks(struct ds_msg *m, u32 focus, u32 n)
{
    u32 i;

    memset(m, 0, sizeof(*m));
    m->type = DS_MSG_LIST_REPLY;
    m->u.list_reply.count = n;
    ds_list_entry_set(&m->u.list_reply.entries[0], 1, 7, 0, 0, 1280, 32,
                      (focus == 1), "Aster Panel");
    if (n >= 2) {
        ds_list_entry_set(&m->u.list_reply.entries[1], 2, 9, 192, 152, 400, 300,
                          (focus == 2), "Demo");
    }
    if (n >= 3) {
        ds_list_entry_set(&m->u.list_reply.entries[2], 3, 10, 224, 184, 400,
                          300, (focus == 3), "Notes");
    }
    for (i = 3; i < n; i++) {
        ds_list_entry_set(&m->u.list_reply.entries[i], i + 1, 0, 0, 0, 10, 10,
                          0, "x");
    }
}

static void t_tasks(void)
{
    struct ds_msg m;
    struct ds_wm_tasks t;
    u32 next = 0;

    build_tasks(&m, 1, 2);
    CHECK(ds_wm_tasks_from_msg(&m, &t) == 2);
    CHECK(t.count == 2);
    CHECK(t.focus_id == 1);
    CHECK(strcmp(t.task[1].title, "Demo") == 0);
    CHECK(t.task[1].owner_pid == 9);
    CHECK(t.task[1].x == 192 && t.task[1].y == 152);
    CHECK(ds_wm_tasks_find(&t, 2) == 1);
    CHECK(ds_wm_tasks_find(&t, 9) == -1);
    CHECK(ds_wm_tasks_find(NULL, 1) == -1);
    CHECK(ds_wm_tasks_apps(&t, 1) == 1);
    CHECK(ds_wm_tasks_apps(&t, 0) == 2);
    CHECK(ds_wm_tasks_next(&t, 1, &next) == DS_OK);
    CHECK(next == 2);
    /* self=2 时唯一可选是 1。 */
    CHECK(ds_wm_tasks_next(&t, 2, &next) == DS_OK);
    CHECK(next == 1);

    build_tasks(&m, 2, 3);
    CHECK(ds_wm_tasks_from_msg(&m, &t) == 3);
    CHECK(t.focus_id == 2);
    CHECK(ds_wm_tasks_apps(&t, 1) == 2);
    CHECK(ds_wm_tasks_next(&t, 1, &next) == DS_OK);
    CHECK(next == 3); /* 焦点 2 的下一个 */
    CHECK(ds_wm_tasks_next(&t, 2, &next) == DS_OK);
    CHECK(next == 1); /* 排除 self 后焦点不在列表 -> 第一个 */

    /* 焦点不在可切换列表里 -> 第一个。 */
    build_tasks(&m, 1, 3);
    CHECK(ds_wm_tasks_from_msg(&m, &t) == 3);
    CHECK(ds_wm_tasks_next(&t, 1, &next) == DS_OK);
    CHECK(next == 2);

    /* 只有面板 -> 没有可切换窗口。 */
    build_tasks(&m, 1, 1);
    CHECK(ds_wm_tasks_from_msg(&m, &t) == 1);
    CHECK(ds_wm_tasks_apps(&t, 1) == 0);
    CHECK(ds_wm_tasks_next(&t, 1, &next) == DS_ENOENT);
    CHECK(next == 0);

    /* 非法消息 / 空表 / NULL。 */
    m.type = DS_MSG_FOCUS;
    CHECK(ds_wm_tasks_from_msg(&m, &t) == 0);
    CHECK(t.count == 0);
    m.type = DS_MSG_LIST_REPLY;
    m.status = DS_EINVAL;
    CHECK(ds_wm_tasks_from_msg(&m, &t) == 0);
    CHECK(ds_wm_tasks_from_msg(NULL, &t) == 0);
    CHECK(ds_wm_tasks_from_msg(&m, NULL) == 0);
    CHECK(ds_wm_tasks_next(NULL, 1, &next) == DS_EINVAL);
    CHECK(ds_wm_tasks_next(&t, 1, NULL) == DS_EINVAL);

    /* count 超过表上限时截断。 */
    build_tasks(&m, 2, DS_WM_TASKS_MAX);
    m.u.list_reply.count = 99;
    CHECK(ds_wm_tasks_from_msg(&m, &t) == DS_WM_TASKS_MAX);
}

/* ------------------------------------------------------------------------- */
/* F. 热键状态机                                                              */
/* ------------------------------------------------------------------------- */

static void t_hotkey(void)
{
    struct ds_wm_hotkey h;
    u32 idx = 0;
    u32 i;

    ds_wm_hotkey_init(&h);
    CHECK(h.registered == 0 && h.switches == 0 && h.launches == 0 &&
          h.ignored == 0);
    CHECK(ds_wm_hotkey_index('1') == 0);
    CHECK(ds_wm_hotkey_index('9') == 8);
    CHECK(ds_wm_hotkey_index('0') == DS_WM_LAUNCH_INDEX_NONE);
    CHECK(ds_wm_hotkey_index('a') == DS_WM_LAUNCH_INDEX_NONE);
    CHECK(ds_wm_hotkey_index(0) == DS_WM_LAUNCH_INDEX_NONE);

    CHECK(ds_wm_hotkey_map(9, 0, &idx) == DS_WM_HOTKEY_TASK_SWITCH);
    CHECK(idx == DS_WM_LAUNCH_INDEX_NONE);
    CHECK(ds_wm_hotkey_map('1', 0, &idx) == DS_WM_HOTKEY_LAUNCH);
    CHECK(idx == 0);
    CHECK(ds_wm_hotkey_map('9', 0, &idx) == DS_WM_HOTKEY_LAUNCH);
    CHECK(idx == 8);
    CHECK(ds_wm_hotkey_map('q', 0, &idx) == DS_WM_HOTKEY_NONE);
    CHECK(ds_wm_hotkey_map(0, 0, &idx) == DS_WM_HOTKEY_NONE);
    CHECK(ds_wm_hotkey_map(9, 0, NULL) == DS_WM_HOTKEY_TASK_SWITCH);
    CHECK(ds_wm_hotkey_map('5', 0, NULL) == DS_WM_HOTKEY_LAUNCH);

    CHECK(ds_wm_hotkey_consume(&h, 9, 0, &idx) == DS_WM_HOTKEY_TASK_SWITCH);
    CHECK(h.last_key == 9);
    CHECK(h.last_action == DS_WM_HOTKEY_TASK_SWITCH);
    CHECK(h.ignored == 0);
    CHECK(ds_wm_hotkey_consume(&h, '2', 0, &idx) == DS_WM_HOTKEY_LAUNCH);
    CHECK(idx == 1);
    CHECK(h.last_key == '2');
    CHECK(h.last_action == DS_WM_HOTKEY_LAUNCH);
    CHECK(ds_wm_hotkey_consume(&h, 'x', 0, &idx) == DS_WM_HOTKEY_NONE);
    CHECK(h.ignored == 1);
    CHECK(ds_wm_hotkey_consume(NULL, '3', 0, &idx) == DS_WM_HOTKEY_LAUNCH);
    CHECK(idx == 2);
    CHECK(ds_wm_hotkey_consume(&h, 9, 0, NULL) == DS_WM_HOTKEY_TASK_SWITCH);

    /* 每次 Tab 都返回 TASK_SWITCH（完成计数由 WM 在 FOCUS 确认后加）。 */
    ds_wm_hotkey_init(&h);
    for (i = 0; i < 3; i++) {
        CHECK(ds_wm_hotkey_consume(&h, 9, 0, &idx) == DS_WM_HOTKEY_TASK_SWITCH);
    }
    CHECK(h.last_action == DS_WM_HOTKEY_TASK_SWITCH);

    CHECK(ds_wm_input_is_global(DS_INPUT_F_GLOBAL) == 1);
    CHECK(ds_wm_input_is_global(0) == 0);
    CHECK(ds_wm_input_is_global(DS_INPUT_F_GLOBAL | 0x1u) == 1);
}

int test_wm(void)
{
    t_proto_sizes();
    t_proto_create_v2();
    t_proto_create_v1();
    t_proto_list();
    t_proto_hotkey();
    t_proto_title();
    t_table_ext();
    t_layout();
    t_text_fit();
    t_draw_panel();
    t_launcher();
    t_tasks();
    t_hotkey();
    return 0;
}
