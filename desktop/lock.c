/* Arctian giris ekranlari — giris.md sartnamesi (1280x720 referans).
 *
 * Iki ekran:
 *   1) Hesap olusturma : kullanici adi kapsulu + "Sifre olusturulsun mu?"
 *                        + Evet/Hayir.
 *   2) Kullanici secme : avatar daireleri + secili halka + sifre kapsulu.
 *
 * Ortak katmanlar: arkaplan (masaustu duvar kagidi gorunur), kenar
 * karartmasi, alt karartma, tarih, saat, alt cubuk (ag / TR / yeniden
 * baslat / kapat).
 *
 * Tasarim kosullari:
 *  - Tuval 1280x720; tum degerler bu uzayda. Ekran farkliysa esit oranli
 *    olcek + ortalama uygulanir (SX/SY/SS).
 *  - Fontlar Manrope: 200 (Thin), 600 (SemiBold), 700 (Bold).
 *  - Yazi tipleri kutu icinde dikey ortanir; taban cizgileri sartnamede
 *    verilen degerlerle uyumludur.
 *  - Cizim sirasi: karartmalar -> tarih -> saat -> ekrana ozel ogeler ->
 *    alt cubuk. */

#include "app.h"
#include "asi.h"
#include "lv_port.h"
#include "lock.h"
#include "svg.h"
#include <stdint.h>

/* Manrope (SIL OFL 1.1) - design/font/Manrope-*.ttf, bin2c ile gomulu. */
extern const unsigned char font_manrope_thin[];
extern const unsigned int  font_manrope_thin_len;
extern const unsigned char font_manrope_sb[];
extern const unsigned int  font_manrope_sb_len;
extern const unsigned char font_manrope_bold[];
extern const unsigned int  font_manrope_bold_len;

/* ---- 0.1 Renk paleti ---- */
#define C_INK        0x2B2438   /* ana yazi */
#define C_INK2       0x5A5373   /* ikincil / placeholder */
#define C_DARK       0x3A3560   /* Evet dugmesi */
#define C_SHADE      0x2B2446   /* golge / karartma */
#define C_WHITE      0xFFFFFF
#define C_FALLBACK   0xC9B7C8
#define C_AV1_A      0x6A74A8
#define C_AV1_B      0x8D6FB0
#define C_AV2_A      0xC9778F
#define C_AV2_B      0xE0966F
#define C_ERROR      0xB0452F

/* ---- 0.2 seffafliklar (0..255) ---- */
#define A_018  46
#define A_022  56
#define A_026  66
#define A_028  71
#define A_042  107
#define A_050  128
#define A_055  140
#define A_070  179
#define A_090  230
#define A_095  242

#define REF_W  1280
#define REF_H  720

enum { ST_CREATE = 0, ST_CREATE_PW, ST_SELECT, ST_PASSWORD };

static asi_t      *A;
static lv_font_t  *F_CLOCK, *F_DATE, *F_HEAD, *F_Q, *F_BTN, *F_NAME,
                  *F_NAME_B, *F_FIELD, *F_AV, *F_TR, *F_PLACE;
static lv_obj_t   *s_root, *s_clock, *s_date, *s_shade_img;
static lv_group_t *s_group;
static int         s_step, s_done, s_rebuild;
static lv_obj_t   *s_ta_name, *s_ta_pw1, *s_ta_pw2, *s_ta_login;
static char        s_pending[32];
static char        s_target[32];
static const char *s_error;
static int         s_sel_idx;        /* ekran 2: secili avatar */
static int         s_want_pw;        /* ekran 1: "Evet" secildi mi */

/* olcek */
static int s_sc, s_ox, s_oy;
static int SX(int v) { return s_ox + (int)((long)v * s_sc / 1000); }
static int SY(int v) { return s_oy + (int)((long)v * s_sc / 1000); }
static int SS(int v) { return (int)((long)v * s_sc / 1000); }

static int streq(const char *a, const char *b) {
    int i = 0;
    if (!a || !b) return 0;
    for (; a[i] && b[i]; i++) if (a[i] != b[i]) return 0;
    return a[i] == b[i];
}

/* ---- Madde 26: kilit sirasinda arka plan zamanlayicilari ----
 * (0.6'dan beri: kilit acikken VM/otosave/guncelleme donguleri durur.) */
#define LOCK_FROZEN_MAX 256
static lv_timer_t *s_frozen[LOCK_FROZEN_MAX];
static int s_frozen_n;

static void lock_freeze_timers(void) {
    s_frozen_n = 0;
    lv_timer_t *indev_t[4];
    int ni = 0;
    lv_indev_t *d = lv_indev_get_next(0);
    while (d && ni < 4) {
        lv_timer_t *rt = lv_indev_get_read_timer(d);
        if (rt) indev_t[ni++] = rt;
        d = lv_indev_get_next(d);
    }
    for (lv_timer_t *t = lv_timer_get_next(0); t; t = lv_timer_get_next(t)) {
        if (s_frozen_n >= LOCK_FROZEN_MAX) break;
        if (lv_timer_get_paused(t)) continue;
        int keep = 0;
        for (int i = 0; i < ni; i++) if (indev_t[i] == t) { keep = 1; break; }
        if (keep) continue;
        lv_timer_pause(t);
        s_frozen[s_frozen_n++] = t;
    }
}

static void lock_thaw_timers(void) {
    for (int i = 0; i < s_frozen_n; i++) {
        int alive = 0;
        for (lv_timer_t *t = lv_timer_get_next(0); t; t = lv_timer_get_next(t))
            if (t == s_frozen[i]) { alive = 1; break; }
        if (alive) lv_timer_resume(s_frozen[i]);
    }
    s_frozen_n = 0;
}

static void read_ta(lv_obj_t *ta, char *buf, int cap) {
    buf[0] = 0;
    if (!ta) return;
    const char *t = lv_textarea_get_text(ta);
    if (!t) return;
    int i = 0;
    for (; t[i] && i < cap - 1; i++) buf[i] = t[i];
    buf[i] = 0;
}

static void buf_free_cb(lv_event_t *e) {
    void *p = lv_event_get_user_data(e);
    if (p && A && A->mem && A->mem->free) A->mem->free(p);
}

/* ---- 1.4 Tarih / 1.5 Saat ---- */
static const char *MONTHS[] = { "Ocak", "Subat", "Mart", "Nisan", "Mayis",
                                "Haziran", "Temmuz", "Agustos", "Eylul",
                                "Ekim", "Kasim", "Aralik" };
static const char *DAYS[] = { "Pazar", "Pazartesi", "Sali", "Carsamba",
                              "Persembe", "Cuma", "Cumartesi" };

static void update_clock(void) {
    static uint64_t last_ms;
    uint64_t now = (A && A->time && A->time->ticks_ms) ? A->time->ticks_ms() : 0;
    if (last_ms && now - last_ms < 500u) return;
    last_ms = now;

    asi_datetime_t dt;
    if (!A || !A->time || !A->time->now || !A->time->now(&dt)) return;

    char b[8];
    b[0] = (char)('0' + (dt.hour / 10) % 10);
    b[1] = (char)('0' + dt.hour % 10);
    b[2] = ':';
    b[3] = (char)('0' + (dt.minute / 10) % 10);
    b[4] = (char)('0' + dt.minute % 10);
    b[5] = 0;
    if (s_clock) {
        static char prev[8];
        int same = (prev[0]==b[0] && prev[1]==b[1] && prev[2]==b[2] &&
                    prev[3]==b[3] && prev[4]==b[4]);
        if (!same) { for (int i = 0; i < 8; i++) prev[i] = b[i]; lv_label_set_text(s_clock, b); }
    }

    if (s_date) {
        char d[48];
        int n = 0;
        const char *dn = (dt.weekday >= 0 && dt.weekday < 7) ? DAYS[dt.weekday] : "";
        for (int i = 0; dn[i] && n < 20; i++) d[n++] = dn[i];
        d[n++] = ','; d[n++] = ' ';
        int day = dt.day;
        if (day >= 10) d[n++] = (char)('0' + day / 10);
        d[n++] = (char)('0' + day % 10);
        d[n++] = ' ';
        const char *mn = (dt.month >= 1 && dt.month <= 12) ? MONTHS[dt.month - 1] : "";
        for (int i = 0; mn[i] && n < 44; i++) d[n++] = mn[i];
        d[n] = 0;
        lv_label_set_text(s_date, d);
    }
}

/* ---- SVG ikon -> canvas (sartname bolum 4) ---- */
#define IC_MAX 8
static uint32_t *s_ic_buf[IC_MAX];
static int s_ic_n;

static lv_obj_t *icon_canvas(lv_obj_t *parent, const char *svg, int px) {
    if (s_ic_n >= IC_MAX || !A || !A->mem || !A->mem->alloc) return 0;
    uint32_t *buf = (uint32_t *)A->mem->alloc((size_t)px * px * 4u);
    if (!buf) return 0;
    if (svg_render(svg, px, px, buf) != 0) { A->mem->free(buf); return 0; }
    s_ic_buf[s_ic_n++] = buf;
    lv_obj_t *c = lv_canvas_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_canvas_set_buffer(c, buf, px, px, LV_COLOR_FORMAT_ARGB8888);
    return c;
}

#define SVG_USER  "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#5A5373\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><circle cx=\"12\" cy=\"8\" r=\"4\"/><path d=\"M4 21c0-4.4 3.6-7 8-7s8 2.6 8 7\"/></svg>"
#define SVG_LOCK  "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#5A5373\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><rect x=\"5\" y=\"11\" width=\"14\" height=\"9\" rx=\"2.5\"/><path d=\"M8 11V8a4 4 0 0 1 8 0v3\"/></svg>"
#define SVG_ARROW "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#FFFFFF\" stroke-width=\"2.4\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M5 12h14\"/><path d=\"M13 6l6 6-6 6\"/></svg>"
#define SVG_NET   "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#FFFFFF\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M2 9a15 15 0 0 1 20 0\"/><path d=\"M5 12.5a10 10 0 0 1 14 0\"/><path d=\"M8.5 16a5 5 0 0 1 7 0\"/><circle cx=\"12\" cy=\"19.2\" r=\"1\" fill=\"#FFFFFF\"/></svg>"
#define SVG_RESTART "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#FFFFFF\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M20 11a8 8 0 1 0-2.3 5.7\"/><path d=\"M20 4v7h-7\"/></svg>"
#define SVG_POWER "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#FFFFFF\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M12 3v9\"/><path d=\"M6.4 6.6a8 8 0 1 0 11.2 0\"/></svg>"
#define SVG_PLUS  "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#2B2438\" stroke-width=\"2\" stroke-linecap=\"round\"><path d=\"M12 5v14\"/><path d=\"M5 12h14\"/></svg>"

static lv_obj_t *add_label(lv_obj_t *parent, const char *txt, lv_font_t *f,
                           uint32_t col, lv_opa_t opa) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    if (f) lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    if (opa < LV_OPA_COVER) lv_obj_set_style_text_opa(l, opa, 0);
    return l;
}

/* Saydam kutu yardimcisi: dolgu + cerceve + yuvarlaklik. */
static lv_obj_t *box(lv_obj_t *parent, int w, int h, int radius,
                     uint32_t fill, int fill_opa,
                     uint32_t border, int border_opa, int border_w) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    if (fill_opa > 0) {
        lv_obj_set_style_bg_color(o, lv_color_hex(fill), 0);
        lv_obj_set_style_bg_opa(o, (lv_opa_t)fill_opa, 0);
    }
    if (border_w > 0 && border_opa > 0) {
        lv_obj_set_style_border_width(o, border_w, 0);
        lv_obj_set_style_border_color(o, lv_color_hex(border), 0);
        lv_obj_set_style_border_opa(o, (lv_opa_t)border_opa, 0);
    }
    return o;
}

/* 1.2 kenar karartmasi + 1.3 alt karartma.
 * Tam ekran piksel dongusu TCG'de pahali; yumusak gradyan oldugu icin
 * 1/8 cozunurlukte hesaplanip 8x buyutulerek cizilir (gozle fark yok). */
#define SH_W 160
#define SH_H 90
static lv_image_dsc_t s_shade_dsc;

static void build_shade(void) {
    if (s_shade_img) return;
    if (!A || !A->mem || !A->mem->alloc) return;
    uint32_t *buf = (uint32_t *)A->mem->alloc((size_t)SH_W * SH_H * 4u);
    if (!buf) return;

    for (int y = 0; y < SH_H; y++) {
        int ry = (y * REF_H / SH_H) - 340;
        for (int x = 0; x < SH_W; x++) {
            int rx = (x * REF_W / SH_W) - 640;
            /* elips: rx/760, ry/520 */
            long nx = (long)rx * 10000 / 760, ny = (long)ry * 10000 / 520;
            long d2 = nx * nx + ny * ny;
            int t1000;
            if (d2 >= 100000000L) t1000 = 1000;
            else {
                /* sqrt(d2)/10000 -> t (0..1). isqrt ile. */
                long q = d2, r = 0, b = 1L << 30;
                while (b > q) b >>= 2;
                while (b) {
                    if (q >= r + b) { q -= r + b; r = (r >> 1) + b; }
                    else r >>= 1;
                    b >>= 2;
                }
                /* r = sqrt(d2) *  ?  isqrt(d2) -> *10000/10000 => t */
                t1000 = (int)(((long)r * 1000 + 9999) / 10000);
                if (t1000 > 1000) t1000 = 1000;
            }
            int a = (int)((long)A_026 * t1000 / 1000);
            /* 1.3 alt karartma: y 560..720 -> 0..A_026 */
            int py = y * REF_H / SH_H;
            if (py >= 560) {
                int lin = (int)((long)A_026 * (py - 560) / 160);
                if (lin > a) a = lin;
            }
            buf[y * SH_W + x] = ((uint32_t)a << 24) | (C_SHADE & 0xFFFFFFu);
        }
    }

    s_shade_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_shade_dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    s_shade_dsc.header.w = SH_W;
    s_shade_dsc.header.h = SH_H;
    s_shade_dsc.header.stride = SH_W * 4;
    s_shade_dsc.data = (const uint8_t *)buf;
    s_shade_dsc.data_size = (uint32_t)SH_W * SH_H * 4u;

    s_shade_img = lv_image_create(s_root);
    lv_obj_remove_style_all(s_shade_img);
    lv_obj_remove_flag(s_shade_img, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(s_shade_img, &s_shade_dsc);
    lv_obj_set_pos(s_shade_img, 0, 0);
    /* 8x buyutme: zoom birimi 1/256. Pivot sol-ust -> (0,0) sabit kalir. */
    lv_image_set_pivot(s_shade_img, 0, 0);
    lv_image_set_scale(s_shade_img, (uint32_t)(REF_W * 256 / SH_W));
    lv_obj_add_event_cb(s_shade_img, buf_free_cb, LV_EVENT_DELETE, buf);
}

/* ---- 1.6 alt cubuk ---- */
static void make_focusable(lv_obj_t *o);
static void bar_btn_cb(lv_event_t *e) {
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    if (id == 1) {                 /* ag: durum metni */
        const char *s = "Ag: bagli";
        if (A && A->net && A->net->link_type) {
            int t = A->net->link_type();
            if (t == 0) s = "Ag: baglanti yok";
        }
        if (s_error == 0) { s_error = s; s_rebuild = 1; }
    } else if (id == 2) {          /* TR: klavye duzeni dongusu */
        static const char *l[] = { "trq", "trf", "en" };
        static int k;
        k = (k + 1) % 3;
        if (A && A->input && A->input->kbd_layout)
            A->input->kbd_layout(k == 0 ? 0 : (k == 1 ? 1 : 2));
    } else if (id == 3) {
        if (A && A->system && A->system->reboot) A->system->reboot();
    } else if (id == 4) {
        if (A && A->system && A->system->shutdown) A->system->shutdown();
    }
}

static void build_bottom_bar(void) {
    struct { int x, id; const char *svg; } b[3] = {
        { 32,   1, SVG_NET     },
        { 1156, 3, SVG_RESTART },
        { 1208, 4, SVG_POWER   },
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *o = box(s_root, SS(40), SS(40), SS(20),
                          C_SHADE, A_050, C_WHITE, A_028, 1);
        lv_obj_set_pos(o, SX(b[i].x), SY(660));
        lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(o, bar_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)b[i].id);
        make_focusable(o);
        lv_obj_t *ic = icon_canvas(o, b[i].svg, SS(20));
        if (ic) lv_obj_center(ic);
    }
    /* TR dugmesi */
    lv_obj_t *tr = box(s_root, SS(40), SS(40), SS(20),
                       C_SHADE, A_050, C_WHITE, A_028, 1);
    lv_obj_set_pos(tr, SX(84), SY(660));
    lv_obj_add_flag(tr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tr, bar_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)2);
    make_focusable(tr);
    lv_obj_t *tl = add_label(tr, "TR", F_TR, C_WHITE, LV_OPA_COVER);
    lv_obj_center(tl);
}

/* ---- ortak katmanlar ---- */
static void on_btn(lv_event_t *e);
static void on_field_ready(lv_event_t *e);
static void disk_submit(void);

static void screen_begin(void) {
    lv_obj_clean(s_root);
    s_ta_name = s_ta_pw1 = s_ta_pw2 = s_ta_login = 0;
    s_clock = s_date = s_shade_img = 0;
    s_ic_n = 0;                       /* ikon tamponlari cocuklarla birlikte silindi */

    build_shade();

    s_date = add_label(s_root, "", F_DATE, C_INK, LV_OPA_COVER);
    lv_obj_set_width(s_date, LV_PCT(100));
    lv_obj_set_style_text_align(s_date, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(s_date, SS(6) / 10, 0);
    lv_obj_set_pos(s_date, 0, SY(64));

    s_clock = add_label(s_root, "", F_CLOCK, C_INK, LV_OPA_COVER);
    lv_obj_set_width(s_clock, LV_PCT(100));
    lv_obj_set_style_text_align(s_clock, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(s_clock, -SS(6), 0);
    lv_obj_set_pos(s_clock, 0, SY(92));
    update_clock();

    lv_group_remove_all_objs(s_group);
}

static void error_label(void) {
    if (!s_error) return;
    lv_obj_t *l = add_label(s_root, s_error, F_Q, C_ERROR, LV_OPA_COVER);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(l, 0, SY(600));
}

/* ---- kapsul (kullanici adi / sifre) ---- */
static void on_field_click(lv_event_t *e) {
    lv_obj_t *ta = lv_event_get_user_data(e);
    if (!s_group || !ta) return;
    if (lv_obj_get_group(ta) != s_group) lv_group_add_obj(s_group, ta);
    lv_group_focus_obj(ta);
}

static void on_field_state(lv_event_t *e) {
    lv_obj_t *ta = lv_event_get_target(e);
    lv_opa_t opa = (lv_event_get_code(e) == LV_EVENT_FOCUSED) ? LV_OPA_COVER : LV_OPA_TRANSP;
    lv_obj_set_style_bg_opa(ta, opa, LV_PART_CURSOR);
}

/* kapsul: x450 w380 h60 r30; y disaridan. ikon + yazi alani + ok dugmesi */
static lv_obj_t *make_capsule(int y, const char *icon_svg, const char *ph,
                              int password, int ok_id, lv_obj_t **ta_out) {
    lv_obj_t *cap = box(s_root, SS(380), SS(60), SS(30),
                        C_WHITE, A_042, C_WHITE, A_070, 1);
    lv_obj_set_pos(cap, SX(450), SY(y));
    lv_obj_add_flag(cap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(cap, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ic = icon_canvas(cap, icon_svg, SS(20));
    if (ic) lv_obj_set_pos(ic, SS(26), SS(20));

    /* Ust parlama (inset): 1 px beyaz .55, kapsulun ic ust kenari */
    lv_obj_t *hl = box(cap, SS(340), SS(1), SS(1), C_WHITE, A_055, 0, 0, 0);
    lv_obj_set_pos(hl, SS(20), SS(1));
    lv_obj_remove_flag(hl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *ta = lv_textarea_create(cap);
    lv_obj_remove_style_all(ta);
    lv_obj_remove_flag(ta, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(ta, SS(262), SS(60));
    lv_obj_set_pos(ta, SS(56), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_pad_left(ta, 0, 0);
    lv_obj_set_style_pad_right(ta, 0, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(C_INK), 0);
    lv_obj_set_style_text_font(ta, F_FIELD, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(C_INK2), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_font(ta, F_FIELD, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(ta, lv_color_hex(C_INK), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_style_border_width(ta, 0, LV_PART_CURSOR);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, 31);
    if (ph) lv_textarea_set_placeholder_text(ta, ph);
    if (password) lv_textarea_set_password_mode(ta, true);
    lv_obj_remove_flag(ta, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ta, on_field_state, LV_EVENT_FOCUSED, 0);
    lv_obj_add_event_cb(ta, on_field_state, LV_EVENT_DEFOCUSED, 0);
    lv_obj_add_event_cb(cap, on_field_click, LV_EVENT_CLICKED, ta);

    /* ok dugmesi: x774 y+8 44x44 r22 dolgu #2B2438 */
    lv_obj_t *ok = box(cap, SS(44), SS(44), SS(22), C_INK, 255, C_INK, 0, 0);
    lv_obj_set_pos(ok, SS(324), SS(8));
    lv_obj_add_flag(ok, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ok, on_btn, LV_EVENT_CLICKED, (void *)(intptr_t)ok_id);
    make_focusable(ok);
    lv_obj_t *ai = icon_canvas(ok, SVG_ARROW, SS(22));
    if (ai) lv_obj_center(ai);

    if (ta_out) *ta_out = ta;
    return cap;
}

/* ---- 2.4 Evet / Hayir ---- */
static lv_obj_t *make_pill_btn(int x, int y, const char *txt, int filled, int id) {
    lv_obj_t *b = filled
        ? box(s_root, SS(120), SS(44), SS(22), C_DARK, 255, C_DARK, 0, 0)
        : box(s_root, SS(120), SS(44), SS(22), C_WHITE, A_050, C_WHITE, A_070, 1);
    lv_obj_set_pos(b, SX(x), SY(y));
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, on_btn, LV_EVENT_CLICKED, (void *)(intptr_t)id);
    make_focusable(b);
    lv_obj_t *l = add_label(b, txt, F_BTN, filled ? C_WHITE : C_INK, LV_OPA_COVER);
    lv_obj_center(l);
    return b;
}

static void focus_field(lv_obj_t *ta) {
    if (!ta) return;
    lv_group_add_obj(s_group, ta);
    lv_group_focus_obj(ta);
}

static void focus_pair(lv_obj_t *a, lv_obj_t *b) {
    if (a) lv_group_add_obj(s_group, a);
    if (b) lv_group_add_obj(s_group, b);
    if (a) lv_group_focus_obj(a);
}

/* ---- 2. Avatar dairesi (gradyan) ---- */
static uint32_t lerp_rgb(uint32_t a, uint32_t b, int t) {   /* t: 0..255 */
    int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    int r = ar + (br - ar) * t / 255;
    int g = ag + (bg - ag) * t / 255;
    int bl = ab + (bb - ab) * t / 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

/* ---- 7. Odak halkasi: 2 px bosluk, 3 px beyaz .90 ----
 * Sartname bolum 7. Klavye ile gezinirken dugmelerde gorunur. */
static void make_focusable(lv_obj_t *o) {
    if (!o) return;
    lv_obj_set_style_outline_width(o, SS(3), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(o, SS(2), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(o, lv_color_hex(C_WHITE), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_opa(o, A_090, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(o, 0, 0);
    if (s_group) lv_group_add_obj(s_group, o);
}

/* ---- 5. Avatar golgesi: ofset (0,10), sigma ~12 ----
 * 1/4 cozunurlukte uretilip 4x buyutulur; gozle fark yok, TCG'de hizli. */
static void avatar_shadow(int x, int y, int alpha) {
    int d = SS(88);
    int pad = SS(14);
    int full = d + pad * 2;
    int q = 4;
    int sw = full / q;
    if (sw < 8) { q = 1; sw = full; }
    int sh = sw;
    uint32_t *buf = (uint32_t *)A->mem->alloc((size_t)sw * sh * 4u);
    if (!buf) return;

    int cx = sw / 2, cy = sh / 2;
    int r  = (d / 2) / q;
    int sig = SS(12) / q;
    if (sig < 2) sig = 2;
    long lim = (long)(3 * sig) * (3 * sig);

    for (int py = 0; py < sh; py++) {
        for (int px = 0; px < sw; px++) {
            int dx = px - cx, dy = py - cy;
            long d2 = (long)dx * dx + (long)dy * dy;
            long r2 = (long)r * r;
            int a;
            if (d2 <= r2) {
                a = alpha;
            } else {
                long over = d2 - r2;
                if (over >= lim) { buf[py * sw + px] = 0; continue; }
                a = (int)((long)alpha * (lim - over) / lim);
            }
            buf[py * sw + px] = ((uint32_t)a << 24) | (C_SHADE & 0xFFFFFFu);
        }
    }

    lv_obj_t *c = lv_canvas_create(s_root);
    lv_obj_remove_style_all(c);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_canvas_set_buffer(c, buf, sw, sh, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_add_event_cb(c, buf_free_cb, LV_EVENT_DELETE, buf);
    lv_obj_set_pos(c, SX(x) - pad, SY(y + 10) - pad);
    if (q > 1) {
        lv_image_set_pivot(c, 0, 0);
        lv_image_set_scale(c, (uint32_t)(q * 256));
    }
}

/* ---- 3.1 kesikli daire ("Hesap ekle") ----
 * 48 adimlik donus: tek adimlar cizgi, cift adimlar bosluk.
 * Yaricap uzerinde yuruyup 2px kalinlikta nokta damgalar. */
static lv_obj_t *dashed_circle(int x, int y, int d, uint32_t col, int alpha) {
    uint32_t *buf = (uint32_t *)A->mem->alloc((size_t)d * d * 4u);
    if (!buf) return 0;
    for (int i = 0; i < d * d; i++) buf[i] = 0;

    int c = d / 2;
    int r = c - SS(2);
    if (r < 4) { A->mem->free(buf); return 0; }

    /* 7.5 derece: cos = 0.991445, sin = 0.130526 (Q16) */
    const long COS7 = 64975, SIN7 = 8554;
    long px = (long)r << 16, py = 0;      /* (r, 0) */
    int thick = SS(2);
    if (thick < 1) thick = 1;
    int rad = thick;                      /* damga yaricapi */

    for (int step = 0; step < 48; step++) {
        long nx = (px * COS7 - py * SIN7) >> 16;
        long ny = (px * SIN7 + py * COS7) >> 16;
        int draw = (step & 1) == 0;       /* 24 cizgi, 24 bosluk */
        if (draw) {
            /* p -> n arasi nokta damgalari */
            int steps = 4;
            for (int t = 0; t <= steps; t++) {
                long ix = (px + (nx - px) * t / steps) >> 16;
                long iy = (py + (ny - py) * t / steps) >> 16;
                int sx = c + (int)ix, sy = c + (int)iy;
                for (int oy = -rad; oy <= rad; oy++) {
                    for (int ox = -rad; ox <= rad; ox++) {
                        if (ox * ox + oy * oy > rad * rad) continue;
                        int qx = sx + ox, qy = sy + oy;
                        if (qx < 0 || qy < 0 || qx >= d || qy >= d) continue;
                        uint32_t *p = &buf[qy * d + qx];
                        int old = (int)(*p >> 24);
                        if (alpha > old)
                            *p = ((uint32_t)alpha << 24) | (col & 0xFFFFFFu);
                    }
                }
            }
        }
        px = nx; py = ny;
    }

    lv_obj_t *cv = lv_canvas_create(s_root);
    lv_obj_remove_style_all(cv);
    lv_obj_remove_flag(cv, LV_OBJ_FLAG_CLICKABLE);
    lv_canvas_set_buffer(cv, buf, d, d, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_add_event_cb(cv, buf_free_cb, LV_EVENT_DELETE, buf);
    lv_obj_set_pos(cv, SX(x), SY(y));
    return cv;
}

/* 145 derece gradyanli daire; harf yoksa arti ikonu cizilir. */
static lv_obj_t *avatar_circle(int x, int y, uint32_t ca, uint32_t cb,
                               const char *letter) {
    int d = SS(88);
    uint32_t *buf = (uint32_t *)A->mem->alloc((size_t)d * d * 4u);
    if (!buf) return 0;
    long L = (long)(d * 5736 / 10000) + (long)(d * 8192 / 10000);
    if (L <= 0) L = 1;
    int cx = d / 2, cy = d / 2;
    for (int py = 0; py < d; py++) {
        for (int px = 0; px < d; px++) {
            int dx = px - cx, dy = py - cy;
            long dist2 = (long)dx * dx + (long)dy * dy;
            if (dist2 > (long)cx * cx) { buf[py * d + px] = 0; continue; }
            long t = (((long)dx * 5736 + (long)dy * 8192) * 1000 / L) + 500;
            if (t < 0) t = 0;
            if (t > 1000) t = 1000;
            buf[py * d + px] = 0xFF000000u | lerp_rgb(ca, cb, (int)(t * 255 / 1000));
        }
    }
    lv_obj_t *c = lv_canvas_create(s_root);
    lv_obj_remove_style_all(c);
    lv_canvas_set_buffer(c, buf, d, d, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_add_event_cb(c, buf_free_cb, LV_EVENT_DELETE, buf);
    lv_obj_set_pos(c, SX(x), SY(y));
    if (letter && letter[0]) {
        lv_obj_t *l = add_label(c, letter, F_AV, C_WHITE, LV_OPA_COVER);
        lv_obj_center(l);
    }
    return c;
}

/* ================= Ekran 1: Hesap olusturma ================= */
static void build_create(void) {
    screen_begin();

    lv_obj_t *h = add_label(s_root, "Hesap oluşturun", F_HEAD, C_INK, LV_OPA_COVER);
    lv_obj_set_width(h, LV_PCT(100));
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(h, -SS(1), 0);
    lv_obj_set_pos(h, 0, SY(350));

    make_capsule(406, SVG_USER, "Kullanıcı adı", 0, 1, &s_ta_name);

    lv_obj_t *q = add_label(s_root, "Şifre oluşturulsun mu?", F_Q, C_INK, LV_OPA_COVER);
    lv_obj_set_width(q, LV_PCT(100));
    lv_obj_set_style_text_align(q, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(q, 0, SY(498));

    make_pill_btn(514, 536, "Evet", 1, 3);
    make_pill_btn(646, 536, "Hayır", 0, 4);

    error_label();
    build_bottom_bar();
    focus_field(s_ta_name);
}

/* ================= Ekran 1b: Sifre olusturma ================= */
static void build_create_pw(void) {
    screen_begin();
    lv_obj_t *h = add_label(s_root, "Şifre oluşturun", F_HEAD, C_INK, LV_OPA_COVER);
    lv_obj_set_width(h, LV_PCT(100));
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(h, 0, SY(350));

    make_capsule(398, SVG_LOCK, "Parola", 1, 5, &s_ta_pw1);
    make_capsule(478, SVG_LOCK, "Parola tekrar", 1, 5, &s_ta_pw2);

    error_label();
    build_bottom_bar();
    focus_pair(s_ta_pw1, s_ta_pw2);
}

/* ================= Ekran 2: Kullanici secme ================= */
#define AV_MAX 3
static lv_obj_t *s_ring;

static void build_select(void) {
    screen_begin();
    int n = arctian_auth_user_count();
    if (s_sel_idx >= n) s_sel_idx = 0;

    /* En fazla 2 kullanici + "Hesap ekle" gosterilir (sartname 3 daire).
     * Grup yatayda ortalanir: 3 dairede x0=456 (sartname 3.1). */
    int shown = n > 2 ? 2 : n;
    int nc = shown + (shown < 3 ? 1 : 0);
    int x0 = 640 - ((nc - 1) * 140 + 88) / 2;

    /* Once golgeler (avatarlarin altinda kalsin). */
    for (int i = 0; i < shown; i++)
        avatar_shadow(x0 + i * 140, 328, (i == 0) ? A_028 : A_022);

    for (int i = 0; i < shown; i++) {
        int x = x0 + i * 140;
        uint32_t ca = (i == 0) ? C_AV1_A : C_AV2_A;
        uint32_t cb = (i == 0) ? C_AV1_B : C_AV2_B;
        const char *nm = arctian_auth_user_name(i);
        char letter[2] = { 0, 0 };
        if (nm && nm[0]) {
            char c = nm[0];
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            letter[0] = c;
        }
        lv_obj_t *av = avatar_circle(x, 328, ca, cb, letter);
        if (av) {
            lv_obj_add_flag(av, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(av, on_btn, LV_EVENT_CLICKED, (void *)(intptr_t)(10 + i));
            make_focusable(av);
        }
        lv_obj_t *nl = add_label(s_root, nm,
                                 (i == s_sel_idx) ? F_NAME_B : F_NAME,
                                 C_INK, LV_OPA_COVER);
        lv_obj_set_width(nl, SS(120));
        lv_obj_set_style_text_align(nl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(nl, SX(x - 16), SY(430));
    }

    /* "Hesap ekle" dairesi: beyaz .18 dolgu + 2 px kesikli cerceve */
    int ax = x0 + shown * 140;
    if (shown < 3) {
        lv_obj_t *add = box(s_root, SS(88), SS(88), SS(44), C_WHITE, A_018, 0, 0, 0);
        lv_obj_set_pos(add, SX(ax), SY(328));
        lv_obj_add_flag(add, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(add, on_btn, LV_EVENT_CLICKED, (void *)(intptr_t)6);
        make_focusable(add);
        dashed_circle(ax, 328, SS(88), C_SHADE, A_055);
        lv_obj_t *pi = icon_canvas(add, SVG_PLUS, SS(28));
        if (pi) lv_obj_center(pi);
        lv_obj_t *al = add_label(s_root, "Hesap ekle", F_NAME, C_INK, LV_OPA_COVER);
        lv_obj_set_width(al, SS(120));
        lv_obj_set_style_text_align(al, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(al, SX(ax - 16), SY(430));
    }

    /* Secili halka: dairenin 5 px disinda, 2 px beyaz .95 */
    if (shown > 0) {
        s_ring = box(s_root, SS(98), SS(98), SS(49), 0, 0, C_WHITE, A_095, SS(2));
        lv_obj_set_pos(s_ring, SX(x0 + s_sel_idx * 140 - 5), SY(323));
        lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    }

    /* 3.4 sifre kapsulu (y 490) */
    make_capsule(490, SVG_LOCK, "Şifre", 1, 5, &s_ta_login);
    if (s_ta_login)
        lv_obj_add_event_cb(s_ta_login, on_field_ready, LV_EVENT_READY, (void *)(intptr_t)5);

    error_label();
    build_bottom_bar();
    focus_field(s_ta_login);
}

/* ================= Eylemler ================= */
static void do_action(int id) {
    if (id >= 10) {
        int i = id - 10;
        if (i < 0 || i >= arctian_auth_user_count()) return;
        const char *nm = arctian_auth_user_name(i);
        int k = 0; for (; nm[k] && k < 31; k++) s_target[k] = nm[k];
        s_target[k] = 0;
        s_sel_idx = i;
        if (arctian_auth_user_has_password(i)) {
            s_step = ST_SELECT;      /* ekran 2: sifre kapsulu odaga gelir */
            s_error = 0;
            s_rebuild = 1;
        } else {
            arctian_auth_set_current(s_target);
            s_done = 1;
        }
        return;
    }
    switch (id) {
        case 1:                      /* Ekran 1: Devam oku -> sifre ekrani */
        case 3:                      /* Evet: sifre olustur */
            read_ta(s_ta_name, s_pending, sizeof(s_pending));
            if (!s_pending[0]) { s_error = "Kullanıcı adı gerekli"; s_rebuild = 1; break; }
            s_error = 0;
            s_want_pw = 1;
            s_step = ST_CREATE_PW;
            s_rebuild = 1;
            break;
        case 4: {                    /* Hayir: sifresiz hesap */
            int first = (arctian_auth_user_count() == 0);
            read_ta(s_ta_name, s_pending, sizeof(s_pending));
            if (!s_pending[0]) { s_error = "Kullanıcı adı gerekli"; s_rebuild = 1; break; }
            int r = arctian_auth_create_user(s_pending, 0);
            if (r == 0) {
                s_error = 0;
                if (first) { arctian_auth_set_current(s_pending); s_done = 1; }
                else { s_step = ST_SELECT; s_rebuild = 1; }
            } else if (r == -6) { s_error = "Bu ad zaten var"; s_rebuild = 1; }
            else if (r == -2) { s_error = "Geçersiz karakter"; s_rebuild = 1; }
            else { s_error = "Hesap oluşturulamadı"; s_rebuild = 1; }
            break;
        }
        case 5: {                    /* Giris / sifre olustur */
            if (s_step == ST_CREATE_PW) {
                char p1[40], p2[40];
                read_ta(s_ta_pw1, p1, sizeof(p1));
                read_ta(s_ta_pw2, p2, sizeof(p2));
                if (!p1[0]) { s_error = "Parola gerekli"; s_rebuild = 1; break; }
                if (!streq(p1, p2)) { s_error = "Parolalar eşleşmiyor"; s_rebuild = 1; break; }
                int first = (arctian_auth_user_count() == 0);
                int r = arctian_auth_create_user(s_pending, p1);
                if (r == 0) {
                    s_error = 0;
                    if (first) { arctian_auth_set_current(s_pending); s_done = 1; }
                    else { s_step = ST_SELECT; s_rebuild = 1; }
                } else if (r == -6) { s_error = "Bu ad zaten var"; s_rebuild = 1; }
                else { s_error = "Hesap oluşturulamadı"; s_rebuild = 1; }
                break;
            }
            char p[40];
            if (!s_target[0] && arctian_auth_user_count() > 0) {
                const char *nm = arctian_auth_user_name(s_sel_idx);
                int k = 0;
                for (; nm && nm[k] && k < 31; k++) s_target[k] = nm[k];
                s_target[k] = 0;
            }
            read_ta(s_ta_login, p, sizeof(p));
            if (arctian_auth_verify_user(s_target, p)) {
                arctian_auth_set_current(s_target);
                s_done = 1;
            } else {
                s_error = "Parola hatalı";
                s_rebuild = 1;
            }
            break;
        }
        case 6:                      /* Yeni hesap */
            s_pending[0] = 0;
            s_target[0] = 0;
            s_error = 0;
            s_step = ST_CREATE;
            s_rebuild = 1;
            break;
        case 777:                    /* Disk parolasi */
            disk_submit();
            break;
        default: break;
    }
}

static void on_btn(lv_event_t *e) {
    do_action((int)(intptr_t)lv_event_get_user_data(e));
}

static void on_field_ready(lv_event_t *e) {
    do_action((int)(intptr_t)lv_event_get_user_data(e));
}

/* ================= Giris ================= */
static void lock_fonts_init(void) {
    if (F_CLOCK) return;
    /* 168 px saat, 18 tarih, 28 baslik, 17 soru, 16 dugme, 44 avatar... */
    F_CLOCK = lv_tiny_ttf_create_data(font_manrope_thin, font_manrope_thin_len, 168);
    F_DATE  = lv_tiny_ttf_create_data(font_manrope_sb, font_manrope_sb_len, 18);
    F_HEAD  = lv_tiny_ttf_create_data(font_manrope_bold, font_manrope_bold_len, 28);
    F_Q     = lv_tiny_ttf_create_data(font_manrope_sb, font_manrope_sb_len, 17);
    F_BTN   = lv_tiny_ttf_create_data(font_manrope_bold, font_manrope_bold_len, 16);
    F_NAME  = lv_tiny_ttf_create_data(font_manrope_sb, font_manrope_sb_len, 17);
    F_NAME_B = lv_tiny_ttf_create_data(font_manrope_bold, font_manrope_bold_len, 17);
    F_FIELD = lv_tiny_ttf_create_data(font_manrope_sb, font_manrope_sb_len, 18);
    F_AV    = lv_tiny_ttf_create_data(font_manrope_bold, font_manrope_bold_len, 36);
    F_TR    = lv_tiny_ttf_create_data(font_manrope_bold, font_manrope_bold_len, 13);
}

/* ================= Disk parolasi =================
 * Sifreli disk takiliysa giris ekranindan ONCE calisir. Ayni gorsel dil
 * (kapsul + ok dugmesi), farkli baslik. */
static lv_obj_t *s_ta_disk, *s_disk_errlbl;
static int       s_disk_done;

static void disk_set_err(const char *s) {
    if (s_disk_errlbl) lv_label_set_text(s_disk_errlbl, s ? s : "");
}

static void disk_submit(void) {
    char p[64];
    read_ta(s_ta_disk, p, sizeof(p));
    if (!p[0]) { disk_set_err("Parola gerekli"); return; }
    if (!A->crypt || !A->crypt->unlock) { disk_set_err("Şifreleme servisi yok"); return; }
    if (A->crypt->unlock(p) == 0) {
        s_disk_done = 1;
    } else {
        disk_set_err("Parola hatalı, tekrar deneyin");
        if (s_ta_disk) lv_textarea_set_text(s_ta_disk, "");
    }
}

static void disk_click_cb(lv_event_t *e) { (void)e; disk_submit(); }
static void disk_ready_cb(lv_event_t *e) { (void)e; disk_submit(); }

static void build_disk(void) {
    screen_begin();
    s_ta_disk = 0;
    s_disk_errlbl = 0;

    lv_obj_t *h = add_label(s_root, "Disk parolası", F_HEAD, C_INK, LV_OPA_COVER);
    lv_obj_set_width(h, LV_PCT(100));
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(h, -SS(1), 0);
    lv_obj_set_pos(h, 0, SY(350));

    make_capsule(406, SVG_LOCK, "Disk parolası", 1, 777, &s_ta_disk);
    if (s_ta_disk)
        lv_obj_add_event_cb(s_ta_disk, disk_ready_cb, LV_EVENT_READY, 0);

    s_disk_errlbl = add_label(s_root, "", F_Q, C_ERROR, LV_OPA_COVER);
    lv_obj_set_width(s_disk_errlbl, LV_PCT(100));
    lv_obj_set_style_text_align(s_disk_errlbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_disk_errlbl, 0, SY(498));

    build_bottom_bar();
    focus_field(s_ta_disk);
}

void disk_unlock_run(asi_t *asi) {
    if (!asi || !asi->gfx || !asi->mem || !asi->crypt) return;
    if (!asi->crypt->present() || asi->crypt->unlocked()) return;
    A = asi;

    lock_fonts_init();
    if (!F_CLOCK) return;

    int W = asi->gfx->width(), H = asi->gfx->height();
    int sx = (int)((long)W * 1000 / REF_W);
    int sy = (int)((long)H * 1000 / REF_H);
    s_sc = sx < sy ? sx : sy;
    s_ox = (W - (int)((long)REF_W * s_sc / 1000)) / 2;
    s_oy = (H - (int)((long)REF_H * s_sc / 1000)) / 2;

    s_root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, W, H);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);

    s_group = lv_group_create();
    lv_indev_t *kp = lv_port_get_keypad();
    lv_indev_t *ptr = lv_port_get_indev();
    if (kp) lv_indev_set_group(kp, s_group);
    if (ptr) {
        lv_indev_set_group(ptr, s_group);
        lv_obj_t *cur = lv_indev_get_cursor(ptr);
        if (cur) lv_obj_move_foreground(cur);
    }

    s_disk_done = 0;
    lock_freeze_timers();
    build_disk();

    while (!s_disk_done) {
        update_clock();
        lv_timer_handler();
        {
            int dx, dy, dw, dh;
            if (lv_port_take_dirty(&dx, &dy, &dw, &dh) && dw > 0 && dh > 0)
                asi->gfx->present_rect(dx, dy, dw, dh);
        }
        asm volatile("hlt");
    }

    if (kp) lv_indev_set_group(kp, 0);
    if (ptr) lv_indev_set_group(ptr, 0);
    lv_group_delete(s_group);
    s_group = 0;
    if (s_root) { lv_obj_delete(s_root); s_root = 0; }
    s_ic_n = 0;
    s_ta_disk = 0;
    s_disk_errlbl = 0;
    lock_thaw_timers();
}

void lock_screen_run(asi_t *asi) {
    if (!asi || !asi->gfx || !asi->mem) return;
#ifdef ARCTIAN_AUTOUSER
    if (arctian_auth_has_user() && arctian_auth_current()[0]) return;
#endif
    A = asi;

    lock_fonts_init();
    if (!F_CLOCK) return;            /* font yoksa eski davranisa dusmeyelim */

    int W = asi->gfx->width(), H = asi->gfx->height();
    /* 1280x720 referansini ekrana esit oranli sigdir, ortala. */
    int sx = (int)((long)W * 1000 / REF_W);
    int sy = (int)((long)H * 1000 / REF_H);
    s_sc = sx < sy ? sx : sy;
    s_ox = (W - (int)((long)REF_W * s_sc / 1000)) / 2;
    s_oy = (H - (int)((long)REF_H * s_sc / 1000)) / 2;

    s_root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, W, H);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);   /* duvar kagidi gorunur */

    s_group = lv_group_create();
    lv_indev_t *kp = lv_port_get_keypad();
    lv_indev_t *ptr = lv_port_get_indev();
    if (kp) lv_indev_set_group(kp, s_group);
    if (ptr) lv_indev_set_group(ptr, s_group);

    s_done = 0;
    s_error = 0;
    s_want_pw = 0;
    s_sel_idx = 0;
    s_step = arctian_auth_has_user() ? ST_SELECT : ST_CREATE;
    lock_freeze_timers();

    while (!s_done) {
        s_rebuild = 0;
        if (s_step == ST_CREATE)         build_create();
        else if (s_step == ST_CREATE_PW) build_create_pw();
        else                             build_select();

        while (!s_done && !s_rebuild) {
            update_clock();
            lv_timer_handler();
            {
                int dx, dy, dw, dh;
                if (lv_port_take_dirty(&dx, &dy, &dw, &dh) && dw > 0 && dh > 0)
                    asi->gfx->present_rect(dx, dy, dw, dh);
            }
            asm volatile("hlt");
        }
    }

    if (kp) lv_indev_set_group(kp, 0);
    if (ptr) lv_indev_set_group(ptr, 0);
    lv_group_delete(s_group);
    s_group = 0;
    if (s_root) { lv_obj_delete(s_root); s_root = 0; }
    s_ic_n = 0;
    lock_thaw_timers();
}
