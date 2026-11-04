/* ==================================================================
 * Metin Duzenleyici
 *
 * Arayuz spesifikasyonu (tasarim.md bolum 5):
 *   - 40 px sekme seridi: aktif sekme #1a1d24 zemin + altta 2 px
 *     #3b82f6 cizgi, pasif sekme #8b919c
 *   - govde: sol 52 px satir numarasi sutunu (saga yasli #4a505c,
 *     saginda 1 px #2c313b cizgi); metin alani 14 ust / 20 sol bosluk,
 *     13 px yaz, satir yuksekligi 1.75
 *   - alt durum cubugu 32 px: "Satir X, sutun Y" / "UTF-8 · Markdown · 2 bosluk"
 * Sayilar gercek metinden hesaplanir; uyduurma icerik yuklenmez.
 * ================================================================== */
#include "app.h"
#include "adl_wolf.h"
#include <stdint.h>

#define MT_MAX     8                  /* sekme sayisi        */
#define MT_PATH    200
#define MT_NAME    48
#define MT_MAXBYTES 32768             /* dosya okuma ust siniri */

/* spec paleti */
#define C_BODY   0x14161B
#define C_LINE   0x2A2E37
#define C_DIV    0x2C313B
#define C_TABON  0x1A1D24
#define C_ACC    0x3B82F6
#define C_TXT    0xCDD3DE
#define C_DIM    0x8B919C
#define C_GUT    0x4A505C
#define C_STAT   0x6B717D

static asi_t    *A;
static lv_obj_t *g_win;
static lv_obj_t *g_tabs;
static lv_obj_t *g_gutter;
static lv_obj_t *g_ta;
static lv_obj_t *g_st_l, *g_st_r;
static lv_obj_t *g_tab_btns[MT_MAX];
static lv_obj_t *g_tab_lbls[MT_MAX];

static char g_path[MT_MAX][MT_PATH];
static char g_name[MT_MAX][MT_NAME];
static int  g_n, g_sel;

/* yerel fontlar */
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

/* uzanti kontrolu (kucuk harf) */
static int ext_is(const char *name, const char *ext) {
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

static int is_text(const char *name) {
    static const char *const ex[5] = { "md", "txt", "json", "cfg", "ini" };
    for (int i = 0; i < 5; i++) if (ext_is(name, ex[i])) return 1;
    return 0;
}

static void join_path(char *out, int cap, const char *dir, const char *name) {
    scpy(out, cap, dir);
    int L = slen(out);
    if (L == 0) { scpy(out, cap, name); return; }
    if (out[L - 1] != '/' && out[L - 1] != ':') scat(out, cap, "/");
    scat(out, cap, name);
}

static int seq0(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return !*a && !*b;
}

static void add_file(const char *dir, const char *name) {
    if (g_n >= MT_MAX || !is_text(name)) return;
    /* ayni ad zaten var mi */
    for (int i = 0; i < g_n; i++) if (seq0(g_name[i], name)) return;
    join_path(g_path[g_n], MT_PATH, dir, name);
    scpy(g_name[g_n], MT_NAME, name);
    g_n++;
}

static void scan_dir(const char *dir) {
    if (!A->fs || !A->fs->count_dir || !A->fs->list_dir) return;
    int n = A->fs->count_dir(dir);
    if (n <= 0) return;
    for (int i = 0; i < n && g_n < MT_MAX; i++) {
        char nm[MT_NAME];
        int isd = 0;
        if (A->fs->list_dir(dir, i, nm, sizeof(nm), &isd) != 0) continue;
        if (isd) continue;
        add_file(dir, nm);
    }
}

/* durum satiri ve satir numaralari: metin/her degisiklikte yenilenir */
static void paint_gutter(void) {
    if (!g_gutter) return;
    const char *t = lv_textarea_get_text(g_ta);
    int lines = 1;
    for (int i = 0; t[i]; i++) if (t[i] == '\n') lines++;
    if (lines > 200) lines = 200;
    static char buf[200 * 5];
    int k = 0;
    for (int i = 1; i <= lines; i++) {
        char nb[8];
        u2s((unsigned int)i, nb, sizeof(nb));
        for (int j = 0; nb[j] && k < (int)sizeof(buf) - 2; j++) buf[k++] = nb[j];
        if (i < lines) buf[k++] = '\n';
    }
    buf[k] = 0;
    lv_label_set_text(g_gutter, buf);
}

static void paint_status(void) {
    if (!g_st_l) return;
    const char *t = lv_textarea_get_text(g_ta);
    uint32_t pos = lv_textarea_get_cursor_pos(g_ta);
    int line = 1, col = 1;
    for (uint32_t i = 0; i < pos && t[i]; i++) {
        if (t[i] == '\n') { line++; col = 1; }
        else if (((unsigned char)t[i] & 0xC0) != 0x80) col++;   /* UTF-8 karakter basi */
    }
    char lb[8], cb[8], s[48];
    u2s((unsigned int)line, lb, sizeof(lb));
    u2s((unsigned int)col, cb, sizeof(cb));
    scpy(s, sizeof(s), "Satır ");
    scat(s, sizeof(s), lb);
    scat(s, sizeof(s), ", sütun ");
    scat(s, sizeof(s), cb);
    lv_label_set_text(g_st_l, s);
}

static void on_text_cb(lv_event_t *e) { (void)e; paint_gutter(); paint_status(); }

static void paint_tabs(void) {
    for (int i = 0; i < g_n; i++) {
        if (!g_tab_btns[i]) continue;
        int on = (i == g_sel);
        lv_obj_set_style_bg_color(g_tab_btns[i], lv_color_hex(on ? C_TABON : C_BODY), 0);
        lv_obj_set_style_border_width(g_tab_btns[i], on ? 2 : 0, 0);
        lv_obj_set_style_border_color(g_tab_btns[i], lv_color_hex(C_ACC), 0);
        lv_obj_set_style_border_side(g_tab_btns[i], LV_BORDER_SIDE_BOTTOM, 0);
        if (g_tab_lbls[i])
            lv_obj_set_style_text_color(g_tab_lbls[i], lv_color_hex(on ? 0xE6E8EC : C_DIM), 0);
    }
}

static char *s_read_file(const char *path, uint32_t *len_out) {
    if (!A->fs || !A->fs->read_path || !A->mem) return 0;
    uint8_t *b = (uint8_t *)A->mem->alloc(MT_MAXBYTES + 1);
    if (!b) return 0;
    uint32_t rd = A->fs->read_path(path, b, MT_MAXBYTES);
    b[rd] = 0;
    if (len_out) *len_out = rd;
    return (char *)b;
}

static void load_sel(void) {
    if (g_sel < 0 || g_sel >= g_n) return;
    char *b = s_read_file(g_path[g_sel], 0);
    if (b) {
        lv_textarea_set_text(g_ta, b);
        A->mem->free(b);
    } else {
        lv_textarea_set_text(g_ta, "");
    }
    const char *nm = g_name[g_sel];
    const char *fmt = ext_is(nm, "md") ? "Markdown"
                    : ext_is(nm, "json") ? "JSON"
                    : "Düz Metin";
    char r[48];
    scpy(r, sizeof(r), "UTF-8 \xc2\xb7 ");
    scat(r, sizeof(r), fmt);
    scat(r, sizeof(r), " \xc2\xb7 2 boşluk");
    lv_label_set_text(g_st_r, r);
    paint_tabs();
    paint_gutter();
    paint_status();
}

static void tab_cb(lv_event_t *e) {
    g_sel = (int)(intptr_t)lv_event_get_user_data(e);
    load_sel();
}

static void rebuild_tabs(void) {
    lv_obj_clean(g_tabs);
    for (int i = 0; i < MT_MAX; i++) { g_tab_btns[i] = 0; g_tab_lbls[i] = 0; }
    for (int i = 0; i < g_n; i++) {
        lv_obj_t *b = lv_obj_create(g_tabs);
        lv_obj_remove_style_all(b);
        lv_obj_set_height(b, LV_PCT(100));
        lv_obj_set_width(b, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_BODY), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_hor(b, 16, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, g_name[i]);
        if (f13) lv_obj_set_style_text_font(l, f13, 0);
        lv_obj_center(l);
        g_tab_btns[i] = b;
        g_tab_lbls[i] = l;
    }
    paint_tabs();
}

void app_entry(lv_obj_t *win, asi_t *asi) {
    A = asi;
    g_win = win;
    g_n = 0; g_sel = -1;

    if (!f13) f13 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 13);
    if (!f12) f12 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 12);

    /* dosyalari tara: C: koku ve belgeler klasorleri */
    scan_dir("C:");
    scan_dir("C:/belgeler");
    scan_dir("C:/masaustu");
    scan_dir("C:/indirilenler");

    /* cift tikla ile gelen hedef onceliklidir */
    if (arctian_open_target && arctian_open_target[0] && is_text(arctian_open_target)) {
        const char *p = arctian_open_target;
        const char *nm = p;
        for (const char *q = p; *q; q++) if (*q == '/') nm = q + 1;
        for (int i = 0; i < g_n; i++) {
            if (seq0(g_path[i], p)) { g_sel = i; break; }
        }
        if (g_sel < 0 && g_n < MT_MAX) {
            scpy(g_path[g_n], MT_PATH, p);
            scpy(g_name[g_n], MT_NAME, nm);
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

    /* sekme seridi 40 px */
    g_tabs = lv_obj_create(win);
    lv_obj_remove_style_all(g_tabs);
    lv_obj_set_size(g_tabs, LV_PCT(100), 40);
    lv_obj_set_style_bg_color(g_tabs, lv_color_hex(C_BODY), 0);
    lv_obj_set_style_bg_opa(g_tabs, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_tabs, 1, 0);
    lv_obj_set_style_border_side(g_tabs, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(g_tabs, lv_color_hex(C_LINE), 0);
    lv_obj_remove_flag(g_tabs, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(g_tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_tabs, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* govde: satir numaralari + metin alani */
    lv_obj_t *body = lv_obj_create(win);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *gutcol = lv_obj_create(body);
    lv_obj_remove_style_all(gutcol);
    lv_obj_set_size(gutcol, 52, LV_PCT(100));
    lv_obj_set_style_bg_color(gutcol, lv_color_hex(C_BODY), 0);
    lv_obj_set_style_bg_opa(gutcol, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(gutcol, 1, 0);
    lv_obj_set_style_border_side(gutcol, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_color(gutcol, lv_color_hex(C_DIV), 0);
    lv_obj_remove_flag(gutcol, LV_OBJ_FLAG_SCROLLABLE);
    g_gutter = lv_label_create(gutcol);
    lv_label_set_text(g_gutter, "1");
    lv_obj_set_width(g_gutter, 39);
    lv_obj_set_style_text_align(g_gutter, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(g_gutter, lv_color_hex(C_GUT), 0);
    if (f13) lv_obj_set_style_text_font(g_gutter, f13, 0);
    lv_obj_set_style_text_line_space(g_gutter, 10, 0);   /* 13 px yaz, 1.75 satir */
    lv_obj_set_pos(g_gutter, 0, 14);

    g_ta = lv_textarea_create(body);
    lv_obj_set_height(g_ta, LV_PCT(100));
    lv_obj_set_flex_grow(g_ta, 1);
    lv_obj_set_style_bg_opa(g_ta, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_ta, 0, 0);
    lv_obj_set_style_shadow_width(g_ta, 0, 0);
    lv_obj_set_style_outline_width(g_ta, 0, 0);
    lv_obj_set_style_pad_top(g_ta, 14, 0);
    lv_obj_set_style_pad_bottom(g_ta, 14, 0);
    lv_obj_set_style_pad_left(g_ta, 20, 0);
    lv_obj_set_style_pad_right(g_ta, 20, 0);
    lv_obj_set_style_text_color(g_ta, lv_color_hex(C_TXT), 0);
    if (f13) lv_obj_set_style_text_font(g_ta, f13, 0);
    lv_obj_set_style_text_line_space(g_ta, 10, 0);
    lv_textarea_set_one_line(g_ta, false);
    lv_obj_set_scrollbar_mode(g_ta, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(g_ta, on_text_cb, LV_EVENT_VALUE_CHANGED, 0);
    lv_obj_add_event_cb(g_ta, on_text_cb, LV_EVENT_CLICKED, 0);

    /* durum cubugu 32 px */
    lv_obj_t *sb = lv_obj_create(win);
    lv_obj_remove_style_all(sb);
    lv_obj_set_size(sb, LV_PCT(100), 32);
    lv_obj_set_style_border_width(sb, 1, 0);
    lv_obj_set_style_border_side(sb, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(sb, lv_color_hex(C_LINE), 0);
    lv_obj_remove_flag(sb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_hor(sb, 16, 0);
    lv_obj_set_flex_flow(sb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sb, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    g_st_l = lv_label_create(sb);
    lv_label_set_text(g_st_l, "Satır 1, sütun 1");
    lv_obj_set_style_text_color(g_st_l, lv_color_hex(C_STAT), 0);
    if (f12) lv_obj_set_style_text_font(g_st_l, f12, 0);
    g_st_r = lv_label_create(sb);
    lv_label_set_text(g_st_r, "");
    lv_obj_set_style_text_color(g_st_r, lv_color_hex(C_STAT), 0);
    if (f12) lv_obj_set_style_text_font(g_st_r, f12, 0);

    rebuild_tabs();
    if (g_sel >= 0) load_sel();
    else {
        lv_label_set_text(g_st_r, "Metin dosyası yok");
        paint_gutter();
    }
}
