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
