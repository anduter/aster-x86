#ifndef CORE_DEVICE_H
#define CORE_DEVICE_H

#include "../types.h"

/* =============================================================================
 * dev/device.h - 设备 API v1
 *
 * 设备不走文件树、也没有通用 open/read/write/ioctl：每个设备按类别暴露
 * 强类型函数（字符/显示设备的 putc/getc，块设备走 block_device_t 的
 * block_read/block_write）。本结构体是内核内设备注册表的最小 ops 契约。
 *
 * 版本策略（见 docs/api.md）：
 *   - DEV_API_VERSION 是 ops 布局的版本；任何字段增删/改类型都要 bump；
 *   - 注册时由 dev_register() 写入 api_version，旧实例的 0 按 v1 解释；
 *   - 文件末尾的 _Static_assert 固定本版本的布局，改布局会编译失败。
 * ========================================================================== */
#define DEV_API_VERSION 1u

/* 设备类别：设备不进入文件树，也不提供 ioctl，
 * 而是按类别暴露强类型函数（putc/getc、block_read/...）。 */
typedef enum {
    DEV_CHAR  = 1,   /* 字符设备：键盘、串口 */
    DEV_BLOCK = 2,   /* 块设备：软盘、硬盘（P3 接入） */
    DEV_VIDEO = 3,   /* 显示设备 */
    DEV_TIMER = 4    /* 定时器 */
} dev_class_t;

#define DEV_NAME_MAX 16
#define DEV_MAX      8

typedef struct {
    char        name[DEV_NAME_MAX]; /* NUL 结尾；由 dev_register 复制，最多 15 + NUL */
    dev_class_t cls;
    /* putc：输出一个字符。同步执行；成功返回 E_OK，失败返回负 E_*。
     * 可能在持锁/中断关闭的上下文被调用，实现不得睡眠等待中断。
     * 不保留任何用户指针。 */
    int       (*putc)(char c);
    /* getc：取一个字符写到 *out（调用方提供、至少 1 字节）。成功返回 E_OK；
     * 暂无输入返回 E_NOENT（非阻塞，不能忙等）。*out 只由设备写一次，
     * 缓冲区所有权始终属于调用方。 */
    int       (*getc)(char *out);
    /* 注册时写入的 ops 版本；0 表示旧实例（按 DEV_API_VERSION=1 解释）。
     * 由 dev_register() 自动设置，调用方不要手改。 */
    uint32_t    api_version;
} device_t;

/* v1 布局冻结：name[16] + cls(4) + putc(4) + getc(4) + api_version(4) = 32。
 * 32 位 x86 下函数指针 4 字节；改字段必须 bump DEV_API_VERSION 并改这里。 */
_Static_assert(sizeof(device_t) == 32, "DEV_API_VERSION 1: device_t layout changed");
_Static_assert(__builtin_offsetof(device_t, name) == 0,
               "DEV_API_VERSION 1: device_t.name offset");
_Static_assert(__builtin_offsetof(device_t, cls) == 16,
               "DEV_API_VERSION 1: device_t.cls offset");
_Static_assert(__builtin_offsetof(device_t, putc) == 20,
               "DEV_API_VERSION 1: device_t.putc offset");
_Static_assert(__builtin_offsetof(device_t, getc) == 24,
               "DEV_API_VERSION 1: device_t.getc offset");
_Static_assert(__builtin_offsetof(device_t, api_version) == 28,
               "DEV_API_VERSION 1: device_t.api_version offset");

/* 注册内建设备（kbd0 / fb0 / pit0） */
void dev_init(void);

/* 注册一个设备：name 被复制（调用方保留所有权），putc/getc 可以为 0。
 * 成功返回 E_OK；重名 E_EXIST、表满 E_FULL、参数非法 E_INVAL。
 * 注册表整体归内核所有，调用方不得保存 device_t 指针跨 dev_init 使用。 */
int dev_register(const char *name, dev_class_t cls,
                 int (*putc)(char), int (*getc)(char *));

/* 查询：返回注册表内的只读借用指针（生命周期到下一次 dev_init），
 * 调用方不得 free，也不得写入。dev_at 越界返回 0。 */
const device_t *dev_lookup(const char *name);
const device_t *dev_at(uint32_t index);
uint32_t        dev_count(void);

/* 按名字取一个 OBJ_DEVICE 句柄；失败返回 0。
 * 成功时句柄归调用方（对象表持有设备指针，设备本身仍归注册表），
 * 通过对象/close 路径释放句柄；不要直接释放 device_t。 */
uint32_t dev_open(const char *name);

const char *dev_class_name(dev_class_t cls);

#endif
