/*
 * ng WP2-3: PCI 配置空间 + BAR 探测（L1D，见 include/pci.h）。
 *
 * 只做本 WP 需要的最小集合：地址编码、32/16/8 位读、命令寄存器、按
 * vendor:device / class 扫描、BAR 大小探测。没有资源分配、没有 IRQ 路由
 * （那些是 WP2-4/后续 L1D 的事）。
 */
#include "pci.h"

#define PCI_CFG_ID 0x00u
#define PCI_CFG_COMMAND 0x04u
#define PCI_CFG_CLASS 0x08u
#define PCI_CFG_HEADER 0x0Cu
#define PCI_CFG_BAR0 0x10u

static int io_ok(const struct pci_io_ops *io)
{
    return io != NULL && io->out32 != NULL && io->in32 != NULL;
}

u32 pci_cfg_addr(u8 bus, u8 dev, u8 func, u8 off)
{
    if (dev >= 32u || func >= 8u || (off & 0x3u) != 0) {
        return 0;
    }
    return PCI_CONFIG_ENABLE | ((u32)bus << 16) | ((u32)dev << 11) |
           ((u32)func << 8) | ((u32)off & 0xFCu);
}

int pci_cfg_read32(const struct pci_io_ops *io, u8 bus, u8 dev, u8 func, u8 off,
                   u32 *out)
{
    u32 addr;

    if (!io_ok(io) || out == NULL) {
        return -1;
    }
    addr = pci_cfg_addr(bus, dev, func, off);
    if (addr == 0) {
        return -1;
    }
    io->out32(io->ctx, PCI_CONFIG_ADDRESS_PORT, addr);
    *out = io->in32(io->ctx, PCI_CONFIG_DATA_PORT);
    return 0;
}

int pci_cfg_write32(const struct pci_io_ops *io, u8 bus, u8 dev, u8 func, u8 off,
                    u32 value)
{
    u32 addr;

    if (!io_ok(io)) {
        return -1;
    }
    addr = pci_cfg_addr(bus, dev, func, off);
    if (addr == 0) {
        return -1;
    }
    io->out32(io->ctx, PCI_CONFIG_ADDRESS_PORT, addr);
    io->out32(io->ctx, PCI_CONFIG_DATA_PORT, value);
    return 0;
}

int pci_cfg_read16(const struct pci_io_ops *io, u8 bus, u8 dev, u8 func, u8 off,
                   u16 *out)
{
    u32 v;

    if (out == NULL || pci_cfg_read32(io, bus, dev, func, (u8)(off & 0xFCu), &v) != 0) {
        return -1;
    }
    *out = (u16)(v >> ((off & 0x2u) ? 16 : 0));
    return 0;
}

int pci_cfg_read8(const struct pci_io_ops *io, u8 bus, u8 dev, u8 func, u8 off,
                  u8 *out)
{
    u32 v;

    if (out == NULL || pci_cfg_read32(io, bus, dev, func, (u8)(off & 0xFCu), &v) != 0) {
        return -1;
    }
    *out = (u8)(v >> ((off & 0x3u) * 8u));
    return 0;
}

u64 pci_bar_size(u32 mask, int is_io)
{
    u32 keep = is_io ? 0xFFFFFFFCu : 0xFFFFFFF0u;
    u32 m = mask & keep;

    if (m == 0u) {
        return 0; /* 没有实现 */
    }
    /* ~m+1：0xFFFFF000 -> 0x1000；0xFFFF0000 -> 0x10000。 */
    return (u64)((~m) + 1u) & 0xFFFFFFFFULL;
}

int pci_read_bar(const struct pci_io_ops *io, const struct pci_loc *loc, u8 index,
                 struct pci_bar *out)
{
    u8 off;
    u32 raw_lo;
    u32 raw_hi = 0;
    u32 mask_lo;
    u32 mask_hi = 0;
    int is_io;
    int is_64;

    if (!io_ok(io) || loc == NULL || out == NULL || index >= PCI_BAR_COUNT) {
        return -1;
    }
    off = (u8)(PCI_CFG_BAR0 + (u32)index * 4u);
    if (pci_cfg_read32(io, loc->bus, loc->dev, loc->func, off, &raw_lo) != 0) {
        return -1;
    }
    is_io = (raw_lo & 0x1u) ? 1 : 0;
    is_64 = (!is_io && ((raw_lo >> 1) & 0x3u) == 0x2u) ? 1 : 0;
    if (is_64) {
        if (index + 1u >= PCI_BAR_COUNT ||
            pci_cfg_read32(io, loc->bus, loc->dev, loc->func, (u8)(off + 4u),
                           &raw_hi) != 0) {
            return -1;
        }
        if (raw_hi == 0u && raw_lo == 0u) {
            return -1; /* 未实现的 64 位 BAR */
        }
    } else if (raw_lo == 0u) {
        return -1;
    }

    /* 写全 1 读掩码；随后无论成功与否都恢复原值。 */
    if (pci_cfg_write32(io, loc->bus, loc->dev, loc->func, off, 0xFFFFFFFFu) != 0) {
        return -1;
    }
    if (is_64 &&
        pci_cfg_write32(io, loc->bus, loc->dev, loc->func, (u8)(off + 4u),
                        0xFFFFFFFFu) != 0) {
        (void)pci_cfg_write32(io, loc->bus, loc->dev, loc->func, off, raw_lo);
        return -1;
    }
    (void)pci_cfg_read32(io, loc->bus, loc->dev, loc->func, off, &mask_lo);
    if (is_64) {
        (void)pci_cfg_read32(io, loc->bus, loc->dev, loc->func, (u8)(off + 4u),
                             &mask_hi);
    }
    (void)pci_cfg_write32(io, loc->bus, loc->dev, loc->func, off, raw_lo);
    if (is_64) {
        (void)pci_cfg_write32(io, loc->bus, loc->dev, loc->func, (u8)(off + 4u),
                              raw_hi);
    }

    out->index = index;
    out->is_io = (u8)is_io;
    out->is_64 = (u8)is_64;
    out->prefetch = (!is_io && (raw_lo & 0x8u)) ? 1 : 0;
    out->raw_lo = raw_lo;
    out->raw_hi = raw_hi;
    if (is_io) {
        out->size = pci_bar_size(mask_lo, 1);
        out->base = (u64)(raw_lo & 0xFFFFFFFCu);
    } else if (is_64) {
        u64 mask64 = ((u64)mask_hi << 32) | (u64)mask_lo;

        out->size = (~mask64 + 1ULL) & 0xFFFFFFFFFFFFFFFFULL;
        out->base = ((u64)raw_hi << 32) | (u64)(raw_lo & 0xFFFFFFF0u);
    } else {
        out->size = pci_bar_size(mask_lo, 0);
        out->base = (u64)(raw_lo & 0xFFFFFFF0u);
    }
    return (out->size == 0u) ? -1 : 0;
}

u16 pci_command(const struct pci_io_ops *io, const struct pci_loc *loc, int *ok)
{
    u16 cmd = 0;
    int rc;

    if (loc == NULL) {
        if (ok != NULL) {
            *ok = 0;
        }
        return 0;
    }
    rc = pci_cfg_read16(io, loc->bus, loc->dev, loc->func, PCI_CFG_COMMAND, &cmd);
    if (ok != NULL) {
        *ok = (rc == 0) ? 1 : 0;
    }
    return cmd;
}

int pci_enable(const struct pci_io_ops *io, const struct pci_loc *loc, u16 bits)
{
    u32 v;
    u16 cmd;
    int ok;

    cmd = pci_command(io, loc, &ok);
    if (!ok) {
        return -1;
    }
    /* 0x04 = command(低 16) + status(高 16)：只改 command，status 保持原值。 */
    if (pci_cfg_read32(io, loc->bus, loc->dev, loc->func, PCI_CFG_COMMAND, &v) != 0) {
        return -1;
    }
    v = (v & 0xFFFF0000u) | (u32)(u16)(cmd | bits);
    return pci_cfg_write32(io, loc->bus, loc->dev, loc->func, PCI_CFG_COMMAND, v);
}

static int scan_bus_range(const struct pci_io_ops *io, u16 vendor, u16 device,
                          u8 want_class, u8 want_subclass, struct pci_loc *out)
{
    u32 bus;
    u32 dev;
    u32 func;

    for (bus = 0; bus < 256u; bus++) {
        for (dev = 0; dev < 32u; dev++) {
            for (func = 0; func < 8u; func++) {
                u32 id;
                u32 cls;
                u16 v;
                u16 d;

                if (pci_cfg_read32(io, (u8)bus, (u8)dev, (u8)func, PCI_CFG_ID,
                                   &id) != 0) {
                    return -1;
                }
                v = (u16)(id & 0xFFFFu);
                d = (u16)(id >> 16);
                if (v == PCI_VENDOR_INVALID) {
                    if (func == 0u) {
                        break; /* 设备不存在：不用试其它 function */
                    }
                    continue;
                }
                if (want_class != 0xFFu) {
                    if (pci_cfg_read32(io, (u8)bus, (u8)dev, (u8)func, PCI_CFG_CLASS,
                                       &cls) != 0) {
                        return -1;
                    }
                    if ((u8)(cls >> 24) != want_class ||
                        (want_subclass != 0xFFu &&
                         (u8)((cls >> 16) & 0xFFu) != want_subclass)) {
                        goto next_func;
                    }
                }
                if (vendor != PCI_VENDOR_INVALID && v != vendor) {
                    goto next_func;
                }
                if (device != 0xFFFFu && d != device) {
                    goto next_func;
                }
                if (out != NULL) {
                    out->bus = (u8)bus;
                    out->dev = (u8)dev;
                    out->func = (u8)func;
                }
                return 0;

            next_func:
                if (func == 0u) {
                    u32 ht;

                    if (pci_cfg_read32(io, (u8)bus, (u8)dev, 0u, PCI_CFG_HEADER,
                                       &ht) != 0) {
                        return -1;
                    }
                    if (((ht >> 16) & 0x80u) == 0u) {
                        break; /* 单功能设备：跳过 function 1..7 */
                    }
                }
            }
        }
    }
    return -1;
}

int pci_find(const struct pci_io_ops *io, u16 vendor, u16 device,
             struct pci_loc *out)
{
    if (!io_ok(io)) {
        return -1;
    }
    return scan_bus_range(io, vendor, device, 0xFFu, 0xFFu, out);
}

int pci_find_class(const struct pci_io_ops *io, u8 class_code, u8 subclass,
                   struct pci_loc *out)
{
    if (!io_ok(io)) {
        return -1;
    }
    return scan_bus_range(io, PCI_VENDOR_INVALID, 0xFFFFu, class_code, subclass,
                          out);
}
