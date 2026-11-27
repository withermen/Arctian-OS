/**
 * Arctian Modern Graphics & GPU Subsystem Manager
 *
 * Scans PCI Display Class (0x03) devices, binds the best available hardware
 * backend (VirtIO-GPU 2D, Intel UHD/Iris Xe/Arc, NVIDIA GeForce RTX 20/30/40/50,
 * Bochs/QEMU BGA, or UEFI GOP / BIOS VBE LFB fallback), configures PAT WC
 * framebuffer mappings, dispatches SSE2/AVX2 blits, and runs a quick boot
 * throughput micro-benchmark.
 */
#include "gpu.h"
#include "gpu_simd.h"
#include "backbuffer.h"
#include "vesa.h"
#include "bootinfo.h"
#include "memory_map.h"
#include "paging.h"
#include "clock.h"
#include "kernel.h"
#include "serial.h"

static gpu_pci_info_t s_pci_gpus[GPU_MAX_DEVICES];
static int            s_pci_gpu_count = 0;
static gpu_drv_t      s_active_gpu;
static bool           s_gpu_ready = false;
static uint32_t       s_bench_mbps = 0;
static char           s_status_str[96];

static void str_copy(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    size_t i = 0;
    if (src) {
        while (src[i] && i + 1 < cap) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = 0;
}

static void str_cat(char *dst, size_t cap, const char *src) {
    if (!dst || !src || cap == 0) return;
    size_t i = 0;
    while (i < cap && dst[i]) i++;
    size_t j = 0;
    while (src[j] && i + 1 < cap) {
        dst[i++] = src[j++];
    }
    if (i < cap) dst[i] = 0;
}

/* ---------------- Shared WC + SIMD LFB Operations ---------------- */

void gpu_lfb_blit_rect(gpu_drv_t *drv, const uint32_t *src, int src_pitch_px,
                       int sx, int sy, int dx, int dy, int w, int h) {
    if (!drv || !drv->fb_virt || !src || w <= 0 || h <= 0) return;
    if (dx < 0) { sx -= dx; w += dx; dx = 0; }
    if (dy < 0) { sy -= dy; h += dy; dy = 0; }
    if (sx < 0) { dx -= sx; w += sx; sx = 0; }
    if (sy < 0) { dy -= sy; h += sy; sy = 0; }
    if (dx >= drv->width || dy >= drv->height) return;
    if (w > drv->width - dx)  w = drv->width - dx;
    if (h > drv->height - dy) h = drv->height - dy;
    if (w <= 0 || h <= 0) return;

    int fb_pitch_px = drv->pitch / 4;
    uint32_t page_off_px = (uint32_t)drv->front_page * drv->page_stride_y * (uint32_t)fb_pitch_px;
    uint32_t *fb_base = drv->fb_virt + page_off_px;

    /* Contiguous full-width fast path: single streaming transfer + single sfence */
    if (dx == 0 && sx == 0 && w == fb_pitch_px && w == src_pitch_px) {
        const uint32_t *sptr = src + (size_t)sy * (size_t)src_pitch_px;
        uint32_t *dptr = fb_base + (size_t)dy * (size_t)fb_pitch_px;
        gpu_simd_copy_wc(dptr, sptr, (size_t)w * (size_t)h * 4u);
        return;
    }

    size_t row_bytes = (size_t)w * 4u;
    for (int row = 0; row < h; row++) {
        const uint32_t *srow = src + (size_t)(sy + row) * (size_t)src_pitch_px + (size_t)sx;
        uint32_t *drow = fb_base + (size_t)(dy + row) * (size_t)fb_pitch_px + (size_t)dx;
        gpu_simd_copy_wc(drow, srow, row_bytes);
    }
}

void gpu_lfb_fill_rect(gpu_drv_t *drv, int x, int y, int w, int h, uint32_t color) {
    if (!drv || !drv->fb_virt || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= drv->width || y >= drv->height) return;
    if (w > drv->width - x)  w = drv->width - x;
    if (h > drv->height - y) h = drv->height - y;
    if (w <= 0 || h <= 0) return;

    int fb_pitch_px = drv->pitch / 4;
    uint32_t page_off_px = (uint32_t)drv->front_page * drv->page_stride_y * (uint32_t)fb_pitch_px;
    uint32_t *fb_base = drv->fb_virt + page_off_px;

    if (x == 0 && w == fb_pitch_px) {
        gpu_simd_fill32_wc(fb_base + (size_t)y * (size_t)fb_pitch_px,
                           color, (size_t)w * (size_t)h);
        return;
    }

    for (int row = 0; row < h; row++) {
        gpu_simd_fill32_wc(fb_base + (size_t)(y + row) * (size_t)fb_pitch_px + (size_t)x,
                           color, (size_t)w);
    }
}

void gpu_lfb_alpha_blend(gpu_drv_t *drv, const uint32_t *src, int src_pitch_px,
                         int sx, int sy, int dx, int dy, int w, int h) {
    if (!drv || !src || w <= 0 || h <= 0) return;
    if (dx < 0) { sx -= dx; w += dx; dx = 0; }
    if (dy < 0) { sy -= dy; h += dy; dy = 0; }
    if (sx < 0) { dx -= sx; w += sx; sx = 0; }
    if (sy < 0) { dy -= sy; h += sy; sy = 0; }
    if (dx >= drv->width || dy >= drv->height) return;
    if (w > drv->width - dx)  w = drv->width - dx;
    if (h > drv->height - dy) h = drv->height - dy;
    if (w <= 0 || h <= 0) return;

    /* Blend in cached backbuffer RAM (avoid slow uncached VRAM reads),
     * then stream the blended rect to VRAM/scanout. */
    uint32_t *bb = backbuffer_ptr();
    if (!bb) return;
    for (int row = 0; row < h; row++) {
        const uint32_t *srow = src + (size_t)(sy + row) * (size_t)src_pitch_px + (size_t)sx;
        uint32_t *drow = bb + (size_t)(dy + row) * (size_t)drv->width + (size_t)dx;
        gpu_simd_alpha_blend_row(drow, srow, (size_t)w);
    }
    if (drv->ops.blit_rect) {
        drv->ops.blit_rect(drv, bb, drv->width, dx, dy, dx, dy, w, h);
    }
}

/* ---------------- PCI Display Class (0x03) Scanner ---------------- */

static void classify_pci_gpu(gpu_pci_info_t *g) {
    uint16_t v = g->pci.vendor;
    uint16_t d = g->pci.device;

    if (v == PCI_VENDOR_VIRTIO && d == 0x1050u) {
        g->vendor_kind = GPU_VENDOR_VIRTIO;
        str_copy(g->vendor_name, sizeof(g->vendor_name), "VirtIO");
        str_copy(g->gen_name, sizeof(g->gen_name), "VirtIO-GPU 2D");
    } else if (v == PCI_VENDOR_INTEL) {
        g->vendor_kind = GPU_VENDOR_INTEL;
        str_copy(g->vendor_name, sizeof(g->vendor_name), "Intel");
        str_copy(g->gen_name, sizeof(g->gen_name), gpu_intel_classify(d, 0));
    } else if (v == PCI_VENDOR_NVIDIA) {
        g->vendor_kind = GPU_VENDOR_NVIDIA;
        str_copy(g->vendor_name, sizeof(g->vendor_name), "NVIDIA");
        str_copy(g->gen_name, sizeof(g->gen_name), gpu_nvidia_classify(d));
    } else if (v == PCI_VENDOR_BOCHS && d == 0x1111u) {
        g->vendor_kind = GPU_VENDOR_BOCHS;
        str_copy(g->vendor_name, sizeof(g->vendor_name), "Bochs/QEMU");
        str_copy(g->gen_name, sizeof(g->gen_name), "BGA VBE DISPI");
    } else if (v == 0x1002u || v == PCI_VENDOR_AMD) {
        g->vendor_kind = GPU_VENDOR_AMD;
        str_copy(g->vendor_name, sizeof(g->vendor_name), "AMD");
        str_copy(g->gen_name, sizeof(g->gen_name), "Radeon Display");
    } else if (v == 0x80EEu && d == 0xBEEFu) {
        g->vendor_kind = GPU_VENDOR_GENERIC;
        str_copy(g->vendor_name, sizeof(g->vendor_name), "VirtualBox");
        str_copy(g->gen_name, sizeof(g->gen_name), "VBoxVGA BGA");
    } else if (v == 0x15ADu && d == 0x0405u) {
        g->vendor_kind = GPU_VENDOR_GENERIC;
        str_copy(g->vendor_name, sizeof(g->vendor_name), "VMware/VBox");
        str_copy(g->gen_name, sizeof(g->gen_name), "VMSVGA / SVGA II");
    } else {
        g->vendor_kind = GPU_VENDOR_GENERIC;
        str_copy(g->vendor_name, sizeof(g->vendor_name), "Generic");
        str_copy(g->gen_name, sizeof(g->gen_name), "PCI Display");
    }
}

static void gpu_scan_pci(void) {
    s_pci_gpu_count = 0;
    for (int b = 0; b < 256; b++) {
        for (int s = 0; s < 32; s++) {
            uint16_t vendor0 = pci_config_readw((uint8_t)b, (uint8_t)s, 0, 0);
            if (vendor0 == 0xFFFFu) continue;

            uint8_t hdr = pci_config_readb((uint8_t)b, (uint8_t)s, 0, 0x0E);
            int max_f = (hdr & 0x80u) ? 8 : 1;

            for (int f = 0; f < max_f; f++) {
                uint16_t vendor = pci_config_readw((uint8_t)b, (uint8_t)s, (uint8_t)f, 0);
                if (vendor == 0xFFFFu) continue;

                uint32_t rc = pci_config_readl((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x08);
                uint8_t cc = (uint8_t)((rc >> 24) & 0xFFu);
                if (cc != PCI_CLASS_DISPLAY) continue;
                if (s_pci_gpu_count >= GPU_MAX_DEVICES) break;

                gpu_pci_info_t *g = &s_pci_gpus[s_pci_gpu_count];
                memset(g, 0, sizeof(*g));
                g->pci.bus        = (uint8_t)b;
                g->pci.slot       = (uint8_t)s;
                g->pci.func       = (uint8_t)f;
                g->pci.vendor     = vendor;
                g->pci.device     = pci_config_readw((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x02);
                g->pci.class_code = cc;
                g->pci.subclass   = (uint8_t)((rc >> 16) & 0xFFu);
                g->pci.prog_if    = (uint8_t)((rc >> 8) & 0xFFu);
                g->pci.rev_id     = (uint8_t)(rc & 0xFFu);
                g->pci.irq        = pci_config_readb((uint8_t)b, (uint8_t)s, (uint8_t)f, PCI_CONFIG_INTERRUPT);

                for (int bi = 0; bi < 6; bi++) {
                    g->bar[bi] = pci_read_bar((uint8_t)b, (uint8_t)s, (uint8_t)f, bi);
                    if (g->bar[bi].is_64bit && bi < 5) {
                        bi++;
                    }
                }

                classify_pci_gpu(g);
                serial_printf("[gpu] PCI #%d: %02X:%02X.%X %04X:%04X (%s - %s) BAR0=0x%X%08X(%uMB) BAR1=0x%X%08X(%uMB)\n",
                              s_pci_gpu_count,
                              (unsigned)g->pci.bus, (unsigned)g->pci.slot, (unsigned)g->pci.func,
                              (unsigned)g->pci.vendor, (unsigned)g->pci.device,
                              g->vendor_name, g->gen_name,
                              (unsigned)(g->bar[0].address >> 32), (unsigned)g->bar[0].address,
                              (unsigned)(g->bar[0].size / (1024ull * 1024ull)),
                              (unsigned)(g->bar[1].address >> 32), (unsigned)g->bar[1].address,
                              (unsigned)(g->bar[1].size / (1024ull * 1024ull)));
                s_pci_gpu_count++;
            }
        }
    }
}

/* ---------------- Generic VBE / UEFI GOP LFB Fallback ---------------- */

static void generic_flush_rect(gpu_drv_t *drv, int x, int y, int w, int h) {
    (void)drv; (void)x; (void)y; (void)w; (void)h;
}

static int gpu_probe_bootinfo_lfb(gpu_drv_t *out, const gpu_pci_info_t *pci_hint) {
    const arctian_bootinfo_t *bi = arctian_bootinfo();
    if (!arctian_bootinfo_valid(bi)) return -1;

    uint64_t fb_phys = (uint64_t)bi->fb_phys;
    int width  = (int)bi->width;
    int height = (int)bi->height;
    int bpp    = bi->bpp ? (int)bi->bpp : 32;
    int pitch  = bi->pitch ? (int)bi->pitch : (width * (bpp / 8));
    uint64_t sz = bi->fb_size ? (uint64_t)bi->fb_size
                              : (uint64_t)pitch * (uint64_t)height;

    if (arctian_bootinfo_is_uefi(bi)) {
        const volatile uint32_t *ui32 = (const volatile uint32_t *)(uintptr_t)0x7100u;
        if (ui32[0] == 0x49464555u && ui32[1] >= 0x60u) {
            uint64_t uefi_fb = *(const volatile uint64_t *)(uintptr_t)(0x7100u + 0x50u);
            uint64_t uefi_sz = *(const volatile uint64_t *)(uintptr_t)(0x7100u + 0x58u);
            if (uefi_fb != 0) fb_phys = uefi_fb;
            if (uefi_sz != 0) sz = uefi_sz;
        }
    }
    if (fb_phys == 0 || fb_phys == 0xFFFFFFFFull) return -1;
    if (sz < 16ull * 1024ull * 1024ull) sz = 16ull * 1024ull * 1024ull;

    if (vmm_map_wc_range(fb_phys, sz) != 0) return -2;

    serial_printf("[gpu] bootinfo LFB: phys=0x%X%08X size=%uMB uefi=%d\n",
                  (unsigned)(fb_phys >> 32), (unsigned)fb_phys,
                  (unsigned)(sz / (1024ull * 1024ull)),
                  arctian_bootinfo_is_uefi(bi) ? 1 : 0);

    out->vendor_id  = pci_hint ? pci_hint->pci.vendor : 0;
    out->device_id  = pci_hint ? pci_hint->pci.device : 0;
    out->vram_phys  = fb_phys;
    out->vram_size  = sz;
    out->fb_virt    = (uint32_t *)(uintptr_t)fb_phys;
    out->width      = width;
    out->height     = height;
    out->pitch      = pitch;
    out->bpp        = bpp;
    out->wc_active  = vmm_pat_enabled() != 0;
    out->page_count = 1;
    out->front_page = 0;
    out->page_stride_y = (uint32_t)height;
    out->caps = 0;
    if (out->wc_active) out->caps |= GPU_CAP_WC;
    if (gpu_simd_caps() & GPU_SIMD_SSE2) out->caps |= GPU_CAP_SIMD;

    const char *src_name = arctian_bootinfo_is_uefi(bi) ? "UEFI GOP LFB" : "BIOS VBE LFB";
    str_copy(out->name, sizeof(out->name), src_name);
    str_copy(out->gen_name, sizeof(out->gen_name),
             pci_hint ? pci_hint->gen_name : src_name);

    out->ops.flush_rect  = generic_flush_rect;
    out->ops.fill_rect   = gpu_lfb_fill_rect;
    out->ops.blit_rect   = gpu_lfb_blit_rect;
    out->ops.alpha_blend = gpu_lfb_alpha_blend;
    out->active          = true;

    vesa_attach_external(out->fb_virt, out->vram_phys,
                         out->width, out->height, out->pitch, out->bpp);
    return 0;
}

/* ---------------- Boot Blit Micro-Benchmark ---------------- */

static uint32_t gpu_run_bench(gpu_drv_t *drv) {
    if (!drv || !drv->active || drv->width <= 0 || drv->height <= 0) return 0;

    int bw = drv->width;
    int bh = drv->height;
    uint64_t frame_bytes = (uint64_t)bw * (uint64_t)bh * 4ull;
    if (frame_bytes == 0 || frame_bytes > BACKBUFFER_MAX) return 0;

    uint32_t *bb = backbuffer_ptr();
    if (!bb) return 0;
    if (bb == (uint32_t *)(uintptr_t)BACKBUFFER_BASE && frame_bytes > BACKBUFFER_MAX) return 0;
    gpu_simd_fill32(bb, 0x0E0B12u, (size_t)bw * (size_t)bh);

    uint64_t t0 = clock_ms();
    uint32_t iters = 0;
    uint64_t total_bytes = 0;

    /* Run at least 4 frames or up to ~20ms so throughput is measured accurately
     * without delaying boot. */
    while (iters < 32u) {
        if (drv->caps & GPU_CAP_VIRTIO_2D) {
            /* Measure SIMD backbuffer fill+copy throughput + one real VirtIO flush */
            gpu_simd_fill32(bb, 0x0E0B12u, (size_t)bw * (size_t)bh);
            if (iters == 0 && drv->ops.flush_rect)
                drv->ops.flush_rect(drv, 0, 0, bw, bh);
        } else if (drv->ops.blit_rect) {
            drv->ops.blit_rect(drv, bb, bw, 0, 0, 0, 0, bw, bh);
        }
        iters++;
        total_bytes += frame_bytes;
        uint64_t dt = clock_ms() - t0;
        if (iters >= 4u && dt >= 15ull) break;
    }

    uint64_t dt = clock_ms() - t0;
    if (dt == 0) dt = 1ull; /* sub-millisecond completion */

    /* MB/s = (total_bytes * 1000) / (dt_ms * 1048576) */
    uint64_t mbps = (total_bytes * 1000ull) / (dt * 1024ull * 1024ull);
    if (mbps == 0 && total_bytes > 0) mbps = 1ull;
    return (uint32_t)mbps;
}

/* ---------------- Public Subsystem API ---------------- */

static void gpu_update_status_string(void) {
    s_status_str[0] = 0;
    if (!s_gpu_ready) {
        str_copy(s_status_str, sizeof(s_status_str), "Ekran (yok)");
        return;
    }
    str_copy(s_status_str, sizeof(s_status_str), "GPU - ");
    str_cat(s_status_str, sizeof(s_status_str), s_active_gpu.name);
    str_cat(s_status_str, sizeof(s_status_str), " (");
    str_cat(s_status_str, sizeof(s_status_str), s_active_gpu.wc_active ? "WC+" : "");
    str_cat(s_status_str, sizeof(s_status_str), gpu_simd_level_name());
    str_cat(s_status_str, sizeof(s_status_str), ")");
}

int gpu_init(int req_width, int req_height, int req_bpp) {
    memset(&s_active_gpu, 0, sizeof(s_active_gpu));
    s_gpu_ready = false;

    /* 1) Initialize CPU SIMD (SSE2/SSSE3/AVX/AVX2) & verify PAT */
    gpu_simd_init();
    if (!vmm_pat_enabled()) {
        (void)vmm_pat_init_cpu();
    }

    /* 2) Scan PCI Display Class (0x03) devices */
    gpu_scan_pci();

    /* 3) Probe backends in priority order:
     *    VirtIO-GPU 2D > Intel UHD/Iris Xe > NVIDIA RTX > Bochs BGA > VBE/GOP LFB */
    int bound_idx = -1;

    /* 3a) VirtIO-GPU 2D (0x1AF4:0x1050) */
    for (int i = 0; i < s_pci_gpu_count && !s_gpu_ready; i++) {
        if (s_pci_gpus[i].vendor_kind == GPU_VENDOR_VIRTIO) {
            if (gpu_virtio_probe(&s_pci_gpus[i], &s_active_gpu,
                                 req_width, req_height, req_bpp) == 0) {
                s_gpu_ready = true;
                bound_idx = i;
            }
        }
    }

    /* 3b) Intel UHD / Iris Xe / Arc (0x8086) */
    for (int i = 0; i < s_pci_gpu_count && !s_gpu_ready; i++) {
        if (s_pci_gpus[i].vendor_kind == GPU_VENDOR_INTEL) {
            if (gpu_intel_probe(&s_pci_gpus[i], &s_active_gpu,
                                req_width, req_height, req_bpp) == 0) {
                s_gpu_ready = true;
                bound_idx = i;
            }
        }
    }

    /* 3c) NVIDIA GeForce RTX 20/30/40/50 (0x10DE) */
    for (int i = 0; i < s_pci_gpu_count && !s_gpu_ready; i++) {
        if (s_pci_gpus[i].vendor_kind == GPU_VENDOR_NVIDIA) {
            if (gpu_nvidia_probe(&s_pci_gpus[i], &s_active_gpu,
                                 req_width, req_height, req_bpp) == 0) {
                s_gpu_ready = true;
                bound_idx = i;
            }
        }
    }

    /* 3d) Bochs / QEMU BGA (0x1234:0x1111) */
    for (int i = 0; i < s_pci_gpu_count && !s_gpu_ready; i++) {
        if (s_pci_gpus[i].vendor_kind == GPU_VENDOR_BOCHS) {
            if (gpu_bga_probe(&s_pci_gpus[i], &s_active_gpu,
                              req_width, req_height, req_bpp) == 0) {
                s_gpu_ready = true;
                bound_idx = i;
            }
        }
    }

    /* 3e) VirtualBox (0x80EE:0xBEEF) / VMware VMSVGA (0x15AD:0x0405) / Direct DISPI I/O fallback */
    if (!s_gpu_ready) {
        const gpu_pci_info_t *bga_hint = 0;
        int bga_hint_idx = -1;
        for (int i = 0; i < s_pci_gpu_count; i++) {
            uint16_t v = s_pci_gpus[i].pci.vendor;
            uint16_t d = s_pci_gpus[i].pci.device;
            if ((v == 0x80EEu && d == 0xBEEFu) ||
                (v == 0x15ADu && d == 0x0405u) ||
                v == PCI_VENDOR_BOCHS) {
                bga_hint = &s_pci_gpus[i];
                bga_hint_idx = i;
                break;
            }
        }
        if (gpu_bga_probe(bga_hint, &s_active_gpu, req_width, req_height, req_bpp) == 0) {
            s_gpu_ready = true;
            if (bga_hint_idx >= 0) bound_idx = bga_hint_idx;
        }
    }

    /* 3f) Generic BIOS VBE / UEFI GOP LFB fallback */
    if (!s_gpu_ready) {
        const gpu_pci_info_t *hint = (s_pci_gpu_count > 0) ? &s_pci_gpus[0] : 0;
        if (gpu_probe_bootinfo_lfb(&s_active_gpu, hint) == 0) {
            s_gpu_ready = true;
            if (hint) bound_idx = 0;
        }
    }

    if (!s_gpu_ready) {
        serial_puts("[gpu] no display adapter initialized\n");
        gpu_update_status_string();
        return -1;
    }

    if (bound_idx >= 0) s_pci_gpus[bound_idx].active = true;

    s_bench_mbps = gpu_run_bench(&s_active_gpu);
    gpu_update_status_string();

    serial_printf("[gpu] active=%s (%s) %dx%dx%d WC=%s SIMD=%s caps=0x%02X blit=%u MB/s (gpus=%d)\n",
                  s_active_gpu.name, s_active_gpu.gen_name,
                  s_active_gpu.width, s_active_gpu.height, s_active_gpu.bpp,
                  s_active_gpu.wc_active ? "ON" : "OFF",
                  gpu_simd_level_name(),
                  (unsigned)s_active_gpu.caps,
                  (unsigned)s_bench_mbps,
                  s_pci_gpu_count);
    return 0;
}

bool gpu_is_ready(void) {
    return s_gpu_ready;
}

gpu_drv_t *gpu_active(void) {
    return s_gpu_ready ? &s_active_gpu : 0;
}

int gpu_device_count(void) {
    return s_pci_gpu_count;
}

const gpu_pci_info_t *gpu_device_info(int idx) {
    if (idx < 0 || idx >= s_pci_gpu_count) return 0;
    return &s_pci_gpus[idx];
}

const char *gpu_status_name(void) {
    if (!s_status_str[0]) gpu_update_status_string();
    return s_status_str;
}

uint32_t gpu_bench_mbps(void) {
    return s_bench_mbps;
}

void gpu_present_backbuffer(const uint32_t *bb, int bb_w, int bb_h,
                            int x, int y, int w, int h) {
    if (!s_gpu_ready || !bb || w <= 0 || h <= 0) return;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= bb_w || y >= bb_h) return;
    if (w > bb_w - x) w = bb_w - x;
    if (h > bb_h - y) h = bb_h - y;
    if (w > s_active_gpu.width - x)  w = s_active_gpu.width - x;
    if (h > s_active_gpu.height - y) h = s_active_gpu.height - y;
    if (w <= 0 || h <= 0) return;

    if (s_active_gpu.ops.blit_rect) {
        s_active_gpu.ops.blit_rect(&s_active_gpu, bb, bb_w, x, y, x, y, w, h);
    }
    if (s_active_gpu.ops.flush_rect && !(s_active_gpu.caps & GPU_CAP_VIRTIO_2D)) {
        /* Note: VirtIO blit_rect already performs TRANSFER_TO_HOST_2D + RESOURCE_FLUSH */
        s_active_gpu.ops.flush_rect(&s_active_gpu, x, y, w, h);
    }
}

void gpu_flush_rect(int x, int y, int w, int h) {
    if (!s_gpu_ready || !s_active_gpu.ops.flush_rect) return;
    s_active_gpu.ops.flush_rect(&s_active_gpu, x, y, w, h);
}

void gpu_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!s_gpu_ready || !s_active_gpu.ops.fill_rect) return;
    s_active_gpu.ops.fill_rect(&s_active_gpu, x, y, w, h, color);
}

void gpu_alpha_blend_rect(const uint32_t *src, int src_pitch_px,
                          int sx, int sy, int dx, int dy, int w, int h) {
    if (!s_gpu_ready || !s_active_gpu.ops.alpha_blend) return;
    s_active_gpu.ops.alpha_blend(&s_active_gpu, src, src_pitch_px,
                                 sx, sy, dx, dy, w, h);
}

void gpu_wait_vsync(void) {
    if (!s_gpu_ready || !s_active_gpu.ops.wait_vsync) return;
    s_active_gpu.ops.wait_vsync(&s_active_gpu);
}

int gpu_set_mode(int width, int height, int bpp) {
    if (!s_gpu_ready) return -1;
    if (s_active_gpu.ops.set_mode) {
        int rc = s_active_gpu.ops.set_mode(&s_active_gpu, width, height, bpp);
        if (rc == 0) {
            gpu_update_status_string();
            serial_printf("[gpu] mode -> %dx%dx%d WC=%s\n",
                          s_active_gpu.width, s_active_gpu.height, s_active_gpu.bpp,
                          s_active_gpu.wc_active ? "ON" : "OFF");
        }
        return rc;
    }
    /* Backend without runtime modeset (firmware-fixed display): keep active mode */
    return (width == s_active_gpu.width && height == s_active_gpu.height) ? 0 : -2;
}

void gpu_vsync_flip(void) {
    if (!s_gpu_ready) return;
    gpu_drv_t *drv = &s_active_gpu;
    uint32_t *bb = backbuffer_ptr();
    int bw = backbuffer_width();
    int bh = backbuffer_height();
    if (!bb || bw <= 0 || bh <= 0) return;

    if (drv->caps & GPU_CAP_VIRTIO_2D) {
        if (drv->ops.page_flip) (void)drv->ops.page_flip(drv, 0);
        else gpu_present_backbuffer(bb, bw, bh, 0, 0, bw, bh);
        return;
    }

    if (drv->page_count >= 2 && drv->ops.page_flip && drv->fb_virt) {
        int next_page = drv->front_page ^ 1;
        int fb_pitch_px = drv->pitch / 4;
        int cw = (bw < drv->width) ? bw : drv->width;
        int ch = (bh < drv->height) ? bh : drv->height;
        uint32_t *dst_base = drv->fb_virt +
                             (uint32_t)next_page * drv->page_stride_y * (uint32_t)fb_pitch_px;
        if (cw == bw && cw == fb_pitch_px) {
            gpu_simd_copy_wc(dst_base, bb, (size_t)cw * (size_t)ch * 4u);
        } else {
            size_t row_bytes = (size_t)cw * 4u;
            for (int row = 0; row < ch; row++) {
                gpu_simd_copy_wc(dst_base + (size_t)row * (size_t)fb_pitch_px,
                                 bb + (size_t)row * (size_t)bw, row_bytes);
            }
        }
        (void)drv->ops.page_flip(drv, next_page);
        return;
    }

    /* Tek sayfa / UEFI GOP yedegi: dikey tarama senkronu + tam kare sunumu */
    if (drv->ops.wait_vsync) drv->ops.wait_vsync(drv);
    gpu_present_backbuffer(bb, bw, bh, 0, 0, bw, bh);
}

int gpu_page_flip(int buf_index) {
    if (!s_gpu_ready) return -1;
    if (buf_index < 0) {
        gpu_vsync_flip();
        return 0;
    }
    if (!s_active_gpu.ops.page_flip) {
        if (s_active_gpu.ops.wait_vsync) s_active_gpu.ops.wait_vsync(&s_active_gpu);
        return 0;
    }
    return s_active_gpu.ops.page_flip(&s_active_gpu, buf_index);
}

int gpu_list_modes(gpu_mode_t *out, int max) {
    static const gpu_mode_t std_modes[] = {
        { 1024,  768, 32 },
        { 1280,  720, 32 },
        { 1280,  800, 32 },
        { 1440,  900, 32 },
        { 1600,  900, 32 },
        { 1920, 1080, 32 },
        { 2560, 1440, 32 },
        { 3840, 2160, 32 },
    };
    if (!out || max <= 0) return 0;

    int n = 0;
    if (!s_gpu_ready || !(s_active_gpu.caps & GPU_CAP_MODESET)) {
        if (s_gpu_ready) {
            out[0].width  = s_active_gpu.width;
            out[0].height = s_active_gpu.height;
            out[0].bpp    = s_active_gpu.bpp;
            return 1;
        }
        return 0;
    }

    for (size_t i = 0; i < sizeof(std_modes) / sizeof(std_modes[0]) && n < max; i++) {
        uint64_t bytes = (uint64_t)std_modes[i].width * (uint64_t)std_modes[i].height * 4ull;
        if (s_active_gpu.vram_size && bytes > s_active_gpu.vram_size) continue;
        if ((s_active_gpu.caps & GPU_CAP_VIRTIO_2D) && bytes > BACKBUFFER_MAX) continue;
        out[n++] = std_modes[i];
    }
    return n;
}
