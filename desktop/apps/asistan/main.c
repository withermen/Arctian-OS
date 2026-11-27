/* Arctian Asistanı - uygulama penceresi */
#include "app.h"
#include "dsk.h"
#include "adl_wolf.h"
#include "lv_port.h"
#include <stdint.h>

static asi_t      *s_asi;
static lv_obj_t   *s_list;
static lv_obj_t   *s_input;
static lv_obj_t   *s_win;
static lv_group_t *s_group;

static int as_slen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

static void as_cpy(char *d, int cap, const char *s) {
    int i = 0;
    if (cap <= 0) return;
    if (!s) { d[0] = 0; return; }
    for (; s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}

static void as_cat(char *d, int cap, const char *s) {
    int i = as_slen(d), j = 0;
    for (; s && s[j] && i < cap - 1; i++, j++) d[i] = s[j];
    d[i] = 0;
}

static char as_lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int as_starts_ci(const char *s, const char *p) {
    if (!s || !p) return 0;
    for (int i = 0; p[i]; i++) {
        if (!s[i] || as_lc(s[i]) != as_lc(p[i])) return 0;
    }
    return 1;
}

static int as_has_substr_ci(const char *hay, const char *needle) {
    if (!hay || !needle || !needle[0]) return 0;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] && as_lc(hay[i + j]) == as_lc(needle[j])) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

static lv_obj_t *as_label(lv_obj_t *p, const char *txt, uint32_t col) {
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_style_text_font(l, adl_font(), 0);
    return l;
}

static void scroll_end(void) {
    lv_obj_update_layout(s_list);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_scroll_to_y(s_list, lv_obj_get_scroll_bottom(s_list) + 100000, LV_ANIM_OFF);
}

static void bubble(const char *txt, int mine) {
    if (!s_list) return;
    lv_obj_t *row = lv_obj_create(s_list);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, mine ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *b = lv_obj_create(row);
    lv_obj_remove_style_all(b);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(b, LV_PCT(mine ? 84 : 88));
    lv_obj_set_height(b, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(b, ui_s(12), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(mine ? ADL_ACCENT : ADL_SURFACE_2), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(b, ui_s(10), 0);
    lv_obj_set_style_border_width(b, mine ? 0 : 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(ADL_BORDER), 0);

    lv_obj_t *l = as_label(b, txt, mine ? 0xFFFFFF : ADL_TEXT);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));

    scroll_end();
}

static void app_sink(const char *txt) { bubble(txt, 0); }

/* ---- Basit matematik ifade cozucu (hesapla <ifade>) ---- */
typedef struct {
    const char *p;
    int err;
} expr_ctx_t;

static void expr_skip(expr_ctx_t *c) {
    while (*c->p == ' ' || *c->p == '\t') c->p++;
}

static int64_t expr_add(expr_ctx_t *c);

static int64_t expr_atom(expr_ctx_t *c) {
    expr_skip(c);
    if (*c->p == '(') {
        c->p++;
        int64_t v = expr_add(c);
        expr_skip(c);
        if (*c->p == ')') c->p++;
        else c->err = 1;
        return v;
    }
    int sign = 1;
    if (*c->p == '-') { sign = -1; c->p++; expr_skip(c); }
    else if (*c->p == '+') { c->p++; expr_skip(c); }
    if (*c->p < '0' || *c->p > '9') { c->err = 1; return 0; }
    int64_t v = 0;
    while (*c->p >= '0' && *c->p <= '9') {
        v = v * 10 + (*c->p - '0');
        c->p++;
    }
    return sign * v;
}

static int64_t expr_mul(expr_ctx_t *c) {
    int64_t v = expr_atom(c);
    for (;;) {
        expr_skip(c);
        char op = *c->p;
        if (op != '*' && op != '/' && op != '%') break;
        c->p++;
        int64_t r = expr_atom(c);
        if (c->err) return 0;
        if (op == '*') v *= r;
        else if (r == 0) { c->err = 2; return 0; }
        else if (op == '/') v /= r;
        else v %= r;
    }
    return v;
}

static int64_t expr_add(expr_ctx_t *c) {
    int64_t v = expr_mul(c);
    for (;;) {
        expr_skip(c);
        char op = *c->p;
        if (op != '+' && op != '-') break;
        c->p++;
        int64_t r = expr_mul(c);
        if (c->err) return 0;
        if (op == '+') v += r;
        else v -= r;
    }
    return v;
}

static void i64_to_str(int64_t v, char *out, int cap) {
    char t[24];
    int n = 0, k = 0;
    if (v < 0) {
        if (k < cap - 1) out[k++] = '-';
        v = -v;
    }
    if (v == 0) t[n++] = '0';
    while (v > 0 && n < (int)sizeof(t)) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n && k < cap - 1) out[k++] = t[--n];
    out[k] = 0;
}

/* ---- Dosya arama (ara <dosya>) ---- */
static void as_search_walk(const char *dir, const char *pat, int depth,
                           char *out, int cap, int *hits) {
    if (!s_asi || !s_asi->fs || !s_asi->fs->count_dir || !s_asi->fs->list_dir) return;
    if (depth > 6 || *hits >= 12) return;
    int cnt = s_asi->fs->count_dir(dir);
    for (int i = 0; i < cnt && *hits < 12; i++) {
        char nm[80];
        int isd = 0;
        if (s_asi->fs->list_dir(dir, i, nm, sizeof(nm), &isd) != 0 || !nm[0]) continue;
        char full[220];
        as_cpy(full, sizeof(full), dir);
        int l = as_slen(full);
        if (l > 0 && full[l - 1] != '/') as_cat(full, sizeof(full), "/");
        as_cat(full, sizeof(full), nm);

        if (as_has_substr_ci(nm, pat)) {
            as_cat(out, cap, isd ? "\n[Klasör] " : "\n");
            as_cat(out, cap, full);
            (*hits)++;
        }
        if (isd) as_search_walk(full, pat, depth + 1, out, cap, hits);
    }
}

static int try_local_command(const char *line) {
    if (as_starts_ci(line, "hesapla ")) {
        const char *ex = line + 8;
        while (*ex == ' ') ex++;
        if (!ex[0]) { bubble("Kullanım: hesapla 25*4", 0); return 1; }
        expr_ctx_t c = { ex, 0 };
        int64_t val = expr_add(&c);
        expr_skip(&c);
        if (*c.p != 0 && !c.err) c.err = 1;
        if (c.err == 2) bubble("Sıfıra bölme hatası.", 0);
        else if (c.err) bubble("İfade anlaşılamadı. Örnek: hesapla (12+8)*3", 0);
        else {
            char res[96], nb[32];
            i64_to_str(val, nb, sizeof(nb));
            as_cpy(res, sizeof(res), "Sonuç: ");
            as_cat(res, sizeof(res), nb);
            bubble(res, 0);
        }
        return 1;
    }

    if (as_starts_ci(line, "ara ")) {
        const char *pat = line + 4;
        while (*pat == ' ') pat++;
        if (!pat[0]) { bubble("Kullanım: ara <dosya_adı>", 0); return 1; }
        char out[640];
        as_cpy(out, sizeof(out), "Arama sonuçları:");
        int hits = 0;
        as_search_walk("C:/", pat, 0, out, sizeof(out), &hits);
        if (hits == 0) bubble("Eşleşen dosya veya klasör bulunamadı.", 0);
        else bubble(out, 0);
        return 1;
    }

    if (as_starts_ci(line, "yardım") || as_starts_ci(line, "yardim")) {
        bubble("Yapabildiklerim:\n• Uygulama açma (\"terminal aç\")\n• Matematik (\"hesapla 25*4\")\n• Dosya arama (\"ara notlar\")\n• Sistem durumu, saat, pil, ağ, not alma", 0);
        return 1;
    }
    return 0;
}

static void app_send(void) {
    if (!s_input || !s_list) return;
    const char *q = lv_textarea_get_text(s_input);
    if (!q || !q[0]) return;
    char line[160];
    int i = 0;
    for (; q[i] && i < (int)sizeof(line) - 1; i++) line[i] = q[i];
    line[i] = 0;

    lv_textarea_set_text(s_input, "");
    bubble(line, 1);
    if (!try_local_command(line)) assistant_ask(line);
}

static void send_cb(lv_event_t *e) { (void)e; app_send(); }

static void chip_cb(lv_event_t *e) {
    const char *txt = (const char *)lv_event_get_user_data(e);
    if (!txt || !s_input) return;
    lv_textarea_set_text(s_input, txt);
    app_send();
}

static void chip(lv_obj_t *p, const char *txt) {
    lv_obj_t *c = lv_obj_create(p);
    lv_obj_remove_style_all(c);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(c, LV_SIZE_CONTENT, ui_s(30));
    lv_obj_set_style_radius(c, ui_s(15), 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(ADL_SURFACE_2), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(ADL_BORDER), 0);
    lv_obj_set_style_pad_hor(c, ui_s(12), 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, chip_cb, LV_EVENT_CLICKED, (void *)txt);
    lv_obj_t *l = as_label(c, txt, ADL_TEXT);
    lv_obj_center(l);
}

static void win_del_cb(lv_event_t *e) {
    (void)e;
    assistant_set_sink(0);
    if (s_group) {
        lv_indev_t *kp = lv_port_get_keypad();
        if (kp && lv_indev_get_group(kp) == s_group) lv_indev_set_group(kp, 0);
        lv_group_delete(s_group);
        s_group = 0;
    }
    s_list = s_input = s_win = 0;
    s_asi = 0;
}

void app_entry(lv_obj_t *win, asi_t *asi) {
    s_asi = asi;
    s_win = win;
    s_list = s_input = 0;

    lv_obj_set_style_pad_all(win, ui_s(14), 0);
    lv_obj_set_style_pad_row(win, ui_s(8), 0);
    lv_obj_set_flex_flow(win, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(win, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_event_cb(win, win_del_cb, LV_EVENT_DELETE, 0);

    lv_obj_t *hdr = lv_obj_create(win);
    lv_obj_remove_style_all(hdr);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(hdr, LV_PCT(100));
    lv_obj_set_height(hdr, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_COLUMN);
    as_label(hdr, "Arctian Asistan", ADL_TEXT);
    lv_obj_t *sub = as_label(hdr, "Yerel çalışır; internete veri göndermez.", ADL_TEXT_DIM);
    lv_obj_set_style_text_font(sub, adl_font(), 0);

    s_list = lv_obj_create(win);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, ui_s(8), 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *chips = lv_obj_create(win);
    lv_obj_remove_style_all(chips);
    lv_obj_remove_flag(chips, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(chips, LV_PCT(100));
    lv_obj_set_height(chips, ui_s(34));
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(chips, ui_s(6), 0);
    chip(chips, "Sistem durumu");
    chip(chips, "Saat kaç");
    chip(chips, "hesapla 25*4");
    chip(chips, "Yardım");

    s_input = lv_textarea_create(win);
    lv_obj_remove_style_all(s_input);
    lv_textarea_set_one_line(s_input, true);
    lv_textarea_set_max_length(s_input, 120);
    lv_textarea_set_placeholder_text(s_input, "Bir şey yazın: \"terminal aç\", \"hesapla 25*4\", \"ara not\"");
    lv_obj_set_width(s_input, LV_PCT(100));
    lv_obj_set_height(s_input, ui_s(44));
    lv_obj_set_style_bg_color(s_input, lv_color_hex(ADL_SURFACE_2), 0);
    lv_obj_set_style_bg_opa(s_input, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_input, ui_s(12), 0);
    lv_obj_set_style_pad_all(s_input, ui_s(10), 0);
    lv_obj_set_style_text_color(s_input, lv_color_hex(ADL_TEXT), 0);
    lv_obj_set_style_text_font(s_input, adl_font(), 0);
    lv_obj_add_event_cb(s_input, send_cb, LV_EVENT_READY, 0);

    lv_group_t *g = lv_group_create();
    s_group = g;
    lv_group_add_obj(g, s_input);
    lv_group_focus_obj(s_input);
    lv_indev_t *kp = lv_port_get_keypad();
    if (kp) lv_indev_set_group(kp, g);

    assistant_set_sink(app_sink);
    app_sink("Merhaba! Uygulama açabilir, \"hesapla 25*4\" ile işlem yapabilir, \"ara <dosya>\" ile dosya arayabilir veya not alabilirsiniz.");
}
