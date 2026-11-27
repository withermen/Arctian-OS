/* Masaustu tarafi: Ring-3 POSIX Grafik & Girdi Koprusu (Item 7).
 *
 * Ring-3 surecindeki paylasimli ARGB8888 tamponunu gercek bir masaustu
 * penceresi + lv_canvas uzerine kirli dikdortgen (present_rect) ile aktarir
 * ve fare/klavye/tekerlek/kapanma olaylarini Ring-3 olay kuyruguna iletir.
 */
#include "dsk.h"
#include "dpi.h"
#include "posix_gfx.h"
#include <string.h>

typedef struct {
    int       used;
    int       win_id;
    lv_obj_t *win;
    lv_obj_t *cv;
    uint32_t *buf;
    uint32_t  w;
    uint32_t  h;
    int       last_mx;
    int       last_my;
    int       last_btn;
} posix_win_t;

/* Dinamik POSIX pencere tablosu: sabit 8 siniri yok, yalnizca mevcut RAM ve
 * oom_state() ile sinirlanir. Her slot kendi heap adresini korur ki LVGL
 * silme callback'i ve kernel desktop_handle pointer'lari asla kaymasin. */
static posix_win_t **s_pw = 0;
static int s_pw_cap = 0;

static char     s_last_dclip[4096];
static uint32_t s_last_kseq = 0;
static int      s_clip_synced = 0;

static void pw_clamp_size(int w, int h, uint32_t *out_w, uint32_t *out_h) {
    if (w < 160) w = 160;
    if (h < 120) h = 120;
    if (w > 2560) w = 2560;
    if (h > 1440) h = 1440;
    *out_w = (uint32_t)w;
    *out_h = (uint32_t)h;
}

static posix_win_t *pw_find_by_win(lv_obj_t *win) {
    if (!win || !s_pw) return 0;
    for (int i = 0; i < s_pw_cap; i++) {
        if (s_pw[i] && s_pw[i]->used && s_pw[i]->win == win)
            return s_pw[i];
    }
    return 0;
}

static int pw_streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void pw_sync_clipboard(void) {
    if (!g_asi || !g_asi->posix || !g_asi->posix->clip_seq ||
        !g_asi->posix->clip_get || !g_asi->posix->clip_set)
        return;

    uint32_t kseq = g_asi->posix->clip_seq();
    if (!s_clip_synced) {
        s_clip_synced = 1;
        if (kseq != 0) {
            int n = g_asi->posix->clip_get(s_last_dclip, sizeof(s_last_dclip));
            if (n < 0) s_last_dclip[0] = 0;
            arctian_clipboard_set(s_last_dclip);
            s_last_kseq = kseq;
            return;
        }
        const char *d = arctian_clipboard_get();
        if (!d) d = "";
        size_t dl = strlen(d);
        if (dl >= sizeof(s_last_dclip)) dl = sizeof(s_last_dclip) - 1;
        if (dl > 0) memcpy(s_last_dclip, d, dl);
        s_last_dclip[dl] = 0;
        g_asi->posix->clip_set(s_last_dclip, (uint32_t)dl, &s_last_kseq);
        return;
    }

    if (kseq != s_last_kseq) {
        int n = g_asi->posix->clip_get(s_last_dclip, sizeof(s_last_dclip));
        if (n < 0) s_last_dclip[0] = 0;
        arctian_clipboard_set(s_last_dclip);
        s_last_kseq = kseq;
        return;
    }

    const char *d = arctian_clipboard_get();
    if (!d) d = "";
    if (!pw_streq(d, s_last_dclip)) {
        size_t dl = strlen(d);
        if (dl >= sizeof(s_last_dclip)) dl = sizeof(s_last_dclip) - 1;
        if (dl > 0) memcpy(s_last_dclip, d, dl);
        s_last_dclip[dl] = 0;
        g_asi->posix->clip_set(s_last_dclip, (uint32_t)dl, &s_last_kseq);
    }
}

static void pw_hotkey(int hk) {
    uint32_t ch = 0;
    switch (hk) {
        case 3: ch = 'c'; break;
        case 4: ch = 'x'; break;
        case 5: ch = 'v'; break;
        case 6: ch = 'a'; break;
        case 7: ch = 'z'; break;
        case 8: ch = 'y'; break;
        default: return;
    }
    if (hk == 5) pw_sync_clipboard();
    if (!g_asi || !g_asi->posix || !g_asi->posix->gfx_push_event) return;
    winrec_t *top = winrec_top();
    if (!top || !top->win) return;
    posix_win_t *pw = pw_find_by_win(top->win);
    if (!pw) return;

    arctian_gfx_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ARCTIAN_EV_KEY_DOWN;
    ev.key = ch;
    ev.mods = ARCTIAN_MOD_CTRL;
    g_asi->posix->gfx_push_event(pw->win_id, &ev);

    ev.type = ARCTIAN_EV_KEY_UP;
    g_asi->posix->gfx_push_event(pw->win_id, &ev);
}

static void pw_relayout(lv_obj_t *obj) {
    if (!obj || !g_asi || !g_asi->mem || !g_asi->mem->alloc) return;
    winrec_t *r = winrec_of(obj);
    if (!r || !r->win) return;
    posix_win_t *pw = pw_find_by_win(r->win);
    if (!pw || !pw->cv) return;

    uint32_t new_w = 0, new_h = 0;
    pw_clamp_size(lv_obj_get_width(obj), lv_obj_get_height(obj), &new_w, &new_h);
    if (new_w == pw->w && new_h == pw->h) return;

    size_t buf_bytes = (size_t)new_w * (size_t)new_h * 4u;
    uint32_t *new_buf = (uint32_t *)g_asi->mem->alloc(buf_bytes);
    if (!new_buf) return;
    if (g_asi->mem->memset) g_asi->mem->memset(new_buf, 0, buf_bytes);
    else memset(new_buf, 0, buf_bytes);

    if (pw->buf) {
        uint32_t copy_w = (pw->w < new_w) ? pw->w : new_w;
        uint32_t copy_h = (pw->h < new_h) ? pw->h : new_h;
        for (uint32_t y = 0; y < copy_h; y++) {
            memcpy(new_buf + (size_t)y * new_w,
                   pw->buf + (size_t)y * pw->w,
                   (size_t)copy_w * 4u);
        }
    }

    uint32_t *old_buf = pw->buf;
    lv_obj_set_size(pw->cv, (int32_t)new_w, (int32_t)new_h);
    lv_canvas_set_buffer(pw->cv, new_buf, (int32_t)new_w, (int32_t)new_h, LV_COLOR_FORMAT_ARGB8888);
    pw->buf = new_buf;
    pw->w = new_w;
    pw->h = new_h;
    if (old_buf && g_asi->mem->free) g_asi->mem->free(old_buf);
    lv_obj_invalidate(pw->cv);

    if (g_asi->posix && g_asi->posix->gfx_resize)
        g_asi->posix->gfx_resize(pw->win_id, new_w, new_h);

    if (g_asi->posix && g_asi->posix->gfx_push_event) {
        arctian_gfx_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = ARCTIAN_EV_RESIZE;
        ev.x = (int32_t)new_w;
        ev.y = (int32_t)new_h;
        g_asi->posix->gfx_push_event(pw->win_id, &ev);
    }
}

static posix_win_t *pw_alloc_slot(void) {
    if (!g_asi || !g_asi->mem || !g_asi->mem->alloc) return 0;
    for (int i = 0; i < s_pw_cap; i++) {
        if (s_pw[i] && !s_pw[i]->used) return s_pw[i];
    }
    int new_cap = (s_pw_cap == 0) ? 16 : (s_pw_cap * 2);
    posix_win_t **nt = (posix_win_t **)g_asi->mem->alloc((size_t)new_cap * sizeof(posix_win_t *));
    if (!nt) return 0;
    memset(nt, 0, (size_t)new_cap * sizeof(posix_win_t *));
    for (int i = 0; i < s_pw_cap; i++) nt[i] = s_pw[i];
    if (s_pw && g_asi->mem->free) g_asi->mem->free(s_pw);
    s_pw = nt;
    int idx = s_pw_cap;
    s_pw_cap = new_cap;

    posix_win_t *slot = (posix_win_t *)g_asi->mem->alloc(sizeof(posix_win_t));
    if (!slot) return 0;
    memset(slot, 0, sizeof(*slot));
    s_pw[idx] = slot;
    return slot;
}

static void pw_win_del_cb(lv_event_t *e) {
    posix_win_t *pw = (posix_win_t *)lv_event_get_user_data(e);
    if (!pw || !pw->used) return;
    int wid = pw->win_id;
    uint32_t *b = pw->buf;
    pw->used = 0;
    pw->win = 0;
    pw->cv = 0;
    pw->buf = 0;
    if (b && g_asi && g_asi->mem && g_asi->mem->free)
        g_asi->mem->free(b);
    if (g_asi && g_asi->posix && g_asi->posix->gfx_push_event) {
        arctian_gfx_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = ARCTIAN_EV_CLOSE;
        g_asi->posix->gfx_push_event(wid, &ev);
    }
}

static void *pw_on_open(int win_id, const char *title, uint32_t w, uint32_t h) {
    if (!g_asi || !g_asi->mem || !g_asi->mem->alloc) return 0;
    if (w == 0 || h == 0 || w > 3840 || h > 2160) return 0;
    if (g_asi->proc && g_asi->proc->oom_state &&
        g_asi->proc->oom_state() >= ASI_OOM_CRITICAL)
        return 0;

    posix_win_t *pw = pw_alloc_slot();
    if (!pw) return 0;

    int tbh = dpi_px(36);
    lv_obj_t *win = create_window(title ? title : "Ring-3 App", (int)w + 4, (int)h + tbh + 6);
    if (!win) return 0;
    lv_obj_update_layout(win);

    lv_obj_t *body = lv_obj_create(win);
    if (!body) {
        win_close_rec(winrec_of(win));
        return 0;
    }
    int bw = lv_obj_get_width(win) - 4;
    int bh = lv_obj_get_height(win) - (tbh + 6);
    lv_obj_remove_style_all(body);
    lv_obj_set_style_bg_color(body, lv_color_hex(ADL_SURFACE), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_pos(body, 2, tbh + 2);
    lv_obj_set_size(body, bw, bh);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(body, LV_OBJ_FLAG_USER_2);

    uint32_t cw = w, ch = h;
    if (bw != (int)w || bh != (int)h)
        pw_clamp_size(bw, bh, &cw, &ch);

    size_t buf_bytes = (size_t)cw * (size_t)ch * 4u;
    uint32_t *buf = (uint32_t *)g_asi->mem->alloc(buf_bytes);
    if (!buf) {
        win_close_rec(winrec_of(win));
        return 0;
    }
    if (g_asi->mem->memset) g_asi->mem->memset(buf, 0, buf_bytes);
    else memset(buf, 0, buf_bytes);

    lv_obj_t *cv = lv_canvas_create(body);
    if (!cv) {
        win_close_rec(winrec_of(win));
        g_asi->mem->free(buf);
        return 0;
    }
    lv_obj_set_pos(cv, 0, 0);
    lv_obj_set_size(cv, (int32_t)cw, (int32_t)ch);
    lv_canvas_set_buffer(cv, buf, (int32_t)cw, (int32_t)ch, LV_COLOR_FORMAT_ARGB8888);

    memset(pw, 0, sizeof(*pw));
    pw->used = 1;
    pw->win_id = win_id;
    pw->win = win;
    pw->cv = cv;
    pw->buf = buf;
    pw->w = cw;
    pw->h = ch;
    pw->last_mx = -1;
    pw->last_my = -1;

    win_set_relayout(win, pw_relayout);
    win_set_hotkey(win, pw_hotkey);
    lv_obj_add_event_cb(win, pw_win_del_cb, LV_EVENT_DELETE, pw);

    if (cw != w || ch != h) {
        if (g_asi->posix && g_asi->posix->gfx_resize)
            g_asi->posix->gfx_resize(win_id, cw, ch);
        if (g_asi->posix && g_asi->posix->gfx_push_event) {
            arctian_gfx_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = ARCTIAN_EV_RESIZE;
            ev.x = (int32_t)cw;
            ev.y = (int32_t)ch;
            g_asi->posix->gfx_push_event(win_id, &ev);
        }
    }
    return pw;
}

static void pw_on_blit(void *handle, int win_id, int32_t x, int32_t y, int32_t w, int32_t h) {
    posix_win_t *pw = (posix_win_t *)handle;
    if (!pw || !pw->used || !pw->cv || !pw->buf) return;
    if (!g_asi || !g_asi->posix || !g_asi->posix->gfx_copy_rect) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int32_t)pw->w) w = (int32_t)pw->w - x;
    if (y + h > (int32_t)pw->h) h = (int32_t)pw->h - y;
    if (w <= 0 || h <= 0) return;

    if (g_asi->posix->gfx_copy_rect(win_id, pw->buf, pw->w, x, y, w, h) == 0) {
        if (pw->win && lv_obj_has_flag(pw->win, LV_OBJ_FLAG_HIDDEN)) return;
        lv_area_t a;
        lv_obj_get_coords(pw->cv, &a);
        lv_area_t dirty;
        dirty.x1 = a.x1 + x;
        dirty.y1 = a.y1 + y;
        dirty.x2 = a.x1 + x + w - 1;
        dirty.y2 = a.y1 + y + h - 1;
        int sw = (g_asi->gfx && g_asi->gfx->width) ? g_asi->gfx->width() : 3840;
        int sh = (g_asi->gfx && g_asi->gfx->height) ? g_asi->gfx->height() : 2160;
        if (dirty.x1 < 0) dirty.x1 = 0;
        if (dirty.y1 < 0) dirty.y1 = 0;
        if (dirty.x2 >= sw) dirty.x2 = sw - 1;
        if (dirty.y2 >= sh) dirty.y2 = sh - 1;
        if (dirty.x1 <= dirty.x2 && dirty.y1 <= dirty.y2)
            lv_obj_invalidate_area(pw->cv, &dirty);
    }
}

static void pw_on_close(void *handle) {
    posix_win_t *pw = (posix_win_t *)handle;
    if (!pw || !pw->used) return;
    lv_obj_t *win = pw->win;
    if (win) {
        winrec_t *r = winrec_of(win);
        if (r) {
            win_close_rec(r);
            return;
        }
    }
    uint32_t *b = pw->buf;
    pw->used = 0;
    pw->win = 0;
    pw->cv = 0;
    pw->buf = 0;
    if (b && g_asi && g_asi->mem && g_asi->mem->free)
        g_asi->mem->free(b);
}

void posix_win_init(void) {
    for (int i = 0; i < s_pw_cap; i++) {
        if (s_pw[i]) s_pw[i]->used = 0;
    }
    if (g_asi && g_asi->posix && g_asi->posix->gfx_register_desktop)
        g_asi->posix->gfx_register_desktop(pw_on_open, pw_on_blit, pw_on_close);
}

int posix_win_ram_kb(lv_obj_t *win) {
    if (!win || !s_pw) return 0;
    for (int i = 0; i < s_pw_cap; i++) {
        posix_win_t *pw = s_pw[i];
        if (pw && pw->used && pw->win == win) {
            /* Masaustu canvas + Ring-3 SHM cift tampon bellegi (KB) */
            uint64_t bytes = (uint64_t)pw->w * (uint64_t)pw->h * 8ull;
            int kb = (int)(bytes / 1024ull);
            return kb > 0 ? kb : 64;
        }
    }
    return 0;
}

int posix_win_count(void) {
    int n = 0;
    if (!s_pw) return 0;
    for (int i = 0; i < s_pw_cap; i++) {
        if (s_pw[i] && s_pw[i]->used) n++;
    }
    return n;
}

void posix_win_poll_input(void) {
    if (!g_asi || !g_asi->posix)
        return;

    pw_sync_clipboard();

    if (!g_asi->input || !g_asi->posix->gfx_push_event)
        return;
    if (!s_pw) return;

    /* En ust pencere bir POSIX Ring-3 penceresiyse girdi olaylarini ilet */
    winrec_t *top = winrec_top();
    if (!top || !top->win) return;

    posix_win_t *pw = pw_find_by_win(top->win);
    if (!pw || !pw->cv) return;

    lv_area_t a;
    lv_obj_get_coords(pw->cv, &a);
    int mx = g_asi->input->mouse_x ? g_asi->input->mouse_x() : 0;
    int my = g_asi->input->mouse_y ? g_asi->input->mouse_y() : 0;
    int btn = g_asi->input->mouse_buttons ? g_asi->input->mouse_buttons() : 0;
    int mods = ((g_asi->input->shift_down && g_asi->input->shift_down()) ? 1 : 0) |
               ((g_asi->input->ctrl_down && g_asi->input->ctrl_down()) ? 2 : 0);
    int rx = mx - a.x1;
    int ry = my - a.y1;
    int inside = (rx >= 0 && ry >= 0 && rx < (int)pw->w && ry < (int)pw->h);

    if (inside && (rx != pw->last_mx || ry != pw->last_my)) {
        arctian_gfx_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = ARCTIAN_EV_MOUSE_MOVE;
        ev.x = rx; ev.y = ry;
        ev.delta = btn;
        ev.mods = (uint32_t)mods;
        g_asi->posix->gfx_push_event(pw->win_id, &ev);
        pw->last_mx = rx;
        pw->last_my = ry;
    }

    if (inside && btn != pw->last_btn) {
        int diff = btn ^ pw->last_btn;
        for (int b = 0; b < 3; b++) {
            if (diff & (1 << b)) {
                arctian_gfx_event_t ev;
                memset(&ev, 0, sizeof(ev));
                ev.type = (btn & (1 << b)) ? ARCTIAN_EV_MOUSE_DOWN : ARCTIAN_EV_MOUSE_UP;
                ev.x = rx; ev.y = ry;
                ev.delta = b + 1;
                ev.mods = (uint32_t)mods;
                g_asi->posix->gfx_push_event(pw->win_id, &ev);
            }
        }
        pw->last_btn = btn;
    }

    if (inside && g_asi->input->mouse_scroll) {
        int sc = g_asi->input->mouse_scroll();
        if (sc != 0) {
            arctian_gfx_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = ARCTIAN_EV_MOUSE_WHEEL;
            ev.x = rx; ev.y = ry;
            ev.delta = sc;
            ev.mods = (uint32_t)mods;
            g_asi->posix->gfx_push_event(pw->win_id, &ev);
        }
    }

    if (g_asi->input->key_avail && (g_asi->input->key_pop_cp || g_asi->input->key_pop)) {
        while (g_asi->input->key_avail()) {
            uint32_t cp = 0;
            if (g_asi->input->key_pop_cp) {
                cp = g_asi->input->key_pop_cp();
                if (cp == 0) break;
            } else {
                int ch = g_asi->input->key_pop();
                if (ch < 0) break;
                cp = (uint32_t)ch;
            }
            arctian_gfx_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = ARCTIAN_EV_KEY_DOWN;
            ev.key = cp;
            ev.mods = (uint32_t)mods;
            g_asi->posix->gfx_push_event(pw->win_id, &ev);
        }
    }
}
