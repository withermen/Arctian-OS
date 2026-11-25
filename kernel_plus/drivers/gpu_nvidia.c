/**
 * Arctian NVIDIA GeForce RTX 20/30/40/50 GPU Backend (PCI Vendor 0x10DE)
 *
 * Classifies Turing (RTX 20 / GTX 16), Ampere (RTX 30), Ada Lovelace (RTX 40),
 * Blackwell (RTX 50), and older Pascal/Maxwell/Kepler GeForce GPUs.
 *
 * Safe hardware bring-up:
 *   - Enables PCI Memory Space + Bus Master
 *   - Maps BAR0 MMIO as UC- and reads NV_PMC_BOOT_0 (+0x000000) read-only to
 *     identify real silicon architecture & stepping (never pokes signed
 *     GSP/Falcon registers that could hang real RTX cards!)
 *   - Maps BAR1 / firmware active framebuffer with PAT Write-Combining (WC)
 *   - Accelerates all 2D blits/fills/blends via SSE2/AVX2 streaming stores.
 */
#include "gpu.h"
#include "gpu_simd.h"
#include "vesa.h"
#include "bootinfo.h"
#include "paging.h"
#include "kernel.h"
#include "serial.h"

#define NV_PMC_BOOT_0       0x000000u
#define NV_MMIO_MAP_BYTES   (2ull * 1024ull * 1024ull)
#define NV_VRAM_MAP_CAP     (64ull * 1024ull * 1024ull)

typedef struct {
    volatile uint8_t *mmio;
    uint32_t          pmc_boot0;
    uint32_t          chipset;
    uint8_t           stepping;
} nv_gpu_priv_t;

static nv_gpu_priv_t s_nv;

const char *gpu_nvidia_classify(uint16_t dev_id) {
    if (dev_id >= 0x2900u && dev_id <= 0x2FFFu) {
        return "Blackwell (GeForce RTX 50)";
    }
    if (dev_id >= 0x2600u && dev_id <= 0x28FFu) {
        return "Ada Lovelace (GeForce RTX 40)";
    }
    if (dev_id >= 0x2200u && dev_id <= 0x25FFu) {
        return "Ampere (GeForce RTX 30)";
    }
    if (dev_id >= 0x1E00u && dev_id <= 0x21FFu) {
        return "Turing (GeForce RTX 20 / GTX 16)";
    }
    if (dev_id >= 0x1B00u && dev_id <= 0x1DFFu) {
        return "Pascal (GeForce GTX 10)";
    }
    if (dev_id >= 0x1300u && dev_id <= 0x17FFu) {
        return "Maxwell (GeForce GTX 900/750)";
    }
    if (dev_id >= 0x0FC0u && dev_id <= 0x12FFu) {
        return "Kepler (GeForce GTX 600/700)";
    }
    return "NVIDIA GeForce";
}

static const char *nv_arch_from_boot0(uint32_t boot0) {
    uint32_t arch = (boot0 >> 20) & 0x1FFu;
    if (arch >= 0x1B0u && arch <= 0x1BFu) return "GB20x Blackwell";
    if (arch >= 0x190u && arch <= 0x19Fu) return "AD10x Ada Lovelace";
    if (arch >= 0x170u && arch <= 0x17Fu) return "GA10x Ampere";
    if (arch >= 0x160u && arch <= 0x16Fu) return "TU10x Turing";
    if (arch >= 0x130u && arch <= 0x13Fu) return "GP10x Pascal";
    if (arch >= 0x110u && arch <= 0x12Fu) return "GM10x/GM20x Maxwell";
    if (arch >= 0x0E0u && arch <= 0x10Fu) return "GK10x/GK20x Kepler";
    return 0;
}

static void nv_flush_rect(gpu_drv_t *drv, int x, int y, int w, int h) {
    (void)drv; (void)x; (void)y; (void)w; (void)h;
}

int gpu_nvidia_probe(const gpu_pci_info_t *info, gpu_drv_t *out,
                     int req_w, int req_h, int req_bpp) {
    (void)req_bpp;
    if (!info || !out) return -1;
    if (info->pci.vendor != PCI_VENDOR_NVIDIA) return -1;

    pci_enable_bus_mastering(info->pci.bus, info->pci.slot, info->pci.func);
    memset(&s_nv, 0, sizeof(s_nv));

    /* BAR0 = MMIO registers (32-bit, 16MB), BAR1 = VRAM aperture (64-bit pref) */
    uint64_t mmio_phys = (!info->bar[0].is_io) ? info->bar[0].address : 0;
    uint64_t mmio_size = (!info->bar[0].is_io) ? info->bar[0].size : 0;
    uint64_t bar1_phys = (!info->bar[1].is_io) ? info->bar[1].address : 0;
    uint64_t bar1_size = (!info->bar[1].is_io) ? info->bar[1].size : 0;

    if (mmio_phys != 0) {
        if (vmm_map_mmio_range(mmio_phys, NV_MMIO_MAP_BYTES) == 0) {
            s_nv.mmio = (volatile uint8_t *)(uintptr_t)mmio_phys;
            uint32_t boot0 = *(volatile const uint32_t *)(s_nv.mmio + NV_PMC_BOOT_0);
            if (boot0 != 0xFFFFFFFFu && boot0 != 0u) {
                s_nv.pmc_boot0 = boot0;
                s_nv.chipset   = (boot0 >> 20) & 0x1FFu;
                s_nv.stepping  = (uint8_t)(boot0 & 0xFFu);
            }
        }
    }

    const arctian_bootinfo_t *bi = arctian_bootinfo();
    uint64_t fb_phys = bar1_phys;
    int width = req_w, height = req_h, pitch = req_w * 4, bpp = 32;
    uint64_t vram_size = bar1_size;

    if (arctian_bootinfo_valid(bi)) {
        fb_phys = (uint64_t)bi->fb_phys;
        width   = (int)bi->width;
        height  = (int)bi->height;
        pitch   = bi->pitch ? (int)bi->pitch : (width * 4);
        bpp     = bi->bpp ? (int)bi->bpp : 32;
        if (vram_size == 0 && bi->fb_size) vram_size = bi->fb_size;
    }

    if (fb_phys == 0) return -2;
    if (vram_size == 0) vram_size = 64ull * 1024ull * 1024ull;

    uint64_t map_vram = vram_size;
    if (map_vram > NV_VRAM_MAP_CAP) map_vram = NV_VRAM_MAP_CAP;
    if (map_vram < (uint64_t)pitch * (uint64_t)height)
        map_vram = (uint64_t)pitch * (uint64_t)height;

    if (vmm_map_wc_range(fb_phys, map_vram) != 0) return -3;

    const char *gen_name = 0;
    if (s_nv.pmc_boot0 != 0) gen_name = nv_arch_from_boot0(s_nv.pmc_boot0);
    if (!gen_name) gen_name = gpu_nvidia_classify(info->pci.device);

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
    out->mmio_virt  = (uint64_t)(uintptr_t)s_nv.mmio;
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

    const char *nm = "NVIDIA GeForce RTX";
    for (int i = 0; nm[i] && i < (int)sizeof(out->name) - 1; i++) out->name[i] = nm[i];
    for (int i = 0; gen_name[i] && i < (int)sizeof(out->gen_name) - 1; i++)
        out->gen_name[i] = gen_name[i];

    out->ops.flush_rect  = nv_flush_rect;
    out->ops.fill_rect   = gpu_lfb_fill_rect;
    out->ops.blit_rect   = gpu_lfb_blit_rect;
    out->ops.alpha_blend = gpu_lfb_alpha_blend;
    out->ops.wait_vsync  = 0;
    out->ops.set_mode    = 0;
    out->ops.page_flip   = 0;
    out->priv            = &s_nv;
    out->active          = true;

    vesa_attach_external(out->fb_virt, out->vram_phys,
                         out->width, out->height, out->pitch, out->bpp);

    serial_printf("[gpu-nvidia] %s (0x10DE:0x%04X) PMC_BOOT_0=0x%08X %dx%dx%d BAR1=%uMB WC=%s\n",
                  out->gen_name, (unsigned)out->device_id,
                  s_nv.pmc_boot0, out->width, out->height, out->bpp,
                  (unsigned)(out->vram_size / (1024ull * 1024ull)),
                  out->wc_active ? "ON" : "OFF");
    return 0;
}
