/*
 * ng WP2-7: parity 套件 —— 100 项共享 case 表（kernel 自检 + host 单测同一份）。
 *
 * 设计：
 *   - 表就是这一份 X-macro 列表，编号 1..100 按执行顺序连续；内核
 *     （src/ng/parity_selftest.c）与 host（tests/unit-ng/test_parity.c）都从
 *     这里生成自己的 case 表，因此“第 N 项是什么”只有一个定义。
 *   - 每项一行：id / domain / 描述。断言体在内核与 host **共用**
 *     tests/parity/parity_body.inc（VFS/ramfs/FAT16/auth/log/pkg/shell），
 *     ATA（56..60）与显示/输入（96..100）由各自 harness 提供（内核走真硬件，
 *     host 走 fake ATA；显示/输入只在 QEMU 里有意义，host 跳过）。
 *   - 语义来源：旧树 305 项 + ng WP2-1..6 的 claims。这里**不新增功能**，
 *     只把既有行为钉成回归断言。
 *
 * 覆盖配比（100）：
 *   vfs   1..20   路径规范化/errno/open/stat/unlink/mkdir/symlink/fd/权限/cwd
 *   ramfs 21..35  小文件/跨页/APPEND/TRUNC/EXCL/截断/删除/目录/链接/改名/限额
 *   fat16 36..55  BPB 几何/坏 BPB/读写/跨簇/截断/删除/改名/目录/8.3/簇链错误
 *   ata   56..60  IDENTIFY/读/参数拒绝/写回/纯函数
 *   auth  61..70  种子/登录/错误码/passwd/SHA-256 迭代/锁定
 *   log   71..75  行格式/校验/截断读取/64KiB 溢出/重置
 *   pkg   76..85  种子/列表/校验/清单解析矩阵/篡改检测
 *   shell 86..95  tokenizer/重定向/cwd 解析
 *   disp  96..100 显示模式/帧缓冲读回/控制台几何/输入队列/scancode 解码
 */
#ifndef NG_PARITY_CASES_H
#define NG_PARITY_CASES_H

#define PARITY_TOTAL 100

/* 显示/输入的起始编号（host 跳过；内核用真 hardware）。 */
#define PARITY_DISP_FIRST 96

/*
 * X(id, domain, desc)：id 连续 1..100；domain 是短标签；desc 是纯 ASCII
 * 单行描述（串口/CI 日志里直接可读，中文说明放 docs/claims/ng-WP2-7.md）。
 */
#define PARITY_CASE_LIST(X)                                                    \
    X(1, "vfs", "normalize: \"/\" -> \"/\"")                                   \
    X(2, "vfs", "normalize: \"//a//b/\" -> \"/a/b\"")                          \
    X(3, "vfs", "normalize: \".\" with cwd /x -> /x")                          \
    X(4, "vfs", "normalize: \"..\" with cwd /x/y -> /x")                       \
    X(5, "vfs", "normalize: \"..\" above root stays /")                        \
    X(6, "vfs", "normalize: empty/NULL path -> EINVAL")                        \
    X(7, "vfs", "normalize: overlong path -> ENAMETOOLONG")                    \
    X(8, "vfs", "open: empty and overlong path rejected")                      \
    X(9, "vfs", "open: missing path -> ENOENT")                                \
    X(10, "vfs", "open: dir RDWR -> EISDIR, file O_DIRECTORY -> ENOTDIR")      \
    X(11, "vfs", "open: file as middle component -> ENOTDIR")                  \
    X(12, "vfs", "unlink: dir -> EISDIR, missing -> ENOENT")                   \
    X(13, "vfs", "mkdir: existing -> EEXIST, missing parent -> ENOENT")        \
    X(14, "vfs", "fd: bad fd read/write/close -> EBADF")                       \
    X(15, "vfs", "fd: stale generation after close -> EBADF")                  \
    X(16, "vfs", "fd: table full -> EMFILE, then all closable")                \
    X(17, "vfs", "symlink: relative target resolves, readlink ok")             \
    X(18, "vfs", "symlink: loop -> ELOOP")                                     \
    X(19, "vfs", "perm: uid!=0 owner bits (read/write) -> EPERM")              \
    X(20, "vfs", "chdir: relative open and \"..\" normalization")              \
    X(21, "ramfs", "create/write/stat/read small file")                        \
    X(22, "ramfs", "5000-byte cross-page write/read")                          \
    X(23, "ramfs", "O_APPEND writes at end")                                   \
    X(24, "ramfs", "O_TRUNC clears file")                                      \
    X(25, "ramfs", "O_CREAT|O_EXCL on existing -> EEXIST")                     \
    X(26, "ramfs", "truncate shrink -> EOF at new size")                       \
    X(27, "ramfs", "truncate grow -> zero fill")                               \
    X(28, "ramfs", "unlink hides file, new ino on recreate")                   \
    X(29, "ramfs", "nested mkdir + listdir entries")                           \
    X(30, "ramfs", "symlink/readlink/stat through link")                       \
    X(31, "ramfs", "rename file, old path gone")                               \
    X(32, "ramfs", "rename onto existing -> EEXIST")                           \
    X(33, "ramfs", "unlink while open keeps data readable")                    \
    X(34, "ramfs", "single-file 1 MiB limit -> ENOSPC")                        \
    X(35, "ramfs", "empty file read EOF then write")                           \
    X(36, "fat16", "mount synthetic image geometry")                           \
    X(37, "fat16", "bad boot signature -> EINVAL")                             \
    X(38, "fat16", "bad bytes-per-sector -> EINVAL")                           \
    X(39, "fat16", "bad sectors-per-cluster -> EINVAL")                        \
    X(40, "fat16", "bad FAT count -> EINVAL")                                  \
    X(41, "fat16", "bad total sectors -> EINVAL")                              \
    X(42, "fat16", "create/write/read file")                                   \
    X(43, "fat16", "1500-byte cross-cluster write/read")                       \
    X(44, "fat16", "truncate shrink and grow (zero fill)")                     \
    X(45, "fat16", "unlink frees clusters and entry")                          \
    X(46, "fat16", "rename file preserves data")                               \
    X(47, "fat16", "rename onto existing -> EEXIST")                           \
    X(48, "fat16", "mkdir + subdir readdir")                                   \
    X(49, "fat16", "unlink directory -> EISDIR")                               \
    X(50, "fat16", "8.3 name limits and bad chars")                            \
    X(51, "fat16", "cluster chain loop -> EIO")                                \
    X(52, "fat16", "truncated chain -> EIO")                                   \
    X(53, "fat16", "bad cluster marker -> EIO")                                \
    X(54, "fat16", "out-of-range cluster -> EIO")                              \
    X(55, "fat16", "duplicate create EEXIST, readdir end 0")                   \
    X(56, "ata", "IDENTIFY present/sectors/model")                             \
    X(57, "ata", "read sector 0 stable, BPB signature")                        \
    X(58, "ata", "LBA/count/NULL parameter rejection")                         \
    X(59, "ata", "last-sector write/readback/restore")                         \
    X(60, "ata", "pure lba range/devsel/status class")                         \
    X(61, "auth", "init seeds 2 users")                                        \
    X(62, "auth", "root login -> uid 0")                                       \
    X(63, "auth", "alice login -> uid 1000")                                   \
    X(64, "auth", "wrong password -> EACCES")                                  \
    X(65, "auth", "unknown user -> ENOENT")                                    \
    X(66, "auth", "empty name/pass -> EINVAL")                                 \
    X(67, "auth", "/etc/passwd has two well-formed records")                   \
    X(68, "auth", "hash deterministic; iterations 4096")                       \
    X(69, "auth", "salt changes digest; iterations 0 EINVAL")                  \
    X(70, "auth", "5 failures lock user, correct pass rejected")               \
    X(71, "log", "write/read roundtrip line format")                           \
    X(72, "log", "tag/msg validation boundaries")                              \
    X(73, "log", "log_read truncates to max bytes")                            \
    X(74, "log", "64 KiB ring drops oldest lines")                             \
    X(75, "log", "log_init resets state")                                      \
    X(76, "pkg", "init seeds one package")                                     \
    X(77, "pkg", "list returns hello 1.0.0")                                   \
    X(78, "pkg", "verify hello OK")                                            \
    X(79, "pkg", "verify unknown -> ENOENT")                                   \
    X(80, "pkg", "manifest parse valid -> all fields")                         \
    X(81, "pkg", "manifest missing field -> EINVAL")                           \
    X(82, "pkg", "manifest unknown key -> EINVAL")                             \
    X(83, "pkg", "manifest duplicate key -> EINVAL")                           \
    X(84, "pkg", "manifest bad sha256 hex -> EINVAL")                          \
    X(85, "pkg", "tampered entry -> EIO, restore -> OK")                       \
    X(86, "shell", "tokenize \"echo hi\"")                                     \
    X(87, "shell", "tokenize redirect separated")                              \
    X(88, "shell", "tokenize redirect attached")                               \
    X(89, "shell", "tokenize whitespace/collapse")                             \
    X(90, "shell", "tokenize single command")                                  \
    X(91, "shell", "tokenize duplicate redir -> -4")                          \
    X(92, "shell", "tokenize missing redir path -> -3")                        \
    X(93, "shell", "tokenize empty line -> -2, too many args -> -1")           \
    X(94, "shell", "resolve relative path against cwd")                        \
    X(95, "shell", "resolve absolute/.. and empty -> EINVAL")                  \
    X(96, "disp", "display: VBE framebuffer or VGA text ready")                \
    X(97, "disp", "display: framebuffer pixel readback (or text cell)")        \
    X(98, "disp", "display: console grid/cursor geometry")                     \
    X(99, "input", "PS/2 queue empty -> readkey EAGAIN")                       \
    X(100, "input", "scancode decode shift/caps/ctrl/release")

/* 每项一行的表；两个 harness 各自生成一份（避免 header 里的 static 表告警）。 */
struct parity_case {
    int id;
    const char *domain;
    const char *desc;
};

#define PARITY_ROW(id, dom, desc) { (id), (dom), (desc) },
#define PARITY_ONE(id, dom, desc) +1

enum { PARITY_CASE_COUNT = 0 PARITY_CASE_LIST(PARITY_ONE) };

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(PARITY_CASE_COUNT == PARITY_TOTAL,
               "parity case table must have exactly 100 items");
#endif

#endif /* NG_PARITY_CASES_H */
