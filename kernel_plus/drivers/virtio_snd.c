/**
 * Arctian VirtIO-Sound Driver (PCI 0x1AF4:0x1059)
 *
 * Implements VirtIO 1.0/1.2 Modern PCI capability discovery (COMMON_CFG,
 * NOTIFY_CFG, ISR_CFG, DEVICE_CFG), controlq (q0) + txq (q2), PCM info query,
 * stream lifecycle (SET_PARAMS/PREPARE/START/STOP/RELEASE), and non-blocking
 * TX period streaming via vmm_alloc_frame() pages.
 */
#include "virtio_snd.h"
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

/* Split Virtqueue descriptor flags */
#define VIRTQ_DESC_F_NEXT          1u
#define VIRTQ_DESC_F_WRITE         2u

/* VirtIO-Sound Commands & Status */
#define VIRTIO_SND_R_PCM_INFO        0x0100u
#define VIRTIO_SND_R_PCM_SET_PARAMS  0x0101u
#define VIRTIO_SND_R_PCM_PREPARE     0x0102u
#define VIRTIO_SND_R_PCM_RELEASE     0x0103u
#define VIRTIO_SND_R_PCM_START       0x0104u
#define VIRTIO_SND_R_PCM_STOP        0x0105u

#define VIRTIO_SND_S_OK              0x8000u

#define VIRTIO_SND_D_OUTPUT          0u
#define VIRTIO_SND_PCM_FMT_S16       5u
#define VIRTIO_SND_PCM_RATE_8000     1u
#define VIRTIO_SND_PCM_RATE_11025    2u
#define VIRTIO_SND_PCM_RATE_16000    3u
#define VIRTIO_SND_PCM_RATE_22050    4u
#define VIRTIO_SND_PCM_RATE_32000    5u
#define VIRTIO_SND_PCM_RATE_44100    6u
#define VIRTIO_SND_PCM_RATE_48000    7u
#define VIRTIO_SND_PCM_RATE_88200    9u
#define VIRTIO_SND_PCM_RATE_96000    10u

#define VSND_MAX_QSIZE  64u
#define VSND_TX_BUFS    8u
#define VSND_BUF_BYTES  4096u

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
} vsnd_common_cfg_t;

typedef struct __attribute__((packed)) {
    uint32_t jacks;
    uint32_t streams;
    uint32_t chmaps;
} vsnd_dev_cfg_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} vsnd_virtq_desc_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VSND_MAX_QSIZE];
    uint16_t used_event;
} vsnd_virtq_avail_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t len;
} vsnd_virtq_used_elem_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    vsnd_virtq_used_elem_t ring[VSND_MAX_QSIZE];
    uint16_t avail_event;
} vsnd_virtq_used_t;

typedef struct __attribute__((packed)) {
    uint32_t code;
} vsnd_hdr_t;

typedef struct __attribute__((packed)) {
    vsnd_hdr_t hdr;
    uint32_t   start_id;
    uint32_t   count;
    uint32_t   size;
} vsnd_query_info_t;

typedef struct __attribute__((packed)) {
    uint32_t hda_fn_nid;
    uint32_t features;
    uint64_t formats;
    uint64_t rates;
    uint8_t  direction;
    uint8_t  channels_min;
    uint8_t  channels_max;
    uint8_t  padding[5];
} vsnd_pcm_info_t;

typedef struct __attribute__((packed)) {
    vsnd_hdr_t hdr;
    uint32_t   stream_id;
} vsnd_pcm_hdr_t;

typedef struct __attribute__((packed)) {
    vsnd_pcm_hdr_t hdr;
    uint32_t       buffer_bytes;
    uint32_t       period_bytes;
    uint32_t       features;
    uint8_t        channels;
    uint8_t        format;
    uint8_t        rate;
    uint8_t        padding;
} vsnd_pcm_set_params_t;

typedef struct __attribute__((packed)) {
    uint32_t stream_id;
} vsnd_pcm_xfer_t;

typedef struct __attribute__((packed)) {
    uint32_t status;
    uint32_t latency_bytes;
} vsnd_pcm_status_t;

typedef struct {
    uint16_t                     qsize;
    volatile vsnd_virtq_desc_t  *desc;
    volatile vsnd_virtq_avail_t *avail;
    volatile vsnd_virtq_used_t  *used;
    uint64_t                     desc_pa;
    uint64_t                     avail_pa;
    uint64_t                     used_pa;
    uint16_t                     last_used_idx;
    volatile uint16_t           *notify;
} vsnd_queue_t;

typedef uint32_t (*vsnd_stream_pull_fn)(void *dst, uint32_t max_bytes, void *ctx);

typedef struct {
    bool                       present;
    volatile vsnd_common_cfg_t *common;
    volatile uint8_t           *notify_base;
    uint32_t                    notify_mult;
    volatile uint8_t           *isr_cfg;
    volatile vsnd_dev_cfg_t    *dev_cfg;

    vsnd_queue_t ctrlq;
    vsnd_queue_t txq;

    uint8_t     *ctrl_dma;
    uint64_t     ctrl_dma_pa;

    /* TX headers + period DMA frames */
    uint8_t     *tx_hdr_va;
    uint64_t     tx_hdr_pa;
    uint8_t     *tx_scratch_va;
    uint64_t     tx_scratch_pa;
    uint8_t     *tx_buf_va[VSND_TX_BUFS];
    uint64_t     tx_buf_pa[VSND_TX_BUFS];
    bool         tx_slot_busy[VSND_TX_BUFS];
    uint32_t     tx_inflight;

    uint32_t     out_stream_id;
    uint64_t     supported_formats;
    uint64_t     supported_rates;

    bool                playing;
    bool                prepared;
    bool                stream_mode;
    vsnd_stream_pull_fn pull_fn;
    void               *pull_ctx;
    int                 volume;
    uint32_t            rate;
    const uint8_t      *src;
    uint32_t            src_len;
    uint32_t            src_pos;
} vsnd_state_t;

static vsnd_state_t s_vsnd;

static int vsnd_setup_queue(uint16_t qidx, vsnd_queue_t *q) {
    s_vsnd.common->queue_select = qidx;
    uint16_t max_q = s_vsnd.common->queue_size;
    if (max_q == 0u) return -1;
    uint16_t qsz = (max_q > VSND_MAX_QSIZE) ? VSND_MAX_QSIZE : max_q;
    if (qidx == 2u && qsz < VSND_TX_BUFS * 3u) {
        /* Need at least 3 descriptors per inflight TX buffer; scale down if smaller */
    }
    q->qsize = qsz;
    s_vsnd.common->queue_size = qsz;

    q->desc_pa  = vmm_alloc_frame();
    q->avail_pa = vmm_alloc_frame();
    q->used_pa  = vmm_alloc_frame();
    if (!q->desc_pa || !q->avail_pa || !q->used_pa) return -2;

    q->desc  = (volatile vsnd_virtq_desc_t *)(uintptr_t)q->desc_pa;
    q->avail = (volatile vsnd_virtq_avail_t *)(uintptr_t)q->avail_pa;
    q->used  = (volatile vsnd_virtq_used_t *)(uintptr_t)q->used_pa;
    q->avail->flags = 1u; /* VIRTQ_AVAIL_F_NO_INTERRUPT (polling driver) */
    q->last_used_idx = 0;

    s_vsnd.common->queue_desc_lo   = (uint32_t)(q->desc_pa & 0xFFFFFFFFull);
    s_vsnd.common->queue_desc_hi   = (uint32_t)(q->desc_pa >> 32);
    s_vsnd.common->queue_driver_lo = (uint32_t)(q->avail_pa & 0xFFFFFFFFull);
    s_vsnd.common->queue_driver_hi = (uint32_t)(q->avail_pa >> 32);
    s_vsnd.common->queue_device_lo = (uint32_t)(q->used_pa & 0xFFFFFFFFull);
    s_vsnd.common->queue_device_hi = (uint32_t)(q->used_pa >> 32);

    uint16_t noff = s_vsnd.common->queue_notify_off;
    q->notify = (volatile uint16_t *)(uintptr_t)(
        (uintptr_t)s_vsnd.notify_base + (uint64_t)noff * (uint64_t)s_vsnd.notify_mult);

    s_vsnd.common->queue_enable = 1u;
    return 0;
}

static int vsnd_ctrl_cmd(const void *req, uint32_t req_len,
                         void *resp, uint32_t resp_len) {
    if (!s_vsnd.ctrlq.desc || !s_vsnd.ctrl_dma) return -1;
    if (req_len > 2048u || resp_len > 2048u) return -2;

    uint8_t *req_dma  = s_vsnd.ctrl_dma;
    uint8_t *resp_dma = s_vsnd.ctrl_dma + 2048u;
    memcpy(req_dma, req, req_len);
    memset(resp_dma, 0, resp_len);

    vsnd_queue_t *q = &s_vsnd.ctrlq;
    q->desc[0].addr  = s_vsnd.ctrl_dma_pa;
    q->desc[0].len   = req_len;
    q->desc[0].flags = VIRTQ_DESC_F_NEXT;
    q->desc[0].next  = 1u;

    q->desc[1].addr  = s_vsnd.ctrl_dma_pa + 2048ull;
    q->desc[1].len   = resp_len;
    q->desc[1].flags = VIRTQ_DESC_F_WRITE;
    q->desc[1].next  = 0u;

    __asm__ volatile("mfence" ::: "memory");
    uint16_t aidx = q->avail->idx;
    q->avail->ring[aidx % q->qsize] = 0u;
    __asm__ volatile("mfence" ::: "memory");
    q->avail->idx = (uint16_t)(aidx + 1u);
    __asm__ volatile("mfence" ::: "memory");

    *q->notify = 0u;

    uint32_t spins = 0;
    while (q->used->idx == q->last_used_idx) {
        __asm__ volatile("pause" ::: "memory");
        if (++spins > 50000000u) return -3;
    }
    __asm__ volatile("mfence" ::: "memory");
    q->last_used_idx = q->used->idx;

    if (resp && resp_len) memcpy(resp, resp_dma, resp_len);
    if (s_vsnd.isr_cfg) (void)*s_vsnd.isr_cfg;
    return 0;
}

static int vsnd_pcm_simple(uint32_t code) {
    vsnd_pcm_hdr_t req;
    vsnd_hdr_t resp;
    req.hdr.code  = code;
    req.stream_id = s_vsnd.out_stream_id;
    memset(&resp, 0, sizeof(resp));
    if (vsnd_ctrl_cmd(&req, sizeof(req), &resp, sizeof(resp)) != 0) return -1;
    return (resp.code == VIRTIO_SND_S_OK) ? 0 : -2;
}

static uint8_t vsnd_rate_enum(uint32_t hz, uint32_t *actual_hz) {
    struct { uint32_t hz; uint8_t val; } tbl[] = {
        { 48000u, VIRTIO_SND_PCM_RATE_48000 },
        { 44100u, VIRTIO_SND_PCM_RATE_44100 },
        { 32000u, VIRTIO_SND_PCM_RATE_32000 },
        { 22050u, VIRTIO_SND_PCM_RATE_22050 },
        { 16000u, VIRTIO_SND_PCM_RATE_16000 },
        { 11025u, VIRTIO_SND_PCM_RATE_11025 },
        { 8000u,  VIRTIO_SND_PCM_RATE_8000  },
        { 88200u, VIRTIO_SND_PCM_RATE_88200 },
        { 96000u, VIRTIO_SND_PCM_RATE_96000 },
    };
    for (unsigned i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        if (tbl[i].hz == hz && (s_vsnd.supported_rates & (1ull << tbl[i].val))) {
            *actual_hz = tbl[i].hz;
            return tbl[i].val;
        }
    }
    if (s_vsnd.supported_rates & (1ull << VIRTIO_SND_PCM_RATE_48000)) {
        *actual_hz = 48000u;
        return VIRTIO_SND_PCM_RATE_48000;
    }
    if (s_vsnd.supported_rates & (1ull << VIRTIO_SND_PCM_RATE_44100)) {
        *actual_hz = 44100u;
        return VIRTIO_SND_PCM_RATE_44100;
    }
    *actual_hz = 48000u;
    return VIRTIO_SND_PCM_RATE_48000;
}

void virtio_snd_scale_pcm_s16(int16_t *dst, const uint8_t *src, uint32_t bytes, int volume) {
    if (!dst || !src || bytes == 0u) return;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    if (volume == 0) {
        memset(dst, 0, bytes);
        return;
    }
    if (volume == 100) {
        memcpy(dst, src, bytes);
        return;
    }

    uint32_t nsamp = bytes / 2u;
    for (uint32_t i = 0; i < nsamp; i++) {
        int16_t s = (int16_t)((uint16_t)src[i * 2u] | ((uint16_t)src[i * 2u + 1u] << 8));
        int32_t scaled = ((int32_t)s * (int32_t)volume) / 100;
        if (scaled > 32767) scaled = 32767;
        else if (scaled < -32768) scaled = -32768;
        dst[i] = (int16_t)scaled;
    }
    if (bytes & 1u) ((uint8_t *)dst)[bytes - 1u] = 0;
}

static bool vsnd_submit_tx_slot(uint32_t slot) {
    uint32_t max_slots = s_vsnd.txq.qsize / 3u;
    if (slot >= VSND_TX_BUFS || slot >= max_slots) return false;

    uint32_t n = 0;
    int16_t *dp = (int16_t *)s_vsnd.tx_buf_va[slot];

    if (s_vsnd.stream_mode && s_vsnd.pull_fn) {
        if (!s_vsnd.tx_scratch_va) return false;
        n = s_vsnd.pull_fn(s_vsnd.tx_scratch_va, VSND_BUF_BYTES, s_vsnd.pull_ctx);
        if (n > VSND_BUF_BYTES) n = VSND_BUF_BYTES;
        if (n == 0u) {
            if (s_vsnd.tx_inflight > 0u) return false;
            n = VSND_BUF_BYTES;
            memset(dp, 0, n);
        } else {
            virtio_snd_scale_pcm_s16(dp, s_vsnd.tx_scratch_va, n, s_vsnd.volume);
        }
    } else {
        if (!s_vsnd.src || s_vsnd.src_pos >= s_vsnd.src_len) return false;
        uint32_t rem = s_vsnd.src_len - s_vsnd.src_pos;
        n = (rem > VSND_BUF_BYTES) ? VSND_BUF_BYTES : rem;
        virtio_snd_scale_pcm_s16(dp, s_vsnd.src + s_vsnd.src_pos, n, s_vsnd.volume);
        s_vsnd.src_pos += n;
    }

    /* Per-slot headers in tx_hdr page (64 bytes per slot) */
    uint64_t hdr_off = (uint64_t)slot * 64ull;
    vsnd_pcm_xfer_t   *xfer = (vsnd_pcm_xfer_t *)(s_vsnd.tx_hdr_va + hdr_off);
    vsnd_pcm_status_t *st   = (vsnd_pcm_status_t *)(s_vsnd.tx_hdr_va + hdr_off + 16ull);
    xfer->stream_id = s_vsnd.out_stream_id;
    st->status = 0;
    st->latency_bytes = 0;

    uint16_t d0 = (uint16_t)(slot * 3u);
    uint16_t d1 = (uint16_t)(d0 + 1u);
    uint16_t d2 = (uint16_t)(d0 + 2u);

    vsnd_queue_t *q = &s_vsnd.txq;
    q->desc[d0].addr  = s_vsnd.tx_hdr_pa + hdr_off;
    q->desc[d0].len   = sizeof(vsnd_pcm_xfer_t);
    q->desc[d0].flags = VIRTQ_DESC_F_NEXT;
    q->desc[d0].next  = d1;

    q->desc[d1].addr  = s_vsnd.tx_buf_pa[slot];
    q->desc[d1].len   = n;
    q->desc[d1].flags = VIRTQ_DESC_F_NEXT;
    q->desc[d1].next  = d2;

    q->desc[d2].addr  = s_vsnd.tx_hdr_pa + hdr_off + 16ull;
    q->desc[d2].len   = sizeof(vsnd_pcm_status_t);
    q->desc[d2].flags = VIRTQ_DESC_F_WRITE;
    q->desc[d2].next  = 0u;

    __asm__ volatile("mfence" ::: "memory");
    uint16_t aidx = q->avail->idx;
    q->avail->ring[aidx % q->qsize] = d0;
    __asm__ volatile("mfence" ::: "memory");
    q->avail->idx = (uint16_t)(aidx + 1u);

    s_vsnd.tx_slot_busy[slot] = true;
    s_vsnd.tx_inflight++;
    return true;
}

int virtio_snd_init(void) {
    memset(&s_vsnd, 0, sizeof(s_vsnd));
    s_vsnd.volume = 80;
    s_vsnd.rate = 48000u;

    uint8_t bus = 0, slot = 0, func = 0;
    bool found = false;
    pci_bar_t bars[6];

    for (int b = 0; b < 256 && !found; b++) {
        for (int s = 0; s < 32 && !found; s++) {
            if (pci_config_readw((uint8_t)b, (uint8_t)s, 0, 0) == 0xFFFFu) continue;
            uint8_t hdr = pci_config_readb((uint8_t)b, (uint8_t)s, 0, 0x0Eu);
            int nfunc = (hdr & 0x80u) ? 8 : 1;
            for (int f = 0; f < nfunc; f++) {
                uint16_t ven = pci_config_readw((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x00u);
                if (ven != PCI_VENDOR_VIRTIO) continue;
                uint16_t dev = pci_config_readw((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x02u);
                if (dev == 0x1059u) {
                    bus = (uint8_t)b; slot = (uint8_t)s; func = (uint8_t)f;
                    found = true;
                    break;
                }
            }
        }
    }
    if (!found) return -1;

    pci_enable_bus_mastering(bus, slot, func);
    {
        uint16_t cmd = pci_config_readw(bus, slot, func, PCI_CONFIG_COMMAND);
        pci_config_writew(bus, slot, func, PCI_CONFIG_COMMAND, (uint16_t)(cmd | PCI_CMD_INT_DISABLE));
    }
    for (int i = 0; i < 6; i++) {
        bars[i] = pci_read_bar(bus, slot, func, i);
        if (bars[i].is_64bit && i < 5) i++;
    }

    uint16_t st = pci_config_readw(bus, slot, func, PCI_CONFIG_STATUS);
    if (!(st & 0x0010u)) return -2;

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
                        s_vsnd.common = (volatile vsnd_common_cfg_t *)(uintptr_t)ptr;
                        break;
                    case VIRTIO_PCI_CAP_NOTIFY_CFG:
                        s_vsnd.notify_base = (volatile uint8_t *)(uintptr_t)ptr;
                        s_vsnd.notify_mult = pci_config_readl(bus, slot, func, (uint8_t)(cap_ptr + 16u));
                        break;
                    case VIRTIO_PCI_CAP_ISR_CFG:
                        s_vsnd.isr_cfg = (volatile uint8_t *)(uintptr_t)ptr;
                        break;
                    case VIRTIO_PCI_CAP_DEVICE_CFG:
                        s_vsnd.dev_cfg = (volatile vsnd_dev_cfg_t *)(uintptr_t)ptr;
                        break;
                    default:
                        break;
                }
            }
        }
        cap_ptr = cap_next;
    }

    if (!s_vsnd.common || !s_vsnd.notify_base || !s_vsnd.dev_cfg) return -3;

    /* VirtIO 1.0 reset + feature negotiation */
    s_vsnd.common->device_status = 0u;
    for (int i = 0; i < 10000 && s_vsnd.common->device_status != 0u; i++)
        __asm__ volatile("pause");

    s_vsnd.common->device_status = VIRTIO_STATUS_ACKNOWLEDGE;
    s_vsnd.common->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;

    s_vsnd.common->device_feature_select = 1u;
    uint32_t f1 = s_vsnd.common->device_feature;
    s_vsnd.common->driver_feature_select = 0u;
    s_vsnd.common->driver_feature = 0u;
    s_vsnd.common->driver_feature_select = 1u;
    s_vsnd.common->driver_feature = (f1 & 1u);

    s_vsnd.common->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                                   VIRTIO_STATUS_FEATURES_OK;
    if (!(s_vsnd.common->device_status & VIRTIO_STATUS_FEATURES_OK)) return -4;

    if (vsnd_setup_queue(0u, &s_vsnd.ctrlq) != 0) return -5;
    if (vsnd_setup_queue(2u, &s_vsnd.txq) != 0)   return -6;

    s_vsnd.ctrl_dma_pa   = vmm_alloc_frame();
    s_vsnd.tx_hdr_pa     = vmm_alloc_frame();
    s_vsnd.tx_scratch_pa = vmm_alloc_frame();
    if (!s_vsnd.ctrl_dma_pa || !s_vsnd.tx_hdr_pa || !s_vsnd.tx_scratch_pa) return -7;
    s_vsnd.ctrl_dma      = (uint8_t *)(uintptr_t)s_vsnd.ctrl_dma_pa;
    s_vsnd.tx_hdr_va     = (uint8_t *)(uintptr_t)s_vsnd.tx_hdr_pa;
    s_vsnd.tx_scratch_va = (uint8_t *)(uintptr_t)s_vsnd.tx_scratch_pa;

    for (uint32_t i = 0; i < VSND_TX_BUFS; i++) {
        s_vsnd.tx_buf_pa[i] = vmm_alloc_frame();
        if (!s_vsnd.tx_buf_pa[i]) return -7;
        s_vsnd.tx_buf_va[i] = (uint8_t *)(uintptr_t)s_vsnd.tx_buf_pa[i];
    }

    s_vsnd.common->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                                   VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK;

    uint32_t nstreams = s_vsnd.dev_cfg->streams;
    if (nstreams == 0 || nstreams > 16u) nstreams = 1u;

    /* Query PCM streams */
    vsnd_query_info_t qreq;
    qreq.hdr.code = VIRTIO_SND_R_PCM_INFO;
    qreq.start_id = 0u;
    qreq.count    = nstreams;
    qreq.size     = sizeof(vsnd_pcm_info_t);

    uint8_t qresp_buf[sizeof(vsnd_hdr_t) + 16u * sizeof(vsnd_pcm_info_t)];
    uint32_t qresp_len = (uint32_t)(sizeof(vsnd_hdr_t) + nstreams * sizeof(vsnd_pcm_info_t));
    if (vsnd_ctrl_cmd(&qreq, sizeof(qreq), qresp_buf, qresp_len) != 0) return -8;

    vsnd_hdr_t *rh = (vsnd_hdr_t *)qresp_buf;
    if (rh->code != VIRTIO_SND_S_OK) return -9;

    vsnd_pcm_info_t *infos = (vsnd_pcm_info_t *)(qresp_buf + sizeof(vsnd_hdr_t));
    bool found_out = false;
    for (uint32_t i = 0; i < nstreams; i++) {
        if (infos[i].direction == VIRTIO_SND_D_OUTPUT &&
            (infos[i].formats & (1ull << VIRTIO_SND_PCM_FMT_S16))) {
            s_vsnd.out_stream_id = i;
            s_vsnd.supported_formats = infos[i].formats;
            s_vsnd.supported_rates   = infos[i].rates;
            found_out = true;
            break;
        }
    }
    if (!found_out) return -10;

    s_vsnd.present = true;
    serial_printf("[vsnd] pci=1AF4:1059 streams=%u out_id=%u fmts=0x%llX rates=0x%llX txq=%u\n",
                  nstreams, s_vsnd.out_stream_id,
                  (unsigned long long)s_vsnd.supported_formats,
                  (unsigned long long)s_vsnd.supported_rates,
                  (unsigned)s_vsnd.txq.qsize);
    return 0;
}

bool     virtio_snd_present(void) { return s_vsnd.present; }
uint32_t virtio_snd_rate(void)    { return s_vsnd.rate; }
bool     virtio_snd_playing(void) { return s_vsnd.playing; }

void virtio_snd_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_vsnd.volume = percent;
}

void virtio_snd_stop(void) {
    if (!s_vsnd.present) return;
    if (s_vsnd.playing || s_vsnd.prepared) {
        (void)vsnd_pcm_simple(VIRTIO_SND_R_PCM_STOP);
        (void)vsnd_pcm_simple(VIRTIO_SND_R_PCM_RELEASE);
    }
    s_vsnd.playing = false;
    s_vsnd.prepared = false;
    s_vsnd.stream_mode = false;
    s_vsnd.pull_fn = 0;
    s_vsnd.pull_ctx = 0;
    s_vsnd.src = 0;
    s_vsnd.src_len = 0;
    s_vsnd.src_pos = 0;
    s_vsnd.tx_inflight = 0;
    for (uint32_t i = 0; i < VSND_TX_BUFS; i++) s_vsnd.tx_slot_busy[i] = false;
    s_vsnd.txq.last_used_idx = s_vsnd.txq.used->idx;
}

static int vsnd_prepare_stream(uint32_t sample_rate, uint32_t *actual_hz_out) {
    uint32_t actual_hz = 48000u;
    uint8_t rate_code = vsnd_rate_enum(sample_rate, &actual_hz);

    vsnd_pcm_set_params_t sp;
    memset(&sp, 0, sizeof(sp));
    sp.hdr.hdr.code  = VIRTIO_SND_R_PCM_SET_PARAMS;
    sp.hdr.stream_id = s_vsnd.out_stream_id;
    sp.buffer_bytes  = VSND_TX_BUFS * VSND_BUF_BYTES;
    sp.period_bytes  = VSND_BUF_BYTES;
    sp.channels      = 2u;
    sp.format        = VIRTIO_SND_PCM_FMT_S16;
    sp.rate          = rate_code;

    vsnd_hdr_t resp;
    memset(&resp, 0, sizeof(resp));
    if (vsnd_ctrl_cmd(&sp, sizeof(sp), &resp, sizeof(resp)) != 0 ||
        resp.code != VIRTIO_SND_S_OK) {
        return -2;
    }
    if (vsnd_pcm_simple(VIRTIO_SND_R_PCM_PREPARE) != 0) return -3;
    s_vsnd.prepared = true;
    s_vsnd.rate = actual_hz;
    if (actual_hz_out) *actual_hz_out = actual_hz;
    return 0;
}

int virtio_snd_play_pcm(const void *pcm_s16_stereo, uint32_t bytes, uint32_t sample_rate) {
    if (!s_vsnd.present || !pcm_s16_stereo || bytes == 0) return -1;
    if (sample_rate == 0) sample_rate = 48000u;

    virtio_snd_stop();

    uint32_t actual_hz = 48000u;
    int rc = vsnd_prepare_stream(sample_rate, &actual_hz);
    if (rc != 0) return rc;

    s_vsnd.stream_mode = false;
    s_vsnd.src = (const uint8_t *)pcm_s16_stereo;
    s_vsnd.src_len = bytes;
    s_vsnd.src_pos = 0;

    uint32_t max_slots = s_vsnd.txq.qsize / 3u;
    if (max_slots > VSND_TX_BUFS) max_slots = VSND_TX_BUFS;
    for (uint32_t i = 0; i < max_slots && s_vsnd.src_pos < s_vsnd.src_len; i++) {
        vsnd_submit_tx_slot(i);
    }
    __asm__ volatile("mfence" ::: "memory");
    *s_vsnd.txq.notify = 2u;

    if (vsnd_pcm_simple(VIRTIO_SND_R_PCM_START) != 0) {
        virtio_snd_stop();
        return -4;
    }

    s_vsnd.playing = true;
    serial_printf("[vsnd] stream start id=%u rate=%u bytes=%u inflight=%u vol=%d\n",
                  s_vsnd.out_stream_id, actual_hz, bytes, s_vsnd.tx_inflight, s_vsnd.volume);
    return 0;
}

int virtio_snd_stream_start(uint32_t sample_rate, vsnd_stream_pull_fn pull, void *ctx) {
    if (!s_vsnd.present || !pull) return -1;
    if (sample_rate == 0) sample_rate = 48000u;

    virtio_snd_stop();

    uint32_t actual_hz = 48000u;
    int rc = vsnd_prepare_stream(sample_rate, &actual_hz);
    if (rc != 0) return rc;

    s_vsnd.stream_mode = true;
    s_vsnd.pull_fn = pull;
    s_vsnd.pull_ctx = ctx;

    uint32_t max_slots = s_vsnd.txq.qsize / 3u;
    if (max_slots > VSND_TX_BUFS) max_slots = VSND_TX_BUFS;
    for (uint32_t i = 0; i < max_slots; i++) {
        if (!vsnd_submit_tx_slot(i)) break;
    }
    __asm__ volatile("mfence" ::: "memory");
    *s_vsnd.txq.notify = 2u;

    if (vsnd_pcm_simple(VIRTIO_SND_R_PCM_START) != 0) {
        virtio_snd_stop();
        return -4;
    }

    s_vsnd.playing = true;
    serial_printf("[vsnd] live stream start id=%u rate=%u inflight=%u vol=%d\n",
                  s_vsnd.out_stream_id, actual_hz, s_vsnd.tx_inflight, s_vsnd.volume);
    return 0;
}

void virtio_snd_poll(void) {
    if (!s_vsnd.present || !s_vsnd.playing) return;

    vsnd_queue_t *q = &s_vsnd.txq;
    bool pushed = false;
    while (q->last_used_idx != q->used->idx) {
        uint16_t u = (uint16_t)(q->last_used_idx % q->qsize);
        uint32_t d0 = q->used->ring[u].id;
        q->last_used_idx++;
        uint32_t slot = d0 / 3u;
        if (slot < VSND_TX_BUFS && s_vsnd.tx_slot_busy[slot]) {
            s_vsnd.tx_slot_busy[slot] = false;
            if (s_vsnd.tx_inflight) s_vsnd.tx_inflight--;
            if (s_vsnd.stream_mode || s_vsnd.src_pos < s_vsnd.src_len) {
                if (vsnd_submit_tx_slot(slot)) pushed = true;
            }
        }
    }

    if (s_vsnd.stream_mode) {
        uint32_t max_slots = s_vsnd.txq.qsize / 3u;
        if (max_slots > VSND_TX_BUFS) max_slots = VSND_TX_BUFS;
        for (uint32_t slot = 0; slot < max_slots; slot++) {
            if (!s_vsnd.tx_slot_busy[slot]) {
                if (!vsnd_submit_tx_slot(slot)) break;
                pushed = true;
            }
        }
    }

    if (pushed) {
        __asm__ volatile("mfence" ::: "memory");
        *q->notify = 2u;
    }

    if (!s_vsnd.stream_mode && s_vsnd.src_pos >= s_vsnd.src_len && s_vsnd.tx_inflight == 0) {
        virtio_snd_stop();
    }
}
