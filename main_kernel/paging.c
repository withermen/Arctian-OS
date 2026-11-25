#include "paging.h"
#include "serial.h"
#include "isr.h"

/* C3 TLB shootdown (kernel_plus/sched.c): IPI ile diger CPU'larda CR3
 * yenile. Zayif sembol: main_kernel-tek (boot) imajda NULL kalir.
 * UYARI: IPI beklemesi milisaniye surebilir; vmm_unmap kilit altindayken
 * cagrilirmisa karsi CPU'nun ayni kilidi beklememesi gerekir (C2 esyasamali
 * donemde unmap yollari kilit altindan gelmiyor; B hattinda yeniden dogrula). */
extern void smp_tlb_shootdown_all(void) __attribute__((weak));
extern uint32_t percpu_self_index(void) __attribute__((weak));

#define VMM_PCPU_MAX_CPUS 16u
#define VMM_PCPU_MAG_CAP  32u
#define VMM_PCPU_REFILL   16u
#define VMM_MAG_FREED_BIT 1ull

typedef struct {
    volatile uint32_t lock;
    uint32_t count;
    uint64_t frames[VMM_PCPU_MAG_CAP];
} vmm_pcpu_mag_t;

static vmm_pcpu_mag_t s_pcpu_mags[VMM_PCPU_MAX_CPUS];

static inline uint64_t rd_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3,%0" : "=r"(v));
    return v;
}

static inline void wr_cr3(uint64_t v) {
    __asm__ volatile("mov %0,%%cr3" :: "r"(v) : "memory");
}

static inline void invlpg(uint64_t va) {
    __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
}

static uint64_t *tbl(uint64_t phys) { return (uint64_t *)(uintptr_t)phys; }

static void zero_frame(uint64_t phys) {
    uint64_t *p = tbl(phys);
    for (int i = 0; i < 512; i++) p[i] = 0;
}

static volatile uint32_t s_vmm_lock;

static inline uint64_t vmm_lock_acq(void) {
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    while (__atomic_exchange_n(&s_vmm_lock, 1u, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause" ::: "memory");
    return f;
}

static inline void vmm_lock_rel(uint64_t f) {
    __atomic_store_n(&s_vmm_lock, 0u, __ATOMIC_RELEASE);
    if (f & 0x200ull) __asm__ volatile("sti" ::: "memory");
}

static inline uint16_t *vmm_refs(void) {
    vmm_state_t *st = VMM_STATE;
    return (uint16_t *)(uintptr_t)st->refcount_addr;
}

static inline int64_t vmm_frame_idx(uint64_t phys) {
    vmm_state_t *st = VMM_STATE;
    uint64_t idx;
    if (phys >= st->frames_base && phys < st->pool_end) {
        idx = (phys - st->frames_base) >> 12;
    } else if (st->high_end > st->high_base &&
               phys >= st->high_base && phys < st->high_end) {
        idx = st->low_frame_count + ((phys - st->high_base) >> 12);
    } else {
        return -1;
    }
    if (idx >= st->refcount_count) return -1;
    return (int64_t)idx;
}

static inline uint64_t vmm_total_frames(const vmm_state_t *st) {
    uint64_t total = st->low_frame_count;
    if (st->high_end > st->high_base)
        total += (st->high_end - st->high_base) >> 12;
    return total;
}

static inline int vmm_pcpu_active(uint32_t *cpu_out) {
    if (!percpu_self_index) return 0;
    uint32_t ci = percpu_self_index();
    if (ci >= VMM_PCPU_MAX_CPUS) return 0;
    *cpu_out = ci;
    return 1;
}

static inline uint64_t mag_lock_acq(vmm_pcpu_mag_t *m) {
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    while (__atomic_exchange_n(&m->lock, 1u, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause" ::: "memory");
    return f;
}

static inline void mag_lock_rel(vmm_pcpu_mag_t *m, uint64_t f) {
    __atomic_store_n(&m->lock, 0u, __ATOMIC_RELEASE);
    if (f & 0x200ull) __asm__ volatile("sti" ::: "memory");
}

/* Global havuzdan tek bir ham cerceve ceker (s_vmm_lock tutulmali).
 * *from_free_out: 1 = free_head'den alindi (frames_free'den dusulmedi);
 *                 0 = next_frame/high_next'ten alindi. */
static uint64_t global_pop_raw_locked(vmm_state_t *st, int *from_free_out) {
    uint64_t p = 0;
    *from_free_out = 0;
    if (st->free_head) {
        p = st->free_head;
        st->free_head = *(uint64_t *)(uintptr_t)p;
        *from_free_out = 1;
    } else if (st->next_frame + 0x1000ull <= st->pool_end) {
        p = st->next_frame;
        st->next_frame += 0x1000ull;
    } else if (st->high_end > st->high_base &&
               st->high_next + 0x1000ull <= st->high_end) {
        p = st->high_next;
        st->high_next += 0x1000ull;
    }
    return p;
}

static void global_push_raw_locked(vmm_state_t *st, uint64_t entry) {
    uint64_t phys = entry & ~0xFFFull;
    *(uint64_t *)(uintptr_t)phys = st->free_head;
    st->free_head = phys;
    if (!(entry & VMM_MAG_FREED_BIT)) {
        __atomic_fetch_add(&st->frames_free, 1ull, __ATOMIC_RELAXED);
    }
}

static void flush_one_mag(vmm_state_t *st, uint32_t ci, uint32_t keep) {
    uint64_t buf[VMM_PCPU_MAG_CAP];
    uint32_t n = 0;
    vmm_pcpu_mag_t *m = &s_pcpu_mags[ci];
    uint64_t mf = mag_lock_acq(m);
    while (m->count > keep && n < VMM_PCPU_MAG_CAP) {
        buf[n++] = m->frames[--m->count];
    }
    mag_lock_rel(m, mf);
    if (n == 0) return;

    uint64_t vf = vmm_lock_acq();
    for (uint32_t i = 0; i < n; i++) {
        global_push_raw_locked(st, buf[i]);
    }
    vmm_lock_rel(vf);
}

void vmm_pcpu_flush_all(void) {
    vmm_state_t *st = VMM_STATE;
    if (!st || !st->ready) return;
    for (uint32_t c = 0; c < VMM_PCPU_MAX_CPUS; c++) {
        if (__atomic_load_n(&s_pcpu_mags[c].count, __ATOMIC_RELAXED) > 0) {
            flush_one_mag(st, c, 0u);
        }
    }
}

static uint64_t vmm_alloc_frame_impl(int allow_emergency) {
    vmm_state_t *st = VMM_STATE;
    if (!st->ready) return 0;

    uint64_t total = vmm_total_frames(st);
    if (!allow_emergency && st->emergency_reserve_frames > 0) {
        uint64_t used = __atomic_load_n(&st->frames_used, __ATOMIC_RELAXED);
        if (used + st->emergency_reserve_frames >= total) {
            vmm_pcpu_flush_all();
            used = __atomic_load_n(&st->frames_used, __ATOMIC_RELAXED);
            if (used + st->emergency_reserve_frames >= total) {
                __atomic_store_n(&st->oom_latched, 1u, __ATOMIC_RELEASE);
                return 0;
            }
        }
    }

    uint32_t ci = 0;
    int use_mag = vmm_pcpu_active(&ci);
    if (use_mag) {
        vmm_pcpu_mag_t *m = &s_pcpu_mags[ci];
        uint64_t mf = mag_lock_acq(m);
        if (m->count > 0) {
            uint64_t entry = m->frames[--m->count];
            mag_lock_rel(m, mf);
            uint64_t p = entry & ~0xFFFull;
            if (entry & VMM_MAG_FREED_BIT) {
                /* frames_free zaten bu cerceveyi sayiyordu */
                uint64_t oldf = __atomic_load_n(&st->frames_free, __ATOMIC_RELAXED);
                while (oldf > 0 && !__atomic_compare_exchange_n(&st->frames_free, &oldf, oldf - 1ull,
                                                                1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
            }
            __atomic_fetch_add(&st->frames_used, 1ull, __ATOMIC_RELAXED);
            int64_t idx = vmm_frame_idx(p);
            if (idx >= 0) __atomic_store_n(&vmm_refs()[idx], 1u, __ATOMIC_RELEASE);
            zero_frame(p);
            return p;
        }
        mag_lock_rel(m, mf);
    }

    uint64_t batch[VMM_PCPU_REFILL];
    uint32_t batch_n = 0;
    uint64_t p = 0;

    uint64_t f = vmm_lock_acq();
    if (!allow_emergency && st->emergency_reserve_frames > 0) {
        uint64_t used = __atomic_load_n(&st->frames_used, __ATOMIC_RELAXED);
        if (used + st->emergency_reserve_frames >= total) {
            vmm_lock_rel(f);
            __atomic_store_n(&st->oom_latched, 1u, __ATOMIC_RELEASE);
            return 0;
        }
    }
    int from_free = 0;
    p = global_pop_raw_locked(st, &from_free);
    if (p) {
        if (from_free) {
            uint64_t oldf = __atomic_load_n(&st->frames_free, __ATOMIC_RELAXED);
            while (oldf > 0 && !__atomic_compare_exchange_n(&st->frames_free, &oldf, oldf - 1ull,
                                                            1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
        }
        __atomic_fetch_add(&st->frames_used, 1ull, __ATOMIC_RELAXED);
        int64_t idx = vmm_frame_idx(p);
        if (idx >= 0) __atomic_store_n(&vmm_refs()[idx], 1u, __ATOMIC_RELEASE);

        /* Yeterli bos RAM varsa per-CPU magazini toplu doldur */
        if (use_mag) {
            uint64_t used_now = __atomic_load_n(&st->frames_used, __ATOMIC_RELAXED);
            uint64_t free_rem = (total > used_now) ? (total - used_now) : 0;
            uint64_t guard = st->oom_low_frames ? st->oom_low_frames : st->emergency_reserve_frames;
            if (free_rem > guard + VMM_PCPU_REFILL) {
                for (uint32_t k = 1; k < VMM_PCPU_REFILL; k++) {
                    int ff = 0;
                    uint64_t q = global_pop_raw_locked(st, &ff);
                    if (!q) break;
                    batch[batch_n++] = q | (ff ? VMM_MAG_FREED_BIT : 0ull);
                }
            }
        }
    }
    vmm_lock_rel(f);

    if (!p) {
        /* Diger CPU magazinlerindeki cerceveleri global havuza bosalt ve tekrar dene */
        vmm_pcpu_flush_all();
        f = vmm_lock_acq();
        if (!allow_emergency && st->emergency_reserve_frames > 0) {
            uint64_t used = __atomic_load_n(&st->frames_used, __ATOMIC_RELAXED);
            if (used + st->emergency_reserve_frames >= total) {
                vmm_lock_rel(f);
                __atomic_store_n(&st->oom_latched, 1u, __ATOMIC_RELEASE);
                return 0;
            }
        }
        p = global_pop_raw_locked(st, &from_free);
        if (p) {
            if (from_free) {
                uint64_t oldf = __atomic_load_n(&st->frames_free, __ATOMIC_RELAXED);
                while (oldf > 0 && !__atomic_compare_exchange_n(&st->frames_free, &oldf, oldf - 1ull,
                                                                1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
            }
            __atomic_fetch_add(&st->frames_used, 1ull, __ATOMIC_RELAXED);
            int64_t idx = vmm_frame_idx(p);
            if (idx >= 0) __atomic_store_n(&vmm_refs()[idx], 1u, __ATOMIC_RELEASE);
        } else if (!allow_emergency) {
            __atomic_store_n(&st->oom_latched, 1u, __ATOMIC_RELEASE);
        }
        vmm_lock_rel(f);
    }

    if (batch_n > 0 && use_mag) {
        vmm_pcpu_mag_t *m = &s_pcpu_mags[ci];
        uint64_t mf = mag_lock_acq(m);
        uint32_t pushed = 0;
        while (pushed < batch_n && m->count < VMM_PCPU_MAG_CAP) {
            m->frames[m->count++] = batch[pushed++];
        }
        mag_lock_rel(m, mf);
        if (pushed < batch_n) {
            f = vmm_lock_acq();
            for (uint32_t j = pushed; j < batch_n; j++) {
                global_push_raw_locked(st, batch[j]);
            }
            vmm_lock_rel(f);
        }
    }

    if (p) zero_frame(p);
    return p;
}

uint64_t vmm_alloc_frame(void) {
    return vmm_alloc_frame_impl(0);
}

uint64_t vmm_alloc_frame_emergency(void) {
    return vmm_alloc_frame_impl(1);
}

void vmm_frame_ref(uint64_t phys) {
    vmm_state_t *st = VMM_STATE;
    if (!st->ready || !phys || (phys & 0xFFFull)) return;
    int64_t idx = vmm_frame_idx(phys);
    if (idx < 0) return;
    uint16_t *rp = &vmm_refs()[idx];
    uint16_t cur = __atomic_load_n(rp, __ATOMIC_RELAXED);
    while (cur < 0xFFFEu) {
        if (__atomic_compare_exchange_n(rp, &cur, (uint16_t)(cur + 1u),
                                        1, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            break;
    }
}

uint16_t vmm_frame_refcount(uint64_t phys) {
    int64_t idx = vmm_frame_idx(phys);
    if (idx < 0) return 0;
    return __atomic_load_n(&vmm_refs()[idx], __ATOMIC_ACQUIRE);
}

void vmm_free_frame(uint64_t phys) {
    vmm_state_t *st = VMM_STATE;
    if (!st->ready || !phys || (phys & 0xFFFull)) return;
    int64_t idx = vmm_frame_idx(phys);
    if (idx < 0) return;

    uint16_t *rp = &vmm_refs()[idx];
    uint16_t cur = __atomic_load_n(rp, __ATOMIC_ACQUIRE);
    for (;;) {
        if (cur <= 1u) {
            if (__atomic_compare_exchange_n(rp, &cur, 0u,
                                            1, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
                break;
        } else {
            if (__atomic_compare_exchange_n(rp, &cur, (uint16_t)(cur - 1u),
                                            1, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
                return;
        }
    }

    uint32_t ci = 0;
    if (vmm_pcpu_active(&ci)) {
        vmm_pcpu_mag_t *m = &s_pcpu_mags[ci];
        uint64_t mf = mag_lock_acq(m);
        if (m->count >= VMM_PCPU_MAG_CAP) {
            mag_lock_rel(m, mf);
            flush_one_mag(st, ci, VMM_PCPU_MAG_CAP / 2u);
            mf = mag_lock_acq(m);
        }
        if (m->count < VMM_PCPU_MAG_CAP) {
            m->frames[m->count++] = phys | VMM_MAG_FREED_BIT;
            mag_lock_rel(m, mf);
            __atomic_fetch_add(&st->frames_free, 1ull, __ATOMIC_RELAXED);
            uint64_t old_used = __atomic_load_n(&st->frames_used, __ATOMIC_RELAXED);
            while (old_used > 0 && !__atomic_compare_exchange_n(&st->frames_used, &old_used, old_used - 1ull,
                                                                1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
            uint64_t total = vmm_total_frames(st);
            if (total > old_used && (total - old_used) > st->oom_crit_frames)
                __atomic_store_n(&st->oom_latched, 0u, __ATOMIC_RELEASE);
            return;
        }
        mag_lock_rel(m, mf);
    }

    uint64_t f = vmm_lock_acq();
    *(uint64_t *)(uintptr_t)phys = st->free_head;
    st->free_head = phys;
    __atomic_fetch_add(&st->frames_free, 1ull, __ATOMIC_RELAXED);
    uint64_t old_used = __atomic_load_n(&st->frames_used, __ATOMIC_RELAXED);
    while (old_used > 0 && !__atomic_compare_exchange_n(&st->frames_used, &old_used, old_used - 1ull,
                                                        1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    uint64_t total = vmm_total_frames(st);
    if (total > old_used && (total - old_used) > st->oom_crit_frames)
        __atomic_store_n(&st->oom_latched, 0u, __ATOMIC_RELEASE);
    vmm_lock_rel(f);
}

int vmm_oom_pressure(void) {
    vmm_state_t *st = VMM_STATE;
    if (!st || st->magic != VMM_MAGIC || !st->ready) return VMM_OOM_OK;
    int32_t forced = __atomic_load_n(&st->oom_force, __ATOMIC_ACQUIRE);
    if (forced > 0) return (forced >= VMM_OOM_CRITICAL) ? VMM_OOM_CRITICAL : VMM_OOM_LOW;

    uint64_t total = vmm_total_frames(st);
    uint64_t used = __atomic_load_n(&st->frames_used, __ATOMIC_RELAXED);
    uint64_t free_f = (total > used) ? (total - used) : 0;
    if (__atomic_load_n(&st->oom_latched, __ATOMIC_ACQUIRE) || free_f <= st->oom_crit_frames)
        return VMM_OOM_CRITICAL;
    if (free_f <= st->oom_low_frames)
        return VMM_OOM_LOW;
    return VMM_OOM_OK;
}

void vmm_oom_set_force(int level) {
    vmm_state_t *st = VMM_STATE;
    if (!st || st->magic != VMM_MAGIC) return;
    if (level < 0) level = 0;
    if (level > VMM_OOM_CRITICAL) level = VMM_OOM_CRITICAL;
    __atomic_store_n(&st->oom_force, (int32_t)level, __ATOMIC_RELEASE);
}

uint64_t vmm_avail_above_reserve_bytes(void) {
    vmm_state_t *st = VMM_STATE;
    if (!st || st->magic != VMM_MAGIC || !st->ready) return 0;
    uint64_t total = vmm_total_frames(st);
    uint64_t used = __atomic_load_n(&st->frames_used, __ATOMIC_RELAXED);
    uint64_t free_f = (total > used) ? (total - used) : 0;
    if (free_f <= st->emergency_reserve_frames) return 0;
    return (free_f - st->emergency_reserve_frames) * 0x1000ull;
}

uint32_t vmm_space_ram_kb(vmm_space_t sp) {
    if (!sp || (sp & 0xFFFull)) return 0;
    vmm_state_t *st = VMM_STATE;
    if (!st || !st->ready || sp == st->kernel_space) return 0;
    uint64_t pages = 1; /* PML4 */
    uint64_t *pml4 = tbl(sp);
    for (int i4 = (int)VMM_USER_PML4_INDEX; i4 < (int)VMM_HIGHER_PML4_INDEX; i4++) {
        uint64_t e4 = pml4[i4];
        if (!(e4 & VMM_PTE_P) || (e4 & VMM_PTE_HUGE)) continue;
        pages++;
        uint64_t *pdpt = tbl(e4 & VMM_ADDR_MASK);
        for (int i3 = 0; i3 < 512; i3++) {
            uint64_t e3 = pdpt[i3];
            if (!(e3 & VMM_PTE_P) || (e3 & VMM_PTE_HUGE)) continue;
            pages++;
            uint64_t *pd = tbl(e3 & VMM_ADDR_MASK);
            for (int i2 = 0; i2 < 512; i2++) {
                uint64_t e2 = pd[i2];
                if (!(e2 & VMM_PTE_P) || (e2 & VMM_PTE_HUGE)) continue;
                pages++;
                uint64_t *pt = tbl(e2 & VMM_ADDR_MASK);
                for (int i1 = 0; i1 < 512; i1++) {
                    if (pt[i1] & VMM_PTE_P) pages++;
                }
            }
        }
    }
    return (uint32_t)(pages * 4ull);
}

void vmm_mem_info(vmm_meminfo_t *out) {
    if (!out) return;
    vmm_state_t *st = VMM_STATE;
    uint64_t total = 0, used = 0;
    if (st->ready) {
        total = st->low_frame_count;
        if (st->high_end > st->high_base)
            total += (st->high_end - st->high_base) >> 12;
        used = st->frames_used;
        if (used > total) used = total;
    }
    out->frames_total = total;
    out->frames_used = used;
    out->frames_free = total - used;
    out->total_bytes = total * 0x1000ull;
    out->used_bytes = used * 0x1000ull;
    out->free_bytes = out->frames_free * 0x1000ull;
}

static inline void vmm_outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}

static inline uint8_t vmm_inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static uint8_t cmos_read_reg(uint8_t reg) {
    vmm_outb(0x70, (uint8_t)(reg | 0x80u));
    (void)vmm_inb(0x80);
    return vmm_inb(0x71);
}

/* Takili fiziksel RAM tespiti:
 * 1) UEFI boot yolu: 0x7100 arctian_uefiinfo_t bellek haritasi.
 * 2) BIOS yolu (QEMU SeaBIOS / Bochs / VirtualBox / PC):
 *    CMOS 0x34/0x35 (16 MB ustu, 64 KB birim) + 0x5B..0x5D (4 GB ustu, 64 KB birim). */
static void detect_phys_ram(uint64_t *low_end_out, uint64_t *high_size_out) {
    uint64_t low_end = 0;
    uint64_t high_sz = 0;

    /* 1) UEFI bellek haritasi kontrolu (0x7100, "UEFI") */
    volatile uint32_t *ui = (volatile uint32_t *)(uintptr_t)0x7100u;
    if (ui[0] == 0x49464555u && ui[1] >= 0x48u) {
        uint64_t mm_addr = *(volatile uint64_t *)(uintptr_t)(0x7100u + 0x08u);
        uint32_t mm_cnt  = *(volatile uint32_t *)(uintptr_t)(0x7100u + 0x18u);
        uint32_t mm_dsz  = *(volatile uint32_t *)(uintptr_t)(0x7100u + 0x1Cu);
        if (mm_addr && mm_addr < 0x3FF00000ull && mm_cnt > 0 && mm_cnt < 2048u &&
            mm_dsz >= 40u && mm_dsz <= 256u) {
            uint64_t best_low = 0;
            uint64_t best_hi_end = 0x100000000ull;
            for (uint32_t i = 0; i < mm_cnt; i++) {
                uintptr_t d = (uintptr_t)(mm_addr + (uint64_t)i * mm_dsz);
                uint32_t type = *(volatile uint32_t *)(d + 0);
                uint64_t pstart = *(volatile uint64_t *)(d + 8);
                uint64_t npages = *(volatile uint64_t *)(d + 24);
                /* EfiLoaderCode(1), EfiLoaderData(2), EfiBootServicesCode(3),
                 * EfiBootServicesData(4), EfiConventionalMemory(7) */
                if (type != 7u && type != 3u && type != 4u && type != 1u && type != 2u)
                    continue;
                uint64_t pend = pstart + npages * 0x1000ull;
                if (pend <= 0xC0000000ull) {
                    if (pend > best_low) best_low = pend;
                } else if (pstart >= 0x100000000ull) {
                    if (pend > best_hi_end) best_hi_end = pend;
                }
            }
            if (best_low > (uint64_t)VMM_POOL_BASE + 0x1000000ull) {
                low_end = best_low;
                if (best_hi_end > 0x100000000ull)
                    high_sz = best_hi_end - 0x100000000ull;
            }
        }
    }

    /* 2) BIOS CMOS (SeaBIOS / QEMU / Bochs / VirtualBox) */
    if (low_end == 0) {
        uint64_t ext16 = ((uint64_t)cmos_read_reg(0x35) << 8) |
                         (uint64_t)cmos_read_reg(0x34);
        if (ext16 > 0) {
            low_end = (16ull * 1024ull * 1024ull) + ext16 * 65536ull;
        }
        uint64_t ext4g = (uint64_t)cmos_read_reg(0x5B) |
                         ((uint64_t)cmos_read_reg(0x5C) << 8) |
                         ((uint64_t)cmos_read_reg(0x5D) << 16);
        if (ext4g > 0) {
            high_sz = ext4g * 65536ull;
        }
        /* NMI'yi tekrar ac (CMOS index 0x0D) */
        vmm_outb(0x70, 0x0Du);
        (void)vmm_inb(0x71);
    }

    /* Guvenli yedek (CMOS yoksa / beklenmedik kucuk deger) */
    if (low_end < 0x10000000ull) {
        low_end = 0x20000000ull; /* 512 MB varsayilan */
        high_sz = 0;
    }

    /* PCI MMIO / VESA LFB (3..4 GB) bolgesine asla tasma */
    if (low_end > 0xC0000000ull) low_end = 0xC0000000ull;
    /* Yuksek bellegi refcount tablosunun (<1 GB) makul kalmasi icin 16 GB ile sinirla */
    if (high_sz > (16ull * 1024ull * 1024ull * 1024ull))
        high_sz = 16ull * 1024ull * 1024ull * 1024ull;

    *low_end_out = low_end;
    *high_size_out = high_sz;
}

static int map_gib_2m(vmm_state_t *st, uint64_t gib_idx) {
    if (gib_idx == 0 || gib_idx == 3 || gib_idx >= 512) return 0;
    uint64_t *kpdpt = tbl(st->kernel_pdpt);
    uint64_t *hpdpt = tbl(st->pdpt_hi);
    uint64_t pd = vmm_alloc_frame();
    if (!pd) return -1;
    uint64_t base = gib_idx << 30;
    for (uint64_t i = 0; i < 512; i++) {
        tbl(pd)[i] = (base + (i << 21)) | VMM_PTE_P | VMM_PTE_W | VMM_PTE_HUGE;
    }
    kpdpt[gib_idx] = pd | VMM_PTE_P | VMM_PTE_W;
    hpdpt[gib_idx] = pd | VMM_PTE_P | VMM_PTE_W;
    return 0;
}

void vmm_init(void) {
    vmm_state_t *st = VMM_STATE;
    uint64_t cr3 = rd_cr3() & VMM_ADDR_MASK;

    if (st->magic == VMM_MAGIC && st->ready) {
        st->kernel_space = cr3;
        return;
    }

    uint64_t low_ram_end = 0;
    uint64_t high_ram_sz = 0;
    detect_phys_ram(&low_ram_end, &high_ram_sz);

    /* SeaBIOS ACPI/SMBIOS tablolari dusuk RAM'in en ustunde yer alir; emniyet payi birak. */
    uint64_t low_reserve = (low_ram_end >= 0x20000000ull) ? (16ull * 1024ull * 1024ull)
                                                          : (4ull * 1024ull * 1024ull);
    uint64_t low_pool_end = (low_ram_end - low_reserve) & ~0x1FFFFFull;
    if (low_pool_end < (uint64_t)VMM_POOL_BASE + (16ull * 1024ull * 1024ull))
        low_pool_end = (uint64_t)VMM_POOL_BASE + (16ull * 1024ull * 1024ull);

    uint64_t high_base = 0, high_end = 0;
    if (high_ram_sz > (32ull * 1024ull * 1024ull)) {
        high_base = 0x100000000ull;
        high_end = (high_base + high_ram_sz - (16ull * 1024ull * 1024ull)) & ~0x1FFFFFull;
    }

    /* Dinamik refcount tablosunu VMM_POOL_BASE (<1 GB, zaten kimlik esli) basina yerlestir */
    uint64_t raw_low = low_pool_end - (uint64_t)VMM_POOL_BASE;
    uint64_t raw_high = (high_end > high_base) ? (high_end - high_base) : 0;
    uint64_t est_frames = (raw_low + raw_high) >> 12;
    uint64_t ref_bytes = ((est_frames * sizeof(uint16_t)) + 0xFFFull) & ~0xFFFull;
    uint64_t frames_base = (uint64_t)VMM_POOL_BASE + ref_bytes;

    /* 1 GB sinirina kadar erken havuz: yeni PD sayfalari mutlaka <1 GB icinden ayrilir */
    uint64_t early_end = low_pool_end;
    if (early_end > 0x40000000ull) early_end = 0x40000000ull;
    if (frames_base + (4ull * 1024ull * 1024ull) >= early_end) {
        /* Yuksek RAM cok buyukse refcount tablosu dusuk havuzu tuketmesin */
        high_base = 0;
        high_end = 0;
        raw_high = 0;
        est_frames = raw_low >> 12;
        ref_bytes = ((est_frames * sizeof(uint16_t)) + 0xFFFull) & ~0xFFFull;
        frames_base = (uint64_t)VMM_POOL_BASE + ref_bytes;
    }

    zero_frame(VMM_STATE_ADDR);
    for (uint64_t a = (uint64_t)VMM_POOL_BASE; a < frames_base; a += 0x1000ull)
        zero_frame(a);

    uint64_t low_frames = (low_pool_end - frames_base) >> 12;
    uint64_t high_frames = raw_high >> 12;

    st->magic = VMM_MAGIC;
    st->phys_ram_total = low_ram_end + high_ram_sz;
    st->refcount_addr = (uint64_t)VMM_POOL_BASE;
    st->refcount_count = low_frames + high_frames;
    st->frames_base = frames_base;
    st->low_frame_count = low_frames;
    st->next_frame = frames_base;
    st->pool_end = early_end;          /* once <1 GB; 2 MB PD'ler kurulunca tam havuz acilir */
    st->high_base = 0;
    st->high_next = 0;
    st->high_end = 0;
    st->identity_mapped_end = 0x40000000ull;
    st->frames_used = 0;
    st->frames_free = 0;
    st->free_head = 0;
    st->killed_tasks = 0;
    st->kernel_space = cr3;
    st->kernel_pdpt = tbl(cr3)[0] & VMM_ADDR_MASK;
    st->ready = 1;

    uint64_t pdpt_hi = vmm_alloc_frame();
    uint64_t pd_hi = vmm_alloc_frame();
    if (!pdpt_hi || !pd_hi) {
        serial_puts("[vmm] frame alloc fail\n");
        for (;;) __asm__ volatile("hlt");
    }
    for (int i = 0; i < 512; i++) {
        tbl(pd_hi)[i] = ((uint64_t)i << 21) | VMM_PTE_P | VMM_PTE_W | VMM_PTE_HUGE;
    }
    tbl(pdpt_hi)[0] = pd_hi | VMM_PTE_P | VMM_PTE_W;
    tbl(cr3)[VMM_HIGHER_PML4_INDEX] = pdpt_hi | VMM_PTE_P | VMM_PTE_W;
    st->pdpt_hi = pdpt_hi;

    /* >1 GB dusuk RAM (1..3 GB) icin 2 MB kimlik + higher-half eslemelerini genislet */
    uint64_t mapped_end = 0x40000000ull;
    for (uint64_t g = 1; g < 3; g++) {
        if (low_pool_end > (g << 30)) {
            if (map_gib_2m(st, g) == 0) {
                mapped_end = (g + 1) << 30;
            } else {
                low_pool_end = g << 30;
                break;
            }
        }
    }

    /* >4 GB yuksek RAM icin 2 MB kimlik + higher-half eslemelerini genislet */
    if (high_end > high_base) {
        uint64_t hi_g_end = (high_end + 0x3FFFFFFFull) >> 30;
        for (uint64_t g = 4; g < hi_g_end; g++) {
            if (map_gib_2m(st, g) == 0) {
                mapped_end = (g + 1) << 30;
            } else {
                high_end = g << 30;
                if (high_end <= high_base) { high_base = 0; high_end = 0; }
                break;
            }
        }
    }

    /* Yeni PDPT/PD girisleri icin TLB'yi tazele */
    wr_cr3(cr3);

    /* IA32_PAT: Write-Combining (WC) bellek tipi (PA1=WC, PA2=UC-, PA3=UC) */
    st->pat_enabled = (uint32_t)vmm_pat_init_cpu();

    /* Tam havuzu etkinlestir */
    st->pool_end = low_pool_end;
    st->low_frame_count = (low_pool_end > frames_base) ? ((low_pool_end - frames_base) >> 12) : 0;
    st->high_base = high_base;
    st->high_next = high_base;
    st->high_end = high_end;
    st->refcount_count = st->low_frame_count +
                         ((high_end > high_base) ? ((high_end - high_base) >> 12) : 0);
    st->identity_mapped_end = mapped_end;

    /* E1: Acil durum rezervi (4-8 MB) ve OOM basinc esikleri */
    uint64_t total_f = st->refcount_count;
    uint64_t res_f = (total_f >= 65536ull) ? 2048ull : 1024ull; /* 8 MB / 4 MB */
    if (res_f > total_f / 8ull) res_f = total_f / 8ull;
    uint64_t low_f = total_f / 32ull;
    if (low_f < 4096ull) low_f = 4096ull; /* >= 16 MB */
    if (low_f > total_f / 2ull) low_f = total_f / 2ull;
    uint64_t crit_f = res_f + 1024ull;    /* rezerv + 4 MB */
    if (crit_f >= low_f) crit_f = (low_f > res_f) ? (res_f + (low_f - res_f) / 2ull) : res_f;
    st->emergency_reserve_frames = res_f;
    st->oom_low_frames = low_f;
    st->oom_crit_frames = crit_f;
    st->oom_force = 0;
    st->oom_latched = 0;

    uint64_t pool_mb = (st->refcount_count * 4096ull) / (1024ull * 1024ull);
    uint64_t ram_mb  = st->phys_ram_total / (1024ull * 1024ull);
    uint64_t hi_mb   = (high_end > high_base) ? ((high_end - high_base) / (1024ull * 1024ull)) : 0;
    serial_printf("[vmm] RAM=%uMB pool=%uMB (low=0x%08X..0x%08X high=%uMB frames=%u) PAT=%s higher-half aktif\n",
                  (unsigned)ram_mb, (unsigned)pool_mb,
                  (unsigned)frames_base, (unsigned)low_pool_end,
                  (unsigned)hi_mb, (unsigned)st->refcount_count,
                  st->pat_enabled ? "WC" : "off");
}

vmm_space_t vmm_new_space(void) {
    vmm_state_t *st = VMM_STATE;
    if (!st->ready) return 0;
    uint64_t pml4 = vmm_alloc_frame();
    if (!pml4) return 0;
    tbl(pml4)[0] = st->kernel_pdpt | VMM_PTE_P | VMM_PTE_W;
    tbl(pml4)[VMM_HIGHER_PML4_INDEX] = st->pdpt_hi | VMM_PTE_P | VMM_PTE_W;
    st->space_count++;
    return pml4;
}

int vmm_map(vmm_space_t sp, uint64_t va, uint64_t pa, uint64_t flags) {
    if (!sp || (va & 0xFFFull) || (pa & 0xFFFull)) return -1;
    uint64_t *pml4 = tbl(sp);
    int i4 = (int)((va >> 39) & 0x1FF);
    int i3 = (int)((va >> 30) & 0x1FF);
    int i2 = (int)((va >> 21) & 0x1FF);
    int i1 = (int)((va >> 12) & 0x1FF);

    if (!(pml4[i4] & VMM_PTE_P)) {
        uint64_t t = vmm_alloc_frame();
        if (!t) return -3;
        pml4[i4] = t | VMM_PTE_P | VMM_PTE_W | VMM_PTE_U;
    } else if (pml4[i4] & VMM_PTE_HUGE) {
        return -2;
    }
    uint64_t *pdpt = tbl(pml4[i4] & VMM_ADDR_MASK);

    if (!(pdpt[i3] & VMM_PTE_P)) {
        uint64_t t = vmm_alloc_frame();
        if (!t) return -3;
        pdpt[i3] = t | VMM_PTE_P | VMM_PTE_W | VMM_PTE_U;
    } else if (pdpt[i3] & VMM_PTE_HUGE) {
        return -2;
    }
    uint64_t *pd = tbl(pdpt[i3] & VMM_ADDR_MASK);

    if (!(pd[i2] & VMM_PTE_P)) {
        uint64_t t = vmm_alloc_frame();
        if (!t) return -3;
        pd[i2] = t | VMM_PTE_P | VMM_PTE_W | VMM_PTE_U;
    } else if (pd[i2] & VMM_PTE_HUGE) {
        return -2;
    }
    uint64_t *pt = tbl(pd[i2] & VMM_ADDR_MASK);

    pt[i1] = (pa & VMM_ADDR_MASK) | flags | VMM_PTE_P;
    invlpg(va);
    return 0;
}

int vmm_unmap(vmm_space_t sp, uint64_t va) {
    if (!sp) return -1;
    uint64_t *pml4 = tbl(sp);
    int i4 = (int)((va >> 39) & 0x1FF);
    int i3 = (int)((va >> 30) & 0x1FF);
    int i2 = (int)((va >> 21) & 0x1FF);
    int i1 = (int)((va >> 12) & 0x1FF);

    if (!(pml4[i4] & VMM_PTE_P) || (pml4[i4] & VMM_PTE_HUGE)) return -1;
    uint64_t *pdpt = tbl(pml4[i4] & VMM_ADDR_MASK);
    if (!(pdpt[i3] & VMM_PTE_P) || (pdpt[i3] & VMM_PTE_HUGE)) return -1;
    uint64_t *pd = tbl(pdpt[i3] & VMM_ADDR_MASK);
    if (!(pd[i2] & VMM_PTE_P) || (pd[i2] & VMM_PTE_HUGE)) return -1;
    uint64_t *pt = tbl(pd[i2] & VMM_ADDR_MASK);
    if (!(pt[i1] & VMM_PTE_P)) return -1;
    pt[i1] = 0;
    invlpg(va);
    /* SMP (C3): invlpg yalniz bu islemcinin TLB'sini temizler; diger
     * cevrimici CPU'lara IPI ile CR3 yenileme attirilir. main_kernel-tek
     * (boot) imajda sembol NULL, tek CPU'da gerek yok. */
    if (smp_tlb_shootdown_all) smp_tlb_shootdown_all();
    return 0;
}

int vmm_protect(vmm_space_t sp, uint64_t va, uint64_t flags) {
    if (!sp || (va & 0xFFFull)) return -1;
    uint64_t *pml4 = tbl(sp);
    int i4 = (int)((va >> 39) & 0x1FF);
    int i3 = (int)((va >> 30) & 0x1FF);
    int i2 = (int)((va >> 21) & 0x1FF);
    int i1 = (int)((va >> 12) & 0x1FF);

    if (!(pml4[i4] & VMM_PTE_P) || (pml4[i4] & VMM_PTE_HUGE)) return -1;
    uint64_t *pdpt = tbl(pml4[i4] & VMM_ADDR_MASK);
    if (!(pdpt[i3] & VMM_PTE_P) || (pdpt[i3] & VMM_PTE_HUGE)) return -1;
    uint64_t *pd = tbl(pdpt[i3] & VMM_ADDR_MASK);
    if (!(pd[i2] & VMM_PTE_P) || (pd[i2] & VMM_PTE_HUGE)) return -1;
    uint64_t *pt = tbl(pd[i2] & VMM_ADDR_MASK);
    if (!(pt[i1] & VMM_PTE_P)) return -1;
    uint64_t pa = pt[i1] & VMM_ADDR_MASK;
    pt[i1] = pa | flags | VMM_PTE_P;
    invlpg(va);
    if (smp_tlb_shootdown_all) smp_tlb_shootdown_all();
    return 0;
}

uint64_t vmm_get_pte(vmm_space_t sp, uint64_t va) {
    if (!sp) return 0;
    uint64_t *pml4 = tbl(sp);
    uint64_t e = pml4[(int)((va >> 39) & 0x1FF)];
    if (!(e & VMM_PTE_P) || (e & VMM_PTE_HUGE)) return 0;
    uint64_t *pdpt = tbl(e & VMM_ADDR_MASK);
    e = pdpt[(int)((va >> 30) & 0x1FF)];
    if (!(e & VMM_PTE_P) || (e & VMM_PTE_HUGE)) return 0;
    uint64_t *pd = tbl(e & VMM_ADDR_MASK);
    e = pd[(int)((va >> 21) & 0x1FF)];
    if (!(e & VMM_PTE_P) || (e & VMM_PTE_HUGE)) return 0;
    uint64_t *pt = tbl(e & VMM_ADDR_MASK);
    e = pt[(int)((va >> 12) & 0x1FF)];
    if (!(e & VMM_PTE_P)) return 0;
    return e;
}

void vmm_switch(vmm_space_t sp) {
    if (sp) wr_cr3(sp);
}

vmm_space_t vmm_current(void) {
    return rd_cr3() & VMM_ADDR_MASK;
}

uint64_t vmm_translate(vmm_space_t sp, uint64_t va) {
    if (!sp) return 0;
    uint64_t *pml4 = tbl(sp);
    int i4 = (int)((va >> 39) & 0x1FF);
    int i3 = (int)((va >> 30) & 0x1FF);
    int i2 = (int)((va >> 21) & 0x1FF);
    int i1 = (int)((va >> 12) & 0x1FF);

    uint64_t e = pml4[i4];
    if (!(e & VMM_PTE_P)) return 0;
    uint64_t *pdpt = tbl(e & VMM_ADDR_MASK);
    if (e & VMM_PTE_HUGE) return (e & VMM_ADDR_MASK) | (va & 0x3FFFFFFFull);

    e = pdpt[i3];
    if (!(e & VMM_PTE_P)) return 0;
    uint64_t *pd = tbl(e & VMM_ADDR_MASK);
    if (e & VMM_PTE_HUGE) return (e & VMM_ADDR_MASK) | (va & 0x3FFFFFFFull);

    e = pd[i2];
    if (!(e & VMM_PTE_P)) return 0;
    if (e & VMM_PTE_HUGE) return (e & VMM_ADDR_MASK) | (va & 0x1FFFFFull);

    uint64_t *pt = tbl(e & VMM_ADDR_MASK);
    e = pt[i1];
    if (!(e & VMM_PTE_P)) return 0;
    return (e & VMM_ADDR_MASK) | (va & 0xFFFull);
}

/* Kullanici erisimi gercekten haritali mi: yuruyus yapar, her seviyede
 * U (ve yazma isteniyorsa W) biti ister. Syscall denetimleri sanal
 * adres araligiyla yetinmemeli; haritasiz adresi kernel modunda kendimiz
 * yurutup cekirdegi cokertir, kernel eslemesi ise kullaniciya kernel
 * bellegi okutur. */
int vmm_probe_user(vmm_space_t sp, uint64_t va, int write) {
    if (!sp) return 0;
    uint64_t *pml4 = tbl(sp);
    uint64_t e = pml4[(int)((va >> 39) & 0x1FF)];
    if (!(e & VMM_PTE_P) || !(e & VMM_PTE_U)) return 0;
    if (write && !(e & VMM_PTE_W)) return 0;
    if (e & VMM_PTE_HUGE) return 1;

    uint64_t *pdpt = tbl(e & VMM_ADDR_MASK);
    e = pdpt[(int)((va >> 30) & 0x1FF)];
    if (!(e & VMM_PTE_P) || !(e & VMM_PTE_U)) return 0;
    if (write && !(e & VMM_PTE_W)) return 0;
    if (e & VMM_PTE_HUGE) return 1;

    uint64_t *pd = tbl(e & VMM_ADDR_MASK);
    e = pd[(int)((va >> 21) & 0x1FF)];
    if (!(e & VMM_PTE_P) || !(e & VMM_PTE_U)) return 0;
    if (write && !(e & VMM_PTE_W)) return 0;
    if (e & VMM_PTE_HUGE) return 1;

    uint64_t *pt = tbl(e & VMM_ADDR_MASK);
    e = pt[(int)((va >> 12) & 0x1FF)];
    if (!(e & VMM_PTE_P) || !(e & VMM_PTE_U)) return 0;
    if (write && !(e & VMM_PTE_W)) return 0;
    return 1;
}

int vmm_map_kernel_page(uint64_t va, uint64_t pa, uint64_t flags) {
    vmm_state_t *st = VMM_STATE;
    if (!st || !st->ready || (va & 0xFFFull) || (pa & 0xFFFull)) return -1;
    uint64_t *pml4 = tbl(st->kernel_space);
    int i4 = (int)((va >> 39) & 0x1FF);
    int i3 = (int)((va >> 30) & 0x1FF);
    int i2 = (int)((va >> 21) & 0x1FF);
    int i1 = (int)((va >> 12) & 0x1FF);

    if (!(pml4[i4] & VMM_PTE_P)) {
        uint64_t t = vmm_alloc_frame();
        if (!t) return -3;
        pml4[i4] = t | VMM_PTE_P | VMM_PTE_W;
    } else if (pml4[i4] & VMM_PTE_HUGE) {
        return -2;
    }
    uint64_t *pdpt = tbl(pml4[i4] & VMM_ADDR_MASK);

    if (!(pdpt[i3] & VMM_PTE_P)) {
        uint64_t t = vmm_alloc_frame();
        if (!t) return -3;
        pdpt[i3] = t | VMM_PTE_P | VMM_PTE_W;
    } else if (pdpt[i3] & VMM_PTE_HUGE) {
        return -2;
    }
    uint64_t *pd = tbl(pdpt[i3] & VMM_ADDR_MASK);

    if (!(pd[i2] & VMM_PTE_P)) {
        uint64_t t = vmm_alloc_frame();
        if (!t) return -3;
        pd[i2] = t | VMM_PTE_P | VMM_PTE_W;
    } else if (pd[i2] & VMM_PTE_HUGE) {
        return -2;
    }
    uint64_t *pt = tbl(pd[i2] & VMM_ADDR_MASK);

    pt[i1] = (pa & VMM_ADDR_MASK) | (flags & ~VMM_PTE_U) | VMM_PTE_P;
    invlpg(va);
    return 0;
}

int vmm_map_kernel_range(uint64_t va, uint64_t size) {
    vmm_space_t sp = vmm_current();
    uint64_t start = va & ~0xFFFull;
    uint64_t end = (va + size + 0xFFF) & ~0xFFFull;
    for (uint64_t a = start; a < end; a += 0x1000) {
        if (vmm_translate(sp, a) == 0) {
            if (vmm_map(sp, a, a, VMM_PTE_P | VMM_PTE_W) != 0) return -1;
        }
    }
    return 0;
}

int vmm_pat_init_cpu(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1u), "c"(0u));
    if (!(edx & (1u << 16))) {
        return 0; /* CPUID.01h:EDX[16] PAT desteklenmiyor */
    }

    uint32_t lo = (uint32_t)(VMM_PAT_MSR_VALUE & 0xFFFFFFFFull);
    uint32_t hi = (uint32_t)(VMM_PAT_MSR_VALUE >> 32);
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    __asm__ volatile("wrmsr" :: "c"(0x277u), "a"(lo), "d"(hi) : "memory");
    wr_cr3(rd_cr3());
    if (flags & 0x200ull) __asm__ volatile("sti" ::: "memory");
    return 1;
}

int vmm_pat_enabled(void) {
    vmm_state_t *st = VMM_STATE;
    if (!st || st->magic != VMM_MAGIC || !st->ready) return 0;
    return st->pat_enabled ? 1 : 0;
}

static int vmm_map_identity_2m(uint64_t phys, uint64_t size,
                               uint64_t cache_flags, int overwrite_present) {
    vmm_state_t *st = VMM_STATE;
    if (!st || st->magic != VMM_MAGIC || !st->ready || size == 0) return -1;
    if (phys + size < phys) return -1;

    uint64_t start = phys & ~0x1FFFFFull;
    uint64_t end   = (phys + size + 0x1FFFFFull) & ~0x1FFFFFull;
    if (end <= start) return -1;

    uint64_t *pml4 = tbl(st->kernel_space);
    uint64_t cur_sp = vmm_current();
    uint64_t *cur_pml4 = (cur_sp && cur_sp != st->kernel_space) ? tbl(cur_sp) : 0;

    for (uint64_t a = start; a < end; a += 0x200000ull) {
        int i4 = (int)((a >> 39) & 0x1FF);
        int i3 = (int)((a >> 30) & 0x1FF);
        int i2 = (int)((a >> 21) & 0x1FF);

        if (!(pml4[i4] & VMM_PTE_P)) {
            uint64_t t = vmm_alloc_frame();
            if (!t) return -2;
            pml4[i4] = t | VMM_PTE_P | VMM_PTE_W;
            if (cur_pml4 && !(cur_pml4[i4] & VMM_PTE_P))
                cur_pml4[i4] = pml4[i4];
        } else if (pml4[i4] & VMM_PTE_HUGE) {
            continue;
        }
        uint64_t *pdpt = tbl(pml4[i4] & VMM_ADDR_MASK);

        if (!(pdpt[i3] & VMM_PTE_P)) {
            uint64_t t = vmm_alloc_frame();
            if (!t) return -2;
            pdpt[i3] = t | VMM_PTE_P | VMM_PTE_W;
        } else if (pdpt[i3] & VMM_PTE_HUGE) {
            continue;
        }
        uint64_t *pd = tbl(pdpt[i3] & VMM_ADDR_MASK);

        if (pd[i2] & VMM_PTE_P) {
            if (!(pd[i2] & VMM_PTE_HUGE)) {
                /* 4KB page table already present; update leaf PTEs if requested */
                if (overwrite_present) {
                    uint64_t *pt = tbl(pd[i2] & VMM_ADDR_MASK);
                    uint64_t p0 = (phys > a) ? ((phys - a) >> 12) : 0;
                    uint64_t p1 = ((phys + size) < (a + 0x200000ull))
                                  ? (((phys + size + 0xFFFull - a) >> 12)) : 512ull;
                    if (p1 > 512ull) p1 = 512ull;
                    for (uint64_t k = p0; k < p1; k++) {
                        if (pt[k] & VMM_PTE_P) {
                            pt[k] = (pt[k] & ~(VMM_PTE_PWT | VMM_PTE_PCD | VMM_PTE_PAT_4K))
                                    | cache_flags;
                        }
                    }
                }
            } else if (overwrite_present) {
                pd[i2] = (pd[i2] & ~(VMM_PTE_PWT | VMM_PTE_PCD | VMM_PTE_PAT_2M))
                         | cache_flags;
            }
        } else {
            pd[i2] = a | VMM_PTE_P | VMM_PTE_W | VMM_PTE_HUGE | cache_flags;
        }
    }

    wr_cr3(rd_cr3());
    if (smp_tlb_shootdown_all) smp_tlb_shootdown_all();
    return 0;
}

int vmm_map_wc_range(uint64_t phys, uint64_t size) {
    uint64_t cf = vmm_pat_enabled() ? VMM_PTE_WC : 0ull;
    return vmm_map_identity_2m(phys, size, cf, 1);
}

int vmm_map_mmio_range(uint64_t phys, uint64_t size) {
    /* PCD=1, PWT=0 -> PAT index 2 = UC- (identical to lib/mmio.c) */
    return vmm_map_identity_2m(phys, size, VMM_PTE_PCD, 0);
}

int vmm_set_region_wc(uint64_t virt, uint64_t size) {
    vmm_state_t *st = VMM_STATE;
    if (!st || st->magic != VMM_MAGIC || !st->ready || size == 0) return -1;
    if (!st->pat_enabled) return 0;

    uint64_t start = virt & ~0xFFFull;
    uint64_t end   = (virt + size + 0xFFFull) & ~0xFFFull;
    if (end <= start) return -1;

    uint64_t *pml4 = tbl(st->kernel_space);
    uint64_t a = start;
    while (a < end) {
        int i4 = (int)((a >> 39) & 0x1FF);
        int i3 = (int)((a >> 30) & 0x1FF);
        int i2 = (int)((a >> 21) & 0x1FF);
        int i1 = (int)((a >> 12) & 0x1FF);

        if (!(pml4[i4] & VMM_PTE_P) || (pml4[i4] & VMM_PTE_HUGE)) {
            a = (a + 0x8000000000ull) & ~0x7FFFFFFFFFull;
            continue;
        }
        uint64_t *pdpt = tbl(pml4[i4] & VMM_ADDR_MASK);
        if (!(pdpt[i3] & VMM_PTE_P) || (pdpt[i3] & VMM_PTE_HUGE)) {
            a = (a + 0x40000000ull) & ~0x3FFFFFFFull;
            continue;
        }
        uint64_t *pd = tbl(pdpt[i3] & VMM_ADDR_MASK);
        if (!(pd[i2] & VMM_PTE_P)) {
            a = (a + 0x200000ull) & ~0x1FFFFFull;
            continue;
        }
        if (pd[i2] & VMM_PTE_HUGE) {
            pd[i2] = (pd[i2] & ~(VMM_PTE_PWT | VMM_PTE_PCD | VMM_PTE_PAT_2M)) | VMM_PTE_WC;
            a = (a + 0x200000ull) & ~0x1FFFFFull;
            continue;
        }
        uint64_t *pt = tbl(pd[i2] & VMM_ADDR_MASK);
        if (pt[i1] & VMM_PTE_P) {
            pt[i1] = (pt[i1] & ~(VMM_PTE_PWT | VMM_PTE_PCD | VMM_PTE_PAT_4K)) | VMM_PTE_WC;
        }
        a += 0x1000ull;
    }

    wr_cr3(rd_cr3());
    if (smp_tlb_shootdown_all) smp_tlb_shootdown_all();
    return 0;
}

int vmm_handle_page_fault(uint64_t cr2, uint64_t err, uint64_t rip) {
    vmm_state_t *st = VMM_STATE;
    st->fault_count++;
    st->last_fault_cr2 = cr2;
    st->last_fault_rip = rip;
    st->last_fault_err = (uint32_t)err;

    int user_origin = (err & 4ull) ? 1 : 0;
    int prot_violation = (err & 1ull) ? 1 : 0;

    if (cr2 >= VMM_USER_BASE && cr2 < 0x0000800000000000ull) {
        st->last_fault_killed = 1;
        st->killed_tasks++;
        serial_printf("[vmm] gorev page fault cr2=0x%X%08X rip=0x%X%08X -> gorev sonlandi\n",
                      (unsigned)(cr2 >> 32), (unsigned)cr2,
                      (unsigned)(rip >> 32), (unsigned)rip);
        return 1;
    }
    if (user_origin) {
        st->last_fault_killed = 1;
        st->killed_tasks++;
        serial_printf("[vmm] koruma ihlali cr2=0x%X%08X rip=0x%X%08X (%s) -> gorev sonlandi\n",
                      (unsigned)(cr2 >> 32), (unsigned)cr2,
                      (unsigned)(rip >> 32), (unsigned)rip,
                      prot_violation ? "U/S" : "yok");
        return 1;
    }
    st->last_fault_killed = 0;
    return 0;
}

int vmm_selftest(void) {
    vmm_state_t *st = VMM_STATE;
    if (!st->ready) return -1;

    vmm_space_t ksp = st->kernel_space;
    vmm_space_t spa = vmm_new_space();
    vmm_space_t spb = vmm_new_space();
    if (!spa || !spb) return -2;

    uint64_t fr = vmm_alloc_frame();
    if (!fr) return -2;
    if (vmm_map(spb, (uint64_t)VMM_USER_BASE, fr,
                VMM_PTE_P | VMM_PTE_W | VMM_PTE_U) != 0) return -3;

    static arctian_recover_t rec;
    g_arctian_recover = &rec;
    rec.active = 0;

    int c = arctian_setjmp(&rec);
    if (c == 0) {
        rec.active = 1;
        __asm__ volatile("cli");
        vmm_switch(spa);
        *(volatile uint64_t *)(uintptr_t)VMM_USER_BASE = 0xA5A5A5A5ull;
        __asm__ volatile("cli");
        vmm_switch(ksp);
        rec.active = 0;
        g_arctian_recover = 0;
        __asm__ volatile("sti");
        return -4;
    }

    __asm__ volatile("cli");
    vmm_switch(ksp);
    rec.active = 0;
    g_arctian_recover = 0;
    __asm__ volatile("sti");
    serial_puts("[vmm] selftest OK: baska adres alanina erisim -> task killed, sistem ayakta\n");
    return 0;
}

static uint64_t leaf_pte(vmm_space_t sp, uint64_t va) {
    uint64_t *pml4 = tbl(sp);
    int i4 = (int)((va >> 39) & 0x1FF);
    int i3 = (int)((va >> 30) & 0x1FF);
    int i2 = (int)((va >> 21) & 0x1FF);
    int i1 = (int)((va >> 12) & 0x1FF);
    uint64_t e = pml4[i4];
    if (!(e & VMM_PTE_P) || (e & VMM_PTE_HUGE)) return 0;
    uint64_t *pdpt = tbl(e & VMM_ADDR_MASK);
    e = pdpt[i3];
    if (!(e & VMM_PTE_P) || (e & VMM_PTE_HUGE)) return 0;
    uint64_t *pd = tbl(e & VMM_ADDR_MASK);
    e = pd[i2];
    if (!(e & VMM_PTE_P) || (e & VMM_PTE_HUGE)) return 0;
    uint64_t *pt = tbl(e & VMM_ADDR_MASK);
    e = pt[i1];
    if (!(e & VMM_PTE_P)) return 0;
    return e;
}

int vmm_prot_selftest(void) {
    vmm_state_t *st = VMM_STATE;
    if (!st->ready) return -1;

    uint64_t *kpml4 = tbl(st->kernel_space);
    int kernel_kern = (((kpml4[0] & VMM_PTE_U) == 0) &&
                       ((kpml4[VMM_HIGHER_PML4_INDEX] & VMM_PTE_U) == 0));

    vmm_space_t sp = vmm_new_space();
    if (!sp) return -2;
    uint64_t fr = vmm_alloc_frame();
    if (!fr) return -2;
    if (vmm_map(sp, (uint64_t)VMM_USER_BASE, fr,
                VMM_PTE_P | VMM_PTE_W | VMM_PTE_U) != 0) return -3;

    uint64_t *upml4 = tbl(sp);
    int i4 = (int)(((uint64_t)VMM_USER_BASE >> 39) & 0x1FF);
    int i3 = (int)(((uint64_t)VMM_USER_BASE >> 30) & 0x1FF);
    int i2 = (int)(((uint64_t)VMM_USER_BASE >> 21) & 0x1FF);

    int user_ok = (upml4[i4] & VMM_PTE_U) != 0;
    if (user_ok) {
        uint64_t *pdpt = tbl(upml4[i4] & VMM_ADDR_MASK);
        uint64_t *pd = tbl(pdpt[i3] & VMM_ADDR_MASK);
        user_ok = ((pdpt[i3] & VMM_PTE_U) != 0) &&
                  ((pd[i2] & VMM_PTE_U) != 0) &&
                  ((leaf_pte(sp, (uint64_t)VMM_USER_BASE) & VMM_PTE_U) != 0);
    }

    int ok = kernel_kern && user_ok;
    serial_printf("[vmm] bellek koruma selftest: kernel U=0:%s kullanici U=1:%s -> %s\n",
                  kernel_kern ? "EVET" : "HAYIR",
                  user_ok ? "EVET" : "HAYIR",
                  ok ? "OK" : "HATA");
    return ok ? 0 : -4;
}

int vmm_mem_selftest(void) {
    vmm_state_t *st = VMM_STATE;
    if (!st->ready) return -1;

    uint64_t base_used = st->frames_used;
    uint64_t base_free = st->frames_free;

    uint64_t a = vmm_alloc_frame();
    uint64_t b = vmm_alloc_frame();
    uint64_t c = vmm_alloc_frame();
    if (!a || !b || !c) return -2;
    vmm_free_frame(b);
    uint64_t d = vmm_alloc_frame();

    int reuse = (d == b);
    int mid_ok = (st->frames_used == base_used + 3) && (st->frames_free == base_free);

    vmm_free_frame(a);
    vmm_free_frame(c);
    vmm_free_frame(d);
    int end_ok = (st->frames_used == base_used) && (st->frames_free == base_free + 3);

    int ok = reuse && mid_ok && end_ok;
    serial_printf("[vmm] bellek muhasebe selftest: kullanilan %u->%u cerceve, geri donusum=%s, serbest=%s -> %s\n",
                  (unsigned)base_used, (unsigned)(base_used + 3),
                  reuse ? "EVET" : "HAYIR", end_ok ? "EVET" : "HAYIR",
                  ok ? "OK" : "HATA");
    return ok ? 0 : -1;
}
