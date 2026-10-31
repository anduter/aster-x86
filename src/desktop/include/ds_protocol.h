/*
 * ng WP3-2: display server（L2a）协议 v2 —— 控制面走 IPC（≤4 KiB 消息），
 * 数据面走共享内存（surface 像素，`SYS_SHM_ALLOC/MAP`）。
 *
 * v1（WP3-1）形状：
 *
 *   client                                 displayd
 *     port_create() -> C
 *     read /run/display.port -> S
 *     CREATE(w,h)  from=C          -->     分配 surface + SHM，回 CREATE_REPLY
 *     <--  CREATE_REPLY{id,shm_handle,pages,x,y,w,h}
 *     SYS_SHM_MAP(shm_handle) -> va        （客户端自己映射，数据面）
 *     MAP(id)                      -->     surface marked mapped
 *     COMMIT(id,x,y,w,h)           -->     合成到 LFB，读回像素证据
 *     FOCUS(id)                    -->     置焦点 + 提到最前
 *     <--  INPUT{id,kind,key,scancode,flags}   （推送给焦点客户端）
 *     DESTROY(id)                  -->
 *
 * v2（WP3-2，**最小扩展**，只追加）：
 *
 *   - CREATE 追加 `x,y,owner_pid,title[4]`：WM 需要把面板放到 (0,0)
 *     （flags 的 DS_CREATE_F_POS 位），并让 LIST 能报出窗口标题/属主 pid。
 *     **v1 兼容**：v1 客户端发 16 字节 body（w,h,flags,reserved），服务端把
 *     title 当空串、忽略 x/y、owner_pid 记 0；解析器同时接受 version 1/2。
 *   - LIST / LIST_REPLY：WM 每次轮询拿一份 surface 表快照
 *     `{id, owner_pid, title, x,y,w,h, focused}`（最多 DS_LIST_MAX 条）。
 *   - REGISTER_HOTKEY：WM 注册全局热键（key=DS_HOTKEY_ANY 表示所有键）。
 *     displayd 把每个按键**先给 WM 一份**（INPUT 带 DS_INPUT_F_GLOBAL），
 *     再按焦点正常转发；这样 WM 能用 Tab/1..9 做任务切换/启动器。
 *   - WM_STATUS：WM 把切换/启动计数报给 displayd，displayd 汇总进已有的
 *     `/run/display.status`（不新建 ramfs 节点；内核读同一份状态）。
 *
 * v3（WP3-3，**只追加，v1/v2 兼容**）：
 *
 *   - CREATE 追加 `token`（session manager 签发的 32 位 role_token）：surface
 *     记住属主令牌，LIST 把它报回去，FOCUS 用令牌做归属校验。
 *   - LIST_REPLY 的每条 entry 追加 `token`（44 -> 48 字节）。
 *   - FOCUS 的第二个 u32 从 reserved 变成 `token`：令牌匹配 surface 自己的
 *     令牌、或令牌角色是 WM（shell 可以切换任何窗口）才允许改焦点；v3 且
 *     token=0 走旧行为（兼容 WP3-1/3-2 客户端）。v2 客户端发 0 也走旧行为。
 *   - 解码器接受 version 1..3；v1 CREATE 16 字节、v2 CREATE 40 字节、v3 44。
 *
 * 消息 = 20 字节头（magic/version/type/len/status/seq）+ 固定长度 body，
 * body 只用 u32，编码/解码是**纯函数**（host 单测直接驱动，见
 * tests/unit-ng/test_desktop.c / test_wm.c）。IPC 的 16 字节头由各进程自己拼
 * （`struct ds_ipc_hdr` 与 struct ipc_msg_hdr 逐字节一致）。
 *
 * 像素格式：ARGB8888（u32 = 0xAARRGGBB）；LFB 上按 0x00RRGGBB 小端写
 * （QEMU stdvga = BGRX，内存字节序 B,G,R,X）。
 */
#ifndef NG_DS_PROTOCOL_H
#define NG_DS_PROTOCOL_H

#include "types.h"

/* ---- 发现/状态文件（ramfs 文本；ticket 用十进制 + '\n'） ----------------- */
#define DS_PORT_PATH "/run/display.port"
#define DS_STATUS_PATH "/run/display.status"

/* ---- 协议版本 / 尺寸 ------------------------------------------------------ */
#define DS_PROTO_MAGIC 0x4453314Du /* "DS1M"（小端 "M1SD" 的内存序） */
#define DS_PROTO_VERSION 3u
#define DS_PROTO_VERSION_MIN 1u /* v1/v2 客户端仍然可用（CREATE 无 token） */
#define DS_MSG_HDR_SIZE 20u
#define DS_MSG_MAX 512u        /* 最大单条消息（LIST_REPLY 380 字节） */
#define DS_MSG_WORDS_MAX 123u  /* ds_msg 的 body 联合体：DS_MSG_MAX - 20 字节头 */
#define DS_IPC_HDR_SIZE 16u
#define DS_TITLE_MAX 16u /* 15 字符 + NUL，按 4 个 u32 打包 */
#define DS_TITLE_WORDS (DS_TITLE_MAX / 4u)

/* ---- 资源上限 ------------------------------------------------------------- */
#define DS_SURFACES_MAX 8u
#define DS_LIST_MAX 8u /* LIST_REPLY 最多返回的 surface 数（= DS_SURFACES_MAX） */
#define DS_SURFACE_MAX_W 1280u
#define DS_SURFACE_MAX_H 800u
#define DS_SHM_PAGE_SIZE 4096u
#define DS_SHM_PAGES_MAX 1024u /* 单 surface 缓冲区上限 4 MiB（1280x800x4） */
#define DS_CLIENT_PORT_DEPTH 16u

/* ---- 放置策略（displayd 侧；demo 与 run-ng 共用，便于像素断言） ---------- */
#define DS_FIRST_X 160u
#define DS_FIRST_Y 120u
#define DS_CASCADE 32u

/* CREATE.flags：x/y 有效（v2）。 */
#define DS_CREATE_F_POS 0x1u
/* v1 CREATE body 只有 16 字节（w,h,flags,reserved）。 */
#define DS_CREATE_BODY_V1 16u
/* v2 CREATE body 40 字节（没有 token）。 */
#define DS_CREATE_BODY_V2 40u
/* v2 LIST_REPLY body 360 字节（entry 44；v3 entry 48 -> 392）。 */
#define DS_LIST_REPLY_BODY_V2 360u

/* ---- demo 表面契约（L3 demo -> L2 displayd + run-ng 像素断言共用） ------- */
#define DS_DEMO_W 400u
#define DS_DEMO_H 300u
#define DS_DEMO_ACCENT 0x003070C0u /* 实心矩形颜色；QMP 截图像素断言用 */
#define DS_DEMO_PROBE_X 25u        /* surface 内部探针（落在实心矩形里） */
#define DS_DEMO_PROBE_Y 25u
#define DS_DEMO_TITLE "Demo"       /* v2 CREATE title：WM 任务列表里显示这个 */

/* ---- 时序（TSC tick；QEMU TCG 下 rdtsc 约 1 GHz，20e6 ≈ 20ms） ---------- */
#define DS_POLL_TICKS 20000000ULL
#define DS_INPUT_WAIT_TICKS 20000000ULL
#define DS_INPUT_WAIT_TRIES 100u
#define DS_REPLY_WAIT_TICKS 400000000ULL
#define DS_REPLY_WAIT_TRIES 20u

/* ---- SYS_FB_MAP 的返回结构 ------------------------------------------------ */
#define DS_FB_FLAG_PCD 0x1u
#define DS_FB_FLAG_PWT 0x2u
#define DS_FB_FLAG_NX 0x4u

struct ds_fb_info {
    u64 base;  /* 内核映射进用户 AS 后的 VA */
    u64 phys;  /* 物理 LFB 基址（证据行打印用） */
    u64 size;  /* pitch * height（字节） */
    u32 pitch; /* 每行字节数 */
    u32 bpp;   /* 16/24/32 */
    u32 width;
    u32 height;
    u32 flags; /* DS_FB_FLAG_* */
    u32 reserved;
};
_Static_assert(sizeof(struct ds_fb_info) == 48, "ds_fb_info must be 48 bytes");

/* ---- 消息类型 ------------------------------------------------------------- */
#define DS_MSG_CREATE 1u
#define DS_MSG_CREATE_REPLY 2u
#define DS_MSG_MAP 3u
#define DS_MSG_COMMIT 4u
#define DS_MSG_DESTROY 5u
#define DS_MSG_FOCUS 6u
#define DS_MSG_INPUT 7u
#define DS_MSG_BYE 8u
#define DS_MSG_LIST 9u            /* v2：client -> server，要一份 surface 快照 */
#define DS_MSG_LIST_REPLY 10u     /* v2：server -> client，count + entries[] */
#define DS_MSG_REGISTER_HOTKEY 11u /* v2：WM 注册全局热键 */
#define DS_MSG_WM_STATUS 12u      /* v2：WM -> server，切换/启动计数（状态证据） */
#define DS_MSG_SESSION_STATUS 13u /* v3：sessiond -> server，会话/令牌状态汇总 */

/* INPUT 事件类型 */
#define DS_INPUT_KEY_DOWN 1u
#define DS_INPUT_KEY_UP 2u
/* INPUT 只在"全局热键副本"里带这个位（普通焦点转发不带）。 */
#define DS_INPUT_F_GLOBAL 0x100u

/* REGISTER_HOTKEY.key：0 = 所有按键都先给注册者一份。 */
#define DS_HOTKEY_ANY 0u

/* ---- body（全部 u32，长度固定） ------------------------------------------ */
struct ds_create_body {
    u32 w;
    u32 h;
    u32 flags; /* DS_CREATE_F_* */
    u32 x;     /* flags & DS_CREATE_F_POS 时有效 */
    u32 y;
    u32 owner_pid;               /* v2：客户端 getpid()；v1 没有（记 0） */
    u32 title[DS_TITLE_WORDS];   /* v2：NUL 结尾标题；v1 没有（空串） */
    u32 token;                   /* v3：session manager 签发的 role_token；旧版本 0 */
};

struct ds_create_reply_body {
    u32 surface_id;
    u32 shm_handle;
    u32 pages;
    u32 x;
    u32 y;
    u32 w;
    u32 h;
};

struct ds_map_body {
    u32 surface_id;
    u32 reserved;
};

struct ds_commit_body {
    u32 surface_id;
    u32 x;
    u32 y;
    u32 w;
    u32 h;
};

struct ds_destroy_body {
    u32 surface_id;
    u32 token; /* v3：请求者令牌（v1/v2 的 reserved 必须 0） */
};

struct ds_focus_body {
    u32 surface_id;
    u32 token; /* v3：请求者令牌（0 = 旧行为；v1/v2 的 reserved 必须 0） */
};

struct ds_input_body {
    u32 surface_id;
    u32 kind; /* DS_INPUT_KEY_* */
    u32 key;  /* ASCII（不可打印为 0） */
    u32 scancode;
    u32 flags; /* INPUT_F_*（与 PS/2 驱动同值）| DS_INPUT_F_GLOBAL */
};

struct ds_bye_body {
    u32 reason;
    u32 reserved;
};

struct ds_list_body {
    u32 reserved;
};

/* LIST_REPLY 的一条 surface 记录（v3 48 字节；8 条 = 384 字节）。 */
struct ds_list_entry {
    u32 id;
    u32 owner_pid;
    u32 x;
    u32 y;
    u32 w;
    u32 h;
    u32 focused; /* 1 = 当前焦点 */
    u32 token;   /* v3：surface 的 role_token（v2 及以前为 0） */
    u32 title[DS_TITLE_WORDS];
};

struct ds_list_reply_body {
    u32 count; /* 有效 entry 数（0..DS_LIST_MAX） */
    u32 reserved;
    struct ds_list_entry entries[DS_LIST_MAX];
};

struct ds_register_hotkey_body {
    u32 surface_id; /* 注册者的 surface（INPUT 副本里的 surface_id） */
    u32 key;        /* DS_HOTKEY_ANY 或某个 ASCII */
    u32 flags;      /* 保留，必须 0 */
    u32 token;      /* v3：请求者 role_token；v2 的 reserved 必须 0 */
};

/*
 * v2：WM 把任务切换/启动器计数报给 displayd。displayd 把它们写进现有的
 * `/run/display.status`（**不新建文件**：ramfs 节点池只有 32 个，已经用满；
 * WM 的自治状态经 displayd 汇总，内核读同一个状态文件）。
 */
struct ds_wm_status_body {
    u32 surface_id; /* WM 面板 */
    u32 tasks;      /* 可切换窗口数 */
    u32 focus;      /* 当前焦点 surface id */
    u32 switches;   /* 完成的任务切换次数 */
    u32 lastswitch; /* 最近一次切换的目标 id */
    u32 launches;   /* 启动器 spawn 次数 */
    u32 lastpid;    /* 最近一次 spawn 的 pid */
};

/*
 * v3：sessiond（L2c）把会话状态报给 displayd，displayd 汇总进已有的
 * `/run/display.status`（**不新建 ramfs 节点**：节点池只有 32 个，WP3-3 又
 * 种了一个 /bin/sessiond；内核 wp33 读同一份状态文件的 s* 字段）。
 */
struct ds_session_status_body {
    u32 pid;      /* sessiond 的 pid */
    u32 dpid;     /* sessiond 拉起的 displayd pid */
    u32 procs;    /* 会话表里的进程数（WM + 应用） */
    u32 apps;     /* L3 应用数 */
    u32 windows;  /* 有窗口的会话项 */
    u32 tokens;   /* 已签发令牌数 */
    u32 launches; /* LAUNCH 成功次数 */
    u32 kills;    /* KILL 次数 */
    u32 exits;    /* EXIT 次数 */
    u32 reaps;    /* 崩溃回收次数 */
    u32 restarts; /* WM 重启次数 */
    u32 wmpid;    /* 当前 WM pid（0 = 没有） */
    u32 wmstate;  /* 0 none / 1 running / 2 等待重启 */
    u32 apppid;   /* 最近一个运行中的应用 pid */
    u32 tokenfail;/* 令牌/pid 绑定不一致次数（应为 0） */
    u32 registry; /* 最近一次与 displayd LIST 同步是否通过（1 = 通过） */
    u32 reserved;
};

/* ---- 解码后的完整消息 ----------------------------------------------------- */
struct ds_msg {
    u32 magic;
    u16 version;
    u16 type;
    u32 len;    /* 含 20 字节头的总长 */
    i32 status; /* 回复里 0 = OK，负 = errno */
    u32 seq;
    union {
        struct ds_create_body create;
        struct ds_create_reply_body create_reply;
        struct ds_map_body map;
        struct ds_commit_body commit;
        struct ds_destroy_body destroy;
        struct ds_focus_body focus;
        struct ds_input_body input;
        struct ds_bye_body bye;
        struct ds_list_body list;
        struct ds_list_reply_body list_reply;
        struct ds_register_hotkey_body register_hotkey;
        struct ds_wm_status_body wm_status;
        struct ds_session_status_body session_status;
        u32 words[DS_MSG_WORDS_MAX];
    } u;
};

/* ---- IPC 16 字节头（与 struct ipc_msg_hdr 逐字节一致） ------------------- */
struct ds_ipc_hdr {
    u32 type;
    u32 len;
    u32 from;
    u32 flags;
};
_Static_assert(sizeof(struct ds_ipc_hdr) == DS_IPC_HDR_SIZE,
               "ds_ipc_hdr must be 16 bytes");

/* ---- 纯函数：body 长度 / 编解码 ------------------------------------------ */
/* 返回该类型在**当前协议版本（v2）**下的 body 字节数；未知类型返回 0。 */
u32 ds_msg_body_size(u32 type);
/* 按版本取 body 长度（v1 的 CREATE 是 16，LIST/REGISTER_HOTKEY 只存在于 v2）。 */
u32 ds_msg_body_size_v(u32 type, u16 version);
/* 整条消息（头+body）字节数；未知类型/未知版本返回 0。 */
u32 ds_msg_size(u32 type);
u32 ds_msg_size_v(u32 type, u16 version);

/*
 * 编码：magic/version/len 由编码器填（m->type/status/seq/body 由调用方填，
 * 编码器不修改 m）。成功返回总字节数；cap 不够或类型未知返回 DS_EINVAL。
 */
#define DS_OK 0
#define DS_EPERM (-1)
#define DS_ENOENT (-2)
#define DS_EIO (-5)
#define DS_EBADF (-9)
#define DS_EAGAIN (-11)
#define DS_ENOMEM (-12)
#define DS_EFAULT (-14)
#define DS_EBUSY (-16)
#define DS_ENODEV (-19)
#define DS_EINVAL (-22)
#define DS_ENOSPC (-28)
#define DS_ETIMEDOUT (-110)

int ds_msg_encode(const struct ds_msg *m, void *buf, u32 cap);

/*
 * 解码：严格校验 magic/version/类型/总长（多一个字节也拒），成功后
 * *out 完整可用。version 接受 DS_PROTO_VERSION_MIN..DS_PROTO_VERSION，
 * 所以 v1 客户端（CREATE 无 title）与 v2 客户端共用同一个解码器。
 */
int ds_msg_decode(const void *buf, u32 len, struct ds_msg *out);

/* 取 body 长度时只允许已知类型，且 body 与常量表一致。 */
int ds_msg_status_ok(const struct ds_msg *m);

/*
 * v3：REGISTER_HOTKEY 的令牌校验（纯函数，host 单测覆盖）。
 *   - 还没有 WM 面板（wm_token == 0）：接受任意令牌（v2 直连模式 / 启动早期）；
 *   - 已经有 WM 面板：只接受 WM 自己的令牌（防止后来者抢占全局热键通道）。
 * 返回 1 = 允许，0 = 拒绝。
 */
int ds_hotkey_token_ok(u32 wm_token, u32 token);

/* ---- 标题打包（4 个 u32 = 16 字节，NUL padding） ------------------------- */
void ds_title_set(u32 out[DS_TITLE_WORDS], const char *s);
void ds_title_get(const u32 in[DS_TITLE_WORDS], char *out, u32 cap);

/* 填一条 LIST_REPLY 记录（标题截断/补 NUL；token 见 v3）。 */
void ds_list_entry_set(struct ds_list_entry *e, u32 id, u32 owner_pid, u32 x,
                       u32 y, u32 w, u32 h, u32 focused, u32 token,
                       const char *title);

/* ---- 极简 mem 原语（freestanding；内核/user/host 三处同一份） ------------ */
void ds_memzero(void *p, u64 n);
void ds_memcopy(void *dst, const void *src, u64 n);
int ds_strlen(const char *s);
int ds_streq(const char *a, const char *b);

#endif /* NG_DS_PROTOCOL_H */
