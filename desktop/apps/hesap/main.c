/* ==================================================================
 * Hesap Makinesi (Standart + Bilimsel + Geçmiş Şeridi)
 *
 * Özellikler:
 *   - Koyu / Açık tema anahtarı
 *   - Standart + Bilimsel kip (sin, cos, tan, sqrt, pow, ln, log, pi, %)
 *   - DER / RAD açı birimi
 *   - Tam klavye desteği (0-9, ., +, -, *, /, ^, %, Enter, Backspace, Esc)
 *   - Hesaplama geçmişi ve tek tıkla geri yükleme
 *   - relayout desteği ile yeniden boyutlandırma uyumu
 * ================================================================== */
#include "app.h"
#include "adl_wolf.h"
#include "lv_port.h"
#include <stdint.h>

extern double fabs(double x);
extern double floor(double x);
extern double sqrt(double x);
extern double sin(double x);
extern double cos(double x);

#define CALC_PI 3.14159265358979323846
#define CALC_E  2.71828182845904523536
#define HIST_MAX 20

static asi_t      *A;
static lv_obj_t   *g_win;
static lv_group_t *g_grp;

static int  g_light = 0;
static int  g_sci   = 0;
static int  g_deg   = 1;

static char   g_disp[48] = "0";
static char   g_expr[64] = "";
static double g_acc = 0.0;
static char   g_op  = 0;
static int    g_new_entry = 1;
static int    g_err = 0;

typedef struct {
    char expr[64];
    char res[32];
} calc_hist_t;

static calc_hist_t g_hist[HIST_MAX];
static int         g_hist_n;

extern const unsigned char font_ttf[];
extern const unsigned int  font_ttf_len;
static lv_font_t *f28, *f15, *f13, *f12;

/* ---- Freestanding matematik yardımcıları ---- */
static double calc_exp(double x) {
    if (x > 700.0) x = 700.0;
    if (x < -700.0) return 0.0;
    /* e^x = 2^k * e^r, ln(2) = 0.6931471805599453 */
    const double LN2 = 0.6931471805599453;
    int k = (int)floor(x / LN2 + 0.5);
    double r = x - (double)k * LN2;
    double sum = 1.0, term = 1.0;
    for (int i = 1; i <= 18; i++) {
        term *= r / (double)i;
        sum += term;
    }
    if (k > 0) { for (int i = 0; i < k && i < 1020; i++) sum *= 2.0; }
    else if (k < 0) { for (int i = 0; i < -k && i < 1020; i++) sum *= 0.5; }
    return sum;
}

static double calc_ln(double x) {
    if (x <= 0.0) return 0.0;
    int k = 0;
    while (x >= 2.0) { x *= 0.5; k++; }
    while (x < 1.0)  { x *= 2.0; k--; }
    double t = (x - 1.0) / (x + 1.0);
    double t2 = t * t;
    double sum = 0.0, p = t;
    for (int i = 1; i <= 25; i += 2) {
        sum += p / (double)i;
        p *= t2;
    }
    return 2.0 * sum + (double)k * 0.6931471805599453;
}

static double calc_log10(double x) {
    return calc_ln(x) / 2.30258509299404568402;
}

static double calc_pow(double b, double e) {
    if (e == 0.0) return 1.0;
    if (b == 0.0) return e > 0.0 ? 0.0 : 0.0;
    double ie = floor(e);
    if (ie == e && fabs(e) <= 1000.0) {
        int neg = e < 0.0;
        int n = neg ? (int)(-e) : (int)e;
        double r = 1.0, cur = b;
        while (n > 0) {
            if (n & 1) r *= cur;
            cur *= cur;
            n >>= 1;
        }
        return neg ? (1.0 / r) : r;
    }
    if (b < 0.0) return 0.0;
    return calc_exp(e * calc_ln(b));
}

static double calc_tan(double x) {
    double c = cos(x);
    if (fabs(c) < 1e-12) return 0.0;
    return sin(x) / c;
}

/* ---- Metin ve sayı dönüşümleri ---- */
static int slen(const char *s) { int n = 0; if (s) while (s[n]) n++; return n; }

static int seq0(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return !*a && !*b;
}

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

static double str_to_f(const char *s) {
    if (!s || !*s) return 0.0;
    int neg = 0, i = 0;
    if (s[i] == '-') { neg = 1; i++; }
    else if (s[i] == '+') i++;
    double val = 0.0;
    while (s[i] >= '0' && s[i] <= '9') {
        val = val * 10.0 + (double)(s[i] - '0');
        i++;
    }
    if (s[i] == '.' || s[i] == ',') {
        i++;
        double frac = 0.1;
        while (s[i] >= '0' && s[i] <= '9') {
            val += (double)(s[i] - '0') * frac;
            frac *= 0.1;
            i++;
        }
    }
    return neg ? -val : val;
}

static void f_to_str(double v, char *out, int cap) {
    if (v != v) { scpy(out, cap, "Hata"); return; }
    if (fabs(v) < 1e-12) v = 0.0;
    int k = 0;
    if (v < 0.0) { if (k < cap - 1) out[k++] = '-'; v = -v; }
    if (v >= 1e12) { scpy(out, cap, "Taşma"); return; }

    /* 8 ondalık haneye yuvarla */
    double round_v = v + 0.5e-8;
    uint64_t ipart = (uint64_t)floor(round_v);
    double frac = round_v - (double)ipart;
    char ibuf[24];
    int in = 0;
    if (ipart == 0) ibuf[in++] = '0';
    while (ipart > 0 && in < 22) { ibuf[in++] = (char)('0' + (ipart % 10u)); ipart /= 10u; }
    while (in > 0 && k < cap - 1) out[k++] = ibuf[--in];

    char fbuf[12];
    int fn = 0, last_nz = -1;
    for (int i = 0; i < 8; i++) {
        frac *= 10.0;
        int d = (int)floor(frac);
        if (d < 0) d = 0;
        if (d > 9) d = 9;
        frac -= (double)d;
        fbuf[fn] = (char)('0' + d);
        if (d != 0) last_nz = fn;
        fn++;
    }
    if (last_nz >= 0 && k < cap - 2) {
        out[k++] = '.';
        for (int i = 0; i <= last_nz && k < cap - 1; i++) out[k++] = fbuf[i];
    }
    out[k] = 0;
}

static void push_hist(const char *expr, const char *res) {
    if (g_hist_n >= HIST_MAX) {
        for (int i = 1; i < g_hist_n; i++) g_hist[i - 1] = g_hist[i];
        g_hist_n--;
    }
    scpy(g_hist[g_hist_n].expr, sizeof(g_hist[0].expr), expr);
    scpy(g_hist[g_hist_n].res, sizeof(g_hist[0].res), res);
    g_hist_n++;
}

static void build_ui(void);

static double apply_op(double a, double b, char op, int *err_out) {
    *err_out = 0;
    switch (op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/':
            if (fabs(b) < 1e-15) { *err_out = 1; return 0.0; }
            return a / b;
        case '^': return calc_pow(a, b);
        case '%':
            if (fabs(b) < 1e-15) { *err_out = 1; return 0.0; }
            return a - floor(a / b) * b;
        default: return b;
    }
}

static void press_digit(char d) {
    if (g_err) { scpy(g_disp, sizeof(g_disp), "0"); g_expr[0] = 0; g_op = 0; g_err = 0; }
    if (g_new_entry) {
        if (d == '.') scpy(g_disp, sizeof(g_disp), "0.");
        else { g_disp[0] = d; g_disp[1] = 0; }
        g_new_entry = 0;
    } else {
        int L = slen(g_disp);
        if (d == '.') {
            for (int i = 0; i < L; i++) if (g_disp[i] == '.') return;
            if (L < 24) { g_disp[L] = '.'; g_disp[L + 1] = 0; }
        } else if (seq0(g_disp, "0")) {
            g_disp[0] = d; g_disp[1] = 0;
        } else if (L < 24) {
            g_disp[L] = d; g_disp[L + 1] = 0;
        }
    }
    build_ui();
}

static void press_backspace(void) {
    if (g_err) { scpy(g_disp, sizeof(g_disp), "0"); g_err = 0; build_ui(); return; }
    if (g_new_entry) return;
    int L = slen(g_disp);
    if (L <= 1 || (L == 2 && g_disp[0] == '-')) scpy(g_disp, sizeof(g_disp), "0");
    else g_disp[L - 1] = 0;
    build_ui();
}

static void press_clear(int all) {
    scpy(g_disp, sizeof(g_disp), "0");
    g_new_entry = 1;
    g_err = 0;
    if (all) { g_acc = 0.0; g_op = 0; g_expr[0] = 0; }
    build_ui();
}

static void press_op(char op) {
    if (g_err) { press_clear(1); return; }
    double cur = str_to_f(g_disp);
    if (g_op && !g_new_entry) {
        int e = 0;
        g_acc = apply_op(g_acc, cur, g_op, &e);
        if (e) {
            scpy(g_disp, sizeof(g_disp), "Sıfıra bölünemez");
            g_err = 1; g_op = 0; g_expr[0] = 0;
            build_ui();
            return;
        }
        f_to_str(g_acc, g_disp, sizeof(g_disp));
    } else {
        g_acc = cur;
    }
    g_op = op;
    char ab[32];
    f_to_str(g_acc, ab, sizeof(ab));
    scpy(g_expr, sizeof(g_expr), ab);
    const char *sym = (op == '*') ? " × " : (op == '/') ? " ÷ " : (op == '+') ? " + " :
                      (op == '-') ? " − " : (op == '^') ? " ^ " : " % ";
    scat(g_expr, sizeof(g_expr), sym);
    g_new_entry = 1;
    build_ui();
}

static void press_eq(void) {
    if (g_err) return;
    if (!g_op) { g_new_entry = 1; build_ui(); return; }
    double cur = str_to_f(g_disp);
    char full_expr[64], cb[32];
    scpy(full_expr, sizeof(full_expr), g_expr);
    f_to_str(cur, cb, sizeof(cb));
    scat(full_expr, sizeof(full_expr), cb);

    int e = 0;
    double res = apply_op(g_acc, cur, g_op, &e);
    if (e) {
        scpy(g_disp, sizeof(g_disp), "Sıfıra bölünemez");
        g_err = 1; g_op = 0; g_expr[0] = 0;
    } else {
        f_to_str(res, g_disp, sizeof(g_disp));
        g_acc = res;
        g_op = 0;
        scpy(g_expr, sizeof(g_expr), full_expr);
        scat(g_expr, sizeof(g_expr), " =");
        push_hist(full_expr, g_disp);
        g_new_entry = 1;
    }
    build_ui();
}

static void press_unary(const char *fn) {
    if (g_err) { press_clear(1); return; }
    double v = str_to_f(g_disp);
    double r = v;
    int err = 0;
    char lbl[64];
    scpy(lbl, sizeof(lbl), fn);
    scat(lbl, sizeof(lbl), "(");
    scat(lbl, sizeof(lbl), g_disp);
    scat(lbl, sizeof(lbl), ")");

    if (seq0(fn, "±")) {
        r = -v;
        lbl[0] = 0;
    } else if (seq0(fn, "%")) {
        r = (g_op == '+' || g_op == '-') ? (g_acc * v / 100.0) : (v / 100.0);
    } else if (seq0(fn, "√")) {
        if (v < 0.0) err = 1;
        else r = sqrt(v);
    } else if (seq0(fn, "x²")) {
        r = v * v;
    } else if (seq0(fn, "1/x")) {
        if (fabs(v) < 1e-15) err = 1;
        else r = 1.0 / v;
    } else if (seq0(fn, "sin")) {
        double rad = g_deg ? (v * CALC_PI / 180.0) : v;
        r = sin(rad);
    } else if (seq0(fn, "cos")) {
        double rad = g_deg ? (v * CALC_PI / 180.0) : v;
        r = cos(rad);
    } else if (seq0(fn, "tan")) {
        double rad = g_deg ? (v * CALC_PI / 180.0) : v;
        if (fabs(cos(rad)) < 1e-12) err = 1;
        else r = calc_tan(rad);
    } else if (seq0(fn, "ln")) {
        if (v <= 0.0) err = 1;
        else r = calc_ln(v);
    } else if (seq0(fn, "log")) {
        if (v <= 0.0) err = 1;
        else r = calc_log10(v);
    } else if (seq0(fn, "π")) {
        r = CALC_PI;
        lbl[0] = 0;
    } else if (seq0(fn, "e")) {
        r = CALC_E;
        lbl[0] = 0;
    }

    if (err) {
        scpy(g_disp, sizeof(g_disp), "Geçersiz giriş");
        g_err = 1;
    } else {
        f_to_str(r, g_disp, sizeof(g_disp));
        if (lbl[0]) {
            push_hist(lbl, g_disp);
            scpy(g_expr, sizeof(g_expr), lbl);
        }
        g_new_entry = 1;
    }
    build_ui();
}

static void btn_cb(lv_event_t *e) {
    const char *tag = (const char *)lv_event_get_user_data(e);
    if (!tag || !tag[0]) return;
    if (tag[1] == 0 && ((tag[0] >= '0' && tag[0] <= '9') || tag[0] == '.')) {
        press_digit(tag[0]);
    } else if (seq0(tag, "C")) {
        press_clear(1);
    } else if (seq0(tag, "CE")) {
        press_clear(0);
    } else if (seq0(tag, "⌫")) {
        press_backspace();
    } else if (seq0(tag, "=")) {
        press_eq();
    } else if (seq0(tag, "+")) {
        press_op('+');
    } else if (seq0(tag, "−")) {
        press_op('-');
    } else if (seq0(tag, "×")) {
        press_op('*');
    } else if (seq0(tag, "÷")) {
        press_op('/');
    } else if (seq0(tag, "xʸ")) {
        press_op('^');
    } else {
        press_unary(tag);
    }
}

static void toggle_mode_cb(lv_event_t *e) { (void)e; g_sci = !g_sci; build_ui(); }
static void toggle_theme_cb(lv_event_t *e) { (void)e; g_light = !g_light; build_ui(); }
static void toggle_deg_cb(lv_event_t *e) { (void)e; g_deg = !g_deg; build_ui(); }
static void clear_hist_cb(lv_event_t *e) { (void)e; g_hist_n = 0; build_ui(); }
static void pick_hist_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= g_hist_n) return;
    scpy(g_disp, sizeof(g_disp), g_hist[idx].res);
    scpy(g_expr, sizeof(g_expr), g_hist[idx].expr);
    g_new_entry = 1;
    g_err = 0;
    build_ui();
}

static void key_cb(lv_event_t *e) {
    uint32_t k = lv_event_get_key(e);
    if (k >= '0' && k <= '9') press_digit((char)k);
    else if (k == '.' || k == ',') press_digit('.');
    else if (k == '+') press_op('+');
    else if (k == '-') press_op('-');
    else if (k == '*') press_op('*');
    else if (k == '/') press_op('/');
    else if (k == '^') press_op('^');
    else if (k == '%') press_unary("%");
    else if (k == '=' || k == LV_KEY_ENTER) press_eq();
    else if (k == LV_KEY_BACKSPACE) press_backspace();
    else if (k == LV_KEY_ESC || k == 'c' || k == 'C') press_clear(1);
}

static lv_obj_t *mk_btn(lv_obj_t *parent, const char *txt, uint32_t bg, uint32_t fg,
                        const char *tag, lv_font_t *font) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, LV_PCT(100));
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(g_light ? 0xCBD5E1 : 0x2C313B), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
    if (font) lv_obj_set_style_text_font(l, font, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, btn_cb, LV_EVENT_CLICKED, (void *)tag);
    return b;
}

static lv_obj_t *mk_hdr_pill(lv_obj_t *parent, const char *txt, uint32_t bg, uint32_t fg, lv_event_cb_t cb) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 26);
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(g_light ? 0xCBD5E1 : 0x2C313B), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 10, 0);
    lv_obj_set_style_pad_ver(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
    if (f12) lv_obj_set_style_text_font(l, f12, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, 0);
    return b;
}

static void build_ui(void) {
    if (!g_win) return;
    lv_obj_clean(g_win);
    lv_obj_update_layout(g_win);
    int W = lv_obj_get_width(g_win);
    if (W < 200) W = 420;
    int wide = (W >= 580);

    uint32_t c_bg   = g_light ? 0xF1F5F9 : 0x111318;
    uint32_t c_surf = g_light ? 0xFFFFFF : 0x181B22;
    uint32_t c_num  = g_light ? 0xFFFFFF : 0x222733;
    uint32_t c_fn   = g_light ? 0xE2E8F0 : 0x1B1F29;
    uint32_t c_op   = g_light ? 0xDBEAFE : 0x1E293B;
    uint32_t c_acc  = 0x3B82F6;
    uint32_t c_txt  = g_light ? 0x0F172A : 0xE8EAEE;
    uint32_t c_dim  = g_light ? 0x64748B : 0x8B919C;
    uint32_t c_opfg = g_light ? 0x1D4ED8 : 0x7FA8F0;

    lv_obj_set_style_bg_color(g_win, lv_color_hex(c_bg), 0);
    lv_obj_set_style_bg_opa(g_win, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(g_win, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(g_win, 10, 0);
    lv_obj_set_style_pad_row(g_win, 8, 0);

    /* Üst başlık */
    lv_obj_t *hdr = lv_obj_create(g_win);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LV_PCT(100), 30);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hl = lv_obj_create(hdr);
    lv_obj_remove_style_all(hl);
    lv_obj_set_size(hl, LV_SIZE_CONTENT, 30);
    lv_obj_set_flex_flow(hl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hl, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hl, 6, 0);
    mk_hdr_pill(hl, g_sci ? "Bilimsel" : "Standart", g_sci ? c_acc : c_fn, g_sci ? 0xFFFFFF : c_txt, toggle_mode_cb);
    if (g_sci) mk_hdr_pill(hl, g_deg ? "DER" : "RAD", c_fn, c_txt, toggle_deg_cb);

    mk_hdr_pill(hdr, g_light ? "Koyu Tema" : "Açık Tema", c_fn, c_txt, toggle_theme_cb);

    /* Ana alan: genişse yan yana (hesap + geçmiş), darsa üstte geçmiş şeridi */
    lv_obj_t *main_row = lv_obj_create(g_win);
    lv_obj_remove_style_all(main_row);
    lv_obj_set_width(main_row, LV_PCT(100));
    lv_obj_set_flex_grow(main_row, 1);
    lv_obj_set_flex_flow(main_row, wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_column(main_row, 10, 0);
    lv_obj_set_style_pad_row(main_row, 8, 0);
    lv_obj_remove_flag(main_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *calc_col = lv_obj_create(main_row);
    lv_obj_remove_style_all(calc_col);
    lv_obj_set_height(calc_col, wide ? LV_PCT(100) : LV_SIZE_CONTENT);
    if (wide) lv_obj_set_flex_grow(calc_col, 3);
    else { lv_obj_set_width(calc_col, LV_PCT(100)); lv_obj_set_flex_grow(calc_col, 1); }
    lv_obj_set_flex_flow(calc_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(calc_col, 8, 0);
    lv_obj_remove_flag(calc_col, LV_OBJ_FLAG_SCROLLABLE);

    /* Ekran */
    lv_obj_t *scr = lv_obj_create(calc_col);
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, LV_PCT(100), 82);
    lv_obj_set_style_bg_color(scr, lv_color_hex(c_surf), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(scr, 10, 0);
    lv_obj_set_style_border_width(scr, 1, 0);
    lv_obj_set_style_border_color(scr, lv_color_hex(g_light ? 0xCBD5E1 : 0x2C313B), 0);
    lv_obj_set_style_pad_hor(scr, 14, 0);
    lv_obj_set_style_pad_ver(scr, 10, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *elbl = lv_label_create(scr);
    lv_label_set_text(elbl, g_expr);
    lv_label_set_long_mode(elbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(elbl, LV_PCT(100));
    lv_obj_set_style_text_align(elbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(elbl, lv_color_hex(c_dim), 0);
    if (f13) lv_obj_set_style_text_font(elbl, f13, 0);

    lv_obj_t *dlbl = lv_label_create(scr);
    lv_label_set_text(dlbl, g_disp);
    lv_label_set_long_mode(dlbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(dlbl, LV_PCT(100));
    lv_obj_set_style_text_align(dlbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(dlbl, lv_color_hex(c_txt), 0);
    if (f28) lv_obj_set_style_text_font(dlbl, f28, 0);

    /* Bilimsel tuş satırları */
    if (g_sci) {
        static const char *const SCI[2][5] = {
            { "sin", "cos", "tan", "√", "xʸ" },
            { "ln",  "log", "π",   "x²", "1/x" }
        };
        for (int r = 0; r < 2; r++) {
            lv_obj_t *row = lv_obj_create(calc_col);
            lv_obj_remove_style_all(row);
            lv_obj_set_size(row, LV_PCT(100), 36);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_style_pad_column(row, 6, 0);
            lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            for (int c = 0; c < 5; c++)
                mk_btn(row, SCI[r][c], c_fn, c_opfg, SCI[r][c], f13);
        }
    }

    /* Standart tuş takımı (5 satır x 4 sütun) */
    static const char *const PAD[5][4] = {
        { "CE", "C",  "⌫", "÷" },
        { "7",  "8",  "9",  "×" },
        { "4",  "5",  "6",  "−" },
        { "1",  "2",  "3",  "+" },
        { "%",  "0",  ".",  "=" }
    };
    for (int r = 0; r < 5; r++) {
        lv_obj_t *row = lv_obj_create(calc_col);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_flex_grow(row, 1);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, 6, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        for (int c = 0; c < 4; c++) {
            const char *t = PAD[r][c];
            int is_eq = seq0(t, "=");
            int is_op = (c == 3 && !is_eq) || (r == 0);
            uint32_t bg = is_eq ? c_acc : (is_op ? c_op : c_num);
            uint32_t fg = is_eq ? 0xFFFFFF : (c == 3 ? c_opfg : c_txt);
            mk_btn(row, t, bg, fg, t, f15);
        }
    }

    /* Geçmiş Şeridi */
    lv_obj_t *hpanel = lv_obj_create(main_row);
    lv_obj_remove_style_all(hpanel);
    lv_obj_set_style_bg_color(hpanel, lv_color_hex(c_surf), 0);
    lv_obj_set_style_bg_opa(hpanel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hpanel, 10, 0);
    lv_obj_set_style_border_width(hpanel, 1, 0);
    lv_obj_set_style_border_color(hpanel, lv_color_hex(g_light ? 0xCBD5E1 : 0x2C313B), 0);
    lv_obj_set_style_pad_all(hpanel, 8, 0);
    lv_obj_set_style_pad_row(hpanel, 4, 0);
    lv_obj_set_flex_flow(hpanel, LV_FLEX_FLOW_COLUMN);
    if (wide) {
        lv_obj_set_height(hpanel, LV_PCT(100));
        lv_obj_set_flex_grow(hpanel, 2);
    } else {
        lv_obj_set_size(hpanel, LV_PCT(100), 92);
    }
    lv_obj_remove_flag(hpanel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *htop = lv_obj_create(hpanel);
    lv_obj_remove_style_all(htop);
    lv_obj_set_size(htop, LV_PCT(100), 22);
    lv_obj_set_flex_flow(htop, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(htop, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(htop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *ht = lv_label_create(htop);
    lv_label_set_text(ht, "Geçmiş");
    lv_obj_set_style_text_color(ht, lv_color_hex(c_dim), 0);
    if (f12) lv_obj_set_style_text_font(ht, f12, 0);

    lv_obj_t *clr = lv_button_create(htop);
    lv_obj_set_size(clr, LV_SIZE_CONTENT, 20);
    lv_obj_set_style_bg_opa(clr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(clr, 0, 0);
    lv_obj_set_style_shadow_width(clr, 0, 0);
    lv_obj_set_style_pad_hor(clr, 6, 0);
    lv_obj_t *clrl = lv_label_create(clr);
    lv_label_set_text(clrl, "Temizle");
    lv_obj_set_style_text_color(clrl, lv_color_hex(c_acc), 0);
    if (f12) lv_obj_set_style_text_font(clrl, f12, 0);
    lv_obj_add_event_cb(clr, clear_hist_cb, LV_EVENT_CLICKED, 0);

    lv_obj_t *hlst = lv_obj_create(hpanel);
    lv_obj_remove_style_all(hlst);
    lv_obj_set_width(hlst, LV_PCT(100));
    lv_obj_set_flex_grow(hlst, 1);
    lv_obj_set_flex_flow(hlst, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(hlst, 2, 0);
    lv_obj_set_scroll_dir(hlst, LV_DIR_VER);

    if (g_hist_n == 0) {
        lv_obj_t *emp = lv_label_create(hlst);
        lv_label_set_text(emp, "Henüz işlem yok");
        lv_obj_set_style_text_color(emp, lv_color_hex(c_dim), 0);
        if (f12) lv_obj_set_style_text_font(emp, f12, 0);
    } else {
        for (int i = g_hist_n - 1; i >= 0; i--) {
            lv_obj_t *hb = lv_button_create(hlst);
            lv_obj_set_size(hb, LV_PCT(100), 22);
            lv_obj_set_style_radius(hb, 4, 0);
            lv_obj_set_style_bg_opa(hb, LV_OPA_TRANSP, 0);
            lv_obj_set_style_bg_color(hb, lv_color_hex(c_fn), LV_STATE_HOVERED);
            lv_obj_set_style_bg_opa(hb, LV_OPA_COVER, LV_STATE_HOVERED);
            lv_obj_set_style_border_width(hb, 0, 0);
            lv_obj_set_style_shadow_width(hb, 0, 0);
            lv_obj_set_style_pad_hor(hb, 4, 0);
            char line[104];
            scpy(line, sizeof(line), g_hist[i].expr);
            scat(line, sizeof(line), " = ");
            scat(line, sizeof(line), g_hist[i].res);
            lv_obj_t *hl2 = lv_label_create(hb);
            lv_label_set_text(hl2, line);
            lv_label_set_long_mode(hl2, LV_LABEL_LONG_DOT);
            lv_obj_set_width(hl2, LV_PCT(100));
            lv_obj_set_style_text_align(hl2, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_style_text_color(hl2, lv_color_hex(c_txt), 0);
            if (f12) lv_obj_set_style_text_font(hl2, f12, 0);
            lv_obj_align(hl2, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_obj_add_event_cb(hb, pick_hist_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
    }
}

static void calc_relayout(lv_obj_t *win) {
    (void)win;
    build_ui();
}

static void app_del_cb(lv_event_t *e) {
    (void)e;
    if (g_grp) {
        lv_indev_t *kp = lv_port_get_keypad();
        if (kp && lv_indev_get_group(kp) == g_grp) lv_indev_set_group(kp, 0);
        lv_group_delete(g_grp);
        g_grp = 0;
    }
    g_win = 0;
}

void app_entry(lv_obj_t *win, asi_t *asi) {
    A = asi;
    g_win = win;

    if (!f28) f28 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 28);
    if (!f15) f15 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 15);
    if (!f13) f13 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 13);
    if (!f12) f12 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 12);

    lv_obj_remove_flag(win, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(win, key_cb, LV_EVENT_KEY, 0);

    g_grp = lv_group_create();
    if (g_grp) {
        lv_group_add_obj(g_grp, win);
        lv_group_focus_obj(win);
        lv_indev_t *kp = lv_port_get_keypad();
        if (kp) lv_indev_set_group(kp, g_grp);
    }

    build_ui();
    arctian_shell_set_relayout(win, calc_relayout);
    lv_obj_add_event_cb(win, app_del_cb, LV_EVENT_DELETE, 0);
}
