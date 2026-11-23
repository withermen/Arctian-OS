/* POSIX Sanal Bellek: mmap (anonim + dosya + SHM), munmap, mprotect, brk + SHM.
 *
 * Her surecin kendi izole PML4'u (vmm_space_t) ve VMA tablosu vardir.
 * PROT_READ | PROT_WRITE | PROT_EXEC tam desteklenir (NX biti CPUID/EFER'e
 * gore ayarlanir -> JS JIT icin W->X veya RWX calisir).
 */
#include "posix.h"
#include "alloc.h"
#include "afs.h"
#include "serial.h"
#include <string.h>

#define PAGE_SZ   0x1000ull
#define PAGE_MASK (~(PAGE_SZ - 1ull))
#define PAGE_UP(x) (((uint64_t)(x) + PAGE_SZ - 1ull) & PAGE_MASK)

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
    if (npages > 8192u) return -ENOMEM;  /* tek SHM max 32 MB */

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
    if (need > 8192u) return -ENOMEM;

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

void posix_vm_init_proc(posix_proc_t *p) {
    for (int i = 0; i < POSIX_MAX_VMAS; i++) p->vmas[i].used = 0;
    p->brk_base = 0x0000008020000000ull;
    p->brk_cur  = p->brk_base;
    p->mmap_next = 0x0000010000000000ull;
}

void posix_vm_destroy_proc(posix_proc_t *p) {
    for (int i = 0; i < POSIX_MAX_VMAS; i++) {
        if (!p->vmas[i].used) continue;
        if (p->vmas[i].shm_id >= 0) posix_shm_unref(p->vmas[i].shm_id);
        p->vmas[i].used = 0;
    }
    if (p->space) {
        /* vmm_space_destroy her kullanici PTE yapragi icin vmm_free_frame cagirir;
         * vmm_free_frame artik refcount'lu oldugundan SHM/paylasimli cerceveler
         * guvenle eksiltilir, cift serbest birakma olmaz. */
        vmm_space_destroy(p->space);
        p->space = 0;
    }
}

int posix_vm_add_vma(posix_proc_t *p, uint64_t start, uint64_t end,
                     uint32_t prot, uint32_t flags, int shm_id) {
    for (int i = 0; i < POSIX_MAX_VMAS; i++) {
        if (!p->vmas[i].used) {
            p->vmas[i].start = start;
            p->vmas[i].end = end;
            p->vmas[i].prot = prot;
            p->vmas[i].flags = flags;
            p->vmas[i].shm_id = shm_id;
            p->vmas[i].used = 1;
            return 0;
        }
    }
    return -ENOMEM;
}

static int vma_overlaps(posix_proc_t *p, uint64_t start, uint64_t end) {
    for (int i = 0; i < POSIX_MAX_VMAS; i++) {
        if (!p->vmas[i].used) continue;
        if (start < p->vmas[i].end && end > p->vmas[i].start) return 1;
    }
    return 0;
}

uint64_t posix_vm_find_free(posix_proc_t *p, uint64_t len) {
    uint64_t va = PAGE_UP(p->mmap_next);
    if (va < 0x0000010000000000ull) va = 0x0000010000000000ull;
    for (int tries = 0; tries < 512; tries++) {
        if (va + len < va || va + len > 0x00007F0000000000ull) {
            va = 0x0000010000000000ull;
            continue;
        }
        if (!vma_overlaps(p, va, va + len)) {
            p->mmap_next = va + len;
            return va;
        }
        /* Cakisan VMA'nin sonundan devam et */
        uint64_t next = va + len;
        for (int i = 0; i < POSIX_MAX_VMAS; i++) {
            if (p->vmas[i].used && va < p->vmas[i].end && (va + len) > p->vmas[i].start) {
                if (p->vmas[i].end > next) next = p->vmas[i].end;
            }
        }
        va = PAGE_UP(next);
    }
    return 0;
}

/* Belirtilen [start, end) araligindaki mevcut eslemeleri kaldir / VMA'lari kirp. */
static void vm_unmap_range_locked(posix_proc_t *p, uint64_t start, uint64_t end) {
    for (uint64_t va = start; va < end; va += PAGE_SZ) {
        uint64_t pte = vmm_space_get_user_pte(p->space, va);
        if (pte & VMM_PTE_P) {
            uint64_t pa = pte & VMM_ADDR_MASK;
            vmm_space_unmap_user(p->space, va);
            if (pa) vmm_free_frame(pa);
        }
    }
    for (int i = 0; i < POSIX_MAX_VMAS; i++) {
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
            v->end = start;
            if (v->shm_id >= 0) posix_shm_ref(v->shm_id);
            posix_vm_add_vma(p, end, orig_end, v->prot, v->flags, v->shm_id);
        }
    }
}

int posix_vm_clone(posix_proc_t *dst, posix_proc_t *src) {
    dst->brk_base  = src->brk_base;
    dst->brk_cur   = src->brk_cur;
    dst->mmap_next = src->mmap_next;
    for (int i = 0; i < POSIX_MAX_VMAS; i++) dst->vmas[i].used = 0;

    for (int i = 0; i < POSIX_MAX_VMAS; i++) {
        if (!src->vmas[i].used) continue;
        posix_vma_t *sv = &src->vmas[i];
        int shared = (sv->flags & MAP_SHARED) != 0;
        if (sv->shm_id >= 0) posix_shm_ref(sv->shm_id);
        if (posix_vm_add_vma(dst, sv->start, sv->end, sv->prot, sv->flags, sv->shm_id) < 0)
            return -ENOMEM;

        for (uint64_t va = sv->start; va < sv->end; va += PAGE_SZ) {
            uint64_t pte = vmm_space_get_user_pte(src->space, va);
            if (!(pte & VMM_PTE_P)) continue;
            uint64_t spa = pte & VMM_ADDR_MASK;
            uint64_t pflags = pte & (VMM_PTE_W | VMM_PTE_NX);
            if (shared) {
                vmm_frame_ref(spa);
                if (vmm_space_map_user(dst->space, va, spa, pflags) != 0) {
                    vmm_free_frame(spa);
                    return -ENOMEM;
                }
            } else {
                uint64_t dpa = vmm_alloc_frame();
                if (!dpa) return -ENOMEM;
                memcpy((void *)(uintptr_t)dpa, (const void *)(uintptr_t)spa, PAGE_SZ);
                if (vmm_space_map_user(dst->space, va, dpa, pflags) != 0) {
                    vmm_free_frame(dpa);
                    return -ENOMEM;
                }
            }
        }
    }
    return 0;
}

int64_t posix_sys_mmap(posix_proc_t *p, uint64_t addr, uint64_t len,
                       uint32_t prot, uint32_t flags, int fd, uint64_t off) {
    if (!p || len == 0 || (off & (PAGE_SZ - 1ull))) return -EINVAL;
    uint64_t alen = PAGE_UP(len);
    if (alen == 0 || alen > (64ull * 1024ull * 1024ull)) return -ENOMEM;

    int is_anon = (flags & MAP_ANONYMOUS) != 0;
    int is_fixed = (flags & MAP_FIXED) != 0;
    int is_shared = (flags & MAP_SHARED) != 0;

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

    uint64_t fl = sched_irq_save();
    spin_lock(&p->lock);

    uint64_t va = 0;
    if (is_fixed) {
        if ((addr & (PAGE_SZ - 1ull)) || !vmm_space_user_range(addr, alen)) {
            spin_unlock(&p->lock);
            sched_irq_restore(fl);
            return -EINVAL;
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
            return -ENOMEM;
        }
    }

    /* MAP_SHARED + anonim -> otomatik SHM nesnesi olustur (fork'ta paylasilir) */
    int own_shm = 0;
    if (is_anon && is_shared) {
        spin_unlock(&p->lock);
        sched_irq_restore(fl);
        shm_id = posix_shm_create(alen);
        if (shm_id < 0) return shm_id;
        own_shm = 1;
        fl = sched_irq_save();
        spin_lock(&p->lock);
    } else if (shm_id >= 0) {
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
    if (!p || len == 0 || (addr & (PAGE_SZ - 1ull))) return -EINVAL;
    uint64_t alen = PAGE_UP(len);
    if (!vmm_space_user_range(addr, alen)) return -EINVAL;

    uint64_t fl = sched_irq_save();
    spin_lock(&p->lock);
    vm_unmap_range_locked(p, addr, addr + alen);
    spin_unlock(&p->lock);
    sched_irq_restore(fl);
    return 0;
}

int64_t posix_sys_mprotect(posix_proc_t *p, uint64_t addr, uint64_t len, uint32_t prot) {
    if (!p || (addr & (PAGE_SZ - 1ull))) return -EINVAL;
    if (len == 0) return 0;
    uint64_t alen = PAGE_UP(len);
    if (!vmm_space_user_range(addr, alen)) return -ENOMEM;

    uint64_t pflags = prot_to_pte(prot);
    uint64_t fl = sched_irq_save();
    spin_lock(&p->lock);
    for (uint64_t va = addr; va < addr + alen; va += PAGE_SZ) {
        if (vmm_space_get_user_pte(p->space, va) & VMM_PTE_P) {
            vmm_space_protect_user(p->space, va, pflags);
        }
    }
    /* Ilgili VMA'larin prot bilgisini de guncelle (fork'ta korunsun) */
    for (int i = 0; i < POSIX_MAX_VMAS; i++) {
        if (!p->vmas[i].used) continue;
        if (p->vmas[i].start >= addr && p->vmas[i].end <= addr + alen)
            p->vmas[i].prot = prot;
    }
    spin_unlock(&p->lock);
    sched_irq_restore(fl);
    return 0;
}

int64_t posix_sys_brk(posix_proc_t *p, uint64_t addr) {
    if (!p) return -ENOMEM;
    uint64_t fl = sched_irq_save();
    spin_lock(&p->lock);
    if (addr == 0 || addr < p->brk_base) {
        uint64_t cur = p->brk_cur;
        spin_unlock(&p->lock);
        sched_irq_restore(fl);
        return (int64_t)cur;
    }
    if (addr > p->brk_base + (64ull * 1024ull * 1024ull)) {
        uint64_t cur = p->brk_cur;
        spin_unlock(&p->lock);
        sched_irq_restore(fl);
        return (int64_t)cur;
    }

    uint64_t old_end = PAGE_UP(p->brk_cur);
    uint64_t new_end = PAGE_UP(addr);
    uint64_t pflags = prot_to_pte(PROT_READ | PROT_WRITE);

    if (new_end > old_end) {
        for (uint64_t va = old_end; va < new_end; va += PAGE_SZ) {
            uint64_t pa = vmm_alloc_frame();
            if (!pa || vmm_space_map_user(p->space, va, pa, pflags) != 0) {
                if (pa) vmm_free_frame(pa);
                uint64_t cur = p->brk_cur;
                spin_unlock(&p->lock);
                sched_irq_restore(fl);
                return (int64_t)cur;
            }
        }
        /* brk VMA'sini guncelle veya ekle */
        int found = 0;
        for (int i = 0; i < POSIX_MAX_VMAS; i++) {
            if (p->vmas[i].used && p->vmas[i].start == p->brk_base) {
                p->vmas[i].end = new_end;
                found = 1;
                break;
            }
        }
        if (!found) posix_vm_add_vma(p, p->brk_base, new_end, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1);
    } else if (new_end < old_end) {
        for (uint64_t va = new_end; va < old_end; va += PAGE_SZ) {
            uint64_t pte = vmm_space_get_user_pte(p->space, va);
            if (pte & VMM_PTE_P) {
                vmm_space_unmap_user(p->space, va);
                vmm_free_frame(pte & VMM_ADDR_MASK);
            }
        }
        for (int i = 0; i < POSIX_MAX_VMAS; i++) {
            if (p->vmas[i].used && p->vmas[i].start == p->brk_base) {
                if (new_end <= p->brk_base) p->vmas[i].used = 0;
                else p->vmas[i].end = new_end;
                break;
            }
        }
    }
    p->brk_cur = addr;
    spin_unlock(&p->lock);
    sched_irq_restore(fl);
    return (int64_t)addr;
}
