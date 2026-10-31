# ng VFS / ramfs（WP2-1）

> 状态：E2 / WP2-1 实现记录，2026-10-31。
> 范围：VFS 核心 + ramfs + syscall 20..27 + ring3 demo + host 单测。
> 不做：FAT16（WP2-2）、块设备（WP2-4）、fb/输入（WP2-3）、fsd 服务化。

## 1. 目录与依赖

    src/services/fs/include/vfs.h       VFS 公共 API（节点/挂载/路径/fd/errno）
    src/services/fs/include/ramfs.h     ramfs 池大小/上限/fs_env
    src/services/fs/vfs.c               VFS 核心
    src/services/fs/ramfs.c             内存文件系统
    src/ng/fs_syscall.[ch]              20..27 syscall 胶水（用户指针/uaccess）
    src/ng/fs_selftest.[ch]             内核自检（4 条证据行 + 计数）

这是 **L1 服务域**（`src/services/**`）的第一块落地代码。依赖只有：

- `src/ng/types.h`（L0 公共定宽类型，允许 L1 → L0）；
- `struct fs_env { page_alloc, page_free, ctx }`：ramfs 的数据页分配器，由调用方
  注入（内核绑 PMM；host 单测绑 fake 页池）。

**不** include `src/kernel/**` 或 `src/ng/` 的任何内部头，也不用内核 heap；
`check-domains` 视角下 `src/services/fs` 是 L1，没有向上依赖（0 违规）。

## 2. VFS API

| 分类 | 函数 |
|---|---|
| 挂载 | `vfs_mount(path, ops, ctx, root)` / `vfs_umount(path)` / `vfs_reset()` / `vfs_mount_count()` / `vfs_mount_path_at(i)` |
| 路径 | `vfs_normalize(cwd, path, out, size)` / `vfs_resolve(c, path, &node)` / `vfs_readlink(c, path, buf, max)` / `vfs_chdir(c, path)` |
| 文件 | `vfs_open(c, path, flags, mode)` / `vfs_read` / `vfs_write` / `vfs_close` |
| 元数据 | `vfs_stat` / `vfs_listdir` / `vfs_mkdir` / `vfs_unlink` / `vfs_rename` / `vfs_truncate` / `vfs_symlink` |
| 上下文 | `vfs_ctx_init(c)`（fd 表 + cwd + uid） |

错误码全是负 errno：`EPERM/ENOENT/EIO/EBADF/ENOMEM/EFAULT/EBUSY/EEXIST/
ENOTDIR/EISDIR/EINVAL/ENFILE/EMFILE/ENOSPC/EROFS/ENAMETOOLONG/ENOTEMPTY/ELOOP`。
（`EIO` 是 WP2-2 为 FAT16 的块设备/簇链错误追加的。）
所有负向用例在 host 单测里断言**具体值**，不按“负值即失败”放过。

节点由具体 FS **内嵌**（`struct vfs_node` 是 `ramfs_node` 的成员），VFS 不分配
节点、不拥有内存；`ops` 表把 `lookup/create/unlink/rename/readdir/read/write/
truncate/readlink/symlink/retain/release` 回调给后端。这样 VFS 不需要 heap，
host 与内核跑同一份代码。

## 3. 路径语义

1. `vfs_normalize()` 纯词法：相对路径先接 `ctx->cwd`（默认 `/`），折叠重复
   `/`，忽略 `.`，`..` 弹一层（根处 `..` 仍是根，不越界），末尾 `/` 折叠；
   组件 ≤ 63 字节、整条 ≤ 255 字节，超限 `ENAMETOOLONG`，空串 `EINVAL`。
2. `vfs_resolve()` 先规范化，再按“最长挂载路径前缀 + 组件边界”选挂载点
   （`/mnt` 不会误匹配 `/mnt2`），逐组件 `lookup`。
3. 符号链接：中间组件与末组件都跟随（`stat/open` 语义）；相对目标相对链接
   所在目录，绝对目标从根开始；做法是“已解析前缀 + 目标 + 剩余部分”重新
   规范化后从头解析，深度 > 8 → `ELOOP`（`a→b→a` 已验证）。
4. 创建/删除/改名走 `resolve_parent()`：末组件允许不存在；`/` 本身不可作为
   操作对象（`EINVAL`）。

## 4. 打开文件表

- 每 AS 一份 `struct vfs_ctx`（本 WP 只有单 AS，内核里 `fs_syscall.c` 持有静态
  一份；host 单测用独立 ctx）。
- `fd = (generation << 16) | slot`，slot 0..15，generation 1..0x7FFF；每次分配
  generation+1（跳过 0），close 后旧 fd 即便槽位复用也会因 generation 不匹配
  返回 `EBADF` 并计入 `stale_fd`。与 ng 句柄表同一套 slot+generation 语义。
- fd 记录 `offset/flags/node`；没有 `lseek`，`O_APPEND` 每次写前把 offset 拉到
  文件尾。open 目录只允许只读；`read` 目录 `EISDIR`，`O_DIRECTORY` 打开文件
  `ENOTDIR`。
- 打开时 `retain`、关闭时 `release`：unlink 一个仍被打开的文件只摘目录项并
  标记 `deleted`，最后一个 close 才释放节点与数据页。

## 5. 权限

WP2-1 只做 owner 位检查（`mode & 0400/0200`）：

- `ctx->uid == 0`（内核/自检/当前 syscall）绕过；
- `uid != 0` 时按 owner 位判定，读/写/在目录里创建/删除不足 → `EPERM`；
- 新建节点的 fd 不做自身权限反查（避免 `open(O_CREAT|O_RDWR, 0444)` 创建成功
  却返回 EPERM 留下孤儿文件），父目录写权限仍然检查。

组/其他位、ACL、cap 检查留给 fsd/uid 模型 WP。

## 6. ramfs

- 节点池 32（节点 0 是根，mode 0755，ino 1）、目录项池 64、数据页池 512 页
  （2 MiB）；单文件 ≤ 1 MiB（256 页）。
- 数据页用**页链**（`first_page`/`next` 都是索引+1，0 表示结束），按
  `off/4096` 走链，不建页指针数组；页由 `fs_env.page_alloc` 分配，约定返回
  已清零的页，`page_free` 归还。
- 写：先 `ensure_pages(need)` 追加页（失败回滚本次追加并恢复 first/count），
  再按页拷贝，最后更新 size；`truncate` 缩小释放尾页并清零页尾，扩大分配新页
  （新页零）并补齐旧页尾的洞。
- 删除：目录 `EISDIR`（本 WP 无 rmdir）；文件摘目录项 + `deleted`，open_refs
  归零才回收。改名只支持同一目录内文件/符号链接（目标存在 `EEXIST`）。
- 上限错误：文件 > 1 MiB → `ENOSPC`；页池/节点池/目录项池耗尽 → `ENOSPC`
  或 `ENOMEM`（页分配器返回 NULL）；host 单测用 8 页 fake 池验证回滚后
  size 不变、页数回到原值。

## 7. syscall ABI（20..27）

    rax=调用号，rdi/rsi/rdx/r10=参数，rax 返回（负 = -errno）
    20 open(path, flags, mode)                  -> fd
    21 read(fd, buf, len)                       -> 字节数
    22 write(fd, buf, len)                      -> 字节数
    23 close(fd)                                -> 0
    24 stat(path, struct vfs_stat *out)         -> 0
    25 listdir(path, struct vfs_dirent *ents, max) -> 填了几条
    26 mkdir(path, mode)                        -> 0
    27 unlink(path)                             -> 0

用户结构体（小端、固定宽度）：

    struct vfs_stat   { u32 type; u32 mode; u64 size; u64 ino; }       24 B
    struct vfs_dirent { u32 type; u32 name_len; u64 size;
                        char name[64]; }                               80 B

`fs_syscall.c` 用 `copyin_str` 拷路径、`copyin/copyout` 拷数据与结构体，全部
经 `uaccess.c`：坏指针 `EFAULT`，不会让内核 #PF。单次 read/write ≤ 512 字节、
listdir ≤ 8 条，超限 `EINVAL`（本 WP 的 demo/自检都远小于上限）。

## 8. 自检与单测

内核自检（`fs_selftest`，run-ng 命中）：

    ng: vfs mount ok (root=ramfs mounts=2)     根挂载 + /mnt 子挂载/覆盖/卸载
    ng: ramfs write/read ok (bytes=4196 limit=1MiB)
    ng: fd bad rejected (n=11)
    ng: vfs path edge ok (n=12)
    ng: fs syscall user ptr EFAULT ok
    ng: WP2-1 selftest PASS

ring3 demo（`hello.S`，全部走真实用户指针）：

    ng: fs open ok / write ok / read ok / stat ok (size=5) / close ok /
    ng: fs listdir ok (n=1) / ng: fs user demo ok

host 单测 `tests/unit-ng/test_vfs.c`（+ fake FS 页池、test_syscall 的 20..27
分发用例）覆盖：路径词法 40+ 例、挂载/卸载/前缀边界、create/stat/读写/截断/
删除/改名、跨页读写、1 MiB 与页池上限回滚、符号链接（相对/绝对/循环/悬空/
readlink/删除链接）、fd 代际复用/表满/非法 fd、unlink 打开中文件、权限位、
cwd 相对路径。

## 9. 已知限制 / 遗留

- **单核协作式**：ramfs 没有锁（IF=0、显式调度点）；多核/抢占时加 spinlock，
  API 不变。
- **单 AS**：fd 表接口已按 per-AS 设计，内核只创建一份；进程模型 WP 再按 AS
  分配 `struct vfs_ctx`。
- **无 lseek/fstat/dup/rmdir/rename 跨目录**；listdir 走路径而不是目录 fd。
- **挂载路径与 `..`**：路径先词法规范化再选挂载点，所以 `/mnt/..` 得到 `/`
  （父挂载），不会“反向穿越”到子挂载的父目录——与常见 VFS 行为一致，但
  挂载点内 `..` 的语义在多层挂载时仍是近似。
- **悬空符号链接 + O_CREAT**：解析失败后按“父目录 + 末组件”创建，会看到
  链接本身已存在而返回 `EEXIST`，不会创建目标文件（POSIX 行为差异，记录）。
- **不缓存/不预读**：read/write 直接拷页；page cache、mmap 留给 fsd。
- **上限**：1 MiB/文件、2 MiB 数据池、32 节点、64 目录项、8 挂载、16 fd/AS、
  512 B/syscall IO、8 条/listdir。FAT16（WP2-2）作为第二个 VFS 后端挂在
  `/mnt/fat`，VFS 层不变；FAT16 的几何/镜像/限制见 `fat16-ng.md`。
