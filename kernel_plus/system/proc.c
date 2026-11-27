#include "proc.h"
#include "caps.h"
#include "sched.h"
#include "paging.h"
#include "vmm_space.h"
#include "alloc.h"
#include "spinlock.h"
#include "isr.h"
#include "serial.h"

#define PROC_CHUNK_SHIFT 5u
#define PROC_CHUNK_SIZE  (1u << PROC_CHUNK_SHIFT) /* 32 surec / dilim */
#define PROC_CHUNK_MASK  (PROC_CHUNK_SIZE - 1u)
#define PROC_MAX_CHUNKS  256u                     /* 8192 surece kadar dinamik */

typedef struct {
    int pid;
    int ppid;
    volatile proc_state_t state;
    int exit_code;
    int is_app;
    int is_idle;
    int waiting;
    uint32_t caps;
    uint32_t uid;
    uint32_t gid;
    uint64_t io_bytes;
    uint64_t net_bytes;
    char name[32];
    void (*entry)(void *);
    void *arg;
    vmm_space_t space;
    sched_tid_t task;
} pcb_t;

static pcb_t *s_proc_chunks[PROC_MAX_CHUNKS];
static volatile uint32_t s_proc_num_chunks = 0;
static volatile int s_proc_cap = 0;
static spinlock_t s_proc_lock = SPINLOCK_INIT;
static int s_next_pid = 1;
static int s_init_pid = 1;

extern int  posix_oom_list_cb(asi_oom_app_t *out, int max_out) __attribute__((weak));
extern int  posix_oom_kill_cb(int pid) __attribute__((weak));
extern void posix_oom_freeze_cb(int freeze) __attribute__((weak));

static void name_copy(char *dst, int cap, const char *src) {
    int i = 0;
    if (cap <= 0) return;
    if (!src) { dst[0] = 0; return; }
    for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

static pcb_t *proc_slot(int i) {
    int cap = __atomic_load_n(&s_proc_cap, __ATOMIC_ACQUIRE);
    if (i < 0 || i >= cap) return 0;
    pcb_t *ch = s_proc_chunks[(uint32_t)i >> PROC_CHUNK_SHIFT];
    return ch ? &ch[(uint32_t)i & PROC_CHUNK_MASK] : 0;
}

static pcb_t *proc_find(int pid) {
    if (pid <= 0) return 0;
    int cap = __atomic_load_n(&s_proc_cap, __ATOMIC_ACQUIRE);
    for (int i = 0; i < cap; i++) {
        pcb_t *p = proc_slot(i);
        if (p && p->state != PROC_UNUSED && p->pid == pid)
            return p;
    }
    return 0;
}

static pcb_t *proc_current(void) {
    pcb_t *p = (pcb_t *)sched_owner_current();
    if (p) return p;
    return proc_slot(0);
}

static void proc_init_slot(pcb_t *p) {
    p->pid = 0;
    p->ppid = 0;
    p->state = PROC_UNUSED;
    p->exit_code = 0;
    p->is_app = 0;
    p->is_idle = 0;
    p->waiting = 0;
    p->caps = CAP_ALL;
    p->uid = 0;
    p->gid = 0;
    p->io_bytes = 0;
    p->net_bytes = 0;
    p->name[0] = 0;
    p->entry = 0;
    p->arg = 0;
    p->space = 0;
    p->task = -1;
}

static pcb_t *proc_alloc_slot_locked(uint64_t *fl_inout) {
    for (;;) {
        int cap = s_proc_cap;
        for (int i = 1; i < cap; i++) {
            pcb_t *p = proc_slot(i);
            if (p && p->state == PROC_UNUSED) {
                p->state = PROC_BLOCKED; /* gecici yer ayirma */
                p->pid = -1;
                return p;
            }
        }
        uint32_t nc = s_proc_num_chunks;
        if (nc >= PROC_MAX_CHUNKS) return 0;

        spin_unlock(&s_proc_lock);
        sched_irq_restore(*fl_inout);
        pcb_t *new_ch = (pcb_t *)kmalloc(sizeof(pcb_t) * PROC_CHUNK_SIZE);
        *fl_inout = sched_irq_save();
        if (!new_ch) {
            spin_lock(&s_proc_lock);
            return 0;
        }
        for (uint32_t j = 0; j < PROC_CHUNK_SIZE; j++) proc_init_slot(&new_ch[j]);

        spin_lock(&s_proc_lock);
        if (s_proc_num_chunks == nc && s_proc_num_chunks < PROC_MAX_CHUNKS) {
            s_proc_chunks[s_proc_num_chunks++] = new_ch;
            __atomic_store_n(&s_proc_cap, (int)(s_proc_num_chunks * PROC_CHUNK_SIZE), __ATOMIC_RELEASE);
            new_ch[0].state = PROC_BLOCKED;
            new_ch[0].pid = -1;
            return &new_ch[0];
        }
        spin_unlock(&s_proc_lock);
        kfree(new_ch);
        spin_lock(&s_proc_lock);
    }
}

static void proc_destroy_space_safe(pcb_t *p) {
    if (!p || !p->space) return;
    vmm_state_t *st = VMM_STATE;
    vmm_space_t sp = p->space;
    p->space = 0;
    if (st && st->ready && sp != st->kernel_space && sp != vmm_current()) {
        vmm_space_destroy(sp);
    }
}

static void proc_free(pcb_t *p) {
    if (!p) return;
    if (p->task > 0) sched_reap(p->task);
    proc_destroy_space_safe(p);
    p->state = PROC_UNUSED;
    p->task = -1;
    p->entry = 0;
    p->arg = 0;
    p->pid = 0;
}

static void proc_wake_parent(pcb_t *p) {
    pcb_t *par = proc_find(p->ppid);
    if (par && par->waiting) {
        par->waiting = 0;
        sched_wake(par->task);
    }
}

static void proc_thread_main(void *arg);

static int proc_create(const char *name, void (*entry)(void *), void *arg, int ppid) {
    if (!entry) return -1;
    uint64_t fl = sched_irq_save();
    spin_lock(&s_proc_lock);
    pcb_t *p = proc_alloc_slot_locked(&fl);
    if (!p) { spin_unlock(&s_proc_lock); sched_irq_restore(fl); return -1; }
    int pid = s_next_pid++;
    p->pid = pid;
    p->ppid = ppid > 0 ? ppid : s_init_pid;
    p->exit_code = 0;
    p->is_app = 0;
    p->is_idle = (name && name[0] == 'i' && name[1] == 'd' && name[2] == 'l' && name[3] == 'e') ? 1 : 0;
    p->waiting = 0;
    p->caps = CAP_ALL;
    p->uid = caps_uid();
    p->gid = caps_gid();
    p->io_bytes = 0;
    p->net_bytes = 0;
    p->entry = entry;
    p->arg = arg;
    p->space = 0;
    p->task = -1;
    name_copy(p->name, sizeof(p->name), name ? name : "proc");
    spin_unlock(&s_proc_lock);

    vmm_space_t space = vmm_new_space();
    if (!space) {
        spin_lock(&s_proc_lock);
        proc_free(p);
        spin_unlock(&s_proc_lock);
        sched_irq_restore(fl);
        return -1;
    }
    p->space = space;
    p->state = PROC_READY;

    sched_tid_t tid = sched_spawn_ex(p->name, proc_thread_main, p, space);
    if (tid < 0) {
        spin_lock(&s_proc_lock);
        proc_free(p);
        spin_unlock(&s_proc_lock);
        sched_irq_restore(fl);
        return -1;
    }
    p->task = tid;
    sched_set_owner(tid, p);
    sched_mark_user_app(tid, 1);

    sched_irq_restore(fl);
    serial_printf("[proc] surec olustur: %s (pid=%d, ppid=%d)\n",
                  p->name, p->pid, p->ppid);
    return p->pid;
}

static void proc_thread_main(void *arg) {
    pcb_t *p = (pcb_t *)arg;
    if (!p) sched_exit();
    sched_set_owner(sched_current(), p);
    p->state = PROC_RUNNING;
    if (p->entry) p->entry(p->arg);
    proc_exit(0);
}

extern int posix_fault_handler(int int_no, uint64_t err, uint64_t cr2, uint64_t rip)
    __attribute__((weak));

static void proc_fault_handler(int int_no, uint64_t err, uint64_t cr2, uint64_t rip) {
    if (posix_fault_handler && posix_fault_handler(int_no, err, cr2, rip))
        return;
    (void)err;
    pcb_t *p = (pcb_t *)sched_owner_current();
    if (!p || p->pid <= s_init_pid || p->state != PROC_RUNNING) return;
    p->state = PROC_ZOMBIE;
    p->exit_code = -11;
    serial_printf("[proc] olumcul hata (int=%d cr2=0x%X%08X rip=0x%X%08X) -> pid=%d olduruldu\n",
                  int_no,
                  (unsigned)(cr2 >> 32), (unsigned)cr2,
                  (unsigned)(rip >> 32), (unsigned)rip,
                  p->pid);
    proc_wake_parent(p);
    sched_exit();
}

void proc_init(void) {
    for (uint32_t c = 0; c < PROC_MAX_CHUNKS; c++) s_proc_chunks[c] = 0;
    s_proc_chunks[0] = (pcb_t *)kmalloc(sizeof(pcb_t) * PROC_CHUNK_SIZE);
    if (!s_proc_chunks[0]) {
        serial_puts("[proc] HATA: surec havuzu ayrilamadi\n");
        return;
    }
    s_proc_num_chunks = 1u;
    s_proc_cap = (int)PROC_CHUNK_SIZE;
    for (uint32_t i = 0; i < PROC_CHUNK_SIZE; i++) proc_init_slot(&s_proc_chunks[0][i]);

    pcb_t *p0 = &s_proc_chunks[0][0];
    p0->pid = 1;
    p0->ppid = 0;
    p0->state = PROC_RUNNING;
    name_copy(p0->name, sizeof(p0->name), "init");
    p0->space = vmm_current();
    p0->task = sched_current();
    sched_set_owner(sched_current(), p0);
    s_init_pid = 1;
    s_next_pid = 2;
    arctian_set_fault_handler(proc_fault_handler);
    serial_puts("[proc] dinamik surec yoneticisi aktif (PID tablosu hazir)\n");
}

int proc_spawn(const char *name, void (*entry)(void *), void *arg) {
    pcb_t *cur = proc_current();
    return proc_create(name, entry, arg, cur ? cur->pid : s_init_pid);
}

int proc_fork(const char *name, void (*entry)(void *), void *arg) {
    pcb_t *cur = proc_current();
    return proc_create(name, entry, arg, cur ? cur->pid : s_init_pid);
}

int proc_exec(void (*entry)(void *), void *arg) {
    pcb_t *p = proc_current();
    if (!p) return -1;
    p->entry = entry;
    p->arg = arg;
    if (entry) entry(arg);
    proc_exit(0);
}

void proc_exit(int code) {
    uint64_t fl = sched_irq_save();
    pcb_t *p = proc_current();
    if (p) {
        p->state = PROC_ZOMBIE;
        p->exit_code = code;
        int cap = __atomic_load_n(&s_proc_cap, __ATOMIC_ACQUIRE);
        for (int i = 0; i < cap; i++) {
            pcb_t *c = proc_slot(i);
            if (c && c->state != PROC_UNUSED && c->ppid == p->pid)
                c->ppid = s_init_pid;
        }
        proc_wake_parent(p);
    }
    (void)fl;
    sched_exit();
}

int proc_wait(int pid, int *code) {
    pcb_t *parent = proc_current();
    for (;;) {
        uint64_t fl = sched_irq_save();
        pcb_t *c = proc_find(pid);
        if (!c) { sched_irq_restore(fl); return -1; }
        if (c->state == PROC_ZOMBIE) {
            int rp = c->pid;
            if (code) *code = c->exit_code;
            proc_free(c);
            sched_irq_restore(fl);
            return rp;
        }
        if (parent) parent->waiting = 1;
        sched_block_current();
        if (parent) parent->waiting = 0;
        sched_irq_restore(fl);
    }
}

int proc_kill(int pid) {
    uint64_t fl = sched_irq_save();
    pcb_t *p = proc_find(pid);
    if (!p) { sched_irq_restore(fl); return -1; }
    if (p->state == PROC_ZOMBIE) { sched_irq_restore(fl); return 0; }
    p->state = PROC_ZOMBIE;
    p->exit_code = -1;
    proc_wake_parent(p);
    sched_irq_restore(fl);
    return 0;
}

int proc_getpid(void) {
    pcb_t *p = proc_current();
    return p ? p->pid : -1;
}

int proc_getppid(void) {
    pcb_t *p = proc_current();
    return p ? p->ppid : -1;
}

int proc_count(void) {
    int n = 0;
    int cap = __atomic_load_n(&s_proc_cap, __ATOMIC_ACQUIRE);
    for (int i = 0; i < cap; i++) {
        pcb_t *p = proc_slot(i);
        if (p && p->state != PROC_UNUSED) n++;
    }
    return n;
}

int proc_info(int i, int *pid, char *name, int cap, int *state, int *ppid, int *code) {
    pcb_t *p = proc_slot(i);
    if (!p || p->state == PROC_UNUSED) return -1;
    if (pid) *pid = p->pid;
    if (name && cap > 0) name_copy(name, cap, p->name);
    if (state) *state = (int)p->state;
    if (ppid) *ppid = p->ppid;
    if (code) *code = p->exit_code;
    return 0;
}

static inline uint64_t proc_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static volatile uint64_t s_dsk_active_tsc;
static volatile uint64_t s_dsk_total_tsc;
static volatile uint64_t s_dsk_frame_start_tsc;
static volatile uint64_t s_dsk_last_end_tsc;
static volatile uint8_t  s_dsk_in_frame;
static volatile uint8_t  s_dsk_reap_armed;
static volatile uint32_t s_dsk_last_sched_ticks;
static volatile uint32_t s_dsk_active_ticks;

void proc_desktop_frame_begin(void) {
    if (s_dsk_in_frame || sched_current() != 0) return;
    uint64_t now = proc_rdtsc();
    if (s_dsk_last_end_tsc != 0 && now > s_dsk_last_end_tsc) {
        s_dsk_total_tsc += (now - s_dsk_last_end_tsc);
    }
    s_dsk_frame_start_tsc = now;
    s_dsk_in_frame = 1;
}

void proc_desktop_mark_reap(void) {
    if (sched_current() == 0 && s_dsk_in_frame) {
        s_dsk_reap_armed = 1;
    }
}

void proc_desktop_frame_end_if_armed(void) {
    if (!s_dsk_reap_armed || !s_dsk_in_frame || sched_current() != 0) return;
    s_dsk_reap_armed = 0;
    uint64_t now = proc_rdtsc();
    uint64_t dt = (now > s_dsk_frame_start_tsc) ? (now - s_dsk_frame_start_tsc) : 0;
    s_dsk_active_tsc += dt;
    s_dsk_total_tsc += dt;
    s_dsk_last_end_tsc = now;
    s_dsk_in_frame = 0;
}

void proc_desktop_blit_tsc(uint64_t tsc_delta) {
    if (sched_current() != 0) return;
    s_dsk_active_tsc += tsc_delta;
    s_dsk_total_tsc += tsc_delta;
    s_dsk_last_end_tsc = proc_rdtsc();
}

static uint32_t proc_desktop_active_ticks(void) {
    uint32_t cur_raw = sched_task_ticks(0);
    uint32_t d_raw = cur_raw - s_dsk_last_sched_ticks;
    if (d_raw > 0) {
        uint64_t tot = s_dsk_total_tsc;
        uint64_t act = s_dsk_active_tsc;
        uint32_t add = d_raw;
        if (tot > 0) {
            uint64_t scaled = (act * (uint64_t)d_raw + (tot >> 1)) / tot;
            add = (scaled > d_raw) ? d_raw : (uint32_t)scaled;
            if (d_raw >= 10u) {
                s_dsk_active_tsc = 0;
                s_dsk_total_tsc = 0;
            }
        }
        s_dsk_active_ticks += add;
        s_dsk_last_sched_ticks = cur_raw;
    }
    return s_dsk_active_ticks;
}

void proc_add_io_bytes(uint64_t bytes) {
    if (!bytes) return;
    pcb_t *p = proc_current();
    if (p) __atomic_fetch_add(&p->io_bytes, bytes, __ATOMIC_RELAXED);
}

void proc_add_net_bytes(uint64_t bytes) {
    if (!bytes) return;
    pcb_t *p = proc_current();
    if (p) __atomic_fetch_add(&p->net_bytes, bytes, __ATOMIC_RELAXED);
}

static int name_is_idle(const char *nm) {
    return nm && nm[0] == 'i' && nm[1] == 'd' && nm[2] == 'l' && nm[3] == 'e';
}

int proc_info_ex(int i, asi_proc_info_t *out) {
    if (!out || i < 0) return -1;
    pcb_t *p = proc_slot(i);
    if (!p || p->state == PROC_UNUSED) {
        if (posix_oom_list_cb) {
            asi_oom_app_t apps[16];
            int pn = posix_oom_list_cb(apps, 16);
            if (pn > 0) {
                int unused_before = 0;
                for (int k = 0; k < i; k++) {
                    pcb_t *pk = proc_slot(k);
                    if (!pk || pk->state == PROC_UNUSED) unused_before++;
                }
                if (unused_before < pn) {
                    const asi_oom_app_t *pa = &apps[unused_before];
                    out->pid = pa->pid;
                    out->ppid = s_init_pid;
                    out->state = pa->frozen ? (int)PROC_BLOCKED : (int)PROC_RUNNING;
                    out->exit_code = 0;
                    out->caps = CAP_ALL;
                    out->uid = caps_uid();
                    out->gid = caps_gid();
                    out->cpu_ticks = 0u;
                    out->ram_kb = pa->ram_kb;
                    out->io_bytes = 0;
                    out->net_bytes = 0;
                    out->flags = ASI_PROC_FLAG_POSIX;
                    out->is_idle = 0;
                    name_copy(out->name, sizeof(out->name), pa->name);
                    return 0;
                }
            }
        }
        return -1;
    }
    int is_idle = p->is_idle || name_is_idle(p->name);
    out->pid = p->pid;
    out->ppid = p->ppid;
    out->state = (int)p->state;
    out->exit_code = p->exit_code;
    out->caps = p->caps;
    out->uid = p->uid;
    out->gid = p->gid;
    out->is_idle = is_idle ? 1u : 0u;
    out->flags = 0u;
    if (is_idle) out->flags |= ASI_PROC_FLAG_IDLE;
    if (p->is_app) out->flags |= ASI_PROC_FLAG_APP;

    if (is_idle) {
        out->cpu_ticks = 0u;
    } else if (p->pid == s_init_pid) {
        out->cpu_ticks = proc_desktop_active_ticks();
    } else {
        out->cpu_ticks = (p->task >= 0) ? sched_task_ticks(p->task) : 0u;
    }

    uint32_t kb = 0u;
    if (p->pid == s_init_pid) {
        kb = (uint32_t)(kmalloc_used() / 1024u) + 32u;
    } else {
        if (p->space) kb += vmm_space_ram_kb(p->space);
        if (p->task >= 0) kb += 32u;
        if (p->is_app && kb == 0u) kb = 256u;
        if (kb == 0u && p->state != PROC_ZOMBIE) kb = 32u;
    }
    out->ram_kb = kb;
    out->io_bytes = __atomic_load_n(&p->io_bytes, __ATOMIC_RELAXED);
    out->net_bytes = __atomic_load_n(&p->net_bytes, __ATOMIC_RELAXED);
    name_copy(out->name, sizeof(out->name), p->name);
    return 0;
}

uint32_t proc_sys_ticks(void) {
    return sched_total_ticks();
}

int proc_app_open_caps(const char *title, uint32_t caps) {
    uint64_t fl = sched_irq_save();
    spin_lock(&s_proc_lock);
    pcb_t *p = proc_alloc_slot_locked(&fl);
    if (!p) { spin_unlock(&s_proc_lock); sched_irq_restore(fl); return -1; }
    uint32_t cur_u = caps_uid(), cur_g = caps_gid();
    p->pid = s_next_pid++;
    p->ppid = s_init_pid;
    p->state = PROC_RUNNING;
    p->exit_code = 0;
    p->is_app = 1;
    p->is_idle = 0;
    p->waiting = 0;
    p->caps = caps;
    p->uid = (caps == CAP_ALL) ? cur_u : (cur_u ? cur_u : 1000u);
    p->gid = (caps == CAP_ALL) ? cur_g : (cur_g ? cur_g : 1000u);
    p->io_bytes = 0;
    p->net_bytes = 0;
    p->entry = 0;
    p->arg = 0;
    p->space = 0;
    p->task = -1;
    name_copy(p->name, sizeof(p->name), title ? title : "uygulama");
    spin_unlock(&s_proc_lock);
    sched_irq_restore(fl);
    serial_printf("[proc] uygulama sureci basladi: %s (pid=%d, yetki=0x%X)\n",
                  p->name, p->pid, (unsigned)caps);
    return p->pid;
}

int proc_app_open(const char *title) {
    return proc_app_open_caps(title, CAP_ALL);
}

uint32_t proc_caps_of(int pid) {
    pcb_t *p = proc_find(pid);
    return p ? p->caps : CAP_ALL;
}

/* Sahip isaretcisi gercekten s_procs icinde mi? Zamanlayici gorev
 * yapisindaki owner alani bir sekilde bozulursa (gozlenen: masaustune
 * gecerken bazi gorevlerde cop isaretci) burada p->caps okumak #GP
 * firtinasi baslatiyordu. Gecersiz sahip = yetki bilinmiyor; cagirana
 * "sahip yok" demek guvenli varsayilana (CAP_ALL) dusurur ve sistem
 * ayakta kalir. Bozulma bir kez seri loga yazilir. */
static int proc_slot_valid(const pcb_t *p) {
    if (!p) return 0;
    uint32_t nc = __atomic_load_n(&s_proc_num_chunks, __ATOMIC_ACQUIRE);
    for (uint32_t c = 0; c < nc; c++) {
        pcb_t *base = s_proc_chunks[c];
        if (!base) continue;
        if (p >= base && p < base + PROC_CHUNK_SIZE) {
            return (((uintptr_t)p - (uintptr_t)base) % sizeof(pcb_t)) == 0;
        }
    }
    return 0;
}

int proc_owner_caps(uint32_t *out) {
    pcb_t *p = (pcb_t *)sched_owner_current();
    if (!p) return 0;
    if (!proc_slot_valid(p)) {
        static int s_warned;
        if (!s_warned) {
            s_warned = 1;
            serial_printf("[proc] UYARI: gecersiz sahip isaretcisi=0x%x (tid=%d) -> yetki CAP_ALL\n",
                          (unsigned)(uintptr_t)p, (int)sched_current());
        }
        return 0;
    }
    if (out) *out = p->caps;
    return 1;
}

uint32_t proc_uid_of(int pid) {
    pcb_t *p = proc_find(pid);
    return p ? p->uid : 0u;
}

uint32_t proc_gid_of(int pid) {
    pcb_t *p = proc_find(pid);
    return p ? p->gid : 0u;
}

int proc_owner_ids(uint32_t *uid, uint32_t *gid) {
    pcb_t *p = (pcb_t *)sched_owner_current();
    if (!p) return 0;
    if (uid) *uid = p->uid;
    if (gid) *gid = p->gid;
    return 1;
}

void proc_app_close(int pid, int code) {
    if (pid <= 0) return;
    uint64_t fl = sched_irq_save();
    pcb_t *p = proc_find(pid);
    if (p && p->state != PROC_ZOMBIE) {
        p->state = PROC_ZOMBIE;
        p->exit_code = code;
        proc_wake_parent(p);
    }
    sched_irq_restore(fl);
}

int proc_reap(int *code_out) {
    proc_desktop_mark_reap();
    uint64_t fl = sched_irq_save();
    spin_lock(&s_proc_lock);
    int cap = s_proc_cap;
    for (int i = 0; i < cap; i++) {
        pcb_t *p = proc_slot(i);
        if (!p || p->state != PROC_ZOMBIE) continue;
        pcb_t *par = proc_find(p->ppid);
        if (par && par->waiting) continue;
        int pid = p->pid;
        int code = p->exit_code;
        serial_printf("[proc] zombie toplandi: %s (pid=%d, kod=%d)\n",
                      p->name, pid, code);
        proc_free(p);
        if (code_out) *code_out = code;
        spin_unlock(&s_proc_lock);
        sched_irq_restore(fl);
        return pid;
    }
    spin_unlock(&s_proc_lock);
    sched_irq_restore(fl);
    return -1;
}

static void proc_worker(void *arg) {
    int id = (int)(intptr_t)arg;
    serial_printf("[proc] isci-%d calisiyor (pid=%d, cr3=0x%X)\n",
                  id, proc_getpid(), (unsigned)(vmm_current() >> 12));
    for (volatile int i = 0; i < 300000; i++) __asm__ volatile("pause");
    serial_printf("[proc] isci-%d bitiyor (kod=%d)\n", id, id * 5);
    proc_exit(id * 5);
}

static void proc_fork_child(void *arg) {
    (void)arg;
    serial_printf("[proc] cocuk surec calisti (pid=%d, ppid=%d)\n",
                  proc_getpid(), proc_getppid());
    proc_exit(42);
}

static void proc_exec_target(void *arg) {
    (void)arg;
    serial_printf("[proc] exec hedefi calisti (pid=%d)\n", proc_getpid());
    proc_exit(9);
}

static void proc_exec_caller(void *arg) {
    (void)arg;
    serial_printf("[proc] exec cagriliyor (pid=%d)\n", proc_getpid());
    proc_exec(proc_exec_target, 0);
}

int proc_selftest(void) {
    serial_puts("[proc] ==== SUREC SELF-TEST ====\n");

    int p1 = proc_spawn("isci-A", proc_worker, (void *)(intptr_t)1);
    int p2 = proc_spawn("isci-B", proc_worker, (void *)(intptr_t)2);
    if (p1 < 0 || p2 < 0) {
        serial_puts("[proc] selftest: spawn basarisiz\n");
        return -1;
    }

    int c1 = -1, c2 = -1;
    int r1 = proc_wait(p1, &c1);
    int r2 = proc_wait(p2, &c2);
    serial_printf("[proc] wait: pid=%d kod=%d, pid=%d kod=%d\n", r1, c1, r2, c2);

    int pf = proc_fork("cocuk", proc_fork_child, 0);
    int cf = -1;
    int rf = proc_wait(pf, &cf);
    serial_printf("[proc] fork/wait: pid=%d kod=%d\n", rf, cf);

    int pe = proc_spawn("execi", proc_exec_caller, 0);
    int ce = -1;
    int re = proc_wait(pe, &ce);
    serial_printf("[proc] exec/wait: pid=%d kod=%d\n", re, ce);

    int reaped = proc_reap(0);
    serial_printf("[proc] ek zombie toplama: %d\n", reaped);

    int ok = (r1 == p1 && c1 == 5 && r2 == p2 && c2 == 10 &&
              rf == pf && cf == 42 && re == pe && ce == 9);
    serial_printf("[proc] selftest %s: spawn/fork/exec/exit/wait + zombie toplama\n",
                  ok ? "OK" : "HATA");
    return ok ? 0 : -1;
}

static void proc_fault_child(void *arg) {
    (void)arg;
    serial_printf("[proc] hatali cocuk calisiyor (pid=%d, cr3=0x%X)\n",
                  proc_getpid(), (unsigned)(vmm_current() >> 12));
    volatile uint64_t *bad = (volatile uint64_t *)(uintptr_t)VMM_USER_BASE;
    *bad = 0xDEADull;
    serial_puts("[proc] HATA: erisim page fault uretmedi\n");
    proc_exit(0);
}

int proc_fault_selftest(void) {
    int pid = proc_spawn("hatali", proc_fault_child, 0);
    if (pid < 0) {
        serial_puts("[proc] bellek koruma selftest: spawn basarisiz\n");
        return -1;
    }
    int code = 0;
    int r = proc_wait(pid, &code);
    int ok = (r == pid && code == -11);
    serial_printf("[proc] bellek koruma selftest: pid=%d kod=%d -> %s\n",
                  r, code, ok ? "OK (surec olduruldu, sistem ayakta)" : "HATA");
    return ok ? 0 : -1;
}

static void proc11_idle_worker(void *arg) {
    (void)arg;
    sched_sleep_ms(80);
    proc_exit(0);
}

int proc_v11_selftest(void) {
    asi_proc_info_t i0, i1;
    if (proc_info_ex(0, &i0) != 0) {
        serial_puts("[proc11] info_ex(0) FAIL\n");
        return -1;
    }

    int ipid = proc_spawn("idle-test", proc11_idle_worker, 0);
    int idle_slot = -1;
    if (ipid > 1) {
        int cap = __atomic_load_n(&s_proc_cap, __ATOMIC_ACQUIRE);
        for (int k = 0; k < cap; k++) {
            pcb_t *pk = proc_slot(k);
            if (pk && pk->pid == ipid) { idle_slot = k; break; }
        }
    }

    asi_proc_info_t i_idle;
    int idle_flag_ok = 0;
    if (idle_slot >= 0 && proc_info_ex(idle_slot, &i_idle) == 0) {
        idle_flag_ok = (i_idle.is_idle == 1u && (i_idle.flags & ASI_PROC_FLAG_IDLE) != 0 &&
                        i_idle.cpu_ticks == 0u && i_idle.ram_kb > 0u);
    }

    uint32_t t0 = proc_sys_ticks();
    sched_sleep_ms(50);
    uint32_t t1 = proc_sys_ticks();
    if (proc_info_ex(0, &i1) != 0) return -1;
    if (ipid > 1) proc_wait(ipid, 0);

    uint32_t dt = t1 - t0;
    uint32_t d_cpu = i1.cpu_ticks - i0.cpu_ticks;
    uint32_t idle_cpu_pct = dt ? (d_cpu * 100u / dt) : 0u;

    int ok = (i0.ram_kb > 0u && dt > 0u && idle_cpu_pct < 90u && idle_flag_ok);
    serial_printf("[proc11] pid=%d ram_kb=%u io=%llu net=%llu dt=%u d_cpu=%u idle_cpu_pct=%u%% idle_flag=%s -> %s\n",
                  i0.pid, (unsigned)i0.ram_kb,
                  (unsigned long long)i0.io_bytes, (unsigned long long)i0.net_bytes,
                  (unsigned)dt, (unsigned)d_cpu, (unsigned)idle_cpu_pct,
                  idle_flag_ok ? "OK" : "FAIL",
                  ok ? "OK" : "HATA");
    return ok ? 0 : -1;
}

/* ---- E1: OOM Basinc & Dondurma / Cozme / Sonlandirma ---- */

int proc_oom_state(void) {
    return vmm_oom_pressure();
}

void proc_oom_freeze_apps(void) {
    sched_freeze_user_apps();
    if (posix_oom_freeze_cb) posix_oom_freeze_cb(1);
}

void proc_oom_thaw_apps(void) {
    sched_thaw_user_apps();
    if (posix_oom_freeze_cb) posix_oom_freeze_cb(0);
}

int proc_oom_list_apps(asi_oom_app_t *out, int max_out) {
    if (!out || max_out <= 0) return 0;
    int n = 0;
    uint64_t fl = sched_irq_save();
    spin_lock(&s_proc_lock);
    int cap = s_proc_cap;
    for (int i = 1; i < cap && n < max_out; i++) {
        pcb_t *p = proc_slot(i);
        if (!p || p->state == PROC_UNUSED || p->state == PROC_ZOMBIE || p->pid <= 1)
            continue;
        asi_oom_app_t *e = &out[n++];
        e->pid = p->pid;
        uint32_t kb = p->space ? vmm_space_ram_kb(p->space) : 0u;
        if (p->task > 0) kb += 32u;
        if (kb == 0u) kb = 64u;
        e->ram_kb = kb;
        e->frozen = (p->task > 0 && sched_is_frozen(p->task)) ? 1u : 0u;
        e->kind = p->is_app ? ASI_OOM_KIND_WLF : ASI_OOM_KIND_NATIVE;
        e->reserved[0] = 0;
        e->reserved[1] = 0;
        name_copy(e->name, sizeof(e->name), p->name);
    }
    spin_unlock(&s_proc_lock);
    sched_irq_restore(fl);

    if (n < max_out && posix_oom_list_cb) {
        int pn = posix_oom_list_cb(out + n, max_out - n);
        if (pn > 0) n += pn;
    }
    return n;
}

int proc_oom_kill_app(int pid) {
    if (pid <= 1) return -1;

    uint64_t fl = sched_irq_save();
    spin_lock(&s_proc_lock);
    pcb_t *p = proc_find(pid);
    if (p) {
        sched_tid_t tid = p->task;
        p->task = -1;
        p->state = PROC_ZOMBIE;
        p->exit_code = -9;
        vmm_space_t sp = p->space;
        p->space = 0;
        pcb_t *par = proc_find(p->ppid);
        int par_waiting = (par && par->waiting) ? 1 : 0;
        spin_unlock(&s_proc_lock);
        sched_irq_restore(fl);

        if (tid > 0) {
            sched_terminate_task(tid);
            sched_reap(tid);
        }
        if (sp) {
            vmm_state_t *st = VMM_STATE;
            if (st && st->ready && sp != st->kernel_space && sp != vmm_current())
                vmm_space_destroy(sp);
        }

        fl = sched_irq_save();
        spin_lock(&s_proc_lock);
        if (par_waiting) {
            proc_wake_parent(p);
        } else {
            p->state = PROC_UNUSED;
            p->pid = 0;
            p->entry = 0;
            p->arg = 0;
        }
        spin_unlock(&s_proc_lock);
        sched_irq_restore(fl);
        return 0;
    }
    spin_unlock(&s_proc_lock);
    sched_irq_restore(fl);

    if (posix_oom_kill_cb) return posix_oom_kill_cb(pid);
    return -1;
}
