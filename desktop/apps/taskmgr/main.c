/* ==================================================================
 * Görev Yöneticisi
 *   - Sol 52 px ray: 3 gerçek sekme (İşlemler, Performans, Sistem)
 *   - Gerçek süreç metrikleri: CPU %, Bellek (ram_kb), Disk (io_bytes), Ağ (net_bytes)
 *   - Boşta (idle) süreçler toplam CPU % hesabından hariç tutulur
 *   - Yeniden boyutlandırma (relayout) destekli
 * ================================================================== */
#include "app.h"
#include "adl_wolf.h"
#include "svg.h"
#include "version.h"
#include <stdint.h>

#define TM_MAX   64
#define TM_SLOTS 32
#define RAIL_N   3

#define C_BODY   0x0B0C0F
#define C_CARD   0x14171F
#define C_LINE   0x262A33
#define C_HAIR   0x262B35
#define C_INP    0x0F1115
#define C_ACT    0x1E2330
#define C_HOT    0x241B0E
#define C_ACC    0x3B82F6
#define C_TXT    0xE8EAEE
#define C_DIM    0x8B919C
#define C_MUT    0x7D8491
#define C_HDR    0xC9CED8
#define C_OK     0x4ADE80
#define C_WARN   0xFBBF24
#define C_ERR    0xF87171
#define C_DANGER 0x7F1D1D

static asi_t    *A;
static lv_obj_t *g_win;
static lv_obj_t *g_title;
static lv_obj_t *g_hdr;
static lv_obj_t *g_rows;
static lv_obj_t *g_status;
static lv_obj_t *g_search;
static lv_obj_t *g_bnew;
static lv_obj_t *g_bkill;
static lv_obj_t *g_rail_btns[RAIL_N];
static lv_timer_t *g_timer;
static int       g_tab = 0;        /* 0=Islemler, 1=Performans, 2=Sistem */
static int       g_sel_pid = -1;

static int      g_prev_pid[TM_MAX];
static uint32_t g_prev_tick[TM_MAX];
static uint32_t g_prev_total;
static int      g_prev_n;

extern const unsigned char font_ttf[];
extern const unsigned int  font_ttf_len;
static lv_font_t *f13, *f12, *f15;

#define ICON_MAX 8
static uint32_t *s_icon_buf[ICON_MAX];
static int s_icon_n;

static lv_obj_t *svg_icon(lv_obj_t *parent, const char *svg, int size) {
    if (!svg[0] || !A || !A->mem) return 0;
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

static const char *RAIL_ICONS[RAIL_N] = {
    "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#E8EAEE\" stroke-width=\"1.8\"><rect x=\"4\" y=\"4\" width=\"16\" height=\"16\" rx=\"2\"/><path d=\"M8 9h8M8 13h8M8 17h5\"/></svg>",
    "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#C9CED8\" stroke-width=\"1.8\"><path d=\"M3 20h18M6 16V10M11 16V4M16 16v-6M21 16v-3\"/></svg>",
    "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#C9CED8\" stroke-width=\"1.8\"><rect x=\"3\" y=\"4\" width=\"18\" height=\"7\" rx=\"1.5\"/><rect x=\"3\" y=\"13\" width=\"18\" height=\"7\" rx=\"1.5\"/><circle cx=\"7\" cy=\"7.5\" r=\"1\"/><circle cx=\"7\" cy=\"16.5\" r=\"1\"/></svg>",
};

static void scpy(char *d, int cap, const char *s) {
    int i = 0;
    if (cap <= 0) return;
    if (!s) { d[0] = 0; return; }
    for (; s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}

static void scat(char *d, int cap, const char *s) {
    int i = 0;
    while (d[i]) i++;
    int j = 0;
    for (; s && s[j] && i < cap - 1; i++, j++) d[i] = s[j];
    d[i] = 0;
}

static void u2s(uint64_t v, char *out, int cap) {
    char t[24];
    int n = 0, k = 0;
    if (v == 0) t[n++] = '0';
    while (v > 0 && n < (int)sizeof(t)) { t[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (n && k < cap - 1) out[k++] = t[--n];
    out[k] = 0;
}

static void dec1(int v10, char *out, int cap) {
    char t[16];
    int n = 0, k = 0;
    int frac = v10 % 10, whole = v10 / 10;
    char w[12]; int m = 0; int x = whole;
    if (x == 0) w[m++] = '0';
    while (x) { w[m++] = (char)('0' + x % 10); x /= 10; }
    while (m) t[n++] = w[--m];
    t[n++] = ',';
    t[n++] = (char)('0' + frac);
    t[n] = 0;
    for (; t[k] && k < cap - 1; k++) out[k] = t[k];
    out[k] = 0;
}

static void fmt_kb(uint32_t kb, char *out, int cap) {
    if (kb < 1024u) {
        u2s(kb, out, cap);
        scat(out, cap, " KB");
    } else {
        int mb10 = (int)(((uint64_t)kb * 10u) / 1024u);
        dec1(mb10, out, cap);
        scat(out, cap, " MB");
    }
}

static void fmt_bytes(uint64_t b, char *out, int cap) {
    if (b < 1024ull) {
        u2s(b, out, cap);
        scat(out, cap, " B");
    } else if (b < 1024ull * 1024ull) {
        u2s(b / 1024ull, out, cap);
        scat(out, cap, " KB");
    } else {
        int mb10 = (int)((b * 10ull) / (1024ull * 1024ull));
        dec1(mb10, out, cap);
        scat(out, cap, " MB");
    }
}

static const char *state_txt(int s) {
    switch (s) {
        case 1: return "Hazır";
        case 2: return "Çalışıyor";
        case 3: return "Askıda";
        case 4: return "Zombi";
        default: return "-";
    }
}

static uint32_t state_col(int s) {
    switch (s) {
        case 2: return C_OK;
        case 4: return C_ERR;
        default: return C_DIM;
    }
}

static int cpu_pct_x10(int pid, uint32_t tick, uint32_t total) {
    if (total <= g_prev_total) return 0;
    uint32_t dt = total - g_prev_total;
    for (int i = 0; i < g_prev_n; i++) {
        if (g_prev_pid[i] != pid) continue;
        uint32_t dp = tick - g_prev_tick[i];
        int v = (int)((uint64_t)dp * 1000u / dt);
        if (v > 1000) v = 1000;
        return v;
    }
    return 0;
}

static void remember(int n, const asi_proc_info_t *inf, uint32_t total) {
    if (n > TM_MAX) n = TM_MAX;
    for (int i = 0; i < n; i++) {
        g_prev_pid[i] = inf[i].pid;
        g_prev_tick[i] = inf[i].cpu_ticks;
    }
    g_prev_n = n;
    g_prev_total = total;
}

static int is_app_proc(const char *name) {
    int n = arctian_shell_app_count();
    for (int i = 0; i < n; i++) {
        char title[48], appname[32], cat[32];
        if (arctian_shell_app_info(i, title, sizeof(title), appname, sizeof(appname),
                                   cat, sizeof(cat)) != 0) continue;
        const char *a = title, *b = name;
        int same = 1;
        while (*a && *b) {
            char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
            char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
            if (ca != cb) { same = 0; break; }
            a++; b++;
        }
        if (same && !*a && !*b) return 1;
    }
    return 0;
}

static void build_hdr_cols(int hw) {
    if (!g_hdr) return;
    lv_obj_clean(g_hdr);
    if (hw < 300) hw = 780;
    int r_net  = hw - 10;
    int r_disk = r_net - 68;
    int r_mem  = r_disk - 74;
    int r_cpu  = r_mem - 86;
    int r_st   = r_cpu - 80;
    struct { const char *t; int r; int w; int right; } hh[6] = {
        { "Ad", 0, 0, 0 }, { "Durum", r_st, 76, 1 }, { "CPU", r_cpu, 74, 1 },
        { "Bellek", r_mem, 80, 1 }, { "Disk", r_disk, 68, 1 }, { "Ağ", r_net, 62, 1 },
    };
    for (int i = 0; i < 6; i++) {
        lv_obj_t *l = lv_label_create(g_hdr);
        lv_label_set_text(l, hh[i].t);
        lv_obj_set_style_text_color(l, lv_color_hex(C_DIM), 0);
        lv_obj_set_style_text_font(l, f12, 0);
        if (hh[i].right) {
            lv_obj_set_size(l, hh[i].w, 14);
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_pos(l, hh[i].r - hh[i].w, 8);
        } else {
            lv_obj_set_pos(l, 24, 8);
        }
    }
}

static lv_obj_t *cell_r(lv_obj_t *row, int right, int w, const char *txt,
                        uint32_t col, lv_font_t *f) {
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_size(l, w, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(l, right - w, 8);
    return l;
}

static void refresh(void);

static void row_click_cb(lv_event_t *e) {
    g_sel_pid = (int)(intptr_t)lv_event_get_user_data(e);
    refresh();
}

static void kill_sel_cb(lv_event_t *e) {
    (void)e;
    if (g_sel_pid <= 1) return;
    if (A && A->proc && A->proc->kill) A->proc->kill(g_sel_pid);
    g_sel_pid = -1;
    refresh();
}

static void run_new_cb(lv_event_t *e) {
    (void)e;
    arctian_shell_launch("terminal");
}

static void search_cb(lv_event_t *e) { (void)e; refresh(); }

static void add_row(const asi_proc_info_t *inf, int cpu10, int is_hot, int width) {
    lv_obj_t *row = lv_obj_create(g_rows);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, LV_PCT(100), 32);
    int sel = (inf->pid == g_sel_pid);
    uint32_t bg = is_hot ? C_HOT : (sel ? 0x1F232B : C_BODY);
    lv_obj_set_style_bg_color(row, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)inf->pid);

    int r_net  = width - 10;
    int r_disk = r_net - 68;
    int r_mem  = r_disk - 74;
    int r_cpu  = r_mem - 86;
    int r_st   = r_cpu - 80;

    lv_obj_t *nm = lv_label_create(row);
    char nmtxt[48];
    scpy(nmtxt, sizeof(nmtxt), inf->name[0] ? inf->name : "işlem");
    lv_label_set_text(nm, nmtxt);
    lv_obj_set_style_text_color(nm, lv_color_hex(C_TXT), 0);
    lv_obj_set_style_text_font(nm, f13, 0);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    int nw = r_st - 86;
    if (nw < 60) nw = 60;
    lv_obj_set_size(nm, nw, 16);
    lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 24, 8);

    cell_r(row, r_st, 76, state_txt(inf->state), state_col(inf->state), f12);

    char cpu_buf[16];
    dec1(cpu10, cpu_buf, sizeof(cpu_buf));
    scat(cpu_buf, sizeof(cpu_buf), "%");
    cell_r(row, r_cpu, 74, cpu_buf, is_hot ? C_WARN : C_TXT, f13);

    char mem_buf[20];
    fmt_kb(inf->ram_kb, mem_buf, sizeof(mem_buf));
    cell_r(row, r_mem, 80, mem_buf, C_TXT, f13);

    char dsk_buf[20], net_buf[20];
    fmt_bytes(inf->io_bytes, dsk_buf, sizeof(dsk_buf));
    fmt_bytes(inf->net_bytes, net_buf, sizeof(net_buf));
    cell_r(row, r_disk, 68, dsk_buf, inf->io_bytes ? C_TXT : C_MUT, f12);
    cell_r(row, r_net,  62, net_buf, inf->net_bytes ? C_TXT : C_MUT, f12);
}

static void add_group(const char *txt) {
    lv_obj_t *g = lv_obj_create(g_rows);
    lv_obj_remove_style_all(g);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(g, LV_PCT(100), 30);
    lv_obj_t *l = lv_label_create(g);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(C_HDR), 0);
    lv_obj_set_style_text_font(l, f12, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 12, 0);
}

static lv_obj_t *mk_card(lv_obj_t *parent, const char *title) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_width(c, LV_PCT(100));
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_radius(c, 8, 0);
    lv_obj_set_style_pad_all(c, 14, 0);
    lv_obj_set_style_pad_row(c, 8, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *tl = lv_label_create(c);
    lv_label_set_text(tl, title);
    lv_obj_set_style_text_color(tl, lv_color_hex(C_TXT), 0);
    lv_obj_set_style_text_font(tl, f13, 0);
    return c;
}

static void mk_metric_bar(lv_obj_t *card, const char *lbl, const char *val, int pct, uint32_t col) {
    lv_obj_t *r = lv_obj_create(card);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), 20);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l1 = lv_label_create(r);
    lv_label_set_text(l1, lbl);
    lv_obj_set_style_text_color(l1, lv_color_hex(C_DIM), 0);
    lv_obj_set_style_text_font(l1, f12, 0);
    lv_obj_align(l1, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *l2 = lv_label_create(r);
    lv_label_set_text(l2, val);
    lv_obj_set_style_text_color(l2, lv_color_hex(C_TXT), 0);
    lv_obj_set_style_text_font(l2, f12, 0);
    lv_obj_align(l2, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *bar = lv_bar_create(card);
    lv_obj_set_size(bar, LV_PCT(100), 8);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, pct < 0 ? 0 : (pct > 100 ? 100 : pct), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x262B35), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(col), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
}

static void mk_kv_row(lv_obj_t *card, const char *k, const char *v) {
    lv_obj_t *r = lv_obj_create(card);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), 22);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *kl = lv_label_create(r);
    lv_label_set_text(kl, k);
    lv_obj_set_style_text_color(kl, lv_color_hex(C_DIM), 0);
    lv_obj_set_style_text_font(kl, f12, 0);
    lv_obj_align(kl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *vl = lv_label_create(r);
    lv_label_set_text(vl, v);
    lv_obj_set_style_text_color(vl, lv_color_hex(C_TXT), 0);
    lv_obj_set_style_text_font(vl, f12, 0);
    lv_obj_align(vl, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void refresh(void) {
    if (!A || !g_rows) return;

    int width = lv_obj_get_width(g_rows);
    if (width < 300) width = 780;

    uint32_t total = A->proc && A->proc->sys_ticks ? A->proc->sys_ticks() : 0;
    int n = A->proc ? A->proc->count() : 0;

    asi_proc_info_t inf[TM_MAX];
    int pct10[TM_MAX];
    int got = 0, sum_cpu10 = 0;
    uint64_t sum_io = 0, sum_net = 0, sum_proc_ram_kb = 0;
    if (A->proc && A->proc->info_ex) {
        for (int i = 0; i < TM_SLOTS && got < TM_MAX; i++)
            if (A->proc->info_ex(i, &inf[got]) == 0) got++;
    }
    for (int i = 0; i < got; i++) {
        int is_idle = (inf[i].is_idle != 0) || ((inf[i].flags & ASI_PROC_FLAG_IDLE) != 0);
        pct10[i] = is_idle ? 0 : cpu_pct_x10(inf[i].pid, inf[i].cpu_ticks, total);
        if (!is_idle) sum_cpu10 += pct10[i];
        sum_io += inf[i].io_bytes;
        sum_net += inf[i].net_bytes;
        sum_proc_ram_kb += inf[i].ram_kb;
    }
    remember(got, inf, total);

    asi_meminfo_t mi;
    if (A->mem && A->mem->info) A->mem->info(&mi);
    else { mi.total = mi.used = mi.free = mi.heap_total = mi.heap_used = 0; }
    int mem_pct = mi.total ? (int)((uint64_t)mi.used * 100u / mi.total) : 0;
    int heap_pct = mi.heap_total ? (int)((uint64_t)mi.heap_used * 100u / mi.heap_total) : 0;
    int cpu_pct = sum_cpu10 / 10;
    if (cpu_pct > 100) cpu_pct = 100;

    if (g_tab == 0) {
        build_hdr_cols(width);

        char q[32];
        q[0] = 0;
        if (g_search) {
            const char *t = lv_textarea_get_text(g_search);
            int i = 0;
            for (; t && t[i] && i < (int)sizeof(q) - 1; i++) {
                char c = t[i];
                q[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
            }
            q[i] = 0;
        }

        int app_idx[TM_MAX], bg_idx[TM_MAX];
        int app_n = 0, bg_n = 0;
        for (int i = 0; i < got; i++) {
            if (q[0]) {
                const char *h = inf[i].name;
                char ln[32];
                int k = 0;
                for (; h[k] && k < 31; k++)
                    ln[k] = (h[k] >= 'A' && h[k] <= 'Z') ? (char)(h[k] + 32) : h[k];
                ln[k] = 0;
                int found = 0;
                for (k = 0; ln[k] && !found; k++) {
                    int j = 0;
                    while (ln[k + j] && q[j] && ln[k + j] == q[j]) j++;
                    if (!q[j]) found = 1;
                }
                if (!found) continue;
            }
            int is_app = ((inf[i].flags & ASI_PROC_FLAG_APP) != 0) || is_app_proc(inf[i].name);
            if (is_app) { if (app_n < TM_MAX) app_idx[app_n++] = i; }
            else if (bg_n < TM_MAX) bg_idx[bg_n++] = i;
        }

        lv_obj_clean(g_rows);
        lv_obj_set_style_pad_hor(g_rows, 0, 0);
        lv_obj_set_style_pad_row(g_rows, 0, 0);

        char gh[48], nb[12];
        u2s((uint64_t)app_n, nb, sizeof(nb));
        scpy(gh, sizeof(gh), "Uygulamalar ("); scat(gh, sizeof(gh), nb); scat(gh, sizeof(gh), ")");
        add_group(gh);
        for (int i = 0; i < app_n; i++) {
            int k = app_idx[i];
            add_row(&inf[k], pct10[k], pct10[k] >= 80, width);
        }
        if (app_n == 0) {
            lv_obj_t *e = lv_label_create(g_rows);
            lv_label_set_text(e, q[0] ? "Eşleşen uygulama yok" : "Açık uygulama yok");
            lv_obj_set_style_text_color(e, lv_color_hex(C_MUT), 0);
            lv_obj_set_style_text_font(e, f12, 0);
            lv_obj_set_style_pad_left(e, 24, 0);
        }

        u2s((uint64_t)bg_n, nb, sizeof(nb));
        scpy(gh, sizeof(gh), "Arka plan işlemleri ("); scat(gh, sizeof(gh), nb); scat(gh, sizeof(gh), ")");
        add_group(gh);
        for (int i = 0; i < bg_n; i++) {
            int k = bg_idx[i];
            add_row(&inf[k], pct10[k], pct10[k] >= 80, width);
        }
    } else if (g_tab == 1) {
        lv_obj_clean(g_rows);
        lv_obj_set_style_pad_hor(g_rows, 14, 0);
        lv_obj_set_style_pad_row(g_rows, 10, 0);

        lv_obj_t *c1 = mk_card(g_rows, "İşlemci (CPU)");
        char cb[32], nb[16];
        u2s((uint64_t)cpu_pct, nb, sizeof(nb));
        scpy(cb, sizeof(cb), "%"); scat(cb, sizeof(cb), nb);
        mk_metric_bar(c1, "Etkin Kullanım (Boşta hariç)", cb, cpu_pct, cpu_pct >= 80 ? C_WARN : C_ACC);
        u2s((uint64_t)got, nb, sizeof(nb));
        mk_kv_row(c1, "Aktif Süreç", nb);

        lv_obj_t *c2 = mk_card(g_rows, "Bellek (RAM & Yığın)");
        char mstr[64], u1[20], u2[20];
        fmt_bytes(mi.used, u1, sizeof(u1));
        fmt_bytes(mi.total, u2, sizeof(u2));
        scpy(mstr, sizeof(mstr), u1); scat(mstr, sizeof(mstr), " / "); scat(mstr, sizeof(mstr), u2);
        mk_metric_bar(c2, "Fiziksel Bellek", mstr, mem_pct, C_OK);
        fmt_bytes(mi.heap_used, u1, sizeof(u1));
        fmt_bytes(mi.heap_total, u2, sizeof(u2));
        scpy(mstr, sizeof(mstr), u1); scat(mstr, sizeof(mstr), " / "); scat(mstr, sizeof(mstr), u2);
        mk_metric_bar(c2, "Çekirdek Yığını (Heap)", mstr, heap_pct, C_ACC);
        fmt_kb((uint32_t)sum_proc_ram_kb, u1, sizeof(u1));
        mk_kv_row(c2, "Süreç Bellek Toplamı", u1);

        lv_obj_t *c3 = mk_card(g_rows, "Disk ve Ağ G/Ç");
        fmt_bytes(sum_io, u1, sizeof(u1));
        mk_kv_row(c3, "Toplam Disk G/Ç", u1);
        fmt_bytes(sum_net, u1, sizeof(u1));
        mk_kv_row(c3, "Toplam Ağ Trafiği", u1);
    } else {
        lv_obj_clean(g_rows);
        lv_obj_set_style_pad_hor(g_rows, 14, 0);
        lv_obj_set_style_pad_row(g_rows, 10, 0);

        lv_obj_t *c1 = mk_card(g_rows, "Sistem Özeti");
        mk_kv_row(c1, "İşletim Sistemi", "Arctian OS " ARCTIAN_VERSION);
        const char *cu = arctian_auth_current();
        mk_kv_row(c1, "Kullanıcı", (cu && cu[0]) ? cu : "Varsayılan");

        uint64_t ms = (A->time && A->time->ticks_ms) ? A->time->ticks_ms() : 0;
        uint64_t sec = ms / 1000ull;
        uint64_t min = sec / 60ull;
        uint64_t hr = min / 60ull;
        char up[40], nb[16];
        up[0] = 0;
        if (hr > 0) { u2s(hr, nb, sizeof(nb)); scat(up, sizeof(up), nb); scat(up, sizeof(up), " sa "); }
        u2s(min % 60ull, nb, sizeof(nb)); scat(up, sizeof(up), nb); scat(up, sizeof(up), " dk ");
        u2s(sec % 60ull, nb, sizeof(nb)); scat(up, sizeof(up), nb); scat(up, sizeof(up), " sn");
        mk_kv_row(c1, "Çalışma Süresi (Uptime)", up);

        if (A->gfx && A->gfx->width && A->gfx->height) {
            char rb[24], hb[12];
            u2s((uint64_t)A->gfx->width(), rb, sizeof(rb));
            scat(rb, sizeof(rb), " x ");
            u2s((uint64_t)A->gfx->height(), hb, sizeof(hb));
            scat(rb, sizeof(rb), hb);
            mk_kv_row(c1, "Ekran Çözünürlüğü", rb);
        }

        lv_obj_t *c2 = mk_card(g_rows, "Donanım ve Sürücüler");
        if (A->dev && A->dev->count) {
            int dc = A->dev->count();
            for (int i = 0; i < dc; i++) {
                const char *dn = A->dev->name ? A->dev->name(i) : "?";
                const char *ds = A->dev->status ? A->dev->status(i) : "-";
                mk_kv_row(c2, dn ? dn : "?", ds ? ds : "-");
            }
        } else {
            mk_kv_row(c2, "Sürücü bilgisi", "Yok");
        }
    }

    if (g_status) {
        char s[128], nb[16], dsk[20], net[20];
        s[0] = 0;
        scat(s, sizeof(s), "CPU %");
        u2s((uint64_t)cpu_pct, nb, sizeof(nb)); scat(s, sizeof(s), nb);
        scat(s, sizeof(s), "  ·  Bellek %");
        u2s((uint64_t)mem_pct, nb, sizeof(nb)); scat(s, sizeof(s), nb);
        fmt_bytes(sum_io, dsk, sizeof(dsk));
        fmt_bytes(sum_net, net, sizeof(net));
        scat(s, sizeof(s), "  ·  Disk "); scat(s, sizeof(s), dsk);
        scat(s, sizeof(s), "  ·  Ağ "); scat(s, sizeof(s), net);
        scat(s, sizeof(s), "  ·  ");
        u2s((uint64_t)n, nb, sizeof(nb)); scat(s, sizeof(s), nb);
        scat(s, sizeof(s), " işlem");
        lv_label_set_text(g_status, s);
    }
}

static void sync_tab_ui(void) {
    static const char *const TITLES[RAIL_N] = { "İşlemler", "Performans", "Sistem" };
    if (g_title) lv_label_set_text(g_title, TITLES[g_tab]);
    for (int i = 0; i < RAIL_N; i++) {
        if (!g_rail_btns[i]) continue;
        lv_obj_set_style_bg_opa(g_rail_btns[i], i == g_tab ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
    if (g_hdr) {
        if (g_tab == 0) lv_obj_clear_flag(g_hdr, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(g_hdr, LV_OBJ_FLAG_HIDDEN);
    }
    if (g_search) {
        if (g_tab == 0) lv_obj_clear_flag(g_search, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(g_search, LV_OBJ_FLAG_HIDDEN);
    }
    if (g_bnew) {
        if (g_tab == 0) lv_obj_clear_flag(g_bnew, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(g_bnew, LV_OBJ_FLAG_HIDDEN);
    }
    if (g_bkill) {
        if (g_tab == 0) lv_obj_clear_flag(g_bkill, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(g_bkill, LV_OBJ_FLAG_HIDDEN);
    }
}

static void rail_click_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= RAIL_N || idx == g_tab) return;
    g_tab = idx;
    sync_tab_ui();
    refresh();
}

static void tm_relayout(lv_obj_t *win) {
    (void)win;
    lv_obj_update_layout(g_win);
    refresh();
}

static void timer_cb(lv_timer_t *t) { (void)t; refresh(); }

static void app_del_cb(lv_event_t *e) {
    (void)e;
    if (g_timer) { lv_timer_delete(g_timer); g_timer = 0; }
    for (int i = 0; i < s_icon_n; i++)
        if (A && A->mem) A->mem->free(s_icon_buf[i]);
    s_icon_n = 0;
    g_win = 0; g_title = 0; g_hdr = 0; g_rows = 0; g_status = 0; g_search = 0;
    g_bnew = 0; g_bkill = 0;
    for (int i = 0; i < RAIL_N; i++) g_rail_btns[i] = 0;
    g_prev_n = 0;
    g_prev_total = 0;
}

void app_entry(lv_obj_t *win, asi_t *asi) {
    A = asi;
    g_win = win;
    g_tab = 0;
    g_sel_pid = -1;
    if (!f13) f13 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 13);
    if (!f12) f12 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 12);
    if (!f15) f15 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 15);

    lv_obj_set_flex_flow(win, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(win, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(win, 0, 0);
    lv_obj_set_style_bg_color(win, lv_color_hex(C_BODY), 0);
    lv_obj_set_style_bg_opa(win, LV_OPA_COVER, 0);
    lv_obj_remove_flag(win, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *rail = lv_obj_create(win);
    lv_obj_remove_style_all(rail);
    lv_obj_set_size(rail, 52, LV_PCT(100));
    lv_obj_set_style_bg_color(rail, lv_color_hex(C_BODY), 0);
    lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rail, 1, 0);
    lv_obj_set_style_border_side(rail, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_color(rail, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_pad_top(rail, 8, 0);
    lv_obj_set_style_pad_row(rail, 6, 0);
    lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rail, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(rail, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < RAIL_N; i++) {
        lv_obj_t *b = lv_button_create(rail);
        lv_obj_set_size(b, 32, 32);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_set_style_bg_opa(b, i == 0 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_ACT), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_LINE), LV_STATE_HOVERED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_HOVERED);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_t *ic = svg_icon(b, RAIL_ICONS[i], 16);
        if (ic) lv_obj_center(ic);
        lv_obj_add_event_cb(b, rail_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        g_rail_btns[i] = b;
    }

    lv_obj_t *main_col = lv_obj_create(win);
    lv_obj_remove_style_all(main_col);
    lv_obj_set_height(main_col, LV_PCT(100));
    lv_obj_set_flex_grow(main_col, 1);
    lv_obj_set_flex_flow(main_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(main_col, 0, 0);
    lv_obj_remove_flag(main_col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tb = lv_obj_create(main_col);
    lv_obj_remove_style_all(tb);
    lv_obj_set_size(tb, LV_PCT(100), 46);
    lv_obj_set_style_border_width(tb, 1, 0);
    lv_obj_set_style_border_side(tb, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(tb, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_pad_hor(tb, 14, 0);
    lv_obj_set_style_pad_column(tb, 10, 0);
    lv_obj_set_flex_flow(tb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tb, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(tb, LV_OBJ_FLAG_SCROLLABLE);

    g_title = lv_label_create(tb);
    lv_label_set_text(g_title, "İşlemler");
    lv_obj_set_style_text_color(g_title, lv_color_hex(C_TXT), 0);
    lv_obj_set_style_text_font(g_title, f15, 0);

    g_search = lv_textarea_create(tb);
    lv_obj_set_size(g_search, 190, 30);
    lv_textarea_set_one_line(g_search, true);
    lv_textarea_set_max_length(g_search, 30);
    lv_textarea_set_placeholder_text(g_search, "İşlem ara");
    lv_obj_set_style_radius(g_search, 6, 0);
    lv_obj_set_style_bg_color(g_search, lv_color_hex(C_INP), 0);
    lv_obj_set_style_bg_opa(g_search, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_search, 1, 0);
    lv_obj_set_style_border_color(g_search, lv_color_hex(0x2C313B), 0);
    lv_obj_set_style_pad_hor(g_search, 10, 0);
    lv_obj_set_style_pad_ver(g_search, 5, 0);
    lv_obj_set_style_text_font(g_search, f12, 0);
    {
        lv_obj_t *sp = lv_obj_create(tb);
        lv_obj_remove_style_all(sp);
        lv_obj_set_height(sp, 1);
        lv_obj_set_flex_grow(sp, 1);
        lv_obj_move_foreground(sp);
    }
    lv_obj_add_event_cb(g_search, search_cb, LV_EVENT_VALUE_CHANGED, 0);

    g_bnew = lv_button_create(tb);
    lv_obj_set_size(g_bnew, LV_SIZE_CONTENT, 30);
    lv_obj_set_style_radius(g_bnew, 6, 0);
    lv_obj_set_style_bg_color(g_bnew, lv_color_hex(0x1E2129), 0);
    lv_obj_set_style_bg_opa(g_bnew, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_bnew, 1, 0);
    lv_obj_set_style_border_color(g_bnew, lv_color_hex(0x333A47), 0);
    lv_obj_set_style_shadow_width(g_bnew, 0, 0);
    lv_obj_set_style_pad_hor(g_bnew, 12, 0);
    lv_obj_t *bnewl = lv_label_create(g_bnew);
    lv_label_set_text(bnewl, "Yeni görev çalıştır");
    lv_obj_set_style_text_color(bnewl, lv_color_hex(C_TXT), 0);
    lv_obj_set_style_text_font(bnewl, f12, 0);
    lv_obj_center(bnewl);
    lv_obj_add_event_cb(g_bnew, run_new_cb, LV_EVENT_CLICKED, 0);

    g_bkill = lv_button_create(tb);
    lv_obj_set_size(g_bkill, LV_SIZE_CONTENT, 30);
    lv_obj_set_style_radius(g_bkill, 6, 0);
    lv_obj_set_style_bg_color(g_bkill, lv_color_hex(C_DANGER), 0);
    lv_obj_set_style_bg_opa(g_bkill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_bkill, 1, 0);
    lv_obj_set_style_border_color(g_bkill, lv_color_hex(0xB91C1C), 0);
    lv_obj_set_style_shadow_width(g_bkill, 0, 0);
    lv_obj_set_style_pad_hor(g_bkill, 12, 0);
    lv_obj_t *bkilll = lv_label_create(g_bkill);
    lv_label_set_text(bkilll, "Görevi sonlandır");
    lv_obj_set_style_text_color(bkilll, lv_color_hex(C_TXT), 0);
    lv_obj_set_style_text_font(bkilll, f12, 0);
    lv_obj_center(bkilll);
    lv_obj_add_event_cb(g_bkill, kill_sel_cb, LV_EVENT_CLICKED, 0);

    g_hdr = lv_obj_create(main_col);
    lv_obj_remove_style_all(g_hdr);
    lv_obj_set_size(g_hdr, LV_PCT(100), 30);
    lv_obj_set_style_border_width(g_hdr, 1, 0);
    lv_obj_set_style_border_side(g_hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(g_hdr, lv_color_hex(C_HAIR), 0);
    lv_obj_remove_flag(g_hdr, LV_OBJ_FLAG_SCROLLABLE);

    g_rows = lv_obj_create(main_col);
    lv_obj_remove_style_all(g_rows);
    lv_obj_set_width(g_rows, LV_PCT(100));
    lv_obj_set_flex_grow(g_rows, 1);
    lv_obj_set_style_bg_color(g_rows, lv_color_hex(C_BODY), 0);
    lv_obj_set_style_bg_opa(g_rows, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_ver(g_rows, 6, 0);
    lv_obj_set_flex_flow(g_rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(g_rows, LV_DIR_VER);

    lv_obj_t *sb = lv_obj_create(main_col);
    lv_obj_remove_style_all(sb);
    lv_obj_set_size(sb, LV_PCT(100), 30);
    lv_obj_set_style_border_width(sb, 1, 0);
    lv_obj_set_style_border_side(sb, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(sb, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_pad_hor(sb, 14, 0);
    lv_obj_remove_flag(sb, LV_OBJ_FLAG_SCROLLABLE);
    g_status = lv_label_create(sb);
    lv_label_set_text(g_status, "");
    lv_obj_set_style_text_color(g_status, lv_color_hex(C_DIM), 0);
    lv_obj_set_style_text_font(g_status, f12, 0);
    lv_obj_align(g_status, LV_ALIGN_LEFT_MID, 0, 0);

    arctian_shell_set_relayout(win, tm_relayout);
    lv_obj_add_event_cb(win, app_del_cb, LV_EVENT_DELETE, 0);

    g_prev_n = 0;
    g_prev_total = 0;
    sync_tab_ui();
    refresh();
    g_timer = lv_timer_create(timer_cb, 1000, 0);
}
