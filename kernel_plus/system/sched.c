#include "sched.h"
#include <stdbool.h>
#include "isr.h"
#include "serial.h"
#include "paging.h"
#include "spinlock.h"
#include "percpu.h"
#include "percpu_gdt.h"
#include "lapic.h"
#include "alloc.h"        /* C4: gorev yiginlari heap'ten */

#define MSR_FS_BASE 0xC0000100u

static inline void sched_wrmsr_fs(uint64_t val) {
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" :: "c"(MSR_FS_BASE), "a"(lo), "d"(hi) : "memory");
}

#define SCHED_STACK_SIZE   (32u * 1024u)
#define SCHED_SLICE_TICKS  5u
#define SCHED_CHUNK_SHIFT  6u
#define SCHED_CHUNK_SIZE   (1u << SCHED_CHUNK_SHIFT)
#define SCHED_CHUNK_MASK   (SCHED_CHUNK_SIZE - 1u)
#define SCHED_MAX_CHUNKS   (SCHED_MAX_TASKS >> SCHED_CHUNK_SHIFT)

typedef struct sched_task {
    uint64_t rsp;
    uint64_t fpu[64] __attribute__((aligned(16)));
    volatile int state;
    int id;
    const char *name;
    uint8_t *stack_base;
    uint32_t wake_tick;
    uint64_t switches;
    uint64_t cr3;
    uint64_t kstack_top;          /* TSS.rsp0 & SYSCALL kernel stack */
    uint64_t fs_base;             /* x86-64 TLS MSR_FS_BASE */
    void *owner;
    void *posix_thr;              /* POSIX ring-3 thread baglami */
    uint64_t ticks;
    /* Per-CPU run queue baglanti alanlari (paket 7). */
    int32_t next_q;               /* kuyrukta sonraki task indexi, -1 = yok */
    uint8_t in_q;                 /* bir kuyruga bagli mi (tek kuyruk garantisi) */
    uint8_t stack_heap;           /* C4: yigin heap'ten ayrildi (reap'ta kfree) */
    volatile uint8_t on_cpu;      /* SMP: su an bir CPU'nun yigininda aktif */
    uint8_t user_app;             /* E1: kullanici/uygulama gorevi (OOM dondurma adayi) */
    uint8_t freeze_exempt;        /* E1: cekirdek/masaustu gorevi (asla dondurulmaz) */
    uint8_t frozen_prev_state;    /* E1: TASK_FROZEN oncesi durum */
    uint32_t bound_cpu;           /* bulundugu kuyruk (CPU indexi) */
} sched_task_t;

/* CPU basina hazir task kuyrugu (ticket lock ile korunur).
 * Kilit sirasi: runq kilidi -> baska kilit YOK. Kilit alirken her zaman
 * irq_save() kullanilir; tek kilit alinir, ic ice kilitlenme yok.
 * runq[0] BSP'nin mevcut kuyrugudur: legacy schedule() gorevleri state
 * taramasiyla secer, kuyruk girdileri aynen durur (ayna); pick_local(cpu0)
 * legacy yola gider, yani BSP preemption davranisi degismez. */
typedef struct {
    spinlock_t lock;
    int32_t head;                 /* ilk READY task indexi, -1 = bos */
    int32_t tail;
    uint32_t count;
} runq_t;

extern void arctian_sched_switch(uint64_t *from_rsp, uint64_t *to_rsp,
                                 void *from_fpu, void *to_fpu,
                                 volatile uint8_t *from_on_cpu);
extern void arctian_task_entry(void);

static spinlock_t s_tasks_lock = SPINLOCK_INIT;

/* Paket 7 yardimcilari (tanitularlari asagida; kendine-yonetim yollari
 * sleep/exit/yield bunlari erken kullaniyor). */
static uint32_t sched_self_cpu(void);
static sched_task_t *sched_self_current(void);
static void sched_resched_notify(uint32_t cpu);

static sched_task_t *s_chunks[SCHED_MAX_CHUNKS];
static volatile uint32_t s_num_chunks;
static volatile uint32_t s_alloc_cap;
static uint32_t s_capacity;         /* dinamik RAM tabanli kapasite */
static sched_task_t *s_current;     /* yalniz CPU 0 (BSP) icin gecerli */
static volatile uint32_t s_tick;
static volatile uint64_t s_total_ticks;
static volatile int s_ready;
static uint32_t s_spawn_rr;         /* spawn round-robin sayaci (C3) */

static runq_t s_runq[CPU_MAX_APICS];

static inline sched_task_t *task_at(int tid) {
    uint32_t cap = __atomic_load_n(&s_alloc_cap, __ATOMIC_ACQUIRE);
    if (tid < 0 || (uint32_t)tid >= cap) return 0;
    sched_task_t *ch = s_chunks[(uint32_t)tid >> SCHED_CHUNK_SHIFT];
    return ch ? &ch[(uint32_t)tid & SCHED_CHUNK_MASK] : 0;
}

static void init_task_slot(sched_task_t *t, int id) {
    t->rsp = 0;
    t->state = TASK_UNUSED;
    t->id = id;
    t->name = 0;
    t->stack_base = 0;
    t->wake_tick = 0;
    t->switches = 0;
    t->cr3 = 0;
    t->kstack_top = 0;
    t->fs_base = 0;
    t->owner = 0;
    t->posix_thr = 0;
    t->ticks = 0;
    t->next_q = -1;
    t->in_q = 0;
    t->stack_heap = 0;
    t->on_cpu = 0;
    t->user_app = 0;
    t->freeze_exempt = 0;
    t->frozen_prev_state = TASK_READY;
    t->bound_cpu = 0;
}

static int sched_task_ptr_valid(const sched_task_t *t) {
    if (!t) return 0;
    uint32_t nc = __atomic_load_n(&s_num_chunks, __ATOMIC_ACQUIRE);
    for (uint32_t c = 0; c < nc; c++) {
        sched_task_t *base = s_chunks[c];
        if (!base) continue;
        if (t >= base && t < base + SCHED_CHUNK_SIZE) {
            return (((uintptr_t)t - (uintptr_t)base) % sizeof(sched_task_t)) == 0;
        }
    }
    return 0;
}

/* ---- E1 dinamik RAM tabanli kapasite ---- */
static bool ram_gate_ok(uint64_t need) {
    uint64_t avail_vmm = vmm_avail_above_reserve_bytes();
    if (avail_vmm >= need) return true;
    size_t used = kmalloc_used();
    size_t mapped = kmalloc_heap_mapped();
    return (mapped > used && (mapped - used) >= need);
}

int sched_capacity(void) {
    uint64_t avail = vmm_avail_above_reserve_bytes();
    uint64_t used_tasks = (uint64_t)sched_task_count();
    uint64_t extra = avail / (uint64_t)(SCHED_STACK_SIZE + sizeof(sched_task_t));
    uint64_t total = used_tasks + extra;
    if (total < s_alloc_cap) total = s_alloc_cap;
    if (total > SCHED_MAX_TASKS) total = SCHED_MAX_TASKS;
    return (int)total;
}

void sched_scale_capacity(void) {
    uint32_t n = percpu_online_count();
    if (n == 0) n = 1;
    s_capacity = (uint32_t)sched_capacity();
    serial_printf("[sched] dinamik kapasite=%u (cpu=%u)\n", s_capacity, n);
}

/* AP ilk goreve gecerken "uzerinden gecilen" baglam: AP'nin park dongusu
 * kesimden once calisan bir gorev degildi; switch buradan yapilir. */
static sched_task_t s_ap_idle[CPU_MAX_APICS] __attribute__((aligned(16)));

/* ---- run queue yardimcilari (cagiran ilgili rq->lock'u tutmali) ---- */

static void rq_push_locked(runq_t *rq, int tid) {
    sched_task_t *t = task_at(tid);
    if (!t || t->in_q) return;
    t->in_q = 1;
    t->next_q = -1;
    if (rq->tail >= 0) {
        sched_task_t *tail = task_at(rq->tail);
        if (tail) tail->next_q = tid;
    } else {
        rq->head = tid;
    }
    rq->tail = tid;
    rq->count++;
}

static int rq_pop_locked(runq_t *rq) {
    int tid = rq->head;
    if (tid < 0) return -1;
    sched_task_t *t = task_at(tid);
    if (!t) { rq->head = -1; rq->tail = -1; rq->count = 0; return -1; }
    rq->head = t->next_q;
    if (rq->head < 0) rq->tail = -1;
    t->next_q = -1;
    t->in_q = 0;
    if (rq->count > 0) rq->count--;
    return tid;
}

static void rq_detach_locked(runq_t *rq, int tid) {
    sched_task_t *t = task_at(tid);
    if (!t || !t->in_q) return;
    int prev = -1;
    for (int cur = rq->head; cur >= 0; ) {
        sched_task_t *ct = task_at(cur);
        if (!ct) break;
        int nxt = ct->next_q;
        if (cur == tid) {
            if (prev >= 0) {
                sched_task_t *pt = task_at(prev);
                if (pt) pt->next_q = nxt;
            } else {
                rq->head = nxt;
            }
            if (rq->tail == tid) rq->tail = prev;
            t->next_q = -1;
            t->in_q = 0;
            if (rq->count > 0) rq->count--;
            return;
        }
        prev = cur;
        cur = nxt;
    }
}

/* Gorevi bound_cpu kuyruguna READY ise ekler (dedupe in_q ile). */
static void rq_enqueue_bound(int tid) {
    sched_task_t *t = task_at(tid);
    if (!t) return;
    if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != TASK_READY || t->bound_cpu >= CPU_MAX_APICS) return;
    runq_t *rq = &s_runq[t->bound_cpu];
    uint32_t cpu = t->bound_cpu;
    uint64_t f = irq_save();
    spin_lock(&rq->lock);
    if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == TASK_READY)
        rq_push_locked(rq, tid);
    spin_unlock(&rq->lock);
    irq_restore(f);
    sched_resched_notify(cpu);   /* kilit/bayrak disinda: AP'yi bekletme */
}

/* E1 SMP is calma: en yuksek yuklu cevrimici kurban CPU'nun kuyrugundan
 * READY bir gorevi (tid != 0) CARRY durumuna alip kendine (self) ayirir.
 * Hicbir anda birden fazla runq kilidi tutulmaz (deadlock imkansiz). */
static int steal_runq_locked(uint32_t self) {
    if (self >= CPU_MAX_APICS) return -1;
    uint32_t order[CPU_MAX_APICS];
    uint32_t ocnt = 0;
    for (uint32_t k = 1; k < CPU_MAX_APICS; k++) {
        uint32_t v = (self + k) % CPU_MAX_APICS;
        if (v == self) continue;
        if (s_runq[v].count == 0) continue;
        order[ocnt++] = v;
    }
    /* En yogun kurban kuyrugunu basa al (insertion sort on tiny active list) */
    for (uint32_t i = 1; i < ocnt; i++) {
        uint32_t key = order[i];
        uint32_t kc = s_runq[key].count;
        int j = (int)i - 1;
        while (j >= 0 && s_runq[order[j]].count < kc) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }

    for (uint32_t oi = 0; oi < ocnt; oi++) {
        uint32_t victim = order[oi];
        runq_t *vq = &s_runq[victim];
        int got = -1;
        spin_lock(&vq->lock);
        uint32_t scan = vq->count;
        while (scan-- > 0) {
            int cand = rq_pop_locked(vq);
            if (cand < 0) break;
            sched_task_t *ct = task_at(cand);
            if (!ct) continue;
            if (cand == 0) {
                /* main/desktop gorevi BSP disina tasinmaz */
                rq_push_locked(vq, cand);
                continue;
            }
            if (__atomic_load_n(&ct->state, __ATOMIC_ACQUIRE) == TASK_READY) {
                if (__atomic_load_n(&ct->on_cpu, __ATOMIC_ACQUIRE)) {
                    rq_push_locked(vq, cand);
                    continue;
                }
                int exp = TASK_READY;
                if (__atomic_compare_exchange_n(&ct->state, &exp, TASK_CARRY, false,
                                                __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                    ct->bound_cpu = self;
                    got = cand;
                    break;
                }
            }
        }
        spin_unlock(&vq->lock);
        if (got >= 0) return got;
    }
    return -1;
}

uint64_t sched_irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

void sched_irq_restore(uint64_t flags) {
    if (flags & 0x200ull) __asm__ volatile("sti" ::: "memory");
}

static void sched_switch_to(sched_task_t *from, sched_task_t *to) {
    if (from == to) return;
    if (sched_task_ptr_valid(to)) {
        while (__atomic_load_n(&to->on_cpu, __ATOMIC_ACQUIRE))
            __asm__ volatile("pause" ::: "memory");
        __atomic_store_n(&to->on_cpu, 1, __ATOMIC_RELEASE);
        uint32_t ci = sched_self_cpu();
        if (to->kstack_top) {
            gdt_set_rsp0(ci, to->kstack_top);
            percpu_t *pc = percpu_by_index(ci);
            if (pc) pc->syscall_kstack = to->kstack_top;
        }
        if (to->fs_base != from->fs_base)
            sched_wrmsr_fs(to->fs_base);
    }
    volatile uint8_t *rel = sched_task_ptr_valid(from) ? &from->on_cpu : 0;
    if (to->cr3 && to->cr3 != from->cr3) vmm_switch(to->cr3);
    arctian_sched_switch(&from->rsp, &to->rsp, from->fpu, to->fpu, rel);
}

static void schedule(void) {
    sched_task_t *cur = s_current;
    if (!cur) return;

    sched_task_t *next = 0;
    uint32_t cap = __atomic_load_n(&s_alloc_cap, __ATOMIC_ACQUIRE);
    for (uint32_t k = 1; k < cap; k++) {
        int idx = (int)((cur->id + k) % cap);
        sched_task_t *t = task_at(idx);
        if (!t) continue;
        if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != TASK_READY) continue;
        if (t->bound_cpu != 0) continue;
        if (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) continue;
        next = t;
        break;
    }

    if (!next) {
        sched_task_t *t0 = task_at(0);
        if (cur->state != TASK_RUNNING && t0 &&
            __atomic_load_n(&t0->state, __ATOMIC_ACQUIRE) == TASK_READY &&
            !__atomic_load_n(&t0->on_cpu, __ATOMIC_ACQUIRE)) {
            next = t0;
        } else if (cur->state != TASK_RUNNING) {
            /* BSP bosta: AP kuyruklarindan bir is calmaya calis */
            int stolen = steal_runq_locked(0);
            if (stolen > 0) {
                sched_task_t *st = task_at(stolen);
                if (st && __atomic_load_n(&st->state, __ATOMIC_ACQUIRE) == TASK_CARRY) {
                    __atomic_store_n(&st->state, TASK_READY, __ATOMIC_RELEASE);
                    next = st;
                }
            }
        }
        if (!next) return;
    }
    if (next == cur) return;

    if (cur->state == TASK_RUNNING) cur->state = TASK_READY;
    next->state = TASK_RUNNING;
    s_current = next;
    cur->switches++;
    sched_switch_to(cur, next);
}

void sched_tick(void) {
    if (!s_ready) return;
    s_tick++;
    s_total_ticks++;
    if (s_current && s_current->state == TASK_RUNNING) s_current->ticks++;

    uint32_t cap = __atomic_load_n(&s_alloc_cap, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < cap; i++) {
        sched_task_t *t = task_at((int)i);
        if (t && __atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == TASK_BLOCKED &&
            (int32_t)(s_tick - t->wake_tick) >= 0) {
            int exp = TASK_BLOCKED;
            if (__atomic_compare_exchange_n(&t->state, &exp, TASK_READY, false,
                                            __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                rq_enqueue_bound((int)i);   /* AP kuyruklari icin; cpu0 ayna kayit */
            }
        }
    }

    if ((s_tick % SCHED_SLICE_TICKS) != 0) return;
    schedule();
}

/* ---- gorev kendini yonetim yollari (C3: CPU-bagimsiz) ----
 * s_current yalniz BSP'ninkidir; AP'de "ben" percpu current_task'tan
 * gelir. Bu yuzden tüm "beni" yapan yollar sched_self_current() kullanim.
 * Bekleme taramasi BSP'nin PIT tikindedir (sched_tick); wake_tick tüm
 * sistemde ortak s_tick sayacina baglidir - AP de bu sayaci okur. */

void sched_yield(void) {
    if (!s_ready) return;
    uint32_t ci = sched_self_cpu();
    uint64_t f = sched_irq_save();
    if (ci == 0) schedule();
    else sched_pick_local();
    sched_irq_restore(f);
}

void sched_sleep_ms(uint32_t ms) {
    if (!s_ready || ms == 0) return;
    sched_task_t *me = sched_self_current();
    if (!me) return;
    uint64_t f = sched_irq_save();
    me->wake_tick = s_tick + ms;
    __atomic_store_n(&me->state, TASK_BLOCKED, __ATOMIC_RELEASE);
    sched_commit_block();
    sched_irq_restore(f);
}

void sched_exit(void) {
    sched_task_t *me = sched_self_current();
    if (!me) return;
    uint64_t f = sched_irq_save();
    __atomic_store_n(&me->state, TASK_DONE, __ATOMIC_RELEASE);
    uint32_t ci = sched_self_cpu();
    if (ci == 0) {
        schedule();
    } else {
        if (sched_pick_local() < 0) {
            /* AP'de baska gorev yok: biten gorevin yiginindan inip AP idle
             * dongusune don; boylece on_cpu=0 olur ve reap guvenle kfree yapar. */
            percpu_t *p = percpu_by_index(ci);
            if (p) { p->current_task = 0; p->idle = true; }
            vmm_state_t *vst = VMM_STATE;
            if (vst && vst->kernel_space) s_ap_idle[ci].cr3 = vst->kernel_space;
            sched_switch_to(me, &s_ap_idle[ci]);
        }
    }
    sched_irq_restore(f);
    for (;;) __asm__ volatile("hlt");
}

sched_tid_t sched_spawn(const char *name, void (*fn)(void *), void *arg) {
    return sched_spawn_ex(name, fn, arg, 0);
}

static sched_tid_t sched_alloc_task(const char *name, void (*fn)(void *), void *arg,
                                    uint64_t cr3, void *stack, uint32_t stack_size) {
    if (!s_ready || !fn) return -1;
    uint64_t f = sched_irq_save();

    int idx = -1;
    for (;;) {
        spin_lock(&s_tasks_lock);
        uint32_t cap = s_alloc_cap;
        for (uint32_t i = 1; i < cap; i++) {
            sched_task_t *cand = task_at((int)i);
            if (cand && cand->state == TASK_UNUSED) {
                idx = (int)i;
                cand->state = TASK_CARRY;   /* baska CPU ayni yuvayi almasin */
                break;
            }
        }
        uint32_t nc = s_num_chunks;
        spin_unlock(&s_tasks_lock);
        if (idx >= 0) break;
        if (nc >= SCHED_MAX_CHUNKS) break;

        /* Dinamik havuz genisletme: kilit disinda kmalloc ile yeni 64'luk dilim */
        sched_task_t *new_ch = (sched_task_t *)kmalloc(sizeof(sched_task_t) * SCHED_CHUNK_SIZE);
        if (!new_ch) break;
        uint32_t base_id = nc * SCHED_CHUNK_SIZE;
        for (uint32_t j = 0; j < SCHED_CHUNK_SIZE; j++) {
            init_task_slot(&new_ch[j], (int)(base_id + j));
        }

        spin_lock(&s_tasks_lock);
        if (s_num_chunks == nc && s_num_chunks < SCHED_MAX_CHUNKS) {
            s_chunks[s_num_chunks++] = new_ch;
            __atomic_store_n(&s_alloc_cap, s_num_chunks * SCHED_CHUNK_SIZE, __ATOMIC_RELEASE);
            idx = (int)base_id;
            new_ch[0].state = TASK_CARRY;
            spin_unlock(&s_tasks_lock);
            break;
        }
        spin_unlock(&s_tasks_lock);
        /* Baska bir CPU ayni anda buyuttu; fazlayi geri ver ve tekrar dene */
        kfree(new_ch);
    }
    if (idx < 0) { sched_irq_restore(f); return -1; }

    sched_task_t *t = task_at(idx);
    void *base = stack;
    uint32_t size = stack_size;
    uint8_t heap_stack = 0;
    if (!base) {
        if (!ram_gate_ok(SCHED_STACK_SIZE)) {
            __atomic_store_n(&t->state, TASK_UNUSED, __ATOMIC_RELEASE);
            sched_irq_restore(f);
            serial_printf("[sched] RED %s: RAM dolu (kullanim %u KB)\n",
                          name ? name : "?", (unsigned)(kmalloc_used() / 1024u));
            return -1;
        }
        base = kmalloc(SCHED_STACK_SIZE);
        if (!base) {
            __atomic_store_n(&t->state, TASK_UNUSED, __ATOMIC_RELEASE);
            sched_irq_restore(f);
            serial_printf("[sched] RED %s: heap tukendi\n", name ? name : "?");
            return -1;
        }
        size = SCHED_STACK_SIZE;
        heap_stack = 1;
    }
    uintptr_t top = ((uintptr_t)base + size) & ~(uintptr_t)15;
    uint64_t *fr = (uint64_t *)(top - 64);
    fr[0] = 0;
    fr[1] = 0;
    fr[2] = (uint64_t)(uintptr_t)arg;
    fr[3] = (uint64_t)(uintptr_t)fn;
    fr[4] = 0;
    fr[5] = 0;
    fr[6] = 0x202;
    fr[7] = (uint64_t)(uintptr_t)&arctian_task_entry;

    for (int i = 0; i < 64; i++) t->fpu[i] = 0;
    ((uint32_t *)t->fpu)[6] = 0x1F80u;

    t->rsp = (uint64_t)(uintptr_t)fr;
    t->kstack_top = (uint64_t)top;
    t->fs_base = 0;
    t->posix_thr = 0;
    t->name = name;
    t->stack_base = (uint8_t *)base;
    t->wake_tick = 0;
    t->switches = 0;
    t->ticks = 0;
    t->cr3 = cr3 ? cr3 : vmm_current();
    t->owner = 0;
    t->next_q = -1;
    t->in_q = 0;
    t->stack_heap = heap_stack;
    t->on_cpu = 0;
    vmm_state_t *vst = VMM_STATE;
    t->user_app = (vst && vst->kernel_space && t->cr3 && t->cr3 != vst->kernel_space) ? 1u : 0u;
    t->freeze_exempt = (name && name[0] == 'n' && name[1] == 'e' &&
                        name[2] == 't' && name[3] == 'b' && name[4] == 'g') ? 1u : 0u;
    t->frozen_prev_state = TASK_READY;
    __atomic_store_n(&t->state, TASK_READY, __ATOMIC_RELEASE);

    /* E1 SMP yuk dengeleme: en az yuklu cevrimici CPU'yu sec */
    uint32_t bound = 0;
    {
        uint32_t ncpu = percpu_online_count();
        if (ncpu > 1) {
            uint32_t rr = ++s_spawn_rr;
            uint32_t best_c = 0;
            uint32_t best_load = 0xFFFFFFFFu;
            for (uint32_t k = 0; k < CPU_MAX_APICS; k++) {
                uint32_t c = (rr + k) % CPU_MAX_APICS;
                percpu_t *p = percpu_by_index(c);
                if (!p || !p->online) continue;
                uint32_t ld = s_runq[c].count + ((c == 0) ? 1u : 0u);
                if (ld < best_load) {
                    best_load = ld;
                    best_c = c;
                }
            }
            bound = best_c;
        }
    }
    t->bound_cpu = bound;
    spin_lock(&s_runq[bound].lock);
    rq_push_locked(&s_runq[bound], idx);
    spin_unlock(&s_runq[bound].lock);

    sched_irq_restore(f);
    sched_resched_notify(bound);   /* AP kuyruguna girdiyse hemen uyandir */
    return idx;
}

sched_tid_t sched_spawn_ex(const char *name, void (*fn)(void *), void *arg, uint64_t cr3) {
    return sched_alloc_task(name, fn, arg, cr3, 0, 0);
}

sched_tid_t sched_spawn_stack(const char *name, void (*fn)(void *), void *arg,
                              uint64_t cr3, void *stack, uint32_t stack_size) {
    return sched_alloc_task(name, fn, arg, cr3, stack, stack_size);
}

int sched_state(sched_tid_t tid) {
    sched_task_t *t = task_at(tid);
    return t ? __atomic_load_n(&t->state, __ATOMIC_ACQUIRE) : TASK_UNUSED;
}

uint64_t sched_current_cr3(void) {
    return vmm_current();
}

void sched_prepare_block(void) {
    if (!s_ready) return;
    sched_task_t *me = sched_self_current();
    if (!me) return;
    me->wake_tick = s_tick + 0x40000000u;
    __atomic_store_n(&me->state, TASK_BLOCKED, __ATOMIC_RELEASE);
}

void sched_prepare_sleep_ms(uint32_t ms) {
    if (!s_ready) return;
    sched_task_t *me = sched_self_current();
    if (!me) return;
    me->wake_tick = s_tick + (ms ? ms : 1u);
    __atomic_store_n(&me->state, TASK_BLOCKED, __ATOMIC_RELEASE);
}

void sched_commit_block(void) {
    if (!s_ready) return;
    sched_task_t *me = sched_self_current();
    if (!me) return;
    uint64_t f = sched_irq_save();
    uint32_t ci = sched_self_cpu();
    if (ci == 0) {
        if (__atomic_load_n(&me->state, __ATOMIC_ACQUIRE) == TASK_BLOCKED)
            schedule();
        while (__atomic_load_n(&me->state, __ATOMIC_ACQUIRE) == TASK_BLOCKED)
            __asm__ volatile("sti; hlt; cli" ::: "memory");
        if (__atomic_load_n(&me->state, __ATOMIC_ACQUIRE) == TASK_READY) {
            spin_lock(&s_runq[0].lock);
            if (me->in_q && me->bound_cpu == 0) rq_detach_locked(&s_runq[0], me->id);
            __atomic_store_n(&me->state, TASK_RUNNING, __ATOMIC_RELEASE);
            spin_unlock(&s_runq[0].lock);
        }
    } else {
        if (__atomic_load_n(&me->state, __ATOMIC_ACQUIRE) == TASK_BLOCKED) {
            if (sched_pick_local() < 0 &&
                __atomic_load_n(&me->state, __ATOMIC_ACQUIRE) == TASK_BLOCKED) {
                /* AP'de baska gorev yok: yigindan inip AP idle'a don.
                 * Uyanma (sched_wake) gelince ap_entry bizi geri secer. */
                percpu_t *p = percpu_by_index(ci);
                if (p) { p->current_task = 0; p->idle = true; }
                sched_switch_to(me, &s_ap_idle[ci]);
            }
        }
        if (__atomic_load_n(&me->state, __ATOMIC_ACQUIRE) == TASK_READY) {
            uint32_t c2 = sched_self_cpu();
            spin_lock(&s_runq[c2].lock);
            if (me->in_q && me->bound_cpu == c2) rq_detach_locked(&s_runq[c2], me->id);
            __atomic_store_n(&me->state, TASK_RUNNING, __ATOMIC_RELEASE);
            spin_unlock(&s_runq[c2].lock);
        }
    }
    sched_irq_restore(f);
}

void sched_block_current(void) {
    if (!s_ready) return;
    uint64_t f = sched_irq_save();
    sched_prepare_block();
    sched_commit_block();
    sched_irq_restore(f);
}

void sched_wake(sched_tid_t tid) {
    sched_task_t *t = task_at(tid);
    if (!t) return;
    int st = __atomic_load_n(&t->state, __ATOMIC_ACQUIRE);
    if (st == TASK_FROZEN) {
        t->frozen_prev_state = TASK_READY;
        return;
    }
    int exp = TASK_BLOCKED;
    if (__atomic_compare_exchange_n(&t->state, &exp, TASK_READY, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        rq_enqueue_bound(tid);   /* AP kuyruklari icin; dedupe in_q ile */
    }
}

void sched_reap(sched_tid_t tid) {
    if (tid <= 0) return;
    sched_task_t *t = task_at(tid);
    if (!t) return;
    uint64_t f = sched_irq_save();
    if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == TASK_DONE) {
        /* Gorev AP yiginindan tamamen inene kadar bekle (kfree guvenligi). */
        while (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE))
            __asm__ volatile("pause" ::: "memory");
        uint32_t bc = t->bound_cpu;
        if (bc < CPU_MAX_APICS && t->in_q) {
            spin_lock(&s_runq[bc].lock);
            rq_detach_locked(&s_runq[bc], tid);
            spin_unlock(&s_runq[bc].lock);
        }
        /* C4: heap'ten ayrilan yigin geri verilir; spawn-stack (extern
         * yiginli) gorevlerde stack_heap=0'dir, dokunulmaz. */
        if (t->stack_heap && t->stack_base)
            kfree(t->stack_base);
        spin_lock(&s_tasks_lock);
        t->owner = 0;
        t->name = 0;
        t->stack_base = 0;
        t->stack_heap = 0;
        t->user_app = 0;
        t->freeze_exempt = 0;
        __atomic_store_n(&t->state, TASK_UNUSED, __ATOMIC_RELEASE);
        spin_unlock(&s_tasks_lock);
    }
    sched_irq_restore(f);
}

void sched_set_owner(sched_tid_t tid, void *owner) {
    sched_task_t *t = task_at(tid);
    if (t) t->owner = owner;
}

void *sched_owner(sched_tid_t tid) {
    sched_task_t *t = task_at(tid);
    return t ? t->owner : 0;
}

void *sched_owner_current(void) {
    sched_task_t *me = sched_self_current();
    return me ? me->owner : 0;
}

void sched_set_fs_base(uint64_t fs_base) {
    sched_task_t *me = sched_self_current();
    if (me) me->fs_base = fs_base;
    sched_wrmsr_fs(fs_base);
}

uint64_t sched_get_fs_base(void) {
    sched_task_t *me = sched_self_current();
    return me ? me->fs_base : 0;
}

void sched_set_task_fs_base(sched_tid_t tid, uint64_t fs_base) {
    sched_task_t *t = task_at(tid);
    if (t) t->fs_base = fs_base;
}

void sched_set_posix_thr(sched_tid_t tid, void *thr) {
    sched_task_t *t = task_at(tid);
    if (t) {
        t->posix_thr = thr;
        if (thr) t->user_app = 1u;
    }
}

void *sched_posix_thr_current(void) {
    sched_task_t *me = sched_self_current();
    return me ? me->posix_thr : 0;
}

uint64_t sched_kstack_top_current(void) {
    sched_task_t *me = sched_self_current();
    return me ? me->kstack_top : 0;
}

void sched_set_current_cr3(uint64_t cr3) {
    sched_task_t *me = sched_self_current();
    if (me) {
        me->cr3 = cr3;
        vmm_state_t *vst = VMM_STATE;
        if (me->id > 0 && vst && vst->kernel_space && cr3 && cr3 != vst->kernel_space)
            me->user_app = 1u;
    }
    if (cr3) vmm_switch(cr3);
}

sched_tid_t sched_current(void) {
    sched_task_t *me = sched_self_current();
    return me ? (sched_tid_t)me->id : -1;
}

int sched_task_count(void) {
    int n = 0;
    uint32_t cap = __atomic_load_n(&s_alloc_cap, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < cap; i++) {
        sched_task_t *t = task_at((int)i);
        if (t && __atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != TASK_UNUSED) n++;
    }
    return n;
}

uint32_t sched_task_ticks(sched_tid_t tid) {
    sched_task_t *t = task_at(tid);
    return t ? (uint32_t)t->ticks : 0u;
}

uint32_t sched_total_ticks(void) {
    return (uint32_t)s_total_ticks;
}

void sched_init(void) {
    /* E1: baslangicta tek 64'luk dilim ayrilir; ihtiyac halinde dinamik buyur */
    for (uint32_t c = 0; c < SCHED_MAX_CHUNKS; c++) s_chunks[c] = 0;
    s_chunks[0] = (sched_task_t *)kmalloc(sizeof(sched_task_t) * SCHED_CHUNK_SIZE);
    if (!s_chunks[0]) {
        serial_puts("[sched] HATA: gorev havuzu ayrilamadi, planlayici kapali\n");
        return;
    }
    s_num_chunks = 1u;
    s_alloc_cap = SCHED_CHUNK_SIZE;
    s_capacity = SCHED_CHUNK_SIZE;
    for (uint32_t i = 0; i < SCHED_CHUNK_SIZE; i++) {
        init_task_slot(&s_chunks[0][i], (int)i);
    }
    for (uint32_t c = 0; c < CPU_MAX_APICS; c++) {
        spinlock_t l = SPINLOCK_INIT;
        s_runq[c] = (runq_t){ .lock = l, .head = -1, .tail = -1, .count = 0 };
        s_ap_idle[c].state = TASK_UNUSED;
    }
    sched_task_t *t0 = &s_chunks[0][0];
    t0->state = TASK_RUNNING;
    t0->on_cpu = 1;
    t0->freeze_exempt = 1;
    t0->name = "main";
    t0->cr3 = vmm_current();
    s_current = t0;
    s_tick = 0;
    s_ready = 1;
    arctian_set_sched_tick(sched_tick);
    lapic_timer_set_handler(sched_tick_local);
    serial_puts("[sched] dinamik olceklenebilir preemptive scheduler aktif\n");
}

/* ---- paket 7: per-CPU run queue + yerel secim ----
 *
 * BSP (cpu 0): legacy schedule() aynen calisir; runq[0] yalniz ayna
 * kayittir, pick_local(0) legacy yola yonlendirir. Preemption davranisi
 * degismez.
 * AP: current task percpu_t.current_task'ta tutulur (s_current yalniz
 * BSP'ninkidir). AP kendi kuyrugundan secip s_ap_idle[..] uzerinden
 * goreve gecer. Work stealing YOK (paket 8).
 * Kilit sirasi: runq kilidi -> baska kilit yok; ic ice kilitlenme yok. */

static volatile uint32_t s_ap_tick[CPU_MAX_APICS];

static inline uint32_t sched_self_cpu(void) {
    uint32_t ci = percpu_self_index();
    return (ci < CPU_MAX_APICS) ? ci : 0u;
}

/* Bu CPU'nun calisan gorevi. CPU 0'da s_current (legacy).
 *
 * 0.9.1 sertlestirme: s_current bir gorev tablosu isaretcisi olmali.
 * Ring 3 izolasyon selftesti (B1) sonrasi sahada gozlenen bozulmada bu
 * alan cop deger tasiyordu; sched_current() ondan anlamsiz bir tid
 * uretiyor ve yetki sorgulari #GP firtinasina giriyordu. Gecersiz
 * isaretci "gorev yok" sayilir; cagiranlar guvenli varsayilana duser. */
/* sched_task_ptr_valid yukarida tanimlandi */

static sched_task_t *sched_self_current(void) {
    uint32_t ci = sched_self_cpu();
    if (ci == 0) {
        if (!sched_task_ptr_valid(s_current)) {
            static int s_warned;
            if (!s_warned) {
                s_warned = 1;
                serial_printf("[sched] UYARI: s_current bozuk (0x%x) -> gorev yok\n",
                              (unsigned)(uintptr_t)s_current);
            }
            return 0;
        }
        return s_current;
    }
    percpu_t *p = percpu_by_index(ci);
    if (!p) return 0;
    sched_task_t *t = (sched_task_t *)p->current_task;
    return sched_task_ptr_valid(t) ? t : (sched_task_t *)0;
}

static void sched_set_self_current(sched_task_t *t) {
    uint32_t ci = sched_self_cpu();
    if (ci == 0) { s_current = t; return; }
    percpu_t *p = percpu_by_index(ci);
    if (p) p->current_task = t;
}

int sched_enqueue_cpu(uint32_t cpu, sched_tid_t tid) {
    if (!s_ready) return -1;
    if (tid <= 0) return -1;                  /* main gorevi tasinmaz */
    if (cpu >= CPU_MAX_APICS) return -1;
    sched_task_t *t = task_at(tid);
    if (!t) return -1;
    int st = __atomic_load_n(&t->state, __ATOMIC_ACQUIRE);
    if (st == TASK_UNUSED || st == TASK_DONE) return -1;

    /* Eski kuyruktan cikar (kilitleri ic ice almadan: tek tek). */
    if (t->in_q && t->bound_cpu != cpu) {
        uint32_t oc = t->bound_cpu;
        if (oc < CPU_MAX_APICS) {
            uint64_t f = irq_save();
            spin_lock(&s_runq[oc].lock);
            rq_detach_locked(&s_runq[oc], tid);
            spin_unlock(&s_runq[oc].lock);
            irq_restore(f);
        } else {
            t->in_q = 0;
        }
    }
    uint64_t f = irq_save();
    spin_lock(&s_runq[cpu].lock);
    t->bound_cpu = cpu;
    int pushed = 0;
    if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == TASK_CARRY)
        __atomic_store_n(&t->state, TASK_READY, __ATOMIC_RELEASE);
    if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == TASK_READY) {
        rq_push_locked(&s_runq[cpu], tid);
        pushed = 1;
    }
    spin_unlock(&s_runq[cpu].lock);
    irq_restore(f);
    if (pushed) sched_resched_notify(cpu);
    return 0;
}

sched_tid_t sched_pick_local(void) {
    if (!s_ready) return -1;
    uint32_t ci = sched_self_cpu();

    if (ci == 0) {                 /* BSP: legacy yol, davranis ayni */
        uint64_t f = irq_save();
        schedule();
        irq_restore(f);
        return s_current ? (sched_tid_t)s_current->id : -1;
    }

    runq_t *rq = &s_runq[ci];
    uint64_t f = irq_save();
    spin_lock(&rq->lock);
    int cand;
    sched_task_t *to = 0;
    while ((cand = rq_pop_locked(rq)) >= 0) {
        sched_task_t *ct = task_at(cand);
        if (ct && __atomic_load_n(&ct->state, __ATOMIC_ACQUIRE) == TASK_READY) {
            to = ct;
            break;
        }
    }
    if (!to) {
        spin_unlock(&rq->lock);
        int stolen = steal_runq_locked(ci);
        spin_lock(&rq->lock);
        if (stolen >= 0) {
            sched_task_t *st = task_at(stolen);
            if (st && __atomic_load_n(&st->state, __ATOMIC_ACQUIRE) == TASK_CARRY) {
                __atomic_store_n(&st->state, TASK_READY, __ATOMIC_RELEASE);
                rq_push_locked(rq, stolen);
            }
        }
        while ((cand = rq_pop_locked(rq)) >= 0) {
            sched_task_t *ct = task_at(cand);
            if (ct && __atomic_load_n(&ct->state, __ATOMIC_ACQUIRE) == TASK_READY) {
                to = ct;
                break;
            }
        }
    }
    if (!to) {
        sched_task_t *cur = sched_self_current();
        if (cur) {
            int cst = __atomic_load_n(&cur->state, __ATOMIC_ACQUIRE);
            if (cst == TASK_DONE || cst == TASK_FROZEN || cst == TASK_BLOCKED) {
                percpu_t *p = percpu_by_index(ci);
                if (p) { p->current_task = 0; p->idle = true; }
                spin_unlock(&rq->lock);
                vmm_state_t *vst = VMM_STATE;
                if (vst && vst->kernel_space) s_ap_idle[ci].cr3 = vst->kernel_space;
                sched_switch_to(cur, &s_ap_idle[ci]);
                irq_restore(f);
                return -1;
            }
        }
        spin_unlock(&rq->lock);
        irq_restore(f);
        return -1;
    }
    sched_task_t *cur = sched_self_current();
    if (cur == to) {
        __atomic_store_n(&to->state, TASK_RUNNING, __ATOMIC_RELEASE);
        spin_unlock(&rq->lock);
        irq_restore(f);
        return to->id;
    }
    if (cur && cur != to && __atomic_load_n(&cur->state, __ATOMIC_ACQUIRE) == TASK_RUNNING) {
        __atomic_store_n(&cur->state, TASK_READY, __ATOMIC_RELEASE);
        rq_push_locked(rq, cur->id);
    }
    __atomic_store_n(&to->state, TASK_RUNNING, __ATOMIC_RELEASE);
    sched_set_self_current(to);
    to->switches++;
    spin_unlock(&rq->lock);

    if (!cur) {
        cur = &s_ap_idle[ci];
        serial_printf("[sched] cpu%u gorev devraldi: %s\n",
                      ci, to->name ? to->name : "?");
    }
    sched_switch_to(cur, to);                 /* donus: gorev buraya resumes */
    irq_restore(f);
    return to->id;
}

/* LAPIC timer kancasi */
void sched_tick_local(void) {
    uint32_t ci = sched_self_cpu();
    if (ci == 0) { sched_tick(); return; }

    uint32_t n = ++s_ap_tick[ci];
    if (n == 1)
        serial_printf("[sched] cpu%u lapic tik aliyor\n", ci);
    sched_task_t *cur = sched_self_current();
    int cst = cur ? __atomic_load_n(&cur->state, __ATOMIC_ACQUIRE) : TASK_UNUSED;
    if (cur && cst == TASK_RUNNING) cur->ticks++;
    if ((!cur || (cst != TASK_DONE && cst != TASK_FROZEN)) && (n % SCHED_SLICE_TICKS) != 0)
        return;

    sched_pick_local();
}

void sched_terminate_task(sched_tid_t tid) {
    if (!s_ready || tid <= 0) return;
    sched_task_t *t = task_at(tid);
    if (!t || t == sched_self_current()) return;
    __atomic_store_n(&t->state, TASK_DONE, __ATOMIC_RELEASE);
    uint32_t cpu = t->bound_cpu;
    if (cpu < CPU_MAX_APICS) {
        uint64_t f = irq_save();
        spin_lock(&s_runq[cpu].lock);
        rq_detach_locked(&s_runq[cpu], tid);
        spin_unlock(&s_runq[cpu].lock);
        irq_restore(f);
    }
    if (cpu > 0 && cpu < CPU_MAX_APICS)
        sched_resched_notify(cpu);
    /* Gorevin diger CPU'nun yiginindan ve CR3'unden inmesini bekle. */
    for (int tries = 0; tries < 200000; tries++) {
        if (!__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) break;
        if ((tries & 1023) == 0 && cpu > 0 && cpu < CPU_MAX_APICS)
            sched_resched_notify(cpu);
        __asm__ volatile("pause" ::: "memory");
    }
}

/* ---- C3: resched IPI + TLB shootdown (vektor 0x41 / 0x42) ----
 * ISR dagitimi main_kernel/isr.c (zayif sembol); EOI'yi burada gondeririz. */

void smp_resched_irq(void) {
    lapic_eoi();
    /* BSP'nin PIT tiki 1 ms'de bir zaten secim yapar; bildirim yersiz. */
    if (sched_self_cpu() == 0) return;
    sched_pick_local();
}

void smp_tlb_irq(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");
    lapic_eoi();
}

/* Tum diger cevrimici CPU'larda TLB'yi bosalt + burada da CR3 yenile.
 * PCID yok; CR3 yeniden yuklemesi cekirdek-eslemeleri dahil tam bosaltir
 * (basit ve dogru; maliyet dusuk - unmap seyrek bir islem). */
void smp_tlb_shootdown_all(void) {
    uint64_t f = irq_save();
    lapic_send_ipi_all_excl_self(LAPIC_TLB_VECTOR);
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");
    irq_restore(f);
}

/* Hedef AP'nin kuyruguna bir sey girdiyse onu bekletme: resched IPI.
 * YALNIZ kilit disindan cagrilir (ICR beklemesi milisaniye surabilir);
 * hedef cevrimdisi ise sessizce atla - 5 ms'lik tik dilimi garanti. */
static void sched_resched_notify(uint32_t cpu) {
    if (cpu == 0 || cpu >= CPU_MAX_APICS) return;   /* BSP zaten tikliyor */
    if (cpu == sched_self_cpu()) return;
    percpu_t *p = percpu_by_index(cpu);
    if (!p || !p->online) return;
    lapic_send_ipi(p->apic_id, LAPIC_RESCHED_VECTOR);
}

static volatile uint64_t s_count1;
static volatile uint64_t s_count2;
static volatile int s_stop;

static void sched_counter_task(void *arg) {
    volatile uint64_t *c = (volatile uint64_t *)arg;
    while (!s_stop) {
        (*c)++;
        for (volatile int i = 0; i < 2000; i++) __asm__ volatile("pause");
    }
    sched_exit();
}

static void sched_monitor_task(void *arg) {
    (void)arg;
    for (int i = 0; i < 40; i++) sched_sleep_ms(10);
    uint64_t a = s_count1;
    uint64_t b = s_count2;
    int ok = (a > 0 && b > 0);
    serial_printf("[sched] tick testi: sayac1=%u sayac2=%u -> %s\n",
                  (unsigned)a, (unsigned)b, ok ? "EVET (ikisi de ilerledi)"
                                               : "HAYIR");
    s_stop = 1;
    sched_exit();
}

int sched_selftest(void) {
    s_count1 = 0;
    s_count2 = 0;
    s_stop = 0;

    sched_tid_t a = sched_spawn("sayac1", sched_counter_task, (void *)&s_count1);
    sched_tid_t b = sched_spawn("sayac2", sched_counter_task, (void *)&s_count2);
    sched_tid_t m = sched_spawn("izleyici", sched_monitor_task, 0);
    if (a < 0 || b < 0 || m < 0) {
        serial_puts("[sched] selftest: task olusturulamadi\n");
        return -1;
    }
    serial_puts("[sched] selftest basladi: 2 sayac + izleyici (UI ile paralel)\n");
    return 0;
}

/* ---- E1: OOM Dondurma / Cozme Motoru ---- */

static int str_eq_prefix(const char *s, const char *p) {
    if (!s || !p) return 0;
    while (*p) {
        if (*s++ != *p++) return 0;
    }
    return 1;
}

static int task_is_protected(const sched_task_t *t) {
    if (!t || t->id <= 0 || t->freeze_exempt) return 1;
    if (t->name) {
        if (str_eq_prefix(t->name, "main") ||
            str_eq_prefix(t->name, "netbg") ||
            str_eq_prefix(t->name, "bootanim") ||
            str_eq_prefix(t->name, "idle"))
            return 1;
    }
    return 0;
}

void sched_mark_user_app(sched_tid_t tid, int is_user) {
    sched_task_t *t = task_at(tid);
    if (t && t->id > 0) t->user_app = is_user ? 1u : 0u;
}

void sched_mark_freeze_exempt(sched_tid_t tid, int exempt) {
    sched_task_t *t = task_at(tid);
    if (t) t->freeze_exempt = exempt ? 1u : 0u;
}

int sched_is_frozen(sched_tid_t tid) {
    sched_task_t *t = task_at(tid);
    if (!t) return 0;
    return (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == TASK_FROZEN) ? 1 : 0;
}

int sched_freeze_task(sched_tid_t tid) {
    if (!s_ready || tid <= 0) return -1;
    sched_task_t *t = task_at(tid);
    if (!t || task_is_protected(t) || t == sched_self_current()) return -1;

    uint64_t f = sched_irq_save();
    uint32_t bc = t->bound_cpu;
    if (bc >= CPU_MAX_APICS) bc = 0;
    spin_lock(&s_runq[bc].lock);
    int st = __atomic_load_n(&t->state, __ATOMIC_ACQUIRE);
    if (st == TASK_FROZEN) {
        spin_unlock(&s_runq[bc].lock);
        sched_irq_restore(f);
        return 0;
    }
    if (st == TASK_READY || st == TASK_RUNNING || st == TASK_BLOCKED) {
        t->frozen_prev_state = (st == TASK_BLOCKED) ? (uint8_t)TASK_BLOCKED : (uint8_t)TASK_READY;
        __atomic_store_n(&t->state, TASK_FROZEN, __ATOMIC_RELEASE);
        if (t->in_q) rq_detach_locked(&s_runq[bc], tid);
        spin_unlock(&s_runq[bc].lock);
        sched_irq_restore(f);
        if (st == TASK_RUNNING && bc > 0) sched_resched_notify(bc);
        return 0;
    }
    spin_unlock(&s_runq[bc].lock);
    sched_irq_restore(f);
    return -1;
}

int sched_thaw_task(sched_tid_t tid) {
    if (!s_ready || tid <= 0) return -1;
    sched_task_t *t = task_at(tid);
    if (!t) return -1;

    uint64_t f = sched_irq_save();
    if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != TASK_FROZEN) {
        sched_irq_restore(f);
        return -1;
    }
    /* Eger eski CPU hala bu gorevin yiginindan iniyorsa bekle */
    if (t != sched_self_current()) {
        for (int tries = 0; tries < 50000 && __atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE); tries++)
            __asm__ volatile("pause" ::: "memory");
    }
    int target = (t->frozen_prev_state == TASK_BLOCKED) ? TASK_BLOCKED : TASK_READY;
    __atomic_store_n(&t->state, target, __ATOMIC_RELEASE);
    sched_irq_restore(f);

    if (target == TASK_READY) rq_enqueue_bound(tid);
    return 0;
}

void sched_freeze_user_apps(void) {
    if (!s_ready) return;
    vmm_state_t *vst = VMM_STATE;
    uint64_t ksp = vst ? vst->kernel_space : 0;
    uint32_t cap = __atomic_load_n(&s_alloc_cap, __ATOMIC_ACQUIRE);
    for (uint32_t i = 1; i < cap; i++) {
        sched_task_t *t = task_at((int)i);
        if (!t || task_is_protected(t)) continue;
        int st = __atomic_load_n(&t->state, __ATOMIC_ACQUIRE);
        if (st != TASK_READY && st != TASK_RUNNING && st != TASK_BLOCKED) continue;
        int is_user = (t->user_app != 0) || (t->posix_thr != 0) ||
                      (t->cr3 && ksp && t->cr3 != ksp);
        if (is_user) (void)sched_freeze_task((sched_tid_t)i);
    }
}

void sched_thaw_user_apps(void) {
    if (!s_ready) return;
    uint32_t cap = __atomic_load_n(&s_alloc_cap, __ATOMIC_ACQUIRE);
    for (uint32_t i = 1; i < cap; i++) {
        sched_task_t *t = task_at((int)i);
        if (t && __atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == TASK_FROZEN) {
            (void)sched_thaw_task((sched_tid_t)i);
        }
    }
}

/* ---- E1 Selftest: >64 dinamik gorev + freeze/thaw + OOM basinc ---- */
static volatile uint32_t s_e1_done_cnt;
static volatile uint32_t s_e1_freeze_cnt;
static volatile int s_e1_freeze_stop;

static void e1_scale_worker(void *arg) {
    (void)arg;
    for (volatile int i = 0; i < 5000; i++) __asm__ volatile("pause");
    __atomic_fetch_add(&s_e1_done_cnt, 1u, __ATOMIC_RELAXED);
    sched_exit();
}

static void e1_freeze_worker(void *arg) {
    (void)arg;
    while (!__atomic_load_n(&s_e1_freeze_stop, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_add(&s_e1_freeze_cnt, 1u, __ATOMIC_RELAXED);
        for (volatile int i = 0; i < 2000; i++) __asm__ volatile("pause");
    }
    sched_exit();
}

int sched_e1_selftest(void) {
    enum { E1_N = 72 };
    sched_tid_t tids[E1_N];
    s_e1_done_cnt = 0;
    int spawned = 0;
    for (int i = 0; i < E1_N; i++) {
        tids[i] = sched_spawn("e1-scale", e1_scale_worker, 0);
        if (tids[i] > 0) spawned++;
    }
    for (int w = 0; w < 300; w++) {
        if (__atomic_load_n(&s_e1_done_cnt, __ATOMIC_ACQUIRE) >= (uint32_t)spawned) break;
        sched_sleep_ms(5);
    }
    for (int i = 0; i < E1_N; i++) {
        if (tids[i] > 0) sched_reap(tids[i]);
    }
    int scale_ok = (spawned == E1_N && s_e1_done_cnt == E1_N);

    /* Freeze / thaw testi */
    s_e1_freeze_cnt = 0;
    s_e1_freeze_stop = 0;
    sched_tid_t ftid = sched_spawn("e1-freeze", e1_freeze_worker, 0);
    int freeze_ok = 0;
    if (ftid > 0) {
        sched_mark_user_app(ftid, 1);
        sched_sleep_ms(20);
        uint32_t before = __atomic_load_n(&s_e1_freeze_cnt, __ATOMIC_ACQUIRE);
        sched_freeze_user_apps();
        int is_frz = sched_is_frozen(ftid);
        sched_sleep_ms(10);
        uint32_t f1 = __atomic_load_n(&s_e1_freeze_cnt, __ATOMIC_ACQUIRE);
        sched_sleep_ms(20);
        uint32_t f2 = __atomic_load_n(&s_e1_freeze_cnt, __ATOMIC_ACQUIRE);
        sched_thaw_user_apps();
        int is_unfrz = !sched_is_frozen(ftid);
        sched_sleep_ms(20);
        uint32_t after = __atomic_load_n(&s_e1_freeze_cnt, __ATOMIC_ACQUIRE);
        __atomic_store_n(&s_e1_freeze_stop, 1, __ATOMIC_RELEASE);
        for (int w = 0; w < 100 && sched_state(ftid) != TASK_DONE; w++) sched_sleep_ms(5);
        sched_reap(ftid);
        freeze_ok = (before > 0 && is_frz && f2 == f1 && is_unfrz && after > f2);
    }

    int oom0 = vmm_oom_pressure();
    vmm_oom_set_force(VMM_OOM_CRITICAL);
    int oom2 = vmm_oom_pressure();
    vmm_oom_set_force(0);
    int oom_back = vmm_oom_pressure();
    int oom_ok = (oom2 == VMM_OOM_CRITICAL && oom_back == oom0);

    serial_printf("[e1-selftest] scale=%d/%d(%s) freeze=%s oom=%s cap=%d\n",
                  (int)s_e1_done_cnt, E1_N, scale_ok ? "OK" : "FAIL",
                  freeze_ok ? "OK" : "FAIL", oom_ok ? "OK" : "FAIL",
                  sched_capacity());
    return (scale_ok && freeze_ok && oom_ok) ? 0 : -1;
}
