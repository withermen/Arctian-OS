/* ==================================================================
 * Gorev Yoneticisi
 *
 * Arayuz spesifikasyonu (tasarim.md bolum 4):
 *   - sol 52 px ikon rayi (30x30 dugmeler, aktif #1e2330 + beyaz ikon)
 *   - ust 46 px arac satiri: "Islemler", arama kutusu, "Yeni gorev
 *     calistir", kirmizi "Gorevi sonlandir" (#7f1d1d)
 *   - tablo: Ad | Durum | CPU | Bellek | Disk | Ag
 *     baslik 30 px, satirlar 32 px, sol girinti 36 px, sag sütunlar
 *     saga yasli (Ag sütunu kenara yapismaz - bilinen sorun duzeltildi)
 *   - grup basliklari: Uygulamalar / Arka plan islemleri
 *   - yuksek CPU satiri kehribar zemin + #fbbf24 metin
 *   - alt durum cubugu 30 px
 * Bellek/Disk/Ag kisi bazinda surucu tarafinda izlenmiyor: "gosterilir"
 * yerine "YOK" degeri ("-" ile) verilir; uydurma sayi yazilmaz.
 * ================================================================== */
#include "app.h"
#include "adl_wolf.h"
#include "svg.h"
#include <stdint.h>

#define TM_MAX 64
#define TM_SLOTS 16

/* spec paleti */
#define C_BODY   0x0B0C0F
#define C_LINE   0x262A33
#define C_DIV    0x1D2129
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

static asi_t *A;
static lv_obj_t *g_rows;          /* tablo govdesi (yeniden kurulur)  */
static lv_obj_t *g_status;        /* alt cubuk metni                  */
static lv_obj_t *g_search;        /* arama kutusu                     */
static lv_timer_t *g_timer;
static int g_sel_pid = -1;

static int      g_prev_pid[TM_MAX];
static uint32_t g_prev_tick[TM_MAX];
static uint32_t g_prev_total;
static int      g_prev_n;

/* yerel fontlar */
extern const unsigned char font_ttf[];
extern const unsigned int  font_ttf_len;
static lv_font_t *f13, *f12, *f15;

/* ikon tamponlari */
#define ICON_MAX 8
static uint32_t *s_icon_buf[ICON_MAX];
static int s_icon_n;

static lv_obj_t *svg_icon(lv_obj_t *parent, const char *svg, int size) {
    if (!svg[0] || !A->mem) return 0;
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

/* ray ikonlari (spec svg'leri, stroke #C9CED8 / aktif #FFFFFF) */
#define RAIL_N 7
static const char *RAIL_ICONS[RAIL_N] = {
    "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#FFFFFF\" stroke-width=\"1.8\"><rect x=\"4\" y=\"4\" width=\"16\" height=\"16\" rx=\"2\"/><path d=\"M8 9h8M8 13h8M8 17h5\"/></svg>",
    "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#C9CED8\" stroke-width=\"1.8\"><path d=\"M3 20h18M6 16V10M11 16V4M16 16v-6M21 16v-3\"/></svg>",
    "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#C9CED8\" stroke-width=\"1.8\"><circle cx=\"12\" cy=\"12\" r=\"9\"/><path d=\"M12 7v5l3 2\"/></svg>",
    "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#C9CED8\" stroke-width=\"1.8\"><path d=\"M5 4l14 8-14 8z\"/></svg>",
    "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#C9CED8\" stroke-width=\"1.8\"><circle cx=\"9\" cy=\"8\" r=\"3\"/><circle cx=\"17\" cy=\"9\" r=\"2.5\"/><path d=\"M3 20a6 6 0 0 1 12 0M15 20a4 4 0 0 1 6 0\"/></svg>",
    "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#C9CED8\" stroke-width=\"1.8\"><path d=\"M4 6h16M4 12h16M4 18h16\"/></svg>",
    "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#C9CED8\" stroke-width=\"1.8\"><rect x=\"3\" y=\"3\" width=\"18\" height=\"6\" rx=\"1\"/><rect x=\"3\" y=\"15\" width=\"18\" height=\"6\" rx=\"1\"/></svg>",
};

static void scpy(char *d, int cap, const char *s) {
    int i = 0;
    if (cap <= 0) return;
    if (!s) { d[0] = 0; return; }
    for (; s[i] && i < cap - 1; i++) d[i] = s[i];
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

/* "9,4" gibi tek ondalikli deger yaz (TR uslubu: virgul) */
static void dec1(int v10, char *out, int cap) {
    char t[16];
    int n = 0, k = 0;
    int frac = v10 % 10, whole = v10 / 10;
    n = 0;
    if (whole == 0) t[n++] = '0';
    { char w[12]; int m = 0; int x = whole;
      if (x == 0) w[m++] = '0';
      while (x) { w[m++] = (char)('0' + x % 10); x /= 10; }
      while (m) t[n++] = w[--m];
    }
    t[n++] = ',';
    t[n++] = (char)('0' + frac);
    t[n] = 0;
    for (; t[k] && k < cap - 1; k++) out[k] = t[k];
    out[k] = 0;
}

static const char *state_txt(int s) {
    switch (s) {
        case 1: return "Hazir";
        case 2: return "Calisiyor";
        case 3: return "Askida";
        case 4: return "Zombi";
        default: return "-";
    }
}

static uint32_t state_col(int s) {
    switch (s) {
        case 1: return C_DIM;
        case 2: return C_OK;
        case 3: return C_DIM;
        case 4: return C_ERR;
        default: return C_DIM;
    }
}

/* ondalikli yuzde: tick farkindan x1000 */
static int cpu_pct_x10(int pid, uint32_t tick, uint32_t total) {
    if (total <= g_prev_total) return 0;
    uint32_t dt = total - g_prev_total;
    for (int i = 0; i < g_prev_n; i++) {
        if (g_prev_pid[i] != pid) continue;
        uint32_t dp = tick - g_prev_tick[i];
        return (int)((uint64_t)dp * 1000u / dt);
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

/* surec bir uygulama penceresine mi ait? (kayitli basliklarla karsilastir) */
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

/* ---- secim / sonlandirma ---- */
/* ---- tablo kurulumu (mutlak hucre yerlesimi) ---- */
/* sag blok: Durum 84 | CPU 84 | Bellek 84 | Disk 70 | Ag 64; sag bosluk 10 */
static lv_obj_t *cell_r(lv_obj_t *row, int right, int w, const char *txt,
                        uint32_t col, lv_font_t *f) {
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_size(l, w, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(l, right - w, 8);   /* satir 32 px, etiket ~16 px */
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
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)inf->pid);

    /* sagdan sayilan sutun sag kenarlari (Ag kenara yapismasin: 10 px bosluk) */
    int r_net  = width - 10;
    int r_disk = r_net - 64;
    int r_mem  = r_disk - 70;
    int r_cpu  = r_mem - 84;
    int r_st   = r_cpu - 84;

    /* Ad (sol girinti 36) */
    lv_obj_t *nm = lv_label_create(row);
    char nmtxt[48];
    scpy(nmtxt, sizeof(nmtxt), inf->name[0] ? inf->name : "islem");
    lv_label_set_text(nm, nmtxt);
    lv_obj_set_style_text_color(nm, lv_color_hex(C_TXT), 0);
    lv_obj_set_style_text_font(nm, f13, 0);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_size(nm, r_st - 12 - 36, 16);
    lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 36, 8);

    cell_r(row, r_st, 78, state_txt(inf->state), state_col(inf->state), f12);

    char cpu[12];
    dec1(cpu10, cpu, sizeof(cpu));
    static char cpu_pct_buf[12];
    scpy(cpu_pct_buf, sizeof(cpu_pct_buf), cpu);
    { int L = 0; while (cpu_pct_buf[L]) L++; cpu_pct_buf[L] = '%'; cpu_pct_buf[L+1] = 0; }
    cell_r(row, r_cpu, 78, cpu_pct_buf, is_hot ? C_WARN : C_TXT, f13);

    cell_r(row, r_mem, 78, "-", C_MUT, f13);
    cell_r(row, r_disk, 64, "-", C_MUT, f13);
    cell_r(row, r_net, 58, "-", C_MUT, f13);
}

static void add_group(const char *txt) {
    lv_obj_t *g = lv_obj_create(g_rows);
    lv_obj_remove_style_all(g);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(g, LV_PCT(100), 30);
    lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
    lv_obj_t *l = lv_label_create(g);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(C_HDR), 0);
    lv_obj_set_style_text_font(l, f12, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 12, 0);
}

static void refresh(void) {
    if (!A || !g_rows) return;

    int width = lv_obj_get_width(g_rows);
    if (width < 300) width = 827;

    uint32_t total = A->proc && A->proc->sys_ticks ? A->proc->sys_ticks() : 0;
    int n = A->proc ? A->proc->count() : 0;

    asi_proc_info_t inf[TM_MAX];
    int pct10[TM_MAX];
    int got = 0, sum_cpu10 = 0;
    if (A->proc && A->proc->info_ex) {
        for (int i = 0; i < TM_SLOTS && got < TM_MAX; i++)
            if (A->proc->info_ex(i, &inf[got]) == 0) got++;
    }
    for (int i = 0; i < got; i++) {
        pct10[i] = cpu_pct_x10(inf[i].pid, inf[i].cpu_ticks, total);
        sum_cpu10 += pct10[i];
    }

    /* arama filtresi */
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
            /* kucuk harfe indirgeyerek ara */
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
        if (is_app_proc(inf[i].name)) { if (app_n < TM_MAX) app_idx[app_n++] = i; }
        else if (bg_n < TM_MAX) bg_idx[bg_n++] = i;
    }

    lv_obj_clean(g_rows);

    char gh[40];
    {
        int L = 0;
        const char *t = "Uygulamalar (";
        for (; t[L]; L++) gh[L] = t[L];
        char nb[8]; u2s((uint64_t)app_n, nb, sizeof(nb));
        for (int i = 0; nb[i] && L < 30; i++) gh[L++] = nb[i];
        gh[L++] = ')'; gh[L] = 0;
        add_group(gh);
    }
    for (int i = 0; i < app_n; i++) {
        int k = app_idx[i];
        add_row(&inf[k], pct10[k], pct10[k] >= 80, width);
    }
    if (app_n == 0) {
        lv_obj_t *e = lv_label_create(g_rows);
        lv_label_set_text(e, q[0] ? "Eslesen uygulama yok" : "Acik uygulama yok");
        lv_obj_set_style_text_color(e, lv_color_hex(C_MUT), 0);
        lv_obj_set_style_text_font(e, f12, 0);
        lv_obj_set_pos(e, 36, 8);
    }
    {
        int L = 0;
        const char *t = "Arka plan islemleri (";
        for (; t[L]; L++) gh[L] = t[L];
        char nb[8]; u2s((uint64_t)bg_n, nb, sizeof(nb));
        for (int i = 0; nb[i] && L < 30; i++) gh[L++] = nb[i];
        gh[L++] = ')'; gh[L] = 0;
        add_group(gh);
    }
    for (int i = 0; i < bg_n; i++) {
        int k = bg_idx[i];
        add_row(&inf[k], pct10[k], pct10[k] >= 80, width);
    }

    remember(got, inf, total);

    /* bellek yuzdesi */
    asi_meminfo_t mi;
    if (A->mem && A->mem->info) A->mem->info(&mi);
    else { mi.total = mi.used = 0; }
    int mem_pct = mi.total ? (int)((uint64_t)mi.used * 100u / mi.total) : 0;
    int cpu_pct = sum_cpu10 / 10;
    if (cpu_pct > 100) cpu_pct = 100;

    if (g_status) {
        char s[96], nb[12];
        int k = 0;
        const char *pre = "CPU %";
        for (int i = 0; pre[i]; i++) s[k++] = pre[i];
        u2s((uint64_t)cpu_pct, nb, sizeof(nb));
        for (int i = 0; nb[i]; i++) s[k++] = nb[i];
        const char *mid = "  -  Bellek %";
        for (int i = 0; mid[i] && k < 80; i++) s[k++] = mid[i];
        u2s((uint64_t)mem_pct, nb, sizeof(nb));
        for (int i = 0; nb[i] && k < 84; i++) s[k++] = nb[i];
        const char *mid2 = "  -  Disk -  -  ";
        for (int i = 0; mid2[i] && k < 90; i++) s[k++] = mid2[i];
        u2s((uint64_t)n, nb, sizeof(nb));
        for (int i = 0; nb[i] && k < 94; i++) s[k++] = nb[i];
        const char *tail = " islem";
        for (int i = 0; tail[i] && k < 95; i++) s[k++] = tail[i];
        s[k] = 0;
        lv_label_set_text(g_status, s);
    }
}

static void timer_cb(lv_timer_t *t) { (void)t; refresh(); }

static void app_del_cb(lv_event_t *e) {
    (void)e;
    if (g_timer) { lv_timer_delete(g_timer); g_timer = 0; }
    for (int i = 0; i < s_icon_n; i++)
        if (A->mem) A->mem->free(s_icon_buf[i]);
    s_icon_n = 0;
    g_rows = 0; g_status = 0; g_search = 0;
    g_prev_n = 0;
    g_prev_total = 0;
}

void app_entry(lv_obj_t *win, asi_t *asi) {
    A = asi;
    if (!f13) f13 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 13);
    if (!f12) f12 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 12);
    if (!f15) f15 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 15);

    lv_obj_set_flex_flow(win, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(win, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(win, 0, 0);
    lv_obj_set_style_bg_color(win, lv_color_hex(C_BODY), 0);
    lv_obj_set_style_bg_opa(win, LV_OPA_COVER, 0);
    lv_obj_remove_flag(win, LV_OBJ_FLAG_SCROLLABLE);

    /* --- sol ikon rayi 52 px --- */
    lv_obj_t *rail = lv_obj_create(win);
    lv_obj_remove_style_all(rail);
    lv_obj_set_size(rail, 52, LV_PCT(100));
    lv_obj_set_style_bg_color(rail, lv_color_hex(C_BODY), 0);
    lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rail, 1, 0);
    lv_obj_set_style_border_side(rail, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_color(rail, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_pad_top(rail, 8, 0);
    lv_obj_set_style_pad_row(rail, 4, 0);
    lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rail, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(rail, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < RAIL_N; i++) {
        lv_obj_t *b = lv_button_create(rail);
        lv_obj_set_size(b, 30, 30);
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
    }

    /* --- sag alan --- */
    lv_obj_t *main_col = lv_obj_create(win);
    lv_obj_remove_style_all(main_col);
    lv_obj_set_height(main_col, LV_PCT(100));
    lv_obj_set_flex_grow(main_col, 1);
    lv_obj_set_flex_flow(main_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(main_col, 0, 0);
    lv_obj_remove_flag(main_col, LV_OBJ_FLAG_SCROLLABLE);

    /* arac satiri 46 px */
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

    lv_obj_t *tt = lv_label_create(tb);
    lv_label_set_text(tt, "Islemler");
    lv_obj_set_style_text_color(tt, lv_color_hex(C_TXT), 0);
    lv_obj_set_style_text_font(tt, f15, 0);

    g_search = lv_textarea_create(tb);
    lv_obj_set_size(g_search, 200, 30);
    lv_textarea_set_one_line(g_search, true);
    lv_textarea_set_max_length(g_search, 30);
    lv_textarea_set_placeholder_text(g_search, "Islem ara");
    lv_obj_set_style_radius(g_search, 6, 0);
    lv_obj_set_style_bg_color(g_search, lv_color_hex(C_INP), 0);
    lv_obj_set_style_bg_opa(g_search, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_search, 1, 0);
    lv_obj_set_style_border_color(g_search, lv_color_hex(0x2C313B), 0);
    lv_obj_set_style_pad_hor(g_search, 10, 0);
    lv_obj_set_style_pad_ver(g_search, 5, 0);
    lv_obj_set_style_text_font(g_search, f12, 0);
    lv_obj_set_flex_grow(g_search, 0);
    {   /* sagdaki dugmelerle ayni hizada, sag blok: spacer yerine grow */
        lv_obj_t *sp = lv_obj_create(tb);
        lv_obj_remove_style_all(sp);
        lv_obj_set_height(sp, 1);
        lv_obj_set_flex_grow(sp, 1);
        lv_obj_move_foreground(sp);
    }
    lv_obj_add_event_cb(g_search, search_cb, LV_EVENT_VALUE_CHANGED, 0);

    lv_obj_t *bnew = lv_button_create(tb);
    lv_obj_set_size(bnew, LV_SIZE_CONTENT, 30);
    lv_obj_set_style_radius(bnew, 6, 0);
    lv_obj_set_style_bg_color(bnew, lv_color_hex(0x1E2129), 0);
    lv_obj_set_style_bg_opa(bnew, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bnew, 1, 0);
    lv_obj_set_style_border_color(bnew, lv_color_hex(0x333A47), 0);
    lv_obj_set_style_shadow_width(bnew, 0, 0);
    lv_obj_set_style_pad_hor(bnew, 12, 0);
    lv_obj_t *bnewl = lv_label_create(bnew);
    lv_label_set_text(bnewl, "Yeni gorev calistir");
    lv_obj_set_style_text_color(bnewl, lv_color_hex(C_TXT), 0);
    lv_obj_set_style_text_font(bnewl, f12, 0);
    lv_obj_center(bnewl);
    lv_obj_add_event_cb(bnew, run_new_cb, LV_EVENT_CLICKED, 0);

    lv_obj_t *bkill = lv_button_create(tb);
    lv_obj_set_size(bkill, LV_SIZE_CONTENT, 30);
    lv_obj_set_style_radius(bkill, 6, 0);
    lv_obj_set_style_bg_color(bkill, lv_color_hex(C_DANGER), 0);
    lv_obj_set_style_bg_opa(bkill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bkill, 1, 0);
    lv_obj_set_style_border_color(bkill, lv_color_hex(0xB91C1C), 0);
    lv_obj_set_style_shadow_width(bkill, 0, 0);
    lv_obj_set_style_pad_hor(bkill, 12, 0);
    lv_obj_t *bkilll = lv_label_create(bkill);
    lv_label_set_text(bkilll, "Gorevi sonlandir");
    lv_obj_set_style_text_color(bkilll, lv_color_hex(C_TXT), 0);
    lv_obj_set_style_text_font(bkilll, f12, 0);
    lv_obj_center(bkilll);
    lv_obj_add_event_cb(bkill, kill_sel_cb, LV_EVENT_CLICKED, 0);

    /* baslik satiri 30 px */
    lv_obj_t *hdr = lv_obj_create(main_col);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LV_PCT(100), 30);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(hdr, lv_color_hex(C_HAIR), 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    {
        int hw = lv_obj_get_width(hdr);
        if (hw < 300) hw = 827;
        int r_net  = hw - 10;
        int r_disk = r_net - 64;
        int r_mem  = r_disk - 70;
        int r_cpu  = r_mem - 84;
        int r_st   = r_cpu - 84;
        struct { const char *t; int r; int w; int right; } hh[6] = {
            { "Ad", 0, 0, 0 }, { "Durum", r_st, 78, 1 }, { "CPU", r_cpu, 78, 1 },
            { "Bellek", r_mem, 78, 1 }, { "Disk", r_disk, 64, 1 }, { "Ag", r_net, 58, 1 },
        };
        for (int i = 0; i < 6; i++) {
            lv_obj_t *l = lv_label_create(hdr);
            lv_label_set_text(l, hh[i].t);
            lv_obj_set_style_text_color(l, lv_color_hex(C_DIM), 0);
            lv_obj_set_style_text_font(l, f12, 0);
            if (hh[i].right) {
                lv_obj_set_size(l, hh[i].w, 14);
                lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
                lv_obj_set_pos(l, hh[i].r - hh[i].w, 8);
            } else {
                lv_obj_set_pos(l, 36, 8);
            }
        }
    }

    /* satirlar */
    g_rows = lv_obj_create(main_col);
    lv_obj_remove_style_all(g_rows);
    lv_obj_set_width(g_rows, LV_PCT(100));
    lv_obj_set_flex_grow(g_rows, 1);
    lv_obj_set_style_bg_color(g_rows, lv_color_hex(C_BODY), 0);
    lv_obj_set_style_bg_opa(g_rows, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_ver(g_rows, 4, 0);
    lv_obj_set_flex_flow(g_rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(g_rows, LV_DIR_VER);

    /* alt durum cubugu 30 px */
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

    lv_obj_add_event_cb(win, app_del_cb, LV_EVENT_DELETE, 0);

    g_prev_n = 0;
    g_prev_total = 0;
    refresh();
    g_timer = lv_timer_create(timer_cb, 1000, 0);
}
