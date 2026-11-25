#ifndef ARCTIAN_GPU_H
#define ARCTIAN_GPU_H

#include <stdint.h>
#include <stdbool.h>
#include "pci.h"

/* GPU hardware capabilities */
#define GPU_CAP_WC         (1u << 0)  /* PAT Write-Combining VRAM mapping */
#define GPU_CAP_SIMD       (1u << 1)  /* SSE2 / AVX2 accelerated blitter */
#define GPU_CAP_VSYNC      (1u << 2)  /* Hardware VBlank synchronization */
#define GPU_CAP_2D_BLT     (1u << 3)  /* Hardware 2D blitter engine (BCS/etc.) */
#define GPU_CAP_MODESET    (1u << 4)  /* Runtime resolution mode-setting */
#define GPU_CAP_PAGEFLIP   (1u << 5)  /* Hardware double-buffer page flip */
#define GPU_CAP_VIRTIO_2D  (1u << 6)  /* VirtIO-GPU 2D command queue */

#define GPU_MAX_DEVICES    8

typedef enum {
    GPU_VENDOR_UNKNOWN = 0,
    GPU_VENDOR_INTEL   = 1,
    GPU_VENDOR_NVIDIA  = 2,
    GPU_VENDOR_BOCHS   = 3,
    GPU_VENDOR_VIRTIO  = 4,
    GPU_VENDOR_AMD     = 5,
    GPU_VENDOR_GENERIC = 6
} gpu_vendor_kind_t;

typedef struct {
    pci_device_t      pci;
    pci_bar_t         bar[6];
    gpu_vendor_kind_t vendor_kind;
    char              vendor_name[24];
    char              gen_name[48];
    bool              active;
} gpu_pci_info_t;

typedef struct {
    int width;
    int height;
    int bpp;
} gpu_mode_t;

typedef struct gpu_drv gpu_drv_t;

typedef struct {
    void (*flush_rect)(gpu_drv_t *drv, int x, int y, int w, int h);
    void (*fill_rect)(gpu_drv_t *drv, int x, int y, int w, int h, uint32_t color);
    void (*blit_rect)(gpu_drv_t *drv, const uint32_t *src, int src_pitch_px,
                      int sx, int sy, int dx, int dy, int w, int h);
    void (*alpha_blend)(gpu_drv_t *drv, const uint32_t *src, int src_pitch_px,
                        int sx, int sy, int dx, int dy, int w, int h);
    void (*wait_vsync)(gpu_drv_t *drv);
    int  (*set_mode)(gpu_drv_t *drv, int width, int height, int bpp);
    int  (*page_flip)(gpu_drv_t *drv, int buf_index);
} gpu_ops_t;

struct gpu_drv {
    char       name[64];
    uint16_t   vendor_id;
    uint16_t   device_id;
    uint8_t    bus;
    uint8_t    slot;
    uint8_t    func;
    uint8_t    rev_id;
    char       gen_name[48];

    uint64_t   vram_phys;
    uint64_t   vram_size;
    uint64_t   mmio_phys;
    uint64_t   mmio_size;
    uint32_t  *fb_virt;
    uint64_t   mmio_virt;

    int        width;
    int        height;
    int        pitch;         /* bytes per scanline */
    int        bpp;
    uint32_t   caps;
    bool       wc_active;
    bool       active;

    int        page_count;    /* 1 or 2 */
    int        front_page;    /* 0 or 1 */
    uint32_t   page_stride_y; /* scanlines per page */

    gpu_ops_t  ops;
    void      *priv;
};

/* Core GPU subsystem lifecycle */
int                   gpu_init(int req_width, int req_height, int req_bpp);
bool                  gpu_is_ready(void);
gpu_drv_t            *gpu_active(void);
int                   gpu_device_count(void);
const gpu_pci_info_t *gpu_device_info(int idx);
const char           *gpu_status_name(void);
uint32_t              gpu_bench_mbps(void);

/* High-level rendering / presentation interface */
void gpu_present_backbuffer(const uint32_t *bb, int bb_w, int bb_h,
                            int x, int y, int w, int h);
void gpu_flush_rect(int x, int y, int w, int h);
void gpu_fill_rect(int x, int y, int w, int h, uint32_t color);
void gpu_alpha_blend_rect(const uint32_t *src, int src_pitch_px,
                          int sx, int sy, int dx, int dy, int w, int h);
void gpu_wait_vsync(void);
int  gpu_set_mode(int width, int height, int bpp);
int  gpu_page_flip(int buf_index);
void gpu_vsync_flip(void);
int  gpu_list_modes(gpu_mode_t *out, int max);

/* Shared generic LFB WC+SIMD fallback ops (reusable by hardware backends) */
void gpu_lfb_blit_rect(gpu_drv_t *drv, const uint32_t *src, int src_pitch_px,
                       int sx, int sy, int dx, int dy, int w, int h);
void gpu_lfb_fill_rect(gpu_drv_t *drv, int x, int y, int w, int h, uint32_t color);
void gpu_lfb_alpha_blend(gpu_drv_t *drv, const uint32_t *src, int src_pitch_px,
                         int sx, int sy, int dx, int dy, int w, int h);

/* Per-vendor probe entry points (internal to gpu.c) */
int  gpu_virtio_probe(const gpu_pci_info_t *info, gpu_drv_t *out,
                      int req_w, int req_h, int req_bpp);
int  gpu_intel_probe(const gpu_pci_info_t *info, gpu_drv_t *out,
                     int req_w, int req_h, int req_bpp);
int  gpu_nvidia_probe(const gpu_pci_info_t *info, gpu_drv_t *out,
                      int req_w, int req_h, int req_bpp);
int  gpu_bga_probe(const gpu_pci_info_t *info, gpu_drv_t *out,
                   int req_w, int req_h, int req_bpp);

/* Vendor device classification helpers */
const char *gpu_intel_classify(uint16_t dev_id, int *gen_out);
const char *gpu_nvidia_classify(uint16_t dev_id);

#endif /* ARCTIAN_GPU_H */
