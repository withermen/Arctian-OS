/**
 * Arctian Intel UHD / Iris Xe / Arc Native GPU Backend (PCI Vendor 0x8086)
 *
 * Supports Gen9/9.5 (Skylake, Kaby/Coffee/Comet Lake UHD 610/620/630),
 * Gen11 (Ice Lake), Gen12 / Xe-LP (Tiger/Rocket/Alder/Raptor Lake UHD 730/770,
 * Iris Xe), Xe-HPG (Arc Alchemist), and Xe-LPG (Meteor Lake).
 *
 * Uses open-documented Intel PRM MMIO display registers:
 *   - BAR0 (GTTMMADR) mapped UC- for safe register access
 *   - BAR2 (GMADR) / Boot LFB mapped PAT Write-Combining (WC)
 *   - Active display pipe discovery (PIPE_A/B/C: PIPESRC, PLANE_CTL, PLANE_SURF)
 *   - Hardware VBlank sync via PIPE_FRMCOUNT
 *   - Tear-free double-buffer page flip via PLANE_SURF update
 *   - Open-documented BCS 2D Blitter ring (0x22000 XY_COLOR_BLT / XY_SRC_COPY_BLT)
 *     with strict idle/timeout guards and instant fallback to WC + SIMD blit.
 */
#include "gpu.h"
#include "gpu_simd.h"
#include "vesa.h"
#include "bootinfo.h"
#include "paging.h"
#include "clock.h"
#include "kernel.h"
#include "serial.h"

/* Open-documented Intel Display Pipe registers (Gen9 / Gen11 / Gen12 / Xe) */
#define INTEL_PIPE_SRC(p)        (0x6001Cu + (uint32_t)(p) * 0x1000u)
#define INTEL_PIPE_FRMCOUNT(p)   (0x70040u + (uint32_t)(p) * 0x1000u)
#define INTEL_PLANE_CTL_1(p)     (0x70180u + (uint32_t)(p) * 0x1000u)
#define INTEL_PLANE_STRIDE_1(p)  (0x70188u + (uint32_t)(p) * 0x1000u)
#define INTEL_PLANE_SURF_1(p)    (0x7019Cu + (uint32_t)(p) * 0x1000u)

#define INTEL_PLANE_CTL_ENABLE   (1u << 31)
#define INTEL_PLANE_CTL_TILED    (0x7u << 10)

/* Open-documented Intel Blitter Command Streamer (BCS0) ring registers */
#define INTEL_BCS_RING_TAIL      0x22030u
#define INTEL_BCS_RING_HEAD      0x22034u
#define INTEL_BCS_RING_START     0x22038u
#define INTEL_BCS_RING_CTL       0x2203Cu

#define INTEL_RING_HEAD_MASK     0x001FFFFCu
#define INTEL_RING_TAIL_MASK     0x001FFFF8u
#define INTEL_RING_CTL_ENABLE    0x00000001u

/* Gen8+ 2D Blitter MI/XY opcodes */
#define INTEL_XY_COLOR_BLT_CMD   ((2u << 29) | (0x50u << 22) | (3u << 20) | 5u)
#define INTEL_XY_SRC_COPY_CMD    ((2u << 29) | (0x53u << 22) | (3u << 20) | 8u)
#define INTEL_MI_FLUSH_DW_CMD    ((0u << 29) | (0x26u << 23) | 2u)
#define INTEL_MI_NOOP            0x00000000u

#define INTEL_MMIO_MAP_CAP       (4ull * 1024ull * 1024ull)
#define INTEL_VRAM_MAP_CAP       (64ull * 1024ull * 1024ull)

typedef struct {
    volatile uint8_t *mmio;
    int               gen;
    int               active_pipe;   /* 0=A, 1=B, 2=C, -1=none */
    uint32_t          base_surf_gtt; /* Linear GTT offset of front plane */
    bool              plane_linear;
    bool              bcs_ready;
    uint32_t         *bcs_ring_virt;
    uint32_t          bcs_ring_size; /* bytes */
    uint32_t          bcs_tail;
} intel_gpu_priv_t;

static intel_gpu_priv_t s_intel;

static inline uint32_t intel_rd32(const intel_gpu_priv_t *ig, uint32_t reg) {
    return *(volatile const uint32_t *)(ig->mmio + reg);
}

static inline void intel_wr32(intel_gpu_priv_t *ig, uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(ig->mmio + reg) = val;
    /* Posting read to ensure MMIO write completion */
    (void)*(volatile const uint32_t *)(ig->mmio + reg);
}

const char *gpu_intel_classify(uint16_t dev_id, int *gen_out) {
    uint16_t hi = (uint16_t)(dev_id & 0xFF00u);
    int gen = 9;
    const char *name = "Intel HD/UHD Graphics";

    if (hi == 0x1900u) {
        gen = 9;  name = "Gen9 Skylake HD/Iris";
    } else if (hi == 0x5900u) {
        gen = 95; name = "Gen9.5 Kaby Lake HD/UHD 620/630";
    } else if (hi == 0x3E00u) {
        gen = 95; name = "Gen9.5 Coffee Lake UHD 610/620/630";
    } else if (hi == 0x9B00u) {
        gen = 95; name = "Gen9.5 Comet Lake UHD 620/630";
    } else if (hi == 0x5A00u || hi == 0x3100u) {
        gen = 9;  name = "Gen9 Apollo/Gemini Lake UHD";
    } else if (hi == 0x8A00u) {
        gen = 11; name = "Gen11 Ice Lake UHD/Iris Plus";
    } else if (hi == 0x9A00u) {
        gen = 12; name = "Gen12 Xe-LP Tiger Lake Iris Xe";
    } else if (hi == 0x4C00u) {
        gen = 12; name = "Gen12 Xe-LP Rocket Lake UHD 730/750";
    } else if (hi == 0x4600u) {
        gen = 12; name = "Gen12 Xe-LP Alder Lake UHD 730/770 / Iris Xe";
    } else if (hi == 0xA700u) {
        gen = 12; name = "Gen12 Xe-LP Raptor Lake UHD 770 / Iris Xe";
    } else if (hi == 0x5600u) {
        gen = 127; name = "Xe-HPG Arc Alchemist";
    } else if (hi == 0x7D00u) {
        gen = 128; name = "Xe-LPG Meteor/Arrow Lake Arc";
    } else if (hi == 0x0400u || hi == 0x0A00u || hi == 0x0D00u) {
        gen = 75; name = "Gen7.5 Haswell HD";
    } else if (hi == 0x1600u) {
        gen = 8;  name = "Gen8 Broadwell HD";
    }

    if (gen_out) *gen_out = gen;
    return name;
}

static void intel_wait_vsync(gpu_drv_t *drv) {
    if (!drv || !drv->priv) return;
    intel_gpu_priv_t *ig = (intel_gpu_priv_t *)drv->priv;
    if (!ig->mmio || ig->active_pipe < 0) return;

    uint32_t reg = INTEL_PIPE_FRMCOUNT(ig->active_pipe);
    uint32_t start_cnt = intel_rd32(ig, reg);
    uint64_t deadline = clock_ms() + 25ull;
    uint32_t spins = 0;

    while (intel_rd32(ig, reg) == start_cnt) {
        if (++spins > 500000u || clock_ms() >= deadline) break;
        __asm__ volatile("pause");
    }
}

static int intel_page_flip(gpu_drv_t *drv, int buf_index) {
    if (!drv || !drv->priv || drv->page_count < 2) return -1;
    intel_gpu_priv_t *ig = (intel_gpu_priv_t *)drv->priv;
    if (!ig->mmio || ig->active_pipe < 0 || !ig->plane_linear) return -1;

    int idx = buf_index & 1;
    uint64_t page_bytes = (uint64_t)drv->pitch * (uint64_t)drv->height;
    uint32_t surf_off = (uint32_t)((ig->base_surf_gtt + (uint64_t)idx * page_bytes) & ~0xFFFu);

    /* Writing PLANE_SURF arms atomic flip at next VBlank */
    intel_wr32(ig, INTEL_PLANE_SURF_1(ig->active_pipe), surf_off);
    drv->front_page = idx;
    return 0;
}

/* ---- Open-documented BCS 2D Blitter with strict idle/timeout guards ---- */

static int intel_bcs_wait_idle(intel_gpu_priv_t *ig) {
    if (!ig->bcs_ready) return -1;
    uint64_t deadline = clock_ms() + 5ull;
    uint32_t spins = 0;
    for (;;) {
        uint32_t head = intel_rd32(ig, INTEL_BCS_RING_HEAD) & INTEL_RING_HEAD_MASK;
        uint32_t tail = ig->bcs_tail & INTEL_RING_TAIL_MASK;
        if (head == tail) return 0;
        if (++spins > 100000u || clock_ms() >= deadline) {
            /* Hardware ring stalled/unpowered: permanently fall back to WC+SIMD */
            ig->bcs_ready = false;
            serial_puts("[gpu-intel] BCS idle timeout -> fallback WC+SIMD\n");
            return -1;
        }
        __asm__ volatile("pause");
    }
}

static inline void intel_bcs_emit(intel_gpu_priv_t *ig, uint32_t dw) {
    uint32_t idx = (ig->bcs_tail >> 2) & ((ig->bcs_ring_size >> 2) - 1u);
    ig->bcs_ring_virt[idx] = dw;
    ig->bcs_tail = (ig->bcs_tail + 4u) & (ig->bcs_ring_size - 1u);
}

static int intel_bcs_color_blt(gpu_drv_t *drv, int x, int y, int w, int h, uint32_t color) {
    intel_gpu_priv_t *ig = (intel_gpu_priv_t *)drv->priv;
    if (!ig || !ig->bcs_ready || !ig->plane_linear) return -1;
    if (intel_bcs_wait_idle(ig) != 0) return -1;

    uint64_t dst_gtt = (uint64_t)ig->base_surf_gtt +
                       (uint64_t)drv->front_page * (uint64_t)drv->pitch * (uint64_t)drv->height;
    /* BR13: 32-bit color depth (3<<24) | ROP=0xF0 (PATCOPY, 0xF0<<16) | pitch */
    uint32_t br13 = (3u << 24) | (0xF0u << 16) | ((uint32_t)drv->pitch & 0xFFFFu);

    intel_bcs_emit(ig, INTEL_XY_COLOR_BLT_CMD);
    intel_bcs_emit(ig, br13);
    intel_bcs_emit(ig, ((uint32_t)y << 16) | ((uint32_t)x & 0xFFFFu));
    intel_bcs_emit(ig, ((uint32_t)(y + h) << 16) | ((uint32_t)(x + w) & 0xFFFFu));
    intel_bcs_emit(ig, (uint32_t)(dst_gtt & 0xFFFFFFFFull));
    intel_bcs_emit(ig, (uint32_t)(dst_gtt >> 32));
    intel_bcs_emit(ig, color);
    intel_bcs_emit(ig, INTEL_MI_NOOP); /* keep qword alignment */

    __asm__ volatile("sfence" ::: "memory");
    intel_wr32(ig, INTEL_BCS_RING_TAIL, ig->bcs_tail & INTEL_RING_TAIL_MASK);
    return intel_bcs_wait_idle(ig);
}

static void intel_fill_rect(gpu_drv_t *drv, int x, int y, int w, int h, uint32_t color) {
    if (!drv || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= drv->width || y >= drv->height) return;
    if (w > drv->width - x)  w = drv->width - x;
    if (h > drv->height - y) h = drv->height - y;
    if (w <= 0 || h <= 0) return;

    intel_gpu_priv_t *ig = (intel_gpu_priv_t *)drv->priv;
    if (ig && ig->bcs_ready && (w * h >= 4096)) {
        if (intel_bcs_color_blt(drv, x, y, w, h, color) == 0)
            return;
    }
    gpu_lfb_fill_rect(drv, x, y, w, h, color);
}

static void intel_flush_rect(gpu_drv_t *drv, int x, int y, int w, int h) {
    (void)drv; (void)x; (void)y; (void)w; (void)h;
}

int gpu_intel_probe(const gpu_pci_info_t *info, gpu_drv_t *out,
                    int req_w, int req_h, int req_bpp) {
    (void)req_bpp;
    if (!info || !out) return -1;
    if (info->pci.vendor != PCI_VENDOR_INTEL) return -1;

    pci_enable_bus_mastering(info->pci.bus, info->pci.slot, info->pci.func);

    memset(&s_intel, 0, sizeof(s_intel));
    s_intel.active_pipe = -1;

    const char *gen_name = gpu_intel_classify(info->pci.device, &s_intel.gen);

    /* BAR0 = GTTMMADR (MMIO registers + GTT), BAR2 = GMADR (VRAM aperture) */
    uint64_t mmio_phys = (!info->bar[0].is_io) ? info->bar[0].address : 0;
    uint64_t mmio_size = (!info->bar[0].is_io) ? info->bar[0].size : 0;
    uint64_t vram_phys = (!info->bar[2].is_io) ? info->bar[2].address : 0;
    uint64_t vram_size = (!info->bar[2].is_io) ? info->bar[2].size : 0;

    if (mmio_phys != 0) {
        uint64_t map_mmio = mmio_size ? mmio_size : (2ull * 1024ull * 1024ull);
        if (map_mmio > INTEL_MMIO_MAP_CAP) map_mmio = INTEL_MMIO_MAP_CAP;
        if (vmm_map_mmio_range(mmio_phys, map_mmio) == 0) {
            s_intel.mmio = (volatile uint8_t *)(uintptr_t)mmio_phys;
        }
    }

    /* Check boot VBE/GOP framebuffer info first (firmware-configured display) */
    const arctian_bootinfo_t *bi = arctian_bootinfo();
    int width = req_w, height = req_h, pitch = req_w * 4, bpp = 32;
    uint64_t fb_phys = vram_phys;
    if (arctian_bootinfo_valid(bi)) {
        fb_phys = (uint64_t)bi->fb_phys;
        width   = (int)bi->width;
        height  = (int)bi->height;
        pitch   = bi->pitch ? (int)bi->pitch : (width * 4);
        bpp     = bi->bpp ? (int)bi->bpp : 32;
        if (bi->fb_size && bi->fb_size > vram_size) vram_size = bi->fb_size;
    }

    /* Safe read-only inspection of active display pipe (A/B/C) */
    if (s_intel.mmio) {
        for (int p = 0; p < 3; p++) {
            uint32_t ctl = intel_rd32(&s_intel, INTEL_PLANE_CTL_1(p));
            if (ctl == 0xFFFFFFFFu) break; /* MMIO unresponsive */
            if (ctl & INTEL_PLANE_CTL_ENABLE) {
                uint32_t src = intel_rd32(&s_intel, INTEL_PIPE_SRC(p));
                int pw = (int)((src >> 16) & 0x1FFFu) + 1;
                int ph = (int)(src & 0x1FFFu) + 1;
                if (pw >= 320 && pw <= 7680 && ph >= 200 && ph <= 4320) {
                    s_intel.active_pipe   = p;
                    s_intel.plane_linear  = ((ctl & INTEL_PLANE_CTL_TILED) == 0);
                    s_intel.base_surf_gtt = intel_rd32(&s_intel, INTEL_PLANE_SURF_1(p)) & ~0xFFFu;
                    if (!arctian_bootinfo_valid(bi)) {
                        width  = pw;
                        height = ph;
                        uint32_t stride_cl = intel_rd32(&s_intel, INTEL_PLANE_STRIDE_1(p)) & 0x3FFu;
                        pitch = stride_cl ? (int)(stride_cl * 64u) : (width * 4);
                    }
                    break;
                }
            }
        }

        /* Probe if BIOS/UEFI left BCS0 ring active & idle (never force-enable
         * uninitialized GT forcewake domains on bare metal to avoid hangs). */
        uint32_t bcs_ctl = intel_rd32(&s_intel, INTEL_BCS_RING_CTL);
        if (bcs_ctl != 0xFFFFFFFFu && (bcs_ctl & INTEL_RING_CTL_ENABLE)) {
            uint32_t head = intel_rd32(&s_intel, INTEL_BCS_RING_HEAD) & INTEL_RING_HEAD_MASK;
            uint32_t tail = intel_rd32(&s_intel, INTEL_BCS_RING_TAIL) & INTEL_RING_TAIL_MASK;
            uint32_t rstart = intel_rd32(&s_intel, INTEL_BCS_RING_START) & ~0xFFFu;
            uint32_t rlen = (((bcs_ctl >> 12) & 0x1FFu) + 1u) * 4096u;
            if (head == tail && rstart && rlen >= 4096u && rlen <= 65536u &&
                vram_phys && rstart < vram_size && s_intel.plane_linear) {
                s_intel.bcs_ring_virt = (uint32_t *)(uintptr_t)(vram_phys + (uint64_t)rstart);
                s_intel.bcs_ring_size = rlen;
                s_intel.bcs_tail      = tail;
                s_intel.bcs_ready     = true;
            }
        }
    }

    if (fb_phys == 0) return -2;
    if (vram_size == 0) vram_size = 32ull * 1024ull * 1024ull;

    uint64_t map_vram = vram_size;
    if (map_vram > INTEL_VRAM_MAP_CAP) map_vram = INTEL_VRAM_MAP_CAP;
    if (map_vram < (uint64_t)pitch * (uint64_t)height)
        map_vram = (uint64_t)pitch * (uint64_t)height;

    if (vmm_map_wc_range(fb_phys, map_vram) != 0) return -3;

    out->vendor_id  = info->pci.vendor;
    out->device_id  = info->pci.device;
    out->bus        = info->pci.bus;
    out->slot       = info->pci.slot;
    out->func       = info->pci.func;
    out->rev_id     = info->pci.rev_id;
    out->vram_phys  = fb_phys;
    out->vram_size  = vram_size;
    out->mmio_phys  = mmio_phys;
    out->mmio_size  = mmio_size;
    out->fb_virt    = (uint32_t *)(uintptr_t)fb_phys;
    out->mmio_virt  = (uint64_t)(uintptr_t)s_intel.mmio;
    out->width      = width;
    out->height     = height;
    out->pitch      = pitch;
    out->bpp        = bpp;
    out->wc_active  = vmm_pat_enabled() != 0;
    out->front_page = 0;
    out->page_stride_y = (uint32_t)height;

    uint64_t page_bytes = (uint64_t)pitch * (uint64_t)height;
    out->page_count = (s_intel.active_pipe >= 0 && s_intel.plane_linear &&
                       map_vram >= page_bytes * 2ull) ? 2 : 1;

    out->caps = 0;
    if (out->wc_active) out->caps |= GPU_CAP_WC;
    if (gpu_simd_caps() & GPU_SIMD_SSE2) out->caps |= GPU_CAP_SIMD;
    if (s_intel.active_pipe >= 0) out->caps |= GPU_CAP_VSYNC;
    if (out->page_count >= 2) out->caps |= GPU_CAP_PAGEFLIP;
    if (s_intel.bcs_ready) out->caps |= GPU_CAP_2D_BLT;

    const char *nm = "Intel UHD/Iris Xe";
    for (int i = 0; nm[i] && i < (int)sizeof(out->name) - 1; i++) out->name[i] = nm[i];
    for (int i = 0; gen_name[i] && i < (int)sizeof(out->gen_name) - 1; i++)
        out->gen_name[i] = gen_name[i];

    out->ops.flush_rect  = intel_flush_rect;
    out->ops.fill_rect   = intel_fill_rect;
    out->ops.blit_rect   = gpu_lfb_blit_rect;
    out->ops.alpha_blend = gpu_lfb_alpha_blend;
    out->ops.wait_vsync  = (s_intel.active_pipe >= 0) ? intel_wait_vsync : 0;
    out->ops.set_mode    = 0; /* Preserve firmware-trained eDP/HDMI/DP link safely */
    out->ops.page_flip   = (out->page_count >= 2) ? intel_page_flip : 0;
    out->priv            = &s_intel;
    out->active          = true;

    vesa_attach_external(out->fb_virt, out->vram_phys,
                         out->width, out->height, out->pitch, out->bpp);

    serial_printf("[gpu-intel] %s (0x8086:0x%04X) %dx%dx%d pipe=%c surf=0x%08X BCS=%s WC=%s\n",
                  out->gen_name, (unsigned)out->device_id,
                  out->width, out->height, out->bpp,
                  s_intel.active_pipe >= 0 ? (char)('A' + s_intel.active_pipe) : '-',
                  s_intel.base_surf_gtt,
                  s_intel.bcs_ready ? "ON" : "fallback",
                  out->wc_active ? "ON" : "OFF");
    return 0;
}
