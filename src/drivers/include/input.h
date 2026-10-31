/*
 * ng WP2-3: L1D PS/2 键盘输入（端口 0x64 状态 / 0x60 数据，scancode set 1）。
 *
 * 结构：
 *   - `input_scancode_to_ascii()`：纯映射（set 1 + shift/caps/ctrl），host 单测
 *     直接打表验证；
 *   - `struct input_state`：修饰键状态 + 按下事件环形缓冲（128 项），释放事件
 *     只更新修饰键/计数、不占缓冲（readkey 只返回“按下”）；
 *   - `input_poll()`：轮询控制器输出缓冲，把字节喂给解码器；没有 IRQ 也能工作
 *     （WP3 之前不引入中断/定时器依赖）；
 *   - `ps2_init()`：清空输出、启用键盘接口（0xAE）、发 0xF4（enable scanning）
 *     并等 ACK（0xFA）。
 *
 * 端口 IO 通过 `struct input_io` 注入，host 单测用 fake 控制器验证编码/环。
 */
#ifndef NG_INPUT_H
#define NG_INPUT_H

#include "types.h"

#define PS2_STATUS_PORT 0x64u
#define PS2_DATA_PORT 0x60u

#define PS2_STATUS_OUTPUT_FULL 0x01u
#define PS2_STATUS_INPUT_FULL 0x02u
#define PS2_CMD_READ_CONFIG 0x20u
#define PS2_CMD_WRITE_CONFIG 0x60u
#define PS2_CMD_DISABLE_KBD 0xADu
#define PS2_CMD_ENABLE_KBD 0xAEu
#define PS2_CMD_DISABLE_AUX 0xA7u
#define PS2_CMD_ENABLE_AUX 0xA8u
#define PS2_CMD_WRITE_AUX 0xD4u
#define PS2_KBD_ENABLE_SCAN 0xF4u
#define PS2_ACK 0xFAu
#define PS2_RESEND 0xFEu
#define PS2_SCANCODE_EXTENDED 0xE0u
#define PS2_SCANCODE_RELEASE 0x80u
/* 状态寄存器的 bit5：1 = 输出字节来自 aux（鼠标）端口。 */
#define PS2_STATUS_AUX 0x20u
/* 状态寄存器 bit6/bit7：超时/校验错误（读到 0xFF 时按“没有控制器”处理）。 */
#define PS2_MOUSE_SET_DEFAULT 0xF6u /* 恢复默认（关数据报告） */
#define PS2_MOUSE_SET_SAMPLE 0xF3u  /* 采样率，参数 100 */
#define PS2_MOUSE_SET_RES 0xE8u     /* 分辨率，参数 3（8 counts/mm） */
#define PS2_MOUSE_ENABLE 0xF4u      /* 打开数据报告 */

#define INPUT_RING_SIZE 128u
#define INPUT_POLL_MAX 64u

/* 事件修饰位。 */
#define INPUT_F_SHIFT 0x01u
#define INPUT_F_CAPS 0x02u
#define INPUT_F_CTRL 0x04u
#define INPUT_F_EXT 0x08u

struct input_event {
    u8 scancode; /* set 1 的 make code（0x00..0x7F；扩展键保留原 code） */
    u8 ascii;    /* 可打印 ASCII；不可打印为 0 */
    u8 pressed;  /* 环里只放按下事件，恒为 1 */
    u8 flags;    /* INPUT_F_* */
};

struct input_ring {
    struct input_event ev[INPUT_RING_SIZE];
    u32 head; /* 下一个写位置 */
    u32 tail; /* 下一个读位置 */
    u32 count;
    u32 lost; /* 环满丢弃的按下事件数 */
};

struct input_state {
    struct input_ring ring;
    u8 mods;     /* INPUT_F_SHIFT/CAPS/CTRL（EXT 是逐事件标志，不放这里） */
    u8 pending_ext;
    u8 e1_skip;  /* Pause/Break 的 E1 序列：跳过后续字节 */
    u32 bytes;   /* 收到的 scancode 字节数 */
    u32 presses; /* 解码出的按下事件数 */
    u32 releases;
};

struct input_io {
    u8 (*inb)(void *ctx, u16 port);
    void (*outb)(void *ctx, u16 port, u8 value);
    void *ctx;
};

/* ------------------------------------------------------------------------- */
/* WP5-0h：统一输入服务（键盘 + 鼠标 -> 一条事件队列；syscall 96 的载荷）     */
/* ------------------------------------------------------------------------- */

/* 事件类型（同样写进 ABI 1.1 DRAFT 的 syscall 96；见 claims）。 */
#define INPUT_EV_KEY 1u
#define INPUT_EV_MOVE 2u
#define INPUT_EV_BUTTON 3u

/* 鼠标按键位（与 PS/2 包 byte0 的 bit0..2 同值）。 */
#define INPUT_BTN_LEFT 0x01u
#define INPUT_BTN_RIGHT 0x02u
#define INPUT_BTN_MIDDLE 0x04u

/* syscall 96 一次返回一个包（28 字节，字段顺序即 ABI）。 */
struct input_packet {
    u32 type;     /* INPUT_EV_* */
    u32 key;      /* KEY: ASCII（不可打印为 0）；BUTTON: 当前按键位图 */
    i32 x;        /* MOVE: 相对位移 dx（屏幕坐标系，右正） */
    i32 y;        /* MOVE: 相对位移 dy（屏幕坐标系，下正） */
    u32 buttons;  /* 当前按键位图（所有事件都带） */
    u32 scancode; /* KEY: set 1 make code；其它事件 0 */
    u32 flags;    /* KEY: INPUT_F_*；其它事件 0 */
};

#define INPUT_QUEUE_SIZE 64u

struct input_queue {
    struct input_packet ev[INPUT_QUEUE_SIZE];
    u32 head;
    u32 tail;
    u32 count;
    u32 lost;
};

void input_queue_reset(struct input_queue *q);
/* 0 = 入队；-1 = 满（lost++，包丢弃）。 */
int input_queue_push(struct input_queue *q, const struct input_packet *ev);
/* 0 = 取到；-1 = 空。 */
int input_queue_pop(struct input_queue *q, struct input_packet *out);

/* PS/2 鼠标解码状态：3 字节包，index=0 时等待 bit3=1 的同步字节。 */
struct input_mouse {
    u8 packet[3];
    u8 index;
    u32 buttons;
    u32 packets;       /* 完整 3 字节包数 */
    u32 moves;         /* 产生 MOVE 事件的包数 */
    u32 button_events; /* 产生 BUTTON 事件的次数 */
    u32 overflows;     /* x/y 溢出位命中的包数 */
    u32 bytes;         /* 收到的字节数（含 ACK） */
    i32 x;             /* 最近一次解码的屏幕方向位移 */
    i32 y;
};

void input_mouse_reset(struct input_mouse *ms);

/*
 * 解码一个 3 字节包（纯函数）。返回 0 成功；-1 = 溢出/非法（*buttons 仍更新）。
 * dx/dy 已换成屏幕坐标系（右/下为正）；PS/2 的 y 向上为正，所以内部取反。
 */
int input_mouse_decode(const u8 packet[3], u32 *buttons, i32 *dx, i32 *dy);

/*
 * 喂一个字节：0xFA/0xFE 在包边界上按 ACK/RESEND 忽略；凑满 3 字节时把
 * BUTTON（按键变化）和/或 MOVE（位移非 0）推进 q。返回推进的事件数（0..2）。
 */
int input_mouse_feed(struct input_mouse *ms, u8 byte, struct input_queue *q);

/*
 * 初始化 aux 口并打开鼠标数据报告：0xA7/0xA8 + 配置位 + 0xF6/0xF3/0xE8/0xF4。
 * 返回 0 = 全部 ACK；-1 = 没有控制器/无 ACK。
 */
int ps2_init_mouse(struct input_mouse *ms, const struct input_io *io);

/* ------------------------------------------------------------------------- */
/* WP5-0h：输入所有权（displayd 认领后 shell 的 readkey 让出）                */
/* ------------------------------------------------------------------------- */

struct input_owner {
    u32 pid;      /* 0 = 未认领 */
    u32 claims;   /* 成功认领次数 */
    u32 denials;  /* readkey 因所有权被拒的次数 */
    u32 releases; /* 显式/自动释放次数 */
};

void input_owner_reset(struct input_owner *o);

/*
 * 认领：pid=0 非法。已有属主且仍存活、且不是同一个 pid -> -1（denials++）；
 * 属主为 0 / 已死 / 自己 -> 成功（claims++，pid 更新）。owner_alive 由内核
 * 用进程表判断后注入，纯函数方便 host 单测覆盖“属主死了自动可认领”。
 */
int input_owner_claim(struct input_owner *o, u32 pid, int owner_alive);

/*
 * readkey 是否允许读：没有属主 / 自己是属主 -> 1；属主已死 -> 回收并返回 1；
 * 其它 -> 0（denials++）。
 */
int input_owner_may_read(struct input_owner *o, u32 pid, int owner_alive);

/* 只有属主自己能释放；成功返回 0，否则 -1。 */
int input_owner_release(struct input_owner *o, u32 pid);

void input_init(struct input_state *st);
void input_ring_reset(struct input_ring *r);

/* 0=入环；-1=满（lost++，事件丢弃）。 */
int input_ring_push(struct input_ring *r, const struct input_event *ev);

/* 0=取到；-1=空。 */
int input_ring_pop(struct input_ring *r, struct input_event *out);

/* 纯映射：set 1 make code + 修饰位 -> ASCII（0 = 不可打印/未知）。 */
u8 input_scancode_to_ascii(u8 scancode, u8 mods);

/*
 * 喂一个字节（含 release 位/扩展前缀）。返回：
 *   1 = 产生一个按下事件并入环；
 *   0 = 修饰键/释放/前缀/未知键，没有事件；
 *  -1 = 环满（事件被丢弃，lost++）。
 */
int input_feed(struct input_state *st, u8 byte);

/*
 * WP5-0h：同 input_feed()，但把本次产生的按下事件同时写进 *out（out 可空）。
 * 内核输入服务用它把同一个事件同时放进 readkey 的旧环和统一事件队列。
 */
int input_feed_ev(struct input_state *st, u8 byte, struct input_event *out);

/* 取下一个按下事件。0=取到，-1=空。 */
int input_next(struct input_state *st, struct input_event *out);

/* 轮询控制器：最多 max_bytes 个字节；返回实际喂入的字节数。 */
int input_poll(struct input_state *st, const struct input_io *io, u32 max_bytes);

/*
 * 初始化键盘接口并等 enable-scanning 的 ACK。
 * 返回 0=ACK 收到；-1=没有控制器/无 ACK（调用方可继续用轮询，只是可能没键）。
 */
int ps2_init(struct input_state *st, const struct input_io *io);

#endif /* NG_INPUT_H */
