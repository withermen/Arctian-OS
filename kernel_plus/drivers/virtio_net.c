/**
 * Arctian VirtIO-Net PCI Driver (PCI 0x1AF4:0x1041 Modern / 0x1AF4:0x1000 Transitional)
 *
 * Implements VirtIO 1.0/1.2 Modern PCI capability discovery (COMMON_CFG,
 * NOTIFY_CFG, ISR_CFG, DEVICE_CFG), Split Virtqueue receiveq (q0) and
 * transmitq (q1), polling mode (PCI_CMD_INT_DISABLE + VIRTQ_AVAIL_F_NO_INTERRUPT),
 * and dynamic DMA ring/buffer allocation via vmm_alloc_frame().
 */
#include "virtio_net.h"
#include "net.h"
#include "kernel.h"
#include "pci.h"
#include "paging.h"
#include "mmio.h"
#include "serial.h"

/* VirtIO PCI capability types */
#define VIRTIO_PCI_CAP_COMMON_CFG  1u
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2u
#define VIRTIO_PCI_CAP_ISR_CFG     3u
#define VIRTIO_PCI_CAP_DEVICE_CFG  4u

/* VirtIO device status bits */
#define VIRTIO_STATUS_ACKNOWLEDGE  0x01u
#define VIRTIO_STATUS_DRIVER       0x02u
#define VIRTIO_STATUS_DRIVER_OK    0x04u
#define VIRTIO_STATUS_FEATURES_OK  0x08u

/* VirtIO feature bits */
#define VIRTIO_NET_F_MAC           (1u << 5)
#define VIRTIO_NET_F_STATUS        (1u << 16)
#define VIRTIO_F_VERSION_1_W1      (1u << 0)   /* bit 32 -> feature word 1 bit 0 */

#define VIRTIO_NET_S_LINK_UP       1u

/* Split Virtqueue flags */
#define VIRTQ_DESC_F_NEXT          1u
#define VIRTQ_DESC_F_WRITE         2u
#define VIRTQ_AVAIL_F_NO_INTERRUPT 1u

#define VNET_MAX_QSIZE  64u
#define VNET_MAX_BUFS   32u
#define VNET_BUF_STRIDE 2048u
#define VNET_HDR_STRIDE 16u

typedef struct __attribute__((packed)) {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t  device_status;
    uint8_t  config_generation;
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint32_t queue_desc_lo;
    uint32_t queue_desc_hi;
    uint32_t queue_driver_lo;
    uint32_t queue_driver_hi;
    uint32_t queue_device_lo;
    uint32_t queue_device_hi;
} vnet_common_cfg_t;

typedef struct __attribute__((packed)) {
    uint8_t  mac[6];
    uint16_t status;
    uint16_t max_virtqueue_pairs;
    uint16_t mtu;
} vnet_dev_cfg_t;

typedef struct __attribute__((packed)) {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
} vnet_hdr_v1_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} vnet_virtq_desc_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VNET_MAX_QSIZE];
    uint16_t used_event;
} vnet_virtq_avail_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t len;
} vnet_virtq_used_elem_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    vnet_virtq_used_elem_t ring[VNET_MAX_QSIZE];
    uint16_t avail_event;
} vnet_virtq_used_t;

typedef struct {
    uint16_t                     qsize;
    uint16_t                     nbufs;
    volatile vnet_virtq_desc_t  *desc;
    volatile vnet_virtq_avail_t *avail;
    volatile vnet_virtq_used_t  *used;
    uint64_t                     desc_pa;
    uint64_t                     avail_pa;
    uint64_t                     used_pa;
    uint16_t                     last_used_idx;
    volatile uint16_t           *notify;
    uint16_t                     qidx;

    uint64_t                     hdr_pa;
    uint8_t                     *hdr_va;
    uint64_t                     buf_pages_pa[VNET_MAX_BUFS / 2u];
    uint8_t                     *buf_pages_va[VNET_MAX_BUFS / 2u];
} vnet_queue_t;

typedef struct {
    bool                       present;
    bool                       has_status;
    uint8_t                    bus;
    uint8_t                    slot;
    uint8_t                    func;
    uint16_t                   device_id;
    uint8_t                    mac[6];

    volatile vnet_common_cfg_t *common;
    volatile uint8_t           *notify_base;
    uint32_t                    notify_mult;
    volatile uint8_t           *isr_cfg;
    volatile vnet_dev_cfg_t    *dev_cfg;

    vnet_queue_t rxq;
    vnet_queue_t txq;
    bool         tx_busy[VNET_MAX_BUFS];
    uint16_t     tx_cursor;
    volatile uint32_t tx_lock;
    volatile uint32_t rx_lock;
} vnet_state_t;

static vnet_state_t s_vnet;

static inline uint64_t vnet_lock_acq(volatile uint32_t *lk) {
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
    while (__atomic_exchange_n(lk, 1u, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause" ::: "memory");
    return fl;
}

static inline void vnet_lock_rel(volatile uint32_t *lk, uint64_t fl) {
    __atomic_store_n(lk, 0u, __ATOMIC_RELEASE);
    if (fl & 0x200ull) __asm__ volatile("sti" ::: "memory");
}

static inline uint64_t vnet_hdr_pa(const vnet_queue_t *q, uint32_t slot) {
    return q->hdr_pa + (uint64_t)slot * VNET_HDR_STRIDE;
}

static inline uint8_t *vnet_hdr_va(const vnet_queue_t *q, uint32_t slot) {
    return q->hdr_va + (size_t)slot * VNET_HDR_STRIDE;
}

static inline uint64_t vnet_buf_pa(const vnet_queue_t *q, uint32_t slot) {
    return q->buf_pages_pa[slot >> 1] + (uint64_t)(slot & 1u) * VNET_BUF_STRIDE;
}

static inline uint8_t *vnet_buf_va(const vnet_queue_t *q, uint32_t slot) {
    return q->buf_pages_va[slot >> 1] + (size_t)(slot & 1u) * VNET_BUF_STRIDE;
}

static int vnet_alloc_queue_mem(vnet_queue_t *q) {
    q->desc_pa  = vmm_alloc_frame();
    q->avail_pa = vmm_alloc_frame();
    q->used_pa  = vmm_alloc_frame();
    q->hdr_pa   = vmm_alloc_frame();
    if (!q->desc_pa || !q->avail_pa || !q->used_pa || !q->hdr_pa) return -1;

    q->desc   = (volatile vnet_virtq_desc_t *)(uintptr_t)q->desc_pa;
    q->avail  = (volatile vnet_virtq_avail_t *)(uintptr_t)q->avail_pa;
    q->used   = (volatile vnet_virtq_used_t *)(uintptr_t)q->used_pa;
    q->hdr_va = (uint8_t *)(uintptr_t)q->hdr_pa;

    memset((void *)(uintptr_t)q->desc_pa, 0, 4096);
    memset((void *)(uintptr_t)q->avail_pa, 0, 4096);
    memset((void *)(uintptr_t)q->used_pa, 0, 4096);
    memset(q->hdr_va, 0, 4096);

    uint32_t npages = (q->nbufs + 1u) / 2u;
    for (uint32_t p = 0; p < npages; p++) {
        q->buf_pages_pa[p] = vmm_alloc_frame();
        if (!q->buf_pages_pa[p]) return -1;
        q->buf_pages_va[p] = (uint8_t *)(uintptr_t)q->buf_pages_pa[p];
        memset(q->buf_pages_va[p], 0, 4096);
    }
    return 0;
}

static int vnet_setup_queue(uint16_t qidx, vnet_queue_t *q) {
    s_vnet.common->queue_select = qidx;
    uint16_t max_q = s_vnet.common->queue_size;
    if (max_q < 2u) return -1;

    uint16_t qsz = (max_q > VNET_MAX_QSIZE) ? VNET_MAX_QSIZE : max_q;
    uint16_t nbufs = (uint16_t)(qsz / 2u);
    if (nbufs > VNET_MAX_BUFS) nbufs = VNET_MAX_BUFS;
    if (nbufs == 0u) return -1;

    q->qidx  = qidx;
    q->qsize = qsz;
    q->nbufs = nbufs;
    s_vnet.common->queue_size = qsz;

    if (vnet_alloc_queue_mem(q) != 0) return -2;

    q->avail->flags = VIRTQ_AVAIL_F_NO_INTERRUPT;
    q->last_used_idx = 0u;

    s_vnet.common->queue_desc_lo   = (uint32_t)(q->desc_pa & 0xFFFFFFFFull);
    s_vnet.common->queue_desc_hi   = (uint32_t)(q->desc_pa >> 32);
    s_vnet.common->queue_driver_lo = (uint32_t)(q->avail_pa & 0xFFFFFFFFull);
    s_vnet.common->queue_driver_hi = (uint32_t)(q->avail_pa >> 32);
    s_vnet.common->queue_device_lo = (uint32_t)(q->used_pa & 0xFFFFFFFFull);
    s_vnet.common->queue_device_hi = (uint32_t)(q->used_pa >> 32);

    uint16_t noff = s_vnet.common->queue_notify_off;
    q->notify = (volatile uint16_t *)(uintptr_t)(
        (uintptr_t)s_vnet.notify_base + (uint64_t)noff * (uint64_t)s_vnet.notify_mult);

    s_vnet.common->queue_enable = 1u;
    return 0;
}

static void vnet_post_rx_slot(uint16_t slot) {
    vnet_queue_t *q = &s_vnet.rxq;
    uint16_t d0 = (uint16_t)(slot * 2u);
    uint16_t d1 = (uint16_t)(d0 + 1u);

    q->desc[d0].addr  = vnet_hdr_pa(q, slot);
    q->desc[d0].len   = (uint32_t)sizeof(vnet_hdr_v1_t);
    q->desc[d0].flags = VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT;
    q->desc[d0].next  = d1;

    q->desc[d1].addr  = vnet_buf_pa(q, slot);
    q->desc[d1].len   = NET_PKT_MAX;
    q->desc[d1].flags = VIRTQ_DESC_F_WRITE;
    q->desc[d1].next  = 0u;

    uint16_t aidx = q->avail->idx;
    q->avail->ring[aidx % q->qsize] = d0;
    __asm__ volatile("mfence" ::: "memory");
    q->avail->idx = (uint16_t)(aidx + 1u);
}

static bool vnet_probe_pci_device(uint8_t bus, uint8_t slot, uint8_t func, uint16_t dev_id) {
    pci_bar_t bars[6];
    memset(bars, 0, sizeof(bars));
    for (int i = 0; i < 6; i++) {
        bars[i] = pci_read_bar(bus, slot, func, i);
        if (bars[i].is_64bit && i < 5) i++;
    }

    uint16_t st = pci_config_readw(bus, slot, func, PCI_CONFIG_STATUS);
    if (!(st & 0x0010u)) return false;

    memset(&s_vnet, 0, sizeof(s_vnet));
    s_vnet.bus = bus;
    s_vnet.slot = slot;
    s_vnet.func = func;
    s_vnet.device_id = dev_id;

    uint8_t cap_ptr = (uint8_t)(pci_config_readb(bus, slot, func, PCI_CONFIG_CAP_PTR) & 0xFCu);
    for (int iter = 0; iter < 48 && cap_ptr >= 0x40u; iter++) {
        uint8_t cap_id   = pci_config_readb(bus, slot, func, cap_ptr);
        uint8_t cap_next = (uint8_t)(pci_config_readb(bus, slot, func, (uint8_t)(cap_ptr + 1u)) & 0xFCu);

        if (cap_id == 0x09u) {
            uint8_t  cfg_type = pci_config_readb(bus, slot, func, (uint8_t)(cap_ptr + 3u));
            uint8_t  bar_idx  = pci_config_readb(bus, slot, func, (uint8_t)(cap_ptr + 4u));
            uint32_t offset   = pci_config_readl(bus, slot, func, (uint8_t)(cap_ptr + 8u));
            uint32_t length   = pci_config_readl(bus, slot, func, (uint8_t)(cap_ptr + 12u));

            if (bar_idx < 6u && !bars[bar_idx].is_io && bars[bar_idx].address != 0) {
                uint64_t baddr = bars[bar_idx].address;
                uint64_t bsize = bars[bar_idx].size ? bars[bar_idx].size : (uint64_t)(offset + length);
                if (vmm_map_mmio_range(baddr, bsize) != 0) mmio_map(baddr, bsize);

                uint64_t ptr = baddr + (uint64_t)offset;
                switch (cfg_type) {
                    case VIRTIO_PCI_CAP_COMMON_CFG:
                        s_vnet.common = (volatile vnet_common_cfg_t *)(uintptr_t)ptr;
                        break;
                    case VIRTIO_PCI_CAP_NOTIFY_CFG:
                        s_vnet.notify_base = (volatile uint8_t *)(uintptr_t)ptr;
                        s_vnet.notify_mult = pci_config_readl(bus, slot, func, (uint8_t)(cap_ptr + 16u));
                        break;
                    case VIRTIO_PCI_CAP_ISR_CFG:
                        s_vnet.isr_cfg = (volatile uint8_t *)(uintptr_t)ptr;
                        break;
                    case VIRTIO_PCI_CAP_DEVICE_CFG:
                        s_vnet.dev_cfg = (volatile vnet_dev_cfg_t *)(uintptr_t)ptr;
                        break;
                    default:
                        break;
                }
            }
        }
        cap_ptr = cap_next;
    }

    if (!s_vnet.common || !s_vnet.notify_base || !s_vnet.dev_cfg) {
        return false;
    }

    pci_enable_bus_mastering(bus, slot, func);
    {
        uint16_t cmd = pci_config_readw(bus, slot, func, PCI_CONFIG_COMMAND);
        pci_config_writew(bus, slot, func, PCI_CONFIG_COMMAND, (uint16_t)(cmd | PCI_CMD_INT_DISABLE));
    }

    return true;
}

int virtio_net_init(void) {
    bool found = false;
    uint8_t best_bus = 0, best_slot = 0, best_func = 0;
    uint16_t best_dev = 0;

    for (int b = 0; b < 256; b++) {
        for (int s = 0; s < 32; s++) {
            if (pci_config_readw((uint8_t)b, (uint8_t)s, 0, 0) == 0xFFFFu) continue;
            uint8_t hdr = pci_config_readb((uint8_t)b, (uint8_t)s, 0, 0x0Eu);
            int nfunc = (hdr & 0x80u) ? 8 : 1;
            for (int f = 0; f < nfunc; f++) {
                uint16_t ven = pci_config_readw((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x00u);
                if (ven != PCI_VENDOR_VIRTIO) continue;
                uint16_t dev = pci_config_readw((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x02u);
                if (dev == VIRTIO_NET_PCI_DEV_MODERN) {
                    best_bus = (uint8_t)b; best_slot = (uint8_t)s; best_func = (uint8_t)f;
                    best_dev = dev;
                    found = true;
                    break;
                } else if (dev == VIRTIO_NET_PCI_DEV_TRANSITIONAL && !found) {
                    best_bus = (uint8_t)b; best_slot = (uint8_t)s; best_func = (uint8_t)f;
                    best_dev = dev;
                    found = true;
                }
            }
            if (found && best_dev == VIRTIO_NET_PCI_DEV_MODERN) break;
        }
        if (found && best_dev == VIRTIO_NET_PCI_DEV_MODERN) break;
    }

    if (!found) return -1;
    if (!vnet_probe_pci_device(best_bus, best_slot, best_func, best_dev)) return -2;

    /* VirtIO 1.0 reset + feature negotiation */
    s_vnet.common->device_status = 0u;
    for (int i = 0; i < 10000 && s_vnet.common->device_status != 0u; i++)
        __asm__ volatile("pause");

    s_vnet.common->device_status = VIRTIO_STATUS_ACKNOWLEDGE;
    s_vnet.common->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;

    s_vnet.common->device_feature_select = 0u;
    uint32_t f0 = s_vnet.common->device_feature;
    s_vnet.common->device_feature_select = 1u;
    uint32_t f1 = s_vnet.common->device_feature;

    if (!(f1 & VIRTIO_F_VERSION_1_W1)) {
        serial_puts("[vnet] device lacks VIRTIO_F_VERSION_1\n");
        return -3;
    }

    /* Negotiate MAC + STATUS (if offered) + VERSION_1; deliberately omit MRG_RXBUF */
    uint32_t drv_f0 = f0 & (VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS);
    uint32_t drv_f1 = VIRTIO_F_VERSION_1_W1;

    s_vnet.common->driver_feature_select = 0u;
    s_vnet.common->driver_feature = drv_f0;
    s_vnet.common->driver_feature_select = 1u;
    s_vnet.common->driver_feature = drv_f1;

    s_vnet.common->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                                   VIRTIO_STATUS_FEATURES_OK;
    if (!(s_vnet.common->device_status & VIRTIO_STATUS_FEATURES_OK)) {
        serial_puts("[vnet] FEATURES_OK rejected\n");
        return -4;
    }

    s_vnet.has_status = (drv_f0 & VIRTIO_NET_F_STATUS) != 0u;

    /* Read hardware MAC from DEVICE_CFG (bytes 0..5) */
    if (drv_f0 & VIRTIO_NET_F_MAC) {
        for (int i = 0; i < 6; i++) s_vnet.mac[i] = s_vnet.dev_cfg->mac[i];
    }
    if (s_vnet.mac[0] == 0 && s_vnet.mac[1] == 0 && s_vnet.mac[2] == 0 &&
        s_vnet.mac[3] == 0 && s_vnet.mac[4] == 0 && s_vnet.mac[5] == 0) {
        s_vnet.mac[0] = 0x52; s_vnet.mac[1] = 0x54; s_vnet.mac[2] = 0x00;
        s_vnet.mac[3] = 0x12; s_vnet.mac[4] = 0x34; s_vnet.mac[5] = 0x57;
    }

    /* Setup receiveq (queue 0) and transmitq (queue 1) */
    if (vnet_setup_queue(0u, &s_vnet.rxq) != 0) return -5;
    if (vnet_setup_queue(1u, &s_vnet.txq) != 0) return -6;

    /* Pre-populate receiveq descriptors */
    for (uint16_t i = 0; i < s_vnet.rxq.nbufs; i++) {
        vnet_post_rx_slot(i);
    }

    s_vnet.common->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                                   VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK;

    /* Notify receiveq now that DRIVER_OK is set */
    __asm__ volatile("mfence" ::: "memory");
    *s_vnet.rxq.notify = 0u;

    memcpy(net_if.mac, s_vnet.mac, 6);
    net_if.valid = true;
    s_vnet.present = true;

    serial_printf("[vnet] pci=%02X:%02X.%u id=1AF4:%04X mac=%02X:%02X:%02X:%02X:%02X:%02X rxq=%u txq=%u\n",
                  s_vnet.bus, s_vnet.slot, (unsigned)s_vnet.func,
                  (unsigned)s_vnet.device_id,
                  s_vnet.mac[0], s_vnet.mac[1], s_vnet.mac[2],
                  s_vnet.mac[3], s_vnet.mac[4], s_vnet.mac[5],
                  (unsigned)s_vnet.rxq.nbufs, (unsigned)s_vnet.txq.nbufs);
    return 0;
}

bool virtio_net_present(void) {
    return s_vnet.present;
}

void virtio_net_mac(uint8_t out[6]) {
    if (!out) return;
    if (!s_vnet.present) {
        memset(out, 0, 6);
        return;
    }
    memcpy(out, s_vnet.mac, 6);
}

bool virtio_net_link_up(void) {
    if (!s_vnet.present) return false;
    if (!s_vnet.has_status || !s_vnet.dev_cfg) return true;
    return (s_vnet.dev_cfg->status & VIRTIO_NET_S_LINK_UP) != 0u;
}

bool virtio_net_has_packet(void) {
    if (!s_vnet.present) return false;
    __asm__ volatile("" ::: "memory");
    return s_vnet.rxq.used->idx != s_vnet.rxq.last_used_idx;
}

static void vnet_reclaim_tx_locked(void) {
    vnet_queue_t *q = &s_vnet.txq;
    while (q->last_used_idx != q->used->idx) {
        __asm__ volatile("mfence" ::: "memory");
        uint16_t u = (uint16_t)(q->last_used_idx % q->qsize);
        uint32_t d = q->used->ring[u].id;
        q->last_used_idx++;
        uint32_t slot = d / 2u;
        if (slot < q->nbufs) s_vnet.tx_busy[slot] = false;
    }
}

bool virtio_net_send(const void *frame, size_t len) {
    if (!s_vnet.present || !frame || len == 0 || len > NET_PKT_MAX) return false;

    uint16_t slot = 0xFFFFu;
    for (uint32_t attempt = 0; attempt < 100000u; attempt++) {
        uint64_t fl = vnet_lock_acq(&s_vnet.tx_lock);
        vnet_reclaim_tx_locked();
        for (uint16_t n = 0; n < s_vnet.txq.nbufs; n++) {
            uint16_t idx = (uint16_t)((s_vnet.tx_cursor + n) % s_vnet.txq.nbufs);
            if (!s_vnet.tx_busy[idx]) {
                slot = idx;
                s_vnet.tx_busy[slot] = true;
                s_vnet.tx_cursor = (uint16_t)((slot + 1u) % s_vnet.txq.nbufs);
                break;
            }
        }
        if (slot != 0xFFFFu) {
            vnet_queue_t *q = &s_vnet.txq;
            uint16_t d0 = (uint16_t)(slot * 2u);
            uint16_t d1 = (uint16_t)(d0 + 1u);

            uint8_t *hdr = vnet_hdr_va(q, slot);
            memset(hdr, 0, sizeof(vnet_hdr_v1_t));
            memcpy(vnet_buf_va(q, slot), frame, len);

            q->desc[d0].addr  = vnet_hdr_pa(q, slot);
            q->desc[d0].len   = (uint32_t)sizeof(vnet_hdr_v1_t);
            q->desc[d0].flags = VIRTQ_DESC_F_NEXT;
            q->desc[d0].next  = d1;

            q->desc[d1].addr  = vnet_buf_pa(q, slot);
            q->desc[d1].len   = (uint32_t)len;
            q->desc[d1].flags = 0u;
            q->desc[d1].next  = 0u;

            __asm__ volatile("mfence" ::: "memory");
            uint16_t aidx = q->avail->idx;
            q->avail->ring[aidx % q->qsize] = d0;
            __asm__ volatile("mfence" ::: "memory");
            q->avail->idx = (uint16_t)(aidx + 1u);
            __asm__ volatile("mfence" ::: "memory");
            *q->notify = 1u;

            vnet_lock_rel(&s_vnet.tx_lock, fl);
            return true;
        }
        vnet_lock_rel(&s_vnet.tx_lock, fl);
        __asm__ volatile("pause" ::: "memory");
    }

    return false;
}

int virtio_net_recv(void *buf, size_t cap) {
    if (!s_vnet.present) return -1;

    uint64_t fl = vnet_lock_acq(&s_vnet.rx_lock);
    vnet_queue_t *q = &s_vnet.rxq;

    if (q->last_used_idx == q->used->idx) {
        vnet_lock_rel(&s_vnet.rx_lock, fl);
        return 0;
    }

    __asm__ volatile("mfence" ::: "memory");
    uint16_t u = (uint16_t)(q->last_used_idx % q->qsize);
    uint32_t head_desc = q->used->ring[u].id;
    uint32_t total_len = q->used->ring[u].len;
    q->last_used_idx++;

    uint32_t slot = head_desc / 2u;
    if (slot >= q->nbufs) {
        vnet_lock_rel(&s_vnet.rx_lock, fl);
        return -1;
    }

    int rc = 0;
    size_t hdr_sz = sizeof(vnet_hdr_v1_t);
    if (total_len > hdr_sz) {
        size_t frame_len = (size_t)total_len - hdr_sz;
        if (buf && frame_len <= cap) {
            memcpy(buf, vnet_buf_va(q, slot), frame_len);
            rc = (int)frame_len;
        } else {
            rc = -1;
        }
    }

    /* Immediately re-post consumed RX descriptor to keep receiveq full */
    vnet_post_rx_slot((uint16_t)slot);
    __asm__ volatile("mfence" ::: "memory");
    *q->notify = 0u;

    if (s_vnet.isr_cfg) (void)*s_vnet.isr_cfg;
    vnet_lock_rel(&s_vnet.rx_lock, fl);
    return rc;
}

bool virtio_net_drv_send(void *ctx, const uint8_t *data, uint16_t length) {
    (void)ctx;
    return virtio_net_send(data, (size_t)length);
}

bool virtio_net_drv_has(void *ctx) {
    (void)ctx;
    return virtio_net_has_packet();
}

bool virtio_net_drv_recv(void *ctx, uint8_t *buf, uint16_t *length) {
    (void)ctx;
    if (!buf || !length) return false;
    int n = virtio_net_recv(buf, (size_t)*length);
    if (n <= 0) return false;
    *length = (uint16_t)n;
    return true;
}
