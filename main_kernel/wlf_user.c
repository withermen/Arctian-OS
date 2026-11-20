/**
 * Arctian - .wlf native ikili yukleyici (kullanici adres alani)
 * ============================================================
 *
 * Duz (flat) WLF_KIND_NATIVE imajini ayri bir PML4'te kullanici sanal
 * adres araligina yukler. Sayfa duzeni:
 *
 *   [ kod  : image_size, X (yazilamaz) ]
 *   [ veri : bss_size,   NX + W       ]
 *   [ yigin: 16 KB,      NX + W       ]
 *
 * Kaynak .wlf imaji cekirdek baglaminda (kimlik eslemeli) okunur; hedef
 * sayfalar vmm_alloc_frame ile ayrilir ve fiziksel adresleri kimlik
 * eslemeli oldugundan dogrudan kopyalanabilir. W^X: kod sayfalari W
 * biti olmadan, veri/yigin NX ile eslenir. NX yalnizca IA32_EFER.NXE=1
 * iken yazilir.
 */
#include "wlf_user.h"
#include "vmm_space.h"
#include "alloc.h"
#include <stddef.h>

extern void *memcpy(void *dst, const void *src, size_t n);
extern void *memset(void *s, int c, size_t n);

#define WLF_PAGE_SIZE   0x1000ull
#define WLF_CODE_PAGES(size)  (((uint64_t)(size) + WLF_PAGE_SIZE - 1ull) / WLF_PAGE_SIZE)

/* Basligi dogrular. 0 = gecerli, aksi halde WLF_USER_ERR_*. */
static int wlf_header_check(const wlf_header_t *h, uint64_t size) {
    if (h->magic != WLF_MAGIC) return WLF_USER_ERR_HEADER;
    if (h->version != WLF_VERSION) return WLF_USER_ERR_HEADER;
    if (h->kind != WLF_KIND_NATIVE) return WLF_USER_ERR_HEADER;
    if (h->image_size == 0) return WLF_USER_ERR_HEADER;

    /* Baslik + payload dosyanin icinde olmali (tasma denetimi). */
    if (h->payload_off < WLF_HEADER_SIZE) return WLF_USER_ERR_HEADER;
    if ((uint64_t)h->payload_off + (uint64_t)h->payload_len > size)
        return WLF_USER_ERR_RANGE;
    if ((uint64_t)h->payload_off + (uint64_t)h->image_size > size)
        return WLF_USER_ERR_RANGE;
    if (h->image_size > h->payload_len) return WLF_USER_ERR_RANGE;

    /* Giris noktasi kod araliginda olmali. */
    if (h->entry_off >= h->image_size) return WLF_USER_ERR_ENTRY;

    return WLF_USER_OK;
}

/* Yalnizca eslenmemis cerceveleri serbest birakir; eslenmis olanlari
 * vmm_space_destroy zaten geri verir (cift serbest birakma olmaz). */
static void wlf_free_frames(wlf_user_t *out, uint32_t mapped) {
    uint32_t i;

    if (!out->frames) return;
    for (i = mapped; i < out->frame_count; i++) {
        if (out->frames[i]) vmm_free_frame(out->frames[i]);
    }
    kfree(out->frames);
    out->frames = NULL;
    out->frame_count = 0;
}

uint64_t wlf_user_start(const void *image, uint64_t size, wlf_user_t *out) {
    wlf_header_t hdr;
    uint64_t code_pages, data_pages, stack_pages, total_pages, total_size;
    uint64_t base_va, data_va, stack_va, stack_top, va;
    uint64_t flags_data, nx;
    uint32_t mapped = 0;
    uint32_t i;
    int rc;

    if (out) {
        memset(out, 0, sizeof(*out));
        out->magic = WLF_USER_MAGIC;
    }
    if (!image || !out || size < (uint64_t)WLF_HEADER_SIZE) {
        if (out) out->status = WLF_USER_ERR_ARG;
        return 0;
    }

    /* Baslik packed; hizasiz isaretciden guvenli kopya. */
    memcpy(&hdr, image, sizeof(hdr));

    rc = wlf_header_check(&hdr, size);
    if (rc != WLF_USER_OK) {
        out->status = rc;
        return 0;
    }

    code_pages  = WLF_CODE_PAGES(hdr.image_size);
    data_pages  = WLF_CODE_PAGES(hdr.bss_size);
    stack_pages = (uint64_t)WLF_USER_STACK_SIZE / WLF_PAGE_SIZE;
    total_pages = code_pages + data_pages + stack_pages;

    /* Tasma / aralik denetimi: toplam alan kullanici VA'sina sigmali. */
    if (total_pages == 0 || total_pages > 0xFFFFFFFFull) {
        out->status = WLF_USER_ERR_TOO_BIG;
        return 0;
    }
    total_size = total_pages * WLF_PAGE_SIZE;
    if (total_size < total_pages || !vmm_space_user_range(VMM_USER_BASE,
                                                          total_size)) {
        out->status = WLF_USER_ERR_TOO_BIG;
        return 0;
    }

    base_va  = (uint64_t)VMM_USER_BASE;
    data_va  = base_va + code_pages * WLF_PAGE_SIZE;
    stack_va = data_va + data_pages * WLF_PAGE_SIZE;
    stack_top = stack_va + stack_pages * WLF_PAGE_SIZE;

    /* Adres alani. Basarisizlikta cerceve ayrilmadan cikilir. */
    out->space = (uint64_t)vmm_space_create();
    if (!out->space) {
        out->status = WLF_USER_ERR_NO_SPACE;
        return 0;
    }

    out->frames = (uint64_t *)kmalloc((size_t)total_pages * sizeof(uint64_t));
    if (!out->frames) {
        out->status = WLF_USER_ERR_NO_MEM;
        goto fail;
    }
    out->frame_count = 0;

    nx = vmm_space_nx_ok() ? VMM_PTE_NX : 0ull;
    flags_data = VMM_PTE_W | nx;              /* veri + yigin: W, NX */

    for (i = 0; i < (uint32_t)total_pages; i++) {
        uint64_t pa = vmm_alloc_frame();
        uint64_t flags;

        if (!pa) {
            out->status = WLF_USER_ERR_NO_MEM;
            goto fail;
        }
        out->frames[i] = pa;
        out->frame_count = i + 1u;

        va = base_va + (uint64_t)i * WLF_PAGE_SIZE;
        /* Kod: yazilamaz (X), veri/yigin: W + NX. */
        flags = (i < (uint32_t)code_pages) ? 0ull : flags_data;

        if (vmm_space_map_user((vmm_space_t)out->space, va, pa, flags) != 0) {
            out->status = WLF_USER_ERR_MAP;
            goto fail;
        }
        mapped++;
    }

    /* Kodu fiziksel cercevelere kopyala (kimlik eslemeli). Kalan sayfa
     * kuyrugu ve bss zaten vmm_alloc_frame tarafindan sifirlanmistir. */
    {
        const uint8_t *src = (const uint8_t *)image + hdr.payload_off;
        uint32_t left = hdr.image_size;

        i = 0;
        while (left) {
            uint32_t chunk = left > (uint32_t)WLF_PAGE_SIZE
                           ? (uint32_t)WLF_PAGE_SIZE : left;
            memcpy((void *)(uintptr_t)out->frames[i], src, chunk);
            src  += chunk;
            left -= chunk;
            i++;
        }
    }

    out->base_va     = base_va;
    out->entry_va    = base_va + hdr.entry_off;
    out->stack_top   = stack_top;
    out->code_pages  = (uint32_t)code_pages;
    out->data_pages  = (uint32_t)data_pages;
    out->stack_pages = (uint32_t)stack_pages;
    out->image_size  = hdr.image_size;
    out->bss_size    = hdr.bss_size;
    out->entry_off   = hdr.entry_off;
    out->load_addr   = hdr.load_addr;
    out->nx          = nx ? 1u : 0u;
    out->status      = WLF_USER_OK;

    return out->entry_va;

fail:
    /* Once alan (eslenmis cerceveler + tablolar), sonra eslenmemis
     * cerceveler. mapped sayaci cift serbest birakmayi onler. */
    if (out->space) {
        vmm_space_destroy((vmm_space_t)out->space);
        out->space = 0;
    }
    wlf_free_frames(out, mapped);
    out->base_va = out->entry_va = out->stack_top = 0;
    return 0;
}

int wlf_user_free(wlf_user_t *out) {
    if (!out) return WLF_USER_ERR_ARG;
    if (out->magic == WLF_USER_FREED) return WLF_USER_ERR_STATE;
    if (out->magic != WLF_USER_MAGIC) return WLF_USER_ERR_STATE;

    if (out->space) {
        vmm_space_destroy((vmm_space_t)out->space);
        out->space = 0;
    }
    /* Basarili yuklemede cerceveler alanla birlikte serbest birakildi;
     * burada yalnizca kmalloc listesi geri verilir. */
    if (out->frames) {
        kfree(out->frames);
        out->frames = NULL;
    }
    out->frame_count = 0;

    memset(out, 0, sizeof(*out));
    out->magic  = WLF_USER_FREED;
    out->status = WLF_USER_ERR_STATE;
    return WLF_USER_OK;
}
