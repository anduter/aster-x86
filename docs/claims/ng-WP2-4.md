# ng-WP2-4：块设备（ATA PIO + 真实磁盘上的 FAT16）（PASS）

## 结论

**PASS** —— E2 / WP2-4 出口达成，`src/drivers/block` 成为第二个 L1D 设备驱动：

- **ATA PIO**（`src/drivers/include/ata.h`、`src/drivers/block/ata.c`，L1D）：
  PCI 确认 QEMU PIIX3 IDE `8086:7010`（bus0 dev1 func1），用 legacy 主通道
  `0x1F0–0x1F7` + `0x3F6`；`IDENTIFY DEVICE (0xEC)` 解析型号/LBA28 扇区数/
  LBA/LBA48 能力位；`READ SECTOR(S) (0x20)` / `WRITE SECTOR(S) (0x30)` 用
  LBA28 编码（`0x1F6 = 0xE0 | drive<<4 | lba[27:24]`），单扇区与 ≤8 扇区可用；
  写后 `FLUSH CACHE (0xE7)`。
- **有界轮询 / 错误恢复**：每次状态检查后读 4 次 ALTSTATUS（~400ns）；
  `poll_limit` 默认 1,000,000，超时返回 `ATA_ETIMEDOUT`，`ERR/DF` 返回
  `ATA_EIO`，浮空/ATAPI/SATA 签名返回 `ATA_ENODEV`；**任何路径都不 panic、
  不无限等**。内核里用 fake 控制器（状态永远 BSY，`poll_limit=64`）证明了
  超时路径。
- **blk_env 适配**（`src/ng/ata_blk.c`）：`ata_blk_env()` 暴露
  `read/write/sectors/ctx`，FAT16 直接复用；适配层放在 L0（src/ng）是因为
  check-domains 规定 L1D 只能依赖 L0/A/B，而 `blk.h` 属于 L1。
- **真实磁盘镜像**：`tools/mkfat-ng.ps1 -SizeMB 8`（默认）生成
  `dist-ng/disk.img`（8MB、16384 扇区、2 扇区/簇、2×32 FAT 扇区、512 根目录项、
  data.start=97、**8143 簇**，含 `HELLO.TXT`/`DIR/A.TXT`）；`build-ng.ps1`
  每次构建重新生成并校验；`run-ng.ps1` 用
  `-drive file=dist-ng/disk.img,format=raw,if=ide` 挂成 IDE 主盘；内核把
  FAT16 挂到 `/mnt/disk`。
- **内核自检**：读 0 号扇区校验 BPB `0x55AA`；写最后一个扇区图案 → 读回
  比对 → 恢复原内容（重复运行幂等）；超时负向用例；`/mnt/disk` 读
  `HELLO.TXT`（18B）、`DIR/A.TXT`（6B）；建/写/读回/stat 600B `NG24.TXT`
  再 unlink。
- **ring3 demo**（`src/ng/user/hello.S`，syscall 20..27）：
  `open("/mnt/disk/HELLO.TXT") → read 18 校验 → close →
   open("/mnt/disk/DISK.TXT", O_CREAT|O_RDWR|O_TRUNC) → write("disk24") →
   close → reopen → read 6 校验 → stat(size=6)`，打印
  `ng: fs disk read ok` / `write ok` / `stat ok (size=6)`。
- **门禁**：unit-ng **2522 checks, 0 failures**（WP2-4 +232）；run-ng
  **80/80 期望行 + 34 条正则**（`-Fallback` 79/79 + 28）；旧 32 位
  **305/305**；check-domains / check-deps 0 违规；ci-ng **6/6 PASS**。

`src/kernel/**`、`build.ps1`、`tools/ci.ps1`、`abi.h` 未修改；不做
virtio-blk/AHCI、不做 fsd 进程化、不做 shell。

## 交付物

| 路径 | 内容 |
|---|---|
| `src/drivers/include/ata.h`、`src/drivers/block/ata.c` | ATA PIO：寄存器/LBA28 编码、IDENTIFY、READ/WRITE/FLUSH、有界轮询与错误码（**新**） |
| `src/ng/ata_blk.h`、`src/ng/ata_blk.c` | `ata_dev` -> `blk_env` 适配（L0；host 单测编译同一份）（**新**） |
| `src/ng/wp24.h`、`src/ng/wp24_selftest.c` | PCI PIIX3 + 端口 IO 注入 + ATA 自检 + `/mnt/disk` 挂载/文件读写（**新**） |
| `src/ng/main.c` | WP2-4 自检接线（WP2-2 之后、WP2-3/ring3 之前）（**改**） |
| `src/ng/user/hello.S` | ring3 disk demo：读 HELLO.TXT、写/读回 DISK.TXT、stat（**改**） |
| `tools/mkfat-ng.ps1` | `-SizeMB`（默认 8；0=1.44MB legacy）、8MB FAT16 布局、自校验（**改**，BOM） |
| `tools/build-ng.ps1` | 生成并校验 `dist-ng/disk.img`；ATA/适配/自检源文件（**改**，BOM） |
| `tools/run-ng.ps1` | `-drive file=...,if=ide`、`-DiskImg`、WP2-4 期望行/正则（**改**，BOM） |
| `tests/unit-ng/test_ata.c` | fake ATA 控制器/盘 + 寄存器/LBA/状态机/超时 + fake ATA 上的 FAT16（**新**） |
| `tests/unit-ng/unit_main.c`、`tools/unit-ng.ps1` | 注册 `test_ata()` + 源文件（**改**） |
| `docs/design/block-ng.md` | ATA/适配/镜像/接线/测试/限制（**新**） |
| `docs/design/evolution.md`、`README.md`、`fat16-ng.md` | E2 进度/索引/交叉引用（**改**） |

## 环境

- 分支 `ng`，基线 HEAD `5fbc8b6`（WP2-3），仓库 `D:\OS_Project`。
- clang 23.1.4 / ld.lld 23.1.4（`D:\msys64\mingw64\bin`）；
  `clang --target=x86_64-elf -ffreestanding -nostdlib -mno-red-zone
  -fno-stack-protector -mgeneral-regs-only`。
- QEMU 11.0.0：`-kernel dist-ng/aster-ng.elf -m 256 -cpu max -vga std
  -drive file=dist-ng/disk.img,format=raw,if=ide -display none`。

## 证据（2026-10-31）

### 构建

```text
=== build-ng (WP2-4) ===
mode: legacy 1.44MB
geometry: 2880 sectors x 512, spc=1, FATS=2x12, root=224 (14 sec)
          data.start=39, clusters=2841, bytes/cluster=512
fat16 image: PASS (1474560 bytes, 2880 sectors, 2841 clusters)
mode: disk 8MB
geometry: 16384 sectors x 512, spc=2, FATS=2x32, root=512 (32 sec)
          data.start=97, clusters=8143, bytes/cluster=1024
disk image: PASS (8388608 bytes, 16384 sectors, fats=2x32, data.start=97, clusters=8143)
user-elf: PASS (15224 bytes, 3 PT_LOAD, entry 0x40000000, end 0x400021b0)
ELF64: PASS (1659184 bytes)
  5 PT_LOAD, 4 KiB aligned (per-section pages)
build-ng: PASS -> D:\OS_Project\dist-ng\aster-ng.elf
```

### host 单测

```text
powershell -File tools/unit-ng.ps1
=== unit-ng (WP1-8 host tests) ===
2522 checks, 0 failures
unit-ng: PASS (2522 checks, 0 failures)
```

新增 `test_ata()` 覆盖（`+232 checks`，本 WP 要求 ≥40）：

- `ata_devsel`（drive 只取 bit0、LBA[27:24]）、`ata_lba_range_ok`（count
  0/9、跨末尾、LBA28 上限）、`ata_status_class`（BSY/ERR/DF/DRQ/DRDY）；
- fake ATA 控制器：寄存器文件、BSY→DRQ 状态机、PIO 数据口、IDENTIFY 数据、
  ERR/DF/超时/ATAPI/浮空总线注入、命令日志；
- IDENTIFY：`QEMU HARDDISK` 型号解析（高字节在前、去尾空格）、16384 扇区、
  LBA/LBA48 位、从盘 drive 位、全 0 数据、不支持 LBA、ATAPI 签名、ERR/DF、
  超时后恢复、io/poll_limit 非法；
- 读：单/8/末扇区、LBA 寄存器与 devsel 高位、参数越界、ERR 后恢复、
  超时后恢复；
- 写：单/多扇区真的落盘、命令日志里 WRITE 后跟 FLUSH、错误注入时盘内容
  不变、末扇区边界、全 0xFF 数据；
- `ata_blk_env`：sectors 传播、read/write 走 ATA、u64 扇区号 > LBA28 拒绝、
  未绑定/未 IDENTIFY 拒绝；
- FAT16 on fake ATA：4096 扇区合成卷（2 扇区/簇、2029 簇）挂载、读
  `HELLO.TXT`/`DIR/A.TXT`、创建/写/读回/readdir stat/unlink、重新 mount
  验证写入落盘、设备几何不足 → `EINVAL`、读错误 → `EIO`。

### QEMU：ATA + 真实磁盘（主路径）

```text
=== run-ng (WP2-4) ===
disk: D:\OS_Project\dist-ng\disk.img (8388608 bytes, ide)
args: ... -drive file:<disk.img>,format=raw,if=ide ... -cpu max -vga std
ng: ata pci ide found (8086:7010 bus=0 dev=1 func=1)
ng: ata identify ok (model=QEMU HARDDISK sectors=16384 lba28=1 lba48=1)
ng: ata read ok (lba=0 sig=0xaa55)
ng: ata write ok (lba=16383 verify=1 restored=1)
ng: ata timeout rejected (n=64)
ng: disk fat16 mount ok (clusters=8143)
ng: disk file rw ok (bytes=600)
ng: ata stat reads=55 writes=13 timeouts=0 errors=0
ng: WP2-4 selftest PASS
...
ng: ring3 hello
ng: fs disk read ok
ng: fs disk write ok
ng: fs disk stat ok (size=6)
ng: ring3 exit ok
run-ng: matched 80/80 expected lines + 34 patterns
run-ng: PASS (all expected lines in 0.7s, cpu max)
```

`ata stat reads=55 writes=13` 是本次内核自检的 ATA 命令计数（mount/读文件/
写 600B/恢复末扇区都算），随镜像文件内容略有变化，不作为断言。

### 重复运行（不重建镜像）

同一 `dist-ng/disk.img` 连续跑两次 `run-ng` 都 PASS（80/80 + 34）：ring3 建的
`DISK.TXT` 留在卷里，内核的 `listdir` 只要求 `HELLO.TXT` + `DIR` 存在；
ring3 用 `O_TRUNC` 保证 `DISK.TXT` 恒为 6 字节。

### `-vga none` 降级

```text
run-ng -Fallback
ng: pci vga missing
ng: vga text fallback ok (0xB8000)
ng: ata identify ok (model=QEMU HARDDISK sectors=16384 lba28=1 lba48=1)
ng: disk fat16 mount ok (clusters=8143)
ng: disk file rw ok (bytes=600)
ng: WP2-4 selftest PASS
run-ng: matched 79/79 expected lines + 28 patterns
run-ng: PASS
```

### 无 QMP（readkey timeout 路径）

```text
run-ng -NoQmp
ng: ring3 input ok (timeout)
ng: fs disk read ok / write ok / stat ok (size=6)
run-ng: matched 80/80 expected lines + 34 patterns
run-ng: PASS (all expected lines in 2.8s, cpu max)
```

### 门禁

```text
tools/check-domains.ps1
checked: 226 files, 750 quoted includes
domain include violations: 0

tools/check-deps.ps1
OK: no cross-module include violations.

tools/ci-ng.ps1
  PASS check-domains    0 violations
  PASS check-deps       0 violations
  PASS unit-ng          2522 checks
  PASS build-ng         1659184 bytes, user-elf 15224 bytes
  PASS run-ng           80/80 lines + 34 patterns
  PASS legacy-32bit     305/305 parity checks
=== NG CI PASS ===
```

## 排障记录（踩过的坑）

1. **QEMU `-drive` 用 `file=` 不是 `file:`**：`-drive file:...` 会被解析成
   short-form boolean，QEMU 报 `A block device must be specified for "file"`，
   内核根本收不到盘。改成 `file=D:\...\disk.img,format=raw,if=ide` 后
   `8086:7010` 与 `0x1F0` 立即出现。
2. **LBA 高 4 位断言**：`0x0123456` 其实是 `0x00123456`（高 4 位为 0），
   测 `HDDEVSEL` 高位要用 `0x1123456`（期望 `0xE1`）。
3. **PowerShell 的 `[int]` 是银行家舍入**：`[int](16287 / 2)` 得到
   8144，而 FAT16 的 `u32` 除法是 8143。`mkfat-ng.ps1`/`build-ng.ps1` 用
   `[Math]::Floor` 后与内核 mount 打印一致。
4. **重复 `run-ng` 的 listdir 计数**：ring3 建的 `DISK.TXT` 会留在持久镜像里，
   内核自检一开始断言“根目录恰好 2 项”，第二次跑就 FAIL。改成“集合里含
   HELLO.TXT 和 DIR”后幂等。
5. **末扇区写测试要恢复**：写图案验证后写回原内容；8MB 几何的最后一个扇区
   落在簇范围外（data.start=97、8143 簇 → 最后一簇末扇区 16382），仍恢复
   兜底，镜像重复使用不受影响。
6. **BOM**：`edit`/`write` 重建 `.ps1` 会丢 `EF BB BF`，本次改完 4 个
   `.ps1` 后逐个补 BOM 并用字节检查确认。

## 限制与遗留

1. 只做 ATA **PIO + 轮询**：无 IRQ/DMA；一次 1..8 扇区（FAT16 每次 1）；
   每字一次 `inw/outw`，吞吐有限但 D0 足够。
2. 只做 **LBA28**（≤128 GiB）；`lba48` 只记录能力位，不做 LBA48 传输。
3. **无分区表**：镜像本身就是 FAT16 卷（无 MBR）；不解析分区。
4. 只探测 **legacy 主通道**（0x1F0/0x3F6）：PIIX3 兼容模式；native/PCI BAR、
   从通道、q35/AHCI 不在本 WP 范围。
5. 单核协作式无锁；fsd 进程化 / 块设备缓存 / 预读留到后续 WP。
