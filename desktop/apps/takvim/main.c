/* ==================================================================
 * Takvim — kalici C:/etc/calendar.txt, Gun/Hafta/Ay gorunumleri,
 *          etkinlik ekleme ve silme.
 * ================================================================== */
#include "app.h"
#include "adl_wolf.h"
#include "lv_port.h"
#include "svg.h"
#include <stdint.h>

#define C_BODY  0x14161B
#define C_LINE  0x2C313B
#define C_GRID  0x1F232B
#define C_TXT   0xE8EAEE
#define C_DIM   0x8B919C
#define C_MUTE  0x7D8491
#define C_ACC   0x3B82F6
#define C_EVBG  0x1E3A5F
#define C_HR    44
#define C_TOP   8
#define C_NHR   13            /* 08:00 .. 20:00 */
#define CAL_MAX 128
#define CAL_FILE "C:/etc/calendar.txt"

typedef struct {
    int  year, month, day;
    int  hour, min;
    char title[64];
} cal_ev_t;

static asi_t    *A;
static lv_obj_t *g_win;
static int       g_view = 1;       /* 0=Gun, 1=Hafta, 2=Ay */
static int       g_off  = 0;       /* gun/hafta/ay kaymasi */

static cal_ev_t  g_evs[CAL_MAX];
static int       g_ev_n;

/* Modal durumu */
static lv_obj_t   *g_modal;
static lv_obj_t   *g_m_date, *g_m_time, *g_m_title;
static lv_group_t *g_m_grp;

extern const unsigned char font_ttf[];
extern const unsigned int  font_ttf_len;
static lv_font_t *f15, *f13, *f12, *f11;

#define ICON_MAX 6
static uint32_t *s_icon_buf[ICON_MAX];
static int s_icon_n;

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

static void u2s2(unsigned int v, char *out) {
    out[0] = (char)('0' + (v / 10) % 10);
    out[1] = (char)('0' + v % 10);
    out[2] = 0;
}

static void u2s4(unsigned int v, char *out) {
    out[0] = (char)('0' + (v / 1000) % 10);
    out[1] = (char)('0' + (v / 100) % 10);
    out[2] = (char)('0' + (v / 10) % 10);
    out[3] = (char)('0' + v % 10);
    out[4] = 0;
}

static lv_obj_t *svg_icon(lv_obj_t *parent, const char *svg, int size) {
    if (s_icon_n >= ICON_MAX || !A || !A->mem) return 0;
    uint32_t *buf = (uint32_t *)A->mem->alloc((size_t)size * size * 4u);
    if (!buf) return 0;
    if (svg_render(svg, size, size, buf) != 0) { A->mem->free(buf); return 0; }
    s_icon_buf[s_icon_n++] = buf;
    lv_obj_t *c = lv_canvas_create(parent);
    lv_obj_remove_style_all(c);
    lv_canvas_set_buffer(c, buf, size, size, LV_COLOR_FORMAT_ARGB8888);
    return c;
}

/* --- tarih yardimcilari (Sakamoto: 0=Pazar) --- */
static int dow_of(int y, int m, int d) {
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (int)((y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7);
}

static int days_in_month(int y, int m) {
    static const int dm[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m < 1) m = 1;
    if (m > 12) m = 12;
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return dm[m - 1];
}

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

static void today_date(int *y, int *m, int *d) {
    *y = 2026; *m = 11; *d = 26;
    if (A && A->time && A->time->now) {
        asi_datetime_t dt;
        if (A->time->now(&dt) && dt.year > 2000) {
            *y = dt.year; *m = dt.month; *d = dt.day;
        }
    }
}

static const char *const MON[12] = {
    "Ocak", "\xc5\x9e" "ubat", "Mart", "Nisan", "May\xc4\xb1" "s", "Haziran",
    "Temmuz", "A\xc4\x9f" "ustos", "Eyl\xc3\xbc" "l", "Ekim", "Kas\xc4\xb1" "m", "Aral\xc4\xb1" "k"
};
static const char *const DAYN[7] = {
    "Pzt", "Sal", "\xc3\x87" "ar", "Per", "Cum", "Cmt", "Paz"
};

static void week_dates(int y[7], int m[7], int d[7], int week_off) {
    int ty, tm, td;
    today_date(&ty, &tm, &td);
    int dw = dow_of(ty, tm, td);
    int from_mon = (dw + 6) % 7;
    shift_days(&ty, &tm, &td, -from_mon + week_off * 7);
    for (int i = 0; i < 7; i++) {
        y[i] = ty; m[i] = tm; d[i] = td;
        shift_days(&ty, &tm, &td, 1);
    }
}

/* ---- AFS kaliciligi: YYYY-MM-DD|HH:MM|Baslik ---- */
static int parse_int_n(const char *s, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

static void cal_load(void) {
    g_ev_n = 0;
    if (!A || !A->fs || !A->fs->read_path) return;
    static char buf[8192];
    uint32_t r = A->fs->read_path(CAL_FILE, buf, sizeof(buf) - 1);
    if (r == 0) return;
    buf[r] = 0;
    uint32_t i = 0;
    while (i < r && g_ev_n < CAL_MAX) {
        uint32_t ls = i;
        while (i < r && buf[i] != '\n' && buf[i] != '\r') i++;
        uint32_t le = i;
        while (i < r && (buf[i] == '\n' || buf[i] == '\r')) i++;
        if (le - ls < 18) continue;
        const char *ln = buf + ls;
        if (ln[4] != '-' || ln[7] != '-' || ln[10] != '|' || ln[13] != ':' || ln[16] != '|')
            continue;
        int yr = parse_int_n(ln + 0, 4);
        int mo = parse_int_n(ln + 5, 2);
        int dy = parse_int_n(ln + 8, 2);
        int hr = parse_int_n(ln + 11, 2);
        int mn = parse_int_n(ln + 14, 2);
        if (yr < 2000 || mo < 1 || mo > 12 || dy < 1 || dy > 31 || hr < 0 || hr > 23 || mn < 0 || mn > 59)
            continue;
        cal_ev_t *ev = &g_evs[g_ev_n++];
        ev->year = yr; ev->month = mo; ev->day = dy;
        ev->hour = hr; ev->min = mn;
        uint32_t tl = le - ls - 17;
        if (tl >= sizeof(ev->title)) tl = sizeof(ev->title) - 1;
        for (uint32_t k = 0; k < tl; k++) ev->title[k] = ln[17 + k];
        ev->title[tl] = 0;
    }
}

static void cal_save(void) {
    if (!A || !A->fs || !A->fs->create_in || !A->fs->write) return;
    if (A->fs->mkdir) A->fs->mkdir("C:/etc");
    static char buf[8192];
    uint32_t n = 0;
    for (int i = 0; i < g_ev_n && n + 96 < sizeof(buf); i++) {
        cal_ev_t *ev = &g_evs[i];
        char y4[6], m2[4], d2[4], h2[4], mn2[4];
        u2s4((unsigned)ev->year, y4);
        u2s2((unsigned)ev->month, m2);
        u2s2((unsigned)ev->day, d2);
        u2s2((unsigned)ev->hour, h2);
        u2s2((unsigned)ev->min, mn2);
        char line[96];
        scpy(line, sizeof(line), y4);
        scat(line, sizeof(line), "-"); scat(line, sizeof(line), m2);
        scat(line, sizeof(line), "-"); scat(line, sizeof(line), d2);
        scat(line, sizeof(line), "|"); scat(line, sizeof(line), h2);
        scat(line, sizeof(line), ":"); scat(line, sizeof(line), mn2);
        scat(line, sizeof(line), "|"); scat(line, sizeof(line), ev->title);
        scat(line, sizeof(line), "\n");
        for (int k = 0; line[k] && n < sizeof(buf); k++) buf[n++] = line[k];
    }
    void *h = A->fs->create_in("C:/etc", "calendar.txt", 0);
    if (h) A->fs->write(h, buf, n);
}

static void build_ui(void);

static void modal_close(void) {
    if (g_m_grp) {
        lv_indev_t *kp = lv_port_get_keypad();
        if (kp && lv_indev_get_group(kp) == g_m_grp) lv_indev_set_group(kp, 0);
        lv_group_delete(g_m_grp);
        g_m_grp = 0;
    }
    if (g_modal) { lv_obj_delete(g_modal); g_modal = 0; }
    g_m_date = g_m_time = g_m_title = 0;
}

static void modal_cancel_cb(lv_event_t *e) { (void)e; modal_close(); }

static void modal_save_cb(lv_event_t *e) {
    (void)e;
    if (!g_m_date || !g_m_time || !g_m_title || g_ev_n >= CAL_MAX) { modal_close(); return; }
    const char *ds = lv_textarea_get_text(g_m_date);
    const char *ts = lv_textarea_get_text(g_m_time);
    const char *tl = lv_textarea_get_text(g_m_title);
    if (!ds || slen(ds) != 10 || ds[4] != '-' || ds[7] != '-') return;
    if (!ts || slen(ts) != 5 || ts[2] != ':') return;
    if (!tl || !tl[0]) return;
    int yr = parse_int_n(ds, 4), mo = parse_int_n(ds + 5, 2), dy = parse_int_n(ds + 8, 2);
    int hr = parse_int_n(ts, 2), mn = parse_int_n(ts + 3, 2);
    if (yr < 2000 || mo < 1 || mo > 12 || dy < 1 || dy > 31 || hr < 0 || hr > 23 || mn < 0 || mn > 59)
        return;

    cal_ev_t *ev = &g_evs[g_ev_n++];
    ev->year = yr; ev->month = mo; ev->day = dy;
    ev->hour = hr; ev->min = mn;
    scpy(ev->title, sizeof(ev->title), tl);
    for (int i = 0; ev->title[i]; i++) if (ev->title[i] == '|' || ev->title[i] == '\n') ev->title[i] = ' ';
    cal_save();
    modal_close();
    build_ui();
}

static void open_add_modal(int y, int m, int d) {
    modal_close();
    g_modal = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(g_modal);
    lv_obj_set_size(g_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_modal, 150, 0);
    lv_obj_add_flag(g_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(g_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(g_modal);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 360, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B1E25), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_label_create(card);
    lv_label_set_text(hdr, "Yeni Etkinlik Ekle");
    lv_obj_set_style_text_color(hdr, lv_color_hex(C_TXT), 0);
    if (f15) lv_obj_set_style_text_font(hdr, f15, 0);

    char def_d[16], y4[6], m2[4], d2[4];
    u2s4((unsigned)y, y4); u2s2((unsigned)m, m2); u2s2((unsigned)d, d2);
    scpy(def_d, sizeof(def_d), y4);
    scat(def_d, sizeof(def_d), "-"); scat(def_d, sizeof(def_d), m2);
    scat(def_d, sizeof(def_d), "-"); scat(def_d, sizeof(def_d), d2);

    g_m_date = lv_textarea_create(card);
    lv_textarea_set_one_line(g_m_date, true);
    lv_textarea_set_max_length(g_m_date, 10);
    lv_textarea_set_text(g_m_date, def_d);
    lv_textarea_set_placeholder_text(g_m_date, "YYYY-AA-GG");
    lv_obj_set_size(g_m_date, LV_PCT(100), 38);

    g_m_time = lv_textarea_create(card);
    lv_textarea_set_one_line(g_m_time, true);
    lv_textarea_set_max_length(g_m_time, 5);
    lv_textarea_set_text(g_m_time, "10:00");
    lv_textarea_set_placeholder_text(g_m_time, "SS:DD");
    lv_obj_set_size(g_m_time, LV_PCT(100), 38);

    g_m_title = lv_textarea_create(card);
    lv_textarea_set_one_line(g_m_title, true);
    lv_textarea_set_max_length(g_m_title, 58);
    lv_textarea_set_placeholder_text(g_m_title, "Etkinlik başlığı");
    lv_obj_set_size(g_m_title, LV_PCT(100), 38);

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 36);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bc = lv_button_create(row);
    lv_obj_set_size(bc, 88, 34);
    lv_obj_set_style_bg_color(bc, lv_color_hex(0x262B35), 0);
    lv_obj_set_style_radius(bc, 8, 0);
    lv_obj_t *bcl = lv_label_create(bc);
    lv_label_set_text(bcl, "İptal");
    lv_obj_set_style_text_color(bcl, lv_color_hex(C_TXT), 0);
    lv_obj_center(bcl);
    lv_obj_add_event_cb(bc, modal_cancel_cb, LV_EVENT_CLICKED, 0);

    lv_obj_t *bs = lv_button_create(row);
    lv_obj_set_size(bs, 96, 34);
    lv_obj_set_style_bg_color(bs, lv_color_hex(C_ACC), 0);
    lv_obj_set_style_radius(bs, 8, 0);
    lv_obj_t *bsl = lv_label_create(bs);
    lv_label_set_text(bsl, "Kaydet");
    lv_obj_set_style_text_color(bsl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(bsl);
    lv_obj_add_event_cb(bs, modal_save_cb, LV_EVENT_CLICKED, 0);

    g_m_grp = lv_group_create();
    lv_group_add_obj(g_m_grp, g_m_title);
    lv_group_add_obj(g_m_grp, g_m_date);
    lv_group_add_obj(g_m_grp, g_m_time);
    lv_obj_add_event_cb(g_m_title, modal_save_cb, LV_EVENT_READY, 0);
    lv_indev_t *kp = lv_port_get_keypad();
    if (kp) lv_indev_set_group(kp, g_m_grp);
    lv_group_focus_obj(g_m_title);
}

static void add_btn_cb(lv_event_t *e) {
    (void)e;
    int ty, tm, td;
    today_date(&ty, &tm, &td);
    open_add_modal(ty, tm, td);
}

static void del_ev_confirm_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < g_ev_n) {
        for (int i = idx; i < g_ev_n - 1; i++) g_evs[i] = g_evs[i + 1];
        g_ev_n--;
        cal_save();
    }
    modal_close();
    build_ui();
}

static void ev_click_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= g_ev_n) return;
    modal_close();

    g_modal = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(g_modal);
    lv_obj_set_size(g_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_modal, 150, 0);
    lv_obj_add_flag(g_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(g_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(g_modal);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 320, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B1E25), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    cal_ev_t *ev = &g_evs[idx];
    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, ev->title);
    lv_obj_set_style_text_color(t, lv_color_hex(C_TXT), 0);
    if (f15) lv_obj_set_style_text_font(t, f15, 0);

    char dt_str[32], y4[6], m2[4], d2[4], h2[4], mn2[4];
    u2s4((unsigned)ev->year, y4); u2s2((unsigned)ev->month, m2); u2s2((unsigned)ev->day, d2);
    u2s2((unsigned)ev->hour, h2); u2s2((unsigned)ev->min, mn2);
    scpy(dt_str, sizeof(dt_str), y4);
    scat(dt_str, sizeof(dt_str), "-"); scat(dt_str, sizeof(dt_str), m2);
    scat(dt_str, sizeof(dt_str), "-"); scat(dt_str, sizeof(dt_str), d2);
    scat(dt_str, sizeof(dt_str), "  "); scat(dt_str, sizeof(dt_str), h2);
    scat(dt_str, sizeof(dt_str), ":"); scat(dt_str, sizeof(dt_str), mn2);

    lv_obj_t *dl = lv_label_create(card);
    lv_label_set_text(dl, dt_str);
    lv_obj_set_style_text_color(dl, lv_color_hex(C_DIM), 0);
    if (f12) lv_obj_set_style_text_font(dl, f12, 0);

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 36);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bc = lv_button_create(row);
    lv_obj_set_size(bc, 88, 34);
    lv_obj_set_style_bg_color(bc, lv_color_hex(0x262B35), 0);
    lv_obj_set_style_radius(bc, 8, 0);
    lv_obj_t *bcl = lv_label_create(bc);
    lv_label_set_text(bcl, "Kapat");
    lv_obj_set_style_text_color(bcl, lv_color_hex(C_TXT), 0);
    lv_obj_center(bcl);
    lv_obj_add_event_cb(bc, modal_cancel_cb, LV_EVENT_CLICKED, 0);

    lv_obj_t *bd = lv_button_create(row);
    lv_obj_set_size(bd, 110, 34);
    lv_obj_set_style_bg_color(bd, lv_color_hex(0x7F1D1D), 0);
    lv_obj_set_style_radius(bd, 8, 0);
    lv_obj_t *bdl = lv_label_create(bd);
    lv_label_set_text(bdl, "Etkinliği Sil");
    lv_obj_set_style_text_color(bdl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(bdl);
    lv_obj_add_event_cb(bd, del_ev_confirm_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
}

static void prev_cb(lv_event_t *e) { (void)e; g_off--; build_ui(); }
static void next_cb(lv_event_t *e) { (void)e; g_off++; build_ui(); }

static void view_cb(lv_event_t *e) {
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    if (v == g_view) return;
    g_view = v;
    g_off = 0;
    build_ui();
}

static void month_day_click_cb(lv_event_t *e) {
    uint32_t packed = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    int y = (int)(packed >> 16);
    int m = (int)((packed >> 8) & 0xFF);
    int d = (int)(packed & 0xFF);
    open_add_modal(y, m, d);
}

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

static void seg_item(lv_obj_t *parent, const char *txt, int view_id, int on) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 28);
    lv_obj_set_style_pad_ver(b, 5, 0);
    lv_obj_set_style_pad_hor(b, 12, 0);
    lv_obj_set_style_radius(b, 5, 0);
    if (on) {
        lv_obj_set_style_bg_color(b, lv_color_hex(C_GRID), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    }
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(on ? C_TXT : C_DIM), 0);
    if (f12) lv_obj_set_style_text_font(l, f12, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, view_cb, LV_EVENT_CLICKED, (void *)(intptr_t)view_id);
}

static void place_day_events(lv_obj_t *dc, int y, int m, int d) {
    for (int i = 0; i < g_ev_n; i++) {
        cal_ev_t *ev = &g_evs[i];
        if (ev->year != y || ev->month != m || ev->day != d) continue;
        int hr = ev->hour;
        int min = ev->min;
        if (hr < C_TOP) { hr = C_TOP; min = 0; }
        if (hr >= C_TOP + C_NHR) { hr = C_TOP + C_NHR - 1; min = 0; }
        int py = (hr - C_TOP) * C_HR + (min * C_HR) / 60 + 2;

        lv_obj_t *pill = lv_button_create(dc);
        lv_obj_remove_style_all(pill);
        lv_obj_set_pos(pill, 4, py);
        lv_obj_set_size(pill, LV_PCT(92), 36);
        lv_obj_set_style_bg_color(pill, lv_color_hex(C_EVBG), 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(pill, 5, 0);
        lv_obj_set_style_border_width(pill, 2, 0);
        lv_obj_set_style_border_side(pill, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_color(pill, lv_color_hex(C_ACC), 0);
        lv_obj_set_style_pad_hor(pill, 6, 0);
        lv_obj_set_style_pad_ver(pill, 3, 0);
        lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);

        char lbl[80], h2[4], mn2[4];
        u2s2((unsigned)ev->hour, h2);
        u2s2((unsigned)ev->min, mn2);
        scpy(lbl, sizeof(lbl), h2);
        scat(lbl, sizeof(lbl), ":");
        scat(lbl, sizeof(lbl), mn2);
        scat(lbl, sizeof(lbl), " ");
        scat(lbl, sizeof(lbl), ev->title);

        lv_obj_t *l = lv_label_create(pill);
        lv_label_set_text(l, lbl);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l, LV_PCT(100));
        lv_obj_set_style_text_color(l, lv_color_hex(C_TXT), 0);
        if (f11) lv_obj_set_style_text_font(l, f11, 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_add_event_cb(pill, ev_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void build_time_grid(int ncols, const int *yy, const int *mm, const int *dd) {
    lv_obj_t *hd = lv_obj_create(g_win);
    lv_obj_remove_style_all(hd);
    lv_obj_set_size(hd, LV_PCT(100), 34);
    lv_obj_remove_flag(hd, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(hd, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hd, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *sp = lv_obj_create(hd);
    lv_obj_remove_style_all(sp);
    lv_obj_set_size(sp, 56, 1);

    int ty, tm, td;
    today_date(&ty, &tm, &td);

    for (int i = 0; i < ncols; i++) {
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

        int dw = (dow_of(yy[i], mm[i], dd[i]) + 6) % 7;
        int is_td = (yy[i] == ty && mm[i] == tm && dd[i] == td);

        lv_obj_t *nm = lv_label_create(col);
        lv_label_set_text(nm, DAYN[dw]);
        lv_obj_set_style_text_color(nm, lv_color_hex(is_td ? C_ACC : C_DIM), 0);
        if (f12) lv_obj_set_style_text_font(nm, f12, 0);

        char db[8];
        u2s((unsigned)dd[i], db, sizeof(db));
        lv_obj_t *dd_lbl = lv_label_create(col);
        lv_label_set_text(dd_lbl, db);
        lv_obj_set_style_text_color(dd_lbl, lv_color_hex(is_td ? C_ACC : C_TXT), 0);
        if (f15) lv_obj_set_style_text_font(dd_lbl, f15, 0);
    }

    lv_obj_t *gr = lv_obj_create(g_win);
    lv_obj_remove_style_all(gr);
    lv_obj_set_width(gr, LV_PCT(100));
    lv_obj_set_flex_grow(gr, 1);
    lv_obj_set_scroll_dir(gr, LV_DIR_VER);
    lv_obj_set_style_pad_top(gr, 6, 0);
    lv_obj_set_flex_flow(gr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(gr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *tc = lv_obj_create(gr);
    lv_obj_remove_style_all(tc);
    lv_obj_set_size(tc, 56, C_NHR * C_HR);
    lv_obj_remove_flag(tc, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < C_NHR; i++) {
        lv_obj_t *l = lv_label_create(tc);
        char ht[8], h2[4];
        u2s2((unsigned)(C_TOP + i), h2);
        scpy(ht, sizeof(ht), h2);
        scat(ht, sizeof(ht), ":00");
        lv_label_set_text(l, ht);
        lv_obj_set_style_text_color(l, lv_color_hex(C_MUTE), 0);
        if (f11) lv_obj_set_style_text_font(l, f11, 0);
        lv_obj_set_width(l, 46);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(l, 0, i * C_HR - 6 < 0 ? 0 : i * C_HR - 6);
    }

    for (int i = 0; i < ncols; i++) {
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
        place_day_events(dc, yy[i], mm[i], dd[i]);
    }
}

static void build_month_grid(int y, int m) {
    lv_obj_t *hd = lv_obj_create(g_win);
    lv_obj_remove_style_all(hd);
    lv_obj_set_size(hd, LV_PCT(100), 28);
    lv_obj_remove_flag(hd, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(hd, LV_FLEX_FLOW_ROW);
    for (int i = 0; i < 7; i++) {
        lv_obj_t *col = lv_obj_create(hd);
        lv_obj_remove_style_all(col);
        lv_obj_set_height(col, LV_PCT(100));
        lv_obj_set_flex_grow(col, 1);
        lv_obj_t *l = lv_label_create(col);
        lv_label_set_text(l, DAYN[i]);
        lv_obj_set_style_text_color(l, lv_color_hex(C_DIM), 0);
        if (f12) lv_obj_set_style_text_font(l, f12, 0);
        lv_obj_center(l);
    }

    lv_obj_t *grid = lv_obj_create(g_win);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    int first_dw = (dow_of(y, m, 1) + 6) % 7;
    int dim = days_in_month(y, m);
    int ty, tm, td;
    today_date(&ty, &tm, &td);

    int d = 1 - first_dw;
    for (int row = 0; row < 6; row++) {
        lv_obj_t *rw = lv_obj_create(grid);
        lv_obj_remove_style_all(rw);
        lv_obj_set_width(rw, LV_PCT(100));
        lv_obj_set_flex_grow(rw, 1);
        lv_obj_set_flex_flow(rw, LV_FLEX_FLOW_ROW);
        lv_obj_remove_flag(rw, LV_OBJ_FLAG_SCROLLABLE);
        for (int col = 0; col < 7; col++, d++) {
            lv_obj_t *cell = lv_obj_create(rw);
            lv_obj_remove_style_all(cell);
            lv_obj_set_height(cell, LV_PCT(100));
            lv_obj_set_flex_grow(cell, 1);
            lv_obj_set_style_border_width(cell, 1, 0);
            lv_obj_set_style_border_color(cell, lv_color_hex(C_GRID), 0);
            lv_obj_set_style_pad_all(cell, 6, 0);
            lv_obj_set_style_pad_row(cell, 3, 0);
            lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
            lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

            if (d >= 1 && d <= dim) {
                int is_td = (y == ty && m == tm && d == td);
                lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
                uint32_t pk = ((uint32_t)y << 16) | ((uint32_t)m << 8) | (uint32_t)d;
                lv_obj_add_event_cb(cell, month_day_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)pk);

                char db[8];
                u2s((unsigned)d, db, sizeof(db));
                lv_obj_t *dl = lv_label_create(cell);
                lv_label_set_text(dl, db);
                lv_obj_set_style_text_color(dl, lv_color_hex(is_td ? C_ACC : C_TXT), 0);
                if (f13) lv_obj_set_style_text_font(dl, f13, 0);

                int shown = 0;
                for (int i = 0; i < g_ev_n && shown < 2; i++) {
                    if (g_evs[i].year == y && g_evs[i].month == m && g_evs[i].day == d) {
                        lv_obj_t *evp = lv_label_create(cell);
                        lv_label_set_text(evp, g_evs[i].title);
                        lv_label_set_long_mode(evp, LV_LABEL_LONG_DOT);
                        lv_obj_set_width(evp, LV_PCT(100));
                        lv_obj_set_style_bg_color(evp, lv_color_hex(C_EVBG), 0);
                        lv_obj_set_style_bg_opa(evp, LV_OPA_COVER, 0);
                        lv_obj_set_style_radius(evp, 3, 0);
                        lv_obj_set_style_pad_hor(evp, 4, 0);
                        lv_obj_set_style_text_color(evp, lv_color_hex(C_TXT), 0);
                        if (f11) lv_obj_set_style_text_font(evp, f11, 0);
                        lv_obj_add_flag(evp, LV_OBJ_FLAG_CLICKABLE);
                        lv_obj_add_event_cb(evp, ev_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
                        shown++;
                    }
                }
            }
        }
    }
}

static void build_ui(void) {
    if (!g_win) return;
    for (int i = 0; i < s_icon_n; i++)
        if (A && A->mem) A->mem->free(s_icon_buf[i]);
    s_icon_n = 0;
    lv_obj_clean(g_win);

    int wy[7], wm[7], wd[7];
    int cur_y = 2026, cur_m = 11, cur_d = 26;
    today_date(&cur_y, &cur_m, &cur_d);

    if (g_view == 0) {
        shift_days(&cur_y, &cur_m, &cur_d, g_off);
        wy[0] = cur_y; wm[0] = cur_m; wd[0] = cur_d;
    } else if (g_view == 1) {
        week_dates(wy, wm, wd, g_off);
    } else {
        int tot_m = cur_y * 12 + (cur_m - 1) + g_off;
        cur_y = tot_m / 12;
        cur_m = (tot_m % 12) + 1;
        if (cur_m <= 0) { cur_m += 12; cur_y--; }
        wy[0] = cur_y; wm[0] = cur_m; wd[0] = 1;
    }

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
        char yr[8], txt[40];
        u2s((unsigned)wy[0], yr, sizeof(yr));
        scpy(txt, sizeof(txt), MON[wm[0] - 1]);
        scat(txt, sizeof(txt), " ");
        scat(txt, sizeof(txt), yr);
        lv_obj_t *l = lv_label_create(tb);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_color(l, lv_color_hex(C_TXT), 0);
        if (f15) lv_obj_set_style_text_font(l, f15, 0);
        lv_obj_set_style_pad_left(l, 6, 0);
    }

    if (g_view == 1) {
        char txt[72], nb[8];
        u2s((unsigned)wd[0], nb, sizeof(nb));
        scpy(txt, sizeof(txt), nb);
        scat(txt, sizeof(txt), " "); scat(txt, sizeof(txt), MON[wm[0] - 1]);
        scat(txt, sizeof(txt), " \xe2\x80\x93 ");
        u2s((unsigned)wd[6], nb, sizeof(nb));
        scat(txt, sizeof(txt), nb);
        scat(txt, sizeof(txt), " "); scat(txt, sizeof(txt), MON[wm[6] - 1]);
        lv_obj_t *l = lv_label_create(tb);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_color(l, lv_color_hex(C_DIM), 0);
        if (f12) lv_obj_set_style_text_font(l, f12, 0);
    } else if (g_view == 0) {
        char txt[40], nb[8];
        u2s((unsigned)wd[0], nb, sizeof(nb));
        scpy(txt, sizeof(txt), nb);
        scat(txt, sizeof(txt), " "); scat(txt, sizeof(txt), MON[wm[0] - 1]);
        lv_obj_t *l = lv_label_create(tb);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_color(l, lv_color_hex(C_DIM), 0);
        if (f12) lv_obj_set_style_text_font(l, f12, 0);
    }

    {
        lv_obj_t *sp = lv_obj_create(tb);
        lv_obj_remove_style_all(sp);
        lv_obj_set_height(sp, 1);
        lv_obj_set_flex_grow(sp, 1);
    }

    lv_obj_t *badd = lv_button_create(tb);
    lv_obj_set_size(badd, LV_SIZE_CONTENT, 30);
    lv_obj_set_style_radius(badd, 6, 0);
    lv_obj_set_style_bg_color(badd, lv_color_hex(C_ACC), 0);
    lv_obj_set_style_pad_hor(badd, 12, 0);
    lv_obj_set_style_shadow_width(badd, 0, 0);
    lv_obj_t *baddl = lv_label_create(badd);
    lv_label_set_text(baddl, "+ Etkinlik Ekle");
    lv_obj_set_style_text_color(baddl, lv_color_hex(0xFFFFFF), 0);
    if (f12) lv_obj_set_style_text_font(baddl, f12, 0);
    lv_obj_center(baddl);
    lv_obj_add_event_cb(badd, add_btn_cb, LV_EVENT_CLICKED, 0);

    lv_obj_t *seg = lv_obj_create(tb);
    lv_obj_remove_style_all(seg);
    lv_obj_set_size(seg, LV_SIZE_CONTENT, 30);
    lv_obj_set_style_radius(seg, 6, 0);
    lv_obj_set_style_border_width(seg, 1, 0);
    lv_obj_set_style_border_color(seg, lv_color_hex(C_LINE), 0);
    lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(seg, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    seg_item(seg, "G\xc3\xbcn", 0, g_view == 0);
    seg_item(seg, "Hafta", 1, g_view == 1);
    seg_item(seg, "Ay", 2, g_view == 2);

    if (g_view == 0) build_time_grid(1, wy, wm, wd);
    else if (g_view == 1) build_time_grid(7, wy, wm, wd);
    else build_month_grid(wy[0], wm[0]);
}

static void cal_relayout(lv_obj_t *win) {
    (void)win;
    build_ui();
}

static void app_del_cb(lv_event_t *e) {
    (void)e;
    modal_close();
    for (int i = 0; i < s_icon_n; i++)
        if (A && A->mem) A->mem->free(s_icon_buf[i]);
    s_icon_n = 0;
    g_win = 0;
}

void app_entry(lv_obj_t *win, asi_t *asi) {
    A = asi;
    g_win = win;
    g_off = 0;
    g_view = 1;

    if (!f15) f15 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 15);
    if (!f13) f13 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 13);
    if (!f12) f12 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 12);
    if (!f11) f11 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 11);

    cal_load();

    lv_obj_set_flex_flow(win, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(win, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(win, 0, 0);
    lv_obj_set_style_pad_row(win, 0, 0);
    lv_obj_set_style_bg_color(win, lv_color_hex(C_BODY), 0);
    lv_obj_set_style_bg_opa(win, LV_OPA_COVER, 0);
    lv_obj_remove_flag(win, LV_OBJ_FLAG_SCROLLABLE);

    arctian_shell_set_relayout(win, cal_relayout);
    lv_obj_add_event_cb(win, app_del_cb, LV_EVENT_DELETE, 0);
    build_ui();
}
