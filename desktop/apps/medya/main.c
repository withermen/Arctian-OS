/* ==================================================================
 * Medya Oynatici - Arctian yerel ses oynaticisi (.wav)
 *
 * Arayuz spesifikasyonu (tasarim.md bolum 3):
 *   - sol 300 px kapak sütunu (236x236 kapak, nota ikonu, ad/yil)
 *   - sag parca listesi (48 px satirlar, aktif satir #1f232b + mavi)
 *   - alt 88 px kontrol cubugu (zaman + 4 px ilerleme + SVG kontroller)
 * Kontroller metin karakteri degil SVG ikon kullanir (yazi tipine
 * bagimlilik kaldirmadi).
 * ================================================================== */
#include "app.h"
#include "adl_wolf.h"
#include "svg.h"
#include <stdint.h>

#define MP_MAX   64
#define MP_PATH  200
#define MP_NAME  96

/* spec paleti */
#define C_BODY   0x0B0C0F
#define C_PANEL  0x181B22
#define C_DIV    0x1F232B
#define C_LINE   0x2A2E37
#define C_TRACK  0x1A2233
#define C_NOTE   0x7FA8F0
#define C_ACC    0x3B82F6
#define C_TXT    0xE6E8EC
#define C_DIM    0x8A909B
#define C_MUT    0x7D8491

static asi_t    *A;
static lv_obj_t *g_win;
static lv_obj_t *g_list;
static lv_obj_t *g_meta;      /* parca adi (kapak alti)   */
static lv_obj_t *g_sub;       /* alt bilgi / durum satiri */
static lv_obj_t *g_time;      /* gecen sure               */
static lv_obj_t *g_total;     /* toplam sure              */
static lv_obj_t *g_bar;
static lv_obj_t *g_play_btn;  /* 40 px mavi daire */
static lv_obj_t *g_play_ic;   /* oynat/dur SVG tasiyan canvas */
static lv_timer_t *g_timer;

static char      g_path[MP_MAX][MP_PATH];
static char      g_name[MP_MAX][MP_NAME];
static char      g_dur[MP_MAX][8];
static lv_obj_t *g_rows[MP_MAX];
static lv_obj_t *g_row_lbl[MP_MAX];
static lv_obj_t *g_row_num[MP_MAX];
static lv_obj_t *g_row_dur[MP_MAX];
static int       g_n;
static int       g_sel;

static uint8_t  *g_buf;
static uint32_t  g_buflen;
static uint32_t  g_dur_ms;
static uint32_t  g_start_ms;
static int       g_playing;

/* yerel fontlar (spec: 17 baslik, 13 govde, 12 etiket) */
extern const unsigned char font_ttf[];
extern const unsigned int  font_ttf_len;
static lv_font_t *f17, *f13, *f12;

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

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void mmss(char *out, uint32_t ms) {
    uint32_t s = ms / 1000u;
    uint32_t m = s / 60u;
    s %= 60u;
    if (m > 99u) m = 99u;
    out[0] = (char)('0' + (m / 10u));
    out[1] = (char)('0' + (m % 10u));
    out[2] = ':';
    out[3] = (char)('0' + (s / 10u));
    out[4] = (char)('0' + (s % 10u));
    out[5] = 0;
}

static uint32_t wav_duration_ms(const uint8_t *d, uint32_t len) {
    if (len < 44) return 0;
    uint16_t ch = 0, bits = 0;
    uint32_t rate = 0, data = 0;
    uint32_t off = 12;
    while (off + 8u <= len) {
        uint32_t sz = rd32(d + off + 4);
        uint32_t body = off + 8u;
        if (d[off] == 'f' && d[off + 1] == 'm' && d[off + 2] == 't' && d[off + 3] == ' ') {
            if (body + 16u <= len) {
                ch = rd16(d + body + 2);
                rate = rd32(d + body + 4);
                bits = rd16(d + body + 14);
            }
        } else if (d[off] == 'd' && d[off + 1] == 'a' && d[off + 2] == 't' && d[off + 3] == 'a') {
            data = sz;
            if (data > len - body) data = len - body;
            break;
        }
        uint32_t adv = sz + (sz & 1u);
        if (adv < 8u) break;
        off = body + adv;
    }
    uint32_t bps = rate * (uint32_t)ch * (uint32_t)(bits / 8);
    if (!bps) return 0;
    return (uint32_t)(((uint64_t)data * 1000u) / bps);
}

static int is_wav(const char *name) {
    int L = slen(name);
    if (L < 5) return 0;
    char a = name[L - 4], b = name[L - 3], c = name[L - 2], d = name[L - 1];
    if (a != '.') return 0;
    if (!(b == 'w' || b == 'W')) return 0;
    if (!(c == 'a' || c == 'A')) return 0;
    if (!(d == 'v' || d == 'V')) return 0;
    return 1;
}

static void join_path(char *out, int cap, const char *dir, const char *name) {
    scpy(out, cap, dir);
    int L = slen(out);
    if (L == 0) { scpy(out, cap, name); return; }
    char last = out[L - 1];
    if (last != '/' && last != ':') scat(out, cap, "/");
    scat(out, cap, name);
}

/* dosyanin ilk 4 KB'indan sure etiketi uretir ("--:--" olur) */
static void probe_dur(const char *path, char *out, int cap) {
    scpy(out, cap, "--:--");
    if (!A->fs || !A->fs->read_path || !A->mem) return;
    uint8_t *b = (uint8_t *)A->mem->alloc(4096);
    if (!b) return;
    uint32_t rd = A->fs->read_path(path, b, 4096);
    uint32_t ms = wav_duration_ms(b, rd);
    A->mem->free(b);
    if (ms) mmss(out, ms);
}

static void add_file(const char *dir, const char *name) {
    if (g_n >= MP_MAX) return;
    if (!is_wav(name)) return;
    join_path(g_path[g_n], MP_PATH, dir, name);
    scpy(g_name[g_n], MP_NAME, name);
    {   /* ".wav" ekini at */
        int L = slen(g_name[g_n]);
        if (L >= 4) g_name[g_n][L - 4] = 0;
    }
    probe_dur(g_path[g_n], g_dur[g_n], sizeof(g_dur[g_n]));
    g_n++;
}

static void scan_dir(const char *dir) {
    if (!A->fs || !A->fs->count_dir || !A->fs->list_dir) return;
    int n = A->fs->count_dir(dir);
    if (n <= 0) return;
    for (int i = 0; i < n && g_n < MP_MAX; i++) {
        char nm[MP_NAME];
        int isd = 0;
        if (A->fs->list_dir(dir, i, nm, sizeof(nm), &isd) != 0) continue;
        if (isd) continue;
        add_file(dir, nm);
    }
}

/* ---- ikon tamponlari (pencere kapaninca birakilir) ---- */
#define ICON_MAX 6
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

#define SVG_NOTE \
 "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#7FA8F0\" stroke-width=\"1.4\">" \
 "<path d=\"M9 18V5l11-2v13\"/><circle cx=\"6\" cy=\"18\" r=\"3\"/><circle cx=\"17\" cy=\"16\" r=\"3\"/></svg>"
#define SVG_PREV \
 "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#A3A8B3\" stroke-width=\"1.8\">" \
 "<path d=\"M19 20L9 12l10-8v16z\"/><path d=\"M5 19V5\"/></svg>"
#define SVG_NEXT \
 "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#A3A8B3\" stroke-width=\"1.8\">" \
 "<path d=\"M5 4v16l10-8z\"/><path d=\"M19 5v14\"/></svg>"
#define SVG_PLAY \
 "<svg viewBox=\"0 0 24 24\"><path d=\"M8 5l11 7-11 7z\" fill=\"#FFFFFF\"/></svg>"
#define SVG_PAUSE \
 "<svg viewBox=\"0 0 24 24\"><path d=\"M7 5h4v14H7z M13 5h4v14h-4z\" fill=\"#FFFFFF\"/></svg>"
#define SVG_ROWPLAY \
 "<svg viewBox=\"0 0 24 24\"><path d=\"M7 4l13 8-13 8z\" fill=\"#3B82F6\"/></svg>"

/* ---- oynatma ---- */
static void paint_rows(void);
static void set_play_icon(void);

static void player_stop(void) {
    if (A->audio && A->audio->stop) A->audio->stop();
    if (g_buf && A->mem && A->mem->free) A->mem->free(g_buf);
    g_buf = 0;
    g_buflen = 0;
    g_playing = 0;
    set_play_icon();
}

static void set_status(const char *s) {
    if (g_sub) lv_label_set_text(g_sub, s ? s : "");
}

static void play_path(int i) {
    if (i < 0 || i >= g_n) return;
    if (!A->audio) { set_status("Ses surucusu yok"); return; }
    player_stop();

    asi_stat_t st;
    if (!A->fs || !A->fs->stat ||
        A->fs->stat(g_path[i], &st) != 0 || st.is_dir || st.size == 0) {
        set_status("Dosya okunamadi");
        return;
    }

    uint32_t sz = st.size;
    uint8_t *buf = (uint8_t *)A->mem->alloc(sz);
    if (!buf) { set_status("Bellek yetersiz"); return; }

    uint32_t rd = A->fs->read_path(g_path[i], buf, sz);
    if (rd == 0) {
        A->mem->free(buf);
        set_status("Dosya okunamadi");
        return;
    }

    if (A->audio->play_wav(buf, rd) != 0) {
        const char *er = A->audio->last_error ? A->audio->last_error() : "Oynatilamadi";
        A->mem->free(buf);
        set_status(er);
        return;
    }

    g_buf = buf;
    g_buflen = rd;
    g_dur_ms = wav_duration_ms(buf, rd);
    g_start_ms = (A->time && A->time->ticks_ms) ? A->time->ticks_ms() : 0;
    g_playing = 1;
    g_sel = i;

    if (g_dur[i][0] == '-' && g_dur[i][1] == '-') scpy(g_dur[i], sizeof(g_dur[i]), "");
    if (!g_dur[i][0] && g_dur_ms) mmss(g_dur[i], g_dur_ms);

    if (g_meta) lv_label_set_text(g_meta, g_name[i]);
    if (g_sub) lv_label_set_text(g_sub, "Mavi Hat - Arctian Yerel Kitaplik");
    if (g_bar) lv_bar_set_value(g_bar, 0, LV_ANIM_OFF);
    if (g_time && g_dur_ms) {
        char a[8];
        mmss(a, 0);
        lv_label_set_text(g_time, a);
    }
    if (g_total && g_dur_ms) {
        char b[8];
        mmss(b, g_dur_ms);
        lv_label_set_text(g_total, b);
    }
    set_play_icon();
    paint_rows();
}

static void paint_rows(void) {
    for (int k = 0; k < g_n; k++) {
        if (!g_rows[k]) continue;
        int on = (k == g_sel);
        lv_obj_set_style_bg_color(g_rows[k], lv_color_hex(on ? C_DIV : C_BODY), 0);
        uint32_t fg = on ? C_ACC : C_TXT;
        if (g_row_lbl[k]) lv_obj_set_style_text_color(g_row_lbl[k], lv_color_hex(fg), 0);
        if (g_row_num[k]) lv_obj_set_style_text_color(g_row_num[k],
                                    lv_color_hex(on ? C_ACC : C_MUT), 0);
    }
}

static void row_cb(lv_event_t *e) {
    play_path((int)(intptr_t)lv_event_get_user_data(e));
}

static void play_toggle_cb(lv_event_t *e) {
    (void)e;
    if (g_playing) {
        player_stop();
        set_status("Durdu");
    } else if (g_sel >= 0) {
        play_path(g_sel);
    }
}

static void prev_cb(lv_event_t *e) {
    (void)e;
    if (g_n <= 0) return;
    int i = g_sel <= 0 ? g_n - 1 : g_sel - 1;
    play_path(i);
}

static void next_cb(lv_event_t *e) {
    (void)e;
    if (g_n <= 0) return;
    int i = (g_sel + 1 >= g_n) ? 0 : g_sel + 1;
    play_path(i);
}

static void tick_cb(lv_timer_t *t) {
    (void)t;
    if (!A->audio) return;

    if (g_buf && A->audio->is_playing && !A->audio->is_playing()) {
        if (A->mem && A->mem->free) A->mem->free(g_buf);
        g_buf = 0;
        g_playing = 0;
        if (g_bar) lv_bar_set_value(g_bar, 1000, LV_ANIM_OFF);
        set_status("Bitti");
        set_play_icon();
        return;
    }

    if (g_playing && g_buf) {
        uint32_t now = (A->time && A->time->ticks_ms) ? A->time->ticks_ms() : 0;
        uint32_t el = now - g_start_ms;
        if (g_dur_ms && el > g_dur_ms) el = g_dur_ms;
        if (g_bar && g_dur_ms) {
            uint32_t v = (uint32_t)(((uint64_t)el * 1000u) / g_dur_ms);
            if (v > 1000u) v = 1000u;
            lv_bar_set_value(g_bar, (int32_t)v, LV_ANIM_OFF);
        }
        if (g_time) {
            char a[8];
            mmss(a, el);
            lv_label_set_text(g_time, a);
        }
    }
}

/* ---- arayuz ---- */
static lv_obj_t *mk_label(lv_obj_t *p, const char *txt, uint32_t col, lv_font_t *f) {
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    if (f) lv_obj_set_style_text_font(l, f, 0);
    return l;
}

static void build_cover_col(void) {
    lv_obj_t *col = lv_obj_create(g_win);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 300, LV_PCT(100));
    lv_obj_set_style_bg_color(col, lv_color_hex(C_BODY), 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(col, 22, 0);
    lv_obj_set_style_pad_row(col, 6, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    /* kapak */
    lv_obj_t *cov = lv_obj_create(col);
    lv_obj_remove_style_all(cov);
    lv_obj_set_size(cov, 236, 236);
    lv_obj_set_style_radius(cov, 12, 0);
    lv_obj_set_style_bg_color(cov, lv_color_hex(C_TRACK), 0);
    lv_obj_set_style_bg_opa(cov, LV_OPA_COVER, 0);
    lv_obj_remove_flag(cov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *note = svg_icon(cov, SVG_NOTE, 56);
    if (note) lv_obj_center(note);

    g_meta = mk_label(col, g_n > 0 ? g_name[g_sel >= 0 ? g_sel : 0] : "Parca secin",
                      C_TXT, f17);
    lv_label_set_long_mode(g_meta, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g_meta, 236);
    lv_obj_set_style_text_align(g_meta, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(g_meta, 6, 0);

    g_sub = mk_label(col, "Mavi Hat - Arctian Yerel Kitaplik", C_DIM, f12);
    lv_label_set_long_mode(g_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g_sub, 236);
    lv_obj_set_style_text_align(g_sub, LV_TEXT_ALIGN_CENTER, 0);
}

static void build_list(void) {
    g_list = lv_obj_create(g_win);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_height(g_list, LV_PCT(100));
    lv_obj_set_flex_grow(g_list, 1);
    lv_obj_set_style_bg_color(g_list, lv_color_hex(C_BODY), 0);
    lv_obj_set_style_bg_opa(g_list, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_top(g_list, 14, 0);
    lv_obj_set_style_pad_right(g_list, 16, 0);
    lv_obj_set_style_pad_row(g_list, 0, 0);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);

    /* baslik satiri: Parca / Sure */
    lv_obj_t *hdr = lv_obj_create(g_list);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_width(hdr, LV_PCT(100));
    lv_obj_set_height(hdr, 28);
    lv_obj_set_style_pad_hor(hdr, 12, 0);
    lv_obj_set_style_pad_bottom(hdr, 8, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    mk_label(hdr, "Parca", C_MUT, f12);
    mk_label(hdr, "Sure", C_MUT, f12);

    if (g_n == 0) {
        lv_obj_t *e = mk_label(g_list,
            "WAV dosyasi bulunamadi.\nC:/belgeler veya USB bellege .wav koyun.",
            C_MUT, f13);
        lv_label_set_long_mode(e, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(e, LV_PCT(90));
        lv_obj_set_style_pad_top(e, 20, 0);
        return;
    }

    for (int i = 0; i < g_n; i++) {
        lv_obj_t *r = lv_obj_create(g_list);
        lv_obj_remove_style_all(r);
        lv_obj_set_width(r, LV_PCT(100));
        lv_obj_set_height(r, 48);
        lv_obj_set_style_radius(r, 6, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(C_BODY), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(r, 12, 0);
        lv_obj_set_style_pad_column(r, 12, 0);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        g_rows[i] = r;

        char num[4];
        num[0] = (char)('0' + ((i + 1) / 10) % 10);
        num[1] = (char)('0' + (i + 1) % 10);
        if (i + 1 < 10) { num[0] = ' '; }
        num[2] = 0;
        g_row_num[i] = mk_label(r, num, C_MUT, f13);
        lv_obj_set_width(g_row_num[i], 20);

        g_row_lbl[i] = mk_label(r, g_name[i], C_TXT, f13);
        lv_label_set_long_mode(g_row_lbl[i], LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(g_row_lbl[i], 1);

        g_row_dur[i] = mk_label(r, g_dur[i], C_MUT, f12);
    }
    paint_rows();
}

static void build_controls(void) {
    lv_obj_t *bar = lv_obj_create(g_win);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 88);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_BODY), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_pad_ver(bar, 14, 0);
    lv_obj_set_style_pad_hor(bar, 22, 0);
    lv_obj_set_style_pad_row(bar, 12, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* zaman satiri: gecen | ilerleme | toplam */
    lv_obj_t *tr = lv_obj_create(bar);
    lv_obj_remove_style_all(tr);
    lv_obj_set_size(tr, LV_PCT(100), 14);
    lv_obj_set_flex_flow(tr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tr, 12, 0);
    lv_obj_remove_flag(tr, LV_OBJ_FLAG_SCROLLABLE);

    g_time = mk_label(tr, "00:00", C_MUT, f12);
    g_bar = lv_bar_create(tr);
    lv_obj_set_height(g_bar, 4);
    lv_obj_set_flex_grow(g_bar, 1);
    lv_bar_set_range(g_bar, 0, 1000);
    lv_bar_set_value(g_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(g_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_bar, lv_color_hex(C_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(g_bar, 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_bar, lv_color_hex(C_ACC), LV_PART_INDICATOR);
    g_total = mk_label(tr, "--:--", C_MUT, f12);

    /* kontrol satiri: onceki | oynat | sonraki */
    lv_obj_t *cr = lv_obj_create(bar);
    lv_obj_remove_style_all(cr);
    lv_obj_set_size(cr, LV_PCT(100), 40);
    lv_obj_set_flex_flow(cr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cr, 28, 0);
    lv_obj_remove_flag(cr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bp = lv_button_create(cr);
    lv_obj_set_size(bp, 28, 28);
    lv_obj_set_style_radius(bp, 0, 0);
    lv_obj_set_style_bg_opa(bp, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bp, 0, 0);
    lv_obj_set_style_shadow_width(bp, 0, 0);
    lv_obj_set_style_pad_all(bp, 0, 0);
    lv_obj_t *icp = svg_icon(bp, SVG_PREV, 24);
    if (icp) lv_obj_center(icp);
    lv_obj_add_event_cb(bp, prev_cb, LV_EVENT_CLICKED, 0);

    g_play_btn = lv_button_create(cr);
    lv_obj_set_size(g_play_btn, 40, 40);
    lv_obj_set_style_radius(g_play_btn, 20, 0);
    lv_obj_set_style_bg_color(g_play_btn, lv_color_hex(C_ACC), 0);
    lv_obj_set_style_bg_opa(g_play_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_play_btn, 0, 0);
    lv_obj_set_style_shadow_width(g_play_btn, 0, 0);
    lv_obj_set_style_pad_all(g_play_btn, 0, 0);
    lv_obj_add_event_cb(g_play_btn, play_toggle_cb, LV_EVENT_CLICKED, 0);

    lv_obj_t *bn = lv_button_create(cr);
    lv_obj_set_size(bn, 28, 28);
    lv_obj_set_style_radius(bn, 0, 0);
    lv_obj_set_style_bg_opa(bn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bn, 0, 0);
    lv_obj_set_style_shadow_width(bn, 0, 0);
    lv_obj_set_style_pad_all(bn, 0, 0);
    lv_obj_t *icn = svg_icon(bn, SVG_NEXT, 24);
    if (icn) lv_obj_center(icn);
    lv_obj_add_event_cb(bn, next_cb, LV_EVENT_CLICKED, 0);
}

static void win_del_cb(lv_event_t *e) {
    (void)e;
    player_stop();
    if (g_timer) { lv_timer_delete(g_timer); g_timer = 0; }
    for (int i = 0; i < s_icon_n; i++)
        if (A->mem) A->mem->free(s_icon_buf[i]);
    s_icon_n = 0;
    g_win = 0; g_list = 0; g_meta = 0; g_sub = 0; g_time = 0;
    g_total = 0; g_bar = 0; g_play_btn = 0; g_play_ic = 0;
}

/* oynat/dur ikonu: 40 px'lik mavi dairenin icindeki canvas yenilenir */
static void set_play_icon(void) {
    if (!g_play_btn) return;
    lv_obj_clean(g_play_btn);
    g_play_ic = svg_icon(g_play_btn, g_playing ? SVG_PAUSE : SVG_PLAY, 14);
    if (g_play_ic) lv_obj_center(g_play_ic);
}

void app_entry(lv_obj_t *win, asi_t *asi) {
    A = asi;
    g_win = win;
    g_list = 0; g_meta = 0; g_sub = 0; g_time = 0; g_total = 0;
    g_bar = 0; g_play_btn = 0; g_play_ic = 0; g_timer = 0;
    g_n = 0; g_sel = -1;
    g_buf = 0; g_buflen = 0; g_dur_ms = 0; g_start_ms = 0; g_playing = 0;

    if (!f17) f17 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 17);
    if (!f13) f13 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 13);
    if (!f12) f12 = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 12);

    if (A->fs) {
        scan_dir("C:");
        scan_dir("C:/belgeler");
        scan_dir("C:/indirilenler");
        scan_dir("C:/masaustu");
        scan_dir("D:");
        scan_dir("D:/belgeler");
    }
    if (g_n > 0) g_sel = 0;

    /* Hedef dosya verildiyse (orn. masaustunde .wav cift tiki) onu
     * oynaticiya yukle ve baslat */
    int autoplay = -1;
    if (arctian_open_target && arctian_open_target[0] && is_wav(arctian_open_target)) {
        for (int i = 0; i < g_n; i++) {
            const char *a = g_path[i], *b = arctian_open_target;
            int same = 1;
            while (*a && *b) { if (*a != *b) { same = 0; break; } a++; b++; }
            if (same && !*a && !*b) { autoplay = i; break; }
        }
        if (autoplay < 0 && g_n < MP_MAX) {
            scpy(g_path[g_n], MP_PATH, arctian_open_target);
            const char *nm = arctian_open_target;
            for (const char *p = nm; *p; p++) if (*p == '/') nm = p + 1;
            scpy(g_name[g_n], MP_NAME, nm);
            {   int L = slen(g_name[g_n]); if (L >= 4) g_name[g_n][L - 4] = 0; }
            probe_dur(g_path[g_n], g_dur[g_n], sizeof(g_dur[g_n]));
            autoplay = g_n++;
        }
        arctian_open_target = "";       /* hedefi tuket (tekrar oynatma engeli) */
    }
    if (autoplay >= 0) g_sel = autoplay;

    /* govde: ustte kapak+liste (satir), altta kontrol cubugu */
    lv_obj_set_flex_flow(win, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(win, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(win, 0, 0);
    lv_obj_set_style_pad_row(win, 0, 0);
    lv_obj_set_style_bg_color(win, lv_color_hex(C_BODY), 0);
    lv_obj_set_style_bg_opa(win, LV_OPA_COVER, 0);
    lv_obj_remove_flag(win, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *top = lv_obj_create(win);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(top, 1);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    /* kapak sütunu ve liste 'top' altina kurulur */
    lv_obj_set_flex_flow(win, LV_FLEX_FLOW_COLUMN);
    {
        lv_obj_t *sv = top;   /* yer degisikligi: build_* win yerine top kullanir */
        g_win = sv;
        build_cover_col();
        build_list();
        g_win = win;
    }
    build_controls();

    set_play_icon();

    if (autoplay >= 0 && A->audio) play_path(autoplay);

    g_timer = lv_timer_create(tick_cb, 250, 0);
    lv_obj_add_event_cb(win, win_del_cb, LV_EVENT_DELETE, 0);
}
