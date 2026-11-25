/**
 * Arctian VESA Framebuffer Driver
 * Bochs VBE interface - 64-bit AMD64
 * Port IO (0x01CE/0x01CF) - no real mode needed
 * Page tables: maps LFB at 0xFD000000 via 2MB huge pages
 */
#include "vesa.h"
#include "kernel.h"
#include "paging.h"
#include "gpu.h"
#include "gpu_simd.h"

// Bochs VBE register helpers
static inline void vbe_write(uint16_t index, uint16_t value) {
    outw(VBE_DISPI_INDEX_PORT, index);
    outw(VBE_DISPI_DATA_PORT, value);
}

static uint16_t vbe_read(uint16_t index) {
    outw(VBE_DISPI_INDEX_PORT, index);
    return inw(VBE_DISPI_DATA_PORT);
}

static framebuffer_t fb;

// Stage2 stores BIOS VBE mode info here before entering protected mode.
#define VESA_BOOT_INFO       0x7000
#define VESA_BOOT_MAGIC      0x41534556  // "VESA"

// Physical addresses for stage2 page tables (identity mapped in low memory)
#define PAGE_PDP    0x12000
#define PAGE_PD_FB  0x0D000  // New PD for framebuffer range

static uint64_t s_fb_map_base;
static volatile uint64_t *s_fb_map_pd;

// Map framebuffer physical memory using PAT Write-Combining 2MB huge pages
static void map_framebuffer_memory(uint32_t phys_addr) {
    /* Prefer VMM PAT Write-Combining mapping (works for BIOS, UEFI, and >4GB BARs
     * without clobbering unrelated entries in the 3..4GB page directory). */
    if (vmm_map_wc_range((uint64_t)phys_addr, 32ull * 1024ull * 1024ull) == 0) {
        kernel_debug("Framebuffer PAT WC 2MB sayfalar ile haritalandi");
        return;
    }

    int pd_index = (phys_addr >> 21) & 0x1FF;
    int pdp_index = (phys_addr >> 30) & 0x1FF;
    volatile uint64_t *pdp = (volatile uint64_t *)PAGE_PDP;
    volatile uint64_t *pd;
    if (pdp[pdp_index] & 1) {
        pd = (volatile uint64_t *)(uintptr_t)(pdp[pdp_index] & ~0xFFFULL);
    } else {
        pd = (volatile uint64_t *)PAGE_PD_FB;
        pdp[pdp_index] = (uint64_t)PAGE_PD_FB | 0x03;
    }

    if (!s_fb_map_pd) {
        for (int i = 0; i < 512; i++) pd[i] = 0;
    } else if (s_fb_map_pd == pd && s_fb_map_base) {
        int old_index = (int)((s_fb_map_base >> 21) & 0x1FF);
        for (int i = 0; i < 8 && old_index + i < 512; i++) pd[old_index + i] = 0;
    }

    if (pd_index < 512) {
        int count = 8;
        if (pd_index + count > 512) count = 512 - pd_index;
        uint64_t cf = vmm_pat_enabled() ? VMM_PTE_WC : 0ull;
        for (int i = 0; i < count; i++) {
            uint32_t page_addr = phys_addr + (uint32_t)i * 0x200000u;
            pd[pd_index + i] = (uint64_t)page_addr | 0x83ull | cf;
        }
    }
    s_fb_map_pd = pd;
    s_fb_map_base = phys_addr;

    asm volatile ("mov %%cr3, %%rax; mov %%rax, %%cr3" : : : "rax");
    kernel_debug("Framebuffer 2MB sayfalar ile haritalandi");
}

void vesa_attach_external(uint32_t *virt, uint64_t phys,
                          int width, int height, int pitch, int bpp) {
    fb.address      = virt;
    fb.phys_address = (uint32_t)(phys & 0xFFFFFFFFull);
    fb.width        = width;
    fb.height       = height;
    fb.pitch        = pitch;
    fb.bpp          = bpp;
    fb.enabled      = (virt != 0 && width > 0 && height > 0);
}

int vesa_init(int width, int height, int bpp) {
    kernel_debug("VESA baslatiliyor");

    /* stage2'nin VBIOS ile kurdugu mod bilgisi (varsa).
     * LFB fiziksel adresi buradan alinir (ekran kartinin gercek cikisi). */
    volatile uint8_t *boot_info = (volatile uint8_t *)VESA_BOOT_INFO;
    uint32_t magic = *(volatile uint32_t *)(boot_info + 0);

    uint32_t boot_phys = 0;
    int have_boot = 0;
    if (magic == VESA_BOOT_MAGIC) {
        have_boot = 1;
        boot_phys = *(volatile uint32_t *)(boot_info + 4);
        if (boot_phys == 0) boot_phys = 0xFD000000;
    }

    /* 1) Oncelik ekran kartinda: Bochs VBE arabirimi ile istenen modu kur.
     *    Boylece cozunurluk VBIOS mod listesine bagli kalmaz. */
    vbe_write(VBE_DISPI_INDEX_ID, 0xB0C5);
    uint16_t id = vbe_read(VBE_DISPI_INDEX_ID);
    if (id >= 0xB0C0 && id <= 0xB0C5) {
        vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
        vbe_write(VBE_DISPI_INDEX_XRES, (uint16_t)width);
        vbe_write(VBE_DISPI_INDEX_YRES, (uint16_t)height);
        vbe_write(VBE_DISPI_INDEX_BPP, (uint16_t)bpp);
        vbe_write(VBE_DISPI_INDEX_VIRT_WIDTH, (uint16_t)width);
        vbe_write(VBE_DISPI_INDEX_ENABLE,
                  VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

        uint16_t xres = vbe_read(VBE_DISPI_INDEX_XRES);
        uint16_t yres = vbe_read(VBE_DISPI_INDEX_YRES);
        if (xres == (uint16_t)width && yres == (uint16_t)height) {
            fb.width = width;
            fb.height = height;
            fb.bpp = bpp;
            fb.pitch = width * (bpp / 8);
            fb.phys_address = have_boot ? boot_phys : 0xFD000000;
            fb.enabled = true;

            map_framebuffer_memory(fb.phys_address);
            fb.address = (uint32_t *)(uintptr_t)fb.phys_address;
            kernel_debug("VESA Bochs VBE modu kuruldu");
            return 0;
        }

        /* Kurulamadi: ekran kartini VBIOS moduna geri dondur ve ona dus. */
        kernel_warning("Bochs VBE modu kurulamadi, VBIOS moduna dusuluyor");
        if (have_boot) {
            uint16_t bw = *(volatile uint16_t *)(boot_info + 12);
            uint16_t bh = *(volatile uint16_t *)(boot_info + 14);
            uint8_t bbp = *(volatile uint8_t *)(boot_info + 16);
            if (bw && bh) {
                vbe_write(VBE_DISPI_INDEX_XRES, bw);
                vbe_write(VBE_DISPI_INDEX_YRES, bh);
                vbe_write(VBE_DISPI_INDEX_BPP, bbp ? bbp : 32);
                vbe_write(VBE_DISPI_INDEX_VIRT_WIDTH, bw);
                vbe_write(VBE_DISPI_INDEX_ENABLE,
                          VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);
            }
        } else {
            vesa_disable();
        }
    }

    /* 2) Ekran karti Bochs VBE sunmuyorsa (gercek donanim) VBIOS modu. */
    if (have_boot) {
        uint16_t pitch = *(volatile uint16_t *)(boot_info + 8);
        uint16_t boot_width = *(volatile uint16_t *)(boot_info + 12);
        uint16_t boot_height = *(volatile uint16_t *)(boot_info + 14);
        uint8_t boot_bpp = *(volatile uint8_t *)(boot_info + 16);

        fb.width = boot_width ? boot_width : width;
        fb.height = boot_height ? boot_height : height;
        fb.bpp = boot_bpp ? boot_bpp : bpp;
        fb.pitch = pitch ? pitch : fb.width * (fb.bpp / 8);
        fb.phys_address = boot_phys;
        fb.enabled = true;

        map_framebuffer_memory(fb.phys_address);
        fb.address = (uint32_t *)(uintptr_t)fb.phys_address;

        kernel_debug("VESA VBIOS framebuffer kullaniliyor");
        return 0;
    }

    kernel_warning("Grafik modu kurulamadi");
    return -1;
}

void vesa_disable(void) {
    vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    fb.enabled = false;
    kernel_debug("VESA devre disi");
}

void vesa_set_refresh_rate(int hz) {
    if (hz < 1) return;
    vbe_write(VBE_DISPI_INDEX_REFRESH, (uint16_t)hz);
    kernel_debug("VESA yenileme hizi ayarlandi");
}

bool vesa_is_enabled(void) {
    return fb.enabled;
}

framebuffer_t* vesa_get_framebuffer(void) {
    return &fb;
}

void vesa_put_pixel(int x, int y, uint32_t color) {
    if (!fb.enabled || x < 0 || x >= fb.width || y < 0 || y >= fb.height)
        return;
    fb.address[y * (fb.pitch / 4) + x] = color;
}

uint32_t vesa_get_pixel(int x, int y) {
    if (!fb.enabled || x < 0 || x >= fb.width || y < 0 || y >= fb.height)
        return 0;
    return fb.address[y * (fb.pitch / 4) + x];
}

void vesa_clear(uint32_t color) {
    if (gpu_is_ready()) {
        gpu_drv_t *g = gpu_active();
        if (g) {
            gpu_fill_rect(0, 0, g->width, g->height, color);
            return;
        }
    }
    if (!fb.enabled || !fb.address) return;
    gpu_simd_fill32_wc(fb.address, color, (size_t)fb.width * (size_t)fb.height);
}

void vesa_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (gpu_is_ready()) {
        gpu_fill_rect(x, y, w, h, color);
        return;
    }
    if (!fb.enabled || !fb.address) return;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= fb.width || y >= fb.height) return;
    if (w > fb.width - x)  w = fb.width - x;
    if (h > fb.height - y) h = fb.height - y;
    if (w <= 0 || h <= 0) return;

    int pitch_dwords = fb.pitch / 4;
    for (int row = 0; row < h; row++) {
        gpu_simd_fill32_wc(fb.address + (size_t)(y + row) * (size_t)pitch_dwords + (size_t)x,
                           color, (size_t)w);
    }
}
