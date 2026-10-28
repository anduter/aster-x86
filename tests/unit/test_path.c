/* =============================================================================
 * tests/unit/test_path.c - syscall ????????? host ??????
 *
 * ???????????rc/kernel/proc/trap/syscall_util.c??ake_abs_path / path_has_parent /
 * copy_user_string / user_range_ok / fd_alloc / open_rights???? *
 * ?????? i686 ?????yscall_util.c ??sys_getchar_block() ??32 ???????? * ??ushfl/popl???x86_64 host gcc ????????2 ????????????????????????
 * ????????????????????? -D ???????? *
 * ????shim??roc_current / proc_root_of / proc_block / keyboard_getchar /
 * copy_from_user / copy_to_user??????????????? lib/string.c??D ??????? * ========================================================================== */
#include "unit.h"
#include "../../src/kernel/proc/proc.h"

/* ---- ????proc ??? ---- */
static proc_t fake_proc;
static const char *fake_root = "";

proc_t *proc_current(void) { return &fake_proc; }
const char *proc_root_of(proc_t *p) { (void)p; return fake_root; }
bool proc_signal_pending(void) { return false; }
void proc_block(unsigned int chan) { (void)chan; }
int keyboard_getchar(void) { return -1; }

/* copy_user_string ???0xDEADBEEF ?????????????? */
#define BAD_USER_PTR 0xDEADBEEFU

int copy_from_user(void *dst, const void *src, uint32_t len)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if ((uintptr_t)src == (uintptr_t)BAD_USER_PTR) return 1;
    for (uint32_t i = 0; i < len; i++) d[i] = s[i];
    return 0;
}

int copy_to_user(void *dst, const void *src, uint32_t len)
{
    return copy_from_user(dst, src, len);
}

/* syscall_util.c ????????? */
void make_abs_path(char *out, uint32_t out_max, const char *in);
int  path_has_parent(const char *p);
int  copy_user_string(char *dst, uint32_t dst_size, uint32_t uptr);
bool user_range_ok(uint32_t ptr, uint32_t len);
proc_fd_t *fd_get(proc_t *p, int fd);
int  fd_alloc_kind(proc_t *p, uint8_t kind, uint32_t handle, uint32_t rights);
int  fd_alloc(proc_t *p, uint32_t handle, uint32_t rights);
uint32_t open_rights(uint32_t flags);

static void set_cwd(const char *s)
{
    uint32_t i = 0;

    while (s[i] != '\0' && i + 1U < sizeof(fake_proc.cwd)) {
        fake_proc.cwd[i] = s[i];
        i++;
    }
    fake_proc.cwd[i] = '\0';
}

static void clear_fds(void)
{
    for (uint32_t i = 0; i < PROC_FD_MAX; i++) {
        fake_proc.fds[i].used = false;
        fake_proc.fds[i].kind = 0;
        fake_proc.fds[i].handle = 0;
        fake_proc.fds[i].rights = 0;
    }
}

static bool str_is(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

int main(void)
{
    char out[64];
    char dst[16];
    char userbuf[8];
    proc_fd_t *fd;
    int i;

    UNIT_BEGIN("path/make_abs_path");

    for (i = 0; i < (int)sizeof(fake_proc.cwd); i++) fake_proc.cwd[i] = '\0';
    clear_fds();
    fake_root = "";

    /* ---- path_has_parent ---- */
    CHECK_INT(path_has_parent(".."), 1);
    CHECK_INT(path_has_parent("../a"), 1);
    CHECK_INT(path_has_parent("a/../b"), 1);
    CHECK_INT(path_has_parent("a..b"), 1);      /* ??????????????? ".." ??? */
    CHECK_INT(path_has_parent("..."), 1);
    CHECK_INT(path_has_parent("a.b"), 0);
    CHECK_INT(path_has_parent("."), 0);
    CHECK_INT(path_has_parent("/"), 0);
    CHECK_INT(path_has_parent(""), 0);
    CHECK_INT(path_has_parent("dir/./file"), 0);

    /* ---- ??root??wd = system: ---- */
    set_cwd("system:");
    make_abs_path(out, sizeof(out), "a");
    CHECK(str_is(out, "system:/a"));
    make_abs_path(out, sizeof(out), "");
    CHECK(str_is(out, "system:"));
    make_abs_path(out, sizeof(out), "/");
    CHECK(str_is(out, "/"));
    make_abs_path(out, sizeof(out), "/a/b");
    CHECK(str_is(out, "/a/b"));
    make_abs_path(out, sizeof(out), "data:/x");
    CHECK(str_is(out, "data:/x"));              /* ??':' ?????????????????? */
    make_abs_path(out, sizeof(out), "a:b");
    CHECK(str_is(out, "a:b"));

    /* cwd ?????'/' ????????'//' */
    set_cwd("system:/");
    make_abs_path(out, sizeof(out), "a");
    CHECK(str_is(out, "system:/a"));
    set_cwd("system:/docs");
    make_abs_path(out, sizeof(out), "a");
    CHECK(str_is(out, "system:/docs/a"));
    make_abs_path(out, sizeof(out), "");
    CHECK(str_is(out, "system:/docs"));

    /* ????????cwd ??? -> ???? system: */
    set_cwd("");
    make_abs_path(out, sizeof(out), "a");
    CHECK(str_is(out, "system:/a"));

    /* ---- ??root??hroot ?????---- */
    set_cwd("system:/");
    fake_root = "system:/jail";
    make_abs_path(out, sizeof(out), "/a");
    CHECK(str_is(out, "system:/jail/a"));
    make_abs_path(out, sizeof(out), "/");
    CHECK(str_is(out, "system:/jail/"));
    /* 相对路径走 cwd（cwd 本身已由 chdir 保证在 root 内），不再拼 root */
    make_abs_path(out, sizeof(out), "x");
    CHECK(str_is(out, "system:/x"));
    set_cwd("system:/jail");
    make_abs_path(out, sizeof(out), "x");
    CHECK(str_is(out, "system:/jail/x"));
    set_cwd("system:/");
    make_abs_path(out, sizeof(out), "system:/jail/x");
    CHECK(str_is(out, "system:/jail/x"));        /* ???????????? */
    make_abs_path(out, sizeof(out), "system:/other/x");
    CHECK(str_is(out, ""));                      /* ??????????-> ??? */
    make_abs_path(out, sizeof(out), "data:/x");
    CHECK(str_is(out, ""));
    make_abs_path(out, sizeof(out), "../a");
    CHECK(str_is(out, ""));                      /* root ?????".." */
    make_abs_path(out, sizeof(out), "a/../../b");
    CHECK(str_is(out, ""));
    /* ????????oot ?????????????????system:/jailer" ????????? jail ??*/
    make_abs_path(out, sizeof(out), "system:/jailer/x");
    CHECK(str_is(out, "system:/jailer/x"));

    /* ---- out_max ????????NUL ?????---- */
    set_cwd("system:/docs");
    fake_root = "";
    make_abs_path(out, 8, "a");
    CHECK(str_is(out, "system:"));
    make_abs_path(out, 1, "a");
    CHECK_INT((unsigned char)out[0], 0);
    make_abs_path(out, 2, "abc");
    CHECK(str_is(out, "s"));
    make_abs_path(out, 0, "a");
    /* out_max=0 ???????????out[out_max-1]??????????????????????????*/

    /* ---- user_range_ok ---- */
    CHECK(user_range_ok(0, 1));
    CHECK(user_range_ok(0, 64U * 1024U));
    CHECK(!user_range_ok(0, 64U * 1024U + 1U));
    CHECK(!user_range_ok(0xC0000000U, 0));       /* ?????????????????*/
    CHECK(user_range_ok(0xBFFFFFFFU, 1));
    CHECK(!user_range_ok(0xBFFFFFFFU, 2));
    CHECK(user_range_ok(0xBFFF0000U, 0x10000U));
    CHECK(!user_range_ok(0xFFFFFFFFU, 1));

    /* ---- copy_user_string ---- */
    userbuf[0] = 'a'; userbuf[1] = 'b'; userbuf[2] = 'c'; userbuf[3] = '\0';
    CHECK_INT(copy_user_string(dst, 0, (uint32_t)(uintptr_t)userbuf), -1); /* E_INVAL */
    CHECK_INT(copy_user_string(dst, 5, (uint32_t)(uintptr_t)userbuf), 0);
    CHECK(str_is(dst, "abc"));
    /* dst_size=4 ?????? 3 ????????? "abc" ????4 ?????? NUL??> ??? */
    dst[0] = 'x'; dst[1] = 'x'; dst[2] = 'x'; dst[3] = 'x';
    CHECK_INT(copy_user_string(dst, 4, (uint32_t)(uintptr_t)userbuf), -1);
    CHECK(str_is(dst, "abc"));
    CHECK_INT(copy_user_string(dst, 5, BAD_USER_PTR), -1);
    CHECK_INT(copy_user_string(dst, 5, (uint32_t)(uintptr_t)"z"), 0);
    CHECK(dst[0] == 'z' && dst[1] == '\0');

    /* ---- open_rights ---- */
    CHECK_U32(open_rights(0), 0x1U);             /* ?????? */
    CHECK_U32(open_rights(0x01U), 0x1U);
    CHECK_U32(open_rights(0x02U), 0x2U);
    CHECK_U32(open_rights(0x03U), 0x3U);
    CHECK_U32(open_rights(0x80U), 0x1U);         /* ????????*/

    /* ---- fd_alloc / fd_get ---- */
    clear_fds();
    CHECK(fd_get(&fake_proc, -1) == 0);
    CHECK(fd_get(&fake_proc, PROC_FD_MAX) == 0);
    CHECK(fd_get(&fake_proc, 0) == 0);           /* ?????*/
    CHECK_INT(fd_alloc(&fake_proc, 7, 0x1U), 3);
    fd = fd_get(&fake_proc, 3);
    CHECK(fd != 0);
    if (fd != 0) {
        CHECK(fd->used);
        CHECK_U32(fd->kind, FD_KIND_FILE);
        CHECK_U32(fd->handle, 7);
        CHECK_U32(fd->rights, 0x1U);
    }
    CHECK_INT(fd_alloc_kind(&fake_proc, FD_KIND_PIPE_W, 9, 0x2U), 4);
    CHECK_U32(fake_proc.fds[4].kind, FD_KIND_PIPE_W);
    CHECK(fd_get(&fake_proc, 4) != 0);

    /* 3..PROC_FD_MAX-1 ?????-> ??? -1??/1/2 ?????? */
    for (i = 5; i < PROC_FD_MAX; i++) CHECK_INT(fd_alloc(&fake_proc, (uint32_t)i, 1), i);
    CHECK_INT(fd_alloc(&fake_proc, 1, 1), -1);
    CHECK(fd_get(&fake_proc, 2) == 0);

    UNIT_END();
}

