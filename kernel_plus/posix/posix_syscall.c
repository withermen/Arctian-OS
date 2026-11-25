/* x86-64 SYSCALL/SYSRET kurulumu ve Linux ABI uyumlu POSIX syscall dagiticisi. */
#include "posix.h"
#include "isr.h"
#include "clock.h"
#include "percpu.h"
#include "percpu_gdt.h"
#include "serial.h"
#include <string.h>

#define MSR_EFER           0xC0000080u
#define MSR_STAR           0xC0000081u
#define MSR_LSTAR          0xC0000082u
#define MSR_FMASK          0xC0000084u
#define MSR_GS_BASE        0xC0000101u
#define MSR_KERNEL_GS_BASE 0xC0000102u

extern void arctian_syscall_entry(void);

static inline uint64_t rdmsr64(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr64(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" :: "c"(msr), "a"(lo), "d"(hi) : "memory");
}

static uint8_t s_syscall_fallback_stack[4096] __attribute__((aligned(16)));

void posix_syscall_enable_cpu(void) {
    uint32_t ci = percpu_self_index();
    if (ci >= CPU_MAX_APICS) ci = 0;
    percpu_t *pc = percpu_by_index(ci);
    if (pc) {
        if (!pc->syscall_kstack) {
            uint64_t ktop = sched_kstack_top_current();
            pc->syscall_kstack = ktop ? ktop
                : (uint64_t)(uintptr_t)(s_syscall_fallback_stack + sizeof(s_syscall_fallback_stack));
        }
        wrmsr64(MSR_GS_BASE, 0);
        wrmsr64(MSR_KERNEL_GS_BASE, (uint64_t)(uintptr_t)pc);
    }

    /* EFER.SCE = 1 (SYSCALL/SYSRET etkin) */
    uint64_t efer = rdmsr64(MSR_EFER);
    wrmsr64(MSR_EFER, efer | 1ull);

    /* STAR:
     *   [47:32] = 0x0008 -> SYSCALL: CS=0x08, SS=0x10
     *   [63:48] = 0x0010 -> SYSRET:  SS=0x1B, CS=0x23 */
    uint64_t star = ((uint64_t)0x0010u << 48) | ((uint64_t)0x0008u << 32);
    wrmsr64(MSR_STAR, star);
    wrmsr64(MSR_LSTAR, (uint64_t)(uintptr_t)&arctian_syscall_entry);
    wrmsr64(MSR_FMASK, 0x700ull);   /* SYSCALL girisinde TF|IF|DF kapat */
}

/* Exception kapilarini (ozellikle #PF vektor 14) Interrupt Gate (0x8E) yap:
 * Trap Gate (0x8F) IF=1 biraktigi icin #PF girisinde mov %%cr2 oncesi
 * timer IRQ / gorev degisimi girerse CR2 baska surecin adresiyle ezilir! */
static void posix_harden_exception_gates(void) {
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) idtr;
    struct idt_ent {
        uint16_t off_lo;
        uint16_t sel;
        uint8_t  ist;
        uint8_t  type_attr;
        uint16_t off_mid;
        uint32_t off_hi;
        uint32_t resv;
    } __attribute__((packed));

    __asm__ volatile("sidt %0" : "=m"(idtr) :: "memory");
    if (!idtr.base || idtr.limit < (uint16_t)(32u * sizeof(struct idt_ent) - 1u))
        return;

    uint64_t fl = sched_irq_save();
    volatile struct idt_ent *idt = (volatile struct idt_ent *)(uintptr_t)idtr.base;
    for (int i = 0; i < 32; i++) {
        if ((idt[i].type_attr & 0x8Fu) == 0x8Fu) {
            idt[i].type_attr = (uint8_t)((idt[i].type_attr & ~0x0Fu) | 0x0Eu);
        }
    }
    sched_irq_restore(fl);
}

void posix_init(void) {
    posix_fd_init_subsys();
    posix_sock_init();
    posix_gfx_init();
    posix_harden_exception_gates();
    arctian_set_pf_handler(posix_pf_handler);
    posix_syscall_enable_cpu();
    serial_puts("[posix] SYSCALL/SYSRET + ELF64/VM(demand)/thread/futex/fd/sock/gfx hazir\n");
}

/* ---- Kullanici bellegi guvenli erisim yardimcilari (demand-paging uyumlu) ---- */
int posix_uprobe(uint64_t va, uint64_t len, int write) {
    if (len == 0) return 1;
    uint64_t end = va + len;
    if (end < va) return 0;
    if (va < (uint64_t)VMM_USER_BASE || end > VMM_USER_TOP) return 0;
    vmm_space_t sp = vmm_space_current();
    posix_proc_t *p = posix_cur_proc();
    for (uint64_t pg = va & ~0xFFFull; pg < end; pg += 0x1000ull) {
        if (!vmm_probe_user(sp, pg, write)) {
            if (!p || !posix_vm_fault_page(p, pg, write, 0)) return 0;
            if (!vmm_probe_user(sp, pg, write)) return 0;
        }
    }
    return 1;
}

int posix_ucopy_in(void *kdst, uint64_t usrc, uint64_t len) {
    if (len == 0) return 0;
    if (!posix_uprobe(usrc, len, 0)) return -EFAULT;
    memcpy(kdst, (const void *)(uintptr_t)usrc, (size_t)len);
    return 0;
}

int posix_ucopy_out(uint64_t udst, const void *ksrc, uint64_t len) {
    if (len == 0) return 0;
    if (!posix_uprobe(udst, len, 1)) return -EFAULT;
    memcpy((void *)(uintptr_t)udst, ksrc, (size_t)len);
    return 0;
}

int posix_ucstr_in(char *kdst, uint64_t cap, uint64_t usrc) {
    if (!kdst || cap == 0) return -EINVAL;
    if (usrc < (uint64_t)VMM_USER_BASE || usrc >= VMM_USER_TOP) return -EFAULT;
    vmm_space_t sp = vmm_space_current();
    posix_proc_t *p = posix_cur_proc();
    if (!vmm_probe_user(sp, usrc & ~0xFFFull, 0)) {
        if (!p || !posix_vm_fault_page(p, usrc, 0, 0) ||
            !vmm_probe_user(sp, usrc & ~0xFFFull, 0))
            return -EFAULT;
    }
    uint64_t i = 0;
    for (; i + 1 < cap; i++) {
        uint64_t a = usrc + i;
        if ((a & 0xFFFull) == 0 && !vmm_probe_user(sp, a, 0)) {
            if (!p || !posix_vm_fault_page(p, a, 0, 0) || !vmm_probe_user(sp, a, 0))
                return -EFAULT;
        }
        char c = *(const char *)(uintptr_t)a;
        kdst[i] = c;
        if (c == 0) return 0;
    }
    kdst[cap - 1] = 0;
    return -ENAMETOOLONG;
}

static void posix_fill_rlimit(int resource, uint64_t lim[2]) {
    vmm_meminfo_t mi;
    vmm_mem_info(&mi);
    uint64_t ram = mi.total_bytes ? mi.total_bytes : (96ull * 1024ull * 1024ull);
    switch (resource) {
    case 3: /* RLIMIT_STACK */
        lim[0] = POSIX_USER_STACK_SZ_BYTES;
        lim[1] = POSIX_USER_STACK_SZ_BYTES;
        break;
    case 7: /* RLIMIT_NOFILE */
        lim[0] = (uint64_t)POSIX_MAX_FDS;
        lim[1] = (uint64_t)POSIX_MAX_FDS;
        break;
    case 2: /* RLIMIT_DATA */
        lim[0] = (ram * 3ull) / 4ull;
        lim[1] = (ram * 3ull) / 4ull;
        break;
    case 9: /* RLIMIT_AS */
        lim[0] = ~0ull; /* RLIM_INFINITY */
        lim[1] = ~0ull;
        break;
    default:
        lim[0] = ram;
        lim[1] = ram;
        break;
    }
}

/* ---- Ana Linux x86-64 Syscall Dagitici ---- */
void posix_syscall_dispatch(posix_regs_t *regs) {
    /* Kesmeleri ac: bloklayan syscall'lar (futex/poll/wait4/sleep/net)
     * zamanlayici ve ag kesmeleriyle paralel calisabilsin. */
    __asm__ volatile("sti" ::: "memory");

    posix_thr_t  *t = posix_cur_thr();
    posix_proc_t *p = t ? t->proc : 0;
    uint64_t nr = regs->rax;
    uint64_t a1 = regs->rdi;
    uint64_t a2 = regs->rsi;
    uint64_t a3 = regs->rdx;
    uint64_t a4 = regs->r10;
    uint64_t a5 = regs->r8;
    uint64_t a6 = regs->r9;
    int64_t ret = -ENOSYS;

    switch (nr) {
    case NR_read:       ret = posix_sys_read(p, (int)a1, a2, a3); break;
    case NR_write:      ret = posix_sys_write(p, (int)a1, a2, a3); break;
    case NR_open:       ret = posix_sys_openat(p, AT_FDCWD, a1, (int)a2, (uint32_t)a3); break;
    case NR_openat:     ret = posix_sys_openat(p, (int)a1, a2, (int)a3, (uint32_t)a4); break;
    case NR_close:      ret = posix_sys_close(p, (int)a1); break;
    case NR_stat:       ret = posix_sys_newfstatat(p, AT_FDCWD, a1, a2, 0); break;
    case NR_lstat:      ret = posix_sys_newfstatat(p, AT_FDCWD, a1, a2, AT_SYMLINK_NOFOLLOW); break;
    case NR_fstat:      ret = posix_sys_fstat(p, (int)a1, a2); break;
    case NR_newfstatat: ret = posix_sys_newfstatat(p, (int)a1, a2, a3, (int)a4); break;
    case NR_lseek:      ret = posix_sys_lseek(p, (int)a1, (int64_t)a2, (int)a3); break;
    case NR_readv:      ret = posix_sys_readv(p, (int)a1, a2, (int)a3); break;
    case NR_writev:     ret = posix_sys_writev(p, (int)a1, a2, (int)a3); break;
    case NR_pread64: {
        posix_file_t *f = posix_fd_get(p, (int)a1);
        if (!f) { ret = -EBADF; break; }
        uint64_t saved = f->offset;
        f->offset = a4;
        ret = posix_sys_read(p, (int)a1, a2, a3);
        f->offset = saved;
        break;
    }
    case NR_pwrite64: {
        posix_file_t *f = posix_fd_get(p, (int)a1);
        if (!f) { ret = -EBADF; break; }
        uint64_t saved = f->offset;
        f->offset = a4;
        ret = posix_sys_write(p, (int)a1, a2, a3);
        f->offset = saved;
        break;
    }
    case NR_poll:       ret = posix_sys_poll(p, a1, a2, (int)a3); break;
    case NR_ppoll: {
        int tmo = -1;
        if (a3) {
            posix_timespec_t ts;
            if (posix_ucopy_in(&ts, a3, sizeof(ts)) == 0)
                tmo = (int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
        }
        ret = posix_sys_poll(p, a1, a2, tmo);
        break;
    }
    case NR_mmap:       ret = posix_sys_mmap(p, a1, a2, (uint32_t)a3, (uint32_t)a4, (int)a5, a6); break;
    case NR_mprotect:   ret = posix_sys_mprotect(p, a1, a2, (uint32_t)a3); break;
    case NR_munmap:     ret = posix_sys_munmap(p, a1, a2); break;
    case NR_mremap:     ret = posix_sys_mremap(p, a1, a2, a3, (uint32_t)a4, a5); break;
    case NR_brk:        ret = posix_sys_brk(p, a1); break;
    case NR_madvise:    ret = posix_sys_madvise(p, a1, a2, (int)a3); break;
    case NR_ioctl:      ret = posix_sys_ioctl(p, (int)a1, a2, a3); break;
    case NR_access:     ret = posix_sys_access(p, AT_FDCWD, a1, (int)a2); break;
    case NR_faccessat:
    case NR_faccessat2: ret = posix_sys_access(p, (int)a1, a2, (int)a3); break;
    case NR_pipe:       ret = posix_sys_pipe2(p, a1, 0); break;
    case NR_pipe2:      ret = posix_sys_pipe2(p, a1, (int)a2); break;
    case NR_dup:        ret = posix_sys_dup(p, (int)a1); break;
    case NR_dup2:       ret = posix_sys_dup3(p, (int)a1, (int)a2, 0); break;
    case NR_dup3:       ret = posix_sys_dup3(p, (int)a1, (int)a2, (int)a3); break;
    case NR_fcntl:      ret = posix_sys_fcntl(p, (int)a1, (int)a2, a3); break;
    case NR_flock:
    case NR_fsync:
    case NR_fdatasync:  ret = 0; break;
    case NR_ftruncate:  ret = posix_sys_ftruncate(p, (int)a1, (int64_t)a2); break;
    case NR_getcwd:     ret = posix_sys_getcwd(p, a1, a2); break;
    case NR_chdir:      ret = posix_sys_chdir(p, a1); break;
    case NR_mkdir:      ret = posix_sys_mkdirat(p, AT_FDCWD, a1, (uint32_t)a2); break;
    case NR_mkdirat:    ret = posix_sys_mkdirat(p, (int)a1, a2, (uint32_t)a3); break;
    case NR_unlink:     ret = posix_sys_unlinkat(p, AT_FDCWD, a1, 0); break;
    case NR_rmdir:      ret = posix_sys_unlinkat(p, AT_FDCWD, a1, 0); break;
    case NR_unlinkat:   ret = posix_sys_unlinkat(p, (int)a1, a2, (int)a3); break;
    case NR_rename:     ret = posix_sys_rename(p, a1, a2); break;
    case NR_renameat:
    case NR_renameat2:  ret = posix_sys_rename(p, a2, a4); break;
    case NR_getdents64: ret = posix_sys_getdents64(p, (int)a1, a2, a3); break;
    case NR_readlink:   ret = posix_sys_readlinkat(p, AT_FDCWD, a1, a2, a3); break;
    case NR_readlinkat: ret = posix_sys_readlinkat(p, (int)a1, a2, a3, a4); break;
    case NR_chmod:
    case NR_fchmod:
    case NR_chown:
    case NR_fchown:
    case NR_lchown:
    case NR_fchownat:
    case NR_fchmodat:
    case NR_utimensat:  ret = 0; break;
    case NR_umask:      ret = 0022; break;
    case NR_statfs:
    case NR_fstatfs:    ret = posix_sys_statfs(a2); break;
    case NR_eventfd:    ret = posix_sys_eventfd2(p, (uint32_t)a1, 0); break;
    case NR_eventfd2:   ret = posix_sys_eventfd2(p, (uint32_t)a1, (int)a2); break;
    case NR_sched_yield: sched_yield(); ret = 0; break;
    case NR_nanosleep:  ret = posix_sys_nanosleep(a1, a2); break;
    case NR_clock_nanosleep: {
        if (a2 & 1u /* TIMER_ABSTIME */) {
            posix_timespec_t req;
            if (posix_ucopy_in(&req, a3, sizeof(req)) < 0) { ret = -EFAULT; break; }
            if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000L) {
                ret = -EINVAL; break;
            }
            uint64_t target_ms = (uint64_t)req.tv_sec * 1000ull + (uint64_t)(req.tv_nsec / 1000000L);
            if (req.tv_nsec % 1000000L) target_ms++;
            uint64_t now = ((int)a1 == CLOCK_REALTIME) ? posix_realtime_ms() : clock_ms();
            if (target_ms > now) {
                uint64_t diff = target_ms - now;
                if (diff > 0x7FFFFFFFull) diff = 0x7FFFFFFFull;
                sched_sleep_ms((uint32_t)diff);
            }
            ret = 0;
        } else {
            ret = posix_sys_nanosleep(a3, a4);
        }
        break;
    }
    case NR_clock_gettime:   ret = posix_sys_clock_gettime((int)a1, a2); break;
    case NR_clock_getres: {
        posix_timespec_t res = { 0, 1000000 }; /* 1 ms */
        ret = a2 ? posix_ucopy_out(a2, &res, sizeof(res)) : 0;
        break;
    }
    case NR_gettimeofday: {
        extern uint64_t clock_ms(void);
        extern uint32_t clock_unix(void);
        int64_t tv[2];
        uint64_t ms = clock_ms();
        uint32_t ux = clock_unix();
        tv[0] = ux ? (int64_t)ux : (int64_t)(1795400000ll + (int64_t)(ms / 1000ull));
        tv[1] = (int64_t)((ms % 1000ull) * 1000ull);
        ret = a1 ? posix_ucopy_out(a1, tv, sizeof(tv)) : 0;
        break;
    }
    case NR_getrandom:  ret = posix_sys_getrandom(a1, a2, (uint32_t)a3); break;
    case NR_memfd_create: ret = posix_sys_memfd_create(p, a1, (uint32_t)a2); break;
    case NR_getpid:     ret = p ? p->pid : 1; break;
    case NR_getppid:    ret = p ? p->ppid : 0; break;
    case NR_gettid:     ret = t ? t->tid : 1; break;
    case NR_getuid:
    case NR_geteuid:
    case NR_getgid:
    case NR_getegid:
    case NR_setpgid:
    case NR_getpgrp:
    case NR_setsid:     ret = 0; break;
    case NR_fork:
    case NR_vfork:      ret = posix_sys_fork(regs); break;
    case NR_clone:      ret = posix_sys_clone(regs, a1, a2, a3, a4, a5); break;
    case NR_execve:
        ret = posix_sys_execve(regs, a1, a2, a3);
        if (ret == 0) return; /* regs yeni ELF'e gore kuruldu, rax'i ezme! */
        break;
    case NR_wait4:      ret = posix_sys_wait4((int)a1, a2, (int)a3); break;
    case NR_exit:       posix_sys_exit((int)a1, 0); break;
    case NR_exit_group: posix_sys_exit((int)a1, 1); break;
    case NR_arch_prctl: ret = posix_sys_arch_prctl((int)a1, a2); break;
    case NR_set_tid_address:
        if (t) t->clear_child_tid = a1;
        ret = t ? t->tid : 1;
        break;
    case NR_futex:      ret = posix_sys_futex(a1, (int)a2, (uint32_t)a3, a4, a5, (uint32_t)a6); break;
    case NR_socket:     ret = posix_sys_socket(p, (int)a1, (int)a2, (int)a3); break;
    case NR_socketpair: ret = posix_sys_socketpair(p, (int)a1, (int)a2, (int)a3, a4); break;
    case NR_bind:       ret = posix_sys_bind(p, (int)a1, a2, (uint32_t)a3); break;
    case NR_listen:     ret = posix_sys_listen(p, (int)a1, (int)a2); break;
    case NR_accept:     ret = posix_sys_accept4(p, (int)a1, a2, a3, 0); break;
    case NR_accept4:    ret = posix_sys_accept4(p, (int)a1, a2, a3, (int)a4); break;
    case NR_connect:    ret = posix_sys_connect(p, (int)a1, a2, (uint32_t)a3); break;
    case NR_sendto:     ret = posix_sys_sendto(p, (int)a1, a2, a3, (int)a4, a5, (uint32_t)a6); break;
    case NR_recvfrom:   ret = posix_sys_recvfrom(p, (int)a1, a2, a3, (int)a4, a5, a6); break;
    case NR_sendmsg:    ret = posix_sys_sendmsg(p, (int)a1, a2, (int)a3); break;
    case NR_recvmsg:    ret = posix_sys_recvmsg(p, (int)a1, a2, (int)a3); break;
    case NR_shutdown:   ret = posix_sys_shutdown(p, (int)a1, (int)a2); break;
    case NR_setsockopt: ret = posix_sys_setsockopt(p, (int)a1, (int)a2, (int)a3, a4, (uint32_t)a5); break;
    case NR_getsockopt: ret = posix_sys_getsockopt(p, (int)a1, (int)a2, (int)a3, a4, a5); break;
    case NR_getsockname: ret = posix_sys_getsockname(p, (int)a1, a2, a3, 0); break;
    case NR_getpeername: ret = posix_sys_getsockname(p, (int)a1, a2, a3, 1); break;
    case NR_epoll_create1: ret = posix_sys_epoll_create1(p, (int)a1); break;
    case NR_epoll_ctl:     ret = posix_sys_epoll_ctl(p, (int)a1, (int)a2, (int)a3, a4); break;
    case NR_epoll_wait:
    case NR_epoll_pwait:   ret = posix_sys_epoll_wait(p, (int)a1, a2, (int)a3, (int)a4); break;
    case NR_uname: {
        struct { char sys[65], node[65], rel[65], ver[65], mach[65], dom[65]; } u;
        memset(&u, 0, sizeof(u));
        strcpy(u.sys,  "Arctian");
        strcpy(u.node, "arctian");
        strcpy(u.rel,  "1.0.0");
        strcpy(u.ver,  "Arctian OS 1.0");
        strcpy(u.mach, "x86_64");
        ret = posix_ucopy_out(a1, &u, sizeof(u));
        break;
    }
    case NR_rt_sigaction:
    case NR_rt_sigprocmask:
    case NR_sigaltstack:
    case NR_set_robust_list:
    case NR_prctl:
    case NR_rseq:
    case NR_kill:
    case NR_tkill:
    case NR_tgkill:     ret = 0; break;
    case NR_time: {
        uint32_t ux = clock_unix();
        int64_t tsec = ux ? (int64_t)ux : (int64_t)(1795400000ll + (int64_t)(clock_ms() / 1000ull));
        if (a1 && posix_ucopy_out(a1, &tsec, 8) < 0) { ret = -EFAULT; break; }
        ret = tsec;
        break;
    }
    case NR_sysinfo: {
        struct {
            int64_t  uptime;
            uint64_t loads[3];
            uint64_t totalram;
            uint64_t freeram;
            uint64_t sharedram;
            uint64_t bufferram;
            uint64_t totalswap;
            uint64_t freeswap;
            uint16_t procs;
            uint16_t pad;
            uint32_t _pad2;
            uint64_t totalhigh;
            uint64_t freehigh;
            uint32_t mem_unit;
            uint32_t _f;
        } si;
        memset(&si, 0, sizeof(si));
        vmm_meminfo_t mi;
        vmm_mem_info(&mi);
        si.uptime   = (int64_t)(clock_ms() / 1000ull);
        si.totalram = mi.total_bytes;
        si.freeram  = mi.free_bytes;
        si.procs    = 4;
        si.mem_unit = 1;
        ret = a1 ? posix_ucopy_out(a1, &si, sizeof(si)) : -EFAULT;
        break;
    }
    case NR_getrlimit: {
        uint64_t lim[2];
        posix_fill_rlimit((int)a1, lim);
        ret = a2 ? posix_ucopy_out(a2, lim, sizeof(lim)) : -EFAULT;
        break;
    }
    case NR_prlimit64: {
        uint64_t lim[2];
        posix_fill_rlimit((int)a2, lim);
        if (a4) {
            if (posix_ucopy_out(a4, lim, sizeof(lim)) < 0) { ret = -EFAULT; break; }
        }
        ret = 0;
        break;
    }
    case NR_sched_getaffinity: {
        uint32_t ncpu = percpu_online_count();
        if (ncpu == 0) ncpu = 1;
        if (ncpu > 64) ncpu = 64;
        uint64_t mask = (ncpu == 64) ? ~0ull : ((1ull << ncpu) - 1ull);
        uint64_t sz = (a2 < 8) ? a2 : 8;
        if (sz && a3) posix_ucopy_out(a3, &mask, sz);
        ret = (int64_t)sz;
        break;
    }
    /* ---- Arctian Grafik & Girdi Koprusu (500..505) ---- */
    case NR_arctian_gfx_create:  ret = posix_sys_gfx_create(p, a1, (uint32_t)a2, (uint32_t)a3, (uint32_t)a4); break;
    case NR_arctian_gfx_present: ret = posix_sys_gfx_present(p, (int)a1, (int32_t)a2, (int32_t)a3, (int32_t)a4, (int32_t)a5); break;
    case NR_arctian_gfx_poll:    ret = posix_sys_gfx_poll(p, (int)a1, a2, (int)a3); break;
    case NR_arctian_gfx_resize:  ret = posix_sys_gfx_resize(p, (int)a1, (uint32_t)a2, (uint32_t)a3); break;
    case NR_arctian_gfx_destroy: ret = posix_sys_gfx_destroy(p, (int)a1); break;
    case NR_arctian_shm_create: {
        int sid = posix_shm_create(a1);
        if (sid < 0) { ret = sid; break; }
        posix_file_t *sf = posix_file_alloc(PFD_SHM);
        if (!sf) { posix_shm_unref(sid); ret = -ENFILE; break; }
        sf->obj_id = sid;
        sf->flags = O_RDWR;
        int fd = posix_fd_install(p, sf, 0);
        if (fd < 0) posix_file_unref(sf);
        ret = fd;
        break;
    }
    default:
        ret = -ENOSYS;
        break;
    }
    regs->rax = (uint64_t)ret;
}
