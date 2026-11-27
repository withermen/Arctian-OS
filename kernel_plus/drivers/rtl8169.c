/* ===================================================================== *
 * rtl8169.c — Realtek RTL8168 / RTL8169 / RTL8136 / RTL8125 / RTL8161
 * Gigabit / 2.5G Ethernet sürücüsü (MMIO BAR2/BAR1 & I/O BAR0)
 * ===================================================================== */

#include "kernel.h"
#include "pci.h"
#include "net.h"
#include "isr.h"
#include "pic.h"
#include "mmio.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* RTL8168 / RTL8169 / RTL8125 donanım kayıtları */
#define RTL_REG_MAC0            0x00
#define RTL_REG_MAR0            0x08
#define RTL_REG_TNPDS_LO        0x20    /* TxDescStartAddrLow  */
#define RTL_REG_TNPDS_HI        0x24    /* TxDescStartAddrHigh */
#define RTL_REG_CHIPCMD         0x37    /* ChipCmd             */
#define RTL_REG_TXPOLL          0x38    /* TxPoll (8168/8169)  */
#define RTL_REG_IMR             0x3C    /* IMR (8168/8169)     */
#define RTL_REG_ISR             0x3E    /* ISR (8168/8169)     */
#define RTL_REG_TXCONFIG        0x40    /* TxConfig            */
#define RTL_REG_RXCONFIG        0x44    /* RxConfig            */
#define RTL_REG_CFG9346         0x50    /* Cfg9346             */
#define RTL_REG_PHYSTATUS       0x6C    /* PHYStatus           */
#define RTL_REG_TXPOLL_8125     0x90    /* TPPOLL (RTL8125)    */
#define RTL_REG_IMR_8125        0x38    /* IMR0 (RTL8125)      */
#define RTL_REG_ISR_8125        0x3C    /* ISR0 (RTL8125)      */
#define RTL_REG_RXMAXSIZE       0xDA    /* RxMaxSize           */
#define RTL_REG_CPLUSCMD        0xE0    /* CPlusCmd            */
#define RTL_REG_RDSAR_LO        0xE4    /* RxDescAddrLow       */
#define RTL_REG_RDSAR_HI        0xE8    /* RxDescAddrHigh      */
#define RTL_REG_MTPS            0xEC    /* MaxTxPacketSize     */

/* ChipCmd (0x37) bitleri */
#define RTL_CMD_TE              0x04    /* Transmitter Enable  */
#define RTL_CMD_RE              0x08    /* Receiver Enable     */
#define RTL_CMD_RESET           0x10    /* Software Reset      */

/* TxPoll bitleri */
#define RTL_TXPOLL_NPQ          0x40    /* Normal Priority Queue (8168/8169) */
#define RTL_TXPOLL_8125_NPQ     0x01    /* Normal Priority Queue (8125)      */

/* IMR / ISR bitleri */
#define RTL_INT_ROK             0x0001
#define RTL_INT_RER             0x0002
#define RTL_INT_TOK             0x0004
#define RTL_INT_TER             0x0008
#define RTL_INT_RDU             0x0010
#define RTL_INT_LINK            0x0020
#define RTL_INT_FOVW            0x0040
#define RTL_INT_TDU             0x0080

/* TxConfig (0x40) / RxConfig (0x44) */
#define RTL_TCR_IFG_STD         (3u << 24)
#define RTL_TCR_MXDMA_1024      (6u << 8)
#define RTL_RCR_AAP             (1u << 0)
#define RTL_RCR_APM             (1u << 1)
#define RTL_RCR_AM              (1u << 2)
#define RTL_RCR_AB              (1u << 3)
#define RTL_RCR_MXDMA_1024      (6u << 8)
#define RTL_RCR_RXFTH_NONE      (7u << 13)

/* Cfg9346 (0x50) */
#define RTL_CFG9346_LOCK        0x00
#define RTL_CFG9346_UNLOCK      0xC0

/* PHYStatus (0x6C) */
#define RTL_PHY_LINK_STS        0x02

/* CPlusCmd (0xE0) */
#define RTL_CPLUS_MULRW         0x0008
#define RTL_CPLUS_PCIDAC        0x0010

/* Descriptor opts1 bitleri */
#define RTL_DESC_OWN            0x80000000u
#define RTL_DESC_EOR            0x40000000u
#define RTL_DESC_FS             0x20000000u
#define RTL_DESC_LS             0x10000000u
#define RTL_RX_RES              0x00200000u
#define RTL_RX_LEN_MASK         0x00003FFFu

#define RTL8169_RXDESC_NUM      64
#define RTL8169_TXDESC_NUM      16
#define RTL8169_RX_BUF_SIZE     2048
#define RTL8169_TX_BUF_SIZE     2048

typedef struct {
    uint32_t opts1;
    uint32_t opts2;
    uint32_t addr_lo;
    uint32_t addr_hi;
} __attribute__((packed)) rtl_desc_t;

typedef struct {
    volatile uint8_t *mmio;
    uint64_t mmio_base;
    uint16_t io_base;
    bool     use_mmio;
    bool     is_8125;
    uint16_t rx_cur;
    uint16_t tx_cur;
    uint8_t  irq;
    uint8_t  mac[6];
    bool     active;
} rtl8169_t;

/* Donanım TNPDS / RDSAR için 256-byte hizalı DMA halkaları */
static rtl_desc_t rtl_rx_ring[RTL8169_RXDESC_NUM] __attribute__((aligned(256)));
static rtl_desc_t rtl_tx_ring[RTL8169_TXDESC_NUM] __attribute__((aligned(256)));
static uint8_t    rtl_rx_bufs[RTL8169_RXDESC_NUM][RTL8169_RX_BUF_SIZE] __attribute__((aligned(64)));
static uint8_t    rtl_tx_bufs[RTL8169_TXDESC_NUM][RTL8169_TX_BUF_SIZE] __attribute__((aligned(64)));
static rtl8169_t  r8169;

static inline uint8_t r_rd8(uint16_t off) {
    if (r8169.use_mmio) return *(volatile uint8_t *)(r8169.mmio + off);
    return inb((uint16_t)(r8169.io_base + off));
}

static inline uint16_t r_rd16(uint16_t off) {
    if (r8169.use_mmio) return *(volatile uint16_t *)(r8169.mmio + off);
    return inw((uint16_t)(r8169.io_base + off));
}

static inline uint32_t r_rd32(uint16_t off) {
    if (r8169.use_mmio) return *(volatile uint32_t *)(r8169.mmio + off);
    return inl((uint16_t)(r8169.io_base + off));
}

static inline void r_wr8(uint16_t off, uint8_t v) {
    if (r8169.use_mmio) *(volatile uint8_t *)(r8169.mmio + off) = v;
    else outb((uint16_t)(r8169.io_base + off), v);
}

static inline void r_wr16(uint16_t off, uint16_t v) {
    if (r8169.use_mmio) *(volatile uint16_t *)(r8169.mmio + off) = v;
    else outw((uint16_t)(r8169.io_base + off), v);
}

static inline void r_wr32(uint16_t off, uint32_t v) {
    if (r8169.use_mmio) *(volatile uint32_t *)(r8169.mmio + off) = v;
    else outl((uint16_t)(r8169.io_base + off), v);
}

static void rtl8169_irq_handler(void) {
    if (!r8169.active) return;
    /* Kesme kaynağını temizle; RX halkası tek yoldan (poll) tüketilir. */
    if (r8169.is_8125) {
        uint32_t isr = r_rd32(RTL_REG_ISR_8125);
        if (isr) r_wr32(RTL_REG_ISR_8125, isr);
    } else {
        uint16_t isr = r_rd16(RTL_REG_ISR);
        if (isr) r_wr16(RTL_REG_ISR, isr);
    }
}

static bool rtl8169_select_bar(net_controller_t *nc) {
    static const int mmio_order[4] = { 2, 1, 4, 0 };
    static const int io_order[3]   = { 0, 1, 2 };

    for (int i = 0; i < 4; i++) {
        int b = mmio_order[i];
        if (!nc->bar[b].is_io && nc->bar[b].address != 0) {
            uint64_t sz = nc->bar[b].size ? nc->bar[b].size : 0x1000ull;
            mmio_map(nc->bar[b].address, sz);
            r8169.mmio_base = nc->bar[b].address;
            r8169.mmio = (volatile uint8_t *)(uintptr_t)r8169.mmio_base;
            r8169.use_mmio = true;
            return true;
        }
    }
    for (int i = 0; i < 3; i++) {
        int b = io_order[i];
        if (nc->bar[b].is_io && nc->bar[b].address != 0) {
            r8169.io_base = (uint16_t)(nc->bar[b].address & 0xFFFCu);
            r8169.use_mmio = false;
            return true;
        }
    }
    return false;
}

static void rtl8169_reset_rx_desc(uint16_t idx) {
    uint64_t buf_pa = (uint64_t)(uintptr_t)rtl_rx_bufs[idx];
    uint32_t eor = (idx == RTL8169_RXDESC_NUM - 1) ? RTL_DESC_EOR : 0u;
    rtl_rx_ring[idx].addr_lo = (uint32_t)(buf_pa & 0xFFFFFFFFu);
    rtl_rx_ring[idx].addr_hi = (uint32_t)(buf_pa >> 32);
    rtl_rx_ring[idx].opts2 = 0;
    __asm__ volatile("mfence" ::: "memory");
    rtl_rx_ring[idx].opts1 = RTL_DESC_OWN | eor | (uint32_t)RTL8169_RX_BUF_SIZE;
}

void rtl8169_init(net_controller_t *nc) {
    memset(&r8169, 0, sizeof(r8169));
    if (!nc) return;

    if (!rtl8169_select_bar(nc)) {
        kernel_debug("RTL8169: uygun MMIO/IO BAR bulunamadi!");
        return;
    }

    r8169.irq = nc->dev.irq;
    r8169.is_8125 = (nc->dev.device == 0x8125);

    pci_enable_bus_mastering(nc->dev.bus, nc->dev.slot, nc->dev.func);
    {
        uint16_t cmd = pci_config_readw(nc->dev.bus, nc->dev.slot, nc->dev.func, PCI_CONFIG_COMMAND);
        cmd &= (uint16_t)~PCI_CMD_INT_DISABLE;
        cmd |= PCI_CMD_BUS_MASTER | PCI_CMD_MEM_SPACE | PCI_CMD_IO_SPACE;
        pci_config_writew(nc->dev.bus, nc->dev.slot, nc->dev.func, PCI_CONFIG_COMMAND, cmd);
    }

    /* Donanım sıfırlama (ChipCmd.RST) */
    r_wr8(RTL_REG_CHIPCMD, RTL_CMD_RESET);
    for (volatile int i = 0; i < 100000; i++) {
        if ((r_rd8(RTL_REG_CHIPCMD) & RTL_CMD_RESET) == 0) break;
        asm volatile ("pause");
    }

    /* Kesmeleri maskele ve bekleyen durumları temizle */
    if (r8169.is_8125) {
        r_wr32(RTL_REG_IMR_8125, 0);
        r_wr32(RTL_REG_ISR_8125, 0xFFFFFFFFu);
    } else {
        r_wr16(RTL_REG_IMR, 0x0000);
        r_wr16(RTL_REG_ISR, 0xFFFF);
    }

    /* MAC adresini oku (MAC0..5) */
    bool all_zero = true, all_ff = true;
    for (int i = 0; i < 6; i++) {
        r8169.mac[i] = r_rd8((uint16_t)(RTL_REG_MAC0 + i));
        if (r8169.mac[i] != 0x00) all_zero = false;
        if (r8169.mac[i] != 0xFF) all_ff = false;
    }

    r_wr8(RTL_REG_CFG9346, RTL_CFG9346_UNLOCK);

    if (all_zero || all_ff) {
        r8169.mac[0] = 0x02; r8169.mac[1] = 0x00;
        r8169.mac[2] = 0x00; r8169.mac[3] = 0x81;
        r8169.mac[4] = 0x69; r8169.mac[5] = 0x01;
        for (int i = 0; i < 6; i++) {
            r_wr8((uint16_t)(RTL_REG_MAC0 + i), r8169.mac[i]);
        }
    }
    memcpy(net_if.mac, r8169.mac, 6);
    net_if.valid = true;

    /* RX / TX halkalarını kur (256-byte hizalı) */
    memset(rtl_rx_bufs, 0, sizeof(rtl_rx_bufs));
    memset(rtl_tx_bufs, 0, sizeof(rtl_tx_bufs));
    for (uint16_t i = 0; i < RTL8169_RXDESC_NUM; i++) {
        rtl8169_reset_rx_desc(i);
    }
    for (uint16_t i = 0; i < RTL8169_TXDESC_NUM; i++) {
        uint64_t buf_pa = (uint64_t)(uintptr_t)rtl_tx_bufs[i];
        rtl_tx_ring[i].addr_lo = (uint32_t)(buf_pa & 0xFFFFFFFFu);
        rtl_tx_ring[i].addr_hi = (uint32_t)(buf_pa >> 32);
        rtl_tx_ring[i].opts2 = 0;
        rtl_tx_ring[i].opts1 = (i == RTL8169_TXDESC_NUM - 1) ? RTL_DESC_EOR : 0u;
    }
    r8169.rx_cur = 0;
    r8169.tx_cur = 0;

    /* CPlusCmd, RxMaxSize, MaxTxPacketSize */
    uint16_t cplus = RTL_CPLUS_MULRW;
    if (((uintptr_t)rtl_rx_ring >> 32) != 0 || ((uintptr_t)rtl_tx_ring >> 32) != 0) {
        cplus |= RTL_CPLUS_PCIDAC;
    }
    r_wr16(RTL_REG_CPLUSCMD, cplus);
    r_wr16(RTL_REG_RXMAXSIZE, 0x05F3u);   /* 1523 byte max Ethernet + VLAN + FCS */
    r_wr8(RTL_REG_MTPS, 0x3Fu);           /* MaxTxPacketSize: 63 * 128B */

    /* Halka taban adresleri (64-bit) */
    uint64_t tx_pa = (uint64_t)(uintptr_t)rtl_tx_ring;
    uint64_t rx_pa = (uint64_t)(uintptr_t)rtl_rx_ring;
    r_wr32(RTL_REG_TNPDS_LO, (uint32_t)(tx_pa & 0xFFFFFFFFu));
    r_wr32(RTL_REG_TNPDS_HI, (uint32_t)(tx_pa >> 32));
    r_wr32(RTL_REG_RDSAR_LO, (uint32_t)(rx_pa & 0xFFFFFFFFu));
    r_wr32(RTL_REG_RDSAR_HI, (uint32_t)(rx_pa >> 32));

    /* TxConfig / RxConfig ve TX/RX etkinleştirme */
    r_wr8(RTL_REG_CHIPCMD, RTL_CMD_TE | RTL_CMD_RE);
    r_wr32(RTL_REG_TXCONFIG, RTL_TCR_IFG_STD | RTL_TCR_MXDMA_1024);
    r_wr32(RTL_REG_RXCONFIG, RTL_RCR_APM | RTL_RCR_AM | RTL_RCR_AB |
                             RTL_RCR_RXFTH_NONE | RTL_RCR_MXDMA_1024);

    /* Multicast filtreyi aç (IPv6 NDP / multicast için) */
    r_wr32(RTL_REG_MAR0 + 0, 0xFFFFFFFFu);
    r_wr32(RTL_REG_MAR0 + 4, 0xFFFFFFFFu);

    r_wr8(RTL_REG_CFG9346, RTL_CFG9346_LOCK);

    kernel_debug("RTL8169: %s=0x%llX IRQ=%d MAC=%02X:%02X:%02X:%02X:%02X:%02X",
                 r8169.use_mmio ? "MMIO" : "IO",
                 (unsigned long long)(r8169.use_mmio ? r8169.mmio_base : r8169.io_base),
                 r8169.irq, r8169.mac[0], r8169.mac[1], r8169.mac[2],
                 r8169.mac[3], r8169.mac[4], r8169.mac[5]);

    if (r8169.irq != 0xFF && r8169.irq < 16) {
        if (r8169.is_8125) r_wr32(RTL_REG_IMR_8125, RTL_INT_ROK | RTL_INT_TOK | RTL_INT_LINK);
        else r_wr16(RTL_REG_IMR, (uint16_t)(RTL_INT_ROK | RTL_INT_TOK | RTL_INT_LINK));
        isr_install_handler(r8169.irq, rtl8169_irq_handler);
        pic_set_mask(r8169.irq, false);
    }

    r8169.active = true;
    nc->initialized = true;
}

bool rtl8169_send_packet(void *ctx, const uint8_t *data, uint16_t length) {
    (void)ctx;
    if (!r8169.active || !data || length == 0 || length > RTL8169_TX_BUF_SIZE) return false;

    uint16_t cur = r8169.tx_cur;
    if (rtl_tx_ring[cur].opts1 & RTL_DESC_OWN) {
        for (volatile int w = 0; w < 100000; w++) {
            if (!(rtl_tx_ring[cur].opts1 & RTL_DESC_OWN)) break;
            asm volatile ("pause");
        }
        if (rtl_tx_ring[cur].opts1 & RTL_DESC_OWN) return false;
    }

    uint16_t tx_len = length;
    memcpy(rtl_tx_bufs[cur], data, length);
    if (tx_len < 60) {
        memset(rtl_tx_bufs[cur] + tx_len, 0, (size_t)(60u - tx_len));
        tx_len = 60;
    }

    uint64_t buf_pa = (uint64_t)(uintptr_t)rtl_tx_bufs[cur];
    uint32_t eor = (cur == RTL8169_TXDESC_NUM - 1) ? RTL_DESC_EOR : 0u;

    rtl_tx_ring[cur].addr_lo = (uint32_t)(buf_pa & 0xFFFFFFFFu);
    rtl_tx_ring[cur].addr_hi = (uint32_t)(buf_pa >> 32);
    rtl_tx_ring[cur].opts2 = 0;
    __asm__ volatile("mfence" ::: "memory");
    rtl_tx_ring[cur].opts1 = RTL_DESC_OWN | RTL_DESC_FS | RTL_DESC_LS | eor | (uint32_t)tx_len;
    __asm__ volatile("mfence" ::: "memory");

    r8169.tx_cur = (uint16_t)((cur + 1) % RTL8169_TXDESC_NUM);

    if (r8169.is_8125) r_wr8(RTL_REG_TXPOLL_8125, RTL_TXPOLL_8125_NPQ);
    else r_wr8(RTL_REG_TXPOLL, RTL_TXPOLL_NPQ);

    return true;
}

bool rtl8169_has_packet(void *ctx) {
    (void)ctx;
    if (!r8169.active) return false;
    __asm__ volatile("" ::: "memory");
    return (rtl_rx_ring[r8169.rx_cur].opts1 & RTL_DESC_OWN) == 0;
}

bool rtl8169_receive_packet(void *ctx, uint8_t *buf, uint16_t *length) {
    (void)ctx;
    if (!r8169.active) return false;

    for (int n = 0; n < RTL8169_RXDESC_NUM; n++) {
        uint16_t idx = r8169.rx_cur;
        __asm__ volatile("mfence" ::: "memory");
        uint32_t opts1 = rtl_rx_ring[idx].opts1;

        if (opts1 & RTL_DESC_OWN) return false;

        /* Hatalı veya tek-parça olmayan çerçeveyi geri ver ve devam et */
        if ((opts1 & RTL_RX_RES) || (opts1 & (RTL_DESC_FS | RTL_DESC_LS)) != (RTL_DESC_FS | RTL_DESC_LS)) {
            rtl8169_reset_rx_desc(idx);
            r8169.rx_cur = (uint16_t)((idx + 1) % RTL8169_RXDESC_NUM);
            continue;
        }

        uint32_t raw_len = opts1 & RTL_RX_LEN_MASK;
        uint32_t pkt_len = (raw_len >= 18u) ? (raw_len - 4u) : raw_len;
        if (pkt_len > NET_PKT_MAX) pkt_len = NET_PKT_MAX;

        bool copied = false;
        if (pkt_len > 0 && buf && length && *length >= pkt_len) {
            memcpy(buf, rtl_rx_bufs[idx], pkt_len);
            *length = (uint16_t)pkt_len;
            copied = true;
        } else if (length) {
            *length = (uint16_t)pkt_len;
        }

        rtl8169_reset_rx_desc(idx);
        r8169.rx_cur = (uint16_t)((idx + 1) % RTL8169_RXDESC_NUM);
        return copied;
    }
    return false;
}

bool rtl8169_send(const uint8_t *data, uint16_t length) {
    return rtl8169_send_packet(0, data, length);
}

bool rtl8169_receive(uint8_t *buf, uint16_t *length) {
    return rtl8169_receive_packet(0, buf, length);
}

bool rtl8169_link_up(void) {
    if (!r8169.active) return false;
    return (r_rd8(RTL_REG_PHYSTATUS) & RTL_PHY_LINK_STS) != 0;
}

int rtl8169_selftest_layout(void) {
    if (sizeof(rtl_desc_t) != 16) return -1;
    if (((uintptr_t)rtl_rx_ring & 0xFFu) != 0) return -2;
    if (((uintptr_t)rtl_tx_ring & 0xFFu) != 0) return -3;
    return 0;
}
