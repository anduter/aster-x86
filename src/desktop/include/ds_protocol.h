/*
 * ng WP3-1: display server（L2a）协议 —— 控制面走 IPC（≤4 KiB 消息），
 * 数据面走共享内存（surface 像素，`SYS_SHM_ALLOC/MAP`）。
 *
 * 协议形状（与 docs/design/display-server-ng.md 一致）：
 *
 *   client                                 displayd
 *     port_create() -> C
 *     read /run/display.port -> S
 *     CREATE(w,h)  from=C          -->     分配 surface + SHM，回 CREATE_REPLY
 *     <--  CREATE_REPLY{id,shm_ticket,pages,x,y,w,h}
 *     SYS_SHM_MAP(shm_ticket) -> va        （客户端自己映射，数据面）
 *     MAP(id)                      -->     surface marked mapped
 *     COMMIT(id,x,y,w,h)           -->     合成到 LFB，读回像素证据
 *     FOCUS(id)                    -->     置焦点 + 提到最前
 *     <--  INPUT{id,kind,key,scancode,flags}   （推送给焦点客户端）
 *     DESTROY(id)                  -->
 *
 * 消息 = 20 字节头（magic/version/type/len/status/seq）+ 固定长度 body，
 * body 只用 u32，编码/解码是**纯函数**（host 单测直接驱动，见
 * tests/unit-ng/test_desktop.c）。IPC 的 16 字节头由各进程自己拼
 * （`struct ds_ipc_hdr` 与 struct ipc_msg_hdr 逐字节一致）。
 *
 * 像素格式：ARGB8888（u32 = 0xAARRGGBB）；LFB 上按 0x00RRGGBB 小端写
 * （QEMU stdvga = BGRX，内存字节序 B,G,R,X）。
 */
#ifndef NG_DS_PROTOCOL_H
#define NG_DS_PROTOCOL_H

#include "types.h"

/* ---- 发现文件（ramfs 文本；ticket 用十进制 + '\n'） ---------------------- */
#define DS_PORT_PATH "/run/display.port"
#define DS_STATUS_PATH "/run/display.status"

/* ---- 协议版本 / 尺寸 ------------------------------------------------------ */
#define DS_PROTO_MAGIC 0x4453314Du /* "DS1M"（小端 "M1SD" 的内存序） */
#define DS_PROTO_VERSION 1u
#define DS_MSG_HDR_SIZE 20u
#define DS_MSG_MAX 64u
#define DS_IPC_HDR_SIZE 16u

/* ---- 资源上限 ------------------------------------------------------------- */
#define DS_SURFACES_MAX 8u
#define DS_SURFACE_MAX_W 1280u
#define DS_SURFACE_MAX_H 800u
#define DS_SHM_PAGE_SIZE 4096u
#define DS_SHM_PAGES_MAX 1024u     /* 单 surface 缓冲区上限 4 MiB（1280x800x4） */
#define DS_CLIENT_PORT_DEPTH 16u

/* ---- 放置策略（displayd 侧；demo 与 run-ng 共用，便于像素断言） ---------- */
#define DS_FIRST_X 160u
#define DS_FIRST_Y 120u
#define DS_CASCADE 32u

/* ---- demo 表面契约（L3 demo -> L2 displayd + run-ng 像素断言共用） ------- */
#define DS_DEMO_W 400u
#define DS_DEMO_H 300u
#define DS_DEMO_ACCENT 0x003070C0u /* 实心矩形颜色；QMP 截图像素断言用 */
#define DS_DEMO_PROBE_X 25u        /* surface 内部探针（落在实心矩形里） */
#define DS_DEMO_PROBE_Y 25u

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

/* INPUT 事件类型 */
#define DS_INPUT_KEY_DOWN 1u
#define DS_INPUT_KEY_UP 2u

/* ---- body（全部 u32，长度固定） ------------------------------------------ */
struct ds_create_body {
    u32 w;
    u32 h;
    u32 flags;
    u32 reserved;
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
    u32 reserved;
};

struct ds_focus_body {
    u32 surface_id;
    u32 reserved;
};

struct ds_input_body {
    u32 surface_id;
    u32 kind; /* DS_INPUT_KEY_* */
    u32 key;  /* ASCII（不可打印为 0） */
    u32 scancode;
    u32 flags; /* INPUT_F_*（与 PS/2 驱动同值） */
};

struct ds_bye_body {
    u32 reason;
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
        u32 words[8];
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
/* 返回该类型的 body 字节数；未知类型返回 0。 */
u32 ds_msg_body_size(u32 type);
/* 整条消息（头+body）字节数；未知类型返回 0。 */
u32 ds_msg_size(u32 type);

/*
 * 编码：magic/version/len 由编码器填（m->type/status/seq/body 由调用方填，
 * 编码器不修改 m）。成功返回总字节数；cap 不够或类型未知返回 DS_EINVAL。
 */
#define DS_OK 0
#define DS_ENOENT (-2)
#define DS_EIO (-5)
#define DS_EBADF (-9)
#define DS_ENOMEM (-12)
#define DS_EFAULT (-14)
#define DS_EBUSY (-16)
#define DS_EINVAL (-22)
#define DS_ENOSPC (-28)
#define DS_ENODEV (-19)
#define DS_EAGAIN (-11)
#define DS_ETIMEDOUT (-110)

int ds_msg_encode(const struct ds_msg *m, void *buf, u32 cap);

/*
 * 解码：严格校验 magic/version/类型/总长（多一个字节也拒），成功后
 * *out 完整可用。返回 DS_OK / 负错误码。
 */
int ds_msg_decode(const void *buf, u32 len, struct ds_msg *out);

/* 取 body 长度时只允许已知类型，且 body 与常量表一致。 */
int ds_msg_status_ok(const struct ds_msg *m);

/* ---- 极简 mem 原语（freestanding；内核/user/host 三处同一份） ------------ */
void ds_memzero(void *p, u64 n);
void ds_memcopy(void *dst, const void *src, u64 n);
int ds_strlen(const char *s);
int ds_streq(const char *a, const char *b);

#endif /* NG_DS_PROTOCOL_H */
