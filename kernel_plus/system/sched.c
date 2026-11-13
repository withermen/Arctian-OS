#include "sched.h"
#include "isr.h"
#include "serial.h"
#include "paging.h"
#include "spinlock.h"
#include "percpu.h"
#include "lapic.h"

#define SCHED_STACK_SIZE   (32u * 1024u)
#define SCHED_SLICE_TICKS  5u

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
    void *owner;
    uint64_t ticks;
    /* Per-CPU run queue baglanti alanlari (paket 7). */
    int32_t next_q;               /* kuyrukta sonraki task indexi, -1 = yok */
    uint8_t in_q;                 /* bir kuyruga bagli mi (tek kuyruk garantisi) */
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
                                 void *from_fpu, void *to_fpu);
extern void arctian_task_entry(void);

static sched_task_t s_tasks[SCHED_MAX_TASKS] __attribute__((aligned(16)));
static uint8_t s_stacks[SCHED_MAX_TASKS][SCHED_STACK_SIZE] __attribute__((aligned(16)));
static sched_task_t *s_current;    /* yalniz CPU 0 (BSP) icin gecerli */
static volatile uint32_t s_tick;
static volatile uint64_t s_total_ticks;
static volatile int s_ready;

static runq_t s_runq[CPU_MAX_APICS];

/* AP ilk goreve gecerken "uzerinden gecilen" baglam: AP'nin park dongusu
 * kesimden once calisan bir gorev degildi; switch buradan yapilir. */
static sched_task_t s_ap_idle[CPU_MAX_APICS] __attribute__((aligned(16)));

/* ---- run queue yardimcilari (cagiran ilgili rq->lock'u tutmali) ---- */

static void rq_push_locked(runq_t *rq, int tid) {
    sched_task_t *t = &s_tasks[tid];
    if (t->in_q) return;
    t->in_q = 1;
    t->next_q = -1;
    if (rq->tail >= 0) s_tasks[rq->tail].next_q = tid;
    else rq->head = tid;
    rq->tail = tid;
    rq->count++;
}

static int rq_pop_locked(runq_t *rq) {
    int tid = rq->head;
    if (tid < 0) return -1;
    rq->head = s_tasks[tid].next_q;
    if (rq->head < 0) rq->tail = -1;
    s_tasks[tid].next_q = -1;
    s_tasks[tid].in_q = 0;
    rq->count--;
    return tid;
}

static void rq_detach_locked(runq_t *rq, int tid) {
    if (!s_tasks[tid].in_q) return;
    int prev = -1;
    for (int cur = rq->head; cur >= 0; cur = s_tasks[cur].next_q) {
        if (cur == tid) {
            if (prev >= 0) s_tasks[prev].next_q = s_tasks[tid].next_q;
            else rq->head = s_tasks[tid].next_q;
            if (rq->tail == tid) rq->tail = prev;
            s_tasks[tid].next_q = -1;
            s_tasks[tid].in_q = 0;
            rq->count--;
            return;
        }
        prev = cur;
    }
}

/* Gorevi bound_cpu kuyruguna READY ise ekler (dedupe in_q ile). */
static void rq_enqueue_bound(int tid) {
    if (tid < 0 || tid >= SCHED_MAX_TASKS) return;
    sched_task_t *t = &s_tasks[tid];
    if (t->state != TASK_READY || t->bound_cpu >= CPU_MAX_APICS) return;
    runq_t *rq = &s_runq[t->bound_cpu];
    uint64_t f = irq_save();
    spin_lock(&rq->lock);
    rq_push_locked(rq, tid);
    spin_unlock(&rq->lock);
    irq_restore(f);
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
    if (to->cr3 && to->cr3 != from->cr3) vmm_switch(to->cr3);
    arctian_sched_switch(&from->rsp, &to->rsp, from->fpu, to->fpu);
}

static void schedule(void) {
    sched_task_t *cur = s_current;
    if (!cur) return;

    sched_task_t *next = 0;
    for (int k = 1; k < SCHED_MAX_TASKS; k++) {
        int idx = (cur->id + k) % SCHED_MAX_TASKS;
        sched_task_t *t = &s_tasks[idx];
        if (t->state == TASK_READY) { next = t; break; }
    }

    if (!next) {
        if (cur->state != TASK_RUNNING && s_tasks[0].state == TASK_READY)
            next = &s_tasks[0];
        if (!next) return;
    }
    if (next == cur) return;

    if (cur->state == TASK_RUNNING) cur->state = TASK_READY;
    next->state = TASK_RUNNING;
    s_current = next;
    cur->switches++;
    sched_switch_to(cur, next);
}

static void sched_idle(void) {
    while (s_current && s_current->state == TASK_BLOCKED)
        __asm__ volatile("sti; hlt" ::: "memory");
}

void sched_tick(void) {
    if (!s_ready) return;
    s_tick++;
    s_total_ticks++;
    if (s_current && s_current->state == TASK_RUNNING) s_current->ticks++;

    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        sched_task_t *t = &s_tasks[i];
        if (t->state == TASK_BLOCKED &&
            (int32_t)(s_tick - t->wake_tick) >= 0) {
            t->state = TASK_READY;
            rq_enqueue_bound(i);   /* AP kuyruklari icin; cpu0 ayna kayd */
        }
    }

    if ((s_tick % SCHED_SLICE_TICKS) != 0) return;
    schedule();
}

void sched_yield(void) {
    if (!s_ready) return;
    uint64_t f = sched_irq_save();
    schedule();
    sched_irq_restore(f);
}

void sched_sleep_ms(uint32_t ms) {
    if (!s_ready || ms == 0) return;
    uint64_t f = sched_irq_save();
    s_current->wake_tick = s_tick + ms;
    s_current->state = TASK_BLOCKED;
    schedule();
    sched_idle();
    sched_irq_restore(f);
}

void sched_exit(void) {
    uint64_t f = sched_irq_save();
    s_current->state = TASK_DONE;
    schedule();
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
    for (int i = 1; i < SCHED_MAX_TASKS; i++) {
        if (s_tasks[i].state == TASK_UNUSED) { idx = i; break; }
    }
    if (idx < 0) { sched_irq_restore(f); return -1; }

    sched_task_t *t = &s_tasks[idx];
    void *base = stack ? stack : s_stacks[idx];
    uint32_t size = stack ? stack_size : SCHED_STACK_SIZE;
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
    t->state = TASK_READY;
    t->id = idx;
    t->name = name;
    t->stack_base = (uint8_t *)base;
    t->wake_tick = 0;
    t->switches = 0;
    t->cr3 = cr3 ? cr3 : vmm_current();
    t->owner = 0;
    t->next_q = -1;
    t->in_q = 0;
    t->bound_cpu = 0;                    /* varsayilan: BSP kuyrugu */
    spin_lock(&s_runq[0].lock);          /* ayna kayit; legacy tarama da var */
    rq_push_locked(&s_runq[0], idx);
    spin_unlock(&s_runq[0].lock);

    sched_irq_restore(f);
    serial_printf("[sched] task olustur: %s (id=%d)\n", name ? name : "?", idx);
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
    if (tid < 0 || tid >= SCHED_MAX_TASKS) return TASK_UNUSED;
    return s_tasks[tid].state;
}

uint64_t sched_current_cr3(void) {
    return vmm_current();
}

void sched_block_current(void) {
    if (!s_ready) return;
    uint64_t f = sched_irq_save();
    if (s_current && s_current->state == TASK_RUNNING) {
        s_current->state = TASK_BLOCKED;
        s_current->wake_tick = s_tick + 0x40000000u;
    }
    schedule();
    sched_idle();
    sched_irq_restore(f);
}

void sched_wake(sched_tid_t tid) {
    if (tid < 0 || tid >= SCHED_MAX_TASKS) return;
    uint64_t f = sched_irq_save();
    if (s_tasks[tid].state == TASK_BLOCKED) s_tasks[tid].state = TASK_READY;
    sched_irq_restore(f);
    rq_enqueue_bound(tid);   /* AP kuyruklari icin; dedupe in_q ile */
}

void sched_reap(sched_tid_t tid) {
    if (tid <= 0 || tid >= SCHED_MAX_TASKS) return;
    uint64_t f = sched_irq_save();
    if (s_tasks[tid].state == TASK_DONE) {
        uint32_t bc = s_tasks[tid].bound_cpu;
        if (bc < CPU_MAX_APICS && s_tasks[tid].in_q) {
            spin_lock(&s_runq[bc].lock);
            rq_detach_locked(&s_runq[bc], tid);
            spin_unlock(&s_runq[bc].lock);
        }
        s_tasks[tid].state = TASK_UNUSED;
        s_tasks[tid].owner = 0;
        s_tasks[tid].name = 0;
    }
    sched_irq_restore(f);
}

void sched_set_owner(sched_tid_t tid, void *owner) {
    if (tid < 0 || tid >= SCHED_MAX_TASKS) return;
    s_tasks[tid].owner = owner;
}

void *sched_owner(sched_tid_t tid) {
    if (tid < 0 || tid >= SCHED_MAX_TASKS) return 0;
    return s_tasks[tid].owner;
}

void *sched_owner_current(void) {
    return s_current ? s_current->owner : 0;
}

sched_tid_t sched_current(void) {
    return s_current ? s_current->id : -1;
}

int sched_task_count(void) {
    int n = 0;
    for (int i = 0; i < SCHED_MAX_TASKS; i++)
        if (s_tasks[i].state != TASK_UNUSED) n++;
    return n;
}

uint32_t sched_task_ticks(sched_tid_t tid) {
    if (tid < 0 || tid >= SCHED_MAX_TASKS) return 0;
    return (uint32_t)s_tasks[tid].ticks;
}

uint32_t sched_total_ticks(void) {
    return (uint32_t)s_total_ticks;
}

void sched_init(void) {
    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        s_tasks[i].rsp = 0;
        s_tasks[i].state = TASK_UNUSED;
        s_tasks[i].id = i;
        s_tasks[i].name = 0;
        s_tasks[i].stack_base = 0;
        s_tasks[i].wake_tick = 0;
        s_tasks[i].switches = 0;
        s_tasks[i].cr3 = 0;
        s_tasks[i].owner = 0;
        s_tasks[i].ticks = 0;
        s_tasks[i].next_q = -1;
        s_tasks[i].in_q = 0;
        s_tasks[i].bound_cpu = 0;
    }
    for (uint32_t c = 0; c < CPU_MAX_APICS; c++) {
        spinlock_t l = SPINLOCK_INIT;
        s_runq[c] = (runq_t){ .lock = l, .head = -1, .tail = -1, .count = 0 };
        s_ap_idle[c].state = TASK_UNUSED;
    }
    s_tasks[0].state = TASK_RUNNING;
    s_tasks[0].name = "main";
    s_tasks[0].cr3 = vmm_current();
    s_current = &s_tasks[0];
    s_tick = 0;
    s_ready = 1;
    arctian_set_sched_tick(sched_tick);
    lapic_timer_set_handler(sched_tick_local);
    serial_puts("[sched] preemptive scheduler aktif (round-robin)\n");
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

/* Bu CPU'nun calisan gorevi. CPU 0'da s_current (legacy). */
static sched_task_t *sched_self_current(void) {
    uint32_t ci = sched_self_cpu();
    if (ci == 0) return s_current;
    percpu_t *p = percpu_by_index(ci);
    return p ? (sched_task_t *)p->current_task : (sched_task_t *)0;
}

static void sched_set_self_current(sched_task_t *t) {
    uint32_t ci = sched_self_cpu();
    if (ci == 0) { s_current = t; return; }
    percpu_t *p = percpu_by_index(ci);
    if (p) p->current_task = t;
}

int sched_enqueue_cpu(uint32_t cpu, sched_tid_t tid) {
    if (!s_ready) return -1;
    if (tid < 0 || tid >= SCHED_MAX_TASKS) return -1;
    if (tid == 0) return -1;                  /* main gorevi tasinmaz */
    if (cpu >= CPU_MAX_APICS) return -1;
    sched_task_t *t = &s_tasks[tid];
    if (t->state == TASK_UNUSED || t->state == TASK_DONE) return -1;

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
    if (t->state == TASK_READY) rq_push_locked(&s_runq[cpu], tid);
    spin_unlock(&s_runq[cpu].lock);
    irq_restore(f);
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
        if (s_tasks[cand].state == TASK_READY) { to = &s_tasks[cand]; break; }
        /* TASK_RUNNING: bu CPU'da calisiyor olabilir, dokunma.
         * BLOCKED/DONE/UNUSED: bayat girdi, dusur. */
    }
    if (!to) {
        spin_unlock(&rq->lock);
        irq_restore(f);
        return -1;
    }
    sched_task_t *cur = sched_self_current();
    if (cur && cur != to && cur->state == TASK_RUNNING) {
        cur->state = TASK_READY;
        rq_push_locked(rq, cur->id);
    }
    to->state = TASK_RUNNING;
    sched_set_self_current(to);
    to->switches++;
    spin_unlock(&rq->lock);

    if (!cur) cur = &s_ap_idle[ci];           /* AP ilk gecis: park baglami */
    sched_switch_to(cur, to);                 /* donus: gorev buraya resumes */
    irq_restore(f);
    return to->id;
}

/* LAPIC timer kancasi (lapic_timer_irq cagirmayi planlar; bkz. rapor).
 * CPU 0'da legacy sched_tick'e devreder. AP'de: her tick'ta sayac, her
 * SCHED_SLICE_TICKS'ta bir yerel secim; kuyruk bos ve kosan gorev yoksa
 * hlt (kesme acik bekler, sonraki tick uyandirir). */
void sched_tick_local(void) {
    uint32_t ci = sched_self_cpu();
    if (ci == 0) { sched_tick(); return; }

    uint32_t n = ++s_ap_tick[ci];
    if ((n % SCHED_SLICE_TICKS) != 0) return;

    if (sched_pick_local() < 0) {
        sched_task_t *cur = sched_self_current();
        if (!cur || cur->state != TASK_RUNNING)
            __asm__ volatile("sti; hlt" ::: "memory");
    }
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
