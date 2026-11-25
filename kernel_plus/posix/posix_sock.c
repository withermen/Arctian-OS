/* POSIX BSD Soketleri: AF_INET (TCP/UDP + 127.0.0.1 loopback + dis ag)
 * ve AF_UNIX (socketpair + bind/listen/accept/connect + SCM_RIGHTS FD gecisi).
 *
 * Tum soketler O_NONBLOCK ve poll()/epoll() ile tam entegredir.
 * musl getaddrinfo() -> /etc/resolv.conf + UDP AF_INET soketi uzerinden
 * dogal olarak calisir.
 */
#include "posix.h"
#include "alloc.h"
#include "tcp.h"
#include "udp.h"
#include "ipv4.h"
#include "clock.h"
#include "percpu.h"
#include "serial.h"
#include <string.h>

static inline void ensure_cpu0_for_net(void) {
    if (percpu_self_index() != 0) {
        sched_tid_t me = sched_current();
        if (me > 0) {
            sched_enqueue_cpu(0, me);
            sched_sleep_ms(1);
        }
    }
}

static inline uint16_t bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

/* ---- AF_UNIX soketleri (cift yonlu halka tampon + SCM_RIGHTS) ---- */
#define USOCK_BUF_CAP  131072u
#define USOCK_BACKLOG  16
#define USOCK_MAX_FDS  32

typedef struct {
    int           used;
    int           type;        /* SOCK_STREAM / SOCK_DGRAM */
    int           state;       /* 0=open, 1=bound, 2=listen, 3=connected, 4=closed */
    spinlock_t    lock;
    char          path[108];
    int           peer_id;     /* bagli karsi AF_UNIX yuvasi (-1 = yok) */
    int           peer_closed;
    uint8_t      *rx_buf;
    uint32_t      rx_head;
    uint32_t      rx_tail;
    uint32_t      rx_count;
    posix_file_t *rx_fds[USOCK_MAX_FDS];
    int           rx_fd_count;
    int           backlog[USOCK_BACKLOG];
    int           bl_head;
    int           bl_count;
    waitq_t       rd_wq;
    waitq_t       wr_wq;
} usock_t;

static usock_t    s_usocks[POSIX_MAX_USOCKS];
static spinlock_t s_utab_lock;

/* ---- AF_INET soketleri (TCP / UDP / 127.0.0.1 loopback) ---- */
#define ISOCK_UDP_Q   16
#define ISOCK_UDP_MAX 1500

typedef struct {
    int       used;
    int       type;            /* SOCK_STREAM / SOCK_DGRAM */
    int       state;           /* 0=open, 1=bound, 2=listen, 3=connected */
    int       tcp_id;          /* kernel tcp.c yuvasi (-1 = yok) */
    int       loop_usock;      /* 127.0.0.1 loopback icin dahili AF_UNIX cifti (-1 = dis ag) */
    uint8_t   local_ip[4];
    uint16_t  local_port;      /* host byte order */
    uint8_t   peer_ip[4];
    uint16_t  peer_port;
    int       so_reuseaddr;
    int       tcp_nodelay;
    int       so_error;
    /* UDP RX kuyrugu */
    spinlock_t udp_lock;
    uint16_t   u_head;
    uint16_t   u_count;
    uint16_t   u_len[ISOCK_UDP_Q];
    uint16_t   u_sport[ISOCK_UDP_Q];
    uint8_t    u_sip[ISOCK_UDP_Q][4];
    uint8_t   *u_buf[ISOCK_UDP_Q];
} isock_t;

static isock_t    s_isocks[POSIX_MAX_ISOCKS];
static spinlock_t s_itab_lock;
static uint16_t   s_ephem_port = 52000;

static void isock_udp_cb(const uint8_t src_ip[4], uint16_t src_port,
                         uint16_t dst_port, const uint8_t *data, uint16_t len);

void posix_sock_init(void) {
    memset(s_usocks, 0, sizeof(s_usocks));
    memset(s_isocks, 0, sizeof(s_isocks));
    spin_init(&s_utab_lock);
    spin_init(&s_itab_lock);
}

/* ---- AF_UNIX yardimcilari ---- */
static int usock_alloc(int type) {
    uint8_t *buf = (uint8_t *)kmalloc(USOCK_BUF_CAP);
    if (!buf) return -ENOMEM;
    uint64_t fl = sched_irq_save();
    spin_lock(&s_utab_lock);
    int id = -1;
    for (int i = 0; i < POSIX_MAX_USOCKS; i++) {
        if (!s_usocks[i].used) {
            memset(&s_usocks[i], 0, sizeof(s_usocks[i]));
            s_usocks[i].used = 1;
            s_usocks[i].type = type;
            s_usocks[i].peer_id = -1;
            s_usocks[i].rx_buf = buf;
            spin_init(&s_usocks[i].lock);
            waitq_init(&s_usocks[i].rd_wq);
            waitq_init(&s_usocks[i].wr_wq);
            id = i;
            break;
        }
    }
    spin_unlock(&s_utab_lock);
    sched_irq_restore(fl);
    if (id < 0) { kfree(buf); return -ENFILE; }
    return id;
}

static void usock_close(int id) {
    if (id < 0 || id >= POSIX_MAX_USOCKS) return;
    usock_t *u = &s_usocks[id];
    int peer = -1;
    uint8_t *buf = 0;
    posix_file_t *drop_fds[USOCK_MAX_FDS];
    int dn = 0;

    uint64_t fl = sched_irq_save();
    spin_lock(&u->lock);
    if (!u->used) { spin_unlock(&u->lock); sched_irq_restore(fl); return; }
    peer = u->peer_id;
    u->peer_id = -1;
    buf = u->rx_buf;
    u->rx_buf = 0;
    for (int i = 0; i < u->rx_fd_count && i < USOCK_MAX_FDS; i++)
        drop_fds[dn++] = u->rx_fds[i];
    u->rx_fd_count = 0;
    u->used = 0;
    u->state = 4;
    spin_unlock(&u->lock);
    sched_irq_restore(fl);

    if (peer >= 0 && peer < POSIX_MAX_USOCKS) {
        usock_t *pu = &s_usocks[peer];
        sched_tid_t wakes[WAITQ_MAX * 2];
        int wn = 0;
        fl = sched_irq_save();
        spin_lock(&pu->lock);
        if (pu->used && pu->peer_id == id) {
            pu->peer_closed = 1;
            pu->peer_id = -1;
            int w;
            while ((w = waitq_pop(&pu->rd_wq)) >= 0 && wn < WAITQ_MAX * 2) wakes[wn++] = w;
            while ((w = waitq_pop(&pu->wr_wq)) >= 0 && wn < WAITQ_MAX * 2) wakes[wn++] = w;
        }
        spin_unlock(&pu->lock);
        sched_irq_restore(fl);
        for (int i = 0; i < wn; i++) sched_wake(wakes[i]);
    }
    for (int i = 0; i < dn; i++) posix_file_unref(drop_fds[i]);
    if (buf) kfree(buf);
}

static uint32_t usock_poll(int id) {
    if (id < 0 || id >= POSIX_MAX_USOCKS || !s_usocks[id].used) return POLLERR;
    usock_t *u = &s_usocks[id];
    uint32_t m = 0;
    if (u->state == 2) {
        if (u->bl_count > 0) m |= POLLIN;
        return m;
    }
    if (u->rx_count > 0 || u->rx_fd_count > 0) m |= POLLIN;
    if (u->peer_closed) m |= POLLHUP | POLLIN;
    if (u->peer_id >= 0 && s_usocks[u->peer_id].used &&
        s_usocks[u->peer_id].rx_count < USOCK_BUF_CAP)
        m |= POLLOUT;
    return m;
}

static int64_t usock_write_raw(int id, const uint8_t *buf, uint64_t len, int nonblock,
                               posix_file_t **send_fds, int n_fds) {
    if (id < 0 || id >= POSIX_MAX_USOCKS || !s_usocks[id].used) return -EBADF;
    usock_t *u = &s_usocks[id];
    if (u->peer_closed || u->peer_id < 0) return -EPIPE;
    int pid = u->peer_id;
    usock_t *pu = &s_usocks[pid];

    uint64_t done = 0;
    while (done < len || n_fds > 0) {
        uint64_t fl = sched_irq_save();
        spin_lock(&pu->lock);
        if (!pu->used) {
            spin_unlock(&pu->lock);
            sched_irq_restore(fl);
            return done ? (int64_t)done : -EPIPE;
        }
        uint32_t sp = USOCK_BUF_CAP - pu->rx_count;
        if (len == 0) {
            if (n_fds > 0) {
                for (int i = 0; i < n_fds && pu->rx_fd_count < USOCK_MAX_FDS; i++) {
                    posix_file_ref(send_fds[i]);
                    pu->rx_fds[pu->rx_fd_count++] = send_fds[i];
                }
                n_fds = 0;
            }
            int w = waitq_pop(&pu->rd_wq);
            spin_unlock(&pu->lock);
            if (w >= 0) sched_wake(w);
            sched_irq_restore(fl);
            break;
        }
        if (sp > 0) {
            if (n_fds > 0) {
                for (int i = 0; i < n_fds && pu->rx_fd_count < USOCK_MAX_FDS; i++) {
                    posix_file_ref(send_fds[i]);
                    pu->rx_fds[pu->rx_fd_count++] = send_fds[i];
                }
                n_fds = 0;
            }
            uint64_t ch = len - done;
            if (ch > sp) ch = sp;
            for (uint64_t i = 0; i < ch; i++) {
                pu->rx_buf[pu->rx_tail] = buf[done + i];
                pu->rx_tail = (pu->rx_tail + 1u) % USOCK_BUF_CAP;
            }
            pu->rx_count += (uint32_t)ch;
            done += ch;
            int w = waitq_pop(&pu->rd_wq);
            spin_unlock(&pu->lock);
            if (w >= 0) sched_wake(w);
            sched_irq_restore(fl);
            if (nonblock) break;
            continue;
        }
        if (nonblock) {
            spin_unlock(&pu->lock);
            sched_irq_restore(fl);
            return done ? (int64_t)done : -EAGAIN;
        }
        waitq_add(&pu->wr_wq, sched_current());
        sched_prepare_block();
        spin_unlock(&pu->lock);
        sched_commit_block();
        sched_irq_restore(fl);
    }
    return (int64_t)done;
}

static int64_t usock_read_raw(int id, uint8_t *buf, uint64_t len, int nonblock,
                              posix_file_t **out_fds, int max_fds, int *got_fds) {
    if (got_fds) *got_fds = 0;
    if (id < 0 || id >= POSIX_MAX_USOCKS || !s_usocks[id].used) return -EBADF;
    usock_t *u = &s_usocks[id];

    for (;;) {
        uint64_t fl = sched_irq_save();
        spin_lock(&u->lock);
        if (u->rx_count > 0 || (out_fds && u->rx_fd_count > 0)) {
            uint64_t take = (len < u->rx_count) ? len : u->rx_count;
            for (uint64_t i = 0; i < take; i++) {
                buf[i] = u->rx_buf[u->rx_head];
                u->rx_head = (u->rx_head + 1u) % USOCK_BUF_CAP;
            }
            u->rx_count -= (uint32_t)take;
            if (out_fds && max_fds > 0 && u->rx_fd_count > 0) {
                int k = (u->rx_fd_count < max_fds) ? u->rx_fd_count : max_fds;
                for (int i = 0; i < k; i++) out_fds[i] = u->rx_fds[i];
                for (int i = k; i < u->rx_fd_count; i++) u->rx_fds[i - k] = u->rx_fds[i];
                u->rx_fd_count -= k;
                if (got_fds) *got_fds = k;
            }
            int w = waitq_pop(&u->wr_wq);
            spin_unlock(&u->lock);
            if (w >= 0) sched_wake(w);
            sched_irq_restore(fl);
            return (int64_t)take;
        }
        if (u->peer_closed) {
            spin_unlock(&u->lock);
            sched_irq_restore(fl);
            return 0;
        }
        if (nonblock) {
            spin_unlock(&u->lock);
            sched_irq_restore(fl);
            return -EAGAIN;
        }
        waitq_add(&u->rd_wq, sched_current());
        sched_prepare_block();
        spin_unlock(&u->lock);
        sched_commit_block();
        sched_irq_restore(fl);
    }
}

/* ---- AF_INET yardimcilari ---- */
static int isock_alloc(int type) {
    uint64_t fl = sched_irq_save();
    spin_lock(&s_itab_lock);
    int id = -1;
    for (int i = 0; i < POSIX_MAX_ISOCKS; i++) {
        if (!s_isocks[i].used) {
            memset(&s_isocks[i], 0, sizeof(s_isocks[i]));
            s_isocks[i].used = 1;
            s_isocks[i].type = type;
            s_isocks[i].tcp_id = -1;
            s_isocks[i].loop_usock = -1;
            spin_init(&s_isocks[i].udp_lock);
            id = i;
            break;
        }
    }
    spin_unlock(&s_itab_lock);
    sched_irq_restore(fl);
    return id < 0 ? -ENFILE : id;
}

static uint16_t isock_alloc_port(void) {
    uint16_t p = s_ephem_port++;
    if (s_ephem_port < 49152) s_ephem_port = 52000;
    return p;
}

static void isock_close(int id) {
    if (id < 0 || id >= POSIX_MAX_ISOCKS || !s_isocks[id].used) return;
    isock_t *s = &s_isocks[id];
    if (s->loop_usock >= 0) {
        usock_close(s->loop_usock);
        s->loop_usock = -1;
    }
    if (s->type == SOCK_DGRAM && s->local_port)
        udp_unbind(s->local_port, isock_udp_cb);
    if (s->type == SOCK_STREAM && s->tcp_id >= 0) {
        if (!tcp_close(s->tcp_id)) tcp_abort(s->tcp_id);
        s->tcp_id = -1;
    }
    for (int i = 0; i < ISOCK_UDP_Q; i++) {
        if (s->u_buf[i]) { kfree(s->u_buf[i]); s->u_buf[i] = 0; }
    }
    s->used = 0;
}

static void isock_udp_cb(const uint8_t src_ip[4], uint16_t src_port,
                         uint16_t dst_port, const uint8_t *data, uint16_t len) {
    for (int i = 0; i < POSIX_MAX_ISOCKS; i++) {
        isock_t *s = &s_isocks[i];
        if (!s->used || s->type != SOCK_DGRAM || s->local_port != dst_port) continue;
        uint64_t fl = sched_irq_save();
        spin_lock(&s->udp_lock);
        if (s->u_count >= ISOCK_UDP_Q) {
            if (s->u_buf[s->u_head]) { kfree(s->u_buf[s->u_head]); s->u_buf[s->u_head] = 0; }
            s->u_head = (uint16_t)((s->u_head + 1u) % ISOCK_UDP_Q);
            s->u_count--;
        }
        uint16_t slot = (uint16_t)((s->u_head + s->u_count) % ISOCK_UDP_Q);
        uint16_t n = (len > ISOCK_UDP_MAX) ? ISOCK_UDP_MAX : len;
        uint8_t *b = (uint8_t *)kmalloc(n ? n : 1);
        if (b) {
            memcpy(b, data, n);
            s->u_buf[slot] = b;
            s->u_len[slot] = n;
            s->u_sport[slot] = src_port;
            memcpy(s->u_sip[slot], src_ip, 4);
            s->u_count++;
        }
        spin_unlock(&s->udp_lock);
        sched_irq_restore(fl);
        return;
    }
}

static uint32_t isock_poll(int id) {
    if (id < 0 || id >= POSIX_MAX_ISOCKS || !s_isocks[id].used) return POLLERR;
    isock_t *s = &s_isocks[id];
    if (s->loop_usock >= 0) return usock_poll(s->loop_usock);
    ensure_cpu0_for_net();
    ipv4_poll();
    uint32_t m = 0;
    if (s->type == SOCK_DGRAM) {
        if (s->u_count > 0) m |= POLLIN;
        m |= POLLOUT;
        return m;
    }
    if (s->state == 2 && s->tcp_id >= 0) {
        /* Dinleyici: bekleyen baglanti var mi */
        return m;
    }
    if (s->tcp_id >= 0) {
        int st = tcp_state(s->tcp_id);
        if (st == TCP_STATE_ESTABLISHED) m |= POLLOUT;
        if (st == TCP_STATE_CLOSE_WAIT || st == TCP_STATE_CLOSED || st == TCP_STATE_TIME_WAIT)
            m |= POLLIN | POLLHUP;
        /* RX tamponunda veri var mi yokla (0 bayt okuyarak tcp_receive degil,
         * tcp_rx_avail yardimcisi ile; tcp.h'ye ekleyecegiz veya 0 degil kontrol) */
        extern uint32_t tcp_rx_avail(int conn_id);
        if (tcp_rx_avail(s->tcp_id) > 0) m |= POLLIN;
    }
    return m;
}

void posix_sock_close_obj(int kind, int obj_id) {
    if (kind == PFD_SOCK_UNIX) usock_close(obj_id);
    else if (kind == PFD_SOCK_INET) isock_close(obj_id);
}

uint32_t posix_sock_poll_mask(int kind, int obj_id) {
    if (kind == PFD_SOCK_UNIX) return usock_poll(obj_id);
    if (kind == PFD_SOCK_INET) return isock_poll(obj_id);
    return POLLNVAL;
}

/* ---- Genel read / write girisi ---- */
int64_t posix_sock_read(posix_file_t *f, uint8_t *buf, uint64_t len, int flags) {
    int nb = ((f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) ? 1 : 0;
    if (f->kind == PFD_SOCK_UNIX)
        return usock_read_raw(f->obj_id, buf, len, nb, 0, 0, 0);
    if (f->kind == PFD_SOCK_INET) {
        int id = f->obj_id;
        if (id < 0 || id >= POSIX_MAX_ISOCKS || !s_isocks[id].used) return -EBADF;
        isock_t *s = &s_isocks[id];
        if (s->loop_usock >= 0)
            return usock_read_raw(s->loop_usock, buf, len, nb, 0, 0, 0);
        ensure_cpu0_for_net();
        if (s->type == SOCK_DGRAM) {
            for (;;) {
                ipv4_poll();
                uint64_t fl = sched_irq_save();
                spin_lock(&s->udp_lock);
                if (s->u_count > 0) {
                    uint16_t n = s->u_len[s->u_head];
                    if (n > len) n = (uint16_t)len;
                    memcpy(buf, s->u_buf[s->u_head], n);
                    kfree(s->u_buf[s->u_head]);
                    s->u_buf[s->u_head] = 0;
                    s->u_head = (uint16_t)((s->u_head + 1u) % ISOCK_UDP_Q);
                    s->u_count--;
                    spin_unlock(&s->udp_lock);
                    sched_irq_restore(fl);
                    return n;
                }
                spin_unlock(&s->udp_lock);
                sched_irq_restore(fl);
                if (nb) return -EAGAIN;
                sched_sleep_ms(1);
            }
        }
        if (s->tcp_id < 0) return -ENOTCONN;
        for (;;) {
            uint16_t cap = (len > 65535u) ? 65535u : (uint16_t)len;
            int n = tcp_receive(s->tcp_id, buf, cap);
            if (n > 0) return n;
            if (n < 0) return 0;
            int st = tcp_state(s->tcp_id);
            if (st == TCP_STATE_CLOSE_WAIT || st == TCP_STATE_CLOSED || st == TCP_STATE_TIME_WAIT)
                return 0;
            if (nb) return -EAGAIN;
            sched_sleep_ms(1);
        }
    }
    return -ENOTSOCK;
}

int64_t posix_sock_write(posix_file_t *f, const uint8_t *buf, uint64_t len, int flags) {
    int nb = ((f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) ? 1 : 0;
    if (f->kind == PFD_SOCK_UNIX)
        return usock_write_raw(f->obj_id, buf, len, nb, 0, 0);
    if (f->kind == PFD_SOCK_INET) {
        int id = f->obj_id;
        if (id < 0 || id >= POSIX_MAX_ISOCKS || !s_isocks[id].used) return -EBADF;
        isock_t *s = &s_isocks[id];
        if (s->loop_usock >= 0)
            return usock_write_raw(s->loop_usock, buf, len, nb, 0, 0);
        ensure_cpu0_for_net();
        if (s->type == SOCK_DGRAM) {
            if (s->peer_port == 0) return -EDESTADDRREQ;
            if (s->local_port == 0) {
                s->local_port = isock_alloc_port();
                udp_bind(s->local_port, isock_udp_cb);
            }
            if (len > 1400) return -EMSGSIZE;
            if (!udp_send(s->peer_ip, s->local_port, s->peer_port, buf, (uint16_t)len))
                return -EIO;
            return (int64_t)len;
        }
        if (s->tcp_id < 0) return -ENOTCONN;
        uint64_t off = 0;
        while (off < len) {
            uint64_t rem = len - off;
            uint16_t ch = (rem > 4096) ? 4096 : (uint16_t)rem;
            if (!tcp_send(s->tcp_id, buf + off, ch))
                return off ? (int64_t)off : -EPIPE;
            off += ch;
        }
        return (int64_t)len;
    }
    return -ENOTSOCK;
}

/* ---- BSD Soket Syscall'lari ---- */
int64_t posix_sys_socket(posix_proc_t *p, int domain, int type, int proto) {
    (void)proto;
    int stype = type & 0xF;
    if (stype != SOCK_STREAM && stype != SOCK_DGRAM) return -EPROTONOSUPPORT;

    int kind, oid;
    if (domain == AF_UNIX) {
        kind = PFD_SOCK_UNIX;
        oid = usock_alloc(stype);
    } else if (domain == AF_INET) {
        kind = PFD_SOCK_INET;
        oid = isock_alloc(stype);
    } else {
        return -EAFNOSUPPORT;
    }
    if (oid < 0) return oid;

    posix_file_t *f = posix_file_alloc(kind);
    if (!f) { posix_sock_close_obj(kind, oid); return -ENFILE; }
    f->obj_id = oid;
    f->flags = O_RDWR | ((type & SOCK_NONBLOCK) ? O_NONBLOCK : 0);

    int fd = posix_fd_install(p, f, (type & SOCK_CLOEXEC) ? FD_CLOEXEC : 0);
    if (fd < 0) posix_file_unref(f);
    return fd;
}

int64_t posix_sys_socketpair(posix_proc_t *p, int domain, int type, int proto, uint64_t sv_va) {
    (void)proto;
    if (domain != AF_UNIX) return -EAFNOSUPPORT;
    int stype = type & 0xF;
    if (stype != SOCK_STREAM && stype != SOCK_DGRAM) return -EPROTONOSUPPORT;

    int a = usock_alloc(stype);
    int b = (a >= 0) ? usock_alloc(stype) : -ENFILE;
    if (a < 0 || b < 0) {
        if (a >= 0) usock_close(a);
        return -ENFILE;
    }
    s_usocks[a].peer_id = b; s_usocks[a].state = 3;
    s_usocks[b].peer_id = a; s_usocks[b].state = 3;

    posix_file_t *fa = posix_file_alloc(PFD_SOCK_UNIX);
    posix_file_t *fb = posix_file_alloc(PFD_SOCK_UNIX);
    if (!fa || !fb) {
        if (fa) posix_file_unref(fa); else usock_close(a);
        if (fb) posix_file_unref(fb); else usock_close(b);
        return -ENFILE;
    }
    uint32_t fl = O_RDWR | ((type & SOCK_NONBLOCK) ? O_NONBLOCK : 0);
    uint32_t fdf = (type & SOCK_CLOEXEC) ? FD_CLOEXEC : 0;
    fa->obj_id = a; fa->flags = fl;
    fb->obj_id = b; fb->flags = fl;

    int fda = posix_fd_install(p, fa, fdf);
    int fdb = (fda >= 0) ? posix_fd_install(p, fb, fdf) : -EMFILE;
    if (fda < 0 || fdb < 0) {
        if (fda >= 0) posix_sys_close(p, fda); else posix_file_unref(fa);
        posix_file_unref(fb);
        return -EMFILE;
    }
    int32_t sv[2] = { fda, fdb };
    if (posix_ucopy_out(sv_va, sv, 8) < 0) {
        posix_sys_close(p, fda);
        posix_sys_close(p, fdb);
        return -EFAULT;
    }
    return 0;
}

int64_t posix_sys_bind(posix_proc_t *p, int fd, uint64_t addr_va, uint32_t addrlen) {
    posix_file_t *f = posix_fd_get(p, fd);
    if (!f) return -EBADF;

    if (f->kind == PFD_SOCK_UNIX) {
        if (addrlen < 4) return -EINVAL;
        posix_sockaddr_un_t sun;
        memset(&sun, 0, sizeof(sun));
        uint32_t cp = (addrlen > sizeof(sun)) ? sizeof(sun) : addrlen;
        if (posix_ucopy_in(&sun, addr_va, cp) < 0) return -EFAULT;
        if (sun.sun_family != AF_UNIX || sun.sun_path[0] == 0) return -EINVAL;
        usock_t *u = &s_usocks[f->obj_id];
        strncpy(u->path, sun.sun_path, sizeof(u->path) - 1);
        u->state = 1;
        return 0;
    }

    if (f->kind == PFD_SOCK_INET) {
        if (addrlen < sizeof(posix_sockaddr_in_t)) return -EINVAL;
        posix_sockaddr_in_t sin;
        if (posix_ucopy_in(&sin, addr_va, sizeof(sin)) < 0) return -EFAULT;
        if (sin.sin_family != AF_INET) return -EAFNOSUPPORT;
        isock_t *s = &s_isocks[f->obj_id];
        memcpy(s->local_ip, &sin.sin_addr, 4);
        s->local_port = bswap16(sin.sin_port);
        if (s->local_port == 0) s->local_port = isock_alloc_port();
        s->state = 1;
        if (s->type == SOCK_DGRAM) udp_bind(s->local_port, isock_udp_cb);
        return 0;
    }
    return -ENOTSOCK;
}

int64_t posix_sys_listen(posix_proc_t *p, int fd, int backlog) {
    (void)backlog;
    posix_file_t *f = posix_fd_get(p, fd);
    if (!f) return -EBADF;
    if (f->kind == PFD_SOCK_UNIX) {
        usock_t *u = &s_usocks[f->obj_id];
        u->state = 2;
        return 0;
    }
    if (f->kind == PFD_SOCK_INET) {
        isock_t *s = &s_isocks[f->obj_id];
        if (s->type != SOCK_STREAM) return -EOPNOTSUPP;
        if (s->local_port == 0) s->local_port = isock_alloc_port();
        /* 127.0.0.1 loopback dinleyici olarak dahili usock ayir + gercek TCP dinle */
        if (s->loop_usock < 0) {
            s->loop_usock = usock_alloc(SOCK_STREAM);
            if (s->loop_usock >= 0) s_usocks[s->loop_usock].state = 2;
        }
        if (!(s->local_ip[0] == 127)) {
            int tc = tcp_listen(s->local_port);
            if (tc >= 0) s->tcp_id = tc;
        }
        s->state = 2;
        return 0;
    }
    return -ENOTSOCK;
}

int64_t posix_sys_accept4(posix_proc_t *p, int fd, uint64_t addr_va, uint64_t len_va, int flags) {
    posix_file_t *f = posix_fd_get(p, fd);
    if (!f) return -EBADF;
    int nb = ((f->flags & O_NONBLOCK) || (flags & SOCK_NONBLOCK)) ? 1 : 0;

    if (f->kind == PFD_SOCK_UNIX) {
        usock_t *l = &s_usocks[f->obj_id];
        if (l->state != 2) return -EINVAL;
        for (;;) {
            uint64_t fl = sched_irq_save();
            spin_lock(&l->lock);
            if (l->bl_count > 0) {
                int cid = l->backlog[l->bl_head];
                l->bl_head = (l->bl_head + 1) % USOCK_BACKLOG;
                l->bl_count--;
                spin_unlock(&l->lock);
                sched_irq_restore(fl);

                posix_file_t *nf = posix_file_alloc(PFD_SOCK_UNIX);
                if (!nf) { usock_close(cid); return -ENFILE; }
                nf->obj_id = cid;
                nf->flags = O_RDWR | ((flags & SOCK_NONBLOCK) ? O_NONBLOCK : 0);
                int nfd = posix_fd_install(p, nf, (flags & SOCK_CLOEXEC) ? FD_CLOEXEC : 0);
                if (nfd < 0) posix_file_unref(nf);
                return nfd;
            }
            if (nb) {
                spin_unlock(&l->lock);
                sched_irq_restore(fl);
                return -EAGAIN;
            }
            waitq_add(&l->rd_wq, sched_current());
            sched_prepare_sleep_ms(10);
            spin_unlock(&l->lock);
            sched_commit_block();
            sched_irq_restore(fl);
        }
    }

    if (f->kind == PFD_SOCK_INET) {
        isock_t *l = &s_isocks[f->obj_id];
        if (l->state != 2) return -EINVAL;
        for (;;) {
            /* 1) Loopback (127.0.0.1) kuyrugu */
            if (l->loop_usock >= 0) {
                usock_t *lu = &s_usocks[l->loop_usock];
                uint64_t fl = sched_irq_save();
                spin_lock(&lu->lock);
                if (lu->bl_count > 0) {
                    int cid = lu->backlog[lu->bl_head];
                    lu->bl_head = (lu->bl_head + 1) % USOCK_BACKLOG;
                    lu->bl_count--;
                    spin_unlock(&lu->lock);
                    sched_irq_restore(fl);

                    int nid = isock_alloc(SOCK_STREAM);
                    if (nid < 0) { usock_close(cid); return -ENFILE; }
                    s_isocks[nid].loop_usock = cid;
                    s_isocks[nid].state = 3;
                    s_isocks[nid].local_port = l->local_port;
                    s_isocks[nid].peer_ip[0] = 127; s_isocks[nid].peer_ip[3] = 1;
                    s_isocks[nid].peer_port = 50000;
                    posix_file_t *nf = posix_file_alloc(PFD_SOCK_INET);
                    if (!nf) { isock_close(nid); return -ENFILE; }
                    nf->obj_id = nid;
                    nf->flags = O_RDWR | ((flags & SOCK_NONBLOCK) ? O_NONBLOCK : 0);
                    int nfd = posix_fd_install(p, nf, (flags & SOCK_CLOEXEC) ? FD_CLOEXEC : 0);
                    if (nfd < 0) { posix_file_unref(nf); return nfd; }
                    if (addr_va && len_va) {
                        posix_sockaddr_in_t sin;
                        memset(&sin, 0, sizeof(sin));
                        sin.sin_family = AF_INET;
                        sin.sin_port = bswap16(s_isocks[nid].peer_port);
                        memcpy(&sin.sin_addr, s_isocks[nid].peer_ip, 4);
                        uint32_t sl = sizeof(sin);
                        posix_ucopy_out(addr_va, &sin, sizeof(sin));
                        posix_ucopy_out(len_va, &sl, 4);
                    }
                    return nfd;
                }
                spin_unlock(&lu->lock);
                sched_irq_restore(fl);
            }
            /* 2) Dis TCP baglantisi */
            if (l->tcp_id >= 0) {
                ipv4_poll();
                int tc = tcp_accept(l->tcp_id);
                if (tc >= 0) {
                    int nid = isock_alloc(SOCK_STREAM);
                    if (nid < 0) { tcp_close(tc); return -ENFILE; }
                    s_isocks[nid].tcp_id = tc;
                    s_isocks[nid].state = 3;
                    s_isocks[nid].local_port = l->local_port;
                    posix_file_t *nf = posix_file_alloc(PFD_SOCK_INET);
                    if (!nf) { isock_close(nid); return -ENFILE; }
                    nf->obj_id = nid;
                    nf->flags = O_RDWR | ((flags & SOCK_NONBLOCK) ? O_NONBLOCK : 0);
                    int nfd = posix_fd_install(p, nf, (flags & SOCK_CLOEXEC) ? FD_CLOEXEC : 0);
                    if (nfd < 0) posix_file_unref(nf);
                    return nfd;
                }
            }
            if (nb) return -EAGAIN;
            sched_sleep_ms(2);
        }
    }
    return -ENOTSOCK;
}

int64_t posix_sys_connect(posix_proc_t *p, int fd, uint64_t addr_va, uint32_t addrlen) {
    posix_file_t *f = posix_fd_get(p, fd);
    if (!f) return -EBADF;

    if (f->kind == PFD_SOCK_UNIX) {
        if (addrlen < 4) return -EINVAL;
        posix_sockaddr_un_t sun;
        memset(&sun, 0, sizeof(sun));
        uint32_t cp = (addrlen > sizeof(sun)) ? sizeof(sun) : addrlen;
        if (posix_ucopy_in(&sun, addr_va, cp) < 0) return -EFAULT;
        if (sun.sun_family != AF_UNIX) return -EAFNOSUPPORT;

        /* Dinleyen AF_UNIX soketini bul */
        int lid = -1;
        for (int i = 0; i < POSIX_MAX_USOCKS; i++) {
            if (s_usocks[i].used && s_usocks[i].state == 2 &&
                strcmp(s_usocks[i].path, sun.sun_path) == 0) {
                lid = i;
                break;
            }
        }
        if (lid < 0) return -ECONNREFUSED;

        int srv_end = usock_alloc(s_usocks[f->obj_id].type);
        if (srv_end < 0) return srv_end;

        s_usocks[f->obj_id].peer_id = srv_end;
        s_usocks[f->obj_id].state = 3;
        s_usocks[srv_end].peer_id = f->obj_id;
        s_usocks[srv_end].state = 3;

        usock_t *l = &s_usocks[lid];
        uint64_t fl = sched_irq_save();
        spin_lock(&l->lock);
        if (l->bl_count >= USOCK_BACKLOG) {
            spin_unlock(&l->lock);
            sched_irq_restore(fl);
            usock_close(srv_end);
            return -ECONNREFUSED;
        }
        l->backlog[(l->bl_head + l->bl_count) % USOCK_BACKLOG] = srv_end;
        l->bl_count++;
        int w = waitq_pop(&l->rd_wq);
        spin_unlock(&l->lock);
        if (w >= 0) sched_wake(w);
        sched_irq_restore(fl);
        return 0;
    }

    if (f->kind == PFD_SOCK_INET) {
        if (addrlen < sizeof(posix_sockaddr_in_t)) return -EINVAL;
        posix_sockaddr_in_t sin;
        if (posix_ucopy_in(&sin, addr_va, sizeof(sin)) < 0) return -EFAULT;
        if (sin.sin_family != AF_INET) return -EAFNOSUPPORT;
        isock_t *s = &s_isocks[f->obj_id];
        memcpy(s->peer_ip, &sin.sin_addr, 4);
        s->peer_port = bswap16(sin.sin_port);

        if (s->type == SOCK_DGRAM) {
            if (s->local_port == 0) {
                s->local_port = isock_alloc_port();
                udp_bind(s->local_port, isock_udp_cb);
            }
            s->state = 3;
            return 0;
        }

        /* 127.0.0.1 loopback TCP baglantisi */
        if (s->peer_ip[0] == 127) {
            int lid = -1;
            for (int i = 0; i < POSIX_MAX_ISOCKS; i++) {
                if (s_isocks[i].used && s_isocks[i].state == 2 &&
                    s_isocks[i].local_port == s->peer_port && s_isocks[i].loop_usock >= 0) {
                    lid = i;
                    break;
                }
            }
            if (lid < 0) return -ECONNREFUSED;
            int cli_u = usock_alloc(SOCK_STREAM);
            int srv_u = (cli_u >= 0) ? usock_alloc(SOCK_STREAM) : -ENFILE;
            if (cli_u < 0 || srv_u < 0) {
                if (cli_u >= 0) usock_close(cli_u);
                return -ENFILE;
            }
            s_usocks[cli_u].peer_id = srv_u; s_usocks[cli_u].state = 3;
            s_usocks[srv_u].peer_id = cli_u; s_usocks[srv_u].state = 3;
            s->loop_usock = cli_u;
            s->state = 3;

            usock_t *lu = &s_usocks[s_isocks[lid].loop_usock];
            uint64_t fl = sched_irq_save();
            spin_lock(&lu->lock);
            if (lu->bl_count < USOCK_BACKLOG) {
                lu->backlog[(lu->bl_head + lu->bl_count) % USOCK_BACKLOG] = srv_u;
                lu->bl_count++;
            }
            spin_unlock(&lu->lock);
            sched_irq_restore(fl);
            return 0;
        }

        ensure_cpu0_for_net();
        int tc = tcp_connect(s->peer_ip, s->peer_port);
        if (tc < 0) return -ECONNREFUSED;
        s->tcp_id = tc;
        s->state = 3;
        return 0;
    }
    return -ENOTSOCK;
}

int64_t posix_sys_sendto(posix_proc_t *p, int fd, uint64_t buf_va, uint64_t len,
                         int flags, uint64_t addr_va, uint32_t addrlen) {
    posix_file_t *f = posix_fd_get(p, fd);
    if (!f) return -EBADF;
    if (len > (256ull * 1024ull)) len = 256ull * 1024ull;
    if (len > 0 && !posix_uprobe(buf_va, len, 0)) return -EFAULT;

    uint8_t *kbuf = len ? (uint8_t *)kmalloc((size_t)len) : 0;
    if (len && !kbuf) return -ENOMEM;
    if (len) posix_ucopy_in(kbuf, buf_va, len);

    int64_t ret;
    if (f->kind == PFD_SOCK_INET && s_isocks[f->obj_id].type == SOCK_DGRAM && addr_va &&
        addrlen >= sizeof(posix_sockaddr_in_t)) {
        posix_sockaddr_in_t sin;
        if (posix_ucopy_in(&sin, addr_va, sizeof(sin)) < 0) { if (kbuf) kfree(kbuf); return -EFAULT; }
        isock_t *s = &s_isocks[f->obj_id];
        if (s->local_port == 0) {
            s->local_port = isock_alloc_port();
            udp_bind(s->local_port, isock_udp_cb);
        }
        uint8_t dip[4];
        memcpy(dip, &sin.sin_addr, 4);
        uint16_t dport = bswap16(sin.sin_port);
        if (len > 1400) ret = -EMSGSIZE;
        else if (dip[0] == 127) {
            isock_udp_cb(dip, s->local_port, dport, kbuf, (uint16_t)len);
            ret = (int64_t)len;
        } else {
            ensure_cpu0_for_net();
            ret = udp_send(dip, s->local_port, dport, kbuf, (uint16_t)len) ? (int64_t)len : -EIO;
        }
    } else {
        ret = posix_sock_write(f, kbuf, len, flags);
    }
    if (kbuf) kfree(kbuf);
    return ret;
}

int64_t posix_sys_recvfrom(posix_proc_t *p, int fd, uint64_t buf_va, uint64_t len,
                           int flags, uint64_t addr_va, uint64_t len_va) {
    posix_file_t *f = posix_fd_get(p, fd);
    if (!f) return -EBADF;
    if (len > (256ull * 1024ull)) len = 256ull * 1024ull;
    if (len > 0 && !posix_uprobe(buf_va, len, 1)) return -EFAULT;

    uint8_t *kbuf = len ? (uint8_t *)kmalloc((size_t)len) : 0;
    if (len && !kbuf) return -ENOMEM;

    int nb = ((f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) ? 1 : 0;
    int64_t n;

    if (f->kind == PFD_SOCK_INET && s_isocks[f->obj_id].type == SOCK_DGRAM) {
        isock_t *s = &s_isocks[f->obj_id];
        ensure_cpu0_for_net();
        uint8_t sip[4] = {0}; uint16_t sport = 0;
        for (;;) {
            ipv4_poll();
            uint64_t fl = sched_irq_save();
            spin_lock(&s->udp_lock);
            if (s->u_count > 0) {
                uint16_t r = s->u_len[s->u_head];
                if (r > len) r = (uint16_t)len;
                memcpy(kbuf, s->u_buf[s->u_head], r);
                memcpy(sip, s->u_sip[s->u_head], 4);
                sport = s->u_sport[s->u_head];
                kfree(s->u_buf[s->u_head]);
                s->u_buf[s->u_head] = 0;
                s->u_head = (uint16_t)((s->u_head + 1u) % ISOCK_UDP_Q);
                s->u_count--;
                spin_unlock(&s->udp_lock);
                sched_irq_restore(fl);
                n = r;
                break;
            }
            spin_unlock(&s->udp_lock);
            sched_irq_restore(fl);
            if (nb) { n = -EAGAIN; break; }
            sched_sleep_ms(1);
        }
        if (n >= 0 && addr_va && len_va) {
            posix_sockaddr_in_t sin;
            memset(&sin, 0, sizeof(sin));
            sin.sin_family = AF_INET;
            sin.sin_port = bswap16(sport);
            memcpy(&sin.sin_addr, sip, 4);
            uint32_t sl = sizeof(sin);
            posix_ucopy_out(addr_va, &sin, sizeof(sin));
            posix_ucopy_out(len_va, &sl, 4);
        }
    } else {
        n = posix_sock_read(f, kbuf, len, flags);
    }

    if (n > 0) posix_ucopy_out(buf_va, kbuf, (uint64_t)n);
    if (kbuf) kfree(kbuf);
    return n;
}

/* sendmsg / recvmsg: AF_UNIX SCM_RIGHTS + AF_INET UDP msg_name destekli! */
int64_t posix_sys_sendmsg(posix_proc_t *p, int fd, uint64_t msg_va, int flags) {
    posix_file_t *f = posix_fd_get(p, fd);
    if (!f) return -EBADF;
    posix_msghdr_t msg;
    if (posix_ucopy_in(&msg, msg_va, sizeof(msg)) < 0) return -EFAULT;

    posix_file_t *send_fds[USOCK_MAX_FDS];
    int nfds = 0;
    if (f->kind == PFD_SOCK_UNIX && msg.msg_control && msg.msg_controllen >= sizeof(posix_cmsghdr_t)) {
        uint8_t cbuf[256];
        uint64_t clen = (msg.msg_controllen > sizeof(cbuf)) ? sizeof(cbuf) : msg.msg_controllen;
        if (posix_ucopy_in(cbuf, (uint64_t)(uintptr_t)msg.msg_control, clen) == 0) {
            posix_cmsghdr_t *ch = (posix_cmsghdr_t *)cbuf;
            if (ch->cmsg_level == SOL_SOCKET && ch->cmsg_type == SCM_RIGHTS &&
                ch->cmsg_len > sizeof(posix_cmsghdr_t)) {
                int cnt = (int)((ch->cmsg_len - sizeof(posix_cmsghdr_t)) / 4);
                int32_t *u_fds = (int32_t *)(cbuf + sizeof(posix_cmsghdr_t));
                for (int i = 0; i < cnt && nfds < USOCK_MAX_FDS; i++) {
                    posix_file_t *sf = posix_fd_get(p, u_fds[i]);
                    if (sf) send_fds[nfds++] = sf;
                }
            }
        }
    }

    uint64_t iovcnt = msg.msg_iovlen > 64 ? 64 : msg.msg_iovlen;
    posix_iovec_t iovs[64];
    uint64_t total_len = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        if (posix_ucopy_in(&iovs[i], (uint64_t)(uintptr_t)msg.msg_iov + i * sizeof(iovs[0]), sizeof(iovs[0])) < 0)
            return -EFAULT;
        if (iovs[i].iov_len > (256ull * 1024ull) - total_len)
            iovs[i].iov_len = (256ull * 1024ull) - total_len;
        total_len += iovs[i].iov_len;
    }

    if (f->kind == PFD_SOCK_INET && s_isocks[f->obj_id].type == SOCK_DGRAM) {
        if (total_len > 1400) return -EMSGSIZE;
        uint8_t *kbuf = total_len ? (uint8_t *)kmalloc((size_t)total_len) : 0;
        if (total_len && !kbuf) return -ENOMEM;
        uint64_t off = 0;
        for (uint64_t i = 0; i < iovcnt; i++) {
            if (iovs[i].iov_len == 0) continue;
            if (posix_ucopy_in(kbuf + off, (uint64_t)(uintptr_t)iovs[i].iov_base, iovs[i].iov_len) < 0) {
                kfree(kbuf);
                return -EFAULT;
            }
            off += iovs[i].iov_len;
        }
        int64_t ret;
        if (msg.msg_name && msg.msg_namelen >= sizeof(posix_sockaddr_in_t)) {
            posix_sockaddr_in_t sin;
            if (posix_ucopy_in(&sin, (uint64_t)(uintptr_t)msg.msg_name, sizeof(sin)) < 0) {
                if (kbuf) kfree(kbuf);
                return -EFAULT;
            }
            if (sin.sin_family != AF_INET) {
                if (kbuf) kfree(kbuf);
                return -EAFNOSUPPORT;
            }
            isock_t *s = &s_isocks[f->obj_id];
            if (s->local_port == 0) {
                s->local_port = isock_alloc_port();
                udp_bind(s->local_port, isock_udp_cb);
            }
            uint8_t dip[4];
            memcpy(dip, &sin.sin_addr, 4);
            uint16_t dport = bswap16(sin.sin_port);
            if (dip[0] == 127) {
                isock_udp_cb(dip, s->local_port, dport, kbuf, (uint16_t)total_len);
                ret = (int64_t)total_len;
            } else {
                ensure_cpu0_for_net();
                ret = udp_send(dip, s->local_port, dport, kbuf, (uint16_t)total_len)
                    ? (int64_t)total_len : -EIO;
            }
        } else {
            ret = posix_sock_write(f, kbuf, total_len, flags);
        }
        if (kbuf) kfree(kbuf);
        return ret;
    }

    int64_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        uint8_t *kbuf = iovs[i].iov_len ? (uint8_t *)kmalloc((size_t)iovs[i].iov_len) : 0;
        if (iovs[i].iov_len && !kbuf) return total ? total : -ENOMEM;
        if (iovs[i].iov_len &&
            posix_ucopy_in(kbuf, (uint64_t)(uintptr_t)iovs[i].iov_base, iovs[i].iov_len) < 0) {
            kfree(kbuf);
            return total ? total : -EFAULT;
        }
        int nb = ((f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) ? 1 : 0;
        int64_t r;
        if (f->kind == PFD_SOCK_UNIX) {
            r = usock_write_raw(f->obj_id, kbuf, iovs[i].iov_len, nb, send_fds, nfds);
            nfds = 0;
        } else {
            r = posix_sock_write(f, kbuf, iovs[i].iov_len, flags);
        }
        if (kbuf) kfree(kbuf);
        if (r < 0) return total ? total : r;
        total += r;
    }
    if (f->kind == PFD_SOCK_UNIX && nfds > 0) {
        int nb = ((f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) ? 1 : 0;
        int64_t r = usock_write_raw(f->obj_id, 0, 0, nb, send_fds, nfds);
        if (r < 0 && total == 0) return r;
    }
    return total;
}

int64_t posix_sys_recvmsg(posix_proc_t *p, int fd, uint64_t msg_va, int flags) {
    posix_file_t *f = posix_fd_get(p, fd);
    if (!f) return -EBADF;
    posix_msghdr_t msg;
    if (posix_ucopy_in(&msg, msg_va, sizeof(msg)) < 0) return -EFAULT;

    uint64_t iovcnt = msg.msg_iovlen > 64 ? 64 : msg.msg_iovlen;
    posix_iovec_t iovs[64];
    uint64_t total_cap = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        if (posix_ucopy_in(&iovs[i], (uint64_t)(uintptr_t)msg.msg_iov + i * sizeof(iovs[0]), sizeof(iovs[0])) < 0)
            return -EFAULT;
        if (iovs[i].iov_len > (256ull * 1024ull) - total_cap)
            iovs[i].iov_len = (256ull * 1024ull) - total_cap;
        if (iovs[i].iov_len > 0 && !posix_uprobe((uint64_t)(uintptr_t)iovs[i].iov_base, iovs[i].iov_len, 1))
            return -EFAULT;
        total_cap += iovs[i].iov_len;
    }

    uint8_t *kbuf = total_cap ? (uint8_t *)kmalloc((size_t)total_cap) : 0;
    if (total_cap && !kbuf) return -ENOMEM;

    posix_file_t *got_files[USOCK_MAX_FDS];
    int got_n = 0;
    int nb = ((f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) ? 1 : 0;
    int64_t n;
    int msg_flags_out = 0;

    if (f->kind == PFD_SOCK_UNIX) {
        n = usock_read_raw(f->obj_id, kbuf, total_cap, nb, got_files, USOCK_MAX_FDS, &got_n);
    } else if (f->kind == PFD_SOCK_INET && s_isocks[f->obj_id].type == SOCK_DGRAM) {
        isock_t *s = &s_isocks[f->obj_id];
        ensure_cpu0_for_net();
        uint8_t sip[4] = {0};
        uint16_t sport = 0;
        uint16_t orig_len = 0;
        for (;;) {
            ipv4_poll();
            uint64_t fl = sched_irq_save();
            spin_lock(&s->udp_lock);
            if (s->u_count > 0) {
                orig_len = s->u_len[s->u_head];
                uint16_t r = orig_len;
                if (r > total_cap) {
                    r = (uint16_t)total_cap;
                    msg_flags_out |= 0x20; /* MSG_TRUNC */
                }
                if (r > 0 && kbuf) memcpy(kbuf, s->u_buf[s->u_head], r);
                memcpy(sip, s->u_sip[s->u_head], 4);
                sport = s->u_sport[s->u_head];
                kfree(s->u_buf[s->u_head]);
                s->u_buf[s->u_head] = 0;
                s->u_head = (uint16_t)((s->u_head + 1u) % ISOCK_UDP_Q);
                s->u_count--;
                spin_unlock(&s->udp_lock);
                sched_irq_restore(fl);
                n = r;
                break;
            }
            spin_unlock(&s->udp_lock);
            sched_irq_restore(fl);
            if (nb) { n = -EAGAIN; break; }
            sched_sleep_ms(1);
        }
        if (n >= 0 && msg.msg_name && msg.msg_namelen >= sizeof(posix_sockaddr_in_t)) {
            posix_sockaddr_in_t sin;
            memset(&sin, 0, sizeof(sin));
            sin.sin_family = AF_INET;
            sin.sin_port = bswap16(sport);
            memcpy(&sin.sin_addr, sip, 4);
            posix_ucopy_out((uint64_t)(uintptr_t)msg.msg_name, &sin, sizeof(sin));
            msg.msg_namelen = sizeof(sin);
        } else if (n >= 0 && msg.msg_name) {
            msg.msg_namelen = 0;
        }
    } else {
        n = posix_sock_read(f, kbuf, total_cap, flags);
        if (n >= 0 && msg.msg_name) msg.msg_namelen = 0;
    }

    if (n > 0 && kbuf) {
        uint64_t rem = (uint64_t)n;
        uint64_t off = 0;
        for (uint64_t i = 0; i < iovcnt && rem > 0; i++) {
            uint64_t ch = (iovs[i].iov_len < rem) ? iovs[i].iov_len : rem;
            if (ch > 0) posix_ucopy_out((uint64_t)(uintptr_t)iovs[i].iov_base, kbuf + off, ch);
            off += ch;
            rem -= ch;
        }
    }
    if (kbuf) kfree(kbuf);

    if (n >= 0) {
        msg.msg_flags = msg_flags_out;
        if (got_n > 0 && msg.msg_control && msg.msg_controllen >= sizeof(posix_cmsghdr_t) + 4) {
            uint8_t cbuf[256];
            memset(cbuf, 0, sizeof(cbuf));
            posix_cmsghdr_t *ch = (posix_cmsghdr_t *)cbuf;
            ch->cmsg_level = SOL_SOCKET;
            ch->cmsg_type = SCM_RIGHTS;
            int32_t *out_fds = (int32_t *)(cbuf + sizeof(posix_cmsghdr_t));
            uint32_t fd_fl = (flags & 0x40000000 /* MSG_CMSG_CLOEXEC */) ? FD_CLOEXEC : 0;
            int placed = 0;
            for (int i = 0; i < got_n && sizeof(posix_cmsghdr_t) + (size_t)(placed + 1) * 4 <= msg.msg_controllen; i++) {
                int nfd = posix_fd_install(p, got_files[i], fd_fl);
                if (nfd >= 0) out_fds[placed++] = nfd;
                else posix_file_unref(got_files[i]);
            }
            ch->cmsg_len = sizeof(posix_cmsghdr_t) + (uint64_t)placed * 4;
            uint64_t tot = (ch->cmsg_len + 7ull) & ~7ull;
            posix_ucopy_out((uint64_t)(uintptr_t)msg.msg_control, cbuf, tot);
            msg.msg_controllen = tot;
        } else {
            for (int i = 0; i < got_n; i++) posix_file_unref(got_files[i]);
            msg.msg_controllen = 0;
        }
        posix_ucopy_out(msg_va, &msg, sizeof(msg));
    } else {
        for (int i = 0; i < got_n; i++) posix_file_unref(got_files[i]);
    }
    return n;
}

int64_t posix_sys_shutdown(posix_proc_t *p, int fd, int how) {
    (void)how;
    posix_file_t *f = posix_fd_get(p, fd);
    if (!f) return -EBADF;
    if (f->kind != PFD_SOCK_INET && f->kind != PFD_SOCK_UNIX) return -ENOTSOCK;
    return 0;
}

int64_t posix_sys_setsockopt(posix_proc_t *p, int fd, int level, int optname,
                             uint64_t optval_va, uint32_t optlen) {
    posix_file_t *f = posix_fd_get(p, fd);
    if (!f) return -EBADF;
    if (f->kind != PFD_SOCK_INET && f->kind != PFD_SOCK_UNIX) return -ENOTSOCK;
    int val = 0;
    if (optval_va && optlen >= 4) posix_ucopy_in(&val, optval_va, 4);
    if (f->kind == PFD_SOCK_INET) {
        if (level == SOL_SOCKET && optname == SO_REUSEADDR) s_isocks[f->obj_id].so_reuseaddr = val;
        if (level == IPPROTO_TCP && optname == TCP_NODELAY) s_isocks[f->obj_id].tcp_nodelay = val;
    }
    return 0;
}

int64_t posix_sys_getsockopt(posix_proc_t *p, int fd, int level, int optname,
                             uint64_t optval_va, uint64_t optlen_va) {
    posix_file_t *f = posix_fd_get(p, fd);
    if (!f) return -EBADF;
    if (f->kind != PFD_SOCK_INET && f->kind != PFD_SOCK_UNIX) return -ENOTSOCK;
    int val = 0;
    if (level == SOL_SOCKET) {
        if (optname == SO_TYPE) {
            val = (f->kind == PFD_SOCK_INET) ? s_isocks[f->obj_id].type : s_usocks[f->obj_id].type;
        } else if (optname == SO_ERROR) {
            if (f->kind == PFD_SOCK_INET) {
                val = s_isocks[f->obj_id].so_error;
                s_isocks[f->obj_id].so_error = 0;
            }
        } else if (optname == SO_DOMAIN) {
            val = (f->kind == PFD_SOCK_INET) ? AF_INET : AF_UNIX;
        } else if (optname == SO_PROTOCOL) {
            if (f->kind == PFD_SOCK_INET)
                val = (s_isocks[f->obj_id].type == SOCK_DGRAM) ? 17 : 6;
        } else if (optname == SO_REUSEADDR) {
            val = (f->kind == PFD_SOCK_INET) ? s_isocks[f->obj_id].so_reuseaddr : 0;
        } else if (optname == SO_SNDBUF || optname == SO_RCVBUF) {
            val = 65536;
        }
    } else if (level == IPPROTO_TCP && optname == TCP_NODELAY) {
        val = (f->kind == PFD_SOCK_INET) ? s_isocks[f->obj_id].tcp_nodelay : 0;
    }
    uint32_t len = 4;
    if (optval_va && posix_ucopy_out(optval_va, &val, 4) < 0) return -EFAULT;
    if (optlen_va && posix_ucopy_out(optlen_va, &len, 4) < 0) return -EFAULT;
    return 0;
}

int64_t posix_sys_getsockname(posix_proc_t *p, int fd, uint64_t addr_va, uint64_t len_va, int peer) {
    posix_file_t *f = posix_fd_get(p, fd);
    if (!f) return -EBADF;
    if (f->kind == PFD_SOCK_INET) {
        isock_t *s = &s_isocks[f->obj_id];
        posix_sockaddr_in_t sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = bswap16(peer ? s->peer_port : s->local_port);
        if (peer) {
            memcpy(&sin.sin_addr, s->peer_ip, 4);
        } else if (s->local_ip[0] || s->local_ip[1] || s->local_ip[2] || s->local_ip[3]) {
            memcpy(&sin.sin_addr, s->local_ip, 4);
        } else if (s->state == 3) {
            memcpy(&sin.sin_addr, ipv4_config.src_ip, 4);
        }
        uint32_t sl = sizeof(sin);
        if (addr_va) posix_ucopy_out(addr_va, &sin, sizeof(sin));
        if (len_va)  posix_ucopy_out(len_va, &sl, 4);
        return 0;
    }
    if (f->kind == PFD_SOCK_UNIX) {
        posix_sockaddr_un_t sun;
        memset(&sun, 0, sizeof(sun));
        sun.sun_family = AF_UNIX;
        strncpy(sun.sun_path, s_usocks[f->obj_id].path, sizeof(sun.sun_path) - 1);
        uint32_t sl = sizeof(sun);
        if (addr_va) posix_ucopy_out(addr_va, &sun, sizeof(sun));
        if (len_va)  posix_ucopy_out(len_va, &sl, 4);
        return 0;
    }
    return -ENOTSOCK;
}
