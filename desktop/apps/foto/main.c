/* ==================================================================
 * Fotoğraf Görüntüleyici (PNG, JPEG, GIF, BMP)
 *
 * Özellikler:
 *   - Çoklu biçim çözme (lib/imgdec.h): PNG, JPEG, GIF, BMP
 *   - 16 MB'a kadar dinamik bellek ayırma (asi->mem->alloc)
 *   - Yakınlaştırma (+ / - / %100 / Sığdır), 90° Döndür, Slayt Gösterisi
 *   - Sol/Sağ ok tuşları ve butonlarla önceki/sonraki fotoğraf
 *   - relayout desteği ile yeniden boyutlandırma uyumu
 * ================================================================== */
#include "app.h"
#include "adl_wolf.h"
#include "lv_port.h"
#include "imgdec.h"
#include "svg.h"
#include <stdint.h>

#define FT_MAX      48
#define FT_PATH     200
#define FT_NAME     64
#define FT_MAXBYTES (16u * 1024u * 1024u)

#define C_VIEW   0x0B0C0F
#define C_BODY   0x14161B
#define C_BAR    0x181B22
#define C_BTN    0x232833
#define C_LINE   0x2C313B
#define C_PH     0x1A2233
#define C_TXT    0xE8EAEE
#define C_DIM    0x8B919C
#define C_ACC    0x3B82F6
#define C_BTNBG  0x14161B

static asi_t      *A;
static lv_obj_t   *g_win;
static lv_obj_t   *g_view_wrap;
static lv_obj_t   *g_view;
static lv_obj_t   *g_ph;
static lv_obj_t   *g_img;
static lv_obj_t   *g_err_lbl;
static lv_obj_t   *g_d_name, *g_d_fmt, *g_d_size, *g_d_res;
static lv_obj_t   *g_d_date, *g_d_zoom, *g_d_rot, *g_status;
static lv_obj_t   *g_zoom_lbl, *g_slide_lbl;
static lv_group_t *g_grp;
static lv_timer_t *g_slide_timer;

static char        g_path[FT_MAX][FT_PATH];
static char        g_name[FT_MAX][FT_NAME];
static int         g_n, g_sel;

/* Orijinal çözülen piksel tamponu + dönüşmüş/ölçeklenmiş görüntü tamponu */
static uint32_t   *g_orig_px;
static int         g_orig_w, g_orig_h;
static const char *g_orig_fmt;
static uint32_t   *g_px;
static int         g_pw, g_ph_w;
static lv_image_dsc_t g_dsc;

static int         g_zoom_pct = 100;
static int         g_fit_mode = 1;    /* 1 = pencereye sığdır, 0 = g_zoom_pct */
static int         g_rot_q    = 0;    /* 0..3 -> 0/90/180/270 derece */
static int         g_slide_on = 0;

static uint32_t   *s_icon_buf[8];
static int         s_icon_n;

extern const unsigned char font_ttf[];
extern const unsigned int  font_ttf_len;
static lv_font_t *f13, *f12;

static int slen(const char *s) { int n = 0; if (s) while (s[n]) n++; return n; }

static void scpy(char *d, int cap, const char *s) {
    int i = 0;
    if (cap <= 0) return;
    if (!s) { d[0] = 0; return; }
    for (; s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}

static void scat(char *d, int cap, const char *s) {
    int i = slen(d), j = 0;
    for (; s && s[j] && i < cap - 1; i++, j++) d[i] = s[j];
    d[i] = 0;
}

static void u2s(unsigned int v, char *out, int cap) {
    char t[12];
    int n = 0, k = 0;
    if (v == 0) t[n++] = '0';
    while (v > 0 && n < (int)sizeof(t)) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n && k < cap - 1) out[k++] = t[--n];
    out[k] = 0;
}

static int ext_eq(const char *name, const char *ext) {
    int nl = slen(name), el = slen(ext);
    if (nl < el + 1) return 0;
    const char *p = name + nl - el;
    if (p[-1] != '.') return 0;
    for (int i = 0; i < el; i++) {
        char c = p[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != ext[i]) return 0;
    }
    return 1;
}

static int is_image_file(const char *name) {
    return ext_eq(name, "png") || ext_eq(name, "jpg") ||
           ext_eq(name, "jpeg") || ext_eq(name, "bmp") || ext_eq(name, "gif");
}

static const char *fmt_from_name(const char *name) {
    if (ext_eq(name, "png")) return "PNG";
    if (ext_eq(name, "jpg") || ext_eq(name, "jpeg")) return "JPEG";
    if (ext_eq(name, "bmp")) return "BMP";
    if (ext_eq(name, "gif")) return "GIF";
    return "-";
}

static void join_path(char *out, int cap, const char *dir, const char *name) {
    scpy(out, cap, dir);
    int L = slen(out);
    if (L == 0) { scpy(out, cap, name); return; }
    if (out[L - 1] != '/' && out[L - 1] != ':') scat(out, cap, "/");
    scat(out, cap, name);
}

static int seq_cmp(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 1; a++; b++; }
    return (int)((unsigned char)*a - (unsigned char)*b);
}

static void scan_dir(const char *dir) {
    if (!A->fs || !A->fs->count_dir || !A->fs->list_dir) return;
    int n = A->fs->count_dir(dir);
    if (n <= 0) return;
    for (int i = 0; i < n && g_n < FT_MAX; i++) {
        char nm[FT_NAME];
        int isd = 0;
        if (A->fs->list_dir(dir, i, nm, sizeof(nm), &isd) != 0) continue;
        if (isd || !is_image_file(nm)) continue;
        char full[FT_PATH];
        join_path(full, FT_PATH, dir, nm);
        int dup = 0;
        for (int k = 0; k < g_n; k++)
            if (!seq_cmp(g_path[k], full)) { dup = 1; break; }
        if (dup) continue;
        scpy(g_path[g_n], FT_PATH, full);
        scpy(g_name[g_n], FT_NAME, nm);
        g_n++;
    }
}

static lv_obj_t *svg_icon(lv_obj_t *parent, const char *svg, int size) {
    if (s_icon_n >= 8 || !A->mem) return 0;
    uint32_t *buf = (uint32_t *)A->mem->alloc((size_t)size * size * 4u);
    if (!buf) return 0;
    if (svg_render(svg, size, size, buf) != 0) { A->mem->free(buf); return 0; }
    s_icon_buf[s_icon_n++] = buf;
    lv_obj_t *c = lv_canvas_create(parent);
    lv_obj_remove_style_all(c);
    lv_canvas_set_buffer(c, buf, size, size, LV_COLOR_FORMAT_ARGB8888);
    return c;
}

static void free_disp_px(void) {
    if (g_px && A->mem) A->mem->free(g_px);
    g_px = 0;
    g_pw = g_ph_w = 0;
}

static void free_orig_px(void) {
    free_disp_px();
    if (g_orig_px && A->mem) A->mem->free(g_orig_px);
    g_orig_px = 0;
    g_orig_w = g_orig_h = 0;
    g_orig_fmt = 0;
}

/* Dönüş + ölçekleme tek geçişte yapılır */
static uint32_t *transform_image(const uint32_t *src, int sw, int sh,
                                 int rot_q, int tw, int th) {
    if (!src || sw <= 0 || sh <= 0 || tw <= 0 || th <= 0) return 0;
    uint32_t *dst = (uint32_t *)A->mem->alloc((size_t)tw * (size_t)th * 4u);
    if (!dst) return 0;
    int rw = (rot_q & 1) ? sh : sw;
    int rh = (rot_q & 1) ? sw : sh;
    for (int y = 0; y < th; y++) {
        int ry = (int)((int64_t)y * rh / th);
        if (ry >= rh) ry = rh - 1;
        uint32_t *dr = dst + (size_t)y * (size_t)tw;
        for (int x = 0; x < tw; x++) {
            int rx = (int)((int64_t)x * rw / tw);
            if (rx >= rw) rx = rw - 1;
            int sx, sy;
            if (rot_q == 0)      { sx = rx;          sy = ry; }
            else if (rot_q == 1) { sx = ry;          sy = sh - 1 - rx; }
            else if (rot_q == 2) { sx = sw - 1 - rx; sy = sh - 1 - ry; }
            else                 { sx = sw - 1 - ry; sy = rx; }
            dr[x] = src[(size_t)sy * (size_t)sw + (size_t)sx];
        }
    }
    return dst;
}

static void update_zoom_labels(int eff_pct) {
    char zb[16];
    zb[0] = '%';
    u2s((unsigned int)eff_pct, zb + 1, sizeof(zb) - 1);
    if (g_zoom_lbl) lv_label_set_text(g_zoom_lbl, g_fit_mode ? "Sığdır" : zb);
    if (g_d_zoom) {
        if (g_fit_mode) {
            char fb[28];
            scpy(fb, sizeof(fb), "Sığdır (");
            scat(fb, sizeof(fb), zb);
            scat(fb, sizeof(fb), ")");
            lv_label_set_text(g_d_zoom, fb);
        } else {
            lv_label_set_text(g_d_zoom, zb);
        }
    }
    if (g_d_rot) {
        static const char *const RTXT[4] = { "0°", "90°", "180°", "270°" };
        lv_label_set_text(g_d_rot, RTXT[g_rot_q & 3]);
    }
}

static void render_view(void) {
    free_disp_px();
    if (g_img) { lv_obj_delete(g_img); g_img = 0; }
    if (!g_orig_px || g_orig_w <= 0 || g_orig_h <= 0) {
        if (g_ph) lv_obj_clear_flag(g_ph, LV_OBJ_FLAG_HIDDEN);
        update_zoom_labels(100);
        return;
    }

    int rw = (g_rot_q & 1) ? g_orig_h : g_orig_w;
    int rh = (g_rot_q & 1) ? g_orig_w : g_orig_h;

    lv_obj_update_layout(g_win);
    int vw = g_view ? lv_obj_get_width(g_view) : 560;
    int vh = g_view ? lv_obj_get_height(g_view) : 380;
    if (vw < 120) vw = 560;
    if (vh < 120) vh = 380;
    int maxw = vw - 88;
    int maxh = vh - 24;
    if (maxw < 100) maxw = 100;
    if (maxh < 100) maxh = 100;

    int tw = rw, th = rh;
    int eff_pct = g_zoom_pct;
    if (g_fit_mode) {
        if (tw > maxw || th > maxh) {
            if ((int64_t)maxw * rh <= (int64_t)maxh * rw) {
                tw = maxw;
                th = (int)((int64_t)rh * maxw / rw);
            } else {
                th = maxh;
                tw = (int)((int64_t)rw * maxh / rh);
            }
        }
        eff_pct = (rw > 0) ? (tw * 100 / rw) : 100;
        if (eff_pct < 1) eff_pct = 1;
        g_zoom_pct = eff_pct;
    } else {
        tw = (int)((int64_t)rw * g_zoom_pct / 100);
        th = (int)((int64_t)rh * g_zoom_pct / 100);
    }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    /* Çok büyük yakınlaştırmalarda bellek taşmasını önle */
    while ((int64_t)tw * (int64_t)th > 3200LL * 2400LL) {
        tw = tw * 3 / 4;
        th = th * 3 / 4;
    }

    uint32_t *out = transform_image(g_orig_px, g_orig_w, g_orig_h, g_rot_q, tw, th);
    if (!out) {
        if (g_ph) lv_obj_clear_flag(g_ph, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    g_px = out;
    g_pw = tw;
    g_ph_w = th;
    g_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    g_dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    g_dsc.header.flags = 0;
    g_dsc.header.w = (uint32_t)tw;
    g_dsc.header.h = (uint32_t)th;
    g_dsc.header.stride = (uint32_t)(tw * 4);
    g_dsc.data_size = (uint32_t)((size_t)tw * (size_t)th * 4u);
    g_dsc.data = (const uint8_t *)out;
    g_dsc.reserved = 0;
    g_dsc.reserved_2 = 0;

    if (g_ph) lv_obj_add_flag(g_ph, LV_OBJ_FLAG_HIDDEN);
    if (g_err_lbl) lv_obj_add_flag(g_err_lbl, LV_OBJ_FLAG_HIDDEN);
    g_img = lv_image_create(g_view);
    lv_image_set_src(g_img, &g_dsc);
    if (tw <= vw && th <= vh) lv_obj_center(g_img);
    else {
        int px = (vw > tw) ? (vw - tw) / 2 : 8;
        int py = (vh > th) ? (vh - th) / 2 : 8;
        lv_obj_set_pos(g_img, px, py);
    }
    update_zoom_labels(eff_pct);
}

static void show_sel(void);

static void prev_cb(lv_event_t *e) {
    (void)e;
    if (g_n <= 0) return;
    g_sel = (g_sel <= 0) ? g_n - 1 : g_sel - 1;
    show_sel();
}

static void next_cb(lv_event_t *e) {
    (void)e;
    if (g_n <= 0) return;
    g_sel = (g_sel + 1 >= g_n) ? 0 : g_sel + 1;
    show_sel();
}

static void zoom_in_cb(lv_event_t *e) {
    (void)e;
    g_fit_mode = 0;
    g_zoom_pct += 25;
    if (g_zoom_pct > 400) g_zoom_pct = 400;
    render_view();
}

static void zoom_out_cb(lv_event_t *e) {
    (void)e;
    g_fit_mode = 0;
    g_zoom_pct -= 25;
    if (g_zoom_pct < 25) g_zoom_pct = 25;
    render_view();
}

static void zoom_100_cb(lv_event_t *e) {
    (void)e;
    g_fit_mode = 0;
    g_zoom_pct = 100;
    render_view();
}

static void zoom_fit_cb(lv_event_t *e) {
    (void)e;
    g_fit_mode = 1;
    render_view();
}

static void rotate_cb(lv_event_t *e) {
    (void)e;
    g_rot_q = (g_rot_q + 1) & 3;
    render_view();
}

static void slide_tick_cb(lv_timer_t *t) {
    (void)t;
    if (!g_slide_on || g_n <= 1) return;
    g_sel = (g_sel + 1 >= g_n) ? 0 : g_sel + 1;
    show_sel();
}

static void slide_toggle_cb(lv_event_t *e) {
    (void)e;
    g_slide_on = !g_slide_on;
    if (g_slide_lbl) lv_label_set_text(g_slide_lbl, g_slide_on ? "Durdur" : "Slayt");
    if (g_slide_on) {
        if (!g_slide_timer) g_slide_timer = lv_timer_create(slide_tick_cb, 3000, 0);
        else lv_timer_reset(g_slide_timer);
    } else if (g_slide_timer) {
        lv_timer_delete(g_slide_timer);
        g_slide_timer = 0;
    }
}

static lv_obj_t *detail_row(lv_obj_t *panel, const char *label, lv_obj_t **val_out) {
    lv_obj_t *l = lv_label_create(panel);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, lv_color_hex(C_DIM), 0);
    if (f12) lv_obj_set_style_text_font(l, f12, 0);
    lv_obj_t *v = lv_label_create(panel);
    lv_label_set_text(v, "-");
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_width(v, LV_PCT(100));
    lv_obj_set_style_text_color(v, lv_color_hex(C_TXT), 0);
    if (f13) lv_obj_set_style_text_font(v, f13, 0);
    if (val_out) *val_out = v;
    return v;
}

static void show_sel(void) {
    free_orig_px();
    if (g_img) { lv_obj_delete(g_img); g_img = 0; }
    g_rot_q = 0;
    g_fit_mode = 1;

    if (g_sel < 0 || g_sel >= g_n) {
        lv_obj_add_flag(g_ph, LV_OBJ_FLAG_HIDDEN);
        if (g_err_lbl) lv_obj_clear_flag(g_err_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g_d_name, "-");
        lv_label_set_text(g_d_fmt, "-");
        lv_label_set_text(g_d_size, "-");
        lv_label_set_text(g_d_res, "-");
        lv_label_set_text(g_d_date, "-");
        lv_label_set_text(g_status, "0 / 0");
        update_zoom_labels(100);
        return;
    }

    uint32_t fsize = 0;
    asi_stat_t st;
    if (A->fs && A->fs->stat && A->fs->stat(g_path[g_sel], &st) == 0) fsize = st.size;
    char sb[24];
    if (fsize >= 1024u * 1024u) { u2s(fsize / (1024u * 1024u), sb, sizeof(sb)); scat(sb, sizeof(sb), " MB"); }
    else if (fsize >= 1024u) { u2s(fsize / 1024u, sb, sizeof(sb)); scat(sb, sizeof(sb), " KB"); }
    else { u2s(fsize, sb, sizeof(sb)); scat(sb, sizeof(sb), " B"); }

    char dt[24];
    scpy(dt, sizeof(dt), "-");
    if (A->time && A->time->now) {
        asi_datetime_t d;
        if (A->time->now(&d) && d.year > 2000) {
            char nb[8];
            scpy(dt, sizeof(dt), "");
            u2s((unsigned int)d.year, nb, sizeof(nb)); scat(dt, sizeof(dt), nb);
            scat(dt, sizeof(dt), "-");
            if (d.month < 10) scat(dt, sizeof(dt), "0");
            u2s((unsigned int)d.month, nb, sizeof(nb)); scat(dt, sizeof(dt), nb);
            scat(dt, sizeof(dt), "-");
            if (d.day < 10) scat(dt, sizeof(dt), "0");
            u2s((unsigned int)d.day, nb, sizeof(nb)); scat(dt, sizeof(dt), nb);
        }
    }

    lv_label_set_text(g_d_name, g_name[g_sel]);
    lv_label_set_text(g_d_fmt, fmt_from_name(g_name[g_sel]));
    lv_label_set_text(g_d_size, sb);
    lv_label_set_text(g_d_date, dt);
    {
        char idx[24], nb[8];
        scpy(idx, sizeof(idx), "");
        u2s((unsigned int)(g_sel + 1), nb, sizeof(nb)); scat(idx, sizeof(idx), nb);
        scat(idx, sizeof(idx), " / ");
        u2s((unsigned int)g_n, nb, sizeof(nb)); scat(idx, sizeof(idx), nb);
        lv_label_set_text(g_status, idx);
    }

    int ok = 0;
    if (fsize > 0 && fsize <= FT_MAXBYTES && A->fs && A->fs->read_path) {
        uint8_t *raw = (uint8_t *)A->mem->alloc(fsize);
        if (raw) {
            uint32_t rd = A->fs->read_path(g_path[g_sel], raw, fsize);
            int w = 0, h = 0;
            uint32_t *out = 0;
            const char *dfmt = 0;
            if (rd && imgdec_decode(raw, rd, A->mem->alloc, A->mem->free,
                                    &out, &w, &h, &dfmt) == 0
                && out && w > 0 && h > 0) {
                g_orig_px = out;
                g_orig_w = w;
                g_orig_h = h;
                g_orig_fmt = dfmt;
                ok = 1;
            }
            A->mem->free(raw);
        }
    }

    if (ok) {
        if (g_orig_fmt) lv_label_set_text(g_d_fmt, g_orig_fmt);
        char rb[28], nb[8];
        scpy(rb, sizeof(rb), "");
        u2s((unsigned int)g_orig_w, nb, sizeof(nb)); scat(rb, sizeof(rb), nb);
        scat(rb, sizeof(rb), " × ");
        u2s((unsigned int)g_orig_h, nb, sizeof(nb)); scat(rb, sizeof(rb), nb);
        lv_label_set_text(g_d_res, rb);
        render_view();
    } else {
        lv_obj_clear_flag(g_ph, LV_OBJ_FLAG_HIDDEN);
        if (g_err_lbl) {
            lv_label_set_text(g_err_lbl, fsize > FT_MAXBYTES ? "Dosya 16 MB sınırını aşıyor" : "Görsel çözülemedi");
            lv_obj_clear_flag(g_err_lbl, LV_OBJ_FLAG_HIDDEN);
        }
        lv_label_set_text(g_d_res, "Çözülemedi");
        update_zoom_labels(100);
    }
}

static void key_cb(lv_event_t *e) {
    uint32_t k = lv_event_get_key(e);
    if (k == LV_KEY_LEFT) prev_cb(0);
    else if (k == LV_KEY_RIGHT) next_cb(0);
    else if (k == '+' || k == '=') zoom_in_cb(0);
    else if (k == '-') zoom_out_cb(0);
    else if (k == '0') zoom_100_cb(0);
    else if (k == 'r' || k == 'R') rotate_cb(0);
    else if (k == ' ') slide_toggle_cb(0);
}

static void view_click_cb(lv_event_t *e) {
    (void)e;
    if (g_grp && g_view_wrap) {
        lv_indev_t *kp = lv_port_get_keypad();
        if (kp) lv_indev_set_group(kp, g_grp);
        lv_group_focus_obj(g_view_wrap);
    }
}

static void ft_relayout(lv_obj_t *win) {
    (void)win;
    if (!g_win) return;
    lv_obj_update_layout(g_win);
    if (g_orig_px) render_view();
}

static lv_obj_t *tb_btn(lv_obj_t *p, const char *txt, lv_event_cb_t cb, lv_obj_t **lbl_out) {
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_height(b, 26);
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_BTN), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_ACC), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 10, 0);
    lv_obj_set_style_pad_ver(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(C_TXT), 0);
    if (f12) lv_obj_set_style_text_font(l, f12, 0);
    lv_obj_center(l);
    if (lbl_out) *lbl_out = l;
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, 0);
    return b;
}

static void arrow_btn(int left) {
    lv_obj_t *b = lv_button_create(g_view_wrap);
    lv_obj_set_size(b, 32, 32);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_BTNBG), 0);
    lv_obj_set_style_bg_opa(b, 210, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_t *ic = svg_icon(b, left
        ? "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#FFFFFF\" stroke-width=\"2\"><path d=\"M15 18l-6-6 6-6\"/></svg>"
        : "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#FFFFFF\" stroke-width=\"2\"><path d=\"M9 18l6-6-6-6\"/></svg>", 16);
    if (ic) lv_obj_center(ic);
    if (left) lv_obj_align(b, LV_ALIGN_LEFT_MID, 12, 0);
    else      lv_obj_align(b, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_add_event_cb(b, left ? prev_cb : next_cb, LV_EVENT_CLICKED, 0);
}

static void app_del_cb(lv_event_t *e) {
    (void)e;
    if (g_slide_timer) { lv_timer_delete(g_slide_timer); g_slide_timer = 0; }
    g_slide_on = 0;
    free_orig_px();
    for (int i = 0; i < s_icon_n; i++)
        if (A->mem) A->mem->free(s_icon_buf[i]);
    s_icon_n = 0;
    if (g_grp) {
        lv_indev_t *kp = lv_port_get_keypad();
        if (kp && lv_indev_get_group(kp) == g_grp) lv_indev_set_group(kp, 0);
        lv_group_delete(g_grp);
        g_grp = 0;
    }
    g_win = 0; g_view_wrap = 0; g_view = 0; g_ph = 0; g_img = 0; g_err_lbl = 0;
    g_zoom_lbl = 0; g_slide_lbl = 0;
}

void app_entry(lv_obj_t *win, asi_t *asi) {
    A = asi;
    g_win = win;
    g_n = 0; g_sel = -1;
    g_orig_px = 0; g_px = 0;
    g_zoom_pct = 100; g_fit_mode = 1; g_rot_q = 0; g_slide_on = 0;
    g_slide_timer = 0;

    if (!f13) f13 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 13);
    if (!f12) f12 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 12);

    scan_dir("C:");
    scan_dir("C:/masaustu");
    scan_dir("C:/resimler");
    scan_dir("C:/indirilenler");
    scan_dir("C:/belgeler");

    if (arctian_open_target && arctian_open_target[0] && is_image_file(arctian_open_target)) {
        const char *p = arctian_open_target;
        const char *nm = p;
        for (const char *q = p; *q; q++) if (*q == '/') nm = q + 1;
        for (int i = 0; i < g_n; i++)
            if (!seq_cmp(g_path[i], p)) { g_sel = i; break; }
        if (g_sel < 0 && g_n < FT_MAX) {
            scpy(g_path[g_n], FT_PATH, p);
            scpy(g_name[g_n], FT_NAME, nm);
            g_sel = g_n++;
        }
        arctian_open_target = "";
    }
    if (g_sel < 0 && g_n > 0) g_sel = 0;

    lv_obj_set_flex_flow(win, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(win, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(win, 0, 0);
    lv_obj_set_style_pad_row(win, 0, 0);
    lv_obj_set_style_bg_color(win, lv_color_hex(C_BODY), 0);
    lv_obj_set_style_bg_opa(win, LV_OPA_COVER, 0);
    lv_obj_remove_flag(win, LV_OBJ_FLAG_SCROLLABLE);

    /* Üst araç çubuğu (38 px) */
    lv_obj_t *tb = lv_obj_create(win);
    lv_obj_remove_style_all(tb);
    lv_obj_set_size(tb, LV_PCT(100), 38);
    lv_obj_set_style_bg_color(tb, lv_color_hex(C_BAR), 0);
    lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tb, 1, 0);
    lv_obj_set_style_border_side(tb, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(tb, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_pad_hor(tb, 12, 0);
    lv_obj_set_style_pad_column(tb, 6, 0);
    lv_obj_set_flex_flow(tb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tb, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(tb, LV_OBJ_FLAG_SCROLLABLE);

    tb_btn(tb, "−", zoom_out_cb, 0);
    tb_btn(tb, "%100", zoom_100_cb, 0);
    tb_btn(tb, "+", zoom_in_cb, 0);
    tb_btn(tb, "Sığdır", zoom_fit_cb, &g_zoom_lbl);
    tb_btn(tb, "90° Döndür", rotate_cb, 0);
    tb_btn(tb, "Slayt", slide_toggle_cb, &g_slide_lbl);

    /* Orta alan: sol görsel + sağ ayrıntılar */
    lv_obj_t *mid = lv_obj_create(win);
    lv_obj_remove_style_all(mid);
    lv_obj_set_width(mid, LV_PCT(100));
    lv_obj_set_flex_grow(mid, 1);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(mid, 0, 0);
    lv_obj_remove_flag(mid, LV_OBJ_FLAG_SCROLLABLE);

    g_view_wrap = lv_obj_create(mid);
    lv_obj_remove_style_all(g_view_wrap);
    lv_obj_set_height(g_view_wrap, LV_PCT(100));
    lv_obj_set_flex_grow(g_view_wrap, 1);
    lv_obj_set_style_bg_color(g_view_wrap, lv_color_hex(C_VIEW), 0);
    lv_obj_set_style_bg_opa(g_view_wrap, LV_OPA_COVER, 0);
    lv_obj_remove_flag(g_view_wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_view_wrap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_view_wrap, view_click_cb, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(g_view_wrap, key_cb, LV_EVENT_KEY, 0);

    g_view = lv_obj_create(g_view_wrap);
    lv_obj_remove_style_all(g_view);
    lv_obj_set_size(g_view, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scroll_dir(g_view, LV_DIR_ALL);
    lv_obj_set_scrollbar_mode(g_view, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(g_view, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_view, view_click_cb, LV_EVENT_CLICKED, 0);

    g_ph = lv_obj_create(g_view);
    lv_obj_remove_style_all(g_ph);
    lv_obj_set_size(g_ph, 360, 220);
    lv_obj_set_style_radius(g_ph, 6, 0);
    lv_obj_set_style_bg_color(g_ph, lv_color_hex(C_PH), 0);
    lv_obj_set_style_bg_opa(g_ph, LV_OPA_COVER, 0);
    lv_obj_center(g_ph);

    g_err_lbl = lv_label_create(g_view);
    lv_label_set_text(g_err_lbl, "Görsel bulunamadı");
    lv_obj_set_style_text_color(g_err_lbl, lv_color_hex(C_DIM), 0);
    if (f13) lv_obj_set_style_text_font(g_err_lbl, f13, 0);
    lv_obj_center(g_err_lbl);
    lv_obj_add_flag(g_err_lbl, LV_OBJ_FLAG_HIDDEN);

    arrow_btn(1);
    arrow_btn(0);

    /* Sağ ayrıntılar paneli 240 px */
    lv_obj_t *det = lv_obj_create(mid);
    lv_obj_remove_style_all(det);
    lv_obj_set_size(det, 240, LV_PCT(100));
    lv_obj_set_style_bg_color(det, lv_color_hex(C_BODY), 0);
    lv_obj_set_style_bg_opa(det, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(det, 1, 0);
    lv_obj_set_style_border_side(det, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(det, lv_color_hex(C_LINE), 0);
    lv_obj_set_scroll_dir(det, LV_DIR_VER);
    lv_obj_set_style_pad_all(det, 16, 0);
    lv_obj_set_style_pad_row(det, 4, 0);
    lv_obj_set_flex_flow(det, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *h = lv_label_create(det);
    lv_label_set_text(h, "Ayrıntılar");
    lv_obj_set_style_text_color(h, lv_color_hex(C_TXT), 0);
    if (f13) lv_obj_set_style_text_font(h, f13, 0);
    lv_obj_set_style_pad_bottom(h, 6, 0);

    detail_row(det, "Dosya adı", &g_d_name);
    detail_row(det, "Tür", &g_d_fmt);
    detail_row(det, "Boyut", &g_d_size);
    detail_row(det, "Çözünürlük", &g_d_res);
    detail_row(det, "Tarih", &g_d_date);
    detail_row(det, "Yakınlaştırma", &g_d_zoom);
    detail_row(det, "Döndürme", &g_d_rot);

    if (g_n == 0) {
        lv_obj_t *e2 = lv_label_create(det);
        lv_label_set_text(e2, "C: biriminde görsel bulunamadı (.png, .jpg, .bmp, .gif).");
        lv_label_set_long_mode(e2, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(e2, LV_PCT(100));
        lv_obj_set_style_text_color(e2, lv_color_hex(C_DIM), 0);
        if (f12) lv_obj_set_style_text_font(e2, f12, 0);
        lv_obj_set_style_pad_top(e2, 10, 0);
    }

    /* Alt durum çubuğu 28 px */
    lv_obj_t *sb = lv_obj_create(win);
    lv_obj_remove_style_all(sb);
    lv_obj_set_size(sb, LV_PCT(100), 28);
    lv_obj_set_style_border_width(sb, 1, 0);
    lv_obj_set_style_border_side(sb, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(sb, lv_color_hex(C_LINE), 0);
    lv_obj_remove_flag(sb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_hor(sb, 16, 0);
    lv_obj_set_flex_flow(sb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sb, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_status = lv_label_create(sb);
    lv_label_set_text(g_status, "0 / 0");
    lv_obj_set_style_text_color(g_status, lv_color_hex(C_DIM), 0);
    if (f12) lv_obj_set_style_text_font(g_status, f12, 0);

    lv_obj_t *hint = lv_label_create(sb);
    lv_label_set_text(hint, "Sol/Sağ ok: Geçiş · +/-: Yakınlaştır · R: Döndür");
    lv_obj_set_style_text_color(hint, lv_color_hex(C_DIM), 0);
    if (f12) lv_obj_set_style_text_font(hint, f12, 0);

    g_grp = lv_group_create();
    if (g_grp) {
        lv_group_add_obj(g_grp, g_view_wrap);
        lv_group_focus_obj(g_view_wrap);
        lv_indev_t *kp = lv_port_get_keypad();
        if (kp) lv_indev_set_group(kp, g_grp);
    }

    arctian_shell_set_relayout(win, ft_relayout);
    lv_obj_add_event_cb(win, app_del_cb, LV_EVENT_DELETE, 0);
    show_sel();
}
