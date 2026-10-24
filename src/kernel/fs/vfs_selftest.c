/* =============================================================================
 * fs/vfs_selftest.c - 后端一致性自检
 *
 * 目的：VFS 改成 "fs_backend_t ops 表 + probe 路由" 之后，ramfs 与 FAT16
 * 走的是同一条调用链。这里把两个后端放在同一组操作下逐项对比，确保：
 *   - 路由正确（fat: 走 FAT16，其余走 ramfs）；
 *   - ramfs：创建 -> 写 -> stat -> 读回 -> 删除 -> stat 应失败；
 *   - FAT16：stat 的 mode/uid/gid 必须为 0、句柄不可共享（ref 必须失败）、
 *     listdir 越界必须报错；
 *   - 删除后不能再被 stat 到。
 * 任何一步不符合预期就打印具体步骤，方便定位是哪个后端/哪条路径退化。
 * ============================================================================= */
#include "fs.h"
#include "fat16.h"
#include "include/kernel.h"
#include "include/kernel.h"

#define SELFTEST_RAM_PATH "system:/vfs_self.txt"
#define SELFTEST_FAT_PATH "fat:/SELFTEST.TXT"

static int selftest_fail(int step)
{
    kprintf("fs: backend selftest FAILED at step %d\n", step);
    return -1;
}

void fs_backend_selftest(void)
{
    fs_stat_t st;
    char buf[32];
    char name[FS_NAME_MAX];
    uint8_t be = 0xFFU;
    int h;
    int n;
    int step = 0;

    /* 1. 路由 */
    if (fs_is_fat(SELFTEST_RAM_PATH) != 0) { selftest_fail(++step); return; }
    if (fs_is_fat(SELFTEST_FAT_PATH) == 0) { selftest_fail(++step); return; }

    /* 2. ramfs：创建 + 写 */
    h = fs_open_ex(SELFTEST_RAM_PATH,
                   FS_OPEN_READ | FS_OPEN_WRITE | FS_OPEN_CREAT, 0U, 0U, &be);
    if (h < 0 || be != FS_BE_RAMFS) { selftest_fail(++step); return; }
    if (fs_write_ex(be, h, "hello", 5U) != 5) { selftest_fail(++step); return; }
    (void)fs_close_ex(be, h);

    /* 3. 元数据：大小与权限位都应有效（ramfs 有 Unix 权限） */
    if (fs_stat_ex(SELFTEST_RAM_PATH, &st, &be) != 0 ||
        st.size != 5U || st.mode == 0U) { selftest_fail(++step); return; }

    /* 4. 读回内容 */
    h = fs_open_ex(SELFTEST_RAM_PATH, FS_OPEN_READ, 0U, 0U, &be);
    if (h < 0) { selftest_fail(++step); return; }
    n = fs_read_ex(be, h, buf, sizeof(buf));
    if (n != 5 || memcmp(buf, "hello", 5U) != 0) { selftest_fail(++step); return; }
    (void)fs_close_ex(be, h);

    /* 5. 删除后 stat 必须失败 */
    if (fs_unlink_ex(SELFTEST_RAM_PATH, 0U, 0U, &be) != 0) { selftest_fail(++step); return; }
    if (fs_stat_ex(SELFTEST_RAM_PATH, &st, &be) == 0) { selftest_fail(++step); return; }

    /* 6. listdir 越界必须报错 */
    if (fs_listdir_ex("system:", 9999U, name, sizeof(name), &st, &be) == 0) {
        selftest_fail(++step);
        return;
    }

    /* 7. FAT16（没挂载就跳过，不算失败） */
    if (fat16_mounted()) {
        h = fs_open_ex(SELFTEST_FAT_PATH,
                       FS_OPEN_READ | FS_OPEN_WRITE | FS_OPEN_CREAT, 0U, 0U, &be);
        if (h < 0 || be != FS_BE_FAT16) { selftest_fail(++step); return; }

        /* FAT 句柄没有引用计数，必须拒绝共享 */
        if (fs_ref_ex(be, h) == 0) { selftest_fail(++step); return; }

        if (fs_write_ex(be, h, "fat", 3U) != 3) { selftest_fail(++step); return; }
        (void)fs_close_ex(be, h);

        /* FAT16 没有 Unix 权限位：mode/uid/gid 必须是 0 */
        if (fs_stat_ex(SELFTEST_FAT_PATH, &st, &be) != 0 ||
            st.mode != 0U || st.uid != 0U || st.gid != 0U) {
            selftest_fail(++step);
            return;
        }

        h = fs_open_ex(SELFTEST_FAT_PATH, FS_OPEN_READ, 0U, 0U, &be);
        if (h < 0) { selftest_fail(++step); return; }
        n = fs_read_ex(be, h, buf, sizeof(buf));
        if (n != 3 || memcmp(buf, "fat", 3U) != 0) { selftest_fail(++step); return; }
        (void)fs_close_ex(be, h);

        if (fs_unlink_ex(SELFTEST_FAT_PATH, 0U, 0U, &be) != 0) { selftest_fail(++step); return; }

        if (fs_listdir_ex("fat:", 9999U, name, sizeof(name), &st, &be) == 0) {
            selftest_fail(++step);
            return;
        }
    }

    kprintln("fs: backend selftest ok");
}
