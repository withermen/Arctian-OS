/* ==================================================================
 * Saat — Dünya Saatleri, Kronometre ve Zamanlayıcı
 *
 * Sekmeler:
 *   1) Saat / Dünya Saatleri (İstanbul, Londra, Berlin, New York, Tokyo)
 *   2) Kronometre (Başlat / Duraklat / Tur / Sıfırla — ms hassasiyetli)
 *   3) Zamanlayıcı / Alarm (Geri sayım + arctian_notify + bip sesi)
 * ================================================================== */
#include "app.h"
#include "adl_wolf.h"
#include <stdint.h>

extern double sin(double x);

#define C_BODY  0x14161B
#define C_BAR   0x181B22
#define C_CARD  0x1B1F28
#define C_BTN   0x232833
#define C_LINE  0x2C313B
#define C_ACC   0x3B82F6
#define C_OK    0x22C55E
#define C_WARN  0xEF4444
#define C_TXT   0xE8EAEE
#define C_DIM   0x8B919C

#define LAP_MAX 32

static asi_t      *A;
static lv_obj_t   *g_win;
static lv_obj_t   *g_content;
static lv_obj_t   *g_tab_b[3];
static lv_obj_t   *g_tab_l[3];
static lv_timer_t *g_timer;
static int         g_tab; /* 0=Saat, 1=Kronometre, 2=Zamanlayıcı */

/* Sekme 0: Dünya Saatleri */
static lv_obj_t *g_clk_big, *g_clk_date;
static lv_obj_t *g_city_time[5], *g_city_sub[5];

/* Sekme 1: Kronometre */
static int      g_sw_run;
static uint64_t g_sw_start_ms;
static uint64_t g_sw_accum_ms;
static uint64_t g_laps[LAP_MAX];
static uint64_t g_lap_deltas[LAP_MAX];
static int      g_lap_n;
static lv_obj_t *g_sw_lbl, *g_sw_start_lbl, *g_sw_laps_box;

/* Sekme 2: Zamanlayıcı */
static int      g_tm_set_sec = 60;
static int      g_tm_run;
static uint64_t g_tm_end_ms;
static uint64_t g_tm_rem_ms = 60000;
static int      g_tm_fired;
static lv_obj_t *g_tm_lbl, *g_tm_bar, *g_tm_start_lbl, *g_tm_banner;

/* Kısa bip sesi için statik WAV tamponu (44 header + 8000 örnek @ 16 kHz = 0.5 s) */
static uint8_t s_beep_wav[44 + 8000 * 2];
static int     s_beep_ready;

extern const unsigned char font_ttf[];
extern const unsigned int  font_ttf_len;
static lv_font_t *f36, *f24, *f15, *f13, *f12;

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

static void two_dig(int v, char *out) {
    if (v < 0) v = 0;
    out[0] = (char)('0' + (v / 10) % 10);
    out[1] = (char)('0' + v % 10);
    out[2] = 0;
}

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void play_alarm_beep(void) {
    if (!s_beep_ready) {
        const uint32_t rate = 16000u, n = 8000u, dsz = n * 2u;
        uint8_t *h = s_beep_wav;
        h[0]='R'; h[1]='I'; h[2]='F'; h[3]='F';
        wr32(h + 4, 36u + dsz);
        h[8]='W'; h[9]='A'; h[10]='V'; h[11]='E';
        h[12]='f'; h[13]='m'; h[14]='t'; h[15]=' ';
        wr32(h + 16, 16u);
        wr16(h + 20, 1u);
        wr16(h + 22, 1u);
        wr32(h + 24, rate);
        wr32(h + 28, rate * 2u);
        wr16(h + 32, 2u);
        wr16(h + 34, 16u);
        h[36]='d'; h[37]='a'; h[38]='t'; h[39]='a';
        wr32(h + 40, dsz);
        int16_t *pcm = (int16_t *)(s_beep_wav + 44);
        for (uint32_t i = 0; i < n; i++) {
            /* İki kısa bip darbesi (880 Hz -> 1175 Hz) */
            double t = (double)i / (double)rate;
            double freq = (i < 3500u) ? 880.0 : (i > 4200u ? 1174.66 : 0.0);
            double env = (i >= 3500u && i <= 4200u) ? 0.0 : 0.45;
            double v = (freq > 0.0) ? sin(2.0 * 3.14159265358979 * freq * t) * env * 28000.0 : 0.0;
            pcm[i] = (int16_t)v;
        }
        s_beep_ready = 1;
    }
    if (A->audio && A->audio->play_wav) {
        A->audio->play_wav(s_beep_wav, sizeof(s_beep_wav));
    }
}

/* Howard Hinnant civil-from-days: unix saniyesini (UTC) tarih/saate çevirir */
static void unix_to_dt(int64_t sec, int *y, int *m, int *d, int *hh, int *mm, int *ss, int *wd) {
    int64_t days = sec >= 0 ? (sec / 86400) : ((sec - 86399) / 86400);
    int64_t rem = sec - days * 86400;
    *hh = (int)(rem / 3600);
    *mm = (int)((rem % 3600) / 60);
    *ss = (int)(rem % 60);
    /* 1970-01-01 Perşembe (4) */
    int w = (int)((days + 4) % 7);
    if (w < 0) w += 7;
    *wd = w;

    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint64_t doe = (uint64_t)(z - era * 146097);
    uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yr = (int64_t)yoe + era * 400;
    uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint64_t mp = (5 * doy + 2) / 153;
    uint64_t day = doy - (153 * mp + 2) / 5 + 1;
    uint64_t mon = mp + (mp < 10 ? 3 : -9);
    yr += (mon <= 2);
    *y = (int)yr; *m = (int)mon; *d = (int)day;
}

typedef struct {
    const char *city;
    const char *tz;
    int offset_min;
} world_city_t;

static const world_city_t CITIES[5] = {
    { "İstanbul", "UTC+3",  180 },
    { "Londra",   "UTC+0",    0 },
    { "Berlin",   "UTC+1",   60 },
    { "New York", "UTC-5", -300 },
    { "Tokyo",    "UTC+9",  540 }
};

static const char *const MONTHS_TR[12] = {
    "Ocak", "Şubat", "Mart", "Nisan", "Mayıs", "Haziran",
    "Temmuz", "Ağustos", "Eylül", "Ekim", "Kasım", "Aralık"
};
static const char *const DAYS_TR[7] = {
    "Pazar", "Pazartesi", "Salı", "Çarşamba", "Perşembe", "Cuma", "Cumartesi"
};

static void fmt_hhmmss(char *out, int cap, int h, int m, int s) {
    char b[4];
    scpy(out, cap, "");
    two_dig(h, b); scat(out, cap, b); scat(out, cap, ":");
    two_dig(m, b); scat(out, cap, b); scat(out, cap, ":");
    two_dig(s, b); scat(out, cap, b);
}

static void fmt_sw(char *out, int cap, uint64_t ms) {
    uint64_t total_s = ms / 1000u;
    uint64_t m = total_s / 60u;
    uint64_t s = total_s % 60u;
    uint64_t rem = ms % 1000u;
    char b[8];
    scpy(out, cap, "");
    if (m >= 100u) { u2s((unsigned int)m, b, sizeof(b)); scat(out, cap, b); }
    else { two_dig((int)m, b); scat(out, cap, b); }
    scat(out, cap, ":");
    two_dig((int)s, b); scat(out, cap, b);
    scat(out, cap, ".");
    b[0] = (char)('0' + (rem / 100u) % 10u);
    b[1] = (char)('0' + (rem / 10u) % 10u);
    b[2] = (char)('0' + rem % 10u);
    b[3] = 0;
    scat(out, cap, b);
}

static void update_clock_tab(void) {
    if (g_tab != 0) return;
    asi_datetime_t dt;
    int have = (A->time && A->time->now && A->time->now(&dt) && dt.year > 2000);
    if (have && g_clk_big && g_clk_date) {
        char tb[16];
        fmt_hhmmss(tb, sizeof(tb), dt.hour, dt.minute, dt.second);
        lv_label_set_text(g_clk_big, tb);

        char db[64], nb[8];
        u2s((unsigned int)dt.day, nb, sizeof(nb));
        scpy(db, sizeof(db), nb);
        scat(db, sizeof(db), " ");
        if (dt.month >= 1 && dt.month <= 12) scat(db, sizeof(db), MONTHS_TR[dt.month - 1]);
        scat(db, sizeof(db), " ");
        u2s((unsigned int)dt.year, nb, sizeof(nb));
        scat(db, sizeof(db), nb);
        if (dt.weekday >= 0 && dt.weekday < 7) {
            scat(db, sizeof(db), ", ");
            scat(db, sizeof(db), DAYS_TR[dt.weekday]);
        }
        lv_label_set_text(g_clk_date, db);
    }

    int64_t utc_sec = (A->time && A->time->unix_now) ? (int64_t)A->time->unix_now() : 0;
    if (utc_sec == 0 && have) {
        /* Fallback yaklaşık hesap (UTC+3 varsayımı) */
        utc_sec = ((int64_t)dt.hour * 3600 + dt.minute * 60 + dt.second) - 3 * 3600;
    }
    for (int i = 0; i < 5; i++) {
        if (!g_city_time[i] || !g_city_sub[i]) continue;
        int cy, cm, cd, ch, cmi, cs, cwd;
        unix_to_dt(utc_sec + (int64_t)CITIES[i].offset_min * 60,
                   &cy, &cm, &cd, &ch, &cmi, &cs, &cwd);
        char tb[16], sb[40], nb[8];
        fmt_hhmmss(tb, sizeof(tb), ch, cmi, cs);
        lv_label_set_text(g_city_time[i], tb);

        scpy(sb, sizeof(sb), CITIES[i].tz);
        if (have) {
            scat(sb, sizeof(sb), " · ");
            u2s((unsigned int)cd, nb, sizeof(nb)); scat(sb, sizeof(sb), nb);
            scat(sb, sizeof(sb), " ");
            if (cm >= 1 && cm <= 12) scat(sb, sizeof(sb), MONTHS_TR[cm - 1]);
        }
        lv_label_set_text(g_city_sub[i], sb);
    }
}

static uint64_t cur_sw_ms(void) {
    if (!g_sw_run || !A->time || !A->time->ticks_ms) return g_sw_accum_ms;
    return g_sw_accum_ms + (A->time->ticks_ms() - g_sw_start_ms);
}

static void update_sw_tab(void) {
    if (g_tab != 1 || !g_sw_lbl) return;
    char tb[24];
    fmt_sw(tb, sizeof(tb), cur_sw_ms());
    lv_label_set_text(g_sw_lbl, tb);
}

static void rebuild_tab_content(void);

static void update_tm_tab(uint64_t now_ms) {
    uint64_t rem = g_tm_rem_ms;
    if (g_tm_run) {
        if (now_ms >= g_tm_end_ms) {
            rem = 0;
            g_tm_rem_ms = 0;
            g_tm_run = 0;
            if (!g_tm_fired) {
                g_tm_fired = 1;
                arctian_notify("Saat — Zamanlayıcı", "Süre doldu!");
                play_alarm_beep();
                if (g_tab == 2) rebuild_tab_content();
            }
        } else {
            rem = g_tm_end_ms - now_ms;
            g_tm_rem_ms = rem;
        }
    }
    if (g_tab != 2 || !g_tm_lbl) return;
    uint32_t total_s = (uint32_t)((rem + 999u) / 1000u);
    if (!g_tm_run && rem == (uint64_t)g_tm_set_sec * 1000u) total_s = (uint32_t)g_tm_set_sec;
    int hh = (int)(total_s / 3600u);
    int mm = (int)((total_s % 3600u) / 60u);
    int ss = (int)(total_s % 60u);
    char tb[16];
    fmt_hhmmss(tb, sizeof(tb), hh, mm, ss);
    lv_label_set_text(g_tm_lbl, tb);
    if (g_tm_bar && g_tm_set_sec > 0) {
        uint64_t tot_ms = (uint64_t)g_tm_set_sec * 1000u;
        int32_t val = (int32_t)((rem * 1000u) / tot_ms);
        lv_bar_set_value(g_tm_bar, val, LV_ANIM_OFF);
    }
}

static void tick_cb(lv_timer_t *t) {
    (void)t;
    uint64_t now = (A->time && A->time->ticks_ms) ? A->time->ticks_ms() : 0;
    update_clock_tab();
    update_sw_tab();
    update_tm_tab(now);
}

/* ---- Kronometre eylemleri ---- */
static void sw_start_cb(lv_event_t *e) {
    (void)e;
    uint64_t now = (A->time && A->time->ticks_ms) ? A->time->ticks_ms() : 0;
    if (g_sw_run) {
        g_sw_accum_ms += (now - g_sw_start_ms);
        g_sw_run = 0;
    } else {
        g_sw_start_ms = now;
        g_sw_run = 1;
    }
    if (g_sw_start_lbl) lv_label_set_text(g_sw_start_lbl, g_sw_run ? "Duraklat" : "Başlat");
}

static void sw_lap_cb(lv_event_t *e) {
    (void)e;
    if (g_lap_n >= LAP_MAX) return;
    uint64_t tot = cur_sw_ms();
    uint64_t prev = (g_lap_n > 0) ? g_laps[g_lap_n - 1] : 0;
    g_laps[g_lap_n] = tot;
    g_lap_deltas[g_lap_n] = (tot >= prev) ? (tot - prev) : 0;
    g_lap_n++;
    rebuild_tab_content();
}

static void sw_reset_cb(lv_event_t *e) {
    (void)e;
    g_sw_run = 0;
    g_sw_accum_ms = 0;
    g_lap_n = 0;
    rebuild_tab_content();
}

/* ---- Zamanlayıcı eylemleri ---- */
static void tm_start_cb(lv_event_t *e) {
    (void)e;
    uint64_t now = (A->time && A->time->ticks_ms) ? A->time->ticks_ms() : 0;
    g_tm_fired = 0;
    if (g_tm_run) {
        g_tm_rem_ms = (g_tm_end_ms > now) ? (g_tm_end_ms - now) : 0;
        g_tm_run = 0;
    } else {
        if (g_tm_rem_ms == 0) g_tm_rem_ms = (uint64_t)g_tm_set_sec * 1000u;
        if (g_tm_rem_ms == 0) return;
        g_tm_end_ms = now + g_tm_rem_ms;
        g_tm_run = 1;
    }
    rebuild_tab_content();
}

static void tm_reset_cb(lv_event_t *e) {
    (void)e;
    g_tm_run = 0;
    g_tm_fired = 0;
    g_tm_rem_ms = (uint64_t)g_tm_set_sec * 1000u;
    rebuild_tab_content();
}

static void tm_preset_cb(lv_event_t *e) {
    int sec = (int)(intptr_t)lv_event_get_user_data(e);
    g_tm_run = 0;
    g_tm_fired = 0;
    g_tm_set_sec = sec;
    g_tm_rem_ms = (uint64_t)sec * 1000u;
    rebuild_tab_content();
}

static void tm_adj_cb(lv_event_t *e) {
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    if (g_tm_run) return;
    g_tm_fired = 0;
    g_tm_set_sec += delta;
    if (g_tm_set_sec < 5) g_tm_set_sec = 5;
    if (g_tm_set_sec > 35999) g_tm_set_sec = 35999;
    g_tm_rem_ms = (uint64_t)g_tm_set_sec * 1000u;
    rebuild_tab_content();
}

static void tm_dismiss_cb(lv_event_t *e) {
    (void)e;
    g_tm_fired = 0;
    g_tm_rem_ms = (uint64_t)g_tm_set_sec * 1000u;
    rebuild_tab_content();
}

static lv_obj_t *mk_btn(lv_obj_t *p, const char *txt, uint32_t bg, lv_event_cb_t cb, void *ud, lv_obj_t **lbl_out) {
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_height(b, 34);
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 16, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(C_TXT), 0);
    if (f13) lv_obj_set_style_text_font(l, f13, 0);
    lv_obj_center(l);
    if (lbl_out) *lbl_out = l;
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static void paint_tab_bar(void) {
    for (int i = 0; i < 3; i++) {
        if (!g_tab_b[i]) continue;
        int on = (i == g_tab);
        lv_obj_set_style_bg_color(g_tab_b[i], lv_color_hex(on ? C_ACC : C_BTN), 0);
        if (g_tab_l[i]) lv_obj_set_style_text_color(g_tab_l[i], lv_color_hex(on ? 0xFFFFFF : C_DIM), 0);
    }
}

static void rebuild_tab_content(void) {
    if (!g_content) return;
    lv_obj_clean(g_content);
    g_clk_big = g_clk_date = 0;
    for (int i = 0; i < 5; i++) g_city_time[i] = g_city_sub[i] = 0;
    g_sw_lbl = g_sw_start_lbl = g_sw_laps_box = 0;
    g_tm_lbl = g_tm_bar = g_tm_start_lbl = g_tm_banner = 0;
    paint_tab_bar();

    if (g_tab == 0) {
        /* Yerel Saat + Dünya Saatleri */
        lv_obj_t *hero = lv_obj_create(g_content);
        lv_obj_remove_style_all(hero);
        lv_obj_set_size(hero, LV_PCT(100), 110);
        lv_obj_set_style_bg_color(hero, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_bg_opa(hero, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(hero, 12, 0);
        lv_obj_set_style_border_width(hero, 1, 0);
        lv_obj_set_style_border_color(hero, lv_color_hex(C_LINE), 0);
        lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(hero, 4, 0);
        lv_obj_remove_flag(hero, LV_OBJ_FLAG_SCROLLABLE);

        g_clk_big = lv_label_create(hero);
        lv_label_set_text(g_clk_big, "00:00:00");
        lv_obj_set_style_text_color(g_clk_big, lv_color_hex(C_TXT), 0);
        if (f36) lv_obj_set_style_text_font(g_clk_big, f36, 0);

        g_clk_date = lv_label_create(hero);
        lv_label_set_text(g_clk_date, "-");
        lv_obj_set_style_text_color(g_clk_date, lv_color_hex(C_DIM), 0);
        if (f13) lv_obj_set_style_text_font(g_clk_date, f13, 0);

        lv_obj_t *wt = lv_label_create(g_content);
        lv_label_set_text(wt, "Dünya Saatleri");
        lv_obj_set_style_text_color(wt, lv_color_hex(C_DIM), 0);
        if (f12) lv_obj_set_style_text_font(wt, f12, 0);
        lv_obj_set_style_pad_top(wt, 4, 0);

        lv_obj_t *grid = lv_obj_create(g_content);
        lv_obj_remove_style_all(grid);
        lv_obj_set_width(grid, LV_PCT(100));
        lv_obj_set_flex_grow(grid, 1);
        lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(grid, 10, 0);
        lv_obj_set_style_pad_row(grid, 10, 0);
        lv_obj_set_scroll_dir(grid, LV_DIR_VER);

        for (int i = 0; i < 5; i++) {
            lv_obj_t *c = lv_obj_create(grid);
            lv_obj_remove_style_all(c);
            lv_obj_set_size(c, 196, 86);
            lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0);
            lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(c, 10, 0);
            lv_obj_set_style_border_width(c, 1, 0);
            lv_obj_set_style_border_color(c, lv_color_hex(i == 0 ? C_ACC : C_LINE), 0);
            lv_obj_set_style_pad_all(c, 12, 0);
            lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(c, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
            lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *cn = lv_label_create(c);
            lv_label_set_text(cn, CITIES[i].city);
            lv_obj_set_style_text_color(cn, lv_color_hex(C_TXT), 0);
            if (f13) lv_obj_set_style_text_font(cn, f13, 0);

            g_city_time[i] = lv_label_create(c);
            lv_label_set_text(g_city_time[i], "--:--:--");
            lv_obj_set_style_text_color(g_city_time[i], lv_color_hex(C_ACC), 0);
            if (f24) lv_obj_set_style_text_font(g_city_time[i], f24, 0);

            g_city_sub[i] = lv_label_create(c);
            lv_label_set_text(g_city_sub[i], CITIES[i].tz);
            lv_obj_set_style_text_color(g_city_sub[i], lv_color_hex(C_DIM), 0);
            if (f12) lv_obj_set_style_text_font(g_city_sub[i], f12, 0);
        }
        update_clock_tab();
    } else if (g_tab == 1) {
        /* Kronometre */
        lv_obj_t *box = lv_obj_create(g_content);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, LV_PCT(100), 130);
        lv_obj_set_style_bg_color(box, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(box, 12, 0);
        lv_obj_set_style_border_width(box, 1, 0);
        lv_obj_set_style_border_color(box, lv_color_hex(C_LINE), 0);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(box, 14, 0);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

        g_sw_lbl = lv_label_create(box);
        lv_label_set_text(g_sw_lbl, "00:00.000");
        lv_obj_set_style_text_color(g_sw_lbl, lv_color_hex(C_TXT), 0);
        if (f36) lv_obj_set_style_text_font(g_sw_lbl, f36, 0);

        lv_obj_t *brow = lv_obj_create(box);
        lv_obj_remove_style_all(brow);
        lv_obj_set_size(brow, LV_SIZE_CONTENT, 36);
        lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(brow, 10, 0);
        lv_obj_remove_flag(brow, LV_OBJ_FLAG_SCROLLABLE);
        mk_btn(brow, g_sw_run ? "Duraklat" : "Başlat", C_ACC, sw_start_cb, 0, &g_sw_start_lbl);
        mk_btn(brow, "Tur", C_BTN, sw_lap_cb, 0, 0);
        mk_btn(brow, "Sıfırla", C_BTN, sw_reset_cb, 0, 0);

        g_sw_laps_box = lv_obj_create(g_content);
        lv_obj_remove_style_all(g_sw_laps_box);
        lv_obj_set_width(g_sw_laps_box, LV_PCT(100));
        lv_obj_set_flex_grow(g_sw_laps_box, 1);
        lv_obj_set_style_bg_color(g_sw_laps_box, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_bg_opa(g_sw_laps_box, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(g_sw_laps_box, 10, 0);
        lv_obj_set_style_border_width(g_sw_laps_box, 1, 0);
        lv_obj_set_style_border_color(g_sw_laps_box, lv_color_hex(C_LINE), 0);
        lv_obj_set_style_pad_all(g_sw_laps_box, 10, 0);
        lv_obj_set_style_pad_row(g_sw_laps_box, 4, 0);
        lv_obj_set_flex_flow(g_sw_laps_box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scroll_dir(g_sw_laps_box, LV_DIR_VER);

        if (g_lap_n == 0) {
            lv_obj_t *emp = lv_label_create(g_sw_laps_box);
            lv_label_set_text(emp, "Tur kaydı yok — Başlat ve Tur'a basın");
            lv_obj_set_style_text_color(emp, lv_color_hex(C_DIM), 0);
            if (f13) lv_obj_set_style_text_font(emp, f13, 0);
        } else {
            for (int i = g_lap_n - 1; i >= 0; i--) {
                lv_obj_t *r = lv_obj_create(g_sw_laps_box);
                lv_obj_remove_style_all(r);
                lv_obj_set_size(r, LV_PCT(100), 28);
                lv_obj_set_style_border_width(r, 1, 0);
                lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
                lv_obj_set_style_border_color(r, lv_color_hex(C_LINE), 0);
                lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
                lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
                lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);

                char num[16], d_s[24], t_s[24], nb[8];
                u2s((unsigned int)(i + 1), nb, sizeof(nb));
                scpy(num, sizeof(num), "Tur "); scat(num, sizeof(num), nb);
                scpy(d_s, sizeof(d_s), "+");
                fmt_sw(d_s + 1, sizeof(d_s) - 1, g_lap_deltas[i]);
                fmt_sw(t_s, sizeof(t_s), g_laps[i]);

                lv_obj_t *l1 = lv_label_create(r);
                lv_label_set_text(l1, num);
                lv_obj_set_style_text_color(l1, lv_color_hex(C_DIM), 0);
                if (f13) lv_obj_set_style_text_font(l1, f13, 0);

                lv_obj_t *l2 = lv_label_create(r);
                lv_label_set_text(l2, d_s);
                lv_obj_set_style_text_color(l2, lv_color_hex(C_ACC), 0);
                if (f13) lv_obj_set_style_text_font(l2, f13, 0);

                lv_obj_t *l3 = lv_label_create(r);
                lv_label_set_text(l3, t_s);
                lv_obj_set_style_text_color(l3, lv_color_hex(C_TXT), 0);
                if (f13) lv_obj_set_style_text_font(l3, f13, 0);
            }
        }
        update_sw_tab();
    } else {
        /* Zamanlayıcı / Alarm */
        if (g_tm_fired) {
            g_tm_banner = lv_obj_create(g_content);
            lv_obj_remove_style_all(g_tm_banner);
            lv_obj_set_size(g_tm_banner, LV_PCT(100), 42);
            lv_obj_set_style_bg_color(g_tm_banner, lv_color_hex(0x3B161B), 0);
            lv_obj_set_style_bg_opa(g_tm_banner, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(g_tm_banner, 8, 0);
            lv_obj_set_style_border_width(g_tm_banner, 1, 0);
            lv_obj_set_style_border_color(g_tm_banner, lv_color_hex(C_WARN), 0);
            lv_obj_set_style_pad_hor(g_tm_banner, 12, 0);
            lv_obj_set_flex_flow(g_tm_banner, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(g_tm_banner, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_remove_flag(g_tm_banner, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *bl = lv_label_create(g_tm_banner);
            lv_label_set_text(bl, "Süre doldu!");
            lv_obj_set_style_text_color(bl, lv_color_hex(0xFCA5A5), 0);
            if (f15) lv_obj_set_style_text_font(bl, f15, 0);
            mk_btn(g_tm_banner, "Tamam", C_WARN, tm_dismiss_cb, 0, 0);
        }

        lv_obj_t *box = lv_obj_create(g_content);
        lv_obj_remove_style_all(box);
        lv_obj_set_width(box, LV_PCT(100));
        lv_obj_set_flex_grow(box, 1);
        lv_obj_set_style_bg_color(box, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(box, 12, 0);
        lv_obj_set_style_border_width(box, 1, 0);
        lv_obj_set_style_border_color(box, lv_color_hex(C_LINE), 0);
        lv_obj_set_style_pad_all(box, 18, 0);
        lv_obj_set_style_pad_row(box, 14, 0);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

        g_tm_lbl = lv_label_create(box);
        lv_label_set_text(g_tm_lbl, "00:01:00");
        lv_obj_set_style_text_color(g_tm_lbl, lv_color_hex(C_TXT), 0);
        if (f36) lv_obj_set_style_text_font(g_tm_lbl, f36, 0);

        g_tm_bar = lv_bar_create(box);
        lv_obj_set_size(g_tm_bar, 320, 6);
        lv_bar_set_range(g_tm_bar, 0, 1000);
        lv_bar_set_value(g_tm_bar, 1000, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(g_tm_bar, lv_color_hex(C_LINE), LV_PART_MAIN);
        lv_obj_set_style_bg_color(g_tm_bar, lv_color_hex(C_ACC), LV_PART_INDICATOR);

        lv_obj_t *adj = lv_obj_create(box);
        lv_obj_remove_style_all(adj);
        lv_obj_set_size(adj, LV_SIZE_CONTENT, 36);
        lv_obj_set_flex_flow(adj, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(adj, 6, 0);
        lv_obj_remove_flag(adj, LV_OBJ_FLAG_SCROLLABLE);
        mk_btn(adj, "-1 dk", C_BTN, tm_adj_cb, (void *)(intptr_t)-60, 0);
        mk_btn(adj, "-10 sn", C_BTN, tm_adj_cb, (void *)(intptr_t)-10, 0);
        mk_btn(adj, "+10 sn", C_BTN, tm_adj_cb, (void *)(intptr_t)10, 0);
        mk_btn(adj, "+1 dk", C_BTN, tm_adj_cb, (void *)(intptr_t)60, 0);

        lv_obj_t *pre = lv_obj_create(box);
        lv_obj_remove_style_all(pre);
        lv_obj_set_size(pre, LV_SIZE_CONTENT, 36);
        lv_obj_set_flex_flow(pre, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(pre, 6, 0);
        lv_obj_remove_flag(pre, LV_OBJ_FLAG_SCROLLABLE);
        mk_btn(pre, "1 dk", C_BTN, tm_preset_cb, (void *)(intptr_t)60, 0);
        mk_btn(pre, "3 dk", C_BTN, tm_preset_cb, (void *)(intptr_t)180, 0);
        mk_btn(pre, "5 dk", C_BTN, tm_preset_cb, (void *)(intptr_t)300, 0);
        mk_btn(pre, "10 dk", C_BTN, tm_preset_cb, (void *)(intptr_t)600, 0);

        lv_obj_t *ctl = lv_obj_create(box);
        lv_obj_remove_style_all(ctl);
        lv_obj_set_size(ctl, LV_SIZE_CONTENT, 36);
        lv_obj_set_flex_flow(ctl, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(ctl, 10, 0);
        lv_obj_remove_flag(ctl, LV_OBJ_FLAG_SCROLLABLE);
        mk_btn(ctl, g_tm_run ? "Duraklat" : "Başlat", C_ACC, tm_start_cb, 0, &g_tm_start_lbl);
        mk_btn(ctl, "Sıfırla", C_BTN, tm_reset_cb, 0, 0);

        uint64_t now = (A->time && A->time->ticks_ms) ? A->time->ticks_ms() : 0;
        update_tm_tab(now);
    }
}

static void tab_cb(lv_event_t *e) {
    g_tab = (int)(intptr_t)lv_event_get_user_data(e);
    rebuild_tab_content();
}

static void clk_relayout(lv_obj_t *win) {
    (void)win;
    if (g_win) lv_obj_update_layout(g_win);
}

static void app_del_cb(lv_event_t *e) {
    (void)e;
    if (g_timer) { lv_timer_delete(g_timer); g_timer = 0; }
    g_win = 0; g_content = 0;
    for (int i = 0; i < 3; i++) g_tab_b[i] = g_tab_l[i] = 0;
}

void app_entry(lv_obj_t *win, asi_t *asi) {
    A = asi;
    g_win = win;
    g_tab = 0;

    if (!f36) f36 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 36);
    if (!f24) f24 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 22);
    if (!f15) f15 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 15);
    if (!f13) f13 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 13);
    if (!f12) f12 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 12);

    lv_obj_set_flex_flow(win, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(win, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(win, 0, 0);
    lv_obj_set_style_pad_row(win, 0, 0);
    lv_obj_set_style_bg_color(win, lv_color_hex(C_BODY), 0);
    lv_obj_set_style_bg_opa(win, LV_OPA_COVER, 0);
    lv_obj_remove_flag(win, LV_OBJ_FLAG_SCROLLABLE);

    /* Üst sekme çubuğu */
    lv_obj_t *tbar = lv_obj_create(win);
    lv_obj_remove_style_all(tbar);
    lv_obj_set_size(tbar, LV_PCT(100), 44);
    lv_obj_set_style_bg_color(tbar, lv_color_hex(C_BAR), 0);
    lv_obj_set_style_bg_opa(tbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tbar, 1, 0);
    lv_obj_set_style_border_side(tbar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(tbar, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_pad_hor(tbar, 14, 0);
    lv_obj_set_style_pad_column(tbar, 8, 0);
    lv_obj_set_flex_flow(tbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(tbar, LV_OBJ_FLAG_SCROLLABLE);

    static const char *const TNAMES[3] = { "Dünya Saatleri", "Kronometre", "Zamanlayıcı" };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_button_create(tbar);
        lv_obj_set_height(b, 30);
        lv_obj_set_width(b, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_BTN), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_hor(b, 14, 0);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, TNAMES[i]);
        if (f13) lv_obj_set_style_text_font(l, f13, 0);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        g_tab_b[i] = b;
        g_tab_l[i] = l;
    }

    g_content = lv_obj_create(win);
    lv_obj_remove_style_all(g_content);
    lv_obj_set_width(g_content, LV_PCT(100));
    lv_obj_set_flex_grow(g_content, 1);
    lv_obj_set_style_pad_all(g_content, 16, 0);
    lv_obj_set_style_pad_row(g_content, 12, 0);
    lv_obj_set_flex_flow(g_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(g_content, LV_OBJ_FLAG_SCROLLABLE);

    rebuild_tab_content();
    g_timer = lv_timer_create(tick_cb, 100, 0);
    arctian_shell_set_relayout(win, clk_relayout);
    lv_obj_add_event_cb(win, app_del_cb, LV_EVENT_DELETE, 0);
}
