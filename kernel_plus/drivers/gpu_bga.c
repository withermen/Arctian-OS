/**
 * Arctian Bochs / QEMU BGA GPU Backend (PCI 0x1234:0x1111)
 *
 * Programmatic DISPI I/O (0x01CE / 0x01CF) mode-setting up to 3840x2160x32,
 * double-buffer hardware page-flip via VBE_DISPI_INDEX_Y_OFFSET,
 * PAT Write-Combining (WC) LFB mapping, and SSE2/AVX2 streaming blits.
 */
#include "gpu.h"
#include "gpu_simd.h"
#include "vesa.h"
#include "bootinfo.h"
#include "paging.h"
#include "clock.h"
#include "kernel.h"
#include "serial.h"

#define VGA_INPUT_STATUS_1  0x03DAu
#define BGA_MAX_MAP_BYTES   (64ull * 1024ull * 1024ull)

static inline void bga_write(uint16_t idx, uint16_t val) {
    outw(VBE_DISPI_INDEX_PORT, idx);
    outw(VBE_DISPI_DATA_PORT, val);
}

static inline uint16_t bga_read(uint16_t idx) {
    outw(VBE_DISPI_INDEX_PORT, idx);
    return inw(VBE_DISPI_DATA_PORT);
}

static void bga_wait_vsync(gpu_drv_t *drv) {
    (void)drv;
    uint64_t deadline = clock_ms() + 20ull;
    uint32_t guard = 0;
    /* Wait until current vblank ends (if already inside vblank) */
    while ((inb(VGA_INPUT_STATUS_1) & 0x08u) != 0) {
        if (++guard > 200000u || clock_ms() >= deadline) return;
        __asm__ volatile("pause");
    }
    /* Wait for next vblank leading edge */
    guard = 0;
    while ((inb(VGA_INPUT_STATUS_1) & 0x08u) == 0) {
        if (++guard > 200000u || clock_ms() >= deadline) return;
        __asm__ volatile("pause");
    }
}

static int bga_page_flip(gpu_drv_t *drv, int buf_index) {
    if (!drv || drv->page_count < 2) return -1;
    int idx = (buf_index & 1);
    bga_wait_vsync(drv);
    bga_write(VBE_DISPI_INDEX_X_OFFSET, 0);
    bga_write(VBE_DISPI_INDEX_Y_OFFSET, (uint16_t)((uint32_t)idx * drv->page_stride_y));
    drv->front_page = idx;
    return 0;
}

static int bga_set_mode(gpu_drv_t *drv, int width, int height, int bpp) {
    if (!drv) return -1;
    if (bpp <= 0) bpp = 32;
    if (bpp != 32) return -1;
    if (width < 320 || height < 200 || width > 3840 || height > 2160) return -1;

    uint64_t frame_bytes = (uint64_t)width * (uint64_t)height * 4ull;
    if (drv->vram_size && frame_bytes > drv->vram_size) return -2;

    bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    bga_write(VBE_DISPI_INDEX_XRES, (uint16_t)width);
    bga_write(VBE_DISPI_INDEX_YRES, (uint16_t)height);
    bga_write(VBE_DISPI_INDEX_BPP, (uint16_t)bpp);
    bga_write(VBE_DISPI_INDEX_VIRT_WIDTH, (uint16_t)width);

    int pages = 1;
    if (drv->vram_size >= frame_bytes * 2ull && (uint32_t)height * 2u <= 4320u) {
        bga_write(VBE_DISPI_INDEX_VIRT_HEIGHT, (uint16_t)(height * 2));
        pages = 2;
    } else {
        bga_write(VBE_DISPI_INDEX_VIRT_HEIGHT, (uint16_t)height);
    }
    bga_write(VBE_DISPI_INDEX_X_OFFSET, 0);
    bga_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
    bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    uint16_t xr = bga_read(VBE_DISPI_INDEX_XRES);
    uint16_t yr = bga_read(VBE_DISPI_INDEX_YRES);
    if (xr != (uint16_t)width || yr != (uint16_t)height) {
        return -3;
    }

    uint64_t map_bytes = drv->vram_size ? drv->vram_size : (16ull * 1024ull * 1024ull);
    if (map_bytes < frame_bytes * (uint64_t)pages)
        map_bytes = frame_bytes * (uint64_t)pages;
    if (map_bytes > BGA_MAX_MAP_BYTES) map_bytes = BGA_MAX_MAP_BYTES;

    if (vmm_map_wc_range(drv->vram_phys, map_bytes) == 0) {
        drv->wc_active = vmm_pat_enabled() != 0;
        if (drv->wc_active) drv->caps |= GPU_CAP_WC;
    }

    drv->width = width;
    drv->height = height;
    drv->bpp = bpp;
    drv->pitch = width * 4;
    drv->fb_virt = (uint32_t *)(uintptr_t)drv->vram_phys;
    drv->page_count = pages;
    drv->front_page = 0;
    drv->page_stride_y = (uint32_t)height;
    if (pages >= 2) drv->caps |= GPU_CAP_PAGEFLIP;
    else drv->caps &= ~GPU_CAP_PAGEFLIP;

    vesa_attach_external(drv->fb_virt, drv->vram_phys,
                         drv->width, drv->height, drv->pitch, drv->bpp);
    return 0;
}

static void bga_flush_rect(gpu_drv_t *drv, int x, int y, int w, int h) {
    (void)drv; (void)x; (void)y; (void)w; (void)h;
    /* LFB writes are already flushed via sfence inside gpu_simd_copy_wc */
}

int gpu_bga_probe(const gpu_pci_info_t *info, gpu_drv_t *out,
                  int req_w, int req_h, int req_bpp) {
    if (!out) return -1;

    /* Verify DISPI register interface is present */
    bga_write(VBE_DISPI_INDEX_ID, 0xB0C5u);
    uint16_t id = bga_read(VBE_DISPI_INDEX_ID);
    if (id < 0xB0C0u || id > 0xB0C5u) return -1;

    uint64_t vram_phys = 0;
    uint64_t vram_size = 0;
    if (info) {
        pci_enable_bus_mastering(info->pci.bus, info->pci.slot, info->pci.func);
        if (!info->bar[0].is_io && info->bar[0].address != 0) {
            vram_phys = info->bar[0].address;
            vram_size = info->bar[0].size;
        }
    }
    if (vram_phys == 0) {
        const arctian_bootinfo_t *bi = arctian_bootinfo();
        if (arctian_bootinfo_valid(bi)) {
            vram_phys = (uint64_t)bi->fb_phys;
            if (bi->fb_size) vram_size = (uint64_t)bi->fb_size;
        }
    }
    if (vram_phys == 0) vram_phys = 0xFD000000ull;
    if (vram_size == 0) vram_size = 16ull * 1024ull * 1024ull;

    out->vendor_id = info ? info->pci.vendor : PCI_VENDOR_BOCHS;
    out->device_id = info ? info->pci.device : 0x1111u;
    out->bus       = info ? info->pci.bus : 0;
    out->slot      = info ? info->pci.slot : 0;
    out->func      = info ? info->pci.func : 0;
    out->rev_id    = info ? info->pci.rev_id : 0;
    out->vram_phys = vram_phys;
    out->vram_size = vram_size;
    out->mmio_phys = (info && !info->bar[2].is_io) ? info->bar[2].address : 0;
    out->mmio_size = (info && !info->bar[2].is_io) ? info->bar[2].size : 0;
    out->caps      = GPU_CAP_MODESET | GPU_CAP_VSYNC;
    if (gpu_simd_caps() & GPU_SIMD_SSE2) out->caps |= GPU_CAP_SIMD;

    const char *nm = "Bochs/QEMU BGA";
    for (int i = 0; nm[i] && i < (int)sizeof(out->name) - 1; i++) out->name[i] = nm[i];
    const char *gn = "DISPI VBE 0xB0C5";
    for (int i = 0; gn[i] && i < (int)sizeof(out->gen_name) - 1; i++) out->gen_name[i] = gn[i];

    out->ops.flush_rect  = bga_flush_rect;
    out->ops.fill_rect   = gpu_lfb_fill_rect;
    out->ops.blit_rect   = gpu_lfb_blit_rect;
    out->ops.alpha_blend = gpu_lfb_alpha_blend;
    out->ops.wait_vsync  = bga_wait_vsync;
    out->ops.set_mode    = bga_set_mode;
    out->ops.page_flip   = bga_page_flip;

    if (bga_set_mode(out, req_w, req_h, req_bpp) != 0) {
        /* Fallback to 1024x768 if requested mode didn't fit VRAM */
        if (bga_set_mode(out, 1024, 768, 32) != 0) return -2;
    }

    out->active = true;
    serial_printf("[gpu-bga] DISPI=0x%04X %dx%dx%d VRAM=0x%08X (%uMB) pages=%d WC=%s\n",
                  (unsigned)id, out->width, out->height, out->bpp,
                  (unsigned)out->vram_phys,
                  (unsigned)(out->vram_size / (1024ull * 1024ull)),
                  out->page_count, out->wc_active ? "ON" : "OFF");
    return 0;
}
