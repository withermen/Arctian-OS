/* ==================================================================
 * Metin Düzenleyici
 *
 * Özellikler:
 *   - Çoklu sekme, Yeni / Aç / Kaydet (Ctrl+S) / Farklı Kaydet
 *   - 5 saniyelik otomatik kaydetme ve kirli sekme göstergesi (*)
 *   - Geri Al / Yinele (Ctrl+Z / Ctrl+Y) + Pano (Ctrl+C/X/V/A)
 *   - Bul & Değiştir çubuğu (Ctrl+F)
 *   - relayout desteği ile pencere boyutlandırma uyumu
 * ================================================================== */
#include "app.h"
#include "adl_wolf.h"
#include "lv_port.h"
#include "src/widgets/textarea/lv_textarea_private.h"
#include <stdint.h>

#define MT_MAX       12
#define MT_PATH      200
#define MT_NAME      64
#define MT_MAXBYTES  65536
#define MT_UNDO_MAX  24
#define MT_SNAP_MAX  16384

#define C_BODY   0x14161B
#define C_BAR    0x181B22
#define C_BTN    0x232833
#define C_LINE   0x2A2E37
#define C_DIV    0x2C313B
#define C_TABON  0x1A1D24
#define C_ACC    0x3B82F6
#define C_TXT    0xCDD3DE
#define C_BRIGHT 0xE6E8EC
#define C_DIM    0x8B919C
#define C_GUT    0x4A505C
#define C_STAT   0x7D8491
#define C_OK     0x34D399
#define C_WARN   0xFBBF24

static asi_t      *A;
static lv_obj_t   *g_win;
static lv_obj_t   *g_tabs;
static lv_obj_t   *g_find_bar;
static lv_obj_t   *g_find_ta, *g_repl_ta, *g_find_st;
static lv_obj_t   *g_gutter;
static lv_obj_t   *g_ta;
static lv_obj_t   *g_st_l, *g_st_m, *g_st_r;
static lv_obj_t   *g_dlg;
static lv_obj_t   *g_dlg_ta;
static lv_obj_t   *g_tab_btns[MT_MAX];
static lv_obj_t   *g_tab_lbls[MT_MAX];
static lv_group_t *g_grp;
static lv_timer_t *g_auto_timer;

static char  g_path[MT_MAX][MT_PATH];
static char  g_name[MT_MAX][MT_NAME];
static char *g_buf[MT_MAX];
static int   g_dirty[MT_MAX];
static char *g_last[MT_MAX];
static char *g_undo[MT_MAX][MT_UNDO_MAX];
static char *g_redo[MT_MAX][MT_UNDO_MAX];
static int   g_undo_n[MT_MAX];
static int   g_redo_n[MT_MAX];
static int   g_n, g_sel;
static int   g_guard;
static int   g_untitled_seq;
static int   g_dlg_mode; /* 1=Aç, 2=Farklı Kaydet */

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

static int seq0(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return !*a && !*b;
}

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
    static const char *const ex[8] = { "md", "txt", "json", "cfg", "ini", "log", "csv", "sh" };
    for (int i = 0; i < 8; i++) if (ext_is(name, ex[i])) return 1;
    return 0;
}

static void join_path(char *out, int cap, const char *dir, const char *name) {
    scpy(out, cap, dir);
    int L = slen(out);
    if (L == 0) { scpy(out, cap, name); return; }
    if (out[L - 1] != '/' && out[L - 1] != ':') scat(out, cap, "/");
    scat(out, cap, name);
}

static void split_path(const char *path, char *dir, int dcap, char *name, int ncap) {
    int last = -1;
    for (int i = 0; path[i]; i++) if (path[i] == '/') last = i;
    if (last < 0) {
        scpy(dir, dcap, "C:");
        scpy(name, ncap, path);
    } else {
        int di = 0;
        for (int i = 0; i < last && di < dcap - 1; i++) dir[di++] = path[i];
        dir[di] = 0;
        if (!dir[0]) scpy(dir, dcap, "C:");
        scpy(name, ncap, path + last + 1);
    }
}

static char *dup_str(const char *s, int max_bytes) {
    int n = slen(s);
    if (max_bytes > 0 && n > max_bytes) n = max_bytes;
    char *p = (char *)A->mem->alloc((size_t)n + 1u);
    if (!p) return 0;
    for (int i = 0; i < n; i++) p[i] = s[i];
    p[n] = 0;
    return p;
}

static void set_msg(const char *msg, uint32_t col) {
    if (!g_st_m) return;
    lv_label_set_text(g_st_m, msg ? msg : "");
    lv_obj_set_style_text_color(g_st_m, lv_color_hex(col), 0);
}

static void sync_gutter_scroll(void) {
    if (!g_gutter || !g_ta) return;
    int32_t sy = lv_obj_get_scroll_y(g_ta);
    lv_obj_set_pos(g_gutter, 0, 14 - sy);
}

static void paint_gutter(void) {
    if (!g_gutter || !g_ta) return;
    const char *t = lv_textarea_get_text(g_ta);
    int lines = 1;
    if (t) for (int i = 0; t[i]; i++) if (t[i] == '\n') lines++;
    if (lines > 400) lines = 400;
    static char buf[400 * 5];
    int k = 0;
    for (int i = 1; i <= lines; i++) {
        char nb[8];
        u2s((unsigned int)i, nb, sizeof(nb));
        for (int j = 0; nb[j] && k < (int)sizeof(buf) - 2; j++) buf[k++] = nb[j];
        if (i < lines) buf[k++] = '\n';
    }
    buf[k] = 0;
    lv_label_set_text(g_gutter, buf);
    sync_gutter_scroll();
}

static void paint_status(void) {
    if (!g_st_l || !g_ta) return;
    const char *t = lv_textarea_get_text(g_ta);
    uint32_t pos = lv_textarea_get_cursor_pos(g_ta);
    int line = 1, col = 1;
    uint32_t cp_idx = 0;
    for (uint32_t i = 0; t && t[i] && cp_idx < pos; i++) {
        if (((unsigned char)t[i] & 0xC0) != 0x80) {
            if (t[i] == '\n') { line++; col = 1; }
            else col++;
            cp_idx++;
        }
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

static void update_right_status(void) {
    if (!g_st_r) return;
    if (g_sel < 0 || g_sel >= g_n) {
        lv_label_set_text(g_st_r, "UTF-8");
        return;
    }
    const char *nm = g_name[g_sel];
    const char *fmt = ext_is(nm, "md") ? "Markdown"
                    : ext_is(nm, "json") ? "JSON"
                    : ext_is(nm, "ini") || ext_is(nm, "cfg") ? "Yapılandırma"
                    : "Düz Metin";
    char r[64];
    scpy(r, sizeof(r), "UTF-8 · ");
    scat(r, sizeof(r), fmt);
    if (g_dirty[g_sel]) scat(r, sizeof(r), " · Kaydedilmedi *");
    lv_label_set_text(g_st_r, r);
}

static void paint_tabs(void) {
    for (int i = 0; i < g_n; i++) {
        if (!g_tab_btns[i]) continue;
        int on = (i == g_sel);
        lv_obj_set_style_bg_color(g_tab_btns[i], lv_color_hex(on ? C_TABON : C_BODY), 0);
        lv_obj_set_style_border_width(g_tab_btns[i], on ? 2 : 0, 0);
        lv_obj_set_style_border_color(g_tab_btns[i], lv_color_hex(C_ACC), 0);
        lv_obj_set_style_border_side(g_tab_btns[i], LV_BORDER_SIDE_BOTTOM, 0);
        if (g_tab_lbls[i]) {
            char tl[MT_NAME + 4];
            if (g_dirty[i]) {
                scpy(tl, sizeof(tl), "* ");
                scat(tl, sizeof(tl), g_name[i]);
            } else {
                scpy(tl, sizeof(tl), g_name[i]);
            }
            lv_label_set_text(g_tab_lbls[i], tl);
            lv_obj_set_style_text_color(g_tab_lbls[i],
                lv_color_hex(on ? C_BRIGHT : (g_dirty[i] ? C_WARN : C_DIM)), 0);
        }
    }
}

static void free_tab_stacks(int idx) {
    if (idx < 0 || idx >= MT_MAX) return;
    if (g_buf[idx])  { A->mem->free(g_buf[idx]);  g_buf[idx] = 0; }
    if (g_last[idx]) { A->mem->free(g_last[idx]); g_last[idx] = 0; }
    for (int i = 0; i < g_undo_n[idx]; i++) { if (g_undo[idx][i]) A->mem->free(g_undo[idx][i]); g_undo[idx][i] = 0; }
    for (int i = 0; i < g_redo_n[idx]; i++) { if (g_redo[idx][i]) A->mem->free(g_redo[idx][i]); g_redo[idx][i] = 0; }
    g_undo_n[idx] = 0;
    g_redo_n[idx] = 0;
}

static void push_stack(char *stk[], int *cnt, const char *txt) {
    if (*cnt >= MT_UNDO_MAX) {
        if (stk[0]) A->mem->free(stk[0]);
        for (int i = 1; i < *cnt; i++) stk[i - 1] = stk[i];
        (*cnt)--;
    }
    char *s = dup_str(txt ? txt : "", MT_SNAP_MAX);
    if (s) stk[(*cnt)++] = s;
}

static void sync_cur_buf(void) {
    if (g_sel < 0 || g_sel >= g_n || !g_ta) return;
    const char *txt = lv_textarea_get_text(g_ta);
    char *nb = dup_str(txt ? txt : "", MT_MAXBYTES);
    if (nb) {
        if (g_buf[g_sel]) A->mem->free(g_buf[g_sel]);
        g_buf[g_sel] = nb;
    }
}

static char *s_read_file(const char *path) {
    if (!A->fs || !A->fs->read_path || !A->mem || !path || !path[0]) return 0;
    uint8_t *b = (uint8_t *)A->mem->alloc(MT_MAXBYTES + 1);
    if (!b) return 0;
    uint32_t rd = A->fs->read_path(path, b, MT_MAXBYTES);
    b[rd] = 0;
    return (char *)b;
}

static void load_sel(void) {
    if (g_sel < 0 || g_sel >= g_n || !g_ta) return;
    if (!g_buf[g_sel] && g_path[g_sel][0]) {
        g_buf[g_sel] = s_read_file(g_path[g_sel]);
    }
    const char *txt = g_buf[g_sel] ? g_buf[g_sel] : "";
    if (!g_last[g_sel]) g_last[g_sel] = dup_str(txt, MT_SNAP_MAX);

    g_guard = 1;
    lv_textarea_set_text(g_ta, txt);
    g_guard = 0;

    paint_tabs();
    paint_gutter();
    paint_status();
    update_right_status();
}

static void rebuild_tabs(void);

static int write_tab_to_path(int idx, const char *path, int manual) {
    if (idx < 0 || idx >= g_n || !path || !path[0]) return 0;
    if (!A->fs || !A->fs->find || !A->fs->write) {
        set_msg("Dosya sistemi yok", C_WARN);
        return 0;
    }
    if (idx == g_sel) sync_cur_buf();
    const char *txt = g_buf[idx] ? g_buf[idx] : "";
    uint32_t len = (uint32_t)slen(txt);

    void *h = A->fs->find(0, path);
    if (!h) {
        char dir[MT_PATH], nm[MT_NAME];
        split_path(path, dir, sizeof(dir), nm, sizeof(nm));
        if (!nm[0]) { set_msg("Geçersiz dosya adı", C_WARN); return 0; }
        if (A->fs->mkdir && dir[0] && !seq0(dir, "C:")) A->fs->mkdir(dir);
        h = A->fs->create_in ? A->fs->create_in(dir, nm, 0) : 0;
    }
    if (!h) {
        set_msg("Dosya oluşturulamadı", C_WARN);
        return 0;
    }
    if (A->fs->write(h, txt, len) != 0) {
        set_msg("Yazma hatası", C_WARN);
        return 0;
    }
    g_dirty[idx] = 0;
    paint_tabs();
    if (idx == g_sel) {
        update_right_status();
        set_msg(manual ? "Kaydedildi" : "Otomatik kaydedildi", C_OK);
    }
    return 1;
}

static void close_dlg(void);
static void open_save_as_dlg(void);

static void save_current(int manual) {
    if (g_sel < 0 || g_sel >= g_n) return;
    if (!g_path[g_sel][0]) {
        if (manual) open_save_as_dlg();
        return;
    }
    write_tab_to_path(g_sel, g_path[g_sel], manual);
}

static void do_undo(int redo) {
    if (g_sel < 0 || g_sel >= g_n || !g_ta) return;
    const char *cur = lv_textarea_get_text(g_ta);
    if (redo) {
        if (g_redo_n[g_sel] <= 0) { set_msg("Yinelenecek değişiklik yok", C_DIM); return; }
        push_stack(g_undo[g_sel], &g_undo_n[g_sel], cur);
        char *top = g_redo[g_sel][--g_redo_n[g_sel]];
        g_guard = 1;
        lv_textarea_set_text(g_ta, top);
        g_guard = 0;
        if (g_last[g_sel]) A->mem->free(g_last[g_sel]);
        g_last[g_sel] = dup_str(top, MT_SNAP_MAX);
        A->mem->free(top);
        set_msg("Yinelendi", C_DIM);
    } else {
        if (g_undo_n[g_sel] <= 0) { set_msg("Geri alınacak değişiklik yok", C_DIM); return; }
        push_stack(g_redo[g_sel], &g_redo_n[g_sel], cur);
        char *top = g_undo[g_sel][--g_undo_n[g_sel]];
        g_guard = 1;
        lv_textarea_set_text(g_ta, top);
        g_guard = 0;
        if (g_last[g_sel]) A->mem->free(g_last[g_sel]);
        g_last[g_sel] = dup_str(top, MT_SNAP_MAX);
        A->mem->free(top);
        set_msg("Geri alındı", C_DIM);
    }
    g_dirty[g_sel] = 1;
    sync_cur_buf();
    paint_tabs();
    paint_gutter();
    paint_status();
    update_right_status();
}

static void on_text_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SCROLL) { sync_gutter_scroll(); return; }
    if (code == LV_EVENT_VALUE_CHANGED && !g_guard && g_sel >= 0 && g_sel < g_n) {
        const char *cur = lv_textarea_get_text(g_ta);
        push_stack(g_undo[g_sel], &g_undo_n[g_sel], g_last[g_sel] ? g_last[g_sel] : "");
        if (g_last[g_sel]) A->mem->free(g_last[g_sel]);
        g_last[g_sel] = dup_str(cur ? cur : "", MT_SNAP_MAX);
        for (int i = 0; i < g_redo_n[g_sel]; i++) {
            if (g_redo[g_sel][i]) A->mem->free(g_redo[g_sel][i]);
            g_redo[g_sel][i] = 0;
        }
        g_redo_n[g_sel] = 0;
        g_dirty[g_sel] = 1;
        sync_cur_buf();
        paint_tabs();
        update_right_status();
        set_msg("", C_DIM);
    }
    paint_gutter();
    paint_status();
}

static void focus_obj(lv_obj_t *obj) {
    if (!obj || !g_grp) return;
    if (!lv_obj_get_group(obj)) lv_group_add_obj(g_grp, obj);
    lv_indev_t *kp = lv_port_get_keypad();
    if (kp) lv_indev_set_group(kp, g_grp);
    lv_group_focus_obj(obj);
}

static void ta_click_focus_cb(lv_event_t *e) {
    focus_obj(lv_event_get_target(e));
}

static void tab_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= g_n || idx == g_sel) return;
    sync_cur_buf();
    g_sel = idx;
    load_sel();
    focus_obj(g_ta);
}

static void new_tab(void) {
    if (g_n >= MT_MAX) { set_msg("En fazla 12 sekme açılabilir", C_WARN); return; }
    sync_cur_buf();
    int idx = g_n++;
    g_path[idx][0] = 0;
    g_untitled_seq++;
    char nb[8];
    u2s((unsigned int)g_untitled_seq, nb, sizeof(nb));
    scpy(g_name[idx], MT_NAME, "Adsız ");
    scat(g_name[idx], MT_NAME, nb);
    g_dirty[idx] = 0;
    g_buf[idx] = dup_str("", 4);
    g_last[idx] = dup_str("", 4);
    g_sel = idx;
    rebuild_tabs();
    load_sel();
    focus_obj(g_ta);
    set_msg("Yeni dosya", C_DIM);
}

static void close_tab_idx(int idx) {
    if (idx < 0 || idx >= g_n) return;
    free_tab_stacks(idx);
    for (int i = idx; i < g_n - 1; i++) {
        scpy(g_path[i], MT_PATH, g_path[i + 1]);
        scpy(g_name[i], MT_NAME, g_name[i + 1]);
        g_buf[i] = g_buf[i + 1];
        g_dirty[i] = g_dirty[i + 1];
        g_last[i] = g_last[i + 1];
        g_undo_n[i] = g_undo_n[i + 1];
        g_redo_n[i] = g_redo_n[i + 1];
        for (int k = 0; k < MT_UNDO_MAX; k++) {
            g_undo[i][k] = g_undo[i + 1][k];
            g_redo[i][k] = g_redo[i + 1][k];
        }
    }
    g_n--;
    if (g_n >= 0 && g_n < MT_MAX) {
        g_buf[g_n] = 0; g_last[g_n] = 0;
        g_undo_n[g_n] = g_redo_n[g_n] = 0;
        g_dirty[g_n] = 0;
    }
    if (g_n == 0) {
        g_sel = -1;
        new_tab();
        return;
    }
    if (g_sel >= g_n) g_sel = g_n - 1;
    rebuild_tabs();
    load_sel();
    focus_obj(g_ta);
}

static void tab_close_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    close_tab_idx(idx);
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
        lv_obj_set_style_pad_left(b, 12, 0);
        lv_obj_set_style_pad_right(b, 6, 0);
        lv_obj_set_style_pad_column(b, 6, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, g_name[i]);
        if (f13) lv_obj_set_style_text_font(l, f13, 0);

        lv_obj_t *x = lv_button_create(b);
        lv_obj_set_size(x, 18, 18);
        lv_obj_set_style_radius(x, 4, 0);
        lv_obj_set_style_bg_opa(x, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(x, lv_color_hex(C_BTN), LV_STATE_HOVERED);
        lv_obj_set_style_bg_opa(x, LV_OPA_COVER, LV_STATE_HOVERED);
        lv_obj_set_style_border_width(x, 0, 0);
        lv_obj_set_style_shadow_width(x, 0, 0);
        lv_obj_set_style_pad_all(x, 0, 0);
        lv_obj_t *xl = lv_label_create(x);
        lv_label_set_text(xl, "×");
        lv_obj_set_style_text_color(xl, lv_color_hex(C_DIM), 0);
        if (f12) lv_obj_set_style_text_font(xl, f12, 0);
        lv_obj_center(xl);
        lv_obj_add_event_cb(x, tab_close_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        g_tab_btns[i] = b;
        g_tab_lbls[i] = l;
    }
    paint_tabs();
}

/* ---- Bul & Değiştir ---- */
static uint32_t byte_to_cp(const char *s, uint32_t byte_off) {
    uint32_t cp = 0;
    for (uint32_t i = 0; i < byte_off && s[i]; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) cp++;
    }
    return cp;
}

static uint32_t cp_to_byte(const char *s, uint32_t cp_off) {
    uint32_t cp = 0, i = 0;
    for (; s[i] && cp < cp_off; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) cp++;
    }
    while (s[i] && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i;
}

static int find_substr_from(const char *hay, const char *needle, uint32_t start_b) {
    int nl = slen(needle);
    int hl = slen(hay);
    if (nl <= 0 || hl < nl) return -1;
    for (int i = (int)start_b; i <= hl - nl; i++) {
        int ok = 1;
        for (int j = 0; j < nl; j++) if (hay[i + j] != needle[j]) { ok = 0; break; }
        if (ok) return i;
    }
    return -1;
}

static void select_range_cp(lv_obj_t *ta, uint32_t cp_start, uint32_t cp_end) {
    lv_textarea_set_text_selection(ta, true);
    lv_textarea_set_cursor_pos(ta, (int32_t)cp_end);
    lv_textarea_t *t = (lv_textarea_t *)ta;
    t->sel_start = cp_start;
    t->sel_end = cp_end;
    lv_obj_t *lab = lv_obj_get_child_count(ta) > 0 ? lv_obj_get_child(ta, 0) : 0;
    if (lab) {
        lv_label_set_text_selection_start(lab, cp_start);
        lv_label_set_text_selection_end(lab, cp_end);
    }
    lv_obj_invalidate(ta);
}

static void do_find_next(int from_start) {
    if (!g_find_ta || !g_ta) return;
    const char *q = lv_textarea_get_text(g_find_ta);
    const char *txt = lv_textarea_get_text(g_ta);
    if (!q || !q[0]) {
        if (g_find_st) lv_label_set_text(g_find_st, "");
        return;
    }
    int total = 0, match_idx = 0;
    uint32_t q_cp = byte_to_cp(q, (uint32_t)slen(q));
    uint32_t cur_cp = from_start ? 0 : lv_textarea_get_cursor_pos(g_ta);
    uint32_t cur_b = cp_to_byte(txt, cur_cp);

    int pos = find_substr_from(txt, q, cur_b);
    if (pos < 0 && cur_b > 0) pos = find_substr_from(txt, q, 0);

    for (uint32_t off = 0; ; ) {
        int f = find_substr_from(txt, q, off);
        if (f < 0) break;
        total++;
        if (f == pos) match_idx = total;
        off = (uint32_t)(f + slen(q));
    }

    if (pos >= 0) {
        uint32_t s_cp = byte_to_cp(txt, (uint32_t)pos);
        select_range_cp(g_ta, s_cp, s_cp + q_cp);
        paint_status();
        if (g_find_st) {
            char sb[24], nb[8];
            u2s((unsigned int)match_idx, nb, sizeof(nb)); scpy(sb, sizeof(sb), nb);
            scat(sb, sizeof(sb), "/");
            u2s((unsigned int)total, nb, sizeof(nb)); scat(sb, sizeof(sb), nb);
            lv_label_set_text(g_find_st, sb);
        }
    } else {
        if (g_find_st) lv_label_set_text(g_find_st, "Bulunamadı");
    }
}

static void find_btn_cb(lv_event_t *e) { (void)e; do_find_next(0); }

static void replace_one_cb(lv_event_t *e) {
    (void)e;
    if (!g_find_ta || !g_repl_ta || !g_ta || g_sel < 0) return;
    const char *q = lv_textarea_get_text(g_find_ta);
    const char *rep = lv_textarea_get_text(g_repl_ta);
    if (!q || !q[0]) return;
    if (!rep) rep = "";

    const char *txt = lv_textarea_get_text(g_ta);
    uint32_t cur_cp = lv_textarea_get_cursor_pos(g_ta);
    uint32_t cur_b = cp_to_byte(txt, cur_cp);
    int ql = slen(q), rl = slen(rep), hl = slen(txt);

    /* Seçili aralık q ile eşleşiyorsa orayı değiştir, değilse bir sonrakini bul */
    lv_textarea_t *t = (lv_textarea_t *)g_ta;
    uint32_t a_cp = t->sel_start, b_cp = t->sel_end;
    if (a_cp > b_cp) { uint32_t tmp = a_cp; a_cp = b_cp; b_cp = tmp; }
    uint32_t a_b = cp_to_byte(txt, a_cp), b_b = cp_to_byte(txt, b_cp);
    int pos = -1;
    if ((int)(b_b - a_b) == ql) {
        int ok = 1;
        for (int j = 0; j < ql; j++) if (txt[a_b + j] != q[j]) { ok = 0; break; }
        if (ok) pos = (int)a_b;
    }
    if (pos < 0) {
        do_find_next(0);
        return;
    }
    int new_len = hl - ql + rl;
    if (new_len > MT_MAXBYTES) { set_msg("Boyut sınırı aşıldı", C_WARN); return; }
    char *nb = (char *)A->mem->alloc((size_t)new_len + 1u);
    if (!nb) return;
    for (int i = 0; i < pos; i++) nb[i] = txt[i];
    for (int i = 0; i < rl; i++) nb[pos + i] = rep[i];
    for (int i = pos + ql; i <= hl; i++) nb[pos + rl + (i - (pos + ql))] = txt[i];

    push_stack(g_undo[g_sel], &g_undo_n[g_sel], txt);
    g_guard = 1;
    lv_textarea_set_text(g_ta, nb);
    g_guard = 0;
    if (g_last[g_sel]) A->mem->free(g_last[g_sel]);
    g_last[g_sel] = dup_str(nb, MT_SNAP_MAX);
    lv_textarea_set_cursor_pos(g_ta, (int32_t)byte_to_cp(nb, (uint32_t)(pos + rl)));
    A->mem->free(nb);
    g_dirty[g_sel] = 1;
    sync_cur_buf();
    paint_tabs();
    paint_gutter();
    paint_status();
    update_right_status();
    do_find_next(0);
    (void)cur_b;
}

static void replace_all_cb(lv_event_t *e) {
    (void)e;
    if (!g_find_ta || !g_repl_ta || !g_ta || g_sel < 0) return;
    const char *q = lv_textarea_get_text(g_find_ta);
    const char *rep = lv_textarea_get_text(g_repl_ta);
    if (!q || !q[0]) return;
    if (!rep) rep = "";
    const char *txt = lv_textarea_get_text(g_ta);
    int ql = slen(q), rl = slen(rep), hl = slen(txt);

    char *nb = (char *)A->mem->alloc(MT_MAXBYTES + 1u);
    if (!nb) return;
    int di = 0, si = 0, cnt = 0;
    while (si < hl) {
        int m = (si <= hl - ql);
        if (m) {
            for (int j = 0; j < ql; j++) if (txt[si + j] != q[j]) { m = 0; break; }
        }
        if (m) {
            if (di + rl >= MT_MAXBYTES) break;
            for (int j = 0; j < rl; j++) nb[di++] = rep[j];
            si += ql;
            cnt++;
        } else {
            if (di >= MT_MAXBYTES) break;
            nb[di++] = txt[si++];
        }
    }
    nb[di] = 0;
    if (cnt > 0) {
        push_stack(g_undo[g_sel], &g_undo_n[g_sel], txt);
        g_guard = 1;
        lv_textarea_set_text(g_ta, nb);
        g_guard = 0;
        if (g_last[g_sel]) A->mem->free(g_last[g_sel]);
        g_last[g_sel] = dup_str(nb, MT_SNAP_MAX);
        g_dirty[g_sel] = 1;
        sync_cur_buf();
        paint_tabs();
        paint_gutter();
        paint_status();
        update_right_status();
    }
    A->mem->free(nb);
    if (g_find_st) {
        char sb[24], num[8];
        u2s((unsigned int)cnt, num, sizeof(num));
        scpy(sb, sizeof(sb), num);
        scat(sb, sizeof(sb), " değişti");
        lv_label_set_text(g_find_st, sb);
    }
}

static void toggle_find_bar(int show) {
    if (!g_find_bar) return;
    int vis = !lv_obj_has_flag(g_find_bar, LV_OBJ_FLAG_HIDDEN);
    if (show < 0) show = !vis;
    if (show) {
        lv_obj_remove_flag(g_find_bar, LV_OBJ_FLAG_HIDDEN);
        focus_obj(g_find_ta);
    } else {
        lv_obj_add_flag(g_find_bar, LV_OBJ_FLAG_HIDDEN);
        focus_obj(g_ta);
    }
}

static void find_toggle_cb(lv_event_t *e) { (void)e; toggle_find_bar(-1); }
static void find_close_cb(lv_event_t *e) { (void)e; toggle_find_bar(0); }

/* ---- Aç & Farklı Kaydet Diyalogları ---- */
static void close_dlg(void) {
    if (g_dlg) { lv_obj_delete(g_dlg); g_dlg = 0; g_dlg_ta = 0; }
    g_dlg_mode = 0;
    focus_obj(g_ta);
}

static void dlg_cancel_cb(lv_event_t *e) { (void)e; close_dlg(); }

static void open_or_switch_path(const char *path) {
    if (!path || !path[0]) return;
    for (int i = 0; i < g_n; i++) {
        if (seq0(g_path[i], path)) {
            sync_cur_buf();
            g_sel = i;
            load_sel();
            focus_obj(g_ta);
            return;
        }
    }
    if (g_n >= MT_MAX) { set_msg("En fazla 12 sekme açılabilir", C_WARN); return; }
    sync_cur_buf();
    char dir[MT_PATH], nm[MT_NAME];
    split_path(path, dir, sizeof(dir), nm, sizeof(nm));
    int idx = g_n++;
    scpy(g_path[idx], MT_PATH, path);
    scpy(g_name[idx], MT_NAME, nm[0] ? nm : path);
    g_dirty[idx] = 0;
    g_buf[idx] = s_read_file(path);
    if (!g_buf[idx]) g_buf[idx] = dup_str("", 4);
    g_last[idx] = dup_str(g_buf[idx], MT_SNAP_MAX);
    g_sel = idx;
    rebuild_tabs();
    load_sel();
    focus_obj(g_ta);
}

static void dlg_ok_cb(lv_event_t *e) {
    (void)e;
    if (!g_dlg_ta) { close_dlg(); return; }
    const char *p = lv_textarea_get_text(g_dlg_ta);
    char path[MT_PATH];
    scpy(path, sizeof(path), p ? p : "");
    if (!path[0]) return;

    if (g_dlg_mode == 1) {
        close_dlg();
        open_or_switch_path(path);
    } else if (g_dlg_mode == 2 && g_sel >= 0 && g_sel < g_n) {
        char dir[MT_PATH], nm[MT_NAME];
        split_path(path, dir, sizeof(dir), nm, sizeof(nm));
        scpy(g_path[g_sel], MT_PATH, path);
        scpy(g_name[g_sel], MT_NAME, nm[0] ? nm : path);
        close_dlg();
        write_tab_to_path(g_sel, g_path[g_sel], 1);
        rebuild_tabs();
        update_right_status();
    }
}

static void dlg_pick_file_cb(lv_event_t *e) {
    const char *path = (const char *)lv_event_get_user_data(e);
    if (!path) return;
    char copy[MT_PATH];
    scpy(copy, sizeof(copy), path);
    close_dlg();
    open_or_switch_path(copy);
}

static char s_pick_paths[32][MT_PATH];
static int  s_pick_n;

static void collect_dir_for_dlg(const char *dir) {
    if (!A->fs || !A->fs->count_dir || !A->fs->list_dir) return;
    int n = A->fs->count_dir(dir);
    for (int i = 0; i < n && s_pick_n < 32; i++) {
        char nm[MT_NAME];
        int isd = 0;
        if (A->fs->list_dir(dir, i, nm, sizeof(nm), &isd) != 0 || isd) continue;
        char full[MT_PATH];
        join_path(full, sizeof(full), dir, nm);
        int dup = 0;
        for (int k = 0; k < s_pick_n; k++) if (seq0(s_pick_paths[k], full)) { dup = 1; break; }
        if (!dup) scpy(s_pick_paths[s_pick_n++], MT_PATH, full);
    }
}

static void dlg_ta_ready_cb(lv_event_t *e) { (void)e; dlg_ok_cb(0); }

static void open_path_dlg(int mode) {
    close_dlg();
    g_dlg_mode = mode;

    g_dlg = lv_obj_create(g_win);
    lv_obj_remove_style_all(g_dlg);
    lv_obj_set_size(g_dlg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_dlg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_dlg, 160, 0);
    lv_obj_add_flag(g_dlg, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(g_dlg, 0, 0);

    lv_obj_t *card = lv_obj_create(g_dlg);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 460, mode == 1 ? 340 : 165);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_BAR), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(C_DIV), 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ttl = lv_label_create(card);
    lv_label_set_text(ttl, mode == 1 ? "Dosya Aç" : "Farklı Kaydet");
    lv_obj_set_style_text_color(ttl, lv_color_hex(C_BRIGHT), 0);
    if (f13) lv_obj_set_style_text_font(ttl, f13, 0);

    g_dlg_ta = lv_textarea_create(card);
    lv_obj_set_size(g_dlg_ta, LV_PCT(100), 34);
    lv_textarea_set_one_line(g_dlg_ta, true);
    lv_obj_set_style_bg_color(g_dlg_ta, lv_color_hex(C_BODY), 0);
    lv_obj_set_style_bg_opa(g_dlg_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_dlg_ta, 1, 0);
    lv_obj_set_style_border_color(g_dlg_ta, lv_color_hex(C_ACC), 0);
    lv_obj_set_style_text_color(g_dlg_ta, lv_color_hex(C_BRIGHT), 0);
    if (f13) lv_obj_set_style_text_font(g_dlg_ta, f13, 0);
    if (mode == 2 && g_sel >= 0 && g_path[g_sel][0])
        lv_textarea_set_text(g_dlg_ta, g_path[g_sel]);
    else if (mode == 2)
        lv_textarea_set_text(g_dlg_ta, "C:/belgeler/yeni.txt");
    else
        lv_textarea_set_text(g_dlg_ta, "C:/belgeler/");
    lv_textarea_set_cursor_pos(g_dlg_ta, LV_TEXTAREA_CURSOR_LAST);
    lv_obj_add_event_cb(g_dlg_ta, dlg_ta_ready_cb, LV_EVENT_READY, 0);
    lv_obj_add_event_cb(g_dlg_ta, ta_click_focus_cb, LV_EVENT_CLICKED, 0);

    if (mode == 1) {
        s_pick_n = 0;
        collect_dir_for_dlg("C:");
        collect_dir_for_dlg("C:/belgeler");
        collect_dir_for_dlg("C:/masaustu");
        collect_dir_for_dlg("C:/indirilenler");

        lv_obj_t *lst = lv_obj_create(card);
        lv_obj_remove_style_all(lst);
        lv_obj_set_width(lst, LV_PCT(100));
        lv_obj_set_flex_grow(lst, 1);
        lv_obj_set_style_bg_color(lst, lv_color_hex(C_BODY), 0);
        lv_obj_set_style_bg_opa(lst, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(lst, 6, 0);
        lv_obj_set_style_border_width(lst, 1, 0);
        lv_obj_set_style_border_color(lst, lv_color_hex(C_LINE), 0);
        lv_obj_set_style_pad_all(lst, 4, 0);
        lv_obj_set_style_pad_row(lst, 2, 0);
        lv_obj_set_flex_flow(lst, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scroll_dir(lst, LV_DIR_VER);

        for (int i = 0; i < s_pick_n; i++) {
            lv_obj_t *rb = lv_button_create(lst);
            lv_obj_set_size(rb, LV_PCT(100), 26);
            lv_obj_set_style_radius(rb, 4, 0);
            lv_obj_set_style_bg_opa(rb, LV_OPA_TRANSP, 0);
            lv_obj_set_style_bg_color(rb, lv_color_hex(C_BTN), LV_STATE_HOVERED);
            lv_obj_set_style_bg_opa(rb, LV_OPA_COVER, LV_STATE_HOVERED);
            lv_obj_set_style_border_width(rb, 0, 0);
            lv_obj_set_style_shadow_width(rb, 0, 0);
            lv_obj_set_style_pad_hor(rb, 8, 0);
            lv_obj_t *rl = lv_label_create(rb);
            lv_label_set_text(rl, s_pick_paths[i]);
            lv_obj_set_style_text_color(rl, lv_color_hex(C_TXT), 0);
            if (f12) lv_obj_set_style_text_font(rl, f12, 0);
            lv_obj_align(rl, LV_ALIGN_LEFT_MID, 0, 0);
            lv_obj_add_event_cb(rb, dlg_pick_file_cb, LV_EVENT_CLICKED, s_pick_paths[i]);
        }
    }

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 32);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bc = lv_button_create(row);
    lv_obj_set_size(bc, 88, 30);
    lv_obj_set_style_radius(bc, 6, 0);
    lv_obj_set_style_bg_color(bc, lv_color_hex(C_BTN), 0);
    lv_obj_set_style_shadow_width(bc, 0, 0);
    lv_obj_t *bcl = lv_label_create(bc);
    lv_label_set_text(bcl, "İptal");
    lv_obj_set_style_text_color(bcl, lv_color_hex(C_TXT), 0);
    if (f12) lv_obj_set_style_text_font(bcl, f12, 0);
    lv_obj_center(bcl);
    lv_obj_add_event_cb(bc, dlg_cancel_cb, LV_EVENT_CLICKED, 0);

    lv_obj_t *bo = lv_button_create(row);
    lv_obj_set_size(bo, 96, 30);
    lv_obj_set_style_radius(bo, 6, 0);
    lv_obj_set_style_bg_color(bo, lv_color_hex(C_ACC), 0);
    lv_obj_set_style_shadow_width(bo, 0, 0);
    lv_obj_t *bol = lv_label_create(bo);
    lv_label_set_text(bol, mode == 1 ? "Aç" : "Kaydet");
    lv_obj_set_style_text_color(bol, lv_color_hex(0xFFFFFF), 0);
    if (f12) lv_obj_set_style_text_font(bol, f12, 0);
    lv_obj_center(bol);
    lv_obj_add_event_cb(bo, dlg_ok_cb, LV_EVENT_CLICKED, 0);

    focus_obj(g_dlg_ta);
}

static void open_save_as_dlg(void) { open_path_dlg(2); }
static void open_file_dlg(void) { open_path_dlg(1); }

/* ---- Klavye Kısayolları ---- */
static void on_key_preprocess(lv_event_t *e) {
    uint32_t k = lv_event_get_key(e);
    int ctrl = (A->input && A->input->ctrl_down) ? A->input->ctrl_down() : 0;
    if (ctrl) {
        if (k == 's' || k == 'S') {
            lv_event_stop_processing(e);
            save_current(1);
            return;
        }
        if (k == 'f' || k == 'F') {
            lv_event_stop_processing(e);
            toggle_find_bar(1);
            return;
        }
        if (k == 'n' || k == 'N') {
            lv_event_stop_processing(e);
            new_tab();
            return;
        }
        if (k == 'o' || k == 'O') {
            lv_event_stop_processing(e);
            open_file_dlg();
            return;
        }
    }
    if (k == LV_KEY_ESC) {
        if (g_dlg) { lv_event_stop_processing(e); close_dlg(); return; }
        if (g_find_bar && !lv_obj_has_flag(g_find_bar, LV_OBJ_FLAG_HIDDEN)) {
            lv_event_stop_processing(e);
            toggle_find_bar(0);
            return;
        }
    }
}

static void mt_hotkey(int hk) {
    if (hk == 7) { do_undo(0); return; }
    if (hk == 8) { do_undo(1); return; }

    lv_indev_t *kp = lv_port_get_keypad();
    lv_group_t *g = kp ? lv_indev_get_group(kp) : 0;
    lv_obj_t *foc = g ? lv_group_get_focused(g) : 0;
    if (!foc || !lv_obj_check_type(foc, &lv_textarea_class)) foc = g_ta;
    if (!foc) return;

    static char selbuf[4096];
    lv_textarea_t *t = (lv_textarea_t *)foc;
    lv_obj_t *lab = lv_obj_get_child_count(foc) > 0 ? lv_obj_get_child(foc, 0) : 0;
    const char *txt = lv_textarea_get_text(foc);
    if (!txt) txt = "";

    if (hk == 3 || hk == 4) { /* Ctrl+C / Ctrl+X */
        if (lv_textarea_text_is_selected(foc) && lab) {
            uint32_t a = lv_label_get_text_selection_start(lab);
            uint32_t b = lv_label_get_text_selection_end(lab);
            if (a > b) { uint32_t tmp = a; a = b; b = tmp; }
            uint32_t ab = cp_to_byte(txt, a), bb = cp_to_byte(txt, b);
            uint32_t n = (bb > ab) ? (bb - ab) : 0;
            if (n >= sizeof(selbuf)) n = sizeof(selbuf) - 1;
            for (uint32_t i = 0; i < n; i++) selbuf[i] = txt[ab + i];
            selbuf[n] = 0;
            arctian_clipboard_set(selbuf);
            if (hk == 4) {
                uint32_t bk = LV_KEY_BACKSPACE;
                lv_obj_send_event(foc, LV_EVENT_KEY, &bk);
            }
        } else {
            arctian_clipboard_set(txt);
            if (hk == 4) lv_textarea_set_text(foc, "");
        }
    } else if (hk == 5) { /* Ctrl+V */
        const char *s = arctian_clipboard_get();
        if (s && s[0]) {
            if (lv_textarea_text_is_selected(foc)) {
                uint32_t bk = LV_KEY_BACKSPACE;
                lv_obj_send_event(foc, LV_EVENT_KEY, &bk);
            }
            lv_textarea_add_text(foc, s);
        }
    } else if (hk == 6) { /* Ctrl+A */
        uint32_t cp_len = byte_to_cp(txt, (uint32_t)slen(txt));
        lv_textarea_set_text_selection(foc, true);
        t->sel_start = 0;
        t->sel_end = cp_len;
        if (lab) {
            lv_label_set_text_selection_start(lab, 0);
            lv_label_set_text_selection_end(lab, cp_len);
        }
        lv_obj_invalidate(foc);
    }
}

/* ---- 5s Otomatik Kaydetme ---- */
static void autosave_cb(lv_timer_t *t) {
    (void)t;
    for (int i = 0; i < g_n; i++) {
        if (g_dirty[i] && g_path[i][0]) {
            write_tab_to_path(i, g_path[i], 0);
        }
    }
}

/* ---- Toolbar butonları ---- */
static void btn_new_cb(lv_event_t *e)    { (void)e; new_tab(); }
static void btn_open_cb(lv_event_t *e)   { (void)e; open_file_dlg(); }
static void btn_save_cb(lv_event_t *e)   { (void)e; save_current(1); }
static void btn_saveas_cb(lv_event_t *e) { (void)e; open_save_as_dlg(); }
static void btn_undo_cb(lv_event_t *e)   { (void)e; do_undo(0); }
static void btn_redo_cb(lv_event_t *e)   { (void)e; do_undo(1); }

static lv_obj_t *tb_btn(lv_obj_t *p, const char *txt, lv_event_cb_t cb, int accent) {
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_height(b, 26);
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(accent ? C_ACC : C_BTN), 0);
    lv_obj_set_style_border_width(b, accent ? 0 : 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(C_DIV), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 10, 0);
    lv_obj_set_style_pad_ver(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(accent ? 0xFFFFFF : C_TXT), 0);
    if (f12) lv_obj_set_style_text_font(l, f12, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, 0);
    return b;
}

static void add_file(const char *dir, const char *name) {
    if (g_n >= MT_MAX || !is_text(name)) return;
    char full[MT_PATH];
    join_path(full, sizeof(full), dir, name);
    for (int i = 0; i < g_n; i++) if (seq0(g_path[i], full)) return;
    scpy(g_path[g_n], MT_PATH, full);
    scpy(g_name[g_n], MT_NAME, name);
    g_dirty[g_n] = 0;
    g_n++;
}

static void scan_dir(const char *dir) {
    if (!A->fs || !A->fs->count_dir || !A->fs->list_dir) return;
    int n = A->fs->count_dir(dir);
    if (n <= 0) return;
    for (int i = 0; i < n && g_n < MT_MAX; i++) {
        char nm[MT_NAME];
        int isd = 0;
        if (A->fs->list_dir(dir, i, nm, sizeof(nm), &isd) != 0 || isd) continue;
        add_file(dir, nm);
    }
}

static void mt_relayout(lv_obj_t *win) {
    (void)win;
    if (!g_win) return;
    lv_obj_update_layout(g_win);
    sync_gutter_scroll();
}

static void app_del_cb(lv_event_t *e) {
    (void)e;
    if (g_auto_timer) { lv_timer_delete(g_auto_timer); g_auto_timer = 0; }
    for (int i = 0; i < MT_MAX; i++) free_tab_stacks(i);
    if (g_grp) {
        lv_indev_t *kp = lv_port_get_keypad();
        if (kp && lv_indev_get_group(kp) == g_grp) lv_indev_set_group(kp, 0);
        lv_group_delete(g_grp);
        g_grp = 0;
    }
    g_win = 0; g_tabs = 0; g_find_bar = 0; g_find_ta = 0; g_repl_ta = 0;
    g_find_st = 0; g_gutter = 0; g_ta = 0; g_st_l = 0; g_st_m = 0; g_st_r = 0;
    g_dlg = 0; g_dlg_ta = 0;
}

void app_entry(lv_obj_t *win, asi_t *asi) {
    A = asi;
    g_win = win;
    g_n = 0; g_sel = -1; g_guard = 0; g_untitled_seq = 0; g_dlg = 0; g_dlg_ta = 0;
    for (int i = 0; i < MT_MAX; i++) {
        g_buf[i] = 0; g_last[i] = 0; g_dirty[i] = 0;
        g_undo_n[i] = 0; g_redo_n[i] = 0;
    }

    if (!f13) f13 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 13);
    if (!f12) f12 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 12);

    scan_dir("C:");
    scan_dir("C:/belgeler");
    scan_dir("C:/masaustu");
    scan_dir("C:/indirilenler");

    if (arctian_open_target && arctian_open_target[0]) {
        const char *p = arctian_open_target;
        char dir[MT_PATH], nm[MT_NAME];
        split_path(p, dir, sizeof(dir), nm, sizeof(nm));
        for (int i = 0; i < g_n; i++) {
            if (seq0(g_path[i], p)) { g_sel = i; break; }
        }
        if (g_sel < 0 && g_n < MT_MAX) {
            scpy(g_path[g_n], MT_PATH, p);
            scpy(g_name[g_n], MT_NAME, nm[0] ? nm : p);
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

    /* Araç çubuğu 38 px */
    lv_obj_t *tb = lv_obj_create(win);
    lv_obj_remove_style_all(tb);
    lv_obj_set_size(tb, LV_PCT(100), 38);
    lv_obj_set_style_bg_color(tb, lv_color_hex(C_BAR), 0);
    lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tb, 1, 0);
    lv_obj_set_style_border_side(tb, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(tb, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_pad_hor(tb, 10, 0);
    lv_obj_set_style_pad_column(tb, 6, 0);
    lv_obj_set_flex_flow(tb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tb, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(tb, LV_OBJ_FLAG_SCROLLABLE);

    tb_btn(tb, "Yeni", btn_new_cb, 0);
    tb_btn(tb, "Aç", btn_open_cb, 0);
    tb_btn(tb, "Kaydet", btn_save_cb, 1);
    tb_btn(tb, "Farklı Kaydet", btn_saveas_cb, 0);
    tb_btn(tb, "Geri Al", btn_undo_cb, 0);
    tb_btn(tb, "Yinele", btn_redo_cb, 0);
    tb_btn(tb, "Bul / Değiştir", find_toggle_cb, 0);

    /* Sekme şeridi 36 px */
    g_tabs = lv_obj_create(win);
    lv_obj_remove_style_all(g_tabs);
    lv_obj_set_size(g_tabs, LV_PCT(100), 36);
    lv_obj_set_style_bg_color(g_tabs, lv_color_hex(C_BODY), 0);
    lv_obj_set_style_bg_opa(g_tabs, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_tabs, 1, 0);
    lv_obj_set_style_border_side(g_tabs, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(g_tabs, lv_color_hex(C_LINE), 0);
    lv_obj_set_scroll_dir(g_tabs, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(g_tabs, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(g_tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_tabs, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* Bul & Değiştir çubuğu (varsayılan gizli) */
    g_find_bar = lv_obj_create(win);
    lv_obj_remove_style_all(g_find_bar);
    lv_obj_set_size(g_find_bar, LV_PCT(100), 38);
    lv_obj_set_style_bg_color(g_find_bar, lv_color_hex(C_TABON), 0);
    lv_obj_set_style_bg_opa(g_find_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_find_bar, 1, 0);
    lv_obj_set_style_border_side(g_find_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(g_find_bar, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_pad_hor(g_find_bar, 10, 0);
    lv_obj_set_style_pad_column(g_find_bar, 6, 0);
    lv_obj_set_flex_flow(g_find_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_find_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(g_find_bar, LV_OBJ_FLAG_SCROLLABLE);

    g_find_ta = lv_textarea_create(g_find_bar);
    lv_obj_set_size(g_find_ta, 150, 26);
    lv_textarea_set_one_line(g_find_ta, true);
    lv_textarea_set_placeholder_text(g_find_ta, "Ara...");
    lv_obj_set_style_bg_color(g_find_ta, lv_color_hex(C_BODY), 0);
    lv_obj_set_style_bg_opa(g_find_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_find_ta, 1, 0);
    lv_obj_set_style_border_color(g_find_ta, lv_color_hex(C_DIV), 0);
    lv_obj_set_style_text_color(g_find_ta, lv_color_hex(C_BRIGHT), 0);
    lv_obj_set_style_pad_ver(g_find_ta, 3, 0);
    if (f12) lv_obj_set_style_text_font(g_find_ta, f12, 0);
    lv_obj_add_event_cb(g_find_ta, find_btn_cb, LV_EVENT_READY, 0);
    lv_obj_add_event_cb(g_find_ta, ta_click_focus_cb, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(g_find_ta, on_key_preprocess, LV_EVENT_KEY | LV_EVENT_PREPROCESS, 0);

    g_repl_ta = lv_textarea_create(g_find_bar);
    lv_obj_set_size(g_repl_ta, 150, 26);
    lv_textarea_set_one_line(g_repl_ta, true);
    lv_textarea_set_placeholder_text(g_repl_ta, "Değiştir...");
    lv_obj_set_style_bg_color(g_repl_ta, lv_color_hex(C_BODY), 0);
    lv_obj_set_style_bg_opa(g_repl_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_repl_ta, 1, 0);
    lv_obj_set_style_border_color(g_repl_ta, lv_color_hex(C_DIV), 0);
    lv_obj_set_style_text_color(g_repl_ta, lv_color_hex(C_BRIGHT), 0);
    lv_obj_set_style_pad_ver(g_repl_ta, 3, 0);
    if (f12) lv_obj_set_style_text_font(g_repl_ta, f12, 0);
    lv_obj_add_event_cb(g_repl_ta, ta_click_focus_cb, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(g_repl_ta, on_key_preprocess, LV_EVENT_KEY | LV_EVENT_PREPROCESS, 0);

    tb_btn(g_find_bar, "Bul", find_btn_cb, 1);
    tb_btn(g_find_bar, "Değiştir", replace_one_cb, 0);
    tb_btn(g_find_bar, "Tümünü Değiştir", replace_all_cb, 0);

    g_find_st = lv_label_create(g_find_bar);
    lv_label_set_text(g_find_st, "");
    lv_obj_set_style_text_color(g_find_st, lv_color_hex(C_DIM), 0);
    if (f12) lv_obj_set_style_text_font(g_find_st, f12, 0);
    lv_obj_set_flex_grow(g_find_st, 1);

    tb_btn(g_find_bar, "✕", find_close_cb, 0);
    lv_obj_add_flag(g_find_bar, LV_OBJ_FLAG_HIDDEN);

    /* Gövde: satır numaraları + metin alanı */
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
    lv_obj_set_style_text_line_space(g_gutter, 10, 0);
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
    lv_textarea_set_text_selection(g_ta, true);
    lv_obj_set_scrollbar_mode(g_ta, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(g_ta, on_key_preprocess, LV_EVENT_KEY | LV_EVENT_PREPROCESS, 0);
    lv_obj_add_event_cb(g_ta, on_text_cb, LV_EVENT_VALUE_CHANGED, 0);
    lv_obj_add_event_cb(g_ta, on_text_cb, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(g_ta, on_text_cb, LV_EVENT_SCROLL, 0);
    lv_obj_add_event_cb(g_ta, ta_click_focus_cb, LV_EVENT_CLICKED, 0);

    /* Durum çubuğu 30 px */
    lv_obj_t *sb = lv_obj_create(win);
    lv_obj_remove_style_all(sb);
    lv_obj_set_size(sb, LV_PCT(100), 30);
    lv_obj_set_style_bg_color(sb, lv_color_hex(C_BAR), 0);
    lv_obj_set_style_bg_opa(sb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sb, 1, 0);
    lv_obj_set_style_border_side(sb, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(sb, lv_color_hex(C_LINE), 0);
    lv_obj_remove_flag(sb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_hor(sb, 16, 0);
    lv_obj_set_style_pad_column(sb, 12, 0);
    lv_obj_set_flex_flow(sb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sb, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_st_l = lv_label_create(sb);
    lv_label_set_text(g_st_l, "Satır 1, sütun 1");
    lv_obj_set_style_text_color(g_st_l, lv_color_hex(C_STAT), 0);
    if (f12) lv_obj_set_style_text_font(g_st_l, f12, 0);

    g_st_m = lv_label_create(sb);
    lv_label_set_text(g_st_m, "Ctrl+S: Kaydet · Ctrl+F: Bul · Ctrl+Z/Y: Geri Al/Yinele");
    lv_obj_set_style_text_color(g_st_m, lv_color_hex(C_STAT), 0);
    if (f12) lv_obj_set_style_text_font(g_st_m, f12, 0);

    g_st_r = lv_label_create(sb);
    lv_label_set_text(g_st_r, "");
    lv_obj_set_style_text_color(g_st_r, lv_color_hex(C_STAT), 0);
    if (f12) lv_obj_set_style_text_font(g_st_r, f12, 0);

    g_grp = lv_group_create();
    if (g_grp) {
        lv_group_add_obj(g_grp, g_ta);
        lv_group_focus_obj(g_ta);
        lv_indev_t *kp = lv_port_get_keypad();
        if (kp) lv_indev_set_group(kp, g_grp);
    }

    if (g_n == 0) new_tab();
    else {
        rebuild_tabs();
        load_sel();
    }

    g_auto_timer = lv_timer_create(autosave_cb, 5000, 0);
    arctian_shell_set_relayout(win, mt_relayout);
    arctian_shell_set_hotkey(win, mt_hotkey);
    lv_obj_add_event_cb(win, app_del_cb, LV_EVENT_DELETE, 0);
}
