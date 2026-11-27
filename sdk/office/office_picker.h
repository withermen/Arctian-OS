/**
 * office_picker.h — Ortak Ofis "Aç / Farklı Kaydet" dosya seçici penceresi
 * =========================================================================
 *
 * libWolf (wolf.h) API'si ile freestanding derlenir (libc yok).
 * Sabit widget/olay bütçesi: modal bir kez oluşturulur, aç/kapatta
 * wolf_visible() ile gizlenip yeniden kullanılır; böylece Abaküs gibi
 * yüksek slot kullanan uygulamalarda bile WOLF_MAX_EV (128) aşılmaz.
 *
 * Kullanım:
 *   - build() içinde (wolf_clear sonrası): office_picker_reset();
 *   - Aç:            office_picker_open(OP_MODE_OPEN, ...);
 *   - Farklı Kaydet: office_picker_open(OP_MODE_SAVE, ...);
 */
#ifndef ARCTIAN_OFFICE_PICKER_H
#define ARCTIAN_OFFICE_PICKER_H

#include "wolf.h"

#define OP_MODE_OPEN 0
#define OP_MODE_SAVE 1

#define OP_MAX_ITEMS 64
#define OP_VIS_ROWS  8
#define OP_PATH_CAP  192
#define OP_NAME_CAP  96

typedef void (*office_pick_cb)(const char *full_path, void *user);

typedef struct {
    char name[OP_NAME_CAP];
    int  is_dir;
    int  size;
} op_item_t;

static int  g_op_w = 1280, g_op_h = 720, g_op_s = 1000;
static int  g_op_built = 0;
static int  g_op_open  = 0;
static int  g_op_mode  = OP_MODE_OPEN;

static char g_op_dir[OP_PATH_CAP]     = "C:/belgeler";
static char g_op_filter[64]           = "";
static char g_op_def_ext[16]          = "";
static char g_op_title_str[64]        = "Dosya Aç";
static office_pick_cb g_op_cb         = 0;
static void          *g_op_user       = 0;

static op_item_t g_op_items[OP_MAX_ITEMS];
static int       g_op_nitems = 0;
static int       g_op_page   = 0;
static int       g_op_sel    = -1;

static wolf_id g_op_ov       = 0;
static wolf_id g_op_title    = 0;
static wolf_id g_op_dir_lbl  = 0;
static wolf_id g_op_page_lbl = 0;
static wolf_id g_op_empty    = 0;
static wolf_id g_op_inp      = 0;
static wolf_id g_op_ok_lbl   = 0;
static wolf_id g_op_row[OP_VIS_ROWS];
static wolf_id g_op_row_tag[OP_VIS_ROWS];
static wolf_id g_op_row_name[OP_VIS_ROWS];
static wolf_id g_op_row_meta[OP_VIS_ROWS];

/* ---- küçük yardımcılar (freestanding) ---- */
static int op_S(int v) { return (v * g_op_s + 500) / 1000; }
static int op_F(int px)
{
    int v = (px * g_op_s + 500) / 1000;
    if (v < 6) v = 6;
    return (v + 1) / 2 * 2;
}

static int op_slen(const char *s)
{
    int n = 0;
    if (s) while (s[n]) n++;
    return n;
}

static void op_scpy(char *d, int cap, const char *s)
{
    int i = 0;
    if (cap <= 0) return;
    if (!s) { d[0] = 0; return; }
    for (; i < cap - 1 && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}

static void op_scat(char *d, int cap, const char *s)
{
    int n = op_slen(d);
    if (n < cap) op_scpy(d + n, cap - n, s);
}

static char op_low(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int op_streq_ci(const char *a, const char *b)
{
    int i = 0;
    if (!a || !b) return 0;
    while (a[i] && b[i]) {
        if (op_low(a[i]) != op_low(b[i])) return 0;
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}

static int op_strcmp_ci(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) {
        char ca = op_low(a[i]), cb = op_low(b[i]);
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        i++;
    }
    return (int)(unsigned char)op_low(a[i]) - (int)(unsigned char)op_low(b[i]);
}

static void op_itoa(int v, char *out, int cap)
{
    char t[16];
    int n = 0, o = 0;
    if (cap <= 0) return;
    if (v < 0) { if (o < cap - 1) out[o++] = '-'; v = -v; }
    do { t[n++] = (char)('0' + (v % 10)); v /= 10; } while (v && n < 15);
    while (n > 0 && o < cap - 1) out[o++] = t[--n];
    out[o] = 0;
}

static void op_norm_dir(char *dir)
{
    int n = op_slen(dir);
    if (n == 0) { op_scpy(dir, OP_PATH_CAP, "C:/belgeler"); return; }
    while (n > 3 && dir[n - 1] == '/') { dir[--n] = 0; }
    if (n == 2 && dir[1] == ':') { dir[2] = '/'; dir[3] = 0; }
}

static void op_join_path(const char *dir, const char *name, char *out, int cap)
{
    op_scpy(out, cap, dir);
    int n = op_slen(out);
    if (n > 0 && out[n - 1] != '/') op_scat(out, cap, "/");
    op_scat(out, cap, name);
}

static int op_ext_matches(const char *name, const char *filter)
{
    if (!filter || !filter[0]) return 1;
    const char *dot = 0;
    for (const char *s = name; *s; s++) if (*s == '.') dot = s;
    if (!dot) return 0;

    const char *p = filter;
    while (*p) {
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        char ext[16];
        int k = 0;
        while (*p && *p != ',' && *p != ' ' && k < 15) ext[k++] = *p++;
        ext[k] = 0;
        if (op_streq_ci(dot, ext)) return 1;
    }
    return 0;
}

static void op_fmt_size(int bytes, char *out, int cap)
{
    if (bytes < 0) { op_scpy(out, cap, "—"); return; }
    if (bytes < 1024) {
        op_itoa(bytes, out, cap);
        op_scat(out, cap, " B");
    } else {
        op_itoa((bytes + 1023) / 1024, out, cap);
        op_scat(out, cap, " KB");
    }
}

static void op_style_box(wolf_id w, uint32_t bg, int opa, int r, uint32_t brd)
{
    wolf_style(w, WOLF_SP_BG_COLOR, (int)bg, 0);
    wolf_style(w, WOLF_SP_BG_OPA, opa, 0);
    wolf_style(w, WOLF_SP_RADIUS, r, 0);
    if (brd) {
        wolf_style(w, WOLF_SP_BORDER_WIDTH, 1, 0);
        wolf_style(w, WOLF_SP_BORDER_COLOR, (int)brd, 0);
        wolf_style(w, WOLF_SP_BORDER_SIDE, 15, 0);
    } else {
        wolf_style(w, WOLF_SP_BORDER_WIDTH, 0, 0);
    }
}

static void op_hov(wolf_id w, uint32_t bg)
{
    wolf_style(w, WOLF_SP_BG_COLOR, (int)bg, WOLF_SEL(WOLF_SF_HOVER, 0));
    wolf_style(w, WOLF_SP_BG_OPA, 100, WOLF_SEL(WOLF_SF_HOVER, 0));
}

static wolf_id op_lab(wolf_id p, int x, int y, int wpx, const char *t,
                      int px, uint32_t col, int al)
{
    wolf_id l = wolf_label(p, x, y, t);
    wolf_resize(l, op_S(wpx), op_F(px) + 6);
    wolf_style(l, WOLF_SP_TEXT_LONG, 2, 0);
    if (al >= 0) wolf_style(l, WOLF_SP_TEXT_ALIGN, al, 0);
    wolf_style(l, WOLF_SP_FONT_PX, op_F(px), 0);
    wolf_style(l, WOLF_SP_TEXT_COLOR, (int)col, 0);
    return l;
}

static void op_scan_dir(void)
{
    op_norm_dir(g_op_dir);
    g_op_nitems = 0;
    g_op_sel = -1;

    int total = wolf_fs_count(g_op_dir);
    char nm[OP_NAME_CAP];
    for (int i = 0; i < total && g_op_nitems < OP_MAX_ITEMS; i++) {
        int isd = 0;
        if (wolf_fs_list(g_op_dir, i, nm, sizeof(nm), &isd) != 0) break;
        if (!nm[0] || nm[0] == '.') continue;
        if (!isd && !op_ext_matches(nm, g_op_filter)) continue;

        op_item_t *it = &g_op_items[g_op_nitems++];
        op_scpy(it->name, sizeof(it->name), nm);
        it->is_dir = isd ? 1 : 0;
        if (isd) {
            it->size = -1;
        } else {
            char full[OP_PATH_CAP];
            op_join_path(g_op_dir, nm, full, sizeof(full));
            it->size = wolf_fs_size(full);
        }
    }

    /* Klasörler önce, ardından ada göre sırala (yerinde eklemeli sıralama) */
    for (int i = 1; i < g_op_nitems; i++) {
        op_item_t key;
        unsigned char *kd = (unsigned char *)&key;
        const unsigned char *ks = (const unsigned char *)&g_op_items[i];
        for (unsigned b = 0; b < sizeof(op_item_t); b++) kd[b] = ks[b];

        int j = i - 1;
        while (j >= 0) {
            int swap = 0;
            if (g_op_items[j].is_dir != key.is_dir) {
                swap = (!g_op_items[j].is_dir && key.is_dir);
            } else if (op_strcmp_ci(g_op_items[j].name, key.name) > 0) {
                swap = 1;
            }
            if (!swap) break;
            unsigned char *dst = (unsigned char *)&g_op_items[j + 1];
            const unsigned char *src = (const unsigned char *)&g_op_items[j];
            for (unsigned b = 0; b < sizeof(op_item_t); b++) dst[b] = src[b];
            j--;
        }
        unsigned char *dst = (unsigned char *)&g_op_items[j + 1];
        const unsigned char *src = (const unsigned char *)&key;
        for (unsigned b = 0; b < sizeof(op_item_t); b++) dst[b] = src[b];
    }

    int max_page = (g_op_nitems > 0) ? (g_op_nitems - 1) / OP_VIS_ROWS : 0;
    if (g_op_page > max_page) g_op_page = max_page;
    if (g_op_page < 0) g_op_page = 0;
}

static void op_refresh_view(void)
{
    if (!g_op_built) return;
    wolf_text(g_op_title, g_op_title_str);
    wolf_text(g_op_dir_lbl, g_op_dir);
    wolf_text(g_op_ok_lbl, (g_op_mode == OP_MODE_SAVE) ? "Kaydet" : "Aç");

    int pages = (g_op_nitems + OP_VIS_ROWS - 1) / OP_VIS_ROWS;
    if (pages < 1) pages = 1;
    char pbuf[24], n1[8], n2[8];
    op_itoa(g_op_page + 1, n1, sizeof(n1));
    op_itoa(pages, n2, sizeof(n2));
    op_scpy(pbuf, sizeof(pbuf), n1);
    op_scat(pbuf, sizeof(pbuf), " / ");
    op_scat(pbuf, sizeof(pbuf), n2);
    wolf_text(g_op_page_lbl, pbuf);

    if (g_op_nitems == 0) {
        wolf_visible(g_op_empty, 1);
    } else {
        wolf_visible(g_op_empty, 0);
    }

    int base = g_op_page * OP_VIS_ROWS;
    for (int r = 0; r < OP_VIS_ROWS; r++) {
        int idx = base + r;
        if (idx >= g_op_nitems) {
            wolf_visible(g_op_row[r], 0);
            continue;
        }
        wolf_visible(g_op_row[r], 1);
        op_item_t *it = &g_op_items[idx];
        int selected = (idx == g_op_sel);
        op_style_box(g_op_row[r],
                     selected ? 0x1E293B : 0x0F1115,
                     100, op_S(6),
                     selected ? 0x3B82F6 : 0x262B35);

        if (it->is_dir) {
            wolf_text(g_op_row_tag[r], "KLASÖR");
            wolf_style(g_op_row_tag[r], WOLF_SP_TEXT_COLOR, 0x60A5FA, 0);
            wolf_text(g_op_row_meta[r], "Dizin");
        } else {
            wolf_text(g_op_row_tag[r], "DOSYA");
            wolf_style(g_op_row_tag[r], WOLF_SP_TEXT_COLOR, 0x8B919C, 0);
            char sz[20];
            op_fmt_size(it->size, sz, sizeof(sz));
            wolf_text(g_op_row_meta[r], sz);
        }
        wolf_text(g_op_row_name[r], it->name);
    }
}

static void office_picker_close(void)
{
    g_op_open = 0;
    if (g_op_built && g_op_ov) wolf_visible(g_op_ov, 0);
}

__attribute__((unused))
static int office_picker_is_open(void) { return g_op_open; }

static void office_picker_reset(void)
{
    g_op_built = 0;
    g_op_open = 0;
    g_op_ov = 0;
}

static void op_finish_with_path(const char *path)
{
    office_pick_cb cb = g_op_cb;
    void *user = g_op_user;
    office_picker_close();
    if (cb) cb(path, user);
}

static void op_confirm_cb(wolf_id w, void *u)
{
    (void)w; (void)u;
    char raw[OP_PATH_CAP];
    raw[0] = 0;
    if (wolf_text_of(g_op_inp, raw, sizeof(raw)) < 0) raw[0] = 0;

    /* Baştaki/sondaki boşluk ve satır sonunu kırp */
    int s = 0;
    while (raw[s] == ' ' || raw[s] == '\t' || raw[s] == '\r' || raw[s] == '\n') s++;
    int e = op_slen(raw);
    while (e > s && (raw[e - 1] == ' ' || raw[e - 1] == '\t' ||
                     raw[e - 1] == '\r' || raw[e - 1] == '\n')) e--;
    raw[e] = 0;
    const char *nm = raw + s;

    if (!nm[0]) {
        wolf_notify("Dosya Seçici", "Lütfen bir dosya adı girin");
        return;
    }

    char full[OP_PATH_CAP];
    int has_colon = 0;
    for (int i = 0; nm[i]; i++) if (nm[i] == ':') { has_colon = 1; break; }
    if (has_colon) {
        op_scpy(full, sizeof(full), nm);
    } else if (nm[0] == '/') {
        op_scpy(full, sizeof(full), "C:");
        op_scat(full, sizeof(full), nm);
    } else {
        op_join_path(g_op_dir, nm, full, sizeof(full));
    }

    /* Farklı Kaydet'te uzantı yoksa varsayılan uzantıyı ekle */
    if (g_op_mode == OP_MODE_SAVE && g_op_def_ext[0]) {
        const char *base = full;
        for (const char *p = full; *p; p++) if (*p == '/') base = p + 1;
        int has_dot = 0;
        for (const char *p = base; *p; p++) if (*p == '.') { has_dot = 1; break; }
        if (!has_dot) op_scat(full, sizeof(full), g_op_def_ext);
    }

    if (g_op_mode == OP_MODE_OPEN && !wolf_fs_exists(full)) {
        wolf_notify("Dosya Seçici", "Seçilen dosya bulunamadı");
        return;
    }

    op_finish_with_path(full);
}

static void op_cancel_cb(wolf_id w, void *u)
{
    (void)w; (void)u;
    office_picker_close();
}

static void op_up_cb(wolf_id w, void *u)
{
    (void)w; (void)u;
    op_norm_dir(g_op_dir);
    if (op_streq_ci(g_op_dir, "C:/")) return;
    int n = op_slen(g_op_dir);
    while (n > 3 && g_op_dir[n - 1] != '/') n--;
    if (n <= 3) op_scpy(g_op_dir, sizeof(g_op_dir), "C:/");
    else g_op_dir[n - 1] = 0;
    g_op_page = 0;
    op_scan_dir();
    op_refresh_view();
}

static void op_chip_cb(wolf_id w, void *u)
{
    (void)w;
    const char *dir = (const char *)u;
    if (!dir) return;
    op_scpy(g_op_dir, sizeof(g_op_dir), dir);
    g_op_page = 0;
    op_scan_dir();
    op_refresh_view();
}

static void op_prev_cb(wolf_id w, void *u)
{
    (void)w; (void)u;
    if (g_op_page > 0) {
        g_op_page--;
        op_refresh_view();
    }
}

static void op_next_cb(wolf_id w, void *u)
{
    (void)w; (void)u;
    int pages = (g_op_nitems + OP_VIS_ROWS - 1) / OP_VIS_ROWS;
    if (g_op_page + 1 < pages) {
        g_op_page++;
        op_refresh_view();
    }
}

static void op_row_cb(wolf_id w, void *u)
{
    (void)w;
    int r = (int)(long)u;
    int idx = g_op_page * OP_VIS_ROWS + r;
    if (idx < 0 || idx >= g_op_nitems) return;

    op_item_t *it = &g_op_items[idx];
    if (it->is_dir) {
        char next[OP_PATH_CAP];
        op_join_path(g_op_dir, it->name, next, sizeof(next));
        op_scpy(g_op_dir, sizeof(g_op_dir), next);
        g_op_page = 0;
        op_scan_dir();
        op_refresh_view();
        return;
    }

    g_op_sel = idx;
    wolf_text(g_op_inp, it->name);
    if (g_op_mode == OP_MODE_OPEN) {
        char full[OP_PATH_CAP];
        op_join_path(g_op_dir, it->name, full, sizeof(full));
        op_finish_with_path(full);
    } else {
        op_refresh_view();
    }
}

static void op_build_modal(void)
{
    wolf_id root = wolf_root();
    if (!root) return;

    g_op_ov = wolf_panel(root, 0, 0, g_op_w, g_op_h);
    op_style_box(g_op_ov, 0x0A0B0E, 82, 0, 0);
    wolf_set_click(g_op_ov, 1);

    int bw = op_S(600), bh = op_S(476);
    if (bw > g_op_w - op_S(24)) bw = g_op_w - op_S(24);
    if (bh > g_op_h - op_S(24)) bh = g_op_h - op_S(24);
    int bx = (g_op_w - bw) / 2;
    int by = (g_op_h - bh) / 2;

    wolf_id box = wolf_panel(g_op_ov, bx, by, bw, bh);
    op_style_box(box, 0x181B22, 100, op_S(10), 0x2C313B);

    /* Başlık + Kapat */
    g_op_title = op_lab(box, op_S(20), op_S(14), 440, "Dosya Aç", 15, 0xE8EAEE, -1);
    g_op_dir_lbl = op_lab(box, op_S(20), op_S(38), 460, "C:/belgeler", 11, 0x8B919C, -1);

    wolf_id xbtn = wolf_panel(box, bw - op_S(48), op_S(12), op_S(32), op_S(28));
    op_style_box(xbtn, 0x0F1115, 100, op_S(6), 0x333A47);
    wolf_set_click(xbtn, 1); op_hov(xbtn, 0xC42B1C);
    wolf_on_click(xbtn, op_cancel_cb);
    op_lab(xbtn, 0, (op_S(28) - op_F(13) - 6) / 2, 32, "×", 13, 0xE8EAEE, 1);

    /* Hızlı klasör çipleri */
    static const char *C_DIR[4] = { "C:/belgeler", "C:/masaustu", "C:/indirilenler", "C:/" };
    static const char *C_LBL[4] = { "Belgeler", "Masaüstü", "İndirilenler", "C:/" };
    static const int C_W[4]     = { 84, 88, 96, 48 };

    wolf_id upb = wolf_panel(box, op_S(20), op_S(64), op_S(64), op_S(26));
    op_style_box(upb, 0x0F1115, 100, op_S(6), 0x333A47);
    wolf_set_click(upb, 1); op_hov(upb, 0x262B35);
    wolf_on_click(upb, op_up_cb);
    op_lab(upb, 0, (op_S(26) - op_F(11) - 6) / 2, 64, "↑ Üst", 11, 0xC9CED8, 1);

    int cx = 92;
    for (int i = 0; i < 4; i++) {
        wolf_id cb = wolf_panel(box, op_S(cx), op_S(64), op_S(C_W[i]), op_S(26));
        op_style_box(cb, 0x0F1115, 100, op_S(6), 0x333A47);
        wolf_set_click(cb, 1); op_hov(cb, 0x262B35);
        wolf_on_click_ex(cb, op_chip_cb, (void *)C_DIR[i]);
        op_lab(cb, 0, (op_S(26) - op_F(11) - 6) / 2, C_W[i], C_LBL[i], 11, 0xC9CED8, 1);
        cx += C_W[i] + 6;
    }

    /* Dosya listesi alanı */
    int lw = bw - op_S(40), ly = op_S(98), lh = bh - op_S(98) - op_S(96);
    wolf_id list = wolf_panel(box, op_S(20), ly, lw, lh);
    op_style_box(list, 0x101216, 100, op_S(8), 0x262B35);

    g_op_empty = op_lab(list, op_S(14), op_S(18), 500,
                        "Bu klasörde eşleşen dosya veya klasör yok.", 12, 0x8B919C, -1);

    int rh = (lh - op_S(12)) / OP_VIS_ROWS;
    if (rh < op_S(26)) rh = op_S(26);
    for (int r = 0; r < OP_VIS_ROWS; r++) {
        int ry = op_S(6) + r * rh;
        wolf_id row = wolf_panel(list, op_S(6), ry, lw - op_S(12), rh - op_S(3));
        op_style_box(row, 0x0F1115, 100, op_S(6), 0x262B35);
        wolf_set_click(row, 1); op_hov(row, 0x1E2330);
        wolf_on_click_ex(row, op_row_cb, (void *)(long)r);

        int ty = (rh - op_S(3) - op_F(11) - 6) / 2;
        g_op_row_tag[r]  = op_lab(row, op_S(10), ty, 64, "DOSYA", 10, 0x8B919C, 0);
        g_op_row_name[r] = op_lab(row, op_S(76), ty, 360, "", 12, 0xE8EAEE, 0);
        g_op_row_meta[r] = op_lab(row, lw - op_S(106), ty, 82, "", 11, 0x8B919C, 2);
        g_op_row[r] = row;
    }

    /* Sayfalama + Dosya adı + Aç/Kaydet/İptal */
    int py = ly + lh + op_S(8);
    wolf_id prev = wolf_panel(box, op_S(20), py, op_S(28), op_S(24));
    op_style_box(prev, 0x0F1115, 100, op_S(5), 0x333A47);
    wolf_set_click(prev, 1); op_hov(prev, 0x262B35);
    wolf_on_click(prev, op_prev_cb);
    op_lab(prev, 0, (op_S(24) - op_F(11) - 6) / 2, 28, "<", 11, 0xC9CED8, 1);

    g_op_page_lbl = op_lab(box, op_S(52), py + op_S(3), 56, "1 / 1", 11, 0x8B919C, 1);

    wolf_id next = wolf_panel(box, op_S(112), py, op_S(28), op_S(24));
    op_style_box(next, 0x0F1115, 100, op_S(5), 0x333A47);
    wolf_set_click(next, 1); op_hov(next, 0x262B35);
    wolf_on_click(next, op_next_cb);
    op_lab(next, 0, (op_S(24) - op_F(11) - 6) / 2, 28, ">", 11, 0xC9CED8, 1);

    int fy = py + op_S(32);
    op_lab(box, op_S(20), fy + op_S(6), 76, "Dosya adı:", 12, 0xC9CED8, -1);

    int inp_w = bw - op_S(20) - op_S(78) - op_S(190);
    g_op_inp = wolf_input(box, op_S(96), fy, inp_w, op_S(32), "belge.txt");
    wolf_style(g_op_inp, WOLF_SP_FONT_PX, op_F(12), 0);
    wolf_style(g_op_inp, WOLF_SP_BG_COLOR, 0x0F1115, 0);
    wolf_style(g_op_inp, WOLF_SP_TEXT_COLOR, 0xE8EAEE, 0);
    wolf_style(g_op_inp, WOLF_SP_BORDER_COLOR, 0x333A47, 0);
    wolf_style(g_op_inp, WOLF_SP_RADIUS, op_S(6), 0);
    wolf_on_enter(g_op_inp, op_confirm_cb);

    wolf_id cbtn = wolf_panel(box, bw - op_S(178), fy, op_S(72), op_S(32));
    op_style_box(cbtn, 0x0F1115, 100, op_S(6), 0x333A47);
    wolf_set_click(cbtn, 1); op_hov(cbtn, 0x262B35);
    wolf_on_click(cbtn, op_cancel_cb);
    op_lab(cbtn, 0, (op_S(32) - op_F(12) - 6) / 2, 72, "İptal", 12, 0xE8EAEE, 1);

    wolf_id obtn = wolf_panel(box, bw - op_S(98), fy, op_S(78), op_S(32));
    op_style_box(obtn, 0x3B82F6, 100, op_S(6), 0);
    wolf_set_click(obtn, 1); op_hov(obtn, 0x2E6FD6);
    wolf_on_click(obtn, op_confirm_cb);
    g_op_ok_lbl = op_lab(obtn, 0, (op_S(32) - op_F(12) - 6) / 2, 78, "Aç", 12, 0xFFFFFF, 1);

    g_op_built = 1;
}

static void office_picker_open(int mode, const char *title,
                               const char *init_dir, const char *def_name,
                               const char *ext_filter, const char *def_ext,
                               office_pick_cb cb, void *user)
{
    if (wolf_screen(&g_op_w, &g_op_h) != 0) { g_op_w = 1280; g_op_h = 720; }
    int a = g_op_w * 1000 / 1280, b = g_op_h * 1000 / 720;
    g_op_s = a < b ? a : b;
    if (g_op_s < 300) g_op_s = 300;

    g_op_mode = mode;
    op_scpy(g_op_title_str, sizeof(g_op_title_str),
            (title && title[0]) ? title : (mode == OP_MODE_SAVE ? "Farklı Kaydet" : "Dosya Aç"));
    op_scpy(g_op_dir, sizeof(g_op_dir), (init_dir && init_dir[0]) ? init_dir : "C:/belgeler");
    op_norm_dir(g_op_dir);
    op_scpy(g_op_filter, sizeof(g_op_filter), ext_filter ? ext_filter : "");
    op_scpy(g_op_def_ext, sizeof(g_op_def_ext), def_ext ? def_ext : "");
    g_op_cb = cb;
    g_op_user = user;
    g_op_page = 0;

    if (!g_op_built) op_build_modal();
    if (!g_op_built) return;

    wolf_text(g_op_inp, def_name ? def_name : "");
    op_scan_dir();
    op_refresh_view();
    wolf_visible(g_op_ov, 1);
    wolf_to_front(g_op_ov);
    g_op_open = 1;
}

#endif /* ARCTIAN_OFFICE_PICKER_H */
