/**
 * Arctian VirtIO-GPU 2D Driver (PCI 0x1AF4:0x1050)
 *
 * Implements VirtIO 1.0/1.1 Modern PCI transport (PCI capability discovery for
 * COMMON_CFG, NOTIFY_CFG, ISR_CFG, DEVICE_CFG), Split Virtqueue controlq, and
 * full VirtIO-GPU 2D command pipeline:
 *   GET_DISPLAY_INFO -> RESOURCE_CREATE_2D -> RESOURCE_ATTACH_BACKING ->
 *   SET_SCANOUT -> TRANSFER_TO_HOST_2D -> RESOURCE_FLUSH.
 */
#include "gpu.h"
#include "gpu_simd.h"
#include "backbuffer.h"
#include "vesa.h"
#include "memory_map.h"
#include "paging.h"
#include "clock.h"
#include "kernel.h"
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

/* VirtIO-GPU 2D command / response types */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO        0x0100u
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      0x0101u
#define VIRTIO_GPU_CMD_RESOURCE_UNREF          0x0102u
#define VIRTIO_GPU_CMD_SET_SCANOUT             0x0103u
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH          0x0104u
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     0x0105u
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106u
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107u

#define VIRTIO_GPU_RESP_OK_NODATA              0x1100u
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO        0x1101u

/* B8G8R8X8_UNORM matches Arctian XRGB8888 little-endian layout (B,G,R,X) */
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM       2u

#define VGPU_MAX_QSIZE  64u

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
} virtio_pci_common_cfg_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtq_desc_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VGPU_MAX_QSIZE];
    uint16_t used_event;
} virtq_avail_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t len;
} virtq_used_elem_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[VGPU_MAX_QSIZE];
    uint16_t avail_event;
} virtq_used_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t  ring_idx;
    uint8_t  padding[3];
} vgpu_ctrl_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} vgpu_rect_t;

typedef struct __attribute__((packed)) {
    vgpu_rect_t r;
    uint32_t    enabled;
    uint32_t    flags;
} vgpu_display_one_t;

typedef struct __attribute__((packed)) {
    vgpu_ctrl_hdr_t    hdr;
    vgpu_display_one_t pmodes[16];
} vgpu_resp_display_info_t;

typedef struct __attribute__((packed)) {
    vgpu_ctrl_hdr_t hdr;
    uint32_t        resource_id;
    uint32_t        format;
    uint32_t        width;
    uint32_t        height;
} vgpu_cmd_create_2d_t;

typedef struct __attribute__((packed)) {
    vgpu_ctrl_hdr_t hdr;
    uint32_t        resource_id;
    uint32_t        padding;
} vgpu_cmd_unref_t;

typedef struct __attribute__((packed)) {
    vgpu_ctrl_hdr_t hdr;
    uint32_t        resource_id;
    uint32_t        nr_entries;
    uint64_t        addr;
    uint32_t        length;
    uint32_t        padding;
} vgpu_cmd_attach_backing_1_t;

typedef struct __attribute__((packed)) {
    vgpu_ctrl_hdr_t hdr;
    vgpu_rect_t     r;
    uint32_t        scanout_id;
    uint32_t        resource_id;
} vgpu_cmd_set_scanout_t;

typedef struct __attribute__((packed)) {
    vgpu_ctrl_hdr_t hdr;
    vgpu_rect_t     r;
    uint64_t        offset;
    uint32_t        resource_id;
    uint32_t        padding;
} vgpu_cmd_transfer_2d_t;

typedef struct __attribute__((packed)) {
    vgpu_ctrl_hdr_t hdr;
    vgpu_rect_t     r;
    uint32_t        resource_id;
    uint32_t        padding;
} vgpu_cmd_flush_t;

typedef struct {
    volatile virtio_pci_common_cfg_t *common;
    volatile uint8_t                 *notify_base;
    uint32_t                          notify_mult;
    volatile uint16_t                *ctrl_notify;
    volatile uint8_t                 *isr_cfg;
    volatile uint8_t                 *dev_cfg;

    uint16_t            qsize;
    volatile virtq_desc_t  *desc;
    volatile virtq_avail_t *avail;
    volatile virtq_used_t  *used;
    uint64_t            desc_pa;
    uint64_t            avail_pa;
    uint64_t            used_pa;
    uint16_t            last_used_idx;

    uint8_t            *dma_buf;
    uint64_t            dma_pa;

    uint32_t            res_id;
    uint32_t            width;
    uint32_t            height;
    uint64_t            flush_count;
    volatile uint32_t   lock;
} vgpu_state_t;

static vgpu_state_t s_vgpu;

static inline uint64_t vgpu_lock_acq(vgpu_state_t *vg) {
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
    while (__atomic_exchange_n(&vg->lock, 1u, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause" ::: "memory");
    return fl;
}

static inline void vgpu_lock_rel(vgpu_state_t *vg, uint64_t fl) {
    __atomic_store_n(&vg->lock, 0u, __ATOMIC_RELEASE);
    if (fl & 0x200ull) __asm__ volatile("sti" ::: "memory");
}

static int vgpu_submit_cmd(vgpu_state_t *vg,
                           const void *req, uint32_t req_len,
                           void *resp, uint32_t resp_len) {
    if (!vg || !vg->desc || !vg->avail || !vg->used || !vg->dma_buf) return -1;
    if (req_len > 2048u || resp_len > 2048u) return -2;

    uint64_t fl = vgpu_lock_acq(vg);

    uint8_t *req_dma  = vg->dma_buf;
    uint8_t *resp_dma = vg->dma_buf + 2048u;
    memcpy(req_dma, req, req_len);
    memset(resp_dma, 0, resp_len);

    vg->desc[0].addr  = vg->dma_pa;
    vg->desc[0].len   = req_len;
    vg->desc[0].flags = VIRTQ_DESC_F_NEXT;
    vg->desc[0].next  = 1u;

    vg->desc[1].addr  = vg->dma_pa + 2048ull;
    vg->desc[1].len   = resp_len;
    vg->desc[1].flags = VIRTQ_DESC_F_WRITE;
    vg->desc[1].next  = 0u;

    __asm__ volatile("mfence" ::: "memory");
    uint16_t aidx = vg->avail->idx;
    vg->avail->ring[aidx % vg->qsize] = 0u;
    __asm__ volatile("mfence" ::: "memory");
    vg->avail->idx = (uint16_t)(aidx + 1u);
    __asm__ volatile("mfence" ::: "memory");

    /* Ring queue 0 doorbell */
    *vg->ctrl_notify = 0u;

    /* Poll used ring with bounded timeout (spin guard, works with IRQ off) */
    uint32_t spins = 0;
    while (vg->used->idx == vg->last_used_idx) {
        __asm__ volatile("pause" ::: "memory");
        if (++spins > 50000000u) {
            vgpu_lock_rel(vg, fl);
            serial_puts("[gpu-virtio] controlq timeout!\n");
            return -3;
        }
    }
    __asm__ volatile("mfence" ::: "memory");
    vg->last_used_idx = vg->used->idx;

    if (resp && resp_len) {
        memcpy(resp, resp_dma, resp_len);
    }

    vgpu_lock_rel(vg, fl);
    return 0;
}

static int vgpu_setup_scanout(vgpu_state_t *vg, uint32_t w, uint32_t h) {
    uint64_t fb_bytes = (uint64_t)w * (uint64_t)h * 4ull;
    if (fb_bytes == 0 || fb_bytes > BACKBUFFER_MAX) return -1;

    uint32_t old_id = vg->res_id;
    uint32_t new_id = old_id + 1u;
    if (new_id == 0) new_id = 1u;

    /* 1) RESOURCE_CREATE_2D */
    vgpu_cmd_create_2d_t c2d;
    memset(&c2d, 0, sizeof(c2d));
    c2d.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    c2d.resource_id = new_id;
    c2d.format      = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    c2d.width       = w;
    c2d.height      = h;

    vgpu_ctrl_hdr_t resp;
    if (vgpu_submit_cmd(vg, &c2d, sizeof(c2d), &resp, sizeof(resp)) != 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        serial_printf("[gpu-virtio] RESOURCE_CREATE_2D failed resp=0x%X\n",
                      (unsigned)resp.type);
        return -2;
    }

    /* 2) RESOURCE_ATTACH_BACKING (BACKBUFFER_BASE is physically contiguous!) */
    vgpu_cmd_attach_backing_1_t ab;
    memset(&ab, 0, sizeof(ab));
    ab.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    ab.resource_id = new_id;
    ab.nr_entries  = 1u;
    ab.addr        = (uint64_t)BACKBUFFER_BASE;
    ab.length      = (uint32_t)fb_bytes;

    if (vgpu_submit_cmd(vg, &ab, sizeof(ab), &resp, sizeof(resp)) != 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        serial_printf("[gpu-virtio] RESOURCE_ATTACH_BACKING failed resp=0x%X\n",
                      (unsigned)resp.type);
        return -3;
    }

    /* 3) SET_SCANOUT */
    vgpu_cmd_set_scanout_t so;
    memset(&so, 0, sizeof(so));
    so.hdr.type    = VIRTIO_GPU_CMD_SET_SCANOUT;
    so.r.x         = 0;
    so.r.y         = 0;
    so.r.width     = w;
    so.r.height    = h;
    so.scanout_id  = 0u;
    so.resource_id = new_id;

    if (vgpu_submit_cmd(vg, &so, sizeof(so), &resp, sizeof(resp)) != 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        serial_printf("[gpu-virtio] SET_SCANOUT failed resp=0x%X\n",
                      (unsigned)resp.type);
        return -4;
    }

    /* Release previous resource if mode-switching */
    if (old_id != 0) {
        vgpu_cmd_unref_t det;
        memset(&det, 0, sizeof(det));
        det.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
        det.resource_id = old_id;
        (void)vgpu_submit_cmd(vg, &det, sizeof(det), &resp, sizeof(resp));

        det.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
        (void)vgpu_submit_cmd(vg, &det, sizeof(det), &resp, sizeof(resp));
    }

    vg->res_id = new_id;
    vg->width  = w;
    vg->height = h;

    /* 4) Initial TRANSFER_TO_HOST_2D + RESOURCE_FLUSH */
    vgpu_cmd_transfer_2d_t tr;
    memset(&tr, 0, sizeof(tr));
    tr.hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    tr.r.x         = 0;
    tr.r.y         = 0;
    tr.r.width     = w;
    tr.r.height    = h;
    tr.offset      = 0;
    tr.resource_id = new_id;
    (void)vgpu_submit_cmd(vg, &tr, sizeof(tr), &resp, sizeof(resp));

    vgpu_cmd_flush_t flcmd;
    memset(&flcmd, 0, sizeof(flcmd));
    flcmd.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flcmd.r.x         = 0;
    flcmd.r.y         = 0;
    flcmd.r.width     = w;
    flcmd.r.height    = h;
    flcmd.resource_id = new_id;
    (void)vgpu_submit_cmd(vg, &flcmd, sizeof(flcmd), &resp, sizeof(resp));
    vg->flush_count++;

    return 0;
}

static void vgpu_flush_rect(gpu_drv_t *drv, int x, int y, int w, int h) {
    if (!drv || !drv->priv || w <= 0 || h <= 0) return;
    vgpu_state_t *vg = (vgpu_state_t *)drv->priv;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= (int)vg->width || y >= (int)vg->height) return;
    if (w > (int)vg->width - x)  w = (int)vg->width - x;
    if (h > (int)vg->height - y) h = (int)vg->height - y;
    if (w <= 0 || h <= 0) return;

    vgpu_ctrl_hdr_t resp;
    vgpu_cmd_transfer_2d_t tr;
    memset(&tr, 0, sizeof(tr));
    tr.hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    tr.r.x         = (uint32_t)x;
    tr.r.y         = (uint32_t)y;
    tr.r.width     = (uint32_t)w;
    tr.r.height    = (uint32_t)h;
    tr.offset      = ((uint64_t)(uint32_t)y * (uint64_t)vg->width + (uint64_t)(uint32_t)x) * 4ull;
    tr.resource_id = vg->res_id;
    (void)vgpu_submit_cmd(vg, &tr, sizeof(tr), &resp, sizeof(resp));

    vgpu_cmd_flush_t flcmd;
    memset(&flcmd, 0, sizeof(flcmd));
    flcmd.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flcmd.r.x         = (uint32_t)x;
    flcmd.r.y         = (uint32_t)y;
    flcmd.r.width     = (uint32_t)w;
    flcmd.r.height    = (uint32_t)h;
    flcmd.resource_id = vg->res_id;
    (void)vgpu_submit_cmd(vg, &flcmd, sizeof(flcmd), &resp, sizeof(resp));
    vg->flush_count++;
}

static void vgpu_blit_rect(gpu_drv_t *drv, const uint32_t *src, int src_pitch_px,
                           int sx, int sy, int dx, int dy, int w, int h) {
    if (!drv || !src || w <= 0 || h <= 0) return;

    if (dx < 0) { sx -= dx; w += dx; dx = 0; }
    if (dy < 0) { sy -= dy; h += dy; dy = 0; }
    if (dx >= drv->width || dy >= drv->height) return;
    if (w > drv->width - dx)  w = drv->width - dx;
    if (h > drv->height - dy) h = drv->height - dy;
    if (w <= 0 || h <= 0) return;

    /* Zero-copy fast path when presenting directly from BACKBUFFER_BASE */
    uint32_t *bb = (uint32_t *)(uintptr_t)BACKBUFFER_BASE;
    if (!(src == bb && src_pitch_px == drv->width && sx == dx && sy == dy)) {
        size_t row_bytes = (size_t)w * 4u;
        for (int row = 0; row < h; row++) {
            const uint32_t *srow = src + (size_t)(sy + row) * (size_t)src_pitch_px + (size_t)sx;
            uint32_t *drow = bb + (size_t)(dy + row) * (size_t)drv->width + (size_t)dx;
            gpu_simd_copy_mem(drow, srow, row_bytes);
        }
    }

    vgpu_flush_rect(drv, dx, dy, w, h);
}

static int vgpu_page_flip(gpu_drv_t *drv, int buf_index) {
    (void)buf_index;
    if (!drv || !drv->priv) return -1;
    uint32_t *bb = backbuffer_ptr();
    if (bb && bb != (uint32_t *)(uintptr_t)BACKBUFFER_BASE) {
        vgpu_blit_rect(drv, bb, drv->width, 0, 0, 0, 0, drv->width, drv->height);
    } else {
        vgpu_flush_rect(drv, 0, 0, drv->width, drv->height);
    }
    return 0;
}

static void vgpu_fill_rect(gpu_drv_t *drv, int x, int y, int w, int h, uint32_t color) {
    if (!drv || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= drv->width || y >= drv->height) return;
    if (w > drv->width - x)  w = drv->width - x;
    if (h > drv->height - y) h = drv->height - y;
    if (w <= 0 || h <= 0) return;

    uint32_t *bb = (uint32_t *)(uintptr_t)BACKBUFFER_BASE;
    for (int row = 0; row < h; row++) {
        gpu_simd_fill32(bb + (size_t)(y + row) * (size_t)drv->width + (size_t)x,
                        color, (size_t)w);
    }
    vgpu_flush_rect(drv, x, y, w, h);
}

static int vgpu_set_mode(gpu_drv_t *drv, int width, int height, int bpp) {
    if (!drv || !drv->priv) return -1;
    if (bpp <= 0) bpp = 32;
    if (bpp != 32 || width < 320 || height < 200) return -1;
    if ((uint64_t)width * (uint64_t)height * 4ull > BACKBUFFER_MAX) return -2;

    vgpu_state_t *vg = (vgpu_state_t *)drv->priv;
    if (vgpu_setup_scanout(vg, (uint32_t)width, (uint32_t)height) != 0)
        return -3;

    drv->width  = width;
    drv->height = height;
    drv->pitch  = width * 4;
    drv->bpp    = 32;
    drv->fb_virt = (uint32_t *)(uintptr_t)BACKBUFFER_BASE;
    vesa_attach_external(drv->fb_virt, BACKBUFFER_BASE,
                         drv->width, drv->height, drv->pitch, drv->bpp);
    return 0;
}

int gpu_virtio_probe(const gpu_pci_info_t *info, gpu_drv_t *out,
                     int req_w, int req_h, int req_bpp) {
    (void)req_bpp;
    if (!info || !out) return -1;
    if (info->pci.vendor != PCI_VENDOR_VIRTIO || info->pci.device != 0x1050u)
        return -1;

    uint8_t bus = info->pci.bus, slot = info->pci.slot, func = info->pci.func;
    pci_enable_bus_mastering(bus, slot, func);

    uint16_t status = pci_config_readw(bus, slot, func, PCI_CONFIG_STATUS);
    if (!(status & 0x0010u)) {
        serial_puts("[gpu-virtio] no PCI capability list\n");
        return -2;
    }

    memset(&s_vgpu, 0, sizeof(s_vgpu));

    uint64_t mmio_base = 0;
    uint64_t mmio_size = 0;
    uint8_t cap_ptr = (uint8_t)(pci_config_readb(bus, slot, func, PCI_CONFIG_CAP_PTR) & 0xFCu);
    for (int iter = 0; iter < 48 && cap_ptr >= 0x40u; iter++) {
        uint8_t cap_id   = pci_config_readb(bus, slot, func, cap_ptr);
        uint8_t cap_next = (uint8_t)(pci_config_readb(bus, slot, func, (uint8_t)(cap_ptr + 1u)) & 0xFCu);

        if (cap_id == 0x09u) {
            uint8_t  cfg_type = pci_config_readb(bus, slot, func, (uint8_t)(cap_ptr + 3u));
            uint8_t  bar_idx  = pci_config_readb(bus, slot, func, (uint8_t)(cap_ptr + 4u));
            uint32_t offset   = pci_config_readl(bus, slot, func, (uint8_t)(cap_ptr + 8u));
            uint32_t length   = pci_config_readl(bus, slot, func, (uint8_t)(cap_ptr + 12u));

            if (bar_idx < 6u && !info->bar[bar_idx].is_io && info->bar[bar_idx].address != 0) {
                uint64_t baddr = info->bar[bar_idx].address;
                uint64_t bsize = info->bar[bar_idx].size ? info->bar[bar_idx].size : (uint64_t)(offset + length);
                if (vmm_map_mmio_range(baddr, bsize) == 0) {
                    uint64_t ptr = baddr + (uint64_t)offset;
                    if (!mmio_base) { mmio_base = baddr; mmio_size = bsize; }
                    switch (cfg_type) {
                        case VIRTIO_PCI_CAP_COMMON_CFG:
                            s_vgpu.common = (volatile virtio_pci_common_cfg_t *)(uintptr_t)ptr;
                            break;
                        case VIRTIO_PCI_CAP_NOTIFY_CFG:
                            s_vgpu.notify_base = (volatile uint8_t *)(uintptr_t)ptr;
                            s_vgpu.notify_mult = pci_config_readl(bus, slot, func, (uint8_t)(cap_ptr + 16u));
                            break;
                        case VIRTIO_PCI_CAP_ISR_CFG:
                            s_vgpu.isr_cfg = (volatile uint8_t *)(uintptr_t)ptr;
                            break;
                        case VIRTIO_PCI_CAP_DEVICE_CFG:
                            s_vgpu.dev_cfg = (volatile uint8_t *)(uintptr_t)ptr;
                            break;
                        default:
                            break;
                    }
                }
            }
        }
        cap_ptr = cap_next;
    }

    if (!s_vgpu.common || !s_vgpu.notify_base) {
        serial_puts("[gpu-virtio] missing modern VirtIO PCI caps (COMMON/NOTIFY)\n");
        return -3;
    }

    /* VirtIO 1.0 init sequence */
    s_vgpu.common->device_status = 0u;
    for (int i = 0; i < 10000 && s_vgpu.common->device_status != 0u; i++)
        __asm__ volatile("pause");

    s_vgpu.common->device_status = VIRTIO_STATUS_ACKNOWLEDGE;
    s_vgpu.common->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;

    s_vgpu.common->device_feature_select = 0u;
    uint32_t f0 = s_vgpu.common->device_feature;
    s_vgpu.common->device_feature_select = 1u;
    uint32_t f1 = s_vgpu.common->device_feature;
    (void)f0;

    /* Negotiate VIRTIO_F_VERSION_1 (bit 32 -> word 1 bit 0), 2D baseline */
    s_vgpu.common->driver_feature_select = 0u;
    s_vgpu.common->driver_feature = 0u;
    s_vgpu.common->driver_feature_select = 1u;
    s_vgpu.common->driver_feature = (f1 & 1u);

    s_vgpu.common->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                                   VIRTIO_STATUS_FEATURES_OK;
    if (!(s_vgpu.common->device_status & VIRTIO_STATUS_FEATURES_OK)) {
        serial_puts("[gpu-virtio] FEATURES_OK rejected\n");
        return -4;
    }

    /* Configure controlq (queue 0) */
    s_vgpu.common->queue_select = 0u;
    uint16_t max_q = s_vgpu.common->queue_size;
    if (max_q == 0u) {
        serial_puts("[gpu-virtio] controlq size 0\n");
        return -5;
    }
    uint16_t qsz = (max_q > VGPU_MAX_QSIZE) ? VGPU_MAX_QSIZE : max_q;
    s_vgpu.qsize = qsz;
    s_vgpu.common->queue_size = qsz;

    s_vgpu.desc_pa  = vmm_alloc_frame();
    s_vgpu.avail_pa = vmm_alloc_frame();
    s_vgpu.used_pa  = vmm_alloc_frame();
    s_vgpu.dma_pa   = vmm_alloc_frame();
    if (!s_vgpu.desc_pa || !s_vgpu.avail_pa || !s_vgpu.used_pa || !s_vgpu.dma_pa) {
        serial_puts("[gpu-virtio] DMA frame alloc failed\n");
        return -6;
    }

    s_vgpu.desc    = (volatile virtq_desc_t *)(uintptr_t)s_vgpu.desc_pa;
    s_vgpu.avail   = (volatile virtq_avail_t *)(uintptr_t)s_vgpu.avail_pa;
    s_vgpu.used    = (volatile virtq_used_t *)(uintptr_t)s_vgpu.used_pa;
    s_vgpu.dma_buf = (uint8_t *)(uintptr_t)s_vgpu.dma_pa;

    s_vgpu.common->queue_desc_lo   = (uint32_t)(s_vgpu.desc_pa & 0xFFFFFFFFull);
    s_vgpu.common->queue_desc_hi   = (uint32_t)(s_vgpu.desc_pa >> 32);
    s_vgpu.common->queue_driver_lo = (uint32_t)(s_vgpu.avail_pa & 0xFFFFFFFFull);
    s_vgpu.common->queue_driver_hi = (uint32_t)(s_vgpu.avail_pa >> 32);
    s_vgpu.common->queue_device_lo = (uint32_t)(s_vgpu.used_pa & 0xFFFFFFFFull);
    s_vgpu.common->queue_device_hi = (uint32_t)(s_vgpu.used_pa >> 32);

    uint16_t notify_off = s_vgpu.common->queue_notify_off;
    s_vgpu.ctrl_notify = (volatile uint16_t *)(uintptr_t)(
        (uintptr_t)s_vgpu.notify_base + (uint64_t)notify_off * (uint64_t)s_vgpu.notify_mult);

    s_vgpu.common->queue_enable = 1u;
    s_vgpu.common->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                                   VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK;

    /* 1) GET_DISPLAY_INFO */
    vgpu_ctrl_hdr_t di_req;
    memset(&di_req, 0, sizeof(di_req));
    di_req.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    vgpu_resp_display_info_t di_resp;
    memset(&di_resp, 0, sizeof(di_resp));
    if (vgpu_submit_cmd(&s_vgpu, &di_req, sizeof(di_req), &di_resp, sizeof(di_resp)) != 0 ||
        di_resp.hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        serial_printf("[gpu-virtio] GET_DISPLAY_INFO failed resp=0x%X\n",
                      (unsigned)di_resp.hdr.type);
        return -7;
    }

    uint32_t w = (req_w > 0) ? (uint32_t)req_w : 1280u;
    uint32_t h = (req_h > 0) ? (uint32_t)req_h : 720u;
    if (di_resp.pmodes[0].enabled &&
        di_resp.pmodes[0].r.width >= 640u && di_resp.pmodes[0].r.height >= 480u &&
        (uint64_t)di_resp.pmodes[0].r.width * (uint64_t)di_resp.pmodes[0].r.height * 4ull <= BACKBUFFER_MAX) {
        w = di_resp.pmodes[0].r.width;
        h = di_resp.pmodes[0].r.height;
    }

    serial_printf("[gpu-virtio] GET_DISPLAY_INFO OK pmode0=%ux%u enabled=%u -> using %ux%u\n",
                  di_resp.pmodes[0].r.width, di_resp.pmodes[0].r.height,
                  di_resp.pmodes[0].enabled, w, h);

    /* Clear backbuffer before first scanout flush */
    gpu_simd_fill32((uint32_t *)(uintptr_t)BACKBUFFER_BASE, 0x0E0B12u,
                    (size_t)w * (size_t)h);

    /* 2..4) RESOURCE_CREATE_2D + ATTACH_BACKING + SET_SCANOUT + TRANSFER + FLUSH */
    if (vgpu_setup_scanout(&s_vgpu, w, h) != 0) {
        return -8;
    }

    out->vendor_id  = info->pci.vendor;
    out->device_id  = info->pci.device;
    out->bus        = bus;
    out->slot       = slot;
    out->func       = func;
    out->rev_id     = info->pci.rev_id;
    out->vram_phys  = BACKBUFFER_BASE;
    out->vram_size  = BACKBUFFER_MAX;
    out->mmio_phys  = mmio_base;
    out->mmio_size  = mmio_size;
    out->fb_virt    = (uint32_t *)(uintptr_t)BACKBUFFER_BASE;
    out->mmio_virt  = mmio_base;
    out->width      = (int)w;
    out->height     = (int)h;
    out->pitch      = (int)(w * 4u);
    out->bpp        = 32;
    out->page_count = 1;
    out->front_page = 0;
    out->page_stride_y = h;
    out->wc_active  = false; /* Backing store is cached WB RAM; host DMA transfers on flush */
    out->caps       = GPU_CAP_VIRTIO_2D | GPU_CAP_2D_BLT | GPU_CAP_MODESET | GPU_CAP_PAGEFLIP;
    if (gpu_simd_caps() & GPU_SIMD_SSE2) out->caps |= GPU_CAP_SIMD;

    const char *nm = "VirtIO-GPU 2D";
    for (int i = 0; nm[i] && i < (int)sizeof(out->name) - 1; i++) out->name[i] = nm[i];
    const char *gn = "VirtIO 1.0 PCI 2D";
    for (int i = 0; gn[i] && i < (int)sizeof(out->gen_name) - 1; i++) out->gen_name[i] = gn[i];

    out->ops.flush_rect  = vgpu_flush_rect;
    out->ops.fill_rect   = vgpu_fill_rect;
    out->ops.blit_rect   = vgpu_blit_rect;
    out->ops.alpha_blend = gpu_lfb_alpha_blend;
    out->ops.wait_vsync  = 0;
    out->ops.set_mode    = vgpu_set_mode;
    out->ops.page_flip   = vgpu_page_flip;
    out->priv            = &s_vgpu;
    out->active          = true;

    vesa_attach_external(out->fb_virt, BACKBUFFER_BASE,
                         out->width, out->height, out->pitch, out->bpp);

    serial_printf("[gpu-virtio] RESOURCE_CREATE_2D(id=%u) ATTACH_BACKING(0x%08X) SET_SCANOUT TRANSFER_TO_HOST_2D RESOURCE_FLUSH OK (%ux%u qsz=%u)\n",
                  s_vgpu.res_id, (unsigned)BACKBUFFER_BASE, w, h, (unsigned)qsz);
    return 0;
}
