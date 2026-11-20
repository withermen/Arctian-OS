/**
 * Arctian - Ayri kullanici adres alani (PML4) yardimcilari
 * ======================================================
 *
 * paging.c'deki genel VMM katmanini DEGISTIRMEZ; onun uzerine "gorev
 * basina adres alani" kolayliklarini koyar. Cerceve ayiricisi yine
 * vmm_alloc_frame/vmm_free_frame'tir.
 *
 * Adres alani olusturulurken cekirdek eylemeleri paylasilir (kopyalanan
 * sey tablo isaretcisidir, tablo icerigi degil). U biti her kopyada
 * temizlenir; boylece CPL3 cekirdek belleGine erisemez.
 */
#include "vmm_space.h"
#include "paging.h"
#include "memory_map.h"

#define EFER_MSR      0xC0000080u
#define EFER_NXE      (1ull << 11)

static inline uint64_t rd_cr3_local(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3,%0" : "=r"(v));
    return v;
}

static inline void wr_cr3_local(uint64_t v) {
    __asm__ volatile("mov %0,%%cr3" :: "r"(v) : "memory");
}

static inline uint64_t rdmsr_local(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t *ptbl(uint64_t phys) { return (uint64_t *)(uintptr_t)phys; }

int vmm_space_nx_ok(void) {
    return (rdmsr_local(EFER_MSR) & EFER_NXE) ? 1 : 0;
}

int vmm_space_user_range(uint64_t va, uint64_t size) {
    uint64_t end;
    if (va < (uint64_t)VMM_USER_BASE) return 0;
    if (size > VMM_USER_SIZE) return 0;
    end = va + size;
    if (end < va) return 0;                 /* tasma */
    if (end > VMM_USER_TOP) return 0;
    return 1;
}

vmm_space_t vmm_space_create(void) {
    vmm_state_t *st = VMM_STATE;
    uint64_t pml4;
    uint64_t *src, *dst;
    int i;

    if (!st->ready || !st->kernel_space) return 0;

    pml4 = vmm_alloc_frame();
    if (!pml4) return 0;

    src = ptbl(st->kernel_space);
    dst = ptbl(pml4);

    /* Cekirdek eylemelerini kopyala. U=0 zorunlu: yeni alanda cekirdek
     * bolgeleri supervisor-only kalir. */
    for (i = 0; i < 512; i++) {
        uint64_t e = src[i];
        if (!(e & VMM_PTE_P)) continue;
        dst[i] = e & ~VMM_PTE_U;
    }

    /* Kullanici araligi (PML4 1..255) bos olmali; kaynak alan bir gorev
     * alaniysa oradaki kullanici eslemeleri tasinmaz. */
    for (i = (int)VMM_USER_PML4_INDEX; i < (int)VMM_HIGHER_PML4_INDEX; i++)
        dst[i] = 0;

    return (vmm_space_t)pml4;
}

int vmm_space_map_user(vmm_space_t sp, uint64_t va, uint64_t pa, uint64_t flags) {
    uint64_t pte_flags;

    if (!sp) return -1;
    if (va & 0xFFFull) return -1;
    if (pa & 0xFFFull) return -1;
    if (!pa) return -1;                       /* 0. sayfa kullaniciya verilmez */
    if (!vmm_space_user_range(va, 0x1000)) return -1;
    if (flags & VMM_PTE_HUGE) return -1;      /* kullanici icin 2MB yaprak yok */

    pte_flags = flags & (VMM_PTE_W | VMM_PTE_PWT | VMM_PTE_PCD | VMM_PTE_NX);
    if ((pte_flags & VMM_PTE_NX) && !vmm_space_nx_ok()) return -5;
    pte_flags |= VMM_PTE_P | VMM_PTE_U;

    return vmm_map(sp, va, pa, pte_flags);
}

int vmm_space_unmap_user(vmm_space_t sp, uint64_t va) {
    if (!sp) return -1;
    if (va & 0xFFFull) return -1;
    if (!vmm_space_user_range(va, 0x1000)) return -1;
    return vmm_unmap(sp, va);
}

void vmm_space_destroy(vmm_space_t sp) {
    vmm_state_t *st = VMM_STATE;
    uint64_t *pml4;
    int i4, i3, i2, i1;

    if (!sp || (sp & 0xFFFull)) return;
    if (!st->ready) return;
    if (sp == st->kernel_space) return;       /* cekirdek alani yikilmaz */
    if (sp == vmm_current()) return;          /* aktif CR3 serbest birakilmaz */

    pml4 = ptbl(sp);

    /* Yalniz kullanici PML4 girisi (1..255). 0 ve 256 cekirdekle paylasilir. */
    for (i4 = (int)VMM_USER_PML4_INDEX; i4 < (int)VMM_HIGHER_PML4_INDEX; i4++) {
        uint64_t e4 = pml4[i4];
        uint64_t *pdpt;

        if (!(e4 & VMM_PTE_P)) continue;
        pml4[i4] = 0;
        if (e4 & VMM_PTE_HUGE) continue;      /* PML4'te yaprak gecerli degil */
        pdpt = ptbl(e4 & VMM_ADDR_MASK);

        for (i3 = 0; i3 < 512; i3++) {
            uint64_t e3 = pdpt[i3];
            uint64_t *pd;

            if (!(e3 & VMM_PTE_P)) continue;
            pdpt[i3] = 0;
            if (e3 & VMM_PTE_HUGE) continue;  /* 1GB yaprak: veri cercevesi sayilmaz */
            pd = ptbl(e3 & VMM_ADDR_MASK);

            for (i2 = 0; i2 < 512; i2++) {
                uint64_t e2 = pd[i2];
                uint64_t *pt;

                if (!(e2 & VMM_PTE_P)) continue;
                pd[i2] = 0;
                if (e2 & VMM_PTE_HUGE) continue;  /* 2MB yaprak: ayni sekilde atla */
                pt = ptbl(e2 & VMM_ADDR_MASK);

                for (i1 = 0; i1 < 512; i1++) {
                    uint64_t e1 = pt[i1];
                    uint64_t leaf;

                    if (!(e1 & VMM_PTE_P)) continue;
                    pt[i1] = 0;
                    leaf = e1 & VMM_ADDR_MASK;
                    /* Ayni cerceve iki VA'ya eslenmisse cift serbest
                     * birakma olur; boyle bir kullanim bu API'de yok. */
                    if (leaf) vmm_free_frame(leaf);
                }
                vmm_free_frame(e2 & VMM_ADDR_MASK);
            }
            vmm_free_frame(e3 & VMM_ADDR_MASK);
        }
        vmm_free_frame(e4 & VMM_ADDR_MASK);
    }

    vmm_free_frame(sp);
}

void vmm_space_switch(vmm_space_t sp) {
    if (sp) wr_cr3_local((uint64_t)sp);
}

vmm_space_t vmm_space_current(void) {
    return (vmm_space_t)(rd_cr3_local() & VMM_ADDR_MASK);
}
