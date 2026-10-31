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
#define PS2_KBD_ENABLE_SCAN 0xF4u
#define PS2_ACK 0xFAu
#define PS2_RESEND 0xFEu
#define PS2_SCANCODE_EXTENDED 0xE0u
#define PS2_SCANCODE_RELEASE 0x80u

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
