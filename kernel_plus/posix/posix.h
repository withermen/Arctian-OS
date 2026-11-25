#ifndef ARCTIAN_POSIX_H
#define ARCTIAN_POSIX_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "paging.h"
#include "vmm_space.h"
#include "sched.h"
#include "sync.h"
#include "asi.h"

/* ---- Linux x86-64 errno degerleri (negatif donulur) ---- */
#define EPERM        1
#define ENOENT       2
#define ESRCH        3
#define EINTR        4
#define EIO          5
#define ENXIO        6
#define E2BIG        7
#define ENOEXEC      8
#define EBADF        9
#define ECHILD      10
#define EAGAIN      11
#define ENOMEM      12
#define EACCES      13
#define EFAULT      14
#define EBUSY       16
#define EEXIST      17
#define ENODEV      19
#define ENOTDIR     20
#define EISDIR      21
#define EINVAL      22
#define ENFILE      23
#define EMFILE      24
#define ENOTTY      25
#define ENOSPC      28
#define ESPIPE      29
#define EROFS       30
#define EPIPE       32
#define ERANGE      34
#define ENAMETOOLONG 36
#define ENOSYS      38
#define ENOTEMPTY   39
#define ENOTSOCK    88
#define EDESTADDRREQ 89
#define EMSGSIZE    90
#define EPROTOTYPE  91
#define ENOPROTOOPT 92
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP  95
#define EAFNOSUPPORT 97
#define EADDRINUSE  98
#define EADDRNOTAVAIL 99
#define ENETUNREACH 101
#define ECONNABORTED 103
#define ECONNRESET  104
#define ENOBUFS     105
#define EISCONN     106
#define ENOTCONN    107
#define ETIMEDOUT   110
#define ECONNREFUSED 111
#define EINPROGRESS 115

/* ---- Linux x86-64 syscall numaralari ---- */
#define NR_read             0
#define NR_write            1
#define NR_open             2
#define NR_close            3
#define NR_stat             4
#define NR_fstat            5
#define NR_lstat            6
#define NR_poll             7
#define NR_lseek            8
#define NR_mmap             9
#define NR_mprotect         10
#define NR_munmap           11
#define NR_brk              12
#define NR_rt_sigaction     13
#define NR_rt_sigprocmask   14
#define NR_ioctl            16
#define NR_pread64          17
#define NR_pwrite64         18
#define NR_readv            19
#define NR_writev           20
#define NR_access           21
#define NR_pipe             22
#define NR_sched_yield      24
#define NR_mremap           25
#define NR_madvise          28
#define NR_dup              32
#define NR_dup2             33
#define NR_nanosleep        35
#define NR_getpid           39
#define NR_socket           41
#define NR_connect          42
#define NR_accept           43
#define NR_sendto           44
#define NR_recvfrom         45
#define NR_sendmsg          46
#define NR_recvmsg          47
#define NR_shutdown         48
#define NR_bind             49
#define NR_listen           50
#define NR_getsockname      51
#define NR_getpeername      52
#define NR_socketpair       53
#define NR_setsockopt       54
#define NR_getsockopt       55
#define NR_clone            56
#define NR_fork             57
#define NR_vfork            58
#define NR_execve           59
#define NR_exit             60
#define NR_wait4            61
#define NR_kill             62
#define NR_uname            63
#define NR_fcntl            72
#define NR_flock            73
#define NR_fsync            74
#define NR_fdatasync        75
#define NR_ftruncate        77
#define NR_getcwd           79
#define NR_chdir            80
#define NR_rename           82
#define NR_mkdir            83
#define NR_rmdir            84
#define NR_unlink           87
#define NR_readlink         89
#define NR_chmod            90
#define NR_fchmod           91
#define NR_chown            92
#define NR_fchown           93
#define NR_lchown           94
#define NR_umask            95
#define NR_gettimeofday     96
#define NR_getrlimit        97
#define NR_sysinfo          99
#define NR_getuid           102
#define NR_getgid           104
#define NR_geteuid          107
#define NR_getegid          108
#define NR_setpgid          109
#define NR_getppid          110
#define NR_getpgrp          111
#define NR_setsid           112
#define NR_sigaltstack      131
#define NR_statfs           137
#define NR_fstatfs          138
#define NR_prctl            157
#define NR_arch_prctl       158
#define NR_gettid           186
#define NR_tkill            200
#define NR_time             201
#define NR_futex            202
#define NR_sched_getaffinity 204
#define NR_getdents64       217
#define NR_set_tid_address  218
#define NR_clock_gettime    228
#define NR_clock_getres     229
#define NR_clock_nanosleep  230
#define NR_exit_group       231
#define NR_epoll_wait       232
#define NR_epoll_ctl        233
#define NR_tgkill           234
#define NR_openat           257
#define NR_mkdirat          258
#define NR_fchownat         260
#define NR_newfstatat       262
#define NR_unlinkat         263
#define NR_renameat         264
#define NR_readlinkat       267
#define NR_fchmodat         268
#define NR_faccessat        269
#define NR_ppoll            271
#define NR_set_robust_list  273
#define NR_utimensat        280
#define NR_epoll_pwait      281
#define NR_eventfd          284
#define NR_accept4          288
#define NR_eventfd2         290
#define NR_epoll_create1    291
#define NR_dup3             292
#define NR_pipe2            293
#define NR_prlimit64        302
#define NR_renameat2        316
#define NR_getrandom        318
#define NR_memfd_create     319
#define NR_statx            332
#define NR_rseq             334
#define NR_faccessat2       439

/* Arctian Ring-3 Grafik & Girdi Koprusu syscalls (500..505) */
#define NR_arctian_gfx_create   500
#define NR_arctian_gfx_present  501
#define NR_arctian_gfx_poll     502
#define NR_arctian_gfx_resize   503
#define NR_arctian_gfx_destroy  504
#define NR_arctian_shm_create   505

/* ---- Bayraklar ---- */
#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

#define MAP_SHARED          0x01
#define MAP_PRIVATE         0x02
#define MAP_FIXED           0x10
#define MAP_ANONYMOUS       0x20
#define MAP_NORESERVE       0x4000
#define MAP_FIXED_NOREPLACE 0x100000

#define MADV_DONTNEED       4
#define MADV_FREE           8

#define MREMAP_MAYMOVE      1
#define MREMAP_FIXED        2

#define POSIX_USER_STACK_SZ_BYTES (4ull * 1024ull * 1024ull)

#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

#define CLONE_VM             0x00000100u
#define CLONE_FS             0x00000200u
#define CLONE_FILES          0x00000400u
#define CLONE_SIGHAND        0x00000800u
#define CLONE_VFORK          0x00004000u
#define CLONE_THREAD         0x00010000u
#define CLONE_SETTLS         0x00080000u
#define CLONE_PARENT_SETTID  0x00100000u
#define CLONE_CHILD_CLEARTID 0x00200000u
#define CLONE_CHILD_SETTID   0x01000000u

#define FUTEX_WAIT            0
#define FUTEX_WAKE            1
#define FUTEX_REQUEUE         3
#define FUTEX_CMP_REQUEUE     4
#define FUTEX_WAIT_BITSET     9
#define FUTEX_WAKE_BITSET     10
#define FUTEX_PRIVATE_FLAG    128
#define FUTEX_CLOCK_REALTIME  256
#define FUTEX_CMD_MASK        ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME)

#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_ACCMODE   3
#define O_CREAT     0100
#define O_EXCL      0200
#define O_TRUNC     01000
#define O_APPEND    02000
#define O_NONBLOCK  04000
#define O_DIRECTORY 0200000
#define O_CLOEXEC   02000000

#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC      1

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define AT_FDCWD            (-100)
#define AT_EMPTY_PATH       0x1000
#define AT_SYMLINK_NOFOLLOW 0x100

#define POLLIN    0x0001
#define POLLPRI   0x0002
#define POLLOUT   0x0004
#define POLLERR   0x0008
#define POLLHUP   0x0010
#define POLLNVAL  0x0020

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3
#define EPOLLIN   0x001u
#define EPOLLOUT  0x004u
#define EPOLLERR  0x008u
#define EPOLLHUP  0x010u

#define AF_UNIX   1
#define AF_INET   2
#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_NONBLOCK  04000
#define SOCK_CLOEXEC   02000000
#define SOL_SOCKET     1
#define SO_REUSEADDR   2
#define SO_TYPE        3
#define SO_ERROR       4
#define SO_KEEPALIVE   9
#define SO_RCVBUF      8
#define SO_SNDBUF      7
#define SO_PROTOCOL    38
#define SO_DOMAIN      39
#define IPPROTO_TCP    6
#define TCP_NODELAY    1
#define MSG_DONTWAIT   0x40
#define MSG_NOSIGNAL   0x4000
#define SCM_RIGHTS     1

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
#define CLOCK_BOOTTIME  7

#define WNOHANG 1

/* ---- Linux x86-64 ABI yapilari ---- */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed)) posix_regs_t;

typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} posix_timespec_t;

typedef struct {
    void    *iov_base;
    uint64_t iov_len;
} posix_iovec_t;

typedef struct {
    int32_t fd;
    int16_t events;
    int16_t revents;
} posix_pollfd_t;

typedef struct {
    uint32_t events;
    uint64_t data;
} __attribute__((packed)) posix_epoll_event_t;

/* Linux x86-64 struct stat (144 bayt) */
typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    int64_t  st_atime;
    int64_t  st_atime_nsec;
    int64_t  st_mtime;
    int64_t  st_mtime_nsec;
    int64_t  st_ctime;
    int64_t  st_ctime_nsec;
    int64_t  __unused[3];
} posix_stat_t;

#define S_IFMT   0170000u
#define S_IFIFO  0010000u
#define S_IFCHR  0020000u
#define S_IFDIR  0040000u
#define S_IFREG  0100000u
#define S_IFSOCK 0140000u

typedef struct {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t  sin_zero[8];
} __attribute__((packed)) posix_sockaddr_in_t;

typedef struct {
    uint16_t sun_family;
    char     sun_path[108];
} __attribute__((packed)) posix_sockaddr_un_t;

typedef struct {
    void     *msg_name;
    uint32_t  msg_namelen;
    uint32_t  _pad0;
    posix_iovec_t *msg_iov;
    uint64_t  msg_iovlen;
    void     *msg_control;
    uint64_t  msg_controllen;
    int32_t   msg_flags;
    uint32_t  _pad1;
} posix_msghdr_t;

typedef struct {
    uint64_t cmsg_len;
    int32_t  cmsg_level;
    int32_t  cmsg_type;
} posix_cmsghdr_t;

/* ---- Cekirdek POSIX nesneleri ---- */
#define POSIX_MAX_PROCS     2048  /* dinamik kmalloc dilimleriyle sinirsiz/RAM-olcekli */
#define POSIX_MAX_THREADS   8192  /* dinamik kmalloc dilimleriyle sinirsiz/RAM-olcekli */
#define POSIX_MAX_FDS       256
#define POSIX_MAX_VMAS      4096
#define POSIX_MAX_FILES     1024
#define POSIX_MAX_PIPES     128
#define POSIX_MAX_EPOLLS    32
#define POSIX_MAX_EPOLL_ENT 128
#define POSIX_MAX_SHMS      256
#define POSIX_MAX_EFDS      64
#define POSIX_MAX_USOCKS    128
#define POSIX_MAX_ISOCKS    128
#define POSIX_GFX_MAX_WINS  1024  /* dinamik kmalloc dilimleriyle sinirsiz/RAM-olcekli */

/* VMA */
typedef struct {
    uint64_t start;
    uint64_t end;
    uint32_t prot;
    uint32_t flags;
    int32_t  shm_id;     /* >=0 ise SHM / GFX paylasimli nesnesi */
    int32_t  used;
} posix_vma_t;

/* Dosya aciklamasi turleri */
enum {
    PFD_NONE = 0,
    PFD_CONSOLE,
    PFD_DEVNULL,
    PFD_URANDOM,
    PFD_VFILE,     /* sanal dosya (/etc/resolv.conf, /etc/hosts) */
    PFD_AFS,
    PFD_PIPE_R,
    PFD_PIPE_W,
    PFD_SOCK_INET,
    PFD_SOCK_UNIX,
    PFD_EPOLL,
    PFD_SHM,
    PFD_GFX,
    PFD_EVENTFD
};

typedef struct {
    int      kind;
    int      refs;
    uint32_t flags;          /* O_NONBLOCK, O_APPEND, O_ACCMODE ... */
    uint64_t offset;
    int      obj_id;         /* pipe / sock / epoll / shm / gfx indeksi */
    void    *afs_handle;
    char     path[192];
    int      is_dir;
    const char *vdata;       /* PFD_VFILE icerigi */
    uint32_t vlen;
} posix_file_t;

typedef struct {
    posix_file_t *file;
    uint32_t      fd_flags;  /* FD_CLOEXEC */
} posix_fd_ent_t;

typedef struct posix_proc posix_proc_t;
typedef struct posix_thr  posix_thr_t;

struct posix_proc {
    int          used;
    int          pid;
    int          ppid;
    volatile int state;      /* 0=unused, 1=alive, 2=zombie */
    int          exit_code;
    int          nthreads;
    spinlock_t   lock;
    vmm_space_t  space;
    uint64_t     brk_base;
    uint64_t     brk_cur;
    uint64_t     mmap_next;
    posix_vma_t *vmas;       /* dinamik heap dizisi (vma_cap = POSIX_MAX_VMAS) */
    int          vma_cap;
    posix_fd_ent_t fds[POSIX_MAX_FDS];
    char         cwd[192];
    char         exe_path[192];
    char         name[48];
    waitq_t      wait_wq;    /* wait4 bekleyen ebeveynler */
};

struct posix_thr {
    int           used;
    int           tid;
    sched_tid_t   sched_tid;
    posix_proc_t *proc;
    uint64_t      fs_base;
    uint64_t      clear_child_tid;  /* CLONE_CHILD_CLEARTID adresi */
    posix_regs_t  init_regs;        /* fork/clone/exec ilk giris durumu */
};

/* ---- Alt sistem API'leri ---- */
void posix_init(void);
void posix_syscall_enable_cpu(void);
void posix_enter_user(posix_regs_t *regs) __attribute__((noreturn));

/* Kullanici bellegi dogrulama yardimcilari */
int posix_uprobe(uint64_t va, uint64_t len, int write);
int posix_ucopy_in(void *kdst, uint64_t usrc, uint64_t len);
int posix_ucopy_out(uint64_t udst, const void *ksrc, uint64_t len);
int posix_ucstr_in(char *kdst, uint64_t cap, uint64_t usrc);

/* VM (posix_vm.c) */
int      posix_vm_init_proc(posix_proc_t *p);
void     posix_vm_destroy_proc(posix_proc_t *p);
int      posix_vm_clone(posix_proc_t *dst, posix_proc_t *src);
int      posix_vm_add_vma(posix_proc_t *p, uint64_t start, uint64_t end,
                          uint32_t prot, uint32_t flags, int shm_id);
uint64_t posix_vm_find_free(posix_proc_t *p, uint64_t len);
int      posix_vm_fault_page(posix_proc_t *p, uint64_t va, int is_write, int is_exec);
void     posix_vm_dump_fault(posix_proc_t *p, uint64_t va);
int      posix_pf_handler(uint64_t cr2, uint64_t err, uint64_t rip);
int64_t  posix_sys_mmap(posix_proc_t *p, uint64_t addr, uint64_t len,
                        uint32_t prot, uint32_t flags, int fd, uint64_t off);
int64_t  posix_sys_munmap(posix_proc_t *p, uint64_t addr, uint64_t len);
int64_t  posix_sys_mprotect(posix_proc_t *p, uint64_t addr, uint64_t len, uint32_t prot);
int64_t  posix_sys_madvise(posix_proc_t *p, uint64_t addr, uint64_t len, int advice);
int64_t  posix_sys_mremap(posix_proc_t *p, uint64_t old_addr, uint64_t old_len,
                          uint64_t new_len, uint32_t flags, uint64_t new_addr);
int64_t  posix_sys_brk(posix_proc_t *p, uint64_t addr);

/* SHM (posix_vm.c) */
int      posix_shm_create(uint64_t size);
int      posix_shm_ref(int shm_id);
void     posix_shm_unref(int shm_id);
uint64_t posix_shm_size(int shm_id);
uint64_t posix_shm_frame(int shm_id, uint64_t page_idx);
int      posix_shm_resize(int shm_id, uint64_t new_size);

/* ELF64 (posix_elf.c) */
typedef struct {
    uint64_t entry;
    uint64_t user_rsp;
    uint64_t brk_base;
} posix_elf_info_t;

int posix_elf_load(posix_proc_t *p, const uint8_t *img, uint64_t size,
                   char *const argv[], int argc,
                   char *const envp[], int envc,
                   posix_elf_info_t *out);
int posix_elf_load_afs(posix_proc_t *p, void *afs_handle, uint64_t size,
                       char *const argv[], int argc,
                       char *const envp[], int envc,
                       posix_elf_info_t *out);

/* Proc & Thread (posix_proc.c) */
posix_thr_t  *posix_cur_thr(void);
posix_proc_t *posix_cur_proc(void);
int           posix_fault_handler(int int_no, uint64_t err, uint64_t cr2, uint64_t rip);
int           posix_spawn_elf_mem(const char *name, const uint8_t *elf, uint64_t size,
                                  char *const argv[], int argc);
int           posix_spawn_elf_path(const char *path, char *const argv[], int argc);
int           posix_wait_pid_kernel(int pid, int *exit_code_out);

/* Unified OOM kancalari (kernel_plus/system/proc.c zayif sembollerini doldurur) */
int           posix_oom_list_cb(asi_oom_app_t *out, int max_out);
int           posix_oom_kill_cb(int pid);
void          posix_oom_freeze_cb(int freeze);
int64_t       posix_sys_fork(posix_regs_t *regs);
int64_t       posix_sys_fork_ex(posix_regs_t *regs, uint64_t child_stack, uint64_t flags,
                                uint64_t ptid, uint64_t ctid);
int64_t       posix_sys_clone(posix_regs_t *regs, uint64_t flags, uint64_t child_stack,
                              uint64_t ptid, uint64_t ctid, uint64_t newtls);
int64_t       posix_sys_execve(posix_regs_t *regs, uint64_t path_va,
                               uint64_t argv_va, uint64_t envp_va);
int64_t       posix_sys_wait4(int pid, uint64_t status_va, int options);
void          posix_sys_exit(int code, int group) __attribute__((noreturn));
int64_t       posix_sys_arch_prctl(int code, uint64_t addr);
int64_t       posix_sys_futex(uint64_t uaddr, int op, uint32_t val,
                              uint64_t timeout_va, uint64_t uaddr2, uint32_t val3);
void          posix_futex_wake_phys(uint64_t phys, int count);

/* FD / VFS / Pipe / Poll / Epoll / Time (posix_fd.c) */
void          posix_fd_init_subsys(void);
void          posix_fd_init_stdio(posix_proc_t *p);
void          posix_fd_clone_table(posix_proc_t *dst, posix_proc_t *src);
void          posix_fd_close_all(posix_proc_t *p, int cloexec_only);
posix_file_t *posix_file_alloc(int kind);
void          posix_file_ref(posix_file_t *f);
void          posix_file_unref(posix_file_t *f);
int           posix_fd_install(posix_proc_t *p, posix_file_t *f, uint32_t fd_flags);
posix_file_t *posix_fd_get(posix_proc_t *p, int fd);
uint32_t      posix_file_poll_mask(posix_file_t *f);

int64_t posix_sys_openat(posix_proc_t *p, int dirfd, uint64_t path_va, int flags, uint32_t mode);
int64_t posix_sys_close(posix_proc_t *p, int fd);
int64_t posix_sys_read(posix_proc_t *p, int fd, uint64_t buf_va, uint64_t len);
int64_t posix_sys_write(posix_proc_t *p, int fd, uint64_t buf_va, uint64_t len);
int64_t posix_sys_readv(posix_proc_t *p, int fd, uint64_t iov_va, int iovcnt);
int64_t posix_sys_writev(posix_proc_t *p, int fd, uint64_t iov_va, int iovcnt);
int64_t posix_sys_lseek(posix_proc_t *p, int fd, int64_t off, int whence);
int64_t posix_sys_fstat(posix_proc_t *p, int fd, uint64_t st_va);
int64_t posix_sys_newfstatat(posix_proc_t *p, int dirfd, uint64_t path_va, uint64_t st_va, int flags);
int64_t posix_sys_pipe2(posix_proc_t *p, uint64_t fds_va, int flags);
int64_t posix_sys_dup(posix_proc_t *p, int oldfd);
int64_t posix_sys_dup3(posix_proc_t *p, int oldfd, int newfd, int flags);
int64_t posix_sys_fcntl(posix_proc_t *p, int fd, int cmd, uint64_t arg);
int64_t posix_sys_poll(posix_proc_t *p, uint64_t fds_va, uint64_t nfds, int timeout_ms);
int64_t posix_sys_epoll_create1(posix_proc_t *p, int flags);
int64_t posix_sys_epoll_ctl(posix_proc_t *p, int epfd, int op, int fd, uint64_t ev_va);
int64_t posix_sys_epoll_wait(posix_proc_t *p, int epfd, uint64_t evs_va, int maxev, int timeout_ms);
uint64_t posix_realtime_ms(void);
int64_t posix_sys_clock_gettime(int clk_id, uint64_t ts_va);
int64_t posix_sys_nanosleep(uint64_t req_va, uint64_t rem_va);
int64_t posix_sys_getrandom(uint64_t buf_va, uint64_t len, uint32_t flags);
int64_t posix_sys_memfd_create(posix_proc_t *p, uint64_t name_va, uint32_t flags);
int64_t posix_sys_ftruncate(posix_proc_t *p, int fd, int64_t len);
int64_t posix_sys_getcwd(posix_proc_t *p, uint64_t buf_va, uint64_t size);
int64_t posix_sys_chdir(posix_proc_t *p, uint64_t path_va);
int64_t posix_sys_mkdirat(posix_proc_t *p, int dirfd, uint64_t path_va, uint32_t mode);
int64_t posix_sys_unlinkat(posix_proc_t *p, int dirfd, uint64_t path_va, int flags);
int64_t posix_sys_rename(posix_proc_t *p, uint64_t old_va, uint64_t new_va);
int64_t posix_sys_getdents64(posix_proc_t *p, int fd, uint64_t dirp_va, uint64_t count);
int64_t posix_sys_access(posix_proc_t *p, int dirfd, uint64_t path_va, int mode);
int64_t posix_sys_readlinkat(posix_proc_t *p, int dirfd, uint64_t path_va, uint64_t buf_va, uint64_t bufsiz);
int64_t posix_sys_eventfd2(posix_proc_t *p, uint32_t initval, int flags);
int64_t posix_sys_statfs(uint64_t buf_va);
int64_t posix_sys_ioctl(posix_proc_t *p, int fd, uint64_t req, uint64_t arg);

/* Soketler (posix_sock.c) */
void     posix_sock_init(void);
void     posix_sock_close_obj(int kind, int obj_id);
uint32_t posix_sock_poll_mask(int kind, int obj_id);
int64_t  posix_sock_read(posix_file_t *f, uint8_t *buf, uint64_t len, int flags);
int64_t  posix_sock_write(posix_file_t *f, const uint8_t *buf, uint64_t len, int flags);
int64_t  posix_sys_socket(posix_proc_t *p, int domain, int type, int proto);
int64_t  posix_sys_socketpair(posix_proc_t *p, int domain, int type, int proto, uint64_t sv_va);
int64_t  posix_sys_bind(posix_proc_t *p, int fd, uint64_t addr_va, uint32_t addrlen);
int64_t  posix_sys_listen(posix_proc_t *p, int fd, int backlog);
int64_t  posix_sys_accept4(posix_proc_t *p, int fd, uint64_t addr_va, uint64_t len_va, int flags);
int64_t  posix_sys_connect(posix_proc_t *p, int fd, uint64_t addr_va, uint32_t addrlen);
int64_t  posix_sys_sendto(posix_proc_t *p, int fd, uint64_t buf_va, uint64_t len,
                          int flags, uint64_t addr_va, uint32_t addrlen);
int64_t  posix_sys_recvfrom(posix_proc_t *p, int fd, uint64_t buf_va, uint64_t len,
                            int flags, uint64_t addr_va, uint64_t len_va);
int64_t  posix_sys_sendmsg(posix_proc_t *p, int fd, uint64_t msg_va, int flags);
int64_t  posix_sys_recvmsg(posix_proc_t *p, int fd, uint64_t msg_va, int flags);
int64_t  posix_sys_shutdown(posix_proc_t *p, int fd, int how);
int64_t  posix_sys_setsockopt(posix_proc_t *p, int fd, int level, int optname,
                              uint64_t optval_va, uint32_t optlen);
int64_t  posix_sys_getsockopt(posix_proc_t *p, int fd, int level, int optname,
                              uint64_t optval_va, uint64_t optlen_va);
int64_t  posix_sys_getsockname(posix_proc_t *p, int fd, uint64_t addr_va, uint64_t len_va, int peer);

/* Grafik & Girdi Koprusu (posix_gfx.c) */
#include "posix_gfx.h"
void     posix_gfx_init(void);
void     posix_gfx_close_obj(int obj_id);
uint32_t posix_gfx_poll_mask(int obj_id);
int      posix_gfx_shm_id(int obj_id);
int64_t  posix_gfx_read_events(int obj_id, uint8_t *dst, uint64_t len, int nonblock);
int64_t  posix_sys_gfx_create(posix_proc_t *p, uint64_t title_va, uint32_t w, uint32_t h, uint32_t flags);
int64_t  posix_sys_gfx_present(posix_proc_t *p, int fd, int32_t x, int32_t y, int32_t w, int32_t h);
int64_t  posix_sys_gfx_poll(posix_proc_t *p, int fd, uint64_t ev_va, int timeout_ms);
int64_t  posix_sys_gfx_resize(posix_proc_t *p, int fd, uint32_t w, uint32_t h);
int64_t  posix_sys_gfx_destroy(posix_proc_t *p, int fd);
void     posix_gfx_desktop_drain(void);
void     posix_gfx_register_desktop(
             void *(*on_open)(int win_id, const char *title, uint32_t w, uint32_t h),
             void  (*on_blit)(void *handle, int win_id, int32_t x, int32_t y, int32_t w, int32_t h),
             void  (*on_close)(void *handle));
int      posix_gfx_copy_rect(int win_id, uint32_t *dst_argb, uint32_t dst_stride_px,
                             int32_t x, int32_t y, int32_t w, int32_t h);
void     posix_gfx_push_event(int win_id, const arctian_gfx_event_t *ev);

#endif /* ARCTIAN_POSIX_H */
