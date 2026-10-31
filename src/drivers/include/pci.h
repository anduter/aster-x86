/*
 * ng WP2-3: L1D PCI configuration space (mechanism #1, 0xCF8/0xCFC).
 *
 * 设计目的：把“配置空间寻址 + BAR 探测/大小计算”做成纯逻辑，端口 IO 通过
 * `struct pci_io_ops` 注入：
 *   - 内核（src/ng/wp23_selftest.c）注入 outl/inl，真正访问 0xCF8/0xCFC；
 *   - host 单测注入 fake 配置空间，验证地址编码/BAR 掩码/多设备扫描。
 * 驱动本身不碰串口、不碰页表、不依赖内核 PMM。
 *
 * 约束：
 *   - 只支持 4 字节对齐的配置寄存器访问（read32/write32），16 位读用 read32
 *     再移位；这与 mechanism #1 的 0xFC 对齐要求一致。
 *   - BAR 探测是“破坏性但可恢复”的：保存原值 -> 写 0xFFFFFFFF -> 读掩码 ->
 *     恢复原值。调用方应在设备空闲/初始化早期调用。
 */
#ifndef NG_PCI_H
#define NG_PCI_H

#include "types.h"

#define PCI_CONFIG_ADDRESS_PORT 0xCF8u
#define PCI_CONFIG_DATA_PORT 0xCFCu
#define PCI_CONFIG_ENABLE 0x80000000u
#define PCI_VENDOR_INVALID 0xFFFFu

/* PCI 命令寄存器位（offset 0x04，16 位）。 */
#define PCI_CMD_IO_SPACE 0x0001u
#define PCI_CMD_MEM_SPACE 0x0002u
#define PCI_CMD_BUS_MASTER 0x0004u

/* 标准 BAR 数量（type 0 header）。 */
#define PCI_BAR_COUNT 6u

/* 端口 IO 注入：out32/in32 的 ctx 由调用方解释。 */
struct pci_io_ops {
    void (*out32)(void *ctx, u16 port, u32 value);
    u32 (*in32)(void *ctx, u16 port);
    void *ctx;
};

/* 设备位置 bus:dev.func。 */
struct pci_loc {
    u8 bus;
    u8 dev;
    u8 func;
};

/* vendor/device id（配置空间 offset 0x00）。 */
struct pci_id {
    u16 vendor;
    u16 device;
};

/* BAR 探测结果。 */
struct pci_bar {
    u64 base;   /* 当前映射基址（内存 BAR 去掉低 4 位；IO BAR 去掉低 2 位） */
    u64 size;   /* 字节数；0 = 该 BAR 未实现/探测失败 */
    u32 raw_lo; /* 恢复后的原始低 32 位 */
    u32 raw_hi; /* 64 位 BAR 的高 32 位（否则 0） */
    u8 index;   /* 0..5 */
    u8 is_io;   /* bit0=1 */
    u8 is_64;   /* 内存 BAR 且 bits[2:1]==2 */
    u8 prefetch;/* bit3 */
};

/* 纯函数：mechanism #1 的 address dword。dev>=32/func>=8/off 未对齐时返回 0。 */
u32 pci_cfg_addr(u8 bus, u8 dev, u8 func, u8 off);

/* 配置空间读/写（off 必须 4 字节对齐）。返回 0=成功，-1=参数非法。 */
int pci_cfg_read32(const struct pci_io_ops *io, u8 bus, u8 dev, u8 func, u8 off,
                   u32 *out);
int pci_cfg_write32(const struct pci_io_ops *io, u8 bus, u8 dev, u8 func, u8 off,
                    u32 value);

/* 读 16 位（低半/高半）与 8 位；offset 任意，内部按 dword 对齐访问。 */
int pci_cfg_read16(const struct pci_io_ops *io, u8 bus, u8 dev, u8 func, u8 off,
                   u16 *out);
int pci_cfg_read8(const struct pci_io_ops *io, u8 bus, u8 dev, u8 func, u8 off,
                  u8 *out);

/* 扫描所有 bus/dev/func，找 vendor:device 精确匹配。0=找到，-1=没有。 */
int pci_find(const struct pci_io_ops *io, u16 vendor, u16 device,
             struct pci_loc *out);

/*
 * 扫描 class/subclass（配置空间 0x0B/0x0A）。class=0xFF 表示不限制 class，
 * subclass=0xFF 表示不限制 subclass。用于“没有固定 vendor id”的设备。
 */
int pci_find_class(const struct pci_io_ops *io, u8 class_code, u8 subclass,
                   struct pci_loc *out);

/* 读/写命令寄存器（offset 0x04 的低 16 位），返回 0/-1。 */
int pci_enable(const struct pci_io_ops *io, const struct pci_loc *loc, u16 bits);
u16 pci_command(const struct pci_io_ops *io, const struct pci_loc *loc, int *ok);

/*
 * 纯函数：由 BAR 掩码算大小。
 *   - is_io=1：掩码低 2 位忽略；
 *   - 否则低 4 位忽略；
 *   掩码为 0 时返回 0（BAR 未实现）；全 1 掩码按最小 BAR（内存 16B/IO 4B）返回，
 *   由调用方结合“设备存在”判断是否真是有效 BAR。
 */
u64 pci_bar_size(u32 mask, int is_io);

/*
 * 探测第 index 个 BAR：保存原值 -> 写全 1 -> 读掩码 -> 恢复 -> 解析。
 * 对 64 位内存 BAR 会同时探测 index+1 的高 32 位。返回 0=成功，-1=失败。
 * 注意：探测期间设备不能正在使用该 BAR（本 WP 在初始化早期调用）。
 */
int pci_read_bar(const struct pci_io_ops *io, const struct pci_loc *loc, u8 index,
                 struct pci_bar *out);

#endif /* NG_PCI_H */
