/* =============================================================================
 * fs/sip.c - SIP-lite：系统路径的运行时只读保护
 *
 * 规则（生产默认开启）：
 *   system:/System  system:/bin  system:/drivers   任何写操作 -> E_ACCES
 *   写操作 = 创建/打开写、mkdir、unlink、rename（源或目标）、chmod、chown。
 *   连 root 也一样：root 的 CAP_DAC_OVERRIDE 不能用来改这些目录。
 *
 * 例外：签名更新器（_pkgd）在安装/卸载组件期间进入 "updater scope"。scope 绑定
 * 到**进入时的线程 pid**，只有该线程在 scope 内的写操作被放行；其它线程即使
 * 同时运行也照样被拒（避免"一开全局标志就人人可写"）。
 *
 * 关闭方式：内核参数 sip=0（开发/排障）；运行中也可用 sip_set_enabled(0)
 * 临时关掉（自检用它验证开关本身有效）。测试构建（-Test）默认**仍然开启**，
 * 否则"root 写系统目录被拒"这条测试就没有意义。
 *
 * 设计取舍：路径在判定前先做纯文本规范化（去掉 "."、".."、重复 '/'），
 * 所以 `system:/bin/../System/x` 与 `system:/System/./x` 一样被拦。
 * 规范化不解析符号链接——等符号链接（阶段 4b）落地后再补"解析后复查"。
 * ============================================================================= */
#include "fs.h"
#include "include/kernel.h"
#include "include/proc.h"

#define SIP_SEG_MAX 32U

static int sip_is_on = 1;

/* updater scope：进入时的 pid + 嵌套计数。pid 0 = 启动主线（还没有 proc） */
static uint32_t sip_scope_owner;
static uint32_t sip_scope_depth;

static uint32_t sip_me(void)
{
    proc_t *p = proc_current();

    return (p != 0) ? (uint32_t)p->pid : 0U;
}

void sip_init(void)
{
    if (boot_cmdline_has("sip=0") || boot_cmdline_has("sip=off")) {
        sip_is_on = 0;
        kprintln("sip: disabled by kernel cmdline (sip=0) -- dev mode");
        return;
    }
    sip_is_on = 1;
    kprintln("sip: enabled (system:/System, system:/bin, system:/drivers are read-only)");
}

int sip_enabled(void)
{
    return sip_is_on;
}

void sip_set_enabled(int on)
{
    sip_is_on = (on != 0) ? 1 : 0;
    kprintf("sip: %s\n", sip_is_on ? "enabled" : "disabled");
}

void sip_updater_enter(void)
{
    if (sip_scope_depth == 0U) sip_scope_owner = sip_me();
    sip_scope_depth++;
}

void sip_updater_exit(void)
{
    if (sip_scope_depth > 0U) sip_scope_depth--;
    if (sip_scope_depth == 0U) sip_scope_owner = 0U;
}

int sip_updater_active(void)
{
    return (sip_scope_depth > 0U && sip_scope_owner == sip_me()) ? 1 : 0;
}

/* 把 "vol:relative/path" 规范化成 "vol:seg/seg"；忽略 "."、处理 ".."、压缩
 * 重复 '/'。没有卷前缀时按默认卷 system 处理（和 ramfs 的 path_volume 一致）。 */
static void sip_normalize(const char *path, char *vol, uint32_t vol_max,
                          char *rel, uint32_t rel_max)
{
    const char *colon = strchr(path, ':');
    const char *p = path;

    vol[0] = '\0';
    rel[0] = '\0';

    if (colon != 0) {
        uint32_t n = (uint32_t)(colon - path);
        if (n >= vol_max) n = vol_max - 1U;
        memcpy(vol, path, n);
        vol[n] = '\0';
        p = colon + 1;
    } else {
        strncpy(vol, "system", vol_max - 1U);
        vol[vol_max - 1U] = '\0';
    }

    {
        uint32_t used = 0;
        const char *seg = p;

        while (*seg != '\0') {
            const char *end;
            uint32_t len;

            while (*seg == '/') seg++;
            if (*seg == '\0') break;
            end = seg;
            while (*end != '\0' && *end != '/') end++;
            len = (uint32_t)(end - seg);

            if (len == 1U && seg[0] == '.') {
                /* 当前目录：跳过 */
            } else if (len == 2U && seg[0] == '.' && seg[1] == '.') {
                /* 回退一段（已在根就停住） */
                while (used > 0U && rel[used - 1U] != '/') used--;
                if (used > 0U) used--;              /* 去掉分隔符 */
                rel[used] = '\0';
            } else if (len > 0U) {
                if (used > 0U) {
                    if (used + 1U >= rel_max) break;
                    rel[used++] = '/';
                }
                for (uint32_t i = 0; i < len; i++) {
                    if (used + 1U >= rel_max) break;
                    rel[used++] = seg[i];
                }
                rel[used] = '\0';
            }
            seg = end;
        }
    }
}

static int seg_equals(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;

        if (ca != cb) return 0;
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0') ? 1 : 0;
}

int sip_path_protected(const char *path)
{
    char vol[FS_DRIVE_MAX];
    char rel[FS_PATH_MAX];
    const char *first;
    char head[SIP_SEG_MAX];
    uint32_t n = 0;

    if (path == 0) return 0;
    sip_normalize(path, vol, sizeof(vol), rel, sizeof(rel));
    if (!seg_equals(vol, "system")) return 0;      /* 只保护 system 卷 */
    if (rel[0] == '\0') return 0;                  /* 卷根本身 */

    first = rel;
    while (first[n] != '\0' && first[n] != '/' && n + 1U < sizeof(head)) {
        head[n] = first[n];
        n++;
    }
    head[n] = '\0';

    return (seg_equals(head, "System") || seg_equals(head, "bin") ||
            seg_equals(head, "drivers")) ? 1 : 0;
}

int sip_guard_write(const char *path)
{
    if (!sip_is_on) return E_OK;
    if (sip_updater_active()) return E_OK;         /* 签名更新器作用域 */
    if (sip_path_protected(path)) return E_ACCES;
    return E_OK;
}

/* -------------------------------------------------------------------------
 * 自检：root 被拒 / 路径穿越 / updater 放行 / 开关有效
 * ------------------------------------------------------------------------- */
static int sip_fail(int step)
{
    kprintf("sip: selftest FAILED at step %d\n", step);
    return -1;
}

static int sip_write_denied(const char *path)
{
    return (fs_open_as(path, FS_OPEN_WRITE | FS_OPEN_CREAT, 0U, 0U) == 0U);
}

void sip_selftest(void)
{
    fs_stat_t st;
    uint32_t h;
    int denied, ok;

    if (!sip_is_on) {
        kprintln("sip: selftest SKIPPED (disabled)");
        return;
    }

    /* 1. 三个受保护目录：root 建目录被拒 */
    denied = (fs_mkdir_as("system:/System/sip-t", 0U, 0U) == E_ACCES);
    if (denied) kprintln("sip: root write system:/System denied ok"); else { sip_fail(1); return; }
    denied = (fs_mkdir_as("system:/bin/sip-t", 0U, 0U) == E_ACCES);
    if (denied) kprintln("sip: root write system:/bin denied ok"); else { sip_fail(2); return; }
    denied = (fs_mkdir_as("system:/drivers/sip-t", 0U, 0U) == E_ACCES);
    if (denied) kprintln("sip: root write system:/drivers denied ok"); else { sip_fail(3); return; }

    /* 2. 对已存在文件的写/删/改名/改权限都拒（root 也不行） */
    denied = sip_write_denied("system:/bin/shell");
    ok = (fs_unlink_as("system:/bin/shell", 0U, 0U) == E_ACCES);
    if (!denied || !ok) { sip_fail(4); return; }
    ok = (fs_rename_as("system:/bin/shell", "system:/tmp/sip-moved", 0U, 0U) == E_ACCES);
    if (!ok) { sip_fail(5); return; }
    ok = (fs_chmod("system:/bin/shell", 0777U, 0U) == E_ACCES);
    if (!ok) { sip_fail(6); return; }
    ok = (fs_chown("system:/bin/shell", 1000U, 1000U) == E_ACCES);
    if (!ok) { sip_fail(7); return; }
    kprintln("sip: root write/unlink/rename/chmod/chown denied ok");

    /* 3. 路径穿越：先规范化再判定 */
    ok = (fs_mkdir_as("system:/bin/../System/sip-x", 0U, 0U) == E_ACCES) &&
         (fs_mkdir_as("system:/System/./sip-y", 0U, 0U) == E_ACCES) &&
         (fs_mkdir_as("system://bin//sip-z", 0U, 0U) == E_ACCES);
    if (ok) kprintln("sip: path traversal normalized ok"); else { sip_fail(8); return; }

    /* 4. 包仓库（system:/System/Packages）对普通 root 写同样拒绝 */
    ok = (fs_mkdir_as("system:/System/Packages/sip-pkg", 0U, 0U) == E_ACCES);
    if (ok) kprintln("sip: package store write denied for root ok"); else { sip_fail(9); return; }

    /* 5. updater scope：进入后可写，退出后被拒 */
    sip_updater_enter();
    h = fs_open_as("system:/System/.sip-ok", FS_OPEN_WRITE | FS_OPEN_CREAT, 0U, 0U);
    if (h == 0U) { sip_updater_exit(); sip_fail(10); return; }
    (void)fs_write(h, "x", 1U);
    fs_close(h);
    sip_updater_exit();

    ok = (fs_unlink_as("system:/System/.sip-ok", 0U, 0U) == E_ACCES);
    sip_updater_enter();
    ok = ok && (fs_unlink_as("system:/System/.sip-ok", 0U, 0U) == E_OK);
    sip_updater_exit();
    if (ok) kprintln("sip: updater scope bypass ok"); else { sip_fail(11); return; }

    /* 6. 开关：关闭时 root 可写，重新打开后又被拒 */
    sip_set_enabled(0);
    ok = (fs_mkdir("system:/System/.sip-dev") == E_OK);
    if (ok) (void)fs_unlink("system:/System/.sip-dev");
    sip_set_enabled(1);
    ok = ok && (fs_mkdir_as("system:/System/.sip-dev", 0U, 0U) == E_ACCES);
    if (ok) kprintln("sip: sip=0 disables protection ok"); else { sip_fail(12); return; }

    (void)fs_stat("system:/bin/shell", &st);       /* 读仍然允许 */
    kprintln("sip: selftest PASS");
}
