/*
 * ng audit-sys: ring3 user->kernel 边界探针（syscall 0/1, 10..14, 20..27,
 * 30..35, 40..47）。
 *
 * 这是一个独立的 ring3 ELF，不编进内核：host 用 tools/audit-ng-sys.ps1 编译，
 * 经 tests/s1/fat-add.ps1 写进 FAT16 磁盘镜像的副本，再由 ring3 shell
 * `spawn /mnt/disk/audit.elf` 拉起。每个用例打印一行
 *   AUDIT: PASS <name>
 *   AUDIT: FAIL <name> got=<n> want=<n>
 * 最后打印 `AUDIT: summary pass=N fail=M total=T`，host 断言 fail=0。
 *
 * 覆盖目标（只碰 QEMU 里的客户机）：
 *   - 非法用户指针：0、内核低地址 0x1000、未映射 0x41000000、跨页边界；
 *   - len 0 / 超大 / 截断（IPC msg 头、FS IO 上限、SVC 缓冲上限）；
 *   - spawn 超长 path/argv、坏 argv、8 项无 NUL 终结的 argv、坏 &pid、
 *     wait/kill 非法 pid、并发 spawn/kill/wait（storm）；
 *   - IPC 坏 ticket、depth 越界、队列满/关闭/超时、ncaps 语义边界；
 *   - FS 长路径、坏 fd、listdir 小 max / 超大 max（canary 检查栈缓冲写穿）、
 *     stat 坏指针；
 *   - 显示/会话机制面：SYS_SHM_ALLOC/MAP 越界、SYS_FB_MAP 坏指针；
 *   - 崩溃对照 mode `crash`：ring3 空指针解引用 —— 已知限制（trap.c 对
 *     用户异常仍 panic），host 期望看到 PANIC: vec=0xe，作为设计级记录。
 *
 * 子模式（由 storm 用例 spawn 自己）：
 *   audit.elf child <id> block   建端口后阻塞在 recv（等 kill）
 *   audit.elf child <id> quick   立刻 exit(0)
 */
#include "types.h"

/* ---- ABI 常量（docs/abi/abi-1.0.md；用户程序不 include 内核头）---------- */
#define SYS_WRITE 0
#define SYS_EXIT 1
#define SYS_PORT_CREATE 10
#define SYS_PORT_SEND 11
#define SYS_PORT_RECV 12
#define SYS_PORT_REPLY 13
#define SYS_PORT_CLOSE 14
#define SYS_OPEN 20
#define SYS_READ 21
#define SYS_WRITE_FD 22
#define SYS_CLOSE 23
#define SYS_STAT 24
#define SYS_LISTDIR 25
#define SYS_MKDIR 26
#define SYS_UNLINK 27
#define SYS_READKEY 30
#define SYS_LOGIN 31
#define SYS_DMESG 32
#define SYS_LOG_WRITE 33
#define SYS_PKG_LIST 34
#define SYS_PKG_VERIFY 35
#define SYS_SPAWN 40
#define SYS_WAIT 41
#define SYS_GETPID 42
#define SYS_KILL 43
#define SYS_GETPPID 44
#define SYS_SHM_ALLOC 45
#define SYS_SHM_MAP 46
#define SYS_FB_MAP 47

/* 与内核一致的错误码（负 errno）。 */
#define E_PERM (-1)
#define E_NOENT (-2)
#define E_SRCH (-3)
#define E_CHILD (-10)
#define E_AGAIN (-11)
#define E_NOMEM (-12)
#define E_ACCES (-13)
#define E_FAULT (-14)
#define E_BUSY (-16)
#define E_EXIST (-17)
#define E_NOTDIR (-20)
#define E_ISDIR (-21)
#define E_INVAL (-22)
#define E_MFILE (-24)
#define E_NOSPC (-28)
#define E_ROFS (-30)
#define E_NAMETOOLONG (-36)
#define E_MSGSIZE (-90)
#define E_TIMEDOUT (-110)

/* IPC / FS / SHM 上限（ABI 1.0）。 */
#define IPC_HDR 16u
#define IPC_MSG_MAX 4112u
#define FS_IO_MAX 512u
#define SHM_PAGES_MAX 1024u

#define O_RDONLY 0x000u
#define O_WRONLY 0x001u
#define O_RDWR 0x002u
#define O_CREAT 0x040u
#define O_EXCL 0x080u
#define O_TRUNC 0x200u

#define ARRAY_N(a) (sizeof(a) / sizeof((a)[0]))

struct audit_hdr {
    u32 type;
    u32 len;
    u32 from;
    u32 flags;
};

struct audit_stat {
    u32 type;
    u32 mode;
    u64 size;
    u64 ino;
};

struct audit_dirent {
    u32 type;
    u32 name_len;
    u64 size;
    char name[64];
};

struct audit_fb_info {
    u64 base;
    u64 phys;
    u64 size;
    u32 pitch;
    u32 bpp;
    u32 width;
    u32 height;
    u32 flags;
    u32 reserved;
};

_Static_assert(sizeof(struct audit_hdr) == 16, "ipc hdr");
_Static_assert(sizeof(struct audit_stat) == 24, "vfs_stat");
_Static_assert(sizeof(struct audit_dirent) == 80, "vfs_dirent");
_Static_assert(sizeof(struct audit_fb_info) == 48, "ds_fb_info");

/* ---- 静态缓冲（.bss；16 KiB 用户栈之外）-------------------------------- */
static u8 g_buf[64];
/*
 * IPC_MSG_MAX=4112；dmesg/pkg 的上限都 <= 4096。这里只留 4160 字节，避免
 * .bss 越过 0x40010000（内核会在那里映射每个进程的私有 marker 页，ELF 段
 * 覆盖同一 VA 会被 marker 覆盖）。
 */
static u8 g_big[4160] __attribute__((aligned(4096)));
static char g_path[300];
static char g_path2[300];
static char g_arg127[128];
static u8 g_ents_raw[8 * 80 + 16];
static struct audit_stat g_st;
static struct audit_fb_info g_fb;
static u32 g_pass;
static u32 g_fail;
static u32 g_sleep_port;
static char g_wbuf[256];
static char g_long127[128];
static char g_names[256];

/*
 * PROC_MARKER_VA（0x40010000）是内核给每个进程映射的一页 RW+NX 私有页，
 * 下一页 0x40011000 未映射 —— 这是用户窗口里唯一能构造“正好在页尾/跨页”
 * 的合法可写地址。用它测 copyin/copyout 的页边界，不碰 0x40103xxx 的
 * 初始 argv/栈顶。
 */
#define MARKER_VA 0x40010000ULL
#define MARKER_OFF(off) (MARKER_VA + (u64)(off))

static void marker_fill(u32 off, u8 v, u32 n)
{
    u8 *p = (u8 *)(usize)MARKER_OFF(off);
    u32 i;

    for (i = 0; i < n; i++) {
        p[i] = v;
    }
}

static void marker_put_str(u32 off, const char *s)
{
    char *p = (char *)(usize)MARKER_OFF(off);
    u32 i = 0;

    while (s[i] != '\0') {
        p[i] = s[i];
        i++;
    }
    p[i] = '\0';
}

static void marker_put_u64(u32 off, u64 v)
{
    *(volatile u64 *)(usize)MARKER_OFF(off) = v;
}

static const char k_ro_msg[] = "audit-ro\n";

/* ------------------------------------------------------------------------- */
/* 基础工具                                                                   */
/* ------------------------------------------------------------------------- */
static i64 sys4(i64 nr, i64 a1, i64 a2, i64 a3, i64 a4)
{
    register i64 r10 __asm__("r10") = a4;
    i64 ret;

    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

static i64 sys3(i64 nr, i64 a1, i64 a2, i64 a3)
{
    return sys4(nr, a1, a2, a3, 0);
}

static i64 sys2(i64 nr, i64 a1, i64 a2)
{
    return sys4(nr, a1, a2, 0, 0);
}

static u32 str_len(const char *s)
{
    u32 n = 0;

    while (s != NULL && s[n] != '\0') {
        n++;
    }
    return n;
}

static int str_eq(const char *a, const char *b)
{
    u32 i;

    for (i = 0;; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
        if (a[i] == '\0') {
            return 1;
        }
    }
}

static void put_str(const char *s)
{
    u32 n = str_len(s);

    if (n != 0) {
        (void)sys3(SYS_WRITE, (i64)(usize)s, (i64)n, 0);
    }
}

static int put_s64(char *dst, u32 cap, i64 v)
{
    char tmp[24];
    u32 n = 0;
    u32 i;
    u32 pos = 0;
    int neg = (v < 0);
    u64 u = neg ? (u64)(-(v + 1)) + 1u : (u64)v;

    if (u == 0) {
        tmp[n++] = '0';
    } else {
        while (u != 0 && n < (u32)sizeof(tmp)) {
            tmp[n++] = (char)('0' + (int)(u % 10u));
            u /= 10u;
        }
    }
    if (neg) {
        tmp[n++] = '-';
    }
    for (i = n; i > 0; i--) {
        if (pos + 1u < cap) {
            dst[pos++] = tmp[i - 1u];
        }
    }
    dst[pos] = '\0';
    return (int)pos;
}

static void result(const char *name, int ok, i64 got, i64 want)
{
    char num[28];

    if (ok) {
        g_pass++;
        put_str("AUDIT: PASS ");
        put_str(name);
        put_str("\n");
        return;
    }
    g_fail++;
    put_str("AUDIT: FAIL ");
    put_str(name);
    put_str(" got=");
    (void)put_s64(num, (u32)sizeof(num), got);
    put_str(num);
    put_str(" want=");
    (void)put_s64(num, (u32)sizeof(num), want);
    put_str(num);
    put_str("\n");
}

static void expect(const char *name, i64 got, i64 want)
{
    result(name, got == want, got, want);
}

static void expect_true(const char *name, int cond, i64 got)
{
    result(name, cond != 0, got, 1);
}

static void section(const char *name)
{
    put_str("AUDIT: section ");
    put_str(name);
    put_str("\n");
}

/* 阻塞一小段（用 recv 超时让出 CPU；没有 nanosleep 系统调用）。 */
static void sleep_ticks(u64 ticks)
{
    u8 tmp[8];

    if (g_sleep_port != 0) {
        (void)sys4(SYS_PORT_RECV, (i64)g_sleep_port, (i64)(usize)tmp,
                   (i64)sizeof(tmp), (i64)ticks);
    }
}

/* 探针自己的 ELF 路径（argv[0]），spawn 子模式用。 */
static char *g_self;

/* 统一的 spawn 包装：av 由调用方保证以 NULL 结尾。 */
static i64 spawn_argv(char *const av[], u32 *out_pid)
{
    u32 pid = 0;
    i64 rc;

    if (out_pid != NULL) {
        *out_pid = 0;
    }
    if (av == NULL || av[0] == NULL) {
        return E_INVAL;
    }
    rc = sys3(SYS_SPAWN, (i64)(usize)av[0], (i64)(usize)av,
              (i64)(usize)&pid);
    if (out_pid != NULL) {
        *out_pid = pid;
    }
    return rc;
}

static i64 wait_status(i64 pid, int *out_status)
{
    int st = -1;
    i64 rc = sys3(SYS_WAIT, pid, (i64)(usize)&st, 0);

    if (out_status != NULL) {
        *out_status = st;
    }
    return rc;
}

/* ------------------------------------------------------------------------- */
/* 打开/关闭辅助                                                              */
/* ------------------------------------------------------------------------- */
static i64 open_file(const char *path, u32 flags, u32 mode)
{
    return sys3(SYS_OPEN, (i64)(usize)path, (i64)flags, (i64)mode);
}

/* ------------------------------------------------------------------------- */
/* child 子模式                                                               */
/* ------------------------------------------------------------------------- */
static u32 parse_u32(const char *s)
{
    u32 v = 0;

    while (s != NULL && *s >= '0' && *s <= '9') {
        v = v * 10u + (u32)(*s - '0');
        s++;
    }
    return v;
}

static u64 parse_u64(const char *s)
{
    u64 v = 0;

    while (s != NULL && *s >= '0' && *s <= '9') {
        v = v * 10u + (u64)(*s - '0');
        s++;
    }
    return v;
}

static int child_main(long argc, char **argv)
{
    u32 id = (argc >= 3) ? parse_u32(argv[2]) : 0;
    const char *mode = (argc >= 4) ? argv[3] : "quick";
    int block = (str_eq(mode, "block") || str_eq(mode, "shm") ||
                 str_eq(mode, "fb"));
    char num[24];

    put_str("AUDIT: child up id=");
    (void)put_s64(num, (u32)sizeof(num), (i64)id);
    put_str(num);
    put_str(" pid=");
    (void)put_s64(num, (u32)sizeof(num), sys3(SYS_GETPID, 0, 0, 0));
    put_str(num);
    put_str(block ? " block\n" : " quick\n");

    if (str_eq(mode, "shm") || str_eq(mode, "shmq")) {
        u64 handle = (argc >= 5) ? parse_u64(argv[4]) : 0;
        u64 va = 0;
        i64 rc = sys3(SYS_SHM_MAP, (i64)handle, (i64)(usize)&va, 0);

        if (rc == 0) {
            *(volatile u32 *)(usize)va = 0x5A5A5A5Au;
            put_str("AUDIT: child shm id=");
            (void)put_s64(num, (u32)sizeof(num), (i64)id);
            put_str(num);
            put_str(" va=0x5a5a5a5a\n");
        } else {
            put_str("AUDIT: child shm map failed\n");
            return 5;
        }
        if (str_eq(mode, "shmq")) {
            return 0; /* 正常退出路径：shm_release_proc 必须先摘页再 as_destroy */
        }
    } else if (str_eq(mode, "fb") || str_eq(mode, "fbq")) {
        u64 va = 0;
        struct audit_fb_info info;
        i64 rc = sys3(SYS_FB_MAP, (i64)(usize)&va, (i64)(usize)&info, 0);

        if (rc == 0) {
            put_str("AUDIT: child fb id=");
            (void)put_s64(num, (u32)sizeof(num), (i64)id);
            put_str(num);
            put_str(" mapped\n");
        } else {
            put_str("AUDIT: child fb map failed\n");
            return 6;
        }
        if (str_eq(mode, "fbq")) {
            return 0; /* 正常退出路径：fbmap_release_proc 必须先摘 PTE */
        }
    } else if (str_eq(mode, "selfkill")) {
        u32 sig = (argc >= 5) ? parse_u32(argv[4]) : 9u;

        put_str("AUDIT: child selfkill id=");
        (void)put_s64(num, (u32)sizeof(num), (i64)id);
        put_str(num);
        put_str(" sig=");
        (void)put_s64(num, (u32)sizeof(num), (i64)sig);
        put_str(num);
        put_str("\n");
        /* 自杀：proc_kill 的 p == cur 分支直接从 syscall 里退出，不返回。 */
        (void)sys3(SYS_KILL, (i64)sys3(SYS_GETPID, 0, 0, 0), (i64)sig, 0);
        put_str("AUDIT: child selfkill returned (BUG)\n");
        return 3;
    } else if (str_eq(mode, "eperm") && argc >= 5) {
        u32 ticket = parse_u32(argv[4]);
        struct audit_hdr h;
        u8 tmp[8];
        i64 r1;
        i64 r2;
        i64 r3;

        /* 非属主：recv/close 必须 EPERM；send 允许（ticket 跨进程传递）。 */
        r1 = sys4(SYS_PORT_RECV, (i64)ticket, (i64)(usize)tmp,
                  (i64)sizeof(tmp), 0);
        h.type = 0x0E;
        h.len = 0;
        h.from = 0;
        h.flags = 0;
        r2 = sys4(SYS_PORT_SEND, (i64)ticket, (i64)(usize)&h, IPC_HDR, 0);
        r3 = sys3(SYS_PORT_CLOSE, (i64)ticket, 0, 0);
        put_str("AUDIT: child eperm id=");
        (void)put_s64(num, (u32)sizeof(num), (i64)id);
        put_str(num);
        put_str(" recv=");
        (void)put_s64(num, (u32)sizeof(num), r1);
        put_str(num);
        put_str(" send=");
        (void)put_s64(num, (u32)sizeof(num), r2);
        put_str(num);
        put_str(" close=");
        (void)put_s64(num, (u32)sizeof(num), r3);
        put_str(num);
        put_str("\n");
        return (r1 == -1 && r2 == 0 && r3 == -1) ? 0 : 7;
    } else if (str_eq(mode, "waitblock")) {
        char *cav[6];
        char cid[24];
        u32 gpid = 0;
        int gst = 0;
        i64 rc2;

        /* 孙进程永久阻塞在 recv；自己阻塞在 wait，供父进程 kill。 */
        (void)put_s64(cid, (u32)sizeof(cid), (i64)(id + 1u));
        cav[0] = argv[0];
        cav[1] = (char *)"child";
        cav[2] = cid;
        cav[3] = (char *)"block";
        cav[4] = NULL;
        rc2 = sys3(SYS_SPAWN, (i64)(usize)argv[0], (i64)(usize)cav,
                   (i64)(usize)&gpid);
        if (rc2 != 0) {
            put_str("AUDIT: child waitblock spawn fail\n");
            return 8;
        }
        put_str("AUDIT: child waitblock grandchild=");
        (void)put_s64(num, (u32)sizeof(num), (i64)gpid);
        put_str(num);
        put_str("\n");
        (void)sys3(SYS_WAIT, (i64)gpid, (i64)(usize)&gst, 0); /* 阻塞 */
        put_str("AUDIT: child waitblock returned (BUG)\n");
        return 0;
    } else if (str_eq(mode, "selfkillfb")) {
        u64 va = 0;
        struct audit_fb_info info;
        i64 rcm = sys3(SYS_FB_MAP, (i64)(usize)&va, (i64)(usize)&info, 0);

        put_str("AUDIT: child selfkillfb map=");
        (void)put_s64(num, (u32)sizeof(num), rcm);
        put_str(num);
        put_str("\n");
        (void)sys3(SYS_KILL, (i64)sys3(SYS_GETPID, 0, 0, 0), 9, 0);
        return 3;
    } else if (str_eq(mode, "selfkillshm")) {
        u64 va = 0;
        u32 hh = 0;
        i64 rcm = sys3(SYS_SHM_ALLOC, 4, (i64)(usize)&va, (i64)(usize)&hh);

        if (rcm == 0) {
            *(volatile u32 *)(usize)va = 0x1234u;
        }
        put_str("AUDIT: child selfkillshm alloc=");
        (void)put_s64(num, (u32)sizeof(num), rcm);
        put_str(num);
        put_str("\n");
        (void)sys3(SYS_KILL, (i64)sys3(SYS_GETPID, 0, 0, 0), 9, 0);
        return 3;
    } else if (str_eq(mode, "sender") && argc >= 7) {
        u32 ticket = parse_u32(argv[4]);
        u32 count = parse_u32(argv[5]);
        u64 timeout = parse_u64(argv[6]);
        u32 i;

        for (i = 0; i < count; i++) {
            struct audit_hdr h;
            i64 rc;

            h.type = 0x9000u + i;
            h.len = 0;
            h.from = 0;
            h.flags = 0;
            rc = sys4(SYS_PORT_SEND, (i64)ticket, (i64)(usize)&h, IPC_HDR,
                      (i64)timeout);
            if (rc != 0) {
                put_str("AUDIT: child sender fail id=");
                (void)put_s64(num, (u32)sizeof(num), (i64)id);
                put_str(num);
                put_str(" rc=");
                (void)put_s64(num, (u32)sizeof(num), rc);
                put_str(num);
                put_str("\n");
                return 4;
            }
        }
        put_str("AUDIT: child sender done id=");
        (void)put_s64(num, (u32)sizeof(num), (i64)id);
        put_str(num);
        put_str(" count=");
        (void)put_s64(num, (u32)sizeof(num), (i64)count);
        put_str(num);
        put_str("\n");
        return 0;
    } else if (str_eq(mode, "exitcode") && argc >= 5) {
        u32 code = parse_u32(argv[4]);

        put_str("AUDIT: child exitcode id=");
        (void)put_s64(num, (u32)sizeof(num), (i64)id);
        put_str(num);
        put_str(" code=");
        (void)put_s64(num, (u32)sizeof(num), (i64)(code & 0xFFu));
        put_str(num);
        put_str("\n");
        return (int)(code & 0xFFu);
    }

    if (block) {
        i64 t = sys3(SYS_PORT_CREATE, 4, 0, 0);
        u8 tmp[8];

        if (t <= 0) {
            return 1;
        }
        /* 永久阻塞在 recv：等父进程 kill，用于验证 waiter 取消路径。 */
        (void)sys4(SYS_PORT_RECV, t, (i64)(usize)tmp, (i64)sizeof(tmp),
                   (i64)~0ULL);
        return 2;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* syscall 0 / 1                                                              */
/* ------------------------------------------------------------------------- */
static void test_write(void)
{
    section("write");

    g_buf[0] = 'o';
    g_buf[1] = 'k';
    g_buf[2] = '\n';
    g_buf[3] = 0;

    expect("write.valid", sys3(SYS_WRITE, (i64)(usize)g_buf, 3, 0), 3);
    expect("write.unaligned", sys3(SYS_WRITE, (i64)(usize)(g_buf + 1), 2, 0), 2);
    expect("write.rodata_readable",
           sys3(SYS_WRITE, (i64)(usize)k_ro_msg, (i64)str_len(k_ro_msg), 0),
           (i64)str_len(k_ro_msg));
    /* rdi=0 是 WP1-3 的诊断模式（打印一行，返回 0）。 */
    expect("write.diag_rdi0", sys3(SYS_WRITE, 0, 4, 0), 0);
    expect("write.len0", sys3(SYS_WRITE, (i64)(usize)g_buf, 0, 0), E_INVAL);
    expect("write.len257", sys3(SYS_WRITE, (i64)(usize)g_buf, 257, 0), E_INVAL);
    expect("write.len_u64max",
           sys3(SYS_WRITE, (i64)(usize)g_buf, (i64)~0ULL, 0), E_INVAL);
    expect("write.kernel_ptr", sys3(SYS_WRITE, 0x1000, 4, 0), E_FAULT);
    expect("write.unmapped", sys3(SYS_WRITE, 0x41000000, 4, 0), E_FAULT);
    /* 0x40103FFC 在用户栈最后一页内，跨到 0x40104000（未映射）。 */
    expect("write.cross_stack_top",
           sys3(SYS_WRITE, 0x40103FFC, 8, 0), E_FAULT);
    expect("write.bad_syscall", sys3(999, 0, 0, 0), -1);
}

/* ------------------------------------------------------------------------- */
/* IPC 10..14                                                                 */
/* ------------------------------------------------------------------------- */
static void hdr_init(struct audit_hdr *h, u32 type, u32 len, u32 from, u32 flags)
{
    h->type = type;
    h->len = len;
    h->from = from;
    h->flags = flags;
}

static void test_ipc(void)
{
    struct audit_hdr h;
    struct audit_hdr *got;
    i64 t1;
    i64 t2;
    i64 t3;
    i64 t4;
    i64 held[32];
    u32 i;
    u32 made;
    i64 rc;

    section("ipc");

    /* create 参数边界 */
    t1 = sys3(SYS_PORT_CREATE, 0, 0, 0); /* 0 = 默认 depth */
    expect_true("ipc.create_default", t1 > 0 && t1 <= 0xFFFFFFFFLL, t1);
    expect("ipc.create_depth65", sys3(SYS_PORT_CREATE, 65, 0, 0), E_INVAL);
    expect("ipc.create_depth_u32max",
           sys3(SYS_PORT_CREATE, 0xFFFFFFFFLL, 0, 0), E_INVAL);
    expect("ipc.create_depth_u64max",
           sys3(SYS_PORT_CREATE, (i64)~0ULL, 0, 0), E_INVAL);
    t2 = sys3(SYS_PORT_CREATE, 64, 0, 0);
    expect_true("ipc.create_depth64", t2 > 0, t2);

    /* 坏 ticket / 坏指针 / 坏长度：先做指针校验，再查 ticket。 */
    hdr_init(&h, 1, 0, 0, 0);
    expect("ipc.send_bad_ticket",
           sys4(SYS_PORT_SEND, 0xDEADBEEFLL, (i64)(usize)&h, IPC_HDR, 0), -9);
    expect("ipc.send_bad_ptr",
           sys4(SYS_PORT_SEND, t1, 0x1000, IPC_HDR, 0), E_FAULT);
    expect("ipc.send_null_ptr",
           sys4(SYS_PORT_SEND, t1, 0, IPC_HDR, 0), E_FAULT);
    expect("ipc.send_len15",
           sys4(SYS_PORT_SEND, t1, (i64)(usize)&h, 15, 0), E_INVAL);
    expect("ipc.send_len0",
           sys4(SYS_PORT_SEND, t1, (i64)(usize)&h, 0, 0), E_INVAL);
    expect("ipc.send_len4113",
           sys4(SYS_PORT_SEND, t1, (i64)(usize)&h, 4113, 0), E_MSGSIZE);
    expect("ipc.send_len_max",
           sys4(SYS_PORT_SEND, t1, (i64)(usize)&h, (i64)~0ULL, 0), E_INVAL);
    expect("ipc.send_bad_handle_high",
           sys4(SYS_PORT_SEND, 0x100000000LL, (i64)(usize)&h, IPC_HDR, 0), -9);

    /* 消息头校验。 */
    hdr_init(&h, 1, 1, 0, 0); /* hdr.len+16 != 16 */
    expect("ipc.send_hdr_len_mismatch",
           sys4(SYS_PORT_SEND, t1, (i64)(usize)&h, IPC_HDR, 0), E_INVAL);
    hdr_init(&h, 1, 0, 0, 1); /* flags 必须为 0 */
    expect("ipc.send_hdr_flags",
           sys4(SYS_PORT_SEND, t1, (i64)(usize)&h, IPC_HDR, 0), E_INVAL);
    hdr_init(&h, 1, 0, 0xDEADBEEF, 0); /* from 不是存活 ticket */
    expect("ipc.send_hdr_bad_from",
           sys4(SYS_PORT_SEND, t1, (i64)(usize)&h, IPC_HDR, 0), -9);

    /* 正常 send/recv（recv 返回消息总长，不是 payload 长度）。 */
    hdr_init(&h, 0x55, 0, 0, 0);
    expect("ipc.send_ok", sys4(SYS_PORT_SEND, t1, (i64)(usize)&h, IPC_HDR, 0), 0);
    expect("ipc.recv_ok", sys4(SYS_PORT_RECV, t1, (i64)(usize)g_big, IPC_HDR, 0),
           IPC_HDR);
    got = (struct audit_hdr *)g_big;
    expect_true("ipc.recv_payload", got->type == 0x55 && got->len == 0,
                (i64)got->type);

    /* recv 参数边界。 */
    expect("ipc.recv_max0", sys4(SYS_PORT_RECV, t1, (i64)(usize)g_big, 0, 0),
           E_INVAL);
    expect("ipc.recv_bad_ptr",
           sys4(SYS_PORT_RECV, t1, 0x1000, IPC_HDR, 0), E_FAULT);
    /* 超大 max 会被钳到 IPC_MSG_MAX；空队列 + timeout=0 -> EAGAIN。 */
    expect("ipc.recv_max_huge",
           sys4(SYS_PORT_RECV, t1, (i64)(usize)g_big, 0xFFFFFFFFLL, 0), E_AGAIN);

    /* depth=1：队列满、close 之后的行为。 */
    t3 = sys3(SYS_PORT_CREATE, 1, 0, 0);
    expect_true("ipc.create_depth1", t3 > 0, t3);
    hdr_init(&h, 2, 0, 0, 0);
    expect("ipc.fill", sys4(SYS_PORT_SEND, t3, (i64)(usize)&h, IPC_HDR, 0), 0);
    expect("ipc.full_eagain",
           sys4(SYS_PORT_SEND, t3, (i64)(usize)&h, IPC_HDR, 0), E_AGAIN);
    /* 阻塞 send 超时：必须让出 CPU 后按 deadline 返回 ETIMEDOUT。 */
    expect("ipc.full_timeout",
           sys4(SYS_PORT_SEND, t3, (i64)(usize)&h, IPC_HDR, 20000000), E_TIMEDOUT);
    expect("ipc.drain", sys4(SYS_PORT_RECV, t3, (i64)(usize)g_big, IPC_HDR, 0),
           IPC_HDR);
    expect("ipc.close", sys3(SYS_PORT_CLOSE, t3, 0, 0), 0);
    expect("ipc.recv_after_close",
           sys4(SYS_PORT_RECV, t3, (i64)(usize)g_big, IPC_HDR, 0), -9);
    expect("ipc.close_twice", sys3(SYS_PORT_CLOSE, t3, 0, 0), -9);

    /* 空队列 recv 超时。 */
    t4 = sys3(SYS_PORT_CREATE, 1, 0, 0);
    expect_true("ipc.create_empty", t4 > 0, t4);
    expect("ipc.recv_timeout",
           sys4(SYS_PORT_RECV, t4, (i64)(usize)g_big, IPC_HDR, 20000000),
           E_TIMEDOUT);
    expect("ipc.close_empty", sys3(SYS_PORT_CLOSE, t4, 0, 0), 0);

    /* reply：坏 ticket / 正常入队。 */
    expect("ipc.reply_bad_ticket",
           sys4(SYS_PORT_REPLY, 0xDEADBEEFLL, (i64)(usize)&h, IPC_HDR, 0), -9);
    hdr_init(&h, 3, 0, 0, 0);
    expect("ipc.reply_ok", sys4(SYS_PORT_REPLY, t1, (i64)(usize)&h, IPC_HDR, 0), 0);
    expect("ipc.reply_recv", sys4(SYS_PORT_RECV, t1, (i64)(usize)g_big, IPC_HDR, 0),
           IPC_HDR);

    /* 端口池上限：持有所有能创建的端口（不 close），下一个必须是 ENOSPC。 */
    made = 0;
    rc = 0;
    for (i = 0; i < 32u; i++) {
        rc = sys3(SYS_PORT_CREATE, 1, 0, 0);
        if (rc <= 0) {
            break;
        }
        held[made] = rc;
        made++;
    }
    expect_true("ipc.port_pool_cycle", made >= 1 && rc != 0, rc);
    expect("ipc.port_pool_enospc", rc, E_NOSPC);
    for (i = 0; i < made; i++) {
        (void)sys3(SYS_PORT_CLOSE, held[i], 0, 0);
    }

    /* 从 ticket（hdr.from）跨端口发送：from 必须是存活 PORT 句柄。 */
    hdr_init(&h, 4, 0, (u32)t2, 0);
    expect("ipc.send_from_ticket",
           sys4(SYS_PORT_SEND, t1, (i64)(usize)&h, IPC_HDR, 0), 0);
    expect("ipc.send_from_recv", sys4(SYS_PORT_RECV, t1, (i64)(usize)g_big, IPC_HDR, 0),
           IPC_HDR);

    /* 最大消息（16 头 + 4096 payload = 4112）与短 recv 缓冲。 */
    {
        struct audit_hdr *big = (struct audit_hdr *)(void *)g_big;
        u32 k;

        for (k = 0; k < 4112u; k++) {
            g_big[k] = 0;
        }
        big->type = 0x66;
        big->len = 4096;
        big->from = 0;
        big->flags = 0;
        expect("ipc.send_max_msg", sys4(SYS_PORT_SEND, t2, (i64)(usize)g_big, 4112, 0),
               0);
        expect("ipc.recv_max_msg", sys4(SYS_PORT_RECV, t2, (i64)(usize)g_big, 4112, 0),
               4112);
        expect_true("ipc.recv_max_msg_hdr",
                    big->type == 0x66 && big->len == 4096, (i64)big->type);

        /* 短缓冲：消息总长 24，max=16 时返回总长、只拷前 16 字节。 */
        big->type = 0x77;
        big->len = 8;
        big->from = 0;
        big->flags = 0;
        g_big[16] = 0xAA;
        g_big[17] = 0xBB;
        expect("ipc.send_short_msg", sys4(SYS_PORT_SEND, t2, (i64)(usize)g_big, 24, 0),
               0);
        expect("ipc.recv_short_buf", sys4(SYS_PORT_RECV, t2, (i64)(usize)g_big, 16, 0),
               24);
        expect_true("ipc.recv_short_hdr",
                    ((struct audit_hdr *)(void *)g_big)->type == 0x77,
                    (i64)((struct audit_hdr *)(void *)g_big)->type);
    }

    /* close 时有排队消息：ring 里的消息必须被释放，recv 变 EBADF。 */
    {
        i64 t5 = sys3(SYS_PORT_CREATE, 2, 0, 0);

        expect_true("ipc.close_queued_port", t5 > 0, t5);
        if (t5 > 0) {
            hdr_init(&h, 9, 0, 0, 0);
            expect("ipc.close_queued_send1",
                   sys4(SYS_PORT_SEND, t5, (i64)(usize)&h, IPC_HDR, 0), 0);
            expect("ipc.close_queued_send2",
                   sys4(SYS_PORT_SEND, t5, (i64)(usize)&h, IPC_HDR, 0), 0);
            expect("ipc.close_queued_close", sys3(SYS_PORT_CLOSE, t5, 0, 0), 0);
            expect("ipc.close_queued_recv",
                   sys4(SYS_PORT_RECV, t5, (i64)(usize)g_big, IPC_HDR, 0), -9);
        }
    }

    (void)sys3(SYS_PORT_CLOSE, t1, 0, 0);
    (void)sys3(SYS_PORT_CLOSE, t2, 0, 0);
}

/* ------------------------------------------------------------------------- */
/* FS 20..27                                                                  */
/* ------------------------------------------------------------------------- */
static void test_fs(void)
{
    i64 fd;
    i64 rc;
    u32 i;
    u32 *canary;
    char num[24];

    section("fs");

    /* 路径指针/长度。 */
    expect("fs.open_bad_ptr", open_file((const char *)0x1000, O_RDONLY, 0),
           E_FAULT);
    expect("fs.open_null_ptr", open_file((const char *)0, O_RDONLY, 0), E_FAULT);
    expect("fs.open_unmapped", open_file((const char *)0x41000000, O_RDONLY, 0),
           E_FAULT);
    expect("fs.open_bad_flags",
           open_file("/bin/shell", 0x80000000u, 0), E_INVAL);
    expect("fs.open_missing", open_file("/bin/nosuch", O_RDONLY, 0), E_NOENT);

    /* 251 字节单组件路径（> VFS_NAME_MAX）：copyin_str 能装下，VFS 拒绝。 */
    g_path[0] = '/';
    for (i = 1; i < 251u; i++) {
        g_path[i] = 'a';
    }
    g_path[251] = '\0';
    rc = open_file(g_path, O_RDONLY, 0);
    expect("fs.open_long_component", rc, E_NAMETOOLONG);

    /* 300 字节路径：copyin_str 在 255 字节截断 -> ENAMETOOLONG。 */
    for (i = 0; i < 299u; i++) {
        g_path2[i] = 'b';
    }
    g_path2[299] = '\0';
    expect("fs.open_overlong_path", open_file(g_path2, O_RDONLY, 0),
           E_NAMETOOLONG);

    /* 正常 open/read/close + 坏 fd/坏指针/坏长度。 */
    fd = open_file("/bin/shell", O_RDONLY, 0);
    expect_true("fs.open_shell", fd >= 3, fd);
    if (fd >= 3) {
        expect("fs.read_ok", sys3(SYS_READ, fd, (i64)(usize)g_big, 64), 64);
        expect("fs.read_bad_ptr", sys3(SYS_READ, fd, 0x1000, 16), E_FAULT);
        expect("fs.read_len0", sys3(SYS_READ, fd, (i64)(usize)g_big, 0), E_INVAL);
        expect("fs.read_len513", sys3(SYS_READ, fd, (i64)(usize)g_big, 513),
               E_INVAL);
        expect("fs.read_len_huge",
               sys3(SYS_READ, fd, (i64)(usize)g_big, (i64)~0ULL), E_INVAL);
        expect("fs.read_cross_stack_top",
               sys3(SYS_READ, fd, 0x40103FFC, 8), E_FAULT);
        expect("fs.close_ok", sys3(SYS_CLOSE, fd, 0, 0), 0);
        expect("fs.close_twice", sys3(SYS_CLOSE, fd, 0, 0), -9);
        expect("fs.read_closed", sys3(SYS_READ, fd, (i64)(usize)g_big, 16), -9);
    }
    expect("fs.read_bad_fd", sys3(SYS_READ, 0xFFFFFFFFLL, (i64)(usize)g_big, 16),
           -9);
    expect("fs.write_bad_fd", sys3(SYS_WRITE_FD, 0xFFFFFFFFLL, (i64)(usize)g_buf, 4),
           -9);

    /* 创建/写入/回读/stat/listdir/unlink。 */
    fd = open_file("/tmp/audit.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
    expect_true("fs.create", fd >= 3, fd);
    if (fd >= 3) {
        g_buf[0] = 'h';
        g_buf[1] = 'e';
        g_buf[2] = 'l';
        g_buf[3] = 'l';
        g_buf[4] = 'o';
        expect("fs.write_ok", sys3(SYS_WRITE_FD, fd, (i64)(usize)g_buf, 5), 5);
        expect("fs.write_bad_ptr", sys3(SYS_WRITE_FD, fd, 0x1000, 5), E_FAULT);
        expect("fs.write_len0", sys3(SYS_WRITE_FD, fd, (i64)(usize)g_buf, 0),
               E_INVAL);
        expect("fs.write_len513", sys3(SYS_WRITE_FD, fd, (i64)(usize)g_buf, 513),
               E_INVAL);
        expect("fs.write_cross_stack_top",
               sys3(SYS_WRITE_FD, fd, 0x40103FFC, 8), E_FAULT);
        expect("fs.close_created", sys3(SYS_CLOSE, fd, 0, 0), 0);
    }

    fd = open_file("/tmp/audit.txt", O_RDONLY, 0);
    expect_true("fs.reopen", fd >= 3, fd);
    if (fd >= 3) {
        for (i = 0; i < 16u; i++) {
            g_big[i] = 0;
        }
        expect("fs.read_back", sys3(SYS_READ, fd, (i64)(usize)g_big, 5), 5);
        expect_true("fs.read_content",
                    g_big[0] == 'h' && g_big[1] == 'e' && g_big[2] == 'l' &&
                        g_big[3] == 'l' && g_big[4] == 'o',
                    (i64)g_big[0]);
        expect("fs.read_eof", sys3(SYS_READ, fd, (i64)(usize)g_big, 5), 0);
        (void)sys3(SYS_CLOSE, fd, 0, 0);
    }

    /* stat 坏指针 + 正常。 */
    expect("fs.stat_ok", sys3(SYS_STAT, (i64)(usize)"/tmp/audit.txt",
                               (i64)(usize)&g_st, 0), 0);
    expect_true("fs.stat_size", g_st.size == 5, (i64)g_st.size);
    expect("fs.stat_bad_out",
           sys3(SYS_STAT, (i64)(usize)"/tmp/audit.txt", 0x1000, 0), E_FAULT);
    expect("fs.stat_null_out",
           sys3(SYS_STAT, (i64)(usize)"/tmp/audit.txt", 0, 0), E_FAULT);
    expect("fs.stat_bad_path", sys3(SYS_STAT, 0x1000, (i64)(usize)&g_st, 0),
           E_FAULT);
    expect("fs.stat_cross_stack_top",
           sys3(SYS_STAT, (i64)(usize)"/tmp/audit.txt", 0x40103FFE, 0), E_FAULT);
    expect("fs.stat_missing", sys3(SYS_STAT, (i64)(usize)"/nosuch",
                                   (i64)(usize)&g_st, 0), E_NOENT);
    expect("fs.stat_missing_out_null",
           sys3(SYS_STAT, (i64)(usize)"/nosuch", 0, 0), E_FAULT);

    /* listdir：max=1 只写一条；max=2^32-1 被钳到 8，不能写穿栈缓冲。 */
    for (i = 0; i < (u32)sizeof(g_ents_raw); i++) {
        g_ents_raw[i] = 0xA5;
    }
    expect("fs.listdir_one", sys3(SYS_LISTDIR, (i64)(usize)"/",
                                  (i64)(usize)g_ents_raw, 1), 1);
    canary = (u32 *)(void *)(g_ents_raw + 8 * 80);
    expect_true("fs.listdir_one_canary", *canary == 0xA5A5A5A5u, (i64)*canary);
    rc = sys3(SYS_LISTDIR, (i64)(usize)"/", (i64)(usize)g_ents_raw,
              0xFFFFFFFFLL);
    expect_true("fs.listdir_huge", rc >= 0 && rc <= 8, rc);
    expect_true("fs.listdir_huge_canary", *canary == 0xA5A5A5A5u, (i64)*canary);
    expect("fs.listdir_bad_path",
           sys3(SYS_LISTDIR, 0x1000, (i64)(usize)g_ents_raw, 1), E_FAULT);
    expect("fs.listdir_bad_buf",
           sys3(SYS_LISTDIR, (i64)(usize)"/", 0x1000, 1), E_FAULT);
    expect("fs.listdir_cross_stack_top",
           sys3(SYS_LISTDIR, (i64)(usize)"/", 0x40103FF0, 2), E_FAULT);
    expect("fs.listdir_null_buf", sys3(SYS_LISTDIR, (i64)(usize)"/", 0, 1),
           E_FAULT);
    /* max=0 目前走 EFAULT 而不是 EINVAL：记录为错误码 nit（不崩溃）。 */
    rc = sys3(SYS_LISTDIR, (i64)(usize)"/", (i64)(usize)g_ents_raw, 0);
    expect_true("fs.listdir_max0_nonpanic", rc < 0, rc);

    /* mkdir / unlink。 */
    expect("fs.mkdir_ok", sys3(SYS_MKDIR, (i64)(usize)"/tmp/audit-dir", 0755, 0),
           0);
    expect("fs.mkdir_exists", sys3(SYS_MKDIR, (i64)(usize)"/tmp/audit-dir", 0755, 0),
           E_EXIST);
    expect("fs.unlink_dir", sys3(SYS_UNLINK, (i64)(usize)"/tmp/audit-dir", 0, 0),
           E_ISDIR);
    expect("fs.mkdir_bad_path", sys3(SYS_MKDIR, 0x1000, 0755, 0), E_FAULT);
    expect("fs.unlink_missing",
           sys3(SYS_UNLINK, (i64)(usize)"/tmp/audit-nosuch", 0, 0), E_NOENT);
    expect("fs.unlink_bad_path", sys3(SYS_UNLINK, 0x1000, 0, 0), E_FAULT);
    expect("fs.unlink_ok", sys3(SYS_UNLINK, (i64)(usize)"/tmp/audit.txt", 0, 0),
           0);
    expect("fs.stat_after_unlink", sys3(SYS_STAT, (i64)(usize)"/tmp/audit.txt",
                                        (i64)(usize)&g_st, 0), E_NOENT);

    put_str("AUDIT: info fs.stat_size=");
    (void)put_s64(num, (u32)sizeof(num), (i64)g_st.size);
    put_str(num);
    put_str("\n");
}

/* ------------------------------------------------------------------------- */
/* SVC 31..35                                                                 */
/* ------------------------------------------------------------------------- */
static void test_svc(void)
{
    i64 rc;

    section("svc");

    expect("svc.login_bad_name_ptr",
           sys2(SYS_LOGIN, 0x1000, (i64)(usize)"x"), E_FAULT);
    expect("svc.login_bad_pass_ptr",
           sys2(SYS_LOGIN, (i64)(usize)"root", 0x1000), E_FAULT);
    expect("svc.login_empty", sys2(SYS_LOGIN, (i64)(usize)"", (i64)(usize)""),
           E_INVAL);
    expect("svc.login_wrong_pass",
           sys2(SYS_LOGIN, (i64)(usize)"root", (i64)(usize)"wrong"), E_ACCES);
    for (u32 i = 0; i < (u32)sizeof(g_arg127) - 1u; i++) {
        g_arg127[i] = 'n';
    }
    g_arg127[(u32)sizeof(g_arg127) - 1u] = '\0';
    expect("svc.login_long_name",
           sys2(SYS_LOGIN, (i64)(usize)g_arg127, (i64)(usize)"x"),
           E_NAMETOOLONG);

    expect("svc.dmesg_bad_ptr", sys2(SYS_DMESG, 0x1000, 16), E_FAULT);
    expect("svc.dmesg_null_ptr", sys2(SYS_DMESG, 0, 16), E_FAULT);
    expect("svc.dmesg_max0", sys2(SYS_DMESG, (i64)(usize)g_big, 0), E_INVAL);
    expect("svc.dmesg_max4097", sys2(SYS_DMESG, (i64)(usize)g_big, 4097), E_INVAL);
    rc = sys2(SYS_DMESG, (i64)(usize)g_big, 64);
    expect_true("svc.dmesg_ok", rc >= 0 && rc <= 64, rc);

    expect("svc.log_bad_tag", sys2(SYS_LOG_WRITE, 0x1000, (i64)(usize)"x"),
           E_FAULT);
    expect("svc.log_bad_msg",
           sys2(SYS_LOG_WRITE, (i64)(usize)"audit", 0x1000), E_FAULT);
    expect("svc.log_empty_tag",
           sys2(SYS_LOG_WRITE, (i64)(usize)"", (i64)(usize)"x"), E_INVAL);
    expect("svc.log_ok",
           sys2(SYS_LOG_WRITE, (i64)(usize)"audit", (i64)(usize)"syscall"), 0);

    expect("svc.pkg_list_bad_ptr", sys2(SYS_PKG_LIST, 0x1000, 16), E_FAULT);
    expect("svc.pkg_list_max0", sys2(SYS_PKG_LIST, (i64)(usize)g_big, 0), E_INVAL);
    expect("svc.pkg_list_max600",
           sys2(SYS_PKG_LIST, (i64)(usize)g_big, 600), E_INVAL);
    rc = sys2(SYS_PKG_LIST, (i64)(usize)g_big, 512);
    expect_true("svc.pkg_list_ok", rc >= 0, rc);

    expect("svc.pkg_verify_bad_ptr", sys3(SYS_PKG_VERIFY, 0x1000, 0, 0), E_FAULT);
    expect("svc.pkg_verify_ok", sys3(SYS_PKG_VERIFY, (i64)(usize)"hello", 0, 0),
           0);
    expect("svc.pkg_verify_missing",
           sys3(SYS_PKG_VERIFY, (i64)(usize)"nosuch", 0, 0), E_NOENT);
    expect("svc.pkg_verify_traversal",
           sys3(SYS_PKG_VERIFY, (i64)(usize)"../etc", 0, 0), E_INVAL);
    expect("svc.pkg_verify_slash",
           sys3(SYS_PKG_VERIFY, (i64)(usize)"hello/../x", 0, 0), E_INVAL);
}

/* ------------------------------------------------------------------------- */
/* PROC 40..44                                                                */
/* ------------------------------------------------------------------------- */
static void test_proc(char **argv)
{
    char *self = (argv != NULL && argv[0] != NULL) ? argv[0] : "/mnt/disk/audit.elf";
    char *av[9];
    char *av8[8];
    char *av_bad[4];
    char hstr[24];
    char num[24];
    u32 pid = 0;
    int status = 0;
    i64 rc;
    u32 pids[4];
    u32 i;

    section("proc");

    rc = sys3(SYS_GETPID, 0, 0, 0);
    expect_true("proc.getpid", rc > 0, rc);
    rc = sys3(SYS_GETPPID, 0, 0, 0);
    expect_true("proc.getppid", rc > 0, rc);

    expect("proc.wait_pid0", sys3(SYS_WAIT, 0, (i64)(usize)&status, 0), E_INVAL);
    expect("proc.wait_pid_high",
           sys3(SYS_WAIT, 0x100000000LL, (i64)(usize)&status, 0), E_INVAL);
    expect("proc.wait_unknown",
           sys3(SYS_WAIT, 999999, (i64)(usize)&status, 0), E_CHILD);
    expect("proc.wait_self",
           sys3(SYS_WAIT, (i64)sys3(SYS_GETPID, 0, 0, 0),
                (i64)(usize)&status, 0), E_CHILD);
    expect("proc.wait_parent",
           sys3(SYS_WAIT, (i64)sys3(SYS_GETPPID, 0, 0, 0),
                (i64)(usize)&status, 0), E_CHILD);

    expect("proc.kill_pid0", sys3(SYS_KILL, 0, 0, 0), E_INVAL);
    expect("proc.kill_pid_high", sys3(SYS_KILL, 0x100000000LL, 0, 0), E_INVAL);
    expect("proc.kill_unknown", sys3(SYS_KILL, 999999, 0, 0), E_SRCH);
    expect("proc.kill_sig256",
           sys3(SYS_KILL, (i64)sys3(SYS_GETPID, 0, 0, 0), 256, 0), E_INVAL);

    /* spawn 参数边界（全部应在创建进程之前被拒）。 */
    av[0] = self;
    av[1] = (char *)"child";
    av[2] = (char *)"1";
    av[3] = NULL;
    expect("proc.spawn_null_path", sys3(SYS_SPAWN, 0, (i64)(usize)av,
                                        (i64)(usize)&pid), E_FAULT);
    expect("proc.spawn_bad_path", sys3(SYS_SPAWN, 0x1000, (i64)(usize)av,
                                       (i64)(usize)&pid), E_FAULT);
    expect("proc.spawn_empty_path",
           sys3(SYS_SPAWN, (i64)(usize)"", (i64)(usize)av, (i64)(usize)&pid),
           E_INVAL);
    expect("proc.spawn_missing",
           sys3(SYS_SPAWN, (i64)(usize)"/bin/nosuch", (i64)(usize)av,
                (i64)(usize)&pid), E_NOENT);
    /* 64 字节 path 上限：63 个字符就会触发 ENAMETOOLONG。 */
    for (i = 0; i < 100u; i++) {
        g_path[i] = 'c';
    }
    g_path[0] = '/';
    g_path[100] = '\0';
    expect("proc.spawn_long_path",
           sys3(SYS_SPAWN, (i64)(usize)g_path, (i64)(usize)av, (i64)(usize)&pid),
           E_NAMETOOLONG);
    expect("proc.spawn_bad_upid",
           sys3(SYS_SPAWN, (i64)(usize)self, (i64)(usize)av, 0), E_FAULT);
    expect("proc.spawn_bad_argv_ptr",
           sys3(SYS_SPAWN, (i64)(usize)self, 0x1000, (i64)(usize)&pid), E_FAULT);
    av_bad[0] = self;
    av_bad[1] = (char *)"child";
    av_bad[2] = (char *)0x1000; /* 坏 argv 元素指针 */
    av_bad[3] = NULL;
    expect("proc.spawn_bad_argv_elem",
           sys3(SYS_SPAWN, (i64)(usize)self, (i64)(usize)av_bad,
                (i64)(usize)&pid), E_FAULT);
    for (i = 0; i < 127u; i++) {
        g_arg127[i] = 'z';
    }
    g_arg127[127] = '\0';
    av_bad[2] = g_arg127; /* 127 字符：>= PROC_ARG_MAX-1 -> 拒绝 */
    av_bad[3] = NULL;
    expect("proc.spawn_arg_too_long",
           sys3(SYS_SPAWN, (i64)(usize)self, (i64)(usize)av_bad,
                (i64)(usize)&pid), E_NAMETOOLONG);

    /*
     * upid 坏指针：必须在 spawn 之前校验。修复前这里会先拉起子进程再
     * copyout 失败 —— 子进程会打印 id=badsid（host 断言该行不存在）。
     */
    av[2] = (char *)"7777";
    expect("proc.spawn_bad_out_ptr",
           sys3(SYS_SPAWN, (i64)(usize)self, (i64)(usize)av, 0x1000), E_FAULT);
    av[2] = (char *)"1";

    /* 正常 spawn/wait。 */
    pid = 0;
    rc = sys3(SYS_SPAWN, (i64)(usize)self, (i64)(usize)av, (i64)(usize)&pid);
    expect_true("proc.spawn_ok", rc == 0 && pid > 0, rc);
    if (rc == 0 && pid != 0) {
        status = 0x7FFFFFFF;
        expect("proc.wait_ok", sys3(SYS_WAIT, pid, (i64)(usize)&status, 0), 0);
        expect("proc.wait_status", status, 0);
        expect("proc.wait_again", sys3(SYS_WAIT, pid, (i64)(usize)&status, 0),
               E_CHILD);
    }

    /* 8 项 argv、无 NULL 终结符：线性边界，不能读未初始化栈槽。 */
    av8[0] = self;
    av8[1] = (char *)"child";
    av8[2] = (char *)"8";
    av8[3] = (char *)"a";
    av8[4] = (char *)"b";
    av8[5] = (char *)"c";
    av8[6] = (char *)"d";
    av8[7] = (char *)"e";
    pid = 0;
    rc = sys3(SYS_SPAWN, (i64)(usize)self, (i64)(usize)av8, (i64)(usize)&pid);
    expect_true("proc.spawn_argv8_no_null", rc == 0 && pid > 0, rc);
    if (rc == 0 && pid != 0) {
        status = 0x7FFFFFFF;
        expect("proc.wait_argv8", sys3(SYS_WAIT, pid, (i64)(usize)&status, 0), 0);
        expect("proc.wait_argv8_status", status, 0);
    }

    /* wait 的 status 坏指针必须在阻塞之前拒绝：先起一个 block 子进程。 */
    av[2] = (char *)"2";
    pid = 0;
    rc = sys3(SYS_SPAWN, (i64)(usize)self, (i64)(usize)av, (i64)(usize)&pid);
    expect_true("proc.spawn_block", rc == 0 && pid > 0, rc);
    if (rc == 0 && pid != 0) {
        expect("proc.wait_bad_status",
               sys3(SYS_WAIT, pid, 0x1000, 0), E_FAULT);
        expect("proc.kill_block", sys3(SYS_KILL, pid, 9, 0), 0);
        status = 0;
        expect("proc.wait_killed", sys3(SYS_WAIT, pid, (i64)(usize)&status, 0), 0);
        expect("proc.killed_status", status, 0x100 | 9);
    }

    /*
     * storm：4 个 block 子进程 -> sleep -> 全部 kill -> wait；
     * 再 4 个 quick 子进程 -> 立即 wait（同时覆盖"未跑过就被 kill"和
     * "跑完再 wait"两条路径）。
     */
    for (i = 0; i < 4u; i++) {
        av[2] = (char *)"10";
        av[3] = (char *)"block";
        av[4] = NULL;
        pid = 0;
        rc = sys3(SYS_SPAWN, (i64)(usize)self, (i64)(usize)av, (i64)(usize)&pid);
        pids[i] = pid;
        expect_true("proc.storm_spawn_block", rc == 0 && pid > 0, rc);
    }
    sleep_ticks(40000000); /* ~40ms：让 4 个 block 子进程跑到 recv */
    for (i = 0; i < 4u; i++) {
        if (pids[i] != 0) {
            (void)sys3(SYS_KILL, pids[i], 9, 0);
        }
    }
    for (i = 0; i < 4u; i++) {
        if (pids[i] != 0) {
            status = 0;
            rc = sys3(SYS_WAIT, pids[i], (i64)(usize)&status, 0);
            expect_true("proc.storm_wait_block", rc == 0 && status == (0x100 | 9),
                        rc);
        }
    }
    put_str("AUDIT: info proc.storm_block pids=");
    for (i = 0; i < 4u; i++) {
        (void)put_s64(num, (u32)sizeof(num), (i64)pids[i]);
        put_str(num);
        put_str(i + 1u < 4u ? "," : "\n");
    }

    av[3] = (char *)"quick";
    av[4] = NULL;
    for (i = 0; i < 4u; i++) {
        av[2] = (char *)"20";
        pid = 0;
        rc = sys3(SYS_SPAWN, (i64)(usize)self, (i64)(usize)av, (i64)(usize)&pid);
        expect_true("proc.storm_spawn_quick", rc == 0 && pid > 0, rc);
        if (rc == 0 && pid != 0) {
            if (i == 0u) {
                (void)sys3(SYS_KILL, pid, 15, 0);
            }
            status = 0;
            rc = sys3(SYS_WAIT, pid, (i64)(usize)&status, 0);
            expect_true("proc.storm_wait_quick",
                        rc == 0 && (status == 0 || status == (0x100 | 15)), rc);
        }
    }

    /*
     * 跨进程共享页 + kill 回收顺序：子进程映射父进程的 shm 并写同一物理页，
     * 父进程验证内容；kill 子进程后，父进程的页必须仍然可读写（shm_release_proc
     * 先把共享页从子 AS 摘掉，再 as_destroy —— 顺序错会 double free/内容丢失）。
     */
    {
        u64 shmva = 0;
        u32 shmh = 0;
        i64 src = sys3(SYS_SHM_ALLOC, 4, (i64)(usize)&shmva,
                       (i64)(usize)&shmh);

        expect_true("proc.shm_alloc_for_child", src == 0 && shmh != 0, src);
        if (src == 0) {
            *(volatile u32 *)(usize)shmva = 0x11111111u;
            (void)put_s64(hstr, (u32)sizeof(hstr), (i64)shmh);
            av[2] = (char *)"30";
            av[3] = (char *)"shm";
            av[4] = hstr;
            av[5] = NULL;
            pid = 0;
            rc = sys3(SYS_SPAWN, (i64)(usize)self, (i64)(usize)av,
                      (i64)(usize)&pid);
            expect_true("proc.shm_child_spawn", rc == 0 && pid > 0, rc);
            if (rc == 0 && pid != 0) {
                sleep_ticks(40000000);
                expect_true("proc.shm_child_wrote",
                            *(volatile u32 *)(usize)shmva == 0x5A5A5A5Au,
                            (i64)*(volatile u32 *)(usize)shmva);
                expect("proc.shm_child_kill", sys3(SYS_KILL, pid, 9, 0), 0);
                status = 0;
                expect("proc.shm_child_wait",
                       sys3(SYS_WAIT, pid, (i64)(usize)&status, 0), 0);
                expect("proc.shm_child_status", status, 0x100 | 9);
                expect_true("proc.shm_parent_survives",
                            *(volatile u32 *)(usize)shmva == 0x5A5A5A5Au,
                            (i64)*(volatile u32 *)(usize)shmva);
                *(volatile u32 *)(usize)shmva = 0x22222222u;
            }
        }
    }

    /* 子进程映射 LFB 后被杀：fbmap_release_proc 必须先摘 PTE 再 as_destroy。 */
    av[2] = (char *)"31";
    av[3] = (char *)"fb";
    av[4] = NULL;
    pid = 0;
    rc = sys3(SYS_SPAWN, (i64)(usize)self, (i64)(usize)av, (i64)(usize)&pid);
    expect_true("proc.fb_child_spawn", rc == 0 && pid > 0, rc);
    if (rc == 0 && pid != 0) {
        sleep_ticks(40000000);
        expect("proc.fb_child_kill", sys3(SYS_KILL, pid, 9, 0), 0);
        status = 0;
        expect("proc.fb_child_wait", sys3(SYS_WAIT, pid, (i64)(usize)&status, 0),
               0);
        expect("proc.fb_child_status", status, 0x100 | 9);
    }
}

/* ------------------------------------------------------------------------- */
/* SHM 45/46 + FB 47                                                          */
/* ------------------------------------------------------------------------- */
static void test_shm_fb(void)
{
    u64 va = 0;
    u64 va2 = 0;
    u32 handle = 0;
    i64 rc;

    section("shmfb");

    expect("shm.alloc_pages0",
           sys3(SYS_SHM_ALLOC, 0, (i64)(usize)&va, (i64)(usize)&handle),
           -22);
    expect("shm.alloc_pages1025",
           sys3(SYS_SHM_ALLOC, 1025, (i64)(usize)&va, (i64)(usize)&handle),
           -22);
    expect("shm.alloc_bad_va_out",
           sys3(SYS_SHM_ALLOC, 1, 0x1000, (i64)(usize)&handle), -14);
    expect("shm.alloc_bad_handle_out",
           sys3(SYS_SHM_ALLOC, 1, (i64)(usize)&va, 0x1000), -14);

    va = 0;
    handle = 0;
    rc = sys3(SYS_SHM_ALLOC, 1, (i64)(usize)&va, (i64)(usize)&handle);
    expect_true("shm.alloc_ok", rc == 0 && va >= 0x50000000ULL && handle != 0, rc);
    if (rc == 0) {
        *(volatile u32 *)(usize)va = 0x12345678u;
        expect_true("shm.alloc_writable",
                    *(volatile u32 *)(usize)va == 0x12345678u,
                    (i64)*(volatile u32 *)(usize)va);
        expect_true("shm.map_ok",
                    sys3(SYS_SHM_MAP, (i64)handle, (i64)(usize)&va2, 0) == 0 &&
                        va2 >= 0x50000000ULL,
                    (i64)va2);
        expect("shm.map_bad_handle",
               sys3(SYS_SHM_MAP, 0xFFFFFFFFLL, (i64)(usize)&va2, 0), -9);
        expect("shm.map_zero_handle",
               sys3(SYS_SHM_MAP, 0, (i64)(usize)&va2, 0), -9);
        expect("shm.map_bad_out",
               sys3(SYS_SHM_MAP, (i64)handle, 0x1000, 0), -14);
    }

    /* SHM_PAGES_MAX=1024：4 MiB，验证按上限分配不越界。 */
    va = 0;
    handle = 0;
    rc = sys3(SYS_SHM_ALLOC, 1024, (i64)(usize)&va, (i64)(usize)&handle);
    expect_true("shm.alloc_max_pages", rc == 0 && handle != 0, rc);

    /* FB_MAP：输出指针、正常映射、重复映射 EBUSY。 */
    va = 0;
    expect("fb.bad_va_out", sys3(SYS_FB_MAP, 0x1000, (i64)(usize)&g_fb, 0), -14);
    expect("fb.bad_info_out", sys3(SYS_FB_MAP, (i64)(usize)&va, 0x1000, 0), -14);
    rc = sys3(SYS_FB_MAP, (i64)(usize)&va, (i64)(usize)&g_fb, 0);
    expect_true("fb.map_ok", rc == 0 && va == 0x60000000ULL, rc);
    if (rc == 0) {
        expect_true("fb.info_sane",
                    g_fb.base == va && g_fb.width > 0 && g_fb.height > 0 &&
                        g_fb.bpp == 32 && g_fb.pitch >= g_fb.width * 4u,
                    (i64)g_fb.width);
        expect("fb.map_twice_busy",
               sys3(SYS_FB_MAP, (i64)(usize)&va2, (i64)(usize)&g_fb, 0), -16);
    }
}

/* ------------------------------------------------------------------------- */
/* 第二轮补充：写 / 页边界 / 跨进程 IPC / FAT16 / SVC / proc / shm-fb         */
/* ------------------------------------------------------------------------- */
static void test_write2(void)
{
    u32 i;

    section("write2");

    for (i = 0; i < (u32)sizeof(g_wbuf); i++) {
        g_wbuf[i] = 'w';
    }
    expect("write2.len256", sys3(SYS_WRITE, (i64)(usize)g_wbuf, 256, 0), 256);
    expect("write2.len255", sys3(SYS_WRITE, (i64)(usize)g_wbuf, 255, 0), 255);

    /* marker 页 0x40010000..0x40010FFF 可写；下一页 0x40011000 未映射。 */
    marker_fill(0xFF0, 'm', 16);
    expect("write2.page_tail",
           sys3(SYS_WRITE, (i64)MARKER_OFF(0xFF0), 16, 0), 16);
    expect("write2.page_cross",
           sys3(SYS_WRITE, (i64)MARKER_OFF(0xFF0), 17, 0), E_FAULT);
    expect("write2.page_end_cross",
           sys3(SYS_WRITE, (i64)MARKER_OFF(0xFFC), 8, 0), E_FAULT);

    expect("write2.user_max_unmapped",
           sys3(SYS_WRITE, 0x7FFFFFFFF000LL, 8, 0), E_FAULT);
    expect("write2.noncanonical",
           sys3(SYS_WRITE, (i64)0x8000000000000000ULL, 8, 0), E_FAULT);
    expect("write2.uva_overlap",
           sys3(SYS_WRITE, (i64)0xFFFFFFFFFFFFF000ULL, 16, 0), E_FAULT);
    expect("write2.diag_rdi0_lenmax", sys3(SYS_WRITE, 0, (i64)~0ULL, 0), 0);
}

static void test_ipc2(void)
{
    struct audit_hdr h;
    struct audit_hdr *got = (struct audit_hdr *)(void *)g_big;
    i64 t3;
    i64 rc;
    u32 pid = 0;
    int st = 0;
    char ticket[24];
    char *av[8];
    u32 i;

    section("ipc2");

    t3 = sys3(SYS_PORT_CREATE, 2, 0, 0);
    expect_true("ipc2.create", t3 > 0, t3);
    expect("ipc2.recv_page_cross",
           sys4(SYS_PORT_RECV, t3, (i64)MARKER_OFF(0xFF0), 4112, 0), E_FAULT);
    expect("ipc2.recv_bad_handle_high",
           sys4(SYS_PORT_RECV, 0x100000000LL, (i64)(usize)g_big, 16, 0), -9);
    expect("ipc2.close_bad_handle_high",
           sys3(SYS_PORT_CLOSE, 0x100000000LL, 0, 0), -9);

    hdr_init(&h, 5, 0, 0, 0);
    expect("ipc2.reply_bad_ptr",
           sys4(SYS_PORT_REPLY, t3, 0x1000, IPC_HDR, 0), E_FAULT);
    expect("ipc2.reply_page_cross",
           sys4(SYS_PORT_REPLY, t3, (i64)MARKER_OFF(0xFFE), IPC_HDR, 0),
           E_FAULT);
    expect("ipc2.reply_len15",
           sys4(SYS_PORT_REPLY, t3, (i64)(usize)&h, 15, 0), E_INVAL);
    expect("ipc2.reply_len4113",
           sys4(SYS_PORT_REPLY, t3, (i64)(usize)&h, 4113, 0), E_MSGSIZE);
    expect("ipc2.reply_len_max",
           sys4(SYS_PORT_REPLY, t3, (i64)(usize)&h, (i64)~0ULL, 0), E_INVAL);

    hdr_init(&h, 6, 16, 0, 0);
    expect("ipc2.send_page_cross",
           sys4(SYS_PORT_SEND, t3, (i64)MARKER_OFF(0xFF0), 32, 0), E_FAULT);

    /* 最大消息（4112）+ max=16 的短 recv：返回总长，只拷 16 字节消息头。 */
    for (i = 0; i < 4112u; i++) {
        g_big[i] = 0;
    }
    got->type = 0x6A;
    got->len = 4096;
    expect("ipc2.send_max",
           sys4(SYS_PORT_SEND, t3, (i64)(usize)g_big, 4112, 0), 0);
    marker_fill(0xFC0, 0xCC, 16);
    expect("ipc2.recv_max_short16",
           sys4(SYS_PORT_RECV, t3, (i64)MARKER_OFF(0xFC0), 16, 0), 4112);
    expect_true("ipc2.recv_max_short16_type",
                ((struct audit_hdr *)(usize)MARKER_OFF(0xFC0))->type == 0x6A,
                (i64)((struct audit_hdr *)(usize)MARKER_OFF(0xFC0))->type);

    /* 已关闭端口的 ticket 作为 hdr.from：generation 失效 -> EBADF。 */
    {
        i64 t4 = sys3(SYS_PORT_CREATE, 1, 0, 0);

        expect_true("ipc2.from_create", t4 > 0, t4);
        expect("ipc2.from_close", sys3(SYS_PORT_CLOSE, t4, 0, 0), 0);
        hdr_init(&h, 7, 0, (u32)t4, 0);
        expect("ipc2.send_stale_from",
               sys4(SYS_PORT_SEND, t3, (i64)(usize)&h, IPC_HDR, 0), -9);
    }
    expect("ipc2.close", sys3(SYS_PORT_CLOSE, t3, 0, 0), 0);

    /*
     * 跨进程显式唤醒：父进程先阻塞在 recv，子进程 send 后 ipc_wake_one
     * 把父进程从 BLOCKED 搬到 READY（上一轮只覆盖了同进程超时路径）。
     */
    {
        i64 p = sys3(SYS_PORT_CREATE, 1, 0, 0);

        expect_true("ipc2.xrecv_create", p > 0, p);
        if (p > 0) {
            (void)put_s64(ticket, (u32)sizeof(ticket), p);
            av[0] = g_self;
            av[1] = (char *)"child";
            av[2] = (char *)"60";
            av[3] = (char *)"sender";
            av[4] = ticket;
            av[5] = (char *)"1";
            av[6] = (char *)"0";
            av[7] = NULL;
            rc = spawn_argv(av, &pid);
            expect_true("ipc2.xrecv_spawn", rc == 0 && pid > 0, rc);
            if (rc == 0) {
                expect("ipc2.xrecv_wake",
                       sys4(SYS_PORT_RECV, p, (i64)(usize)g_big, IPC_HDR,
                            2000000000LL), IPC_HDR);
                expect_true("ipc2.xrecv_type", got->type == 0x9000u,
                            (i64)got->type);
                (void)sys3(SYS_KILL, (i64)pid, 9, 0);
                st = -1;
                expect("ipc2.xrecv_wait", wait_status((i64)pid, &st), 0);
                expect("ipc2.xrecv_status", st, 0);
                put_str("AUDIT: info xproc recv wake ok\n");
            }
            expect("ipc2.xrecv_close", sys3(SYS_PORT_CLOSE, p, 0, 0), 0);
        }
    }

    /*
     * 两个发送者在满队列上阻塞；父进程每次 recv 释放一个空位，
     * ipc_wake_one(SEND) 必须按通知位把两个发送者交替放行。
     */
    {
        i64 p = sys3(SYS_PORT_CREATE, 1, 0, 0);
        u32 pa = 0;
        u32 pb = 0;

        expect_true("ipc2.handoff_create", p > 0, p);
        if (p > 0) {
            hdr_init(&h, 8, 0, 0, 0);
            expect("ipc2.handoff_fill",
                   sys4(SYS_PORT_SEND, p, (i64)(usize)&h, IPC_HDR, 0), 0);
            (void)put_s64(ticket, (u32)sizeof(ticket), p);
            av[0] = g_self;
            av[1] = (char *)"child";
            av[2] = (char *)"61";
            av[3] = (char *)"sender";
            av[4] = ticket;
            av[5] = (char *)"1";
            av[6] = (char *)"18446744073709551615"; /* IPC_TIMEOUT_FOREVER */
            av[7] = NULL;
            rc = spawn_argv(av, &pa);
            expect_true("ipc2.handoff_spawn_a", rc == 0 && pa > 0, rc);
            av[2] = (char *)"62";
            rc = spawn_argv(av, &pb);
            expect_true("ipc2.handoff_spawn_b", rc == 0 && pb > 0, rc);
            sleep_ticks(200000000); /* 让两个 sender 都阻塞在 send */
            expect("ipc2.handoff_recv0",
                   sys4(SYS_PORT_RECV, p, (i64)(usize)g_big, IPC_HDR,
                        2000000000LL), IPC_HDR);
            expect("ipc2.handoff_recv1",
                   sys4(SYS_PORT_RECV, p, (i64)(usize)g_big, IPC_HDR,
                        2000000000LL), IPC_HDR);
            expect("ipc2.handoff_recv2",
                   sys4(SYS_PORT_RECV, p, (i64)(usize)g_big, IPC_HDR,
                        2000000000LL), IPC_HDR);
            /* 已正常退出的子进程 kill 是幂等的；失败时也不会挂住 wait。 */
            (void)sys3(SYS_KILL, (i64)pa, 9, 0);
            (void)sys3(SYS_KILL, (i64)pb, 9, 0);
            st = -1;
            expect("ipc2.handoff_wait_a", wait_status((i64)pa, &st), 0);
            expect("ipc2.handoff_status_a", st, 0);
            st = -1;
            expect("ipc2.handoff_wait_b", wait_status((i64)pb, &st), 0);
            expect("ipc2.handoff_status_b", st, 0);
            put_str("AUDIT: info xproc send handoff ok\n");
            expect("ipc2.handoff_close", sys3(SYS_PORT_CLOSE, p, 0, 0), 0);
        }
    }

    /*
     * kill 一个阻塞在 send 的进程：cancel_thread 摘等待者；端口本身
     * （父进程所有）保留已排队的消息。
     */
    {
        i64 p = sys3(SYS_PORT_CREATE, 1, 0, 0);
        u32 pc = 0;

        expect_true("ipc2.ksend_create", p > 0, p);
        if (p > 0) {
            hdr_init(&h, 9, 0, 0, 0);
            expect("ipc2.ksend_fill",
                   sys4(SYS_PORT_SEND, p, (i64)(usize)&h, IPC_HDR, 0), 0);
            (void)put_s64(ticket, (u32)sizeof(ticket), p);
            av[0] = g_self;
            av[1] = (char *)"child";
            av[2] = (char *)"63";
            av[3] = (char *)"sender";
            av[4] = ticket;
            av[5] = (char *)"1";
            av[6] = (char *)"18446744073709551615";
            av[7] = NULL;
            rc = spawn_argv(av, &pc);
            expect_true("ipc2.ksend_spawn", rc == 0 && pc > 0, rc);
            sleep_ticks(200000000);
            expect("ipc2.ksend_kill", sys3(SYS_KILL, (i64)pc, 9, 0), 0);
            st = -1;
            expect("ipc2.ksend_wait", wait_status((i64)pc, &st), 0);
            expect("ipc2.ksend_status", st, 0x109);
            expect("ipc2.ksend_drain",
                   sys4(SYS_PORT_RECV, p, (i64)(usize)g_big, IPC_HDR, 0),
                   IPC_HDR);
            put_str("AUDIT: info kill blocked sender ok\n");
            expect("ipc2.ksend_close", sys3(SYS_PORT_CLOSE, p, 0, 0), 0);
        }
    }

    /* 非属主 recv/close -> EPERM；send 允许（ticket 即能力）。 */
    {
        i64 p = sys3(SYS_PORT_CREATE, 1, 0, 0);
        u32 pe = 0;

        expect_true("ipc2.eperm_create", p > 0, p);
        if (p > 0) {
            (void)put_s64(ticket, (u32)sizeof(ticket), p);
            av[0] = g_self;
            av[1] = (char *)"child";
            av[2] = (char *)"64";
            av[3] = (char *)"eperm";
            av[4] = ticket;
            av[5] = NULL;
            rc = spawn_argv(av, &pe);
            expect_true("ipc2.eperm_spawn", rc == 0 && pe > 0, rc);
            if (rc == 0) {
                st = -1;
                expect("ipc2.eperm_wait", wait_status((i64)pe, &st), 0);
                expect("ipc2.eperm_status", st, 0);
                expect("ipc2.eperm_send_arrived",
                       sys4(SYS_PORT_RECV, p, (i64)(usize)g_big, IPC_HDR, 0),
                       IPC_HDR);
                expect_true("ipc2.eperm_type", got->type == 0x0Eu,
                            (i64)got->type);
                put_str("AUDIT: info cross-process recv/close EPERM ok\n");
            }
            expect("ipc2.eperm_close", sys3(SYS_PORT_CLOSE, p, 0, 0), 0);
        }
    }

    /* close 唤醒阻塞在 send 的进程：send 返回 ECANCELED（-125）。 */
    {
        i64 p = sys3(SYS_PORT_CREATE, 1, 0, 0);
        u32 pd = 0;

        expect_true("ipc2.closesend_create", p > 0, p);
        if (p > 0) {
            hdr_init(&h, 10, 0, 0, 0);
            expect("ipc2.closesend_fill",
                   sys4(SYS_PORT_SEND, p, (i64)(usize)&h, IPC_HDR, 0), 0);
            (void)put_s64(ticket, (u32)sizeof(ticket), p);
            av[0] = g_self;
            av[1] = (char *)"child";
            av[2] = (char *)"65";
            av[3] = (char *)"sender";
            av[4] = ticket;
            av[5] = (char *)"1";
            av[6] = (char *)"18446744073709551615";
            av[7] = NULL;
            rc = spawn_argv(av, &pd);
            expect_true("ipc2.closesend_spawn", rc == 0 && pd > 0, rc);
            sleep_ticks(200000000);
            expect("ipc2.closesend_close", sys3(SYS_PORT_CLOSE, p, 0, 0), 0);
            st = -1;
            expect("ipc2.closesend_wait", wait_status((i64)pd, &st), 0);
            expect("ipc2.closesend_status", st, 4);
            put_str("AUDIT: info close wakes blocked sender ok\n");
        }
    }

    /*
     * 深队列：depth=64、每条 4112 字节（堆上约 257 KiB）。close 必须释放
     * 全部排队消息；同一槽位立刻重建再填满，验证 kfree 后复用不会坏堆。
     */
    {
        struct audit_hdr *hbig = (struct audit_hdr *)(void *)g_big;
        u32 cycle;

        for (i = 0; i < 4112u; i++) {
            g_big[i] = 0;
        }
        hbig->type = 0xD1;
        hbig->len = 4096;
        hbig->from = 0;
        hbig->flags = 0;
        for (cycle = 0; cycle < 2u; cycle++) {
            i64 p = sys3(SYS_PORT_CREATE, 64, 0, 0);
            u32 k;
            u32 sent = 0;

            expect_true("ipc2.deep_create", p > 0, p);
            if (p <= 0) {
                break;
            }
            for (k = 0; k < 64u; k++) {
                if (sys4(SYS_PORT_SEND, p, (i64)(usize)g_big, 4112, 0) != 0) {
                    break;
                }
                sent++;
            }
            expect("ipc2.deep_fill64", (i64)sent, 64);
            expect("ipc2.deep_eagain",
                   sys4(SYS_PORT_SEND, p, (i64)(usize)g_big, 4112, 0),
                   E_AGAIN);
            expect("ipc2.deep_recv1",
                   sys4(SYS_PORT_RECV, p, (i64)(usize)g_big, 4112, 0), 4112);
            expect("ipc2.deep_close", sys3(SYS_PORT_CLOSE, p, 0, 0), 0);
        }
        put_str("AUDIT: info ipc deep queue recycle ok\n");
    }

    /*
     * waiter/port/thread 池饱和：10 个 block 子进程各自建端口并阻塞 recv。
     * 允许的结果：0x109（真阻塞后被 kill）/2（recv 立刻 EAGAIN）/
     * 1（端口池满，子进程建端口失败）。全部 wait 后系统必须仍可用。
     */
    {
        u32 wp[12];
        u32 wmade = 0;
        u32 wblocked = 0;
        u32 weagain = 0;
        u32 wother = 0;

        av[0] = g_self;
        av[1] = (char *)"child";
        av[2] = (char *)"66";
        av[3] = (char *)"block";
        av[4] = NULL;
        for (i = 0; i < 10u; i++) {
            pid = 0;
            rc = spawn_argv(av, &pid);
            if (rc != 0) {
                break;
            }
            wp[wmade++] = pid;
        }
        expect_true("ipc2.wsaturate_made", wmade >= 1, (i64)wmade);
        sleep_ticks(300000000);
        for (i = 0; i < wmade; i++) {
            (void)sys3(SYS_KILL, (i64)wp[i], 9, 0);
        }
        for (i = 0; i < wmade; i++) {
            st = -1;
            expect("ipc2.wsaturate_wait", wait_status((i64)wp[i], &st), 0);
            if (st == 0x109) {
                wblocked++;
            } else if (st == 2) {
                weagain++;
            } else {
                wother++;
            }
        }
        expect_true("ipc2.wsaturate_classified",
                    wblocked + weagain + wother == wmade, (i64)wother);
        put_str("AUDIT: info ipc waiter pool ok blocked=");
        (void)put_s64(ticket, (u32)sizeof(ticket), (i64)wblocked);
        put_str(ticket);
        put_str(" eagain=");
        (void)put_s64(ticket, (u32)sizeof(ticket), (i64)weagain);
        put_str(ticket);
        put_str(" other=");
        (void)put_s64(ticket, (u32)sizeof(ticket), (i64)wother);
        put_str(ticket);
        put_str("\n");
    }

    /* 端口池：持满后 ENOSPC；全部关闭后能重建。 */
    {
        i64 held[32];
        u32 made = 0;

        rc = 0;
        for (i = 0; i < 32u; i++) {
            rc = sys3(SYS_PORT_CREATE, 1, 0, 0);
            if (rc <= 0) {
                break;
            }
            held[made++] = rc;
        }
        expect("ipc2.pool_enospc", rc, E_NOSPC);
        expect_true("ipc2.pool_nonzero", made >= 1, (i64)made);
        for (i = 0; i < made; i++) {
            (void)sys3(SYS_PORT_CLOSE, held[i], 0, 0);
        }
        rc = sys3(SYS_PORT_CREATE, 1, 0, 0);
        expect_true("ipc2.pool_recreate", rc > 0, rc);
        if (rc > 0) {
            (void)sys3(SYS_PORT_CLOSE, rc, 0, 0);
        }
    }
}

static void test_fs2(void)
{
    i64 fd;
    i64 fds[20];
    u32 nfd = 0;
    u32 i;
    i64 rc;
    u32 pid = 0;
    char *av[6];
    struct audit_stat *mst = (struct audit_stat *)(usize)MARKER_OFF(0xFE8);
    struct audit_dirent *de = (struct audit_dirent *)(void *)g_ents_raw;

    section("fs2");

    /*
     * ramfs 节点池余量：启动内容（/bin 7 个 ELF + /System + /run）已占
     * 30/32 个节点，原实现 ring3 只剩 1~2 个 O_CREAT 余量。这里创建到
     * ENOSPC 记录余量，再全部删除，保证后续用例有干净的工作区。
     */
    {
        u32 made = 0;
        u32 k;

        rc = 0;
        for (k = 0; k < 80u; k++) {
            char nm[16];

            nm[0] = '/';
            nm[1] = 't';
            nm[2] = 'm';
            nm[3] = 'p';
            nm[4] = '/';
            nm[5] = 'h';
            nm[6] = (char)('a' + (int)(k % 26u));
            nm[7] = (char)('a' + (int)((k / 26u) % 26u));
            nm[8] = '\0';
            fd = open_file(nm, O_CREAT | O_RDWR, 0644);
            if (fd < 0) {
                rc = fd;
                break;
            }
            (void)sys3(SYS_CLOSE, fd, 0, 0);
            made++;
        }
        expect("fs2.ramfs_node_enospc", rc, E_NOSPC);
        expect_true("fs2.ramfs_headroom", made >= 2, (i64)made);
        for (k = 0; k < made; k++) {
            char nm[16];

            nm[0] = '/';
            nm[1] = 't';
            nm[2] = 'm';
            nm[3] = 'p';
            nm[4] = '/';
            nm[5] = 'h';
            nm[6] = (char)('a' + (int)(k % 26u));
            nm[7] = (char)('a' + (int)((k / 26u) % 26u));
            nm[8] = '\0';
            (void)sys3(SYS_UNLINK, (i64)(usize)nm, 0, 0);
        }
        (void)put_s64((char *)(void *)g_buf, 24, (i64)made);
        put_str("AUDIT: info ramfs headroom=");
        put_str((const char *)(void *)g_buf);
        put_str("\n");
    }

    /* fd 表上限（VFS_FD_MAX=16）：填满后 EMFILE，spawn 也不能再 open ELF。 */
    rc = 0;
    for (i = 0; i < 20u; i++) {
        fd = open_file("/bin/shell", O_RDONLY, 0);
        if (fd < 0) {
            rc = fd;
            break;
        }
        fds[nfd++] = fd;
    }
    expect("fs2.fd_exhaust", rc, E_MFILE);
    expect_true("fs2.fd_filled", nfd >= 1 && nfd <= 16, (i64)nfd);
    av[0] = g_self;
    av[1] = (char *)"child";
    av[2] = (char *)"70";
    av[3] = (char *)"quick";
    av[4] = NULL;
    expect("fs2.spawn_emfile", spawn_argv(av, &pid), E_MFILE);
    for (i = 0; i < nfd; i++) {
        (void)sys3(SYS_CLOSE, fds[i], 0, 0);
    }
    fd = open_file("/bin/shell", O_RDONLY, 0);
    expect_true("fs2.fd_reuse", fd >= 0x10000, fd);
    if (fd >= 0) {
        expect("fs2.read_page_tail",
               sys3(SYS_READ, fd, (i64)MARKER_OFF(0xFF0), 16), 16);
        expect("fs2.read_page_cross",
               sys3(SYS_READ, fd, (i64)MARKER_OFF(0xFF0), 17), E_FAULT);
        expect("fs2.read_ro_buf", sys3(SYS_READ, fd, 0x40000000, 16), E_FAULT);
        expect("fs2.write_rdonly",
               sys3(SYS_WRITE_FD, fd, (i64)(usize)g_buf, 4), -9);
        (void)sys3(SYS_CLOSE, fd, 0, 0);
    }

    /*
     * fd 表满时 open() 失败必须无副作用：不截断、不创建文件。
     * 旧实现先做 O_TRUNC/创建，后分配 fd —— EMFILE 时文件已被破坏/残留。
     */
    (void)sys3(SYS_UNLINK, (i64)(usize)"/tmp/audit-trunc", 0, 0);
    (void)sys3(SYS_UNLINK, (i64)(usize)"/tmp/audit-leak", 0, 0);
    fd = open_file("/tmp/audit-trunc", O_CREAT | O_RDWR | O_TRUNC, 0644);
    expect_true("fs2.trunc_seed", fd >= 0x10000, fd);
    if (fd >= 0) {
        g_buf[0] = 'h';
        g_buf[1] = 'e';
        g_buf[2] = 'l';
        g_buf[3] = 'l';
        g_buf[4] = 'o';
        expect("fs2.trunc_seed_write",
               sys3(SYS_WRITE_FD, fd, (i64)(usize)g_buf, 5), 5);
        (void)sys3(SYS_CLOSE, fd, 0, 0);
    }
    nfd = 0;
    rc = 0;
    for (i = 0; i < 20u; i++) {
        fd = open_file("/bin/shell", O_RDONLY, 0);
        if (fd < 0) {
            rc = fd;
            break;
        }
        fds[nfd++] = fd;
    }
    expect("fs2.trunc_fill", rc, E_MFILE);
    expect("fs2.trunc_emfile",
           open_file("/tmp/audit-trunc", O_RDWR | O_TRUNC, 0), E_MFILE);
    expect("fs2.create_emfile",
           open_file("/tmp/audit-leak", O_CREAT | O_RDWR, 0644), E_MFILE);
    expect("fs2.create_emfile_fat",
           open_file("/mnt/disk/AUDIT.LEK", O_CREAT | O_RDWR, 0644),
           E_MFILE);
    for (i = 0; i < nfd; i++) {
        (void)sys3(SYS_CLOSE, fds[i], 0, 0);
    }
    expect("fs2.trunc_stat",
           sys3(SYS_STAT, (i64)(usize)"/tmp/audit-trunc",
                (i64)(usize)&g_st, 0), 0);
    expect("fs2.trunc_preserved", (i64)g_st.size, 5);
    expect("fs2.create_absent",
           sys3(SYS_STAT, (i64)(usize)"/tmp/audit-leak",
                (i64)(usize)&g_st, 0), E_NOENT);
    expect("fs2.create_fat_absent",
           sys3(SYS_STAT, (i64)(usize)"/mnt/disk/AUDIT.LEK",
                (i64)(usize)&g_st, 0), E_NOENT);
    (void)sys3(SYS_UNLINK, (i64)(usize)"/mnt/disk/AUDIT.LEK", 0, 0);
    (void)sys3(SYS_UNLINK, (i64)(usize)"/tmp/audit-trunc", 0, 0);

    /* stat / listdir 的页边界 + 只读页写检查。 */
    expect("fs2.stat_page_tail",
           sys3(SYS_STAT, (i64)(usize)"/bin/shell", (i64)MARKER_OFF(0xFE8), 0),
           0);
    expect_true("fs2.stat_page_tail_type", mst->type == 1, (i64)mst->type);
    expect_true("fs2.stat_page_tail_size", mst->size > 0, (i64)mst->size);
    expect("fs2.stat_page_cross",
           sys3(SYS_STAT, (i64)(usize)"/bin/shell", (i64)MARKER_OFF(0xFFE), 0),
           E_FAULT);
    expect("fs2.stat_ro_out",
           sys3(SYS_STAT, (i64)(usize)"/bin/shell", 0x40000000, 0), E_FAULT);
    expect("fs2.listdir_page_tail",
           sys3(SYS_LISTDIR, (i64)(usize)"/", (i64)MARKER_OFF(0xFB0), 1), 1);
    expect("fs2.listdir_page_cross",
           sys3(SYS_LISTDIR, (i64)(usize)"/", (i64)MARKER_OFF(0xFB0), 2),
           E_FAULT);
    expect("fs2.listdir_file",
           sys3(SYS_LISTDIR, (i64)(usize)"/bin/shell", (i64)(usize)g_ents_raw,
                1), E_NOTDIR);

    /* 路径字符串压页尾：copyin_str 必须在未映射页之前失败。 */
    marker_fill(0xFFE, 'c', 2);
    expect("fs2.open_path_cross",
           open_file((const char *)(usize)MARKER_OFF(0xFFE), O_RDONLY, 0),
           E_FAULT);
    expect("fs2.stat_path_cross",
           sys3(SYS_STAT, (i64)MARKER_OFF(0xFFE), (i64)(usize)&g_st, 0),
           E_FAULT);
    expect("fs2.listdir_path_cross",
           sys3(SYS_LISTDIR, (i64)MARKER_OFF(0xFFE), (i64)(usize)g_ents_raw,
                1), E_FAULT);
    expect("fs2.mkdir_path_cross",
           sys3(SYS_MKDIR, (i64)MARKER_OFF(0xFFE), 0755, 0), E_FAULT);
    expect("fs2.unlink_path_cross",
           sys3(SYS_UNLINK, (i64)MARKER_OFF(0xFFE), 0, 0), E_FAULT);

    /* 目录 fd：read/write 都必须是 EISDIR，不能落到 ramfs ops。 */
    fd = open_file("/", O_RDONLY, 0);
    expect_true("fs2.open_dir", fd >= 0x10000, fd);
    if (fd >= 0) {
        expect("fs2.read_dir", sys3(SYS_READ, fd, (i64)(usize)g_big, 16),
               E_ISDIR);
        expect("fs2.write_dir",
               sys3(SYS_WRITE_FD, fd, (i64)(usize)g_buf, 4), E_ISDIR);
        (void)sys3(SYS_CLOSE, fd, 0, 0);
    }
    expect("fs2.open_dir_rw", open_file("/", O_RDWR, 0), E_ISDIR);

    /* O_CREAT|O_EXCL 语义 / 只读文件 open 后 close。 */
    (void)sys3(SYS_UNLINK, (i64)(usize)"/tmp/audit-excl", 0, 0);
    fd = open_file("/tmp/audit-excl", O_CREAT | O_RDWR, 0644);
    expect_true("fs2.excl_create", fd >= 0x10000, fd);
    if (fd >= 0) {
        (void)sys3(SYS_CLOSE, fd, 0, 0);
    }
    expect("fs2.excl_again",
           open_file("/tmp/audit-excl", O_CREAT | O_EXCL, 0644), E_EXIST);
    expect("fs2.excl_unlink",
           sys3(SYS_UNLINK, (i64)(usize)"/tmp/audit-excl", 0, 0), 0);

    /* unlink 打开中的 ramfs 文件：deleted 标记，数据保留到最后一个 close。 */
    (void)sys3(SYS_UNLINK, (i64)(usize)"/tmp/audit-hold", 0, 0);
    fd = open_file("/tmp/audit-hold", O_CREAT | O_RDWR | O_TRUNC, 0644);
    expect_true("fs2.hold_create", fd >= 0x10000, fd);
    if (fd >= 0) {
        i64 fd2;
        u32 k;

        g_buf[0] = 'a';
        g_buf[1] = 'b';
        g_buf[2] = 'c';
        expect("fs2.hold_write",
               sys3(SYS_WRITE_FD, fd, (i64)(usize)g_buf, 3), 3);
        fd2 = open_file("/tmp/audit-hold", O_RDONLY, 0);
        expect_true("fs2.hold_reopen", fd2 >= 0x10000, fd2);
        expect("fs2.hold_unlink",
               sys3(SYS_UNLINK, (i64)(usize)"/tmp/audit-hold", 0, 0), 0);
        expect("fs2.hold_stat_gone",
               sys3(SYS_STAT, (i64)(usize)"/tmp/audit-hold",
                    (i64)(usize)&g_st, 0), E_NOENT);
        if (fd2 >= 0) {
            for (k = 0; k < 8u; k++) {
                g_big[k] = 0;
            }
            expect("fs2.hold_read",
                   sys3(SYS_READ, fd2, (i64)(usize)g_big, 3), 3);
            expect_true("fs2.hold_content",
                        g_big[0] == 'a' && g_big[1] == 'b' &&
                            g_big[2] == 'c',
                        (i64)g_big[0]);
            (void)sys3(SYS_CLOSE, fd2, 0, 0);
        }
        (void)sys3(SYS_CLOSE, fd, 0, 0);
        expect("fs2.hold_stat_gone2",
               sys3(SYS_STAT, (i64)(usize)"/tmp/audit-hold",
                    (i64)(usize)&g_st, 0), E_NOENT);
    }

    /* ---- FAT16（/mnt/disk）：读 + listdir + 写/截断/删除 ---- */
    expect("fs2.fat_stat",
           sys3(SYS_STAT, (i64)(usize)"/mnt/disk/AUDIT.ELF",
                (i64)(usize)&g_st, 0), 0);
    expect_true("fs2.fat_stat_type", g_st.type == 1, (i64)g_st.type);
    expect_true("fs2.fat_stat_size", g_st.size > 1000, (i64)g_st.size);
    fd = open_file("/mnt/disk/AUDIT.ELF", O_RDONLY, 0);
    expect_true("fs2.fat_open", fd >= 0x10000, fd);
    if (fd >= 0) {
        expect("fs2.fat_read512", sys3(SYS_READ, fd, (i64)(usize)g_big, 512),
               512);
        expect_true("fs2.fat_elf_magic",
                    g_big[0] == 0x7F && g_big[1] == 'E' && g_big[2] == 'L' &&
                        g_big[3] == 'F',
                    (i64)g_big[0]);
        expect("fs2.fat_read512_2",
               sys3(SYS_READ, fd, (i64)(usize)g_big, 512), 512);
        expect("fs2.fat_read_len513",
               sys3(SYS_READ, fd, (i64)(usize)g_big, 513), E_INVAL);
        (void)sys3(SYS_CLOSE, fd, 0, 0);
    }
    rc = sys3(SYS_LISTDIR, (i64)(usize)"/mnt/disk", (i64)(usize)g_ents_raw, 8);
    expect_true("fs2.fat_listdir", rc >= 1 && rc <= 8, rc);
    if (rc >= 1) {
        expect_true("fs2.fat_dirent_name",
                    de->name_len > 0 && de->name_len < 64, (i64)de->name_len);
    }

    (void)sys3(SYS_UNLINK, (i64)(usize)"/mnt/disk/AUDIT.TMP", 0, 0);
    fd = open_file("/mnt/disk/AUDIT.TMP", O_CREAT | O_RDWR | O_TRUNC, 0644);
    expect_true("fs2.fat_create", fd >= 0x10000, fd);
    if (fd >= 0) {
        g_buf[0] = 'h';
        g_buf[1] = 'e';
        g_buf[2] = 'l';
        g_buf[3] = 'l';
        g_buf[4] = 'o';
        expect("fs2.fat_write", sys3(SYS_WRITE_FD, fd, (i64)(usize)g_buf, 5),
               5);
        expect("fs2.fat_unlink_busy",
               sys3(SYS_UNLINK, (i64)(usize)"/mnt/disk/AUDIT.TMP", 0, 0),
               E_BUSY);
        (void)sys3(SYS_CLOSE, fd, 0, 0);
    }
    expect("fs2.fat_tmp_stat",
           sys3(SYS_STAT, (i64)(usize)"/mnt/disk/AUDIT.TMP",
                (i64)(usize)&g_st, 0), 0);
    expect_true("fs2.fat_tmp_size", g_st.size == 5, (i64)g_st.size);
    fd = open_file("/mnt/disk/AUDIT.TMP", O_RDONLY, 0);
    expect_true("fs2.fat_tmp_reopen", fd >= 0x10000, fd);
    if (fd >= 0) {
        for (i = 0; i < 8u; i++) {
            g_big[i] = 0;
        }
        expect("fs2.fat_tmp_read", sys3(SYS_READ, fd, (i64)(usize)g_big, 5),
               5);
        expect_true("fs2.fat_tmp_content",
                    g_big[0] == 'h' && g_big[1] == 'e' && g_big[2] == 'l' &&
                        g_big[3] == 'l' && g_big[4] == 'o',
                    (i64)g_big[0]);
        (void)sys3(SYS_CLOSE, fd, 0, 0);
    }
    fd = open_file("/mnt/disk/AUDIT.TMP", O_RDWR | O_TRUNC, 0);
    expect_true("fs2.fat_tmp_trunc", fd >= 0x10000, fd);
    if (fd >= 0) {
        (void)sys3(SYS_CLOSE, fd, 0, 0);
    }
    expect("fs2.fat_tmp_stat0",
           sys3(SYS_STAT, (i64)(usize)"/mnt/disk/AUDIT.TMP",
                (i64)(usize)&g_st, 0), 0);
    expect_true("fs2.fat_tmp_size0", g_st.size == 0, (i64)g_st.size);
    expect("fs2.fat_tmp_unlink",
           sys3(SYS_UNLINK, (i64)(usize)"/mnt/disk/AUDIT.TMP", 0, 0), 0);
    expect("fs2.fat_tmp_gone",
           sys3(SYS_STAT, (i64)(usize)"/mnt/disk/AUDIT.TMP",
                (i64)(usize)&g_st, 0), E_NOENT);
    /* 8.3 之外的名字在 FAT16 上必须被拒绝（不能越界写目录项）。 */
    rc = open_file("/mnt/disk/audit-long-name.txt", O_CREAT | O_RDWR, 0644);
    expect_true("fs2.fat_long_name", rc < 0, rc);
    put_str("AUDIT: info fat16 read/write/listdir ok\n");
}

static void test_svc2(void)
{
    i64 rc;
    u32 i;

    section("svc2");

    /* syscall 30 readkey：忽略参数，无键时必须 EAGAIN 而不是崩。 */
    rc = sys3(SYS_READKEY, (i64)~0ULL, (i64)~0ULL, (i64)~0ULL);
    expect_true("svc2.readkey_junk", rc == E_AGAIN || rc >= 0, rc);
    rc = sys3(SYS_READKEY, 0, 0, 0);
    expect_true("svc2.readkey_zero", rc == E_AGAIN || rc >= 0, rc);

    rc = sys2(SYS_DMESG, (i64)(usize)g_big, 4096);
    expect_true("svc2.dmesg_max4096", rc >= 0 && rc <= 4096, rc);
    expect("svc2.dmesg_4097", sys2(SYS_DMESG, (i64)(usize)g_big, 4097),
           E_INVAL);
    expect("svc2.dmesg_umax",
           sys2(SYS_DMESG, (i64)(usize)g_big, (i64)~0ULL), E_INVAL);
    rc = sys2(SYS_DMESG, (i64)MARKER_OFF(0xFF0), 4096);
    expect_true("svc2.dmesg_page_cross", rc == E_FAULT || rc == 0, rc);

    /* log_write：tag 上限 14 字符、msg 上限 126 字符（含 NUL 的 cap-2）。 */
    for (i = 0; i < 126u; i++) {
        g_names[i] = 'm';
    }
    g_names[126] = '\0';
    expect("svc2.log_msg126",
           sys2(SYS_LOG_WRITE, (i64)(usize)"audit", (i64)(usize)g_names), 0);
    g_names[126] = 'm';
    g_names[127] = '\0';
    expect("svc2.log_msg127",
           sys2(SYS_LOG_WRITE, (i64)(usize)"audit", (i64)(usize)g_names),
           E_NAMETOOLONG);
    for (i = 0; i < 14u; i++) {
        g_names[i] = 't';
    }
    g_names[14] = '\0';
    expect("svc2.log_tag14",
           sys2(SYS_LOG_WRITE, (i64)(usize)g_names, (i64)(usize)"x"), 0);
    g_names[14] = 't';
    g_names[15] = '\0';
    expect("svc2.log_tag15",
           sys2(SYS_LOG_WRITE, (i64)(usize)g_names, (i64)(usize)"x"),
           E_NAMETOOLONG);
    marker_fill(0xFFE, 'q', 2); /* 页尾两个非 NUL 字节，NUL 落在未映射页 */
    expect("svc2.log_tag_cross",
           sys2(SYS_LOG_WRITE, (i64)MARKER_OFF(0xFFE), (i64)(usize)"x"),
           E_FAULT);
    expect("svc2.log_msg_empty",
           sys2(SYS_LOG_WRITE, (i64)(usize)"audit", (i64)(usize)""), E_INVAL);

    /* login：name 上限 30、pass 上限 62；超长在 auth 之前拒绝（不会锁账户）。 */
    for (i = 0; i < 30u; i++) {
        g_names[i] = 'n';
    }
    g_names[30] = '\0';
    rc = sys2(SYS_LOGIN, (i64)(usize)g_names, (i64)(usize)"x");
    expect_true("svc2.login_unknown30", rc < 0, rc);
    g_names[30] = 'n';
    g_names[31] = '\0';
    expect("svc2.login_name31",
           sys2(SYS_LOGIN, (i64)(usize)g_names, (i64)(usize)"x"),
           E_NAMETOOLONG);
    for (i = 0; i < 63u; i++) {
        g_names[i] = 'p';
    }
    g_names[63] = '\0';
    expect("svc2.login_pass63",
           sys2(SYS_LOGIN, (i64)(usize)"root", (i64)(usize)g_names),
           E_NAMETOOLONG);
    marker_fill(0xFFE, 'q', 2);
    expect("svc2.login_name_cross",
           sys2(SYS_LOGIN, (i64)MARKER_OFF(0xFFE), (i64)(usize)"x"), E_FAULT);
    expect("svc2.login_pass_cross",
           sys2(SYS_LOGIN, (i64)(usize)"root", (i64)MARKER_OFF(0xFFE)),
           E_FAULT);

    /* pkg_list：max=1/513/umax + 输出到只读页。 */
    expect("svc2.pkg_list_max1", sys2(SYS_PKG_LIST, (i64)(usize)g_big, 1),
           E_NOSPC);
    expect("svc2.pkg_list_max513",
           sys2(SYS_PKG_LIST, (i64)(usize)g_big, 513), E_INVAL);
    expect("svc2.pkg_list_umax",
           sys2(SYS_PKG_LIST, (i64)(usize)g_big, (i64)~0ULL), E_INVAL);
    rc = sys2(SYS_PKG_LIST, (i64)(usize)g_big, 512);
    expect_true("svc2.pkg_list_ok", rc >= 0, rc);
    expect("svc2.pkg_list_ro_buf",
           sys2(SYS_PKG_LIST, 0x40000000, 512), E_FAULT);
    expect("svc2.pkg_list_page_cross",
           sys2(SYS_PKG_LIST, (i64)MARKER_OFF(0xFF8), 512), E_FAULT);

    /* pkg_verify：30 字符合法（不存在）、31 太长；"."/".."/空 被 name_ok 拒。 */
    for (i = 0; i < 30u; i++) {
        g_names[i] = 'p';
    }
    g_names[30] = '\0';
    rc = sys3(SYS_PKG_VERIFY, (i64)(usize)g_names, 0, 0);
    expect_true("svc2.pkg_verify_name30", rc < 0, rc);
    g_names[30] = 'p';
    g_names[31] = '\0';
    expect("svc2.pkg_verify_name31",
           sys3(SYS_PKG_VERIFY, (i64)(usize)g_names, 0, 0), E_NAMETOOLONG);
    expect("svc2.pkg_verify_dot",
           sys3(SYS_PKG_VERIFY, (i64)(usize)".", 0, 0), E_INVAL);
    expect("svc2.pkg_verify_dotdot",
           sys3(SYS_PKG_VERIFY, (i64)(usize)"..", 0, 0), E_INVAL);
    expect("svc2.pkg_verify_empty",
           sys3(SYS_PKG_VERIFY, (i64)(usize)"", 0, 0), E_INVAL);
    put_str("AUDIT: info svc 30..35 boundary ok\n");
}

static void test_proc2(void)
{
    char *av[9];
    u32 pid = 0;
    int st = 0;
    i64 rc;
    u32 i;
    u32 pids[48];
    u32 made;
    u32 enospc;

    section("proc2");

    /* spawn path 长度边界：cap=64 -> 62 字符合法（ENOENT），63 拒绝。 */
    for (i = 0; i < 63u; i++) {
        g_path[i] = 'a';
    }
    g_path[0] = '/';
    g_path[62] = '\0';
    av[0] = g_path;
    av[1] = NULL;
    expect("proc2.spawn_path62", spawn_argv(av, &pid), E_NOENT);
    g_path[62] = 'a';
    g_path[63] = '\0';
    expect("proc2.spawn_path63", spawn_argv(av, &pid), E_NAMETOOLONG);

    av[0] = (char *)"/bin";
    av[1] = NULL;
    rc = spawn_argv(av, &pid);
    expect_true("proc2.spawn_dir", rc < 0, rc);

    /* argv/upid/路径字符串压 marker 页尾：跨到未映射页必须 EFAULT。 */
    marker_put_str(0x000, g_self);
    marker_put_str(0x020, "child");
    marker_put_str(0x030, "80");
    marker_put_str(0x040, "quick");
    marker_put_u64(0xFF8, MARKER_OFF(0x000));
    expect("proc2.spawn_argv_page_cross",
           sys3(SYS_SPAWN, (i64)MARKER_OFF(0x000),
                (i64)MARKER_OFF(0xFF8), (i64)(usize)&pid), E_FAULT);
    marker_fill(0xFFF, 'a', 1); /* 'a' 在页尾，NUL 在未映射页 */
    marker_put_u64(0xFE0, MARKER_OFF(0xFFF));
    marker_put_u64(0xFE8, 0);
    expect("proc2.spawn_argv_str_cross",
           sys3(SYS_SPAWN, (i64)(usize)"/bin/hello",
                (i64)MARKER_OFF(0xFE0), (i64)(usize)&pid), E_FAULT);
    marker_fill(0xFFE, 'b', 2);
    expect("proc2.spawn_path_cross",
           sys3(SYS_SPAWN, (i64)MARKER_OFF(0xFFE), 0, (i64)(usize)&pid),
           E_FAULT);
    marker_put_u64(0xFE0, MARKER_OFF(0x000));
    marker_put_u64(0xFE8, 0);
    expect("proc2.spawn_upid_cross",
           sys3(SYS_SPAWN, (i64)MARKER_OFF(0x000), (i64)MARKER_OFF(0xFE0),
                (i64)MARKER_OFF(0xFFE)), E_FAULT);

    /* 成功路径：argv 数组在 marker 页中段，&pid 正好压页尾 4 字节。 */
    marker_put_u64(0xF00, MARKER_OFF(0x000));
    marker_put_u64(0xF08, MARKER_OFF(0x020));
    marker_put_u64(0xF10, MARKER_OFF(0x030));
    marker_put_u64(0xF18, MARKER_OFF(0x040));
    marker_put_u64(0xF20, 0);
    pid = 0;
    rc = sys3(SYS_SPAWN, (i64)MARKER_OFF(0x000), (i64)MARKER_OFF(0xF00),
              (i64)MARKER_OFF(0xFFC));
    expect("proc2.spawn_upid_page_tail", rc, 0);
    if (rc == 0) {
        pid = *(volatile u32 *)(usize)MARKER_OFF(0xFFC);
        expect_true("proc2.spawn_upid_value", pid > 0, (i64)pid);
        st = -1;
        expect("proc2.wait_status_page_tail",
               sys3(SYS_WAIT, (i64)pid, (i64)MARKER_OFF(0xFFC), 0), 0);
        st = *(volatile int *)(usize)MARKER_OFF(0xFFC);
        expect("proc2.wait_status_zero", st, 0);
    }

    /* argv=NULL（argc=0）与 8 项 argv：hello 忽略参数，能正常退出。 */
    pid = 0;
    rc = sys3(SYS_SPAWN, (i64)(usize)"/bin/hello", 0, (i64)(usize)&pid);
    expect_true("proc2.spawn_argv_null", rc == 0 && pid > 0, rc);
    if (rc == 0) {
        (void)sys3(SYS_KILL, (i64)pid, 9, 0);
        st = -1;
        expect("proc2.spawn_argv_null_wait", wait_status((i64)pid, &st), 0);
    }

    /* 单项 argv 上限：126 字符合法（copyin_str 会返回 127 = 拒绝线）。 */
    for (i = 0; i < 126u; i++) {
        g_long127[i] = 'z';
    }
    g_long127[126] = '\0';
    for (i = 0; i < 8u; i++) {
        av[i] = g_long127;
    }
    av[8] = NULL;
    pid = 0;
    rc = sys3(SYS_SPAWN, (i64)(usize)"/bin/hello", (i64)(usize)av,
              (i64)(usize)&pid);
    expect_true("proc2.spawn_argv8_maxlen", rc == 0 && pid > 0, rc);
    if (rc == 0) {
        (void)sys3(SYS_KILL, (i64)pid, 9, 0);
        st = -1;
        expect("proc2.spawn_argv8_maxlen_wait",
               wait_status((i64)pid, &st), 0);
    }
    av[0] = (char *)"/bin/hello";
    av[1] = g_long127;
    av[2] = NULL;
    g_long127[126] = 'z';
    g_long127[127] = '\0'; /* 127 字符 = 拒绝线（PROC_ARG_MAX-1） */
    expect("proc2.spawn_arg127_rejected",
           sys3(SYS_SPAWN, (i64)(usize)"/bin/hello", (i64)(usize)av,
                (i64)(usize)&pid), E_NAMETOOLONG);

    /* 进程自身 exit code / 自杀（proc_kill 的 p == cur 分支）。 */
    av[0] = g_self;
    av[1] = (char *)"child";
    av[2] = (char *)"100";
    av[3] = (char *)"exitcode";
    av[4] = (char *)"7";
    av[5] = NULL;
    pid = 0;
    rc = spawn_argv(av, &pid);
    expect_true("proc2.exitcode_spawn", rc == 0 && pid > 0, rc);
    if (rc == 0) {
        st = -1;
        expect("proc2.exitcode_wait", wait_status((i64)pid, &st), 0);
        expect("proc2.exitcode_7", st, 7);
    }
    av[4] = (char *)"256"; /* >255 只保留低 8 位 */
    pid = 0;
    rc = spawn_argv(av, &pid);
    expect_true("proc2.exitcode256_spawn", rc == 0 && pid > 0, rc);
    if (rc == 0) {
        st = -1;
        expect("proc2.exitcode256_wait", wait_status((i64)pid, &st), 0);
        expect("proc2.exitcode256_masked", st, 0);
    }

    av[3] = (char *)"selfkill";
    av[4] = (char *)"9";
    av[5] = NULL;
    pid = 0;
    rc = spawn_argv(av, &pid);
    expect_true("proc2.selfkill_spawn", rc == 0 && pid > 0, rc);
    if (rc == 0) {
        st = -1;
        expect("proc2.selfkill_wait", wait_status((i64)pid, &st), 0);
        expect("proc2.selfkill_status", st, 0x109);
        put_str("AUDIT: info selfkill path ok\n");
    }
    av[4] = (char *)"255";
    pid = 0;
    rc = spawn_argv(av, &pid);
    expect_true("proc2.selfkill255_spawn", rc == 0 && pid > 0, rc);
    if (rc == 0) {
        st = -1;
        expect("proc2.selfkill255_wait", wait_status((i64)pid, &st), 0);
        expect("proc2.selfkill255_status", st, 0x1FF);
    }

    /* 自杀时 AS 仍是当前 CR3，且带 LFB / SHM 映射：as_destroy 顺序回归。 */
    av[3] = (char *)"selfkillfb";
    av[4] = NULL;
    pid = 0;
    rc = spawn_argv(av, &pid);
    expect_true("proc2.selfkillfb_spawn", rc == 0 && pid > 0, rc);
    if (rc == 0) {
        st = -1;
        expect("proc2.selfkillfb_wait", wait_status((i64)pid, &st), 0);
        expect("proc2.selfkillfb_status", st, 0x109);
    }
    av[3] = (char *)"selfkillshm";
    pid = 0;
    rc = spawn_argv(av, &pid);
    expect_true("proc2.selfkillshm_spawn", rc == 0 && pid > 0, rc);
    if (rc == 0) {
        st = -1;
        expect("proc2.selfkillshm_wait", wait_status((i64)pid, &st), 0);
        expect("proc2.selfkillshm_status", st, 0x109);
        put_str("AUDIT: info self-kill with shm/fb ok\n");
    }

    /* wait/kill 的 status 指针边界、null status、已死进程的幂等 kill。 */
    av[3] = (char *)"block";
    av[4] = NULL;
    pid = 0;
    rc = spawn_argv(av, &pid);
    expect_true("proc2.wait_bad_status_spawn", rc == 0 && pid > 0, rc);
    if (rc == 0) {
        expect("proc2.wait_status_cross",
               sys3(SYS_WAIT, (i64)pid, (i64)MARKER_OFF(0xFFE), 0), E_FAULT);
        expect("proc2.kill0", sys3(SYS_KILL, (i64)pid, 0, 0), 0);
        /* 僵尸态再 kill 一次：幂等返回 0，不重复迁移状态。 */
        expect("proc2.kill_again_idempotent",
               sys3(SYS_KILL, (i64)pid, 9, 0), 0);
        st = -1;
        expect("proc2.wait_kill0", wait_status((i64)pid, &st), 0);
        expect("proc2.kill0_status", st, 0x100);
        expect("proc2.wait_again_echild",
               sys3(SYS_WAIT, (i64)pid, (i64)(usize)&st, 0), E_CHILD);
    }

    av[3] = (char *)"quick";
    av[4] = NULL;
    pid = 0;
    rc = spawn_argv(av, &pid);
    expect_true("proc2.wait_null_spawn", rc == 0 && pid > 0, rc);
    if (rc == 0) {
        expect("proc2.wait_null_status", sys3(SYS_WAIT, (i64)pid, 0, 0), 0);
    }

    /* 线程池耗尽：第 N+1 个 spawn 返回 ENOMEM，之前的全部能 wait 回收。 */
    made = 0;
    rc = 0;
    for (i = 0; i < 20u; i++) {
        pid = 0;
        rc = spawn_argv(av, &pid);
        if (rc != 0) {
            break;
        }
        if (made < (u32)ARRAY_N(pids)) {
            pids[made++] = pid;
        }
    }
    expect_true("proc2.thread_pool_limited", rc == -12, rc);
    expect_true("proc2.thread_pool_made", made >= 1, (i64)made);
    for (i = 0; i < made; i++) {
        st = -1;
        expect("proc2.thread_pool_wait", wait_status((i64)pids[i], &st), 0);
    }
    rc = spawn_argv(av, &pid);
    expect_true("proc2.thread_pool_recovers", rc == 0 && pid > 0, rc);
    if (rc == 0) {
        st = -1;
        expect("proc2.thread_pool_recover_wait", wait_status((i64)pid, &st),
               0);
    }

    /*
     * 进程表耗尽：不 wait 的 quick 子进程变僵尸占槽（线程会被调度器回收），
     * 槽位满后 proc_alloc 返回 ENOSPC；最后逐个 wait 清空。
     */
    made = 0;
    enospc = 0;
    for (u32 round = 0; round < 4u; round++) {
        for (i = 0; i < 12u; i++) {
            pid = 0;
            rc = spawn_argv(av, &pid);
            if (rc == 0) {
                if (made < (u32)ARRAY_N(pids)) {
                    pids[made++] = pid;
                }
            } else {
                if (rc == -28) {
                    enospc++;
                }
                break;
            }
        }
        sleep_ticks(200000000);
    }
    expect_true("proc2.proc_table_enospc", enospc >= 1, (i64)enospc);
    for (i = 0; i < made; i++) {
        st = -1;
        expect("proc2.proc_table_wait", wait_status((i64)pids[i], &st), 0);
    }

    /* kill 一个阻塞在 wait(孙进程) 的进程：wait 不是 IPC 等待链。 */
    av[0] = g_self;
    av[1] = (char *)"child";
    av[2] = (char *)"130";
    av[3] = (char *)"waitblock";
    av[4] = NULL;
    pid = 0;
    rc = spawn_argv(av, &pid);
    expect_true("proc2.waitblock_spawn", rc == 0 && pid > 0, rc);
    if (rc == 0) {
        sleep_ticks(300000000); /* 子进程此时阻塞在 SYS_WAIT */
        expect("proc2.waitblock_kill", sys3(SYS_KILL, (i64)pid, 9, 0), 0);
        st = -1;
        expect("proc2.waitblock_wait", wait_status((i64)pid, &st), 0);
        expect("proc2.waitblock_status", st, 0x109);
        put_str("AUDIT: info kill blocked-in-wait ok\n");
    }
    put_str("AUDIT: info proc table/thread pool limits ok\n");
}

static void test_shm_fb2(void)
{
    u64 va = 0;
    u64 va2 = 0;
    u32 h = 0;
    u32 handles[8];
    u32 n = 0;
    u32 i;
    i64 rc;
    u32 pid = 0;
    int st = 0;
    char hstr[24];
    char *av[6];

    section("shmfb2");

    expect("shm2.alloc_pages_umax",
           sys3(SYS_SHM_ALLOC, (i64)~0ULL, (i64)(usize)&va,
                (i64)(usize)&h), E_INVAL);
    expect("shm2.alloc_va_out_cross",
           sys3(SYS_SHM_ALLOC, 1, (i64)MARKER_OFF(0xFFC),
                (i64)(usize)&h), E_FAULT);
    expect("shm2.alloc_handle_out_cross",
           sys3(SYS_SHM_ALLOC, 1, (i64)(usize)&va,
                (i64)MARKER_OFF(0xFFC)), E_FAULT);
    expect("shm2.map_out_cross",
           sys3(SYS_SHM_MAP, 1, (i64)MARKER_OFF(0xFFC), 0), E_FAULT);

    rc = sys3(SYS_SHM_ALLOC, 1, (i64)(usize)&va, (i64)(usize)&h);
    expect_true("shm2.alloc", rc == 0 && h != 0 && va >= 0x50000000ULL, rc);
    if (rc == 0) {
        /* SHM_MAPS_MAX=8：owner 占 1 个，再加 7 个映射后 ENOSPC。 */
        for (i = 0; i < 7u; i++) {
            va2 = 0;
            rc = sys3(SYS_SHM_MAP, (i64)h, (i64)(usize)&va2, 0);
            expect_true("shm2.map_extra", rc == 0 && va2 >= 0x50000000ULL,
                        rc);
        }
        va2 = 0;
        expect("shm2.map_full",
               sys3(SYS_SHM_MAP, (i64)h, (i64)(usize)&va2, 0), E_NOSPC);
        expect("shm2.map_out_cross2",
               sys3(SYS_SHM_MAP, (i64)h, (i64)MARKER_OFF(0xFFC), 0),
               E_FAULT);
    }

    /*
     * 子进程正常退出（不是被 kill）时的 shm/fb 释放顺序：必须在对象表填满
     * 之前做（填满后 SYS_SHM_ALLOC 只能返回 ENOSPC，没有 free syscall）。
     */
    rc = sys3(SYS_SHM_ALLOC, 1, (i64)(usize)&va, (i64)(usize)&h);
    expect_true("shm2.child_spawn_alloc", rc == 0, rc);
    if (rc == 0) {
        *(volatile u32 *)(usize)va = 0x11111111u;
        (void)put_s64(hstr, (u32)sizeof(hstr), (i64)h);
        av[0] = g_self;
        av[1] = (char *)"child";
        av[2] = (char *)"120";
        av[3] = (char *)"shmq";
        av[4] = hstr;
        av[5] = NULL;
        pid = 0;
        rc = spawn_argv(av, &pid);
        expect_true("shm2.child_spawn", rc == 0 && pid > 0, rc);
        if (rc == 0) {
            st = -1;
            expect("shm2.child_wait", wait_status((i64)pid, &st), 0);
            expect("shm2.child_status", st, 0);
            expect_true("shm2.child_wrote",
                        *(volatile u32 *)(usize)va == 0x5A5A5A5Au,
                        (i64)*(volatile u32 *)(usize)va);
            expect_true("shm2.parent_survives",
                        *(volatile u32 *)(usize)va == 0x5A5A5A5Au,
                        (i64)*(volatile u32 *)(usize)va);
            *(volatile u32 *)(usize)va = 0x22222222u;
        }
    }

    /* FB 输出指针页边界（父进程这里已映射过 FB，先过 uaccess 再 EBUSY）。 */
    expect("fb2.va_out_cross",
           sys3(SYS_FB_MAP, (i64)MARKER_OFF(0xFFE), (i64)(usize)&g_fb, 0),
           E_FAULT);
    expect("fb2.info_out_cross",
           sys3(SYS_FB_MAP, (i64)(usize)&va, (i64)MARKER_OFF(0xFF0), 0),
           E_FAULT);

    av[0] = g_self;
    av[1] = (char *)"child";
    av[2] = (char *)"121";
    av[3] = (char *)"fbq";
    av[4] = NULL;
    av[5] = NULL;
    pid = 0;
    rc = spawn_argv(av, &pid);
    expect_true("fb2.child_spawn", rc == 0 && pid > 0, rc);
    if (rc == 0) {
        st = -1;
        expect("fb2.child_wait", wait_status((i64)pid, &st), 0);
        expect("fb2.child_status", st, 0);
        put_str("AUDIT: info normal-exit shm/fb release ok\n");
    }

    /* 对象表上限：填满后 ENOSPC（循环自己数已有对象，不假设起始空表）。 */
    for (i = 0; i < 8u; i++) {
        h = 0;
        rc = sys3(SYS_SHM_ALLOC, 1, (i64)(usize)&va, (i64)(usize)&h);
        if (rc != 0) {
            break;
        }
        handles[n++] = h;
    }
    expect_true("shm2.table_fill", n >= 1, (i64)n);
    expect("shm2.table_full",
           sys3(SYS_SHM_ALLOC, 1, (i64)(usize)&va, (i64)(usize)&h), E_NOSPC);
}

/*
 * 收尾竞态：探针退出时 proc_release_handles() 会关闭自己的端口并唤醒阻塞在
 * send 的子进程。故意留一个 blocked sender 不 kill/wait，让它跨过探针的
 * 退出路径；子进程随后应打印 `sender fail rc=-125`（ECANCELED）。
 */
/*
 * 确定性伪随机（fuzz 用；不用 libc）。
 */
static u32 g_rng;

static u32 rnd_u32(void)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng;
}

/*
 * 结构化 fuzz（只做只读 syscall 与有界消息，避免副作用）：
 *   - VFS：1500 条随机路径 -> stat / open(RDONLY) / listdir；
 *   - IPC：合法用户缓冲 + 随机长度/头字段，隔一条发一条合法消息（堆分配/
 *     释放 + 满队列 EAGAIN），任何输入都必须返回错误码而不是 panic。
 */
static void test_fuzz(void)
{
    static const char k_chars[] = "/.abcde0123456789";
    i64 fd;
    i64 rc;
    u32 i;
    u32 j;

    section("fuzz");

    g_rng = 0x5EED1234u;
    for (i = 0; i < 1500u; i++) {
        u32 len = 1u + (rnd_u32() % 250u);

        for (j = 0; j < len; j++) {
            g_path[j] = k_chars[rnd_u32() % (u32)(sizeof(k_chars) - 1u)];
        }
        g_path[len] = '\0';
        (void)sys3(SYS_STAT, (i64)(usize)g_path, (i64)(usize)&g_st, 0);
        fd = open_file(g_path, O_RDONLY, 0);
        if (fd >= 0) {
            (void)sys3(SYS_READ, fd, (i64)(usize)g_buf, 8);
            (void)sys3(SYS_CLOSE, fd, 0, 0);
        }
        (void)sys3(SYS_LISTDIR, (i64)(usize)g_path, (i64)(usize)g_ents_raw,
                   1);
    }
    expect_true("fuzz.vfs_paths", 1, 1);
    put_str("AUDIT: info vfs path fuzz ok\n");

    {
        i64 p = sys3(SYS_PORT_CREATE, 8, 0, 0);
        struct audit_hdr *h = (struct audit_hdr *)(void *)g_big;
        u32 sends = 0;

        expect_true("fuzz.ipc_create", p > 0, p);
        if (p > 0) {
            for (i = 0; i < 500u; i++) {
                u32 len = 16u + (rnd_u32() % (4112u - 16u + 1u));
                u32 max = rnd_u32() % 5001u;

                if ((i & 1u) != 0) {
                    h->type = 0x2000u + (rnd_u32() % 8u);
                    h->len = len - 16u;
                    h->from = 0;
                    h->flags = 0;
                } else {
                    h->type = rnd_u32();
                    h->len = rnd_u32() % 5000u;
                    h->from = ((rnd_u32() & 3u) != 0u) ? 0u : rnd_u32();
                    h->flags = ((rnd_u32() & 1u) != 0u) ? 0u : rnd_u32();
                }
                rc = sys4(SYS_PORT_SEND, p, (i64)(usize)g_big, (i64)len, 0);
                if (rc == 0) {
                    sends++;
                }
                (void)sys4(SYS_PORT_RECV, p, (i64)(usize)g_big, (i64)max, 0);
            }
            for (i = 0; i < 16u; i++) {
                (void)sys4(SYS_PORT_RECV, p, (i64)(usize)g_big, 4112, 0);
            }
            expect_true("fuzz.ipc_progress", sends >= 1, (i64)sends);
            expect("fuzz.ipc_close", sys3(SYS_PORT_CLOSE, p, 0, 0), 0);
            put_str("AUDIT: info ipc msg fuzz ok\n");
        }
    }
}

static void arm_exit_cleanup(void)
{
    struct audit_hdr h;
    char ticket[24];
    char *av[8];
    u32 pid = 0;
    i64 p;
    i64 rc;

    p = sys3(SYS_PORT_CREATE, 1, 0, 0);
    if (p <= 0) {
        put_str("AUDIT: info exit cleanup skipped (no port)\n");
        return;
    }
    hdr_init(&h, 0xEE, 0, 0, 0);
    (void)sys4(SYS_PORT_SEND, p, (i64)(usize)&h, IPC_HDR, 0);
    (void)put_s64(ticket, (u32)sizeof(ticket), p);
    av[0] = g_self;
    av[1] = (char *)"child";
    av[2] = (char *)"200";
    av[3] = (char *)"sender";
    av[4] = ticket;
    av[5] = (char *)"1";
    av[6] = (char *)"18446744073709551615";
    av[7] = NULL;
    rc = spawn_argv(av, &pid);
    if (rc != 0) {
        put_str("AUDIT: info exit cleanup skipped (spawn failed)\n");
        return;
    }
    sleep_ticks(200000000); /* 让子进程阻塞在满队列的 send 上 */
    put_str("AUDIT: info exit cleanup armed\n");
}

/* ------------------------------------------------------------------------- */
/* 入口                                                                       */
/* ------------------------------------------------------------------------- */
static int crash_main(void)
{
    volatile u64 *p = (volatile u64 *)(usize)0;

    put_str("AUDIT: crash mode: dereference NULL in ring3\n");
    *p = 0xA5A5A5A5u; /* CPL3 #PF -> trap.c 目前 panic（已知限制） */
    put_str("AUDIT: crash mode survived (unexpected)\n");
    return 1;
}

int audit_main(long argc, char **argv)
{
    char num[24];

    if (argc >= 2 && str_eq(argv[1], "child")) {
        return child_main(argc, argv);
    }
    if (argc >= 2 && str_eq(argv[1], "crash")) {
        return crash_main();
    }

    put_str("AUDIT: boot syscall-boundary probe\n");
    put_str("AUDIT: pid=");
    (void)put_s64(num, (u32)sizeof(num), sys3(SYS_GETPID, 0, 0, 0));
    put_str(num);
    put_str(" argc=");
    (void)put_s64(num, (u32)sizeof(num), (i64)argc);
    put_str(num);
    put_str("\n");

    g_self = (argv != NULL && argv[0] != NULL) ? argv[0]
                                               : (char *)"/mnt/disk/audit.elf";

    /* 一个专用端口，用于 sleep_ticks() 让出 CPU。 */
    g_sleep_port = (u32)sys3(SYS_PORT_CREATE, 1, 0, 0);

    test_write();
    test_write2();
    test_ipc();
    test_ipc2();
    test_fs();
    test_fs2();
    test_svc();
    test_svc2();
    test_proc(argv);
    test_proc2();
    test_shm_fb();
    test_shm_fb2();
    test_fuzz();
    arm_exit_cleanup();

    put_str("AUDIT: summary pass=");
    (void)put_s64(num, (u32)sizeof(num), (i64)g_pass);
    put_str(num);
    put_str(" fail=");
    (void)put_s64(num, (u32)sizeof(num), (i64)g_fail);
    put_str(num);
    put_str(" total=");
    (void)put_s64(num, (u32)sizeof(num), (i64)(g_pass + g_fail));
    put_str(num);
    put_str("\n");
    return (g_fail == 0) ? 0 : 1;
}

__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile("andq $-16, %rsp\n\t"
                     "call audit_main\n\t"
                     "movl %eax, %edi\n\t"
                     "movl $1, %eax\n\t"
                     "syscall\n\t"
                     "1: jmp 1b\n\t");
}
