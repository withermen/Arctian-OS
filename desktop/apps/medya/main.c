/* ==================================================================
 * Medya Oynatıcı — .wav, .mp3, .ogg, .flac Yerel Ses Oynatıcısı
 *
 * Özellikler:
 *   - Çoklu biçim çözme (medya_dec.h): WAV, MP3, OGG, FLAC
 *   - Sarma çubuğu (Seek slider), Duraklat / Devam Et
 *   - Ses seviyesi sürgüsü (g_asi->audio->set_volume)
 *   - Çalma listesi (C:/, C:/belgeler, C:/indirilenler, C:/masaustu, D:/)
 *   - Desteklenmeyen/bozuk dosyalarda temiz bildirim (arctian_notify)
 *   - relayout desteği ile yeniden boyutlandırma uyumu
 * ================================================================== */
#include "app.h"
#include "adl_wolf.h"
#include "svg.h"
#include "medya_dec.h"
#include <stdint.h>

#define MP_MAX   64
#define MP_PATH  200
#define MP_NAME  96

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

static asi_t      *A;
static lv_obj_t   *g_win;
static lv_obj_t   *g_top;
static lv_obj_t   *g_list;
static lv_obj_t   *g_meta;
static lv_obj_t   *g_sub;
static lv_obj_t   *g_time;
static lv_obj_t   *g_total;
static lv_obj_t   *g_seek;
static lv_obj_t   *g_vol_sl;
static lv_obj_t   *g_vol_lbl;
static lv_obj_t   *g_play_btn;
static lv_obj_t   *g_play_ic;
static lv_timer_t *g_timer;

static char        g_path[MP_MAX][MP_PATH];
static char        g_name[MP_MAX][MP_NAME];
static char        g_fmt[MP_MAX][8];
static char        g_dur[MP_MAX][8];
static lv_obj_t   *g_rows[MP_MAX];
static lv_obj_t   *g_row_lbl[MP_MAX];
static lv_obj_t   *g_row_num[MP_MAX];
static lv_obj_t   *g_row_dur[MP_MAX];
static int         g_n;
static int         g_sel;

static medya_audio_t g_aud;        /* çözülen tam WAV/PCM tamponu */
static uint8_t      *g_slice_buf;  /* seek sonrası oynatma dilimi */
static uint32_t      g_dur_ms;
static uint32_t      g_base_ms;    /* son başlatma/seek konumu */
static uint32_t      g_start_ms;   /* son başlatma tick_ms */
static uint32_t      g_pause_ms;   /* duraklatıldığındaki konum */
static int           g_playing;
static int           g_paused;
static int           g_seeking;

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

static const char *audio_fmt_tag(const char *name) {
    if (md_ext_eq(name, "wav"))  return "WAV";
    if (md_ext_eq(name, "mp3"))  return "MP3";
    if (md_ext_eq(name, "ogg"))  return "OGG";
    if (md_ext_eq(name, "flac")) return "FLAC";
    return 0;
}

static void join_path(char *out, int cap, const char *dir, const char *name) {
    scpy(out, cap, dir);
    int L = slen(out);
    if (L == 0) { scpy(out, cap, name); return; }
    char last = out[L - 1];
    if (last != '/' && last != ':') scat(out, cap, "/");
    scat(out, cap, name);
}

static void strip_ext(const char *name, char *out, int cap) {
    scpy(out, cap, name);
    int dot = -1;
    for (int i = 0; out[i]; i++) if (out[i] == '.') dot = i;
    if (dot > 0) out[dot] = 0;
}

static void probe_dur(const char *path, const char *name, char *out, int cap) {
    scpy(out, cap, "--:--");
    if (!A->fs || !A->fs->read_path || !A->mem) return;
    uint32_t fsz = 0;
    asi_stat_t st;
    if (A->fs->stat && A->fs->stat(path, &st) == 0) fsz = st.size;
    uint8_t *b = (uint8_t *)A->mem->alloc(8192);
    if (!b) return;
    uint32_t rd = A->fs->read_path(path, b, 8192);
    uint32_t ms = medya_probe_duration(b, rd, fsz, name);
    A->mem->free(b);
    if (ms) mmss(out, ms);
}

static void add_file(const char *dir, const char *name) {
    if (g_n >= MP_MAX) return;
    const char *ft = audio_fmt_tag(name);
    if (!ft) return;
    char full[MP_PATH];
    join_path(full, sizeof(full), dir, name);
    for (int i = 0; i < g_n; i++) if (seq0(g_path[i], full)) return;
    scpy(g_path[g_n], MP_PATH, full);
    strip_ext(name, g_name[g_n], MP_NAME);
    scpy(g_fmt[g_n], sizeof(g_fmt[0]), ft);
    probe_dur(g_path[g_n], name, g_dur[g_n], sizeof(g_dur[0]));
    g_n++;
}

static void scan_dir(const char *dir) {
    if (!A->fs || !A->fs->count_dir || !A->fs->list_dir) return;
    int n = A->fs->count_dir(dir);
    if (n <= 0) return;
    for (int i = 0; i < n && g_n < MP_MAX; i++) {
        char nm[MP_NAME];
        int isd = 0;
        if (A->fs->list_dir(dir, i, nm, sizeof(nm), &isd) != 0 || isd) continue;
        add_file(dir, nm);
    }
}

#define ICON_MAX 8
static uint32_t *s_icon_buf[ICON_MAX];
static int s_icon_n;

static lv_obj_t *svg_icon(lv_obj_t *parent, const char *svg, int size) {
    if (!svg[0] || !A->mem || s_icon_n >= ICON_MAX) return 0;
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
#define SVG_VOL \
 "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#8A909B\" stroke-width=\"1.8\">" \
 "<path d=\"M11 5L6 9H2v6h4l5 4V5z\"/><path d=\"M15.5 8.5a5 5 0 0 1 0 7\"/></svg>"

static void paint_rows(void);
static void set_play_icon(void);

static void free_audio_buffers(void) {
    if (g_slice_buf && A->mem) A->mem->free(g_slice_buf);
    g_slice_buf = 0;
    if (g_aud.wav_buf && A->mem) A->mem->free(g_aud.wav_buf);
    memset(&g_aud, 0, sizeof(g_aud));
}

static void player_stop(int keep_decoded) {
    if (A->audio && A->audio->stop) A->audio->stop();
    if (!keep_decoded) {
        free_audio_buffers();
        g_dur_ms = 0;
        g_base_ms = 0;
        g_pause_ms = 0;
        g_paused = 0;
    }
    g_playing = 0;
    set_play_icon();
}

static void set_status(const char *s) {
    if (g_sub) lv_label_set_text(g_sub, s ? s : "");
}

static int start_from_ms(uint32_t offset_ms) {
    if (!g_aud.wav_buf || g_aud.pcm_len == 0 || !A->audio || !A->audio->play_wav) return -1;
    if (offset_ms >= g_dur_ms && g_dur_ms > 0) offset_ms = 0;

    if (A->audio->stop) A->audio->stop();
    if (g_slice_buf && A->mem) { A->mem->free(g_slice_buf); g_slice_buf = 0; }

    uint32_t align = (uint32_t)g_aud.channels * 2u;
    uint32_t byte_off = (uint32_t)(((uint64_t)offset_ms * (uint64_t)g_aud.rate / 1000ull) * (uint64_t)align);
    if (byte_off >= g_aud.pcm_len) byte_off = 0;

    const uint8_t *play_buf = g_aud.wav_buf;
    uint32_t play_len = g_aud.wav_len;

    if (byte_off > 0) {
        uint32_t rem = g_aud.pcm_len - byte_off;
        g_slice_buf = (uint8_t *)A->mem->alloc(44u + rem);
        if (!g_slice_buf) return -1;
        md_write_wav_hdr(g_slice_buf, g_aud.rate, g_aud.channels, rem);
        memcpy(g_slice_buf + 44u, g_aud.wav_buf + g_aud.pcm_off + byte_off, rem);
        play_buf = g_slice_buf;
        play_len = 44u + rem;
    }

    if (A->audio->play_wav(play_buf, play_len) != 0) {
        const char *er = A->audio->last_error ? A->audio->last_error() : "Ses çalınamadı";
        set_status(er);
        return -1;
    }
    g_base_ms = offset_ms;
    g_start_ms = (A->time && A->time->ticks_ms) ? (uint32_t)A->time->ticks_ms() : 0;
    g_playing = 1;
    g_paused = 0;
    set_play_icon();
    return 0;
}

static void play_path(int i) {
    if (i < 0 || i >= g_n) return;
    if (!A->audio) { set_status("Ses sürücüsü yok"); return; }
    player_stop(0);

    asi_stat_t st;
    if (!A->fs || !A->fs->stat ||
        A->fs->stat(g_path[i], &st) != 0 || st.is_dir || st.size == 0) {
        set_status("Dosya okunamadı");
        return;
    }
    if (st.size > 24u * 1024u * 1024u) {
        set_status("Dosya boyutu çok büyük");
        arctian_notify("Medya Oynatıcı", "Dosya 24 MB sınırını aşıyor");
        return;
    }

    uint8_t *raw = (uint8_t *)A->mem->alloc(st.size);
    if (!raw) { set_status("Bellek yetersiz"); return; }
    uint32_t rd = A->fs->read_path(g_path[i], raw, st.size);
    if (rd == 0) {
        A->mem->free(raw);
        set_status("Dosya okunamadı");
        return;
    }

    int rc = medya_decode_audio(raw, rd, g_path[i], A->mem->alloc, A->mem->free, &g_aud);
    A->mem->free(raw);
    if (rc != 0) {
        const char *err = g_aud.err ? g_aud.err : "Desteklenmeyen ses dosyası";
        set_status(err);
        arctian_notify("Medya Oynatıcı", err);
        free_audio_buffers();
        return;
    }

    g_dur_ms = g_aud.dur_ms;
    g_sel = i;
    if (g_dur_ms) {
        mmss(g_dur[i], g_dur_ms);
        if (g_row_dur[i]) lv_label_set_text(g_row_dur[i], g_dur[i]);
    }

    if (start_from_ms(0) != 0) {
        free_audio_buffers();
        return;
    }

    if (g_meta) lv_label_set_text(g_meta, g_name[i]);
    {
        char sub[64], rb[12];
        scpy(sub, sizeof(sub), g_aud.fmt_name ? g_aud.fmt_name : g_fmt[i]);
        scat(sub, sizeof(sub), " · ");
        u2s(g_aud.rate / 1000u, rb, sizeof(rb));
        scat(sub, sizeof(sub), rb);
        scat(sub, sizeof(sub), " kHz · ");
        scat(sub, sizeof(sub), g_aud.channels == 1 ? "Mono" : "Stereo");
        set_status(sub);
    }
    if (g_seek) lv_slider_set_value(g_seek, 0, LV_ANIM_OFF);
    if (g_time) lv_label_set_text(g_time, "00:00");
    if (g_total && g_dur_ms) {
        char b[8];
        mmss(b, g_dur_ms);
        lv_label_set_text(g_total, b);
    }
    paint_rows();
}

static void paint_rows(void) {
    for (int k = 0; k < g_n; k++) {
        if (!g_rows[k]) continue;
        int on = (k == g_sel);
        lv_obj_set_style_bg_color(g_rows[k], lv_color_hex(on ? C_DIV : C_BODY), 0);
        uint32_t fg = on ? C_ACC : C_TXT;
        if (g_row_lbl[k]) lv_obj_set_style_text_color(g_row_lbl[k], lv_color_hex(fg), 0);
        if (g_row_num[k]) lv_obj_set_style_text_color(g_row_num[k], lv_color_hex(on ? C_ACC : C_MUT), 0);
    }
}

static void row_cb(lv_event_t *e) {
    play_path((int)(intptr_t)lv_event_get_user_data(e));
}

static void play_toggle_cb(lv_event_t *e) {
    (void)e;
    if (g_playing) {
        uint32_t now = (A->time && A->time->ticks_ms) ? (uint32_t)A->time->ticks_ms() : 0;
        g_pause_ms = g_base_ms + (now - g_start_ms);
        if (g_dur_ms && g_pause_ms > g_dur_ms) g_pause_ms = g_dur_ms;
        player_stop(1);
        g_paused = 1;
        set_status("Duraklatıldı");
    } else if (g_paused && g_aud.wav_buf) {
        if (start_from_ms(g_pause_ms) == 0) {
            set_status("Devam ediyor");
        }
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

static void seek_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSING) {
        g_seeking = 1;
        if (g_dur_ms && g_time) {
            int32_t v = lv_slider_get_value(g_seek);
            uint32_t ms = (uint32_t)(((uint64_t)v * g_dur_ms) / 1000ull);
            char a[8]; mmss(a, ms);
            lv_label_set_text(g_time, a);
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_VALUE_CHANGED) {
        if (code == LV_EVENT_VALUE_CHANGED && g_seeking) return;
        g_seeking = 0;
        if (!g_aud.wav_buf || g_dur_ms == 0) return;
        int32_t v = lv_slider_get_value(g_seek);
        uint32_t target_ms = (uint32_t)(((uint64_t)v * g_dur_ms) / 1000ull);
        if (g_playing) {
            start_from_ms(target_ms);
        } else {
            g_pause_ms = target_ms;
            g_paused = 1;
            if (g_time) { char a[8]; mmss(a, target_ms); lv_label_set_text(g_time, a); }
        }
    }
}

static void vol_cb(lv_event_t *e) {
    (void)e;
    if (!g_vol_sl) return;
    int v = (int)lv_slider_get_value(g_vol_sl);
    if (A->audio && A->audio->set_volume) A->audio->set_volume(v);
    if (g_vol_lbl) {
        char vb[12], nb[8];
        vb[0] = '%';
        u2s((unsigned int)v, nb, sizeof(nb));
        scpy(vb + 1, sizeof(vb) - 1, nb);
        lv_label_set_text(g_vol_lbl, vb);
    }
}

static void tick_cb(lv_timer_t *t) {
    (void)t;
    if (!A->audio) return;

    if (g_playing && A->audio->is_playing && !A->audio->is_playing()) {
        g_playing = 0;
        g_paused = 0;
        g_pause_ms = 0;
        if (g_seek) lv_slider_set_value(g_seek, 1000, LV_ANIM_OFF);
        set_status("Bitti");
        set_play_icon();
        return;
    }

    if (g_playing && g_aud.wav_buf && !g_seeking) {
        uint32_t now = (A->time && A->time->ticks_ms) ? (uint32_t)A->time->ticks_ms() : 0;
        uint32_t el = g_base_ms + (now - g_start_ms);
        if (g_dur_ms && el > g_dur_ms) el = g_dur_ms;
        if (g_seek && g_dur_ms) {
            uint32_t v = (uint32_t)(((uint64_t)el * 1000u) / g_dur_ms);
            if (v > 1000u) v = 1000u;
            lv_slider_set_value(g_seek, (int32_t)v, LV_ANIM_OFF);
        }
        if (g_time) {
            char a[8];
            mmss(a, el);
            lv_label_set_text(g_time, a);
        }
    }
}

static lv_obj_t *mk_label(lv_obj_t *p, const char *txt, uint32_t col, lv_font_t *f) {
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    if (f) lv_obj_set_style_text_font(l, f, 0);
    return l;
}

static void build_cover_col(void) {
    lv_obj_t *col = lv_obj_create(g_top);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 290, LV_PCT(100));
    lv_obj_set_style_bg_color(col, lv_color_hex(C_BODY), 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(col, 20, 0);
    lv_obj_set_style_pad_row(col, 6, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cov = lv_obj_create(col);
    lv_obj_remove_style_all(cov);
    lv_obj_set_size(cov, 220, 220);
    lv_obj_set_style_radius(cov, 12, 0);
    lv_obj_set_style_bg_color(cov, lv_color_hex(C_TRACK), 0);
    lv_obj_set_style_bg_opa(cov, LV_OPA_COVER, 0);
    lv_obj_remove_flag(cov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *note = svg_icon(cov, SVG_NOTE, 56);
    if (note) lv_obj_center(note);

    g_meta = mk_label(col, g_n > 0 ? g_name[g_sel >= 0 ? g_sel : 0] : "Parça seçin", C_TXT, f17);
    lv_label_set_long_mode(g_meta, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g_meta, 230);
    lv_obj_set_style_text_align(g_meta, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(g_meta, 6, 0);

    g_sub = mk_label(col, "WAV · MP3 · OGG · FLAC", C_DIM, f12);
    lv_label_set_long_mode(g_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g_sub, 230);
    lv_obj_set_style_text_align(g_sub, LV_TEXT_ALIGN_CENTER, 0);
}

static void build_list(void) {
    g_list = lv_obj_create(g_top);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_height(g_list, LV_PCT(100));
    lv_obj_set_flex_grow(g_list, 1);
    lv_obj_set_style_bg_color(g_list, lv_color_hex(C_BODY), 0);
    lv_obj_set_style_bg_opa(g_list, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_top(g_list, 14, 0);
    lv_obj_set_style_pad_right(g_list, 16, 0);
    lv_obj_set_style_pad_row(g_list, 2, 0);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);

    lv_obj_t *hdr = lv_obj_create(g_list);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_width(hdr, LV_PCT(100));
    lv_obj_set_height(hdr, 28);
    lv_obj_set_style_pad_hor(hdr, 12, 0);
    lv_obj_set_style_pad_bottom(hdr, 8, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    mk_label(hdr, "Çalma Listesi", C_MUT, f12);
    mk_label(hdr, "Süre", C_MUT, f12);

    if (g_n == 0) {
        lv_obj_t *e = mk_label(g_list,
            "Ses dosyası bulunamadı.\nC:/, C:/belgeler veya C:/indirilenler klasörüne .wav, .mp3, .ogg veya .flac koyun.",
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
        lv_obj_set_height(r, 46);
        lv_obj_set_style_radius(r, 6, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(C_BODY), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(r, 12, 0);
        lv_obj_set_style_pad_column(r, 10, 0);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        g_rows[i] = r;

        char num[4];
        num[0] = (i + 1 < 10) ? ' ' : (char)('0' + ((i + 1) / 10) % 10);
        num[1] = (char)('0' + (i + 1) % 10);
        num[2] = 0;
        g_row_num[i] = mk_label(r, num, C_MUT, f13);
        lv_obj_set_width(g_row_num[i], 22);

        g_row_lbl[i] = mk_label(r, g_name[i], C_TXT, f13);
        lv_label_set_long_mode(g_row_lbl[i], LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(g_row_lbl[i], 1);

        lv_obj_t *fmt = mk_label(r, g_fmt[i], C_DIM, f12);
        lv_obj_set_width(fmt, 40);

        g_row_dur[i] = mk_label(r, g_dur[i], C_MUT, f12);
    }
    paint_rows();
}

static void build_controls(void) {
    lv_obj_t *bar = lv_obj_create(g_win);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 92);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_BODY), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_pad_ver(bar, 12, 0);
    lv_obj_set_style_pad_hor(bar, 22, 0);
    lv_obj_set_style_pad_row(bar, 10, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* Zaman + Sarma Çubuğu (Seek slider) */
    lv_obj_t *tr = lv_obj_create(bar);
    lv_obj_remove_style_all(tr);
    lv_obj_set_size(tr, LV_PCT(100), 18);
    lv_obj_set_flex_flow(tr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tr, 12, 0);
    lv_obj_remove_flag(tr, LV_OBJ_FLAG_SCROLLABLE);

    g_time = mk_label(tr, "00:00", C_MUT, f12);
    g_seek = lv_slider_create(tr);
    lv_obj_set_height(g_seek, 5);
    lv_obj_set_flex_grow(g_seek, 1);
    lv_slider_set_range(g_seek, 0, 1000);
    lv_slider_set_value(g_seek, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_seek, lv_color_hex(C_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_seek, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_seek, lv_color_hex(C_ACC), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_seek, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(g_seek, 3, LV_PART_KNOB);
    lv_obj_add_event_cb(g_seek, seek_cb, LV_EVENT_PRESSING, 0);
    lv_obj_add_event_cb(g_seek, seek_cb, LV_EVENT_RELEASED, 0);
    lv_obj_add_event_cb(g_seek, seek_cb, LV_EVENT_VALUE_CHANGED, 0);
    g_total = mk_label(tr, "--:--", C_MUT, f12);

    /* Kontrol satırı: sol boşluk | önceki/oynat/sonraki | ses seviyesi sürgüsü */
    lv_obj_t *cr = lv_obj_create(bar);
    lv_obj_remove_style_all(cr);
    lv_obj_set_size(cr, LV_PCT(100), 42);
    lv_obj_set_flex_flow(cr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(cr, LV_OBJ_FLAG_SCROLLABLE);

    /* Sol denge kutusu */
    lv_obj_t *lsp = lv_obj_create(cr);
    lv_obj_remove_style_all(lsp);
    lv_obj_set_size(lsp, 160, 20);

    /* Orta kontroller */
    lv_obj_t *mid = lv_obj_create(cr);
    lv_obj_remove_style_all(mid);
    lv_obj_set_size(mid, LV_SIZE_CONTENT, 40);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(mid, 24, 0);
    lv_obj_remove_flag(mid, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bp = lv_button_create(mid);
    lv_obj_set_size(bp, 28, 28);
    lv_obj_set_style_bg_opa(bp, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bp, 0, 0);
    lv_obj_set_style_shadow_width(bp, 0, 0);
    lv_obj_set_style_pad_all(bp, 0, 0);
    lv_obj_t *icp = svg_icon(bp, SVG_PREV, 22);
    if (icp) lv_obj_center(icp);
    lv_obj_add_event_cb(bp, prev_cb, LV_EVENT_CLICKED, 0);

    g_play_btn = lv_button_create(mid);
    lv_obj_set_size(g_play_btn, 40, 40);
    lv_obj_set_style_radius(g_play_btn, 20, 0);
    lv_obj_set_style_bg_color(g_play_btn, lv_color_hex(C_ACC), 0);
    lv_obj_set_style_bg_opa(g_play_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_play_btn, 0, 0);
    lv_obj_set_style_shadow_width(g_play_btn, 0, 0);
    lv_obj_set_style_pad_all(g_play_btn, 0, 0);
    lv_obj_add_event_cb(g_play_btn, play_toggle_cb, LV_EVENT_CLICKED, 0);

    lv_obj_t *bn = lv_button_create(mid);
    lv_obj_set_size(bn, 28, 28);
    lv_obj_set_style_bg_opa(bn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bn, 0, 0);
    lv_obj_set_style_shadow_width(bn, 0, 0);
    lv_obj_set_style_pad_all(bn, 0, 0);
    lv_obj_t *icn = svg_icon(bn, SVG_NEXT, 22);
    if (icn) lv_obj_center(icn);
    lv_obj_add_event_cb(bn, next_cb, LV_EVENT_CLICKED, 0);

    /* Sağ Ses Seviyesi Sürgüsü */
    lv_obj_t *vbox = lv_obj_create(cr);
    lv_obj_remove_style_all(vbox);
    lv_obj_set_size(vbox, 160, 28);
    lv_obj_set_flex_flow(vbox, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vbox, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(vbox, 8, 0);
    lv_obj_remove_flag(vbox, LV_OBJ_FLAG_SCROLLABLE);

    svg_icon(vbox, SVG_VOL, 16);
    int cur_vol = (A->audio && A->audio->volume) ? A->audio->volume() : 80;
    if (cur_vol < 0 || cur_vol > 100) cur_vol = 80;

    g_vol_sl = lv_slider_create(vbox);
    lv_obj_set_size(g_vol_sl, 88, 5);
    lv_slider_set_range(g_vol_sl, 0, 100);
    lv_slider_set_value(g_vol_sl, cur_vol, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_vol_sl, lv_color_hex(C_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_vol_sl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_vol_sl, lv_color_hex(C_ACC), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_vol_sl, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(g_vol_sl, 3, LV_PART_KNOB);
    lv_obj_add_event_cb(g_vol_sl, vol_cb, LV_EVENT_VALUE_CHANGED, 0);

    char vtxt[12], nb[8];
    vtxt[0] = '%';
    u2s((unsigned int)cur_vol, nb, sizeof(nb));
    scpy(vtxt + 1, sizeof(vtxt) - 1, nb);
    g_vol_lbl = mk_label(vbox, vtxt, C_DIM, f12);
    lv_obj_set_width(g_vol_lbl, 34);
}

static void set_play_icon(void) {
    if (!g_play_btn) return;
    lv_obj_clean(g_play_btn);
    g_play_ic = svg_icon(g_play_btn, g_playing ? SVG_PAUSE : SVG_PLAY, 14);
    if (g_play_ic) lv_obj_center(g_play_ic);
}

static void mp_relayout(lv_obj_t *win) {
    (void)win;
    if (g_win) lv_obj_update_layout(g_win);
}

static void win_del_cb(lv_event_t *e) {
    (void)e;
    player_stop(0);
    if (g_timer) { lv_timer_delete(g_timer); g_timer = 0; }
    for (int i = 0; i < s_icon_n; i++)
        if (A->mem) A->mem->free(s_icon_buf[i]);
    s_icon_n = 0;
    g_win = 0; g_top = 0; g_list = 0; g_meta = 0; g_sub = 0;
    g_time = 0; g_total = 0; g_seek = 0; g_vol_sl = 0; g_vol_lbl = 0;
    g_play_btn = 0; g_play_ic = 0;
}

void app_entry(lv_obj_t *win, asi_t *asi) {
    A = asi;
    g_win = win;
    g_top = 0; g_list = 0; g_meta = 0; g_sub = 0; g_time = 0; g_total = 0;
    g_seek = 0; g_vol_sl = 0; g_vol_lbl = 0; g_play_btn = 0; g_play_ic = 0; g_timer = 0;
    g_n = 0; g_sel = -1;
    memset(&g_aud, 0, sizeof(g_aud));
    g_slice_buf = 0;
    g_dur_ms = 0; g_base_ms = 0; g_start_ms = 0; g_pause_ms = 0;
    g_playing = 0; g_paused = 0; g_seeking = 0;

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

    int autoplay = -1;
    if (arctian_open_target && arctian_open_target[0] && audio_fmt_tag(arctian_open_target)) {
        for (int i = 0; i < g_n; i++) {
            if (seq0(g_path[i], arctian_open_target)) { autoplay = i; break; }
        }
        if (autoplay < 0 && g_n < MP_MAX) {
            scpy(g_path[g_n], MP_PATH, arctian_open_target);
            const char *nm = arctian_open_target;
            for (const char *p = nm; *p; p++) if (*p == '/') nm = p + 1;
            strip_ext(nm, g_name[g_n], MP_NAME);
            scpy(g_fmt[g_n], sizeof(g_fmt[0]), audio_fmt_tag(nm));
            probe_dur(g_path[g_n], nm, g_dur[g_n], sizeof(g_dur[0]));
            autoplay = g_n++;
        }
        arctian_open_target = "";
    }
    if (autoplay >= 0) g_sel = autoplay;

    lv_obj_set_flex_flow(win, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(win, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(win, 0, 0);
    lv_obj_set_style_pad_row(win, 0, 0);
    lv_obj_set_style_bg_color(win, lv_color_hex(C_BODY), 0);
    lv_obj_set_style_bg_opa(win, LV_OPA_COVER, 0);
    lv_obj_remove_flag(win, LV_OBJ_FLAG_SCROLLABLE);

    g_top = lv_obj_create(win);
    lv_obj_remove_style_all(g_top);
    lv_obj_set_width(g_top, LV_PCT(100));
    lv_obj_set_flex_grow(g_top, 1);
    lv_obj_set_flex_flow(g_top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(g_top, 0, 0);
    lv_obj_remove_flag(g_top, LV_OBJ_FLAG_SCROLLABLE);

    build_cover_col();
    build_list();
    build_controls();
    set_play_icon();

    if (autoplay >= 0 && A->audio) play_path(autoplay);

    g_timer = lv_timer_create(tick_cb, 250, 0);
    arctian_shell_set_relayout(win, mp_relayout);
    lv_obj_add_event_cb(win, win_del_cb, LV_EVENT_DELETE, 0);
}
