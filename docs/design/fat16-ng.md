# ng FAT16（WP2-2）

> 状态：E2 / WP2-2 实现记录，2026-10-31。
> 范围：L1 块设备抽象 + FAT16（VFS 后端）+ 1.44MB RAM 盘镜像 + ring3 FAT demo。
> 不做：块设备驱动（WP2-4）、fb/输入（WP2-3）、shell（WP2-6）、LFN/rmdir/权限模型。

## 1. 目录与依赖

    src/services/fs/include/blk.h       struct blk_env（read/write/sectors/ctx）
    src/services/fs/include/fat16.h     FAT16 几何/节点/统计/API
    src/services/fs/fat16.c             FAT16 实现（VFS 后端）
    src/ng/fat_ramdisk.[ch]             嵌入镜像 -> PMM RAM 盘 -> blk_env（L0 侧绑定）
    src/ng/fat16_selftest.[ch]          内核自检（5 条证据行 + 计数）
    tools/mkfat-ng.ps1                  1.44MB FAT16 测试镜像生成器
    tests/unit-ng/test_fat16.c          合成镜像 host 单测（含坏 BPB/坏链/满盘）

依赖方向：`fat16.c -> fat16.h -> {blk.h, vfs.h, types.h}`。**不** include
`src/kernel/**`、`src/ng/**` 或任何设备/内核内部头；也不使用内核 heap。
块设备由调用方注入：

```c
struct blk_env {
    int (*read)(void *ctx, u64 sector, void *buf, u32 count);
    int (*write)(void *ctx, u64 sector, const void *buf, u32 count);
    u64 sectors;
    void *ctx;
};
```

- 内核：`fat_ramdisk.c` 把嵌入镜像复制到 360 个 PMM 页（1.44MB），
  read/write 按“扇区 -> 页号/页内偏移”寻址；
- host 单测：`test_fat16.c` 的 fake disk 直接读写静态字节数组；
- WP2-4 的 ATA PIO 驱动只要实现同样四个字段即可复用 FAT16：现在由
  `src/ng/ata_blk.c` 提供 `ata_blk_env()`，真实磁盘挂到 `/mnt/disk`
  （见 `docs/design/block-ng.md`）。

## 2. 镜像几何（1.44MB）

`tools/mkfat-ng.ps1` 生成 `obj-ng/fat16.img`：

| 项 | 值 |
|---|---|
| 总扇区 | 2880（512B/扇区，1474560 字节） |
| 扇区/簇 | 1（512 字节/簇） |
| 保留扇区 | 1 |
| FAT 副本 | 2 × 12 扇区 |
| 根目录 | 224 项（14 扇区） |
| 数据区起点 | 39 |
| 数据簇数 | **2841**（簇号 2..2842） |
| 内容 | `HELLO.TXT`（18B）、`DIR/A.TXT`（6B，子目录） |

> **FAT12/FAT16 边界说明**：1.44MB 软盘按微软原始规范是 FAT12（簇数 <
> 4085）。WP2-2 的“FAT16”判定标准是 **16 位 FAT 表项几何**：FAT 区必须覆盖
> `clusters+2` 个 16 位项，本镜像用 `fat_sectors=12`（FAT12 软盘是 9）容纳
> 2843 个表项。mount 因此**不**硬性要求 clusters ≥ 4085，而是校验
> `2..65524` 与 FAT 区大小覆盖；这是为了让 1.44MB 测试镜像可用，已在
> host 单测里用 28/30 簇的小镜像覆盖同一代码路径。

镜像由 `build-ng.ps1` 以 `.incbin` 放进 **`.fat16` 段（链接地址 2 MiB）**，
作为独立 PT_LOAD 由 QEMU 加载：

- 内核正文/`.rodata`/`.bss` 仍全部 < 2 MiB（`link.ld` 的 ASSERT 不变），
  保持 PD[0] 的 4 KiB W^X 窗口；
- 镜像段落在 PD[1] 的 2 MiB 恒等大页（RW+NX），不参与 W^X 校验；
- `mem_selftest.c` 在 PMM 里 `pmm_reserve(0x200000, 1474560)`，防止被分配；
- 启动时 `fat_ramdisk_init()` 复制到 PMM 页，FAT16 的写操作只改 PMM 副本，
  不改只读的 `.rodata`。

## 3. 挂载校验

`fat16_mount()` 先读 0 号扇区，全部校验通过后才提交几何（失败不破坏旧状态），
任何一条不满足返回 `EINVAL`：

1. 引导扇区 510/511 = `0x55 0xAA`；
2. `bytes_per_sector == 512`；
3. `sectors_per_cluster` 非 0、≤128、2 的幂、每簇 ≤ 32 KiB；
4. `reserved != 0`；
5. `num_fats` 1..4；
6. `root_entries` 1..512（FAT16 固定根目录区）；
7. `total_sectors`（16 位为 0 时用 32 位）非 0 且 ≤ 设备扇区数；
8. `fat_sectors != 0`，且 `fat_sectors*512 ≥ (clusters+2)*2`；
9. `data_start = reserved + fats*fat_sectors + root_sectors < total_sectors`；
10. `clusters = (total-data_start)/sectors_per_cluster` 在 1..65524。

`EIO` 用于设备读失败；空实例/空 `blk_env`/缺 read/write/`sectors=0` 返回
`EINVAL`。mount 失败只增加 `mount_fails`，已挂载的实例保持不变。

## 4. FAT 表缓存 / 簇链

- **表缓存**：一个 512 字节扇区缓存（`fat_cache`/`index`/`valid`/`dirty`）。
  `fat_get` 命中缓存；`fat_set` 读-改-写后立刻回写**所有** FAT 副本
  （FAT16 冗余表保持一致）。
- **遍历**：`chain_at(first,index)` / `chain_length` / `chain_last`，全部用
  `cluster_count` 做预算：
  - `0xFFF8..0xFFFF` = EOC（正常结束）；
  - `0xFFF7` = 坏簇 → `EIO`；
  - `0` = 空闲项出现在链中 → `EIO`；
  - `<2` / `> cluster_count+1` = 越界 → `EIO`；
  - 回到链首/超过预算 = 循环 → `EIO`（`chain_loops` 计数）。
- **分配**：从 `last_alloc` 轮转扫描空闲表项，标记 EOC 并清零整簇；任何一步
  失败都把刚占用的表项还回去（不泄漏簇）。满盘 → `ENOSPC`。
- **释放**：整链逐项清 0，遇到坏项/越界停下；循环用预算检测。
- 文件大小/首簇写在目录项里（cluster_hi 恒 0）；数据读写成扇区级
  read-modify-write，跨簇按 `offset / bytes_per_cluster` 走链。

## 5. 目录项与 8.3

目录项 32 字节：name[11] + attr + cluster_lo(26) + size(28)，cluster_hi 恒 0。

- 首字节 `0x00` = 目录结束；`0xE5` = 已删除（可复用）；
- `attr == 0x0F` = LFN 项：**跳过**（不还原长名）；`attr & 0x08` = 卷标：
  **跳过**；`"."` / `".."` 在 readdir 里跳过；
- 名字转换：大小写归一为大写、空格填充；base ≤ 8、ext ≤ 3；多个点、空 base、
  非法字符（`" * + , / : ; < = > ? [ \ ] |`、控制字符/非 ASCII）→ `EINVAL`；
  超长 → `ENAMETOOLONG`；lookup 一个不可能存在的 8.3 名字 → `ENOENT`；
- 根目录是固定区域（`root_start..root_start+root_dir_sectors`），子目录是簇
  链；子目录写满后追加一个簇（`dir_extend`），根目录写满 → `ENOSPC`；
- 新建子目录写 `"."`（自身首簇）与 `".."`（父目录首簇，根为 0）。

## 6. 节点与操作

节点身份 = `(parent, entry_off)`：父目录标识（0 = 根，否则父目录首簇）+ 目录
项在目录流里的字节偏移。改名只改 11 字节名字，身份不变，所以打开中的节点不会
失效。节点缓存 32 项（0 号固定为根）；`refs>0` 或调用方正在使用的节点不会被
回收；池满 → `ENOSPC`。

| 操作 | 行为 | 主要错误 |
|---|---|---|
| mount | 见 §3 | EINVAL/EIO |
| lookup | 逐项目录项匹配 8.3，命中复用节点 | ENOENT/ENOTDIR/EIO |
| create | 文件/目录；找 0xE5 或 0x00 空位；子目录可扩簇 | EEXIST/ENOSPC/ENAMETOOLONG/EINVAL |
| read | 按 size 截断，跨簇走链；链坏 → EIO | EISDIR/EIO |
| write | 先扩链（失败回滚），稀疏区间补零，写后回写目录项 size/首簇 | ENOSPC/EIO/EISDIR |
| truncate | 0/缩小（释放尾簇+清尾）/扩大（分配+补零） | ENOSPC/EIO |
| unlink | 先写 0xE5 再释放簇链；打开中 → EBUSY；目录 → EISDIR | ENOENT/EISDIR/EBUSY/EIO |
| rename | 同目录、8.3 名字；大小写相同视为同一项；目标存在 → EEXIST | EEXIST/ENAMETOOLONG/EINVAL/EISDIR |
| readdir | 枚举可见项，跳过卷标/LFN/`.`/`..`；名字缓冲不足 → ENAMETOOLONG | ENOTDIR/EIO |

错误码全集：`ENOENT / ENOSPC / EIO / ENAMETOOLONG / EISDIR / ENOTDIR /
EEXIST / EBUSY`（另有 `EINVAL` 表示不支持的组合/坏参数）。

## 7. VFS 接线

- `fat16_ops()` 填 `lookup/create/unlink/rename/readdir/read/write/truncate/
  retain/release`；`readlink/symlink` 为 NULL（VFS 对符号链接返回 EROFS）。
- `vfs.h` 新增 `VFS_EIO (-5)`（WP2-2 需要，与 Linux 同值）。
- 启动顺序（`main.c`）：WP2-1 `fs_selftest()` 建根 ramfs + `/tmp` →
  WP2-2 `fat16_selftest()` 建 `/mnt/fat` 并挂 FAT16 → WP1-7 ring3 demo。
- ring3 demo（`hello.S`）走真实 syscall 20..27：
  `open("/mnt/fat/HELLO.TXT") -> read 18 字节校验 -> close ->
   open("/mnt/fat/NEW.TXT", O_CREAT|O_RDWR) -> write("fat16") -> close ->
   stat(size=5) -> unlink -> stat == -ENOENT`。
- 单 AS / 单核协作式：与 ramfs 相同，FAT16 无锁（IF=0 + 显式调度点）。

## 8. 自检与单测

内核自检（run-ng 命中）：

    ng: fat16 mount ok (clusters=2841)
    ng: fat16 read ok
    ng: fat16 write ok
    ng: fat16 bad bpb rejected (n=11)
    ng: fat16 chain edge ok (n=4)
    ng: WP2-2 selftest PASS

- mount/read/write 用真实 1.44MB 镜像（HELLO.TXT / DIR/A.TXT / NEW.TXT 全链路）；
- bad bpb：11 种坏几何（magic/bps/spc/reserved/FAT 数/root/FAT size/total/
  越界/布局）逐个 `EINVAL`；
- chain edge：合成 64 扇区镜像，循环/截断/坏簇/越界簇号读第二簇 → `EIO`。

host 单测 `tests/unit-ng/test_fat16.c`（+386 checks）覆盖：坏 BPB 21 例、
几何/统计、8.3 名字边界、0x00/0xE5/卷标/LFN、子目录与跨目录、读/写/稀疏写/
跨簇、截断缩小/扩大/0、删除/改名/打开中 EBUSY、满盘 ENOSPC 与回收、簇链循环/
截断/坏簇/越界/空闲项、目录链循环、VFS 集成（mount 到 "/" 走完整 vfs_*）。

## 9. 已知限制 / 遗留

- **单核协作式**：无锁；多核/抢占时在公开入口加 spinlock。
- **无 LFN**：带长名的目录项跳过；创建/改名 > 8.3 返回 `ENAMETOOLONG`。
- **无 rmdir / 跨目录改名**：目录删除 `EISDIR`；跨目录改名 `EINVAL`。
- **无时间戳/属性编辑/Unix 权限**：mode 固定（目录 0755、文件 0644，
  read-only 属性 0444），uid/gid 不参与。
- **unlink 打开中的文件返回 EBUSY**（不模拟延迟回收）：FAT 没有 inode，
  簇链在目录项删除后立即需要回收。
- **FAT12/FAT16 簇数边界**：见 §2，1.44MB 镜像按 16 位表项几何生成。
- **无 page cache / 预读**：每次读写直接走扇区（FAT 表有单扇区缓存）。
- **镜像段占 2 MiB 物理段**：1.44MB `.fat16` PT_LOAD 必须由 PMM 保留；
  WP2-4 已把“真实块设备上的 FAT16”接上（`/mnt/disk`，见 block-ng.md），
  但 WP2-2 的 1.44MB RAM 盘镜像与 `/mnt/fat` 保留为回归路径，不删除该段。
