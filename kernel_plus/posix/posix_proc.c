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

static posix_proc_t s_pprocs[POSIX_MAX_PROCS];
static posix_thr_t  s_pthrs[POSIX_MAX_THREADS];
static spinlock_t   s_ptab_lock;
static int          s_next_pid = 100;
static int          s_next_tid = 100;

/* ---- Futex bekleme kuyrugu (fiziksel adres tabanli -> MAP_SHARED uyumlu) ---- */
#define FUTEX_MAX_WAITERS 64

typedef struct {
    volatile int used;
    uint64_t     phys;
    sched_tid_t  sched_tid;
    volatile int woken;
} futex_waiter_t;

static futex_waiter_t s_fwaiters[FUTEX_MAX_WAITERS];
static spinlock_t     s_futex_lock;

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
    for (int i = 0; i < POSIX_MAX_PROCS; i++) {
        if (!s_pprocs[i].used) {
            p = &s_pprocs[i];
            memset(p, 0, sizeof(*p));
            p->used = 1;
            p->pid = s_next_pid++;
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
            break;
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
    for (int i = 0; i < POSIX_MAX_THREADS; i++) {
        if (!s_pthrs[i].used) {
            t = &s_pthrs[i];
            memset(t, 0, sizeof(*t));
            t->used = 1;
            t->tid = (p->nthreads == 0) ? p->pid : s_next_tid++;
            t->proc = p;
            p->nthreads++;
            break;
        }
    }
    spin_unlock(&s_ptab_lock);
    sched_irq_restore(f);
    return t;
}

static posix_proc_t *pproc_find_locked(int pid) {
    for (int i = 0; i < POSIX_MAX_PROCS; i++)
        if (s_pprocs[i].used && s_pprocs[i].pid == pid) return &s_pprocs[i];
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

int posix_spawn_elf_mem(const char *name, const uint8_t *elf, uint64_t size,
                        char *const argv[], int argc) {
    if (!elf || size < 64) return -ENOEXEC;
    posix_proc_t *p = pproc_alloc(1, name ? name : "elf");
    if (!p) return -ENOMEM;

    p->space = vmm_space_create();
    if (!p->space) { p->used = 0; return -ENOMEM; }
    posix_vm_init_proc(p);
    posix_fd_init_stdio(p);

    char *def_argv[2] = { (char *)(name ? name : "elf"), 0 };
    char *def_envp[3] = { "PATH=/bin:/", "HOME=/", "TERM=arctian" };
    posix_elf_info_t info;
    int rc = posix_elf_load(p, elf, size,
                            (argv && argc > 0) ? argv : def_argv,
                            (argv && argc > 0) ? argc : 1,
                            def_envp, 3, &info);
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

/* AFS yolundan ELF64 oku ve baslat. */
static uint8_t *load_afs_file(const char *path, uint64_t *sz_out) {
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
    uint32_t fsz = ast.size;
    if (fsz < 64 || fsz > (16u * 1024u * 1024u)) return 0;
    uint8_t *buf = (uint8_t *)kmalloc(fsz);
    if (!buf) return 0;
    int rd = afs_read_at(h, buf, fsz, 0);
    if (rd <= 0) { kfree(buf); return 0; }
    *sz_out = (uint64_t)rd;
    return buf;
}

int posix_spawn_elf_path(const char *path, char *const argv[], int argc) {
    if (!path) return -EINVAL;
    uint64_t sz = 0;
    uint8_t *buf = load_afs_file(path, &sz);
    if (!buf) return -ENOENT;
    int pid = posix_spawn_elf_mem(path, buf, sz, argv, argc);
    kfree(buf);
    return pid;
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

/* ---- fork() ---- */
int64_t posix_sys_fork(posix_regs_t *regs) {
    posix_thr_t *cur_t = posix_cur_thr();
    if (!cur_t || !cur_t->proc) return -ESRCH;
    posix_proc_t *par = cur_t->proc;

    posix_proc_t *ch = pproc_alloc(par->pid, par->name);
    if (!ch) return -ENOMEM;
    ch->space = vmm_space_create();
    if (!ch->space) { ch->used = 0; return -ENOMEM; }
    posix_vm_init_proc(ch);
    strcpy(ch->cwd, par->cwd);

    uint64_t fl = sched_irq_save();
    spin_lock(&par->lock);
    int vrc = posix_vm_clone(ch, par);
    spin_unlock(&par->lock);
    sched_irq_restore(fl);
    if (vrc < 0) {
        posix_vm_destroy_proc(ch);
        ch->used = 0;
        return vrc;
    }

    posix_fd_clone_table(ch, par);

    posix_thr_t *ct = pthr_alloc(ch);
    if (!ct) {
        posix_fd_close_all(ch, 0);
        posix_vm_destroy_proc(ch);
        ch->used = 0;
        return -ENOMEM;
    }

    ct->fs_base = cur_t->fs_base;
    ct->init_regs = *regs;
    ct->init_regs.rax = 0;  /* fork() cocukta 0 doner */
    ct->init_regs.cs = 0x23;
    ct->init_regs.ss = 0x1B;
    ct->init_regs.rflags |= 0x200;

    sched_tid_t stid = sched_spawn_ex(ch->name, posix_thr_trampoline, ct, ch->space);
    if (stid < 0) {
        ct->used = 0;
        posix_fd_close_all(ch, 0);
        posix_vm_destroy_proc(ch);
        ch->used = 0;
        return -ENOMEM;
    }
    ct->sched_tid = stid;
    return ch->pid;
}

/* ---- clone() (gercek cekirdek thread'i veya fork) ---- */
int64_t posix_sys_clone(posix_regs_t *regs, uint64_t flags, uint64_t child_stack,
                        uint64_t ptid, uint64_t ctid, uint64_t newtls) {
    if (!(flags & CLONE_VM)) {
        /* Surec catallama (fork / posix_spawn) */
        int64_t cpid = posix_sys_fork(regs);
        if (cpid > 0 && (flags & CLONE_PARENT_SETTID) && ptid) {
            int32_t v = (int32_t)cpid;
            posix_ucopy_out(ptid, &v, 4);
        }
        return cpid;
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
int64_t posix_sys_execve(posix_regs_t *regs, uint64_t path_va,
                         uint64_t argv_va, uint64_t envp_va) {
    posix_thr_t *cur_t = posix_cur_thr();
    if (!cur_t || !cur_t->proc) return -ESRCH;
    posix_proc_t *p = cur_t->proc;

    char path[192];
    if (posix_ucstr_in(path, sizeof(path), path_va) < 0) return -EFAULT;

    /* argv ve envp'yi eski adres alanindan kernel tamponlarina kopyala */
    static char k_args[16][128];
    static char k_envs[16][128];
    char *k_argv[16];
    char *k_envp[16];
    int argc = 0, envc = 0;

    if (argv_va) {
        for (int i = 0; i < 16; i++) {
            uint64_t ptr = 0;
            if (posix_ucopy_in(&ptr, argv_va + (uint64_t)i * 8, 8) < 0) break;
            if (!ptr) break;
            if (posix_ucstr_in(k_args[argc], sizeof(k_args[0]), ptr) == 0) {
                k_argv[argc] = k_args[argc];
                argc++;
            }
        }
    }
    if (argc == 0) {
        strncpy(k_args[0], path, sizeof(k_args[0]) - 1);
        k_args[0][sizeof(k_args[0]) - 1] = 0;
        k_argv[0] = k_args[0];
        argc = 1;
    }

    if (envp_va) {
        for (int i = 0; i < 16; i++) {
            uint64_t ptr = 0;
            if (posix_ucopy_in(&ptr, envp_va + (uint64_t)i * 8, 8) < 0) break;
            if (!ptr) break;
            if (posix_ucstr_in(k_envs[envc], sizeof(k_envs[0]), ptr) == 0) {
                k_envp[envc] = k_envs[envc];
                envc++;
            }
        }
    }

    uint64_t elf_sz = 0;
    uint8_t *elf = load_afs_file(path, &elf_sz);
    if (!elf) return -ENOENT;

    /* Yeni adres alani kur; basarili olursa eskisini yik. */
    vmm_space_t old_sp = p->space;
    posix_vma_t old_vmas[POSIX_MAX_VMAS];
    memcpy(old_vmas, p->vmas, sizeof(old_vmas));

    vmm_space_t new_sp = vmm_space_create();
    if (!new_sp) { kfree(elf); return -ENOMEM; }

    p->space = new_sp;
    posix_vm_init_proc(p);
    posix_elf_info_t info;
    int rc = posix_elf_load(p, elf, elf_sz, k_argv, argc, k_envp, envc, &info);
    kfree(elf);

    if (rc < 0) {
        posix_vm_destroy_proc(p);
        p->space = old_sp;
        memcpy(p->vmas, old_vmas, sizeof(old_vmas));
        return rc;
    }

    /* Yeni adres alanina gec, eski VMA/SHM ve PML4'u temizle. */
    sched_set_current_cr3(new_sp);
    for (int i = 0; i < POSIX_MAX_VMAS; i++) {
        if (old_vmas[i].used && old_vmas[i].shm_id >= 0)
            posix_shm_unref(old_vmas[i].shm_id);
    }
    if (old_sp) vmm_space_destroy(old_sp);

    posix_fd_close_all(p, 1);  /* yalniz FD_CLOEXEC kapat */
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
        for (int i = 0; i < POSIX_MAX_PROCS; i++) {
            posix_proc_t *c = &s_pprocs[i];
            if (!c->used || c->ppid != par->pid) continue;
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
        uint64_t phys = vmm_translate(p->space, t->clear_child_tid);
        posix_ucopy_out(t->clear_child_tid, &zero, 4);
        if (phys) posix_futex_wake_phys(phys, 1);
    }

    int last = 0;
    int wake_list[WAITQ_MAX * 2];
    int wn = 0;

    uint64_t f = sched_irq_save();
    spin_lock(&s_ptab_lock);
    t->used = 0;
    p->nthreads--;
    if (group || p->nthreads <= 0) {
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
void posix_futex_wake_phys(uint64_t phys, int count) {
    if (!phys || count <= 0) return;
    sched_tid_t wake_ids[FUTEX_MAX_WAITERS];
    int wn = 0;

    uint64_t f = sched_irq_save();
    spin_lock(&s_futex_lock);
    for (int i = 0; i < FUTEX_MAX_WAITERS && wn < count; i++) {
        if (s_fwaiters[i].used && s_fwaiters[i].phys == phys && !s_fwaiters[i].woken) {
            s_fwaiters[i].woken = 1;
            wake_ids[wn++] = s_fwaiters[i].sched_tid;
        }
    }
    spin_unlock(&s_futex_lock);
    sched_irq_restore(f);

    for (int i = 0; i < wn; i++) sched_wake(wake_ids[i]);
}

int64_t posix_sys_futex(uint64_t uaddr, int op, uint32_t val,
                        uint64_t timeout_va, uint64_t uaddr2, uint32_t val3) {
    posix_proc_t *p = posix_cur_proc();
    if (!p || (uaddr & 3ull)) return -EINVAL;
    int cmd = op & FUTEX_CMD_MASK;

    uint64_t phys = vmm_translate(p->space, uaddr);
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
                /* Mutlak zaman: clock_ms ile farkini al */
                uint64_t now = clock_ms();
                deadline = (ms > now) ? ms : now;
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
        int slot = -1;
        for (int i = 0; i < FUTEX_MAX_WAITERS; i++) {
            if (!s_fwaiters[i].used) {
                s_fwaiters[i].used = 1;
                s_fwaiters[i].phys = phys;
                s_fwaiters[i].sched_tid = sched_current();
                s_fwaiters[i].woken = 0;
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            spin_unlock(&s_futex_lock);
            sched_irq_restore(f);
            return -ENOMEM;
        }

        if (has_tmo) {
            uint64_t now = clock_ms();
            if (now >= deadline) {
                s_fwaiters[slot].used = 0;
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
        int was_woken = s_fwaiters[slot].woken;
        s_fwaiters[slot].used = 0;
        spin_unlock(&s_futex_lock);
        sched_irq_restore(f);

        if (!was_woken && has_tmo && clock_ms() >= deadline)
            return -ETIMEDOUT;
        return 0;
    }

    if (cmd == FUTEX_WAKE || cmd == FUTEX_WAKE_BITSET) {
        int want = (int)val;
        if (want <= 0) return 0;
        sched_tid_t wake_ids[FUTEX_MAX_WAITERS];
        int wn = 0;
        uint64_t f = sched_irq_save();
        spin_lock(&s_futex_lock);
        for (int i = 0; i < FUTEX_MAX_WAITERS && wn < want; i++) {
            if (s_fwaiters[i].used && s_fwaiters[i].phys == phys && !s_fwaiters[i].woken) {
                s_fwaiters[i].woken = 1;
                wake_ids[wn++] = s_fwaiters[i].sched_tid;
            }
        }
        spin_unlock(&s_futex_lock);
        sched_irq_restore(f);
        for (int i = 0; i < wn; i++) sched_wake(wake_ids[i]);
        return wn;
    }

    if (cmd == FUTEX_REQUEUE || cmd == FUTEX_CMP_REQUEUE) {
        uint64_t phys2 = vmm_translate(p->space, uaddr2);
        if (!phys2) return -EFAULT;
        int wake_max = (int)val;
        int req_max = (int)(uint32_t)timeout_va;
        sched_tid_t wake_ids[FUTEX_MAX_WAITERS];
        int wn = 0, rn = 0;

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
        for (int i = 0; i < FUTEX_MAX_WAITERS; i++) {
            if (!s_fwaiters[i].used || s_fwaiters[i].phys != phys || s_fwaiters[i].woken)
                continue;
            if (wn < wake_max) {
                s_fwaiters[i].woken = 1;
                wake_ids[wn++] = s_fwaiters[i].sched_tid;
            } else if (rn < req_max) {
                s_fwaiters[i].phys = phys2;
                rn++;
            } else {
                break;
            }
        }
        spin_unlock(&s_futex_lock);
        sched_irq_restore(f);
        for (int i = 0; i < wn; i++) sched_wake(wake_ids[i]);
        return wn + rn;
    }

    return -ENOSYS;
}

int posix_fault_handler(int int_no, uint64_t err, uint64_t cr2, uint64_t rip) {
    posix_thr_t *t = posix_cur_thr();
    if (!t || !t->proc) return 0;
    serial_printf("[posix] ring3 hata (int=%d err=0x%X cr2=0x%X%08X rip=0x%X%08X) -> pid=%d tid=%d olduruldu\n",
                  int_no, (unsigned)err,
                  (unsigned)(cr2 >> 32), (unsigned)cr2,
                  (unsigned)(rip >> 32), (unsigned)rip,
                  t->proc->pid, t->tid);
    posix_sys_exit(-11, 1);
    return 1;
}
