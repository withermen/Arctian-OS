/* POSIX Dosya Tablosu, Boru (pipe), poll/epoll, Zaman ve getrandom.
 *
 * Sanal dosyalar:
 *   /etc/resolv.conf  -> aktif DNS sunucusu (musl getaddrinfo icin!)
 *   /etc/hosts        -> 127.0.0.1 localhost
 *   /dev/null         -> EOF / yutucu
 *   /dev/urandom      -> net_rand32() entropi akisi
 */
#include "posix.h"
#include "alloc.h"
#include "afs.h"
#include "clock.h"
#include "rtc.h"
#include "net.h"
#include "ipv4.h"
#include "serial.h"
#include <string.h>

/* ---- Dosya aciklama havuzu ---- */
static posix_file_t s_files[POSIX_MAX_FILES];
static spinlock_t   s_file_lock;

/* ---- Borular (pipe) ---- */
#define PPIPE_CAP 16384u
typedef struct {
    int        used;
    spinlock_t lock;
    uint8_t   *buf;
    uint32_t   head;
    uint32_t   tail;
    uint32_t   count;
    int        readers;
    int        writers;
    waitq_t    rd_wq;
    waitq_t    wr_wq;
} ppipe_t;

static ppipe_t    s_pipes[POSIX_MAX_PIPES];
static spinlock_t s_pipe_tab_lock;

/* ---- Epoll ornekleri ---- */
typedef struct {
    int      used;
    int      fd;
    uint32_t events;
    uint64_t data;
} pepoll_ent_t;

typedef struct {
    int          used;
    spinlock_t   lock;
    pepoll_ent_t ents[POSIX_MAX_EPOLL_ENT];
} pepoll_t;

static pepoll_t   s_epolls[POSIX_MAX_EPOLLS];
static spinlock_t s_epoll_tab_lock;

static char s_resolv_buf[128];
static const char s_hosts_buf[] = "127.0.0.1 localhost\n";

void posix_fd_init_subsys(void) {
    memset(s_files, 0, sizeof(s_files));
    memset(s_pipes, 0, sizeof(s_pipes));
    memset(s_epolls, 0, sizeof(s_epolls));
    spin_init(&s_file_lock);
    spin_init(&s_pipe_tab_lock);
    spin_init(&s_epoll_tab_lock);
}

posix_file_t *posix_file_alloc(int kind) {
    uint64_t fl = sched_irq_save();
    spin_lock(&s_file_lock);
    posix_file_t *f = 0;
    for (int i = 0; i < POSIX_MAX_FILES; i++) {
        if (s_files[i].kind == PFD_NONE) {
            memset(&s_files[i], 0, sizeof(s_files[i]));
            s_files[i].kind = kind;
            s_files[i].refs = 1;
            s_files[i].obj_id = -1;
            f = &s_files[i];
            break;
        }
    }
    spin_unlock(&s_file_lock);
    sched_irq_restore(fl);
    return f;
}

void posix_file_ref(posix_file_t *f) {
    if (!f) return;
    uint64_t fl = sched_irq_save();
    spin_lock(&s_file_lock);
    f->refs++;
    spin_unlock(&s_file_lock);
    sched_irq_restore(fl);
}

static void pipe_end_close(int id, int is_write) {
    if (id < 0 || id >= POSIX_MAX_PIPES) return;
    ppipe_t *pp = &s_pipes[id];
    sched_tid_t wakes[WAITQ_MAX * 2];
    int wn = 0;
    uint8_t *free_buf = 0;

    uint64_t fl = sched_irq_save();
    spin_lock(&pp->lock);
    if (is_write) { if (pp->writers > 0) pp->writers--; }
    else          { if (pp->readers > 0) pp->readers--; }
    int w;
    while ((w = waitq_pop(&pp->rd_wq)) >= 0 && wn < WAITQ_MAX * 2) wakes[wn++] = w;
    while ((w = waitq_pop(&pp->wr_wq)) >= 0 && wn < WAITQ_MAX * 2) wakes[wn++] = w;
    if (pp->readers == 0 && pp->writers == 0) {
        free_buf = pp->buf;
        pp->buf = 0;
        pp->used = 0;
    }
    spin_unlock(&pp->lock);
    sched_irq_restore(fl);

    for (int i = 0; i < wn; i++) sched_wake(wakes[i]);
    if (free_buf) kfree(free_buf);
}

void posix_file_unref(posix_file_t *f) {
    if (!f) return;
    int kind = PFD_NONE;
    int obj_id = -1;
    uint64_t fl = sched_irq_save();
    spin_lock(&s_file_lock);
    if (--f->refs <= 0) {
        kind = f->kind;
        obj_id = f->obj_id;
        f->kind = PFD_NONE;
        f->refs = 0;
    }
    spin_unlock(&s_file_lock);
    sched_irq_restore(fl);

    if (kind == PFD_PIPE_R) pipe_end_close(obj_id, 0);
    else if (kind == PFD_PIPE_W) pipe_end_close(obj_id, 1);
    else if (kind == PFD_SOCK_INET || kind == PFD_SOCK_UNIX)
        posix_sock_close_obj(kind, obj_id);
    else if (kind == PFD_EPOLL && obj_id >= 0 && obj_id < POSIX_MAX_EPOLLS)
        s_epolls[obj_id].used = 0;
    else if (kind == PFD_SHM)
        posix_shm_unref(obj_id);
    else if (kind == PFD_GFX)
        posix_gfx_close_obj(obj_id);
}

int posix_fd_install(posix_proc_t *p, posix_file_t *f, uint32_t fd_flags) {
    if (!p || !f) return -EINVAL;
    uint64_t fl = sched_irq_save();
    spin_lock(&p->lock);
    int fd = -EMFILE;
    for (int i = 0; i < POSIX_MAX_FDS; i++) {
        if (!p->fds[i].file) {
            p->fds[i].file = f;
            p->fds[i].fd_flags = fd_flags;
            fd = i;
            break;
        }
    }
    spin_unlock(&p->lock);
    sched_irq_restore(fl);
    return fd;
}

posix_file_t *posix_fd_get(posix_proc_t *p, int fd) {
    if (!p || fd < 0 || fd >= POSIX_MAX_FDS) return 0;
    return p->fds[fd].file;
}

void posix_fd_init_stdio(posix_proc_t *p) {
    for (int i = 0; i < 3; i++) {
        posix_file_t *f = posix_file_alloc(PFD_CONSOLE);
        if (f) {
            f->flags = (i == 0) ? O_RDONLY : O_WRONLY;
            p->fds[i].file = f;
            p->fds[i].fd_flags = 0;
        }
    }
}

void posix_fd_clone_table(posix_proc_t *dst, posix_proc_t *src) {
    uint64_t fl = sched_irq_save();
    spin_lock(&src->lock);
    for (int i = 0; i < POSIX_MAX_FDS; i++) {
        dst->fds[i].file = src->fds[i].file;
        dst->fds[i].fd_flags = src->fds[i].fd_flags;
        if (dst->fds[i].file) posix_file_ref(dst->fds[i].file);
    }
    spin_unlock(&src->lock);
    sched_irq_restore(fl);
}

void posix_fd_close_all(posix_proc_t *p, int cloexec_only) {
    posix_file_t *to_drop[POSIX_MAX_FDS];
    int dn = 0;
    uint64_t fl = sched_irq_save();
    spin_lock(&p->lock);
    for (int i = 0; i < POSIX_MAX_FDS; i++) {
        if (!p->fds[i].file) continue;
        if (cloexec_only && !(p->fds[i].fd_flags & FD_CLOEXEC)) continue;
        to_drop[dn++] = p->fds[i].file;
        p->fds[i].file = 0;
        p->fds[i].fd_flags = 0;
    }
    spin_unlock(&p->lock);
    sched_irq_restore(fl);
    for (int i = 0; i < dn; i++) posix_file_unref(to_drop[i]);
}

/* Yol normalizasyonu: goreli yolu cwd ile birlestir */
static void resolve_path(posix_proc_t *p, const char *in, char *out, size_t cap) {
    if (!in || !in[0]) { strncpy(out, "/", cap); out[cap - 1] = 0; return; }
    if (in[0] == '/' || (in[0] && in[1] == ':')) {
        strncpy(out, in, cap - 1);
        out[cap - 1] = 0;
        return;
    }
    size_t cl = strlen(p->cwd);
    size_t pos = 0;
    for (size_t i = 0; i < cl && pos + 1 < cap; i++) out[pos++] = p->cwd[i];
    if (pos > 0 && out[pos - 1] != '/' && pos + 1 < cap) out[pos++] = '/';
    for (size_t i = 0; in[i] && pos + 1 < cap; i++) out[pos++] = in[i];
    out[pos] = 0;
}

static uint32_t afs_handle_size(void *h) {
    if (!h) return 0;
    asi_stat_t st;
    memset(&st, 0, sizeof(st));
    if (afs_stat_handle(h, &st) != 0) return 0;
    return st.size;
}

static void *afs_lookup_path(const char *path) {
    void *h = afs_find_handle(path);
    if (h) return h;
    const char *p = (path[0] == '/') ? path + 1 : path;
    h = afs_find_handle(p);
    if (h) return h;
    char cpath[208];
    cpath[0] = 'C'; cpath[1] = ':'; cpath[2] = '/';
    size_t n = strlen(p);
    if (n + 4 > sizeof(cpath)) return 0;
    memcpy(cpath + 3, p, n + 1);
    return afs_find_handle(cpath);
}

static void build_resolv_conf(void) {
    uint8_t *d = ipv4_config.dns_server;
    int n = 0;
    const char *pre = "nameserver ";
    while (*pre) s_resolv_buf[n++] = *pre++;
    for (int i = 0; i < 4; i++) {
        uint8_t v = d[i];
        char tmp[4]; int ti = 0;
        if (v == 0) tmp[ti++] = '0';
        else { while (v) { tmp[ti++] = (char)('0' + (v % 10)); v /= 10; } }
        while (ti--) s_resolv_buf[n++] = tmp[ti];
        if (i < 3) s_resolv_buf[n++] = '.';
    }
    s_resolv_buf[n++] = '\n';
    s_resolv_buf[n] = 0;
}

/* ---- openat / close ---- */
int64_t posix_sys_openat(posix_proc_t *p, int dirfd, uint64_t path_va, int flags, uint32_t mode) {
    (void)dirfd; (void)mode;
    char raw[192], path[192];
    if (posix_ucstr_in(raw, sizeof(raw), path_va) < 0) return -EFAULT;
    resolve_path(p, raw, path, sizeof(path));

    uint32_t fd_flags = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;

    if (strcmp(path, "/dev/null") == 0) {
        posix_file_t *f = posix_file_alloc(PFD_DEVNULL);
        if (!f) return -ENFILE;
        f->flags = (uint32_t)flags;
        int fd = posix_fd_install(p, f, fd_flags);
        if (fd < 0) posix_file_unref(f);
        return fd;
    }
    if (strcmp(path, "/dev/urandom") == 0 || strcmp(path, "/dev/random") == 0) {
        posix_file_t *f = posix_file_alloc(PFD_URANDOM);
        if (!f) return -ENFILE;
        f->flags = (uint32_t)flags;
        int fd = posix_fd_install(p, f, fd_flags);
        if (fd < 0) posix_file_unref(f);
        return fd;
    }
    if (strcmp(path, "/etc/resolv.conf") == 0) {
        build_resolv_conf();
        posix_file_t *f = posix_file_alloc(PFD_VFILE);
        if (!f) return -ENFILE;
        f->flags = (uint32_t)flags;
        f->vdata = s_resolv_buf;
        f->vlen  = (uint32_t)strlen(s_resolv_buf);
        strncpy(f->path, path, sizeof(f->path) - 1);
        int fd = posix_fd_install(p, f, fd_flags);
        if (fd < 0) posix_file_unref(f);
        return fd;
    }
    if (strcmp(path, "/etc/hosts") == 0) {
        posix_file_t *f = posix_file_alloc(PFD_VFILE);
        if (!f) return -ENFILE;
        f->flags = (uint32_t)flags;
        f->vdata = s_hosts_buf;
        f->vlen  = (uint32_t)(sizeof(s_hosts_buf) - 1);
        strncpy(f->path, path, sizeof(f->path) - 1);
        int fd = posix_fd_install(p, f, fd_flags);
        if (fd < 0) posix_file_unref(f);
        return fd;
    }

    void *h = afs_lookup_path(path);
    if (!h && (flags & O_CREAT)) {
        const char *ap = (path[0] == '/') ? path + 1 : path;
        h = afs_create_handle(ap);
    }
    if (!h) return -ENOENT;
    if ((flags & O_TRUNC) && ((flags & O_ACCMODE) != O_RDONLY)) {
        afs_write_handle(h, "", 0);
    }

    posix_file_t *f = posix_file_alloc(PFD_AFS);
    if (!f) return -ENFILE;
    f->flags = (uint32_t)flags;
    f->afs_handle = h;
    f->offset = (flags & O_APPEND) ? afs_handle_size(h) : 0;
    strncpy(f->path, path, sizeof(f->path) - 1);

    int fd = posix_fd_install(p, f, fd_flags);
    if (fd < 0) posix_file_unref(f);
    return fd;
}

int64_t posix_sys_close(posix_proc_t *p, int fd) {
    if (!p || fd < 0 || fd >= POSIX_MAX_FDS) return -EBADF;
    uint64_t fl = sched_irq_save();
    spin_lock(&p->lock);
    posix_file_t *f = p->fds[fd].file;
    if (!f) { spin_unlock(&p->lock); sched_irq_restore(fl); return -EBADF; }
    p->fds[fd].file = 0;
    p->fds[fd].fd_flags = 0;
    spin_unlock(&p->lock);
    sched_irq_restore(fl);
    posix_file_unref(f);
    return 0;
}

/* ---- Pipe okuma / yazma ---- */
static int64_t pipe_do_read(posix_file_t *f, uint8_t *dst, uint64_t len) {
    int id = f->obj_id;
    if (id < 0 || id >= POSIX_MAX_PIPES) return -EIO;
    ppipe_t *pp = &s_pipes[id];

    for (;;) {
        uint64_t fl = sched_irq_save();
        spin_lock(&pp->lock);
        if (pp->count > 0) {
            uint64_t take = (len < pp->count) ? len : pp->count;
            for (uint64_t i = 0; i < take; i++) {
                dst[i] = pp->buf[pp->head];
                pp->head = (pp->head + 1u) % PPIPE_CAP;
            }
            pp->count -= (uint32_t)take;
            int w = waitq_pop(&pp->wr_wq);
            spin_unlock(&pp->lock);
            if (w >= 0) sched_wake(w);
            sched_irq_restore(fl);
            return (int64_t)take;
        }
        if (pp->writers == 0) {
            spin_unlock(&pp->lock);
            sched_irq_restore(fl);
            return 0;  /* EOF */
        }
        if (f->flags & O_NONBLOCK) {
            spin_unlock(&pp->lock);
            sched_irq_restore(fl);
            return -EAGAIN;
        }
        waitq_add(&pp->rd_wq, sched_current());
        sched_prepare_block();
        spin_unlock(&pp->lock);
        sched_commit_block();
        sched_irq_restore(fl);
    }
}

static int64_t pipe_do_write(posix_file_t *f, const uint8_t *src, uint64_t len) {
    int id = f->obj_id;
    if (id < 0 || id >= POSIX_MAX_PIPES) return -EIO;
    ppipe_t *pp = &s_pipes[id];
    uint64_t done = 0;

    while (done < len) {
        uint64_t fl = sched_irq_save();
        spin_lock(&pp->lock);
        if (pp->readers == 0) {
            spin_unlock(&pp->lock);
            sched_irq_restore(fl);
            return done ? (int64_t)done : -EPIPE;
        }
        uint32_t space = PPIPE_CAP - pp->count;
        if (space > 0) {
            uint64_t chunk = len - done;
            if (chunk > space) chunk = space;
            for (uint64_t i = 0; i < chunk; i++) {
                pp->buf[pp->tail] = src[done + i];
                pp->tail = (pp->tail + 1u) % PPIPE_CAP;
            }
            pp->count += (uint32_t)chunk;
            done += chunk;
            int w = waitq_pop(&pp->rd_wq);
            spin_unlock(&pp->lock);
            if (w >= 0) sched_wake(w);
            sched_irq_restore(fl);
            if (f->flags & O_NONBLOCK) break;
            continue;
        }
        if (f->flags & O_NONBLOCK) {
            spin_unlock(&pp->lock);
            sched_irq_restore(fl);
            return done ? (int64_t)done : -EAGAIN;
        }
        waitq_add(&pp->wr_wq, sched_current());
        sched_prepare_block();
        spin_unlock(&pp->lock);
        sched_commit_block();
        sched_irq_restore(fl);
    }
    return (int64_t)done;
}

/* ---- read / write / readv / writev ---- */
static int64_t file_read_kbuf(posix_file_t *f, uint8_t *kbuf, uint64_t len) {
    if (len == 0) return 0;
    switch (f->kind) {
    case PFD_DEVNULL: return 0;
    case PFD_CONSOLE: return 0;
    case PFD_URANDOM:
        for (uint64_t i = 0; i < len; i += 4) {
            uint32_t r = net_rand32();
            uint64_t c = (len - i < 4) ? (len - i) : 4;
            memcpy(kbuf + i, &r, (size_t)c);
        }
        return (int64_t)len;
    case PFD_VFILE:
        if (!f->vdata || f->offset >= f->vlen) return 0;
        {
            uint64_t avail = f->vlen - f->offset;
            if (avail > len) avail = len;
            memcpy(kbuf, f->vdata + f->offset, (size_t)avail);
            f->offset += avail;
            return (int64_t)avail;
        }
    case PFD_AFS: {
        int n = afs_read_at(f->afs_handle, kbuf, (uint32_t)len, (uint32_t)f->offset);
        if (n > 0) f->offset += (uint64_t)n;
        return n < 0 ? -EIO : (int64_t)n;
    }
    case PFD_PIPE_R:
        return pipe_do_read(f, kbuf, len);
    case PFD_SOCK_INET:
    case PFD_SOCK_UNIX:
        return posix_sock_read(f, kbuf, len, 0);
    case PFD_GFX:
        return posix_gfx_read_events(f->obj_id, kbuf, len, (f->flags & O_NONBLOCK) ? 1 : 0);
    case PFD_SHM: {
        uint64_t sz = posix_shm_size(f->obj_id);
        if (f->offset >= sz) return 0;
        uint64_t avail = sz - f->offset;
        if (avail > len) avail = len;
        uint64_t done = 0;
        while (done < avail) {
            uint64_t pos = f->offset + done;
            uint64_t pa = posix_shm_frame(f->obj_id, pos >> 12);
            if (!pa) break;
            uint64_t off = pos & 0xFFFull;
            uint64_t ch = 0x1000ull - off;
            if (ch > avail - done) ch = avail - done;
            memcpy(kbuf + done, (const void *)(uintptr_t)(pa + off), (size_t)ch);
            done += ch;
        }
        f->offset += done;
        return (int64_t)done;
    }
    default:
        return -EBADF;
    }
}

static int64_t file_write_kbuf(posix_file_t *f, const uint8_t *kbuf, uint64_t len) {
    if (len == 0) return 0;
    switch (f->kind) {
    case PFD_DEVNULL: return (int64_t)len;
    case PFD_CONSOLE:
        for (uint64_t i = 0; i < len; i++) serial_putc((char)kbuf[i]);
        return (int64_t)len;
    case PFD_AFS: {
        /* AFS yazma: mevcut boyu okuyup offset'e yaz veya ustune yaz */
        uint32_t cur_sz = afs_handle_size(f->afs_handle);
        if (f->flags & O_APPEND) f->offset = cur_sz;
        uint64_t end = f->offset + len;
        if (end > (4ull * 1024ull * 1024ull)) return -ENOSPC;
        uint32_t nsz = (end > cur_sz) ? (uint32_t)end : cur_sz;
        uint8_t *tmp = (uint8_t *)kmalloc(nsz ? nsz : 1);
        if (!tmp) return -ENOMEM;
        memset(tmp, 0, nsz);
        if (cur_sz > 0) afs_read_at(f->afs_handle, tmp, cur_sz, 0);
        memcpy(tmp + f->offset, kbuf, (size_t)len);
        int rc = afs_write_handle(f->afs_handle, tmp, nsz);
        kfree(tmp);
        if (rc != 0) return -EIO;
        f->offset += len;
        return (int64_t)len;
    }
    case PFD_PIPE_W:
        return pipe_do_write(f, kbuf, len);
    case PFD_SOCK_INET:
    case PFD_SOCK_UNIX:
        return posix_sock_write(f, kbuf, len, 0);
    case PFD_SHM: {
        uint64_t end = f->offset + len;
        if (end > posix_shm_size(f->obj_id)) {
            int rc = posix_shm_resize(f->obj_id, end);
            if (rc < 0) return rc;
        }
        uint64_t done = 0;
        while (done < len) {
            uint64_t pos = f->offset + done;
            uint64_t pa = posix_shm_frame(f->obj_id, pos >> 12);
            if (!pa) break;
            uint64_t off = pos & 0xFFFull;
            uint64_t ch = 0x1000ull - off;
            if (ch > len - done) ch = len - done;
            memcpy((void *)(uintptr_t)(pa + off), kbuf + done, (size_t)ch);
            done += ch;
        }
        f->offset += done;
        return (int64_t)done;
    }
    default:
        return -EBADF;
    }
}

int64_t posix_sys_read(posix_proc_t *p, int fd, uint64_t buf_va, uint64_t len) {
    posix_file_t *f = posix_fd_get(p, fd);
    if (!f) return -EBADF;
    if (len == 0) return 0;
    if (len > (1024ull * 1024ull)) len = 1024ull * 1024ull;
    if (!posix_uprobe(buf_va, len, 1)) return -EFAULT;

    uint8_t *kbuf = (uint8_t *)kmalloc((size_t)len);
    if (!kbuf) return -ENOMEM;
    int64_t n = file_read_kbuf(f, kbuf, len);
    if (n > 0) posix_ucopy_out(buf_va, kbuf, (uint64_t)n);
    kfree(kbuf);
    return n;
}

int64_t posix_sys_write(posix_proc_t *p, int fd, uint64_t buf_va, uint64_t len) {
    posix_file_t *f = posix_fd_get(p, fd);
    if (!f) return -EBADF;
    if (len == 0) return 0;
    if (len > (1024ull * 1024ull)) len = 1024ull * 1024ull;
    if (!posix_uprobe(buf_va, len, 0)) return -EFAULT;

    uint8_t *kbuf = (uint8_t *)kmalloc((size_t)len);
    if (!kbuf) return -ENOMEM;
    posix_ucopy_in(kbuf, buf_va, len);
    int64_t n = file_write_kbuf(f, kbuf, len);
    kfree(kbuf);
    return n;
}

int64_t posix_sys_readv(posix_proc_t *p, int fd, uint64_t iov_va, int iovcnt) {
    if (iovcnt <= 0 || iovcnt > 64) return -EINVAL;
    int64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        posix_iovec_t iov;
        if (posix_ucopy_in(&iov, iov_va + (uint64_t)i * sizeof(iov), sizeof(iov)) < 0)
            return total ? total : -EFAULT;
        if (iov.iov_len == 0) continue;
        int64_t n = posix_sys_read(p, fd, (uint64_t)(uintptr_t)iov.iov_base, iov.iov_len);
        if (n < 0) return total ? total : n;
        total += n;
        if ((uint64_t)n < iov.iov_len) break;
    }
    return total;
}

int64_t posix_sys_writev(posix_proc_t *p, int fd, uint64_t iov_va, int iovcnt) {
    if (iovcnt <= 0 || iovcnt > 64) return -EINVAL;
    int64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        posix_iovec_t iov;
        if (posix_ucopy_in(&iov, iov_va + (uint64_t)i * sizeof(iov), sizeof(iov)) < 0)
            return total ? total : -EFAULT;
        if (iov.iov_len == 0) continue;
        int64_t n = posix_sys_write(p, fd, (uint64_t)(uintptr_t)iov.iov_base, iov.iov_len);
        if (n < 0) return total ? total : n;
        total += n;
        if ((uint64_t)n < iov.iov_len) break;
    }
    return total;
}

/* ---- lseek / stat / fstat ---- */
int64_t posix_sys_lseek(posix_proc_t *p, int fd, int64_t off, int whence) {
    posix_file_t *f = posix_fd_get(p, fd);
    if (!f) return -EBADF;
    if (f->kind == PFD_PIPE_R || f->kind == PFD_PIPE_W ||
        f->kind == PFD_SOCK_INET || f->kind == PFD_SOCK_UNIX)
        return -ESPIPE;

    int64_t base = 0;
    int64_t sz = 0;
    if (f->kind == PFD_AFS && f->afs_handle) sz = (int64_t)afs_handle_size(f->afs_handle);
    else if (f->kind == PFD_VFILE) sz = (int64_t)f->vlen;
    else if (f->kind == PFD_SHM) sz = (int64_t)posix_shm_size(f->obj_id);

    if (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = (int64_t)f->offset;
    else if (whence == SEEK_END) base = sz;
    else return -EINVAL;

    int64_t npos = base + off;
    if (npos < 0) return -EINVAL;
    f->offset = (uint64_t)npos;
    return npos;
}

static void fill_stat_for_file(posix_file_t *f, posix_stat_t *st) {
    memset(st, 0, sizeof(*st));
    st->st_dev = 1;
    st->st_ino = (uint64_t)(uintptr_t)f;
    st->st_nlink = 1;
    st->st_blksize = 4096;
    if (f->kind == PFD_CONSOLE || f->kind == PFD_DEVNULL || f->kind == PFD_URANDOM) {
        st->st_mode = S_IFCHR | 0666u;
    } else if (f->kind == PFD_PIPE_R || f->kind == PFD_PIPE_W) {
        st->st_mode = S_IFIFO | 0600u;
    } else if (f->kind == PFD_SOCK_INET || f->kind == PFD_SOCK_UNIX) {
        st->st_mode = S_IFSOCK | 0777u;
    } else if (f->kind == PFD_VFILE) {
        st->st_mode = S_IFREG | 0444u;
        st->st_size = f->vlen;
    } else if (f->kind == PFD_SHM || f->kind == PFD_GFX) {
        st->st_mode = S_IFREG | 0666u;
        int sid = (f->kind == PFD_GFX) ? posix_gfx_shm_id(f->obj_id) : f->obj_id;
        st->st_size = (int64_t)posix_shm_size(sid);
    } else if (f->kind == PFD_AFS && f->afs_handle) {
        st->st_mode = f->is_dir ? (S_IFDIR | 0755u) : (S_IFREG | 0644u);
        st->st_size = (int64_t)afs_handle_size(f->afs_handle);
    } else {
        st->st_mode = S_IFREG | 0644u;
    }
    st->st_blocks = (st->st_size + 511) / 512;
}

int64_t posix_sys_fstat(posix_proc_t *p, int fd, uint64_t st_va) {
    posix_file_t *f = posix_fd_get(p, fd);
    if (!f) return -EBADF;
    posix_stat_t st;
    fill_stat_for_file(f, &st);
    if (posix_ucopy_out(st_va, &st, sizeof(st)) < 0) return -EFAULT;
    return 0;
}

int64_t posix_sys_newfstatat(posix_proc_t *p, int dirfd, uint64_t path_va, uint64_t st_va, int flags) {
    if ((flags & AT_EMPTY_PATH) && dirfd >= 0)
        return posix_sys_fstat(p, dirfd, st_va);
    char raw[192], path[192];
    if (posix_ucstr_in(raw, sizeof(raw), path_va) < 0) return -EFAULT;
    resolve_path(p, raw, path, sizeof(path));

    posix_stat_t st;
    memset(&st, 0, sizeof(st));
    st.st_dev = 1; st.st_nlink = 1; st.st_blksize = 4096;

    if (strcmp(path, "/") == 0 || strcmp(path, "/etc") == 0 || strcmp(path, "/dev") == 0) {
        st.st_mode = S_IFDIR | 0755u;
    } else if (strcmp(path, "/etc/resolv.conf") == 0) {
        build_resolv_conf();
        st.st_mode = S_IFREG | 0444u;
        st.st_size = (int64_t)strlen(s_resolv_buf);
    } else if (strcmp(path, "/etc/hosts") == 0) {
        st.st_mode = S_IFREG | 0444u;
        st.st_size = (int64_t)(sizeof(s_hosts_buf) - 1);
    } else if (strcmp(path, "/dev/null") == 0 || strcmp(path, "/dev/urandom") == 0) {
        st.st_mode = S_IFCHR | 0666u;
    } else {
        void *h = afs_lookup_path(path);
        if (!h) return -ENOENT;
        st.st_mode = S_IFREG | 0644u;
        st.st_size = (int64_t)afs_handle_size(h);
        st.st_ino  = (uint64_t)(uintptr_t)h;
    }
    st.st_blocks = (st.st_size + 511) / 512;
    if (posix_ucopy_out(st_va, &st, sizeof(st)) < 0) return -EFAULT;
    return 0;
}

/* ---- pipe2 / dup / dup3 / fcntl ---- */
int64_t posix_sys_pipe2(posix_proc_t *p, uint64_t fds_va, int flags) {
    uint8_t *buf = (uint8_t *)kmalloc(PPIPE_CAP);
    if (!buf) return -ENOMEM;

    uint64_t fl = sched_irq_save();
    spin_lock(&s_pipe_tab_lock);
    int id = -1;
    for (int i = 0; i < POSIX_MAX_PIPES; i++) {
        if (!s_pipes[i].used) {
            s_pipes[i].used = 1;
            spin_init(&s_pipes[i].lock);
            s_pipes[i].buf = buf;
            s_pipes[i].head = s_pipes[i].tail = s_pipes[i].count = 0;
            s_pipes[i].readers = 1;
            s_pipes[i].writers = 1;
            waitq_init(&s_pipes[i].rd_wq);
            waitq_init(&s_pipes[i].wr_wq);
            id = i;
            break;
        }
    }
    spin_unlock(&s_pipe_tab_lock);
    sched_irq_restore(fl);
    if (id < 0) { kfree(buf); return -ENFILE; }

    posix_file_t *fr = posix_file_alloc(PFD_PIPE_R);
    posix_file_t *fw = posix_file_alloc(PFD_PIPE_W);
    if (!fr || !fw) {
        if (fr) posix_file_unref(fr);
        if (fw) posix_file_unref(fw);
        s_pipes[id].used = 0;
        kfree(buf);
        return -ENFILE;
    }
    fr->obj_id = id; fr->flags = O_RDONLY | ((flags & O_NONBLOCK) ? O_NONBLOCK : 0);
    fw->obj_id = id; fw->flags = O_WRONLY | ((flags & O_NONBLOCK) ? O_NONBLOCK : 0);

    uint32_t fdf = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
    int rfd = posix_fd_install(p, fr, fdf);
    int wfd = (rfd >= 0) ? posix_fd_install(p, fw, fdf) : -EMFILE;
    if (rfd < 0 || wfd < 0) {
        if (rfd >= 0) posix_sys_close(p, rfd); else posix_file_unref(fr);
        posix_file_unref(fw);
        return -EMFILE;
    }
    int32_t pair[2] = { rfd, wfd };
    if (posix_ucopy_out(fds_va, pair, 8) < 0) {
        posix_sys_close(p, rfd);
        posix_sys_close(p, wfd);
        return -EFAULT;
    }
    return 0;
}

int64_t posix_sys_dup(posix_proc_t *p, int oldfd) {
    posix_file_t *f = posix_fd_get(p, oldfd);
    if (!f) return -EBADF;
    posix_file_ref(f);
    int nfd = posix_fd_install(p, f, 0);
    if (nfd < 0) posix_file_unref(f);
    return nfd;
}

int64_t posix_sys_dup3(posix_proc_t *p, int oldfd, int newfd, int flags) {
    if (newfd < 0 || newfd >= POSIX_MAX_FDS) return -EBADF;
    posix_file_t *f = posix_fd_get(p, oldfd);
    if (!f) return -EBADF;
    if (oldfd == newfd) return newfd;

    posix_file_t *old_new = 0;
    uint64_t fl = sched_irq_save();
    spin_lock(&p->lock);
    old_new = p->fds[newfd].file;
    posix_file_ref(f);
    p->fds[newfd].file = f;
    p->fds[newfd].fd_flags = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
    spin_unlock(&p->lock);
    sched_irq_restore(fl);
    if (old_new) posix_file_unref(old_new);
    return newfd;
}

int64_t posix_sys_fcntl(posix_proc_t *p, int fd, int cmd, uint64_t arg) {
    if (!p || fd < 0 || fd >= POSIX_MAX_FDS || !p->fds[fd].file) return -EBADF;
    posix_file_t *f = p->fds[fd].file;
    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        int minfd = (int)arg;
        if (minfd < 0 || minfd >= POSIX_MAX_FDS) return -EINVAL;
        uint64_t fl = sched_irq_save();
        spin_lock(&p->lock);
        int res = -EMFILE;
        for (int i = minfd; i < POSIX_MAX_FDS; i++) {
            if (!p->fds[i].file) {
                posix_file_ref(f);
                p->fds[i].file = f;
                p->fds[i].fd_flags = (cmd == F_DUPFD_CLOEXEC) ? FD_CLOEXEC : 0;
                res = i;
                break;
            }
        }
        spin_unlock(&p->lock);
        sched_irq_restore(fl);
        return res;
    }
    case F_GETFD: return (int64_t)p->fds[fd].fd_flags;
    case F_SETFD: p->fds[fd].fd_flags = (uint32_t)(arg & FD_CLOEXEC); return 0;
    case F_GETFL: return (int64_t)f->flags;
    case F_SETFL:
        f->flags = (f->flags & O_ACCMODE) | ((uint32_t)arg & (O_NONBLOCK | O_APPEND));
        return 0;
    default:
        return 0;
    }
}

/* ---- Birlesik hazirlik yoklamasi (poll + epoll) ---- */
uint32_t posix_file_poll_mask(posix_file_t *f) {
    if (!f) return POLLNVAL;
    switch (f->kind) {
    case PFD_CONSOLE:
    case PFD_DEVNULL:
    case PFD_URANDOM:
    case PFD_VFILE:
    case PFD_AFS:
    case PFD_SHM:
        return POLLIN | POLLOUT;
    case PFD_PIPE_R: {
        if (f->obj_id < 0 || f->obj_id >= POSIX_MAX_PIPES) return POLLERR;
        ppipe_t *pp = &s_pipes[f->obj_id];
        uint32_t m = 0;
        if (pp->count > 0) m |= POLLIN;
        if (pp->writers == 0) m |= POLLHUP;
        return m;
    }
    case PFD_PIPE_W: {
        if (f->obj_id < 0 || f->obj_id >= POSIX_MAX_PIPES) return POLLERR;
        ppipe_t *pp = &s_pipes[f->obj_id];
        uint32_t m = 0;
        if (pp->readers == 0) m |= POLLERR | POLLHUP;
        else if (pp->count < PPIPE_CAP) m |= POLLOUT;
        return m;
    }
    case PFD_SOCK_INET:
    case PFD_SOCK_UNIX:
        return posix_sock_poll_mask(f->kind, f->obj_id);
    case PFD_GFX:
        return posix_gfx_poll_mask(f->obj_id);
    default:
        return POLLNVAL;
    }
}

int64_t posix_sys_poll(posix_proc_t *p, uint64_t fds_va, uint64_t nfds, int timeout_ms) {
    if (nfds > 64) return -EINVAL;
    posix_pollfd_t kfds[64];
    if (nfds > 0 && posix_ucopy_in(kfds, fds_va, nfds * sizeof(posix_pollfd_t)) < 0)
        return -EFAULT;

    uint64_t deadline = (timeout_ms > 0) ? (clock_ms() + (uint64_t)timeout_ms) : 0;

    for (;;) {
        ipv4_poll();
        int ready = 0;
        for (uint64_t i = 0; i < nfds; i++) {
            kfds[i].revents = 0;
            if (kfds[i].fd < 0) continue;
            posix_file_t *f = posix_fd_get(p, kfds[i].fd);
            if (!f) { kfds[i].revents = POLLNVAL; ready++; continue; }
            uint32_t m = posix_file_poll_mask(f);
            uint32_t hit = m & ((uint32_t)kfds[i].events | POLLERR | POLLHUP | POLLNVAL);
            if (hit) { kfds[i].revents = (int16_t)hit; ready++; }
        }
        if (ready > 0 || timeout_ms == 0) {
            if (nfds > 0) posix_ucopy_out(fds_va, kfds, nfds * sizeof(posix_pollfd_t));
            return ready;
        }
        if (timeout_ms > 0 && clock_ms() >= deadline) {
            if (nfds > 0) posix_ucopy_out(fds_va, kfds, nfds * sizeof(posix_pollfd_t));
            return 0;
        }
        sched_sleep_ms(1);
    }
}

int64_t posix_sys_epoll_create1(posix_proc_t *p, int flags) {
    uint64_t fl = sched_irq_save();
    spin_lock(&s_epoll_tab_lock);
    int id = -1;
    for (int i = 0; i < POSIX_MAX_EPOLLS; i++) {
        if (!s_epolls[i].used) {
            memset(&s_epolls[i], 0, sizeof(s_epolls[i]));
            s_epolls[i].used = 1;
            spin_init(&s_epolls[i].lock);
            id = i;
            break;
        }
    }
    spin_unlock(&s_epoll_tab_lock);
    sched_irq_restore(fl);
    if (id < 0) return -ENFILE;

    posix_file_t *f = posix_file_alloc(PFD_EPOLL);
    if (!f) { s_epolls[id].used = 0; return -ENFILE; }
    f->obj_id = id;
    int fd = posix_fd_install(p, f, (flags & O_CLOEXEC) ? FD_CLOEXEC : 0);
    if (fd < 0) posix_file_unref(f);
    return fd;
}

int64_t posix_sys_epoll_ctl(posix_proc_t *p, int epfd, int op, int fd, uint64_t ev_va) {
    posix_file_t *ef = posix_fd_get(p, epfd);
    if (!ef || ef->kind != PFD_EPOLL) return -EBADF;
    if (!posix_fd_get(p, fd)) return -EBADF;
    pepoll_t *ep = &s_epolls[ef->obj_id];

    posix_epoll_event_t ev;
    memset(&ev, 0, sizeof(ev));
    if (op != EPOLL_CTL_DEL) {
        if (posix_ucopy_in(&ev, ev_va, sizeof(ev)) < 0) return -EFAULT;
    }

    uint64_t fl = sched_irq_save();
    spin_lock(&ep->lock);
    int rc = 0;
    if (op == EPOLL_CTL_ADD) {
        int slot = -1;
        for (int i = 0; i < POSIX_MAX_EPOLL_ENT; i++) {
            if (ep->ents[i].used && ep->ents[i].fd == fd) { rc = -EEXIST; break; }
            if (!ep->ents[i].used && slot < 0) slot = i;
        }
        if (rc == 0) {
            if (slot < 0) rc = -ENOMEM;
            else {
                ep->ents[slot].used = 1;
                ep->ents[slot].fd = fd;
                ep->ents[slot].events = ev.events;
                ep->ents[slot].data = ev.data;
            }
        }
    } else if (op == EPOLL_CTL_MOD) {
        rc = -ENOENT;
        for (int i = 0; i < POSIX_MAX_EPOLL_ENT; i++) {
            if (ep->ents[i].used && ep->ents[i].fd == fd) {
                ep->ents[i].events = ev.events;
                ep->ents[i].data = ev.data;
                rc = 0;
                break;
            }
        }
    } else if (op == EPOLL_CTL_DEL) {
        rc = -ENOENT;
        for (int i = 0; i < POSIX_MAX_EPOLL_ENT; i++) {
            if (ep->ents[i].used && ep->ents[i].fd == fd) {
                ep->ents[i].used = 0;
                rc = 0;
                break;
            }
        }
    } else {
        rc = -EINVAL;
    }
    spin_unlock(&ep->lock);
    sched_irq_restore(fl);
    return rc;
}

int64_t posix_sys_epoll_wait(posix_proc_t *p, int epfd, uint64_t evs_va, int maxev, int timeout_ms) {
    posix_file_t *ef = posix_fd_get(p, epfd);
    if (!ef || ef->kind != PFD_EPOLL) return -EBADF;
    if (maxev <= 0) return -EINVAL;
    if (maxev > 64) maxev = 64;
    pepoll_t *ep = &s_epolls[ef->obj_id];

    posix_epoll_event_t out[64];
    uint64_t deadline = (timeout_ms > 0) ? (clock_ms() + (uint64_t)timeout_ms) : 0;

    for (;;) {
        ipv4_poll();
        int n = 0;
        for (int i = 0; i < POSIX_MAX_EPOLL_ENT && n < maxev; i++) {
            if (!ep->ents[i].used) continue;
            posix_file_t *f = posix_fd_get(p, ep->ents[i].fd);
            if (!f) continue;
            uint32_t m = posix_file_poll_mask(f);
            uint32_t hit = m & (ep->ents[i].events | EPOLLERR | EPOLLHUP);
            if (hit) {
                out[n].events = hit;
                out[n].data   = ep->ents[i].data;
                n++;
            }
        }
        if (n > 0 || timeout_ms == 0) {
            if (n > 0 && posix_ucopy_out(evs_va, out, (uint64_t)n * sizeof(out[0])) < 0)
                return -EFAULT;
            return n;
        }
        if (timeout_ms > 0 && clock_ms() >= deadline) return 0;
        sched_sleep_ms(1);
    }
}

/* ---- Zaman ve entropi ---- */
static int64_t rtc_unix_seconds(void) {
    rtc_time_t t;
    memset(&t, 0, sizeof(t));
    rtc_read_time(&t);
    int y = t.year, m = t.month, d = t.day;
    if (y < 1970) y = 2026;
    /* Howard Hinnant sivil gun formulu */
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    return days * 86400 + (int64_t)t.hour * 3600 + (int64_t)t.minute * 60 + (int64_t)t.second;
}

int64_t posix_sys_clock_gettime(int clk_id, uint64_t ts_va) {
    posix_timespec_t ts;
    uint64_t ms = clock_ms();
    if (clk_id == CLOCK_REALTIME) {
        ts.tv_sec  = rtc_unix_seconds();
        ts.tv_nsec = (int64_t)((ms % 1000ull) * 1000000ull);
    } else {
        ts.tv_sec  = (int64_t)(ms / 1000ull);
        ts.tv_nsec = (int64_t)((ms % 1000ull) * 1000000ull);
    }
    if (posix_ucopy_out(ts_va, &ts, sizeof(ts)) < 0) return -EFAULT;
    return 0;
}

int64_t posix_sys_nanosleep(uint64_t req_va, uint64_t rem_va) {
    posix_timespec_t req;
    if (posix_ucopy_in(&req, req_va, sizeof(req)) < 0) return -EFAULT;
    if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000L) return -EINVAL;
    uint64_t ms = (uint64_t)req.tv_sec * 1000ull + (uint64_t)(req.tv_nsec / 1000000L);
    if (ms == 0 && req.tv_nsec > 0) ms = 1;
    if (ms > 0) sched_sleep_ms((uint32_t)ms);
    if (rem_va) {
        posix_timespec_t z = { 0, 0 };
        posix_ucopy_out(rem_va, &z, sizeof(z));
    }
    return 0;
}

int64_t posix_sys_getrandom(uint64_t buf_va, uint64_t len, uint32_t flags) {
    (void)flags;
    if (len == 0) return 0;
    if (len > 4096) len = 4096;
    if (!posix_uprobe(buf_va, len, 1)) return -EFAULT;
    uint8_t tmp[256];
    uint64_t done = 0;
    while (done < len) {
        uint64_t ch = (len - done > sizeof(tmp)) ? sizeof(tmp) : (len - done);
        for (uint64_t i = 0; i < ch; i += 4) {
            uint32_t r = net_rand32();
            uint64_t c = (ch - i < 4) ? (ch - i) : 4;
            memcpy(tmp + i, &r, (size_t)c);
        }
        posix_ucopy_out(buf_va + done, tmp, ch);
        done += ch;
    }
    return (int64_t)done;
}

int64_t posix_sys_memfd_create(posix_proc_t *p, uint64_t name_va, uint32_t flags) {
    char name[64];
    if (name_va) posix_ucstr_in(name, sizeof(name), name_va);
    int sid = posix_shm_create(0);
    if (sid < 0) return sid;
    posix_file_t *f = posix_file_alloc(PFD_SHM);
    if (!f) { posix_shm_unref(sid); return -ENFILE; }
    f->obj_id = sid;
    f->flags = O_RDWR;
    int fd = posix_fd_install(p, f, (flags & 1u) ? FD_CLOEXEC : 0);
    if (fd < 0) posix_file_unref(f);
    return fd;
}

int64_t posix_sys_ftruncate(posix_proc_t *p, int fd, int64_t len) {
    if (len < 0) return -EINVAL;
    posix_file_t *f = posix_fd_get(p, fd);
    if (!f) return -EBADF;
    if (f->kind == PFD_SHM) return posix_shm_resize(f->obj_id, (uint64_t)len);
    if (f->kind == PFD_AFS && f->afs_handle) {
        if (len == 0) return afs_write_handle(f->afs_handle, "", 0) == 0 ? 0 : -EIO;
        return 0;
    }
    return -EINVAL;
}

int64_t posix_sys_getcwd(posix_proc_t *p, uint64_t buf_va, uint64_t size) {
    size_t n = strlen(p->cwd) + 1;
    if (size < n) return -ERANGE;
    if (posix_ucopy_out(buf_va, p->cwd, n) < 0) return -EFAULT;
    return (int64_t)n;
}

int64_t posix_sys_chdir(posix_proc_t *p, uint64_t path_va) {
    char raw[192], path[192];
    if (posix_ucstr_in(raw, sizeof(raw), path_va) < 0) return -EFAULT;
    resolve_path(p, raw, path, sizeof(path));
    strncpy(p->cwd, path, sizeof(p->cwd) - 1);
    p->cwd[sizeof(p->cwd) - 1] = 0;
    return 0;
}

int64_t posix_sys_mkdirat(posix_proc_t *p, int dirfd, uint64_t path_va, uint32_t mode) {
    (void)dirfd; (void)mode;
    char raw[192], path[192];
    if (posix_ucstr_in(raw, sizeof(raw), path_va) < 0) return -EFAULT;
    resolve_path(p, raw, path, sizeof(path));
    const char *ap = (path[0] == '/') ? path + 1 : path;
    afs_ensure_dir(ap);
    return 0;
}

int64_t posix_sys_unlinkat(posix_proc_t *p, int dirfd, uint64_t path_va, int flags) {
    (void)dirfd; (void)flags;
    char raw[192], path[192];
    if (posix_ucstr_in(raw, sizeof(raw), path_va) < 0) return -EFAULT;
    resolve_path(p, raw, path, sizeof(path));
    const char *ap = (path[0] == '/') ? path + 1 : path;
    return afs_remove(ap) == 0 ? 0 : -ENOENT;
}

int64_t posix_sys_rename(posix_proc_t *p, uint64_t old_va, uint64_t new_va) {
    char r1[192], r2[192], p1[192], p2[192];
    if (posix_ucstr_in(r1, sizeof(r1), old_va) < 0) return -EFAULT;
    if (posix_ucstr_in(r2, sizeof(r2), new_va) < 0) return -EFAULT;
    resolve_path(p, r1, p1, sizeof(p1));
    resolve_path(p, r2, p2, sizeof(p2));
    return afs_rename((p1[0] == '/') ? p1 + 1 : p1, (p2[0] == '/') ? p2 + 1 : p2) == 0 ? 0 : -ENOENT;
}

int64_t posix_sys_getdents64(posix_proc_t *p, int fd, uint64_t dirp_va, uint64_t count) {
    (void)p; (void)fd; (void)dirp_va; (void)count;
    return 0;
}

int64_t posix_sys_access(posix_proc_t *p, int dirfd, uint64_t path_va, int mode) {
    (void)dirfd; (void)mode;
    char raw[192], path[192];
    if (posix_ucstr_in(raw, sizeof(raw), path_va) < 0) return -EFAULT;
    resolve_path(p, raw, path, sizeof(path));
    if (strcmp(path, "/") == 0 || strcmp(path, "/etc/resolv.conf") == 0 ||
        strcmp(path, "/etc/hosts") == 0 || strcmp(path, "/dev/null") == 0)
        return 0;
    return afs_lookup_path(path) ? 0 : -ENOENT;
}

int64_t posix_sys_ioctl(posix_proc_t *p, int fd, uint64_t req, uint64_t arg) {
    posix_file_t *f = posix_fd_get(p, fd);
    if (!f) return -EBADF;
    if (req == 0x5421u /* FIONBIO */) {
        int val = 0;
        if (posix_ucopy_in(&val, arg, 4) < 0) return -EFAULT;
        if (val) f->flags |= O_NONBLOCK;
        else f->flags &= ~O_NONBLOCK;
        return 0;
    }
    if (req == 0x5413u /* TIOCGWINSZ */ && f->kind == PFD_CONSOLE) {
        uint16_t ws[4] = { 25, 80, 0, 0 };
        posix_ucopy_out(arg, ws, sizeof(ws));
        return 0;
    }
    return -ENOTTY;
}
