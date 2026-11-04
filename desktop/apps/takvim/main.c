/* ==================================================================
 * Takvim
 *
 * Arayuz spesifikasyonu (tasarim.md bolum 6):
 *   - 52 px arac satiri: 30 px onceden/sonraki (1 px #2c313b kenarlik),
 *     15 px kalin ay-yil, 12 px tarih araligi, sagda Gun|Hafta|Ay
 *     bolmeli dugme (Hafta secili #1f232b)
 *   - gun basliklari 34 px: 56 px saat sutunu + yedi esit sutun
 *   - saat izgarasi 09:00-17:00, saat basina 48 px, cizgiler #1f232b
 *   - etkinlikler mutlak konumlu (konum = (saat-9) x 48 px)
 * Tarihler saat surucusunden (RTC) gercek okunur. Etkinlik verisi
 * surucude tutulmadigi icin izgar bos basilir - uyduurma etkinlik
 * kutusu cozmek yerine bos birakildi (bilinen fark). 09:00 etiketi
 * spec'teki kirpma hatasi olmadan 6 px ust boslukla cizilir.
 * ================================================================== */
#include "app.h"
#include "adl_wolf.h"
#include "svg.h"
#include <stdint.h>

/* spec paleti */
#define C_BODY  0x14161B
#define C_LINE  0x2C313B
#define C_GRID  0x1F232B
#define C_TXT   0xE8EAEE
#define C_DIM   0x8B919C
#define C_MUTE  0x7D8491
#define C_HR    48            /* saat basina piksel */
#define C_TOP   9             /* izgara baslangic saati */
#define C_NHR   9             /* 09:00 .. 17:00 etiketleri */

static asi_t    *A;
static lv_obj_t *g_win;

/* gosterilen hafta: su andan itibaren hafta kaymasi (-2..+2) */
static int g_week_off;

extern const unsigned char font_ttf[];
extern const unsigned int  font_ttf_len;
static lv_font_t *f15, *f13, *f12, *f11;

#define ICON_MAX 4
static uint32_t *s_icon_buf[ICON_MAX];
static int s_icon_n;

static lv_obj_t *svg_icon(lv_obj_t *parent, const char *svg, int size) {
    if (s_icon_n >= ICON_MAX) return 0;
    uint32_t *buf = (uint32_t *)A->mem->alloc((size_t)size * size * 4u);
    if (!buf) return 0;
    if (svg_render(svg, size, size, buf) != 0) { A->mem->free(buf); return 0; }
    s_icon_buf[s_icon_n++] = buf;
    lv_obj_t *c = lv_canvas_create(parent);
    lv_obj_remove_style_all(c);
    lv_canvas_set_buffer(c, buf, size, size, LV_COLOR_FORMAT_ARGB8888);
    return c;
}

static void u2s(unsigned int v, char *out, int cap) {
    char t[12];
    int n = 0, k = 0;
    if (v == 0) t[n++] = '0';
    while (v > 0 && n < (int)sizeof(t)) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n && k < cap - 1) out[k++] = t[--n];
    out[k] = 0;
}

/* --- tarih hesaplan (Sakamoto: 0=Pazar) --- */
static int dow_of(int y, int m, int d) {
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (int)((y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7);
}

static int days_in_month(int y, int m) {
    static const int dm[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return dm[m - 1];
}

/* tarihi n gun kaydir (n negatif olabilir) */
static void shift_days(int *y, int *m, int *d, int n) {
    *d += n;
    while (*d < 1) {
        (*m)--;
        if (*m < 1) { *m = 12; (*y)--; }
        *d += days_in_month(*y, *m);
    }
    for (;;) {
        int dim = days_in_month(*y, *m);
        if (*d <= dim) break;
        *d -= dim;
        (*m)++;
        if (*m > 12) { *m = 1; (*y)++; }
    }
}

static const char *const MON[12] = {
    "Ocak", "Subat", "Mart", "Nisan", "Mayis", "Haziran",
    "Temmuz", "Agustos", "Eylul", "Ekim", "Kasim", "Aralik"
};
static const char *const DAYN[7] = {
    "Pzt", "Sal", "\xc3\x87" "ar", "Per", "Cum", "Cmt", "Paz"
};

/* gosterilen haftanin Pazartesi/cuma... tarihlerini uret */
static void week_dates(int y[7], int m[7], int d[7]) {
    int ty = 2026, tm = 1, td = 1;   /* saat yoksa: sistem tarihi bilinmiyor */
    int have = 0;
    if (A->time && A->time->now) {
        asi_datetime_t dt;
        if (A->time->now(&dt) && dt.year > 2000) {
            ty = dt.year; tm = dt.month; td = dt.day; have = 1;
        }
    }
    (void)have;
    int dw = dow_of(ty, tm, td);          /* 0=Pazar */
    int from_mon = (dw + 6) % 7;
    shift_days(&ty, &tm, &td, -from_mon + g_week_off * 7);
    for (int i = 0; i < 7; i++) {
        y[i] = ty; m[i] = tm; d[i] = td;
        shift_days(&ty, &tm, &td, 1);
    }
}

static void prev_cb(lv_event_t *e);
static void next_cb(lv_event_t *e);
static void build_ui(void);

/* ---- arac satiri ---- */
static void nav_btn(lv_obj_t *parent, const char *svg, lv_event_cb_t cb) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 30, 30);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1E2129), LV_STATE_HOVERED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_HOVERED);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_t *ic = svg_icon(b, svg, 16);
    if (ic) lv_obj_center(ic);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, 0);
}

static void seg_item(lv_obj_t *parent, const char *txt, int on) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(on ? C_TXT : C_DIM), 0);
    if (f12) lv_obj_set_style_text_font(l, f12, 0);
    lv_obj_set_style_pad_ver(l, 6, 0);
    lv_obj_set_style_pad_hor(l, 12, 0);
    if (on) {
        lv_obj_set_style_bg_color(l, lv_color_hex(C_GRID), 0);
        lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    }
}

/* ---- govdeyi yeniden kur (onceden/sonraki tiklamalarinda) ---- */
static void build_ui(void) {
    /* eski tamponlari birak */
    for (int i = 0; i < s_icon_n; i++)
        if (A->mem) A->mem->free(s_icon_buf[i]);
    s_icon_n = 0;
    lv_obj_clean(g_win);

    int wy[7], wm[7], wd[7];
    week_dates(wy, wm, wd);

    /* --- arac satiri 52 px --- */
    lv_obj_t *tb = lv_obj_create(g_win);
    lv_obj_remove_style_all(tb);
    lv_obj_set_size(tb, LV_PCT(100), 52);
    lv_obj_set_style_border_width(tb, 1, 0);
    lv_obj_set_style_border_side(tb, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(tb, lv_color_hex(C_LINE), 0);
    lv_obj_remove_flag(tb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_hor(tb, 16, 0);
    lv_obj_set_style_pad_column(tb, 10, 0);
    lv_obj_set_flex_flow(tb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tb, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    nav_btn(tb, "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#A3A8B3\" stroke-width=\"2\"><path d=\"M15 18l-6-6 6-6\"/></svg>", prev_cb);
    nav_btn(tb, "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#A3A8B3\" stroke-width=\"2\"><path d=\"M9 18l6-6-6-6\"/></svg>", next_cb);

    {
        /* ay yil: haftanin ilk gununun ayi ("Kasim 2026") */
        char yr[8];
        u2s((unsigned int)wy[0], yr, sizeof(yr));
        char txt[40];
        const char *mn = MON[wm[0] - 1];
        int k = 0;
        for (int i = 0; mn[i]; i++) txt[k++] = mn[i];
        txt[k++] = ' ';
        for (int i = 0; yr[i]; i++) txt[k++] = yr[i];
        txt[k] = 0;
        lv_obj_t *l = lv_label_create(tb);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_color(l, lv_color_hex(C_TXT), 0);
        if (f15) lv_obj_set_style_text_font(l, f15, 0);
        lv_obj_set_style_pad_left(l, 6, 0);
    }
    {
        /* tarih araligi: "26 Ekim - 1 Kasim" (en-dash) */
        char txt[72];
        int k = 0;
        const char *dash = " \xe2\x80\x93 ";
        char nb[8];
        u2s((unsigned int)wd[0], nb, sizeof(nb));
        for (int i = 0; nb[i]; i++) txt[k++] = nb[i];
        txt[k++] = ' ';
        for (int i = 0; MON[wm[0] - 1][i]; i++) txt[k++] = MON[wm[0] - 1][i];
        for (int i = 0; dash[i]; i++) txt[k++] = dash[i];
        u2s((unsigned int)wd[6], nb, sizeof(nb));
        for (int i = 0; nb[i]; i++) txt[k++] = nb[i];
        txt[k++] = ' ';
        for (int i = 0; MON[wm[6] - 1][i]; i++) txt[k++] = MON[wm[6] - 1][i];
        txt[k] = 0;
        lv_obj_t *l = lv_label_create(tb);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_color(l, lv_color_hex(C_DIM), 0);
        if (f12) lv_obj_set_style_text_font(l, f12, 0);
    }

    /* sagda bolmeli gorunum dugmesi (Hafta secili; gun/ ay gorunumu yok) */
    {
        lv_obj_t *sp = lv_obj_create(tb);
        lv_obj_remove_style_all(sp);
        lv_obj_set_height(sp, 1);
        lv_obj_set_flex_grow(sp, 1);
    }
    lv_obj_t *seg = lv_obj_create(tb);
    lv_obj_remove_style_all(seg);
    lv_obj_set_size(seg, LV_SIZE_CONTENT, 30);
    lv_obj_set_style_radius(seg, 6, 0);
    lv_obj_set_style_border_width(seg, 1, 0);
    lv_obj_set_style_border_color(seg, lv_color_hex(C_LINE), 0);
    lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(seg, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    seg_item(seg, "G\xC3\xBCn", 0);
    seg_item(seg, "Hafta", 1);
    seg_item(seg, "Ay", 0);

    /* --- gun basliklari 34 px --- */
    lv_obj_t *hd = lv_obj_create(g_win);
    lv_obj_remove_style_all(hd);
    lv_obj_set_size(hd, LV_PCT(100), 34);
    lv_obj_remove_flag(hd, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(hd, 0, 0);
    lv_obj_set_flex_flow(hd, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hd, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    {
        lv_obj_t *sp = lv_obj_create(hd);
        lv_obj_remove_style_all(sp);
        lv_obj_set_size(sp, 56, 1);
    }
    for (int i = 0; i < 7; i++) {
        lv_obj_t *col = lv_obj_create(hd);
        lv_obj_remove_style_all(col);
        lv_obj_set_height(col, LV_PCT(100));
        lv_obj_set_flex_grow(col, 1);
        lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_border_width(col, 1, 0);
        lv_obj_set_style_border_side(col, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_color(col, lv_color_hex(C_GRID), 0);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(col, 6, 0);
        lv_obj_t *nm = lv_label_create(col);
        lv_label_set_text(nm, DAYN[i]);
        lv_obj_set_style_text_color(nm, lv_color_hex(C_DIM), 0);
        if (f12) lv_obj_set_style_text_font(nm, f12, 0);
        char db[8];
        u2s((unsigned int)wd[i], db, sizeof(db));
        lv_obj_t *dd = lv_label_create(col);
        lv_label_set_text(dd, db);
        lv_obj_set_style_text_color(dd, lv_color_hex(C_TXT), 0);
        if (f15) lv_obj_set_style_text_font(dd, f15, 0);
    }

    /* --- saat izgarasi --- */
    lv_obj_t *gr = lv_obj_create(g_win);
    lv_obj_remove_style_all(gr);
    lv_obj_set_width(gr, LV_PCT(100));
    lv_obj_set_flex_grow(gr, 1);
    lv_obj_remove_flag(gr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(gr, 0, 0);
    lv_obj_set_style_pad_top(gr, 6, 0);   /* 09:00 etiketi kirpilmasin */
    lv_obj_set_flex_flow(gr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(gr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* saat etiketleri sutunu (56 px, saga yasli) */
    lv_obj_t *tc = lv_obj_create(gr);
    lv_obj_remove_style_all(tc);
    lv_obj_set_size(tc, 56, C_NHR * C_HR);
    lv_obj_remove_flag(tc, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < C_NHR; i++) {
        lv_obj_t *l = lv_label_create(tc);
        char ht[8];
        int hh = C_TOP + i;
        ht[0] = (char)('0' + (hh / 10) % 10);
        ht[1] = (char)('0' + hh % 10);
        ht[2] = ':'; ht[3] = '0'; ht[4] = '0'; ht[5] = 0;
        lv_label_set_text(l, ht);
        lv_obj_set_style_text_color(l, lv_color_hex(C_MUTE), 0);
        if (f11) lv_obj_set_style_text_font(l, f11, 0);
        lv_obj_set_width(l, 46);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(l, 0, i * C_HR - 6 < 0 ? 0 : i * C_HR - 6);
    }

    /* yedi gun sutunu: 1 px sol cizgi + yatay saat cizgileri */
    for (int i = 0; i < 7; i++) {
        lv_obj_t *dc = lv_obj_create(gr);
        lv_obj_remove_style_all(dc);
        lv_obj_set_height(dc, C_NHR * C_HR);
        lv_obj_set_flex_grow(dc, 1);
        lv_obj_remove_flag(dc, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_border_width(dc, 1, 0);
        lv_obj_set_style_border_side(dc, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_color(dc, lv_color_hex(C_GRID), 0);
        for (int h = 1; h < C_NHR; h++) {
            lv_obj_t *ln = lv_obj_create(dc);
            lv_obj_remove_style_all(ln);
            lv_obj_set_size(ln, LV_PCT(100), 1);
            lv_obj_set_style_bg_color(ln, lv_color_hex(C_GRID), 0);
            lv_obj_set_style_bg_opa(ln, LV_OPA_COVER, 0);
            lv_obj_set_pos(ln, 0, h * C_HR);
        }
        /* etkinlikler: surucude etkinlik deposu yok - bos sutun */
    }
}

static void prev_cb(lv_event_t *e) { (void)e; if (g_week_off > -53) g_week_off--; build_ui(); }
static void next_cb(lv_event_t *e) { (void)e; if (g_week_off < 53) g_week_off++; build_ui(); }

static void app_del_cb(lv_event_t *e) {
    (void)e;
    for (int i = 0; i < s_icon_n; i++)
        if (A->mem) A->mem->free(s_icon_buf[i]);
    s_icon_n = 0;
    g_win = 0;
}

void app_entry(lv_obj_t *win, asi_t *asi) {
    A = asi;
    g_win = win;
    g_week_off = 0;

    if (!f15) f15 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 15);
    if (!f13) f13 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 13);
    if (!f12) f12 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 12);
    if (!f11) f11 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 11);

    lv_obj_set_flex_flow(win, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(win, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(win, 0, 0);
    lv_obj_set_style_pad_row(win, 0, 0);
    lv_obj_set_style_bg_color(win, lv_color_hex(C_BODY), 0);
    lv_obj_set_style_bg_opa(win, LV_OPA_COVER, 0);
    lv_obj_remove_flag(win, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(win, app_del_cb, LV_EVENT_DELETE, 0);
    build_ui();
}
