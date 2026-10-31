/*
 * ng WP2-3: PS/2 键盘驱动实现（见 include/input.h）。
 *
 * scancode set 1 映射表用 C11 designated initializer，只列有意义的键；未列出的
 * 键 ascii=0，但仍以 scancode 事件交给上层（例如方向键/F 键）。
 */
#include "input.h"

/* ---- set 1 基本映射（make code 0x00..0x7F）-------------------------------- */
static const u8 keymap[128] = {
    [0x01] = 27,   /* Esc */
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=', [0x0E] = 8, [0x0F] = 9, /* Backspace/Tab */
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '[', [0x1B] = ']', [0x1C] = 13, /* Enter */
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
    [0x27] = ';', [0x28] = '\'', [0x29] = '`', [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm',
    [0x33] = ',', [0x34] = '.', [0x35] = '/', [0x37] = '*', [0x39] = ' ',
    /* 小键盘（NumLock 未建模，按数字处理） */
    [0x47] = '7', [0x48] = '8', [0x49] = '9', [0x4A] = '-',
    [0x4B] = '4', [0x4C] = '5', [0x4D] = '6', [0x4E] = '+',
    [0x4F] = '1', [0x50] = '2', [0x51] = '3', [0x52] = '0', [0x53] = '.'
};

/* Shift 下与基本映射不同的键（字母另按大小写处理）。 */
static const u8 keymap_shift[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
    [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')',
    [0x0C] = '_', [0x0D] = '+',
    [0x1A] = '{', [0x1B] = '}',
    [0x27] = ':', [0x28] = '"', [0x29] = '~', [0x2B] = '|',
    [0x33] = '<', [0x34] = '>', [0x35] = '?'
};

u8 input_scancode_to_ascii(u8 scancode, u8 mods)
{
    u8 base;

    if (scancode >= 128u) {
        return 0;
    }
    base = keymap[scancode];
    if (base == 0) {
        return 0;
    }
    if (base >= 'a' && base <= 'z') {
        int upper = ((mods & INPUT_F_SHIFT) != 0) ^ ((mods & INPUT_F_CAPS) != 0);

        return (u8)(upper ? (u8)(base - ('a' - 'A')) : base);
    }
    if ((mods & INPUT_F_SHIFT) != 0) {
        u8 shifted = keymap_shift[scancode];

        if (shifted != 0) {
            return shifted;
        }
    }
    return base;
}

void input_ring_reset(struct input_ring *r)
{
    if (r == NULL) {
        return;
    }
    r->head = 0;
    r->tail = 0;
    r->count = 0;
    r->lost = 0;
}

int input_ring_push(struct input_ring *r, const struct input_event *ev)
{
    if (r == NULL || ev == NULL) {
        return -1;
    }
    if (r->count >= INPUT_RING_SIZE) {
        r->lost++;
        return -1;
    }
    r->ev[r->head] = *ev;
    r->head = (r->head + 1u) % INPUT_RING_SIZE;
    r->count++;
    return 0;
}

int input_ring_pop(struct input_ring *r, struct input_event *out)
{
    if (r == NULL || out == NULL || r->count == 0) {
        return -1;
    }
    *out = r->ev[r->tail];
    r->tail = (r->tail + 1u) % INPUT_RING_SIZE;
    r->count--;
    return 0;
}

void input_init(struct input_state *st)
{
    if (st == NULL) {
        return;
    }
    input_ring_reset(&st->ring);
    st->mods = 0;
    st->pending_ext = 0;
    st->e1_skip = 0;
    st->bytes = 0;
    st->presses = 0;
    st->releases = 0;
}

int input_feed(struct input_state *st, u8 byte)
{
    return input_feed_ev(st, byte, NULL);
}

int input_feed_ev(struct input_state *st, u8 byte, struct input_event *out)
{
    u8 code;
    u8 flags;
    u8 ascii;
    int pressed;
    struct input_event ev;
    int ext;

    if (st == NULL) {
        return 0;
    }
    st->bytes++;
    if (st->e1_skip > 0) {
        st->e1_skip--;
        return 0;
    }
    if (byte == 0xE1u) {
        st->e1_skip = 5; /* E1 1D 45 E1 9D C5 = Pause，整段忽略 */
        return 0;
    }
    if (byte == PS2_SCANCODE_EXTENDED) {
        st->pending_ext = 1;
        return 0;
    }
    ext = st->pending_ext ? 1 : 0;
    st->pending_ext = 0;
    pressed = (byte & PS2_SCANCODE_RELEASE) ? 0 : 1;
    code = (u8)(byte & 0x7Fu);

    /* 修饰键：只更新状态，不产生字符事件。 */
    if (!ext && (code == 0x2Au || code == 0x36u)) {
        if (pressed) {
            st->mods |= INPUT_F_SHIFT;
        } else {
            st->mods &= (u8)~INPUT_F_SHIFT;
        }
        return 0;
    }
    if (code == 0x1Du) { /* 左 Ctrl（0x1D）或 E0 1D 右 Ctrl */
        if (pressed) {
            st->mods |= INPUT_F_CTRL;
        } else {
            st->mods &= (u8)~INPUT_F_CTRL;
        }
        return 0;
    }
    if (!ext && code == 0x3Au) { /* CapsLock：按下时翻转 */
        if (pressed) {
            st->mods ^= INPUT_F_CAPS;
        }
        return 0;
    }
    if (!pressed) {
        st->releases++;
        return 0;
    }

    flags = st->mods;
    if (ext) {
        flags |= INPUT_F_EXT;
    }
    /* 扩展键不映射 ASCII（方向键/Home 等交给上层按 scancode 解释）。 */
    ascii = ext ? 0 : input_scancode_to_ascii(code, st->mods);
    ev.scancode = code;
    ev.ascii = ascii;
    ev.pressed = 1;
    ev.flags = flags;
    st->presses++;
    if (input_ring_push(&st->ring, &ev) != 0) {
        return -1;
    }
    if (out != NULL) {
        *out = ev;
    }
    return 1;
}

int input_next(struct input_state *st, struct input_event *out)
{
    if (st == NULL) {
        return -1;
    }
    return input_ring_pop(&st->ring, out);
}

int input_poll(struct input_state *st, const struct input_io *io, u32 max_bytes)
{
    u32 n = 0;

    if (st == NULL || io == NULL || io->inb == NULL) {
        return 0;
    }
    while (n < max_bytes) {
        u8 status = io->inb(io->ctx, PS2_STATUS_PORT);

        if (status == 0xFFu || (status & PS2_STATUS_OUTPUT_FULL) == 0) {
            break;
        }
        (void)input_feed(st, io->inb(io->ctx, PS2_DATA_PORT));
        n++;
    }
    return (int)n;
}

static int wait_input_clear(const struct input_io *io)
{
    u32 i;

    for (i = 0; i < 100000u; i++) {
        u8 status = io->inb(io->ctx, PS2_STATUS_PORT);

        if (status == 0xFFu) {
            return -1;
        }
        if ((status & PS2_STATUS_INPUT_FULL) == 0) {
            return 0;
        }
    }
    return -1;
}

static void flush_output(const struct input_io *io)
{
    u32 i;

    for (i = 0; i < 64u; i++) {
        u8 status = io->inb(io->ctx, PS2_STATUS_PORT);

        if (status == 0xFFu || (status & PS2_STATUS_OUTPUT_FULL) == 0) {
            return;
        }
        (void)io->inb(io->ctx, PS2_DATA_PORT);
    }
}

static int kbd_write(const struct input_io *io, u8 cmd)
{
    if (wait_input_clear(io) != 0) {
        return -1;
    }
    io->outb(io->ctx, PS2_DATA_PORT, cmd);
    return 0;
}

int ps2_init(struct input_state *st, const struct input_io *io)
{
    u32 attempt;

    if (st == NULL || io == NULL || io->inb == NULL || io->outb == NULL) {
        return -1;
    }
    input_init(st);
    if (io->inb(io->ctx, PS2_STATUS_PORT) == 0xFFu) {
        return -1; /* 没有 i8042 */
    }
    flush_output(io);

    if (wait_input_clear(io) == 0) {
        io->outb(io->ctx, PS2_STATUS_PORT, PS2_CMD_DISABLE_KBD);
    }
    if (wait_input_clear(io) == 0) {
        io->outb(io->ctx, PS2_STATUS_PORT, PS2_CMD_ENABLE_KBD);
    }
    flush_output(io);

    /* enable scanning；QEMU 正常回 0xFA，若回 0xFE（resend）重试几次。 */
    for (attempt = 0; attempt < 3u; attempt++) {
        u32 i;

        if (kbd_write(io, PS2_KBD_ENABLE_SCAN) != 0) {
            return -1;
        }
        for (i = 0; i < 100000u; i++) {
            u8 status = io->inb(io->ctx, PS2_STATUS_PORT);

            if (status == 0xFFu) {
                return -1;
            }
            if ((status & PS2_STATUS_OUTPUT_FULL) == 0) {
                continue;
            }
            {
                u8 data = io->inb(io->ctx, PS2_DATA_PORT);

                if (data == PS2_ACK) {
                    return 0;
                }
                if (data == PS2_RESEND) {
                    break;
                }
                /* 其它扫描码（初始化期间残留）照常解码，不阻塞 ACK 等待。 */
                (void)input_feed(st, data);
            }
        }
    }
    return -1;
}

/* ========================================================================== */
/* WP5-0h：统一输入队列 / PS/2 鼠标 / 所有权                                   */
/* ========================================================================== */

void input_queue_reset(struct input_queue *q)
{
    if (q == NULL) {
        return;
    }
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->lost = 0;
}

int input_queue_push(struct input_queue *q, const struct input_packet *ev)
{
    if (q == NULL || ev == NULL) {
        return -1;
    }
    if (q->count >= INPUT_QUEUE_SIZE) {
        q->lost++;
        return -1;
    }
    q->ev[q->head] = *ev;
    q->head = (q->head + 1u) % INPUT_QUEUE_SIZE;
    q->count++;
    return 0;
}

int input_queue_pop(struct input_queue *q, struct input_packet *out)
{
    if (q == NULL || out == NULL || q->count == 0) {
        return -1;
    }
    *out = q->ev[q->tail];
    q->tail = (q->tail + 1u) % INPUT_QUEUE_SIZE;
    q->count--;
    return 0;
}

void input_mouse_reset(struct input_mouse *ms)
{
    u32 i;

    if (ms == NULL) {
        return;
    }
    for (i = 0; i < 3u; i++) {
        ms->packet[i] = 0;
    }
    ms->index = 0;
    ms->buttons = 0;
    ms->packets = 0;
    ms->moves = 0;
    ms->button_events = 0;
    ms->overflows = 0;
    ms->bytes = 0;
    ms->x = 0;
    ms->y = 0;
}

int input_mouse_decode(const u8 packet[3], u32 *buttons, i32 *dx, i32 *dy)
{
    u8 flags;
    i32 rx;
    i32 ry;

    if (packet == NULL) {
        return -1;
    }
    flags = packet[0];
    if (buttons != NULL) {
        *buttons = (u32)(flags & 0x07u);
    }
    rx = (i32)packet[1];
    ry = (i32)packet[2];
    if ((flags & 0x10u) != 0u) {
        rx -= 256; /* byte1 的符号位（9 位补码的低 8 位） */
    }
    if ((flags & 0x20u) != 0u) {
        ry -= 256; /* byte2 的符号位 */
    }
    if (dx != NULL) {
        *dx = rx;
    }
    if (dy != NULL) {
        *dy = -ry; /* PS/2 的 y 向上为正；屏幕坐标向下为正 */
    }
    return ((flags & 0xC0u) != 0u) ? -1 : 0;
}

int input_mouse_feed(struct input_mouse *ms, u8 byte, struct input_queue *q)
{
    u32 buttons = 0;
    struct input_packet ev;
    i32 dx = 0;
    i32 dy = 0;
    int pushed = 0;

    if (ms == NULL) {
        return 0;
    }
    ms->bytes++;
    if (ms->index == 0u) {
        /* 初始化 ACK/RESEND 或失步字节：丢掉，等新的 bit3=1 同步字节。 */
        if (byte == PS2_ACK || byte == PS2_RESEND) {
            return 0;
        }
        if ((byte & 0x08u) == 0u) {
            return 0;
        }
    }
    ms->packet[ms->index] = byte;
    ms->index++;
    if (ms->index < 3u) {
        return 0;
    }
    ms->index = 0;
    ms->packets++;

    if (input_mouse_decode(ms->packet, &buttons, &dx, &dy) != 0) {
        ms->overflows++;
        return 0;
    }
    ms->x = dx;
    ms->y = dy;

    if (buttons != ms->buttons) {
        ms->buttons = buttons;
        ms->button_events++;
        ev.type = INPUT_EV_BUTTON;
        ev.key = buttons;
        ev.x = 0;
        ev.y = 0;
        ev.buttons = buttons;
        ev.scancode = 0;
        ev.flags = 0;
        if (input_queue_push(q, &ev) == 0) {
            pushed++;
        }
    }
    if (dx != 0 || dy != 0) {
        ms->moves++;
        ev.type = INPUT_EV_MOVE;
        ev.key = 0;
        ev.x = dx;
        ev.y = dy;
        ev.buttons = ms->buttons;
        ev.scancode = 0;
        ev.flags = 0;
        if (input_queue_push(q, &ev) == 0) {
            pushed++;
        }
    }
    return pushed;
}

/* ---- aux 口小工具 --------------------------------------------------------- */

static int aux_write(const struct input_io *io, u8 value)
{
    if (wait_input_clear(io) != 0) {
        return -1;
    }
    io->outb(io->ctx, PS2_STATUS_PORT, PS2_CMD_WRITE_AUX);
    if (wait_input_clear(io) != 0) {
        return -1;
    }
    io->outb(io->ctx, PS2_DATA_PORT, value);
    return 0;
}

/* 0=ACK，1=RESEND（调用方重试），-1=超时/没有控制器。 */
static int aux_read_ack(const struct input_io *io)
{
    u32 i;

    for (i = 0; i < 200000u; i++) {
        u8 status = io->inb(io->ctx, PS2_STATUS_PORT);
        u8 data;

        if (status == 0xFFu) {
            return -1;
        }
        if ((status & PS2_STATUS_OUTPUT_FULL) == 0u) {
            continue;
        }
        data = io->inb(io->ctx, PS2_DATA_PORT);
        if (data == PS2_ACK) {
            return 0;
        }
        if (data == PS2_RESEND) {
            return 1;
        }
        /* 初始化期间残留的键盘字节/鼠标包：丢弃（还没打开数据报告）。 */
    }
    return -1;
}

static int aux_cmd(const struct input_io *io, u8 cmd)
{
    u32 attempt;

    for (attempt = 0; attempt < 3u; attempt++) {
        int rc;

        if (aux_write(io, cmd) != 0) {
            return -1;
        }
        rc = aux_read_ack(io);
        if (rc == 0) {
            return 0;
        }
        if (rc < 0) {
            return -1;
        }
    }
    return -1;
}

static int aux_cmd_param(const struct input_io *io, u8 cmd, u8 param)
{
    if (aux_cmd(io, cmd) != 0) {
        return -1;
    }
    if (aux_write(io, param) != 0) {
        return -1;
    }
    return (aux_read_ack(io) == 0) ? 0 : -1;
}

static int read_config_byte(const struct input_io *io, u8 *out)
{
    u32 i;

    if (wait_input_clear(io) != 0) {
        return -1;
    }
    io->outb(io->ctx, PS2_STATUS_PORT, PS2_CMD_READ_CONFIG);
    for (i = 0; i < 200000u; i++) {
        u8 status = io->inb(io->ctx, PS2_STATUS_PORT);

        if (status == 0xFFu) {
            return -1;
        }
        if ((status & PS2_STATUS_OUTPUT_FULL) != 0u) {
            *out = io->inb(io->ctx, PS2_DATA_PORT);
            return 0;
        }
    }
    return -1;
}

int ps2_init_mouse(struct input_mouse *ms, const struct input_io *io)
{
    u8 config = 0;

    if (ms == NULL || io == NULL || io->inb == NULL || io->outb == NULL) {
        return -1;
    }
    input_mouse_reset(ms);
    if (io->inb(io->ctx, PS2_STATUS_PORT) == 0xFFu) {
        return -1;
    }
    flush_output(io);

    /* 只动 aux 接口，不重配键盘（键盘的 0xF4 已在 ps2_init 里开过）。 */
    if (wait_input_clear(io) == 0) {
        io->outb(io->ctx, PS2_STATUS_PORT, PS2_CMD_DISABLE_AUX);
    }
    flush_output(io);
    if (read_config_byte(io, &config) != 0) {
        return -1;
    }
    config |= 0x02u;  /* IRQ12（本 WP 仍轮询，置位只为和固件默认一致） */
    config &= (u8)~0x20u; /* 允许 aux 时钟/数据 */
    if (wait_input_clear(io) != 0) {
        return -1;
    }
    io->outb(io->ctx, PS2_STATUS_PORT, PS2_CMD_WRITE_CONFIG);
    if (wait_input_clear(io) != 0) {
        return -1;
    }
    io->outb(io->ctx, PS2_DATA_PORT, config);
    if (wait_input_clear(io) != 0) {
        return -1;
    }
    io->outb(io->ctx, PS2_STATUS_PORT, PS2_CMD_ENABLE_AUX);
    flush_output(io);

    /* WP5-0h 要求的初始化序列：默认值 -> 采样率 100 -> 分辨率 3 -> 开报告。 */
    if (aux_cmd(io, PS2_MOUSE_SET_DEFAULT) != 0) {
        return -1;
    }
    if (aux_cmd_param(io, PS2_MOUSE_SET_SAMPLE, 100u) != 0) {
        return -1;
    }
    if (aux_cmd_param(io, PS2_MOUSE_SET_RES, 3u) != 0) {
        return -1;
    }
    if (aux_cmd(io, PS2_MOUSE_ENABLE) != 0) {
        return -1;
    }
    flush_output(io);
    return 0;
}

/* ---- 所有权 --------------------------------------------------------------- */

void input_owner_reset(struct input_owner *o)
{
    if (o == NULL) {
        return;
    }
    o->pid = 0;
    o->claims = 0;
    o->denials = 0;
    o->releases = 0;
}

int input_owner_claim(struct input_owner *o, u32 pid, int owner_alive)
{
    if (o == NULL || pid == 0u) {
        return -1;
    }
    if (o->pid != 0u && o->pid != pid && owner_alive != 0) {
        o->denials++;
        return -1;
    }
    o->pid = pid;
    o->claims++;
    return 0;
}

int input_owner_may_read(struct input_owner *o, u32 pid, int owner_alive)
{
    if (o == NULL || o->pid == 0u || o->pid == pid) {
        return 1;
    }
    if (owner_alive == 0) {
        /* 属主进程已经死了：自动回收，任何人都可以重新读/认领。 */
        o->pid = 0;
        o->releases++;
        return 1;
    }
    o->denials++;
    return 0;
}

int input_owner_release(struct input_owner *o, u32 pid)
{
    if (o == NULL || pid == 0u || o->pid != pid) {
        return -1;
    }
    o->pid = 0;
    o->releases++;
    return 0;
}

