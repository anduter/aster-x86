/* =============================================================================
 * demo.c - 启动自检
 *
 * 内核启动时跑一次，验证“程序 / 文件 / 设备”三类对象各自独立：
 *   - 程序要在程序表里注册，拿到的是 OBJ_PROGRAM 句柄；
 *   - 文件走 fs 模块（路径 -> fs_open -> 读写 -> fs_close），拿到 OBJ_FILE 句柄；
 *   - 设备只能按类别调用，拿到 OBJ_DEVICE 句柄；
 *   - 把一种句柄当另一种用会被 E_TYPE 拒绝。
 *
 * 这里没有任何命令解析或交互逻辑 —— shell 是独立程序。
 * ============================================================================= */
#include "demo.h"
#include "error.h"
#include "object.h"
#include "program.h"
#include "device.h"
#include "../fs/fs.h"
#include "../proc/cap.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"

void demo_run(void)
{
    static const char payload[] = "hello from ramfs";
    fs_stat_t st = { .type = FS_TYPE_NONE };
    char buf[64];
    bool fs_ok = true;
    int n = 0;

    kprintln("demo: start");

    /* 1. 程序表：程序是注册出来的对象，不是磁盘路径 */
    kprintf("demo: programs = %u\n", prog_count());
    for (uint32_t i = 0; i < prog_count(); i++) {
        const program_t *p = prog_at(i);
        kprintf("  prog %s@%u\n", p->name, p->version);
    }

    /* 2. 文件系统：建目录 -> 建文件 -> 写入 -> 读回比对 -> stat
     *    已存在时 mkdir 返回 E_EXIST，属于幂等成功（重启后目录还在）。 */
    if (fs_mkdir("system:/docs") != E_OK && fs_stat("system:/docs", 0) != E_OK) {
        fs_ok = false;
    }

    uint32_t h = fs_open("system:/docs/hello.txt", FS_OPEN_WRITE | FS_OPEN_CREAT);
    if (h == 0) {
        fs_ok = false;
    } else {
        if (fs_write(h, payload, (uint32_t)strlen(payload)) != (int)strlen(payload)) {
            fs_ok = false;
        }
        fs_close(h);
    }

    h = fs_open("system:/docs/hello.txt", FS_OPEN_READ);
    if (h == 0) {
        fs_ok = false;
    } else {
        n = fs_read(h, buf, sizeof(buf) - 1U);
        fs_close(h);
        if (n != (int)strlen(payload)) {
            fs_ok = false;
        } else {
            buf[n] = '\0';
            if (strcmp(buf, payload) != 0) fs_ok = false;
        }
    }

    if (fs_stat("system:/docs/hello.txt", &st) != E_OK || st.type != FS_TYPE_FILE) {
        fs_ok = false;
    }
    kprintf("demo: fs write/read %s (%u bytes)\n", fs_ok ? "ok" : "FAIL", st.size);

    /* 对象引用计数：同一句柄 ref 后 close 一次，另一个引用仍可读。 */
    bool ref_ok = false;
    h = fs_open("system:/docs/hello.txt", FS_OPEN_READ);
    if (h != 0U) {
        if (fs_ref(h) == E_OK) {
            (void)fs_close(h);
            n = fs_read(h, buf, sizeof(buf) - 1U);
            ref_ok = (n == (int)strlen(payload));
            (void)fs_close(h);
        } else {
            (void)fs_close(h);
        }
    }
    kprintf("demo: fs refcount %s\n", ref_ok ? "ok" : "FAIL");
    if (!ref_ok) fs_ok = false;

    /* 通用 capability 表：install -> transfer -> close/revoke。 */
    if (cap_selftest()) {
        kprintln("cap: table selftest ok");
    } else {
        kprintln("cap: table selftest FAIL");
    }

    /* 3. 句柄类型校验：程序句柄不能当文件用，文件句柄不能当设备用 */
    uint32_t ph = prog_open("shell");
    uint32_t fh = fs_open("system:/docs/hello.txt", FS_OPEN_READ);
    void *data = 0;
    int e1 = obj_deref(ph, OBJ_FILE, &data);      /* 期望 E_TYPE */
    int e2 = obj_deref(fh, OBJ_DEVICE, &data);    /* 期望 E_TYPE */

    bool type_ok = (e1 == E_TYPE) && (e2 == E_TYPE);
    kprintf("demo: handle type check %s (prog-as-file=%d, file-as-device=%d)\n",
            type_ok ? "ok" : "FAIL", e1, e2);

    obj_close(ph, OBJ_PROGRAM);
    fs_close(fh);

    /* 4. 设备表：设备不进文件树，也没有 ioctl */
    kprintf("demo: devices = %u\n", dev_count());
    for (uint32_t i = 0; i < dev_count(); i++) {
        const device_t *d = dev_at(i);
        kprintf("  dev %s:%s\n", dev_class_name(d->cls), d->name);
    }

    kprintln("demo: done");
}
