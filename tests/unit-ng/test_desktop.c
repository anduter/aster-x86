/*
 * ng WP3-1 host 单测：display server 的 L2 纯逻辑（≥80 checks）。
 *
 * 覆盖：
 *   - 协议编解码（src/desktop/src/ds_protocol.c）：8 种消息的 body 长度、
 *     往返字段、magic/version/len 严格校验、truncation/unknown type 拒绝；
 *   - surface 表（ds_surface.c）：id 单调、级联放置/贴边、表满、MAP 顺序、
 *     COMMIT 裁剪、FOCUS 提层、DESTROY 焦点回退、几何 helper；
 *   - 合成器（ds_composite.c）：z-order、alpha source-over、屏幕/region 裁剪、
 *     光标绘制；
 *   - 绘制/字模（ds_draw.c / ds_font8x8.c）：边界、渐变公式、8x8 字形朝向。
 */
#include <string.h>

#include "ds_composite.h"
#include "ds_draw.h"
#include "ds_font8x8.h"
#include "ds_protocol.h"
#include "ds_surface.h"
#include "test_util.h"

/* ------------------------------------------------------------------------- */
/* A. 协议编解码                                                              */
/* ------------------------------------------------------------------------- */

static void t_protocol_sizes(void)
{
    CHECK(ds_msg_body_size(DS_MSG_CREATE) == 16);
    CHECK(ds_msg_body_size(DS_MSG_CREATE_REPLY) == 28);
    CHECK(ds_msg_body_size(DS_MSG_MAP) == 8);
    CHECK(ds_msg_body_size(DS_MSG_COMMIT) == 20);
    CHECK(ds_msg_body_size(DS_MSG_DESTROY) == 8);
    CHECK(ds_msg_body_size(DS_MSG_FOCUS) == 8);
    CHECK(ds_msg_body_size(DS_MSG_INPUT) == 20);
    CHECK(ds_msg_body_size(DS_MSG_BYE) == 8);
    CHECK(ds_msg_body_size(99) == 0);
    CHECK(ds_msg_size(DS_MSG_CREATE) == DS_MSG_HDR_SIZE + 16);
    CHECK(ds_msg_size(DS_MSG_CREATE_REPLY) == DS_MSG_HDR_SIZE + 28);
    CHECK(ds_msg_size(99) == 0);
    CHECK(sizeof(struct ds_msg) <= DS_MSG_MAX);
    CHECK(sizeof(struct ds_ipc_hdr) == 16);
    CHECK(sizeof(struct ds_fb_info) == 48);
}

static void t_protocol_roundtrip(void)
{
    u8 buf[DS_MSG_MAX];
    struct ds_msg m;
    struct ds_msg d;
    int n;

    memset(&m, 0, sizeof(m));
    m.type = DS_MSG_CREATE;
    m.seq = 7;
    m.u.create.w = 400;
    m.u.create.h = 300;
    m.u.create.flags = 1;
    n = ds_msg_encode(&m, buf, sizeof(buf));
    CHECK(n == (int)ds_msg_size(DS_MSG_CREATE));
    CHECK(ds_msg_decode(buf, (u32)n, &d) == DS_OK);
    CHECK(d.magic == DS_PROTO_MAGIC);
    CHECK(d.version == DS_PROTO_VERSION);
    CHECK(d.type == DS_MSG_CREATE);
    CHECK(d.len == (u32)n);
    CHECK(d.seq == 7);
    CHECK(d.u.create.w == 400 && d.u.create.h == 300);
    CHECK(d.u.create.flags == 1);
    CHECK(ds_msg_status_ok(&d) == 1);

    memset(&m, 0, sizeof(m));
    m.type = DS_MSG_CREATE_REPLY;
    m.status = 0;
    m.u.create_reply.surface_id = 3;
    m.u.create_reply.shm_handle = 0x10003;
    m.u.create_reply.pages = 118;
    m.u.create_reply.x = 160;
    m.u.create_reply.y = 120;
    m.u.create_reply.w = 400;
    m.u.create_reply.h = 300;
    n = ds_msg_encode(&m, buf, sizeof(buf));
    CHECK(n == (int)ds_msg_size(DS_MSG_CREATE_REPLY));
    CHECK(ds_msg_decode(buf, (u32)n, &d) == DS_OK);
    CHECK(d.type == DS_MSG_CREATE_REPLY);
    CHECK(d.u.create_reply.surface_id == 3);
    CHECK(d.u.create_reply.shm_handle == 0x10003u);
    CHECK(d.u.create_reply.pages == 118);
    CHECK(d.u.create_reply.x == 160 && d.u.create_reply.y == 120);

    memset(&m, 0, sizeof(m));
    m.type = DS_MSG_COMMIT;
    m.u.commit.surface_id = 2;
    m.u.commit.x = 1;
    m.u.commit.y = 2;
    m.u.commit.w = 3;
    m.u.commit.h = 4;
    n = ds_msg_encode(&m, buf, sizeof(buf));
    CHECK(n == (int)ds_msg_size(DS_MSG_COMMIT));
    CHECK(ds_msg_decode(buf, (u32)n, &d) == DS_OK);
    CHECK(d.u.commit.surface_id == 2 && d.u.commit.x == 1 &&
          d.u.commit.y == 2 && d.u.commit.w == 3 && d.u.commit.h == 4);

    memset(&m, 0, sizeof(m));
    m.type = DS_MSG_INPUT;
    m.u.input.surface_id = 1;
    m.u.input.kind = DS_INPUT_KEY_DOWN;
    m.u.input.key = 'b';
    m.u.input.scancode = 0x30;
    m.u.input.flags = 0;
    n = ds_msg_encode(&m, buf, sizeof(buf));
    CHECK(n == (int)ds_msg_size(DS_MSG_INPUT));
    CHECK(ds_msg_decode(buf, (u32)n, &d) == DS_OK);
    CHECK(d.type == DS_MSG_INPUT);
    CHECK(d.u.input.key == 'b' && d.u.input.scancode == 0x30);

    /* 短 body 类型也能往返（body 之外的字节必须被清零）。 */
    memset(&m, 0, sizeof(m));
    m.type = DS_MSG_FOCUS;
    m.u.focus.surface_id = 9;
    n = ds_msg_encode(&m, buf, sizeof(buf));
    CHECK(n == (int)ds_msg_size(DS_MSG_FOCUS));
    CHECK(ds_msg_decode(buf, (u32)n, &d) == DS_OK);
    CHECK(d.u.focus.surface_id == 9);
    CHECK(d.u.input.key == 0);

    /* 错误状态码原样往返。 */
    memset(&m, 0, sizeof(m));
    m.type = DS_MSG_CREATE_REPLY;
    m.status = DS_EINVAL;
    m.seq = 11;
    n = ds_msg_encode(&m, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(ds_msg_decode(buf, (u32)n, &d) == DS_OK);
    CHECK(d.status == DS_EINVAL);
    CHECK(ds_msg_status_ok(&d) == 0);
}

static void t_protocol_reject(void)
{
    u8 buf[DS_MSG_MAX];
    struct ds_msg m;
    struct ds_msg d;
    int n;

    memset(&m, 0, sizeof(m));
    m.type = DS_MSG_MAP;
    m.u.map.surface_id = 1;
    n = ds_msg_encode(&m, buf, sizeof(buf));
    CHECK(n > 0);

    CHECK(ds_msg_decode(NULL, (u32)n, &d) == DS_EINVAL);
    CHECK(ds_msg_decode(buf, (u32)n, NULL) == DS_EINVAL);
    CHECK(ds_msg_decode(buf, 0, &d) == DS_EINVAL);
    CHECK(ds_msg_decode(buf, (u32)n - 1u, &d) == DS_EINVAL);
    CHECK(ds_msg_decode(buf, (u32)n + 1u, &d) == DS_EINVAL);
    CHECK(ds_msg_decode(buf, (u32)n, &d) == DS_OK);

    /* 坏 magic / version / 类型 / hdr.len。 */
    buf[0] ^= 0xFFu;
    CHECK(ds_msg_decode(buf, (u32)n, &d) == DS_EINVAL);
    buf[0] ^= 0xFFu;
    buf[4] = 9;
    CHECK(ds_msg_decode(buf, (u32)n, &d) == DS_EINVAL);
    buf[4] = (u8)DS_PROTO_VERSION;
    buf[6] = 77;
    CHECK(ds_msg_decode(buf, (u32)n, &d) == DS_EINVAL);
    buf[6] = (u8)DS_MSG_MAP;
    buf[8] = (u8)(n - 4);
    CHECK(ds_msg_decode(buf, (u32)n, &d) == DS_EINVAL);
    buf[8] = (u8)n;

    /* 编码拒绝：未知类型 / cap 不够 / NULL。 */
    memset(&m, 0, sizeof(m));
    m.type = 42;
    CHECK(ds_msg_encode(&m, buf, sizeof(buf)) == DS_EINVAL);
    m.type = DS_MSG_CREATE;
    CHECK(ds_msg_encode(&m, buf, 4) == DS_EINVAL);
    CHECK(ds_msg_encode(NULL, buf, sizeof(buf)) == DS_EINVAL);
    CHECK(ds_msg_encode(&m, NULL, sizeof(buf)) == DS_EINVAL);
    CHECK(ds_msg_encode(&m, buf, sizeof(buf)) ==
          (int)ds_msg_size(DS_MSG_CREATE));

    /* 字符串 helper。 */
    CHECK(ds_strlen("aster") == 5);
    CHECK(ds_strlen(NULL) == 0);
    CHECK(ds_streq("ng", "ng") == 1);
    CHECK(ds_streq("ng", "NG") == 0);
    CHECK(ds_streq(NULL, "x") == 0);
    {
        u8 a[4] = {1, 2, 3, 4};
        u8 b[4] = {0, 0, 0, 0};

        ds_memcopy(b, a, 4);
        CHECK(b[0] == 1 && b[3] == 4);
        ds_memzero(b, 4);
        CHECK(b[0] == 0 && b[3] == 0);
    }
}

/* ------------------------------------------------------------------------- */
/* B. surface 表                                                              */
/* ------------------------------------------------------------------------- */

#define TBL_W 1280u
#define TBL_H 800u

static u32 px_a[8 * 8];
static u32 px_b[8 * 8];

static void fill_px(u32 *p, u32 n, u32 v)
{
    u32 i;

    for (i = 0; i < n; i++) {
        p[i] = v;
    }
}

static void t_table_create(void)
{
    struct ds_table t;
    struct ds_surface *s;
    u32 id = 0;

    ds_table_init(&t, TBL_W, TBL_H);
    CHECK(ds_table_count(&t) == 0);
    CHECK(ds_table_focus_id(&t) == 0);
    CHECK(ds_table_at(&t, 0) == NULL);

    CHECK(ds_table_create(&t, 8, 8, 1, 0x20001, 0x10001, 1, (u64)(usize)px_a,
                          &id) == DS_OK);
    CHECK(id == 1);
    CHECK(ds_table_count(&t) == 1);
    CHECK(ds_table_focus_id(&t) == 1);
    s = ds_table_find(&t, 1);
    CHECK(s != NULL);
    CHECK(s != NULL && s->w == 8 && s->h == 8);
    CHECK(s != NULL && s->x == DS_FIRST_X && s->y == DS_FIRST_Y);
    CHECK(s != NULL && s->mapped == 0 && s->committed == 0);
    CHECK(s != NULL && s->client_ticket == 0x20001u);
    CHECK(s != NULL && s->pixels == (u64)(usize)px_a);
    CHECK(ds_table_at(&t, 0) == s);
    CHECK(ds_table_at(&t, 1) == NULL);

    /* 第二个级联放置，且到最前；焦点仍是第一个（自动聚焦之后不抢）。 */
    CHECK(ds_table_create(&t, 8, 8, 1, 0x20002, 0x10002, 1,
                          (u64)(usize)px_b, &id) == DS_OK);
    CHECK(id == 2);
    CHECK(ds_table_count(&t) == 2);
    s = ds_table_find(&t, 2);
    CHECK(s != NULL && s->x == DS_FIRST_X + DS_CASCADE);
    CHECK(s != NULL && s->y == DS_FIRST_Y + DS_CASCADE);
    CHECK(ds_table_at(&t, 1) == s);
    CHECK(ds_table_focus_id(&t) == 1);

    /* 非法几何 / 页数不足 / 页数超上限。 */
    CHECK(ds_table_create(&t, 0, 8, 1, 0, 0, 1, 0, &id) == DS_EINVAL);
    CHECK(ds_table_create(&t, 8, 0, 1, 0, 0, 1, 0, &id) == DS_EINVAL);
    CHECK(ds_table_create(&t, DS_SURFACE_MAX_W + 1u, 8, 1, 0, 0, 1, 0, &id) ==
          DS_EINVAL);
    CHECK(ds_table_create(&t, 8, DS_SURFACE_MAX_H + 1u, 1, 0, 0, 1, 0, &id) ==
          DS_EINVAL);
    CHECK(ds_table_create(&t, 400, 300, 1, 0, 0, 2, 0, &id) == DS_EINVAL);
    CHECK(ds_table_create(&t, 400, 300, 1, 0, 0, DS_SHM_PAGES_MAX + 1u, 0,
                          &id) == DS_EINVAL);
    CHECK(t.rejects >= 6);

    /* 超过屏幕的大 surface 贴边（左上角 0,0）。 */
    CHECK(ds_table_create(&t, TBL_W, TBL_H, 1, 0, 0, 1000, 0, &id) == DS_OK);
    s = ds_table_find(&t, id);
    CHECK(s != NULL && s->x == 0 && s->y == 0);
}

static void t_table_lifecycle(void)
{
    struct ds_table t;
    struct ds_rect r;
    struct ds_rect clip;
    u32 a = 0;
    u32 b = 0;
    u32 i;
    u32 id;

    ds_table_init(&t, TBL_W, TBL_H);
    fill_px(px_a, 64, 0xFF0000FFu);
    fill_px(px_b, 64, 0xFF00FF00u);
    CHECK(ds_table_create(&t, 8, 8, 1, 1, 1, 1, (u64)(usize)px_a, &a) == DS_OK);
    CHECK(ds_table_create(&t, 8, 8, 1, 2, 2, 1, (u64)(usize)px_b, &b) == DS_OK);

    CHECK(ds_table_mark_mapped(&t, 999) == DS_ENOENT);
    CHECK(ds_table_mark_mapped(&t, a) == DS_OK);
    CHECK(ds_table_mark_mapped(&t, a) == DS_OK);
    r.x = 0;
    r.y = 0;
    r.w = 8;
    r.h = 8;
    CHECK(ds_table_commit(&t, b, &r, &clip) == DS_EBUSY); /* 还没 MAP */
    CHECK(ds_table_mark_mapped(&t, b) == DS_OK);
    CHECK(ds_table_commit(&t, b, &r, &clip) == DS_OK);
    CHECK(clip.x == 0 && clip.y == 0 && clip.w == 8 && clip.h == 8);
    CHECK(ds_table_find(&t, b)->commits == 1);
    CHECK(ds_table_find(&t, b)->committed == 1);
    CHECK(t.commit_total == 1);

    /* 裁剪：右下角只露 1x1。 */
    r.x = 7;
    r.y = 7;
    r.w = 5;
    r.h = 5;
    CHECK(ds_table_commit(&t, b, &r, &clip) == DS_OK);
    CHECK(clip.x == 7 && clip.y == 7 && clip.w == 1 && clip.h == 1);

    /* 完全在 surface 外 / 空矩形 -> EINVAL，不记账。 */
    r.x = 8;
    r.y = 0;
    CHECK(ds_table_commit(&t, b, &r, &clip) == DS_EINVAL);
    r.x = 0;
    r.w = 0;
    CHECK(ds_table_commit(&t, b, &r, &clip) == DS_EINVAL);
    CHECK(ds_table_commit(&t, b, NULL, &clip) == DS_EINVAL);
    CHECK(ds_table_commit(&t, 999, &r, &clip) == DS_ENOENT);
    CHECK(ds_table_find(&t, b)->commits == 2);

    /* FOCUS 提层。 */
    CHECK(ds_table_focus(&t, 999) == DS_ENOENT);
    CHECK(ds_table_focus(&t, a) == DS_OK);
    CHECK(ds_table_focus_id(&t) == a);
    CHECK(ds_table_at(&t, 1)->id == a);
    CHECK(ds_table_at(&t, 0)->id == b);

    /* 销毁焦点 -> 焦点回退到当前最上层。 */
    CHECK(ds_table_destroy(&t, a) == DS_OK);
    CHECK(ds_table_count(&t) == 1);
    CHECK(ds_table_find(&t, a) == NULL);
    CHECK(ds_table_focus_id(&t) == b);
    CHECK(ds_table_destroy(&t, a) == DS_ENOENT);
    CHECK(ds_table_destroy(&t, b) == DS_OK);
    CHECK(ds_table_count(&t) == 0);
    CHECK(ds_table_focus_id(&t) == 0);
    CHECK(t.destroyed == 2);

    /* 表满：填满 DS_SURFACES_MAX 后 ENOSPC。 */
    for (i = 0; i < DS_SURFACES_MAX; i++) {
        CHECK(ds_table_create(&t, 4, 4, 1, i, i, 1, (u64)(usize)px_a, &id) ==
              DS_OK);
    }
    CHECK(ds_table_create(&t, 4, 4, 1, 99, 99, 1, 0, &id) == DS_ENOSPC);
    while (ds_table_count(&t) > 0) {
        (void)ds_table_destroy(&t, ds_table_at(&t, 0)->id);
    }
    CHECK(ds_table_count(&t) == 0);

    /* 几何 helper。 */
    {
        struct ds_rect a1;
        struct ds_rect b1;
        struct ds_rect out;

        a1.x = 0;
        a1.y = 0;
        a1.w = 10;
        a1.h = 10;
        b1.x = 5;
        b1.y = 5;
        b1.w = 10;
        b1.h = 10;
        CHECK(ds_rect_intersect(&a1, &b1, &out) == 1);
        CHECK(out.x == 5 && out.y == 5 && out.w == 5 && out.h == 5);
        b1.x = 20;
        CHECK(ds_rect_intersect(&a1, &b1, &out) == 0);
        CHECK(ds_rect_empty(&out) == 1);
        CHECK(ds_rect_area(&a1) == 100);
        CHECK(ds_rect_area(&out) == 0);
        CHECK(ds_rect_clip(&a1, 4, 4, &out) == 1);
        CHECK(out.w == 4 && out.h == 4);
        CHECK(ds_rect_clip(&a1, 0, 10, &out) == 0);
        CHECK(ds_rect_clip(NULL, 10, 10, &out) == 0);
        CHECK(ds_rect_empty(NULL) == 1);
    }
}

/* ------------------------------------------------------------------------- */
/* C. 合成器                                                                  */
/* ------------------------------------------------------------------------- */

#define SCR_W 48u
#define SCR_H 32u

static u32 scr[SCR_W * SCR_H];

static struct ds_screen make_screen(void)
{
    struct ds_screen s;

    s.px = scr;
    s.w = SCR_W;
    s.h = SCR_H;
    s.pitch_px = SCR_W;
    return s;
}

static void t_composite_zorder(void)
{
    struct ds_table t;
    struct ds_screen s;
    struct ds_composite_stats st;
    u32 a = 0;
    u32 b = 0;
    u32 n;
    u32 i;

    for (i = 0; i < SCR_W * SCR_H; i++) {
        scr[i] = 0;
    }
    fill_px(px_a, 64, 0xFFFF0000u); /* 红，不透明 */
    fill_px(px_b, 64, 0xFF0000FFu); /* 蓝，不透明 */
    ds_table_init(&t, SCR_W, SCR_H);
    s = make_screen();

    CHECK(ds_table_create(&t, 8, 8, 1, 1, 1, 1, (u64)(usize)px_a, &a) == DS_OK);
    CHECK(ds_table_create(&t, 8, 8, 1, 2, 2, 1, (u64)(usize)px_b, &b) == DS_OK);
    CHECK(ds_table_mark_mapped(&t, a) == DS_OK);
    CHECK(ds_table_mark_mapped(&t, b) == DS_OK);

    /* 48x32 的测试屏装不下级联坐标：两个 surface 都被贴边到右下角。 */
    n = ds_composite_all(&s, &t, &st);
    CHECK(n > 0);
    CHECK(st.surfaces == 2);

    /* 手工挪到屏幕内（表不阻止外部改几何；displayd 用默认级联）。 */
    ds_table_find(&t, a)->x = 0;
    ds_table_find(&t, a)->y = 0;
    ds_table_find(&t, b)->x = 4;
    ds_table_find(&t, b)->y = 4;
    n = ds_composite_all(&s, &t, &st);
    CHECK(n > 0);
    CHECK(st.surfaces == 2);
    CHECK(ds_screen_read(&s, 0, 0) == 0xFF0000u);
    CHECK(ds_screen_read(&s, 4, 4) == 0x0000FFu); /* 上层蓝覆盖红 */
    CHECK(ds_screen_read(&s, 11, 11) == 0x0000FFu);
    CHECK(ds_screen_read(&s, 0, 7) == 0xFF0000u);
    CHECK(ds_screen_read(&s, 12, 0) == 0);

    /* FOCUS A：A 到最前，重叠区变红。 */
    CHECK(ds_table_focus(&t, a) == DS_OK);
    (void)ds_composite_all(&s, &t, NULL);
    CHECK(ds_screen_read(&s, 4, 4) == 0xFF0000u);
    CHECK(ds_screen_read(&s, 8, 8) == 0x0000FFu); /* 只有 B 的区域 */

    /* 屏幕裁剪：B 挪到右下角，只露 1 列/1 行。 */
    ds_table_find(&t, b)->x = SCR_W - 2;
    ds_table_find(&t, b)->y = SCR_H - 2;
    n = ds_composite_region(&s, &t, NULL, &st);
    CHECK(n > 0);
    CHECK(st.surfaces == 2);
    CHECK(ds_screen_read(&s, SCR_W - 1, SCR_H - 1) == 0x0000FFu);
    CHECK(ds_screen_read(&s, 2, 2) == 0xFF0000u); /* A 仍在 */

    /* region 合成只写 region 内：清屏后只用 region 覆盖 A 的一小块。 */
    for (i = 0; i < SCR_W * SCR_H; i++) {
        scr[i] = 0x00123456u;
    }
    {
        struct ds_rect region;

        region.x = 2;
        region.y = 2;
        region.w = 2;
        region.h = 2;
        n = ds_composite_region(&s, &t, &region, &st);
        CHECK(n == 4);
        CHECK(ds_screen_read(&s, 2, 2) == 0xFF0000u);
        CHECK(ds_screen_read(&s, 0, 0) == 0x123456u);
        CHECK(ds_screen_read(&s, 5, 5) == 0x123456u);
        CHECK(st.clipped == 1);
        CHECK(st.surfaces == 1);
    }
}

static void t_composite_alpha(void)
{
    struct ds_table t;
    struct ds_screen s;
    u32 a = 0;
    u32 i;

    for (i = 0; i < SCR_W * SCR_H; i++) {
        scr[i] = 0x00102030u; /* dst */
    }
    /* 半透明白：A=128。 */
    fill_px(px_a, 8 * 8, 0x80FFFFFFu);
    ds_table_init(&t, SCR_W, SCR_H);
    s = make_screen();
    CHECK(ds_table_create(&t, 8, 8, 1, 1, 1, 1, (u64)(usize)px_a, &a) == DS_OK);
    CHECK(ds_table_mark_mapped(&t, a) == DS_OK);
    ds_table_find(&t, a)->x = 0;
    ds_table_find(&t, a)->y = 0;
    (void)ds_composite_all(&s, &t, NULL);
    CHECK(ds_screen_read(&s, 0, 0) == ds_pixel_blend(0x00102030u, 0x80FFFFFFu));
    CHECK(ds_screen_read(&s, 3, 3) != 0x00102030u);

    /* A=0 全透明：dst 不变。 */
    for (i = 0; i < SCR_W * SCR_H; i++) {
        scr[i] = 0x00102030u;
    }
    fill_px(px_a, 8 * 8, 0x00123456u);
    (void)ds_composite_all(&s, &t, NULL);
    CHECK(ds_screen_read(&s, 0, 0) == 0x00102030u);

    /* A=255 直接拷贝（含 0 值也不能被跳过）。 */
    for (i = 0; i < SCR_W * SCR_H; i++) {
        scr[i] = 0x00102030u;
    }
    fill_px(px_a, 8 * 8, 0xFF000000u);
    (void)ds_composite_all(&s, &t, NULL);
    CHECK(ds_screen_read(&s, 0, 0) == 0x000000u);

    CHECK(ds_pixel_blend(0x00102030u, 0x00FFFFFFu) == 0x102030u);
    CHECK(ds_pixel_blend(0x00102030u, 0xFF00FF00u) == 0x00FF00u);
    CHECK(ds_pixel_blend(0x00102030u, 0x00000000u) == 0x102030u);
}

static void t_cursor(void)
{
    struct ds_screen s;
    u32 i;

    for (i = 0; i < SCR_W * SCR_H; i++) {
        scr[i] = 0x00112233u;
    }
    s = make_screen();
    ds_cursor_draw(&s, 0, 0);
    CHECK(ds_screen_read(&s, 0, 0) == 0xFFFFFFu);           /* 描边 */
    CHECK(ds_screen_read(&s, DS_CURSOR_W - 1, 0) == 0xFFFFFFu);
    CHECK(ds_screen_read(&s, 0, DS_CURSOR_H - 1) == 0xFFFFFFu);
    CHECK(ds_screen_read(&s, 1, 5) == 0xFFFFFFu);            /* 高亮竖线 */
    CHECK(ds_screen_read(&s, 2, 5) == 0x000000u);            /* 内部填充 */
    CHECK(ds_screen_read(&s, DS_CURSOR_W, 0) == 0x112233u);  /* 边界外不动 */

    /* 右下角裁剪：不越界、不崩溃。 */
    ds_cursor_draw(&s, SCR_W - 3, SCR_H - 3);
    CHECK(ds_screen_read(&s, SCR_W - 1, SCR_H - 3) != 0x112233u);
    /* 完全在屏幕外：一个像素都不写。 */
    {
        u32 before = ds_screen_read(&s, 10, 10);

        ds_cursor_draw(&s, SCR_W + 5, SCR_H + 5);
        CHECK(ds_screen_read(&s, 10, 10) == before);
    }
}

/* ------------------------------------------------------------------------- */
/* D. 绘制 / 字模                                                             */
/* ------------------------------------------------------------------------- */

static u32 dbuf[16 * 16];

static void t_draw(void)
{
    struct ds_buffer b;
    u32 i;

    for (i = 0; i < 16 * 16; i++) {
        dbuf[i] = 0;
    }
    ds_buffer_init(&b, dbuf, 16, 16);
    CHECK(b.w == 16 && b.h == 16 && b.pitch_px == 16);

    ds_draw_pixel(&b, 0, 0, 0xFFFF0000u);
    CHECK(dbuf[0] == 0xFFFF0000u);
    ds_draw_pixel(&b, -1, 0, 0xFFFFFFFFu); /* 越界忽略 */
    ds_draw_pixel(&b, 16, 0, 0xFFFFFFFFu);
    ds_draw_pixel(&b, 0, 16, 0xFFFFFFFFu);
    CHECK(dbuf[0] == 0xFFFF0000u);

    /* 半透明叠加后画布仍不透明（A=255），只影响 RGB。 */
    ds_draw_pixel(&b, 1, 1, 0x80FFFFFFu);
    CHECK((dbuf[17] >> 24) == 0xFFu);
    CHECK((dbuf[17] & 0x00FFFFFFu) != 0);

    /* 填充 + 裁剪。 */
    ds_draw_fill(&b, 2, 2, 4, 4, 0xFF00FF00u);
    CHECK(dbuf[2 * 16 + 2] == 0xFF00FF00u);
    CHECK(dbuf[5 * 16 + 5] == 0xFF00FF00u);
    CHECK(dbuf[6 * 16 + 6] == 0);
    ds_draw_fill(&b, 14, 14, 10, 10, 0xFF0000FFu);
    CHECK(dbuf[15 * 16 + 15] == 0xFF0000FFu);
    ds_draw_fill(&b, 0, 0, 0, 4, 0xFFFFFFFFu); /* 空矩形 */

    /* 渐变公式：左上 / 右下 / 不透明。 */
    CHECK(ds_gradient_pixel(0, 0, 400, 300) == 0xFF000040u);
    CHECK(ds_gradient_pixel(399, 299, 400, 300) == 0xFFFEFE7Fu);
    CHECK((ds_gradient_pixel(10, 10, 400, 300) >> 24) == 0xFFu);
    CHECK(ds_gradient_pixel(0, 0, 0, 0) == 0);

    /* 字模：空格全零；'A' 的 row0=0x0C（bit2/bit3）。 */
    CHECK(ds_font_is_blank(' ') == 1);
    CHECK(ds_font_is_blank('A') == 0);
    CHECK(ds_font_glyph('A')[0] == 0x0Cu);
    CHECK(ds_font_glyph(0x01) == ds_font_glyph('?'));
    CHECK(ds_font_glyph('~') != NULL);

    /* 画 'A'：bit2/bit3 亮，bit0 暗；画布仍不透明。 */
    for (i = 0; i < 16 * 16; i++) {
        dbuf[i] = 0;
    }
    ds_draw_char(&b, 0, 0, 'A', 0xFFFF0000u, 0xFF000000u);
    CHECK(dbuf[0] == 0xFF000000u);      /* col0 暗 */
    CHECK(dbuf[2] == 0xFFFF0000u);      /* row0 bit2 亮 */
    CHECK(dbuf[3] == 0xFFFF0000u);      /* row0 bit3 亮 */
    CHECK(dbuf[1] == 0xFF000000u);
    CHECK(dbuf[1 * 16 + 3] == 0xFFFF0000u); /* row1 0x1E：bit1..4 */

    /* 文本按 8 像素步进。 */
    for (i = 0; i < 16 * 16; i++) {
        dbuf[i] = 0;
    }
    ds_draw_text(&b, 0, 0, "A", 0xFFFFFFFFu, 0xFF000000u);
    CHECK(dbuf[2] == 0xFFFFFFFFu);
    ds_draw_text(&b, 0, 0, NULL, 0, 0); /* 空指针安全 */
}

int test_desktop(void)
{
    t_protocol_sizes();
    t_protocol_roundtrip();
    t_protocol_reject();
    t_table_create();
    t_table_lifecycle();
    t_composite_zorder();
    t_composite_alpha();
    t_cursor();
    t_draw();
    return 0;
}
