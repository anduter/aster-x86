# 显示与输入（WP2-3）

> 状态：2026-10-31 实现，分支 `ng`。对应 `docs/claims/ng-WP2-3.md`。
> 域：L1D 设备服务（`src/drivers/**`）+ L0 接线（`src/ng/wp23_selftest.c`）。
> 上游约束：`architecture.md` §2.3（L1D 只依赖 L0/A/B）、`evolution.md` E2 / WP2-3。

## 1. 范围与非目标

出口：D0 的显示/输入最小闭环。

- PCI 配置空间（mechanism #1）定位 QEMU stdvga `1234:1111`，读 BAR0；
- Bochs VBE 设 1280×800×32（失败回退 1024×768×32，再失败回 VGA 文本 0xB8000）；
- 帧缓冲控制台（8×16 内嵌字模、putc/print/fill/rect、光标）并读回 LFB 像素；
- PS/2 键盘（端口 0x64/0x60、scancode set 1、环形缓冲、轮询）；
- syscall 30 `readkey`（非阻塞）与 ring3 demo；
- `run-ng` 用 QMP 注入按键，验证用户态真的拿到输入。

非目标（按 `evolution.md`）：

- 不做块设备（WP2-4）、不做 GUI/桌面（E3）、不做鼠标（可选项）；
- 不做 IRQ/中断驱动输入、不做 virtio-gpu/GPU 3D、不做多显示器；
- 不做 ring3 直写 framebuffer（display server 是 E3；本 WP 的 LFB 只映射进内核 PML4）。

## 2. 分层与文件

| 层 | 路径 | 职责 |
|---|---|---|
| L1D | `src/drivers/include/pci.h`、`src/drivers/pci/pci.c` | 配置空间地址编码、32/16/8 位读、命令寄存器、vendor/class 扫描、BAR 探测 |
| L1D | `src/drivers/include/vbe.h`、`src/drivers/video/vbe.c` | Bochs dispi 寄存器编码、设模式/读回/关模式 |
| L1D | `src/drivers/include/fb.h`、`src/drivers/video/fb.c` | 32/24/16bpp 像素、填充/矩形/棋盘、8×16 字形、控制台/滚屏 |
| L1D | `src/drivers/include/font8x16.h`、`src/drivers/video/font8x16.c` | 95 个 ASCII 字形（0x20..0x7E），16 字节/字形 |
| L1D | `src/drivers/include/vga_text.h`、`src/drivers/video/vga_text.c` | 0xB8000 文本兜底（80×25、换行/滚屏） |
| L1D | `src/drivers/include/input.h`、`src/drivers/input/ps2.c` | set 1 映射、修饰键、128 项事件环、控制器轮询/初始化 |
| L0 接线 | `src/ng/wp23_selftest.c`、`src/ng/wp23.h` | 注入端口 IO、PCI/VBE/fb/PS2 编排、LFB 映射、自检证据行、`readkey` |
| L0 | `src/ng/syscall.c`、`src/ng/syscall.h` | syscall 30 分发 |
| L3 demo | `src/ng/user/hello.S` | `readkey` 轮询 → `ng: ring3 input ok (key=...)` / `(timeout)` |

依赖方向：驱动只 include `types.h` 和自己的头；端口 IO、MMIO 映射、串口输出全部由
`wp23_selftest.c` 注入。`check-domains` 因此保持 L1D → L0/A/B 合法。

## 3. PCI 配置空间

地址（mechanism #1）：

    addr = 0x8000_0000 | (bus << 16) | (dev << 11) | (func << 8) | (off & 0xFC)

写 0xCF8 后读写 0xCFC。`pci_find(1234:1111)` 扫 bus 0..255 / dev 0..31 / func 0..7，
用 header type bit7 判断多功能设备；没有设备（id=0xFFFFFFFF）时 func0 直接跳过该 dev。

BAR 探测（`pci_read_bar`）是标准掩码法：

1. 读原值（保存）；
2. 写 0xFFFFFFFF，读回掩码（64 位 BAR 同时处理相邻 dword）；
3. 恢复原值；
4. `size = (~mask & ~0xF) + 1`（IO BAR 用低 2 位掩码），base 取原值去掉低 4 位。

QEMU 11 + `-kernel`（PVH、无 BIOS）实测 BAR0 已经是 `0xFD000000`、大小 16 MiB；
代码仍保留“base==0 时写传统 VGA LFB 地址”的兜底，避免换 QEMU/机器类型后没有固件
分配 BAR。启用 `MEM_SPACE | BUS_MASTER`（后者按需：本 WP 不用 DMA，但与后续
virtio-gpu/块设备一致，先打开不改变行为）。

## 4. Bochs VBE

寄存器（16 位端口）：0x1CE 索引、0x1CF 数据。

    0 ID / 1 XRES / 2 YRES / 3 BPP / 4 ENABLE / 6 VIRT_WIDTH / 7 VIRT_HEIGHT
    ENABLE = 0x01 (enabled) | 0x40 (LFB)

设模式顺序：`ENABLE=0` → 写 XRES/YRES/BPP/VIRT_WIDTH/VIRT_HEIGHT → `ENABLE=0x41`
→ 读回 ENABLE/XRES/YRES/BPP 校验。1280×800×32 的 pitch = 5120、占用 4,096,000 字节，
BAR0 16 MiB 足够。

QEMU 不跑 VBE BIOS 也能用这套端口寄存器，所以 `-kernel` 直接启动时不需要 INT 10h。
如果 `vbe_probe` 读不到 0xB0C0..0xB0CF 家族 ID（例如 `-vga none`），内核走文本兜底，
不会把系统留在半配置状态（会先 `ENABLE=0`）。

## 5. LFB 映射

`0xFD000000` 在 0..1 GiB 恒等映射之外，`pg_map()` 会按需分配 PDPT/PD/PT，把
`[BAR0, BAR0+pitch*height)` 逐 4 KiB 恒等映射为：

    PG_PRESENT | PG_WRITE | PG_PCD | PG_PWT | (NX if available)

- PCD/PWT：MMIO 不能走普通 write-back 缓存；
- NX：显示内存永远不执行；
- 只映射实际用到的 `pitch*height`（本次 1000 页），不是整个 16 MiB BAR；
- 映射只进内核 PML4。`useras.c` 的用户地址空间只共享 0..1 GiB 的内核 PD，
  ring3 看不到 LFB；E3 的 display server 再决定共享 buffer 的授权方式；
- 若 BAR 恰好在 < 1 GiB 且已被 2 MiB 恒等大页覆盖，`PG_EBIGPAGE` 分支确认身份映射后
  接受该页（QEMU stdvga 不走这条）。

## 6. 帧缓冲控制台

- 颜色统一 `0x00RRGGBB`；写出时 32bpp = 0x00RRGGBB（QEMU stdvga 是 BGRX8888，
  小端低字节 B）、24bpp = B,G,R、16bpp = RGB565。读回按同一规则还原。
- 字模：8×16、95 个字形（0x20..0x7E），每字形 16 字节，bit0 是最左列。当前字表由
  旧树 8×8 基线字库纵向 2 倍展开生成（`src/drivers/video/font8x16.c` 顶部注明）；
  好处是不依赖 BIOS ROM、ASCII 全覆盖、字表可整体重生成；缺点是竖笔画偏粗，后续
  E3 可以替换成完整 VGA 8×16 ROM 字模而不改 API。
- 控制台：一个字符 8×16；`\n` 换行、行尾自动折行、到底部滚屏 16 像素；
  `fb_putc/fb_print/fb_fill/fb_rect/fb_glyph/fb_checkerboard`。
- 启动自检（`fb_draw_selftest`）：
  1. 全屏填深色；
  2. 固定坐标画 'A'（8×8 基线第 2 行 0x33 → 展开后第 4 行 bit0=1），读回该像素和
     空白像素；
  3. 64×64、8 像素棋盘，读回 (0,0)/(1,0)/(0,1) 三个格；
  4. 红色实心矩形，读回内部/外部；
  5. 从 y=200 打印三行控制台文字，读回首字符像素。
  全部通过才打印 `ng: fb draw ok (readback)`；任何一项不符打印 `ng: fb draw FAIL (n=...)`。
  读回真的走 LFB（QEMU VRAM 读通道），不是查内存镜像，所以能抓 pitch/颜色/映射错误。

## 7. VGA 文本兜底

`-vga none` 或没有 Bochs VBE 时：

- 不映射 LFB、不启用 VBE；
- `vga_text_init(0xB8000)`，清屏后写两行 Banner（属性 0x07）；
- 读回 (0,0)='A'、(1,0)='V'、属性=0x07，打印 `ng: vga text fallback ok (0xB8000)`；
- 键盘/自检/ring3 输入不受影响（QEMU 的 i8042 不属于 VGA）。

这满足“stdvga 不可用时系统仍可用”，并为后续 display server 的软件兜底留出位置。

## 8. PS/2 键盘

### 8.1 控制器初始化

端口：0x64 状态/命令、0x60 数据。`ps2_init`：

1. 读状态；0xFF = 无 i8042，返回失败；
2. 清空输出缓冲（最多 64 字节）；
3. 等 input buffer 空 → 0x64 写 0xAD（disable）→ 0xAE（enable）；
4. 清空输出缓冲；
5. 0x60 写 0xF4（enable scanning），轮询等 ACK 0xFA；0xFE（resend）最多重试 3 次。

QEMU i8042 默认开启 scancode 翻译，所以 guest 看到的是 set 1。驱动按 set 1 解码；
没有去改翻译位，避免不同 QEMU/硬件默认值差异（`docs/claims/ng-WP2-3.md` 记录实测）。

### 8.2 解码与环形缓冲

- `input_scancode_to_ascii(scancode, mods)` 是纯函数：数字/字母/标点/空格/回车/退格/
  Tab/Esc/小键盘，Shift 表单独覆盖符号；字母按 `Shift XOR Caps` 大小写。
- `input_feed` 处理：0xE0 扩展前缀（下一字节带 `INPUT_F_EXT`）、0xE1 Pause 整段跳过、
  0x80 release 位、Shift/Ctrl/Caps 状态。
- **环里只放按下事件**（128 项，`count/lost` 统计）；释放只更新修饰键和计数。这样
  `readkey` 不会先返回一个无意义的释放事件；按键重复（typematic）会表现为多次按下。
- `input_poll()` 每次最多读 64 字节：状态 bit0=1 → 读 0x60 → `input_feed`。没有 IRQ
  也能工作；`readkey` 和 idle 循环都会调用它。

### 8.3 中断与后续

本 WP 不接 IRQ1：IDT 有异常入口但没有设备 IRQ 路由/PIC 重映射，轮询是最小且可测的
路径。E3 的 inputd/display server 再改为 IRQ + 事件队列。

## 9. syscall 30 `readkey`

    rax = 30
    返回：flags<<24 | ascii<<16 | scancode   （非负，flags = INPUT_F_*）
          (u64)-11 (-EAGAIN)                （当前无按下事件）

- 非阻塞；用户态自己重试/超时；
- 每次调用先 `input_poll`，所以按键在 syscall 边界被吸收；
- ascii=0 表示不可打印键（方向键等），scancode 仍有效，flags 带 `INPUT_F_EXT`；
- ring3 demo 轮询最多 5,000,000 次，拿到 'a' 打印 `ng: ring3 input ok (key=a)`，
  拿到其它键打印 `(key=other)`，超时打印 `(timeout)`；三种都算通过。

## 10. 测试与注入

### host 单测（`tests/unit-ng/test_drivers.c`）

不链接内核、不需要 QEMU，端口 IO 用 fake ops：

- PCI：地址编码（bus/dev/func/off 边界）、16/8 位读、命令寄存器高位保留、
  BAR 掩码/大小（32 位/IO/64 位）、探测后恢复原值、未实现 BAR；
- VBE：ID 家族、寄存器读写、pitch/size、非法宽高/bpp、设模式写序列与读回、
  EVERIFY 路径、关模式；
- 字体：字形数/尺寸/指针、'A' 的固定行、空格、越界回退 '?'、94 个可见字符非空；
- 键映射：字母/Shift/Caps、数字符号、回车/退格/Tab/Esc、未知键；
- 环：FIFO、满/丢计数、空、修饰键、E0 扩展、E1 Pause、128 项容量；
- 控制器：`input_poll` 状态位、`ps2_init` ACK/无控制器；
- VGA 文本：清屏/换行/行尾折行/滚屏/读回；
- 帧缓冲：32/24/16bpp 像素与读回、裁剪、棋盘、字形、控制台折行、滚屏、
  pitch/bpp 参数拒绝。

### QEMU / run-ng

- `-vga std`，串口文件轮询；
- QMP（随机 TCP 端口）在用户态打印 `ng: ring3 input ready` 后注入按键。
  **QEMU 11 的 QMP 已删除 `sendkey`（返回 CommandNotFound）**，所以用
  `input-send-event`（qcode 'a' 的按下+释放）；失败再退 HMP `sendkey`；
- QMP 不可用 / `-NoQmp`：用户程序走 timeout 分支，期望行仍然通过（证明“无输入也不
  卡死”的降级路径）；
- `-Fallback`：QEMU `-vga none`，期望 `ng: pci vga missing` +
  `ng: vga text fallback ok (0xB8000)`，不期望 VBE/fb 行；键盘/输入行照旧。

## 11. 限制与后续

1. 只支持 QEMU stdvga 的 1234:1111 + Bochs VBE；真实硬件/其他显卡走 VGA 文本兜底。
2. 32bpp 按 BGRX 小端处理（QEMU stdvga 固定）；16/24bpp 有实现但未在 QEMU 实测。
3. 字模是 8×8 纵向展开的 8×16；笔画偏粗，E3 可替换完整 ROM 字库（API 不变）。
4. LFB 只在 ring0 可写；用户态 framebuffer/surface 是 E3 display server 的职责。
5. PS/2 只做键盘、轮询；没有鼠标（可选）和 IRQ。
6. `run-ng` 的 QMP 端口是“先占再还”的竞态窗口（极短）；QMP 失败会自动走 timeout 路径。
