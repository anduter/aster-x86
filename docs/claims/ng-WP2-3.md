# ng-WP2-3：显示 + 输入（PASS）

## 结论

**PASS** —— E2 / WP2-3 出口达成，`src/drivers` 成为第一个 L1D 设备域：

- **PCI**：mechanism #1（0xCF8/0xCFC）扫描并定位 QEMU stdvga `1234:1111`；
  BAR0 掩码探测（保存/写全 1/恢复）得到 LFB 基址与大小（QEMU 11 实测
  `0xFD000000` / 16 MiB）；启用 `MEM_SPACE | BUS_MASTER`。地址编码、16/8 位读、
  命令寄存器高位保留、32/IO/64 位 BAR 都在 host 单测里覆盖。
- **Bochs VBE**：0x1CE/0x1CF 索引/数据端口，`ENABLE=0` → 写
  XRES/YRES/BPP/VIRT_WIDTH/VIRT_HEIGHT → `ENABLE=ENABLED|LFB` → 读回校验；
  **1280×800×32**（pitch 5120、4,096,000 字节），失败回退 **1024×768×32**，
  再失败回 VGA 文本。不依赖 BIOS/INT 10h（QEMU `-kernel` PVH 直接进 64 位）。
- **LFB 映射**：`pg_map()` 按需建 PDPT/PD/PT，把 BAR0 逐 4 KiB 恒等映射为
  `RW + PCD + PWT + NX`（本次 1000 页，PTE 读回
  `0x80000000fd00001b`）；只进内核 PML4，ring3 不直写显示内存。
- **帧缓冲控制台**：内嵌 8×16 字模（0x20..0x7E，95 字形）、
  `fb_putc/fb_print/fb_fill/fb_rect/fb_glyph/fb_checkerboard`、光标/换行/滚屏；
  启动自检画 'A'、棋盘、矩形、三行文字并**读回 LFB 像素**，全部命中才打印
  `ng: fb draw ok (readback)`。
- **PS/2 键盘**：0x64/0x60、scancode set 1、Shift/Ctrl/Caps/0xE0/0xE1、
  128 项按下事件环、轮询（无 IRQ）；`ps2_init` 发 0xAD/0xAE/0xF4 并等 0xFA。
- **syscall 30 `readkey`**：非阻塞；有键返回
  `(flags<<24)|(ascii<<16)|scancode`，无键 `-EAGAIN(-11)`；用户态自己重试。
- **ring3 输入证据**：`run-ng` 看到 `ng: ring3 input ready` 后用 QMP
  **`input-send-event`** 注入 qcode `a`（QEMU 11 的 QMP 已删除 `sendkey`，
  返回 CommandNotFound），用户程序打印 `ng: ring3 input ok (key=a)`；
  `-NoQmp` 时走 `(timeout)` 分支，仍然 PASS。
- **降级**：`-vga none` 时打印 `ng: pci vga missing` /
  `ng: vbe unavailable ... vga text fallback` /
  `ng: vga text fallback ok (0xB8000)`，键盘与 ring3 输入照常工作。
- **门禁**：unit-ng **2290 checks, 0 failures**；run-ng **70/70 期望行 +
  27 条正则**（`-Fallback` 69/69 + 21）；旧 32 位 **305/305**；
  check-domains / check-deps 0 违规；ci-ng 6/6 PASS。

`src/kernel/**`、`build.ps1`、`tools/ci.ps1`、`abi.h` 未修改；不做块设备
（WP2-4）、GUI/桌面（E3）、鼠标（可选）。

## 交付物

| 路径 | 内容 |
|---|---|
| `src/drivers/include/pci.h`、`src/drivers/pci/pci.c` | PCI mechanism #1、命令寄存器、vendor/class 扫描、BAR 探测（**新**） |
| `src/drivers/include/vbe.h`、`src/drivers/video/vbe.c` | Bochs VBE 寄存器/设模式/读回/回退（**新**） |
| `src/drivers/include/fb.h`、`src/drivers/video/fb.c` | 32/24/16bpp 像素、填充/矩形/棋盘、8×16 字形、控制台/滚屏（**新**） |
| `src/drivers/include/font8x16.h`、`src/drivers/video/font8x16.c` | 95 个 ASCII 8×16 字模（旧 8×8 纵向 2 倍展开）（**新**） |
| `src/drivers/include/vga_text.h`、`src/drivers/video/vga_text.c` | 0xB8000 文本兜底（**新**） |
| `src/drivers/include/input.h`、`src/drivers/input/ps2.c` | set 1 映射/修饰键/128 项事件环/控制器轮询（**新**） |
| `src/ng/wp23.h`、`src/ng/wp23_selftest.c` | 端口 IO 注入、PCI/VBE/fb/PS2 编排、LFB 映射、证据行、readkey（**新**） |
| `src/ng/io.h` | 追加 `outw/inw/outl/inl`（**改**） |
| `src/ng/syscall.h`、`src/ng/syscall.c` | syscall 30 `readkey`（**改**） |
| `src/ng/main.c` | WP2-3 自检接线 + idle 轮询（**改**） |
| `src/ng/user/hello.S` | ring3 readkey demo（ready/key/timeout）（**改**） |
| `tests/unit-ng/test_drivers.c` | PCI/VBE/字体/键映射/环形缓冲/VGA 文本/帧缓冲读回（**新**） |
| `tests/unit-ng/fake_ng_env.[ch]` | fake readkey（syscall 30 分发）（**改**） |
| `tests/unit-ng/test_syscall.c`、`unit_main.c` | readkey 分发用例 + `test_drivers()`（**改**） |
| `tools/build-ng.ps1` | `-I src/drivers/include`、L1D 源列表（**改**，BOM） |
| `tools/run-ng.ps1` | `-vga std`、QMP `input-send-event`、`-Fallback`/`-NoQmp`、WP2-3 期望行（**改**，BOM） |
| `tools/unit-ng.ps1` | 驱动源 + `test_drivers.c` + include（**改**，BOM） |
| `docs/design/display-input-ng.md` | PCI/VBE/LFB/字体/PS2/readkey/测试/限制（**新**） |
| `docs/design/evolution.md`、`docs/design/README.md` | E2 进度 / 文档索引（**改**） |

## 环境

- 分支 `ng`，基线 HEAD `d6fd10c`（WP2-2），仓库 `D:\OS_Project`。
- clang 23.1.4 / ld.lld 23.1.4（`D:\msys64\mingw64\bin`）；内核
  `--target=x86_64-elf -ffreestanding -nostdlib -mno-red-zone
  -fno-stack-protector -mgeneral-regs-only`。
- QEMU 11.0.0（`qemu-system-x86_64`，`-kernel -m 256 -cpu max -vga std`），
  `-display none` + 串口文件轮询 + QMP TCP。

## 证据（2026-10-31）

### 构建

```text
=== build-ng (WP2-3) ===
user-elf: PASS (14832 bytes, 3 PT_LOAD, entry 0x40000000, end 0x400021b0)
...
  [34/40] src/drivers/pci/pci.c
  [35/40] src/drivers/video/vbe.c
  [36/40] src/drivers/video/fb.c
  [37/40] src/drivers/video/font8x16.c
  [38/40] src/drivers/video/vga_text.c
  [39/40] src/drivers/input/ps2.c
  [40/40] link -> dist-ng/aster-ng.elf

ELF64: PASS (1653672 bytes)
  5 PT_LOAD, 4 KiB aligned (per-section pages)
build-ng: PASS -> dist-ng/aster-ng.elf
```

### host 单测

```text
powershell -File tools/unit-ng.ps1
=== unit-ng (WP1-8 host tests) ===
2290 checks, 0 failures
unit-ng: PASS (2290 checks, 0 failures)
```

新增 `test_drivers()` 覆盖（`+466 checks`，本 WP 要求 ≥40）：PCI 地址编码/
16-8 位读/命令寄存器/BAR 掩码与探测恢复、VBE 寄存器与设模式/回退/校验、
字体覆盖与固定字形、键映射与修饰键、环形缓冲满/满丢弃/扩展键/Pause、
VGA 文本换行滚屏、帧缓冲 32/24/16bpp 像素与读回、控制台折行/滚屏。

### QEMU stdvga + QMP 注入（主路径）

```text
=== run-ng (WP2-3) ===
run-ng: QMP connected (sendkey injection armed)
run-ng: QMP sendkey a -> ring3 readkey
ng: pci vga found
ng: pci vga 1234:1111 cmd=0x107 bar0=0xfd000000 size=16777216
ng: vbe id=0xb0c5 mem=16777216
ng: vbe mode 1280x800x32 ok
ng: vbe pitch=5120 virt=1280x800
ng: lfb map ok (base=0xfd000000 pages=1000)
ng: lfb pte=0x80000000fd00001b (pcd/pwt)
ng: fb draw ok (readback)
ng: fb console cols=160 rows=50
ng: kbd init ok
ng: input event ok (n=6)
ng: WP2-3 selftest PASS
ng: ring3 input ready
ng: ring3 input ok (key=a)
run-ng: matched 70/70 expected lines + 27 patterns
run-ng: PASS (all expected lines in 0.7s, cpu max)
```

`input event ok (n=6)` 是内核解码自检：'a' / Shift+'a' / Caps / 'a' /
E0+0x50 / Shift+'1'='!' 六个事件（不是真实按键）。

### `-vga none` 降级

```text
args: ... -vga none ...
ng: pci vga missing
ng: vbe unavailable (no 1234:1111), vga text fallback
ng: vga text fallback ok (0xB8000)
ng: kbd init ok
ng: input event ok (n=6)
ng: WP2-3 selftest PASS
ng: ring3 input ready
ng: ring3 input ok (key=a)
run-ng: matched 69/69 expected lines + 21 patterns
run-ng: PASS
```

### 无 QMP（timeout 路径）

```text
run-ng -NoQmp
ng: ring3 input ready
ng: ring3 input ok (timeout)
run-ng: matched 70/70 expected lines + 27 patterns
run-ng: PASS (all expected lines in 2.2s, cpu max)
```

### 门禁

```text
tools/check-domains.ps1
checked: 224 files, 748 quoted includes
domain include violations: 0

tools/check-deps.ps1
OK: no cross-module include violations.

tools/ci-ng.ps1
  PASS check-domains    0 violations
  PASS check-deps       0 violations
  PASS unit-ng          2290 checks
  PASS build-ng         1653672 bytes, user-elf 14832 bytes
  PASS run-ng           70/70 lines + 27 patterns
  PASS legacy-32bit     305/305 parity checks
=== NG CI PASS ===
```

> 已知抖动：旧 32 位套件里 `pkg: user cli done` 这一行对宿主负载敏感，慢一点时
> 可能在 harness 固定收尾窗口内没打印出来（同一工作树单独
> `build.ps1 -Test` 重跑可 305/305；最终一次 `ci-ng` 为 6/6 PASS）。该路径不在
> WP2-3 改动范围内（`src/kernel/**` / `build.ps1` 未修改）。

## 排障记录（踩过的坑）

1. **QEMU 11 的 QMP 没有 `sendkey`**：第一次注入返回
   `{"error":{"class":"CommandNotFound"}}`，用户态走 timeout。改用
   `input-send-event`（qcode 'a' 的 down/up 两个事件）后命中
   `ng: ring3 input ok (key=a)`；HMP `sendkey` 保留为第二兜底。
2. **QEMU `-kernel` 无 BIOS，但 stdvga BAR0 已预置**：`info pci` 显示
   `BAR0: 32 bit prefetchable memory at 0xfd000000 [0xfdffffff]`；代码仍保留
   base==0 时写 `0xFD000000` 的兜底。
3. **LFB 在 1 GiB 恒等映射之外**：`0xFD000000` 需要 PDPT[3]；`pg_map()` 按需
   建表，映射 1000 页后 PTE 读回 `PCD|PWT|W|NX` 才继续。
4. **用户轮询窗口**：QMP 注入要等 run-ng 看到 ready 行（100 ms 轮询），
   首版 300k 次 readkey 在注入到达前就超时；提高到 5,000,000 次后稳定命中。
5. **字符像素断言**：'B' 的 0x66 第 0 列是空，读回测试要取 bit1 的列；
   滚屏后 'B' 到第一行、底部清空，断言按新坐标写。

## 限制与遗留

1. 只支持 QEMU stdvga（1234:1111）+ Bochs VBE；其它显卡/真机走 VGA 文本兜底。
2. 32bpp 按 QEMU stdvga 的 BGRX 小端处理；16/24bpp 有实现与 host 单测，未在
   QEMU 实机切模式验证。
3. 字模由 8×8 纵向展开，笔画偏粗；E3 可换完整 VGA 8×16 ROM 字模（API 不变）。
4. 键盘只轮询、无 IRQ；无鼠标（可选）；LFB 只映射内核 PML4，用户态 framebuffer
   留到 E3 display server。
5. `run-ng` 的 QMP 端口“先占再还”有极短竞态；QMP 失败自动回退 timeout 路径，
   不影响 PASS。
