#include "app.h"
#include "adl_wolf.h"
#include <stdint.h>

#define CP_MAX   256
#define CP_NAME  80
#define CP_PATH  256
#define CP_DIR   "C:/Cop"

static asi_t    *A;
static lv_obj_t *g_win;
static lv_obj_t *g_list;
static lv_obj_t *g_status;
static lv_obj_t *g_empty;
static lv_obj_t *g_bar;
static lv_obj_t *g_rows[CP_MAX];
static char      g_names[CP_MAX][CP_NAME];
static char      g_orig[CP_MAX][CP_PATH];
static uint8_t   g_isdir[CP_MAX];
static int       g_n;
static int       g_sel;

static uint8_t  *g_buf;
static uint32_t  g_buf_cap;

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

static int ends_with(const char *s, const char *suf) {
    int ls = slen(s), lf = slen(suf);
    if (ls < lf) return 0;
    for (int i = 0; i < lf; i++)
        if (s[ls - lf + i] != suf[i]) return 0;
    return 1;
}

static void set_status(const char *txt) {
    if (g_status) lv_label_set_text(g_status, txt);
}

static void ume(int v, char *out, int cap) {
    char t[12];
    int n = 0, k = 0;
    if (v == 0) t[n++] = '0';
    while (v > 0 && n < (int)sizeof(t)) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n && k < cap - 1) out[k++] = t[--n];
    out[k] = 0;
}

static void clear_rows(void) {
    for (int i = 0; i < CP_MAX; i++) {
        if (g_rows[i]) { lv_obj_delete(g_rows[i]); g_rows[i] = 0; }
    }
    g_n = 0;
    g_sel = -1;
}

static void trash_path(char *out, int cap, const char *name) {
    scpy(out, cap, CP_DIR "/");
    scat(out, cap, name);
}

static void meta_path(char *out, int cap, const char *name) {
    trash_path(out, cap, name);
    scat(out, cap, ".meta");
}

static void read_orig_meta(const char *name, char *orig_out, int cap) {
    orig_out[0] = 0;
    if (!A || !A->fs || !A->fs->read_path) return;
    char mp[CP_PATH];
    meta_path(mp, (int)sizeof(mp), name);
    uint32_t r = A->fs->read_path(mp, orig_out, (uint32_t)(cap - 1));
    if (r == 0) return;
    orig_out[r] = 0;
    int k = 0;
    while (orig_out[k] && orig_out[k] != '\n' && orig_out[k] != '\r') k++;
    orig_out[k] = 0;
}

static uint8_t *ensure_buf(uint32_t need) {
    if (need == 0) need = 1;
    if (g_buf_cap >= need && g_buf) return g_buf;
    uint8_t *nb = (uint8_t *)A->mem->alloc(need);
    if (!nb) return 0;
    if (g_buf) A->mem->free(g_buf);
    g_buf = nb;
    g_buf_cap = need;
    return g_buf;
}

static int copy_file_full(const char *src, const char *dst_dir, const char *dst_name) {
    asi_stat_t st;
    if (!A->fs->stat || A->fs->stat(src, &st) != 0 || st.is_dir) return -1;
    uint8_t *b = ensure_buf(st.size);
    if (!b) return -1;
    if (st.size > 0) {
        uint32_t r = A->fs->read_path(src, b, st.size);
        if (r != st.size) return -1;
    }
    if (A->fs->mkdir) A->fs->mkdir(dst_dir);
    void *fh = A->fs->create_in(dst_dir, dst_name, 0);
    if (!fh) return -1;
    return A->fs->write(fh, b, st.size);
}

static int remove_tree(const char *path, int depth) {
    if (depth > 12 || !A || !A->fs) return -1;
    asi_stat_t st;
    if (A->fs->stat && A->fs->stat(path, &st) == 0 && st.is_dir) {
        if (A->fs->count_dir && A->fs->list_dir) {
            for (;;) {
                int c = A->fs->count_dir(path);
                if (c <= 0) break;
                char nm[CP_NAME];
                int isd = 0;
                if (A->fs->list_dir(path, 0, nm, sizeof(nm), &isd) != 0 || !nm[0]) break;
                char sub[CP_PATH];
                scpy(sub, sizeof(sub), path);
                scat(sub, sizeof(sub), "/");
                scat(sub, sizeof(sub), nm);
                if (remove_tree(sub, depth + 1) != 0) break;
            }
        }
        char dp[CP_PATH];
        scpy(dp, sizeof(dp), path);
        int l = slen(dp);
        if (l > 0 && dp[l - 1] != '/') scat(dp, sizeof(dp), "/");
        if (A->fs->remove(dp) == 0) return 0;
    }
    return A->fs->remove(path);
}

static int copy_tree(const char *src_dir, const char *dst_dir, int depth) {
    if (depth > 12 || !A || !A->fs) return -1;
    if (A->fs->mkdir && A->fs->mkdir(dst_dir) != 0) return -1;
    if (!A->fs->count_dir || !A->fs->list_dir) return 0;
    int cnt = A->fs->count_dir(src_dir);
    for (int i = 0; i < cnt; i++) {
        char nm[CP_NAME];
        int isd = 0;
        if (A->fs->list_dir(src_dir, i, nm, sizeof(nm), &isd) != 0 || !nm[0]) continue;
        char sp[CP_PATH], dp[CP_PATH];
        scpy(sp, sizeof(sp), src_dir); scat(sp, sizeof(sp), "/"); scat(sp, sizeof(sp), nm);
        scpy(dp, sizeof(dp), dst_dir); scat(dp, sizeof(dp), "/"); scat(dp, sizeof(dp), nm);
        if (isd) {
            if (copy_tree(sp, dp, depth + 1) != 0) return -1;
        } else {
            if (copy_file_full(sp, dst_dir, nm) != 0) return -1;
        }
    }
    return 0;
}

static void row_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= g_n) return;
    g_sel = idx;
    for (int i = 0; i < g_n; i++)
        if (g_rows[i]) lv_obj_set_style_bg_opa(g_rows[i], i == g_sel ? 90 : LV_OPA_TRANSP, 0);
}

static void refresh(void) {
    clear_rows();
    if (!A || !A->fs || !A->fs->list_dir) { set_status("Dosya sistemi yok"); return; }
    if (A->fs->mkdir) A->fs->mkdir(CP_DIR);

    int total = A->fs->count_dir ? A->fs->count_dir(CP_DIR) : CP_MAX;
    for (int i = 0; i < total && g_n < CP_MAX; i++) {
        char nm[CP_NAME];
        int is_dir = 0;
        if (A->fs->list_dir(CP_DIR, i, nm, sizeof(nm), &is_dir) != 0) break;
        if (!nm[0] || ends_with(nm, ".meta")) continue;
        scpy(g_names[g_n], CP_NAME, nm);
        g_isdir[g_n] = is_dir ? 1 : 0;
        read_orig_meta(nm, g_orig[g_n], CP_PATH);
        g_n++;
    }

    if (g_empty) lv_obj_set_style_opa(g_empty, g_n ? LV_OPA_TRANSP : LV_OPA_COVER, 0);

    int lw = g_list ? lv_obj_get_width(g_list) : 700;
    if (lw < 200) lw = 700;
    int y = 4;
    for (int i = 0; i < g_n; i++) {
        lv_obj_t *r = lv_obj_create(g_list);
        lv_obj_remove_style_all(r);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(r, lw - 8, 38);
        lv_obj_set_pos(r, 4, y);
        lv_obj_set_style_radius(r, 8, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(ADL_ACCENT), 0);
        lv_obj_set_style_bg_opa(r, i == g_sel ? 90 : LV_OPA_TRANSP, 0);

        lv_obj_t *pick = lv_obj_create(r);
        lv_obj_remove_style_all(pick);
        lv_obj_set_size(pick, lw - 8, 38);
        lv_obj_set_pos(pick, 0, 0);
        lv_obj_add_flag(pick, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(pick, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        char disp[CP_PATH + 32];
        disp[0] = 0;
        if (g_isdir[i]) scat(disp, sizeof(disp), "[Klasör] ");
        scat(disp, sizeof(disp), g_names[i]);
        if (g_orig[i][0]) {
            scat(disp, sizeof(disp), "  <-  ");
            scat(disp, sizeof(disp), g_orig[i]);
        }

        lv_obj_t *l = lv_label_create(r);
        lv_label_set_text(l, disp);
        lv_obj_set_style_text_color(l, lv_color_hex(ADL_TEXT), 0);
        lv_obj_set_style_text_font(l, adl_font(), 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l, lw - 32);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 12, 0);

        g_rows[i] = r;
        y += 42;
    }

    char cnt[8];
    ume(g_n, cnt, (int)sizeof(cnt));
    char st[48];
    scpy(st, (int)sizeof(st), "Çöp: ");
    scat(st, (int)sizeof(st), cnt);
    scat(st, (int)sizeof(st), " öge");
    set_status(st);
}

static void split_parent(const char *full, char *dir, int dcap, char *nm, int ncap) {
    int last = -1;
    for (int i = 0; full[i]; i++) if (full[i] == '/') last = i;
    scpy(nm, ncap, full + last + 1);
    if (last <= 0) { scpy(dir, dcap, "C:"); return; }
    int k = 0;
    for (int i = 0; i < last && k < dcap - 1; i++) dir[k++] = full[i];
    dir[k] = 0;
}

static void restore_cb(lv_event_t *e) {
    (void)e;
    if (g_sel < 0 || g_sel >= g_n) { set_status("Önce bir öge seçin"); return; }
    if (!A || !A->fs || !A->fs->read_path || !A->fs->create_in || !A->fs->write || !A->fs->remove) return;

    char src[CP_PATH], mp[CP_PATH];
    trash_path(src, (int)sizeof(src), g_names[g_sel]);
    meta_path(mp, (int)sizeof(mp), g_names[g_sel]);

    char dst_full[CP_PATH], dst_dir[CP_PATH], dst_name[CP_NAME];
    if (g_orig[g_sel][0]) {
        scpy(dst_full, sizeof(dst_full), g_orig[g_sel]);
        split_parent(dst_full, dst_dir, sizeof(dst_dir), dst_name, sizeof(dst_name));
    } else {
        scpy(dst_dir, sizeof(dst_dir), "C:/belgeler");
        scpy(dst_name, sizeof(dst_name), g_names[g_sel]);
        scpy(dst_full, sizeof(dst_full), "C:/belgeler/");
        scat(dst_full, sizeof(dst_full), dst_name);
    }

    if (A->fs->mkdir) A->fs->mkdir(dst_dir);

    int ok = 0;
    if (g_isdir[g_sel]) {
        if (A->fs->rename && A->fs->rename(src, dst_full) == 0) ok = 1;
        else if (copy_tree(src, dst_full, 0) == 0) { remove_tree(src, 0); ok = 1; }
    } else {
        asi_stat_t st;
        if (!A->fs->stat || A->fs->stat(src, &st) != 0 || st.is_dir) {
            set_status("Öge bulunamadı");
            return;
        }
        if (A->fs->rename && A->fs->rename(src, dst_full) == 0) ok = 1;
        else if (copy_file_full(src, dst_dir, dst_name) == 0) {
            A->fs->remove(src);
            ok = 1;
        }
    }

    if (!ok) {
        set_status("Geri yüklenemedi; kaynak korundu");
        return;
    }

    A->fs->remove(mp);
    char msg[CP_PATH + 32];
    scpy(msg, sizeof(msg), "Geri yüklendi: ");
    scat(msg, sizeof(msg), dst_full);
    set_status(msg);
    refresh();
}

static void empty_cb(lv_event_t *e) {
    (void)e;
    if (!A || !A->fs || !A->fs->remove || !g_n) { set_status("Çöp zaten boş"); return; }
    int n = 0;
    for (int i = 0; i < g_n; i++) {
        char p[CP_PATH], mp[CP_PATH];
        trash_path(p, (int)sizeof(p), g_names[i]);
        meta_path(mp, (int)sizeof(mp), g_names[i]);
        if (remove_tree(p, 0) == 0) n++;
        A->fs->remove(mp);
    }
    char cnt[8];
    ume(n, cnt, (int)sizeof(cnt));
    char st[40];
    scpy(st, (int)sizeof(st), "Boşaltıldı: ");
    scat(st, (int)sizeof(st), cnt);
    set_status(st);
    refresh();
}

static void do_refresh_cb(lv_event_t *e) { (void)e; refresh(); }

static lv_obj_t *mk_btn(lv_obj_t *parent, const char *txt, int w, uint32_t col,
                        lv_event_cb_t cb) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, 34);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(ADL_TEXT), 0);
    lv_obj_set_style_text_font(l, adl_font(), 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, 0);
    return b;
}

static void trash_relayout(lv_obj_t *win) {
    if (!win) return;
    int W = lv_obj_get_width(win);
    int H = lv_obj_get_height(win);
    if (W < 300 || H < 200) return;
    if (g_bar) lv_obj_set_size(g_bar, W, 42);
    if (g_list) lv_obj_set_size(g_list, W, H - 48 - 28);
    refresh();
}

static void app_del_cb(lv_event_t *e) {
    (void)e;
    if (g_buf && A && A->mem && A->mem->free) { A->mem->free(g_buf); }
    g_buf = 0;
    g_buf_cap = 0;
    g_win = 0; g_bar = 0; g_list = 0; g_status = 0; g_empty = 0;
    for (int i = 0; i < CP_MAX; i++) g_rows[i] = 0;
    g_n = 0; g_sel = -1;
}

void app_entry(lv_obj_t *win, asi_t *asi) {
    A = asi;
    g_win = win;
    int W = lv_obj_get_width(win);
    int H = lv_obj_get_height(win);
    if (W < 300) W = 750;
    if (H < 200) H = 470;

    g_bar = lv_obj_create(win);
    lv_obj_remove_style_all(g_bar);
    lv_obj_remove_flag(g_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(g_bar, W, 42);
    lv_obj_set_pos(g_bar, 0, 0);

    lv_obj_t *t = lv_label_create(g_bar);
    lv_label_set_text(t, "Çöp Kovası");
    lv_obj_set_style_text_color(t, lv_color_hex(ADL_TEXT), 0);
    lv_obj_set_style_text_font(t, adl_font(), 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 6, 0);

    lv_obj_t *brest = mk_btn(g_bar, "Geri Yükle", 112, ADL_SURFACE_2, restore_cb);
    lv_obj_align(brest, LV_ALIGN_RIGHT_MID, -220, 0);
    lv_obj_t *bdel = mk_btn(g_bar, "Çöpü Boşalt", 122, ADL_DANGER, empty_cb);
    lv_obj_align(bdel, LV_ALIGN_RIGHT_MID, -92, 0);
    lv_obj_t *bref = mk_btn(g_bar, "Yenile", 80, ADL_SURFACE_2, do_refresh_cb);
    lv_obj_align(bref, LV_ALIGN_RIGHT_MID, -6, 0);

    g_list = lv_obj_create(win);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_style_bg_color(g_list, lv_color_hex(ADL_SURFACE), 0);
    lv_obj_set_style_bg_opa(g_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_list, 1, 0);
    lv_obj_set_style_border_color(g_list, lv_color_hex(ADL_BORDER), 0);
    lv_obj_set_style_radius(g_list, 10, 0);
    lv_obj_set_pos(g_list, 0, 48);
    lv_obj_set_size(g_list, W, H - 48 - 28);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);

    g_empty = lv_label_create(g_list);
    lv_label_set_text(g_empty, "Çöp kovası boş");
    lv_obj_set_style_text_color(g_empty, lv_color_hex(ADL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(g_empty, adl_font(), 0);
    lv_obj_align(g_empty, LV_ALIGN_TOP_MID, 0, 24);

    g_status = lv_label_create(win);
    lv_label_set_text(g_status, "");
    lv_obj_set_style_text_color(g_status, lv_color_hex(ADL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(g_status, adl_font(), 0);
    lv_obj_align(g_status, LV_ALIGN_BOTTOM_LEFT, 6, -4);

    arctian_shell_set_relayout(win, trash_relayout);
    lv_obj_add_event_cb(win, app_del_cb, LV_EVENT_DELETE, 0);
    refresh();
}
