/* POSIX Surec, Cekirdek Thread, TLS (arch_prctl) ve Futex alt sistemi.
 *
 * Desteklenenler:
 *   - Izole 64-bit adres alanli surecler (posix_proc_t)
 *   - Gercek cekirdek thread'leri (clone CLONE_VM|CLONE_THREAD|CLONE_SETTLS)
 *   - fork / vfork (MAP_PRIVATE kopya, MAP_SHARED/SHM paylasim, FD refcount)
 *   - execve (AFS'den ELF64 yukleme, FD_CLOEXEC temizleme)
 *   - wait4 / waitpid (WNOHANG, -1 / belirli PID, zombi toplama)
 *   - exit / exit_group (CLONE_CHILD_CLEARTID + futex wake -> pthread_join)
 *   - arch_prctl (ARCH_SET_FS / ARCH_GET_FS)
 *   - futex (WAIT / WAKE / REQUEUE / CMP_REQUEUE, fiziksel adres tablosu,
 *            zaman asimli bekleme -> pthread_mutex / cond / sem)
 */
#include "posix.h"
#include "alloc.h"
#include "afs.h"
#include "clock.h"
#include "percpu.h"
#include "percpu_gdt.h"
#include "serial.h"
#include <string.h>

/* E2B: Dinamik kmalloc dilimli POSIX surec/thread/futex tablolari (sabit 32/256 siniri yok) */
#define PPROC_CHUNK_SHIFT 3u
#define PPROC_CHUNK_SIZE  (1u << PPROC_CHUNK_SHIFT) /* 8 surec / dilim */
#define PPROC_CHUNK_MASK  (PPROC_CHUNK_SIZE - 1u)
#define PPROC_MAX_CHUNKS  256u                      /* 2048 surece kadar dinamik */

#define PTHR_CHUNK_SHIFT  5u
#define PTHR_CHUNK_SIZE   (1u << PTHR_CHUNK_SHIFT)  /* 32 thread / dilim */
#define PTHR_CHUNK_MASK   (PTHR_CHUNK_SIZE - 1u)
#define PTHR_MAX_CHUNKS   256u                      /* 8192 thread'e kadar dinamik */

#define FWAIT_CHUNK_SHIFT 6u
#define FWAIT_CHUNK_SIZE  (1u << FWAIT_CHUNK_SHIFT) /* 64 bekleyen / dilim */
#define FWAIT_CHUNK_MASK  (FWAIT_CHUNK_SIZE - 1u)
#define FWAIT_MAX_CHUNKS  32u                       /* 2048 bekleyene kadar dinamik */

static posix_proc_t     *s_pproc_chunks[PPROC_MAX_CHUNKS];
static volatile uint32_t s_pproc_num_chunks = 0;
static volatile int      s_pproc_cap = 0;

static posix_thr_t      *s_pthr_chunks[PTHR_MAX_CHUNKS];
static volatile uint32_t s_pthr_num_chunks = 0;
static volatile int      s_pthr_cap = 0;

static spinlock_t        s_ptab_lock;
static int               s_next_tid = 1000;

/* ---- Futex bekleme kuyrugu (fiziksel adres tabanli -> MAP_SHARED uyumlu) ---- */
typedef struct {
    volatile int used;
    uint64_t     phys;
    sched_tid_t  sched_tid;
    volatile int woken;
} futex_waiter_t;

static futex_waiter_t   *s_fwait_chunks[FWAIT_MAX_CHUNKS];
static volatile uint32_t s_fwait_num_chunks = 0;
static volatile int      s_fwait_cap = 0;
static spinlock_t        s_futex_lock;

static inline posix_proc_t *pproc_slot(int i) {
    int cap = __atomic_load_n(&s_pproc_cap, __ATOMIC_ACQUIRE);
    if (i < 0 || i >= cap) return 0;
    posix_proc_t *ch = s_pproc_chunks[(uint32_t)i >> PPROC_CHUNK_SHIFT];
    return ch ? &ch[(uint32_t)i & PPROC_CHUNK_MASK] : 0;
}

static inline posix_thr_t *pthr_slot(int i) {
    int cap = __atomic_load_n(&s_pthr_cap, __ATOMIC_ACQUIRE);
    if (i < 0 || i >= cap) return 0;
    posix_thr_t *ch = s_pthr_chunks[(uint32_t)i >> PTHR_CHUNK_SHIFT];
    return ch ? &ch[(uint32_t)i & PTHR_CHUNK_MASK] : 0;
}

static inline futex_waiter_t *fwait_slot(int i) {
    int cap = __atomic_load_n(&s_fwait_cap, __ATOMIC_ACQUIRE);
    if (i < 0 || i >= cap) return 0;
    futex_waiter_t *ch = s_fwait_chunks[(uint32_t)i >> FWAIT_CHUNK_SHIFT];
    return ch ? &ch[(uint32_t)i & FWAIT_CHUNK_MASK] : 0;
}

posix_thr_t *posix_cur_thr(void) {
    return (posix_thr_t *)sched_posix_thr_current();
}

posix_proc_t *posix_cur_proc(void) {
    posix_thr_t *t = posix_cur_thr();
    return t ? t->proc : 0;
}

static posix_proc_t *pproc_alloc(int ppid, const char *name) {
    uint64_t f = sched_irq_save();
    spin_lock(&s_ptab_lock);
    posix_proc_t *p = 0;

    for (;;) {
        int cap = s_pproc_cap;
        for (int i = 0; i < cap; i++) {
            posix_proc_t *cand = pproc_slot(i);
            if (cand && !cand->used) {
                p = cand;
                break;
            }
        }
        if (p) break;

        uint32_t nc = s_pproc_num_chunks;
        if (nc >= PPROC_MAX_CHUNKS) break;

        spin_unlock(&s_ptab_lock);
        sched_irq_restore(f);
        posix_proc_t *new_ch = (posix_proc_t *)kmalloc(sizeof(posix_proc_t) * PPROC_CHUNK_SIZE);
        f = sched_irq_save();
        if (!new_ch) {
            spin_lock(&s_ptab_lock);
            break;
        }
        memset(new_ch, 0, sizeof(posix_proc_t) * PPROC_CHUNK_SIZE);

        spin_lock(&s_ptab_lock);
        if (s_pproc_num_chunks == nc && s_pproc_num_chunks < PPROC_MAX_CHUNKS) {
            s_pproc_chunks[s_pproc_num_chunks++] = new_ch;
            __atomic_store_n(&s_pproc_cap, (int)(s_pproc_num_chunks * PPROC_CHUNK_SIZE), __ATOMIC_RELEASE);
            p = &new_ch[0];
            break;
        }
        spin_unlock(&s_ptab_lock);
        sched_irq_restore(f);
        kfree(new_ch);
        f = sched_irq_save();
        spin_lock(&s_ptab_lock);
    }

    if (p) {
        memset(p, 0, sizeof(*p));
        p->used = 1;
        p->pid = s_next_tid++;
        p->ppid = ppid;
        p->state = 1;
        p->nthreads = 0;
        spin_init(&p->lock);
        waitq_init(&p->wait_wq);
        strcpy(p->cwd, "/");
        if (name) {
            size_t n = strlen(name);
            if (n >= sizeof(p->name)) n = sizeof(p->name) - 1;
            memcpy(p->name, name, n);
            p->name[n] = 0;
        }
    }
    spin_unlock(&s_ptab_lock);
    sched_irq_restore(f);
    return p;
}

static posix_thr_t *pthr_alloc(posix_proc_t *p) {
    uint64_t f = sched_irq_save();
    spin_lock(&s_ptab_lock);
    posix_thr_t *t = 0;

    for (;;) {
        int cap = s_pthr_cap;
        for (int i = 0; i < cap; i++) {
            posix_thr_t *cand = pthr_slot(i);
            if (cand && !cand->used) {
                t = cand;
                break;
            }
        }
        if (t) break;

        uint32_t nc = s_pthr_num_chunks;
        if (nc >= PTHR_MAX_CHUNKS) break;

        spin_unlock(&s_ptab_lock);
        sched_irq_restore(f);
        posix_thr_t *new_ch = (posix_thr_t *)kmalloc(sizeof(posix_thr_t) * PTHR_CHUNK_SIZE);
        f = sched_irq_save();
        if (!new_ch) {
            spin_lock(&s_ptab_lock);
            break;
        }
        memset(new_ch, 0, sizeof(posix_thr_t) * PTHR_CHUNK_SIZE);

        spin_lock(&s_ptab_lock);
        if (s_pthr_num_chunks == nc && s_pthr_num_chunks < PTHR_MAX_CHUNKS) {
            s_pthr_chunks[s_pthr_num_chunks++] = new_ch;
            __atomic_store_n(&s_pthr_cap, (int)(s_pthr_num_chunks * PTHR_CHUNK_SIZE), __ATOMIC_RELEASE);
            t = &new_ch[0];
            break;
        }
        spin_unlock(&s_ptab_lock);
        sched_irq_restore(f);
        kfree(new_ch);
        f = sched_irq_save();
        spin_lock(&s_ptab_lock);
    }

    if (t) {
        memset(t, 0, sizeof(*t));
        t->used = 1;
        t->tid = (p->nthreads == 0) ? p->pid : s_next_tid++;
        t->proc = p;
        p->nthreads++;
    }
    spin_unlock(&s_ptab_lock);
    sched_irq_restore(f);
    return t;
}

static posix_proc_t *pproc_find_locked(int pid) {
    int cap = s_pproc_cap;
    for (int i = 0; i < cap; i++) {
        posix_proc_t *p = pproc_slot(i);
        if (p && p->used && p->pid == pid) return p;
    }
    return 0;
}

/* Yeni ring-3 gorevinin kernel giris trampolini: TSS/percpu/FS_BASE ayarla
 * ve posix_enter_user(&t->init_regs) ile CPL3'e gir. */
static void posix_thr_trampoline(void *arg) {
    posix_thr_t *t = (posix_thr_t *)arg;
    sched_set_posix_thr(sched_current(), t);
    sched_set_fs_base(t->fs_base);

    uint32_t ci = percpu_self_index();
    if (ci >= CPU_MAX_APICS) ci = 0;
    uint64_t ktop = sched_kstack_top_current();
    if (ktop) {
        gdt_set_rsp0(ci, ktop);
        percpu_t *pc = percpu_by_index(ci);
        if (pc) pc->syscall_kstack = ktop;
    }

    posix_regs_t regs = t->init_regs;
    posix_enter_user(&regs);
}

static void set_proc_exe_path(posix_proc_t *p, const char *path) {
    if (!p) return;
    if (!path || !path[0]) {
        strcpy(p->exe_path, "/elf");
        return;
    }
    const char *s = path;
    if ((s[0] == 'C' || s[0] == 'c') && s[1] == ':' && s[2] == '/')
        s += 2; /* keep leading '/' */
    size_t pos = 0;
    if (s[0] != '/') p->exe_path[pos++] = '/';
    for (size_t i = 0; s[i] && pos + 1 < sizeof(p->exe_path); i++)
        p->exe_path[pos++] = s[i];
    p->exe_path[pos] = 0;
}

static int spawn_elf_common(const char *name,
                            const uint8_t *elf_mem, void *afs_handle, uint64_t size,
                            char *const argv[], int argc) {
    if ((!elf_mem && !afs_handle) || size < 64) return -ENOEXEC;
    posix_proc_t *p = pproc_alloc(1, name ? name : "elf");
    if (!p) return -ENOMEM;
    set_proc_exe_path(p, name ? name : "/elf");

    p->space = vmm_space_create();
    if (!p->space) { p->used = 0; return -ENOMEM; }
    if (posix_vm_init_proc(p) < 0) {
        vmm_space_destroy(p->space);
        p->space = 0;
        p->used = 0;
        return -ENOMEM;
    }
    posix_fd_init_stdio(p);

    char *def_argv[2] = { (char *)(name ? name : "elf"), 0 };
    char *def_envp[3] = { "PATH=/bin:/", "HOME=/", "TERM=arctian" };
    posix_elf_info_t info;
    int rc;
    if (afs_handle) {
        rc = posix_elf_load_afs(p, afs_handle, size,
                                (argv && argc > 0) ? argv : def_argv,
                                (argv && argc > 0) ? argc : 1,
                                def_envp, 3, &info);
    } else {
        rc = posix_elf_load(p, elf_mem, size,
                            (argv && argc > 0) ? argv : def_argv,
                            (argv && argc > 0) ? argc : 1,
                            def_envp, 3, &info);
    }
    if (rc < 0) {
        posix_fd_close_all(p, 0);
        posix_vm_destroy_proc(p);
        p->used = 0;
        return rc;
    }

    posix_thr_t *t = pthr_alloc(p);
    if (!t) {
        posix_fd_close_all(p, 0);
        posix_vm_destroy_proc(p);
        p->used = 0;
        return -ENOMEM;
    }

    memset(&t->init_regs, 0, sizeof(t->init_regs));
    t->init_regs.rip = info.entry;
    t->init_regs.rsp = info.user_rsp;
    t->init_regs.cs  = 0x23;
    t->init_regs.ss  = 0x1B;
    t->init_regs.rflags = 0x202;  /* IF=1 */

    sched_tid_t stid = sched_spawn_ex(p->name, posix_thr_trampoline, t, p->space);
    if (stid < 0) {
        t->used = 0;
        posix_fd_close_all(p, 0);
        posix_vm_destroy_proc(p);
        p->used = 0;
        return -ENOMEM;
    }
    t->sched_tid = stid;
    return p->pid;
}

int posix_spawn_elf_mem(const char *name, const uint8_t *elf, uint64_t size,
                        char *const argv[], int argc) {
    return spawn_elf_common(name, elf, 0, size, argv, argc);
}

/* AFS yolundan ELF64 tutamagi bul (boyut siniri yok; akisli yuklenir). */
static void *find_afs_elf(const char *path, uint64_t *sz_out) {
    if (!path || !sz_out) return 0;
    char clean[192];
    const char *p = path;
    if (p[0] == '/') p++;
    size_t n = strlen(p);
    if (n >= sizeof(clean)) return 0;
    memcpy(clean, p, n + 1);

    void *h = afs_find_handle(path);
    if (!h) h = afs_find_handle(clean);
    if (!h) {
        char cpath[200];
        cpath[0] = 'C'; cpath[1] = ':'; cpath[2] = '/';
        size_t m = strlen(clean);
        if (m + 4 > sizeof(cpath)) return 0;
        memcpy(cpath + 3, clean, m + 1);
        h = afs_find_handle(cpath);
    }
    if (!h) return 0;

    asi_stat_t ast;
    memset(&ast, 0, sizeof(ast));
    if (afs_stat_handle(h, &ast) != 0) return 0;
    if (ast.is_dir || ast.size < 64u) return 0;
    *sz_out = (uint64_t)ast.size;
    return h;
}

int posix_spawn_elf_path(const char *path, char *const argv[], int argc) {
    if (!path) return -EINVAL;
    uint64_t sz = 0;
    void *h = find_afs_elf(path, &sz);
    if (!h) return -ENOENT;
    return spawn_elf_common(path, 0, h, sz, argv, argc);
}

/* Kernel icinden bir POSIX surecinin bitmesini bekle (selftest / elfrun icin). */
int posix_wait_pid_kernel(int pid, int *exit_code_out) {
    for (;;) {
        uint64_t f = sched_irq_save();
        spin_lock(&s_ptab_lock);
        posix_proc_t *c = pproc_find_locked(pid);
        if (!c) {
            spin_unlock(&s_ptab_lock);
            sched_irq_restore(f);
            return -ECHILD;
        }
        if (c->state == 2) {
            int code = c->exit_code;
            c->used = 0;
            c->state = 0;
            spin_unlock(&s_ptab_lock);
            sched_irq_restore(f);
            if (exit_code_out) *exit_code_out = code;
            return pid;
        }
        waitq_add(&c->wait_wq, sched_current());
        sched_prepare_sleep_ms(20);
        spin_unlock(&s_ptab_lock);
        sched_commit_block();
        sched_irq_restore(f);
    }
}

/* ---- fork() / clone(vfork/posix_spawn) ---- */
int64_t posix_sys_fork_ex(posix_regs_t *regs, uint64_t child_stack, uint64_t flags,
                          uint64_t ptid, uint64_t ctid) {
    posix_thr_t *cur_t = posix_cur_thr();
    if (!cur_t || !cur_t->proc) return -ESRCH;
    posix_proc_t *par = cur_t->proc;

    posix_proc_t *ch = pproc_alloc(par->pid, par->name);
    if (!ch) { serial_puts("[fork] fail: pproc_alloc\n"); return -ENOMEM; }
    ch->space = vmm_space_create();
    if (!ch->space) { serial_puts("[fork] fail: vmm_space_create\n"); ch->used = 0; return -ENOMEM; }
    if (posix_vm_init_proc(ch) < 0) {
        serial_puts("[fork] fail: vm_init_proc\n");
        vmm_space_destroy(ch->space);
        ch->space = 0;
        ch->used = 0;
        return -ENOMEM;
    }
    strcpy(ch->cwd, par->cwd);
    strcpy(ch->exe_path, par->exe_path);

    uint64_t fl = sched_irq_save();
    spin_lock(&par->lock);
    int vrc = posix_vm_clone(ch, par);
    spin_unlock(&par->lock);
    sched_irq_restore(fl);
    if (vrc < 0) {
        serial_printf("[fork] fail: vm_clone rc=%d\n", vrc);
        posix_vm_destroy_proc(ch);
        ch->used = 0;
        return vrc;
    }

    posix_fd_clone_table(ch, par);

    posix_thr_t *ct = pthr_alloc(ch);
    if (!ct) {
        serial_puts("[fork] fail: pthr_alloc\n");
        posix_fd_close_all(ch, 0);
        posix_vm_destroy_proc(ch);
        ch->used = 0;
        return -ENOMEM;
    }

    ct->fs_base = cur_t->fs_base;
    ct->clear_child_tid = (flags & CLONE_CHILD_CLEARTID) ? ctid : 0;
    ct->init_regs = *regs;
    ct->init_regs.rax = 0;  /* fork()/clone() cocukta 0 doner */
    if (child_stack) ct->init_regs.rsp = child_stack;
    ct->init_regs.cs = 0x23;
    ct->init_regs.ss = 0x1B;
    ct->init_regs.rflags |= 0x200;

    int32_t cpid32 = (int32_t)ch->pid;
    if ((flags & CLONE_PARENT_SETTID) && ptid)
        posix_ucopy_out(ptid, &cpid32, 4);
    if ((flags & CLONE_CHILD_SETTID) && ctid) {
        uint64_t phys = vmm_translate(ch->space, ctid);
        if (phys) memcpy((void *)(uintptr_t)phys, &cpid32, 4);
    }

    sched_tid_t stid = sched_spawn_ex(ch->name, posix_thr_trampoline, ct, ch->space);
    if (stid < 0) {
        serial_printf("[fork] fail: sched_spawn_ex rc=%d\n", stid);
        ct->used = 0;
        posix_fd_close_all(ch, 0);
        posix_vm_destroy_proc(ch);
        ch->used = 0;
        return -ENOMEM;
    }
    ct->sched_tid = stid;
    return ch->pid;
}

int64_t posix_sys_fork(posix_regs_t *regs) {
    return posix_sys_fork_ex(regs, 0, 0, 0, 0);
}

/* ---- clone() (gercek cekirdek thread'i veya yeni surec / posix_spawn) ---- */
int64_t posix_sys_clone(posix_regs_t *regs, uint64_t flags, uint64_t child_stack,
                        uint64_t ptid, uint64_t ctid, uint64_t newtls) {
    /* CLONE_THREAD yoksa (orn. fork, vfork, musl posix_spawn CLONE_VM|CLONE_VFORK)
     * izole yeni surec olustur; aksi halde ebeveynin execve/FD tablosu bozulur! */
    if (!(flags & CLONE_THREAD)) {
        return posix_sys_fork_ex(regs, child_stack, flags, ptid, ctid);
    }

    /* Gercek cekirdek thread'i: ayni posix_proc_t + ayni vmm_space_t + ayni FD tablosu */
    posix_thr_t *cur_t = posix_cur_thr();
    if (!cur_t || !cur_t->proc) return -ESRCH;
    posix_proc_t *p = cur_t->proc;

    posix_thr_t *nt = pthr_alloc(p);
    if (!nt) return -ENOMEM;

    nt->fs_base = (flags & CLONE_SETTLS) ? newtls : cur_t->fs_base;
    nt->clear_child_tid = (flags & CLONE_CHILD_CLEARTID) ? ctid : 0;
    nt->init_regs = *regs;
    nt->init_regs.rax = 0;
    if (child_stack) nt->init_regs.rsp = child_stack;
    nt->init_regs.cs = 0x23;
    nt->init_regs.ss = 0x1B;
    nt->init_regs.rflags |= 0x200;

    int32_t tid32 = (int32_t)nt->tid;
    if ((flags & CLONE_PARENT_SETTID) && ptid)
        posix_ucopy_out(ptid, &tid32, 4);
    if ((flags & CLONE_CHILD_SETTID) && ctid)
        posix_ucopy_out(ctid, &tid32, 4);

    sched_tid_t stid = sched_spawn_ex(p->name, posix_thr_trampoline, nt, p->space);
    if (stid < 0) {
        uint64_t f = sched_irq_save();
        spin_lock(&s_ptab_lock);
        nt->used = 0;
        p->nthreads--;
        spin_unlock(&s_ptab_lock);
        sched_irq_restore(f);
        return -ENOMEM;
    }
    nt->sched_tid = stid;
    return nt->tid;
}

/* ---- execve() ---- */
#define EXEC_MAX_ARGS 64
#define EXEC_MAX_ENVS 64
#define EXEC_STR_MAX  512

int64_t posix_sys_execve(posix_regs_t *regs, uint64_t path_va,
                         uint64_t argv_va, uint64_t envp_va) {
    posix_thr_t *cur_t = posix_cur_thr();
    if (!cur_t || !cur_t->proc) return -ESRCH;
    posix_proc_t *p = cur_t->proc;

    char path[192];
    if (posix_ucstr_in(path, sizeof(path), path_va) < 0) {
        serial_printf("[execve] pid=%d EFAULT\n", p->pid);
        return -EFAULT;
    }

    uint64_t elf_sz = 0;
    void *afs_h = find_afs_elf(path, &elf_sz);
    if (!afs_h) {
        serial_printf("[execve] pid=%d ENOENT path='%s'\n", p->pid, path);
        return -ENOENT;
    }

    /* argv ve envp'yi eski adres alanindan heap tamponlarina kopyala
     * (thread-safe, cekirdek yiginini sisirmez, 64x512B destekler). */
    char *k_args = (char *)kmalloc((size_t)(EXEC_MAX_ARGS * EXEC_STR_MAX));
    char *k_envs = (char *)kmalloc((size_t)(EXEC_MAX_ENVS * EXEC_STR_MAX));
    if (!k_args || !k_envs) {
        if (k_args) kfree(k_args);
        if (k_envs) kfree(k_envs);
        return -ENOMEM;
    }
    char *k_argv[EXEC_MAX_ARGS];
    char *k_envp[EXEC_MAX_ENVS];
    int argc = 0, envc = 0;

    if (argv_va) {
        for (int i = 0; i < EXEC_MAX_ARGS; i++) {
            uint64_t ptr = 0;
            if (posix_ucopy_in(&ptr, argv_va + (uint64_t)i * 8, 8) < 0) break;
            if (!ptr) break;
            char *dst = k_args + (size_t)argc * EXEC_STR_MAX;
            if (posix_ucstr_in(dst, EXEC_STR_MAX, ptr) == 0) {
                k_argv[argc++] = dst;
            }
        }
    }
    if (argc == 0) {
        char *dst = k_args;
        strncpy(dst, path, EXEC_STR_MAX - 1);
        dst[EXEC_STR_MAX - 1] = 0;
        k_argv[0] = dst;
        argc = 1;
    }

    if (envp_va) {
        for (int i = 0; i < EXEC_MAX_ENVS; i++) {
            uint64_t ptr = 0;
            if (posix_ucopy_in(&ptr, envp_va + (uint64_t)i * 8, 8) < 0) break;
            if (!ptr) break;
            char *dst = k_envs + (size_t)envc * EXEC_STR_MAX;
            if (posix_ucstr_in(dst, EXEC_STR_MAX, ptr) == 0) {
                k_envp[envc++] = dst;
            }
        }
    }

    /* Yeni adres alani + yeni VMA dizisi kur; basarili olursa eskisini yik. */
    vmm_space_t old_sp = p->space;
    posix_vma_t *old_vmas = p->vmas;
    int old_vma_cap = p->vma_cap;
    uint64_t old_brk_base = p->brk_base;
    uint64_t old_brk_cur = p->brk_cur;
    uint64_t old_mmap_next = p->mmap_next;

    vmm_space_t new_sp = vmm_space_create();
    if (!new_sp) {
        kfree(k_args);
        kfree(k_envs);
        return -ENOMEM;
    }

    p->space = new_sp;
    p->vmas = 0;
    p->vma_cap = 0;
    int rc = posix_vm_init_proc(p);
    posix_elf_info_t info;
    if (rc == 0)
        rc = posix_elf_load_afs(p, afs_h, elf_sz, k_argv, argc, k_envp, envc, &info);

    kfree(k_args);
    kfree(k_envs);

    if (rc < 0) {
        posix_vm_destroy_proc(p);
        p->space = old_sp;
        p->vmas = old_vmas;
        p->vma_cap = old_vma_cap;
        p->brk_base = old_brk_base;
        p->brk_cur = old_brk_cur;
        p->mmap_next = old_mmap_next;
        return rc;
    }

    /* Yeni adres alanina gec, eski VMA/SHM ve PML4'u temizle. */
    sched_set_current_cr3(new_sp);
    if (old_vmas) {
        for (int i = 0; i < old_vma_cap; i++) {
            if (old_vmas[i].used && old_vmas[i].shm_id >= 0)
                posix_shm_unref(old_vmas[i].shm_id);
        }
        kfree(old_vmas);
    }
    if (old_sp) vmm_space_destroy(old_sp);

    posix_fd_close_all(p, 1);  /* yalniz FD_CLOEXEC kapat */
    set_proc_exe_path(p, path);
    cur_t->fs_base = 0;
    cur_t->clear_child_tid = 0;
    sched_set_fs_base(0);

    /* Kullanici yazmaclarini yeni ELF girisine ayarla */
    memset(regs, 0, sizeof(*regs));
    regs->rip = info.entry;
    regs->rsp = info.user_rsp;
    regs->cs  = 0x23;
    regs->ss  = 0x1B;
    regs->rflags = 0x202;
    return 0;
}

/* ---- wait4() ---- */
int64_t posix_sys_wait4(int pid, uint64_t status_va, int options) {
    posix_proc_t *par = posix_cur_proc();
    if (!par) return -ESRCH;

    for (;;) {
        uint64_t f = sched_irq_save();
        spin_lock(&s_ptab_lock);
        int has_child = 0;
        posix_proc_t *found = 0;
        int pcap = s_pproc_cap;
        for (int i = 0; i < pcap; i++) {
            posix_proc_t *c = pproc_slot(i);
            if (!c || !c->used || c->ppid != par->pid) continue;
            if (pid > 0 && c->pid != pid) continue;
            has_child = 1;
            if (c->state == 2) { found = c; break; }
        }
        if (!has_child) {
            spin_unlock(&s_ptab_lock);
            sched_irq_restore(f);
            return -ECHILD;
        }
        if (found) {
            int cpid = found->pid;
            int wstatus = (found->exit_code & 0xFF) << 8;
            found->used = 0;
            found->state = 0;
            spin_unlock(&s_ptab_lock);
            sched_irq_restore(f);
            if (status_va) posix_ucopy_out(status_va, &wstatus, 4);
            return cpid;
        }
        if (options & WNOHANG) {
            spin_unlock(&s_ptab_lock);
            sched_irq_restore(f);
            return 0;
        }
        waitq_add(&par->wait_wq, sched_current());
        sched_prepare_sleep_ms(10);
        spin_unlock(&s_ptab_lock);
        sched_commit_block();
        sched_irq_restore(f);
    }
}

/* ---- exit() / exit_group() ---- */
void posix_sys_exit(int code, int group) {
    posix_thr_t *t = posix_cur_thr();
    if (!t || !t->proc) sched_exit();
    posix_proc_t *p = t->proc;

    /* CLONE_CHILD_CLEARTID: musl pthread_join bunun uzerinden uyanir! */
    if (t->clear_child_tid) {
        uint32_t zero = 0;
        posix_ucopy_out(t->clear_child_tid, &zero, 4);
        uint64_t phys = vmm_translate(p->space, t->clear_child_tid);
        if (phys) posix_futex_wake_phys(phys, 1);
    }

    int last = 0;
    int wake_list[WAITQ_MAX * 2];
    int wn = 0;

    uint64_t f = sched_irq_save();
    spin_lock(&s_ptab_lock);
    t->used = 0;
    if (p->nthreads > 0) p->nthreads--;
    if (p->state == 1 && (group || p->nthreads <= 0)) {
        p->nthreads = 0;
        p->exit_code = code;
        p->state = 2; /* zombie */
        last = 1;
        int w;
        while ((w = waitq_pop(&p->wait_wq)) >= 0 && wn < WAITQ_MAX)
            wake_list[wn++] = w;
        posix_proc_t *par = pproc_find_locked(p->ppid);
        if (par) {
            while ((w = waitq_pop(&par->wait_wq)) >= 0 && wn < WAITQ_MAX * 2)
                wake_list[wn++] = w;
        }
    }
    spin_unlock(&s_ptab_lock);
    sched_irq_restore(f);

    if (last) {
        /* SMP guvenligi: adres alanini (PML4) yikmadan once tum kardes
         * thread'lerin diger CPU'lardan inmesini bekle (sinirsiz thread). */
        for (;;) {
            sched_tid_t sib_batch[64];
            int sib_n = 0;
            uint64_t sf = sched_irq_save();
            spin_lock(&s_ptab_lock);
            int tcap = s_pthr_cap;
            for (int i = 0; i < tcap && sib_n < 64; i++) {
                posix_thr_t *st = pthr_slot(i);
                if (st && st->used && st->proc == p) {
                    st->used = 0;
                    if (st->sched_tid > 0 && st->sched_tid != sched_current())
                        sib_batch[sib_n++] = st->sched_tid;
                }
            }
            spin_unlock(&s_ptab_lock);
            sched_irq_restore(sf);
            if (sib_n == 0) break;
            for (int i = 0; i < sib_n; i++) {
                sched_terminate_task(sib_batch[i]);
                sched_reap(sib_batch[i]);
            }
        }
        posix_fd_close_all(p, 0);
        /* Kendi CR3'umuzden cekirdek CR3'une gec ki vmm_space_destroy
         * aktif CR3'u yikmaya calismasin. */
        vmm_state_t *vst = VMM_STATE;
        if (vst && vst->kernel_space) sched_set_current_cr3(vst->kernel_space);
        posix_vm_destroy_proc(p);
    }

    for (int i = 0; i < wn; i++) sched_wake(wake_list[i]);
    sched_set_posix_thr(sched_current(), 0);
    sched_set_fs_base(0);
    sched_exit();
    for (;;) __asm__ volatile("hlt");
}

/* ---- arch_prctl() ---- */
int64_t posix_sys_arch_prctl(int code, uint64_t addr) {
    posix_thr_t *t = posix_cur_thr();
    if (!t) return -ESRCH;
    if (code == ARCH_SET_FS) {
        t->fs_base = addr;
        sched_set_fs_base(addr);
        return 0;
    }
    if (code == ARCH_GET_FS) {
        uint64_t cur = t->fs_base;
        if (posix_ucopy_out(addr, &cur, 8) < 0) return -EFAULT;
        return 0;
    }
    return -EINVAL;
}

/* ---- futex() ---- */
static void fwait_clear_tid(sched_tid_t tid) {
    if (tid <= 0) return;
    uint64_t f = sched_irq_save();
    spin_lock(&s_futex_lock);
    int cap = s_fwait_cap;
    for (int i = 0; i < cap; i++) {
        futex_waiter_t *fw = fwait_slot(i);
        if (fw && fw->used && fw->sched_tid == tid) {
            fw->used = 0;
            fw->woken = 1;
        }
    }
    spin_unlock(&s_futex_lock);
    sched_irq_restore(f);
}

static futex_waiter_t *fwait_alloc_locked(uint64_t *fl_inout) {
    for (;;) {
        int cap = s_fwait_cap;
        for (int i = 0; i < cap; i++) {
            futex_waiter_t *fw = fwait_slot(i);
            if (fw && !fw->used) return fw;
        }
        uint32_t nc = s_fwait_num_chunks;
        if (nc >= FWAIT_MAX_CHUNKS) return 0;

        spin_unlock(&s_futex_lock);
        sched_irq_restore(*fl_inout);
        futex_waiter_t *new_ch = (futex_waiter_t *)kmalloc(sizeof(futex_waiter_t) * FWAIT_CHUNK_SIZE);
        *fl_inout = sched_irq_save();
        if (!new_ch) {
            spin_lock(&s_futex_lock);
            return 0;
        }
        memset(new_ch, 0, sizeof(futex_waiter_t) * FWAIT_CHUNK_SIZE);

        spin_lock(&s_futex_lock);
        if (s_fwait_num_chunks == nc && s_fwait_num_chunks < FWAIT_MAX_CHUNKS) {
            s_fwait_chunks[s_fwait_num_chunks++] = new_ch;
            __atomic_store_n(&s_fwait_cap, (int)(s_fwait_num_chunks * FWAIT_CHUNK_SIZE), __ATOMIC_RELEASE);
            return &new_ch[0];
        }
        spin_unlock(&s_futex_lock);
        sched_irq_restore(*fl_inout);
        kfree(new_ch);
        *fl_inout = sched_irq_save();
        spin_lock(&s_futex_lock);
    }
}

void posix_futex_wake_phys(uint64_t phys, int count) {
    if (!phys || count <= 0) return;
    int woken_total = 0;
    while (woken_total < count) {
        sched_tid_t wake_ids[64];
        int wn = 0;
        uint64_t f = sched_irq_save();
        spin_lock(&s_futex_lock);
        int cap = s_fwait_cap;
        for (int i = 0; i < cap && wn < 64 && (woken_total + wn) < count; i++) {
            futex_waiter_t *fw = fwait_slot(i);
            if (fw && fw->used && fw->phys == phys && !fw->woken) {
                fw->woken = 1;
                wake_ids[wn++] = fw->sched_tid;
            }
        }
        spin_unlock(&s_futex_lock);
        sched_irq_restore(f);
        if (wn == 0) break;
        for (int i = 0; i < wn; i++) sched_wake(wake_ids[i]);
        woken_total += wn;
    }
}

int64_t posix_sys_futex(uint64_t uaddr, int op, uint32_t val,
                        uint64_t timeout_va, uint64_t uaddr2, uint32_t val3) {
    posix_proc_t *p = posix_cur_proc();
    if (!p || (uaddr & 3ull)) return -EINVAL;
    int cmd = op & FUTEX_CMD_MASK;

    uint64_t phys = vmm_translate(p->space, uaddr);
    if (!phys && posix_vm_fault_page(p, uaddr, 0, 0))
        phys = vmm_translate(p->space, uaddr);
    if (!phys) return -EFAULT;

    if (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET) {
        uint64_t deadline = 0;
        int has_tmo = 0;
        if (timeout_va) {
            posix_timespec_t ts;
            if (posix_ucopy_in(&ts, timeout_va, sizeof(ts)) < 0) return -EFAULT;
            if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L)
                return -EINVAL;
            uint64_t ms = (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000L);
            if (ms == 0 && ts.tv_nsec > 0) ms = 1;
            if (cmd == FUTEX_WAIT_BITSET) {
                /* Mutlak zaman: FUTEX_CLOCK_REALTIME ise gercek zamana gore farki al */
                if (op & FUTEX_CLOCK_REALTIME) {
                    uint64_t rt_now = posix_realtime_ms();
                    uint64_t rem = (ms > rt_now) ? (ms - rt_now) : 0;
                    deadline = clock_ms() + rem;
                } else {
                    deadline = ms;
                }
            } else {
                deadline = clock_ms() + ms;
            }
            has_tmo = 1;
        }

        uint64_t f = sched_irq_save();
        spin_lock(&s_futex_lock);
        uint32_t cur_val = *(volatile uint32_t *)(uintptr_t)phys;
        if (cur_val != val) {
            spin_unlock(&s_futex_lock);
            sched_irq_restore(f);
            return -EAGAIN;
        }
        futex_waiter_t *fw = fwait_alloc_locked(&f);
        if (!fw) {
            spin_unlock(&s_futex_lock);
            sched_irq_restore(f);
            return -ENOMEM;
        }
        fw->used = 1;
        fw->phys = phys;
        fw->sched_tid = sched_current();
        fw->woken = 0;

        if (has_tmo) {
            uint64_t now = clock_ms();
            if (now >= deadline) {
                fw->used = 0;
                spin_unlock(&s_futex_lock);
                sched_irq_restore(f);
                return -ETIMEDOUT;
            }
            uint64_t rem = deadline - now;
            if (rem > 0x7FFFFFFFull) rem = 0x7FFFFFFFull;
            sched_prepare_sleep_ms((uint32_t)rem);
        } else {
            sched_prepare_block();
        }
        spin_unlock(&s_futex_lock);
        sched_commit_block();

        spin_lock(&s_futex_lock);
        int was_woken = fw->woken;
        fw->used = 0;
        spin_unlock(&s_futex_lock);
        sched_irq_restore(f);

        if (!was_woken && has_tmo && clock_ms() >= deadline)
            return -ETIMEDOUT;
        return 0;
    }

    if (cmd == FUTEX_WAKE || cmd == FUTEX_WAKE_BITSET) {
        int want = (int)val;
        if (want <= 0) return 0;
        int woken_total = 0;
        while (woken_total < want) {
            sched_tid_t wake_ids[64];
            int wn = 0;
            uint64_t f = sched_irq_save();
            spin_lock(&s_futex_lock);
            int cap = s_fwait_cap;
            for (int i = 0; i < cap && wn < 64 && (woken_total + wn) < want; i++) {
                futex_waiter_t *fw = fwait_slot(i);
                if (fw && fw->used && fw->phys == phys && !fw->woken) {
                    fw->woken = 1;
                    wake_ids[wn++] = fw->sched_tid;
                }
            }
            spin_unlock(&s_futex_lock);
            sched_irq_restore(f);
            if (wn == 0) break;
            for (int i = 0; i < wn; i++) sched_wake(wake_ids[i]);
            woken_total += wn;
        }
        return woken_total;
    }

    if (cmd == FUTEX_REQUEUE || cmd == FUTEX_CMP_REQUEUE) {
        uint64_t phys2 = vmm_translate(p->space, uaddr2);
        if (!phys2 && posix_vm_fault_page(p, uaddr2, 0, 0))
            phys2 = vmm_translate(p->space, uaddr2);
        if (!phys2) return -EFAULT;
        int wake_max = (int)val;
        int req_max = (int)(uint32_t)timeout_va;
        int woken_total = 0, rn = 0;

        uint64_t f = sched_irq_save();
        spin_lock(&s_futex_lock);
        if (cmd == FUTEX_CMP_REQUEUE) {
            uint32_t cur_val = *(volatile uint32_t *)(uintptr_t)phys;
            if (cur_val != val3) {
                spin_unlock(&s_futex_lock);
                sched_irq_restore(f);
                return -EAGAIN;
            }
        }
        int cap = s_fwait_cap;
        for (int i = 0; i < cap; i++) {
            futex_waiter_t *fw = fwait_slot(i);
            if (!fw || !fw->used || fw->phys != phys || fw->woken)
                continue;
            if (woken_total < wake_max) {
                fw->woken = 1;
                sched_tid_t tid = fw->sched_tid;
                spin_unlock(&s_futex_lock);
                sched_irq_restore(f);
                sched_wake(tid);
                woken_total++;
                f = sched_irq_save();
                spin_lock(&s_futex_lock);
                cap = s_fwait_cap;
            } else if (rn < req_max) {
                fw->phys = phys2;
                rn++;
            } else {
                break;
            }
        }
        spin_unlock(&s_futex_lock);
        sched_irq_restore(f);
        return woken_total + rn;
    }

    return -ENOSYS;
}

/* ---- E2B: Unified "RAM Doldu!" OOM Kancalari ---- */

static void pproc_name_copy(char *dst, int cap, const char *src) {
    int i = 0;
    if (cap <= 0) return;
    if (!src) { dst[0] = 0; return; }
    for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

int posix_oom_list_cb(asi_oom_app_t *out, int max_out) {
    if (!out || max_out <= 0) return 0;
    int n = 0;
    uint64_t f = sched_irq_save();
    spin_lock(&s_ptab_lock);
    int pcap = s_pproc_cap;
    int tcap = s_pthr_cap;
    for (int i = 0; i < pcap && n < max_out; i++) {
        posix_proc_t *p = pproc_slot(i);
        if (!p || !p->used || p->state != 1 || p->pid <= 1) continue;

        asi_oom_app_t *e = &out[n++];
        e->pid = p->pid;
        uint32_t kb = p->space ? vmm_space_ram_kb(p->space) : 0u;
        kb += (uint32_t)(p->nthreads > 0 ? p->nthreads : 1) * 32u;
        if (kb == 0u) kb = 64u;
        e->ram_kb = kb;

        uint8_t frz = 0;
        for (int j = 0; j < tcap; j++) {
            posix_thr_t *t = pthr_slot(j);
            if (t && t->used && t->proc == p && t->sched_tid > 0) {
                if (sched_is_frozen(t->sched_tid)) { frz = 1; break; }
            }
        }
        e->frozen = frz;
        e->kind = ASI_OOM_KIND_POSIX;
        e->reserved[0] = 0;
        e->reserved[1] = 0;
        pproc_name_copy(e->name, sizeof(e->name), p->name[0] ? p->name : "posix");
    }
    spin_unlock(&s_ptab_lock);
    sched_irq_restore(f);
    return n;
}

void posix_oom_freeze_cb(int freeze) {
    int cursor = 0;
    for (;;) {
        sched_tid_t batch[64];
        int bn = 0;
        uint64_t f = sched_irq_save();
        spin_lock(&s_ptab_lock);
        int tcap = s_pthr_cap;
        while (cursor < tcap && bn < 64) {
            posix_thr_t *t = pthr_slot(cursor++);
            if (t && t->used && t->proc && t->proc->used &&
                t->proc->state == 1 && t->sched_tid > 0) {
                batch[bn++] = t->sched_tid;
            }
        }
        spin_unlock(&s_ptab_lock);
        sched_irq_restore(f);

        if (bn == 0) break;
        for (int i = 0; i < bn; i++) {
            if (freeze) (void)sched_freeze_task(batch[i]);
            else        (void)sched_thaw_task(batch[i]);
        }
    }
}

int posix_oom_kill_cb(int pid) {
    if (pid <= 1) return -1;

    uint64_t f = sched_irq_save();
    spin_lock(&s_ptab_lock);
    posix_proc_t *p = pproc_find_locked(pid);
    if (!p || p->state == 0) {
        spin_unlock(&s_ptab_lock);
        sched_irq_restore(f);
        return -1;
    }
    if (p->state == 2) {
        p->used = 0;
        p->state = 0;
        spin_unlock(&s_ptab_lock);
        sched_irq_restore(f);
        return 0;
    }

    p->state = 2; /* zombie / teardown claimed */
    p->exit_code = -9;
    p->nthreads = 0;

    int wake_list[WAITQ_MAX * 2];
    int wn = 0;
    int w;
    while ((w = waitq_pop(&p->wait_wq)) >= 0 && wn < WAITQ_MAX)
        wake_list[wn++] = w;
    posix_proc_t *par = pproc_find_locked(p->ppid);
    if (par) {
        while ((w = waitq_pop(&par->wait_wq)) >= 0 && wn < WAITQ_MAX * 2)
            wake_list[wn++] = w;
    }
    int keep_zombie = (wn > 0) || (par != 0);
    spin_unlock(&s_ptab_lock);
    sched_irq_restore(f);

    /* Surecin tum cekirdek thread'lerini durdur, futex kuyrugunu temizle ve bic */
    for (;;) {
        sched_tid_t batch[64];
        int bn = 0;
        f = sched_irq_save();
        spin_lock(&s_ptab_lock);
        int tcap = s_pthr_cap;
        for (int i = 0; i < tcap && bn < 64; i++) {
            posix_thr_t *t = pthr_slot(i);
            if (t && t->used && t->proc == p) {
                t->used = 0;
                if (t->sched_tid > 0) batch[bn++] = t->sched_tid;
            }
        }
        spin_unlock(&s_ptab_lock);
        sched_irq_restore(f);
        if (bn == 0) break;

        for (int i = 0; i < bn; i++) {
            fwait_clear_tid(batch[i]);
            if (batch[i] != sched_current()) {
                sched_terminate_task(batch[i]);
                sched_reap(batch[i]);
            }
        }
    }

    /* FD'leri kapat (boru/soket/SHM/GFX pencereleri temizlenir) */
    posix_fd_close_all(p, 0);

    /* Adres alanini ve tum fiziksel sayfalari hemen geri kazan */
    vmm_state_t *vst = VMM_STATE;
    if (p->space && vst && vst->kernel_space && vmm_current() == p->space) {
        sched_set_current_cr3(vst->kernel_space);
    }
    posix_vm_destroy_proc(p);

    if (!keep_zombie) {
        f = sched_irq_save();
        spin_lock(&s_ptab_lock);
        p->state = 0;
        p->used = 0;
        spin_unlock(&s_ptab_lock);
        sched_irq_restore(f);
    }

    for (int i = 0; i < wn; i++) sched_wake(wake_list[i]);
    return 0;
}

int posix_fault_handler(int int_no, uint64_t err, uint64_t cr2, uint64_t rip) {
    posix_thr_t *t = posix_cur_thr();
    if (!t || !t->proc) return 0;
    if (int_no == 14) posix_vm_dump_fault(t->proc, cr2);
    serial_printf("[posix] ring3 hata (int=%d err=0x%X cr2=0x%X%08X rip=0x%X%08X) -> pid=%d tid=%d olduruldu\n",
                  int_no, (unsigned)err,
                  (unsigned)(cr2 >> 32), (unsigned)cr2,
                  (unsigned)(rip >> 32), (unsigned)rip,
                  t->proc->pid, t->tid);
    posix_sys_exit(-11, 1);
    return 1;
}
