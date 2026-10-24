#ifndef CORE_DEVICE_H
#define CORE_DEVICE_H

#include "../types.h"

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
    char        name[DEV_NAME_MAX];
    dev_class_t cls;
    int       (*putc)(char c);      /* 字符/显示类：输出一个字符 */
    int       (*getc)(char *out);   /* 字符类：取一个字符，暂无输入返回 E_NOENT */
} device_t;

/* 注册内建设备（kbd0 / fb0 / pit0） */
void dev_init(void);

int dev_register(const char *name, dev_class_t cls,
                 int (*putc)(char), int (*getc)(char *));

const device_t *dev_lookup(const char *name);
const device_t *dev_at(uint32_t index);
uint32_t        dev_count(void);

/* 按名字取一个 OBJ_DEVICE 句柄；失败返回 0 */
uint32_t dev_open(const char *name);

const char *dev_class_name(dev_class_t cls);

#endif
