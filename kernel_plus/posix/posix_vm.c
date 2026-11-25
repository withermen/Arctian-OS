/* POSIX Sanal Bellek: RAM-olcekli mmap (anonim + PROT_NONE demand paging + dosya + SHM),
 * munmap, mprotect (tam VMA bolme/birlestirme), madvise(MADV_DONTNEED), mremap, brk + SHM.
 *
 * Her surecin kendi izole PML4'u (vmm_space_t) ve dinamik heap VMA tablosu vardir.
 * PROT_READ | PROT_WRITE | PROT_EXEC tam desteklenir (NX biti CPUID/EFER'e
 * gore ayarlanir -> JS JIT icin W->X veya RWX calisir).
 *
 * JSC / bmalloc / WebAssembly icin kritik:
 *   - PROT_NONE / MAP_NORESERVE anonim eslemeler fiziksel cerceve ayirmadan
 *     256 GB'a kadar sanal alan rezerve eder (GigaCage / StructureID / Wasm guard).
 *   - mprotect() VMA'lari boler; ilk erisimde #PF demand-paging ile 4 KB sifir
 *     cerceve tahsis edilir.
 *   - Tum unmap/mprotect/fork/madvise taramalari vmm_space_next_present() ile
 *     seyrek (sparse) yurur: 64 GB GigaCage uzerinde mikrosaniyede biter.
 */
#include "posix.h"
#include "alloc.h"
#include "afs.h"
#include "serial.h"
#include <string.h>

#define PAGE_SZ   0x1000ull
#define PAGE_MASK (~(PAGE_SZ - 1ull))
#define PAGE_UP(x) (((uint64_t)(x) + PAGE_SZ - 1ull) & PAGE_MASK)

#define VMA_RESERVE_MAX (256ull * 1024ull * 1024ull * 1024ull) /* 256 GB sanal rezerv (GigaCage) */

static uint64_t vm_total_ram_bytes(void) {
    vmm_meminfo_t mi;
    vmm_mem_info(&mi);
    return mi.total_bytes ? mi.total_bytes : (96ull * 1024ull * 1024ull);
}

static uint64_t vm_shm_max_pages(void) {
    uint64_t lim = vm_total_ram_bytes() / 4ull; /* VMM RAM'in %25'i */
    if (lim < (32ull * 1024ull * 1024ull)) lim = 32ull * 1024ull * 1024ull;
    return lim >> 12;
}

static uint64_t vm_commit_max_bytes(void) {
    uint64_t lim = (vm_total_ram_bytes() * 3ull) / 4ull; /* VMM RAM'in %75'i */
    if (lim < (64ull * 1024ull * 1024ull)) lim = 64ull * 1024ull * 1024ull;
    return lim;
}

/* ---- Paylasimli bellek nesneleri (SHM / memfd / GFX tamponu) ---- */
typedef struct {
    int       used;
    int       refs;
    uint64_t  size;
    uint64_t  npages;
    uint64_t *frames;
} posix_shm_t;

static posix_shm_t s_shms[POSIX_MAX_SHMS];
static spinlock_t  s_shm_lock;

int posix_shm_create(uint64_t size) {
    uint64_t npages = PAGE_UP(size) >> 12;
    if (npages > vm_shm_max_pages()) return -ENOMEM;

    uint64_t *frames = 0;
    if (npages > 0) {
        frames = (uint64_t *)kmalloc((size_t)(npages * sizeof(uint64_t)));
        if (!frames) return -ENOMEM;
        for (uint64_t i = 0; i < npages; i++) {
            frames[i] = vmm_alloc_frame();
            if (!frames[i]) {
                for (uint64_t j = 0; j < i; j++) vmm_free_frame(frames[j]);
                kfree(frames);
                return -ENOMEM;
            }
        }
    }

    uint64_t f = sched_irq_save();
    spin_lock(&s_shm_lock);
    int id = -1;
    for (int i = 0; i < POSIX_MAX_SHMS; i++) {
        if (!s_shms[i].used) {
            s_shms[i].used = 1;
            s_shms[i].refs = 1;
            s_shms[i].size = size;
            s_shms[i].npages = npages;
            s_shms[i].frames = frames;
            id = i;
            break;
        }
    }
    spin_unlock(&s_shm_lock);
    sched_irq_restore(f);

    if (id < 0 && frames) {
        for (uint64_t i = 0; i < npages; i++) vmm_free_frame(frames[i]);
        kfree(frames);
        return -ENOSPC;
    }
    return id;
}

int posix_shm_ref(int shm_id) {
    if (shm_id < 0 || shm_id >= POSIX_MAX_SHMS) return -EINVAL;
    uint64_t f = sched_irq_save();
    spin_lock(&s_shm_lock);
    if (!s_shms[shm_id].used) { spin_unlock(&s_shm_lock); sched_irq_restore(f); return -ENOENT; }
    s_shms[shm_id].refs++;
    spin_unlock(&s_shm_lock);
    sched_irq_restore(f);
    return 0;
}

void posix_shm_unref(int shm_id) {
    if (shm_id < 0 || shm_id >= POSIX_MAX_SHMS) return;
    uint64_t *frames = 0;
    uint64_t npages = 0;
    uint64_t f = sched_irq_save();
    spin_lock(&s_shm_lock);
    if (s_shms[shm_id].used && --s_shms[shm_id].refs <= 0) {
        frames = s_shms[shm_id].frames;
        npages = s_shms[shm_id].npages;
        s_shms[shm_id].used = 0;
        s_shms[shm_id].refs = 0;
        s_shms[shm_id].size = 0;
        s_shms[shm_id].npages = 0;
        s_shms[shm_id].frames = 0;
    }
    spin_unlock(&s_shm_lock);
    sched_irq_restore(f);
    if (frames) {
        for (uint64_t i = 0; i < npages; i++) vmm_free_frame(frames[i]);
        kfree(frames);
    }
}

uint64_t posix_shm_size(int shm_id) {
    if (shm_id < 0 || shm_id >= POSIX_MAX_SHMS || !s_shms[shm_id].used) return 0;
    return s_shms[shm_id].size;
}

uint64_t posix_shm_frame(int shm_id, uint64_t page_idx) {
    if (shm_id < 0 || shm_id >= POSIX_MAX_SHMS || !s_shms[shm_id].used) return 0;
    if (page_idx >= s_shms[shm_id].npages) return 0;
    return s_shms[shm_id].frames[page_idx];
}

int posix_shm_resize(int shm_id, uint64_t new_size) {
    if (shm_id < 0 || shm_id >= POSIX_MAX_SHMS) return -EINVAL;
    uint64_t need = PAGE_UP(new_size) >> 12;
    if (need > vm_shm_max_pages()) return -ENOMEM;

    uint64_t f = sched_irq_save();
    spin_lock(&s_shm_lock);
    if (!s_shms[shm_id].used) { spin_unlock(&s_shm_lock); sched_irq_restore(f); return -ENOENT; }
    uint64_t old_n = s_shms[shm_id].npages;
    uint64_t *old_f = s_shms[shm_id].frames;
    if (need <= old_n) {
        s_shms[shm_id].size = new_size;
        spin_unlock(&s_shm_lock);
        sched_irq_restore(f);
        return 0;
    }
    spin_unlock(&s_shm_lock);
    sched_irq_restore(f);

    uint64_t *nf = (uint64_t *)kmalloc((size_t)(need * sizeof(uint64_t)));
    if (!nf) return -ENOMEM;
    for (uint64_t i = 0; i < old_n; i++) nf[i] = old_f[i];
    for (uint64_t i = old_n; i < need; i++) {
        nf[i] = vmm_alloc_frame();
        if (!nf[i]) {
            for (uint64_t j = old_n; j < i; j++) vmm_free_frame(nf[j]);
            kfree(nf);
            return -ENOMEM;
        }
    }

    f = sched_irq_save();
    spin_lock(&s_shm_lock);
    s_shms[shm_id].frames = nf;
    s_shms[shm_id].npages = need;
    s_shms[shm_id].size = new_size;
    spin_unlock(&s_shm_lock);
    sched_irq_restore(f);
    if (old_f) kfree(old_f);
    return 0;
}

/* ---- PROT -> x86-64 PTE bayraklari ---- */
static uint64_t prot_to_pte(uint32_t prot) {
    uint64_t flags = 0;
    if (prot & PROT_WRITE) flags |= VMM_PTE_W;
    if (!(prot & PROT_EXEC) && vmm_space_nx_ok()) flags |= VMM_PTE_NX;
    return flags;
}

int posix_vm_init_proc(posix_proc_t *p) {
    if (!p) return -EINVAL;
    if (!p->vmas) {
        p->vmas = (posix_vma_t *)kmalloc((size_t)POSIX_MAX_VMAS * sizeof(posix_vma_t));
        if (!p->vmas) {
            p->vma_cap = 0;
            return -ENOMEM;
        }
        p->vma_cap = POSIX_MAX_VMAS;
    }
    memset(p->vmas, 0, (size_t)p->vma_cap * sizeof(posix_vma_t));
    p->brk_base  = 0x0000008020000000ull;
    p->brk_cur   = p->brk_base;
    p->mmap_next = 0x0000010000000000ull;
    return 0;
}

void posix_vm_destroy_proc(posix_proc_t *p) {
    if (!p) return;
    if (p->vmas) {
        for (int i = 0; i < p->vma_cap; i++) {
            if (!p->vmas[i].used) continue;
            if (p->vmas[i].shm_id >= 0) posix_shm_unref(p->vmas[i].shm_id);
            p->vmas[i].used = 0;
        }
        kfree(p->vmas);
        p->vmas = 0;
        p->vma_cap = 0;
    }
    if (p->space) {
        /* vmm_space_destroy her kullanici PTE yapragi icin vmm_free_frame cagirir;
         * vmm_free_frame refcount'lu oldugundan SHM/paylasimli cerceveler
         * guvenle eksiltilir, cift serbest birakma olmaz. */
        vmm_space_destroy(p->space);
        p->space = 0;
    }
}

int posix_vm_add_vma(posix_proc_t *p, uint64_t start, uint64_t end,
                     uint32_t prot, uint32_t flags, int shm_id) {
    if (!p || !p->vmas || start >= end) return -EINVAL;
    for (int i = 0; i < p->vma_cap; i++) {
        if (!p->vmas[i].used) {
            p->vmas[i].start  = start;
            p->vmas[i].end    = end;
            p->vmas[i].prot   = prot;
            p->vmas[i].flags  = flags;
            p->vmas[i].shm_id = shm_id;
            p->vmas[i].used   = 1;
            return 0;
        }
    }
    return -ENOMEM;
}

/* Bitisik ve ozdes anonim ozel VMA'lari birlestir (bmalloc mprotect parcalanmasini onler). */
static void vma_coalesce_locked(posix_proc_t *p) {
    if (!p || !p->vmas) return;
    int merged;
    do {
        merged = 0;
        for (int i = 0; i < p->vma_cap; i++) {
            posix_vma_t *a = &p->vmas[i];
            if (!a->used || a->shm_id >= 0 || (a->flags & MAP_SHARED)) continue;
            for (int j = 0; j < p->vma_cap; j++) {
                if (j == i) continue;
                posix_vma_t *b = &p->vmas[j];
                if (!b->used || b->shm_id >= 0 || (b->flags & MAP_SHARED)) continue;
                if (a->end == b->start && a->prot == b->prot &&
                    (a->flags & ~MAP_FIXED) == (b->flags & ~MAP_FIXED)) {
                    a->end = b->end;
                    b->used = 0;
                    merged = 1;
                }
            }
        }
    } while (merged);
}

static posix_vma_t *vma_find_locked(posix_proc_t *p, uint64_t va) {
    if (!p || !p->vmas) return 0;
    for (int i = 0; i < p->vma_cap; i++) {
        if (p->vmas[i].used && va >= p->vmas[i].start && va < p->vmas[i].end)
            return &p->vmas[i];
    }
    return 0;
}

static int vma_overlaps(posix_proc_t *p, uint64_t start, uint64_t end) {
    if (!p || !p->vmas) return 0;
    for (int i = 0; i < p->vma_cap; i++) {
        if (!p->vmas[i].used) continue;
        if (start < p->vmas[i].end && end > p->vmas[i].start) return 1;
    }
    return 0;
}

uint64_t posix_vm_find_free(posix_proc_t *p, uint64_t len) {
    if (!p || !p->vmas) return 0;
    uint64_t va = PAGE_UP(p->mmap_next);
    if (va < 0x0000010000000000ull) va = 0x0000010000000000ull;
    int max_tries = p->vma_cap + 64;
    for (int tries = 0; tries < max_tries; tries++) {
        if (va + len < va || va + len > 0x00007F0000000000ull) {
            va = 0x0000010000000000ull;
            continue;
        }
        if (!vma_overlaps(p, va, va + len)) {
            p->mmap_next = va + len;
            return va;
        }
        uint64_t next = va + len;
        for (int i = 0; i < p->vma_cap; i++) {
            if (p->vmas[i].used && va < p->vmas[i].end && (va + len) > p->vmas[i].start) {
                if (p->vmas[i].end > next) next = p->vmas[i].end;
            }
        }
        va = PAGE_UP(next);
    }
    return 0;
}

/* Belirtilen [start, end) araligindaki mevcut eslemeleri seyrek olarak kaldir / VMA'lari kirp. */
static void vm_unmap_range_locked(posix_proc_t *p, uint64_t start, uint64_t end) {
    for (uint64_t va = vmm_space_next_present(p->space, start, end);
         va < end;
         va = vmm_space_next_present(p->space, va + PAGE_SZ, end)) {
        uint64_t pte = vmm_space_get_user_pte(p->space, va);
        if (pte & VMM_PTE_P) {
            uint64_t pa = pte & VMM_ADDR_MASK;
            vmm_space_unmap_user(p->space, va);
            if (pa) vmm_free_frame(pa);
        }
    }
    if (!p->vmas) return;
    for (int i = 0; i < p->vma_cap; i++) {
        if (!p->vmas[i].used) continue;
        posix_vma_t *v = &p->vmas[i];
        if (v->end <= start || v->start >= end) continue;
        if (start <= v->start && end >= v->end) {
            if (v->shm_id >= 0) posix_shm_unref(v->shm_id);
            v->used = 0;
        } else if (start <= v->start && end < v->end) {
            v->start = end;
        } else if (start > v->start && end >= v->end) {
            v->end = start;
        } else {
            /* Ortadan bolundu: sag parcayi yeni VMA yap */
            uint64_t orig_end = v->end;
            uint32_t vprot = v->prot;
            uint32_t vflags = v->flags;
            int vshm = v->shm_id;
            v->end = start;
            if (vshm >= 0) posix_shm_ref(vshm);
            if (posix_vm_add_vma(p, end, orig_end, vprot, vflags, vshm) < 0) {
                vma_coalesce_locked(p);
                if (posix_vm_add_vma(p, end, orig_end, vprot, vflags, vshm) < 0) {
                    if (vshm >= 0) posix_shm_unref(vshm);
                }
            }
        }
    }
}

/* Demand-paging cekirdek isleyici: anonim ozel VMA icindeki haritasiz sayfaya
 * ilk erisimde (veya posix_uprobe icinden) 4 KB sifirlanmis cerceve esler.
 * 1 = cozuldu, 0 = gecersiz erisim (gercek SIGSEGV/EFAULT). */
int posix_vm_fault_page(posix_proc_t *p, uint64_t va, int is_write, int is_exec) {
    if (!p || !p->space || !p->vmas) return 0;
    if (va < (uint64_t)VMM_USER_BASE || va >= VMM_USER_TOP) return 0;
    uint64_t pg = va & PAGE_MASK;

    uint64_t fl = sched_irq_save();
    spin_lock(&p->lock);

    posix_vma_t *v = vma_find_locked(p, pg);
    if (!v || v->prot == PROT_NONE) {
        spin_unlock(&p->lock);
        sched_irq_restore(fl);
        return 0;
    }
    if (is_write && !(v->prot & PROT_WRITE)) {
        spin_unlock(&p->lock);
        sched_irq_restore(fl);
        return 0;
    }
    if (is_exec && !(v->prot & PROT_EXEC)) {
        spin_unlock(&p->lock);
        sched_irq_restore(fl);
        return 0;
    }

    /* SMP yarisi: kardes thread ayni sayfayi az once eslemis olabilir */
    uint64_t existing = vmm_space_get_user_pte(p->space, pg);
    if (existing & VMM_PTE_P) {
        int ok = (existing & VMM_PTE_U) != 0;
        if (ok && is_write && !(existing & VMM_PTE_W)) ok = 0;
        if (ok && is_exec && (existing & VMM_PTE_NX)) ok = 0;
        spin_unlock(&p->lock);
        sched_irq_restore(fl);
        return ok;
    }

    /* Yalnizca anonim ozel (veya demand-zero) VMA'lar tembel tahsis edilir */
    if (v->shm_id >= 0 || (v->flags & MAP_SHARED)) {
        spin_unlock(&p->lock);
        sched_irq_restore(fl);
        return 0;
    }

    uint64_t pa = vmm_alloc_frame();
    if (!pa) {
        spin_unlock(&p->lock);
        sched_irq_restore(fl);
        return 0;
    }
    uint64_t pflags = prot_to_pte(v->prot);
    if (vmm_space_map_user(p->space, pg, pa, pflags) != 0) {
        vmm_free_frame(pa);
        spin_unlock(&p->lock);
        sched_irq_restore(fl);
        return 0;
    }

    spin_unlock(&p->lock);
    sched_irq_restore(fl);
    return 1;
}

/* ISR #PF kancasi (main_kernel/isr.c): log/panik oncesi demand paging cozer. */
void posix_vm_dump_fault(posix_proc_t *p, uint64_t va) {
    if (!p || !p->vmas) return;
    int cnt = 0;
    posix_vma_t *hit = 0;
    for (int i = 0; i < p->vma_cap; i++) {
        if (!p->vmas[i].used) continue;
        cnt++;
        if (va >= p->vmas[i].start && va < p->vmas[i].end)
            hit = &p->vmas[i];
    }
    uint64_t pte = p->space ? vmm_space_get_user_pte(p->space, va & PAGE_MASK) : 0;
    if (hit) {
        serial_printf("[vm-fault] pid=%d vmas=%d/%d hit=[0x%X%08X..0x%X%08X) prot=%u flags=0x%X shm=%d pte=0x%X%08X\n",
                      p->pid, cnt, p->vma_cap,
                      (unsigned)(hit->start >> 32), (unsigned)hit->start,
                      (unsigned)(hit->end >> 32), (unsigned)hit->end,
                      hit->prot, hit->flags, hit->shm_id,
                      (unsigned)(pte >> 32), (unsigned)pte);
    } else {
        serial_printf("[vm-fault] pid=%d vmas=%d/%d NO_VMA for va=0x%X%08X pte=0x%X%08X\n",
                      p->pid, cnt, p->vma_cap,
                      (unsigned)(va >> 32), (unsigned)va,
                      (unsigned)(pte >> 32), (unsigned)pte);
        for (int i = 0; i < p->vma_cap; i++) {
            posix_vma_t *v = &p->vmas[i];
            if (!v->used) continue;
            serial_printf("  vma[%d]: [0x%X%08X..0x%X%08X) prot=%u flags=0x%X shm=%d\n",
                          i,
                          (unsigned)(v->start >> 32), (unsigned)v->start,
                          (unsigned)(v->end >> 32), (unsigned)v->end,
                          v->prot, v->flags, v->shm_id);
        }
    }
}

int posix_pf_handler(uint64_t cr2, uint64_t err, uint64_t rip) {
    (void)rip;
    if (cr2 < (uint64_t)VMM_USER_BASE || cr2 >= VMM_USER_TOP) return 0;
    /* P=1 (koruma ihlali) ise demand paging degil; gercek izin ihlalidir */
    if (err & 1ull) return 0;
    posix_proc_t *p = posix_cur_proc();
    if (!p || !p->space) return 0;
    int is_write = (err & 2ull) != 0;
    int is_exec  = (err & 16ull) != 0;
    return posix_vm_fault_page(p, cr2, is_write, is_exec);
}

int posix_vm_clone(posix_proc_t *dst, posix_proc_t *src) {
    if (!dst || !src || !dst->vmas || !src->vmas) return -EINVAL;
    dst->brk_base  = src->brk_base;
    dst->brk_cur   = src->brk_cur;
    dst->mmap_next = src->mmap_next;
    memset(dst->vmas, 0, (size_t)dst->vma_cap * sizeof(posix_vma_t));

    for (int i = 0; i < src->vma_cap; i++) {
        if (!src->vmas[i].used) continue;
        posix_vma_t *sv = &src->vmas[i];
        int shared = (sv->flags & MAP_SHARED) != 0;
        if (sv->shm_id >= 0) posix_shm_ref(sv->shm_id);
        if (posix_vm_add_vma(dst, sv->start, sv->end, sv->prot, sv->flags, sv->shm_id) < 0)
            return -ENOMEM;

        /* Seyrek tarama: 64 GB PROT_NONE VMA icinde sadece gercekten mevcut sayfalar kopyalanir */
        for (uint64_t va = vmm_space_next_present(src->space, sv->start, sv->end);
             va < sv->end;
             va = vmm_space_next_present(src->space, va + PAGE_SZ, sv->end)) {
            uint64_t pte = vmm_space_get_user_pte(src->space, va);
            if (!(pte & VMM_PTE_P)) continue;
            uint64_t spa = pte & VMM_ADDR_MASK;
            int is_none = (sv->prot == PROT_NONE) || !(pte & VMM_PTE_U);
            uint64_t pflags = pte & (VMM_PTE_W | VMM_PTE_NX);
            int ro_share = !is_none && !(sv->prot & PROT_WRITE) && !(pte & VMM_PTE_W);
            if (shared || ro_share) {
                vmm_frame_ref(spa);
                if (vmm_space_map_user(dst->space, va, spa, pflags) != 0) {
                    vmm_free_frame(spa);
                    return -ENOMEM;
                }
                if (is_none) vmm_space_protect_none(dst->space, va);
            } else {
                uint64_t dpa = vmm_alloc_frame();
                if (!dpa) return -ENOMEM;
                memcpy((void *)(uintptr_t)dpa, (const void *)(uintptr_t)spa, PAGE_SZ);
                if (vmm_space_map_user(dst->space, va, dpa, pflags) != 0) {
                    vmm_free_frame(dpa);
                    return -ENOMEM;
                }
                if (is_none) vmm_space_protect_none(dst->space, va);
            }
        }
    }
    return 0;
}

int64_t posix_sys_mmap(posix_proc_t *p, uint64_t addr, uint64_t len,
                       uint32_t prot, uint32_t flags, int fd, uint64_t off) {
    if (!p || !p->vmas || len == 0 || (off & (PAGE_SZ - 1ull))) return -EINVAL;
    uint64_t alen = PAGE_UP(len);
    if (alen == 0 || alen < len) return -ENOMEM;

    int is_anon   = (flags & MAP_ANONYMOUS) != 0;
    int is_fixed  = (flags & MAP_FIXED) != 0;
    int is_norepl = (flags & MAP_FIXED_NOREPLACE) != 0;
    int is_shared = (flags & MAP_SHARED) != 0;
    int is_lazy   = is_anon && !is_shared;

    /* Sanal rezerv (PROT_NONE / MAP_NORESERVE): GigaCage icin 256 GB'a kadar izin ver;
     * normal anonim ozel eslemeler de demand-paging ile RAM'e gore olceklenir. */
    if (is_lazy && (prot == PROT_NONE || (flags & MAP_NORESERVE))) {
        if (alen > VMA_RESERVE_MAX) return -ENOMEM;
    } else if (is_lazy) {
        uint64_t lim = vm_total_ram_bytes();
        if (lim < (512ull * 1024ull * 1024ull)) lim = 512ull * 1024ull * 1024ull;
        if (alen > lim) return -ENOMEM;
    } else {
        if (alen > vm_commit_max_bytes()) return -ENOMEM;
    }

    posix_file_t *file = 0;
    int shm_id = -1;
    if (!is_anon) {
        file = posix_fd_get(p, fd);
        if (!file) return -EBADF;
        if (file->kind == PFD_SHM) {
            shm_id = file->obj_id;
        } else if (file->kind == PFD_GFX) {
            shm_id = posix_gfx_shm_id(file->obj_id);
        } else if (file->kind != PFD_AFS && file->kind != PFD_VFILE) {
            return -ENODEV;
        }
    }

    /* MAP_SHARED + anonim -> kilit almadan once SHM olustur (VA secim yarisi olmaz) */
    int own_shm = 0;
    if (is_anon && is_shared) {
        shm_id = posix_shm_create(alen);
        if (shm_id < 0) return shm_id;
        own_shm = 1;
    }

    uint64_t fl = sched_irq_save();
    spin_lock(&p->lock);

    uint64_t va = 0;
    if (is_fixed || is_norepl) {
        if ((addr & (PAGE_SZ - 1ull)) || !vmm_space_user_range(addr, alen)) {
            spin_unlock(&p->lock);
            sched_irq_restore(fl);
            if (own_shm && shm_id >= 0) posix_shm_unref(shm_id);
            return -EINVAL;
        }
        if (is_norepl && vma_overlaps(p, addr, addr + alen)) {
            spin_unlock(&p->lock);
            sched_irq_restore(fl);
            if (own_shm && shm_id >= 0) posix_shm_unref(shm_id);
            return -EEXIST;
        }
        vm_unmap_range_locked(p, addr, addr + alen);
        va = addr;
    } else if (addr && (addr & (PAGE_SZ - 1ull)) == 0 &&
               vmm_space_user_range(addr, alen) && !vma_overlaps(p, addr, addr + alen)) {
        va = addr;
    } else {
        va = posix_vm_find_free(p, alen);
        if (!va) {
            spin_unlock(&p->lock);
            sched_irq_restore(fl);
            if (own_shm && shm_id >= 0) posix_shm_unref(shm_id);
            return -ENOMEM;
        }
    }

    /* Anonim ozel esleme: demand paging! Fiziksel cerceve ayirmadan VMA ekle. */
    if (is_lazy) {
        if (posix_vm_add_vma(p, va, va + alen, prot, flags, -1) < 0) {
            vma_coalesce_locked(p);
            if (posix_vm_add_vma(p, va, va + alen, prot, flags, -1) < 0) {
                spin_unlock(&p->lock);
                sched_irq_restore(fl);
                return -ENOMEM;
            }
        }
        spin_unlock(&p->lock);
        sched_irq_restore(fl);
        return (int64_t)va;
    }

    if (!own_shm && shm_id >= 0) {
        posix_shm_ref(shm_id);
        own_shm = 1;
    }

    uint64_t pflags = prot_to_pte(prot);
    uint64_t npages = alen >> 12;
    uint64_t mapped = 0;

    for (uint64_t i = 0; i < npages; i++) {
        uint64_t pa = 0;
        if (shm_id >= 0) {
            pa = posix_shm_frame(shm_id, (off >> 12) + i);
            if (!pa) break;
            vmm_frame_ref(pa);
        } else {
            pa = vmm_alloc_frame();
            if (!pa) break;
        }
        if (vmm_space_map_user(p->space, va + i * PAGE_SZ, pa, pflags) != 0) {
            vmm_free_frame(pa);
            break;
        }
        if (prot == PROT_NONE) vmm_space_protect_none(p->space, va + i * PAGE_SZ);
        mapped++;
    }

    if (mapped < npages) {
        for (uint64_t i = 0; i < mapped; i++) {
            uint64_t pte = vmm_space_get_user_pte(p->space, va + i * PAGE_SZ);
            vmm_space_unmap_user(p->space, va + i * PAGE_SZ);
            if (pte & VMM_PTE_P) vmm_free_frame(pte & VMM_ADDR_MASK);
        }
        if (own_shm && shm_id >= 0) posix_shm_unref(shm_id);
        spin_unlock(&p->lock);
        sched_irq_restore(fl);
        return -ENOMEM;
    }

    /* Dosya destekli mmap: AFS / VFILE icerigini eslenen cercevelere kopyala. */
    if (!is_anon && shm_id < 0 && file) {
        for (uint64_t i = 0; i < npages; i++) {
            uint64_t pte = vmm_space_get_user_pte(p->space, va + i * PAGE_SZ);
            uint64_t pa = pte & VMM_ADDR_MASK;
            uint64_t foff = off + i * PAGE_SZ;
            if (file->kind == PFD_AFS && file->afs_handle) {
                afs_read_at(file->afs_handle, (void *)(uintptr_t)pa,
                            (uint32_t)PAGE_SZ, (uint32_t)foff);
            } else if (file->kind == PFD_VFILE && file->vdata && foff < file->vlen) {
                uint64_t n = file->vlen - foff;
                if (n > PAGE_SZ) n = PAGE_SZ;
                memcpy((void *)(uintptr_t)pa, file->vdata + foff, (size_t)n);
            }
        }
    }

    if (posix_vm_add_vma(p, va, va + alen, prot, flags, shm_id) < 0) {
        for (uint64_t i = 0; i < npages; i++) {
            uint64_t pte = vmm_space_get_user_pte(p->space, va + i * PAGE_SZ);
            vmm_space_unmap_user(p->space, va + i * PAGE_SZ);
            if (pte & VMM_PTE_P) vmm_free_frame(pte & VMM_ADDR_MASK);
        }
        if (own_shm && shm_id >= 0) posix_shm_unref(shm_id);
        spin_unlock(&p->lock);
        sched_irq_restore(fl);
        return -ENOMEM;
    }
    spin_unlock(&p->lock);
    sched_irq_restore(fl);
    return (int64_t)va;
}

int64_t posix_sys_munmap(posix_proc_t *p, uint64_t addr, uint64_t len) {
    if (!p || !p->vmas || len == 0 || (addr & (PAGE_SZ - 1ull))) return -EINVAL;
    uint64_t alen = PAGE_UP(len);
    if (alen == 0 || !vmm_space_user_range(addr, alen)) return -EINVAL;

    uint64_t fl = sched_irq_save();
    spin_lock(&p->lock);
    vm_unmap_range_locked(p, addr, addr + alen);
    spin_unlock(&p->lock);
    sched_irq_restore(fl);
    return 0;
}

int64_t posix_sys_mprotect(posix_proc_t *p, uint64_t addr, uint64_t len, uint32_t prot) {
    if (!p || !p->vmas || (addr & (PAGE_SZ - 1ull))) return -EINVAL;
    if (len == 0) return 0;
    uint64_t alen = PAGE_UP(len);
    if (alen == 0 || !vmm_space_user_range(addr, alen)) return -ENOMEM;
    uint64_t end = addr + alen;

    uint64_t pflags = prot_to_pte(prot);
    uint64_t fl = sched_irq_save();
    spin_lock(&p->lock);

    /* 1) Sadece mevcut (P=1) PTE'leri seyrek olarak guncelle */
    for (uint64_t va = vmm_space_next_present(p->space, addr, end);
         va < end;
         va = vmm_space_next_present(p->space, va + PAGE_SZ, end)) {
        if (prot == PROT_NONE) {
            vmm_space_protect_none(p->space, va);
        } else {
            if (prot & PROT_WRITE) {
                posix_vma_t *cv = vma_find_locked(p, va);
                if (cv && !(cv->flags & MAP_SHARED) && cv->shm_id < 0) {
                    uint64_t pte = vmm_space_get_user_pte(p->space, va);
                    uint64_t opa = pte & VMM_ADDR_MASK;
                    if ((pte & VMM_PTE_P) && !(pte & VMM_PTE_W) && opa && vmm_frame_refcount(opa) > 1u) {
                        uint64_t npa = vmm_alloc_frame();
                        if (!npa) {
                            spin_unlock(&p->lock);
                            sched_irq_restore(fl);
                            return -ENOMEM;
                        }
                        memcpy((void *)(uintptr_t)npa, (const void *)(uintptr_t)opa, PAGE_SZ);
                        if (vmm_space_map_user(p->space, va, npa, pflags) != 0) {
                            vmm_free_frame(npa);
                            spin_unlock(&p->lock);
                            sched_irq_restore(fl);
                            return -ENOMEM;
                        }
                        vmm_free_frame(opa);
                        continue;
                    }
                }
            }
            vmm_space_protect_user(p->space, va, pflags);
        }
    }

    /* 2) Kesisen VMA'lari bol ve yeni prot degerini kaydet (demand paging + fork icin).
     * Dongu icinde vma_coalesce_locked cagrilmaz (v isaretcisini gecersiz kilabilir);
     * yer daralirsa once birlestirip taramayi bastan aliriz. */
    int free_slots = 0;
    for (int k = 0; k < p->vma_cap; k++) {
        if (!p->vmas[k].used) free_slots++;
    }
    if (free_slots < 4) vma_coalesce_locked(p);

    for (int i = 0; i < p->vma_cap; i++) {
        posix_vma_t *v = &p->vmas[i];
        if (!v->used) continue;
        if (v->end <= addr || v->start >= end) continue;
        if (v->prot == prot) continue;

        if (addr <= v->start && end >= v->end) {
            v->prot = prot;
        } else if (addr <= v->start && end < v->end) {
            uint64_t orig_start = v->start;
            uint32_t vflags = v->flags;
            int vshm = v->shm_id;
            v->start = end;
            if (vshm >= 0) posix_shm_ref(vshm);
            if (posix_vm_add_vma(p, orig_start, end, prot, vflags, vshm) < 0) {
                v->start = orig_start;
                if (vshm >= 0) posix_shm_unref(vshm);
                spin_unlock(&p->lock);
                sched_irq_restore(fl);
                return -ENOMEM;
            }
        } else if (addr > v->start && end >= v->end) {
            uint64_t orig_end = v->end;
            uint32_t vflags = v->flags;
            int vshm = v->shm_id;
            v->end = addr;
            if (vshm >= 0) posix_shm_ref(vshm);
            if (posix_vm_add_vma(p, addr, orig_end, prot, vflags, vshm) < 0) {
                v->end = orig_end;
                if (vshm >= 0) posix_shm_unref(vshm);
                spin_unlock(&p->lock);
                sched_irq_restore(fl);
                return -ENOMEM;
            }
        } else {
            /* Ortadan bolme: [v.start, addr) eski, [addr, end) yeni, [end, orig_end) eski */
            uint64_t orig_end = v->end;
            uint32_t old_prot = v->prot;
            uint32_t vflags = v->flags;
            int vshm = v->shm_id;
            if (vshm >= 0) { posix_shm_ref(vshm); posix_shm_ref(vshm); }
            int r1 = posix_vm_add_vma(p, addr, end, prot, vflags, vshm);
            int r2 = (r1 == 0) ? posix_vm_add_vma(p, end, orig_end, old_prot, vflags, vshm) : -ENOMEM;
            if (r1 < 0 || r2 < 0) {
                if (r1 == 0) {
                    for (int k = 0; k < p->vma_cap; k++) {
                        if (p->vmas[k].used && p->vmas[k].start == addr && p->vmas[k].end == end) {
                            p->vmas[k].used = 0;
                            break;
                        }
                    }
                }
                if (vshm >= 0) { posix_shm_unref(vshm); posix_shm_unref(vshm); }
                spin_unlock(&p->lock);
                sched_irq_restore(fl);
                return -ENOMEM;
            }
            v->end = addr;
        }
    }
    vma_coalesce_locked(p);
    spin_unlock(&p->lock);
    sched_irq_restore(fl);
    return 0;
}

int64_t posix_sys_madvise(posix_proc_t *p, uint64_t addr, uint64_t len, int advice) {
    if (!p || !p->vmas || (addr & (PAGE_SZ - 1ull))) return -EINVAL;
    if (len == 0) return 0;
    uint64_t alen = PAGE_UP(len);
    if (alen == 0 || !vmm_space_user_range(addr, alen)) return -EINVAL;
    uint64_t end = addr + alen;

    if (advice == MADV_DONTNEED) {
        uint64_t fl = sched_irq_save();
        spin_lock(&p->lock);
        for (uint64_t va = vmm_space_next_present(p->space, addr, end);
             va < end;
             va = vmm_space_next_present(p->space, va + PAGE_SZ, end)) {
            posix_vma_t *v = vma_find_locked(p, va);
            if (!v || v->shm_id >= 0 || (v->flags & MAP_SHARED)) continue;
            uint64_t pte = vmm_space_get_user_pte(p->space, va);
            if (pte & VMM_PTE_P) {
                uint64_t pa = pte & VMM_ADDR_MASK;
                vmm_space_unmap_user(p->space, va);
                if (pa) vmm_free_frame(pa);
            }
        }
        spin_unlock(&p->lock);
        sched_irq_restore(fl);
    }
    return 0;
}

int64_t posix_sys_mremap(posix_proc_t *p, uint64_t old_addr, uint64_t old_len,
                         uint64_t new_len, uint32_t flags, uint64_t new_addr) {
    if (!p || !p->vmas || (old_addr & (PAGE_SZ - 1ull)) || new_len == 0)
        return -EINVAL;
    uint64_t olen = PAGE_UP(old_len);
    uint64_t nlen = PAGE_UP(new_len);
    if (nlen == 0 || !vmm_space_user_range(old_addr, olen ? olen : PAGE_SZ))
        return -EFAULT;

    uint64_t fl = sched_irq_save();
    spin_lock(&p->lock);

    posix_vma_t *v = vma_find_locked(p, old_addr);
    if (!v || old_addr + olen > v->end) {
        spin_unlock(&p->lock);
        sched_irq_restore(fl);
        return -EFAULT;
    }

    /* musl pthread_getattr_np yigin siniri sondasi: mremap(p, 4096, 4096, 0) */
    if (olen == nlen && !(flags & MREMAP_FIXED)) {
        spin_unlock(&p->lock);
        sched_irq_restore(fl);
        return (int64_t)old_addr;
    }

    if (nlen < olen && !(flags & MREMAP_FIXED)) {
        vm_unmap_range_locked(p, old_addr + nlen, old_addr + olen);
        spin_unlock(&p->lock);
        sched_irq_restore(fl);
        return (int64_t)old_addr;
    }

    /* Yerinde buyutme: sadece VMA'nin sonundaysa ve arkasinda bosluk varsa */
    if (!(flags & MREMAP_FIXED) && old_addr == v->start && old_addr + olen == v->end) {
        if (vmm_space_user_range(old_addr, nlen) &&
            !vma_overlaps(p, old_addr + olen, old_addr + nlen)) {
            v->end = old_addr + nlen;
            spin_unlock(&p->lock);
            sched_irq_restore(fl);
            return (int64_t)old_addr;
        }
    }

    if (!(flags & MREMAP_MAYMOVE) && !(flags & MREMAP_FIXED)) {
        spin_unlock(&p->lock);
        sched_irq_restore(fl);
        return -ENOMEM;
    }

    uint32_t vprot  = v->prot;
    uint32_t vflags = v->flags;
    int      vshm   = v->shm_id;
    uint64_t copy_len = (olen < nlen) ? olen : nlen;

    uint64_t dst_va = 0;
    if (flags & MREMAP_FIXED) {
        if ((new_addr & (PAGE_SZ - 1ull)) || !vmm_space_user_range(new_addr, nlen)) {
            spin_unlock(&p->lock);
            sched_irq_restore(fl);
            return -EINVAL;
        }
        vm_unmap_range_locked(p, new_addr, new_addr + nlen);
        dst_va = new_addr;
    } else {
        dst_va = posix_vm_find_free(p, nlen);
        if (!dst_va) {
            spin_unlock(&p->lock);
            sched_irq_restore(fl);
            return -ENOMEM;
        }
    }

    if (vshm >= 0) posix_shm_ref(vshm);
    if (posix_vm_add_vma(p, dst_va, dst_va + nlen, vprot, vflags, vshm) < 0) {
        if (vshm >= 0) posix_shm_unref(vshm);
        spin_unlock(&p->lock);
        sched_irq_restore(fl);
        return -ENOMEM;
    }

    /* Mevcut fiziksel cerceveleri kopyalamadan yeni VA'ya tasi */
    for (uint64_t va = vmm_space_next_present(p->space, old_addr, old_addr + copy_len);
         va < old_addr + copy_len;
         va = vmm_space_next_present(p->space, va + PAGE_SZ, old_addr + copy_len)) {
        uint64_t pte = vmm_space_get_user_pte(p->space, va);
        if (!(pte & VMM_PTE_P)) continue;
        uint64_t pa = pte & VMM_ADDR_MASK;
        uint64_t pflags = pte & (VMM_PTE_W | VMM_PTE_NX);
        int is_none = !(pte & VMM_PTE_U);
        uint64_t nva = dst_va + (va - old_addr);
        vmm_frame_ref(pa);
        if (vmm_space_map_user(p->space, nva, pa, pflags) == 0) {
            if (is_none) vmm_space_protect_none(p->space, nva);
        } else {
            vmm_free_frame(pa);
        }
    }

    vm_unmap_range_locked(p, old_addr, old_addr + olen);
    spin_unlock(&p->lock);
    sched_irq_restore(fl);
    return (int64_t)dst_va;
}

int64_t posix_sys_brk(posix_proc_t *p, uint64_t addr) {
    if (!p || !p->vmas) return -ENOMEM;
    uint64_t fl = sched_irq_save();
    spin_lock(&p->lock);
    if (addr == 0 || addr < p->brk_base) {
        uint64_t cur = p->brk_cur;
        spin_unlock(&p->lock);
        sched_irq_restore(fl);
        return (int64_t)cur;
    }
    uint64_t max_brk = vm_commit_max_bytes();
    if (addr > p->brk_base + max_brk) {
        uint64_t cur = p->brk_cur;
        spin_unlock(&p->lock);
        sched_irq_restore(fl);
        return (int64_t)cur;
    }

    uint64_t old_end = PAGE_UP(p->brk_cur);
    uint64_t new_end = PAGE_UP(addr);

    if (new_end > old_end) {
        /* Demand paging: [old_end, new_end) araligini RW anonim VMA ile genislet.
         * Guard sayfasini (musl mallocng mmap(brk_base, 4K, PROT_NONE, MAP_FIXED))
         * veya onceki split/mprotect VMA'larini bozmamak icin start == brk_base
         * yerine end == old_end olan RW anonim VMA'yi uzatiriz. */
        if (vma_overlaps(p, old_end, new_end)) {
            uint64_t cur = p->brk_cur;
            spin_unlock(&p->lock);
            sched_irq_restore(fl);
            return (int64_t)cur;
        }
        int found = 0;
        for (int i = 0; i < p->vma_cap; i++) {
            posix_vma_t *v = &p->vmas[i];
            if (v->used && v->end == old_end && v->start >= p->brk_base &&
                v->prot == (PROT_READ | PROT_WRITE) &&
                v->shm_id < 0 && !(v->flags & MAP_SHARED)) {
                v->end = new_end;
                found = 1;
                break;
            }
        }
        if (!found) {
            uint64_t add_start = (old_end > p->brk_base) ? old_end : p->brk_base;
            if (posix_vm_add_vma(p, add_start, new_end, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1) < 0) {
                vma_coalesce_locked(p);
                if (posix_vm_add_vma(p, add_start, new_end, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1) < 0) {
                    uint64_t cur = p->brk_cur;
                    spin_unlock(&p->lock);
                    sched_irq_restore(fl);
                    return (int64_t)cur;
                }
            }
        }
    } else if (new_end < old_end) {
        vm_unmap_range_locked(p, new_end, old_end);
    }
    p->brk_cur = addr;
    spin_unlock(&p->lock);
    sched_irq_restore(fl);
    return (int64_t)addr;
}
