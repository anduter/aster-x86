#ifndef KLOG_H_MODULE_H
#define KLOG_H_MODULE_H

/* =============================================================================
 * include/klog.h - 内核日志环（P2 dmesg 的机制层）
 *
 * 设计要点：
 *   - 固定 64KB 静态环，按“行”保存（丢弃时整行丢弃，不产生半行）；
 *   - 写入只做内存拷贝 + 短自旋锁（关中断），绝不阻塞/睡眠，可在 IRQ 上下文
 *     和 kprintf 路径里调用；
 *   - kprintf/kprintln 通过 klog_capture() 同步写入环，读侧只拿快照；
 *   - SYS_DMESG(85) 与内核 shell 的 `dmesg` 都基于 klog_read*() 只读快照。
 *
 * 并发：写方用 klog_lock（irqsave）；读方短暂持锁拷贝，不在锁内做任何
 * 可能缺页/睡眠的操作（用户拷贝发生在锁外）。
 * ============================================================================= */

#include "types.h"

/* 环容量：固定 64KB（ABI/文档里写死的“dmesg 环 64KB”） */
#define KLOG_RING_SIZE 65536U

/* 单条 klog_printf 的消息上限（格式化缓冲，超出截断） */
#define KLOG_MSG_MAX   512U

/* 日志级别：只影响环里的前缀（INFO 无前缀，保证测试断言行原样可 grep） */
#define KLOG_DEBUG 0
#define KLOG_INFO  1
#define KLOG_WARN  2
#define KLOG_ERR   3

/* 显式初始化入口（可选）：环是静态 BSS，本身零初始化即可用；第一次写入时
 * 也会惰性完成初始化，因此 kprintf 在 klog_init 之前调用是安全的。 */
void klog_init(void);

/* 追加一行：格式为 "<tag>: <msg>\n"（INFO 级无级别前缀）。 */
void klog_write(int level, const char *tag, const char *msg);

/* 格式化后追加一行，并同步输出到 console/串口（不经过 kprintf，避免二次入环）。 */
void klog_printf(int level, const char *tag, const char *fmt, ...);

/* kprintf hook：把内核现有输出同步追加进环（保留原始字节，尾部补换行由调用方保证）。
 * 同时负责在启动里程碑（"kernel: ready"）之后跑一次 dmesg 自检。 */
void klog_capture(const char *buf, uint32_t len);

/* 只读快照：从最旧到最新拷贝，返回字节数。
 * maxlen 小于环内数据时返回“最新”的 maxlen 字节，并尽量对齐到行首。 */
uint32_t klog_read(char *out, uint32_t maxlen);

/* 只读快照：最近 lines 行（用于 `dmesg -n N`），返回字节数。
 * 结果超过 maxlen 时保留最新的一段并对齐行首。 */
uint32_t klog_read_tail_lines(char *out, uint32_t maxlen, uint32_t lines);

/* 当前状态：占用字节 / 完整行数 / 因溢出丢掉的旧行数。 */
uint32_t klog_used(void);
uint32_t klog_lines(void);
uint32_t klog_dropped(void);

/* 环内是否存在某子串（调试/自检用；不分配、不阻塞）。 */
bool klog_contains(const char *needle);

/* 打印最近 lines 行到 console/串口（`dmesg` 命令与自检共用）。
 * 返回实际打印的行数。 */
uint32_t klog_print_tail(uint32_t lines);

/* 自检：写 >64KB 后验证保留最近行/无越界/小缓冲截断/坏指针拒绝，
 * 结束后恢复启动日志尾部并输出固定的通过行。只跑一次。 */
void klog_selftest(void);

#endif /* KLOG_H_MODULE_H */
