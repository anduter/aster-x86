# ng-WP2-1：VFS + ramfs 移植（PASS）

## 结论

**PASS** —— `src/services/fs` 成为第一个真正落地的 **L1 系统服务域**，E2/WP2-1
出口达成：

- **VFS 核心**：挂载表（`vfs_mount/umount`，最长前缀 + 组件边界匹配）、节点抽象
  （dir/file/symlink，节点由后端内嵌、VFS 不分配内存）、路径解析（绝对/相对/
  cwd/`.`/`..`/重复与末尾 `/`/NUL/长度上限/符号链接跟随）、`open/read/write/
  close/stat/listdir/mkdir/unlink/rename/truncate/symlink/readlink`，全部失败路径
  返回**具体负 errno**（ENOENT/EINVAL/ENOTDIR/EISDIR/ENOSPC/EBADF/EFAULT/EPERM/
  EEXIST/ELOOP/ENAMETOOLONG/EBUSY/EMFILE/ENOMEM）。
- **ramfs**：32 节点 / 64 目录项 / 512 数据页（2 MiB）静态池；数据页来自注入的
  `fs_env`（内核绑 PMM，host 绑 fake 页池）；**页链**读写、截断（缩/扩+补零）、
  删除（打开中延迟回收）、改名、符号链接；**单文件 ≤ 1 MiB**。
- **syscall 接入**：`20 open / 21 read / 22 write(fd) / 23 close / 24 stat /
  25 listdir / 26 mkdir / 27 unlink`；用户指针全部经 `uaccess.c`（copyin_str/
  copyin/copyout），坏指针 `EFAULT`；fd 表 per-AS（单 AS 下一份）。
- **ring3 demo**：`hello.S` 走完 `open("/tmp/a", O_CREAT|O_RDWR)` → `write("hello")`
  → 关闭重开 → `read` 校验 → `stat(size=5)` → `close` → 旧 fd 读必须 `-EBADF`
  → `listdir("/tmp")==1`，串口打印 `ng: fs ...`。
- **自检**：run-ng 52 条期望行 + 17 条正则全绿（新增 5 条固定行 + 6 条正则）；
  host 单测 **1085 → 1438 checks**；旧 32 位 **305/305**；check-domains /
  check-deps 0 违规；ci-ng **6/6 PASS**。

`src/kernel/**`、`build.ps1`、`tools/ci.ps1`、`abi.h` 未修改；不做 FAT16（WP2-2）、
块设备（WP2-4）、fb/输入（WP2-3）。

## 交付物

| 路径 | 内容 |
|---|---|
| `src/services/fs/include/vfs.h` | VFS 公共 API：errno/节点/ops/挂载/路径/fd/stat/dirent（**新**） |
| `src/services/fs/include/ramfs.h` | ramfs 池大小/上限/`fs_env`/统计（**新**） |
| `src/services/fs/vfs.c` | 挂载表、词法规范化、逐组件解析+符号链接、fd 表、权限、文件/目录操作（**新**） |
| `src/services/fs/ramfs.c` | 节点/目录项/页链、读写/截断/删除/改名/链接、延迟回收（**新**） |
| `src/ng/fs_syscall.h` / `.c` | syscall 20..27 胶水（路径 copyin_str、bounce buffer、per-AS ctx）（**新**） |
| `src/ng/fs_selftest.h` / `.c` | 内核自检：挂载/写入读出/错误码/路径边界/用户指针（**新**） |
| `src/ng/syscall.h` / `.c` | 追加 20..27、`ring3_result.fs_calls`、分发与统计（**改**） |
| `src/ng/main.c` | WP2-1 自检接线 + PASS 行（排在 WP1-7 前，ring3 demo 需要 fs）（**改**） |
| `src/ng/user/hello.S` | ring3 VFS demo + 每步断言（**改**） |
| `src/ng/elf_selftest.c` | ring3 断言放宽为 `writes>=1 && fs_calls>=6`；坏镜像 `p_filesz>p_memsz` 改为按实际 memsz 生成（**改**） |
| `tests/unit-ng/test_vfs.c` | 路径/挂载/节点/fd/权限/链接/边界（**新**） |
| `tests/unit-ng/fake_ng_env.[ch]` | fake FS 页池（8 页，清零分配）（**改**） |
| `tests/unit-ng/test_syscall.c` | 20..27 分发/EFAULT/EBADF 用例（**改**） |
| `tests/unit-ng/unit_main.c` | 加 `test_vfs()`（**改**） |
| `tools/build-ng.ps1` | 加 `fs_syscall.c`/`fs_selftest.c`/`vfs.c`/`ramfs.c` + `-I src/services/fs/include`；30 步（**改**） |
| `tools/run-ng.ps1` | +5 固定行、+6 正则、WP2-1 失败模式（**改**） |
| `tools/unit-ng.ps1` | 加 fs 源与 test_vfs/fake 页池、include 路径（**改**） |
| `docs/design/vfs-ng.md` | VFS/ramfs/syscall ABI/限制（**新**） |
| `docs/design/evolution.md` | E2 进度：WP2-1 完成（**改**） |

## 环境

- 分支 `ng`，基线 HEAD `69d84ca`（tag `ng-e1`），仓库 `D:\OS_Project`。
- clang 23.1.4 / ld.lld 23.1.4（`D:\msys64\mingw64\bin`）；内核
  `--target=x86_64-elf -ffreestanding -nostdlib -mno-red-zone
  -fno-stack-protector`；host 单测同一 clang 的默认 PE 目标。
- QEMU 11.0.0，`qemu-system-x86_64 -kernel -m 256 -cpu max`（另跑 `-cpu qemu64`
  验证 SMAP=off 路径）。

## 证据（2026-10-31）

### 构建

```text
powershell -File tools/build-ng.ps1
```

```text
=== build-ng (WP2-1) ===
clang: D:\msys64\mingw64\bin\clang.exe
lld:   D:\msys64\mingw64\bin\ld.lld.exe

=== user-elf (WP1-7) ===
  hello.S -> obj-ng/user_hello.o
  link    -> obj-ng/hello.elf (base 0x40000000)
user-elf: PASS (13960 bytes, 3 PT_LOAD, entry 0x40000000, end 0x40002180)

  [1/30] entry.S
  ...
  [25/30] fs_syscall.c
  [26/30] fs_selftest.c
  [27/30] main.c
  [28/30] src/services/fs/vfs.c
  [29/30] src/services/fs/ramfs.c
  [30/30] link -> dist-ng/aster-ng.elf

ELF64: PASS (128568 bytes)
  ...
multiboot2: PASS
pvh-note: PASS

build-ng: PASS -> D:\OS_Project\dist-ng\aster-ng.elf
```

用户 ELF 仍 3 个 PT_LOAD：`.text 0x24e / .rodata 0xab / .data+.bss
filesz 0x8 memsz 0x180`（新增 `fs_*` 缓冲在 `.bss`，加载器清零尾巴的路径继续被
验证）。

### 串口（`powershell -File tools/run-ng.ps1`，`-cpu max`）

WP1-2..WP1-7 全部保持 PASS。WP2-1 段：

```text
ng: WP1-6 selftest PASS
ng: vfs mount ok (root=ramfs mounts=2)
ng: ramfs write/read ok (bytes=4196 limit=1MiB)
ng: fd bad rejected (n=11)
ng: vfs path edge ok (n=12)
ng: fs syscall user ptr EFAULT ok
ng: WP2-1 selftest PASS
ng: elf64 header ok
...
ng: ring3 exec ok
ng: ring3 cr3=0x193000 (as pml4=0x193000)
ng: ring3 hello
ng: fs open ok
ng: fs write ok
ng: fs read ok
ng: fs stat ok (size=5)
ng: fs close ok
ng: fs listdir ok (n=1)
ng: fs user demo ok
ng: syscall exit test
ng: ring3 exit ok
ng: WP1-7 selftest PASS
------------------------------------------------
run-ng: matched 52/52 expected lines + 17 patterns
run-ng: PASS (all expected lines in 0.2s, cpu max)
```

`run-ng.ps1` 新增的期望（全部命中才 PASS）：

```text
6 条固定行：
  ng: vfs mount ok
  ng: ramfs write/read ok
  ng: fd bad rejected
  ng: vfs path edge ok
  ng: fs syscall user ptr EFAULT ok
  ng: WP2-1 selftest PASS
7 条用户态固定行：
  ng: fs open ok / write ok / read ok / stat ok / close ok / listdir ok /
  ng: fs user demo ok
6 条正则：
  ng: vfs mount ok \(root=ramfs mounts=2\)
  ng: ramfs write/read ok \(bytes=[0-9]+ limit=1MiB\)
  ng: fd bad rejected \(n=[0-9]+\)
  ng: vfs path edge ok \(n=[0-9]+\)
  ng: fs stat ok \(size=5\)
  ng: fs listdir ok \(n=1\)
失败早停模式新增：
  ng: vfs .* FAIL / ng: ramfs .* FAIL / ng: fd .* FAIL / ng: fs .* FAIL /
  ng: WP2-1 selftest FAIL
```

### host 单测

```text
powershell -File tools/unit-ng.ps1
=== unit-ng (WP1-8 host tests) ===
clang: D:\msys64\mingw64\bin\clang.exe
1438 checks, 0 failures
unit-ng: PASS (1438 checks, 0 failures)
```

WP1-8 为 1085，本 WP **+353**：`tests/unit-ng/test_vfs.c`（路径词法 40+ 例、
挂载/卸载与前缀边界、create/stat/读写/截断/删除/改名、跨页读写、1 MiB 与页池
上限回滚、符号链接、fd 代际/表满/非法 fd、unlink 打开中文件、权限位、cwd 相对
路径）+ `test_syscall.c` 的 20..27 分发/EFAULT/EBADF 用例。

关键断言（不是“负值即失败”）：

- `vfs_normalize`：`/a//b`→`/a/b`、`/a/./b`→`/a/b`、`/a/../b`→`/b`、
  `/../..`→`/`、空路径 `EINVAL`、63 字节组件 OK / 64 字节 `ENAMETOOLONG`、
  255 字节路径 OK / 256 字节 `ENAMETOOLONG`；
- 挂载：重复 `EEXIST`、坏挂载点 `ENOENT`、根卸载 `EBUSY`、有子挂载 `EBUSY`、
  `/mnt` 不误匹配 `/mnt2`；
- 数据：4096+100 字节写、跨页读、截断 100/8192、扩展段读回全 0、`+1 MiB`
  `ENOSPC`、fake 页池耗尽 `ENOMEM` 且 size 不变；
- fd：`(gen<<16)|slot`，close 后同槽位复用 generation+1，旧 fd `EBADF`，
  16 个槽位满 `EMFILE`；
- 链接：相对/绝对/`..` 目标、`a→b→a` `ELOOP`、悬空 `ENOENT`、readlink 非链接
  `EINVAL`、删链接不影响目标、通过链接写命中目标；
- 权限：uid 1000 下 0555 目录建/删 `EPERM`、0444 文件写/截断 `EPERM`，
  uid 0 绕过。

### 一条命令门禁

```text
powershell -File tools/ci-ng.ps1
=== ci-ng summary ===
  PASS check-domains    0 violations; exit=0, 0.5s
  PASS check-deps       0 violations; exit=0, 0.5s
  PASS unit-ng          1438 checks; exit=0, 2.4s
  PASS build-ng         128568 bytes, user-elf 13960 bytes; exit=0, 2.5s
  PASS run-ng           52/52 lines + 17 patterns; exit=0, 0.5s
  PASS legacy-32bit     305/305 parity checks; exit=0, 53.0s
  6 passed, 0 failed, 0 skipped, 59.5s

=== NG CI PASS ===
```

说明：中间有一次 ci-ng 的 legacy 步骤出现 **1/305** 失败（随机项），紧接着
两次独立 `build.ps1 -Test` 均 **305/305**（53.3s / 53.0s）且 ci-ng 复跑 6/6
PASS；与 `docs/parallel-sessions.md` §“已知偶发问题”记录的旧树用户态偶发同族，
本 WP 未改 `src/kernel/**`。

### 旧树 / 边界 / 负向

```text
powershell -File build.ps1 -Test
=== compile: 1 compiled, 103 skipped ===
=== elapsed: 53.3s ===
=== TEST PASS (305/305) ===

powershell -File tools/check-domains.ps1
checked: 209 files, 730 quoted includes
domain include violations: 0

powershell -File tools/check-deps.ps1
checked: 201 files, 650 quoted includes
interface layer violations (include/*.h -> *_internal.h): 0
whitelisted syscall-subsystem includes: 13
module include violations: 0
OK: no cross-module include violations.
```

panic 路径回归（`build-ng.ps1 -PanicTest` + `run-ng.ps1 -Panic`）：

```text
ng: panic selftest: triggering unrecoverable #UD
PANIC: vec=0x6 err=0x0 rip=0x10f0ad cr2=0x114000
run-ng: PASS (panic path observed in 0.2s, cpu max)
```

随后重新普通构建，产物回到 128568 bytes。

`-cpu qemu64`（SMAP=off）：

```text
ng: fs listdir ok (n=1)
ng: fs user demo ok
ng: ring3 exit ok
ng: WP1-7 selftest PASS
run-ng: matched 52/52 expected lines + 17 patterns
run-ng: PASS (all expected lines in 0.2s, cpu qemu64)
```

## VFS API（摘要）

```c
/* 挂载 */
int vfs_mount(const char *path, const struct vfs_fs_ops *ops, void *ctx,
              struct vfs_node *root);
int vfs_umount(const char *path);
void vfs_reset(void);
u32 vfs_mount_count(void);
const char *vfs_mount_path_at(u32 index);

/* 路径 */
int vfs_normalize(const char *cwd, const char *path, char *out, u32 out_size);
int vfs_resolve(struct vfs_ctx *c, const char *path, struct vfs_node **out);
int vfs_readlink(struct vfs_ctx *c, const char *path, char *buf, u32 max);
int vfs_chdir(struct vfs_ctx *c, const char *path);

/* 文件 / 元数据 */
int vfs_open(struct vfs_ctx *c, const char *path, u32 flags, u32 mode);
int vfs_read(struct vfs_ctx *c, int fd, void *buf, u64 len);
int vfs_write(struct vfs_ctx *c, int fd, const void *buf, u64 len);
int vfs_close(struct vfs_ctx *c, int fd);
int vfs_stat(struct vfs_ctx *c, const char *path, struct vfs_stat *out);
int vfs_listdir(struct vfs_ctx *c, const char *path, struct vfs_dirent *ents,
                u32 max);
int vfs_mkdir(struct vfs_ctx *c, const char *path, u32 mode);
int vfs_unlink(struct vfs_ctx *c, const char *path);
int vfs_rename(struct vfs_ctx *c, const char *oldpath, const char *newpath);
int vfs_truncate(struct vfs_ctx *c, const char *path, u64 size);
int vfs_symlink(struct vfs_ctx *c, const char *path, const char *target);
```

节点由具体 FS 内嵌（`struct vfs_node` 是 `ramfs_node` 的成员），VFS 只经
`struct vfs_fs_ops` 回调；`fs_env` 是 L1 与 L0 的唯一依赖（数据页分配）。
详见 `docs/design/vfs-ng.md`。

## ramfs 限制

| 项 | 值 |
|---|---|
| 节点 / 目录项 | 32 / 64（节点 0 为根，ino 1） |
| 数据页池 | 512 × 4 KiB = 2 MiB |
| 单文件 | 1 MiB（256 页，`ENOSPC`） |
| 单次 syscall IO | read/write ≤ 512 B，listdir ≤ 8 条（`EINVAL`） |
| fd | 16 / AS（`EMFILE`）；`fd=(gen<<16)|slot`，gen 1..0x7FFF |
| 挂载 | 8 个；符号链接深度 8（`ELOOP`） |
| 路径 / 组件 | 255 / 63 字节（`ENAMETOOLONG`） |
| 删除 | 目录 `EISDIR`（无 rmdir）；打开中文件延迟到最后一个 close 回收 |
| 改名 | 只支持同一目录内文件/符号链接；目标存在 `EEXIST` |

## 自检项清单

1. 挂载：根 ramfs + `/mnt` 子挂载/覆盖/卸载；重复 `EEXIST`、坏点 `ENOENT`、
   根 `EBUSY`、子挂载占用 `EBUSY`；`/mnt` 不误匹配 `/mnt2`。
2. ramfs：4096+100 字节跨页写、读回一致、截断缩小/扩大、扩展段全 0、
   `+1 MiB` `ENOSPC`、删除后 `ENOENT`。
3. fd：旧 fd/0/-1/越界 generation 全 `EBADF`；目录 RDWR/写 `EISDIR`；目录
   unlink `EISDIR`；文件当目录 `ENOTDIR`。
4. 路径：`.`/`..`/`//`/尾 `/`/空串/超长/`ENOTDIR`/cwd 相对/符号链接
   （相对、绝对、循环 `ELOOP`）。
5. 用户指针：20..27 在内核 CR3 下用坏指针 → `EFAULT`（不 #PF）；ring3 demo
   用真实用户页走完 open/write/read/stat/close/listdir，旧 fd 读 `-EBADF`。
6. host 1438/0；旧 32 位 305/305；check-domains/check-deps 0；ci-ng 6/6；
   panic 负向与 `-cpu qemu64` PASS。

## 未做 / 遗留（按 WP 边界）

- **FAT16（WP2-2）**、**块设备（WP2-4）**、**fb/输入（WP2-3）** 未动；VFS 的
  `vfs_fs_ops` 已为它们留位，但挂载点创建/格式化不在本 WP。
- **单核协作式**：ramfs 无锁（IF=0、显式调度点）；多核/抢占时加 spinlock，
  API 不变。
- **单 AS**：fd 表接口按 per-AS 设计，内核只创建一份；进程模型 WP 再按 AS
  分配。
- **POSIX 缺口**：无 lseek/fstat/dup/rmdir/rename 跨目录；listdir 走路径而不是
  目录 fd；悬空符号链接 + `O_CREAT` 返回 `EEXIST`（不创建目标）。
- **挂载内 `..`**：路径先词法规范化再选挂载点，`/mnt/..` 得到 `/`；与常见
  VFS 一致，但多层挂载下仍是近似语义。
- **性能**：无 page cache/预读/mmap，read/write 直接按页拷贝；留给 fsd。
- **fsd 服务化**：VFS 现在仍以内核内 L1 库形式链接（架构允许 D0 暂留内核），
  port+cap 的 fsd 进程化是后续 WP。

## 出口

```text
powershell -File tools/build-ng.ps1    # PASS，128568 bytes，30 步
powershell -File tools/run-ng.ps1      # PASS，52 行 + 17 正则（含 ng: fs user demo ok）
powershell -File tools/unit-ng.ps1     # PASS，1438 checks, 0 failures
powershell -File build.ps1 -Test       # PASS，305/305（旧 32 位）
powershell -File tools/check-domains.ps1   # 0 违规（209 files）
powershell -File tools/check-deps.ps1      # 0 违规
powershell -File tools/ci-ng.ps1           # 6/6 PASS
```
