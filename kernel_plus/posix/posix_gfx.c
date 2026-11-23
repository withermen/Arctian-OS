/* Ring-3 Grafik & Girdi Koprusu (Item 7).
 *
 * Ring-3 sureci:
 *   1. NR_arctian_gfx_create(title, w, h, flags) -> pencere FD'si alir;
 *      cekirdek w*h*4 baytlik sayfa destekli ARGB8888 SHM tamponu ayirir ve
 *      masaustu broker'ina pencere acma istegi koyar.
 *   2. mmap(NULL, w*h*4, PROT_READ|PROT_WRITE, MAP_SHARED, win_fd, 0) ile
 *      paylasimli ARGB8888 tamponunu kendi adres alanina esler.
 *   3. NR_arctian_gfx_present(win_fd, x, y, w, h) ile kirli dikdortgeni
 *      masaustune bildirir (masaustu yalniz o dikdortgeni kopyalar!).
 *   4. poll()/epoll() + read(win_fd, &ev, sizeof(ev)) veya NR_arctian_gfx_poll
 *      ile fare/klavye/tekerlek/kapanma olaylarini alir.
 */
#include "posix.h"
#include "posix_gfx.h"
#include "clock.h"
#include "serial.h"
#include <string.h>

#define PGFX_MAX_WINS  8
#define PGFX_EV_CAP    64

typedef struct {
    int        used;
    spinlock_t lock;
    char       title[64];
    uint32_t   width;
    uint32_t   height;
    int        shm_id;
    /* Masaustu tarafinda olusturulan pencere durumu */
    volatile int want_open;
    volatile int want_close;
    volatile int dirty;
    int32_t      dx, dy, dw, dh;
    void        *desktop_handle;
    /* Girdi olay halkasi */
    arctian_gfx_event_t ev[PGFX_EV_CAP];
    uint32_t   ev_head;
    uint32_t   ev_tail;
    uint32_t   ev_count;
} pgfx_win_t;

static pgfx_win_t s_wins[PGFX_MAX_WINS];
static spinlock_t s_win_tab_lock;

/* Masaustu tarafi kancalari (desktop/posix_win.c tarafindan kaydedilir) */
typedef void *(*pgfx_open_cb_t)(int win_id, const char *title, uint32_t w, uint32_t h);
typedef void  (*pgfx_blit_cb_t)(void *handle, int win_id, int32_t x, int32_t y, int32_t w, int32_t h);
typedef void  (*pgfx_close_cb_t)(void *handle);

static pgfx_open_cb_t  s_cb_open;
static pgfx_blit_cb_t  s_cb_blit;
static pgfx_close_cb_t s_cb_close;

void posix_gfx_register_desktop(pgfx_open_cb_t on_open, pgfx_blit_cb_t on_blit,
                                pgfx_close_cb_t on_close) {
    s_cb_open  = on_open;
    s_cb_blit  = on_blit;
    s_cb_close = on_close;
}

void posix_gfx_init(void) {
    memset(s_wins, 0, sizeof(s_wins));
    spin_init(&s_win_tab_lock);
}

int posix_gfx_shm_id(int obj_id) {
    if (obj_id < 0 || obj_id >= PGFX_MAX_WINS || !s_wins[obj_id].used) return -1;
    return s_wins[obj_id].shm_id;
}

/* Masaustunun paylasimli ARGB8888 tamponundan satir/piksel okuyabilmesi icin */
int posix_gfx_copy_rect(int win_id, uint32_t *dst_argb, uint32_t dst_stride_px,
                        int32_t x, int32_t y, int32_t w, int32_t h) {
    if (win_id < 0 || win_id >= PGFX_MAX_WINS || !s_wins[win_id].used) return -1;
    pgfx_win_t *gw = &s_wins[win_id];
    if (gw->shm_id < 0 || !dst_argb) return -1;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int32_t)gw->width)  w = (int32_t)gw->width - x;
    if (y + h > (int32_t)gw->height) h = (int32_t)gw->height - y;
    if (w <= 0 || h <= 0) return 0;

    for (int32_t row = y; row < y + h; row++) {
        uint64_t byte_off = ((uint64_t)row * gw->width + (uint64_t)x) * 4ull;
        uint64_t rem = (uint64_t)w * 4ull;
        uint8_t *dst = (uint8_t *)(dst_argb + (uint64_t)row * dst_stride_px + (uint64_t)x);
        while (rem > 0) {
            uint64_t pa = posix_shm_frame(gw->shm_id, byte_off >> 12);
            if (!pa) return -1;
            uint64_t po = byte_off & 0xFFFull;
            uint64_t ch = 0x1000ull - po;
            if (ch > rem) ch = rem;
            memcpy(dst, (const void *)(uintptr_t)(pa + po), (size_t)ch);
            dst += ch;
            byte_off += ch;
            rem -= ch;
        }
    }
    return 0;
}

/* Masaustunden Ring-3 penceresine girdi olayi gonder (fare/klavye/tekerlek/kapanma). */
void posix_gfx_push_event(int win_id, const arctian_gfx_event_t *ev) {
    if (win_id < 0 || win_id >= PGFX_MAX_WINS || !ev) return;
    pgfx_win_t *gw = &s_wins[win_id];
    uint64_t fl = sched_irq_save();
    spin_lock(&gw->lock);
    if (gw->used) {
        if (gw->ev_count >= PGFX_EV_CAP) {
            gw->ev_head = (gw->ev_head + 1u) % PGFX_EV_CAP;
            gw->ev_count--;
        }
        gw->ev[gw->ev_tail] = *ev;
        gw->ev_tail = (gw->ev_tail + 1u) % PGFX_EV_CAP;
        gw->ev_count++;
    }
    spin_unlock(&gw->lock);
    sched_irq_restore(fl);
}

uint32_t posix_gfx_poll_mask(int obj_id) {
    if (obj_id < 0 || obj_id >= PGFX_MAX_WINS || !s_wins[obj_id].used) return POLLERR;
    uint32_t m = POLLOUT;
    if (s_wins[obj_id].ev_count > 0) m |= POLLIN;
    return m;
}

int64_t posix_gfx_read_events(int obj_id, uint8_t *dst, uint64_t len, int nonblock) {
    if (obj_id < 0 || obj_id >= PGFX_MAX_WINS || !s_wins[obj_id].used) return -EBADF;
    if (len < sizeof(arctian_gfx_event_t)) return -EINVAL;
    pgfx_win_t *gw = &s_wins[obj_id];

    for (;;) {
        uint64_t fl = sched_irq_save();
        spin_lock(&gw->lock);
        if (gw->ev_count > 0) {
            uint64_t done = 0;
            while (gw->ev_count > 0 && done + sizeof(arctian_gfx_event_t) <= len) {
                memcpy(dst + done, &gw->ev[gw->ev_head], sizeof(arctian_gfx_event_t));
                gw->ev_head = (gw->ev_head + 1u) % PGFX_EV_CAP;
                gw->ev_count--;
                done += sizeof(arctian_gfx_event_t);
            }
            spin_unlock(&gw->lock);
            sched_irq_restore(fl);
            return (int64_t)done;
        }
        spin_unlock(&gw->lock);
        sched_irq_restore(fl);
        if (nonblock) return -EAGAIN;
        sched_sleep_ms(2);
    }
}

void posix_gfx_close_obj(int obj_id) {
    if (obj_id < 0 || obj_id >= PGFX_MAX_WINS) return;
    pgfx_win_t *gw = &s_wins[obj_id];
    uint64_t fl = sched_irq_save();
    spin_lock(&gw->lock);
    if (gw->used) gw->want_close = 1;
    spin_unlock(&gw->lock);
    sched_irq_restore(fl);
}

/* Masaustu ana dongusunden her karede cagirilir (wolf_bridge_drain ile yan yana). */
void posix_gfx_desktop_drain(void) {
    for (int i = 0; i < PGFX_MAX_WINS; i++) {
        pgfx_win_t *gw = &s_wins[i];
        if (!gw->used) continue;

        if (gw->want_close) {
            void *h = gw->desktop_handle;
            int sid = gw->shm_id;
            gw->desktop_handle = 0;
            gw->shm_id = -1;
            gw->want_close = 0;
            gw->used = 0;
            if (h && s_cb_close) s_cb_close(h);
            if (sid >= 0) posix_shm_unref(sid);
            continue;
        }

        if (gw->want_open) {
            gw->want_open = 0;
            if (s_cb_open)
                gw->desktop_handle = s_cb_open(i, gw->title, gw->width, gw->height);
        }

        if (gw->dirty && gw->desktop_handle && s_cb_blit) {
            int32_t x = gw->dx, y = gw->dy, w = gw->dw, h = gw->dh;
            gw->dirty = 0;
            s_cb_blit(gw->desktop_handle, i, x, y, w, h);
        }
    }
}

/* ---- Syscall'lar (500..504) ---- */
int64_t posix_sys_gfx_create(posix_proc_t *p, uint64_t title_va, uint32_t w, uint32_t h, uint32_t flags) {
    (void)flags;
    if (w == 0 || h == 0 || w > 1920 || h > 1080) return -EINVAL;
    char title[64];
    if (title_va && posix_ucstr_in(title, sizeof(title), title_va) == 0) {
        /* ok */
    } else {
        strcpy(title, "Arctian Ring-3");
    }

    uint64_t fb_bytes = (uint64_t)w * (uint64_t)h * 4ull;
    int sid = posix_shm_create(fb_bytes);
    if (sid < 0) return sid;

    uint64_t fl = sched_irq_save();
    spin_lock(&s_win_tab_lock);
    int wid = -1;
    for (int i = 0; i < PGFX_MAX_WINS; i++) {
        if (!s_wins[i].used) {
            memset(&s_wins[i], 0, sizeof(s_wins[i]));
            s_wins[i].used = 1;
            spin_init(&s_wins[i].lock);
            strncpy(s_wins[i].title, title, sizeof(s_wins[i].title) - 1);
            s_wins[i].width = w;
            s_wins[i].height = h;
            s_wins[i].shm_id = sid;
            s_wins[i].want_open = 1;
            wid = i;
            break;
        }
    }
    spin_unlock(&s_win_tab_lock);
    sched_irq_restore(fl);
    if (wid < 0) { posix_shm_unref(sid); return -ENOSPC; }

    posix_file_t *f = posix_file_alloc(PFD_GFX);
    if (!f) { posix_gfx_close_obj(wid); return -ENFILE; }
    f->obj_id = wid;
    f->flags = O_RDWR;
    int fd = posix_fd_install(p, f, 0);
    if (fd < 0) posix_file_unref(f);
    return fd;
}

int64_t posix_sys_gfx_present(posix_proc_t *p, int fd, int32_t x, int32_t y, int32_t w, int32_t h) {
    posix_file_t *f = posix_fd_get(p, fd);
    if (!f || f->kind != PFD_GFX) return -EBADF;
    pgfx_win_t *gw = &s_wins[f->obj_id];
    if (!gw->used) return -EBADF;
    if (w <= 0 || h <= 0) { x = 0; y = 0; w = (int32_t)gw->width; h = (int32_t)gw->height; }

    uint64_t fl = sched_irq_save();
    spin_lock(&gw->lock);
    if (!gw->dirty) {
        gw->dx = x; gw->dy = y; gw->dw = w; gw->dh = h;
        gw->dirty = 1;
    } else {
        /* Kirli dikdortgenleri birlestir */
        int32_t x0 = (x < gw->dx) ? x : gw->dx;
        int32_t y0 = (y < gw->dy) ? y : gw->dy;
        int32_t x1 = ((x + w) > (gw->dx + gw->dw)) ? (x + w) : (gw->dx + gw->dw);
        int32_t y1 = ((y + h) > (gw->dy + gw->dh)) ? (y + h) : (gw->dy + gw->dh);
        gw->dx = x0; gw->dy = y0; gw->dw = x1 - x0; gw->dh = y1 - y0;
    }
    spin_unlock(&gw->lock);
    sched_irq_restore(fl);
    return 0;
}

int64_t posix_sys_gfx_poll(posix_proc_t *p, int fd, uint64_t ev_va, int timeout_ms) {
    posix_file_t *f = posix_fd_get(p, fd);
    if (!f || f->kind != PFD_GFX) return -EBADF;
    pgfx_win_t *gw = &s_wins[f->obj_id];
    uint64_t deadline = (timeout_ms > 0) ? (clock_ms() + (uint64_t)timeout_ms) : 0;

    for (;;) {
        uint64_t fl = sched_irq_save();
        spin_lock(&gw->lock);
        if (gw->ev_count > 0) {
            arctian_gfx_event_t ev = gw->ev[gw->ev_head];
            gw->ev_head = (gw->ev_head + 1u) % PGFX_EV_CAP;
            gw->ev_count--;
            spin_unlock(&gw->lock);
            sched_irq_restore(fl);
            if (posix_ucopy_out(ev_va, &ev, sizeof(ev)) < 0) return -EFAULT;
            return 1;
        }
        spin_unlock(&gw->lock);
        sched_irq_restore(fl);
        if (timeout_ms == 0) return 0;
        if (timeout_ms > 0 && clock_ms() >= deadline) return 0;
        sched_sleep_ms(2);
    }
}

int64_t posix_sys_gfx_resize(posix_proc_t *p, int fd, uint32_t w, uint32_t h) {
    posix_file_t *f = posix_fd_get(p, fd);
    if (!f || f->kind != PFD_GFX) return -EBADF;
    if (w == 0 || h == 0 || w > 1920 || h > 1080) return -EINVAL;
    pgfx_win_t *gw = &s_wins[f->obj_id];
    int rc = posix_shm_resize(gw->shm_id, (uint64_t)w * (uint64_t)h * 4ull);
    if (rc < 0) return rc;
    gw->width = w;
    gw->height = h;
    return 0;
}

int64_t posix_sys_gfx_destroy(posix_proc_t *p, int fd) {
    return posix_sys_close(p, fd);
}
