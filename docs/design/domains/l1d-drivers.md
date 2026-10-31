# L1D 设备服务（plane=l1d）

> 状态：WP0-2 域文档补全，2026-10-31；描述 ng HEAD `c4efabc` 的已提交实现。
> 上位：`../architecture.md` §2.3、§5 不变量 8（L1D 只依赖 L0/A/B）、ADR-007。
> 证据：`../../claims/ng-WP2-3.md`、`ng-WP2-4.md`；显示数据面在 WP3-1。
> 注意：**当前所有驱动都在 ring0**，以库形式链接进内核；ring3 迁移冻结到 D1。

## 职责

| 设备类 | 当前驱动 | 代码 | 状态 |
|---|---|---|---|
| PCI 配置空间 | mechanism #1、32/16/8 位读、BAR 掩码探测、命令寄存器 | `src/drivers/pci/pci.c` | 已实现（WP2-3） |
| 显示 | Bochs VBE 1280×800×32（回退 1024×768×32）、LFB 映射、8×16 字模控制台、VGA 文本兜底 | `src/drivers/video/{vbe,fb,vga_text}.c` | 已实现（WP2-3） |
| 输入 | PS/2 键盘、set 1 解码、修饰键、128 项按下事件环、轮询 | `src/drivers/input/ps2.c` | 已实现（WP2-3） |
| 块 | ATA PIO：IDENTIFY / LBA28 READ / WRITE / FLUSH、有界轮询、8 MB FAT16 盘 | `src/drivers/block/ata.c` | 已实现（WP2-4） |
| 网卡 | — | — | 未移植（旧树 e1000 仅 oracle） |
| GPU / USB / 音频 | — | — | D1/D2；D0 不做 |
| 输入鼠标 | — | — | 可选；architecture 的“键鼠”与实现不一致（D08） |
| IRQ / DMA | — | — | 未接；PS/2/ATA 都轮询（ADR-007 前的现状） |

## 不做什么

1. **不实现文件系统/网络协议/显示策略**：FAT16 在 L1，surface/合成/焦点在 L2。
2. **不直接暴露 syscall**：驱动只 include `types.h` 和自己的头；端口 IO、MMIO 映射、
   LFB 共享由 L0 注入（`src/ng/wp23_selftest.c`、`wp24_selftest.c`）。
3. **不依赖 L1**：`blk_env` 属于 L1，所以适配层 `src/ng/ata_blk.c` 放在 L0，
   避免 L1D → L1 的向上依赖（`../block-ng.md` §1）。
4. **不做 ring3 驱动/IOMMU/DMA 隔离**：ADR-007 冻结到 D1；旧树 hwcap 不迁 ng（D09）。
5. **不做 virtio-gpu/virtio-blk/AHCI/LBA48/分区表/热插拔/多显示器**。
6. **不做鼠标/按键释放重复率/IRQ 输入**：当前只有键盘按下事件。

## 接口（syscall / IPC / ABI）

L1D 自身没有 syscall；只有两条经 L0 暴露给用户态的路径：

| 路径 | 机制 | 说明 |
|---|---|---|
| `SYS_READKEY`（30） | `ng_input_readkey()` → `input_poll()` → PS/2 环 | 返回 `(flags<<24)\|(ascii<<16)\|scancode`；无键返回 `-EAGAIN`；非阻塞 |
| `SYS_FB_MAP`（47） | `fbmap_map_proc()` 把 LFB 逐页映射进用户 AS | root-only；固定 VA `0x60000000`；PCD/PWT/NX；返回 48 B `ds_fb_info` |

其余驱动接口是**内核内注入式 API**：

```text
struct pci_io   { inl/outl/... }      PCI 配置空间访问（L0 注入端口 IO）
struct vbe_io   { inb/outb/inw/outw } Bochs VBE 寄存器
struct fb_info  { base/pitch/bpp/w/h } LFB 描述（L0 负责映射）
struct input_io { inb/outb }          i8042 端口
struct ata_io   { inb/outb/inw/outw } ATA 端口
struct blk_env  { read/write/sectors/ctx }  L1 块设备抽象（适配层在 src/ng）
```

约定：

- 驱动不分配物理页、不改页表、不写 VFS；缓冲由调用方给。
- ATA 一次 1..8 扇区（FAT16 每次 1）；只做 LBA28；`lba48` 只记录能力位。
- 所有等待都是有界轮询；`ERR/DF` → `ATA_EIO`，超时 → `ATA_ETIMEDOUT`，不 panic。
- 显示：LFB 只映射实际用到的 `pitch*height`（1280×800×32 = 1000 页），
  不是整个 16 MiB BAR；`-vga none` 走 VGA 文本 0xB8000，`fb_map` 返回 `ENODEV`。
- PS/2：环里只放按下事件；`readkey` 每次先 `input_poll`，没有 IRQ 也能工作。
- LFB 现在可由 L2 displayd 映射（WP3-1），但**合成/绘制策略不在 L1D**。

错误码：驱动局部 `ATA_EIO(-1)/ETIMEDOUT(-2)/ENODEV(-3)/EINVAL(-4)` 在 `ata_blk`
边界映射成 VFS 的 `EIO`；`readkey` 用 `-EAGAIN(-11)`；`fb_map` 用
`EPERM/ENODEV/EBUSY/ENOMEM/EFAULT/EINVAL`。

## 失败等级与恢复

| 组件 | 设计等级 | 当前行为 | 恢复 |
|---|---|---|---|
| 块（blkd） | `core` | ATA 错误/超时返回 `EIO/ETIMEDOUT`；FAT16 把设备错误映射 `EIO` | 调用方重试/降级；无热重启、无第二个块设备 |
| 显示 | `optional` | 没有 VBE/LFB → VGA 文本兜底；`fb_map` → `ENODEV`；displayd 跳过 | 文本模式可继续跑 shell/串口 |
| 输入 | `optional` | i8042 缺失 → `ps2_init` 失败；`readkey` 一直 `EAGAIN` | shell 靠 QMP/超时；无备用输入 |
| PCI | `core`（引导期） | 找不到设备返回 NULL；IDE 回退 legacy 端口 | 有兜底 |
| 网卡 | `optional` | 未实现 | 无网络 |

设备故障**不会 panic**（ATA/PS2/VBE 都有失败分支），但驱动与内核同地址空间，
驱动里的内存越界仍可导致 fatal。ring3 驱动是 D1 的隔离手段（ADR-007）。

## manifest 五元组

设计值（当前无解析器，D04）：

| 组件 | plane | criticality | fallback | health | restart |
|---|---|---|---|---|---|
| `blkd`（ring3，D1） | `l1d` | `core` | `ata-pio-ring0`（D0 现状） | `heartbeat:5s` | `on-failure`，3 次 |
| `inputd`（ring3，D1） | `l1d` | `optional` | `ps2-poll-ring0` | `heartbeat:5s` | `on-failure`，3 次 |
| `display`（ring0/VBE） | `l1d` | `optional` | `vga-text` | `none` | `never`（引导期一次性） |
| `netd`（未实现） | `l1d` | `optional` | `no-network` | `heartbeat:10s` | `on-failure`，3 次 |
| `gpu/usb/audio`（未实现） | `l1d` | `optional` | `none`（不得进 L2） | `none` | `on-failure` |

规则：L1D 只能依赖 L0/A/B；fallback 不能指向 L2/L3。`blkd` 的 fallback 在 D0
就是 ring0 ATA 本身，ring3 化后才是真正的“回退到 ring0 引导驱动”。

## 当前实现状态（ng WP）

| WP | 内容 | 证据 |
|---|---|---|
| WP2-3 | PCI + Bochs VBE 1280×800×32、LFB 读回、8×16 fb 控制台、VGA 文本兜底、PS/2 set 1、`readkey` | `../../claims/ng-WP2-3.md` |
| WP2-4 | ATA PIO、IDENTIFY/LBA28/FLUSH、有界超时、8 MB FAT16 IDE 盘、`blk_env` | `../../claims/ng-WP2-4.md` |
| WP3-1 | `SYS_FB_MAP` 把 LFB 映射进 displayd（机制在 L0，L1D 只提供 fb 描述） | `../../claims/ng-WP3-1.md` |
| WP2-2 | FAT16 通过 `blk_env` 与设备解耦（RAM 盘/ATA 共用） | `../../claims/ng-WP2-2.md` |

QEMU 证据：`ng: pci vga found`、`ng: vbe mode 1280x800x32 ok`、
`ng: fb draw ok (readback)`、`ng: kbd init ok`、`ng: ata identify ok (...)`、
`ng: ata timeout rejected (n=64)`、`ng: disk fat16 mount ok (clusters=8143)`。
`-Fallback`（`-vga none`）走 `ng: vga text fallback ok (0xB8000)`。

## 待办

1. **IRQ/DMA**：PS/2 IRQ1、ATA IRQ14、PIC/APIC 重映射；当前全轮询。
2. **ring3 驱动**：blkd/inputd/netd + MMIO/IRQ/DMA cap（D1，ADR-007）。
3. **鼠标**：PS/2 鼠标或 QEMU 注入，补点击焦点；先解决 D08 的 D0 口径。
4. **更多块设备**：virtio-blk/AHCI/LBA48/分区表/多设备选择。
5. **显示**：完整 VGA 8×16 ROM 字模（API 不变）、virtio-gpu 2D、双缓冲/vsync（L2 配合）。
6. **热插拔/错误恢复**：设备掉线后的重探测与重连（S3）。
7. **性能**：PIO → DMA、批量扇区、framebuffer 写入优化；保持 host 单测 fake 控制器。
