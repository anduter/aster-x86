/* =============================================================================
 * fs/fs_link_selftest.c - 硬链接 / 符号链接回归（阶段 4b）
 *
 * 断言（build.ps1 收集 "fs:" 开头的行）：
 *   1. 硬链接：nlink=2，通过任一名字写、另一个名字读得到；
 *   2. 删掉一个名字后数据还在、nlink 回到 1；
 *   3. 目录不能做硬链接（E_INVAL）；
 *   4. 跨卷硬链接拒绝（E_INVAL）；
 *   5. 符号链接：相对目标能被 open 跟随；
 *   6. readlink 读到目标文本；lstat 报 LINK，stat 跟随报 FILE；
 *   7. O_NOFOLLOW 打开符号链接失败；
 *   8. 软链接环有跳数上限（E_INVAL，不会死循环）；
 *   9. FAT16 上的 link/symlink 返回 E_INVAL；
 *  10. 软链接指向 system:/bin 时写操作被 SIP 拦下（防止绕过只读策略）。
 * ============================================================================= */
#include "fs.h"
#include "include/kernel.h"
#include "include/fs.h"

#define LK_DIR      "system:/tmp"
#define LK_A        "system:/tmp/lk-a"
#define LK_B        "system:/tmp/lk-b"
#define LK_SL       "system:/tmp/lk-sl"
#define LK_SL2      "system:/tmp/lk-sl2"
#define LK_LOOP_A   "system:/tmp/lk-loop-a"
#define LK_LOOP_B   "system:/tmp/lk-loop-b"
#define LK_NOFOLLOW "system:/tmp/lk-nofollow"
#define LK_BYPASS   "system:/tmp/lk-bypass"

static int lk_fail(int step)
{
    kprintf("fs: link selftest FAILED at step %d\n", step);
    return -1;
}

static int write_file(const char *path, const void *buf, uint32_t len)
{
    uint32_t h = fs_open_as(path, FS_OPEN_WRITE | FS_OPEN_CREAT, 0U, 0U);
    int n;

    if (h == 0U) return -1;
    n = fs_write(h, buf, len);
    fs_close(h);
    return (n == (int)len) ? 0 : -1;
}

static void unlink_all(void)
{
    (void)fs_unlink_as(LK_A, 0U, 0U);
    (void)fs_unlink_as(LK_B, 0U, 0U);
    (void)fs_unlink_as(LK_SL, 0U, 0U);
    (void)fs_unlink_as(LK_SL2, 0U, 0U);
    (void)fs_unlink_as(LK_LOOP_A, 0U, 0U);
    (void)fs_unlink_as(LK_LOOP_B, 0U, 0U);
    (void)fs_unlink_as(LK_NOFOLLOW, 0U, 0U);
    (void)fs_unlink_as(LK_BYPASS, 0U, 0U);
}

void fs_link_selftest(void)
{
    fs_stat_t st;
    char buf[32];
    uint32_t h;
    int n;

    (void)fs_mkdir(LK_DIR);
    unlink_all();

    /* 1. 硬链接 + 通过两个名字看到同一份数据 */
    if (write_file(LK_A, "hello", 5U) != 0) { (void)lk_fail(1); return; }
    if (fs_link_as(LK_A, LK_B, 0U, 0U) != E_OK) { (void)lk_fail(2); return; }
    if (fs_stat(LK_B, &st) != E_OK || st.nlink != 2U) { (void)lk_fail(3); return; }
    if (write_file(LK_B, "HELLO", 5U) != 0) { (void)lk_fail(4); return; }
    h = fs_open_as(LK_A, FS_OPEN_READ, 0U, 0U);
    if (h == 0U) { (void)lk_fail(5); return; }
    n = fs_read(h, buf, sizeof(buf));
    fs_close(h);
    if (n != 5 || memcmp(buf, "HELLO", 5U) != 0) { (void)lk_fail(6); return; }
    kprintln("fs: hard link ok (nlink=2, write via link visible)");

    /* 2. 删一个名字，数据仍在，nlink 回到 1 */
    if (fs_unlink_as(LK_A, 0U, 0U) != E_OK) { (void)lk_fail(7); return; }
    if (fs_stat(LK_B, &st) != E_OK || st.nlink != 1U || st.size != 5U) {
        (void)lk_fail(8);
        return;
    }
    h = fs_open_as(LK_B, FS_OPEN_READ, 0U, 0U);
    if (h == 0U) { (void)lk_fail(9); return; }
    n = fs_read(h, buf, sizeof(buf));
    fs_close(h);
    if (n != 5 || memcmp(buf, "HELLO", 5U) != 0) { (void)lk_fail(10); return; }
    kprintln("fs: hard link unlink keeps data ok");

    /* 3. 目录不能做硬链接 */
    if (fs_link_as(LK_DIR, "system:/tmp/lk-dir-link", 0U, 0U) != E_INVAL) {
        (void)lk_fail(11);
        return;
    }
    kprintln("fs: hard link rejects directory ok");

    /* 4. 跨卷硬链接拒绝（data: 卷在启动时已挂载） */
    if (fs_link_as(LK_B, "data:/lk-cross", 0U, 0U) != E_INVAL) {
        (void)lk_fail(12);
        return;
    }
    kprintln("fs: hard link rejects cross-volume ok");

    /* 5. 软链接：相对目标 + open 跟随 */
    if (fs_symlink_as("lk-b", LK_SL, 0U, 0U) != E_OK) { (void)lk_fail(13); return; }
    h = fs_open_as(LK_SL, FS_OPEN_READ, 0U, 0U);
    if (h == 0U) { (void)lk_fail(14); return; }
    n = fs_read(h, buf, sizeof(buf));
    fs_close(h);
    if (n != 5 || memcmp(buf, "HELLO", 5U) != 0) { (void)lk_fail(15); return; }
    kprintln("fs: symlink follow ok");

    /* 6. readlink / lstat / stat */
    {
        char target[64];

        n = fs_readlink_as(LK_SL, target, sizeof(target), 0U, 0U);
        if (n <= 0 || strcmp(target, "lk-b") != 0) { (void)lk_fail(16); return; }
        if (fs_lstat_as(LK_SL, &st, 0U, 0U) != E_OK || st.type != FS_TYPE_LINK ||
            st.size != 4U) {
            (void)lk_fail(17);
            return;
        }
        if (fs_stat(LK_SL, &st) != E_OK || st.type != FS_TYPE_FILE || st.size != 5U) {
            (void)lk_fail(18);
            return;
        }
    }
    kprintln("fs: readlink/lstat ok");

    /* 7. O_NOFOLLOW 打开软链接必须失败 */
    if (fs_open_as(LK_SL, FS_OPEN_READ | FS_OPEN_NOFOLLOW, 0U, 0U) != 0U) {
        (void)lk_fail(19);
        return;
    }
    kprintln("fs: O_NOFOLLOW rejects symlink ok");

    /* 8. 软链接环：跳数上限，返回 E_INVAL 而不是死循环 */
    if (fs_symlink_as("lk-loop-b", LK_LOOP_A, 0U, 0U) != E_OK ||
        fs_symlink_as("lk-loop-a", LK_LOOP_B, 0U, 0U) != E_OK) {
        (void)lk_fail(20);
        return;
    }
    if (fs_open_as(LK_LOOP_A, FS_OPEN_READ, 0U, 0U) != 0U) {
        (void)lk_fail(21);
        return;
    }
    kprintln("fs: symlink loop limited ok");

    /* 9. FAT16 不支持 link/symlink（返回 E_INVAL） */
    if (fat16_mounted()) {
        if (fs_link_as(LK_B, "fat:/LINK.TXT", 0U, 0U) != E_INVAL ||
            fs_symlink_as("x", "fat:/SLINK.TXT", 0U, 0U) != E_INVAL ||
            fs_readlink_as("fat:/host.txt", buf, sizeof(buf), 0U, 0U) != E_INVAL) {
            (void)lk_fail(22);
            return;
        }
        kprintln("fs: FAT16 link ops E_INVAL ok");
    }

    /* 10. 软链接指向 system:/bin：写操作仍被 SIP 拒绝（防止绕过只读策略） */
    if (sip_enabled()) {
        if (fs_symlink_as("/bin/shell", LK_BYPASS, 0U, 0U) != E_OK) {
            (void)lk_fail(23);
            return;
        }
        if (fs_open_as(LK_BYPASS, FS_OPEN_READ, 0U, 0U) == 0U) {
            (void)lk_fail(24);      /* 读应该被允许（跟随到 shell） */
            return;
        }
        if (fs_open_as(LK_BYPASS, FS_OPEN_WRITE, 0U, 0U) != 0U) {
            (void)lk_fail(25);      /* 写必须被 SIP 拦下 */
            return;
        }
        kprintln("fs: symlink into system path denied ok");
    }

    unlink_all();
    /* 不删 /tmp 本身：标准目录，其它自检（SIP）还要用 */
    kprintln("fs: link selftest PASS");
}
