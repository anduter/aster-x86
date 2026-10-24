#include "device.h"
#include "include/mm.h"
#include "include/kernel.h"
#include "include/kernel.h"
#include "include/kernel.h"
#include "include/dev.h"
#include "include/proc.h"

static device_t devices[DEV_MAX];
static uint32_t count;

/* ---- 内建设备的函数实现 ---- */

static int fb_putc(char c)
{
    console_putc(c);
    return E_OK;
}

static int kbd_getc(char *out)
{
    int c = keyboard_getchar();
    if (c < 0 || out == 0) return E_NOENT;
    *out = (char)c;
    return E_OK;
}

void dev_init(void)
{
    count = 0;
    dev_register("kbd0", DEV_CHAR,  0,        kbd_getc);
    dev_register("fb0",  DEV_VIDEO, fb_putc,  0);
    dev_register("pit0", DEV_TIMER, 0,        0);
}

int dev_register(const char *name, dev_class_t cls,
                 int (*putc)(char), int (*getc)(char *))
{
    if (name == 0 || name[0] == '\0') return E_INVAL;
    if (strlen(name) >= DEV_NAME_MAX) return E_INVAL;
    if (count >= DEV_MAX) return E_FULL;
    if (dev_lookup(name) != 0) return E_EXIST;

    device_t *d = &devices[count];
    strncpy(d->name, name, DEV_NAME_MAX - 1U);
    d->name[DEV_NAME_MAX - 1U] = '\0';
    d->cls = cls;
    d->putc = putc;
    d->getc = getc;
    count++;
    return E_OK;
}

const device_t *dev_lookup(const char *name)
{
    if (name == 0) return 0;

    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(devices[i].name, name) == 0) {
            return &devices[i];
        }
    }
    return 0;
}

const device_t *dev_at(uint32_t index)
{
    return (index < count) ? &devices[index] : 0;
}

uint32_t dev_count(void)
{
    return count;
}

uint32_t dev_open(const char *name)
{
    const device_t *d = dev_lookup(name);
    if (d == 0) return 0;

    return obj_create(OBJ_DEVICE, (void *)(uintptr_t)d);
}

const char *dev_class_name(dev_class_t cls)
{
    switch (cls) {
    case DEV_CHAR:  return "char";
    case DEV_BLOCK: return "block";
    case DEV_VIDEO: return "video";
    case DEV_TIMER: return "timer";
    default:        return "unknown";
    }
}
