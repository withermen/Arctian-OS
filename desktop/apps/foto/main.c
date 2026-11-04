/* ==================================================================
 * Fotograf Goruntuleyici
 *
 * Arayuz spesifikasyonu (tasarim.md bolum 7):
 *   - solda esnek gorsel alani (#0b0c0f), ortada en fazla 520x340
 *     gorsel, kenarlarda 32 px yuvarlak ok dugmeleri
 *   - sagda 250 px Ayrintilar paneli (12 px etiket / 13 px deger)
 *   - alt durum cubugu 28 px: "3 / 24"
 * Bilinen fark: spec'te JPEG girdi varsayilir; surucude JPEG cozucu
 * yok, .png dosyalari cozulur. Verisi olmayan alanlarda (kamera,
 * konum) uydurma deger yerine "-" gosterilir.
 * ================================================================== */
#include "app.h"
#include "adl_wolf.h"
#include "png.h"
#include "svg.h"
#include <stdint.h>

#define FT_MAX    24
#define FT_PATH   200
#define FT_NAME   64
#define FT_MAXBYTES (4u * 1024u * 1024u)

/* spec paleti */
#define C_VIEW   0x0B0C0F
#define C_BODY   0x14161B
#define C_LINE   0x2C313B
#define C_PH     0x1A2233
#define C_TXT    0xE8EAEE
#define C_DIM    0x8B919C
#define C_BTNBG  0x14161B   /* rgba(20,22,27,.8) */

static asi_t    *A;
static lv_obj_t *g_win;
static lv_obj_t *g_view;      /* gorsel alani            */
static lv_obj_t *g_ph;        /* yer tutucu dikdortgen   */
static lv_obj_t *g_img;       /* lv_image (cozulunce)    */
static lv_obj_t *g_d_name, *g_d_fmt, *g_d_size, *g_d_res;
static lv_obj_t *g_d_date, *g_status;

static char      g_path[FT_MAX][FT_PATH];
static char      g_name[FT_MAX][FT_NAME];
static int       g_n, g_sel;

static uint32_t *g_px;        /* cozulen ARGB tamponu (mem->alloc) */
static int       g_pw, g_ph_w;
static lv_image_dsc_t g_dsc;

static uint32_t *s_icon_buf[4];
static int s_icon_n;

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

static lv_obj_t *svg_icon(lv_obj_t *parent, const char *svg, int size) {
    if (s_icon_n >= 4) return 0;
    uint32_t *buf = (uint32_t *)A->mem->alloc((size_t)size * size * 4u);
    if (!buf) return 0;
    if (svg_render(svg, size, size, buf) != 0) { A->mem->free(buf); return 0; }
    s_icon_buf[s_icon_n++] = buf;
    lv_obj_t *c = lv_canvas_create(parent);
    lv_obj_remove_style_all(c);
    lv_canvas_set_buffer(c, buf, size, size, LV_COLOR_FORMAT_ARGB8888);
    return c;
}

/* ---- dosya tarama ---- */
static int is_png(const char *name) {
    int L = slen(name);
    if (L < 5) return 0;
    if (name[L - 4] != '.') return 0;
    char a = name[L - 3], b = name[L - 2], c = name[L - 1];
    if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
    if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
    if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return a == 'p' && b == 'n' && c == 'g';
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
        if (isd || !is_png(nm)) continue;
        int dup = 0;
        for (int k = 0; k < g_n; k++)
            if (slen(g_name[k]) == slen(nm) && !seq_cmp(g_name[k], nm)) { dup = 1; break; }
        if (dup) continue;
        join_path(g_path[g_n], FT_PATH, dir, nm);
        scpy(g_name[g_n], FT_NAME, nm);
        g_n++;
    }
}

/* ---- gorsel ---- */
static void free_px(void) {
    if (g_px && A->mem) A->mem->free(g_px);
    g_px = 0;
    g_pw = g_ph_w = 0;
}

/* en-yakin olceklemeye yakin kucultme (kacis icin basit atlamali orneklem) */
static uint32_t *fit_down(const uint32_t *src, int w, int h, int tw, int th) {
    uint32_t *dst = (uint32_t *)A->mem->alloc((size_t)tw * th * 4u);
    if (!dst) return 0;
    for (int y = 0; y < th; y++) {
        int sy = (int)((int64_t)y * h / th);
        const uint32_t *sr = src + (size_t)sy * w;
        uint32_t *dr = dst + (size_t)y * tw;
        for (int x = 0; x < tw; x++)
            dr[x] = sr[(int)((int64_t)x * w / tw)];
    }
    return dst;
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

/* detay satiri: ustte 12 px soluk etiket, altta 13 px deger */
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
    free_px();
    if (g_img) { lv_obj_delete(g_img); g_img = 0; }

    if (g_sel < 0 || g_sel >= g_n) {
        lv_obj_add_flag(g_ph, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g_d_name, "-");
        lv_label_set_text(g_d_fmt, "-");
        lv_label_set_text(g_d_size, "-");
        lv_label_set_text(g_d_res, "-");
        lv_label_set_text(g_d_date, "-");
        lv_label_set_text(g_status, "0 / 0");
        return;
    }

    /* dosya boyutu */
    uint32_t fsize = 0;
    asi_stat_t st;
    if (A->fs && A->fs->stat && A->fs->stat(g_path[g_sel], &st) == 0) fsize = st.size;
    char sb[20];
    if (fsize >= 1024u * 1024u) { u2s(fsize / (1024u * 1024u), sb, sizeof(sb)); scat(sb, sizeof(sb), " MB"); }
    else if (fsize >= 1024u) { u2s(fsize / 1024u, sb, sizeof(sb)); scat(sb, sizeof(sb), " KB"); }
    else { u2s(fsize, sb, sizeof(sb)); scat(sb, sizeof(sb), " B"); }

    /* tarih */
    char dt[24];
    scpy(dt, sizeof(dt), "-");
    if (A->time && A->time->now) {
        asi_datetime_t d;
        if (A->time->now(&d) && d.year > 2000) {
            char nb[8];
            scpy(dt, sizeof(dt), "");
            u2s((unsigned int)d.year, nb, sizeof(nb)); scat(dt, sizeof(dt), nb);
            scat(dt, sizeof(dt), "-");
            u2s((unsigned int)d.month, nb, sizeof(nb)); if (d.month < 10) { scat(dt, sizeof(dt), "0"); }
            scat(dt, sizeof(dt), nb);
            scat(dt, sizeof(dt), "-");
            u2s((unsigned int)d.day, nb, sizeof(nb)); if (d.day < 10) { scat(dt, sizeof(dt), "0"); }
            scat(dt, sizeof(dt), nb);
        }
    }

    lv_label_set_text(g_d_name, g_name[g_sel]);
    lv_label_set_text(g_d_fmt, "PNG");
    lv_label_set_text(g_d_size, sb);
    lv_label_set_text(g_d_date, dt);
    {
        char idx[16];
        char nb[8];
        scpy(idx, sizeof(idx), "");
        u2s((unsigned int)(g_sel + 1), nb, sizeof(nb)); scat(idx, sizeof(idx), nb);
        scat(idx, sizeof(idx), " / ");
        u2s((unsigned int)g_n, nb, sizeof(nb)); scat(idx, sizeof(idx), nb);
        lv_label_set_text(g_status, idx);
    }

    /* coz */
    uint32_t ok = 0;
    if (fsize && fsize <= FT_MAXBYTES && A->fs && A->fs->read_path) {
        uint8_t *raw = (uint8_t *)A->mem->alloc(fsize);
        if (raw) {
            uint32_t rd = A->fs->read_path(g_path[g_sel], raw, fsize);
            int w = 0, h = 0;
            uint32_t *out = 0;
            if (rd && png_decode_argb(raw, rd, A->mem->alloc, A->mem->free, &out, &w, &h) == 0
                && out && w > 0 && h > 0) {
                /* 520x340 kutusuna sigdir */
                int tw = w, th = h;
                if (tw > 520 || th > 340) {
                    if ((int64_t)520 * h <= (int64_t)340 * w) { tw = 520; th = (int)((int64_t)h * 520 / w); }
                    else { th = 340; tw = (int)((int64_t)w * 340 / h); }
                    uint32_t *sm = fit_down(out, w, h, tw, th);
                    if (sm) { A->mem->free(out); out = sm; w = tw; h = th; }
                }
                g_px = out; g_pw = w; g_ph_w = h;
                g_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
                g_dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
                g_dsc.header.flags = 0;
                g_dsc.header.w = (uint32_t)w;
                g_dsc.header.h = (uint32_t)h;
                g_dsc.header.stride = (uint32_t)(w * 4);
                g_dsc.data_size = (uint32_t)((size_t)w * h * 4);
                g_dsc.data = (const uint8_t *)out;
                g_dsc.reserved = 0;
                g_dsc.reserved_2 = 0;
                ok = 1;
            }
            A->mem->free(raw);
        }
    }

    if (ok) {
        lv_obj_add_flag(g_ph, LV_OBJ_FLAG_HIDDEN);
        g_img = lv_image_create(g_view);
        lv_image_set_src(g_img, &g_dsc);
        lv_obj_center(g_img);
        char rb[24];
        char nb[8];
        scpy(rb, sizeof(rb), "");
        u2s((unsigned int)g_pw, nb, sizeof(nb)); scat(rb, sizeof(rb), nb);
        scat(rb, sizeof(rb), " x ");
        u2s((unsigned int)g_ph_w, nb, sizeof(nb)); scat(rb, sizeof(rb), nb);
        lv_label_set_text(g_d_res, rb);
    } else {
        lv_obj_clear_flag(g_ph, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g_d_res, "cozulemedi");
    }
}

/* yuvarlak 32 px ok dugmesi */
static void arrow_btn(int left) {
    lv_obj_t *b = lv_button_create(g_view);
    lv_obj_set_size(b, 32, 32);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_BTNBG), 0);
    lv_obj_set_style_bg_opa(b, 204, 0);
    lv_obj_set_style_border_width(b, 0, 0);
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
    free_px();
    for (int i = 0; i < s_icon_n; i++)
        if (A->mem) A->mem->free(s_icon_buf[i]);
    s_icon_n = 0;
    g_win = 0; g_view = 0; g_ph = 0; g_img = 0;
}

void app_entry(lv_obj_t *win, asi_t *asi) {
    A = asi;
    g_win = win;
    g_n = 0; g_sel = -1;
    g_px = 0;

    if (!f13) f13 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 13);
    if (!f12) f12 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 12);

    scan_dir("C:");
    scan_dir("C:/masaustu");
    scan_dir("C:/resimler");
    scan_dir("C:/indirilenler");

    /* cift tikla ile gelen hedef */
    if (arctian_open_target && arctian_open_target[0] && is_png(arctian_open_target)) {
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

    /* govde: sol gorsel alani + sag ayrintilar */
    lv_obj_set_flex_flow(win, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(win, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(win, 0, 0);
    lv_obj_set_style_pad_row(win, 0, 0);
    lv_obj_set_style_bg_color(win, lv_color_hex(C_BODY), 0);
    lv_obj_set_style_bg_opa(win, LV_OPA_COVER, 0);
    lv_obj_remove_flag(win, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *mid = lv_obj_create(win);
    lv_obj_remove_style_all(mid);
    lv_obj_set_width(mid, LV_PCT(100));
    lv_obj_set_flex_grow(mid, 1);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(mid, 0, 0);
    lv_obj_remove_flag(mid, LV_OBJ_FLAG_SCROLLABLE);

    g_view = lv_obj_create(mid);
    lv_obj_remove_style_all(g_view);
    lv_obj_set_height(g_view, LV_PCT(100));
    lv_obj_set_flex_grow(g_view, 1);
    lv_obj_set_style_bg_color(g_view, lv_color_hex(C_VIEW), 0);
    lv_obj_set_style_bg_opa(g_view, LV_OPA_COVER, 0);
    lv_obj_remove_flag(g_view, LV_OBJ_FLAG_SCROLLABLE);

    /* yer tutucu (gorsel cozuleyince gizlenir) */
    g_ph = lv_obj_create(g_view);
    lv_obj_remove_style_all(g_ph);
    lv_obj_set_size(g_ph, 520, 340);
    lv_obj_set_style_radius(g_ph, 4, 0);
    lv_obj_set_style_bg_color(g_ph, lv_color_hex(C_PH), 0);
    lv_obj_set_style_bg_opa(g_ph, LV_OPA_COVER, 0);
    lv_obj_center(g_ph);

    arrow_btn(1);
    arrow_btn(0);

    /* ayrintilar paneli 250 px */
    lv_obj_t *det = lv_obj_create(mid);
    lv_obj_remove_style_all(det);
    lv_obj_set_size(det, 250, LV_PCT(100));
    lv_obj_set_style_bg_color(det, lv_color_hex(C_BODY), 0);
    lv_obj_set_style_bg_opa(det, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(det, 1, 0);
    lv_obj_set_style_border_side(det, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(det, lv_color_hex(C_LINE), 0);
    lv_obj_remove_flag(det, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(det, 16, 0);
    lv_obj_set_style_pad_row(det, 2, 0);
    lv_obj_set_flex_flow(det, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *h = lv_label_create(det);
    lv_label_set_text(h, "Ayrintilar");
    lv_obj_set_style_text_color(h, lv_color_hex(C_TXT), 0);
    if (f13) lv_obj_set_style_text_font(h, f13, 0);
    lv_obj_set_style_pad_bottom(h, 8, 0);
    detail_row(det, "Dosya adi", &g_d_name);
    detail_row(det, "Tur", &g_d_fmt);
    detail_row(det, "Boyut", &g_d_size);
    detail_row(det, "Cozunurluk", &g_d_res);
    detail_row(det, "Tarih", &g_d_date);
    {
        lv_obj_t *l = lv_label_create(det);
        lv_label_set_text(l, "Kamera / konum");
        lv_obj_set_style_text_color(l, lv_color_hex(C_DIM), 0);
        if (f12) lv_obj_set_style_text_font(l, f12, 0);
        lv_obj_t *v = lv_label_create(det);
        lv_label_set_text(v, "-");
        lv_obj_set_style_text_color(v, lv_color_hex(C_TXT), 0);
        if (f13) lv_obj_set_style_text_font(v, f13, 0);
    }
    if (g_n == 0) {
        lv_obj_t *e2 = lv_label_create(det);
        lv_label_set_text(e2, "C: biriminde .png dosya bulunamadi.");
        lv_label_set_long_mode(e2, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(e2, LV_PCT(100));
        lv_obj_set_style_text_color(e2, lv_color_hex(C_DIM), 0);
        if (f12) lv_obj_set_style_text_font(e2, f12, 0);
        lv_obj_set_style_pad_top(e2, 10, 0);
    }

    /* alt durum cubugu 28 px */
    lv_obj_t *sb = lv_obj_create(win);
    lv_obj_remove_style_all(sb);
    lv_obj_set_size(sb, LV_PCT(100), 28);
    lv_obj_set_style_border_width(sb, 1, 0);
    lv_obj_set_style_border_side(sb, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(sb, lv_color_hex(C_LINE), 0);
    lv_obj_remove_flag(sb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_hor(sb, 16, 0);
    g_status = lv_label_create(sb);
    lv_label_set_text(g_status, "0 / 0");
    lv_obj_set_style_text_color(g_status, lv_color_hex(C_DIM), 0);
    if (f12) lv_obj_set_style_text_font(g_status, f12, 0);
    lv_obj_align(g_status, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_add_event_cb(win, app_del_cb, LV_EVENT_DELETE, 0);
    show_sel();
}
