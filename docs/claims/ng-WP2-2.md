# ng-WP2-2：FAT16 移植（PASS）

## 结论

**PASS** —— E2 / WP2-2 出口达成，FAT16 成为 VFS 的第二个后端（L1
`src/services/fs`）：

- **块设备抽象**：`struct blk_env { read(sector,buf,n); write(sector,buf,n);
  sectors; ctx; }`（`src/services/fs/include/blk.h`）。内核绑到嵌入
  `.fat16` 段的 1.44MB 镜像（启动时复制到 360 个 PMM 页），host 单测绑到
  fake 镜像；`fat16.c` 不 include 任何内核头，也不依赖 PMM/heap。
- **FAT16**：BPB 几何校验（magic/bytes-per-sector/sectors-per-cluster/
  reserved/FAT 数/root entries/total sectors/FAT size/每簇字节/簇数上限）、
  FAT 表单扇区缓存 + 多副本回写、簇链遍历（EOC/坏簇/空闲/循环/越界）、
  空闲簇分配（清零）与整链释放、8.3 目录项（0x00/0xE5/卷标/LFN）、根目录 +
  子目录、`mount/read/write/create/unlink/rename/truncate/readdir` 接到
  `struct vfs_fs_ops`，错误码 `ENOENT/ENOSPC/EIO/ENAMETOOLONG/EISDIR/
  ENOTDIR/EEXIST/EBUSY`（+`EINVAL`）。
- **镜像与接线**：`tools/mkfat-ng.ps1` 生成 1.44MB 镜像（`HELLO.TXT` 18B、
  `DIR/A.TXT` 6B），`build-ng.ps1` 用 `.incbin` 放入 2 MiB 的 `.fat16`
  PT_LOAD（PMM 保留），启动挂到 `/mnt/fat`；ring3 demo 读 `HELLO.TXT` 校验、
  创建 `NEW.TXT` 写入、stat、unlink。
- **自检**：run-ng **62/62 期望行 + 20 条正则**；host 单测 **1438 → 1824
  checks**（本 WP +386）；旧 32 位 **305/305**；check-domains / check-deps
  0 违规；ci-ng **6/6 PASS**。

`src/kernel/**`、`build.ps1`、`tools/ci.ps1`、`abi.h` 未修改；不做块设备驱动
（WP2-4）、fb/输入（WP2-3）、shell（WP2-6）。

## 交付物

| 路径 | 内容 |
|---|---|
| `src/services/fs/include/blk.h` | L1 块设备抽象 `blk_env`（**新**） |
| `src/services/fs/include/fat16.h` | FAT16 几何/节点/统计/API（**新**） |
| `src/services/fs/fat16.c` | FAT16 实现：mount 校验、FAT 缓存、簇链、8.3、目录/文件 ops（**新**） |
| `src/services/fs/include/vfs.h` | 追加 `VFS_EIO (-5)`（**改**） |
| `src/ng/fat_ramdisk.[ch]` | 嵌入镜像 -> PMM RAM 盘 -> `blk_env`（**新**） |
| `src/ng/fat16_selftest.[ch]` | 内核自检：真实镜像读/写 + 坏 BPB + 坏链（**新**） |
| `src/ng/link.ld` | 新增 `.fat16` 段（2 MiB，独立 PT_LOAD），内核主体仍 < 2 MiB（**改**） |
| `src/ng/mem_selftest.c` | PMM 保留 FAT 镜像段（**改**） |
| `src/ng/main.c` | WP2-2 自检接线 + PASS 行（排在 WP1-7 前）（**改**） |
| `src/ng/user/hello.S` | ring3 FAT demo：读/写/stat/unlink（**改**） |
| `tests/unit-ng/test_fat16.c` | 合成镜像 host 单测：坏 BPB/坏链/满盘/跨目录/VFS 集成（**新**） |
| `tests/unit-ng/unit_main.c` | 加 `test_fat16()`（**改**） |
| `tools/mkfat-ng.ps1` | 1.44MB FAT16 镜像生成器（**新**，BOM） |
| `tools/build-ng.ps1` | 生成/校验/嵌入镜像；加 `fat16.c`/`fat_ramdisk.c`/`fat16_selftest.c`（**改**，BOM） |
| `tools/run-ng.ps1` | +6 固定行、+4 用户行、+3 正则、WP2-2 失败模式（**改**，BOM） |
| `tools/unit-ng.ps1` | 加 `fat16.c` 与 `test_fat16.c`（**改**，BOM） |
| `docs/design/fat16-ng.md` | 块设备抽象/FAT16/镜像/RAM 盘/限制（**新**） |
| `docs/design/vfs-ng.md` | 错误码加 `EIO`、FAT16 指向新文档（**改**） |
| `docs/design/evolution.md` | E2 进度：WP2-2 完成（**改**） |
| `docs/design/README.md` | 当前状态/文档索引（**改**） |

## 环境

- 分支 `ng`，基线 HEAD `80a2237`（WP2-1），仓库 `D:\OS_Project`。
- clang 23.1.4 / ld.lld 23.1.4（`D:\msys64\mingw64\bin`）；内核
  `--target=x86_64-elf -ffreestanding -nostdlib -mno-red-zone
  -fno-stack-protector`；host 单测同一 clang 的默认 PE 目标。
- QEMU 11.0.0，`qemu-system-x86_64 -kernel -m 256 -cpu max`。

## 证据（2026-10-31）

### 构建

```text
powershell -File tools/build-ng.ps1
```

```text
=== build-ng (WP2-2) ===
clang: D:\msys64\mingw64\bin\clang.exe
lld:   D:\msys64\mingw64\bin\ld.lld.exe

=== user-elf (WP1-7) ===
user-elf: PASS (14496 bytes, 3 PT_LOAD, entry 0x40000000, end 0x400021b0)

=== fat16 image (WP2-2) ===
=== mkfat-ng (WP2-2) ===
geometry: 2880 sectors x 512, spc=1, FATS=2x12, root=224 (14 sec)
          data.start=39, clusters=2841, bytes/cluster=512
files: HELLO.TXT (18 bytes), DIR/A.TXT (6 bytes)
mkfat-ng: PASS -> D:\OS_Project\obj-ng\fat16.img (1474560 bytes)
fat16 image: PASS (1474560 bytes, 2880 sectors, 2841 clusters)

  [1/33] entry.S
  ...
  [27/33] fat_ramdisk.c
  [28/33] fat16_selftest.c
  [29/33] main.c
  [30/33] src/services/fs/vfs.c
  [31/33] src/services/fs/ramfs.c
  [32/33] src/services/fs/fat16.c
  [33/33] link -> dist-ng/aster-ng.elf

ELF64: PASS (1630512 bytes)
  entry    0x0000000000101000
  phnum    7
segments: PASS
  5 PT_LOAD, 4 KiB aligned (per-section pages)
multiboot2: PASS
pvh-note: PASS

build-ng: PASS -> D:\OS_Project\dist-ng\aster-ng.elf
```

镜像作为独立 `.fat16` PT_LOAD 放在 **2 MiB**（链接地址 0x200000），内核
正文/`.rodata`/`.bss` 仍全部 < 2 MiB；`link.ld` 的
`ASSERT(__kernel_end <= 0x200000)` 继续成立。`mem_selftest.c` 在 PMM 里保留
`[__fat16_img_start, __fat16_img_end)`（360 页），启动时 `fat_ramdisk_init()`
复制到 PMM。用户 ELF 仍 3 个 PT_LOAD（`.text 0x440 / .rodata 0x139 /
`.data+.bss filesz 0x8 memsz 0x1b0`）。

### 串口（`powershell -File tools/run-ng.ps1`，`-cpu max`）

WP1-2..WP1-7 全部保持 PASS。WP2-2 段：

```text
ng: WP2-1 selftest PASS
ng: fat16 mount ok (clusters=2841)
ng: fat16 read ok
ng: fat16 write ok
ng: fat16 bad bpb rejected (n=11)
ng: fat16 chain edge ok (n=4)
ng: WP2-2 selftest PASS
ng: elf64 header ok
...
ng: ring3 hello
ng: fs open ok
ng: fs write ok
ng: fs read ok
ng: fs stat ok (size=5)
ng: fs close ok
ng: fs listdir ok (n=1)
ng: fs fat read ok
ng: fs fat write ok
ng: fs fat stat ok
ng: fs fat unlink ok
ng: fs user demo ok
ng: ring3 exit ok
ng: WP1-7 selftest PASS
------------------------------------------------
run-ng: matched 62/62 expected lines + 20 patterns
run-ng: PASS (all expected lines in 0.2s, cpu max)
```

`run-ng.ps1` 新增的期望（全部命中才 PASS）：

```text
6 条固定行 + 4 条用户态固定行：
  ng: fat16 mount ok / read ok / write ok / chain edge ok / bad bpb rejected /
  ng: WP2-2 selftest PASS
  ng: fs fat read ok / ng: fs fat write ok / ng: fs fat stat ok /
  ng: fs fat unlink ok
3 条正则：
  ng: fat16 mount ok \(clusters=[0-9]+\)
  ng: fat16 chain edge ok \(n=[0-9]+\)
  ng: fat16 bad bpb rejected \(n=[0-9]+\)
失败早停模式新增：
  ng: fat16 .* FAIL / ng: WP2-2 selftest FAIL
```

### host 单测

```text
powershell -File tools/unit-ng.ps1
=== unit-ng (WP1-8 host tests) ===
clang: D:\msys64\mingw64\bin\clang.exe
1824 checks, 0 failures
unit-ng: PASS (1824 checks, 0 failures)
```

WP2-1 为 1438，本 WP **+386**（`tests/unit-ng/test_fat16.c`）。关键断言
（全部具体错误码，不按“负值即失败”放过）：

- 坏 BPB 21 例：magic、bytes/sector、spc=0/非 2 的幂/>128、reserved=0、
  FAT 数 0/5、root entries 0/513、FAT size 0/覆盖不了簇、total=0/超过设备/
  布局越界、每簇 64 KiB、簇数 >65524、空实例/空设备/缺 read → 具体
  `EINVAL`/`EIO`；
- 几何：512 扇区/2 扇区每簇/2 份 FAT/64 root entries → cluster_count=252、
  bytes/cluster=1024、data_start=7、根 ino=1；
- 读：整文件/部分/EOF/跨簇（3000B 三簇）/子目录 A.TXT；`EISDIR`/`ENOTDIR`；
- 8.3：大小写归一（`hello.txt` == `HELLO.TXT`）、base 9 / ext 4
  `ENAMETOOLONG`、非法字符 `EINVAL`、空名字/空 base `EINVAL`；
- 写：600B、稀疏写 offset 1500（[600,1500) 全 0）、5000B 跨 7 簇、读回一致、
  目录项 size 更新；
- 截断：5000→100（尾部释放）、100→3000（补零）、→0（链释放/幂等）、
  4 GiB `ENOSPC`；
- 删除/改名：同目录改名、大小写相同 OK、目标存在 `EEXIST`、>8.3
  `ENAMETOOLONG`、跨目录 `EINVAL`、目录 `EISDIR`、打开中 `EBUSY`；
- 满盘：28 簇小镜像写满 14336B 后新文件写 1B `ENOSPC`、扩截断 `ENOSPC`，
  删除后 28 簇全部回收并可再写满；
- 簇链：自环 `EIO`（chain_loops）、截断 `EIO`（chain_short）、坏簇/
  越界簇号/链中空闲项 `EIO`、目录簇链循环 readdir `EIO`；
- VFS 集成：FAT 挂到 `/` 后 `vfs_open/read/stat/listdir/mkdir/truncate/
  rename/unlink`、目录 fd 写 `EISDIR`、打开中 unlink `EBUSY`、
  symlink `EROFS`、根卸载 `EBUSY`。

### 一条命令门禁

```text
powershell -File tools/ci-ng.ps1
=== ci-ng summary ===
  PASS check-domains    0 violations; exit=0, 0.5s
  PASS check-deps       0 violations; exit=0, 0.5s
  PASS unit-ng          1824 checks; exit=0, 2.9s
  PASS build-ng         1630512 bytes, user-elf 14496 bytes; exit=0, 3.2s
  PASS run-ng           62/62 lines + 20 patterns; exit=0, 0.6s
  PASS legacy-32bit     305/305 parity checks; exit=0, 52.3s
  6 passed, 0 failed, 0 skipped, 59.9s

=== NG CI PASS ===
```

说明：第一次 ci-ng 的 legacy 步骤出现 **3/305** 失败（随机项），紧接着两次
独立 `build.ps1 -Test` 均 **305/305**（55.0s / 52.3s）且 ci-ng 复跑 6/6
PASS；与 `docs/parallel-sessions.md` §“已知偶发问题”记录的旧树用户态偶发
同族，本 WP 未改 `src/kernel/**`。

### 旧树 / 边界 / 负向

```text
powershell -File build.ps1 -Test
=== compile: 1 compiled, 103 skipped ===
=== elapsed: 52.3s ===
=== TEST PASS (305/305) ===

powershell -File tools/check-domains.ps1
checked: 212 files, 735 quoted includes
domain include violations: 0

powershell -File tools/check-deps.ps1
checked: 201 files, 650 quoted includes
interface layer violations (include/*.h -> *_internal.h): 0
module include violations: 0
OK: no cross-module include violations.
```

`-cpu qemu64`（SMAP=off）同样全绿：

```text
powershell -File tools/run-ng.ps1 -Cpu qemu64
ng: fs fat read ok
ng: fs fat write ok
ng: fs fat stat ok
ng: fs fat unlink ok
run-ng: matched 62/62 expected lines + 20 patterns
run-ng: PASS (all expected lines in 0.2s, cpu qemu64)
```

## FAT16 几何 / 限制

| 项 | 值 |
|---|---|
| 镜像 | 2880 扇区 × 512B = 1474560 字节（1.44MB） |
| 扇区/簇 | 1（512 字节/簇） |
| 保留 / FAT | 1 扇区；2 份 × 12 扇区 |
| 根目录 | 224 项（14 扇区） |
| 数据区 | 起点 39；**2841 个簇**（簇号 2..2842） |
| 内容 | `HELLO.TXT`（18B）、`DIR/A.TXT`（6B） |
| 文件名 | 8.3（base ≤8、ext ≤3，大写归一）；>8.3 `ENAMETOOLONG` |
| 文件大小 | ≤ 4 GiB−1（32 位目录项字段） |
| 簇号 | 2..65525（FAT16 表项；0xFFF7 坏簇、0xFFF8+ EOC） |
| 单次 syscall IO | read/write ≤ 512B，listdir ≤ 8 条（沿用 WP2-1） |
| 节点缓存 | 32（根固定 0 号；池满 `ENOSPC`） |
| 目录 | 根固定区 + 子目录簇链；无 rmdir / 跨目录改名 |
| 权限 | 无 uid/gid；目录 0755、文件 0644、read-only 属性 0444 |
| 时间戳/属性 | 不读取、不写 |
| 并发 | 单核协作式，无锁 |

**FAT12/FAT16 边界**：1.44MB 软盘按微软原始规范落在 FAT12 簇数区间
（<4085）。本 WP 的“FAT16”判定是 **16 位表项几何 + FAT 区覆盖
clusters+2 项**；镜像用 `fat_sectors=12`（FAT12 软盘为 9）容纳 2843 项。
mount 因此不硬性要求 clusters ≥ 4085，而是在 1..65524 内校验几何
（见 `docs/design/fat16-ng.md` §2/§3）。

## 镜像生成方式

```powershell
powershell -File tools/mkfat-ng.ps1            # -> obj-ng/fat16.img
powershell -File tools/mkfat-ng.ps1 -Out tmp\fat.img
```

脚本按扇区手写 BPB/两份 FAT/根目录/`DIR` 子目录/文件内容，生成后自校验
文件大小、`0x55AA`、BPB 字段、两份文件内容。`build-ng.ps1` 每次构建都会
重新生成并校验镜像，然后：

1. 生成 `obj-ng/fat16_img_blob.S`：`.section .fat16` + `.incbin`，
   符号 `ng_fat16_img_start/end`；
2. 编译成对象并链接进内核（`.fat16` 段起始 0x200000，独立 PT_LOAD）；
3. 内核启动时 `fat_ramdisk_init()` 把镜像复制到 360 个 PMM 页；
4. `fat16_selftest()` 把 RAM 盘挂到 `/mnt/fat`，ring3 demo 继续使用。

## 出口

```text
powershell -File tools/build-ng.ps1      # PASS，1630512 bytes，33 步，5 PT_LOAD
powershell -File tools/run-ng.ps1        # PASS，62 行 + 20 正则（含 FAT 5 条 + ring3 4 条）
powershell -File tools/unit-ng.ps1       # PASS，1824 checks, 0 failures
powershell -File build.ps1 -Test         # PASS，305/305（旧 32 位）
powershell -File tools/check-domains.ps1 # 0 违规（212 files）
powershell -File tools/check-deps.ps1    # 0 违规
powershell -File tools/ci-ng.ps1         # 6/6 PASS
```

## 未做 / 遗留（按 WP 边界）

- **块设备（WP2-4）**、**fb/输入（WP2-3）**、**shell（WP2-6）** 未动；FAT16
  已通过 `blk_env` 与设备解耦，ATA/virtio-blk 只要实现 read/write/sectors 即可。
- **无 LFN**：带长名的目录项跳过，创建/改名 >8.3 `ENAMETOOLONG`。
- **无 rmdir / 跨目录改名**：`EISDIR` / `EINVAL`；改名只支持同目录文件。
- **unlink 打开中的文件 `EBUSY`**：FAT 没有 inode，簇链在目录项删除后立即
  需要回收（不模拟延迟删除）。
- **无时间戳/属性编辑/Unix 权限**；无 page cache/预读（FAT 表有单扇区缓存）。
- **单 AS / 单核协作式**：与 ramfs 相同，多核/抢占时在公开入口加 spinlock。
- **镜像段**：1.44MB `.fat16` PT_LOAD 在 2 MiB，必须由 PMM 保留；WP2-4 换成
  真实块设备后删除该段即可。
