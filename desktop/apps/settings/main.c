#include "app.h"
#include "adl_wolf.h"
#include "lv_port.h"
#include "svg.h"
#include <stdint.h>

#define S_NAV_W   200
#define S_POOL    64
#define S_NPAGES  7

static asi_t    *s_asi;
static lv_obj_t *g_win;
static lv_obj_t *g_nav;
static lv_obj_t *g_content;
static lv_obj_t *g_nav_btns[S_NPAGES];
static lv_obj_t *g_nav_lbl[S_NPAGES];
static lv_obj_t *g_nav_rod[S_NPAGES];
static int       g_page = 0;

static lv_obj_t   *g_search_ta;
static lv_group_t *g_search_grp;
static char        s_search_q[48];

static lv_obj_t   *g_crypt_ta;
static lv_group_t *g_crypt_group;
static char        s_crypt_msg[96];

static lv_obj_t *g_fw_port;
static lv_obj_t *g_fw_name;
static lv_obj_t *g_fw_dir;
static lv_obj_t *g_fw_proto;
static lv_obj_t *g_fw_action;
static char      s_fw_msg[96];

static lv_obj_t   *g_acc_user;
static lv_obj_t   *g_acc_pw;
static lv_obj_t   *g_pw_new1;
static lv_obj_t   *g_pw_new2;
static lv_group_t *g_acc_grp;
static char        s_acc_msg[96];

static char s_pool[S_POOL][40];
static int  s_pool_n = 0;

static lv_obj_t   *g_upd_msg;
static lv_obj_t   *g_upd_bar;
static lv_timer_t *g_upd_timer;

static const char *const g_nav_labels[S_NPAGES] = {
    "Görünüm", "Ağ ve internet", "Bildirimler", "Gizlilik ve güvenlik",
    "Güncelleme", "Hakkında", "Hesaplar"
};

static const char *const g_nav_icons[S_NPAGES] = {
    "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#B4BAC5\" stroke-width=\"1.8\"><circle cx=\"12\" cy=\"12\" r=\"4\"/><path d=\"M12 2v2M12 20v2M2 12h2M20 12h2\"/></svg>",
    "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#B4BAC5\" stroke-width=\"1.8\"><path d=\"M2 9a15 15 0 0 1 20 0M5 13a10 10 0 0 1 14 0M8.5 16.5a5 5 0 0 1 7 0\"/></svg>",
    "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#B4BAC5\" stroke-width=\"1.8\"><path d=\"M6 8a6 6 0 0 1 12 0c0 7 3 8 3 8H3s3-1 3-8M10 20a2 2 0 0 0 4 0\"/></svg>",
    "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#B4BAC5\" stroke-width=\"1.8\"><rect x=\"5\" y=\"11\" width=\"14\" height=\"10\" rx=\"2\"/><path d=\"M8 11V7a4 4 0 0 1 8 0v4\"/></svg>",
    "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#B4BAC5\" stroke-width=\"1.8\"><path d=\"M4 12a8 8 0 0 1 14-5M20 12a8 8 0 0 1-14 5M18 3v4h-4M6 21v-4h4\"/></svg>",
    "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#B4BAC5\" stroke-width=\"1.8\"><circle cx=\"12\" cy=\"12\" r=\"9\"/><path d=\"M12 11v5M12 8h.01\"/></svg>",
    "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#B4BAC5\" stroke-width=\"1.8\"><circle cx=\"12\" cy=\"8\" r=\"4\"/><path d=\"M4 20a8 8 0 0 1 16 0\"/></svg>",
};

#define SC_CARD    0x181B22
#define SC_CARDBRD 0x262B35
#define SC_HAIR    0x222731
#define SC_NAVON   0x1E2330
#define SC_NAVBAR  0x60A5FA
#define SC_NAVFG   0xB4BAC5
#define SC_INP     0x0F1115
#define SC_BTNBRD  0x333A47
#define SC_ACC     0x3B82F6

extern const unsigned char font_ttf[];
extern const unsigned int  font_ttf_len;
static lv_font_t *f22, *f13, *f12;

#define S_ICON_MAX 32
static uint32_t *s_icon_buf[S_ICON_MAX];
static int s_icon_n;

static lv_obj_t *s_svg_icon(lv_obj_t *parent, const char *svg, int size) {
    if (!svg[0] || !s_asi || !s_asi->mem) return 0;
    if (s_icon_n >= S_ICON_MAX) return 0;
    uint32_t *buf = (uint32_t *)s_asi->mem->alloc((size_t)size * size * 4u);
    if (!buf) return 0;
    if (svg_render(svg, size, size, buf) != 0) { s_asi->mem->free(buf); return 0; }
    s_icon_buf[s_icon_n++] = buf;
    lv_obj_t *c = lv_canvas_create(parent);
    lv_obj_remove_style_all(c);
    lv_canvas_set_buffer(c, buf, size, size, LV_COLOR_FORMAT_ARGB8888);
    return c;
}

static int slen(const char *s) { int n = 0; if (s) while (s[n]) n++; return n; }

static void scpy(char *d, int cap, const char *s) {
    int i = 0;
    if (cap <= 0) return;
    if (!s) { d[0] = 0; return; }
    for (; s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}

static int seq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

static void snum(char *b, int cap, unsigned int v) {
    char t[12];
    int n = 0, k = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n && k < cap - 1) b[k++] = t[--n];
    b[k] = 0;
}

static void scat(char *d, int cap, const char *s) {
    int i = slen(d), j = 0;
    for (; s && s[j] && i < cap - 1; i++, j++) d[i] = s[j];
    d[i] = 0;
}

static char s_lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int s_has_ci(const char *hay, const char *needle) {
    if (!hay || !needle || !needle[0]) return 0;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] && s_lc(hay[i + j]) == s_lc(needle[j])) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

static const char *spool(const char *s) {
    if (s_pool_n >= S_POOL) return "";
    char *d = s_pool[s_pool_n++];
    scpy(d, 40, s);
    return d;
}

static void sip(char *out, int cap, const uint8_t ip[4]) {
    out[0] = 0;
    for (int i = 0; i < 4; i++) {
        char n[8];
        snum(n, sizeof(n), ip[i]);
        scat(out, cap, n);
        if (i < 3) scat(out, cap, ".");
    }
}

static lv_obj_t *scard(lv_obj_t *parent) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_width(c, LV_PCT(100));
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, lv_color_hex(SC_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 8, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(SC_CARDBRD), 0);
    lv_obj_set_style_pad_all(c, 16, 0);
    lv_obj_set_style_pad_row(c, 8, 0);
    lv_obj_set_style_pad_column(c, 8, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static void stitle(lv_obj_t *parent, const char *txt) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(ADL_TEXT), 0);
    if (f13) lv_obj_set_style_text_font(l, f13, 0);
}

static void sdesc(lv_obj_t *parent, const char *txt) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(ADL_TEXT_DIM), 0);
    if (f12) lv_obj_set_style_text_font(l, f12, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
}

static lv_obj_t *srow(lv_obj_t *parent, int h) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, h);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_column(r, 8, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return r;
}

static lv_obj_t *srow_start(lv_obj_t *parent, int h) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, h);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_column(r, 8, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return r;
}

static lv_obj_t *sbutton(lv_obj_t *parent, const char *txt, uint32_t bg, uint32_t fg, int w, int h) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
    if (f12) lv_obj_set_style_text_font(l, f12, 0);
    lv_obj_center(l);
    return b;
}

static void skv(lv_obj_t *parent, const char *k, const char *v) {
    lv_obj_t *r = srow(parent, 30);
    lv_obj_t *kl = lv_label_create(r);
    lv_label_set_text(kl, k);
    lv_obj_set_style_text_color(kl, lv_color_hex(ADL_TEXT_DIM), 0);
    if (f12) lv_obj_set_style_text_font(kl, f12, 0);
    lv_obj_t *vl = lv_label_create(r);
    lv_label_set_text(vl, v);
    lv_obj_set_style_text_color(vl, lv_color_hex(ADL_TEXT), 0);
    if (f12) lv_obj_set_style_text_font(vl, f12, 0);
}

static void s_content_clear(void) {
    if (g_content) lv_obj_clean(g_content);
}

static void s_nav_active(void) {
    for (int i = 0; i < S_NPAGES; i++) {
        if (!g_nav_btns[i]) continue;
        int on = (i == g_page);
        lv_obj_set_style_bg_color(g_nav_btns[i], lv_color_hex(on ? SC_NAVON : 0x000000), 0);
        lv_obj_set_style_bg_opa(g_nav_btns[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (g_nav_lbl[i])
            lv_obj_set_style_text_color(g_nav_lbl[i],
                lv_color_hex(on ? 0xFFFFFF : SC_NAVFG), 0);
        if (g_nav_rod[i])
            lv_obj_set_style_bg_opa(g_nav_rod[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

static void s_page(int p);

static void s_nav_cb(lv_event_t *e) {
    int p = (int)(intptr_t)lv_event_get_user_data(e);
    s_search_q[0] = 0;
    if (g_search_ta) lv_textarea_set_text(g_search_ta, "");
    s_page(p);
}

static void s_toggle_cb(lv_event_t *e);

static void s_phead(const char *crumb, const char *title) {
    lv_obj_t *c = lv_label_create(g_content);
    lv_label_set_text(c, crumb);
    lv_obj_set_style_text_color(c, lv_color_hex(ADL_TEXT_DIM), 0);
    if (f12) lv_obj_set_style_text_font(c, f12, 0);
    lv_obj_t *t = lv_label_create(g_content);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, lv_color_hex(ADL_TEXT), 0);
    if (f22) lv_obj_set_style_text_font(t, f22, 0);
}

static void s_sect(const char *txt) {
    lv_obj_t *l = lv_label_create(g_content);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(ADL_TEXT), 0);
    if (f13) lv_obj_set_style_text_font(l, f13, 0);
}

static lv_obj_t *s_gcard(void) {
    lv_obj_t *c = lv_obj_create(g_content);
    lv_obj_remove_style_all(c);
    lv_obj_set_width(c, LV_PCT(100));
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, lv_color_hex(SC_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 8, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(SC_CARDBRD), 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_pad_row(c, 0, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t *s_grow(lv_obj_t *card, int last) {
    lv_obj_t *r = lv_obj_create(card);
    lv_obj_remove_style_all(r);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, LV_SIZE_CONTENT);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_ver(r, 14, 0);
    lv_obj_set_style_pad_hor(r, 16, 0);
    lv_obj_set_style_pad_column(r, 12, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (!last) {
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(r, lv_color_hex(SC_HAIR), 0);
    }
    return r;
}

static void s_gtexts(lv_obj_t *row, const char *label, const char *sub) {
    lv_obj_t *col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(col);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, lv_color_hex(ADL_TEXT), 0);
    if (f13) lv_obj_set_style_text_font(l, f13, 0);
    if (sub && sub[0]) {
        lv_obj_t *d = lv_label_create(col);
        lv_label_set_text(d, sub);
        lv_obj_set_style_text_color(d, lv_color_hex(ADL_TEXT_DIM), 0);
        if (f12) lv_obj_set_style_text_font(d, f12, 0);
    }
}

static lv_obj_t *s_gswitch(lv_obj_t *row, const char *key, int def,
                           lv_event_cb_t cb) {
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 38, 20);
    lv_obj_set_style_radius(sw, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x3A4150), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(SC_ACC), LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_pad_all(sw, 0, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_width(sw, 12, LV_PART_KNOB);
    lv_obj_set_style_height(sw, 12, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw, 0, LV_PART_INDICATOR);
    if (arctian_settings && arctian_settings->get_int &&
        arctian_settings->get_int(key, def)) lv_obj_add_state(sw, LV_STATE_CHECKED);
    if (cb) lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, (void *)key);
    return sw;
}

static void s_theme_cb(lv_event_t *e) {
    int mode = (int)(intptr_t)lv_event_get_user_data(e); /* 0=Acik, 1=Koyu, 2=Sistem */
    int light = (mode == 0) ? 1 : 0;
    if (arctian_settings && arctian_settings->set_int) {
        arctian_settings->set_int("theme_mode", mode);
        arctian_settings->set_int("light", light);
    }
    s_page(g_page);
}

static void s_roundwin_cb(lv_event_t *e) { s_toggle_cb(e); }

static int s_fontscale(void) {
    int v = (arctian_settings && arctian_settings->get_int)
            ? arctian_settings->get_int("fontscale", 100) : 100;
    return v;
}

static void s_scale_slider_cb(lv_event_t *e) {
    static const int opts[3] = { 100, 125, 150 };
    int i = lv_slider_get_value(lv_event_get_target(e));
    if (i < 0) i = 0;
    if (i > 2) i = 2;
    if (opts[i] == s_fontscale()) return;
    if (arctian_settings && arctian_settings->set_int) {
        arctian_settings->set_int("fontscale", opts[i]);
        arctian_settings->set_int("uiscale", opts[i]);
    }
}

static void s_density_cb(lv_event_t *e) {
    (void)e;
    static const int opts[3] = { 100, 125, 150 };
    int cur = s_fontscale();
    int idx = (cur == 150) ? 2 : (cur == 125 ? 1 : 0);
    idx = (idx + 1) % 3;
    if (arctian_settings && arctian_settings->set_int) {
        arctian_settings->set_int("fontscale", opts[idx]);
        arctian_settings->set_int("uiscale", opts[idx]);
    }
}

static void s_bright_cb(lv_event_t *e) {
    int v = lv_slider_get_value(lv_event_get_target(e));
    if (v < 30) v = 30;
    if (v > 100) v = 100;
    if (arctian_settings && arctian_settings->set_int) arctian_settings->set_int("bright", v);
    if (s_asi && s_asi->power && s_asi->power->brightness_set) s_asi->power->brightness_set(v);
}

static void s_vol_cb(lv_event_t *e) {
    int v = lv_slider_get_value(lv_event_get_target(e));
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (arctian_settings && arctian_settings->set_int) arctian_settings->set_int("volume", v);
    if (s_asi && s_asi->audio && s_asi->audio->set_volume) s_asi->audio->set_volume(v);
}

static void s_toggle_cb(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    const char *key = (const char *)lv_event_get_user_data(e);
    int on = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    if (arctian_settings && arctian_settings->set_int) arctian_settings->set_int(key, on);
}

static void s_add_toggle(lv_obj_t *card, const char *label, const char *key, int def) {
    lv_obj_t *r = srow(card, 40);
    lv_obj_t *l = lv_label_create(r);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, lv_color_hex(ADL_TEXT), 0);
    if (f13) lv_obj_set_style_text_font(l, f13, 0);
    lv_obj_t *sw = lv_switch_create(r);
    if (arctian_settings && arctian_settings->get_int &&
        arctian_settings->get_int(key, def)) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, s_toggle_cb, LV_EVENT_VALUE_CHANGED, (void *)key);
}

static void s_motion_cb(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    int on = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    if (arctian_settings && arctian_settings->set_int)
        arctian_settings->set_int("motion", on ? 0 : 1);
}

static void s_add_motion_toggle(lv_obj_t *card, const char *label) {
    lv_obj_t *r = srow(card, 40);
    lv_obj_t *l = lv_label_create(r);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, lv_color_hex(ADL_TEXT), 0);
    if (f13) lv_obj_set_style_text_font(l, f13, 0);
    lv_obj_t *sw = lv_switch_create(r);
    if (arctian_settings && arctian_settings->get_int &&
        arctian_settings->get_int("motion", 1) == 0) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, s_motion_cb, LV_EVENT_VALUE_CHANGED, 0);
}

static void s_choice_text(char *out, int cap, const char *unit, int v) {
    int n = 0;
    if (v <= 0) {
        scpy(out, cap, "Kapalı");
        return;
    }
    if (v >= 10) out[n++] = (char)('0' + (v / 10) % 10);
    out[n++] = (char)('0' + v % 10);
    out[n++] = ' ';
    for (int i = 0; unit[i] && n < cap - 1; i++) out[n++] = unit[i];
    out[n] = 0;
}

static void s_idle_cycle_cb(lv_event_t *e) {
    lv_obj_t *b = lv_event_get_target(e);
    int v = (arctian_settings && arctian_settings->get_int)
            ? arctian_settings->get_int("idle_lock_min", 0) : 0;
    static const int opts[4] = {0, 5, 10, 30};
    int idx = 0;
    for (int i = 0; i < 4; i++) if (opts[i] == v) idx = i;
    idx = (idx + 1) % 4;
    if (arctian_settings && arctian_settings->set_int)
        arctian_settings->set_int("idle_lock_min", opts[idx]);
    char t[24];
    s_choice_text(t, sizeof(t), "dk", opts[idx]);
    lv_obj_t *l = lv_obj_get_child(b, 0);
    if (l) lv_label_set_text(l, t);
}

static void s_ws_cycle_cb(lv_event_t *e) {
    lv_obj_t *b = lv_event_get_target(e);
    int v = (arctian_settings && arctian_settings->get_int)
            ? arctian_settings->get_int("ws_count", 3) : 3;
    v++;
    if (v > 4) v = 1;
    if (arctian_settings && arctian_settings->set_int)
        arctian_settings->set_int("ws_count", v);
    char t[24];
    s_choice_text(t, sizeof(t), "alan", v);
    lv_obj_t *l = lv_obj_get_child(b, 0);
    if (l) lv_label_set_text(l, t);
}

static void s_kbd_cycle_cb(lv_event_t *e) {
    lv_obj_t *b = lv_event_get_target(e);
    int idx = (arctian_settings && arctian_settings->get_int)
              ? arctian_settings->get_int("kbdlayout", 0) : 0;
    idx = (idx + 1) % 3;
    static const char *names[3] = { "Türkçe Q", "Türkçe F", "İngilizce Q" };
    if (arctian_settings && arctian_settings->set_int)
        arctian_settings->set_int("kbdlayout", idx);
    lv_obj_t *l = lv_obj_get_child(b, 0);
    if (l) lv_label_set_text(l, names[idx]);
}

typedef struct { int mins; const char *label; } tz_opt_t;
static const tz_opt_t s_tz_opts[6] = {
    {  180, "UTC+3 (İstanbul)" },
    {    0, "UTC+0 (UTC)" },
    {   60, "UTC+1 (CET)" },
    {  120, "UTC+2 (EET)" },
    { -300, "UTC-5 (EST)" },
    {  540, "UTC+9 (JST)" },
};

static const char *s_tz_label(int mins) {
    for (int i = 0; i < 6; i++) if (s_tz_opts[i].mins == mins) return s_tz_opts[i].label;
    return "UTC+3 (İstanbul)";
}

static void s_tz_cycle_cb(lv_event_t *e) {
    lv_obj_t *b = lv_event_get_target(e);
    int cur = (arctian_settings && arctian_settings->get_int)
              ? arctian_settings->get_int("tz_minutes", 180) : 180;
    int idx = 0;
    for (int i = 0; i < 6; i++) if (s_tz_opts[i].mins == cur) idx = i;
    idx = (idx + 1) % 6;
    int nm = s_tz_opts[idx].mins;
    if (arctian_settings && arctian_settings->set_int)
        arctian_settings->set_int("tz_minutes", nm);
    if (s_asi && s_asi->time && s_asi->time->set_tz_offset)
        s_asi->time->set_tz_offset(nm);
    lv_obj_t *l = lv_obj_get_child(b, 0);
    if (l) lv_label_set_text(l, s_tz_opts[idx].label);
}

static void s_add_choice(lv_obj_t *card, const char *label, const char *cur,
                         lv_event_cb_t cb, int w) {
    lv_obj_t *r = srow(card, 40);
    lv_obj_t *l = lv_label_create(r);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, lv_color_hex(ADL_TEXT), 0);
    if (f13) lv_obj_set_style_text_font(l, f13, 0);
    lv_obj_t *b = lv_button_create(r);
    lv_obj_set_size(b, w > 0 ? w : 108, 32);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(ADL_SURFACE_2), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(ADL_BORDER), 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, 0);
    lv_obj_t *bl = lv_label_create(b);
    lv_label_set_text(bl, cur);
    lv_obj_set_style_text_color(bl, lv_color_hex(ADL_TEXT), 0);
    if (f12) lv_obj_set_style_text_font(bl, f12, 0);
    lv_obj_center(bl);
}

static void s_accent_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (arctian_settings && arctian_settings->accent_set) arctian_settings->accent_set(i);
    s_page(g_page);
}

static void s_wall_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s_asi || !s_asi->fs || !arctian_store || !arctian_store->set_wallpaper) return;
    const char *nm = s_asi->fs->name(idx);
    if (nm) arctian_store->set_wallpaper(nm);
    s_page(0);
}

static void s_wall_reset_cb(lv_event_t *e) {
    (void)e;
    if (arctian_store && arctian_store->set_wallpaper) arctian_store->set_wallpaper("");
    s_page(0);
}

static void s_res_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (arctian_settings && arctian_settings->resolution_set)
        arctian_settings->resolution_set(i);
}

static void s_open_cb(lv_event_t *e) {
    const char *q = (const char *)lv_event_get_user_data(e);
    arctian_shell_launch(q);
}

static void s_inst_open_cb(lv_event_t *e) {
    const char *id = (const char *)lv_event_get_user_data(e);
    if (arctian_store && arctian_store->launch) arctian_store->launch(id);
}

static void s_inst_del_cb(lv_event_t *e) {
    const char *id = (const char *)lv_event_get_user_data(e);
    if (arctian_store && arctian_store->uninstall) arctian_store->uninstall(id);
    s_page(g_page);
}

static void s_dhcp_work(void *u) {
    (void)u;
    if (s_asi && s_asi->net && s_asi->net->autoconfig) s_asi->net->autoconfig();
}

static void s_dhcp_done(int status, void *user) {
    (void)status; (void)user;
    if (g_page == 1 && !s_search_q[0]) s_page(1);
}

static void s_dhcp_cb(lv_event_t *e) {
    (void)e;
    if (s_asi && s_asi->net && s_asi->net->submit &&
        !(s_asi->net->busy && s_asi->net->busy()) &&
        s_asi->net->submit(s_dhcp_work, s_dhcp_done, 0) == 0)
        return;
    if (s_asi && s_asi->net && s_asi->net->autoconfig) s_asi->net->autoconfig();
    if (g_page == 1) s_page(1);
}

static void s_netpanel_cb(lv_event_t *e) { (void)e; arctian_shell_network_panel(); }

/* ---- Guncelleme ---- */
static void s_upd_refresh(void) {
    const update_status_t *st = update_status();
    if (g_upd_msg) lv_label_set_text(g_upd_msg, st->message);
    if (g_upd_bar) {
        if (st->state == UPDATE_DOWNLOADING) {
            lv_obj_remove_flag(g_upd_bar, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(g_upd_bar, st->progress, LV_ANIM_OFF);
        } else {
            lv_obj_add_flag(g_upd_bar, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void s_upd_timer_cb(lv_timer_t *t) {
    (void)t;
    s_upd_refresh();
    const update_status_t *st = update_status();
    if (st->state != UPDATE_CHECKING && st->state != UPDATE_DOWNLOADING) {
        if (g_upd_timer) { lv_timer_delete(g_upd_timer); g_upd_timer = 0; }
    }
}

static void s_upd_work(void *u) {
    if ((intptr_t)u) update_apply();
    else update_check();
}

static void s_upd_done(int status, void *user) { (void)status; (void)user; }

static void s_upd_start(int apply) {
    if (!s_asi || !s_asi->net || !s_asi->net->submit) {
        if (apply) update_apply(); else update_check();
        s_upd_refresh();
        return;
    }
    if (s_asi->net->busy && s_asi->net->busy()) {
        if (g_upd_msg) lv_label_set_text(g_upd_msg, "Ağ meşgul, sonra deneyin");
        return;
    }
    if (apply && !update_has_update()) {
        if (g_upd_msg) lv_label_set_text(g_upd_msg, "Önce 'Kontrol Et'");
        return;
    }
    if (s_asi->net->submit(s_upd_work, s_upd_done, (void *)(intptr_t)apply) != 0) {
        if (g_upd_msg) lv_label_set_text(g_upd_msg, "İşlem başlatılamadı");
        return;
    }
    if (g_upd_msg) lv_label_set_text(g_upd_msg, apply ? "Güncelleme başladı..." : "Kontrol ediliyor...");
    if (g_upd_timer) { lv_timer_delete(g_upd_timer); g_upd_timer = 0; }
    g_upd_timer = lv_timer_create(s_upd_timer_cb, 250, 0);
}

static void s_upd_check_cb(lv_event_t *e) { (void)e; s_upd_start(0); }

/* ---- Guvenlik duvari ---- */
static void sfw_read(lv_obj_t *ta, char *out, int cap) {
    out[0] = 0;
    if (!ta) return;
    const char *t = lv_textarea_get_text(ta);
    if (!t) return;
    int i = 0;
    for (; t[i] && i < cap - 1; i++) out[i] = t[i];
    out[i] = 0;
}

static int sfw_num(const char *s, int *out) {
    if (!s || !*s) { *out = 0; return 0; }
    int v = 0, any = 0;
    while (*s >= '0' && *s <= '9') {
        if (v < 1000000) v = v * 10 + (*s - '0');
        if (v > 65535) v = 65536;
        s++; any = 1;
    }
    *out = v;
    return any;
}

static void s_fw_toggle_cb(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    int on = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    if (s_asi && s_asi->firewall && s_asi->firewall->set_enabled)
        s_asi->firewall->set_enabled(on);
    scpy(s_fw_msg, sizeof(s_fw_msg), on ? "Güvenlik duvarı açık." : "Güvenlik duvarı kapalı.");
    s_page(3);
}

static void s_fw_add_cb(lv_event_t *e) {
    (void)e;
    s_fw_msg[0] = 0;
    asi_firewall_t *fw = s_asi ? s_asi->firewall : 0;
    if (!fw || !fw->add) { scpy(s_fw_msg, sizeof(s_fw_msg), "Güvenlik duvarı servisi yok"); s_page(3); return; }

    asi_fw_rule_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((uint8_t *)&r)[i] = 0;
    sfw_read(g_fw_name, r.name, ASI_FW_NAME_MAX);
    if (!r.name[0]) scpy(r.name, ASI_FW_NAME_MAX, "kural");
    r.enabled = 1;
    r.dir = (uint8_t)(lv_dropdown_get_selected(g_fw_dir) + 1);
    int pi = lv_dropdown_get_selected(g_fw_proto);
    r.proto = (pi == 0) ? ASI_FW_PROTO_ANY : (pi == 1 ? ASI_FW_PROTO_TCP : ASI_FW_PROTO_UDP);
    r.action = (lv_dropdown_get_selected(g_fw_action) == 0) ? ASI_FW_DENY : ASI_FW_ALLOW;

    char pt[24];
    sfw_read(g_fw_port, pt, sizeof(pt));
    int lo = 0, hi = 0, dash = -1;
    for (int i = 0; pt[i]; i++) if (pt[i] == '-') { dash = i; break; }
    if (dash >= 0) {
        pt[dash] = 0;
        sfw_num(pt, &lo);
        sfw_num(pt + dash + 1, &hi);
    } else {
        sfw_num(pt, &lo);
        hi = lo;
    }
    if (lo < 0) lo = 0;
    if (lo > 65535) lo = 65535;
    if (hi < 0) hi = 0;
    if (hi > 65535) hi = 65535;
    r.port_lo = (uint16_t)lo;
    r.port_hi = (uint16_t)hi;

    int rc = fw->add(&r);
    scpy(s_fw_msg, sizeof(s_fw_msg), rc < 0 ? "Kural eklenemedi (liste dolu?)" : "Kural eklendi.");
    s_page(3);
}

static void s_fw_del_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_asi && s_asi->firewall && s_asi->firewall->remove) s_asi->firewall->remove(idx);
    scpy(s_fw_msg, sizeof(s_fw_msg), "Kural kaldırıldı.");
    s_page(3);
}

static void s_page_firewall(lv_obj_t *c) {
    asi_firewall_t *fw = s_asi ? s_asi->firewall : 0;
    if (!fw) { sdesc(c, "Güvenlik duvarı servisi yok."); return; }

    lv_obj_t *tr = srow(c, 40);
    lv_obj_t *tl = lv_label_create(tr);
    lv_label_set_text(tl, "Etkin");
    lv_obj_set_style_text_color(tl, lv_color_hex(ADL_TEXT), 0);
    lv_obj_t *sw = lv_switch_create(tr);
    if (fw->enabled && fw->enabled()) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, s_fw_toggle_cb, LV_EVENT_VALUE_CHANGED, 0);

    int n = fw->count ? fw->count() : 0;
    if (n <= 0) {
        sdesc(c, "Henüz kural yok. Kural eşleşmezse trafik izinlidir.");
    } else {
        for (int i = 0; i < n; i++) {
            asi_fw_rule_t r;
            if (!fw->rule || fw->rule(i, &r) != 0) continue;
            lv_obj_t *row = srow_start(c, 34);
            char txt[96];
            scpy(txt, sizeof(txt), r.name[0] ? r.name : "kural");
            scat(txt, sizeof(txt), "  ");
            scat(txt, sizeof(txt), r.dir == ASI_FW_DIR_IN ? "gelen" :
                                     r.dir == ASI_FW_DIR_OUT ? "giden" : "gelen/giden");
            scat(txt, sizeof(txt), " ");
            scat(txt, sizeof(txt), r.proto == ASI_FW_PROTO_TCP ? "TCP" :
                                     r.proto == ASI_FW_PROTO_UDP ? "UDP" : "hepsi");
            scat(txt, sizeof(txt), " ");
            if (r.port_lo || r.port_hi) {
                char a[8];
                snum(a, sizeof(a), r.port_lo);
                scat(txt, sizeof(txt), ":");
                scat(txt, sizeof(txt), a);
                if (r.port_hi && r.port_hi != r.port_lo) {
                    char b[8];
                    snum(b, sizeof(b), r.port_hi);
                    scat(txt, sizeof(txt), "-");
                    scat(txt, sizeof(txt), b);
                }
            } else {
                scat(txt, sizeof(txt), ":*");
            }
            scat(txt, sizeof(txt), r.action == ASI_FW_ALLOW ? "  izin" : "  engel");
            lv_obj_t *l = lv_label_create(row);
            lv_label_set_text(l, txt);
            lv_obj_set_style_text_color(l, lv_color_hex(ADL_TEXT), 0);
            lv_obj_t *bd = sbutton(row, "Kaldır", ADL_SURFACE_2, ADL_DANGER, 76, 30);
            lv_obj_add_event_cb(bd, s_fw_del_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
    }

    stitle(c, "Yeni Kural");
    lv_obj_t *f1 = srow_start(c, 44);
    g_fw_name = lv_textarea_create(f1);
    lv_obj_set_size(g_fw_name, 140, 38);
    lv_textarea_set_one_line(g_fw_name, true);
    lv_textarea_set_max_length(g_fw_name, ASI_FW_NAME_MAX - 1);
    lv_textarea_set_placeholder_text(g_fw_name, "Ad");

    g_fw_dir = lv_dropdown_create(f1);
    lv_dropdown_set_options(g_fw_dir, "Gelen\nGiden\nHer ikisi");
    lv_obj_set_width(g_fw_dir, 120);

    g_fw_proto = lv_dropdown_create(f1);
    lv_dropdown_set_options(g_fw_proto, "Hepsi\nTCP\nUDP");
    lv_obj_set_width(g_fw_proto, 100);

    g_fw_action = lv_dropdown_create(f1);
    lv_dropdown_set_options(g_fw_action, "Engelle\nİzin ver");
    lv_obj_set_width(g_fw_action, 110);

    g_fw_port = lv_textarea_create(f1);
    lv_obj_set_size(g_fw_port, 120, 38);
    lv_textarea_set_one_line(g_fw_port, true);
    lv_textarea_set_max_length(g_fw_port, 12);
    lv_textarea_set_placeholder_text(g_fw_port, "Port (80)");

    lv_obj_t *addb = sbutton(c, "Kural Ekle", ADL_ACCENT, 0xFFFFFF, 140, 36);
    lv_obj_add_event_cb(addb, s_fw_add_cb, LV_EVENT_CLICKED, 0);
    if (s_fw_msg[0]) sdesc(c, s_fw_msg);
}

static void s_crypt_focus(void) {
    if (!g_crypt_ta) return;
    if (!g_crypt_group) g_crypt_group = lv_group_create();
    lv_group_remove_all_objs(g_crypt_group);
    lv_group_add_obj(g_crypt_group, g_crypt_ta);
    lv_indev_t *kp = lv_port_get_keypad();
    if (kp) lv_indev_set_group(kp, g_crypt_group);
    lv_group_focus_obj(g_crypt_ta);
}

static void read_crypt_pw(char *out, int cap) {
    out[0] = 0;
    if (!g_crypt_ta) return;
    const char *t = lv_textarea_get_text(g_crypt_ta);
    if (!t) return;
    int i = 0;
    for (; t[i] && i < cap - 1; i++) out[i] = t[i];
    out[i] = 0;
}

static void s_crypt_setup_cb(lv_event_t *e) {
    (void)e;
    char p[40];
    read_crypt_pw(p, sizeof(p));
    s_crypt_msg[0] = 0;
    if (!s_asi || !s_asi->crypt || !s_asi->crypt->setup) { scpy(s_crypt_msg, sizeof(s_crypt_msg), "Şifreleme servisi yok"); s_page(3); return; }
    int r = s_asi->crypt->setup(p);
    if (r == 0) scpy(s_crypt_msg, sizeof(s_crypt_msg), "Disk şifrelendi. Yeniden başlatınca parola sorulur.");
    else if (r == -2) scpy(s_crypt_msg, sizeof(s_crypt_msg), "Parola en az 4 karakter olmalı.");
    else scpy(s_crypt_msg, sizeof(s_crypt_msg), "Şifreleme başlatılamadı.");
    s_page(3);
}

static void s_crypt_disable_cb(lv_event_t *e) {
    (void)e;
    char p[40];
    read_crypt_pw(p, sizeof(p));
    s_crypt_msg[0] = 0;
    if (!s_asi || !s_asi->crypt || !s_asi->crypt->disable) { scpy(s_crypt_msg, sizeof(s_crypt_msg), "Şifreleme servisi yok"); s_page(3); return; }
    int r = s_asi->crypt->disable(p);
    if (r == 0) scpy(s_crypt_msg, sizeof(s_crypt_msg), "Şifreleme kaldırıldı.");
    else if (r == -4) scpy(s_crypt_msg, sizeof(s_crypt_msg), "Parola hatalı.");
    else scpy(s_crypt_msg, sizeof(s_crypt_msg), "İşlem başarısız.");
    s_page(3);
}

static void s_reboot_cb(lv_event_t *e) { (void)e; if (s_asi && s_asi->system && s_asi->system->reboot) s_asi->system->reboot(); }
static void s_shutdown_cb(lv_event_t *e) { (void)e; if (s_asi && s_asi->system && s_asi->system->shutdown) s_asi->system->shutdown(); }
static void s_power_cb(lv_event_t *e) { (void)e; arctian_shell_power_dialog(); }

static lv_obj_t *s_mk_slider(lv_obj_t *parent, int minv, int maxv, int val, lv_event_cb_t cb) {
    lv_obj_t *sl = lv_slider_create(parent);
    lv_obj_set_size(sl, 150, 6);
    lv_slider_set_range(sl, minv, maxv);
    lv_slider_set_value(sl, val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0x2C313B), LV_PART_MAIN);
    lv_obj_set_style_radius(sl, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(SC_ACC), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 4, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sl, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, 0);
    return sl;
}

static void s_page_view(void) {
    s_phead("Sistem \xe2\x80\xba Görünüm", "Görünüm");

    int light = (arctian_settings && arctian_settings->get_int)
                ? arctian_settings->get_int("light", 0) : 0;
    int tmode = (arctian_settings && arctian_settings->get_int)
                ? arctian_settings->get_int("theme_mode", -1) : -1;
    int sel = (tmode >= 0 && tmode <= 2) ? tmode : (light ? 0 : 1);

    s_sect("Uygulama teması");

    lv_obj_t *tr = lv_obj_create(g_content);
    lv_obj_remove_style_all(tr);
    lv_obj_set_width(tr, LV_PCT(100));
    lv_obj_set_height(tr, LV_SIZE_CONTENT);
    lv_obj_remove_flag(tr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_column(tr, 12, 0);
    lv_obj_set_flex_flow(tr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    static const uint32_t cardbg[3] = { 0xEEF1F5, 0x0F1115, 0xEEF1F5 };
    static const char *const cardnm[3] = { "Açık", "Koyu", "Sistem" };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *col = lv_obj_create(tr);
        lv_obj_remove_style_all(col);
        lv_obj_set_height(col, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(col, 1);
        lv_obj_set_style_pad_row(col, 6, 0);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(col, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(col, s_theme_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *pv = lv_obj_create(col);
        lv_obj_remove_style_all(pv);
        lv_obj_set_size(pv, LV_PCT(100), 76);
        lv_obj_set_style_radius(pv, 8, 0);
        lv_obj_set_style_bg_color(pv, lv_color_hex(cardbg[i]), 0);
        lv_obj_set_style_bg_opa(pv, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(pv, (i == sel) ? 2 : 1, 0);
        lv_obj_set_style_border_color(pv, lv_color_hex((i == sel) ? SC_ACC : 0x2C313B), 0);
        lv_obj_set_style_pad_all(pv, 8, 0);
        lv_obj_remove_flag(pv, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(pv, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(pv, s_theme_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        if (i == 2) {
            lv_obj_t *half = lv_obj_create(pv);
            lv_obj_remove_style_all(half);
            lv_obj_set_size(half, LV_PCT(50), LV_PCT(100));
            lv_obj_align(half, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_obj_set_style_bg_color(half, lv_color_hex(0x0F1115), 0);
            lv_obj_set_style_bg_opa(half, LV_OPA_COVER, 0);
        } else {
            lv_obj_t *bar = lv_obj_create(pv);
            lv_obj_remove_style_all(bar);
            lv_obj_set_size(bar, LV_PCT(40), 8);
            lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
            lv_obj_set_style_radius(bar, 3, 0);
            lv_obj_set_style_bg_color(bar, lv_color_hex(i == 0 ? 0xCFD6E2 : 0x2A2E37), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        }

        lv_obj_t *l = lv_label_create(col);
        char lt[32];
        scpy(lt, sizeof(lt), cardnm[i]);
        if (i == sel) scat(lt, sizeof(lt), " \xc2\xb7 Seçili");
        lv_label_set_text(l, lt);
        lv_obj_set_style_text_color(l, lv_color_hex(i == sel ? ADL_TEXT : ADL_TEXT_DIM), 0);
        if (f13) lv_obj_set_style_text_font(l, f13, 0);
    }

    lv_obj_t *gc = s_gcard();

    {
        lv_obj_t *r = s_grow(gc, 0);
        s_gtexts(r, "Vurgu rengi", "Seçimler ve düğmeler için");
        lv_obj_t *wrap = lv_obj_create(r);
        lv_obj_remove_style_all(wrap);
        lv_obj_set_height(wrap, 22);
        lv_obj_set_width(wrap, LV_SIZE_CONTENT);
        lv_obj_remove_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_column(wrap, 8, 0);
        lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        int n = (arctian_settings && arctian_settings->accent_count) ? arctian_settings->accent_count() : 0;
        int cur = (arctian_settings && arctian_settings->accent_current) ? arctian_settings->accent_current() : 0;
        for (int i = 0; i < n; i++) {
            lv_obj_t *b = lv_button_create(wrap);
            lv_obj_set_size(b, 22, 22);
            lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(b, (i == cur) ? 2 : 0, 0);
            lv_obj_set_style_border_color(b, lv_color_hex(0xE8EAEE), 0);
            lv_obj_set_style_shadow_width(b, 0, 0);
            lv_obj_set_style_pad_all(b, 0, 0);
            lv_obj_add_event_cb(b, s_accent_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_t *dot = lv_obj_create(b);
            lv_obj_remove_style_all(dot);
            lv_obj_set_size(dot, 18, 18);
            lv_obj_center(dot);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(dot, lv_color_hex(arctian_settings->accent_color(i)), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    {
        lv_obj_t *r = s_grow(gc, 0);
        s_gtexts(r, "Yuvarlak pencere köşeleri", "Pencere kenarlarında yumuşak köşeler");
        s_gswitch(r, "roundwin", 1, s_roundwin_cb);
    }

    {
        lv_obj_t *r = s_grow(gc, 0);
        s_gtexts(r, "Yazı boyutu", "Sistem genelindeki metinler");
        int fs = s_fontscale();
        s_mk_slider(r, 0, 2, fs >= 150 ? 2 : (fs >= 125 ? 1 : 0), s_scale_slider_cb);
    }

    {
        lv_obj_t *r = s_grow(gc, 1);
        s_gtexts(r, "Yoğunluk", "Liste satırlarının yüksekliği");
        int fs = s_fontscale();
        const char *dn = fs >= 150 ? "Sıkı" : (fs >= 125 ? "Orta" : "Rahat");
        lv_obj_t *b = lv_button_create(r);
        lv_obj_set_size(b, LV_SIZE_CONTENT, 30);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(SC_INP), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(SC_BTNBRD), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_hor(b, 12, 0);
        lv_obj_set_style_pad_column(b, 20, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *dl = lv_label_create(b);
        lv_label_set_text(dl, dn);
        lv_obj_set_style_text_color(dl, lv_color_hex(ADL_TEXT), 0);
        if (f13) lv_obj_set_style_text_font(dl, f13, 0);
        s_svg_icon(b, "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#7D8491\" stroke-width=\"2\"><path d=\"M6 9l6 6 6-6\"/></svg>", 14);
        lv_obj_add_event_cb(b, s_density_cb, LV_EVENT_CLICKED, 0);
    }

    /* Ses ve Ekran Parlakligi */
    lv_obj_t *c_av = s_gcard();
    {
        lv_obj_t *r = s_grow(c_av, 0);
        s_gtexts(r, "Ekran Parlaklığı", "Arka ışık ve karartma düzeyi (%30–%100)");
        int br = (arctian_settings && arctian_settings->get_int)
                 ? arctian_settings->get_int("bright", 100) : 100;
        if (s_asi && s_asi->power && s_asi->power->brightness_get) {
            int hb = s_asi->power->brightness_get();
            if (hb >= 30 && hb <= 100) br = hb;
        }
        s_mk_slider(r, 30, 100, br, s_bright_cb);
    }
    {
        lv_obj_t *r = s_grow(c_av, 1);
        s_gtexts(r, "Ses Seviyesi", "Sistem ses çıkışı (%0–%100)");
        int vol = (arctian_settings && arctian_settings->get_int)
                  ? arctian_settings->get_int("volume", 80) : 80;
        if (s_asi && s_asi->audio && s_asi->audio->volume) {
            int av = s_asi->audio->volume();
            if (av >= 0 && av <= 100) vol = av;
        }
        s_mk_slider(r, 0, 100, vol, s_vol_cb);
    }

    lv_obj_t *c2 = scard(g_content);
    stitle(c2, "Erişilebilirlik");
    sdesc(c2, "Yazı ölçeği yukarıdaki kaydır çubuğuyla ayarlanır.");
    s_add_toggle(c2, "Yüksek kontrast", "contrast", 0);
    s_add_toggle(c2, "Klavye odak halkası", "focusring", 0);
    s_add_toggle(c2, "Büyük simgeler", "bigicons", 0);
    s_add_motion_toggle(c2, "Hareketi azalt");

    lv_obj_t *c3 = scard(g_content);
    stitle(c3, "Arka plan");
    sdesc(c3, "AFS'te kurulu .wp görselleri. Yenilerini DuckStore'dan indirebilirsiniz.");
    {
        const char *active = (arctian_store && arctian_store->wallpaper_active && arctian_store->wallpaper_active())
                             ? arctian_store->wallpaper_name() : "";
        int found = 0;
        if (s_asi && s_asi->fs && s_asi->fs->count) {
            int fc = s_asi->fs->count();
            for (int i = 0; i < fc; i++) {
                const char *nm = s_asi->fs->name(i);
                int L = slen(nm);
                if (L <= 3 || nm[L - 3] != '.' || nm[L - 2] != 'w' || nm[L - 1] != 'p') continue;
                found = 1;
                lv_obj_t *r = srow_start(c3, 36);
                lv_obj_t *l = lv_label_create(r);
                char txt[48];
                scpy(txt, sizeof(txt), nm);
                if (active[0] && seq(active, nm)) scat(txt, sizeof(txt), "  (aktif)");
                lv_label_set_text(l, txt);
                lv_obj_set_style_text_color(l, lv_color_hex(ADL_TEXT), 0);
                lv_obj_t *b = sbutton(r, "Uygula", SC_ACC, 0xFFFFFF, 90, 32);
                lv_obj_add_event_cb(b, s_wall_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            }
        }
        if (!found) sdesc(c3, "Henüz .wp görseli yok.");
        lv_obj_t *rb = sbutton(c3, "Varsayılan Arka Plan", 0x1E2129, ADL_TEXT, 190, 36);
        lv_obj_add_event_cb(rb, s_wall_reset_cb, LV_EVENT_CLICKED, 0);
    }

    lv_obj_t *c4 = scard(g_content);
    stitle(c4, "Çözünürlük");
    sdesc(c4, "Değişiklik anında uygulanır; açık pencereler kapanır.");
    {
        int n   = (arctian_settings && arctian_settings->resolution_count) ? arctian_settings->resolution_count() : 0;
        int cur = (arctian_settings && arctian_settings->resolution_current) ? arctian_settings->resolution_current() : -1;
        if (n <= 0) sdesc(c4, "Çözünürlük servisi bulunamadı.");
        for (int i = 0; i < n; i++) {
            lv_obj_t *r = srow(c4, 42);
            lv_obj_t *l = lv_label_create(r);
            lv_label_set_text(l, arctian_settings->resolution_name(i));
            lv_obj_set_style_text_color(l, lv_color_hex(i == cur ? SC_ACC : ADL_TEXT), 0);
            if (i == cur) {
                lv_obj_t *b = sbutton(r, "Aktif", 0x1E2129, SC_ACC, 96, 34);
                lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
            } else {
                lv_obj_t *b = sbutton(r, "Uygula", SC_ACC, 0xFFFFFF, 96, 34);
                lv_obj_add_event_cb(b, s_res_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            }
        }
    }
}

static void s_page_notify(void) {
    s_phead("Sistem \xe2\x80\xba Bildirimler", "Bildirimler");
    lv_obj_t *c = scard(g_content);
    stitle(c, "Görev Çubuğu ve Saat");
    s_add_toggle(c, "Masaüstü kısayollarını göster", "dicons", 1);
    s_add_toggle(c, "Görev çubuğunda saati göster", "clockshow", 1);
    s_add_toggle(c, "24 saat biçimi", "clock24", 1);
    s_add_toggle(c, "Boştayken kilitle", "idle_lock", 0);

    {
        int tz = (arctian_settings && arctian_settings->get_int)
                 ? arctian_settings->get_int("tz_minutes", 180) : 180;
        s_add_choice(c, "Saat Dilimi", s_tz_label(tz), s_tz_cycle_cb, 152);

        int mi = (arctian_settings && arctian_settings->get_int)
                 ? arctian_settings->get_int("idle_lock_min", 0) : 0;
        char t[24];
        s_choice_text(t, sizeof(t), "dk", mi);
        s_add_choice(c, "Boşta kilit süresi", t, s_idle_cycle_cb, 108);

        int ws = (arctian_settings && arctian_settings->get_int)
                 ? arctian_settings->get_int("ws_count", 3) : 3;
        s_choice_text(t, sizeof(t), "alan", ws);
        s_add_choice(c, "Çalışma alanı sayısı", t, s_ws_cycle_cb, 108);

        const char *kname = "Türkçe Q";
        if (arctian_settings && arctian_settings->get_int) {
            int ki = arctian_settings->get_int("kbdlayout", 0);
            kname = (ki == 2) ? "İngilizce Q" : (ki == 1 ? "Türkçe F" : "Türkçe Q");
        }
        s_add_choice(c, "Klavye düzeni", kname, s_kbd_cycle_cb, 116);
    }
}

static void s_page_network(void) {
    char v[48];
    s_phead("Sistem \xe2\x80\xba Ağ ve internet", "Ağ ve internet");
    lv_obj_t *c = scard(g_content);
    stitle(c, "Bağlantı Durumu");
    asi_net_t *n = (s_asi ? s_asi->net : 0);
    int lt = (n && n->link_type) ? n->link_type() : 0;
    skv(c, "Bağlantı", lt == ASI_LINK_ETHERNET ? "Ethernet" : lt == ASI_LINK_WIFI ? "Wi-Fi" : "yok");
    uint8_t ip[4];
    if (n && n->ip) { n->ip(ip); sip(v, sizeof(v), ip); skv(c, "IP Adresi", v); }
    if (n && n->netmask) { n->netmask(ip); sip(v, sizeof(v), ip); skv(c, "Alt Ağ Maskesi", v); }
    if (n && n->gateway) { n->gateway(ip); sip(v, sizeof(v), ip); skv(c, "Geçit", v); }
    if (n && n->dns) { n->dns(ip); sip(v, sizeof(v), ip); skv(c, "DNS", v); }
    if (n && n->wifi_state) {
        int ws = n->wifi_state();
        const char *st = ws == ASI_WIFI_CONNECTED ? "bağlı" :
                         ws == ASI_WIFI_CONNECTING ? "bağlanıyor" :
                         ws == ASI_WIFI_FAILED ? "başarısız" : "kapalı";
        skv(c, "Wi-Fi", st);
    }

    lv_obj_t *c2 = scard(g_content);
    stitle(c2, "İşlemler");
    sdesc(c2, "IP adresini DHCP ile yenile veya kablosuz ağları yönet.");
    lv_obj_t *r = srow_start(c2, 44);
    lv_obj_t *b1 = sbutton(r, "IP Yenile (DHCP)", SC_ACC, 0xFFFFFF, 180, 38);
    lv_obj_add_event_cb(b1, s_dhcp_cb, LV_EVENT_CLICKED, 0);
    lv_obj_t *b2 = sbutton(r, "Ağ Paneli", 0x1E2129, ADL_TEXT, 150, 38);
    lv_obj_add_event_cb(b2, s_netpanel_cb, LV_EVENT_CLICKED, 0);
}

static void s_apps_card(void) {
    lv_obj_t *c = scard(g_content);
    stitle(c, "Uygulamalar");
    sdesc(c, "Sistemle gelen uygulamalar. Açmak için dokun.");
    int n = arctian_shell_app_count();
    for (int i = 0; i < n; i++) {
        char title[48], name[32], cat[32];
        if (arctian_shell_app_info(i, title, sizeof(title), name, sizeof(name), cat, sizeof(cat)) != 0) continue;
        lv_obj_t *r = srow_start(c, 38);
        lv_obj_t *l = lv_label_create(r);
        char txt[80];
        scpy(txt, sizeof(txt), title);
        scat(txt, sizeof(txt), "   [");
        scat(txt, sizeof(txt), cat);
        scat(txt, sizeof(txt), "]");
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_color(l, lv_color_hex(ADL_TEXT), 0);
        lv_obj_t *b = sbutton(r, "Aç", ADL_ACCENT, 0xFFFFFF, 70, 32);
        lv_obj_add_event_cb(b, s_open_cb, LV_EVENT_CLICKED, (void *)spool(name));
    }

    lv_obj_t *c2 = scard(g_content);
    stitle(c2, "Kurulu Paketler");
    int ic = (arctian_store && arctian_store->count_installed) ? arctian_store->count_installed() : 0;
    if (ic <= 0) {
        sdesc(c2, "Henüz ek paket kurulmadı. DuckStore'dan uygulama kurabilirsiniz.");
    } else {
        for (int i = 0; i < ic; i++) {
            const char *id = arctian_store->installed_id(i);
            const char *title = arctian_store->installed_title(i);
            const char *ver = arctian_store->installed_version_at(i);
            lv_obj_t *r = srow_start(c2, 38);
            lv_obj_t *l = lv_label_create(r);
            char txt[80];
            scpy(txt, sizeof(txt), title && title[0] ? title : id);
            scat(txt, sizeof(txt), "  ");
            scat(txt, sizeof(txt), ver ? ver : "");
            lv_label_set_text(l, txt);
            lv_obj_set_style_text_color(l, lv_color_hex(ADL_TEXT), 0);
            lv_obj_t *bo = sbutton(r, "Aç", ADL_ACCENT, 0xFFFFFF, 60, 32);
            lv_obj_add_event_cb(bo, s_inst_open_cb, LV_EVENT_CLICKED, (void *)spool(id));
            lv_obj_t *bd = sbutton(r, "Kaldır", ADL_SURFACE_2, ADL_DANGER, 76, 32);
            lv_obj_add_event_cb(bd, s_inst_del_cb, LV_EVENT_CLICKED, (void *)spool(id));
        }
    }
}

static void s_page_privacy(void) {
    s_phead("Sistem \xe2\x80\xba Gizlilik ve güvenlik", "Gizlilik ve güvenlik");

    lv_obj_t *c3 = scard(g_content);
    stitle(c3, "Güvenlik Duvarı");
    sdesc(c3, "İstenmeyen portları kapatmak için kural tanımlayın. Eşleşmeyen trafik izinlidir.");
    s_page_firewall(c3);

    lv_obj_t *c4 = scard(g_content);
    stitle(c4, "Disk Şifreleme");
    asi_crypt_t *cr = s_asi ? s_asi->crypt : 0;
    int present = (cr && cr->present) ? cr->present() : 0;
    if (present) {
        sdesc(c4, "Disk AES-XTS ile şifreli. Açılışta parola sorulur. Kaldırmak için parolayı girin.");
        skv(c4, "Durum", (cr && cr->status) ? cr->status() : "Açık");
        lv_obj_t *row = srow_start(c4, 52);
        g_crypt_ta = lv_textarea_create(row);
        lv_obj_set_size(g_crypt_ta, 240, 40);
        lv_textarea_set_one_line(g_crypt_ta, true);
        lv_textarea_set_password_mode(g_crypt_ta, true);
        lv_textarea_set_max_length(g_crypt_ta, 31);
        lv_textarea_set_placeholder_text(g_crypt_ta, "Parola");
        lv_obj_t *b = sbutton(row, "Kaldır", ADL_DANGER, 0xFFFFFF, 110, 40);
        lv_obj_add_event_cb(b, s_crypt_disable_cb, LV_EVENT_CLICKED, 0);
    } else {
        sdesc(c4, "Diski AES-XTS ile şifreleyin. Bir parola belirleyin; sonraki açılışlarda parola sorulur.");
        lv_obj_t *row = srow_start(c4, 52);
        g_crypt_ta = lv_textarea_create(row);
        lv_obj_set_size(g_crypt_ta, 240, 40);
        lv_textarea_set_one_line(g_crypt_ta, true);
        lv_textarea_set_password_mode(g_crypt_ta, true);
        lv_textarea_set_max_length(g_crypt_ta, 31);
        lv_textarea_set_placeholder_text(g_crypt_ta, "Yeni parola (min 4)");
        lv_obj_t *b = sbutton(row, "Şifrele", SC_ACC, 0xFFFFFF, 110, 40);
        lv_obj_add_event_cb(b, s_crypt_setup_cb, LV_EVENT_CLICKED, 0);
    }
    if (s_crypt_msg[0]) sdesc(c4, s_crypt_msg);
    s_crypt_focus();
}

static void s_page_about(void) {
    char v[48];
    s_phead("Sistem \xe2\x80\xba Hakkında", "Hakkında");

    lv_obj_t *c = scard(g_content);
    stitle(c, "Arctian OS");
    sdesc(c, "Beyaz Kurt temalı, terminal ile yönetilebilen masaüstü işletim sistemi.");
    skv(c, "Sürüm", update_current_version());
    skv(c, "Arayüz", "LVGL + ADL-Wolf tema");
    skv(c, "Kabuk", "arctian> (terminal)");
    if (s_asi && s_asi->gfx) {
        char res[24], h[12];
        snum(res, sizeof(res), (unsigned int)s_asi->gfx->width());
        scat(res, sizeof(res), " x ");
        snum(h, sizeof(h), (unsigned int)s_asi->gfx->height());
        scat(res, sizeof(res), h);
        skv(c, "Ekran", res);
    }
    snum(v, sizeof(v), (unsigned int)arctian_shell_app_count());
    skv(c, "Uygulama Sayısı", v);
    snum(v, sizeof(v), (unsigned int)arctian_shell_window_count());
    skv(c, "Açık Pencere", v);
    snum(v, sizeof(v), (unsigned int)((arctian_store && arctian_store->count_installed) ? arctian_store->count_installed() : 0));
    skv(c, "Kurulu Paket", v);
    if (s_asi && s_asi->time && s_asi->time->now) {
        asi_datetime_t dt;
        if (s_asi->time->now(&dt)) {
            char d[32], t[8];
            snum(d, sizeof(d), (unsigned int)dt.year);
            scat(d, sizeof(d), "-");
            snum(t, sizeof(t), (unsigned int)dt.month); if (dt.month < 10) { t[1] = t[0]; t[0] = '0'; t[2] = 0; }
            scat(d, sizeof(d), t);
            scat(d, sizeof(d), "-");
            snum(t, sizeof(t), (unsigned int)dt.day); if (dt.day < 10) { t[1] = t[0]; t[0] = '0'; t[2] = 0; }
            scat(d, sizeof(d), t);
            skv(c, "Tarih", d);
        }
    }

    lv_obj_t *c2 = scard(g_content);
    stitle(c2, "Güç");
    if (s_asi && s_asi->power) {
        asi_power_t *pw = s_asi->power;
        int present = pw->present ? pw->present() : 0;
        int pct = pw->percent ? pw->percent() : -1;
        int src = pw->source ? pw->source() : ASI_POWER_AC;
        int chg = pw->charging ? pw->charging() : 0;
        char pb[16];
        if (pct >= 0) { snum(pb, sizeof(pb), (unsigned int)pct); scat(pb, sizeof(pb), "%"); }
        else scpy(pb, sizeof(pb), "--");
        skv(c2, "Güç Kaynağı", src == ASI_POWER_BATTERY ? "Pil" : "AC");
        skv(c2, "Pil", present ? pb : "Yok (AC)");
        if (present) skv(c2, "Şarj", chg ? "Şarj oluyor" : "Deşarj");
        skv(c2, "ACPI", (pw->acpi && pw->acpi()) ? "Var" : "Yok");
    }
    lv_obj_t *r = srow_start(c2, 46);
    lv_obj_t *b1 = sbutton(r, "Yeniden Başlat", SC_ACC, 0xFFFFFF, 170, 40);
    lv_obj_add_event_cb(b1, s_reboot_cb, LV_EVENT_CLICKED, 0);
    lv_obj_t *b2 = sbutton(r, "Kapat", ADL_DANGER, 0xFFFFFF, 120, 40);
    lv_obj_add_event_cb(b2, s_shutdown_cb, LV_EVENT_CLICKED, 0);
    lv_obj_t *b3 = sbutton(r, "Güç Menüsü", 0x1E2129, ADL_TEXT, 140, 40);
    lv_obj_add_event_cb(b3, s_power_cb, LV_EVENT_CLICKED, 0);

    s_apps_card();

    lv_obj_t *c3 = scard(g_content);
    stitle(c3, "Klavye Kısayolları");
    skv(c3, "Alt+Tab", "Pencere değiştir");
    skv(c3, "Ctrl+Alt+T", "Terminal aç");
    skv(c3, "Ctrl+Alt+A", "Asistan paneli");
    skv(c3, "Ctrl+Alt+F", "FPS göstergesi");
    skv(c3, "Ctrl+Alt+, / .", "Önceki / sonraki çalışma alanı");
    skv(c3, "Ctrl+C / X / V", "Kopyala / kes / yapıştır");
}

/* ---- Guncelleme sekmesi ---- */
static lv_obj_t *g_upd_plan_lbl;
static lv_obj_t *g_upd_delay_lbl;
static lv_obj_t *g_upd_auto_lbl;

static int s_upd_get(const char *k, int def) {
    return (arctian_settings && arctian_settings->get_int)
           ? arctian_settings->get_int(k, def) : def;
}

static void s_upd_set(const char *k, int v) {
    if (arctian_settings && arctian_settings->set_int)
        arctian_settings->set_int(k, v);
}

static void s_upd_plan_refresh(void) {
    if (!g_upd_plan_lbl) return;
    const update_manifest_t *m = update_manifest();
    int delay = s_upd_get("update_delay_days", 0);
    char b[180];
    scpy(b, sizeof(b), "");
    if (!m->version[0]) {
        scpy(b, sizeof(b), "Sunucu bilgisi yok. Önce 'Kontrol Et'.");
    } else if (m->released) {
        uint64_t need = (uint64_t)m->released + (uint64_t)delay * 86400ull;
        uint64_t now = 0;
        if (s_asi && s_asi->time && s_asi->time->unix_now) now = s_asi->time->unix_now();
        char nb[8];
        scpy(b, sizeof(b), "Sunucu: "); scat(b, sizeof(b), m->version);
        scat(b, sizeof(b), " | gecikme: "); snum(nb, sizeof(nb), (unsigned)delay);
        scat(b, sizeof(b), nb); scat(b, sizeof(b), " gün | ");
        if (!now) scat(b, sizeof(b), "saat yok");
        else if (now >= need) scat(b, sizeof(b), "kurulabilir");
        else {
            uint64_t kalan = (need - now + 86399) / 86400;
            scat(b, sizeof(b), "yaklaşık "); snum(nb, sizeof(nb), (unsigned)kalan);
            scat(b, sizeof(b), nb); scat(b, sizeof(b), " gün sonra kurulur");
        }
    } else {
        scpy(b, sizeof(b), "Sunucu sürümü: "); scat(b, sizeof(b), m->version);
    }
    lv_label_set_text(g_upd_plan_lbl, b);
    if (g_upd_auto_lbl) {
        lv_label_set_text(g_upd_auto_lbl,
            s_upd_get("update_auto", 1) ? "Otomatik güncellemeler: Açık"
                                        : "Otomatik güncellemeler: Kapalı");
    }
    if (g_upd_delay_lbl) {
        char d[32], nb[8];
        snum(nb, sizeof(nb), (unsigned)delay);
        scpy(d, sizeof(d), "Gecikme: "); scat(d, sizeof(d), nb); scat(d, sizeof(d), " gün");
        lv_label_set_text(g_upd_delay_lbl, d);
    }
}

static void s_upd_delay_cb(lv_event_t *e) {
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    int v = s_upd_get("update_delay_days", 0) + delta;
    if (v < 0) v = 0;
    if (v > 30) v = 30;
    s_upd_set("update_delay_days", v);
    s_upd_plan_refresh();
}

static void s_upd_apply_cb2(lv_event_t *e) {
    (void)e;
    update_set_ready(1);
    update_install_now();
    s_upd_plan_refresh();
}

static void s_page_update(void) {
    s_phead("Sistem \xe2\x80\xba Güncelleme", "Güncelleme");
    lv_obj_t *c = scard(g_content);
    stitle(c, "Otomatik Güncelleme");
    sdesc(c, "Açıkken sistem yeni sürümü kendisi indirir ve uygun anda kurar. "
             "Kurulum kapanış veya açılışta yapılır; indirme masaüstünü kilitlemez.");
    g_upd_auto_lbl = lv_label_create(c);
    lv_obj_set_style_text_color(g_upd_auto_lbl, lv_color_hex(ADL_TEXT), 0);
    s_add_toggle(c, "Otomatik güncellemeler", "update_auto", 1);

    lv_obj_t *c2 = scard(g_content);
    stitle(c2, "Gecikme");
    sdesc(c2, "Yayınlandıktan kaç gün sonra kurulsun? 0 = sonraki kapanış veya açılış.");
    g_upd_delay_lbl = lv_label_create(c2);
    lv_obj_set_style_text_color(g_upd_delay_lbl, lv_color_hex(ADL_TEXT), 0);
    lv_obj_t *r = srow_start(c2, 46);
    lv_obj_t *m10 = sbutton(r, "-10", ADL_SURFACE_2, ADL_TEXT, 70, 40);
    lv_obj_add_event_cb(m10, s_upd_delay_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-10);
    lv_obj_t *m1 = sbutton(r, "-1", ADL_SURFACE_2, ADL_TEXT, 60, 40);
    lv_obj_add_event_cb(m1, s_upd_delay_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    lv_obj_t *p1 = sbutton(r, "+1", ADL_SURFACE_2, ADL_TEXT, 60, 40);
    lv_obj_add_event_cb(p1, s_upd_delay_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_t *p10 = sbutton(r, "+10", ADL_SURFACE_2, ADL_TEXT, 70, 40);
    lv_obj_add_event_cb(p10, s_upd_delay_cb, LV_EVENT_CLICKED, (void *)(intptr_t)10);

    lv_obj_t *c3 = scard(g_content);
    stitle(c3, "Durum");
    skv(c3, "Kurulu Sürüm", update_applied_version());
    skv(c3, "Derleme", update_current_version());
    skv(c3, "Sunucu", update_base_url());
    skv(c3, "Manifesto İmzası", update_manifest_verified() ? "Geçerli" : "Yok");
    skv(c3, "Hazır Paket", update_ready() ? "Var" : "Yok");
    g_upd_plan_lbl = lv_label_create(c3);
    lv_obj_set_style_text_color(g_upd_plan_lbl, lv_color_hex(ADL_TEXT_DIM), 0);
    lv_obj_t *r2 = srow_start(c3, 46);
    lv_obj_t *b1 = sbutton(r2, "Kontrol Et", ADL_ACCENT, 0xFFFFFF, 150, 40);
    lv_obj_add_event_cb(b1, s_upd_check_cb, LV_EVENT_CLICKED, 0);
    lv_obj_t *b2 = sbutton(r2, "Kur", ADL_SURFACE_2, ADL_TEXT, 110, 40);
    lv_obj_add_event_cb(b2, s_upd_apply_cb2, LV_EVENT_CLICKED, 0);
    sdesc(c3, "\"Kur\" gecikmeyi yok sayar: bu kullanıcı kararıdır.");
    s_upd_plan_refresh();
}

/* ---- Kullanici Hesaplari (g_page == 6) ---- */
static void s_acc_focus_cb(lv_event_t *e) {
    lv_obj_t *ta = lv_event_get_target(e);
    if (!ta) return;
    if (!g_acc_grp) g_acc_grp = lv_group_create();
    lv_group_remove_all_objs(g_acc_grp);
    lv_group_add_obj(g_acc_grp, ta);
    lv_indev_t *kp = lv_port_get_keypad();
    if (kp) lv_indev_set_group(kp, g_acc_grp);
    lv_group_focus_obj(ta);
}

static void s_acc_add_cb(lv_event_t *e) {
    (void)e;
    char nm[32], pw[40];
    sfw_read(g_acc_user, nm, sizeof(nm));
    sfw_read(g_acc_pw, pw, sizeof(pw));
    if (!nm[0]) {
        scpy(s_acc_msg, sizeof(s_acc_msg), "Kullanıcı adı boş olamaz.");
        s_page(6);
        return;
    }
    int rc = arctian_auth_create_user(nm, pw[0] ? pw : 0);
    if (rc == 0) scpy(s_acc_msg, sizeof(s_acc_msg), "Hesap oluşturuldu.");
    else scpy(s_acc_msg, sizeof(s_acc_msg), "Hesap oluşturulamadı (zaten var veya sınır aşıldı).");
    s_page(6);
}

static void s_acc_passwd_cb(lv_event_t *e) {
    (void)e;
    char p1[40], p2[40];
    sfw_read(g_pw_new1, p1, sizeof(p1));
    sfw_read(g_pw_new2, p2, sizeof(p2));
    const char *cur = arctian_auth_current();
    if (!cur || !cur[0]) cur = arctian_auth_username();
    if (!cur || !cur[0]) {
        scpy(s_acc_msg, sizeof(s_acc_msg), "Aktif kullanıcı bulunamadı.");
        s_page(6);
        return;
    }
    int rc = arctian_auth_set_password_for(cur, p1, p2);
    if (rc == 0) scpy(s_acc_msg, sizeof(s_acc_msg), "Parola güncellendi.");
    else scpy(s_acc_msg, sizeof(s_acc_msg), "Parolalar uyuşmuyor veya geçersiz.");
    s_page(6);
}

static void s_page_accounts(void) {
    s_phead("Sistem \xe2\x80\xba Hesaplar", "Kullanıcı Hesapları");
    const char *cur = arctian_auth_current();
    if (!cur || !cur[0]) cur = arctian_auth_username();

    lv_obj_t *c1 = scard(g_content);
    stitle(c1, "Kayıtlı Kullanıcılar");
    int n = arctian_auth_user_count();
    if (n <= 0) {
        sdesc(c1, "Henüz kayıtlı kullanıcı yok.");
    } else {
        for (int i = 0; i < n; i++) {
            const char *un = arctian_auth_user_name(i);
            uint32_t uid = arctian_auth_user_uid(i);
            int has_pw = arctian_auth_user_has_password(i);
            char left[64], right[40], nb[12];
            scpy(left, sizeof(left), un);
            if (cur && seq(un, cur)) scat(left, sizeof(left), "  (aktif)");
            snum(nb, sizeof(nb), (unsigned)uid);
            scpy(right, sizeof(right), "UID "); scat(right, sizeof(right), nb);
            scat(right, sizeof(right), has_pw ? " · Parolalı" : " · Parolasız");
            skv(c1, left, right);
        }
    }

    lv_obj_t *c2 = scard(g_content);
    stitle(c2, "Yeni Kullanıcı Ekle");
    lv_obj_t *r1 = srow_start(c2, 46);
    g_acc_user = lv_textarea_create(r1);
    lv_obj_set_size(g_acc_user, 180, 38);
    lv_textarea_set_one_line(g_acc_user, true);
    lv_textarea_set_max_length(g_acc_user, 24);
    lv_textarea_set_placeholder_text(g_acc_user, "Kullanıcı adı");
    lv_obj_add_event_cb(g_acc_user, s_acc_focus_cb, LV_EVENT_CLICKED, 0);

    g_acc_pw = lv_textarea_create(r1);
    lv_obj_set_size(g_acc_pw, 180, 38);
    lv_textarea_set_one_line(g_acc_pw, true);
    lv_textarea_set_password_mode(g_acc_pw, true);
    lv_textarea_set_max_length(g_acc_pw, 31);
    lv_textarea_set_placeholder_text(g_acc_pw, "Parola (isteğe bağlı)");
    lv_obj_add_event_cb(g_acc_pw, s_acc_focus_cb, LV_EVENT_CLICKED, 0);

    lv_obj_t *badd = sbutton(r1, "Kullanıcı Ekle", SC_ACC, 0xFFFFFF, 140, 38);
    lv_obj_add_event_cb(badd, s_acc_add_cb, LV_EVENT_CLICKED, 0);

    lv_obj_t *c3 = scard(g_content);
    stitle(c3, "Oturum Parolası Değiştir");
    sdesc(c3, "Aktif kullanıcı hesabı için yeni parola belirleyin.");
    lv_obj_t *r2 = srow_start(c3, 46);
    g_pw_new1 = lv_textarea_create(r2);
    lv_obj_set_size(g_pw_new1, 180, 38);
    lv_textarea_set_one_line(g_pw_new1, true);
    lv_textarea_set_password_mode(g_pw_new1, true);
    lv_textarea_set_max_length(g_pw_new1, 31);
    lv_textarea_set_placeholder_text(g_pw_new1, "Yeni parola");
    lv_obj_add_event_cb(g_pw_new1, s_acc_focus_cb, LV_EVENT_CLICKED, 0);

    g_pw_new2 = lv_textarea_create(r2);
    lv_obj_set_size(g_pw_new2, 180, 38);
    lv_textarea_set_one_line(g_pw_new2, true);
    lv_textarea_set_password_mode(g_pw_new2, true);
    lv_textarea_set_max_length(g_pw_new2, 31);
    lv_textarea_set_placeholder_text(g_pw_new2, "Parola (tekrar)");
    lv_obj_add_event_cb(g_pw_new2, s_acc_focus_cb, LV_EVENT_CLICKED, 0);

    lv_obj_t *bpw = sbutton(r2, "Parola Belirle", SC_ACC, 0xFFFFFF, 140, 38);
    lv_obj_add_event_cb(bpw, s_acc_passwd_cb, LV_EVENT_CLICKED, 0);

    if (s_acc_msg[0]) sdesc(c3, s_acc_msg);
}

/* ---- Ayarlarda Arama ---- */
typedef struct {
    const char *title;
    const char *keywords;
    int         page;
} s_search_item_t;

static const s_search_item_t s_search_items[] = {
    { "Uygulama Teması (Açık / Koyu / Sistem)", "tema acik koyu sistem gorunum", 0 },
    { "Vurgu Rengi", "vurgu renk accent mavi", 0 },
    { "Yuvarlak Pencere Köşeleri", "yuvarlak pencere kose", 0 },
    { "Yazı Boyutu ve Yoğunluk", "yazi boyut olcek font yogunluk", 0 },
    { "Ekran Parlaklığı", "parlaklik ekran bright isik", 0 },
    { "Ses Seviyesi", "ses audio volume hoparlor", 0 },
    { "Erişilebilirlik (Kontrast, Büyük Simgeler)", "erisilebilirlik kontrast odak buyuk simge hareket", 0 },
    { "Arka Plan Duvar Kağıdı", "arka plan duvar kagidi wallpaper", 0 },
    { "Ekran Çözünürlüğü", "cozunurluk ekran resolution 1280 1920", 0 },
    { "Ağ Bağlantısı ve DHCP", "ag internet wifi ethernet ip dns gecit dhcp", 1 },
    { "Saat Dilimi (UTC)", "saat dilimi timezone utc istanbul", 2 },
    { "Görev Çubuğu, Saat ve Klavye Düzeni", "gorev cubugu saat 24 klavye turkce calisma alani kilit", 2 },
    { "Güvenlik Duvarı", "guvenlik duvari firewall port tcp udp", 3 },
    { "Disk Şifreleme (AES-XTS)", "disk sifreleme parola gizlilik guvenlik", 3 },
    { "Sistem Güncelleme", "guncelleme ota otomatik surum", 4 },
    { "Sistem Bilgisi, Güç ve Uygulamalar", "hakkinda sistem guc pil acpi uygulama paket kisayol", 5 },
    { "Kullanıcı Hesapları ve Parola", "kullanici hesap parola sifre ekle", 6 },
};

static void s_jump_page_cb(lv_event_t *e) {
    int p = (int)(intptr_t)lv_event_get_user_data(e);
    s_search_q[0] = 0;
    if (g_search_ta) lv_textarea_set_text(g_search_ta, "");
    s_page(p);
}

static void s_render_search(const char *q) {
    s_content_clear();
    s_phead("Ayarlar \xe2\x80\xba Arama", "Arama Sonuçları");
    lv_obj_t *c = scard(g_content);
    int n_items = (int)(sizeof(s_search_items) / sizeof(s_search_items[0]));
    int hits = 0;
    for (int i = 0; i < n_items; i++) {
        if (!s_has_ci(s_search_items[i].title, q) &&
            !s_has_ci(s_search_items[i].keywords, q) &&
            !s_has_ci(g_nav_labels[s_search_items[i].page], q)) continue;
        hits++;
        lv_obj_t *r = srow(c, 42);
        lv_obj_t *col = lv_obj_create(r);
        lv_obj_remove_style_all(col);
        lv_obj_set_height(col, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(col, 1);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *tl = lv_label_create(col);
        lv_label_set_text(tl, s_search_items[i].title);
        lv_obj_set_style_text_color(tl, lv_color_hex(ADL_TEXT), 0);
        if (f13) lv_obj_set_style_text_font(tl, f13, 0);

        lv_obj_t *pl = lv_label_create(col);
        lv_label_set_text(pl, g_nav_labels[s_search_items[i].page]);
        lv_obj_set_style_text_color(pl, lv_color_hex(ADL_TEXT_DIM), 0);
        if (f12) lv_obj_set_style_text_font(pl, f12, 0);

        lv_obj_t *b = sbutton(r, "Git", SC_ACC, 0xFFFFFF, 72, 32);
        lv_obj_add_event_cb(b, s_jump_page_cb, LV_EVENT_CLICKED, (void *)(intptr_t)s_search_items[i].page);
    }
    if (hits == 0) {
        sdesc(c, "Eşleşen ayar bulunamadı.");
    }
}

static void s_search_changed_cb(lv_event_t *e) {
    (void)e;
    if (!g_search_ta) return;
    scpy(s_search_q, sizeof(s_search_q), lv_textarea_get_text(g_search_ta));
    if (s_search_q[0]) s_render_search(s_search_q);
    else s_page(g_page);
}

static void s_search_click_cb(lv_event_t *e) {
    (void)e;
    if (!g_search_ta) return;
    if (!g_search_grp) g_search_grp = lv_group_create();
    lv_group_remove_all_objs(g_search_grp);
    lv_group_add_obj(g_search_grp, g_search_ta);
    lv_indev_t *kp = lv_port_get_keypad();
    if (kp) lv_indev_set_group(kp, g_search_grp);
    lv_group_focus_obj(g_search_ta);
}

static void s_page(int p) {
    if (p < 0 || p >= S_NPAGES) p = 0;
    g_page = p;
    s_pool_n = 0;
    g_crypt_ta = 0;
    g_fw_port = 0; g_fw_name = 0; g_fw_dir = 0; g_fw_proto = 0; g_fw_action = 0;
    g_acc_user = 0; g_acc_pw = 0; g_pw_new1 = 0; g_pw_new2 = 0;
    if (g_upd_timer) { lv_timer_delete(g_upd_timer); g_upd_timer = 0; }
    g_upd_msg = 0; g_upd_bar = 0; g_upd_plan_lbl = 0; g_upd_delay_lbl = 0; g_upd_auto_lbl = 0;
    if (g_crypt_group) lv_group_remove_all_objs(g_crypt_group);
    if (g_acc_grp) lv_group_remove_all_objs(g_acc_grp);
    {
        lv_indev_t *kp = lv_port_get_keypad();
        if (kp) lv_indev_set_group(kp, 0);
    }
    s_content_clear();
    s_nav_active();
    switch (p) {
        case 0: s_page_view(); break;
        case 1: s_page_network(); break;
        case 2: s_page_notify(); break;
        case 3: s_page_privacy(); break;
        case 4: s_page_update(); break;
        case 5: s_page_about(); break;
        default: s_page_accounts(); break;
    }
}

static void s_build_nav(void) {
    g_nav = lv_obj_create(g_win);
    lv_obj_remove_style_all(g_nav);
    lv_obj_set_size(g_nav, S_NAV_W, LV_PCT(100));
    lv_obj_set_style_bg_color(g_nav, lv_color_hex(ADL_SURFACE), 0);
    lv_obj_set_style_bg_opa(g_nav, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_nav, 1, 0);
    lv_obj_set_style_border_side(g_nav, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_color(g_nav, lv_color_hex(0x262A33), 0);
    lv_obj_set_style_pad_hor(g_nav, 8, 0);
    lv_obj_set_style_pad_top(g_nav, 10, 0);
    lv_obj_set_style_pad_row(g_nav, 4, 0);
    lv_obj_set_flex_flow(g_nav, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(g_nav, LV_OBJ_FLAG_SCROLLABLE);

    const char *uname = arctian_auth_current();
    if (!uname || !uname[0]) uname = arctian_auth_username();
    if (!uname || !uname[0]) uname = "Kullanıcı";

    char initial[5];
    unsigned char c0 = (unsigned char)uname[0];
    if (c0 < 0x80) {
        initial[0] = (c0 >= 'a' && c0 <= 'z') ? (char)(c0 - 32) : (char)c0;
        initial[1] = 0;
    } else {
        int nb = (c0 >= 0xF0) ? 4 : (c0 >= 0xE0) ? 3 : 2;
        int k = 0;
        for (; k < nb && uname[k]; k++) initial[k] = uname[k];
        initial[k] = 0;
    }

    lv_obj_t *av = lv_obj_create(g_nav);
    lv_obj_remove_style_all(av);
    lv_obj_set_size(av, LV_PCT(100), 56);
    lv_obj_remove_flag(av, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_left(av, 10, 0);
    lv_obj_set_style_pad_column(av, 10, 0);
    lv_obj_set_flex_flow(av, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(av, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *avc = lv_obj_create(av);
    lv_obj_remove_style_all(avc);
    lv_obj_set_size(avc, 36, 36);
    lv_obj_set_style_radius(avc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(avc, lv_color_hex(0x2A3550), 0);
    lv_obj_set_style_bg_opa(avc, LV_OPA_COVER, 0);
    lv_obj_remove_flag(avc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *avl = lv_label_create(avc);
    lv_label_set_text(avl, initial);
    lv_obj_set_style_text_color(avl, lv_color_hex(0xDBE7FF), 0);
    if (f13) lv_obj_set_style_text_font(avl, f13, 0);
    lv_obj_center(avl);
    lv_obj_t *avcol = lv_obj_create(av);
    lv_obj_remove_style_all(avcol);
    lv_obj_set_height(avcol, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(avcol, 1);
    lv_obj_remove_flag(avcol, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(avcol, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *avn = lv_label_create(avcol);
    lv_label_set_text(avn, uname);
    lv_label_set_long_mode(avn, LV_LABEL_LONG_DOT);
    lv_obj_set_width(avn, LV_PCT(100));
    lv_obj_set_style_text_color(avn, lv_color_hex(ADL_TEXT), 0);
    if (f13) lv_obj_set_style_text_font(avn, f13, 0);
    lv_obj_t *avs = lv_label_create(avcol);
    lv_label_set_text(avs, "Yerel hesap");
    lv_obj_set_style_text_color(avs, lv_color_hex(ADL_TEXT_DIM), 0);
    if (f12) lv_obj_set_style_text_font(avs, f12, 0);

    lv_obj_t *sb = lv_obj_create(g_nav);
    lv_obj_remove_style_all(sb);
    lv_obj_set_size(sb, LV_PCT(100), 34);
    lv_obj_set_style_radius(sb, 6, 0);
    lv_obj_set_style_bg_color(sb, lv_color_hex(SC_INP), 0);
    lv_obj_set_style_bg_opa(sb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sb, 1, 0);
    lv_obj_set_style_border_color(sb, lv_color_hex(0x2C313B), 0);
    lv_obj_remove_flag(sb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_left(sb, 8, 0);
    lv_obj_set_style_pad_right(sb, 6, 0);
    lv_obj_set_style_pad_column(sb, 6, 0);
    lv_obj_set_flex_flow(sb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sb, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_svg_icon(sb, "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#7D8491\" stroke-width=\"2\"><circle cx=\"11\" cy=\"11\" r=\"7\"/><path d=\"M20 20l-4-4\"/></svg>", 14);

    g_search_ta = lv_textarea_create(sb);
    lv_obj_remove_style_all(g_search_ta);
    lv_textarea_set_one_line(g_search_ta, true);
    lv_textarea_set_max_length(g_search_ta, 40);
    lv_textarea_set_placeholder_text(g_search_ta, "Ayarlarda ara");
    lv_obj_set_flex_grow(g_search_ta, 1);
    lv_obj_set_height(g_search_ta, 24);
    lv_obj_set_style_text_color(g_search_ta, lv_color_hex(ADL_TEXT), 0);
    lv_obj_set_style_text_color(g_search_ta, lv_color_hex(0x7D8491), LV_PART_TEXTAREA_PLACEHOLDER);
    if (f12) lv_obj_set_style_text_font(g_search_ta, f12, 0);
    lv_obj_add_event_cb(g_search_ta, s_search_changed_cb, LV_EVENT_VALUE_CHANGED, 0);
    lv_obj_add_event_cb(g_search_ta, s_search_click_cb, LV_EVENT_CLICKED, 0);

    for (int i = 0; i < S_NPAGES; i++) {
        lv_obj_t *b = lv_button_create(g_nav);
        lv_obj_set_size(b, LV_PCT(100), 34);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x181B22), LV_STATE_HOVERED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_HOVERED);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_left(b, 14, 0);
        lv_obj_set_style_pad_column(b, 10, 0);

        lv_obj_t *rod = lv_obj_create(b);
        lv_obj_remove_style_all(rod);
        lv_obj_set_size(rod, 3, 16);
        lv_obj_set_style_radius(rod, 2, 0);
        lv_obj_set_style_bg_color(rod, lv_color_hex(SC_NAVBAR), 0);
        lv_obj_set_style_bg_opa(rod, LV_OPA_TRANSP, 0);
        g_nav_rod[i] = rod;

        s_svg_icon(b, g_nav_icons[i], 16);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, g_nav_labels[i]);
        lv_obj_set_style_text_color(l, lv_color_hex(SC_NAVFG), 0);
        if (f13) lv_obj_set_style_text_font(l, f13, 0);
        lv_obj_add_event_cb(b, s_nav_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        g_nav_btns[i] = b;
        g_nav_lbl[i] = l;
    }
}

static void s_build_content(void) {
    g_content = lv_obj_create(g_win);
    lv_obj_remove_style_all(g_content);
    lv_obj_set_height(g_content, LV_PCT(100));
    lv_obj_set_flex_grow(g_content, 1);
    lv_obj_set_style_bg_color(g_content, lv_color_hex(ADL_BG), 0);
    lv_obj_set_style_bg_opa(g_content, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_ver(g_content, 18, 0);
    lv_obj_set_style_pad_hor(g_content, 26, 0);
    lv_obj_set_style_pad_row(g_content, 12, 0);
    lv_obj_set_flex_flow(g_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(g_content, LV_DIR_VER);
}

static void s_win_del_cb(lv_event_t *e) {
    (void)e;
    if (g_upd_timer) { lv_timer_delete(g_upd_timer); g_upd_timer = 0; }
    lv_indev_t *kp = lv_port_get_keypad();
    if (kp) lv_indev_set_group(kp, 0);
    if (g_search_grp) { lv_group_delete(g_search_grp); g_search_grp = 0; }
    if (g_crypt_group) { lv_group_delete(g_crypt_group); g_crypt_group = 0; }
    if (g_acc_grp) { lv_group_delete(g_acc_grp); g_acc_grp = 0; }
    g_search_ta = 0; s_search_q[0] = 0;
    g_upd_msg = 0; g_upd_bar = 0; g_upd_plan_lbl = 0;
    g_crypt_ta = 0;
    g_acc_user = 0; g_acc_pw = 0; g_pw_new1 = 0; g_pw_new2 = 0; s_acc_msg[0] = 0;
    g_win = 0; g_nav = 0; g_content = 0;
    for (int i = 0; i < S_NPAGES; i++) { g_nav_btns[i] = 0; g_nav_lbl[i] = 0; g_nav_rod[i] = 0; }
    for (int i = 0; i < s_icon_n; i++)
        if (s_asi && s_asi->mem) s_asi->mem->free(s_icon_buf[i]);
    s_icon_n = 0;
}

void app_entry(lv_obj_t *win, asi_t *asi) {
    s_asi = asi;
    g_win = win;
    g_nav = 0; g_content = 0; g_page = 0; s_pool_n = 0;
    s_search_q[0] = 0; s_acc_msg[0] = 0;
    for (int i = 0; i < S_NPAGES; i++) { g_nav_btns[i] = 0; g_nav_lbl[i] = 0; g_nav_rod[i] = 0; }

    if (!f22) f22 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 22);
    if (!f13) f13 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 13);
    if (!f12) f12 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 12);

    lv_obj_set_flex_flow(win, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(win, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(win, 0, 0);
    lv_obj_set_style_pad_column(win, 0, 0);

    s_build_nav();
    s_build_content();
    lv_obj_add_event_cb(win, s_win_del_cb, LV_EVENT_DELETE, 0);

    s_page(0);
#ifdef ARCTIAN_UPDATE_SELFTEST
    s_page(4);
#endif
}
