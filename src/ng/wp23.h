/*
 * ng WP2-3: 显示 + 输入内核接线（L1D 驱动 -> L0 内核）。
 *
 * 职责边界：
 *   - src/drivers/ 下的驱动只做纯逻辑/寄存器编码（PCI/VBE/fb/PS2/VGA text），
 *     端口 IO 和页表映射由本文件注入；
 *   - 本文件负责：PCI 找 1234:1111、BAR0、启用 memory/bus-master、设
 *     1280x800x32（失败回退 1024x768，再失败回 VGA 文本 0xB8000）、把 LFB
 *     恒等映射成 PCD/PWT 页、跑画图/读回自检、初始化 PS/2、把 readkey 暴露给
 *     syscall 30。
 *
 * 所有证据行都从 wp23_selftest() 打印；main.c 在成功时补
 * `ng: WP2-3 selftest PASS`。
 */
#ifndef NG_WP23_H
#define NG_WP23_H

#include "types.h"

/* readkey 没有按键时的返回值（与 fs 的负 errno 风格一致）。 */
#define NG_EAGAIN (-11)

/*
 * 初始化显示/输入并跑自检。返回失败项数（0 = 全部通过）。
 * 显示降级路径（无 stdvga）也算通过：系统仍可用（VGA 文本控制台）。
 */
int wp23_selftest(void);

/*
 * syscall 30 readkey：非阻塞兼容接口。
 *   有键：返回 (flags << 24) | (ascii << 16) | scancode（flags 见 INPUT_F_*）；
 *   无键：返回 (u64)(i64)NG_EAGAIN。
 * WP5-0h 起不再只在 readkey 里轮询 PS/2：内核队列由 sched_yield() 调度点
 * 的 ng_input_poll() 填充（boot 的同步 ring3 演示仍由 readkey 兜底一次）。
 * displayd 认领输入后，其它进程返回 -EAGAIN 并让出一轮（避免 shell 忙等
 * 抢键/饿死桌面）。
 */
u64 ng_input_readkey(void);

/* 调度点轮询（sched_yield 调用；readkey 对 boot 演示兜底）。 */
void ng_input_poll(void);

/*
 * syscall 96 input_event(struct input_packet*)：非阻塞取一个统一事件
 * （键盘/鼠标移动/鼠标按键）。0=取到；NG_EAGAIN=没有；用户指针由
 * syscall.c 的 copyout 校验。
 */
struct input_packet;
int ng_input_event(struct input_packet *out);

/*
 * syscall 97 input_claim(pid, flags)：pid=0 表示当前进程，flags bit0=释放。
 * 返回 0 / -1(EPERM) / -16(EBUSY：已有存活属主)。
 */
i64 ng_input_claim(u64 pid, u64 flags);

/* 诊断计数（claims 用；只读）。 */
u32 ng_input_aux_bytes(void);
u32 ng_input_key_bytes(void);
u32 ng_input_poll_rounds(void);
u32 ng_input_mouse_moves(void);
u32 ng_input_mouse_buttons(void);
u32 ng_input_owner_pid(void);
u32 ng_input_owner_denials(void);
int ng_input_mouse_ready(void);

/*
 * WP2-7 parity 诊断（只读）：
 *   - ng_display_fb()  ：VBE 帧缓冲路径就绪时返回 fb_info，否则 NULL；
 *   - ng_display_text()：VGA 文本降级路径就绪时返回 vga_text，否则 NULL；
 *   - ng_input_state() ：PS/2 输入状态（永远非 NULL，初始化后）。
 * 只暴露指针，不改变初始化顺序；显示/输入 parity 用例据此断言“当前实际
 * 走的是哪条路径”，而不是假设一定有 stdvga。
 */
struct fb_info;
struct vga_text;
struct input_state;
const struct fb_info *ng_display_fb(void);
const struct vga_text *ng_display_text(void);
const struct input_state *ng_input_state(void);

#endif /* NG_WP23_H */
