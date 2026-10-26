# fs 独立审计（audit-fs）

- 审计对象：`src/kernel/fs/**`（ramfs / XFS1 v6、FAT16、VFS 路由、legacy fs syscall）
- 审计时间：2026-10-27，预算 8h（实际约 3h）
- 独立构建：`.obj-audit-fs` / `.dist-audit-fs`，QEMU QMP **4542**，串口 `obj/audit/*.log`
- 代码修复提交：**`b4d5fd9`** `[fs] audit: reject corrupt XFS1 ...`
- 结论：**11 个已修问题**（3 个 P0、5 个 P1、3 个 P2/P3），另有 7 项记录为未修/架构性问题。
- 协作：FS-3/FS-10/FS-11 由 audit-syscall 定位并转交，本会话在 `fs/**` 内修复 + 加回归。

> 说明：共享工作树同时被 audit-net / audit-smp / audit-syscall 修改，`build.ps1 -Test`
> 的 12345 回显端口按约定不可占用，因此整机回归使用**独立 git worktree + 自建 QEMU**
> 验证 fs 自检行；FAT16 用 host 单元测试（真实 `fat16.c`）覆盖。完整 225 项回归未在本
> 会话重跑。

---

## 1. 已修问题

| ID | 级别 | 问题 | 复现 | 根因 | 修复 |
|---|---|---|---|---|---|
| FS-1 | **P0** | 损坏 XFS1 镜像 `type=0` 的 inode 被接受，启动时目录兄弟链自环，整机永久卡死 | `craft_xfs.py type_none` 作 ata0 启动；修复前串口停在 `fat16: host.txt = hello from host`，不再前进 | `fs_mount_disk` 只拒绝 `type > LINK`，`type==FS_TYPE_NONE(0)` 通过；`node_alloc()` 把已挂在父目录里的该槽当空闲复用，`link_child()` 令 `next_sibling` 指回自己 | 挂载时要求 `type ∈ {FILE, DIR, LINK}`；新增 `AUD_TYPE_NONE` 回归 |
| FS-2 | **P0** | 挂载失败路径把 `fs_init()` 的默认 `system:` 卷清空，启动安装器无落点；随后 `fs_sync` 把“只有 data 卷”的半残树覆盖回镜像 | `craft_xfs.py orphan` / `bad_size` / `bad_data_lba`；修复前 `fs: cannot create system:/bin/shell`、`getty ... falling back to kernel shell` | `mount_disk_fail()` 只 `memset`，没有恢复 `fs_init()` 的默认卷不变量 | 失败时重建空的 `system` 卷并把单文件限额复位；回归断言失败后 `fs_stat("system:/")` 仍可用 |
| FS-3 | **P0** | `SYS_CHROOT` 把最长 255B 的路径 `strcpy` 到 `proc_t.cwd[192]`，覆盖 `user_brk/fds/caps` | 构造 192..255B 的已存在目录后 `SYS_CHROOT`；旧代码无长度检查 | `fs/fs_syscall.c` 用 `strcpy` 写 192B 字段，且 `proc_root_set` 静默截断 | 新增 `fs_chroot_set_cwd()`：超长返回 `E_INVAL` 且**一个字节都不写**；`SYS_CHROOT` 先做长度检查再改 root/cwd；自检用 `proc_t` canary 验证 |
| FS-4 | **P1** | 无卷根 / 空卷名 / 非法卷名的 XFS1 镜像被接受，启动降级并丢盘 | `craft_xfs.py empty`、`volname_empty`；修复前先打印 `XFS1 v6 mounted`，随后 `cannot create system:/bin/shell`、`fs: synced to ata0` 覆盖镜像 | 挂载只校验 `volume < 4` 和卷槽未占用，不校验“至少一个卷根”和卷名合法性 | 拒绝 0 个卷根；卷名必须非空、不含 `:`/`/`、不是 `.`/`..`、不能与已有卷重名；v5 兼容自检改为手工写一个真实的 v5 卷根 |
| FS-5 | **P1** | FAT16 的 FAT 表可以小到覆盖不了 `cluster_count+2` 个表项，`fat_get()` 把根目录/数据区当 FAT 读 | host 单测 `fat_sectors=1`；修复前 `fat16_mount` 返回 0 | 挂载只检查 `fat_sectors != 0`，没有检查表项覆盖范围；`reserved=0` 也接受 | 挂载要求 `reserved > 0` 且 `fat_sectors >= ceil((clusters+2)*2/512)` |
| FS-6 | **P1** | FAT16 非空文件首簇为 0/1/保留值时，`cluster_lba()` 下溢到 FAT/根目录区，把元数据当文件内容返回 | host 单测 `BADCLU`（cluster=0）与 `BADCL2`（0xFFF0） | `fat16_open` 不校验目录项 `cluster_lo` | 非空文件首簇必须落在 `2..cluster_count+1`，否则打开失败 |
| FS-7 | **P1** | FAT16 簇链以 EOC(0xFFFF) 结束时被当成“下一个簇号 65535”，读越界后整次 `read` 返回 -1 | host 单测 `TRUNC.DAT`（size 2048，FAT[6]=0xFFFF）→ 修复前返回 -1，修复后返回 512 | `fat16_read` 只判断 `next < 2`，没把 `>= cluster_count+2`（EOC/坏簇/越界）当链结束 | 首簇和每次 `next` 都做范围校验；越界按链结束返回已读字节 |
| FS-8 | **P1** | FAT16 对已打开读句柄的文件再次 `WRITE|CREAT` 会截断并释放旧簇链，旧读句柄读到被复用的簇（跨文件数据泄漏） | host 单测：先 `open(READ)`，再 `open(WRITE|CREAT)`；修复前第二次成功 | 截断路径只检查目录项类型，不检查同一 `(dir_lba,dir_idx)` 是否还有其他句柄 | 与 `unlink` 一致：同目录项仍有句柄时返回 `-1`（E_BUSY） |
| FS-9 | **P2** | `fs_listdir` 在 `path_volume()` 提前返回（`":/x"`、盘符 ≥32 字符、未知卷）时，把未初始化的 `rel` 指针交给 `resolve_in_volume()` 解引用 | `fs_listdir(":/x", ...)`；修复前栈上残留指针恰好可读时只返回 E_NOENT，栈内容不合适时会在内核态缺页/oops | `path_volume()` 的提前返回路径不写 `*rel`；`listdir_impl()` 也没有先判 `vol==0` | `path_volume()` 入口统一 `*rel=""`；`listdir_impl()` 先判空卷；自检覆盖 4 种坏前缀 |
| FS-10 | **P2** | `SYS_TIME` 完全忽略 `ecx=max`，4B 用户缓冲也会被写 20B | 自检用 32B canary、`max=4`；修复前 canary[4..19] 被改 | `fs_syscall.c` SYS_TIME 直接按 `strlen(tbuf)+1` 拷贝 | 按 `min(需要, max)` 拷贝；截断时最后一个字节写 NUL；`max==0` 返回 -1 |
| FS-11 | **P3** | `SYS_FSCK` 忽略 `copy_to_user` 返回值，坏用户指针仍返回错误计数（用户以为报告已写） | 自检传 `0xBFFFFF00` 未映射用户地址；修复前返回 fsck 错误数，修复后 -1 | SYS_FSCK 用 `(void)copy_to_user(...)` | 先做 `user_range_ok`，拷贝失败返回 -1 |

### 关键复现/证据

**FS-1（type=0 启动卡死）**

```text
# 修复前（baseline floppy + type_none 镜像），22s 后串口最后一行：
fat16: host.txt = hello from host
# 再没有任何输出；QEMU 仍在运行（不是 panic，是目录链死循环）
```

修复后同一镜像：

```text
fs: ata0 has no valid filesystem, will format
fs: volumes = 2, system:/bin/shell installed (41456 bytes)
...
kernel: ready
osteachos login:
```

**FS-2（mount 失败丢默认卷）**

```text
# 修复前 orphan 镜像：
fs: ata0 has no valid filesystem, will format
fs: cannot create system:/bin/shell
getty: cannot load /bin/shell, falling back to kernel shell
fs: synced to ata0          <- 半残树覆盖镜像

# 修复后 orphan 镜像：
fs: volumes = 2, system:/bin/shell installed (41456 bytes)
osteachos login:
```

**FS-5..8（FAT16 host 单测，真实 `fat16.c`）**

```text
# 修复前（HEAD 的 fat16.c）：
FAIL ... fat16_mount("m0","fat") = 0, expected -1            (reserved=0)
FAIL ... fat16_mount("m0","fat") = 0, expected -1            (FAT too small)
FAIL ... fat16_read(h, buf, 2048U) = -1, expected 512        (EOC)
FAIL ... fat16_open("badclu.bin") = 0, expected -1
FAIL ... fat16_open("badcl2.bin") = 0, expected -1
FAIL ... fat16_open("busy.txt", WRITE|CREAT) = 1, expected -1
27 checks, 6 failures

# 修复后：
27 checks, 0 failures
```

**FS-3/10/11（fs syscall 缓冲边界）**

```text
# 修复前变体（保留新 helper、恢复旧 SYS_TIME/SYS_FSCK/chroot 拷贝）：
fs: audit selftest FAILED at step 6

# 修复后：
fs: audit syscall chroot/time/fsck buffer guards ok
fs: audit selftest PASS
```

---

## 2. 未修项（记录，不在本次 fs 权限/风险范围内）

| ID | 级别 | 结论 | 建议 |
|---|---|---|---|
| UF-1 | P1 | **chroot 可被绝对符号链接绕过**：`make_abs_path()` 只做文本前缀限制，但 `resolve_path()` 把软链接目标 `"/etc/shadow"` 解释为**卷根**绝对路径，不是 chroot 根。普通用户可在自己目录建 `x -> /home/...` 读到 chroot 外世界可读文件 | 在 VFS/fs 之间传递 `proc_root_of()`，解析绝对软链接目标时以 chroot 根为基准，并同步处理相对目标的 `..`；属于设计级改动，未在本次动 |
| UF-2 | P2 | FAT16 没有锁：`sec_buf`、`handles[]`、`fat` 全局状态被 SMP 上并发用户 I/O 共享，两个 CPU 同时读写同一/不同文件会互相踩扇区缓冲 | 给 FAT16 后端加一把 `spinlock_t`（与 ramfs 的 `fs_lock` 同粒度） |
| UF-3 | P3 | `fs_open(O_CREAT)` 先建节点、后找句柄槽；句柄表满（32）时返回失败但文件已创建 | 先分配/预留句柄槽，再创建节点；或失败时回滚目录项 |
| UF-4 | P3 | `O_CREAT` 打开**悬空软链接**时，`resolve_path` 已把路径展开到目标，但 `split_parent_ex()` 用原始路径，在链接名下建了一个普通文件遮蔽链接 | 用展开后的路径创建；或显式返回 `E_NOENT`（不跟随悬空链接创建） |
| UF-5 | P3 | `fs_check()` 的兄弟链遍历无 guard；坏 `canon` 指针只由内存破坏产生，`fs_sync` 的 `dump_tree()` 会解引用它 | 给兄弟链遍历加上限（0..FS_MAX_NODES），`dump_tree` 用 `node_ptr_in_pool` 校验 canon |
| UF-6 | P3 | `blkfs/svcfs` 的 `s->off + len > MAX` 存在无符号回绕（`off=127,len=0xFFFFFF81` 可绕过），`memcpy` 会写穿 BSS | 改成 `len > MAX - off`；当前 `sys_fwrite_user` 按 128B 分块，用户态暂不可达，故未改 |
| UF-7 | P3 | XFS1 挂载接受重复兄弟名、`.`/`..` 名、含 `/` 的名字；不会崩，但这些名字无法通过路径访问，`fsck` 也不报告 | 挂载时做名字合法性/唯一性校验（低优先级） |

---

## 3. 覆盖范围

### 3.1 路径攻击面

| 用例 | 结果 |
|---|---|
| 空路径、`"/"`、`"system:"`、未知卷 | `fs_listdir`/`stat` 安全返回错误 |
| `":/x"`、64+ 字符盘符 | 修复后不再读未初始化 `rel`；返回 E_NOENT |
| `system:///bin//./../`、`system:/../../../../..` | 归一到卷根，不越界 |
| 12 层深目录、64 字符名字 | 创建/stat/删除正常 |
| 软链接自指/互指 | 8 跳上限，E_INVAL（既有 `fs_link_selftest`） |
| 跨卷软链接 | `"vol:"` 目标一律 E_INVAL（既有） |
| 软链接写穿 SIP | 解析后复查，E_ACCES（既有） |

### 3.2 XFS1 v6 损坏镜像

`tests/audit/fs/craft_xfs.py` 生成 13 种镜像，`tools/audit-fs.ps1 -Scenarios`
逐个启动；修复后 **13/13 PASS**（全部到达 `login:`，无 `KERNEL PANIC`，
无 `fs: cannot create system:/bin/shell`）：

```text
good, nlink_mismatch, magic_bad, version_bad, inode_size_bad,
empty, orphan, bad_size, bad_data_lba, bad_parent_type,
type_none, dup_root, volname_empty
```

覆盖：坏 magic/版本/inode_size、坏类型、孤儿父节点、size/data_lba 越界、
父节点非目录、重复卷根、空卷名、无卷根、nlink 与目录项数不符（应修复）。

### 3.3 FAT16 损坏镜像

`tests/audit/fs/test_fat16_audit.c`（真实 `fat16.c`，host 32 位）：
**27 checks / 0 failures**。覆盖：

- 坏 BPB：`reserved=0`、FAT 表过小、`sec_per_cluster=0`、`fats=0`、
  `fat_sectors=0`、`root_entries=0`、总扇区不足/超设备、簇数不在 FAT16 范围；
- 目录项：坏首簇 0/1/0xFFF0、最后合法簇、EOC/坏簇截断、自环簇链；
- 句柄：读句柄期间截断（E_BUSY）、8 句柄上限、unlink busy；
- fuzz：800 组随机 BPB、600 组随机目录项、300 组随机 FAT（每轮做 mount/
  listdir/open/read），无崩溃/无死循环（90s 超时门限）。

### 3.4 并发 / 64KB / 满盘

- 64KB 边界：既有 `fs: 64KB file limit ok` 保留并通过；
- 稀疏/超大文件：ramfs 固定 64KB 上限、FAT16 size 只受链长限制，已由
  fuzz 覆盖读路径；未做“写满 8MB FAT 盘”的专项（`cluster_alloc` 有回滚）；
- 并发 create/unlink/rename：ramfs 有全局 `fs_lock`；FAT16 **无锁**（UF-2）。
  未做 SMP 并发压力（共享树并行会话会干扰，且非本次 P0）。

### 3.5 fsck / mount 对损坏镜像

- `fs_mount_disk` 对结构性损坏（坏类型/孤儿/越界/重复卷根/空卷名/无卷根）
  整次拒绝；nlink 不一致按目录项数修复（`fs: XFS1 v6 nlink repaired on mount`）。
- `fs_check` 对 nlink 注入故障可发现并修复（既有 `fs_link_selftest`）；
  结构损坏只报告（未修 UF-5 的兄弟链 guard）。
- 挂载失败后保留默认 `system:` 卷，启动流程可继续安装/格式化，不再 panic/
  降级（FS-2）。

---

## 4. 回归入口

```powershell
# 1) host FAT16 审计（真实 fat16.c，27 checks）
powershell -NoProfile -ExecutionPolicy Bypass -File tools/audit-fs-unit.ps1

# 2) 独立 worktree 构建（避免共享树被并行会话改坏）
git worktree add .wt-audit-fs b4d5fd9
cmd /c mklink /J .wt-audit-fs\tools\i686-elf D:\OS_Project\tools\i686-elf
copy keys\shell_private.pem,keys\shell_public.pem .wt-audit-fs\keys\
powershell -NoProfile -ExecutionPolicy Bypass -File tools/audit-fs.ps1 `
    -Repo .wt-audit-fs -Build -Unit -Scenarios
```

`tools/audit-fs.ps1` 约定：构建目录 `.obj-audit-fs`/`.dist-audit-fs`，
QMP **4542**，串口 `obj/audit/<scenario>.log`，不监听 12345。

### 串口证据行（修复后）

```text
fs: audit listdir bad volume prefixes rejected ok
fs: audit corrupt image mounts rejected, default volume kept ok
fs: audit valid image mount + nlink repair ok
fs: audit hardlink nlink / ino_promote ok
fs: audit deep/long/.. path edges ok
fs: audit syscall chroot/time/fsck buffer guards ok
fs: audit selftest PASS
fs: backend selftest ok
fs: link selftest PASS
```

### 既有回归

- `tools/audit-fs-unit.ps1`：**27 checks / 0 failures**（真实 `fat16.c`）。
- `tools/unit.ps1 -Only fat16,path`：**241 checks PASS**（FAT16 162 + path 79）。
  早先 `pipe` 未改动时三套合计 287 checks PASS；最后复跑时 `pipe` 因
  audit-syscall 正在修改 `fs/pipe.c` 而编译失败（不属于本提交改动）。
- fs 自检：`fs: 64KB file limit ok`、`fs: error-path selftest ok`、
  `selftest: rename ok`、`fs: xfs v5 image mount compat ok`、
  `fs: xfs v6 hardlink persisted ok` 全部保留。

---

## 5. 改动文件

| 文件 | 内容 |
|---|---|
| `src/kernel/fs/ramfs.c` | FS-1/2/4/9：类型/卷根/卷名校验、失败恢复默认卷、`rel` 初始化 |
| `src/kernel/fs/fat16.c` | FS-5/6/7/8：BPB/FAT 覆盖、首簇、EOC、截断忙 |
| `src/kernel/fs/fs_syscall.c` / `.h` | FS-3/10/11：chroot cwd、time 长度、fsck 返回值 |
| `src/kernel/fs/fs_audit_selftest.c` | 新增：6 类坏镜像 + good/nlink、路径边界、硬链接、syscall 缓冲 canary |
| `src/kernel/fs/fs_link_selftest.c` | v5 兼容自检改为写真实 v5 卷根 |
| `src/kernel/fs/vfs_selftest.c` | 挂接 `fs_audit_selftest()` |
| `tests/audit/fs/craft_xfs.py` | 13 种 XFS1 镜像生成器 |
| `tests/audit/fs/test_fat16_audit.c` | FAT16 host 审计 + fuzz |
| `tools/audit-fs.ps1` / `tools/audit-fs-unit.ps1` | 场景驱动 / host 单测入口 |
