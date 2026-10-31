# ng 块设备：ATA PIO + 真实磁盘上的 FAT16（WP2-4）

> 状态：E2 / WP2-4 实现记录，2026-10-31。
> 范围：L1D ATA PIO 驱动（IDENTIFY / LBA28 READ / WRITE / FLUSH）+ L0 的
> `blk_env` 适配 + 8MB FAT16 磁盘镜像（QEMU PIIX3 IDE 主盘）+ `/mnt/disk`
> 挂载 + ring3 disk demo + 有界超时/错误恢复。
> 不做：virtio-blk / AHCI / LBA48 传输 / DMA / IRQ / 分区表 / fsd 进程化 /
> shell（WP2-6）。

## 1. 目录与依赖

    src/drivers/include/ata.h            ATA 寄存器/命令/错误码/ata_io/ata_dev
    src/drivers/block/ata.c              ATA PIO 驱动（L1D，纯寄存器 + 注入 IO）
    src/ng/ata_blk.h                     ata_dev -> blk_env 适配接口
    src/ng/ata_blk.c                     read/write/sectors 包装（L0，host 可编译）
    src/ng/wp24_selftest.c               PCI PIIX3 + ATA 自检 + /mnt/disk 挂载
    src/ng/wp24.h                        自检入口
    tools/mkfat-ng.ps1                   -SizeMB 8 -> dist-ng/disk.img（默认）
    tests/unit-ng/test_ata.c             fake ATA 控制器 + fake 盘上的 FAT16

依赖方向：

    src/drivers/block/ata.c  ->  ata.h -> types.h            （L1D -> L0）
    src/ng/ata_blk.c         ->  ata.h + blk.h + types.h     （L0 -> L1D/L1）
    src/ng/wp24_selftest.c   ->  pci.h + ata.h + blk.h + fat16.h + vfs.h + io.h

`struct blk_env` 属于 L1（`src/services/fs/include/blk.h`），而 check-domains
规定 L1D 只允许依赖 L0/A/B，所以**适配层放在 src/ng（L0）**，不放进
`src/drivers`。ATA 驱动本身只认识端口和内存缓冲，不认识 VFS/页表/PMM。

## 2. ATA PIO 驱动

### 2.1 端口与寄存器

QEMU `-M pc` 的 PIIX3 IDE 在兼容（legacy）模式下把主通道固定在：

| 端口 | 用途 |
|---|---|
| `0x1F0` | DATA（16 位 PIO） |
| `0x1F1` | ERROR（读）/ FEATURES（写） |
| `0x1F2` | SECCOUNT0（本次扇区数 1..8） |
| `0x1F3/0x1F4/0x1F5` | LBA[7:0]/[15:8]/[23:16] |
| `0x1F6` | HDDEVSEL：`0xE0 \| drive<<4 \| LBA[27:24]` |
| `0x1F7` | COMMAND（写）/ STATUS（读） |
| `0x3F6` | ALTSTATUS（读，无副作用）/ DEVCTL（写） |

状态位：`BSY=0x80`、`DRDY=0x40`、`DF=0x20`、`DRQ=0x08`、`ERR=0x01`。
命令：`IDENTIFY=0xEC`、`READ SECTORS=0x20`、`WRITE SECTORS=0x30`、
`FLUSH CACHE=0xE7`。

### 2.2 LBA28 编码

- `0x1F6 = 0xE0 | (drive & 1) << 4 | ((lba >> 24) & 0x0F)`；
- `0x1F3..0x1F5 = lba[7:0] / [15:8] / [23:16]`；
- `0x1F2 = count`（1..8；本驱动不支持 0=256 的多扇区整块，FAT16 每次只用 1）。

纯函数 `ata_devsel(drive, lba)` / `ata_lba_range_ok(lba, count, sectors)` 在
host 单测里直接断言（含 drive 只取 bit0、LBA 高位、跨设备末尾、> LBA28 上限）。

### 2.3 状态机与有界轮询

每次端口操作后按 ATA 规范读 4 次 `0x3F6` 做 ~400ns 延时。所有等待都走
同一个函数：

```c
ata_wait(dev, want_set, want_clear)
  for (i = 0; i < dev->poll_limit; i++) {
      st = status();
      if (!(st & BSY)) {
          if (st & (ERR|DF)) -> dev->errors++, return ATA_EIO;
          if ((st & want_set) == want_set && !(st & want_clear)) return ATA_OK;
      }
      delay_400ns();
  }
  dev->timeouts++, return ATA_ETIMEDOUT;
```

- `poll_limit` 默认 1,000,000（≈0.4s 硬件时间），单测/内核 fake 可调小；
- **超时不 panic、不无限等**：返回 `ATA_ETIMEDOUT`，调用方把整个块设备操作
  变成 `-1`（FAT16 映射 `EIO`）；
- `ERR/DF` 在 BSY 落地的第一时间返回 `ATA_EIO`，不误报成超时；
- 命令序列：select + 400ns → count/LBA → command → 等 DRQ（读/写）或
  BSY 落地（FLUSH）→ 逐扇区 256 个 16 位字 → 收尾等 BSY=0 且 DRQ=0；
- 写路径结束后发 `FLUSH CACHE (0xE7)`，确保镜像真的落盘。

### 2.4 IDENTIFY

`0xEC` 后解析：

- word0 必须是 ATA 签名（0x0000/0xFFFF 视为浮空/非 ATA → `ATA_ENODEV`）；
- word49 bit9 = LBA 支持，不支持 → `ATA_ENODEV`；
- word60/61 = LBA28 总扇区数，0 → `ATA_ENODEV`；
- word83 bit10 = LBA48 能力（只记录 `lba48` 诊断位，不做 LBA48 传输）；
- word27..46 = 型号，每个 word **高字节在前**，去掉尾部空格和非可打印字符；
- 若 BSY 落地后 `ERR`，读 LBA1/LBA2 签名：`0x14/0xEB`（ATAPI）或
  `0x3C/0xC3`（SATA）都按 `ATA_ENODEV` 处理，不 panic。

QEMU 11 实测：`model=QEMU HARDDISK sectors=16384 lba28=1 lba48=1`
（8MB 镜像 = 16384 个 512B 扇区）。

### 2.5 错误码（驱动局部，调用方映射）

| 码 | 含义 |
|---|---|
| `ATA_OK` (0) | 成功 |
| `ATA_EIO` (-1) | 设备 ERR/DF，或传输中途出错 |
| `ATA_ETIMEDOUT` (-2) | 有界轮询用完（BSY 不落 / DRQ 不来） |
| `ATA_ENODEV` (-3) | 浮空总线 / 非 ATA（ATAPI、SATA）/ 不支持 LBA |
| `ATA_EINVAL` (-4) | 空参数、count 0 或 >8、LBA 越界、IO 未注入 |

## 3. blk_env 适配

`src/ng/ata_blk.c` 把 `struct ata_dev` 包装成 FAT16 认识的四个字段：

```c
struct blk_env {
    int (*read)(void *ctx, u64 sector, void *buf, u32 count);
    int (*write)(void *ctx, u64 sector, const void *buf, u32 count);
    u64 sectors;
    void *ctx;      /* = struct ata_dev * */
};
```

- 只允许 `present=1 && sectors>0` 的设备绑定；
- `sector > 0x0FFFFFFF` 直接失败（LBA28 上限），`count` 由驱动限制 1..8；
- 返回 0/-1，FAT16 统一映射 `VFS_EIO`；
- 同一份 `ata_blk.c` 在 host 单测里被编译，用 fake ATA 控制器驱动。

## 4. 磁盘镜像（8MB FAT16）

`tools/mkfat-ng.ps1` 现在支持 `-SizeMB`（默认 8，`0` = WP2-2 的 1.44MB）：

| 项 | 1.44MB（`-SizeMB 0`，RAM 盘） | 8MB（默认，真实磁盘） |
|---|---|---|
| 总扇区 | 2880 | 16384 |
| 扇区/簇 | 1（512B） | 2（1024B） |
| 保留扇区 | 1 | 1 |
| FAT 副本 | 2 × 12 扇区 | 2 × 32 扇区 |
| 根目录 | 224 项（14 扇区） | 512 项（32 扇区） |
| 数据区起点 | 39 | 97 |
| 数据簇数 | 2841 | **8143** |
| 内容 | `HELLO.TXT`(18B)、`DIR/A.TXT`(6B) | 相同 |

8MB 用 2 扇区/簇使簇数 8143 落在 FAT16 的簇数区间（≥4085），
`mkfat-ng.ps1` 迭代求解 `fat_sectors` 与簇数的固定点，并校验
`clusters+2` 个 16 位表项能被 FAT 区覆盖。

- 镜像生成在 `dist-ng/disk.img`，**不嵌入内核**；
- `build-ng.ps1` 每次构建都重新生成（`-SizeMB 8`），并校验尺寸/BPB/
  `HELLO.TXT` 内容；
- `run-ng.ps1` 用 `-drive file=<disk.img>,format=raw,if=ide` 把它挂成
  QEMU PIIX3 IDE 主盘；
- 1.44MB 镜像仍以 `.fat16` PT_LOAD 嵌入内核、复制到 PMM RAM 盘挂
  `/mnt/fat`（WP2-2 的回归不变）。

## 5. 内核接线与自检

顺序（`src/ng/main.c`）：WP2-1 → WP2-2 → **WP2-4** → WP2-3 → ring3。
WP2-4 排在 ring3 前，因为用户程序要读 `/mnt/disk/HELLO.TXT`。

`wp24_selftest()`：

1. `pci_find(0x8086, 0x7010)` 确认 PIIX3 IDE（QEMU 11：bus0 dev1 func1），
   打开 IO 空间；找不到也回退 legacy 端口（打印 `pci ide missing`）；
2. 端口 IO 注入（`inb/outb/inw/outw`）→ `ata_identify` 主盘；
3. 读 0 号扇区，校验 BPB 签名 `0x55AA` 与 `bytes/sector=512`；
4. 读最后一个扇区原内容 → 写 0xA5 图案 → 读回比对 → **恢复原内容**
   （同一镜像重复跑幂等；8MB 几何的末扇区在簇范围之外，仍做恢复兜底）；
5. 用 fake 控制器（状态永远 BSY，`poll_limit=64`）验证有界超时
   `ATA_ETIMEDOUT`，计数 +1 —— 内核里也证明“坏设备不 panic、不无限等”；
6. `ata_blk_bind` → 建 `/mnt/disk` → `fat16_mount` → `vfs_mount`；
7. 读 `HELLO.TXT`（18B）、`DIR/A.TXT`（6B）；建/写/读回/stat 600B
   `NG24.TXT`，再 unlink；最后打印 ATA 统计。

证据行：

    ng: ata pci ide found (8086:7010 bus=0 dev=1 func=1)
    ng: ata identify ok (model=QEMU HARDDISK sectors=16384 lba28=1 lba48=1)
    ng: ata read ok (lba=0 sig=0xaa55)
    ng: ata write ok (lba=16383 verify=1 restored=1)
    ng: ata timeout rejected (n=64)
    ng: disk fat16 mount ok (clusters=8143)
    ng: disk file rw ok (bytes=600)
    ng: ata stat reads=55 writes=13 timeouts=0 errors=0
    ng: WP2-4 selftest PASS

ring3 demo（`src/ng/user/hello.S`，经 syscall 20..27）：
`open("/mnt/disk/HELLO.TXT") → read 18 校验 → close →
open("/mnt/disk/DISK.TXT", O_CREAT|O_RDWR|O_TRUNC) → write("disk24") →
close → reopen → read 6 校验 → stat(size=6)`，打印
`ng: fs disk read ok` / `ng: fs disk write ok` / `ng: fs disk stat ok (size=6)`。

多次 `run-ng`（不重新构建）时镜像里会留下 `DISK.TXT`，所以内核的
`listdir("/mnt/disk")` 只要求集合包含 `HELLO.TXT` 与 `DIR`，不要求条目总数
固定；ring3 用 `O_TRUNC` 保证 `DISK.TXT` 始终是 6 字节。

## 6. host 单测

`tests/unit-ng/test_ata.c`（+232 checks）用 fake ATA 控制器（寄存器文件 +
BSY/DRQ 状态机 + PIO 数据口 + IDENTIFY 数据 + 可注入 ERR/DF/超时/ATAPI/浮空）
覆盖：

- `ata_devsel` / `ata_lba_range_ok` / `ata_status_class` 纯函数边界；
- IDENTIFY：型号解析（高字节在前、去尾空格）、扇区数、LBA/LBA48 位、
  从盘 drive 位、全 0 数据、不支持 LBA、ATAPI 签名、ERR/DF、BSY 永不落地、
  超时后恢复、io/poll_limit 非法；
- 读：单扇区/8 扇区/末扇区、LBA 寄存器与 devsel 高位、参数越界、
  ERR 后恢复、超时后恢复；
- 写：单/多扇区、数据真的写进 fake 盘、末尾 FLUSH（命令日志）、
  错误注入时盘内容不变、末扇区边界、全 0xFF 数据；
- `ata_blk_env`：sectors 传播、read/write 走 ATA、u64 扇区号 > LBA28 拒绝、
  未绑定/未 IDENTIFY 拒绝；
- FAT16 on fake ATA：4096 扇区合成卷（2 扇区/簇、2029 簇）挂载、读
  `HELLO.TXT`/`DIR/A.TXT`、创建/写/读回/readdir stat/unlink、重新 mount
  验证写入落盘、设备几何不足 → `EINVAL`、读错误 → `EIO`。

## 7. 已知限制 / 遗留

- **只做 PIO + 轮询**：无 IRQ/DMA；一次 1..8 扇区（多扇区命令可用，FAT16
  每次 1）；每字一次 `inw/outw`，大文件吞吐低但 D0 足够。
- **只做 LBA28**：镜像 ≤ 128 GiB；`lba48` 只作诊断位。
- **无分区表**：镜像本身就是 FAT16 卷（无 MBR），FAT16 从 0 号扇区读 BPB。
- **只探测 legacy 主通道**：PIIX3 兼容模式 0x1F0/0x3F6；native/PCI BAR 与
  从通道、第二个 IDE 控制器（q35/AHCI）不在本 WP 范围。
- **单核协作式**：与 VFS/FAT16 一样无锁，公开入口在多核/抢占前需要 spinlock。
- **fsd 进程化 / 块设备缓存 / 预读**：未做（后续 WP 或 E3 之后）。
