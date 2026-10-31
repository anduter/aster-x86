/*
 * ng WP3-5: S1 故障注入的纯逻辑（见 s1_logic.h）。
 *
 * 这一份同时被三处使用：
 *   - 客户机代理 tests/s1/s1fault.c（ring3，无 libc）；
 *   - host 单测 tests/unit-ng/test_s1.c（同一份断言，不依赖 QEMU）；
 *   - 文档/claims 里引用的步骤名与证据行格式。
 * 所以这里不 include 任何内核/驱动头，也不做 IO。
 */
#include "s1_logic.h"

static int s1_is_space(char c)
{
    return (c == ' ' || c == '\n' || c == '\r' || c == '\t') ? 1 : 0;
}

static int s1_key_eq(const char *p, u32 n, const char *key)
{
    u32 i = 0;

    while (key[i] != '\0') {
        if (i >= n || p[i] != key[i]) {
            return 0;
        }
        i++;
    }
    return (i == n) ? 1 : 0;
}

/*
 * 十进制或 0x 十六进制；整段必须都是数字，否则视为无效（0 与缺失分开）。
 * 32 位溢出按无符号回绕（状态文件里的计数不会到那个量级）。
 */
static int s1_value_u32(const char *p, u32 n, u32 *out)
{
    u32 i = 0;
    u32 base = 10u;
    u32 v = 0;

    if (n == 0u) {
        return 0;
    }
    if (n >= 2u && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16u;
        i = 2u;
    }
    if (i >= n) {
        return 0;
    }
    for (; i < n; i++) {
        u32 c = (u32)(u8)p[i];
        u32 d;

        if (c >= (u32)'0' && c <= (u32)'9') {
            d = c - (u32)'0';
        } else if (base == 16u && c >= (u32)'a' && c <= (u32)'f') {
            d = c - (u32)'a' + 10u;
        } else if (base == 16u && c >= (u32)'A' && c <= (u32)'F') {
            d = c - (u32)'A' + 10u;
        } else {
            return 0;
        }
        if (d >= base) {
            return 0;
        }
        v = v * base + d;
    }
    *out = v;
    return 1;
}

u32 s1_status_parse(const char *text, u32 len, struct s1_status *out)
{
    u32 i = 0;
    u32 seen = 0;
    u32 count = 0;

    if (out == NULL) {
        return 0;
    }
    out->sess = 0;
    out->spid = 0;
    out->sdpid = 0;
    out->swmpid = 0;
    out->swmstate = 0;
    out->sapps = 0;
    out->swindows = 0;
    out->slaunches = 0;
    out->sreaps = 0;
    out->srestarts = 0;
    out->sapppid = 0;
    out->stokenfail = 0;
    out->found = 0;
    if (text == NULL || len == 0u) {
        return 0;
    }
    while (i < len) {
        u32 start;
        u32 end;
        u32 eq;
        u32 key_len;
        u32 bit = 0;
        u32 *slot = NULL;
        u32 v = 0;

        while (i < len && s1_is_space(text[i])) {
            i++;
        }
        if (i >= len) {
            break;
        }
        start = i;
        while (i < len && !s1_is_space(text[i])) {
            i++;
        }
        end = i;
        eq = start;
        while (eq < end && text[eq] != '=') {
            eq++;
        }
        if (eq >= end || eq == start) {
            continue; /* 没有 '=' 或空 key */
        }
        key_len = eq - start;
        if (!s1_value_u32(text + eq + 1u, end - eq - 1u, &v)) {
            continue;
        }
        if (s1_key_eq(text + start, key_len, "sess") != 0) {
            bit = S1_F_SESS;
            slot = &out->sess;
        } else if (s1_key_eq(text + start, key_len, "spid") != 0) {
            bit = S1_F_SPID;
            slot = &out->spid;
        } else if (s1_key_eq(text + start, key_len, "sdpid") != 0) {
            bit = S1_F_SDPID;
            slot = &out->sdpid;
        } else if (s1_key_eq(text + start, key_len, "swmpid") != 0) {
            bit = S1_F_SWMPID;
            slot = &out->swmpid;
        } else if (s1_key_eq(text + start, key_len, "swmstate") != 0) {
            bit = S1_F_SWMSTATE;
            slot = &out->swmstate;
        } else if (s1_key_eq(text + start, key_len, "sapps") != 0) {
            bit = S1_F_SAPPS;
            slot = &out->sapps;
        } else if (s1_key_eq(text + start, key_len, "swindows") != 0) {
            bit = S1_F_SWINDOWS;
            slot = &out->swindows;
        } else if (s1_key_eq(text + start, key_len, "slaunches") != 0) {
            bit = S1_F_SLAUNCHES;
            slot = &out->slaunches;
        } else if (s1_key_eq(text + start, key_len, "sreaps") != 0) {
            bit = S1_F_SREAPS;
            slot = &out->sreaps;
        } else if (s1_key_eq(text + start, key_len, "srestarts") != 0) {
            bit = S1_F_SRESTARTS;
            slot = &out->srestarts;
        } else if (s1_key_eq(text + start, key_len, "sapppid") != 0) {
            bit = S1_F_SAPPPID;
            slot = &out->sapppid;
        } else if (s1_key_eq(text + start, key_len, "stokenfail") != 0) {
            bit = S1_F_STOKENFAIL;
            slot = &out->stokenfail;
        }
        if (slot != NULL) {
            *slot = v;
            if ((seen & bit) == 0u) {
                seen |= bit;
                count++;
            }
        }
    }
    out->found = seen;
    return count;
}

int s1_mode_parse(const char *name)
{
    if (name == NULL) {
        return -1;
    }
    if (name[0] == 'a' && name[1] == '\0') {
        return S1_MODE_WM;
    }
    if (name[0] == 'b' && name[1] == '\0') {
        return S1_MODE_INPUT;
    }
    if (name[0] == 'c' && name[1] == '\0') {
        return S1_MODE_SESSIOND;
    }
    if (name[0] == 'd' && name[1] == '\0') {
        return S1_MODE_SPIN;
    }
    return -1;
}

const char *s1_mode_name(int mode)
{
    switch (mode) {
    case S1_MODE_WM:
        return "a";
    case S1_MODE_INPUT:
        return "b";
    case S1_MODE_SESSIOND:
        return "c";
    case S1_MODE_SPIN:
        return "d";
    default:
        return "?";
    }
}

u32 s1_step_count(int mode)
{
    switch (mode) {
    case S1_MODE_WM:
        return 4u;
    case S1_MODE_INPUT:
        return 3u;
    case S1_MODE_SESSIOND:
        return 2u;
    case S1_MODE_SPIN:
        return 1u;
    default:
        return 0u;
    }
}

const char *s1_step_name(int mode, u32 step)
{
    static const char *const names_wm[4] = {"session-up", "wm-restart",
                                           "demo-alive", "input-launch"};
    static const char *const names_input[3] = {"session-up", "app-reap",
                                              "input-recovery"};
    static const char *const names_sess[2] = {"session-up", "sessiond-dead"};
    static const char *const names_spin[1] = {"session-up"};

    if (step >= s1_step_count(mode)) {
        return NULL;
    }
    switch (mode) {
    case S1_MODE_WM:
        return names_wm[step];
    case S1_MODE_INPUT:
        return names_input[step];
    case S1_MODE_SESSIOND:
        return names_sess[step];
    case S1_MODE_SPIN:
        return names_spin[step];
    default:
        return NULL;
    }
}

int s1_step_action(int mode, u32 step)
{
    if (step >= s1_step_count(mode)) {
        return S1_ACT_NONE;
    }
    switch (mode) {
    case S1_MODE_WM:
        if (step == 0u) {
            return S1_ACT_KILL_WM;
        }
        if (step == 2u) {
            return S1_ACT_INPUT_REQ;
        }
        return (step == 3u) ? S1_ACT_DONE : S1_ACT_NONE;
    case S1_MODE_INPUT:
        if (step == 0u) {
            return S1_ACT_KILL_APP;
        }
        return (step == 1u) ? S1_ACT_INPUT_REQ : S1_ACT_DONE;
    case S1_MODE_SESSIOND:
        return (step == 0u) ? S1_ACT_KILL_SESSIOND : S1_ACT_DONE;
    case S1_MODE_SPIN:
        return S1_ACT_ARM_SPIN;
    default:
        return S1_ACT_NONE;
    }
}

void s1_ctx_capture(struct s1_ctx *ctx, const struct s1_status *st)
{
    if (ctx == NULL) {
        return;
    }
    ctx->wm_pid = 0;
    ctx->app_pid = 0;
    ctx->sessiond_pid = 0;
    ctx->launches0 = 0;
    ctx->reaps0 = 0;
    ctx->restarts0 = 0;
    ctx->kill_ok = 0;
    if (st == NULL) {
        return;
    }
    ctx->wm_pid = st->swmpid;
    ctx->app_pid = st->sapppid;
    ctx->sessiond_pid = st->spid;
    ctx->launches0 = st->slaunches;
    ctx->reaps0 = st->sreaps;
    ctx->restarts0 = st->srestarts;
}

/* "会话已起来 + 第一个应用在跑"：S1 每个场景的起点。 */
static int s1_up_ok(const struct s1_status *st)
{
    if ((st->found & S1_F_UP) != S1_F_UP) {
        return 0;
    }
    return (st->sess != 0u && st->spid != 0u && st->swmpid != 0u &&
            st->swmstate == 1u && st->sapps >= 1u && st->swindows >= 2u &&
            st->sapppid != 0u)
               ? 1
               : 0;
}

/* 应用被 kill 后重新 LAUNCH：启动计数 +1 且最后一个 app pid 换了。 */
static int s1_relaunch_ok(const struct s1_status *st,
                          const struct s1_ctx *ctx)
{
    return (st->slaunches > ctx->launches0 && st->sapppid != 0u &&
            st->sapppid != ctx->app_pid)
               ? 1
               : 0;
}

int s1_step_eval(int mode, u32 step, const struct s1_status *st,
                 const struct s1_ctx *ctx)
{
    if (st == NULL || ctx == NULL) {
        return S1_FAIL;
    }
    if (s1_step_count(mode) == 0u || step >= s1_step_count(mode)) {
        return S1_FAIL;
    }
    switch (mode) {
    case S1_MODE_WM:
        if (step == 0u) {
            return s1_up_ok(st) ? S1_OK : S1_WAIT;
        }
        if (step == 1u) {
            /* 新 WM：restarts 增加、wm pid 换了且状态 running。 */
            return (st->srestarts > ctx->restarts0 && st->swmpid != 0u &&
                    st->swmpid != ctx->wm_pid && st->swmstate == 1u)
                       ? S1_OK
                       : S1_WAIT;
        }
        if (step == 2u) {
            /* 应用不归 WM：app pid / 窗口都还在，且没有被回收。 */
            return (st->sapps >= 1u && st->sapppid == ctx->app_pid &&
                    st->sreaps == ctx->reaps0 && st->swindows >= 2u)
                       ? S1_OK
                       : S1_WAIT;
        }
        /*
         * 重启后的输入探测：新 WM 的自检 fallback 可能先自己 auto-launch 一次
         * （只一次），所以要求 launches 至少 +2 —— 主机连按两次 '1'，其中
         * 至少一次必须真的经热键走通（host 另外断言 via=key 行）。
         */
        return (st->slaunches >= ctx->launches0 + 2u && st->sapppid != 0u &&
                st->sapppid != ctx->app_pid)
                   ? S1_OK
                   : S1_WAIT;
    case S1_MODE_INPUT:
        if (step == 0u) {
            return s1_up_ok(st) ? S1_OK : S1_WAIT;
        }
        if (step == 1u) {
            /* 被 kill 的应用被回收，但 WM 没有跟着重启。 */
            return (st->sreaps > ctx->reaps0 &&
                    st->srestarts == ctx->restarts0 && st->swmpid != 0u &&
                    st->swmstate == 1u)
                       ? S1_OK
                       : S1_WAIT;
        }
        return s1_relaunch_ok(st, ctx) ? S1_OK : S1_WAIT;
    case S1_MODE_SESSIOND:
        if (step == 0u) {
            return s1_up_ok(st) ? S1_OK : S1_WAIT;
        }
        /*
         * sessiond 死亡后状态文件停在最后一次 SESSION_STATUS：spid 还是它，
         * WM 计数不变（没有被重启）。只要求确认"没恢复"，不要求恢复。
         */
        return (ctx->kill_ok != 0u && st->sess != 0u &&
                st->spid == ctx->sessiond_pid &&
                st->srestarts == ctx->restarts0 && st->swmstate == 1u)
                   ? S1_OK
                   : S1_WAIT;
    case S1_MODE_SPIN:
        return s1_up_ok(st) ? S1_OK : S1_WAIT;
    default:
        return S1_FAIL;
    }
}

u32 s1_u32_dec(char *out, u32 cap, u32 v)
{
    char tmp[11];
    u32 n = 0;
    u32 i;

    if (out == NULL || cap == 0u) {
        return 0;
    }
    if (v == 0u) {
        out[0] = '0';
        if (cap > 1u) {
            out[1] = '\0';
        }
        return 1;
    }
    while (v != 0u && n < 10u) {
        tmp[n++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    }
    for (i = 0; i < n; i++) {
        if (i + 1u >= cap) {
            break;
        }
        out[i] = tmp[n - 1u - i];
    }
    if (i < cap) {
        out[i] = '\0';
    }
    return i;
}

struct s1_buf {
    char *out;
    u32 cap;
    u32 len;
};

static void s1_buf_put(struct s1_buf *b, const char *s)
{
    u32 i = 0;

    while (s != NULL && s[i] != '\0') {
        if (b->len + 1u >= b->cap) {
            break;
        }
        b->out[b->len++] = s[i++];
    }
    if (b->cap > 0u) {
        b->out[b->len] = '\0';
    }
}

static void s1_buf_dec(struct s1_buf *b, u32 v)
{
    char num[12];

    (void)s1_u32_dec(num, (u32)sizeof(num), v);
    s1_buf_put(b, num);
}

u32 s1_format_result(char *out, u32 cap, int mode, int pass, u32 step)
{
    struct s1_buf b;

    if (out == NULL || cap == 0u) {
        return 0;
    }
    b.out = out;
    b.cap = cap;
    b.len = 0;
    out[0] = '\0';
    s1_buf_put(&b, "ng: s1 scenario ");
    s1_buf_put(&b, s1_mode_name(mode));
    s1_buf_put(&b, (pass != 0) ? " PASS (step=" : " FAIL (step=");
    s1_buf_dec(&b, step);
    s1_buf_put(&b, ")");
    return b.len;
}
