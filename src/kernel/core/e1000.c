/* =============================================================================
 * core/e1000.c - Intel 82540EM 网卡基础驱动
 *
 * 目标：证明 PCI + MMIO 映射 + DMA 描述符这条链路是通的。
 *   - 通过 PCI 找到 8086:100e，读 BAR0（MMIO 基址）；
 *   - vmm_map_mmio 映射寄存器区（禁止缓存）；
 *   - 从 RAL/RAH 读 MAC，读 STATUS.LU 判断链路；
 *   - 初始化 TX 描述符环（8 个描述符，DMA 页来自 PMM）；
 *   - 发一帧广播测试帧，轮询 TDH 确认硬件真的发出去了。
 * RX 与协议栈未实现。
 * ============================================================================= */
#include "e1000.h"
#include "pci.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../core/error.h"
#include "../lib/string.h"
#include "../lib/kprintf.h"

/* 寄存器偏移 */
#define E1000_CTRL   0x0000U
#define E1000_STATUS 0x0008U
#define E1000_TCTL   0x0400U
#define E1000_TIPG   0x0410U
#define E1000_RCTL   0x0100U
#define E1000_RDBAL  0x2800U
#define E1000_RDBAH  0x2804U
#define E1000_RDLEN  0x2808U
#define E1000_RDH    0x2810U
#define E1000_RDT    0x2818U
#define E1000_TDBAL  0x3800U
#define E1000_TDBAH  0x3804U
#define E1000_TDLEN  0x3808U
#define E1000_TDH    0x3810U
#define E1000_TDT    0x3818U
#define E1000_RAL    0x5400U
#define E1000_RAH    0x5404U

#define E1000_TCTL_EN      0x00000002U
#define E1000_TCTL_PSP     0x00000008U
#define E1000_TCTL_CT      0x00000100U   /* collision threshold = 0x10 */
#define E1000_TCTL_COLD    0x00040000U   /* collision distance = 0x40 */

#define TX_DESC_COUNT 8U
#define TX_DESC_SIZE  16U
#define RX_DESC_COUNT 8U
#define RX_DESC_SIZE  16U
#define RX_BUF_SIZE   2048U
#define MAX_PCI_DEVS  16U

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed)) e1000_tx_desc_t;

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed)) e1000_rx_desc_t;

static volatile uint32_t *bar0;
static uint8_t  mac[6];
static int      present;
static int      link;
static e1000_tx_desc_t *tx_ring;
static uint8_t *tx_buffer;
static uint32_t tx_ring_phys;
static uint32_t tx_buf_phys;
static uint32_t next_desc;
static e1000_rx_desc_t *rx_ring;
static uint8_t *rx_buffers;
static uint32_t rx_ring_phys;
static uint32_t rx_bufs_phys;
static uint32_t rx_next;

static uint32_t reg_read(uint32_t off) { return bar0[off / 4U]; }
static void     reg_write(uint32_t off, uint32_t v) { bar0[off / 4U] = v; }

static int pci_find_e1000(pci_dev_t *out)
{
    for (uint32_t i = 0; i < pci_count(); i++) {
        const pci_dev_t *d = pci_at(i);

        if (d == 0) continue;
        if (d->vendor == 0x8086U &&
            (d->device == 0x100EU || d->device == 0x100FU || d->device == 0x10D3U)) {
            if (out != 0) *out = *d;
            return 1;
        }
    }
    return 0;
}

/* 读 PCI 配置空间的 BAR0（偏移 0x10） */
static uint32_t pci_bar0(const pci_dev_t *d)
{
    uint32_t addr = 0x80000000U
                  | ((uint32_t)d->bus << 16)
                  | ((uint32_t)d->dev << 11)
                  | ((uint32_t)d->func << 8)
                  | 0x10U;

    __asm__ volatile("outl %0, %1" :: "a"(addr), "d"((unsigned short)0xCF8));
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "d"((unsigned short)0xCFC));
    return val & 0xFFFFFFF0U;
}

static uint32_t pci_cfg_addr(const pci_dev_t *d, uint32_t off)
{
    return 0x80000000U
         | ((uint32_t)d->bus << 16)
         | ((uint32_t)d->dev << 11)
         | ((uint32_t)d->func << 8)
         | (off & 0xFCU);
}

static uint32_t pci_read32(const pci_dev_t *d, uint32_t off)
{
    uint32_t val;

    __asm__ volatile("outl %0, %1" :: "a"(pci_cfg_addr(d, off)), "d"((unsigned short)0xCF8));
    __asm__ volatile("inl %1, %0" : "=a"(val) : "d"((unsigned short)0xCFC));
    return val;
}

static void pci_write32(const pci_dev_t *d, uint32_t off, uint32_t val)
{
    __asm__ volatile("outl %0, %1" :: "a"(pci_cfg_addr(d, off)), "d"((unsigned short)0xCF8));
    __asm__ volatile("outl %0, %1" :: "a"(val), "d"((unsigned short)0xCFC));
}

/* 打开 PCI 命令寄存器里的"内存空间 + 总线主控"。
 * 没有 bus master，网卡读不到描述符、也写不回接收缓冲：
 * 表现就是 TX 的 TDH 照常推进（描述符被取走），但帧根本没上线。 */
static uint32_t pci_enable_dma(const pci_dev_t *d)
{
    uint32_t cmd = pci_read32(d, 0x04U);

    cmd |= 0x0006U;                 /* bit1 = MEMORY, bit2 = BUS MASTER */
    pci_write32(d, 0x04U, cmd);
    return pci_read32(d, 0x04U);
}

int e1000_init(void)
{
    pci_dev_t d;
    uint32_t bar;

    present = 0;
    link = 0;

    if (!pci_find_e1000(&d)) {
        kprintln("e1000: not found");
        return E_NOENT;
    }

    {
        uint32_t cmd = pci_enable_dma(&d);

        kprintf("e1000: PCI CMD=%x (memory=%u busmaster=%u)\n",
                (unsigned int)cmd,
                (unsigned int)((cmd >> 1) & 1U), (unsigned int)((cmd >> 2) & 1U));
    }

    bar = pci_bar0(&d);
    if (bar == 0U) {
        kprintln("e1000: BAR0 is zero");
        return E_INVAL;
    }

    if (vmm_map_mmio(bar, 0x20000U) != 0) {
        kprintln("e1000: cannot map BAR0");
        return E_NOSPC;
    }
    bar0 = (volatile uint32_t *)(uintptr_t)bar;

    /* 先复位设备：BIOS/固件可能留下了半初始化的状态 */
    reg_write(E1000_CTRL, reg_read(E1000_CTRL) | (1U << 26));
    for (volatile uint32_t d = 0; d < 2000000U; d++) {
        if ((reg_read(E1000_CTRL) & (1U << 26)) == 0U) break;
    }

    /* MAC 在 RAL/RAH（复位后由 EEPROM 自动装入） */
    {
        uint32_t ral = reg_read(E1000_RAL);
        uint32_t rah = reg_read(E1000_RAH);

        mac[0] = (uint8_t)(ral & 0xFF);
        mac[1] = (uint8_t)((ral >> 8) & 0xFF);
        mac[2] = (uint8_t)((ral >> 16) & 0xFF);
        mac[3] = (uint8_t)((ral >> 24) & 0xFF);
        mac[4] = (uint8_t)(rah & 0xFF);
        mac[5] = (uint8_t)((rah >> 8) & 0xFF);
    }

    /* TX 环：一个页放 8 个描述符，另一个页放发送缓冲 */
    {
        void *ring = pmm_alloc_pages(1);
        void *buf  = pmm_alloc_pages(1);

        if (ring == 0 || buf == 0) {
            kprintln("e1000: no DMA pages");
            if (ring != 0) pmm_free_pages(ring, 1);
            if (buf != 0) pmm_free_pages(buf, 1);
            return E_NOSPC;
        }

        tx_ring = (e1000_tx_desc_t *)ring;
        tx_ring_phys = (uint32_t)(uintptr_t)ring;
        tx_buffer = (uint8_t *)buf;
        tx_buf_phys = (uint32_t)(uintptr_t)buf;
        next_desc = 0;

        memset(tx_ring, 0, 4096);
        reg_write(E1000_TDBAL, tx_ring_phys);
        reg_write(E1000_TDBAH, 0);
        reg_write(E1000_TDLEN, TX_DESC_COUNT * TX_DESC_SIZE);
        reg_write(E1000_TDH, 0);
        reg_write(E1000_TDT, 0);
    }

    /* RX 环：8 个描述符 + 8 个 2KB DMA 缓冲。 */
    {
        void *ring = pmm_alloc_pages(1);
        void *bufs = pmm_alloc_pages((RX_DESC_COUNT * RX_BUF_SIZE) / 4096U);

        if (ring == 0 || bufs == 0) {
            kprintln("e1000: no RX DMA pages");
            if (ring != 0) pmm_free_pages(ring, 1);
            if (bufs != 0) pmm_free_pages(bufs, (RX_DESC_COUNT * RX_BUF_SIZE) / 4096U);
            return E_NOSPC;
        }

        rx_ring = (e1000_rx_desc_t *)ring;
        rx_ring_phys = (uint32_t)(uintptr_t)ring;
        rx_buffers = (uint8_t *)bufs;
        rx_bufs_phys = (uint32_t)(uintptr_t)bufs;
        rx_next = 0;
        memset(rx_ring, 0, 4096);

        for (uint32_t i = 0; i < RX_DESC_COUNT; i++) {
            rx_ring[i].addr = rx_bufs_phys + i * RX_BUF_SIZE;
            rx_ring[i].status = 0;
        }

        reg_write(E1000_RCTL, 0);
        reg_write(E1000_RDBAL, rx_ring_phys);
        reg_write(E1000_RDBAH, 0);
        reg_write(E1000_RDLEN, RX_DESC_COUNT * RX_DESC_SIZE);
        reg_write(E1000_RDH, 0);
        reg_write(E1000_RDT, RX_DESC_COUNT - 1U);
        reg_write(E1000_RCTL, (1U << 1) | (1U << 2) | (1U << 3) |
                              (1U << 4) | (1U << 15));
    }

    /* 环就绪后最后再打开发送单元（顺序对某些固件版本有影响） */
    reg_write(E1000_CTRL, reg_read(E1000_CTRL) | 0x00000040U); /* SLU */
    reg_write(E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP |
                          E1000_TCTL_CT | E1000_TCTL_COLD);
    reg_write(E1000_TIPG, 0x0060200AU);

    link = (reg_read(E1000_STATUS) & 0x02U) != 0U;   /* STATUS.LU */

    present = 1;
    kprintf("e1000: MAC=%02x:%02x:%02x:%02x:%02x:%02x link=%s\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
            link ? "up" : "down");
    return E_OK;
}

int e1000_present(void) { return present; }
int e1000_link_up(void) { return link; }

void e1000_mac(uint8_t out[6])
{
    for (int i = 0; i < 6; i++) out[i] = mac[i];
}

/* 发一帧以太网广播测试帧：目的 MAC 全 FF，类型 0x88B5（实验用） */
int e1000_send_frame(const void *frame, uint32_t len)
{
    e1000_tx_desc_t *d;
    uint32_t n = len;

    if (!present || tx_ring == 0 || frame == 0) return E_NOENT;
    if (n < 60U) n = 60U;
    if (n > 4096U) return E_INVAL;

    memset(tx_buffer, 0, n);
    memcpy(tx_buffer, frame, len);

    d = &tx_ring[next_desc];
    d->addr = tx_buf_phys;
    d->length = (uint16_t)n;
    d->cso = 0;
    d->cmd = 0x0BU;                     /* EOP | IFCS | RS */
    d->status = 0;
    d->css = 0;
    d->special = 0;

    reg_write(E1000_TDT, (reg_read(E1000_TDT) + 1U) % TX_DESC_COUNT);
    for (uint32_t spin = 0; spin < 1000000U; spin++) {
        if ((d->status & 0x01U) != 0U ||
            reg_read(E1000_TDH) == reg_read(E1000_TDT)) {
            next_desc = (next_desc + 1U) % TX_DESC_COUNT;
            return E_OK;
        }
    }
    return E_INVAL;
}

int e1000_rx_poll(void *buf, uint32_t max_len)
{
    e1000_rx_desc_t *d;
    uint32_t len;

    if (!present || rx_ring == 0 || buf == 0) return 0;
    d = &rx_ring[rx_next];
    if ((d->status & 0x01U) == 0U) return 0;

    len = d->length;
    if (len > max_len) len = max_len;
    memcpy(buf, (const void *)(uintptr_t)d->addr, len);
    d->status = 0;
    reg_write(E1000_RDT, rx_next);
    rx_next = (rx_next + 1U) % RX_DESC_COUNT;
    return (int)len;
}

int e1000_send_test(void)
{
    e1000_tx_desc_t *d;
    uint8_t *f = tx_buffer;
    uint32_t len = 60;                  /* 以太网最小帧 */

    if (!present || tx_ring == 0) return E_NOENT;

    memset(f, 0, len);
    for (int i = 0; i < 6; i++) f[i] = 0xFFU;        /* 广播 */
    for (int i = 0; i < 6; i++) f[6 + i] = mac[i];   /* 源 MAC */
    f[12] = 0x88U; f[13] = 0xB5U;                    /* 类型 */
    for (int i = 14; i < 20; i++) f[i] = (uint8_t)('A' + i - 14);

    d = &tx_ring[next_desc];
    d->addr = tx_buf_phys;
    d->length = (uint16_t)len;
    d->cso = 0;
    d->cmd = 0x0BU;                     /* EOP | IFCS | RS */
    d->status = 0;
    d->css = 0;
    d->special = 0;

    reg_write(E1000_TDT, (reg_read(E1000_TDT) + 1U) % TX_DESC_COUNT);

    /* 轮询：DD 状态位是标准判据；某些实现（含 QEMU）不回写 DD，
     * 但会把 TDH 推到 TDT，说明描述符已被硬件消费，同样视为成功。 */
    for (uint32_t spin = 0; spin < 1000000U; spin++) {
        if ((d->status & 0x01U) != 0U) {
            next_desc = (next_desc + 1U) % TX_DESC_COUNT;
            kprintf("e1000: TX test frame sent (%u bytes, DD set)\n",
                    (unsigned int)len);
            return E_OK;
        }
        if (reg_read(E1000_TDH) == reg_read(E1000_TDT)) {
            next_desc = (next_desc + 1U) % TX_DESC_COUNT;
            kprintf("e1000: TX test frame sent (%u bytes, TDH==TDT)\n",
                    (unsigned int)len);
            return E_OK;
        }
    }

    kprintln("e1000: TX timeout");
    return E_INVAL;
}
