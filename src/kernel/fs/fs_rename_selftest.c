/* =============================================================================
 * fs/fs_rename_selftest.c - rename / move 自检
 *
 * 覆盖本批新增的语义（详见 fs.h 与 docs/changes-fs.md）：
 *   - ramfs：改名、跨卷拒绝、源不存在、文件移进目录、目录改名、目录移进
 *     自己的子树、非空目录不可覆盖、空目录可替换、文件覆盖文件、目标被打开
 *     时拒绝替换（E_BUSY）、父目录 w+x 判权与 root 绕过；
 *   - FAT16：根目录 8.3 改名、跨目录/长名/目标已存在分别拒绝；
 *   - 所有临时文件跑完都删掉，不落盘。
 *
 * 成功打印 "selftest: rename ok"（回归脚本后续按这行加检查项）。
 * ============================================================================= */
#include "fs.h"
#include "fat16.h"
#include "include/kernel.h"

#define RN_A    "system:/rn_a.txt"
#define RN_B    "system:/rn_b.txt"
#define RN_DIR  "system:/rn_dir"
#define RN_DIR2 "system:/rn_dir2"
#define RN_SUB  "system:/rn_dir/b.txt"      /* 移进 rn_dir 之后 */
#define RN_SUB2 "system:/rn_dir2/b.txt"     /* rn_dir 改名成 rn_dir2 之后 */
#define RN_T1   "system:/rn_t1"
#define RN_T2   "system:/rn_t2"
#define RN_T2X  "system:/rn_t2/x.txt"
#define RN_E1   "system:/rn_e1"
#define RN_E2   "system:/rn_e2"
#define RN_F1   "system:/rn_f1.txt"
#define RN_F2   "system:/rn_f2.txt"
#define RN_SRC  "system:/rn_src.txt"
#define RN_TGT  "system:/rn_tgt.txt"
#define RN_PERM "system:/rn_perm.txt"
#define RN_ROOT "system:/rn_root.txt"

#define RN_FAT1 "fat:/RN1.TXT"
#define RN_FAT2 "fat:/RN2.TXT"
#define RN_FAT3 "fat:/RN3.TXT"
#define RN_FAT_LONG "fat:/TOOLONGNAME.TXT"    /* 12 字符主名：不是 8.3 */

static int rn_fail(int step)
{
    kprintf("fs: rename selftest FAILED at step %d\n", step);
    return -1;
}

/* 创建/覆盖一个文件并写入 text（root 身份，走 VFS 统一入口） */
static int rn_write(const char *path, const char *text)
{
    uint8_t be = 0xFFU;
    uint32_t len = (uint32_t)strlen(text);
    int h, n;

    h = fs_open_ex(path, FS_OPEN_READ | FS_OPEN_WRITE | FS_OPEN_CREAT, 0U, 0U, &be);
    if (h < 0) return -1;

    n = fs_write_ex(be, h, text, len);
    (void)fs_close_ex(be, h);
    return (n == (int)len) ? 0 : -1;
}

static int rn_is(const char *path, const char *want)
{
    uint8_t be = 0xFFU;
    char buf[32];
    uint32_t len = (uint32_t)strlen(want);
    int h, n;

    if (len >= sizeof(buf)) return 0;

    h = fs_open_ex(path, FS_OPEN_READ, 0U, 0U, &be);
    if (h < 0) return 0;
    n = fs_read_ex(be, h, buf, sizeof(buf));
    (void)fs_close_ex(be, h);
    return (n == (int)len && memcmp(buf, want, len) == 0);
}

static int rn_size_is(const char *path, uint32_t size)
{
    fs_stat_t st;

    return (fs_stat_ex(path, &st, 0) == 0 &&
            st.type == FS_TYPE_FILE && st.size == size);
}

/* 清理临时项（可重复调用；目录要先删空再删自己） */
static void rn_cleanup(void)
{
    static const char *files[] = {
        RN_A, RN_B, RN_SUB, RN_SUB2, RN_T2X, RN_F1, RN_F2,
        RN_SRC, RN_TGT, RN_PERM, RN_ROOT
    };
    static const char *dirs[] = { RN_DIR, RN_DIR2, RN_T1, RN_T2, RN_E1, RN_E2 };

    for (uint32_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        (void)fs_unlink(files[i]);
    }
    for (uint32_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        (void)fs_unlink(dirs[i]);
    }
    if (fat16_mounted()) {
        (void)fs_unlink_ex(RN_FAT1, 0U, 0U, 0);
        (void)fs_unlink_ex(RN_FAT2, 0U, 0U, 0);
        (void)fs_unlink_ex(RN_FAT3, 0U, 0U, 0);
    }
}

/* 返回 0 成功；-1 失败（已打印步骤号） */
static int rn_test_ramfs(void)
{
    fs_stat_t st;
    uint8_t be = 0xFFU;
    int h;

    /* 1. 基本改名 + 内容/大小保持 */
    if (rn_write(RN_A, "hello") != 0) return rn_fail(1);
    if (fs_rename(RN_A, RN_B) != E_OK) return rn_fail(2);
    if (fs_stat_ex(RN_A, &st, 0) == 0) return rn_fail(3);      /* 旧名必须消失 */
    if (!rn_size_is(RN_B, 5U)) return rn_fail(4);
    if (!rn_is(RN_B, "hello")) return rn_fail(5);

    /* 2. 跨卷：system: -> data: 必须 E_INVAL（data 卷由启动流程挂载） */
    if (fs_rename(RN_B, "data:/rn_b.txt") != E_INVAL) return rn_fail(6);

    /* 3. 源不存在 */
    if (fs_rename("system:/rn_nope.txt", "system:/rn_never.txt") != E_NOENT) {
        return rn_fail(7);
    }

    /* 4. 文件移进子目录 + 目录改名 */
    if (fs_mkdir(RN_DIR) != E_OK) return rn_fail(8);
    if (fs_rename(RN_B, RN_SUB) != E_OK) return rn_fail(9);
    if (fs_stat_ex(RN_B, &st, 0) == 0) return rn_fail(10);
    if (!rn_is(RN_SUB, "hello")) return rn_fail(11);
    if (fs_rename(RN_DIR, RN_DIR2) != E_OK) return rn_fail(12);
    if (!rn_size_is(RN_SUB2, 5U)) return rn_fail(13);

    /* 5. 目录不能移进自己的子树（否则父链成环） */
    if (fs_rename(RN_DIR2, "system:/rn_dir2/inner") != E_INVAL) return rn_fail(14);

    /* 6. 非空目录不能被覆盖 */
    if (fs_mkdir(RN_T1) != E_OK || fs_mkdir(RN_T2) != E_OK) return rn_fail(15);
    if (rn_write(RN_T2X, "x") != 0) return rn_fail(16);
    if (fs_rename(RN_T1, RN_T2) != E_EXIST) return rn_fail(17);

    /* 7. 空目录可以替换 */
    if (fs_mkdir(RN_E1) != E_OK || fs_mkdir(RN_E2) != E_OK) return rn_fail(18);
    if (fs_rename(RN_E1, RN_E2) != E_OK) return rn_fail(19);
    if (fs_stat_ex(RN_E1, &st, 0) == 0) return rn_fail(20);
    if (fs_stat_ex(RN_E2, &st, 0) != 0 || st.type != FS_TYPE_DIR) return rn_fail(21);

    /* 8. 文件覆盖文件：目标被替换，内容取源文件 */
    if (rn_write(RN_F1, "aaa") != 0 || rn_write(RN_F2, "bbb") != 0) return rn_fail(22);
    if (fs_rename(RN_F1, RN_F2) != E_OK) return rn_fail(23);
    if (!rn_is(RN_F2, "aaa")) return rn_fail(24);
    if (fs_stat_ex(RN_F1, &st, 0) == 0) return rn_fail(25);

    /* 9. 目标文件仍被打开：拒绝替换（E_BUSY），关掉后可以替换 */
    if (rn_write(RN_SRC, "ss") != 0 || rn_write(RN_TGT, "tt") != 0) return rn_fail(26);
    h = fs_open_ex(RN_TGT, FS_OPEN_READ | FS_OPEN_WRITE, 0U, 0U, &be);
    if (h < 0) return rn_fail(27);
    if (fs_rename(RN_SRC, RN_TGT) != E_BUSY) {
        (void)fs_close_ex(be, h);
        return rn_fail(28);
    }
    if (fs_close_ex(be, h) != E_OK) return rn_fail(29);
    if (fs_rename(RN_SRC, RN_TGT) != E_OK) return rn_fail(30);
    if (!rn_is(RN_TGT, "ss")) return rn_fail(31);

    /* 10. 权限：system: 卷根 0755 属 root，uid=1000 缺 w -> E_ACCES；
     *     同一操作走 fs_rename()（root）必须成功。 */
    if (fs_rename_as(RN_TGT, RN_PERM, 1000U, 1000U) != E_ACCES) return rn_fail(32);
    if (fs_rename(RN_TGT, RN_ROOT) != E_OK) return rn_fail(33);
    return 0;
}

static int rn_test_fat16(void)
{
    fs_stat_t st;

    if (!fat16_mounted()) return 0;             /* 没插交换盘：跳过，不算失败 */

    /* 11. 根目录 8.3 改名 + 内容保持 */
    if (rn_write(RN_FAT1, "fatmv") != 0) return rn_fail(34);
    if (fs_rename(RN_FAT1, RN_FAT2) != E_OK) return rn_fail(35);
    if (fs_stat_ex(RN_FAT1, &st, 0) == 0) return rn_fail(36);
    if (!rn_is(RN_FAT2, "fatmv")) return rn_fail(37);

    /* 12. 跨目录 / 长名必须 E_INVAL */
    if (fs_rename(RN_FAT2, "fat:/SUB/RN3.TXT") != E_INVAL) return rn_fail(38);
    if (fs_rename(RN_FAT2, RN_FAT_LONG) != E_INVAL) return rn_fail(39);

    /* 13. 目标已存在不覆盖（FAT16 刻意不做替换） */
    if (rn_write(RN_FAT3, "keep") != 0) return rn_fail(40);
    if (fs_rename(RN_FAT2, RN_FAT3) != E_EXIST) return rn_fail(41);
    if (!rn_is(RN_FAT2, "fatmv")) return rn_fail(42);
    return 0;
}

void fs_rename_selftest(void)
{
    /* 先清掉上一轮的残留，保证自检可重复运行 */
    rn_cleanup();

    if (rn_test_ramfs() != 0) return;
    if (rn_test_fat16() != 0) return;

    rn_cleanup();
    kprintln("selftest: rename ok");
}
