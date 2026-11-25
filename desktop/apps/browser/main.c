/* Arctian gelismis tarayici.
 * Adres cubuguna bir URL yazip "Git"e bas (veya Enter). HTTP/HTTPS uzerinden
 * sayfayi ceker, HTML'i basit bloklara ayristirir (baslik, paragraf, link,
 * liste ogesi, resim yer tutucusu, yatay cizgi, form alanlari, secim kutusu)
 * ve bunlari LVGL nesneleri olarak stilize edip gosterir. Linklere tiklayinca
 * sayfa icinde gezinilir, "<" butonu ile gecmise donulur. Formlar (orn. Google
 * arama kutusu) gercekten calisir: metin girilebilir, "Gonder"e basilinca ya
 * da Enter'a basilinca form action'ina GET sorgu dizesi olarak eklenip
 * gezinilir.
 *
 * Icerik iki katmanda cizilir: (1) agdan gelen HTML, <style> ve harici
 * stylesheet'lerden kurulan gercek bir CSS alt kumesi motoruyla,
 * (2) <img> kaynaklari agdan cekilip gercek PNG cozucuyle cozulerek. Motor
 * satir ici/klas/kimlik/soy selectors, kutu modeli, flex, tablo hucreleri,
 * form alanlari ve basit yerlesim kurallarini destekler.
 *
 * Kisitlar: libc yok, tum bellek statik. JS calistirilmaz; CSS'in yalnizca
 * bir alt kumesi ve PNG resim formati desteklenir. */
#include "app.h"
#include "lv_port.h"
#include "adl_wolf.h"
#include "png.h"
#include "svg.h"
#include "browser_img.h"
#include "browser_canvas.h"
#include "browser_pdf.h"
#include <stdint.h>

extern const unsigned char font_ttf[];
extern const unsigned int  font_ttf_len;

/* ---------------- Sabitler ---------------- */
#define MAX_BLOCKS     240
#define BLOCK_TEXT_MAX 220
#define HREF_MAX       220     /* linklerde URL, formlarda deger listesi tutar */
#define NAME_MAX       48
#define URL_MAX        300
#define PAGE_RAW_MAX   (2 * 1024 * 1024)
#define HIST_MAX       20
#define PARSE_GUARD    500000L
#define MAX_FORMS      6
#define MAX_SELECT_OPT 10

typedef enum {
    BLK_TEXT = 0,
    BLK_H1,
    BLK_H2,
    BLK_H3,
    BLK_LINK,
    BLK_LIST_ITEM,
    BLK_IMG,
    BLK_HR,
    BLK_FORM_TEXT,     /* gercek, duzenlenebilir metin kutusu */
    BLK_FORM_HIDDEN,   /* gorunmez, sadece gonderimde deger tasir */
    BLK_FORM_SELECT,   /* acilir kutu (dropdown) */
    BLK_FORM_SUBMIT    /* form gonderme butonu */
} block_type_t;

typedef struct {
    block_type_t type;
    char text[BLOCK_TEXT_MAX];  /* goruntu metni / dropdown icin "etiket1\netiket2..." */
    char href[HREF_MAX];        /* link URL'si / hidden-input degeri / dropdown icin "deger1\ndeger2..." */
    char name[NAME_MAX];        /* form alani adi (name="...") */
    int  form_id;                /* -1 = forma ait degil */
    lv_obj_t *widget;             /* render sirasinda olusturulan canli nesne (deger okumak icin) */
} block_t;

typedef struct {
    char action[HREF_MAX];
    int  used;
} form_t;

/* ---------------- Global durum ---------------- */
static block_t g_blocks[MAX_BLOCKS];
static int     g_block_count;

static form_t  g_forms[MAX_FORMS];
static int     g_form_count;

static asi_t   *g_basi;
static char    *g_raw;
static uint32_t g_raw_cap        = 2u * 1024u * 1024u;
static uint32_t g_dl_cap         = 4u * 1024u * 1024u;
static size_t   g_js_mem_limit   = 24u * 1024u * 1024u;
static int      g_css_pool_limit = 4 * 1024 * 1024;
static int      g_css_rules_max  = 65536;
static int      g_dn_max         = 8192;
static int      g_vstk_max       = 16384;
static uint32_t g_res_item_cap   = 4u * 1024u * 1024u;

static void browser_init_mem_limits(void) {
    uint64_t avail = 0;
    if (g_basi && g_basi->mem && g_basi->mem->info) {
        asi_meminfo_t mi;
        mi.total = mi.used = mi.free = mi.heap_total = mi.heap_used = 0;
        g_basi->mem->info(&mi);
        if (mi.heap_total > mi.heap_used) avail = mi.heap_total - mi.heap_used;
        else if (mi.free > 0) avail = mi.free;
        else if (mi.total > 0) avail = mi.total / 4u;
    }
    if (avail < 16u * 1024u * 1024u) avail = 64u * 1024u * 1024u;
    uint64_t budget = avail / 2u;
    if (budget < 12u * 1024u * 1024u) budget = 12u * 1024u * 1024u;

    uint64_t jslim = budget / 2u;
    if (jslim < 8u * 1024u * 1024u) jslim = 8u * 1024u * 1024u;
    if (jslim > 512u * 1024u * 1024u) jslim = 512u * 1024u * 1024u;
    g_js_mem_limit = (size_t)jslim;

    uint64_t dl = budget / 4u;
    if (dl < 4u * 1024u * 1024u) dl = 4u * 1024u * 1024u;
    if (dl > 128u * 1024u * 1024u) dl = 128u * 1024u * 1024u;
    g_dl_cap = (uint32_t)dl;

    uint64_t rc = budget / 8u;
    if (rc < 2u * 1024u * 1024u) rc = 2u * 1024u * 1024u;
    if (rc > 32u * 1024u * 1024u) rc = 32u * 1024u * 1024u;
    g_raw_cap = (uint32_t)rc;
    g_res_item_cap = (uint32_t)rc;

    uint64_t cp = budget / 4u;
    if (cp < 2u * 1024u * 1024u) cp = 2u * 1024u * 1024u;
    if (cp > 32u * 1024u * 1024u) cp = 32u * 1024u * 1024u;
    g_css_pool_limit = (int)cp;

    uint64_t cr = budget / 512u;
    if (cr < 16384u) cr = 16384u;
    if (cr > 262144u) cr = 262144u;
    g_css_rules_max = (int)cr;

    uint64_t dn = budget / 8192u;
    if (dn < 4096u) dn = 4096u;
    if (dn > 16384u) dn = 16384u;
    g_dn_max = (int)dn;

    uint64_t vs = budget / 2048u;
    if (vs < 8192u) vs = 8192u;
    if (vs > 32768u) vs = 32768u;
    g_vstk_max = (int)vs;
}

static int ensure_raw_buf(void) {
    if (g_raw) return 1;
    if (!g_basi || !g_basi->mem || !g_basi->mem->alloc) return 0;
    g_raw = (char *)g_basi->mem->alloc(g_raw_cap);
    if (g_raw) g_raw[0] = 0;
    return g_raw != 0;
}

static char g_cur_url[URL_MAX];
static char g_cur_host[160];
static char g_cur_scheme[8];

static char g_history[HIST_MAX][URL_MAX];
static int  g_hist_count;
/* Ileri yigini (0.6): geri alinan sayfa buraya itilir; yeni gezinme temizler. */
static char g_fwd[HIST_MAX][URL_MAX];
static int  g_fwd_count;

static lv_obj_t   *g_addr;
static lv_obj_t   *g_status;
static lv_obj_t   *g_content;
/* Sekme seridi (spec bolum 8): tek gercek sekme, canli sayfa basligini
 * gosterir; yesil nokta https gostergesidir (g_cur_scheme'den). */
static lv_obj_t   *g_tab_label;
static lv_obj_t   *g_secure_dot;
static lv_group_t *g_group;
static lv_timer_t *g_wheel_timer;

/* Ayristirici durumu */
static char          g_cur[BLOCK_TEXT_MAX];
static int            g_cur_len;
static int            g_last_space;
static block_type_t   g_cur_type;
static int             g_in_anchor;
static char           g_cur_href[HREF_MAX];
static int             g_cur_form;      /* parse sirasinda icinde bulunulan form id, yoksa -1 */
static int             g_cell_in_row;   /* tablo hucresi ayirici icin */

/* <select> ayristirma durumu */
static int  g_in_select;
static char g_sel_name[NAME_MAX];
static char g_sel_labels[BLOCK_TEXT_MAX];
static char g_sel_values[HREF_MAX];
static int  g_sel_opt_count;
static int  g_in_option;
static char g_opt_value_attr[64];
static char g_opt_label[64];
static int  g_opt_label_len;

/* Ileri bildirimler */
static void render_blocks(void);
static void set_status(const char *text, uint32_t color);
static void navigate(const char *url_in, int record_history);
static void navigate_post(const char *url_in, const char *post_body, int record_history);
static void submit_form(int form_id, const char *fallback_action);
static void render_page(const char *html);
static void collect_resources(const char *html, const char *scheme, const char *host);
static void render_pdf_viewer(const char *url, const uint8_t *data, uint32_t len);
static void cookies_persist_load(void);
static void cookies_persist_save(void);
static void ws_poll_all(void);
static void ws_close_all(void);
static void file_picker_open(int dom_idx);
static int  res_find(const char *url);
static int  res_add_owned(const char *url, uint8_t *owned_buf, uint32_t len, int is_css);
static void res_clear(void);
static void pq_cancel(void);
static int  pq_start_from_html(const char *html, const char *scheme, const char *host);
static void ghist_record(const char *url);
static void tab_set_active_title(const char *t);
static void tab_sync_active_meta(void);
static int  g_nav_in_flight;
static int  g_in_submit;
static void images_clear(void);
static void dl_log(const char *name, uint32_t size, const char *url);
static void js_reset(void);
static void js_tick(void);
static int  js_dispatch_dom_event(int dom_idx, const char *type);
static int  js_node_has_click_handler(int dom_idx);

/* ---------------- Kucuk yardimci fonksiyonlar (libc yok) ---------------- */
static int starts_with(const char *s, const char *p) {
    while (*p) { if (*s != *p) return 0; s++; p++; }
    return 1;
}

static char lower_c(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_unreserved(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static int str_eq_ci(const char *a, const char *b) {
    while (*a && *b) {
        if (lower_c(*a) != lower_c(*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static void bcopy_str(char *dst, const char *src, int dstsz) {
    int i = 0;
    if (!src) { dst[0] = 0; return; }
    while (src[i] && i < dstsz - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void bcat(char *dst, const char *src, int dstsz) {
    int i = 0;
    while (dst[i]) i++;
    int j = 0;
    while (src[j] && i < dstsz - 1) { dst[i++] = src[j++]; }
    dst[i] = 0;
}

static int b_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* Case-insensitive alt dizi arama (strstr yerine) */
static const char *find_substr_ci(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return 0;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && lower_c(*h) == lower_c(*n)) { h++; n++; }
        if (!*n) return hay;
    }
    return 0;
}

/* application/x-www-form-urlencoded uretir: bosluk -> '+', ayrilmis
 * karakterler -> %XX. Form gonderiminde dogru sorgu dizesi icin sarttir. */
static const char HEXD[] = "0123456789ABCDEF";
static void url_encode_append(char *dst, int dstsz, const char *src) {
    int i = b_len(dst);
    for (; *src && i < dstsz - 4; src++) {
        unsigned char c = (unsigned char)*src;
        if (is_unreserved((char)c)) {
            dst[i++] = (char)c;
        } else if (c == ' ') {
            dst[i++] = '+';
        } else {
            dst[i++] = '%';
            dst[i++] = HEXD[(c >> 4) & 0xF];
            dst[i++] = HEXD[c & 0xF];
        }
    }
    dst[i] = 0;
}

/* <etiket ...  veya </etiket ... icinden etiket adini kucuk harfle cikarir */
static void get_tag_name(const char *p, char *out, int outsz) {
    int n = 0;
    if (*p == '/') p++;
    if (*p && is_alpha(*p) && n < outsz - 1) { out[n++] = lower_c(*p); p++; }
    while (*p && (is_alpha(*p) || (*p >= '0' && *p <= '9') || *p == '-') && n < outsz - 1) {
        out[n++] = lower_c(*p); p++;
    }
    out[n] = 0;
}

/* tag_start '<' karakterini, tag_end kapanis '>' karakterini gosterir.
 * attr, kucuk harfli oznitelik adi (orn. "href"). Bulunursa 1 doner. */
static int find_attr(const char *tag_start, const char *tag_end,
                      const char *attr, char *out, int outsz) {
    const char *p = tag_start;
    int alen = 0;
    while (attr[alen]) alen++;
    while (p < tag_end) {
        int match = 1;
        for (int i = 0; i < alen; i++) {
            if (p + i >= tag_end || lower_c(p[i]) != attr[i]) { match = 0; break; }
        }
        /* onceki karakter alfanumerik olmamali (orn. "href" ararken
         * "xhref" gibi bir seye yanlislikla eslesmesin) */
        if (match && p > tag_start && (is_alpha(p[-1]) || p[-1] == '-')) match = 0;
        if (match) {
            const char *q = p + alen;
            while (q < tag_end && (*q == ' ' || *q == '\t')) q++;
            if (q < tag_end && *q == '=') {
                q++;
                while (q < tag_end && (*q == ' ' || *q == '\t')) q++;
                char quote = 0;
                if (q < tag_end && (*q == '"' || *q == '\'')) { quote = *q; q++; }
                int n = 0;
                while (q < tag_end && n < outsz - 1) {
                    if (quote) { if (*q == quote) break; }
                    else { if (*q == ' ' || *q == '>' || *q == '\t') break; }
                    out[n++] = *q++;
                }
                out[n] = 0;
                return 1;
            }
            /* "checked" gibi degersiz oznitelik: varlik olarak isaretle */
            out[0] = 0;
            return 1;
        }
        p++;
    }
    out[0] = 0;
    return 0;
}

/* ---------------- URL yardimcilari ---------------- */
static void parse_url(const char *url, char *scheme_out, int scheme_sz,
                       char *host_out, int host_sz) {
    const char *p = url;
    int i = 0;
    while (*p && *p != ':' && i < scheme_sz - 1) { scheme_out[i++] = *p; p++; }
    scheme_out[i] = 0;
    if (starts_with(p, "://")) p += 3;
    i = 0;
    while (*p && *p != '/' && i < host_sz - 1) { host_out[i++] = *p; p++; }
    host_out[i] = 0;
}

static void resolve_url(const char *scheme, const char *host,
                         const char *href, char *out, int outsz) {
    out[0] = 0;
    if (!href || !href[0] || href[0] == '#') return;
    if (starts_with(href, "http://") || starts_with(href, "https://")) {
        bcopy_str(out, href, outsz);
        return;
    }
    if (href[0] == '/' && href[1] == '/') {
        bcopy_str(out, scheme, outsz); bcat(out, ":", outsz); bcat(out, href, outsz);
        return;
    }
    if (href[0] == '/') {
        bcopy_str(out, scheme, outsz); bcat(out, "://", outsz);
        bcat(out, host, outsz); bcat(out, href, outsz);
        return;
    }
    bcopy_str(out, scheme, outsz); bcat(out, "://", outsz);
    bcat(out, host, outsz); bcat(out, "/", outsz); bcat(out, href, outsz);
}

/* ---------------- Blok listesi ---------------- */
static block_t *push_block_full(block_type_t type, const char *text, const char *href,
                                 const char *name, int form_id) {
    if (g_block_count >= MAX_BLOCKS) return 0;
    block_t *b = &g_blocks[g_block_count];
    b->type = type;
    bcopy_str(b->text, text, BLOCK_TEXT_MAX);
    bcopy_str(b->href, href ? href : "", HREF_MAX);
    bcopy_str(b->name, name ? name : "", NAME_MAX);
    b->form_id = form_id;
    b->widget = 0;
    g_block_count++;
    return b;
}

static void push_block(block_type_t type, const char *text, const char *href) {
    push_block_full(type, text, href, "", -1);
}

static void append_char(char c) {
    if (c == ' ') {
        if (g_last_space) return;
        g_last_space = 1;
    } else {
        g_last_space = 0;
    }
    if (g_cur_len < BLOCK_TEXT_MAX - 1) g_cur[g_cur_len++] = c;
}

/* Unicode kod noktasini UTF-8 olarak akisa ekler (sayisal varliklar icin). */
static void append_utf8(uint32_t cp) {
    if (cp == 0) return;
    if (cp < 0x80) { append_char((char)cp); return; }
    if (cp < 0x800) {
        append_char((char)(0xC0 | (cp >> 6)));
        append_char((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        append_char((char)(0xE0 | (cp >> 12)));
        append_char((char)(0x80 | ((cp >> 6) & 0x3F)));
        append_char((char)(0x80 | (cp & 0x3F)));
    } else {
        append_char((char)(0xF0 | (cp >> 18)));
        append_char((char)(0x80 | ((cp >> 12) & 0x3F)));
        append_char((char)(0x80 | ((cp >> 6) & 0x3F)));
        append_char((char)(0x80 | (cp & 0x3F)));
    }
}

/* Bilinen HTML varliklarini (entity) coker; ortak Google/GitHub sayfalarinda
 * sikca gecen birkac ek varlik da eklendi. */
static int try_entity(const char *p, int *consumed) {
    struct { const char *name; uint32_t cp; } table[] = {
        {"amp;", '&'}, {"lt;", '<'}, {"gt;", '>'}, {"quot;", '"'},
        {"apos;", '\''}, {"nbsp;", ' '}, {"mdash;", 0x2014}, {"ndash;", 0x2013},
        {"hellip;", 0x2026}, {"rsquo;", 0x2019}, {"lsquo;", 0x2018},
        {"ldquo;", 0x201C}, {"rdquo;", 0x201D}, {"copy;", 0x00A9},
        {"reg;", 0x00AE}, {"trade;", 0x2122},
    };
    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        int n = b_len(table[i].name);
        int ok = 1;
        for (int j = 0; j < n; j++) if (p[j] != table[i].name[j]) { ok = 0; break; }
        if (ok) { append_utf8(table[i].cp); *consumed = n; return 1; }
    }
    return 0;
}

/* <title>...</title> metnini cikarir. */
static int extract_title(const char *html, char *out, int cap) {
    const char *t = find_substr_ci(html, "<title");
    if (!t) return 0;
    const char *gt = t;
    while (*gt && *gt != '>') gt++;
    if (*gt != '>') return 0;
    gt++;
    const char *end = find_substr_ci(gt, "</title>");
    if (!end || end <= gt) return 0;
    int n = 0;
    for (const char *p = gt; p < end && n < cap - 1; p++) {
        char c = *p;
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        out[n++] = c;
    }
    out[n] = 0;
    while (n > 0 && out[n - 1] == ' ') out[--n] = 0;
    return n > 0;
}

static void flush_block(void) {
    if (g_in_anchor || g_in_option) return; /* metin sadece kapanista yayinlanir */
    g_cur[g_cur_len] = 0;
    while (g_cur_len > 0 && g_cur[g_cur_len - 1] == ' ') g_cur_len--;
    g_cur[g_cur_len] = 0;
    if (g_cur_len > 0) push_block(g_cur_type, g_cur, 0);
    g_cur_len = 0;
    g_last_space = 1;
}

/* ---------------- HTML -> blok ayristirici ---------------- */
static void html_parse(const char *html) {
    g_block_count = 0;
    g_form_count = 0;
    g_cur_len = 0; g_last_space = 1;
    g_cur_type = BLK_TEXT;
    g_in_anchor = 0; g_cur_href[0] = 0;
    g_cur_form = -1;
    g_cell_in_row = 0;
    g_in_select = 0; g_in_option = 0; g_sel_opt_count = 0;

    /* Sayfa basligini en uste koy. */
    {
        char t[BLOCK_TEXT_MAX];
        if (extract_title(html, t, sizeof(t))) push_block(BLK_H1, t, 0);
    }

    const char *p = html;
    long guard = 0;

    while (*p) {
        if (++guard > PARSE_GUARD) break; /* bozuk/asiri buyuk sayfaya karsi guvenlik */

        if (*p == '<') {
            const char *tag_start = p;
            const char *q = p + 1;
            while (*q && *q != '>') q++;
            const char *tag_end = q;
            int closing = (tag_start[1] == '/');
            char name[16];
            get_tag_name(tag_start + 1, name, sizeof(name));

            /* Gorunmemesi gereken icerik: script/style/title/head govdesi.
             * NOT: <noscript> BILINCLI olarak buraya eklenmedi - JS calismadigi
             * icin sitelerin "JS kapaliyken goster" icin koydugu yedek icerik
             * tam da bize lazim olan sey; normal metin gibi islenmesine izin
             * verilir. svg de ozel islem gerektirmez: ic ice etiketlerin metin
             * tasimayan ozniteligi zaten atlanir. */
            if (!closing && (str_eq(name, "script") || str_eq(name, "style") ||
                              str_eq(name, "title")  || str_eq(name, "head") ||
                              str_eq(name, "svg"))) {
                char close_tag[16] = "</";
                bcat(close_tag, name, sizeof(close_tag));
                const char *from = (*tag_end == '>') ? tag_end + 1 : tag_end;
                const char *found = find_substr_ci(from, close_tag);
                if (found) {
                    const char *r = found;
                    while (*r && *r != '>') r++;
                    p = (*r == '>') ? r + 1 : r;
                } else {
                    p = from;
                    while (*p) p++;
                }
                continue;
            }

            if (!closing) {
                if (str_eq(name, "h1")) {
                    flush_block(); g_cur_type = BLK_H1; g_cur_len = 0; g_last_space = 1;
                } else if (str_eq(name, "h2")) {
                    flush_block(); g_cur_type = BLK_H2; g_cur_len = 0; g_last_space = 1;
                } else if (str_eq(name, "h3") || str_eq(name, "h4") ||
                           str_eq(name, "h5") || str_eq(name, "h6")) {
                    flush_block(); g_cur_type = BLK_H3; g_cur_len = 0; g_last_space = 1;
                } else if (str_eq(name, "li")) {
                    flush_block(); g_cur_type = BLK_LIST_ITEM; g_cur_len = 0; g_last_space = 1;
                } else if (str_eq(name, "a")) {
                    flush_block();
                    find_attr(tag_start, tag_end, "href", g_cur_href, sizeof(g_cur_href));
                    g_in_anchor = 1; g_cur_len = 0; g_last_space = 1;
                } else if (str_eq(name, "img")) {
                    flush_block();
                    char alt[BLOCK_TEXT_MAX];
                    if (!find_attr(tag_start, tag_end, "alt", alt, sizeof(alt)) || alt[0] == 0)
                        bcopy_str(alt, "resim", sizeof(alt));
                    push_block(BLK_IMG, alt, 0);
                } else if (str_eq(name, "form")) {
                    flush_block();
                    if (g_form_count < MAX_FORMS) {
                        char action[HREF_MAX];
                        find_attr(tag_start, tag_end, "action", action, sizeof(action));
                        bcopy_str(g_forms[g_form_count].action, action, HREF_MAX);
                        g_forms[g_form_count].used = 1;
                        g_cur_form = g_form_count;
                        g_form_count++;
                    } else {
                        g_cur_form = -1;
                    }
                } else if (str_eq(name, "select")) {
                    flush_block();
                    g_in_select = 1; g_sel_opt_count = 0;
                    g_sel_labels[0] = 0; g_sel_values[0] = 0;
                    find_attr(tag_start, tag_end, "name", g_sel_name, sizeof(g_sel_name));
                } else if (str_eq(name, "option") && g_in_select) {
                    g_in_option = 1; g_opt_label_len = 0; g_opt_label[0] = 0;
                    find_attr(tag_start, tag_end, "value", g_opt_value_attr, sizeof(g_opt_value_attr));
                } else if (str_eq(name, "input")) {
                    flush_block();
                    char type[24], nm[NAME_MAX], val[BLOCK_TEXT_MAX], ph[BLOCK_TEXT_MAX];
                    if (!find_attr(tag_start, tag_end, "type", type, sizeof(type)) || !type[0])
                        bcopy_str(type, "text", sizeof(type));
                    find_attr(tag_start, tag_end, "name", nm, sizeof(nm));
                    find_attr(tag_start, tag_end, "value", val, sizeof(val));
                    if (!find_attr(tag_start, tag_end, "placeholder", ph, sizeof(ph)) || !ph[0])
                        ph[0] = 0;

                    if (str_eq_ci(type, "hidden")) {
                        push_block_full(BLK_FORM_HIDDEN, "", val, nm, g_cur_form);
                    } else if (str_eq_ci(type, "submit") || str_eq_ci(type, "button") ||
                               str_eq_ci(type, "image")) {
                        char label[BLOCK_TEXT_MAX];
                        bcopy_str(label, val[0] ? val : "Gonder", sizeof(label));
                        push_block_full(BLK_FORM_SUBMIT, label, "", nm, g_cur_form);
                    } else if (str_eq_ci(type, "checkbox") || str_eq_ci(type, "radio")) {
                        /* basit tutuluyor: gorsel kutu, form gonderiminde
                         * (bu surumde) dahil edilmez. */
                        push_block(BLK_TEXT, "[ ] (secim kutusu - bu surumde desteklenmiyor)", 0);
                    } else {
                        /* text, search, email, password, url, number, tel, vb. */
                        block_t *b = push_block_full(BLK_FORM_TEXT, ph, val, nm, g_cur_form);
                        (void)b;
                    }
                } else if (str_eq(name, "textarea")) {
                    flush_block();
                    char nm[NAME_MAX];
                    find_attr(tag_start, tag_end, "name", nm, sizeof(nm));
                    push_block_full(BLK_FORM_TEXT, "", "", nm, g_cur_form);
                } else if (str_eq(name, "hr")) {
                    flush_block();
                    push_block(BLK_HR, "", 0);
                } else if (str_eq(name, "td") || str_eq(name, "th")) {
                    if (g_cell_in_row && g_cur_len < BLOCK_TEXT_MAX - 4) {
                        append_char(' '); append_char('|'); append_char(' ');
                    }
                    g_cell_in_row = 1;
                } else if (str_eq(name, "tr")) {
                    flush_block(); g_cell_in_row = 0;
                } else if (str_eq(name, "br") || str_eq(name, "p")  || str_eq(name, "div") ||
                           str_eq(name, "table") || str_eq(name, "ul") || str_eq(name, "ol")) {
                    flush_block();
                }
                /* Diger etiketler (span, b, strong, vb.) yoksayilir;
                 * icindeki metin akisina dokunulmaz. */
            } else {
                if (str_eq(name, "a")) {
                    g_cur[g_cur_len] = 0;
                    while (g_cur_len > 0 && g_cur[g_cur_len - 1] == ' ') g_cur_len--;
                    g_cur[g_cur_len] = 0;
                    if (g_cur_len > 0) push_block(BLK_LINK, g_cur, g_cur_href);
                    g_cur_len = 0; g_in_anchor = 0; g_last_space = 1;
                } else if (str_eq(name, "option") && g_in_option) {
                    g_opt_label[g_opt_label_len] = 0;
                    while (g_opt_label_len > 0 && g_opt_label[g_opt_label_len - 1] == ' ')
                        g_opt_label[--g_opt_label_len] = 0;
                    const char *value = g_opt_value_attr[0] ? g_opt_value_attr : g_opt_label;
                    if (g_sel_opt_count < MAX_SELECT_OPT) {
                        if (g_sel_opt_count > 0) {
                            bcat(g_sel_labels, "\n", sizeof(g_sel_labels));
                            bcat(g_sel_values, "\n", sizeof(g_sel_values));
                        }
                        bcat(g_sel_labels, g_opt_label[0] ? g_opt_label : value, sizeof(g_sel_labels));
                        bcat(g_sel_values, value, sizeof(g_sel_values));
                        g_sel_opt_count++;
                    }
                    g_in_option = 0;
                } else if (str_eq(name, "select") && g_in_select) {
                    if (g_sel_opt_count > 0)
                        push_block_full(BLK_FORM_SELECT, g_sel_labels, g_sel_values,
                                        g_sel_name, g_cur_form);
                    g_in_select = 0;
                } else if (str_eq(name, "form")) {
                    flush_block(); g_cur_form = -1;
                } else if (str_eq(name, "h1") || str_eq(name, "h2") || str_eq(name, "h3") ||
                           str_eq(name, "h4") || str_eq(name, "h5") || str_eq(name, "h6")) {
                    flush_block(); g_cur_type = BLK_TEXT;
                } else if (str_eq(name, "li")) {
                    flush_block(); g_cur_type = BLK_TEXT;
                } else if (str_eq(name, "p") || str_eq(name, "div") || str_eq(name, "tr")) {
                    flush_block();
                    if (str_eq(name, "tr")) g_cell_in_row = 0;
                }
            }

            if (*tag_end == '>') { p = tag_end + 1; continue; }
            break; /* '>' bulunamadi: bozuk HTML, ayristirmayi bitir */
        }

        if (*p == '&') {
            int consumed = 0;
            if (try_entity(p + 1, &consumed)) { p += 1 + consumed; continue; }
            if (p[1] == '#') {                       /* sayisal varlik: &#305; / &#x131; */
                const char *q = p + 2;
                uint32_t cp = 0;
                int hex = 0;
                if (*q == 'x' || *q == 'X') { hex = 1; q++; }
                const char *q0 = q;
                while (*q) {
                    int v = -1;
                    if (*q >= '0' && *q <= '9') v = *q - '0';
                    else if (hex && *q >= 'a' && *q <= 'f') v = *q - 'a' + 10;
                    else if (hex && *q >= 'A' && *q <= 'F') v = *q - 'A' + 10;
                    else break;
                    cp = cp * (hex ? 16u : 10u) + (uint32_t)v;
                    q++;
                }
                if (q > q0 && *q == ';') { append_utf8(cp); p = q + 1; continue; }
            }
        }

        if (g_in_option) {
            /* option etiketi icindeki metni ayri bir tampona topla */
            char c = *p;
            if (c == '\t' || c == '\n' || c == '\r') c = ' ';
            if (c == ' ') {
                if (g_opt_label_len == 0 || g_opt_label[g_opt_label_len - 1] == ' ') { p++; continue; }
            }
            if (g_opt_label_len < (int)sizeof(g_opt_label) - 1) g_opt_label[g_opt_label_len++] = c;
            p++;
            continue;
        }

        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            append_char(' ');
            p++;
            continue;
        }

        append_char(*p);
        p++;
    }

    flush_block();
    if (g_block_count == 0) push_block(BLK_TEXT, "(Bos sayfa)", 0);
}

/* ---------------- Gecmis ---------------- */
static void fwd_clear(void) { g_fwd_count = 0; }

static void push_history(const char *url) {
    /* Yeni bir gezinme ileri yiginini gecersiz kilar (tarayici kurali). */
    fwd_clear();
    if (g_hist_count < HIST_MAX) {
        bcopy_str(g_history[g_hist_count], url, URL_MAX);
        g_hist_count++;
        return;
    }
    for (int i = 1; i < HIST_MAX; i++) bcopy_str(g_history[i - 1], g_history[i], URL_MAX);
    bcopy_str(g_history[HIST_MAX - 1], url, URL_MAX);
}

/* ---------------- Durum satiri ---------------- */
static void set_status(const char *text, uint32_t color) {
    if (!g_status) return;
    lv_label_set_text(g_status, text);
    lv_obj_set_style_text_color(g_status, lv_color_hex(color), 0);
}

/* Sekme seridi basligi: aktif sekmenin basligini gunceller ve cubugu tazeler. */
static void set_tab_title(const char *t) {
    tab_set_active_title(t);
    if (g_tab_label)
        lv_label_set_text(g_tab_label, (t && t[0]) ? t : "Yeni sekme");
}

/* ---------------- Gezinme (fetch + parse + render) ---------------- */

/* Arka plan cekme: ag isi fiber'de; ayristirma + cizim (UI) done'da. */
static char g_nav_url[URL_MAX];
static char g_nav_final[URL_MAX];
static char g_nav_pending[URL_MAX];
static int  g_nav_is_https = 0;
static int  g_nav_ok = 0;
static int  g_nav_have_pending = 0;
static int  g_search_fb = 0;
static int  g_google_tries = 0;

static int search_query_of(const char *url, char *out, int cap) {
    const char *q = 0;
    const char *p = url;
    while (*p) {
        int bound = (p == url || p[-1] == '?' || p[-1] == '&');
        if (bound && p[0] == 'q' && p[1] == '=') { q = p + 2; break; }
        if (bound && (p[0] == 'q' || p[0] == 'Q') && (p[1] == 'u' || p[1] == 'U') &&
            (p[2] == 'e' || p[2] == 'E') && (p[3] == 'r' || p[3] == 'R') &&
            (p[4] == 'y' || p[4] == 'Y') && p[5] == '=') { q = p + 6; break; }
        p++;
    }
    if (!q) return 0;
    int o = 0;
    while (*q && *q != '&' && *q != '#' && o < cap - 1) {
        if (*q == '+') { out[o++] = ' '; q++; }
        else if (*q == '%' && q[1] && q[2]) {
            int hi = -1, lo = -1;
            if (q[1] >= '0' && q[1] <= '9') hi = q[1] - '0';
            else if (q[1] >= 'a' && q[1] <= 'f') hi = q[1] - 'a' + 10;
            else if (q[1] >= 'A' && q[1] <= 'F') hi = q[1] - 'A' + 10;
            if (q[2] >= '0' && q[2] <= '9') lo = q[2] - '0';
            else if (q[2] >= 'a' && q[2] <= 'f') lo = q[2] - 'a' + 10;
            else if (q[2] >= 'A' && q[2] <= 'F') lo = q[2] - 'A' + 10;
            if (hi >= 0 && lo >= 0) { out[o++] = (char)(hi * 16 + lo); q += 3; }
            else { out[o++] = *q++; }
        }
        else out[o++] = *q++;
    }
    out[o] = 0;
    return o > 0;
}

static void search_url_encode(const char *s, char *out, int cap) {
    int o = 0;
    for (int i = 0; s[i] && o < cap - 1; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else if (c == ' ') {
            out[o++] = '+';
        } else if (o < cap - 4) {
            out[o++] = '%';
            out[o++] = "0123456789ABCDEF"[c >> 4];
            out[o++] = "0123456789ABCDEF"[c & 15];
        }
    }
    out[o] = 0;
}

static void trigger_search_fallback(void) {
    char q[200];
    if ((search_query_of(g_nav_url, q, sizeof(q)) ||
         search_query_of(g_nav_final, q, sizeof(q))) && q[0]) {
        g_search_fb = 1;
        char nurl[URL_MAX];
        bcopy_str(nurl, "https://html.duckduckgo.com/html/?q=", sizeof(nurl));
        int base = b_len(nurl);
        search_url_encode(q, nurl + base, URL_MAX - base);
        bcopy_str(g_nav_pending, nurl, URL_MAX);
        g_nav_have_pending = 1;
    }
}

/* --- HTML Disi Dosya Algilama & Otomatik Indirme (C:/indirilenler) --- */
static int       g_nav_is_dl;
static int       g_nav_force_dl;
static uint8_t  *g_nav_dl_buf;
static uint32_t  g_nav_dl_len;
static char      g_nav_dl_ext[12];
#define NAV_DL_CAP (4u * 1024u * 1024u)

static int url_ext_is(const char *url, const char *ext) {
    if (!url || !ext) return 0;
    const char *p = url;
    const char *path_end = p;
    while (*path_end && *path_end != '?' && *path_end != '#') path_end++;
    int el = b_len(ext);
    if ((int)(path_end - p) < el + 1) return 0;
    const char *s = path_end - el;
    if (s[-1] != '.') return 0;
    for (int i = 0; i < el; i++)
        if (lower_c(s[i]) != lower_c(ext[i])) return 0;
    return 1;
}

static int url_is_downloadable(const char *url) {
    static const char *exts[] = {
        "wlf", "png", "jpg", "jpeg", "gif", "webp", "bmp", "ico",
        "pdf", "zip", "tar", "gz", "tgz", "bz2", "xz", "7z", "rar",
        "iso", "img", "bin", "elf", "exe", "dmg", "deb", "apk", "wasm",
        "mp3", "mp4", "wav", "ogg", "webm", "mkv", "avi", "flac",
        "ttf", "otf", "woff", "woff2", "csv", "doc", "docx", "xls", "xlsx", "ppt", "pptx",
        0
    };
    for (int i = 0; exts[i]; i++)
        if (url_ext_is(url, exts[i])) return 1;
    return 0;
}

static int sniff_non_html(const uint8_t *buf, uint32_t len, const char *url, char ext_out[12]) {
    ext_out[0] = 0;
    if (url_is_downloadable(url)) return 1;
    if (!buf || len == 0) return 0;
    /* Sihirli bayt (magic header) kontrolu */
    if (len >= 8 && buf[0]==0x89 && buf[1]=='P' && buf[2]=='N' && buf[3]=='G' &&
        buf[4]==0x0D && buf[5]==0x0A && buf[6]==0x1A && buf[7]==0x0A) { bcopy_str(ext_out, ".png", 12); return 1; }
    if (len >= 3 && buf[0]==0xFF && buf[1]==0xD8 && buf[2]==0xFF) { bcopy_str(ext_out, ".jpg", 12); return 1; }
    if (len >= 6 && buf[0]=='G' && buf[1]=='I' && buf[2]=='F' && buf[3]=='8') { bcopy_str(ext_out, ".gif", 12); return 1; }
    if (len >= 12 && buf[0]=='R' && buf[1]=='I' && buf[2]=='F' && buf[3]=='F' &&
        buf[8]=='W' && buf[9]=='E' && buf[10]=='B' && buf[11]=='P') { bcopy_str(ext_out, ".webp", 12); return 1; }
    if (len >= 2 && buf[0]=='B' && buf[1]=='M') { bcopy_str(ext_out, ".bmp", 12); return 1; }
    if (len >= 5 && buf[0]=='%' && buf[1]=='P' && buf[2]=='D' && buf[3]=='F' && buf[4]=='-') { bcopy_str(ext_out, ".pdf", 12); return 1; }
    if (len >= 4 && buf[0]=='P' && buf[1]=='K' && buf[2]==0x03 && buf[3]==0x04) { bcopy_str(ext_out, ".zip", 12); return 1; }
    if (len >= 2 && buf[0]==0x1F && buf[1]==0x8B) { bcopy_str(ext_out, ".gz", 12); return 1; }
    if (len >= 4 && buf[0]=='W' && buf[1]=='L' && buf[2]=='F') { bcopy_str(ext_out, ".wlf", 12); return 1; }
    if (len >= 4 && buf[0]=='W' && buf[1]=='O' && buf[2]=='L' && buf[3]=='F') { bcopy_str(ext_out, ".wlf", 12); return 1; }
    if (len >= 4 && buf[0]==0x7F && buf[1]=='E' && buf[2]=='L' && buf[3]=='F') { bcopy_str(ext_out, ".wlf", 12); return 1; }
    if (len >= 4 && buf[0]==0x00 && buf[1]=='a' && buf[2]=='s' && buf[3]=='m') { bcopy_str(ext_out, ".wasm", 12); return 1; }
    if (len >= 8 && buf[4]=='f' && buf[5]=='t' && buf[6]=='y' && buf[7]=='p') { bcopy_str(ext_out, ".mp4", 12); return 1; }
    if (len >= 4 && buf[0]=='O' && buf[1]=='g' && buf[2]=='g' && buf[3]=='S') { bcopy_str(ext_out, ".ogg", 12); return 1; }
    if (len >= 3 && buf[0]=='I' && buf[1]=='D' && buf[2]=='3') { bcopy_str(ext_out, ".mp3", 12); return 1; }
    /* Ikili veri (NUL veya kontrol bayti yogunlugu) kontrolu */
    uint32_t sample = len < 512u ? len : 512u;
    uint32_t ctrl = 0;
    for (uint32_t i = 0; i < sample; i++) {
        uint8_t c = buf[i];
        if (c == 0) { bcopy_str(ext_out, ".bin", 12); return 1; }
        if (c < 0x09 || (c > 0x0D && c < 0x20)) ctrl++;
    }
    if (sample >= 16 && ctrl * 20u > sample) { bcopy_str(ext_out, ".bin", 12); return 1; }
    return 0;
}

static void build_download_filename(const char *url, const char *sniff_ext, char *name, int cap) {
    const char *base = url ? url : "";
    for (const char *p = base; *p && *p != '?' && *p != '#'; p++)
        if (*p == '/') base = p + 1;
    int n = 0, has_dot = 0;
    for (int i = 0; base[i] && base[i] != '?' && base[i] != '#' && n < cap - 12; i++) {
        char c = base[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        if (c == '.') has_dot = 1;
        name[n++] = ok ? c : '_';
    }
    if (n == 0 || name[0] == '_' || name[0] == '.') {
        const char *d = (sniff_ext && sniff_ext[0]) ? "indir" : "indir.bin";
        n = 0; while (d[n] && n < cap - 12) { name[n] = d[n]; n++; }
        has_dot = (sniff_ext && sniff_ext[0]) ? 0 : 1;
    }
    name[n] = 0;
    if (!has_dot && sniff_ext && sniff_ext[0]) bcat(name, sniff_ext, cap);
}

static void save_non_html_download(const char *url, const uint8_t *data, uint32_t len, const char *sniff_ext) {
    char name[80];
    build_download_filename(url, sniff_ext, name, sizeof(name));
    asi_fs_t *fs = g_basi ? g_basi->fs : 0;
    if (!fs || !fs->create_in || !fs->write) {
        set_status("Dosya sistemi yok - indirilemedi", ADL_DANGER);
        return;
    }
    if (fs->mkdir) fs->mkdir("C:/indirilenler");
    void *h = fs->create_in("C:/indirilenler", name, 0);
    if (!h) {
        set_status("Indirme dosyasi olusturulamadi", ADL_DANGER);
        return;
    }
    fs->write(h, data, len);
    dl_log(name, len, url);
    arctian_notify("Indirme tamam", name);
    char msg[160];
    bcopy_str(msg, "Indirildi: C:/indirilenler/", sizeof(msg));
    bcat(msg, name, sizeof(msg));
    set_status(msg, 0x10B981);
    if (!g_cur_url[0]) {
        g_block_count = 0;
        push_block(BLK_H1, "Dosya Indirildi", 0);
        push_block(BLK_TEXT, msg, 0);
        push_block(BLK_TEXT, url, 0);
        render_blocks();
    }
}

static char g_nav_post_body[2048];
static int  g_nav_is_post;

static void browser_nav_work(void *u) {
    (void)u;
    g_nav_is_dl = 0;
    g_nav_dl_len = 0;
    g_nav_dl_ext[0] = 0;
    int is_post = g_nav_is_post;
    g_nav_is_post = 0;
    int want_dl = !is_post && (g_nav_force_dl || url_is_downloadable(g_nav_url));
    g_nav_force_dl = 0;

    if (want_dl && g_basi && g_basi->net && g_basi->net->download && g_basi->mem && g_basi->mem->alloc) {
        if (!g_nav_dl_buf) g_nav_dl_buf = (uint8_t *)g_basi->mem->alloc(g_dl_cap);
        if (g_nav_dl_buf) {
            uint32_t dlen = 0;
            int rc = g_basi->net->download(g_nav_url, g_nav_dl_buf, g_dl_cap, &dlen);
            g_nav_ok = (rc == 0) ? 1 : 0;
            g_nav_dl_len = dlen;
            g_nav_is_dl = 1;
            const char *fin = (g_basi->net->last_url) ? g_basi->net->last_url() : 0;
            bcopy_str(g_nav_final, (fin && fin[0]) ? fin : g_nav_url, URL_MAX);
            sniff_non_html(g_nav_dl_buf, dlen, g_nav_final, g_nav_dl_ext);
            return;
        }
    }

    uint32_t len = 0;
    const char *body = 0;
    if (is_post && g_basi && g_basi->net && g_basi->net->http_request) {
        int st = 0;
        body = g_basi->net->http_request("POST", g_nav_url,
                                         "application/x-www-form-urlencoded", 0,
                                         g_nav_post_body, (uint32_t)b_len(g_nav_post_body),
                                         &st, &len);
    } else {
        body = g_nav_is_https ? g_basi->net->https_get(g_nav_url, &len)
                              : g_basi->net->http_get(g_nav_url, &len);
    }
    g_nav_ok = body ? 1 : 0;
    const char *fin = (g_basi->net && g_basi->net->last_url) ? g_basi->net->last_url() : 0;
    if (fin && fin[0]) bcopy_str(g_nav_final, fin, URL_MAX);
    else bcopy_str(g_nav_final, g_nav_url, URL_MAX);

    if (body && len > 0 && sniff_non_html((const uint8_t *)body, len, g_nav_final, g_nav_dl_ext)) {
        if (!g_nav_dl_buf && g_basi && g_basi->mem && g_basi->mem->alloc)
            g_nav_dl_buf = (uint8_t *)g_basi->mem->alloc(g_dl_cap);
        if (g_nav_dl_buf) {
            uint32_t cp = len < g_dl_cap ? len : g_dl_cap;
            for (uint32_t i = 0; i < cp; i++) g_nav_dl_buf[i] = (uint8_t)body[i];
            g_nav_dl_len = cp;
            g_nav_is_dl = 1;
            return;
        }
    }

    if (!ensure_raw_buf()) { g_nav_ok = 0; return; }
    if (body) {
        if (len > 0) {
            uint32_t cp = len < (g_raw_cap - 1u) ? len : (g_raw_cap - 1u);
            for (uint32_t i = 0; i < cp; i++) g_raw[i] = body[i];
            g_raw[cp] = 0;
        } else {
            bcopy_str(g_raw, body, (int)g_raw_cap);
        }
    } else g_raw[0] = 0;
    if (g_nav_ok && !g_search_fb) {
        if (find_substr_ci(g_nav_final, "/sorry/") != 0)
            trigger_search_fallback();
    }
}

static void browser_nav_done(int status, void *u) {
    (void)status; (void)u;
    g_nav_in_flight = 0;

    if (!g_nav_ok) {
        g_block_count = 0;
        if (g_nav_is_https) {
            push_block(BLK_TEXT, "HTTPS getirilemedi (ag/DNS/TLS/sertifika).", 0);
        } else {
            push_block(BLK_TEXT, "Sayfa getirilemedi.", 0);
            push_block(BLK_TEXT, g_nav_url, 0);
            push_block(BLK_TEXT, "(Sunucu yok, DNS veya baglanti sorunu.)", 0);
        }
        render_blocks();
        set_status("Hata", ADL_DANGER);
        set_tab_title(0);
        if (g_secure_dot)
            lv_obj_set_style_text_color(g_secure_dot, lv_color_hex(0x6B717D), 0);
    } else if (g_nav_is_dl) {
        if (g_nav_dl_buf && g_nav_dl_len >= 8 &&
            g_nav_dl_buf[0] == '%' && g_nav_dl_buf[1] == 'P' &&
            g_nav_dl_buf[2] == 'D' && g_nav_dl_buf[3] == 'F' && g_nav_dl_buf[4] == '-') {
            render_pdf_viewer(g_nav_final, g_nav_dl_buf, g_nav_dl_len);
        } else {
            save_non_html_download(g_nav_final, g_nav_dl_buf, g_nav_dl_len, g_nav_dl_ext);
        }
    } else {
        bcopy_str(g_cur_url, g_nav_final, URL_MAX);
        parse_url(g_nav_final, g_cur_scheme, sizeof(g_cur_scheme), g_cur_host, sizeof(g_cur_host));
        if (g_addr) lv_textarea_set_text(g_addr, g_nav_final);
        ghist_record(g_nav_final);
        cookies_persist_save();
        res_clear();
        /* Senkron/test kosumunda kaynaklari tek seferde indir; gercek asenkron
         * agda HTML'i ANINDA ciz ve CSS/JS/gorselleri asamali olarak yansit! */
        int is_async = (!g_in_submit && g_basi && g_basi->net && g_basi->net->submit) ? 1 : 0;
        if (!is_async) {
            collect_resources(g_raw, g_cur_scheme, g_cur_host);
        }
        render_page(g_raw);
        /* Sekme basligi + https gostergesi (spec bolum 8: yesil nokta). */
        {
            char t[120];
            if (extract_title(g_raw, t, sizeof(t))) set_tab_title(t);
            else set_tab_title(g_cur_host);
        }
        tab_sync_active_meta();
        if (g_secure_dot)
            lv_obj_set_style_text_color(g_secure_dot,
                lv_color_hex(str_eq(g_cur_scheme, "https") ? 0x4ADE80 : 0x6B717D), 0);

        if (is_async && !g_nav_have_pending) {
            if (pq_start_from_html(g_raw, g_cur_scheme, g_cur_host)) {
                return;
            }
        }
        set_status(g_nav_final, ADL_TEXT_DIM);
    }

    if (g_nav_have_pending) {
        char next[URL_MAX];
        bcopy_str(next, g_nav_pending, sizeof(next));
        g_nav_have_pending = 0;
        navigate(next, 1);
    }
}

static void navigate(const char *url_in, int record_history) {
    pq_cancel();
    if (!url_in || !url_in[0]) {
        g_block_count = 0;
        push_block(BLK_TEXT, "Adres girin.", 0);
        render_blocks();
        set_status("Hazir", ADL_TEXT_DIM);
        return;
    }

    if (g_basi && g_basi->net && g_basi->net->link_type &&
        g_basi->net->link_type() == ASI_LINK_NONE) {
        g_block_count = 0;
        push_block(BLK_TEXT, "Ag baglantisi yok.", 0);
        render_blocks();
        set_status("Baglanti yok", ADL_DANGER);
        return;
    }

    char full[URL_MAX];
    const char *u = url_in;
    if (!starts_with(url_in, "http://") && !starts_with(url_in, "https://")) {
        bcopy_str(full, "https://", sizeof(full));
        bcat(full, url_in, sizeof(full));
        u = full;
    }

    set_status("Yukleniyor...", ADL_WARN);

    if (!find_substr_ci(u, "duckduckgo.com")) g_search_fb = 0;
    if (!find_substr_ci(u, "google.")) g_google_tries = 0;

    if (record_history && g_cur_url[0] && !str_eq(g_cur_url, u))
        push_history(g_cur_url);

    bcopy_str(g_nav_url, u, URL_MAX);
    g_nav_is_https = starts_with(u, "https://");

    if (g_basi && g_basi->net && g_basi->net->submit) {
        if (g_basi->net->busy && g_basi->net->busy()) {
            bcopy_str(g_nav_pending, u, URL_MAX);
            g_nav_have_pending = 1;
            return;
        }
        g_nav_in_flight = 1;
        g_in_submit = 1;
        int rc = g_basi->net->submit(browser_nav_work, browser_nav_done, 0);
        g_in_submit = 0;
        if (rc == 0) return;
        g_nav_in_flight = 0;
    }

    browser_nav_work(0);
    browser_nav_done(0, 0);
}

static void navigate_post(const char *url_in, const char *post_body, int record_history) {
    bcopy_str(g_nav_post_body, post_body ? post_body : "", sizeof(g_nav_post_body));
    g_nav_is_post = 1;
    navigate(url_in, record_history);
}

/* ---------------- Form gonderimi ---------------- */
/* form_id'ye ait tum alanlari toplar, x-www-form-urlencoded sorgu dizesi
 * kurar ve action'a (yoksa mevcut sayfaya) GET olarak gezinir.
 * NOT: ASI ag katmaninda yalnizca GET vardir; method="post" olan formlar da
 * GET'e dusurulur. Gercek POST govdesi gerekiyorsa once asi->net'e bir
 * "https_post" ilkeli eklenmelidir - bu dosyanin degistiremeyecegi bir sey. */
static void submit_form(int form_id, const char *fallback_action) {
    char action[HREF_MAX];
    if (form_id >= 0 && form_id < g_form_count && g_forms[form_id].action[0]) {
        resolve_url(g_cur_scheme, g_cur_host, g_forms[form_id].action, action, sizeof(action));
        if (!action[0]) bcopy_str(action, g_cur_url, sizeof(action));
    } else {
        bcopy_str(action, fallback_action && fallback_action[0] ? fallback_action : g_cur_url,
                  sizeof(action));
    }

    char query[URL_MAX];
    query[0] = 0;
    int first = 1;

    for (int i = 0; i < g_block_count; i++) {
        block_t *b = &g_blocks[i];
        if (b->form_id != form_id || !b->name[0]) continue;

        const char *value = 0;
        char selbuf[64];

        if (b->type == BLK_FORM_TEXT && b->widget) {
            value = lv_textarea_get_text(b->widget);
        } else if (b->type == BLK_FORM_HIDDEN) {
            value = b->href;
        } else if (b->type == BLK_FORM_SELECT && b->widget) {
            /* secilen index'e karsilik gelen degeri "deger1\ndeger2..." icinden cek */
            uint16_t sel = lv_dropdown_get_selected(b->widget);
            const char *p = b->href;
            uint16_t idx = 0;
            int n = 0;
            while (*p && idx < sel) { if (*p == '\n') idx++; p++; }
            while (*p && *p != '\n' && n < (int)sizeof(selbuf) - 1) selbuf[n++] = *p++;
            selbuf[n] = 0;
            value = selbuf;
        } else {
            continue;
        }
        if (!value) continue;

        bcat(query, first ? "?" : "&", sizeof(query));
        url_encode_append(query, sizeof(query), b->name);
        bcat(query, "=", sizeof(query));
        url_encode_append(query, sizeof(query), value);
        first = 0;
    }

    char full[URL_MAX];
    bcopy_str(full, action, sizeof(full));
    bcat(full, query, sizeof(full));

    if (g_addr) lv_textarea_set_text(g_addr, full);
    navigate(full, 1);
}

/* ---------------- Linke tiklama ---------------- */
static void link_click_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= g_block_count) return;

    char href_copy[HREF_MAX];
    bcopy_str(href_copy, g_blocks[idx].href, sizeof(href_copy));

    char resolved[URL_MAX];
    resolve_url(g_cur_scheme, g_cur_host, href_copy, resolved, sizeof(resolved));
    if (!resolved[0]) return; /* sayfa ici capa (#...) - yoksay */

    if (g_addr) lv_textarea_set_text(g_addr, resolved);
    navigate(resolved, 1);
}

static void form_submit_click_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= g_block_count) return;
    submit_form(g_blocks[idx].form_id, "");
}

/* Metin kutusunda Enter'a basmak, o forma ait bir gonder butonu olmasa bile
 * formu gonderir (Google arama kutusu tam olarak boyle calisir). */
static void form_text_ready_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= g_block_count) return;
    submit_form(g_blocks[idx].form_id, "");
}

/* ---------------- Bloklari LVGL nesnelerine cizme ---------------- */
static void render_blocks(void) {
    if (!g_content) return;
    lv_obj_clean(g_content);
    lv_obj_set_style_bg_color(g_content, lv_color_hex(ADL_BG), 0);
    lv_obj_set_style_bg_opa(g_content, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_top(g_content, 28, 0);   /* spec bolum 8: 28 ust, 40 yan */
    lv_obj_set_style_pad_bottom(g_content, 16, 0);
    lv_obj_set_style_pad_hor(g_content, 40, 0);
    lv_obj_set_style_pad_row(g_content, 6, 0);

    for (int i = 0; i < g_block_count; i++) {
        block_t *b = &g_blocks[i];
        switch (b->type) {
        case BLK_H1:
        case BLK_H2:
        case BLK_H3: {
            lv_obj_t *l = lv_label_create(g_content);
            lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(l, LV_PCT(100));
            lv_label_set_text(l, b->text);
            uint32_t col = (b->type == BLK_H1) ? ADL_ACCENT :
                           (b->type == BLK_H2) ? ADL_ACCENT_2 : ADL_TEXT;
            lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
            lv_obj_set_style_pad_top(l, (b->type == BLK_H1) ? 14 : 8, 0);
            lv_obj_set_style_pad_bottom(l, 4, 0);
            break;
        }
        case BLK_TEXT: {
            lv_obj_t *l = lv_label_create(g_content);
            lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(l, LV_PCT(100));
            lv_label_set_text(l, b->text);
            lv_obj_set_style_text_color(l, lv_color_hex(ADL_TEXT), 0);
            lv_obj_set_style_pad_bottom(l, 4, 0);
            break;
        }
        case BLK_LIST_ITEM: {
            lv_obj_t *row = lv_obj_create(g_content);
            lv_obj_remove_style_all(row);
            lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_style_pad_left(row, 14, 0);
            lv_obj_set_style_pad_bottom(row, 2, 0);
            lv_obj_t *l = lv_label_create(row);
            lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(l, LV_PCT(96));
            lv_label_set_text_fmt(l, "- %s", b->text);
            lv_obj_set_style_text_color(l, lv_color_hex(ADL_TEXT), 0);
            break;
        }
        case BLK_LINK: {
            lv_obj_t *l = lv_label_create(g_content);
            lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(l, LV_PCT(100));
            lv_label_set_text(l, b->text);
            lv_obj_set_style_text_color(l, lv_color_hex(ADL_ACCENT), 0);
            lv_obj_set_style_text_decor(l, LV_TEXT_DECOR_UNDERLINE, 0);
            lv_obj_set_style_pad_bottom(l, 4, 0);
            lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(l, link_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            break;
        }
        case BLK_IMG: {
            lv_obj_t *box = lv_obj_create(g_content);
            lv_obj_set_size(box, LV_PCT(100), 56);
            lv_obj_set_style_bg_color(box, lv_color_hex(ADL_SURFACE_2), 0);
            lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(box, lv_color_hex(ADL_BORDER), 0);
            lv_obj_set_style_border_width(box, 1, 0);
            lv_obj_set_style_radius(box, 6, 0);
            lv_obj_set_style_pad_bottom(box, 8, 0);
            lv_obj_t *l = lv_label_create(box);
            lv_label_set_text_fmt(l, "[Resim: %s]", b->text);
            lv_obj_set_style_text_color(l, lv_color_hex(ADL_TEXT_DIM), 0);
            lv_obj_center(l);
            break;
        }
        case BLK_HR: {
            lv_obj_t *line = lv_obj_create(g_content);
            lv_obj_remove_style_all(line);
            lv_obj_set_size(line, LV_PCT(100), 1);
            lv_obj_set_style_bg_color(line, lv_color_hex(ADL_BORDER), 0);
            lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
            lv_obj_set_style_margin_top(line, 6, 0);
            lv_obj_set_style_margin_bottom(line, 6, 0);
            break;
        }
        case BLK_FORM_TEXT: {
            lv_obj_t *ta = lv_textarea_create(g_content);
            lv_textarea_set_one_line(ta, 1);
            lv_obj_set_width(ta, LV_PCT(94));
            if (b->text[0]) lv_textarea_set_placeholder_text(ta, b->text);
            if (b->href[0]) lv_textarea_set_text(ta, b->href); /* value= ile gelen baslangic degeri */
            lv_obj_set_style_pad_bottom(ta, 6, 0);
            lv_obj_add_event_cb(ta, form_text_ready_cb, LV_EVENT_READY, (void *)(intptr_t)i);
            b->widget = ta;
            break;
        }
        case BLK_FORM_HIDDEN:
            /* gorunmez alan, cizilmez */
            break;
        case BLK_FORM_SELECT: {
            lv_obj_t *dd = lv_dropdown_create(g_content);
            lv_obj_set_width(dd, LV_PCT(94));
            lv_dropdown_set_options(dd, b->text);
            lv_obj_set_style_pad_bottom(dd, 6, 0);
            b->widget = dd;
            break;
        }
        case BLK_FORM_SUBMIT: {
            lv_obj_t *btn = lv_button_create(g_content);
            lv_obj_set_style_bg_color(btn, lv_color_hex(ADL_ACCENT), 0);
            lv_obj_set_style_radius(btn, 8, 0);
            lv_obj_set_style_pad_bottom(btn, 6, 0);
            lv_obj_add_event_cb(btn, form_submit_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_t *l = lv_label_create(btn);
            lv_label_set_text(l, b->text);
            lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
            break;
        }
        default:
            break;
        }
    }
}

/* ================= CSS3 Motoru, DOM, Yerlesim, Boyama ve Etkilesim ================= */

#define CSS_NODES      2000
#define CSS_RULES_MAX  65536
#define CSS_SEL        256
#define CSS_POOL_LIMIT (4 * 1024 * 1024)
#define CSS_NTEXT      420
#define CSS_KF_MAX     64
#define CSS_VSTK_MAX   2048

#define IMG_MAX        64
#define SVG_BLOB_MAX   220000
#define IMG_EXTRA      512
#define BG_DSC_MAX     512

#define RULE_F_HOVER     (1u << 0)
#define RULE_F_CHECKED   (1u << 1)
#define RULE_F_TARGET    (1u << 2)
#define RULE_F_FOCUS     (1u << 3)
#define RULE_F_CONT      (1u << 4)

typedef struct {
    char tag[24];
    char id[64];
    char cls[512];
    char istyle[256];
    char text[CSS_NTEXT];
    char src[160];
    char href[160];
    char name[32];
    char type[16];
    char value[120];
    char ph[96];
    char alt[48];
    char for_attr[40];
    char data_cat[32];
    char data_tip[96];
    char extra_attrs[320];
    int  wa, wa_pct, ha, ha_pct;
    int  align_attr;
    int  svg_off, svg_len;
    int  has_bgc; uint32_t bgc;
    int  form_id;
    int  rows;
    uint8_t is_text;
    uint8_t checked;
    uint8_t is_open;
    uint8_t required;
    uint8_t hover_sens;
    uint8_t focus_sens;
    uint8_t is_ta;
    uint8_t hidden_attr;
    int16_t eff_rot_y;
    int parent, first, last, next;
    lv_obj_t *widget;
} dnode_t;

typedef struct {
    char sel[CSS_SEL];
    int  spec;
    int  decl_off;
    int  decl_len;
    int  order;
    uint8_t flags;
    int16_t min_cw;
    int16_t max_cw;
    char key_tag[24];
    char key_id[64];
    char key_cls[96];
    uint8_t key_cls_len;
    int  idx_next;
} css_rule_t;

typedef struct {
    int pct;
    int decl_off;
    int decl_len;
} css_kf_stop_t;

typedef struct {
    char name[40];
    css_kf_stop_t stops[6];
    int nstops;
} css_kf_t;

typedef struct {
    uint32_t rgb;
    uint8_t  a;
    float    pos;   /* 0.0 .. 1.0 */
    uint8_t  has_pos;
} css_gstop_t;

#define GRAD_NONE   0
#define GRAD_LINEAR 1
#define GRAD_RADIAL 2
#define GRAD_CONIC  3

typedef struct {
    uint8_t     kind;
    float       angle;      /* deg: linear (CSS angle, 180=to bottom) or conic from-angle */
    float       cx, cy;     /* 0..1 center for radial/conic */
    css_gstop_t stops[8];
    int         nstops;
} css_grad_t;

typedef struct {
    int x, y, blur, spread;
    uint32_t rgb;
    uint8_t  a;
    uint8_t  inset;
} css_shadow_t;

typedef struct {
    float x[12], y[12];
    uint8_t xp[12], yp[12]; /* 1 = percent */
    int n;
} css_poly_t;

typedef struct {
    uint8_t type; /* 0=fr, 1=px, 2=auto */
    int     val;
} css_gtrack_t;

typedef struct {
    uint32_t color; int has_color; int color_a;
    uint32_t bg; int has_bg; int bg_transparent; int bg_a;
    int font_size; int has_font;
    int bold, italic, mono, underline, strike;
    int align;
    int pad[4]; int has_pad;
    int mar[4]; int has_mar;
    int mar_auto_l, mar_auto_r;
    int has_w, width, w_pct;
    int has_h, height, h_pct;
    int has_minw, minw, minw_pct; int has_maxw, maxw, maxw_pct;
    int has_minh, minh, minh_pct; int has_maxh, maxh, maxh_pct;
    /* border radius: simple or 8-value elliptical */
    int radius, radius_pct;
    int has_corner_radius;
    int rad_rx[4], rad_ry[4];
    uint8_t rad_rx_p[4], rad_ry_p[4];
    int radius_inherit;
    /* borders */
    int border_w; uint32_t border_c; int border_a;
    int border_sides;   /* bitmask: 1=top,2=right,4=bottom,8=left; 0=all when border_w>0 */
    int border_style;   /* 0=solid, 1=none, 2=dashed, 3=dotted */
    int side_bw[4]; uint32_t side_bc[4]; int side_ba[4]; int side_bs[4];
    /* shadows (up to 2) */
    css_shadow_t shadows[2];
    int nshadows;
    int shadow; /* compat */
    /* outline */
    int has_outline, outline_w, outline_a, outline_offset; uint32_t outline_c;
    /* display / visibility */
    int display; /* 0=block, 1=inline, 2=inline-block, 3=flex, 4=inline-flex, 5=grid, 6=inline-grid, 7=none */
    int hide;
    int vis_hidden;
    int sr_clip;
    int is_flex;
    int is_grid;
    int row;
    int wrap;
    int has_flexdir;
    int justify;
    int align_items;
    int justify_items;
    int align_content;
    int align_self;
    int gap, row_gap, col_gap;
    int flex_grow;
    int flex_basis, has_flex_basis;
    int order, has_order;
    int float_dir; /* 0=none, 1=left, 2=right */
    /* positioning */
    int position; /* 0=static, 1=relative, 2=absolute, 3=fixed, 4=sticky */
    int top, left, right, bottom;
    int top_pct, left_pct, right_pct, bottom_pct;
    int has_top, has_left, has_right, has_bottom;
    int z_index, has_z;
    int pointer_none;
    /* typography & overflow */
    int overflow_hidden;
    int line_height;
    int letter_space;
    int nowrap;
    int text_indent;
    int ellipsis;
    int transform; /* text-transform: 1=upper,2=lower,3=cap */
    int writing_vert; /* 1=vertical-rl/lr */
    int list_type, has_list;
    /* opacity */
    int has_opacity;
    int opacity; /* 0..100 */
    /* gradients & background-image (up to 2 layers) */
    css_grad_t grads[2];
    int ngrads;
    int has_grad; uint32_t grad_c; int grad_dir; /* compat */
    int bg_clip_text;
    int text_fill_transp;
    char bg_url[280];
    int has_bg_url;
    int bg_size, bg_size_w, bg_size_h, bg_size_wp, bg_size_hp;
    int bg_repeat;
    int has_bg_pos, bg_pos_x, bg_pos_y, bg_pos_xp, bg_pos_yp;
    /* mask */
    int has_mask_fade;
    /* pseudo content */
    int has_content; char content[160];
    int content_url; char content_src[160];
    /* transforms & 3D */
    int tx, ty, tx_pct, ty_pct, has_translate;
    int tscale, tscalex, tscaley, has_scale;
    int trot, has_rot;
    int trot_y, has_rot_y;
    int preserve_3d;
    int backface_hidden;
    /* filters & blend & clip-path */
    int has_filter;
    float flt_blur;
    float flt_hue;
    float flt_contrast;
    float flt_bright;
    float flt_sat;
    float flt_gray;
    float flt_invert;
    float flt_sepia;
    int flt_dsh_x, flt_dsh_y, flt_dsh_b; uint32_t flt_dsh_c; uint8_t flt_dsh_a;
    int has_backdrop_blur;
    int blend_mode; /* 0=normal, 1=difference, 2=screen, 3=multiply */
    css_poly_t clip_poly;
    /* grid */
    int grid_cols;
    css_gtrack_t gtracks[16];
    int gtrack_n;
    int grid_auto_fit;
    int grid_min_w;
    int grid_rows;
    int col_span, has_col_span;
    int row_span, has_row_span;
    /* multi-column */
    int col_count;
    int col_rule_w, col_rule_style; uint32_t col_rule_c; int col_rule_a;
    /* aspect ratio & object-fit */
    int object_fit;
    int aspect; int has_aspect;
    /* accent & animation */
    uint32_t accent_c; int has_accent;
    char anim_name[40];
    int  anim_dur_ms;
    int  anim_paused;
    int  anim_alt;
} css_style_t;

static dnode_t    *g_dn;
static int         g_dn_count;
static int         g_dn_cap;
static css_rule_t *g_css;
static int         g_css_count;
static int         g_css_cap;
#define CSS_IDX_CLS_SZ 4096
#define CSS_IDX_TAG_SZ 256
#define CSS_IDX_ID_SZ  256
static int         g_css_cls_ht[CSS_IDX_CLS_SZ];
static int         g_css_tag_ht[CSS_IDX_TAG_SZ];
static int         g_css_id_ht[CSS_IDX_ID_SZ];
static int         g_css_univ_head = -1;
static int         g_css_idx_built = 0;
static char       *g_css_pool;
static int         g_css_pool_len;
static int         g_css_pool_cap;
static css_kf_t    g_kf[CSS_KF_MAX];
static int         g_kf_count;

/* Scoped CSS custom property stack (dinamik heap + hizli hash) */
typedef struct {
    char     name[80];
    char     val[220];
    uint32_t hash;
    int      depth;
} css_var_t;
static css_var_t *g_vstk;
static int        g_vstk_n;
static int        g_vstk_cap;
static int        g_vcount; /* compat alias */

/* Dynamic CSS3 interaction state */
static int       g_pe; /* 0=elem, 1=::before, 2=::after, 3=::first-letter */
static char      g_cur_hash[64];
static int       g_hover_node = -1;
static int       g_focus_node = -1;
static int       g_cur_avail_w = 1280;
static int       g_cur_cont_w  = 1280;
static int       g_view_w = 1280, g_view_h = 800;
static int       g_inh_font = 16;
static uint32_t  g_inh_color = 0x202124;
static int       g_root_font = 16;
static int       g_css_alpha = 255;

/* Fixed overlay & sticky header & animation slots */
static lv_obj_t   *g_fixed_layer;
static lv_obj_t   *g_sticky_obj;
static int         g_last_mx = -9999, g_last_my = -9999;
static int         g_restyle_pending = 0;

typedef struct {
    lv_obj_t *obj;
    int       node;
    int       kind; /* 1=pulse_dot, 2=marquee, 3=pulse_glow, 4=float_orb */
    int       dur_ms;
    int       param;
    int       paused;
} css_anim_ent_t;
#define CSS_ANIM_MAX 32
static css_anim_ent_t g_anims[CSS_ANIM_MAX];
static int g_anim_n;

/* Sayfadaki <script> ve <svg> tamponlari */
static char g_script_src[300000];
static int  g_script_len;
static char g_svg_blob[SVG_BLOB_MAX];
static int  g_svg_blob_len;

/* Formlar ve kaynaklar */
typedef struct { char action[200]; char method[12]; int node; } pform_t;
static pform_t  g_pforms[24];
static int      g_pform_count;

typedef struct {
    char     url[440];
    uint8_t *data;
    uint32_t len;
    int      is_css;
} res_ent_t;
static res_ent_t *g_res;
static int        g_res_count;
static int        g_res_cap;

typedef struct { char url[440]; uint32_t *px; int w, h; } img_ent_t;
static img_ent_t g_imgs[IMG_MAX];
static int       g_img_count;

static uint32_t      *g_img_extra[IMG_EXTRA];
static int            g_img_extra_n;
static lv_image_dsc_t g_bg_dsc[BG_DSC_MAX];
static int            g_bg_dsc_n;

/* Ileri bildirimler */
static void css_trim(char *s);
static int  strn_ci_eq(const char *a, const char *b, int n);
static int  css_color(const char *s, uint32_t *out);
static int  sel_match(int n, const char *sel);
static int  sel_spec(const char *sel);
static void restyle_page(void);
static void collect_css_from_html(const char *html);
static void build_node(int n, lv_obj_t *parent, css_style_t *ps, int avail_w, int avail_h, int depth);
static lv_font_t *bfont_ex(int size, int bold);

/* ---------------- Yardimcilar ---------------- */
static void css_trim(char *s) {
    int n = b_len(s);
    while (n > 0 && (s[n-1]==' '||s[n-1]=='\t'||s[n-1]=='\n'||s[n-1]=='\r')) s[--n] = 0;
    int i = 0;
    while (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r') i++;
    int j = 0;
    while (s[i]) s[j++] = s[i++];
    s[j] = 0;
}

static int strn_ci_eq(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) if (lower_c(a[i]) != lower_c(b[i])) return 0;
    return 1;
}

static int css_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ---------------- Scoped CSS Custom Properties ---------------- */
static uint32_t css_var_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

static int ensure_vstk(int need) {
    if (g_vstk && need <= g_vstk_cap) return 1;
    if (need > g_vstk_max) return 0;
    if (!g_basi || !g_basi->mem || !g_basi->mem->alloc) return 0;
    int nc = g_vstk_cap ? g_vstk_cap * 2 : 2048;
    while (nc < need) nc *= 2;
    if (nc > g_vstk_max) nc = g_vstk_max;
    css_var_t *na = (css_var_t *)g_basi->mem->alloc((size_t)nc * sizeof(css_var_t));
    if (!na) return 0;
    for (int i = 0; i < g_vstk_n; i++) na[i] = g_vstk[i];
    if (g_vstk && g_basi->mem->free) g_basi->mem->free(g_vstk);
    g_vstk = na;
    g_vstk_cap = nc;
    return 1;
}

static void css_var_push(const char *name, const char *val, int depth) {
    uint32_t h = css_var_hash(name);
    if (g_vstk) {
        for (int i = g_vstk_n - 1; i >= 0 && g_vstk[i].depth == depth; i--) {
            if (g_vstk[i].hash == h && str_eq(g_vstk[i].name, name)) {
                bcopy_str(g_vstk[i].val, val, sizeof(g_vstk[i].val));
                return;
            }
        }
    }
    if (!ensure_vstk(g_vstk_n + 1)) return;
    bcopy_str(g_vstk[g_vstk_n].name, name, sizeof(g_vstk[0].name));
    bcopy_str(g_vstk[g_vstk_n].val, val, sizeof(g_vstk[0].val));
    g_vstk[g_vstk_n].hash = h;
    g_vstk[g_vstk_n].depth = depth;
    g_vstk_n++;
    g_vcount = g_vstk_n;
}

static void css_var_pop(int depth) {
    if (!g_vstk) { g_vstk_n = 0; g_vcount = 0; return; }
    while (g_vstk_n > 0 && g_vstk[g_vstk_n - 1].depth >= depth) g_vstk_n--;
    g_vcount = g_vstk_n;
}

static const char *css_var_get(const char *name) {
    if (!g_vstk) return 0;
    uint32_t h = css_var_hash(name);
    for (int i = g_vstk_n - 1; i >= 0; i--) {
        if (g_vstk[i].hash == h && str_eq(g_vstk[i].name, name)) return g_vstk[i].val;
    }
    return 0;
}

static void css_var_subst_once(const char *in, char *out, int cap) {
    int o = 0;
    const char *p = in;
    while (*p && o < cap - 1) {
        if ((p[0]=='v'||p[0]=='V') && (p[1]=='a'||p[1]=='A') && (p[2]=='r'||p[2]=='R') && p[3]=='(') {
            const char *q = p + 4;
            while (*q == ' ' || *q == '\t') q++;
            char name[80]; int n = 0;
            while (*q && *q != ')' && *q != ',') {
                if (n < 79) name[n++] = *q;
                q++;
            }
            name[n] = 0;
            css_trim(name);
            char fb[220]; fb[0] = 0;
            int depth = 1;
            if (*q == ',') {
                q++;
                while (*q == ' ' || *q == '\t') q++;
                int fn = 0;
                while (*q && depth > 0) {
                    if (*q == '(') depth++;
                    else if (*q == ')') { depth--; if (depth == 0) break; }
                    if (fn < 219) fb[fn++] = *q;
                    q++;
                }
                fb[fn] = 0;
                css_trim(fb);
            } else {
                while (*q && depth > 0) {
                    if (*q == '(') depth++;
                    else if (*q == ')') { depth--; if (depth == 0) break; }
                    q++;
                }
            }
            if (*q == ')') q++;
            const char *val = css_var_get(name);
            if (!val || !val[0]) val = fb;
            if (val) {
                for (int i = 0; val[i] && o < cap - 1; i++) out[o++] = val[i];
            }
            p = q;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = 0;
}

static void css_var_subst(const char *in, char *out, int cap) {
    char tmp1[440], tmp2[440];
    bcopy_str(tmp1, in, sizeof(tmp1));
    for (int pass = 0; pass < 6; pass++) {
        if (!find_substr_ci(tmp1, "var(")) break;
        css_var_subst_once(tmp1, tmp2, sizeof(tmp2));
        bcopy_str(tmp1, tmp2, sizeof(tmp1));
    }
    bcopy_str(out, tmp1, cap);
}

/* ---------------- Matematik yardimcilari (libc yok) ---------------- */
static double d_fabs(double x) { return x < 0 ? -x : x; }
static double d_sqrt(double x) {
    if (x <= 0) return 0;
    double g = x > 1.0 ? x * 0.5 : 1.0;
    for (int i = 0; i < 16; i++) g = 0.5 * (g + x / g);
    return g;
}
static double d_log(double x) {
    if (x <= 0) return -1e30;
    int k = 0;
    while (x >= 2.0) { x /= 2.0; k++; }
    while (x < 1.0) { x *= 2.0; k--; }
    double z = (x - 1.0) / (x + 1.0);
    double z2 = z * z, sum = 0, term = z;
    for (int i = 1; i <= 21; i += 2) { sum += term / i; term *= z2; }
    return 2.0 * sum + k * 0.6931471805599453;
}
static double d_exp(double x) {
    int k = (int)(x / 0.6931471805599453 + (x >= 0 ? 0.5 : -0.5));
    double r = x - k * 0.6931471805599453;
    double sum = 1.0, term = 1.0;
    for (int i = 1; i <= 18; i++) { term *= r / i; sum += term; }
    while (k > 0) { sum *= 2.0; k--; }
    while (k < 0) { sum /= 2.0; k++; }
    return sum;
}
static double d_pow(double b, double e) {
    if (b < 0) return -d_exp(e * d_log(-b));
    if (b == 0) return 0;
    return d_exp(e * d_log(b));
}
static double d_sin(double x) {
    while (x > 3.141592653589793) x -= 6.283185307179586;
    while (x < -3.141592653589793) x += 6.283185307179586;
    double x2 = x * x, term = x, sum = x;
    for (int i = 3; i < 22; i += 2) { term *= -x2 / (double)(i * (i - 1)); sum += term; }
    return sum;
}
static double d_cos(double x) { return d_sin(x + 1.5707963267948966); }
static float f_atan2(float y, float x) {
    if (x == 0 && y == 0) return 0;
    float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, a;
    if (ax >= ay) {
        float z = ay / ax, z2 = z * z;
        a = z * (0.9998660f - z2 * (0.3302995f - z2 * (0.1801410f - z2 * 0.0851330f)));
        if (x < 0) a = 3.14159265f - a;
    } else {
        float z = ax / ay, z2 = z * z;
        a = 1.57079632f - z * (0.9998660f - z2 * (0.3302995f - z2 * (0.1801410f - z2 * 0.0851330f)));
    }
    return y < 0 ? -a : a;
}

static double css_f(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    double v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10.0 + (*s - '0'); s++; }
    if (*s == '.') { s++; double f = 0.1; while (*s >= '0' && *s <= '9') { v += (*s - '0') * f; f /= 10.0; s++; } }
    return neg ? -v : v;
}

/* ---------------- CSS3 Renk Motoru ---------------- */
typedef struct { const char *name; uint32_t rgb; } css_named_t;
static int css_named(const char *s, uint32_t *out) {
    static const css_named_t t[] = {
        {"aliceblue",0xF0F8FF},{"antiquewhite",0xFAEBD7},{"aqua",0x00FFFF},
        {"aquamarine",0x7FFFD4},{"azure",0xF0FFFF},{"beige",0xF5F5DC},
        {"bisque",0xFFE4C4},{"black",0x000000},{"blanchedalmond",0xFFEBCD},
        {"blue",0x0000FF},{"blueviolet",0x8A2BE2},{"brown",0xA52A2A},
        {"burlywood",0xDEB887},{"cadetblue",0x5F9EA0},{"chartreuse",0x7FFF00},
        {"chocolate",0xD2691E},{"coral",0xFF7F50},{"cornflowerblue",0x6495ED},
        {"cornsilk",0xFFF8DC},{"crimson",0xDC143C},{"cyan",0x00FFFF},
        {"darkblue",0x00008B},{"darkcyan",0x008B8B},{"darkgoldenrod",0xB8860B},
        {"darkgray",0xA9A9A9},{"darkgrey",0xA9A9A9},{"darkgreen",0x006400},
        {"darkkhaki",0xBDB76B},{"darkmagenta",0x8B008B},{"darkolivegreen",0x556B2F},
        {"darkorange",0xFF8C00},{"darkorchid",0x9932CC},{"darkred",0x8B0000},
        {"darksalmon",0xE9967A},{"darkseagreen",0x8FBC8F},{"darkslateblue",0x483D8B},
        {"darkslategray",0x2F4F4F},{"darkslategrey",0x2F4F4F},{"darkturquoise",0x00CED1},
        {"darkviolet",0x9400D3},{"deeppink",0xFF1493},{"deepskyblue",0x00BFFF},
        {"dimgray",0x696969},{"dimgrey",0x696969},{"dodgerblue",0x1E90FF},
        {"firebrick",0xB22222},{"floralwhite",0xFFFAF0},{"forestgreen",0x228B22},
        {"fuchsia",0xFF00FF},{"gainsboro",0xDCDCDC},{"ghostwhite",0xF8F8FF},
        {"gold",0xFFD700},{"goldenrod",0xDAA520},{"gray",0x808080},
        {"grey",0x808080},{"green",0x008000},{"greenyellow",0xADFF2F},
        {"honeydew",0xF0FFF0},{"hotpink",0xFF69B4},{"indianred",0xCD5C5C},
        {"indigo",0x4B0082},{"ivory",0xFFFFF0},{"khaki",0xF0E68C},
        {"lavender",0xE6E6FA},{"lavenderblush",0xFFF0F5},{"lawngreen",0x7CFC00},
        {"lemonchiffon",0xFFFACD},{"lightblue",0xADD8E6},{"lightcoral",0xF08080},
        {"lightcyan",0xE0FFFF},{"lightgoldenrodyellow",0xFAFAD2},{"lightgray",0xD3D3D3},
        {"lightgrey",0xD3D3D3},{"lightgreen",0x90EE90},{"lightpink",0xFFB6C1},
        {"lightsalmon",0xFFA07A},{"lightseagreen",0x20B2AA},{"lightskyblue",0x87CEFA},
        {"lightslategray",0x778899},{"lightslategrey",0x778899},{"lightsteelblue",0xB0C4DE},
        {"lightyellow",0xFFFFE0},{"lime",0x00FF00},{"limegreen",0x32CD32},
        {"linen",0xFAF0E6},{"magenta",0xFF00FF},{"maroon",0x800000},
        {"mediumaquamarine",0x66CDAA},{"mediumblue",0x0000CD},{"mediumorchid",0xBA55D3},
        {"mediumpurple",0x9370DB},{"mediumseagreen",0x3CB371},{"mediumslateblue",0x7B68EE},
        {"mediumspringgreen",0x00FA9A},{"mediumturquoise",0x48D1CC},{"mediumvioletred",0xC71585},
        {"midnightblue",0x191970},{"mintcream",0xF5FFFA},{"mistyrose",0xFFE4E1},
        {"moccasin",0xFFE4B5},{"navajowhite",0xFFDEAD},{"navy",0x000080},
        {"oldlace",0xFDF5E6},{"olive",0x808000},{"olivedrab",0x6B8E23},
        {"orange",0xFFA500},{"orangered",0xFF4500},{"orchid",0xDA70D6},
        {"palegoldenrod",0xEEE8AA},{"palegreen",0x98FB98},{"paleturquoise",0xAFEEEE},
        {"palevioletred",0xDB7093},{"papayawhip",0xFFEFD5},{"peachpuff",0xFFDAB9},
        {"peru",0xCD853F},{"pink",0xFFC0CB},{"plum",0xDDA0DD},
        {"powderblue",0xB0E0E6},{"purple",0x800080},{"rebeccapurple",0x663399},
        {"red",0xFF0000},{"rosybrown",0xBC8F8F},{"royalblue",0x4169E1},
        {"saddlebrown",0x8B4513},{"salmon",0xFA8072},{"sandybrown",0xF4A460},
        {"seagreen",0x2E8B57},{"seashell",0xFFF5EE},{"sienna",0xA0522D},
        {"silver",0xC0C0C0},{"skyblue",0x87CEEB},{"slateblue",0x6A5ACD},
        {"slategray",0x708090},{"slategrey",0x708090},{"snow",0xFFFAFA},
        {"springgreen",0x00FF7F},{"steelblue",0x4682B4},{"tan",0xD2B48C},
        {"teal",0x008080},{"thistle",0xD8BFD8},{"tomato",0xFF6347},
        {"turquoise",0x40E0D0},{"violet",0xEE82EE},{"wheat",0xF5DEB3},
        {"white",0xFFFFFF},{"whitesmoke",0xF5F5F5},{"yellow",0xFFFF00},
        {"yellowgreen",0x9ACD32},
    };
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (str_eq_ci(s, t[i].name)) { *out = t[i].rgb; return 1; }
    return 0;
}

static double css_compf(const char **pp, int *pct) {
    const char *p = *pp;
    while (*p == ' ' || *p == '\t' || *p == ',' || *p == '/') p++;
    int neg = 0, any = 0;
    if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
    double v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10.0 + (*p - '0'); p++; any = 1; }
    if (*p == '.') { p++; double f = 0.1; while (*p >= '0' && *p <= '9') { v += (*p - '0') * f; f /= 10.0; p++; any = 1; } }
    *pct = 0;
    if (*p == '%') { *pct = 1; p++; }
    else if (strn_ci_eq(p, "deg", 3)) p += 3;
    else if (strn_ci_eq(p, "turn", 4)) { v *= 360.0; p += 4; }
    else if (strn_ci_eq(p, "rad", 3)) { v *= 57.2957795; p += 3; }
    *pp = p;
    if (!any) return -999999.0;
    return neg ? -v : v;
}

static int css_alpha_val(const char **pp) {
    const char *p = *pp;
    while (*p == ' ' || *p == '\t' || *p == ',' || *p == '/') p++;
    if (!*p || *p == ')') { *pp = p; return -1; }
    int pc = 0;
    double v = css_compf(&p, &pc);
    *pp = p;
    if (v < -900000.0) return -1;
    if (pc) v = v * 255.0 / 100.0;
    else v = (v <= 1.0) ? v * 255.0 : v;
    int r = (int)(v + 0.5);
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    return r;
}

static void hsl_to_rgb(double h, double s, double l, uint32_t *out) {
    while (h < 0) h += 360.0;
    while (h >= 360.0) h -= 360.0;
    s /= 100.0; l /= 100.0;
    if (s < 0) s = 0; if (s > 1) s = 1;
    if (l < 0) l = 0; if (l > 1) l = 1;
    double c = (1.0 - d_fabs(2.0 * l - 1.0)) * s;
    double hp = h / 60.0;
    int hi = (int)hp;
    double x = c * (1.0 - d_fabs((hp - (hi & ~1)) - 1.0));
    double r = 0, g = 0, b = 0;
    switch (hi) {
        case 0: r = c; g = x; break;
        case 1: r = x; g = c; break;
        case 2: g = c; b = x; break;
        case 3: g = x; b = c; break;
        case 4: r = x; b = c; break;
        default: r = c; b = x; break;
    }
    double m = l - c * 0.5;
    int R = (int)((r + m) * 255.0 + 0.5);
    int G = (int)((g + m) * 255.0 + 0.5);
    int B = (int)((b + m) * 255.0 + 0.5);
    if (R < 0) R = 0; if (R > 255) R = 255;
    if (G < 0) G = 0; if (G > 255) G = 255;
    if (B < 0) B = 0; if (B > 255) B = 255;
    *out = ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
}

static void srgb_pack(double r, double g, double b, uint32_t *out, int alpha) {
    double v[3] = { r, g, b };
    int o[3];
    for (int i = 0; i < 3; i++) {
        double c = v[i];
        if (c < 0) c = 0;
        if (c > 1) c = 1;
        double s = (c <= 0.0031308) ? c * 12.92 : 1.055 * d_pow(c, 1.0 / 2.4) - 0.055;
        int iv = (int)(s * 255.0 + 0.5);
        if (iv < 0) iv = 0;
        if (iv > 255) iv = 255;
        o[i] = iv;
    }
    *out = ((uint32_t)o[0] << 16) | ((uint32_t)o[1] << 8) | (uint32_t)o[2];
    g_css_alpha = alpha;
}

static int css_lab_color(const char *s, uint32_t *out) {
    const char *p = s;
    int kind = 0;
    if (strn_ci_eq(p, "oklab(", 6)) { kind = 1; p += 6; }
    else if (strn_ci_eq(p, "oklch(", 6)) { kind = 2; p += 6; }
    else if (strn_ci_eq(p, "lab(", 4)) { kind = 3; p += 4; }
    else if (strn_ci_eq(p, "lch(", 4)) { kind = 4; p += 4; }
    else return -1;
    int p1 = 0, p2 = 0, p3 = 0;
    double c1 = css_compf(&p, &p1);
    double c2 = css_compf(&p, &p2);
    double c3 = css_compf(&p, &p3);
    int alpha = css_alpha_val(&p);
    if (alpha < 0) alpha = 255;
    if (kind == 1 || kind == 2) {
        double L = p1 ? c1 / 100.0 : c1;
        double a, b;
        if (kind == 2) { double C = p2 ? c2 / 100.0 : c2; a = C * d_cos(c3 * 3.141592653589793 / 180.0); b = C * d_sin(c3 * 3.141592653589793 / 180.0); }
        else { a = p2 ? c2 / 100.0 : c2; b = p3 ? c3 / 100.0 : c3; }
        double l_ = L + 0.3963377774 * a + 0.2158037573 * b;
        double m_ = L - 0.1055613458 * a - 0.0638541728 * b;
        double s_ = L - 0.0894841775 * a - 1.2914855480 * b;
        double l = l_*l_*l_, m = m_*m_*m_, ss = s_*s_*s_;
        srgb_pack(4.0767416621*l - 3.3077115913*m + 0.2309699292*ss,
                  -1.2684380046*l + 2.6097574011*m - 0.3413193965*ss,
                  -0.0041960863*l - 0.7034186147*m + 1.7076147010*ss, out, alpha);
    } else {
        double L = c1, a, b;
        if (kind == 4) { double C = p2 ? c2 * 1.25 : c2; a = C * d_cos(c3 * 3.141592653589793 / 180.0); b = C * d_sin(c3 * 3.141592653589793 / 180.0); }
        else { a = c2; b = c3; }
        double fy = (L + 16.0) / 116.0, fx = fy + a / 500.0, fz = fy - b / 200.0;
        double e = 216.0 / 24389.0, kk = 24389.0 / 27.0;
        double tx = fx*fx*fx, ty = fy*fy*fy, tz = fz*fz*fz;
        double X = 0.95047 * (tx > e ? tx : (116.0 * fx - 16.0) / kk);
        double Y = (ty > e ? ty : (116.0 * fy - 16.0) / kk);
        double Z = 1.08883 * (tz > e ? tz : (116.0 * fz - 16.0) / kk);
        srgb_pack(3.2406*X - 1.5372*Y - 0.4986*Z, -0.9689*X + 1.8758*Y + 0.0415*Z,
                  0.0557*X - 0.2040*Y + 1.0570*Z, out, alpha);
    }
    return 1;
}

static int css_color(const char *s, uint32_t *out) {
    if (!s || !s[0]) return 0;
    while (*s == ' ' || *s == '\t') s++;
    g_css_alpha = 255;
    if (str_eq_ci(s, "transparent")) { *out = 0; g_css_alpha = 0; return 1; }
    if (s[0] == '#') {
        int n = 0;
        while (s[1 + n] && css_hexval(s[1 + n]) >= 0) n++;
        if (s[1 + n] != 0 && s[1 + n] != ' ' && s[1 + n] != '\t' && s[1 + n] != ',' && s[1 + n] != ')') return 0;
        if (n == 3 || n == 4) {
            int r = css_hexval(s[1]), g = css_hexval(s[2]), b = css_hexval(s[3]);
            *out = (uint32_t)((r * 17) << 16 | (g * 17) << 8 | (b * 17));
            if (n == 4) g_css_alpha = css_hexval(s[4]) * 17;
            return 1;
        }
        if (n == 6 || n == 8) {
            int v[6];
            for (int i = 0; i < 6; i++) v[i] = css_hexval(s[1 + i]);
            *out = (uint32_t)((v[0]<<20)|(v[1]<<16)|(v[2]<<12)|(v[3]<<8)|(v[4]<<4)|v[5]);
            if (n == 8) g_css_alpha = css_hexval(s[7]) * 16 + css_hexval(s[8]);
            return 1;
        }
        return 0;
    }
    if (strn_ci_eq(s, "rgb(", 4) || strn_ci_eq(s, "rgba(", 5)) {
        const char *p = s + (s[3] == '(' ? 4 : 5);
        int pc1 = 0, pc2 = 0, pc3 = 0;
        double dr = css_compf(&p, &pc1);
        double dg = css_compf(&p, &pc2);
        double db = css_compf(&p, &pc3);
        if (dr < -900000.0 || dg < -900000.0 || db < -900000.0) return 0;
        if (pc1) dr = dr * 2.55;
        if (pc2) dg = dg * 2.55;
        if (pc3) db = db * 2.55;
        int r = (int)(dr + 0.5), g = (int)(dg + 0.5), b = (int)(db + 0.5);
        if (r < 0) r = 0; if (r > 255) r = 255;
        if (g < 0) g = 0; if (g > 255) g = 255;
        if (b < 0) b = 0; if (b > 255) b = 255;
        int av = css_alpha_val(&p);
        if (av >= 0) g_css_alpha = av;
        *out = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        return 1;
    }
    if (strn_ci_eq(s, "hsl(", 4) || strn_ci_eq(s, "hsla(", 5)) {
        const char *p = s + (s[3] == '(' ? 4 : 5);
        int pc1 = 0, pc2 = 0, pc3 = 0;
        double h = css_compf(&p, &pc1);
        double sat = css_compf(&p, &pc2);
        double lum = css_compf(&p, &pc3);
        if (h < -900000.0 || sat < -900000.0 || lum < -900000.0) return 0;
        int av = css_alpha_val(&p);
        if (av >= 0) g_css_alpha = av;
        hsl_to_rgb(h, sat, lum, out);
        return 1;
    }
    {
        int lr = css_lab_color(s, out);
        if (lr >= 0) return lr;
    }
    return css_named(s, out);
}

/* ---------------- CSS3 Uzunluk ve Matematik Fonksiyonlari ---------------- */
static int css_num(const char *v, int *pct) {
    int i = 0, n = 0;
    while (v[i] == ' ' || v[i] == '\t') i++;
    if (v[i] == '-') i++;
    while (v[i] >= '0' && v[i] <= '9') { n = n * 10 + (v[i] - '0'); i++; }
    while (v[i] == ' ') i++;
    *pct = (v[i] == '%');
    return n;
}

typedef struct { double px; int pct; int ok; } cexpr_t;

static double css_unit_mult(int u, double font_px) {
    switch (u) {
        case 0: return 1.0;
        case 2: return font_px > 0 ? font_px : 16.0;    /* em */
        case 3: return (double)g_root_font;              /* rem */
        case 4: return 4.0 / 3.0;                        /* pt */
        case 5: return (g_view_w > 0 ? g_view_w : 1280) / 100.0; /* vw */
        case 6: return (g_view_h > 0 ? g_view_h : 800) / 100.0;  /* vh */
        case 7: { int m = g_view_w < g_view_h ? g_view_w : g_view_h; return (m > 0 ? m : 800) / 100.0; }
        case 8: { int m = g_view_w > g_view_h ? g_view_w : g_view_h; return (m > 0 ? m : 1280) / 100.0; }
        case 9: return 37.8;
        case 10: return 3.78;
        case 11: return 96.0;
        default: return 1.0;
    }
}

static cexpr_t cexpr_parse(const char **pp, double ref_px, double font_px);

static cexpr_t cexpr_primary(const char **pp, double ref_px, double font_px) {
    const char *p = *pp;
    cexpr_t r = { 0, 0, 0 };
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '(') {
        p++;
        r = cexpr_parse(&p, ref_px, font_px);
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ')') p++;
        *pp = p;
        return r;
    }
    if (is_alpha(*p)) {
        char fn[16]; int n = 0;
        while (is_alpha(*p) && n < 15) { fn[n++] = lower_c(*p); p++; }
        fn[n] = 0;
        while (*p == ' ') p++;
        if (*p != '(') { *pp = p; return r; }
        p++;
        int iscalc  = str_eq(fn, "calc");
        int ismin   = str_eq(fn, "min");
        int ismax   = str_eq(fn, "max");
        int isclamp = str_eq(fn, "clamp");
        if (iscalc) {
            r = cexpr_parse(&p, ref_px, font_px);
            while (*p == ' ' || *p == '\t') p++;
            if (*p == ')') p++;
            r.pct = 0;
            *pp = p; return r;
        }
        if (ismin || ismax || isclamp) {
            cexpr_t a[8]; int na = 0;
            while (na < 8) {
                a[na++] = cexpr_parse(&p, ref_px, font_px);
                while (*p == ' ' || *p == '\t') p++;
                if (*p == ',') { p++; continue; }
                break;
            }
            int d = 1;
            while (*p && d > 0) { if (*p == '(') d++; else if (*p == ')') { d--; if (d == 0) { p++; break; } } p++; }
            r = a[0];
            if (ismin) { for (int i = 1; i < na; i++) if (a[i].ok && a[i].px < r.px) r = a[i]; }
            else if (ismax) { for (int i = 1; i < na; i++) if (a[i].ok && a[i].px > r.px) r = a[i]; }
            else if (isclamp && na == 3) {
                r = a[1];
                if (a[0].ok && r.px < a[0].px) r = a[0];
                if (a[2].ok && r.px > a[2].px) r = a[2];
            }
            r.pct = 0; /* resolved to concrete px against ref_px */
            *pp = p; return r;
        }
        int d = 1;
        while (*p && d > 0) { if (*p == '(') d++; else if (*p == ')') { d--; if (d == 0) { p++; break; } } p++; }
        *pp = p; return r;
    }
    int neg = 0, any = 0;
    if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
    double num = 0;
    while (*p >= '0' && *p <= '9') { num = num * 10.0 + (*p - '0'); p++; any = 1; }
    if (*p == '.') { p++; double f = 0.1; while (*p >= '0' && *p <= '9') { num += (*p - '0') * f; f /= 10.0; p++; any = 1; } }
    if (!any) { *pp = p; return r; }
    if (neg) num = -num;
    if (*p == '%') {
        p++;
        r.px = num * ref_px / 100.0;
        r.pct = 1;
        r.ok = 1;
        *pp = p; return r;
    }
    int u = -1, len = 0;
    if (strn_ci_eq(p, "px", 2)) { u = 0; len = 2; }
    else if (strn_ci_eq(p, "rem", 3)) { u = 3; len = 3; }
    else if (strn_ci_eq(p, "em", 2)) { u = 2; len = 2; }
    else if (strn_ci_eq(p, "pt", 2)) { u = 4; len = 2; }
    else if (strn_ci_eq(p, "vw", 2)) { u = 5; len = 2; }
    else if (strn_ci_eq(p, "vh", 2)) { u = 6; len = 2; }
    else if (strn_ci_eq(p, "vmin", 4)) { u = 7; len = 4; }
    else if (strn_ci_eq(p, "vmax", 4)) { u = 8; len = 4; }
    else if (strn_ci_eq(p, "cm", 2)) { u = 9; len = 2; }
    else if (strn_ci_eq(p, "mm", 2)) { u = 10; len = 2; }
    else if (strn_ci_eq(p, "in", 2)) { u = 11; len = 2; }
    if (u >= 0) { p += len; r.px = num * css_unit_mult(u, font_px); }
    else r.px = num;
    r.pct = 0; r.ok = 1;
    *pp = p;
    return r;
}

static cexpr_t cexpr_term(const char **pp, double ref_px, double font_px) {
    cexpr_t r = cexpr_primary(pp, ref_px, font_px);
    for (;;) {
        const char *p = *pp;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '*' || *p == '/') {
            char op = *p++;
            cexpr_t b = cexpr_primary(&p, ref_px, font_px);
            r.ok &= b.ok;
            r.px = (op == '*') ? (r.px * b.px) : (b.px != 0.0 ? r.px / b.px : 0.0);
            r.pct = 0;
            *pp = p;
        } else { *pp = p; break; }
    }
    return r;
}

static cexpr_t cexpr_parse(const char **pp, double ref_px, double font_px) {
    cexpr_t r = cexpr_term(pp, ref_px, font_px);
    for (;;) {
        const char *p = *pp;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '+' || *p == '-') {
            char op = *p++;
            cexpr_t b = cexpr_term(&p, ref_px, font_px);
            r.ok &= b.ok;
            r.px = (op == '+') ? (r.px + b.px) : (r.px - b.px);
            r.pct = 0;
            *pp = p;
        } else { *pp = p; break; }
    }
    return r;
}

/* Parses CSS length with full float precision & negative values.
 * Returns 1 on success (writing *out_val and *is_pct), 0 on failure. */
static int css_parse_len(const char *v, int *out_val, int *is_pct, double ref_px, double font_px) {
    *is_pct = 0;
    *out_val = 0;
    if (!v) return 0;
    const char *p = v;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return 0;
    if (strn_ci_eq(p, "calc(", 5) || strn_ci_eq(p, "min(", 4) ||
        strn_ci_eq(p, "max(", 4)  || strn_ci_eq(p, "clamp(", 6)) {
        cexpr_t e = cexpr_primary(&p, ref_px, font_px);
        if (!e.ok) return 0;
        *out_val = (int)(e.px + (e.px >= 0 ? 0.5 : -0.5));
        *is_pct = 0;
        return 1;
    }
    int neg = 0, any = 0;
    if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
    double num = 0;
    while (*p >= '0' && *p <= '9') { num = num * 10.0 + (*p - '0'); p++; any = 1; }
    if (*p == '.') { p++; double f = 0.1; while (*p >= '0' && *p <= '9') { num += (*p - '0') * f; f /= 10.0; p++; any = 1; } }
    if (!any) return 0;
    if (neg) num = -num;
    if (*p == '%') {
        p++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p) return 0;
        *is_pct = 1;
        *out_val = (int)(num + (num >= 0 ? 0.5 : -0.5));
        return 1;
    }
    int u = 0;
    if (strn_ci_eq(p, "px", 2)) { u = 0; p += 2; }
    else if (strn_ci_eq(p, "rem", 3)) { u = 3; p += 3; }
    else if (strn_ci_eq(p, "em", 2)) { u = 2; p += 2; }
    else if (strn_ci_eq(p, "pt", 2)) { u = 4; p += 2; }
    else if (strn_ci_eq(p, "vw", 2)) { u = 5; p += 2; }
    else if (strn_ci_eq(p, "vh", 2)) { u = 6; p += 2; }
    else if (strn_ci_eq(p, "vmin", 4)) { u = 7; p += 4; }
    else if (strn_ci_eq(p, "vmax", 4)) { u = 8; p += 4; }
    else if (strn_ci_eq(p, "cm", 2)) { u = 9; p += 2; }
    else if (strn_ci_eq(p, "mm", 2)) { u = 10; p += 2; }
    else if (strn_ci_eq(p, "in", 2)) { u = 11; p += 2; }
    while (*p == ' ' || *p == '\t') p++;
    if (*p) return 0;
    double px = num * css_unit_mult(u, font_px);
    *out_val = (int)(px + (px >= 0 ? 0.5 : -0.5));
    return 1;
}

static int css_len_px(const char *v, int *pct) {
    int out = 0;
    if (!css_parse_len(v, &out, pct, (double)g_cur_avail_w, (double)g_inh_font)) return -1;
    return out;
}

static int css_auto(const char *v) { return str_eq_ci(v, "auto"); }

/* Splits space-separated tokens respecting parentheses (for padding/margin/border/etc.) */
static int css_split_ws(const char *val, char toks[][96], int maxt) {
    int n = 0;
    const char *p = val;
    while (*p && n < maxt) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        int k = 0, d = 0;
        while (*p) {
            if (*p == '(') d++;
            else if (*p == ')' && d > 0) d--;
            else if (d == 0 && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) break;
            if (k < 95) toks[n][k++] = *p;
            p++;
        }
        toks[n][k] = 0;
        if (k > 0) n++;
    }
    return n;
}

/* Splits top-level comma-separated items respecting parentheses */
static int css_split_comma(const char *val, char out[][220], int maxo) {
    int n = 0, d = 0, o = 0;
    const char *p = val;
    for (;;) {
        char c = *p;
        if (c == 0 || (c == ',' && d == 0)) {
            if (n < maxo) {
                out[n][o] = 0;
                css_trim(out[n]);
                if (out[n][0]) n++;
            }
            o = 0;
            if (!c) break;
            p++;
            continue;
        }
        if (c == '(') d++;
        else if (c == ')' && d > 0) d--;
        if (n < maxo && o < 219) out[n][o++] = c;
        p++;
    }
    return n;
}

static void css_sides_ex(const char *val, int *out, int *auto_l, int *auto_r) {
    char toks[6][96];
    int nt = css_split_ws(val, toks, 6);
    int v[4] = {0,0,0,0};
    int au[4] = {0,0,0,0};
    int n = 0;
    for (int i = 0; i < nt && n < 4; i++) {
        if (css_auto(toks[i])) { v[n] = 0; au[n] = 1; n++; }
        else {
            int pc = 0, px = 0;
            if (css_parse_len(toks[i], &px, &pc, (double)g_cur_avail_w, (double)g_inh_font)) {
                v[n] = pc ? (g_cur_avail_w * px / 100) : px;
                n++;
            }
        }
    }
    if (n == 1) { for (int k = 0; k < 4; k++) out[k] = v[0]; if (auto_l && au[0]) *auto_l = 1; if (auto_r && au[0]) *auto_r = 1; }
    else if (n == 2) { out[0] = out[2] = v[0]; out[1] = out[3] = v[1]; if (auto_l && au[1]) *auto_l = 1; if (auto_r && au[1]) *auto_r = 1; }
    else if (n == 3) { out[0] = v[0]; out[1] = out[3] = v[1]; out[2] = v[2]; if (auto_l && au[1]) *auto_l = 1; if (auto_r && au[1]) *auto_r = 1; }
    else if (n == 4) { for (int k = 0; k < 4; k++) out[k] = v[k]; if (auto_r && au[1]) *auto_r = 1; if (auto_l && au[3]) *auto_l = 1; }
}

/* ---------------- CSS3 Gradient Parser ---------------- */
static int css_parse_one_gradient(const char *fn_str, css_grad_t *g) {
    g->kind = GRAD_NONE;
    g->angle = 180.0f;
    g->cx = 0.5f; g->cy = 0.5f;
    g->nstops = 0;

    const char *p = fn_str;
    while (*p == ' ' || *p == '\t') p++;
    if (strn_ci_eq(p, "linear-gradient(", 16)) { g->kind = GRAD_LINEAR; p += 16; }
    else if (strn_ci_eq(p, "repeating-linear-gradient(", 26)) { g->kind = GRAD_LINEAR; p += 26; }
    else if (strn_ci_eq(p, "radial-gradient(", 16)) { g->kind = GRAD_RADIAL; p += 16; }
    else if (strn_ci_eq(p, "conic-gradient(", 15)) { g->kind = GRAD_CONIC; g->angle = 0.0f; p += 15; }
    else return 0;

    /* Extract inside parentheses */
    char inner[320];
    int d = 1, oi = 0;
    while (*p && d > 0) {
        if (*p == '(') d++;
        else if (*p == ')') { d--; if (d == 0) break; }
        if (oi < 319) inner[oi++] = *p;
        p++;
    }
    inner[oi] = 0;

    char args[10][220];
    int na = css_split_comma(inner, args, 10);
    if (na < 2) return 0;

    int start_idx = 0;
    if (g->kind == GRAD_LINEAR) {
        const char *a0 = args[0];
        if (find_substr_ci(a0, "deg")) { g->angle = (float)css_f(a0); start_idx = 1; }
        else if (find_substr_ci(a0, "turn")) { g->angle = (float)(css_f(a0) * 360.0); start_idx = 1; }
        else if (strn_ci_eq(a0, "to ", 3)) {
            int rt = find_substr_ci(a0, "right") != 0;
            int lt = find_substr_ci(a0, "left") != 0;
            int tp = find_substr_ci(a0, "top") != 0;
            int bt = find_substr_ci(a0, "bottom") != 0;
            if (rt && bt) g->angle = 135.0f;
            else if (rt && tp) g->angle = 45.0f;
            else if (lt && bt) g->angle = 225.0f;
            else if (lt && tp) g->angle = 315.0f;
            else if (rt) g->angle = 90.0f;
            else if (lt) g->angle = 270.0f;
            else if (tp) g->angle = 0.0f;
            else g->angle = 180.0f;
            start_idx = 1;
        }
    } else if (g->kind == GRAD_RADIAL) {
        if (find_substr_ci(args[0], "circle") || find_substr_ci(args[0], "ellipse") ||
            find_substr_ci(args[0], "at ")) {
            if (find_substr_ci(args[0], "left")) g->cx = 0.0f;
            else if (find_substr_ci(args[0], "right")) g->cx = 1.0f;
            if (find_substr_ci(args[0], "top")) g->cy = 0.0f;
            else if (find_substr_ci(args[0], "bottom")) g->cy = 1.0f;
            start_idx = 1;
        }
    } else if (g->kind == GRAD_CONIC) {
        if (strn_ci_eq(args[0], "from ", 5)) {
            g->angle = (float)css_f(args[0] + 5);
            start_idx = 1;
        }
    }

    for (int i = start_idx; i < na && g->nstops < 8; i++) {
        char toks[4][96];
        int nt = css_split_ws(args[i], toks, 4);
        if (nt < 1) continue;
        uint32_t col = 0;
        if (!css_color(toks[0], &col)) continue;
        uint8_t ca = (uint8_t)g_css_alpha;
        int has_p1 = 0, has_p2 = 0;
        float p1 = 0, p2 = 0;
        if (nt >= 2) {
            if (find_substr_ci(toks[1], "%")) { p1 = (float)(css_f(toks[1]) / 100.0); has_p1 = 1; }
            else if (find_substr_ci(toks[1], "px")) { p1 = (float)(css_f(toks[1]) / 20.0); has_p1 = 1; }
            else if (find_substr_ci(toks[1], "deg")) { p1 = (float)(css_f(toks[1]) / 360.0); has_p1 = 1; }
        }
        if (nt >= 3) {
            if (find_substr_ci(toks[2], "%")) { p2 = (float)(css_f(toks[2]) / 100.0); has_p2 = 1; }
            else if (find_substr_ci(toks[2], "deg")) { p2 = (float)(css_f(toks[2]) / 360.0); has_p2 = 1; }
        }
        g->stops[g->nstops].rgb = col;
        g->stops[g->nstops].a = ca;
        g->stops[g->nstops].pos = p1;
        g->stops[g->nstops].has_pos = (uint8_t)has_p1;
        g->nstops++;
        if (has_p2 && g->nstops < 8) {
            g->stops[g->nstops].rgb = col;
            g->stops[g->nstops].a = ca;
            g->stops[g->nstops].pos = p2;
            g->stops[g->nstops].has_pos = 1;
            g->nstops++;
        }
    }
    if (g->nstops < 2) { g->kind = GRAD_NONE; return 0; }
    /* Fill unspecified stop positions */
    if (!g->stops[0].has_pos) g->stops[0].pos = 0.0f;
    if (!g->stops[g->nstops - 1].has_pos) g->stops[g->nstops - 1].pos = 1.0f;
    for (int i = 1; i < g->nstops - 1; i++) {
        if (!g->stops[i].has_pos) {
            int j = i + 1;
            while (j < g->nstops - 1 && !g->stops[j].has_pos) j++;
            float a = g->stops[i - 1].pos, b = g->stops[j].pos;
            int span = j - (i - 1);
            for (int k = i; k < j; k++)
                g->stops[k].pos = a + (b - a) * (float)(k - (i - 1)) / (float)span;
        }
    }
    return 1;
}

static void css_bg_images(css_style_t *s, const char *val) {
    char layers[4][220];
    int nl = css_split_comma(val, layers, 4);
    for (int i = 0; i < nl; i++) {
        if (find_substr_ci(layers[i], "gradient(")) {
            if (s->ngrads < 2) {
                css_grad_t g;
                if (css_parse_one_gradient(layers[i], &g)) {
                    s->grads[s->ngrads++] = g;
                    if (!s->has_grad && g.nstops >= 2) {
                        s->has_grad = 1;
                        s->grad_c = g.stops[g.nstops - 1].rgb;
                        s->grad_dir = (g.angle > 45.0f && g.angle < 135.0f) ? 2 : 1;
                    }
                }
            }
        }
        const char *u = find_substr_ci(layers[i], "url(");
        if (u) {
            u += 4;
            while (*u == ' ' || *u == '\t') u++;
            char qch = 0;
            if (*u == '\'' || *u == '"') { qch = *u++; }
            int n = 0;
            while (u[n] && n < (int)sizeof(s->bg_url) - 1) {
                if (qch) { if (u[n] == qch) break; }
                else { if (u[n] == ')') break; }
                n++;
            }
            if (n > 0) {
                for (int k = 0; k < n; k++) s->bg_url[k] = u[k];
                s->bg_url[n] = 0;
                s->has_bg_url = 1;
            }
        }
    }
}

static void css_bg(css_style_t *s, const char *val) {
    uint32_t c;
    if (str_eq_ci(val, "none")) { s->has_bg = 0; s->ngrads = 0; s->has_grad = 0; s->has_bg_url = 0; return; }
    if (str_eq_ci(val, "transparent")) { s->has_bg = 1; s->bg = 0; s->bg_a = 0; s->bg_transparent = 1; return; }
    if (css_color(val, &c)) { s->bg = c; s->bg_a = g_css_alpha; s->has_bg = 1; s->bg_transparent = (g_css_alpha == 0); return; }
    char toks[6][96];
    int nt = css_split_ws(val, toks, 6);
    for (int i = 0; i < nt; i++) {
        if (str_eq_ci(toks[i], "transparent")) { s->has_bg = 1; s->bg = 0; s->bg_a = 0; s->bg_transparent = 1; }
        else if (css_color(toks[i], &c)) { s->bg = c; s->bg_a = g_css_alpha; s->has_bg = 1; s->bg_transparent = (g_css_alpha == 0); }
    }
}

/* ---------------- CSS3 Property Setter ---------------- */
static void css_parse_border_shorthand(const char *val, int *w_out, int *st_out, uint32_t *c_out, int *a_out) {
    if (str_eq_ci(val, "none") || str_eq_ci(val, "0") || find_substr_ci(val, "hidden")) {
        *w_out = 0; *st_out = 1; return;
    }
    char toks[6][96];
    int nt = css_split_ws(val, toks, 6);
    int saw_w = 0;
    *st_out = 0;
    for (int i = 0; i < nt; i++) {
        if (str_eq_ci(toks[i], "dashed")) { *st_out = 2; continue; }
        if (str_eq_ci(toks[i], "dotted")) { *st_out = 3; continue; }
        if (str_eq_ci(toks[i], "solid"))  { *st_out = 0; continue; }
        if (str_eq_ci(toks[i], "none") || str_eq_ci(toks[i], "hidden")) { *w_out = 0; *st_out = 1; return; }
        int px = 0, pc = 0;
        uint32_t col = 0;
        if ((toks[i][0] >= '0' && toks[i][0] <= '9') &&
            css_parse_len(toks[i], &px, &pc, (double)g_cur_avail_w, (double)g_inh_font)) {
            *w_out = px; saw_w = 1;
        } else if (css_color(toks[i], &col)) {
            *c_out = col; *a_out = g_css_alpha;
        }
    }
    if (!saw_w && *w_out == 0) *w_out = 1;
}

static void css_parse_radius_list(const char *s, int out[4], uint8_t pct[4]) {
    char toks[6][96];
    int nt = css_split_ws(s, toks, 6);
    int v[4] = {0,0,0,0}; uint8_t p[4] = {0,0,0,0};
    int k = 0;
    for (int i = 0; i < nt && k < 4; i++) {
        int val = 0, is_p = 0;
        if (css_parse_len(toks[i], &val, &is_p, (double)g_cur_avail_w, (double)g_inh_font)) {
            v[k] = val; p[k] = (uint8_t)is_p; k++;
        }
    }
    if (k == 1) { for (int i = 0; i < 4; i++) { out[i] = v[0]; pct[i] = p[0]; } }
    else if (k == 2) { out[0]=out[2]=v[0]; pct[0]=pct[2]=p[0]; out[1]=out[3]=v[1]; pct[1]=pct[3]=p[1]; }
    else if (k == 3) { out[0]=v[0]; pct[0]=p[0]; out[1]=out[3]=v[1]; pct[1]=pct[3]=p[1]; out[2]=v[2]; pct[2]=p[2]; }
    else if (k == 4) { for (int i = 0; i < 4; i++) { out[i] = v[i]; pct[i] = p[i]; } }
}

static void css_parse_filter(css_style_t *s, const char *val) {
    if (str_eq_ci(val, "none")) { s->has_filter = 0; return; }
    const char *p = val;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char fn[24]; int k = 0;
        while (*p && *p != '(' && *p != ' ' && k < 23) fn[k++] = lower_c(*p++);
        fn[k] = 0;
        if (*p != '(') { while (*p && *p != ' ') p++; continue; }
        p++;
        char ar[120]; int ai = 0, d = 1;
        while (*p && d > 0) {
            if (*p == '(') d++;
            else if (*p == ')') { d--; if (d == 0) { p++; break; } }
            if (ai < 119) ar[ai++] = *p;
            p++;
        }
        ar[ai] = 0;
        css_trim(ar);
        s->has_filter = 1;
        if (str_eq(fn, "blur")) s->flt_blur = (float)css_f(ar);
        else if (str_eq(fn, "hue-rotate")) s->flt_hue = (float)css_f(ar);
        else if (str_eq(fn, "contrast")) { double v = css_f(ar); s->flt_contrast = (float)(find_substr_ci(ar, "%") ? v / 100.0 : v); }
        else if (str_eq(fn, "brightness")) { double v = css_f(ar); s->flt_bright = (float)(find_substr_ci(ar, "%") ? v / 100.0 : v); }
        else if (str_eq(fn, "saturate")) { double v = css_f(ar); s->flt_sat = (float)(find_substr_ci(ar, "%") ? v / 100.0 : v); }
        else if (str_eq(fn, "grayscale")) { double v = css_f(ar); s->flt_gray = (float)(find_substr_ci(ar, "%") ? v / 100.0 : v); }
        else if (str_eq(fn, "invert")) { double v = css_f(ar); s->flt_invert = (float)(find_substr_ci(ar, "%") ? v / 100.0 : v); }
        else if (str_eq(fn, "sepia")) { double v = css_f(ar); s->flt_sepia = (float)(find_substr_ci(ar, "%") ? v / 100.0 : v); }
        else if (str_eq(fn, "drop-shadow")) {
            char toks[6][96];
            int nt = css_split_ws(ar, toks, 6);
            int nums[3] = {0,0,0}, nn = 0;
            for (int i = 0; i < nt; i++) {
                uint32_t col = 0; int px = 0, pc = 0;
                if ((toks[i][0]=='-'||(toks[i][0]>='0'&&toks[i][0]<='9')) &&
                    css_parse_len(toks[i], &px, &pc, 100, 16) && nn < 3) nums[nn++] = px;
                else if (css_color(toks[i], &col)) { s->flt_dsh_c = col; s->flt_dsh_a = (uint8_t)g_css_alpha; }
            }
            s->flt_dsh_x = nums[0]; s->flt_dsh_y = nums[1]; s->flt_dsh_b = nums[2];
        }
    }
}

static void css_parse_clip_path(css_style_t *s, const char *val) {
    const char *p = find_substr_ci(val, "polygon(");
    if (!p) { if (str_eq_ci(val, "none")) s->clip_poly.n = 0; return; }
    p += 8;
    char inner[220]; int oi = 0, d = 1;
    while (*p && d > 0) {
        if (*p == '(') d++;
        else if (*p == ')') { d--; if (d == 0) break; }
        if (oi < 219) inner[oi++] = *p;
        p++;
    }
    inner[oi] = 0;
    char pts[12][220];
    int np = css_split_comma(inner, pts, 12);
    s->clip_poly.n = 0;
    for (int i = 0; i < np && s->clip_poly.n < 12; i++) {
        char toks[3][96];
        int nt = css_split_ws(pts[i], toks, 3);
        if (nt < 2) continue;
        int idx = s->clip_poly.n++;
        s->clip_poly.x[idx] = (float)css_f(toks[0]);
        s->clip_poly.xp[idx] = (uint8_t)(find_substr_ci(toks[0], "%") != 0);
        s->clip_poly.y[idx] = (float)css_f(toks[1]);
        s->clip_poly.yp[idx] = (uint8_t)(find_substr_ci(toks[1], "%") != 0);
    }
}

static void css_set(css_style_t *s, const char *prop, const char *val) {
    uint32_t c;
    int pc = 0, px = 0;
    if (str_eq(prop, "color")) {
        if (str_eq_ci(val, "inherit")) { s->color = g_inh_color; s->color_a = 255; s->has_color = 1; }
        else if (css_color(val, &c)) { s->color = c; s->color_a = g_css_alpha; s->has_color = 1; }
    }
    else if (str_eq(prop, "-webkit-text-fill-color")) {
        if (str_eq_ci(val, "transparent")) s->text_fill_transp = 1;
        else if (css_color(val, &c)) { s->color = c; s->color_a = g_css_alpha; s->has_color = 1; s->text_fill_transp = 0; }
    }
    else if (str_eq(prop, "background-clip") || str_eq(prop, "-webkit-background-clip")) {
        s->bg_clip_text = str_eq_ci(val, "text") ? 1 : 0;
    }
    else if (str_eq(prop, "background-color")) css_bg(s, val);
    else if (str_eq(prop, "background")) {
        if (str_eq_ci(val, "none")) { s->has_bg = 0; s->ngrads = 0; s->has_grad = 0; s->has_bg_url = 0; }
        else {
            s->ngrads = 0; s->has_grad = 0; s->has_bg_url = 0;
            if (find_substr_ci(val, "gradient(") || find_substr_ci(val, "url("))
                css_bg_images(s, val);
            else
                css_bg(s, val);
        }
    }
    else if (str_eq(prop, "background-image")) {
        if (str_eq_ci(val, "none")) { s->ngrads = 0; s->has_grad = 0; s->has_bg_url = 0; }
        else { s->ngrads = 0; css_bg_images(s, val); }
    }
    else if (str_eq(prop, "font-family")) {
        s->mono = (find_substr_ci(val, "monospace") != 0 || find_substr_ci(val, "courier") != 0);
    }
    else if (str_eq(prop, "font-size")) {
        s->has_font = 1;
        if (str_eq_ci(val, "xx-small")) s->font_size = 9;
        else if (str_eq_ci(val, "x-small")) s->font_size = 10;
        else if (str_eq_ci(val, "small")) s->font_size = 12;
        else if (str_eq_ci(val, "medium")) s->font_size = 16;
        else if (str_eq_ci(val, "large")) s->font_size = 18;
        else if (str_eq_ci(val, "x-large")) s->font_size = 24;
        else if (str_eq_ci(val, "xx-large")) s->font_size = 32;
        else if (str_eq_ci(val, "smaller")) s->font_size = g_inh_font * 5 / 6;
        else if (str_eq_ci(val, "larger")) s->font_size = g_inh_font * 6 / 5;
        else if (css_parse_len(val, &px, &pc, (double)g_inh_font, (double)g_inh_font)) {
            if (pc) s->font_size = g_inh_font * px / 100;
            else if (px > 0) s->font_size = px;
        }
    }
    else if (str_eq(prop, "font-weight")) {
        if (str_eq_ci(val, "bold") || str_eq_ci(val, "bolder")) s->bold = 1;
        else if (str_eq_ci(val, "normal") || str_eq_ci(val, "lighter")) s->bold = 0;
        else { int n = css_num(val, &pc); s->bold = (n >= 600); }
    }
    else if (str_eq(prop, "font-style")) {
        if (str_eq_ci(val, "italic") || str_eq_ci(val, "oblique")) s->italic = 1;
        else if (str_eq_ci(val, "normal")) s->italic = 0;
    }
    else if (str_eq(prop, "text-decoration") || str_eq(prop, "text-decoration-line")) {
        if (str_eq_ci(val, "none")) { s->underline = 0; s->strike = 0; }
        else {
            if (find_substr_ci(val, "underline")) s->underline = 1;
            if (find_substr_ci(val, "line-through")) s->strike = 1;
        }
    }
    else if (str_eq(prop, "text-align")) {
        if (str_eq_ci(val, "center")) s->align = 1;
        else if (str_eq_ci(val, "right") || str_eq_ci(val, "end")) s->align = 2;
        else if (str_eq_ci(val, "justify")) s->align = 3;
        else s->align = 0;
    }
    else if (str_eq(prop, "line-height")) {
        if (!str_eq_ci(val, "normal")) {
            double f = css_f(val);
            if (find_substr_ci(val, "%")) s->line_height = (int)(s->font_size * f / 100.0 + 0.5);
            else if (find_substr_ci(val, "px") || find_substr_ci(val, "rem") || find_substr_ci(val, "em")) {
                if (css_parse_len(val, &px, &pc, (double)s->font_size, (double)s->font_size) && px > 0)
                    s->line_height = px;
            } else if (f > 0.2 && f < 8.0) {
                s->line_height = (int)(s->font_size * f + 0.5);
            }
        }
    }
    else if (str_eq(prop, "letter-spacing")) {
        double fv = css_f(val);
        if (fv > -10.0 && fv < 40.0) s->letter_space = (int)fv; /* truncate <1px subpixel values */
    }
    else if (str_eq(prop, "border-radius")) {
        if (str_eq_ci(val, "inherit")) { s->radius_inherit = 1; return; }
        s->radius_inherit = 0;
        const char *slash = 0;
        for (const char *q = val; *q; q++) if (*q == '/') { slash = q; break; }
        if (slash) {
            char a[96], b[96];
            int al = (int)(slash - val);
            if (al > 95) al = 95;
            for (int i = 0; i < al; i++) a[i] = val[i];
            a[al] = 0;
            bcopy_str(b, slash + 1, sizeof(b));
            css_parse_radius_list(a, s->rad_rx, s->rad_rx_p);
            css_parse_radius_list(b, s->rad_ry, s->rad_ry_p);
            s->has_corner_radius = 1;
            s->radius = s->rad_rx[0];
            s->radius_pct = s->rad_rx_p[0];
        } else {
            char toks[6][96];
            int nt = css_split_ws(val, toks, 6);
            if (nt == 1) {
                int v = 0, ip = 0;
                if (css_parse_len(toks[0], &v, &ip, (double)g_cur_avail_w, (double)g_inh_font)) {
                    s->radius = v; s->radius_pct = ip;
                    s->has_corner_radius = 0;
                    for (int i = 0; i < 4; i++) { s->rad_rx[i]=s->rad_ry[i]=v; s->rad_rx_p[i]=s->rad_ry_p[i]=(uint8_t)ip; }
                }
            } else if (nt >= 2) {
                css_parse_radius_list(val, s->rad_rx, s->rad_rx_p);
                for (int i = 0; i < 4; i++) { s->rad_ry[i] = s->rad_rx[i]; s->rad_ry_p[i] = s->rad_rx_p[i]; }
                s->has_corner_radius = 1;
                s->radius = s->rad_rx[0];
            }
        }
    }
    else if (str_eq(prop, "border")) {
        int bw = 0, bs = 0, ba = 255; uint32_t bc = s->border_c;
        css_parse_border_shorthand(val, &bw, &bs, &bc, &ba);
        s->border_w = bw; s->border_style = bs; s->border_c = bc; s->border_a = ba;
        s->border_sides = 15;
        for (int i = 0; i < 4; i++) { s->side_bw[i] = bw; s->side_bc[i] = bc; s->side_ba[i] = ba; s->side_bs[i] = bs; }
    }
    else if (str_eq(prop, "border-top") || str_eq(prop, "border-right") ||
             str_eq(prop, "border-bottom") || str_eq(prop, "border-left")) {
        int idx = str_eq(prop, "border-top") ? 0 : str_eq(prop, "border-right") ? 1 :
                  str_eq(prop, "border-bottom") ? 2 : 3;
        int bw = 0, bs = 0, ba = 255; uint32_t bc = s->border_c;
        css_parse_border_shorthand(val, &bw, &bs, &bc, &ba);
        s->side_bw[idx] = bw; s->side_bc[idx] = bc; s->side_ba[idx] = ba; s->side_bs[idx] = bs;
        if (bw > 0) {
            s->border_sides |= (1 << idx);
            s->border_w = bw; s->border_c = bc; s->border_a = ba; s->border_style = bs;
        }
    }
    else if (str_eq(prop, "border-width")) {
        if (css_parse_len(val, &px, &pc, (double)g_cur_avail_w, (double)g_inh_font) && px >= 0) {
            s->border_w = px; s->border_sides = 15;
            for (int i = 0; i < 4; i++) s->side_bw[i] = px;
        }
    }
    else if (str_eq(prop, "border-color")) {
        if (css_color(val, &c)) {
            s->border_c = c; s->border_a = g_css_alpha;
            for (int i = 0; i < 4; i++) { s->side_bc[i] = c; s->side_ba[i] = g_css_alpha; }
        }
    }
    else if (str_eq(prop, "border-style")) {
        if (str_eq_ci(val, "none") || str_eq_ci(val, "hidden")) { s->border_w = 0; s->border_style = 1; }
        else if (str_eq_ci(val, "dashed")) { s->border_style = 2; if (s->border_w == 0) s->border_w = 1; }
        else if (str_eq_ci(val, "dotted")) { s->border_style = 3; if (s->border_w == 0) s->border_w = 1; }
        else { s->border_style = 0; if (s->border_w == 0) s->border_w = 1; }
    }
    else if (str_eq(prop, "box-shadow")) {
        s->nshadows = 0;
        if (!str_eq_ci(val, "none")) {
            char parts[4][220];
            int np = css_split_comma(val, parts, 4);
            for (int pi = 0; pi < np && s->nshadows < 2; pi++) {
                char toks[8][96];
                int nt = css_split_ws(parts[pi], toks, 8);
                int nums[4] = {0,0,0,0}, nn = 0, inset = 0;
                uint32_t sc = 0x000000; int sa = 100;
                for (int i = 0; i < nt; i++) {
                    if (str_eq_ci(toks[i], "inset")) { inset = 1; continue; }
                    int lv = 0, lp = 0; uint32_t col = 0;
                    if ((toks[i][0]=='-'||toks[i][0]=='+'||(toks[i][0]>='0'&&toks[i][0]<='9')||toks[i][0]=='.') &&
                        css_parse_len(toks[i], &lv, &lp, 100, 16)) {
                        if (nn < 4) nums[nn++] = lv;
                    } else if (css_color(toks[i], &col)) {
                        sc = col; sa = g_css_alpha;
                    }
                }
                if (nn >= 2) {
                    css_shadow_t *sh = &s->shadows[s->nshadows++];
                    sh->x = nums[0]; sh->y = nums[1];
                    sh->blur = nn > 2 ? nums[2] : 0;
                    sh->spread = nn > 3 ? nums[3] : 0;
                    sh->rgb = sc; sh->a = (uint8_t)sa; sh->inset = (uint8_t)inset;
                }
            }
        }
    }
    else if (str_eq(prop, "outline")) {
        if (str_eq_ci(val, "none") || str_eq_ci(val, "0")) { s->has_outline = 0; s->outline_w = 0; }
        else {
            int bw = 0, bs = 0, ba = 255; uint32_t bc = s->outline_c;
            css_parse_border_shorthand(val, &bw, &bs, &bc, &ba);
            if (bw > 0) { s->has_outline = 1; s->outline_w = bw; s->outline_c = bc; s->outline_a = ba; }
        }
    }
    else if (str_eq(prop, "outline-offset")) {
        if (css_parse_len(val, &px, &pc, 100, (double)g_inh_font)) s->outline_offset = px;
    }
    else if (str_eq(prop, "display")) {
        if (str_eq_ci(val, "none")) { s->display = 7; s->hide = 1; s->is_flex = 0; s->is_grid = 0; }
        else if (str_eq_ci(val, "flex")) { s->display = 3; s->is_flex = 1; s->is_grid = 0; s->hide = 0; if (!s->has_flexdir) s->row = 1; }
        else if (str_eq_ci(val, "inline-flex")) { s->display = 4; s->is_flex = 1; s->is_grid = 0; s->hide = 0; if (!s->has_flexdir) s->row = 1; }
        else if (str_eq_ci(val, "grid")) { s->display = 5; s->is_grid = 1; s->is_flex = 0; s->hide = 0; }
        else if (str_eq_ci(val, "inline-grid")) { s->display = 6; s->is_grid = 1; s->is_flex = 0; s->hide = 0; }
        else if (str_eq_ci(val, "inline")) { s->display = 1; s->is_flex = 0; s->is_grid = 0; s->hide = 0; }
        else if (str_eq_ci(val, "inline-block")) { s->display = 2; s->is_flex = 0; s->is_grid = 0; s->hide = 0; }
        else if (str_eq_ci(val, "contents")) { s->display = 8; s->is_flex = 0; s->is_grid = 0; s->hide = 0; }
        else { s->display = 0; s->is_flex = 0; s->is_grid = 0; s->hide = 0; }
    }
    else if (str_eq(prop, "visibility")) {
        if (str_eq_ci(val, "hidden") || str_eq_ci(val, "collapse")) s->vis_hidden = 1;
        else if (str_eq_ci(val, "visible")) s->vis_hidden = 0;
    }
    else if (str_eq(prop, "clip")) { if (find_substr_ci(val, "rect(0")) s->sr_clip = 1; }
    else if (str_eq(prop, "opacity")) {
        double f = css_f(val);
        if (find_substr_ci(val, "%")) s->opacity = (int)(f + 0.5);
        else s->opacity = (int)(f * 100.0 + 0.5);
        if (s->opacity < 0) s->opacity = 0;
        if (s->opacity > 100) s->opacity = 100;
        s->has_opacity = 1;
    }
    else if (str_eq(prop, "float")) {
        if (str_eq_ci(val, "left")) s->float_dir = 1;
        else if (str_eq_ci(val, "right")) s->float_dir = 2;
        else s->float_dir = 0;
    }
    else if (str_eq(prop, "flex-direction")) {
        s->row = (str_eq_ci(val, "row") || str_eq_ci(val, "row-reverse"));
        s->has_flexdir = 1;
    }
    else if (str_eq(prop, "flex-wrap")) s->wrap = (str_eq_ci(val, "wrap") || str_eq_ci(val, "wrap-reverse"));
    else if (str_eq(prop, "flex-flow")) {
        s->row = (find_substr_ci(val, "row") != 0);
        s->wrap = (find_substr_ci(val, "wrap") != 0) && !find_substr_ci(val, "nowrap");
        s->has_flexdir = 1;
    }
    else if (str_eq(prop, "justify-content")) {
        if (str_eq_ci(val, "center")) s->justify = 1;
        else if (str_eq_ci(val, "flex-end") || str_eq_ci(val, "end") || str_eq_ci(val, "right")) s->justify = 2;
        else if (str_eq_ci(val, "space-between")) s->justify = 3;
        else if (str_eq_ci(val, "space-around") || str_eq_ci(val, "space-evenly")) s->justify = 4;
        else s->justify = 0;
    }
    else if (str_eq(prop, "align-items")) {
        if (str_eq_ci(val, "center")) s->align_items = 1;
        else if (str_eq_ci(val, "flex-end") || str_eq_ci(val, "end")) s->align_items = 2;
        else if (str_eq_ci(val, "stretch")) s->align_items = 3;
        else s->align_items = 0;
    }
    else if (str_eq(prop, "justify-items")) {
        if (str_eq_ci(val, "center")) s->justify_items = 1;
        else if (str_eq_ci(val, "flex-end") || str_eq_ci(val, "end") || str_eq_ci(val, "right")) s->justify_items = 2;
        else if (str_eq_ci(val, "stretch")) s->justify_items = 3;
        else s->justify_items = 0;
    }
    else if (str_eq(prop, "align-self")) {
        if (str_eq_ci(val, "center")) s->align_self = 1;
        else if (str_eq_ci(val, "flex-end") || str_eq_ci(val, "end")) s->align_self = 2;
        else s->align_self = 0;
    }
    else if (str_eq(prop, "align-content")) {
        if (str_eq_ci(val, "center")) s->align_content = 1;
        else if (str_eq_ci(val, "flex-end") || str_eq_ci(val, "end")) s->align_content = 2;
        else if (str_eq_ci(val, "space-between")) s->align_content = 3;
        else if (str_eq_ci(val, "space-around") || str_eq_ci(val, "space-evenly")) s->align_content = 4;
        else s->align_content = 0;
    }
    else if (str_eq(prop, "gap") || str_eq(prop, "grid-gap")) {
        char toks[3][96]; int nt = css_split_ws(val, toks, 3);
        if (nt >= 1 && css_parse_len(toks[0], &px, &pc, (double)g_cur_avail_w, (double)g_inh_font) && px >= 0) {
            s->gap = px; s->row_gap = px; s->col_gap = px;
        }
        if (nt >= 2 && css_parse_len(toks[1], &px, &pc, (double)g_cur_avail_w, (double)g_inh_font) && px >= 0) {
            s->col_gap = px;
        }
    }
    else if (str_eq(prop, "row-gap")) {
        if (css_parse_len(val, &px, &pc, (double)g_cur_avail_w, (double)g_inh_font) && px >= 0) s->row_gap = px;
    }
    else if (str_eq(prop, "column-gap")) {
        if (css_parse_len(val, &px, &pc, (double)g_cur_avail_w, (double)g_inh_font) && px >= 0) s->col_gap = px;
    }
    else if (str_eq(prop, "flex-grow")) s->flex_grow = css_num(val, &pc);
    else if (str_eq(prop, "flex")) {
        if (str_eq_ci(val, "none")) s->flex_grow = 0;
        else if (str_eq_ci(val, "initial") || str_eq_ci(val, "auto")) s->flex_grow = 1;
        else {
            char toks[3][96]; int nt = css_split_ws(val, toks, 3);
            if (nt == 1 && (find_substr_ci(toks[0], "%") || find_substr_ci(toks[0], "px") ||
                            find_substr_ci(toks[0], "rem") || find_substr_ci(toks[0], "em"))) {
                /* CSS spec: flex: 100% / <length-percentage> -> flex-grow: 1 */
                s->flex_grow = 1;
            } else {
                int n = (int)css_f(val);
                if (n >= 0) s->flex_grow = n;
            }
        }
    }
    else if (str_eq(prop, "flex-basis")) {
        if (str_eq_ci(val, "auto") || str_eq_ci(val, "content")) s->has_flex_basis = 0;
        else if (css_parse_len(val, &px, &pc, (double)g_cur_avail_w, (double)g_inh_font) && px >= 0) {
            s->has_flex_basis = 1; s->flex_basis = pc ? (g_cur_avail_w * px / 100) : px;
        }
    }
    else if (str_eq(prop, "order")) { s->has_order = 1; s->order = (int)css_f(val); }
    else if (str_eq(prop, "padding")) { s->has_pad = 1; css_sides_ex(val, s->pad, 0, 0); }
    else if (str_eq(prop, "padding-inline")) {
        int t[4] = {0}; css_sides_ex(val, t, 0, 0);
        s->has_pad = 1; s->pad[3] = t[0]; s->pad[1] = t[1];
    }
    else if (str_eq(prop, "padding-block")) {
        int t[4] = {0}; css_sides_ex(val, t, 0, 0);
        s->has_pad = 1; s->pad[0] = t[0]; s->pad[2] = t[1];
    }
    else if (str_eq(prop, "padding-top") || str_eq(prop, "padding-block-start"))    { if (css_parse_len(val, &px, &pc, (double)g_cur_avail_w, (double)g_inh_font)) { s->has_pad = 1; s->pad[0] = px < 0 ? 0 : px; } }
    else if (str_eq(prop, "padding-right") || str_eq(prop, "padding-inline-end"))  { if (css_parse_len(val, &px, &pc, (double)g_cur_avail_w, (double)g_inh_font)) { s->has_pad = 1; s->pad[1] = px < 0 ? 0 : px; } }
    else if (str_eq(prop, "padding-bottom") || str_eq(prop, "padding-block-end"))  { if (css_parse_len(val, &px, &pc, (double)g_cur_avail_w, (double)g_inh_font)) { s->has_pad = 1; s->pad[2] = px < 0 ? 0 : px; } }
    else if (str_eq(prop, "padding-left") || str_eq(prop, "padding-inline-start")) { if (css_parse_len(val, &px, &pc, (double)g_cur_avail_w, (double)g_inh_font)) { s->has_pad = 1; s->pad[3] = px < 0 ? 0 : px; } }
    else if (str_eq(prop, "margin")) { s->has_mar = 1; css_sides_ex(val, s->mar, &s->mar_auto_l, &s->mar_auto_r); }
    else if (str_eq(prop, "margin-inline")) {
        s->has_mar = 1;
        char toks[3][96]; int nt = css_split_ws(val, toks, 3);
        if (nt >= 1) {
            if (css_auto(toks[0])) { s->mar_auto_l = 1; s->mar[3] = 0; }
            else if (css_parse_len(toks[0], &px, &pc, (double)g_cur_avail_w, (double)g_inh_font)) s->mar[3] = px;
            if (nt == 1) { s->mar_auto_r = s->mar_auto_l; s->mar[1] = s->mar[3]; }
            else {
                if (css_auto(toks[1])) { s->mar_auto_r = 1; s->mar[1] = 0; }
                else if (css_parse_len(toks[1], &px, &pc, (double)g_cur_avail_w, (double)g_inh_font)) s->mar[1] = px;
            }
        }
    }
    else if (str_eq(prop, "margin-block")) {
        s->has_mar = 1;
        int t[4] = {0}; css_sides_ex(val, t, 0, 0);
        s->mar[0] = t[0]; s->mar[2] = t[1];
    }
    else if (str_eq(prop, "margin-top") || str_eq(prop, "margin-block-start"))    { s->has_mar = 1; if (css_parse_len(val, &px, &pc, (double)g_cur_avail_w, (double)g_inh_font)) s->mar[0] = px; }
    else if (str_eq(prop, "margin-right") || str_eq(prop, "margin-inline-end"))   { s->has_mar = 1; if (css_auto(val)) s->mar_auto_r = 1; else if (css_parse_len(val, &px, &pc, (double)g_cur_avail_w, (double)g_inh_font)) s->mar[1] = px; }
    else if (str_eq(prop, "margin-bottom") || str_eq(prop, "margin-block-end"))   { s->has_mar = 1; if (css_parse_len(val, &px, &pc, (double)g_cur_avail_w, (double)g_inh_font)) s->mar[2] = px; }
    else if (str_eq(prop, "margin-left") || str_eq(prop, "margin-inline-start"))  { s->has_mar = 1; if (css_auto(val)) s->mar_auto_l = 1; else if (css_parse_len(val, &px, &pc, (double)g_cur_avail_w, (double)g_inh_font)) s->mar[3] = px; }
    else if (str_eq(prop, "width") || str_eq(prop, "inline-size")) {
        if (css_auto(val) || str_eq_ci(val, "fit-content") || str_eq_ci(val, "max-content") || str_eq_ci(val, "min-content")) s->has_w = 0;
        else if (css_parse_len(val, &px, &pc, (double)g_cur_avail_w, (double)g_inh_font) && px >= 0) {
            s->has_w = 1; s->width = px; s->w_pct = pc;
        }
    }
    else if (str_eq(prop, "height") || str_eq(prop, "block-size")) {
        if (css_auto(val) || str_eq_ci(val, "fit-content") || str_eq_ci(val, "max-content") || str_eq_ci(val, "min-content")) s->has_h = 0;
        else if (css_parse_len(val, &px, &pc, (double)g_view_h, (double)g_inh_font) && px >= 0) {
            s->has_h = 1; s->height = px; s->h_pct = pc;
        }
    }
    else if (str_eq(prop, "min-width") || str_eq(prop, "min-inline-size"))  { if (!css_auto(val) && css_parse_len(val, &px, &pc, (double)g_cur_avail_w, (double)g_inh_font) && px >= 0) { s->has_minw = 1; s->minw = px; s->minw_pct = pc; } }
    else if (str_eq(prop, "max-width") || str_eq(prop, "max-inline-size"))  { if (!css_auto(val) && !str_eq_ci(val, "none") && css_parse_len(val, &px, &pc, (double)g_cur_avail_w, (double)g_inh_font) && px >= 0) { s->has_maxw = 1; s->maxw = px; s->maxw_pct = pc; } }
    else if (str_eq(prop, "min-height") || str_eq(prop, "min-block-size"))  { if (!css_auto(val) && css_parse_len(val, &px, &pc, (double)g_view_h, (double)g_inh_font) && px >= 0) { s->has_minh = 1; s->minh = px; s->minh_pct = pc; } }
    else if (str_eq(prop, "max-height") || str_eq(prop, "max-block-size"))  { if (!css_auto(val) && !str_eq_ci(val, "none") && css_parse_len(val, &px, &pc, (double)g_view_h, (double)g_inh_font) && px >= 0) { s->has_maxh = 1; s->maxh = px; s->maxh_pct = pc; } }
    else if (str_eq(prop, "position")) {
        if (str_eq_ci(val, "relative")) s->position = 1;
        else if (str_eq_ci(val, "absolute")) s->position = 2;
        else if (str_eq_ci(val, "fixed")) s->position = 3;
        else if (str_eq_ci(val, "sticky")) s->position = 4;
        else s->position = 0;
    }
    else if (str_eq(prop, "z-index")) { if (!css_auto(val)) { s->has_z = 1; s->z_index = (int)css_f(val); } }
    else if (str_eq(prop, "pointer-events")) { s->pointer_none = str_eq_ci(val, "none") ? 1 : 0; }
    else if (str_eq(prop, "inset")) {
        int t[4] = {0}; css_sides_ex(val, t, 0, 0);
        s->has_top = s->has_right = s->has_bottom = s->has_left = 1;
        s->top = t[0]; s->right = t[1]; s->bottom = t[2]; s->left = t[3];
    }
    else if (str_eq(prop, "top"))    { if (css_parse_len(val, &px, &pc, (double)g_view_h, (double)g_inh_font)) { s->has_top = 1; s->top = px; s->top_pct = pc; } }
    else if (str_eq(prop, "left"))   { if (css_parse_len(val, &px, &pc, (double)g_cur_avail_w, (double)g_inh_font)) { s->has_left = 1; s->left = px; s->left_pct = pc; } }
    else if (str_eq(prop, "right"))  { if (css_parse_len(val, &px, &pc, (double)g_cur_avail_w, (double)g_inh_font)) { s->has_right = 1; s->right = px; s->right_pct = pc; } }
    else if (str_eq(prop, "bottom")) { if (css_parse_len(val, &px, &pc, (double)g_view_h, (double)g_inh_font)) { s->has_bottom = 1; s->bottom = px; s->bottom_pct = pc; } }
    else if (str_eq(prop, "overflow") || str_eq(prop, "overflow-x") || str_eq(prop, "overflow-y")) {
        s->overflow_hidden = (str_eq_ci(val, "hidden") || str_eq_ci(val, "clip") || str_eq_ci(val, "auto") || str_eq_ci(val, "scroll"));
    }
    else if (str_eq(prop, "white-space")) s->nowrap = (str_eq_ci(val, "nowrap") || str_eq_ci(val, "pre") || str_eq_ci(val, "pre-wrap"));
    else if (str_eq(prop, "text-transform")) {
        if (str_eq_ci(val, "uppercase")) s->transform = 1;
        else if (str_eq_ci(val, "lowercase")) s->transform = 2;
        else if (str_eq_ci(val, "capitalize")) s->transform = 3;
        else s->transform = 0;
    }
    else if (str_eq(prop, "writing-mode")) {
        s->writing_vert = (find_substr_ci(val, "vertical") != 0);
    }
    else if (str_eq(prop, "list-style-type") || str_eq(prop, "list-style")) {
        s->has_list = 1;
        if (find_substr_ci(val, "none")) s->list_type = 0;
        else if (find_substr_ci(val, "square")) s->list_type = 1;
        else if (find_substr_ci(val, "circle")) s->list_type = 2;
        else if (find_substr_ci(val, "disc")) s->list_type = 3;
        else if (find_substr_ci(val, "decimal")) s->list_type = 4;
    }
    else if (str_eq(prop, "text-indent")) { if (css_parse_len(val, &px, &pc, (double)g_cur_avail_w, (double)g_inh_font) && px >= 0) s->text_indent = px; }
    else if (str_eq(prop, "text-overflow")) { if (find_substr_ci(val, "ellipsis")) s->ellipsis = 1; }
    else if (str_eq(prop, "place-items") || str_eq(prop, "place-content")) {
        if (find_substr_ci(val, "center")) { s->align_items = 1; s->justify = 1; }
        else if (find_substr_ci(val, "end")) { s->align_items = 2; s->justify = 2; }
        else if (find_substr_ci(val, "between")) s->justify = 3;
    }
    else if (str_eq(prop, "place-self")) {
        if (str_eq_ci(val, "center")) s->align_self = 1;
        else if (str_eq_ci(val, "end")) s->align_self = 2;
    }
    else if (str_eq(prop, "object-fit")) {
        if (str_eq_ci(val, "contain")) s->object_fit = 1;
        else if (str_eq_ci(val, "cover")) s->object_fit = 2;
        else if (str_eq_ci(val, "none")) s->object_fit = 3;
        else s->object_fit = 0;
    }
    else if (str_eq(prop, "aspect-ratio")) {
        int a = 0, b = 0;
        const char *p = val;
        while (*p == ' ') p++;
        while (*p >= '0' && *p <= '9') { a = a * 10 + (*p - '0'); p++; }
        while (*p && *p != '/') p++;
        if (*p == '/') { p++; while (*p == ' ') p++; while (*p >= '0' && *p <= '9') { b = b * 10 + (*p - '0'); p++; } }
        if (a > 0) { s->has_aspect = 1; s->aspect = b > 0 ? a * 1000 / b : a * 1000; }
    }
    else if (str_eq(prop, "grid-template-columns")) {
        s->grid_cols = 0; s->gtrack_n = 0; s->grid_auto_fit = 0; s->grid_min_w = 0;
        if (find_substr_ci(val, "auto-fit") || find_substr_ci(val, "auto-fill")) {
            s->grid_auto_fit = 1;
            const char *mm = find_substr_ci(val, "minmax(");
            if (mm) {
                mm += 7;
                int mw = (int)css_f(mm);
                if (mw > 40) s->grid_min_w = mw;
            }
            if (s->grid_min_w <= 0) s->grid_min_w = 280;
        } else if (find_substr_ci(val, "repeat(")) {
            const char *p = find_substr_ci(val, "repeat(") + 7;
            while (*p == ' ') p++;
            int cols = (int)css_f(p);
            if (cols > 0 && cols <= 16) {
                s->grid_cols = cols;
                s->gtrack_n = cols;
                for (int i = 0; i < cols; i++) { s->gtracks[i].type = 0; s->gtracks[i].val = 1; }
            }
        } else {
            char toks[16][96];
            int nt = css_split_ws(val, toks, 16);
            for (int i = 0; i < nt && s->gtrack_n < 16; i++) {
                if (find_substr_ci(toks[i], "fr")) {
                    int fv = (int)css_f(toks[i]);
                    s->gtracks[s->gtrack_n].type = 0;
                    s->gtracks[s->gtrack_n].val = fv > 0 ? fv : 1;
                    s->gtrack_n++;
                } else if (str_eq_ci(toks[i], "auto")) {
                    s->gtracks[s->gtrack_n].type = 2;
                    s->gtracks[s->gtrack_n].val = 0;
                    s->gtrack_n++;
                } else if (css_parse_len(toks[i], &px, &pc, (double)g_cur_avail_w, (double)g_inh_font) && px > 0) {
                    s->gtracks[s->gtrack_n].type = 1;
                    s->gtracks[s->gtrack_n].val = pc ? (g_cur_avail_w * px / 100) : px;
                    s->gtrack_n++;
                }
            }
            s->grid_cols = s->gtrack_n;
        }
    }
    else if (str_eq(prop, "grid-column")) {
        const char *p = val;
        while (*p == ' ') p++;
        int sp = 1;
        if (strn_ci_eq(p, "span", 4)) { p += 4; while (*p == ' ') p++; sp = (int)css_f(p); }
        else {
            int a = (int)css_f(p);
            while (*p && *p != '/') p++;
            if (*p == '/') {
                p++; while (*p == ' ') p++;
                if (strn_ci_eq(p, "span", 4)) { p += 4; sp = (int)css_f(p); }
                else { int b = (int)css_f(p); if (b > a) sp = b - a; else if (b == -1) sp = 12; }
            }
        }
        if (sp < 1) sp = 1;
        if (sp > 16) sp = 16;
        s->has_col_span = 1; s->col_span = sp;
    }
    else if (str_eq(prop, "grid-row")) {
        const char *p = val; while (*p == ' ') p++;
        int sp = 1;
        if (strn_ci_eq(p, "span", 4)) { p += 4; sp = (int)css_f(p); }
        if (sp < 1) sp = 1; if (sp > 12) sp = 12;
        s->has_row_span = 1; s->row_span = sp;
    }
    else if (str_eq(prop, "column-count")) {
        int cc = (int)css_f(val);
        if (cc >= 1 && cc <= 4) s->col_count = cc;
    }
    else if (str_eq(prop, "column-rule")) {
        int bw = 0, bs = 0, ba = 255; uint32_t bc = 0x444444;
        css_parse_border_shorthand(val, &bw, &bs, &bc, &ba);
        s->col_rule_w = bw; s->col_rule_style = bs; s->col_rule_c = bc; s->col_rule_a = ba;
    }
    else if (str_eq(prop, "background-size")) {
        if (str_eq_ci(val, "cover")) s->bg_size = 2;
        else if (str_eq_ci(val, "contain")) s->bg_size = 1;
        else if (str_eq_ci(val, "auto")) s->bg_size = 0;
        else {
            char toks[3][96]; int nt = css_split_ws(val, toks, 3);
            if (nt >= 1 && css_parse_len(toks[0], &px, &pc, (double)g_cur_avail_w, (double)g_inh_font)) {
                s->bg_size = 3; s->bg_size_w = px; s->bg_size_wp = pc;
                if (nt >= 2 && css_parse_len(toks[1], &px, &pc, (double)g_cur_avail_w, (double)g_inh_font)) {
                    s->bg_size_h = px; s->bg_size_hp = pc;
                } else { s->bg_size_h = s->bg_size_w; s->bg_size_hp = s->bg_size_wp; }
            }
        }
    }
    else if (str_eq(prop, "background-repeat")) {
        if (find_substr_ci(val, "no-repeat")) s->bg_repeat = 1;
        else if (find_substr_ci(val, "repeat-x")) s->bg_repeat = 2;
        else if (find_substr_ci(val, "repeat-y")) s->bg_repeat = 3;
        else if (find_substr_ci(val, "repeat")) s->bg_repeat = 0;
    }
    else if (str_eq(prop, "transform")) {
        const char *p = val;
        while (*p == ' ') p++;
        if (str_eq_ci(p, "none")) { s->has_translate = 0; s->has_scale = 0; s->has_rot = 0; s->has_rot_y = 0; return; }
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            char fn[24]; int k = 0;
            while (*p && *p != '(' && k < 23) fn[k++] = lower_c(*p++);
            fn[k] = 0; css_trim(fn);
            if (*p != '(') { while (*p && *p != ' ') p++; continue; }
            p++;
            char ar[96]; int ai = 0;
            while (*p && *p != ')' && ai < 95) ar[ai++] = *p++;
            ar[ai] = 0;
            if (*p == ')') p++;
            if (str_eq(fn, "translate") || str_eq(fn, "translate3d")) {
                char parts[3][220]; int np = css_split_comma(ar, parts, 3);
                if (np >= 1 && css_parse_len(parts[0], &px, &pc, (double)g_cur_avail_w, (double)g_inh_font)) {
                    s->has_translate = 1; s->tx = px; s->tx_pct = pc;
                }
                if (np >= 2 && css_parse_len(parts[1], &px, &pc, (double)g_view_h, (double)g_inh_font)) {
                    s->has_translate = 1; s->ty = px; s->ty_pct = pc;
                }
            }
            else if (str_eq(fn, "translatex")) {
                if (css_parse_len(ar, &px, &pc, (double)g_cur_avail_w, (double)g_inh_font)) {
                    s->has_translate = 1; s->tx = px; s->tx_pct = pc;
                }
            }
            else if (str_eq(fn, "translatey")) {
                if (css_parse_len(ar, &px, &pc, (double)g_view_h, (double)g_inh_font)) {
                    s->has_translate = 1; s->ty = px; s->ty_pct = pc;
                }
            }
            else if (str_eq(fn, "scale") || str_eq(fn, "scale3d")) {
                double v1 = css_f(ar), v2 = v1;
                const char *q = ar; while (*q && *q != ',') q++;
                if (*q == ',') v2 = css_f(q + 1);
                s->has_scale = 1;
                s->tscalex = (int)(v1 * 256.0 + 0.5);
                s->tscaley = (int)(v2 * 256.0 + 0.5);
            }
            else if (str_eq(fn, "rotate") || str_eq(fn, "rotatez")) {
                s->has_rot = 1; s->trot = (int)(css_f(ar) * 10.0);
            }
            else if (str_eq(fn, "rotatey")) {
                s->has_rot_y = 1; s->trot_y = (int)(css_f(ar) + 0.5);
            }
        }
    }
    else if (str_eq(prop, "transform-style")) {
        s->preserve_3d = str_eq_ci(val, "preserve-3d") ? 1 : 0;
    }
    else if (str_eq(prop, "backface-visibility") || str_eq(prop, "-webkit-backface-visibility")) {
        s->backface_hidden = str_eq_ci(val, "hidden") ? 1 : 0;
    }
    else if (str_eq(prop, "filter")) { css_parse_filter(s, val); }
    else if (str_eq(prop, "backdrop-filter") || str_eq(prop, "-webkit-backdrop-filter")) {
        s->has_backdrop_blur = (find_substr_ci(val, "blur(") != 0);
    }
    else if (str_eq(prop, "mix-blend-mode")) {
        if (str_eq_ci(val, "difference")) s->blend_mode = 1;
        else if (str_eq_ci(val, "screen")) s->blend_mode = 2;
        else if (str_eq_ci(val, "multiply")) s->blend_mode = 3;
        else s->blend_mode = 0;
    }
    else if (str_eq(prop, "clip-path")) {
        if (find_substr_ci(val, "inset(50%)") || find_substr_ci(val, "rect(0")) s->sr_clip = 1;
        else css_parse_clip_path(s, val);
    }
    else if (str_eq(prop, "mask-image") || str_eq(prop, "-webkit-mask-image")) {
        s->has_mask_fade = (find_substr_ci(val, "transparent") != 0);
    }
    else if (str_eq(prop, "accent-color")) {
        if (css_color(val, &c)) { s->accent_c = c; s->has_accent = 1; }
    }
    else if (str_eq(prop, "animation")) {
        char toks[8][96]; int nt = css_split_ws(val, toks, 8);
        if (nt >= 1 && !str_eq_ci(toks[0], "none")) {
            bcopy_str(s->anim_name, toks[0], sizeof(s->anim_name));
            s->anim_dur_ms = 2000;
            for (int i = 1; i < nt; i++) {
                if (find_substr_ci(toks[i], "ms")) s->anim_dur_ms = (int)css_f(toks[i]);
                else if (find_substr_ci(toks[i], "s") && toks[i][0] >= '0' && toks[i][0] <= '9')
                    s->anim_dur_ms = (int)(css_f(toks[i]) * 1000.0);
                else if (find_substr_ci(toks[i], "alternate")) s->anim_alt = 1;
                else if (str_eq_ci(toks[i], "paused")) s->anim_paused = 1;
            }
        }
    }
    else if (str_eq(prop, "animation-play-state")) {
        s->anim_paused = str_eq_ci(val, "paused") ? 1 : 0;
    }
    else if (str_eq(prop, "content")) {
        const char *p = val;
        while (*p == ' ') p++;
        if (str_eq_ci(p, "none") || str_eq_ci(p, "normal")) {
            s->has_content = 0; s->content[0] = 0; s->content_url = 0;
        } else if (*p == '"' || *p == '\'') {
            char q = *p++;
            int o = 0;
            s->has_content = 1; s->content_url = 0;
            while (*p && *p != q && o < (int)sizeof(s->content) - 4) {
                if (*p == '\\' && p[1]) {
                    p++;
                    if (p[0] == 'n') { s->content[o++] = '\n'; p++; }
                    else if (css_hexval(p[0]) >= 0) {
                        int v = 0, n = 0;
                        while (n < 6 && p[n] && css_hexval(p[n]) >= 0) { v = v * 16 + css_hexval(p[n]); n++; }
                        p += n; if (*p == ' ') p++;
                        if (v < 0x80) s->content[o++] = (char)v;
                        else if (v < 0x800) { s->content[o++] = (char)(0xC0|(v>>6)); s->content[o++] = (char)(0x80|(v&0x3F)); }
                        else { s->content[o++] = (char)(0xE0|(v>>12)); s->content[o++] = (char)(0x80|((v>>6)&0x3F)); s->content[o++] = (char)(0x80|(v&0x3F)); }
                    } else s->content[o++] = *p++;
                } else s->content[o++] = *p++;
            }
            s->content[o] = 0;
        } else if (strn_ci_eq(p, "attr(", 5)) {
            char nm[48]; int k = 0; p += 5;
            while (*p == ' ') p++;
            while (*p && *p != ')' && k < 47) nm[k++] = *p++;
            nm[k] = 0; css_trim(nm);
            s->has_content = 1; s->content_url = 0;
            s->content[0] = 1;
            bcopy_str(s->content + 1, nm, sizeof(s->content) - 1);
        } else if (strn_ci_eq(p, "url(", 4)) {
            char u[150]; int k = 0; p += 4;
            while (*p == ' ' || *p == '"' || *p == '\'') p++;
            while (*p && *p != ')' && *p != '"' && *p != '\'' && k < 149) u[k++] = *p++;
            u[k] = 0;
            s->has_content = 1; s->content_url = 1;
            bcopy_str(s->content_src, u, sizeof(s->content_src));
            s->content[0] = 0;
        }
    }
}

/* Pass modes: 0=custom vars only, 1=normal decls, 2=!important decls */
static void css_apply_pass(css_style_t *s, const char *decl, int dlen, int pass, int depth) {
    const char *p = decl;
    const char *end = decl + dlen;
    while (p < end) {
        while (p < end && (*p == ';' || *p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')) p++;
        if (p >= end) break;
        /* Read property name */
        char prop[48]; int pi = 0;
        while (p < end && *p != ':' && *p != ';') {
            if (pi < 47) prop[pi++] = *p;
            p++;
        }
        prop[pi] = 0;
        css_trim(prop);
        if (p >= end || *p != ':') { while (p < end && *p != ';') p++; continue; }
        p++;
        /* Read value respecting quotes and parens (so data:image/svg+xml;... doesn't split on ';') */
        char val[300]; int vi = 0;
        int paren = 0; char inq = 0;
        while (p < end) {
            char c = *p;
            if (inq) { if (c == inq) inq = 0; }
            else {
                if (c == '"' || c == '\'') inq = c;
                else if (c == '(') paren++;
                else if (c == ')' && paren > 0) paren--;
                else if (c == ';' && paren == 0) break;
            }
            if (vi < 299) val[vi++] = c;
            p++;
        }
        val[vi] = 0;
        css_trim(val);
        /* Detect & strip !important */
        int is_imp = 0;
        for (int i = 0; val[i]; i++) {
            if (val[i] == '!') {
                const char *q = val + i + 1;
                while (*q == ' ') q++;
                if (strn_ci_eq(q, "important", 9)) { is_imp = 1; val[i] = 0; css_trim(val); break; }
            }
        }
        if (!prop[0]) continue;
        if (prop[0] == '-' && prop[1] == '-') {
            if (pass == 0) css_var_push(prop, val, depth);
            continue;
        }
        if (pass == 0) continue;
        if ((pass == 1 && !is_imp) || (pass == 2 && is_imp)) {
            char prop_l[48];
            int k = 0;
            for (; prop[k] && k < 47; k++) prop_l[k] = lower_c(prop[k]);
            prop_l[k] = 0;
            char vs[320];
            css_var_subst(val, vs, sizeof(vs));
            css_set(s, prop_l, vs);
        }
    }
}

/* ---------------- CSS3 Selector Engine ---------------- */
static int class_has(const char *cls, const char *tok, int tl) {
    const char *p = cls;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        const char *s = p;
        while (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') s++;
        if ((int)(s - p) == tl) {
            int k = 0;
            while (k < tl && p[k] == tok[k]) k++;
            if (k == tl) return 1;
        }
        p = s;
    }
    return 0;
}

/* Element-only sibling navigation (skips #text nodes per CSS3 spec) */
static int prev_elem_sib(int n) {
    int par = g_dn[n].parent;
    if (par < 0) return -1;
    int prev = -1;
    for (int c = g_dn[par].first; c >= 0 && c != n; c = g_dn[c].next)
        if (!g_dn[c].is_text) prev = c;
    return prev;
}

static int next_elem_sib(int n) {
    for (int c = g_dn[n].next; c >= 0; c = g_dn[c].next)
        if (!g_dn[c].is_text) return c;
    return -1;
}

static int dn_attr(int n, const char *name, char *out, int cap) {
    const dnode_t *d = &g_dn[n];
    out[0] = 0;
    if (str_eq_ci(name, "class")) bcopy_str(out, d->cls, cap);
    else if (str_eq_ci(name, "id")) bcopy_str(out, d->id, cap);
    else if (str_eq_ci(name, "type")) bcopy_str(out, d->type, cap);
    else if (str_eq_ci(name, "name")) bcopy_str(out, d->name, cap);
    else if (str_eq_ci(name, "value")) bcopy_str(out, d->value, cap);
    else if (str_eq_ci(name, "placeholder") || str_eq_ci(name, "title")) bcopy_str(out, d->ph, cap);
    else if (str_eq_ci(name, "alt")) bcopy_str(out, d->alt, cap);
    else if (str_eq_ci(name, "src")) bcopy_str(out, d->src, cap);
    else if (str_eq_ci(name, "href")) bcopy_str(out, d->href, cap);
    else if (str_eq_ci(name, "style")) bcopy_str(out, d->istyle, cap);
    else if (str_eq_ci(name, "for")) bcopy_str(out, d->for_attr, cap);
    else if (str_eq_ci(name, "data-category")) bcopy_str(out, d->data_cat, cap);
    else if (str_eq_ci(name, "data-tooltip")) bcopy_str(out, d->data_tip, cap);
    else if (str_eq_ci(name, "open")) { if (d->is_open) bcopy_str(out, "open", cap); return d->is_open; }
    else if (str_eq_ci(name, "checked")) { if (d->checked) bcopy_str(out, "checked", cap); return d->checked; }
    else if (str_eq_ci(name, "required")) { if (d->required) bcopy_str(out, "required", cap); return d->required; }
    else if (d->extra_attrs[0]) {
        /* extra_attrs format: "key=val\x1fkey2=val2" */
        const char *p = d->extra_attrs;
        int nl = b_len(name);
        while (*p) {
            const char *eq = p;
            while (*eq && *eq != '=' && *eq != '\x1f') eq++;
            const char *end = eq;
            while (*end && *end != '\x1f') end++;
            if ((int)(eq - p) == nl && strn_ci_eq(p, name, nl)) {
                if (*eq == '=') {
                    int vl = (int)(end - (eq + 1));
                    if (vl > cap - 1) vl = cap - 1;
                    for (int i = 0; i < vl; i++) out[i] = eq[1 + i];
                    out[vl] = 0;
                }
                return 1;
            }
            p = (*end == '\x1f') ? end + 1 : end;
        }
    }
    return out[0] != 0;
}

static int sel_attr_match(int n, const char *t) {
    char name[48]; int ni = 0;
    const char *p = t;
    while (*p == ' ') p++;
    while (*p && *p != '=' && *p != '~' && *p != '|' && *p != '^' &&
           *p != '$' && *p != '*' && ni < 47) name[ni++] = *p++;
    name[ni] = 0;
    css_trim(name);
    char val[120];
    if (!dn_attr(n, name, val, sizeof(val))) return 0;
    if (!*p) return 1;
    char op = *p++;
    if (*p == '=') p++;
    while (*p == ' ') p++;
    char want[96]; int wi = 0;
    if (*p == '"' || *p == '\'') {
        char q = *p++;
        while (*p && *p != q && wi < 95) want[wi++] = *p++;
        if (*p == q) p++;
    } else {
        while (*p && *p != ' ' && wi < 95) want[wi++] = *p++;
    }
    want[wi] = 0;
    if (op == '=') return str_eq_ci(val, want);
    if (op == '~') return class_has(val, want, wi);
    if (op == '|') {
        if (str_eq_ci(val, want)) return 1;
        int wl = b_len(want);
        return val[wl] == '-' && strn_ci_eq(val, want, wl);
    }
    if (op == '^') return wi > 0 && strn_ci_eq(val, want, wi);
    if (op == '$') { int vl = b_len(val); return wi > 0 && wi <= vl && strn_ci_eq(val + vl - wi, want, wi); }
    if (op == '*') return wi > 0 && find_substr_ci(val, want) != 0;
    return 0;
}

static int nth_match(const char *e, int idx) {
    while (*e == ' ') e++;
    if (str_eq_ci(e, "odd")) return idx % 2 == 1;
    if (str_eq_ci(e, "even")) return idx % 2 == 0;
    int a = 0, b = 0;
    if (*e == 'n' || *e == 'N') { a = 1; e++; }
    else if (*e == '-' && (e[1] == 'n' || e[1] == 'N')) { a = -1; e += 2; }
    else {
        int neg = 0;
        if (*e == '-') { neg = 1; e++; } else if (*e == '+') e++;
        int num = 0;
        while (*e >= '0' && *e <= '9') { num = num * 10 + (*e - '0'); e++; }
        if (*e == 'n' || *e == 'N') { a = neg ? -num : num; e++; }
        else return (neg ? -num : num) == idx;
    }
    while (*e == ' ') e++;
    if (*e == '+') { e++; while (*e == ' ') e++; while (*e >= '0' && *e <= '9') { b = b * 10 + (*e - '0'); e++; } }
    else if (*e == '-') { e++; while (*e == ' ') e++; while (*e >= '0' && *e <= '9') { b = b * 10 + (*e - '0'); e++; } b = -b; }
    if (a == 0) return idx == b;
    int d = idx - b;
    if (a > 0) return d >= 0 && d % a == 0;
    return d <= 0 && (-d) % (-a) == 0;
}

static int pseudo_inner(const char *t, char *out, int cap) {
    const char *a = t;
    while (*a && *a != '(') a++;
    if (*a != '(') { out[0] = 0; return 0; }
    a++;
    int depth = 1, o = 0;
    while (*a && depth) {
        if (*a == '(') depth++;
        else if (*a == ')') { depth--; if (depth == 0) break; }
        if (o < cap - 1) out[o++] = *a;
        a++;
    }
    out[o] = 0;
    return 1;
}

static int split_top(const char *list, char alts[][CSS_SEL], int maxalt) {
    int cnt = 0, depth = 0, br = 0, o = 0;
    const char *p = list;
    for (;;) {
        char c = *p;
        if (c == 0 || (c == ',' && depth == 0 && br == 0)) {
            if (cnt < maxalt) {
                alts[cnt][o] = 0;
                css_trim(alts[cnt]);
                if (alts[cnt][0]) cnt++;
            }
            o = 0;
            if (c == 0) break;
            p++;
            continue;
        }
        if (c == '(') depth++;
        else if (c == ')' && depth > 0) depth--;
        else if (c == '[') br++;
        else if (c == ']' && br > 0) br--;
        if (cnt < maxalt && o < CSS_SEL - 1) alts[cnt][o++] = c;
        p++;
    }
    return cnt;
}

static int dn_is_ancestor(int a, int b);

static int node_input_valid(int n) {
    const dnode_t *d = &g_dn[n];
    if (str_eq(d->tag, "input") && str_eq(d->type, "checkbox"))
        return d->required ? d->checked : 1;
    const char *v = d->value;
    if (d->widget && d->is_ta)
        v = lv_textarea_get_text(d->widget);
    if (!v) v = "";
    if (d->required && !v[0]) return 0;
    if (str_eq(d->type, "email") && v[0]) {
        const char *at = 0, *dot = 0;
        for (const char *p = v; *p; p++) {
            if (*p == '@') at = p;
            else if (at && *p == '.') dot = p;
        }
        if (!at || at == v || !dot || dot <= at + 1 || !dot[1]) return 0;
    }
    return 1;
}

static int node_placeholder_shown(int n) {
    const dnode_t *d = &g_dn[n];
    if (!str_eq(d->tag, "input") && !str_eq(d->tag, "textarea")) return 0;
    const char *v = d->value;
    if (d->widget && d->is_ta) v = lv_textarea_get_text(d->widget);
    return (!v || !v[0]);
}

static int sel_match_simple(int n, const char *sel);

static int sel_list_match(int n, const char *list) {
    char alts[12][CSS_SEL];
    int c = split_top(list, alts, 12);
    for (int i = 0; i < c; i++) {
        /* Handle simple pseudo/attr inside :not(...) directly */
        if (sel_match(n, alts[i])) return 1;
    }
    return 0;
}

static int sel_pseudo_match(int n, const char *t) {
    if (str_eq(t, "first-child")) return prev_elem_sib(n) < 0;
    if (str_eq(t, "last-child")) return next_elem_sib(n) < 0;
    if (str_eq(t, "only-child")) return prev_elem_sib(n) < 0 && next_elem_sib(n) < 0;
    if (str_eq(t, "root")) return n == 0 || str_eq(g_dn[n].tag, "html");
    if (str_eq(t, "empty")) return g_dn[n].first < 0 && g_dn[n].text[0] == 0;
    if (str_eq(t, "scope")) return 1;
    if (str_eq(t, "link") || str_eq(t, "visited") || str_eq(t, "any-link"))
        return str_eq(g_dn[n].tag, "a") && g_dn[n].href[0] != 0;
    if (str_eq(t, "checked")) return g_dn[n].checked != 0;
    if (str_eq(t, "target")) return g_cur_hash[0] != 0 && g_dn[n].id[0] != 0 && str_eq(g_dn[n].id, g_cur_hash);
    if (str_eq(t, "hover")) return g_hover_node >= 0 && (n == g_hover_node || dn_is_ancestor(n, g_hover_node));
    if (str_eq(t, "focus") || str_eq(t, "focus-visible")) return g_focus_node == n;
    if (str_eq(t, "focus-within")) return g_focus_node >= 0 && (n == g_focus_node || dn_is_ancestor(n, g_focus_node));
    if (str_eq(t, "placeholder-shown")) return node_placeholder_shown(n);
    if (str_eq(t, "valid")) return (str_eq(g_dn[n].tag, "input") || str_eq(g_dn[n].tag, "textarea")) && node_input_valid(n);
    if (str_eq(t, "invalid")) return (str_eq(g_dn[n].tag, "input") || str_eq(g_dn[n].tag, "textarea")) && !node_input_valid(n);
    if (str_eq(t, "required")) return g_dn[n].required != 0;
    if (str_eq(t, "optional")) return !g_dn[n].required;
    if (starts_with(t, "is(") || starts_with(t, "where(") || starts_with(t, "matches(")) {
        char inner[CSS_SEL];
        if (pseudo_inner(t, inner, CSS_SEL)) return sel_list_match(n, inner);
        return 0;
    }
    if (starts_with(t, "not(")) {
        char inner[CSS_SEL];
        if (pseudo_inner(t, inner, CSS_SEL)) {
            int save_pe = g_pe;
            g_pe = 0;
            int m = sel_list_match(n, inner);
            g_pe = save_pe;
            return !m;
        }
        return 1;
    }
    if (starts_with(t, "nth-child(") || starts_with(t, "nth-last-child(")) {
        char inner[CSS_SEL];
        if (!pseudo_inner(t, inner, CSS_SEL)) return 0;
        int idx = 1;
        if (t[4] == 'l') { for (int s = next_elem_sib(n); s >= 0; s = next_elem_sib(s)) idx++; }
        else { for (int p = prev_elem_sib(n); p >= 0; p = prev_elem_sib(p)) idx++; }
        return nth_match(inner, idx);
    }
    if (starts_with(t, "lang(")) return 1;
    return 0;
}

static int sel_match_simple(int n, const char *sel) {
    if (g_dn[n].is_text) return 0;
    const dnode_t *d = &g_dn[n];
    const char *p = sel;
    if (*p == '*') p++;
    else if (*p && *p != '.' && *p != '#' && *p != '[' && *p != ':') {
        const char *s = p; int tl = 0;
        while (*p && *p != '.' && *p != '#' && *p != '[' && *p != ':') { p++; tl++; }
        if (b_len(d->tag) != tl || !strn_ci_eq(d->tag, s, tl)) return 0;
    }
    while (*p) {
        if (*p == '.') {
            p++;
            const char *s = p; int tl = 0;
            while (*p && *p != '.' && *p != '#' && *p != '[' && *p != ':') { p++; tl++; }
            char tok[128]; if (tl > 127) tl = 127;
            for (int i = 0; i < tl; i++) tok[i] = s[i];
            tok[tl] = 0;
            if (!class_has(d->cls, tok, tl)) return 0;
        } else if (*p == '#') {
            p++;
            const char *s = p; int tl = 0;
            while (*p && *p != '.' && *p != '#' && *p != '[' && *p != ':') { p++; tl++; }
            if (b_len(d->id) != tl) return 0;
            for (int i = 0; i < tl; i++) if (d->id[i] != s[i]) return 0;
        } else if (*p == '[') {
            p++;
            const char *s = p; int tl = 0;
            while (*p && *p != ']') { p++; tl++; }
            char tok[160]; if (tl > 159) tl = 159;
            for (int i = 0; i < tl; i++) tok[i] = s[i];
            tok[tl] = 0;
            if (!sel_attr_match(n, tok)) return 0;
            if (*p == ']') p++;
        } else if (*p == ':') {
            if (p[1] == ':') {
                p += 2;
                while (*p && *p != '.' && *p != '#' && *p != '[' && *p != ':') p++;
                continue;
            }
            p++;
            const char *s = p;
            while (*p && *p != '(' && *p != '.' && *p != '#' && *p != '[' && *p != ':') p++;
            if (*p == '(') {
                int dd = 0;
                do { if (*p == '(') dd++; else if (*p == ')') dd--; if (!*p) break; p++; } while (dd > 0);
            }
            int tl = (int)(p - s);
            char tok[CSS_SEL]; if (tl > CSS_SEL - 1) tl = CSS_SEL - 1;
            for (int i = 0; i < tl; i++) tok[i] = s[i];
            tok[tl] = 0;
            if (!sel_pseudo_match(n, tok)) return 0;
        } else p++;
    }
    return 1;
}

static int sel_match_from(int n, char *parts[], int comb[], int idx) {
    if (idx < 0) return 1;
    if (!sel_match_simple(n, parts[idx])) return 0;
    if (idx == 0) return 1;
    int c = comb[idx];
    if (c == 1) {
        int par = g_dn[n].parent;
        return par >= 0 && sel_match_from(par, parts, comb, idx - 1);
    }
    if (c == 2) {
        int ps = prev_elem_sib(n);
        return ps >= 0 && sel_match_from(ps, parts, comb, idx - 1);
    }
    if (c == 3) {
        for (int ps = prev_elem_sib(n); ps >= 0; ps = prev_elem_sib(ps))
            if (sel_match_from(ps, parts, comb, idx - 1)) return 1;
        return 0;
    }
    for (int a = g_dn[n].parent; a >= 0; a = g_dn[a].parent)
        if (sel_match_from(a, parts, comb, idx - 1)) return 1;
    return 0;
}

static int sel_pe_strip(char *s) {
    int paren = 0, br = 0;
    for (char *p = s; *p; p++) {
        if (*p == '(') { paren++; continue; }
        if (*p == ')' && paren > 0) { paren--; continue; }
        if (*p == '[') { br++; continue; }
        if (*p == ']' && br > 0) { br--; continue; }
        if (paren || br || *p != ':') continue;
        if (p[1] == ':') {
            char nm[28]; int k = 0; const char *x = p + 2;
            while (k < 27 && ((*x>='a'&&*x<='z')||(*x>='A'&&*x<='Z')||*x=='-')) nm[k++] = lower_c(*x++);
            nm[k] = 0;
            if (!nm[0]) continue;
            *p = 0;
            if (str_eq(nm, "before")) return 1;
            if (str_eq(nm, "after")) return 2;
            if (str_eq(nm, "first-letter")) return 3;
            return 4; /* ::selection, ::-webkit-details-marker, etc. */
        }
        const char *x = p + 1;
        char nm[24]; int k = 0;
        while (k < 23 && ((*x>='a'&&*x<='z')||(*x>='A'&&*x<='Z')||*x=='-')) nm[k++] = lower_c(*x++);
        nm[k] = 0;
        if (str_eq(nm, "before")) { *p = 0; return 1; }
        if (str_eq(nm, "after"))  { *p = 0; return 2; }
        if (str_eq(nm, "first-letter")) { *p = 0; return 3; }
    }
    return 0;
}

static int sel_match(int n, const char *sel) {
    char buf[CSS_SEL];
    bcopy_str(buf, sel, CSS_SEL);
    int pe = sel_pe_strip(buf);
    if (pe != g_pe) return 0;
    char *parts[12]; int comb[12]; int np = 0;
    char *p = buf; int pending = 0;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        int c = pending; pending = 0;
        if (*p == '>') { c = 1; p++; while (*p == ' ') p++; }
        else if (*p == '+') { c = 2; p++; while (*p == ' ') p++; }
        else if (*p == '~') { c = 3; p++; while (*p == ' ') p++; }
        if (!*p) break;
        char *s = p; int par = 0, br = 0;
        while (*p) {
            if (*p == '(') { par++; p++; continue; }
            if (*p == ')' && par > 0) { par--; p++; continue; }
            if (*p == '[') { br++; p++; continue; }
            if (*p == ']' && br > 0) { br--; p++; continue; }
            if (par == 0 && br == 0 && (*p == ' ' || *p == '\t' || *p == '>' || *p == '+' || *p == '~')) break;
            p++;
        }
        char save = *p;
        *p = 0;
        if (np < 12) { parts[np] = s; comb[np] = c; np++; }
        if (save) {
            pending = (save == '>') ? 1 : (save == '+') ? 2 : (save == '~') ? 3 : 0;
            p++;
        }
    }
    if (np == 0) return (pe != 0); /* standalone ::before / ::after matches all elements */
    return sel_match_from(n, parts, comb, np - 1);
}

static int sel_spec(const char *sel) {
    int s = 0;
    const char *p = sel;
    while (*p) {
        char c = *p;
        if (c == '[') { s += 10; while (*p && *p != ']') p++; if (*p) p++; continue; }
        if (c == ':') {
            if (p[1] == ':') {
                s += 1; p += 2;
                while (*p && *p != '.' && *p != '#' && *p != '[' && *p != ':' && *p != ' ' && *p != '>' && *p != '+' && *p != '~') p++;
                continue;
            }
            p++;
            const char *nm = p;
            while (*p && *p != '(' && *p != '.' && *p != '#' && *p != '[' && *p != ':' && *p != ' ' && *p != '>' && *p != '+' && *p != '~') p++;
            int nlen = (int)(p - nm);
            if (*p == '(') {
                char inner[CSS_SEL]; pseudo_inner(nm, inner, CSS_SEL);
                if (nlen == 5 && strn_ci_eq(nm, "where", 5)) { }
                else if ((nlen == 2 && strn_ci_eq(nm, "is", 2)) || (nlen == 3 && strn_ci_eq(nm, "not", 3)))
                    s += sel_spec(inner);
                else s += 10;
                int d = 0; do { if (*p == '(') d++; else if (*p == ')') d--; if (!*p) break; p++; } while (d > 0);
                continue;
            }
            s += 10; continue;
        }
        if (c == '#') { s += 100; p++; }
        else if (c == '.') { s += 10; p++; }
        else if (c == ' ' || c == '>' || c == '+' || c == '~' || c == '\t' || c == '\n' || c == '*') p++;
        else { s += 1; while (*p && *p != '.' && *p != '#' && *p != ' ' && *p != ':' && *p != '[' && *p != '>' && *p != '+' && *p != '~') p++; }
    }
    return s;
}

/* ---------------- @media / @supports / @container / @keyframes ---------------- */
static int media_feature_ok(const char *name, const char *val) {
    if (find_substr_ci(name, "prefers-reduced-motion"))
        return str_eq_ci(val, "no-preference") ? 1 : 0;
    if (find_substr_ci(name, "prefers-color-scheme"))
        return str_eq_ci(val, "dark") ? 1 : 0;
    if (find_substr_ci(name, "forced-colors"))
        return str_eq_ci(val, "none") ? 1 : 0;
    if (find_substr_ci(name, "prefers-contrast"))
        return str_eq_ci(val, "no-preference") ? 1 : 0;
    if (find_substr_ci(name, "pointer"))
        return str_eq_ci(val, "fine") ? 1 : 0;
    if (find_substr_ci(name, "hover"))
        return str_eq_ci(val, "hover") ? 1 : 0;
    if (find_substr_ci(name, "orientation")) {
        int vw = g_view_w > 0 ? g_view_w : 1280;
        int vh = g_view_h > 0 ? g_view_h : 800;
        if (str_eq_ci(val, "landscape")) return vw >= vh;
        if (str_eq_ci(val, "portrait")) return vh > vw;
    }
    int v;
    if (find_substr_ci(name, "width")) v = g_view_w > 0 ? g_view_w : 1280;
    else if (find_substr_ci(name, "height")) v = g_view_h > 0 ? g_view_h : 800;
    else return 1;
    int pc = 0, n = 0;
    if (!css_parse_len(val, &n, &pc, (double)v, 16.0)) return 1;
    if (find_substr_ci(name, "min-")) return v >= n;
    if (find_substr_ci(name, "max-")) return v <= n;
    return v == n;
}

static int media_cmp_eval(double a, int op, double b) {
    /* op: 1=<, 2=<=, 3=>, 4=>=, 5== */
    if (op == 1) return a < b;
    if (op == 2) return a <= b;
    if (op == 3) return a > b;
    if (op == 4) return a >= b;
    return a == b;
}

/* CSS Media Queries Level 4 range syntax: (width >= 1012px), (width <= calc(48rem - .02px)), (544px <= width < 1012px) */
static int media_range_ok(const char *ft) {
    /* Split into up to 3 operands and 2 comparison operators outside parentheses */
    char parts[3][96];
    int ops[2] = {0, 0};
    int np = 0, nop = 0, par = 0;
    const char *p = ft, *seg = ft;
    while (*p) {
        if (*p == '(') par++;
        else if (*p == ')' && par > 0) par--;
        else if (par == 0 && (*p == '<' || *p == '>' || *p == '=')) {
            if (np < 3) {
                int l = (int)(p - seg); if (l > 95) l = 95;
                for (int i = 0; i < l; i++) parts[np][i] = seg[i];
                parts[np][l] = 0; css_trim(parts[np]); np++;
            }
            int op = 5;
            if (*p == '<' && p[1] == '=') { op = 2; p += 2; }
            else if (*p == '<') { op = 1; p++; }
            else if (*p == '>' && p[1] == '=') { op = 4; p += 2; }
            else if (*p == '>') { op = 3; p++; }
            else { op = 5; p++; }
            if (nop < 2) ops[nop++] = op;
            seg = p;
            continue;
        }
        p++;
    }
    if (np < 3) {
        int l = (int)(p - seg); if (l > 95) l = 95;
        for (int i = 0; i < l; i++) parts[np][i] = seg[i];
        parts[np][l] = 0; css_trim(parts[np]); np++;
    }
    if (nop == 0) return -1; /* not a range query */
    int vw = g_view_w > 0 ? g_view_w : 1280;
    int vh = g_view_h > 0 ? g_view_h : 800;
    double vals[3] = {0, 0, 0};
    int valid[3] = {0, 0, 0};
    for (int i = 0; i < np; i++) {
        if (str_eq_ci(parts[i], "width") || str_eq_ci(parts[i], "device-width")) { vals[i] = (double)vw; valid[i] = 1; }
        else if (str_eq_ci(parts[i], "height") || str_eq_ci(parts[i], "device-height")) { vals[i] = (double)vh; valid[i] = 1; }
        else {
            int px = 0, pc = 0;
            if (css_parse_len(parts[i], &px, &pc, (double)vw, 16.0)) { vals[i] = (double)px; valid[i] = 1; }
            else { vals[i] = css_f(parts[i]); valid[i] = 1; }
        }
    }
    if (nop == 1 && np >= 2 && valid[0] && valid[1])
        return media_cmp_eval(vals[0], ops[0], vals[1]);
    if (nop == 2 && np >= 3 && valid[0] && valid[1] && valid[2])
        return media_cmp_eval(vals[0], ops[0], vals[1]) && media_cmp_eval(vals[1], ops[1], vals[2]);
    return 1;
}

static int media_alt(const char *p, int len) {
    char b[256];
    int n = len > 255 ? 255 : len;
    for (int i = 0; i < n; i++) b[i] = p[i];
    b[n] = 0;
    const char *q = b;
    while (*q == ' ') q++;
    int neg = 0;
    if (strn_ci_eq(q, "not", 3) && (q[3] == ' ' || q[3] == 0)) { neg = 1; q += 3; }
    int ok = 1;
    const char *s = q;
    while (*s) {
        if (*s == '(') {
            const char *e = s + 1; int d = 1;
            while (*e && d) { if (*e == '(') d++; else if (*e == ')') d--; if (d == 0) break; e++; }
            char ft[128]; int o = 0;
            for (const char *x = s + 1; x < e && o < 127; x++) ft[o++] = *x;
            ft[o] = 0;
            int ro = media_range_ok(ft);
            if (ro >= 0) {
                if (!ro) ok = 0;
            } else {
                char *colon = ft; while (*colon && *colon != ':') colon++;
                if (*colon == ':') {
                    *colon = 0; char *val = colon + 1;
                    css_trim(ft); css_trim(val);
                    if (!media_feature_ok(ft, val)) ok = 0;
                }
            }
            s = (*e == ')') ? e + 1 : e;
        } else if (strn_ci_eq(s, "print", 5)) { ok = 0; s += 5; }
        else if (strn_ci_eq(s, "screen", 6)) s += 6;
        else if (strn_ci_eq(s, "all", 3)) s += 3;
        else s++;
    }
    return neg ? !ok : ok;
}

static int media_matches(const char *q) {
    const char *p = q;
    while (*p) {
        const char *c = p; int depth = 0;
        while (*c && !(*c == ',' && depth == 0)) {
            if (*c == '(') depth++; else if (*c == ')' && depth) depth--; c++;
        }
        if (media_alt(p, (int)(c - p))) return 1;
        if (!*c) break;
        p = c + 1;
    }
    return 0;
}

static int css_pool_append(const char *s, int len) {
    if (len < 0) len = 0;
    if (!g_basi || !g_basi->mem || !g_basi->mem->alloc) return -1;
    if (g_css_pool_len + len + 1 >= g_css_pool_limit) return -1;
    if (g_css_pool_len + len + 1 >= g_css_pool_cap) {
        int nc = g_css_pool_cap ? g_css_pool_cap * 2 : 65536;
        while (nc < g_css_pool_len + len + 1) nc *= 2;
        if (nc > g_css_pool_limit) nc = g_css_pool_limit;
        char *np = (char *)g_basi->mem->alloc((size_t)nc);
        if (!np) return -1;
        for (int i = 0; i < g_css_pool_len; i++) np[i] = g_css_pool[i];
        if (g_css_pool && g_basi->mem->free) g_basi->mem->free(g_css_pool);
        g_css_pool = np;
        g_css_pool_cap = nc;
    }
    int off = g_css_pool_len;
    for (int i = 0; i < len; i++) g_css_pool[g_css_pool_len++] = s[i];
    g_css_pool[g_css_pool_len++] = 0;
    return off;
}

static css_rule_t *css_rule_alloc(void) {
    if (g_css_count >= g_css_rules_max || !g_basi || !g_basi->mem || !g_basi->mem->alloc) return 0;
    if (g_css_count >= g_css_cap) {
        int nc = g_css_cap ? g_css_cap * 2 : 2048;
        if (nc > g_css_rules_max) nc = g_css_rules_max;
        css_rule_t *na = (css_rule_t *)g_basi->mem->alloc((size_t)nc * sizeof(css_rule_t));
        if (!na) return 0;
        for (int i = 0; i < g_css_count; i++) na[i] = g_css[i];
        if (g_css && g_basi->mem->free) g_basi->mem->free(g_css);
        g_css = na;
        g_css_cap = nc;
    }
    g_css_idx_built = 0;
    return &g_css[g_css_count++];
}

/* O(1) hizli eslesme on-filtresi: en sagdaki bilesik seciciden tag/id/class cikarir */
static void css_extract_key(css_rule_t *r) {
    r->key_tag[0] = 0; r->key_id[0] = 0; r->key_cls[0] = 0; r->key_cls_len = 0;
    const char *s = r->sel;
    const char *last = s;
    int par = 0, br = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '(') par++;
        else if (*p == ')' && par) par--;
        else if (*p == '[') br++;
        else if (*p == ']' && br) br--;
        else if (!par && !br && (*p == ' ' || *p == '>' || *p == '+' || *p == '~'))
            last = p + 1;
    }
    const char *p = last;
    while (*p == ' ') p++;
    /* :is() / :where() kokteyse on-filtreyi atla */
    if (strn_ci_eq(p, ":is(", 4) || strn_ci_eq(p, ":where(", 7)) return;
    if (*p && is_alpha(*p)) {
        int n = 0;
        while (*p && (is_alpha(*p) || (*p >= '0' && *p <= '9') || *p == '-')) {
            if (n < 23) r->key_tag[n++] = lower_c(*p);
            else { n = 0; break; }
            p++;
        }
        r->key_tag[n] = 0;
    }
    par = 0; br = 0;
    while (*p) {
        if (*p == '(') { par++; p++; continue; }
        if (*p == ')') { if (par) par--; p++; continue; }
        if (*p == '[') { br++; p++; continue; }
        if (*p == ']') { if (br) br--; p++; continue; }
        if (!par && !br) {
            if (*p == '#' && !r->key_id[0]) {
                p++; int n = 0, ov = 0;
                while (*p && (is_alpha(*p) || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '\\')) {
                    if (*p == '\\') { ov = 1; p++; if (*p) p++; continue; }
                    if (n < 63) r->key_id[n++] = *p; else ov = 1;
                    p++;
                }
                if (ov) { r->key_id[0] = 0; } else r->key_id[n] = 0;
                continue;
            }
            if (*p == '.' && !r->key_cls[0]) {
                p++; int n = 0, ov = 0;
                while (*p && (is_alpha(*p) || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '\\')) {
                    if (*p == '\\') { ov = 1; p++; if (*p) p++; continue; }
                    if (n < 95) r->key_cls[n++] = *p; else ov = 1;
                    p++;
                }
                if (ov) { r->key_cls[0] = 0; r->key_cls_len = 0; }
                else { r->key_cls[n] = 0; r->key_cls_len = (uint8_t)n; }
                continue;
            }
        }
        p++;
    }
}

static void css_parse_keyframes(const char *name, const char *body, int blen) {
    if (g_kf_count >= CSS_KF_MAX) return;
    css_kf_t *kf = &g_kf[g_kf_count++];
    bcopy_str(kf->name, name, sizeof(kf->name));
    css_trim(kf->name);
    kf->nstops = 0;
    const char *p = body, *end = body + blen;
    while (p < end && kf->nstops < 6) {
        while (p < end && (*p==' '||*p=='\n'||*p=='\r'||*p=='\t'||*p=='}')) p++;
        if (p >= end) break;
        const char *ks = p;
        while (p < end && *p != '{') p++;
        if (p >= end) break;
        const char *ob = p + 1;
        int d = 1; p++;
        while (p < end && d > 0) { if (*p == '{') d++; else if (*p == '}') { d--; if (d == 0) break; } p++; }
        int dl = (int)(p - ob);
        int doff = css_pool_append(ob, dl);
        if (doff >= 0) {
            char ksel[64]; int kl = (int)(ob - 1 - ks); if (kl > 63) kl = 63;
            for (int i = 0; i < kl; i++) ksel[i] = ks[i];
            ksel[kl] = 0;
            int pct = 0;
            if (find_substr_ci(ksel, "from")) pct = 0;
            else if (find_substr_ci(ksel, "to")) pct = 100;
            else pct = (int)css_f(ksel);
            kf->stops[kf->nstops].pct = pct;
            kf->stops[kf->nstops].decl_off = doff;
            kf->stops[kf->nstops].decl_len = dl;
            kf->nstops++;
        }
        if (p < end && *p == '}') p++;
    }
}

static void css_parse_d(const char *css, int cdepth, int min_cw, int max_cw) {
    const char *p = css;
    while (*p && g_css_count < g_css_rules_max) {
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r' || *p == '}') p++;
        if (!*p) break;
        const char *sel = p;
        const char *ob = p;
        char inq = 0;
        while (*ob) {
            if (inq) { if (*ob == inq) inq = 0; }
            else {
                if (*ob == '"' || *ob == '\'') inq = *ob;
                else if (*ob == '{' || *ob == ';') break;
            }
            ob++;
        }
        if (*ob == ';') { p = ob + 1; continue; }
        if (!*ob) break;
        const char *ce = ob + 1;
        int depth = 1; inq = 0;
        while (*ce && depth > 0) {
            if (inq) { if (*ce == inq) inq = 0; }
            else {
                if (*ce == '"' || *ce == '\'') inq = *ce;
                else if (*ce == '{') depth++;
                else if (*ce == '}') { depth--; if (depth == 0) break; }
            }
            ce++;
        }
        if (*sel == '@') {
            if (strn_ci_eq(sel, "@media", 6) && cdepth < 12) {
                char mq[256]; int l = (int)(ob - (sel + 6)); if (l > 255) l = 255;
                for (int i = 0; i < l; i++) mq[i] = sel[6 + i];
                mq[l] = 0;
                if (media_matches(mq)) {
                    char saved = *ce; *(char *)ce = 0;
                    css_parse_d(ob + 1, cdepth + 1, min_cw, max_cw);
                    *(char *)ce = saved;
                }
            } else if (strn_ci_eq(sel, "@container", 10) && cdepth < 12) {
                char cq[160]; int l = (int)(ob - (sel + 10)); if (l > 159) l = 159;
                for (int i = 0; i < l; i++) cq[i] = sel[10 + i];
                cq[l] = 0;
                int cmin = min_cw, cmax = max_cw;
                const char *mw = find_substr_ci(cq, "min-width:");
                if (mw) { int px = 0, pc = 0; if (css_parse_len(mw + 10, &px, &pc, 1280.0, 16.0)) cmin = px; }
                const char *xw = find_substr_ci(cq, "max-width:");
                if (xw) { int px = 0, pc = 0; if (css_parse_len(xw + 10, &px, &pc, 1280.0, 16.0)) cmax = px; }
                const char *ge = find_substr_ci(cq, "width>=");
                if (!ge) ge = find_substr_ci(cq, "width >=");
                if (ge) { while (*ge && *ge != '=') ge++; if (*ge == '=') ge++; int px = 0, pc = 0; if (css_parse_len(ge, &px, &pc, 1280.0, 16.0)) cmin = px; }
                const char *le = find_substr_ci(cq, "width<=");
                if (!le) le = find_substr_ci(cq, "width <=");
                if (le) { while (*le && *le != '=') le++; if (*le == '=') le++; int px = 0, pc = 0; if (css_parse_len(le, &px, &pc, 1280.0, 16.0)) cmax = px; }
                char saved = *ce; *(char *)ce = 0;
                css_parse_d(ob + 1, cdepth + 1, cmin, cmax);
                *(char *)ce = saved;
            } else if ((strn_ci_eq(sel, "@keyframes", 10) || strn_ci_eq(sel, "@-webkit-keyframes", 18))) {
                const char *nm = sel + (sel[1] == '-' ? 18 : 10);
                char kname[48]; int kl = (int)(ob - nm); if (kl > 47) kl = 47;
                for (int i = 0; i < kl; i++) kname[i] = nm[i];
                kname[kl] = 0;
                css_parse_keyframes(kname, ob + 1, (int)(ce - (ob + 1)));
            } else if ((strn_ci_eq(sel, "@supports", 9) || strn_ci_eq(sel, "@layer", 6)) && cdepth < 12) {
                char saved = *ce; *(char *)ce = 0;
                css_parse_d(ob + 1, cdepth + 1, min_cw, max_cw);
                *(char *)ce = saved;
            }
            p = (*ce == '}') ? ce + 1 : ce;
            continue;
        }
        int dl = (int)(ce - (ob + 1));
        int doff = css_pool_append(ob + 1, dl);
        if (doff < 0) break;

        char selbuf[CSS_SEL * 8];
        int tl = (int)(ob - sel);
        if (tl > (int)sizeof(selbuf) - 1) tl = (int)sizeof(selbuf) - 1;
        for (int i = 0; i < tl; i++) selbuf[i] = sel[i];
        selbuf[tl] = 0;

        char *cur = selbuf;
        while (*cur && g_css_count < g_css_rules_max) {
            char *comma = cur; int par = 0, br = 0;
            while (*comma) {
                if (*comma == '(') par++;
                else if (*comma == ')' && par) par--;
                else if (*comma == '[') br++;
                else if (*comma == ']' && br) br--;
                else if (*comma == ',' && !par && !br) break;
                comma++;
            }
            char save = *comma; *comma = 0;
            char one[CSS_SEL];
            bcopy_str(one, cur, CSS_SEL);
            css_trim(one);
            if (one[0]) {
                css_rule_t *r = css_rule_alloc();
                if (!r) break;
                bcopy_str(r->sel, one, CSS_SEL);
                r->spec = sel_spec(one);
                r->decl_off = doff;
                r->decl_len = dl;
                r->order = g_css_count - 1;
                r->flags = 0;
                if (find_substr_ci(one, ":hover")) r->flags |= RULE_F_HOVER;
                if (find_substr_ci(one, ":checked")) r->flags |= RULE_F_CHECKED;
                if (find_substr_ci(one, ":target")) r->flags |= RULE_F_TARGET;
                if (find_substr_ci(one, ":focus") || find_substr_ci(one, ":valid") || find_substr_ci(one, ":invalid"))
                    r->flags |= RULE_F_FOCUS;
                r->min_cw = (int16_t)min_cw;
                r->max_cw = (int16_t)max_cw;
                if (min_cw > 0 || max_cw > 0) r->flags |= RULE_F_CONT;
                css_extract_key(r);
            }
            if (!save) break;
            cur = comma + 1;
        }
        p = (*ce == '}') ? ce + 1 : ce;
    }
}

static void css_parse(const char *css) {
    if (!css || !css[0] || !g_basi || !g_basi->mem) return;
    int len = b_len(css);
    char *clean = (char *)g_basi->mem->alloc((size_t)len + 2);
    if (!clean) return;
    /* Strip all CSS comments respecting string literals */
    int o = 0; char inq = 0;
    for (int i = 0; i < len; i++) {
        char c = css[i];
        if (inq) {
            clean[o++] = c;
            if (c == '\\' && i + 1 < len) { clean[o++] = css[++i]; }
            else if (c == inq) inq = 0;
        } else {
            if (c == '"' || c == '\'') { inq = c; clean[o++] = c; }
            else if (c == '/' && i + 1 < len && css[i + 1] == '*') {
                i += 2;
                while (i + 1 < len && !(css[i] == '*' && css[i + 1] == '/')) i++;
                i++;
                clean[o++] = ' ';
            } else clean[o++] = c;
        }
    }
    clean[o] = 0;
    css_parse_d(clean, 0, 0, 0);
    g_basi->mem->free(clean);
}

/* ---------------- DOM Tree Builder (with #text nodes & full attributes) ---------------- */
static int dn_new(const char *tag) {
    if (g_dn_count >= g_dn_max || !g_basi || !g_basi->mem || !g_basi->mem->alloc) return -1;
    if (g_dn_count >= g_dn_cap) {
        int nc = g_dn_cap ? g_dn_cap * 2 : 512;
        if (nc > g_dn_max) nc = g_dn_max;
        dnode_t *na = (dnode_t *)g_basi->mem->alloc((size_t)nc * sizeof(dnode_t));
        if (!na) return -1;
        for (int k = 0; k < g_dn_count; k++) na[k] = g_dn[k];
        if (g_dn && g_basi->mem->free) g_basi->mem->free(g_dn);
        g_dn = na;
        g_dn_cap = nc;
    }
    int i = g_dn_count++;
    dnode_t *d = &g_dn[i];
    if (g_basi && g_basi->mem && g_basi->mem->memset)
        g_basi->mem->memset(d, 0, sizeof(*d));
    else {
        uint8_t *z = (uint8_t *)d;
        for (unsigned k = 0; k < sizeof(*d); k++) z[k] = 0;
    }
    d->wa = -1; d->ha = -1; d->align_attr = -1; d->form_id = -1;
    d->parent = -1; d->first = -1; d->last = -1; d->next = -1;
    bcopy_str(d->tag, tag, sizeof(d->tag));
    return i;
}

static void dn_add(int par, int ch) {
    if (par < 0 || ch < 0) return;
    g_dn[ch].parent = par;
    if (g_dn[par].last < 0) { g_dn[par].first = ch; g_dn[par].last = ch; }
    else { g_dn[g_dn[par].last].next = ch; g_dn[par].last = ch; }
}

static void dn_unlink(int ch) {
    int p = g_dn[ch].parent;
    if (p < 0) return;
    int prev = -1, guard = 0;
    for (int c = g_dn[p].first; c >= 0 && guard++ < g_dn_max; c = g_dn[c].next) {
        if (c == ch) break;
        prev = c;
    }
    if (g_dn[p].first == ch) g_dn[p].first = g_dn[ch].next;
    else if (prev >= 0) g_dn[prev].next = g_dn[ch].next;
    if (g_dn[p].last == ch) g_dn[p].last = prev;
    g_dn[ch].parent = -1;
    g_dn[ch].next = -1;
}

static int dn_is_ancestor(int a, int b) {
    int guard = 0;
    for (int x = b; x >= 0 && guard++ < g_dn_max; x = g_dn[x].parent)
        if (x == a) return 1;
    return 0;
}

static int dom_entity(const char *p, int maxlen, uint32_t *cp) {
    if (maxlen < 3 || p[0] != '&') return 0;
    if (p[1] == '#') {
        uint32_t v = 0; int i = 2, hex = 0;
        if (i < maxlen && (p[i] == 'x' || p[i] == 'X')) { hex = 1; i++; }
        int start = i;
        while (i < maxlen && p[i] != ';') {
            int d; char c = p[i];
            if (c >= '0' && c <= '9') d = c - '0';
            else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return 0;
            v = v * (hex ? 16 : 10) + d; i++;
        }
        if (i >= maxlen || p[i] != ';' || i == start) return 0;
        *cp = v; return i + 1;
    }
    static const struct { const char *name; uint32_t cp; } tbl[] = {
        {"amp;", '&'}, {"lt;", '<'}, {"gt;", '>'}, {"quot;", '"'},
        {"apos;", '\''}, {"nbsp;", ' '}, {"mdash;", 0x2014}, {"ndash;", 0x2013},
        {"hellip;", 0x2026}, {"rsquo;", 0x2019}, {"lsquo;", 0x2018},
        {"ldquo;", 0x201C}, {"rdquo;", 0x201D}, {"copy;", 0x00A9},
        {"reg;", 0x00AE}, {"trade;", 0x2122}, {"middot;", 0x00B7},
        {"deg;", 0x00B0}, {"euro;", 0x20AC}, {"pound;", 0x00A3},
        {"times;", 0x00D7}, {"minus;", 0x2212}, {"rarr;", 0x2192},
    };
    for (unsigned t = 0; t < sizeof(tbl) / sizeof(tbl[0]); t++) {
        int n = b_len(tbl[t].name);
        if (n >= maxlen) continue;
        int ok = 1;
        for (int j = 0; j < n; j++) if (p[j + 1] != tbl[t].name[j]) { ok = 0; break; }
        if (ok) { *cp = tbl[t].cp; return n + 1; }
    }
    return 0;
}

static void utf8_emit(char *dst, int *len, int cap, uint32_t cp) {
    if (cp == 0) return;
    if (cp < 0x80) { if (*len < cap - 1) dst[(*len)++] = (char)cp; }
    else if (cp < 0x800) {
        if (*len < cap - 2) { dst[(*len)++] = (char)(0xC0|(cp>>6)); dst[(*len)++] = (char)(0x80|(cp&0x3F)); }
    } else if (cp < 0x10000) {
        if (*len < cap - 3) {
            dst[(*len)++] = (char)(0xE0|(cp>>12));
            dst[(*len)++] = (char)(0x80|((cp>>6)&0x3F));
            dst[(*len)++] = (char)(0x80|(cp&0x3F));
        }
    } else {
        if (*len < cap - 4) {
            dst[(*len)++] = (char)(0xF0|(cp>>18));
            dst[(*len)++] = (char)(0x80|((cp>>12)&0x3F));
            dst[(*len)++] = (char)(0x80|((cp>>6)&0x3F));
            dst[(*len)++] = (char)(0x80|(cp&0x3F));
        }
    }
}

static void dn_decode(const char *in, char *out, int cap) {
    int o = 0, i = 0, len = b_len(in);
    while (i < len && o < cap - 4) {
        if (in[i] == '&') {
            uint32_t cp = 0;
            int adv = dom_entity(in + i, len - i, &cp);
            if (adv > 0 && cp) { utf8_emit(out, &o, cap, cp); i += adv; continue; }
        }
        out[o++] = in[i++];
    }
    out[o] = 0;
}

static void dn_text(int n, const char *s, int len) {
    dnode_t *d = &g_dn[n];
    int l = b_len(d->text);
    for (int i = 0; i < len && l < CSS_NTEXT - 4; i++) {
        char c = s[i];
        if (c == '&') {
            uint32_t cp = 0;
            int adv = dom_entity(s + i, len - i, &cp);
            if (adv > 0) { utf8_emit(d->text, &l, CSS_NTEXT, cp); i += adv - 1; continue; }
        }
        if (c == '\n' || c == '\t' || c == '\r') c = ' ';
        if (c == ' ' && l > 0 && d->text[l - 1] == ' ') continue;
        d->text[l++] = c;
    }
    d->text[l] = 0;
}

/* Check boolean attribute presence inside <tag ...> */
static int find_bool_attr(const char *tag_start, const char *tag_end, const char *name) {
    int nl = b_len(name);
    const char *p = tag_start;
    while (p < tag_end && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
    while (p < tag_end) {
        while (p < tag_end && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r'||*p=='/')) p++;
        const char *ns = p;
        while (p < tag_end && *p!='=' && *p!=' ' && *p!='\t' && *p!='\n' && *p!='\r' && *p!='>' && *p!='/') p++;
        int tl = (int)(p - ns);
        while (p < tag_end && (*p==' '||*p=='\t')) p++;
        int has_val = (*p == '=');
        if (has_val) {
            p++;
            while (p < tag_end && (*p==' '||*p=='\t')) p++;
            if (p < tag_end && (*p == '"' || *p == '\'')) {
                char q = *p++;
                while (p < tag_end && *p != q) p++;
                if (p < tag_end) p++;
            } else {
                while (p < tag_end && *p!=' ' && *p!='\t' && *p!='\n' && *p!='\r' && *p!='>') p++;
            }
        }
        if (tl == nl && strn_ci_eq(ns, name, nl)) return 1;
    }
    return 0;
}

static int dn_void(const char *t) {
    return str_eq(t, "img") || str_eq(t, "br") || str_eq(t, "input") ||
           str_eq(t, "meta") || str_eq(t, "link") || str_eq(t, "hr") ||
           str_eq(t, "wbr") || str_eq(t, "source") || str_eq(t, "col") ||
           str_eq(t, "area") || str_eq(t, "base") || str_eq(t, "embed");
}

/* Etiket icindeki tum data-* niteliklerini extra_attrs'a kaydeder (orn. GitHub data-color-mode) */
static void dn_collect_data_attrs(dnode_t *dn, const char *ts, const char *te) {
    const char *p = ts;
    while (p < te && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
    while (p < te) {
        while (p < te && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '/')) p++;
        if (p >= te) break;
        const char *ns = p;
        while (p < te && *p != '=' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '>') p++;
        int nl = (int)(p - ns);
        const char *vs = 0; int vl = 0;
        while (p < te && (*p == ' ' || *p == '\t')) p++;
        if (p < te && *p == '=') {
            p++;
            while (p < te && (*p == ' ' || *p == '\t')) p++;
            if (p < te && (*p == '"' || *p == '\'')) {
                char q = *p++; vs = p;
                while (p < te && *p != q) p++;
                vl = (int)(p - vs);
                if (p < te && *p == q) p++;
            } else {
                vs = p;
                while (p < te && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '>') p++;
                vl = (int)(p - vs);
            }
        }
        if (nl > 5 && strn_ci_eq(ns, "data-", 5) && nl < 48) {
            char nm[48]; for (int i = 0; i < nl; i++) nm[i] = lower_c(ns[i]); nm[nl] = 0;
            if (!str_eq(nm, "data-category") && !str_eq(nm, "data-tooltip")) {
                char val[80]; int cv = vl < 79 ? vl : 79;
                for (int i = 0; i < cv; i++) val[i] = vs ? vs[i] : 0;
                val[cv] = 0;
                if (dn->extra_attrs[0]) bcat(dn->extra_attrs, "\x1f", sizeof(dn->extra_attrs));
                bcat(dn->extra_attrs, nm, sizeof(dn->extra_attrs));
                bcat(dn->extra_attrs, "=", sizeof(dn->extra_attrs));
                bcat(dn->extra_attrs, val, sizeof(dn->extra_attrs));
            }
        }
    }
}

static void dom_parse(const char *html) {
    g_dn_count = 0;
    g_pform_count = 0;
    g_script_len = 0;
    g_svg_blob_len = 0;
    int root = dn_new("html");
    int st[80];
    int sp = 1;
    st[0] = root;
    int cur_form = -1;
    const char *p = html;
    while (*p) {
        if (*p == '<') {
            const char *tag_start = p;
            if (p[1] == '!' && p[2] == '-' && p[3] == '-') {
                const char *ec = find_substr_ci(p + 4, "-->");
                p = ec ? ec + 3 : p + 4;
                continue;
            }
            const char *q = p + 1;
            char inq = 0;
            while (*q) {
                if (inq) { if (*q == inq) inq = 0; }
                else { if (*q == '"' || *q == '\'') inq = *q; else if (*q == '>') break; }
                q++;
            }
            const char *tag_end = q;
            if (p[1] == '/') {
                char nm[16];
                get_tag_name(p + 1, nm, sizeof(nm));
                if (str_eq(nm, "form")) cur_form = -1;
                for (int i = sp - 1; i >= 1; i--) {
                    if (str_eq(g_dn[st[i]].tag, nm)) { sp = i; break; }
                }
                p = (*q == '>') ? q + 1 : q;
                continue;
            }
            if (p[1] == '!' || p[1] == '?') { p = (*q == '>') ? q + 1 : q; continue; }
            char nm[16];
            get_tag_name(p + 1, nm, sizeof(nm));
            if (nm[0]) {
                if (str_eq(nm, "html")) {
                    char a[200];
                    if (find_attr(tag_start, tag_end, "class", a, sizeof(a))) bcopy_str(g_dn[root].cls, a, sizeof(g_dn[root].cls));
                    if (find_attr(tag_start, tag_end, "style", a, sizeof(a))) bcopy_str(g_dn[root].istyle, a, sizeof(g_dn[root].istyle));
                    dn_collect_data_attrs(&g_dn[root], tag_start, tag_end);
                    p = (*q == '>') ? q + 1 : q;
                    continue;
                }
                if (str_eq(nm, "script")) {
                    char sa[260], stype[64];
                    const char *c = find_substr_ci(q, "</script");
                    int is_js = 1;
                    if (find_attr(tag_start, tag_end, "type", stype, sizeof(stype)) && stype[0]) {
                        if (!find_substr_ci(stype, "javascript") && !str_eq_ci(stype, "module") &&
                            !find_substr_ci(stype, "ecmascript")) is_js = 0;
                    }
                    if (is_js && find_attr(tag_start, tag_end, "src", sa, sizeof(sa)) && sa[0]) {
                        char abs[440]; resolve_url(g_cur_scheme, g_cur_host, sa, abs, sizeof(abs));
                        int ri = abs[0] ? res_find(abs) : -1;
                        if (ri >= 0 && g_res[ri].data && g_res[ri].len > 0) {
                            int n = (int)g_res[ri].len;
                            if (g_script_len + n + 2 < (int)sizeof(g_script_src)) {
                                for (int k = 0; k < n; k++) g_script_src[g_script_len++] = (char)g_res[ri].data[k];
                                g_script_src[g_script_len++] = '\n';
                            }
                        }
                    } else if (is_js && c) {
                        int n = (int)(c - (q + 1));
                        if (n > 0 && g_script_len + n + 2 < (int)sizeof(g_script_src)) {
                            for (int k = 0; k < n; k++) g_script_src[g_script_len++] = q[1 + k];
                            g_script_src[g_script_len++] = '\n';
                        }
                    }
                    if (c) { const char *ce = c; while (*ce && *ce != '>') ce++; p = (*ce == '>') ? ce + 1 : ce; }
                    else p = (*q == '>') ? q + 1 : q;
                    continue;
                }
                if (str_eq(nm, "svg")) {
                    const char *c = find_substr_ci(q, "</svg");
                    const char *ce = c ? c : q;
                    while (*ce && *ce != '>') ce++;
                    const char *end = (*ce == '>') ? ce + 1 : ce;
                    int sl = (int)(end - tag_start);
                    if (sl > 0 && sl < 60000 && g_svg_blob_len + sl + 1 < SVG_BLOB_MAX) {
                        int idx = dn_new("svg");
                        if (idx >= 0) {
                            dn_add(st[sp - 1], idx);
                            g_dn[idx].svg_off = g_svg_blob_len;
                            g_dn[idx].svg_len = sl;
                            for (int k = 0; k < sl; k++) g_svg_blob[g_svg_blob_len++] = tag_start[k];
                            g_svg_blob[g_svg_blob_len++] = 0;
                            char a[200];
                            if (find_attr(tag_start, tag_end, "class", a, sizeof(a))) bcopy_str(g_dn[idx].cls, a, sizeof(g_dn[idx].cls));
                            if (find_attr(tag_start, tag_end, "id", a, sizeof(a))) bcopy_str(g_dn[idx].id, a, sizeof(g_dn[idx].id));
                            if (find_attr(tag_start, tag_end, "style", a, sizeof(a))) bcopy_str(g_dn[idx].istyle, a, sizeof(g_dn[idx].istyle));
                            if (find_attr(tag_start, tag_end, "width", a, sizeof(a))) g_dn[idx].wa = css_num(a, &g_dn[idx].wa_pct);
                            if (find_attr(tag_start, tag_end, "height", a, sizeof(a))) g_dn[idx].ha = css_num(a, &g_dn[idx].ha_pct);
                        }
                    }
                    p = end;
                    continue;
                }
                if (str_eq(nm, "head") || str_eq(nm, "meta") || str_eq(nm, "link") || str_eq(nm, "base")) {
                    p = (*q == '>') ? q + 1 : q;
                    continue;
                }
                if (str_eq(nm, "style") || str_eq(nm, "title") ||
                    str_eq(nm, "noscript") || str_eq(nm, "template") ||
                    str_eq(nm, "object") || str_eq(nm, "video") ||
                    str_eq(nm, "audio") || str_eq(nm, "map")) {
                    char close[24]; bcopy_str(close, "</", sizeof(close)); bcat(close, nm, sizeof(close));
                    const char *c = find_substr_ci(q, close);
                    if (c) { const char *ce = c; while (*ce && *ce != '>') ce++; p = (*ce == '>') ? ce + 1 : ce; }
                    else p = (*q == '>') ? q + 1 : q;
                    continue;
                }
                int idx = dn_new(nm);
                if (idx >= 0) {
                    dn_add(st[sp - 1], idx);
                    dnode_t *dn = &g_dn[idx];
                    char a[540];
                    if (find_attr(tag_start, tag_end, "class", a, sizeof(a))) bcopy_str(dn->cls, a, sizeof(dn->cls));
                    if (find_attr(tag_start, tag_end, "id", a, sizeof(a))) bcopy_str(dn->id, a, sizeof(dn->id));
                    if (find_attr(tag_start, tag_end, "style", a, sizeof(a))) bcopy_str(dn->istyle, a, sizeof(dn->istyle));
                    if (find_attr(tag_start, tag_end, "src", a, sizeof(a))) bcopy_str(dn->src, a, sizeof(dn->src));
                    if (find_attr(tag_start, tag_end, "href", a, sizeof(a))) bcopy_str(dn->href, a, sizeof(dn->href));
                    if (find_attr(tag_start, tag_end, "name", a, sizeof(a))) bcopy_str(dn->name, a, sizeof(dn->name));
                    if (find_attr(tag_start, tag_end, "type", a, sizeof(a))) bcopy_str(dn->type, a, sizeof(dn->type));
                    if (find_attr(tag_start, tag_end, "value", a, sizeof(a))) dn_decode(a, dn->value, sizeof(dn->value));
                    if (find_attr(tag_start, tag_end, "placeholder", a, sizeof(a))) dn_decode(a, dn->ph, sizeof(dn->ph));
                    else if (find_attr(tag_start, tag_end, "title", a, sizeof(a))) dn_decode(a, dn->ph, sizeof(dn->ph));
                    if (find_attr(tag_start, tag_end, "alt", a, sizeof(a))) dn_decode(a, dn->alt, sizeof(dn->alt));
                    if (find_attr(tag_start, tag_end, "for", a, sizeof(a))) bcopy_str(dn->for_attr, a, sizeof(dn->for_attr));
                    if (find_attr(tag_start, tag_end, "data-category", a, sizeof(a))) bcopy_str(dn->data_cat, a, sizeof(dn->data_cat));
                    if (find_attr(tag_start, tag_end, "data-tooltip", a, sizeof(a))) dn_decode(a, dn->data_tip, sizeof(dn->data_tip));
                    if (find_attr(tag_start, tag_end, "rows", a, sizeof(a))) dn->rows = (int)css_f(a);
                    if (find_bool_attr(tag_start, tag_end, "checked")) dn->checked = 1;
                    if (find_bool_attr(tag_start, tag_end, "open")) dn->is_open = 1;
                    if (find_bool_attr(tag_start, tag_end, "required")) dn->required = 1;
                    if (find_bool_attr(tag_start, tag_end, "hidden")) dn->hidden_attr = 1;
                    dn_collect_data_attrs(dn, tag_start, tag_end);
                    /* Satirici JS olay & download niteliklerini extra_attrs'a sakla */
                    {
                        static const char *ev_attrs[] = { "onclick", "oninput", "onchange", "onsubmit", "download", 0 };
                        for (int ei = 0; ev_attrs[ei]; ei++) {
                            char ea[140];
                            if (find_attr(tag_start, tag_end, ev_attrs[ei], ea, sizeof(ea)) ||
                                (str_eq(ev_attrs[ei], "download") && find_bool_attr(tag_start, tag_end, "download") && (ea[0]='1', ea[1]=0, 1))) {
                                if (dn->extra_attrs[0]) bcat(dn->extra_attrs, "\x1f", sizeof(dn->extra_attrs));
                                bcat(dn->extra_attrs, ev_attrs[ei], sizeof(dn->extra_attrs));
                                bcat(dn->extra_attrs, "=", sizeof(dn->extra_attrs));
                                bcat(dn->extra_attrs, ea, sizeof(dn->extra_attrs));
                            }
                        }
                    }
                    if (find_attr(tag_start, tag_end, "width", a, sizeof(a))) dn->wa = css_num(a, &dn->wa_pct);
                    if (find_attr(tag_start, tag_end, "height", a, sizeof(a))) dn->ha = css_num(a, &dn->ha_pct);
                    if (find_attr(tag_start, tag_end, "align", a, sizeof(a))) {
                        if (str_eq_ci(a, "center")) dn->align_attr = 1;
                        else if (str_eq_ci(a, "right")) dn->align_attr = 2;
                        else dn->align_attr = 0;
                    }
                    if (find_attr(tag_start, tag_end, "bgcolor", a, sizeof(a))) {
                        uint32_t c; if (css_color(a, &c)) { dn->has_bgc = 1; dn->bgc = c; }
                    }
                    dn->form_id = cur_form;
                    if (str_eq(nm, "form") && g_pform_count < 24) {
                        pform_t *f = &g_pforms[g_pform_count];
                        if (find_attr(tag_start, tag_end, "action", a, sizeof(a))) bcopy_str(f->action, a, sizeof(f->action));
                        else f->action[0] = 0;
                        if (find_attr(tag_start, tag_end, "method", a, sizeof(a))) bcopy_str(f->method, a, sizeof(f->method));
                        else bcopy_str(f->method, "get", sizeof(f->method));
                        f->node = idx;
                        dn->form_id = g_pform_count;
                        cur_form = g_pform_count;
                        g_pform_count++;
                    }
                    if (str_eq(nm, "br") && sp >= 1) {
                        int tn = dn_new("#text");
                        if (tn >= 0) { g_dn[tn].is_text = 1; bcopy_str(g_dn[tn].text, "\n", CSS_NTEXT); dn_add(st[sp - 1], tn); }
                    }
                    int selfclose = (tag_end > tag_start + 1 && tag_end[-1] == '/');
                    if (!dn_void(nm) && !selfclose && sp < 79) st[sp++] = idx;
                }
            }
            p = (*q == '>') ? q + 1 : q;
            continue;
        }
        const char *ts = p;
        while (*p && *p != '<') p++;
        int tlen = (int)(p - ts);
        /* Check if text has any non-whitespace character */
        int has_vis = 0;
        for (int k = 0; k < tlen; k++) {
            char c = ts[k];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') { has_vis = 1; break; }
        }
        if (has_vis) {
            int par = st[sp - 1];
            int tn = dn_new("#text");
            if (tn >= 0) {
                g_dn[tn].is_text = 1;
                dn_text(tn, ts, tlen);
                dn_add(par, tn);
            }
            dn_text(par, ts, tlen);
        }
    }
}

/* ---------------- Style Defaults & Cascade ---------------- */
static void style_defaults(css_style_t *s) {
    if (g_basi && g_basi->mem && g_basi->mem->memset)
        g_basi->mem->memset(s, 0, sizeof(*s));
    else {
        uint8_t *z = (uint8_t *)s;
        for (unsigned i = 0; i < sizeof(*s); i++) z[i] = 0;
    }
    s->color = 0x202124; s->color_a = 255;
    s->bg = 0xFFFFFF; s->bg_a = 255;
    s->font_size = 16;
    s->border_c = 0xDDDDDD; s->border_a = 255;
    s->outline_a = 255;
    s->opacity = 100;
    s->shadow = -1;
    s->bg_repeat = -1;
    s->tscale = 256; s->tscalex = 256; s->tscaley = 256;
    s->col_span = 1; s->row_span = 1;
    s->flt_contrast = 1.0f; s->flt_bright = 1.0f; s->flt_sat = 1.0f;
}

static uint32_t css_idx_hash(const char *s, int len) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < len && s[i]; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

static void css_build_index(void) {
    for (int i = 0; i < CSS_IDX_CLS_SZ; i++) g_css_cls_ht[i] = -1;
    for (int i = 0; i < CSS_IDX_TAG_SZ; i++) g_css_tag_ht[i] = -1;
    for (int i = 0; i < CSS_IDX_ID_SZ; i++) g_css_id_ht[i] = -1;
    g_css_univ_head = -1;
    for (int i = g_css_count - 1; i >= 0; i--) {
        css_rule_t *r = &g_css[i];
        if (r->key_id[0]) {
            uint32_t b = css_idx_hash(r->key_id, 64) & (CSS_IDX_ID_SZ - 1);
            r->idx_next = g_css_id_ht[b];
            g_css_id_ht[b] = i;
        } else if (r->key_cls[0]) {
            uint32_t b = css_idx_hash(r->key_cls, r->key_cls_len) & (CSS_IDX_CLS_SZ - 1);
            r->idx_next = g_css_cls_ht[b];
            g_css_cls_ht[b] = i;
        } else if (r->key_tag[0]) {
            uint32_t b = css_idx_hash(r->key_tag, 24) & (CSS_IDX_TAG_SZ - 1);
            r->idx_next = g_css_tag_ht[b];
            g_css_tag_ht[b] = i;
        } else {
            r->idx_next = g_css_univ_head;
            g_css_univ_head = i;
        }
    }
    g_css_idx_built = 1;
}

static void css_match_bucket(int n, const dnode_t *d, int head, int *matched, int *cnt) {
    for (int i = head; i >= 0 && *cnt < 4096; i = g_css[i].idx_next) {
        const css_rule_t *r = &g_css[i];
        if (r->key_id[0] && !str_eq(d->id, r->key_id)) continue;
        if (r->key_cls[0] && !class_has(d->cls, r->key_cls, r->key_cls_len)) continue;
        if (r->key_tag[0] && !str_eq(d->tag, r->key_tag)) continue;
        if (r->flags & RULE_F_CONT) {
            if (r->min_cw > 0 && g_cur_cont_w < r->min_cw) continue;
            if (r->max_cw > 0 && g_cur_cont_w > r->max_cw) continue;
        }
        if (sel_match(n, r->sel)) matched[(*cnt)++] = i;
    }
}

static void compute_style(int n, css_style_t *ps, css_style_t *s, int pe, int depth) {
    int save_pe = g_pe;
    g_pe = pe;
    style_defaults(s);
    if (ps) {
        s->color = ps->color; s->has_color = 0; s->color_a = ps->color_a;
        s->font_size = ps->font_size;
        s->bold = ps->bold; s->italic = ps->italic; s->mono = ps->mono;
        s->underline = ps->underline; s->strike = ps->strike;
        s->align = ps->align;
        s->line_height = ps->line_height;
        s->letter_space = ps->letter_space;
        s->nowrap = ps->nowrap;
        s->transform = ps->transform;
        s->pointer_none = ps->pointer_none;
        if (ps->has_accent) { s->has_accent = 1; s->accent_c = ps->accent_c; }
    }
    g_inh_font = s->font_size;
    g_inh_color = s->color;

    const dnode_t *d = &g_dn[n];
    /* UA defaults applied BEFORE author stylesheet so author rules override cleanly */
    if (pe == 0) {
        if (d->hidden_attr) s->hide = 1;
        if (str_eq(d->tag, "h1")) { s->font_size = 32; s->bold = 1; }
        else if (str_eq(d->tag, "h2")) { s->font_size = 24; s->bold = 1; }
        else if (str_eq(d->tag, "h3")) { s->font_size = 19; s->bold = 1; }
        else if (str_eq(d->tag, "h4")) { s->font_size = 16; s->bold = 1; }
        else if (str_eq(d->tag, "b") || str_eq(d->tag, "strong")) s->bold = 1;
        else if (str_eq(d->tag, "i") || str_eq(d->tag, "em")) s->italic = 1;
        else if (str_eq(d->tag, "code") || str_eq(d->tag, "pre") || str_eq(d->tag, "kbd")) s->mono = 1;
        else if (str_eq(d->tag, "button")) { s->align = 1; }
        else if (str_eq(d->tag, "center")) { s->align = 1; }
        else if (str_eq(d->tag, "input") && (str_eq(d->type, "submit") || str_eq(d->type, "button") || str_eq(d->type, "reset"))) {
            s->display = 2; s->align = 1;
        }
        else if (str_eq(d->tag, "a")) { s->display = 1; if (!s->has_color) s->color = 0x60A5FA; }
        else if (str_eq(d->tag, "ul") || str_eq(d->tag, "ol")) { s->list_type = str_eq(d->tag, "ol") ? 4 : 3; }
        else if (str_eq(d->tag, "span") || str_eq(d->tag, "a") || str_eq(d->tag, "strong") ||
                 str_eq(d->tag, "b") || str_eq(d->tag, "em") || str_eq(d->tag, "i") ||
                 str_eq(d->tag, "label") || str_eq(d->tag, "code") || str_eq(d->tag, "kbd") ||
                 str_eq(d->tag, "small") || str_eq(d->tag, "abbr") || str_eq(d->tag, "mark") ||
                 str_eq(d->tag, "time") || str_eq(d->tag, "sub") || str_eq(d->tag, "sup")) {
            s->display = 1;
        }
    }

    if (!g_css_idx_built) css_build_index();
    static int matched[4096];
    int cnt = 0;
    /* O(1) hash-indexed bucket lookup: univ + tag + id + each unique class token */
    css_match_bucket(n, d, g_css_univ_head, matched, &cnt);
    if (d->tag[0]) {
        uint32_t tb = css_idx_hash(d->tag, 24) & (CSS_IDX_TAG_SZ - 1);
        css_match_bucket(n, d, g_css_tag_ht[tb], matched, &cnt);
    }
    if (d->id[0]) {
        uint32_t ib = css_idx_hash(d->id, 64) & (CSS_IDX_ID_SZ - 1);
        css_match_bucket(n, d, g_css_id_ht[ib], matched, &cnt);
    }
    if (d->cls[0]) {
        uint32_t seen_b[32]; int nseen = 0;
        const char *cp = d->cls;
        while (*cp) {
            while (*cp == ' ' || *cp == '\t' || *cp == '\n' || *cp == '\r') cp++;
            if (!*cp) break;
            const char *cs = cp;
            while (*cp && *cp != ' ' && *cp != '\t' && *cp != '\n' && *cp != '\r') cp++;
            int cl = (int)(cp - cs);
            uint32_t cb = css_idx_hash(cs, cl) & (CSS_IDX_CLS_SZ - 1);
            int dup = 0;
            for (int k = 0; k < nseen; k++) if (seen_b[k] == cb) { dup = 1; break; }
            if (!dup) {
                if (nseen < 32) seen_b[nseen++] = cb;
                css_match_bucket(n, d, g_css_cls_ht[cb], matched, &cnt);
            }
        }
    }
    /* Insertion sort by (spec, order) — exact CSS cascade order! */
    for (int i = 1; i < cnt; i++) {
        int key = matched[i];
        int ks = g_css[key].spec;
        int ko = g_css[key].order;
        int j = i - 1;
        while (j >= 0 && (g_css[matched[j]].spec > ks ||
                          (g_css[matched[j]].spec == ks && g_css[matched[j]].order > ko))) {
            matched[j + 1] = matched[j];
            j--;
        }
        matched[j + 1] = key;
    }

    /* Pass 0: custom properties (--*) */
    for (int i = 0; i < cnt; i++) {
        const css_rule_t *r = &g_css[matched[i]];
        css_apply_pass(s, g_css_pool + r->decl_off, r->decl_len, 0, depth);
    }
    if (pe == 0 && d->istyle[0])
        css_apply_pass(s, d->istyle, b_len(d->istyle), 0, depth);

    /* Pass 1: normal declarations */
    for (int i = 0; i < cnt; i++) {
        const css_rule_t *r = &g_css[matched[i]];
        css_apply_pass(s, g_css_pool + r->decl_off, r->decl_len, 1, depth);
    }
    if (pe == 0 && d->istyle[0])
        css_apply_pass(s, d->istyle, b_len(d->istyle), 1, depth);

    /* Pass 2: !important declarations */
    for (int i = 0; i < cnt; i++) {
        const css_rule_t *r = &g_css[matched[i]];
        css_apply_pass(s, g_css_pool + r->decl_off, r->decl_len, 2, depth);
    }
    if (pe == 0 && d->istyle[0])
        css_apply_pass(s, d->istyle, b_len(d->istyle), 2, depth);

    /* Handle border-radius: inherit */
    if (s->radius_inherit && ps) {
        s->radius = ps->radius; s->radius_pct = ps->radius_pct;
        s->has_corner_radius = ps->has_corner_radius;
        for (int k = 0; k < 4; k++) {
            s->rad_rx[k] = ps->rad_rx[k]; s->rad_ry[k] = ps->rad_ry[k];
            s->rad_rx_p[k] = ps->rad_rx_p[k]; s->rad_ry_p[k] = ps->rad_ry_p[k];
        }
    }

    /* HTML root font size update */
    if (pe == 0 && n == 0 && s->has_font && s->font_size >= 8) g_root_font = s->font_size;

    /* Presentational HTML attributes */
    if (pe == 0) {
        if (d->has_bgc && !s->has_bg) { s->bg = d->bgc; s->has_bg = 1; s->bg_transparent = 0; }
        if (d->align_attr >= 0) s->align = d->align_attr;
        if (d->wa >= 0 && !s->has_w) { s->has_w = 1; s->width = d->wa; s->w_pct = d->wa_pct; }
        if (d->ha >= 0 && !s->has_h) { s->has_h = 1; s->height = d->ha; s->h_pct = d->ha_pct; }
        if (str_eq(d->tag, "table")) { s->is_flex = 1; s->row = 0; }
        if (str_eq(d->tag, "tr")) { s->is_flex = 1; s->row = 1; }
    }
    g_pe = save_pe;
}

/* ---------------- Font Cache (Regular + Bold, up to 80px) ---------------- */
extern const unsigned char font_arimo[];
extern const unsigned int  font_arimo_len;

#define BFONT_MAX 36
static lv_font_t *g_bfont[BFONT_MAX];
static int        g_bsize[BFONT_MAX];
static int        g_bbold[BFONT_MAX];
static int        g_bn;

static lv_font_t *bfont_ex(int size, int bold) {
    if (size < 10) size = 10;
    if (size > 80) size = 80;
    /* Quantize large sizes to even numbers to save font slots */
    if (size > 28) size = (size + 1) & ~1;
    bold = bold ? 1 : 0;
    for (int i = 0; i < g_bn; i++)
        if (g_bsize[i] == size && g_bbold[i] == bold) return g_bfont[i];
    if (g_bn >= BFONT_MAX) {
        /* Pick closest existing font */
        int best = 0, bd = 9999;
        for (int i = 0; i < g_bn; i++) {
            int dd = (g_bsize[i] > size ? g_bsize[i] - size : size - g_bsize[i]) + (g_bbold[i] != bold ? 6 : 0);
            if (dd < bd) { bd = dd; best = i; }
        }
        return g_bfont[best];
    }
    lv_font_t *f = bold
        ? lv_tiny_ttf_create_data(font_arimo, font_arimo_len, size)
        : lv_tiny_ttf_create_data(font_ttf, font_ttf_len, size);
    if (!f) return g_bn ? g_bfont[0] : (lv_font_t *)LV_FONT_DEFAULT;
    g_bfont[g_bn] = f;
    g_bsize[g_bn] = size;
    g_bbold[g_bn] = bold;
    g_bn++;
    return f;
}

static lv_font_t *bfont(int size) { return bfont_ex(size, 0); }

/* ---------------- Tarayici kaplamasi (chrome) ---------------- */
#define CHROME_IC_MAX 16
static uint32_t *g_chrome_buf[CHROME_IC_MAX];
static int       g_chrome_n;

static lv_obj_t *chrome_icon(lv_obj_t *parent, const char *svg, int size) {
    if (g_chrome_n >= CHROME_IC_MAX || !g_basi || !g_basi->mem) return 0;
    uint32_t *buf = (uint32_t *)g_basi->mem->alloc((size_t)size * size * 4u);
    if (!buf) return 0;
    if (svg_render(svg, size, size, buf) != 0) { g_basi->mem->free(buf); return 0; }
    g_chrome_buf[g_chrome_n++] = buf;
    lv_obj_t *c = lv_canvas_create(parent);
    lv_obj_remove_style_all(c);
    lv_canvas_set_buffer(c, buf, size, size, LV_COLOR_FORMAT_ARGB8888);
    return c;
}

#define SVG_BACK   "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#A3A8B3\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M15 18l-6-6 6-6\"/></svg>"
#define SVG_FWD    "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#A3A8B3\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M9 18l6-6-6-6\"/></svg>"
#define SVG_RELOAD "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#A3A8B3\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M21 12a9 9 0 1 1-2.64-6.36\"/><path d=\"M21 3v6h-6\"/></svg>"
#define SVG_GO     "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#60A5FA\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M4 12h15M13 6l6 6-6 6\"/></svg>"
#define SVG_DL     "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#A3A8B3\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M12 4v11M7 10l5 5 5-5M5 20h14\"/></svg>"
#define SVG_STAR   "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#E8B95C\" stroke-width=\"1.6\" stroke-linejoin=\"round\"><path d=\"M12 3.6l2.5 5.1 5.6.8-4 4 .9 5.6-5-2.7-5 2.7.9-5.6-4-4 5.6-.8z\"/></svg>"
#define SVG_BM     "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#A3A8B3\" stroke-width=\"2\" stroke-linecap=\"round\"><path d=\"M6 5h12M6 12h12M6 19h8\"/></svg>"
#define SVG_DLM    "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#A3A8B3\" stroke-width=\"1.8\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M4 13h4l2 3h4l2-3h4M4 13V7a2 2 0 0 1 2-2h12a2 2 0 0 1 2 2v6M4 13v5a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-5\"/></svg>"
#define SVG_HIST     "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#A3A8B3\" stroke-width=\"1.8\" stroke-linecap=\"round\"><circle cx=\"12\" cy=\"12\" r=\"8\"/><path d=\"M12 7v5l3 2\"/></svg>"
#define SVG_FIND     "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#A3A8B3\" stroke-width=\"1.8\" stroke-linecap=\"round\"><circle cx=\"11\" cy=\"11\" r=\"6.5\"/><path d=\"M16 16l4.5 4.5\"/></svg>"
#define SVG_DEVTOOLS "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#A3A8B3\" stroke-width=\"1.8\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><rect x=\"3\" y=\"4\" width=\"18\" height=\"16\" rx=\"2.5\"/><path d=\"M7.5 10l3 2.5-3 2.5\"/><path d=\"M12.5 15h4\"/></svg>"

static void back_cb(lv_event_t *e);
static void forward_cb(lv_event_t *e);
static void reload_cb(lv_event_t *e);
static void go_cb(lv_event_t *e);
static void download_cb(lv_event_t *e);
static void bm_star_cb(lv_event_t *e);
static void bm_btn_cb(lv_event_t *e);
static void hist_btn_cb(lv_event_t *e);
static void newtab_cb(lv_event_t *e);

static void chrome_btn(lv_obj_t *parent, const char *svg, lv_event_cb_t cb, int bordered) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 30, 30);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1E2129), LV_STATE_HOVERED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_HOVERED);
    lv_obj_set_style_border_width(b, bordered ? 1 : 0, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x2C313B), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_t *ic = chrome_icon(b, svg, 16);
    if (ic) lv_obj_center(ic);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
}

/* ---------------- Ag kaynaklari (dinamik heap, CSS limiti yok) ---------------- */
static void res_clear(void) {
    for (int i = 0; i < g_res_count; i++) {
        if (g_res && g_res[i].data && g_basi && g_basi->mem) g_basi->mem->free(g_res[i].data);
        if (g_res) { g_res[i].data = 0; g_res[i].len = 0; g_res[i].url[0] = 0; g_res[i].is_css = 0; }
    }
    g_res_count = 0;
}

static int res_find(const char *url) {
    for (int i = 0; i < g_res_count; i++) if (str_eq(g_res[i].url, url)) return i;
    return -1;
}

static int res_add_owned(const char *url, uint8_t *d, uint32_t len, int is_css) {
    if (!g_basi || !g_basi->mem || !g_basi->mem->alloc || !d) {
        if (d && g_basi && g_basi->mem && g_basi->mem->free) g_basi->mem->free(d);
        return -1;
    }
    if (g_res_count >= g_res_cap) {
        int nc = g_res_cap ? g_res_cap * 2 : 64;
        res_ent_t *na = (res_ent_t *)g_basi->mem->alloc((size_t)nc * sizeof(res_ent_t));
        if (!na) { if (g_basi->mem->free) g_basi->mem->free(d); return -1; }
        for (int i = 0; i < g_res_count; i++) na[i] = g_res[i];
        if (g_res && g_basi->mem->free) g_basi->mem->free(g_res);
        g_res = na;
        g_res_cap = nc;
    }
    bcopy_str(g_res[g_res_count].url, url, sizeof(g_res[0].url));
    g_res[g_res_count].data = d;
    g_res[g_res_count].len = len;
    g_res[g_res_count].is_css = is_css;
    g_res_count++;
    return g_res_count - 1;
}

static int res_add(const char *url, const char *body, uint32_t len, int is_css) {
    if (!g_basi || !g_basi->mem || !g_basi->mem->alloc) return -1;
    if (len > g_res_item_cap) len = g_res_item_cap;
    uint8_t *d = g_basi->mem->alloc((size_t)len + 1);
    if (!d) return -1;
    for (uint32_t i = 0; i < len; i++) d[i] = (uint8_t)body[i];
    d[len] = 0;
    return res_add_owned(url, d, len, is_css);
}

static void fetch_resource(const char *url, int is_css) {
    if (!g_basi || !g_basi->net) return;
    if (!url[0] || res_find(url) >= 0) return;
    uint32_t len = 0;
    const char *b = 0;
    if (starts_with(url, "https://")) b = g_basi->net->https_get(url, &len);
    else if (starts_with(url, "http://")) b = g_basi->net->http_get(url, &len);
    else return;
    if (!b || len == 0) return;
    res_add(url, b, len, is_css);
}

static void collect_resources(const char *html, const char *scheme, const char *host) {
    if (!g_basi || !g_basi->net || !html) return;
    int has_t = (g_basi->time && g_basi->time->ticks_ms) ? 1 : 0;
    uint64_t t0 = has_t ? g_basi->time->ticks_ms() : 0;

    /* 1. Gecis: TUM harici CSS dosyalari (yapay limit YOK!) */
    const char *p = html;
    while (*p) {
        const char *tl = find_substr_ci(p, "<link");
        if (!tl) break;
        const char *q = tl; while (*q && *q != '>') q++;
        char rel[64], href[420];
        if (find_attr(tl, q, "rel", rel, sizeof(rel)) && find_substr_ci(rel, "stylesheet") &&
            find_attr(tl, q, "href", href, sizeof(href)) && href[0]) {
            char abs[440]; resolve_url(scheme, host, href, abs, sizeof(abs));
            if (abs[0]) fetch_resource(abs, 1);
        }
        p = (*q == '>') ? q + 1 : q;
    }

    /* 2. Gecis: Harici JS betikleri ve gorseller */
    p = html;
    int imgs = 0, jsn = 0;
    while (*p) {
        if (has_t && g_basi->time->ticks_ms() - t0 > 45000) break;
        const char *ti = find_substr_ci(p, "<img");
        const char *ts = find_substr_ci(p, "<script");
        const char *next = ti;
        if (ts && (!next || ts < next)) next = ts;
        if (!next) break;
        const char *q = next; while (*q && *q != '>') q++;
        char a[420];
        if (next == ti && imgs < 32) {
            if (find_attr(next, q, "src", a, sizeof(a)) && !starts_with(a, "data:")) {
                char abs[440]; resolve_url(scheme, host, a, abs, sizeof(abs));
                if (abs[0]) { fetch_resource(abs, 0); imgs++; }
            }
        } else if (next == ts && jsn < 24) {
            char stype[48];
            if (find_attr(next, q, "type", stype, sizeof(stype)) && str_eq_ci(stype, "module")) {
                /* ES module bundler paketlerini (orn. 2.4 MB React chunk) atla */
            } else if (find_attr(next, q, "src", a, sizeof(a)) && a[0]) {
                char abs[440]; resolve_url(scheme, host, a, abs, sizeof(abs));
                if (abs[0]) { fetch_resource(abs, 2); jsn++; }
            }
        }
        p = (*q == '>') ? q + 1 : q;
    }
}

/* ---------------- Resim + HTML5 <canvas> 2D + CSS3 Yazilim Boyama Motoru ---------------- */
typedef struct {
    int       dom_idx;
    bc_ctx_t  bc;
    lv_obj_t *lv_c;
} bcanvas_slot_t;
#define BCANVAS_MAX 8
static bcanvas_slot_t g_canvases[BCANVAS_MAX];
static int            g_canvas_n;
static int            g_zoom_pct = 100;
static lv_obj_t      *s_find_bar;
static lv_obj_t      *s_find_ta;
static lv_obj_t      *s_find_lbl;
static lv_obj_t      *s_find_hits[64];
static int            s_find_n;
static int            s_find_cur;
static uint32_t *img_alloc_extra(int w, int h);

static bcanvas_slot_t *bcanvas_get(int dom_idx, int def_w, int def_h) {
    for (int i = 0; i < g_canvas_n; i++) {
        if (g_canvases[i].dom_idx == dom_idx) return &g_canvases[i];
    }
    if (g_canvas_n >= BCANVAS_MAX) return 0;
    int cw = def_w > 0 ? def_w : 300;
    int ch = def_h > 0 ? def_h : 150;
    if (cw > 1024) cw = 1024;
    if (ch > 1024) ch = 1024;
    uint32_t *px = img_alloc_extra(cw, ch);
    if (!px) return 0;
    for (size_t k = 0; k < (size_t)cw * (size_t)ch; k++) px[k] = 0x00000000u;
    bcanvas_slot_t *sl = &g_canvases[g_canvas_n++];
    sl->dom_idx = dom_idx;
    sl->lv_c = 0;
    bc_init(&sl->bc, px, cw, ch);
    return sl;
}

static void images_clear(void) {
    g_canvas_n = 0;
    s_find_n = 0; s_find_cur = -1;
    for (int i = 0; i < g_img_count; i++) {
        if (g_imgs[i].px && g_basi && g_basi->mem) g_basi->mem->free(g_imgs[i].px);
        g_imgs[i].px = 0; g_imgs[i].url[0] = 0; g_imgs[i].w = g_imgs[i].h = 0;
    }
    g_img_count = 0;
    for (int i = 0; i < g_img_extra_n; i++)
        if (g_img_extra[i] && g_basi && g_basi->mem) g_basi->mem->free(g_img_extra[i]);
    g_img_extra_n = 0;
    g_bg_dsc_n = 0;
    g_anim_n = 0;
    g_sticky_obj = 0;
}

static uint32_t *img_alloc_extra(int w, int h) {
    if (w <= 0 || h <= 0 || w > 1800 || h > 1600 || !g_basi || !g_basi->mem || g_img_extra_n >= IMG_EXTRA)
        return 0;
    uint32_t *px = (uint32_t *)g_basi->mem->alloc((size_t)w * (size_t)h * 4u);
    if (!px) return 0;
    g_img_extra[g_img_extra_n++] = px;
    return px;
}

static lv_image_dsc_t *img_make_dsc(const uint32_t *px, int w, int h) {
    if (!px || w <= 0 || h <= 0 || g_bg_dsc_n >= BG_DSC_MAX) return 0;
    lv_image_dsc_t *d = &g_bg_dsc[g_bg_dsc_n++];
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_ARGB8888;
    d->header.flags = 0;
    d->header.w = (uint32_t)w;
    d->header.h = (uint32_t)h;
    d->header.stride = (uint32_t)(w * 4);
    d->header.reserved_2 = 0;
    d->data_size = (uint32_t)((size_t)w * (size_t)h * 4u);
    d->data = (const uint8_t *)px;
    d->reserved = 0;
    d->reserved_2 = 0;
    return d;
}

static uint32_t *img_get(const char *url, int *out_w, int *out_h) {
    *out_w = 0; *out_h = 0;
    for (int i = 0; i < g_img_count; i++)
        if (str_eq(g_imgs[i].url, url)) { *out_w = g_imgs[i].w; *out_h = g_imgs[i].h; return g_imgs[i].px; }
    if (g_img_count >= IMG_MAX || !g_basi || !g_basi->mem) return 0;
    int ri = res_find(url);
    if (ri < 0 || !g_res[ri].data || g_res[ri].is_css) return 0;
    uint32_t *px = 0; int w = 0, h = 0;
    const uint8_t *rd = g_res[ri].data;
    uint32_t rl = g_res[ri].len;
    if (browser_img_decode(rd, rl, g_basi->mem->alloc, g_basi->mem->free, &px, &w, &h) != 0 || !px) {
        /* Harici .svg gorsel (<img src="...svg">) destegi */
        uint32_t sp = 0;
        while (sp < rl && (rd[sp] == ' ' || rd[sp] == '\t' || rd[sp] == '\r' || rd[sp] == '\n')) sp++;
        if (sp + 4 < rl && rd[sp] == '<' && (find_substr_ci((const char *)rd + sp, "<svg") != 0)) {
            char *sv = (char *)g_basi->mem->alloc((size_t)rl + 2u);
            if (sv) {
                for (uint32_t k = 0; k < rl; k++) sv[k] = (char)rd[k];
                sv[rl] = 0;
                int sw = 96, sh = 96;
                svg_intrinsic_size(sv, &sw, &sh);
                if (sw < 8) sw = 96; if (sh < 8) sh = 96;
                if (sw > 512) sw = 512; if (sh > 512) sh = 512;
                px = (uint32_t *)g_basi->mem->alloc((size_t)sw * (size_t)sh * 4u);
                if (px && svg_render(sv, sw, sh, px) == 0) { w = sw; h = sh; }
                else { if (px) g_basi->mem->free(px); px = 0; }
                g_basi->mem->free(sv);
            }
        }
    }
    if (!px) return 0;
    bcopy_str(g_imgs[g_img_count].url, url, sizeof(g_imgs[0].url));
    g_imgs[g_img_count].px = px; g_imgs[g_img_count].w = w; g_imgs[g_img_count].h = h;
    g_img_count++;
    *out_w = w; *out_h = h;
    return px;
}

static uint32_t *img_scale(const uint32_t *src, int sw, int sh, int dw, int dh) {
    uint32_t *d = img_alloc_extra(dw, dh);
    if (!d) return 0;
    for (int y = 0; y < dh; y++) {
        int sy = (int)((long)y * sh / dh); if (sy >= sh) sy = sh - 1;
        for (int x = 0; x < dw; x++) {
            int sx = (int)((long)x * sw / dw); if (sx >= sw) sx = sw - 1;
            d[(long)y * dw + x] = src[(long)sy * sw + sx];
        }
    }
    return d;
}

static int b64_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Supports both base64 PNG AND URL-encoded/raw data:image/svg+xml,... */
static uint32_t *img_from_data_uri(const char *src, int *out_w, int *out_h) {
    *out_w = 0; *out_h = 0;
    if (!g_basi || !g_basi->mem) return 0;
    const char *comma = src;
    while (*comma && *comma != ',') comma++;
    if (!*comma) return 0;

    if (find_substr_ci(src, "image/svg+xml") && find_substr_ci(src, "image/svg+xml") < comma) {
        const char *p = comma + 1;
        int plen = b_len(p);
        char *svg = (char *)g_basi->mem->alloc((size_t)plen + 16);
        if (!svg) return 0;
        int o = 0;
        for (int i = 0; i < plen; i++) {
            if (p[i] == '%' && i + 2 < plen) {
                int h1 = css_hexval(p[i + 1]), h2 = css_hexval(p[i + 2]);
                if (h1 >= 0 && h2 >= 0) { svg[o++] = (char)((h1 << 4) | h2); i += 2; continue; }
            }
            svg[o++] = p[i];
        }
        svg[o] = 0;
        int sw = 100, sh = 100;
        svg_intrinsic_size(svg, &sw, &sh);
        if (sw < 8) sw = 100; if (sh < 8) sh = 100;
        if (sw > 512) sw = 512; if (sh > 512) sh = 512;
        uint32_t *px = (uint32_t *)g_basi->mem->alloc((size_t)sw * (size_t)sh * 4u);
        if (px) {
            if (svg_render(svg, sw, sh, px) == 0) { *out_w = sw; *out_h = sh; }
            else { g_basi->mem->free(px); px = 0; }
        }
        g_basi->mem->free(svg);
        return px;
    }

    int is64 = 0;
    for (const char *q = src; q + 5 < comma; q++)
        if (strn_ci_eq(q, "base64", 6)) { is64 = 1; break; }
    if (!is64) return 0;
    const char *p = comma + 1;
    size_t n = (size_t)b_len(p);
    uint8_t *buf = g_basi->mem->alloc((n / 4 + 1) * 3 + 4);
    if (!buf) return 0;
    size_t o = 0; int acc = 0, bits = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '=') break;
        int v = b64_val((unsigned char)p[i]);
        if (v < 0) continue;
        acc = (acc << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; buf[o++] = (uint8_t)((acc >> bits) & 0xFF); }
    }
    uint32_t *px = 0; int w = 0, h = 0;
    if (browser_img_decode(buf, (uint32_t)o, g_basi->mem->alloc, g_basi->mem->free, &px, &w, &h) != 0 || !px)
        px = 0;
    g_basi->mem->free(buf);
    if (px) { *out_w = w; *out_h = h; }
    return px;
}

static inline uint32_t px_blend_over(uint32_t dst, uint32_t src) {
    uint32_t sa = (src >> 24) & 0xFF;
    if (sa == 0) return dst;
    if (sa == 255) return src;
    uint32_t da = (dst >> 24) & 0xFF;
    if (da == 0) return src;
    uint32_t inv = 255 - sa;
    uint32_t oa = sa + (da * inv + 127) / 255;
    if (oa == 0) return 0;
    uint32_t sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
    uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    uint32_t rr = (sr * sa * 255 + dr * da * inv) / (oa * 255);
    uint32_t gg = (sg * sa * 255 + dg * da * inv) / (oa * 255);
    uint32_t bb = (sb * sa * 255 + db * da * inv) / (oa * 255);
    return (oa << 24) | (rr << 16) | (gg << 8) | bb;
}

static uint32_t eval_grad_stop(const css_grad_t *g, float t) {
    if (g->nstops <= 0) return 0;
    if (t <= g->stops[0].pos)
        return ((uint32_t)g->stops[0].a << 24) | g->stops[0].rgb;
    if (t >= g->stops[g->nstops - 1].pos)
        return ((uint32_t)g->stops[g->nstops - 1].a << 24) | g->stops[g->nstops - 1].rgb;
    for (int i = 0; i < g->nstops - 1; i++) {
        float p0 = g->stops[i].pos, p1 = g->stops[i + 1].pos;
        if (t <= p1) {
            float f = (p1 > p0) ? (t - p0) / (p1 - p0) : 0.0f;
            if (f < 0.0f) f = 0.0f; if (f > 1.0f) f = 1.0f;
            uint32_t c0 = g->stops[i].rgb, c1 = g->stops[i + 1].rgb;
            int r = (int)(((c0 >> 16) & 0xFF) * (1.0f - f) + ((c1 >> 16) & 0xFF) * f + 0.5f);
            int gr = (int)(((c0 >> 8) & 0xFF) * (1.0f - f) + ((c1 >> 8) & 0xFF) * f + 0.5f);
            int b = (int)((c0 & 0xFF) * (1.0f - f) + (c1 & 0xFF) * f + 0.5f);
            int a = (int)(g->stops[i].a * (1.0f - f) + g->stops[i + 1].a * f + 0.5f);
            return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)gr << 8) | (uint32_t)b;
        }
    }
    return ((uint32_t)g->stops[g->nstops - 1].a << 24) | g->stops[g->nstops - 1].rgb;
}

static uint32_t sample_grad(const css_grad_t *g, float fx, float fy, int w, int h) {
    float t = 0.0f;
    if (g->kind == GRAD_LINEAR) {
        /* CSS angle: 0deg = up (0,-1), 90deg = right (1,0), 180deg = down (0,1), 135deg = down-right */
        double rad = (double)g->angle * 3.141592653589793 / 180.0;
        float dx = (float)d_sin(rad);
        float dy = (float)(-d_cos(rad));
        float ux = (w > 1 ? fx / (float)(w - 1) : 0.5f) - 0.5f;
        float uy = (h > 1 ? fy / (float)(h - 1) : 0.5f) - 0.5f;
        float half_len = (dx < 0 ? -dx : dx) * 0.5f + (dy < 0 ? -dy : dy) * 0.5f;
        if (half_len < 0.001f) half_len = 0.5f;
        t = (ux * dx + uy * dy) / (2.0f * half_len) + 0.5f;
    } else if (g->kind == GRAD_RADIAL) {
        float cx = g->cx * (float)w, cy = g->cy * (float)h;
        /* CSS radial-gradient(circle) default sizing is farthest-corner! */
        float rx = (float)w * 0.5f, ry = (float)h * 0.5f;
        float r = (float)d_sqrt((double)(rx * rx + ry * ry));
        if (r < 1.0f) r = 1.0f;
        float dx = (fx - cx) / r, dy = (fy - cy) / r;
        t = (float)d_sqrt((double)(dx * dx + dy * dy));
    } else if (g->kind == GRAD_CONIC) {
        float cx = g->cx * (float)w, cy = g->cy * (float)h;
        float dx = fx - cx, dy = fy - cy;
        /* CSS conic 0deg is at 12 o'clock (0,-1), clockwise */
        float ang = f_atan2(dx, -dy) * 57.2957795f - g->angle;
        while (ang < 0.0f) ang += 360.0f;
        while (ang >= 360.0f) ang -= 360.0f;
        t = ang / 360.0f;
    }
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return eval_grad_stop(g, t);
}

/* Apply CSS3 filter chain in-place to an ARGB8888 buffer */
static void apply_filter_chain(uint32_t *px, int w, int h, const css_style_t *s) {
    if (!s->has_filter || !px || w <= 0 || h <= 0) return;

    /* 1. Color matrix filters (contrast, brightness, saturate, hue-rotate, grayscale, invert, sepia) */
    int do_color = (d_fabs(s->flt_hue) > 0.1 || d_fabs(s->flt_contrast - 1.0) > 0.01 ||
                    d_fabs(s->flt_bright - 1.0) > 0.01 || d_fabs(s->flt_sat - 1.0) > 0.01 ||
                    s->flt_gray > 0.01f || s->flt_invert > 0.01f || s->flt_sepia > 0.01f);
    if (do_color) {
        double hr = (double)s->flt_hue * 3.141592653589793 / 180.0;
        float c = (float)d_cos(hr), sn = (float)d_sin(hr);
        /* W3C SVG feColorMatrix hueRotate coefficients */
        float m00 = 0.213f + c*0.787f - sn*0.213f, m01 = 0.715f - c*0.715f - sn*0.715f, m02 = 0.072f - c*0.072f + sn*0.928f;
        float m10 = 0.213f - c*0.213f + sn*0.143f, m11 = 0.715f + c*0.285f + sn*0.140f, m12 = 0.072f - c*0.072f - sn*0.283f;
        float m20 = 0.213f - c*0.213f - sn*0.787f, m21 = 0.715f - c*0.715f + sn*0.715f, m22 = 0.072f + c*0.928f + sn*0.072f;
        int total = w * h;
        for (int i = 0; i < total; i++) {
            uint32_t p = px[i];
            uint32_t a = (p >> 24) & 0xFF;
            if (a == 0) continue;
            float r = (float)((p >> 16) & 0xFF);
            float g = (float)((p >> 8) & 0xFF);
            float b = (float)(p & 0xFF);
            if (d_fabs(s->flt_contrast - 1.0) > 0.01) {
                r = (r - 128.0f) * s->flt_contrast + 128.0f;
                g = (g - 128.0f) * s->flt_contrast + 128.0f;
                b = (b - 128.0f) * s->flt_contrast + 128.0f;
            }
            if (d_fabs(s->flt_bright - 1.0) > 0.01) {
                r *= s->flt_bright; g *= s->flt_bright; b *= s->flt_bright;
            }
            if (d_fabs(s->flt_hue) > 0.1) {
                float nr = m00*r + m01*g + m02*b;
                float ng = m10*r + m11*g + m12*b;
                float nb = m20*r + m21*g + m22*b;
                r = nr; g = ng; b = nb;
            }
            if (s->flt_gray > 0.01f) {
                float l = 0.2126f*r + 0.7152f*g + 0.0722f*b;
                float f = s->flt_gray;
                r = r*(1-f) + l*f; g = g*(1-f) + l*f; b = b*(1-f) + l*f;
            }
            if (s->flt_invert > 0.01f) {
                float f = s->flt_invert;
                r = r*(1-f) + (255-r)*f; g = g*(1-f) + (255-g)*f; b = b*(1-f) + (255-b)*f;
            }
            int ri = (int)(r + 0.5f), gi = (int)(g + 0.5f), bi = (int)(b + 0.5f);
            if (ri < 0) ri = 0; if (ri > 255) ri = 255;
            if (gi < 0) gi = 0; if (gi > 255) gi = 255;
            if (bi < 0) bi = 0; if (bi > 255) bi = 255;
            px[i] = (a << 24) | ((uint32_t)ri << 16) | ((uint32_t)gi << 8) | (uint32_t)bi;
        }
    }

    /* 2. Separable box blur (2 passes approximates Gaussian cleanly) */
    if (s->flt_blur >= 1.0f && g_basi && g_basi->mem) {
        int rad = (int)(s->flt_blur + 0.5f);
        if (rad > 28) rad = 28;
        uint32_t *tmp = (uint32_t *)g_basi->mem->alloc((size_t)w * (size_t)h * 4u);
        if (tmp) {
            int dia = 2 * rad + 1;
            for (int pass = 0; pass < 2; pass++) {
                /* Horizontal */
                for (int y = 0; y < h; y++) {
                    const uint32_t *row = px + y * w;
                    uint32_t *out = tmp + y * w;
                    int sa = 0, sr = 0, sg = 0, sb = 0;
                    for (int k = -rad; k <= rad; k++) {
                        int xk = k < 0 ? 0 : (k >= w ? w - 1 : k);
                        uint32_t p = row[xk];
                        sa += (p >> 24) & 0xFF; sr += (p >> 16) & 0xFF; sg += (p >> 8) & 0xFF; sb += p & 0xFF;
                    }
                    for (int x = 0; x < w; x++) {
                        out[x] = ((uint32_t)(sa/dia)<<24) | ((uint32_t)(sr/dia)<<16) | ((uint32_t)(sg/dia)<<8) | (uint32_t)(sb/dia);
                        int rem = x - rad; if (rem < 0) rem = 0;
                        int add = x + rad + 1; if (add >= w) add = w - 1;
                        uint32_t pr = row[rem], pa = row[add];
                        sa += (int)((pa>>24)&0xFF) - (int)((pr>>24)&0xFF);
                        sr += (int)((pa>>16)&0xFF) - (int)((pr>>16)&0xFF);
                        sg += (int)((pa>>8)&0xFF)  - (int)((pr>>8)&0xFF);
                        sb += (int)(pa&0xFF)       - (int)(pr&0xFF);
                    }
                }
                /* Vertical */
                for (int x = 0; x < w; x++) {
                    int sa = 0, sr = 0, sg = 0, sb = 0;
                    for (int k = -rad; k <= rad; k++) {
                        int yk = k < 0 ? 0 : (k >= h ? h - 1 : k);
                        uint32_t p = tmp[yk * w + x];
                        sa += (p >> 24) & 0xFF; sr += (p >> 16) & 0xFF; sg += (p >> 8) & 0xFF; sb += p & 0xFF;
                    }
                    for (int y = 0; y < h; y++) {
                        px[y * w + x] = ((uint32_t)(sa/dia)<<24) | ((uint32_t)(sr/dia)<<16) | ((uint32_t)(sg/dia)<<8) | (uint32_t)(sb/dia);
                        int rem = y - rad; if (rem < 0) rem = 0;
                        int add = y + rad + 1; if (add >= h) add = h - 1;
                        uint32_t pr = tmp[rem * w + x], pa = tmp[add * w + x];
                        sa += (int)((pa>>24)&0xFF) - (int)((pr>>24)&0xFF);
                        sr += (int)((pa>>16)&0xFF) - (int)((pr>>16)&0xFF);
                        sg += (int)((pa>>8)&0xFF)  - (int)((pr>>8)&0xFF);
                        sb += (int)(pa&0xFF)       - (int)(pr&0xFF);
                    }
                }
            }
            g_basi->mem->free(tmp);
        }
    }
}

static int poly_inside(float x, float y, const float *vx, const float *vy, int n) {
    int c = 0;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        if (((vy[i] > y) != (vy[j] > y)) &&
            (x < (vx[j] - vx[i]) * (y - vy[i]) / (vy[j] - vy[i]) + vx[i]))
            c = !c;
    }
    return c;
}

/* Compute rounded/elliptical corner alpha (0..255) for pixel (px,py) in w x h */
static uint8_t corner_alpha(float x, float y, int w, int h, const float rx[4], const float ry[4]) {
    /* 0:TL, 1:TR, 2:BR, 3:BL */
    float cx = 0, cy = 0, ex = 0, ey = 0;
    int check = 0;
    if (x < rx[0] && y < ry[0]) { cx = rx[0]; cy = ry[0]; ex = rx[0]; ey = ry[0]; check = 1; }
    else if (x > (float)w - rx[1] && y < ry[1]) { cx = (float)w - rx[1]; cy = ry[1]; ex = rx[1]; ey = ry[1]; check = 1; }
    else if (x > (float)w - rx[2] && y > (float)h - ry[2]) { cx = (float)w - rx[2]; cy = (float)h - ry[2]; ex = rx[2]; ey = ry[2]; check = 1; }
    else if (x < rx[3] && y > (float)h - ry[3]) { cx = rx[3]; cy = (float)h - ry[3]; ex = rx[3]; ey = ry[3]; check = 1; }
    if (!check || ex <= 0.5f || ey <= 0.5f) return 255;
    float dx = (x - cx) / ex, dy = (y - cy) / ey;
    float d2 = dx * dx + dy * dy;
    if (d2 <= 0.94f) return 255;
    if (d2 >= 1.06f) return 0;
    float dist = (float)d_sqrt((double)d2);
    float f = (1.03f - dist) / 0.06f;
    if (f < 0.0f) f = 0.0f; if (f > 1.0f) f = 1.0f;
    return (uint8_t)(f * 255.0f + 0.5f);
}

static int style_needs_custom_surface(const css_style_t *s) {
    if (s->bg_clip_text) return 0; /* background belongs to text, not box */
    if (s->ngrads > 0) return 1;
    if (s->has_bg_url) return 1;
    if (s->clip_poly.n >= 3) return 1;
    if (s->has_corner_radius) return 1;
    if (s->has_filter) return 1;
    if (s->border_w > 0 && (s->border_style == 2 || s->border_style == 3)) return 1;
    return 0;
}

static void paint_css_surface(lv_obj_t *o, int w, int h, const css_style_t *s) {
    if (w < 2 || h < 2 || !style_needs_custom_surface(s)) return;
    if (w > 1600) w = 1600;
    if (h > 1000) h = 1000;
    uint32_t *px = img_alloc_extra(w, h);
    if (!px) return;

    /* 1. Base background color */
    uint32_t base_col = (s->has_bg && !s->bg_transparent) ? (((uint32_t)s->bg_a << 24) | s->bg) : 0;
    int total = w * h;
    for (int i = 0; i < total; i++) px[i] = base_col;

    /* 2. Background gradients (1 or 2 layers, with optional background-size tiling!) */
    int tile_w = w, tile_h = h;
    if (s->bg_size == 3 && s->bg_size_w > 0) {
        tile_w = s->bg_size_wp ? (w * s->bg_size_w / 100) : s->bg_size_w;
        tile_h = s->bg_size_h > 0 ? (s->bg_size_hp ? (h * s->bg_size_h / 100) : s->bg_size_h) : tile_w;
        if (tile_w < 2) tile_w = 2; if (tile_h < 2) tile_h = 2;
    }
    for (int gi = 0; gi < s->ngrads; gi++) {
        const css_grad_t *g = &s->grads[gi];
        if (g->kind == GRAD_NONE) continue;
        int is_grid_line = (tile_w < w || tile_h < h) && g->kind == GRAD_LINEAR;
        for (int y = 0; y < h; y++) {
            float sy = is_grid_line ? (float)(y % tile_h) : (float)y;
            int sh = is_grid_line ? tile_h : h;
            for (int x = 0; x < w; x++) {
                float sx = is_grid_line ? (float)(x % tile_w) : (float)x;
                int sw = is_grid_line ? tile_w : w;
                uint32_t gc = sample_grad(g, sx, sy, sw, sh);
                if (gc >> 24) px[y * w + x] = px_blend_over(px[y * w + x], gc);
            }
        }
    }

    /* 3. Background URL image (data:image/svg+xml, data:image/png, or network PNG) */
    if (s->has_bg_url && s->bg_url[0]) {
        int iw = 0, ih = 0;
        uint32_t *img = 0;
        int free_img = 0;
        if (starts_with(s->bg_url, "data:")) {
            img = img_from_data_uri(s->bg_url, &iw, &ih);
            free_img = (img != 0);
        } else {
            char abs[440]; resolve_url(g_cur_scheme, g_cur_host, s->bg_url, abs, sizeof(abs));
            if (abs[0]) img = img_get(abs, &iw, &ih);
        }
        if (img && iw > 0 && ih > 0) {
            for (int y = 0; y < h; y++) {
                int sy = (s->bg_repeat == 1) ? y : (y % ih);
                if (sy >= ih) continue;
                for (int x = 0; x < w; x++) {
                    int sx = (s->bg_repeat == 1) ? x : (x % iw);
                    if (sx >= iw) continue;
                    uint32_t ic = img[sy * iw + sx];
                    if (ic >> 24) px[y * w + x] = px_blend_over(px[y * w + x], ic);
                }
            }
            if (free_img && g_basi && g_basi->mem) g_basi->mem->free(img);
        }
    }

    /* 4. CSS Filter chain (blur, hue-rotate, contrast, brightness, etc.) */
    if (s->has_filter) apply_filter_chain(px, w, h, s);

    /* 5. Dashed / dotted border drawn directly into surface */
    if (s->border_w > 0 && (s->border_style == 2 || s->border_style == 3)) {
        int bw = s->border_w;
        int dash = (s->border_style == 2) ? (bw * 3 + 2) : (bw + 1);
        int period = dash * 2;
        uint32_t bc = ((uint32_t)s->border_a << 24) | s->border_c;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int on_tb = (y < bw || y >= h - bw);
                int on_lr = (x < bw || x >= w - bw);
                if (on_tb && ((x % period) < dash)) px[y * w + x] = px_blend_over(px[y * w + x], bc);
                else if (on_lr && ((y % period) < dash)) px[y * w + x] = px_blend_over(px[y * w + x], bc);
            }
        }
    }

    /* 6. Clip-path polygon() mask (2x2 supersampled AA) */
    if (s->clip_poly.n >= 3) {
        float vx[12], vy[12];
        int np = s->clip_poly.n;
        for (int i = 0; i < np; i++) {
            vx[i] = s->clip_poly.xp[i] ? (s->clip_poly.x[i] * (float)w / 100.0f) : s->clip_poly.x[i];
            vy[i] = s->clip_poly.yp[i] ? (s->clip_poly.y[i] * (float)h / 100.0f) : s->clip_poly.y[i];
        }
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int hits = 0;
                if (poly_inside(x + 0.25f, y + 0.25f, vx, vy, np)) hits++;
                if (poly_inside(x + 0.75f, y + 0.25f, vx, vy, np)) hits++;
                if (poly_inside(x + 0.25f, y + 0.75f, vx, vy, np)) hits++;
                if (poly_inside(x + 0.75f, y + 0.75f, vx, vy, np)) hits++;
                if (hits == 0) px[y * w + x] = 0;
                else if (hits < 4) {
                    uint32_t p = px[y * w + x];
                    uint32_t a = ((p >> 24) * (uint32_t)hits) / 4u;
                    px[y * w + x] = (a << 24) | (p & 0x00FFFFFFu);
                }
            }
        }
    }

    /* 7. Elliptical or rounded border-radius mask */
    if (s->has_corner_radius || s->radius > 0) {
        float rx[4], ry[4];
        for (int i = 0; i < 4; i++) {
            if (s->has_corner_radius) {
                rx[i] = s->rad_rx_p[i] ? (s->rad_rx[i] * (float)w / 100.0f) : (float)s->rad_rx[i];
                ry[i] = s->rad_ry_p[i] ? (s->rad_ry[i] * (float)h / 100.0f) : (float)s->rad_ry[i];
            } else {
                rx[i] = s->radius_pct ? (s->radius * (float)w / 100.0f) : (float)s->radius;
                ry[i] = s->radius_pct ? (s->radius * (float)h / 100.0f) : (float)s->radius;
            }
            if (rx[i] > w * 0.5f) rx[i] = w * 0.5f;
            if (ry[i] > h * 0.5f) ry[i] = h * 0.5f;
        }
        /* Normalize adjacent corners per CSS3 spec if sum > w or h */
        float sx = 1.0f, sy = 1.0f;
        if (rx[0] + rx[1] > w) { float f = (float)w / (rx[0] + rx[1]); if (f < sx) sx = f; }
        if (rx[3] + rx[2] > w) { float f = (float)w / (rx[3] + rx[2]); if (f < sx) sx = f; }
        if (ry[0] + ry[3] > h) { float f = (float)h / (ry[0] + ry[3]); if (f < sy) sy = f; }
        if (ry[1] + ry[2] > h) { float f = (float)h / (ry[1] + ry[2]); if (f < sy) sy = f; }
        for (int i = 0; i < 4; i++) { rx[i] *= sx; ry[i] *= sy; }

        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint8_t ca = corner_alpha((float)x + 0.5f, (float)y + 0.5f, w, h, rx, ry);
                if (ca == 0) px[y * w + x] = 0;
                else if (ca < 255) {
                    uint32_t p = px[y * w + x];
                    uint32_t a = ((p >> 24) * ca) / 255u;
                    px[y * w + x] = (a << 24) | (p & 0x00FFFFFFu);
                }
            }
        }
    }

    lv_image_dsc_t *dsc = img_make_dsc(px, w, h);
    if (dsc) {
        lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_image_src(o, dsc, 0);
        lv_obj_set_style_bg_image_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_image_tiled(o, false, 0);
    }
}

/* ---------------- UTF-8 & Rich Inline / Gradient / Blend / Vertical Text Painter ---------------- */
static uint32_t utf8_next(const char **pp) {
    const unsigned char *p = (const unsigned char *)(*pp);
    if (!*p) return 0;
    uint32_t cp;
    if (*p < 0x80) { cp = *p; *pp += 1; }
    else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); *pp += 2; }
    else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
        cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); *pp += 3;
    } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
        cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); *pp += 4;
    } else { cp = *p; *pp += 1; }
    return cp;
}

static void apply_text_transform(const char *in, char *out, int cap, int mode) {
    if (!mode) { bcopy_str(out, in, cap); return; }
    int o = 0;
    const char *p = in;
    int prev_sp = 1;
    while (*p && o < cap - 4) {
        uint32_t cp = utf8_next(&p);
        if (mode == 1) { /* uppercase (Turkish-aware) */
            if (cp >= 'a' && cp <= 'z') cp -= 32;
            else if (cp == 0x0131) cp = 'I';
            else if (cp == 0x00E7) cp = 0x00C7;
            else if (cp == 0x011F) cp = 0x011E;
            else if (cp == 0x00F6) cp = 0x00D6;
            else if (cp == 0x015F) cp = 0x015E;
            else if (cp == 0x00FC) cp = 0x00DC;
        } else if (mode == 2) {
            if (cp >= 'A' && cp <= 'Z') cp += 32;
        } else if (mode == 3 && prev_sp) {
            if (cp >= 'a' && cp <= 'z') cp -= 32;
        }
        prev_sp = (cp == ' ' || cp == '\t' || cp == '\n');
        utf8_emit(out, &o, cap, cp);
    }
    out[o] = 0;
}

/* Draw crisp vector fallback icon for missing emoji codepoints (moon, sun, cycle, bolt) */
static int draw_emoji_fallback(uint32_t *buf, int bw, int bh, int x0, int y0, int sz, uint32_t cp) {
    if (cp != 0x1F319 && cp != 0x2600 && cp != 0x1F504 && cp != 0x26A1) return 0;
    int r = sz / 2;
    if (r < 4) r = 4;
    float cx = (float)x0 + r, cy = (float)y0 + r;
    for (int py = y0; py < y0 + sz && py < bh; py++) {
        if (py < 0) continue;
        for (int px = x0; px < x0 + sz && px < bw; px++) {
            if (px < 0) continue;
            float dx = (float)px - cx, dy = (float)py - cy;
            float d = (float)d_sqrt((double)(dx*dx + dy*dy));
            uint32_t col = 0;
            if (cp == 0x1F319) { /* crescent moon (amber/yellow) */
                float d2 = (float)d_sqrt((double)((dx - r*0.35f)*(dx - r*0.35f) + (dy + r*0.2f)*(dy + r*0.2f)));
                if (d <= r * 0.78f && d2 >= r * 0.58f) col = 0xFFFBBF24u;
            } else if (cp == 0x2600) { /* sun */
                if (d <= r * 0.48f) col = 0xFFFBBF24u;
                else if (d <= r * 0.85f && d >= r * 0.62f) {
                    float ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
                    if (ax < 1.3f || ay < 1.3f || d_fabs(ax - ay) < 1.6f) col = 0xFFF59E0Bu;
                }
            } else if (cp == 0x1F504) { /* refresh / flip icon (blue pill) */
                if (d <= r * 0.85f && d >= r * 0.52f && !(dx > 0 && dy > -1.5f && dy < 1.5f)) col = 0xFF3B82F6u;
            } else if (cp == 0x26A1) { /* lightning bolt (amber) */
                float nx = dx / (float)r, ny = dy / (float)r;
                if (ny >= -0.8f && ny <= 0.8f) {
                    if (ny < 0.05f && nx > -0.25f - ny*0.6f && nx < 0.35f - ny*0.3f) col = 0xFFFACC15u;
                    if (ny >= -0.05f && nx > -0.35f - ny*0.3f && nx < 0.25f - ny*0.6f) col = 0xFFF59E0Bu;
                }
            }
            if (col) buf[py * bw + px] = px_blend_over(buf[py * bw + px], col);
        }
    }
    return sz;
}

/* Measure UTF-8 string width in a given LVGL font */
static int measure_str_w(const lv_font_t *f, const char *s, int ls, int fsz) {
    int w = 0;
    const char *p = s;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        if (cp == 0xFE0F) continue;
        if (cp == 0x1F319 || cp == 0x2600 || cp == 0x1F504 || cp == 0x26A1) { w += fsz + 2; continue; }
        const char *q = p;
        uint32_t nxt = utf8_next(&q);
        lv_font_glyph_dsc_t dsc;
        if (lv_font_get_glyph_dsc(f, &dsc, cp, nxt)) w += dsc.adv_w + ls;
        else w += fsz / 2;
    }
    return w;
}

/* Rasterize a single line of text at (pen_x, line_y) into ARGB8888 buffer.
 * Supports solid color, linear-gradient text (background-clip: text), and mix-blend-mode: difference! */
static int blit_str_run(uint32_t *buf, int bw, int bh, int pen_x, int line_y,
                        const lv_font_t *f, const char *s, int fsz, int ls,
                        uint32_t col, uint8_t ca, const css_grad_t *grad,
                        int grad_w, int grad_h, int blend_diff, const css_grad_t *parent_bg) {
    const char *p = s;
    int baseline_y = line_y + (f->line_height - f->base_line);
    while (*p) {
        uint32_t cp = utf8_next(&p);
        if (cp == 0xFE0F) continue;
        if (cp == 0x1F319 || cp == 0x2600 || cp == 0x1F504 || cp == 0x26A1) {
            draw_emoji_fallback(buf, bw, bh, pen_x, line_y + (f->line_height - fsz)/2, fsz, cp);
            pen_x += fsz + 2;
            continue;
        }
        const char *q = p;
        uint32_t nxt = utf8_next(&q);
        lv_font_glyph_dsc_t dsc;
        if (!lv_font_get_glyph_dsc(f, &dsc, cp, nxt)) { pen_x += fsz / 2; continue; }
        if (dsc.box_w > 0 && dsc.box_h > 0) {
            const lv_draw_buf_t *db = (const lv_draw_buf_t *)lv_font_get_glyph_bitmap(&dsc, NULL);
            if (db && db->data) {
                int gx0 = pen_x + dsc.ofs_x;
                int gy0 = baseline_y - dsc.box_h - dsc.ofs_y;
                int stride = (int)db->header.stride;
                if (stride < dsc.box_w) stride = dsc.box_w;
                for (int r = 0; r < dsc.box_h; r++) {
                    int dy = gy0 + r;
                    if (dy < 0 || dy >= bh) continue;
                    const uint8_t *row = db->data + r * stride;
                    for (int c = 0; c < dsc.box_w; c++) {
                        uint8_t ga = row[c];
                        if (!ga) continue;
                        int dx = gx0 + c;
                        if (dx < 0 || dx >= bw) continue;
                        uint32_t rgb = col;
                        uint32_t a = ((uint32_t)ga * ca) / 255u;
                        if (grad && grad->kind != GRAD_NONE) {
                            uint32_t gc = sample_grad(grad, (float)dx, (float)dy, grad_w, grad_h);
                            rgb = gc & 0x00FFFFFFu;
                            a = (a * ((gc >> 24) & 0xFF)) / 255u;
                        }
                        if (blend_diff && parent_bg && parent_bg->kind != GRAD_NONE) {
                            uint32_t bgc = sample_grad(parent_bg, (float)(dx + (grad_w - bw)/2), (float)(dy + (grad_h - bh)/2), grad_w, grad_h);
                            int fr = (int)((rgb >> 16) & 0xFF), fg = (int)((rgb >> 8) & 0xFF), fb = (int)(rgb & 0xFF);
                            int br = (int)((bgc >> 16) & 0xFF), bg = (int)((bgc >> 8) & 0xFF), bb = (int)(bgc & 0xFF);
                            int dr = fr > br ? fr - br : br - fr;
                            int dg = fg > bg ? fg - bg : bg - fg;
                            int db2 = fb > bb ? fb - bb : bb - fb;
                            rgb = ((uint32_t)dr << 16) | ((uint32_t)dg << 8) | (uint32_t)db2;
                        }
                        buf[dy * bw + dx] = px_blend_over(buf[dy * bw + dx], (a << 24) | rgb);
                    }
                }
            }
            lv_font_glyph_release_draw_data(&dsc);
        }
        pen_x += dsc.adv_w + ls;
    }
    return pen_x;
}

/* Render a single rich text canvas (gradient text, mix-blend-mode, vertical writing-mode, or emoji-containing label) */
static lv_obj_t *create_fancy_text_canvas(lv_obj_t *parent, const char *raw_txt, const css_style_t *s,
                                          const css_style_t *ps, int max_w, int wrap) {
    char txt[CSS_NTEXT];
    apply_text_transform(raw_txt, txt, sizeof(txt), s->transform);
    css_trim(txt);
    if (!txt[0]) return 0;

    int fsz = s->font_size * g_zoom_pct / 100;
    if (fsz < 8) fsz = 8;
    if (fsz > 72) fsz = 72;
    lv_font_t *f = bfont_ex(fsz, s->bold);
    int lh = (s->line_height > f->line_height) ? s->line_height : (f->line_height + 2);
    const css_grad_t *grad = 0;
    if (s->bg_clip_text && s->ngrads > 0) grad = &s->grads[0];
    else if (ps && ps->bg_clip_text && ps->ngrads > 0 && !s->has_color && !s->has_bg) grad = &ps->grads[0];
    const css_grad_t *pbg = (ps && ps->ngrads > 0) ? &ps->grads[0] : 0;

    /* Vertical writing mode (e.g. "SPEC TEST" rotated 180deg -> reads bottom-to-top) */
    if (s->writing_vert) {
        int tw = measure_str_w(f, txt, s->letter_space, s->font_size) + 4;
        int th = f->line_height + 4;
        if (tw < 8) tw = 8; if (th < 8) th = 8;
        if (tw > 400) tw = 400;
        uint32_t *tmp = (uint32_t *)g_basi->mem->alloc((size_t)tw * (size_t)th * 4u);
        if (!tmp) return 0;
        for (int i = 0; i < tw * th; i++) tmp[i] = 0;
        blit_str_run(tmp, tw, th, 2, 2, f, txt, s->font_size, s->letter_space,
                     s->color, (uint8_t)s->color_a, grad, tw, th, 0, 0);
        /* Rotate 90deg CCW (writing-mode: vertical-rl + rotate(180deg)) */
        int rw = th, rh = tw;
        uint32_t *rot = img_alloc_extra(rw, rh);
        if (!rot) { g_basi->mem->free(tmp); return 0; }
        for (int y = 0; y < th; y++)
            for (int x = 0; x < tw; x++)
                rot[(tw - 1 - x) * rw + y] = tmp[y * tw + x];
        g_basi->mem->free(tmp);
        lv_obj_t *c = lv_canvas_create(parent);
        lv_obj_remove_style_all(c);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_canvas_set_buffer(c, rot, rw, rh, LV_COLOR_FORMAT_ARGB8888);
        return c;
    }

    int tw = measure_str_w(f, txt, s->letter_space, s->font_size) + 6;
    if (!wrap || max_w <= 20 || tw <= max_w) {
        int cw = tw; int ch = lh + 4;
        if (cw < 4) cw = 4; if (ch < 8) ch = 8;
        if (cw > 1400) cw = 1400;
        uint32_t *px = img_alloc_extra(cw, ch);
        if (!px) return 0;
        for (int i = 0; i < cw * ch; i++) px[i] = 0;
        int gw = (s->blend_mode == 1 && ps && ps->has_w && !ps->w_pct) ? ps->width : (max_w > cw ? max_w : cw);
        int gh = (s->blend_mode == 1 && ps && ps->has_h && !ps->h_pct) ? ps->height : ch;
        blit_str_run(px, cw, ch, 2, (ch - f->line_height)/2, f, txt, s->font_size, s->letter_space,
                     s->color, (uint8_t)s->color_a, grad, gw, gh, s->blend_mode == 1, pbg);
        lv_obj_t *c = lv_canvas_create(parent);
        lv_obj_remove_style_all(c);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_canvas_set_buffer(c, px, cw, ch, LV_COLOR_FORMAT_ARGB8888);
        return c;
    }

    /* Word-wrapping multi-line canvas */
    int cw = max_w; if (cw > 1400) cw = 1400;
    char lines[24][200]; int nl = 0;
    const char *p = txt;
    while (*p && nl < 24) {
        while (*p == ' ') p++;
        if (!*p) break;
        int lo = 0;
        lines[nl][0] = 0;
        while (*p && *p != '\n') {
            const char *ws = p;
            while (*p && *p != ' ' && *p != '\n') p++;
            char word[120]; int wl = (int)(p - ws); if (wl > 119) wl = 119;
            for (int k = 0; k < wl; k++) word[k] = ws[k];
            word[wl] = 0;
            char cand[200]; bcopy_str(cand, lines[nl], sizeof(cand));
            if (lo > 0) bcat(cand, " ", sizeof(cand));
            bcat(cand, word, sizeof(cand));
            if (lo > 0 && measure_str_w(f, cand, s->letter_space, s->font_size) > cw - 4) {
                p = ws; break;
            }
            bcopy_str(lines[nl], cand, sizeof(lines[nl]));
            lo = b_len(lines[nl]);
            while (*p == ' ') p++;
        }
        if (*p == '\n') p++;
        nl++;
    }
    int ch = nl * lh + 6;
    if (ch < lh) ch = lh;
    if (ch > 1000) ch = 1000;
    uint32_t *px = img_alloc_extra(cw, ch);
    if (!px) return 0;
    for (int i = 0; i < cw * ch; i++) px[i] = 0;
    for (int i = 0; i < nl; i++) {
        int lw = measure_str_w(f, lines[i], s->letter_space, s->font_size);
        int x0 = (s->align == 1) ? (cw - lw) / 2 : (s->align == 2 ? (cw - lw - 2) : 0);
        if (x0 < 0) x0 = 0;
        blit_str_run(px, cw, ch, x0, i * lh + 2, f, lines[i], s->font_size, s->letter_space,
                     s->color, (uint8_t)s->color_a, grad, cw, ch, s->blend_mode == 1, pbg);
    }
    lv_obj_t *c = lv_canvas_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_canvas_set_buffer(c, px, cw, ch, LV_COLOR_FORMAT_ARGB8888);
    return c;
}

/* Rich mixed-inline word-wrapped block painter (handles <h1>SÄ±fÄ±rdan <span class="gradient-text">...</span>...</h1>,
 * <p class="lead-text"> with ::first-letter drop cap, <span>KaanDemir<span>.css</span></span>, etc.) */
typedef struct {
    char       word[80];
    int        bold;
    int        font_size;
    uint32_t   color;
    uint8_t    color_a;
    int        ls;
    int        has_grad;
    css_grad_t grad;
    int        space_before;
    int        w;
} rich_word_t;

static int is_inline_text_only_subtree(int n) {
    const dnode_t *d = &g_dn[n];
    int has_text_or_span = 0;
    for (int c = d->first; c >= 0; c = g_dn[c].next) {
        if (g_dn[c].is_text) { has_text_or_span = 1; continue; }
        const char *ct = g_dn[c].tag;
        if (str_eq(ct, "span") || str_eq(ct, "strong") || str_eq(ct, "b") ||
            str_eq(ct, "em") || str_eq(ct, "i") || str_eq(ct, "code")) {
            /* Exclude pill/badge spans that have their own background/border/padding */
            if (find_substr_ci(g_dn[c].cls, "css-tag") || find_substr_ci(g_dn[c].cls, "status-dot") ||
                find_substr_ci(g_dn[c].cls, "theme-switch-ball"))
                return 0;
            if (g_dn[c].first >= 0 && !is_inline_text_only_subtree(c)) return 0;
            has_text_or_span = 1;
        } else return 0;
    }
    return has_text_or_span;
}

static void collect_rich_words(int n, css_style_t *ps, rich_word_t *words, int *nw, int maxw, int depth, int *pend_sp) {
    const dnode_t *d = &g_dn[n];
    for (int c = d->first; c >= 0; c = g_dn[c].next) {
        if (g_dn[c].is_text) {
            char buf[CSS_NTEXT];
            apply_text_transform(g_dn[c].text, buf, sizeof(buf), ps->transform);
            const char *p = buf;
            while (*p && *nw < maxw) {
                while (*p == ' ') { *pend_sp = 1; p++; }
                if (!*p) break;
                const char *ws = p;
                while (*p && *p != ' ' && *p != '\n') p++;
                int wl = (int)(p - ws); if (wl > 79) wl = 79;
                if (wl > 0) {
                    rich_word_t *rw = &words[(*nw)++];
                    for (int k = 0; k < wl; k++) rw->word[k] = ws[k];
                    rw->word[wl] = 0;
                    rw->bold = ps->bold;
                    rw->font_size = ps->font_size;
                    rw->color = ps->color;
                    rw->color_a = (uint8_t)ps->color_a;
                    rw->ls = ps->letter_space;
                    rw->has_grad = (ps->bg_clip_text && ps->ngrads > 0);
                    if (rw->has_grad) rw->grad = ps->grads[0];
                    rw->space_before = (*nw > 1 && *pend_sp) ? 1 : 0;
                    lv_font_t *f = bfont_ex(rw->font_size, rw->bold);
                    rw->w = measure_str_w(f, rw->word, rw->ls, rw->font_size);
                    *pend_sp = 0;
                }
                if (*p == '\n') { *pend_sp = 1; p++; }
            }
        } else {
            css_style_t cs;
            compute_style(c, ps, &cs, 0, depth + 1);
            if (!cs.hide) collect_rich_words(c, &cs, words, nw, maxw, depth + 1, pend_sp);
            css_var_pop(depth + 1);
        }
    }
}

static lv_obj_t *render_rich_inline_block(lv_obj_t *parent, int n, css_style_t *s, int avail_w, int depth, int allow_shrink) {
    /* Check ::first-letter pseudo-element */
    css_style_t fls;
    compute_style(n, s, &fls, 3, depth + 1);
    css_var_pop(depth + 1);
    int has_fl = (fls.has_font && fls.font_size > s->font_size + 6);

    static rich_word_t words[160];
    int nw = 0, pend_sp = 0;
    collect_rich_words(n, s, words, &nw, 160, depth, &pend_sp);
    if (nw == 0) return 0;

    lv_font_t *bf = bfont_ex(s->font_size, s->bold);
    int sp_w = measure_str_w(bf, " ", s->letter_space, s->font_size);
    if (sp_w < 3) sp_w = s->font_size / 3;
    int lh = (s->line_height > bf->line_height) ? s->line_height : (bf->line_height + 4);

    /* Drop-cap first-letter geometry */
    char fl_utf8[8] = {0};
    int fl_w = 0, fl_h = 0, fl_lines = 0;
    lv_font_t *fl_font = 0;
    if (has_fl && words[0].word[0]) {
        const char *p0 = words[0].word;
        uint32_t cp0 = utf8_next(&p0);
        int o = 0; utf8_emit(fl_utf8, &o, sizeof(fl_utf8), cp0); fl_utf8[o] = 0;
        /* Shift remaining chars of words[0] left */
        char rem[80]; bcopy_str(rem, p0, sizeof(rem));
        bcopy_str(words[0].word, rem, sizeof(words[0].word));
        words[0].w = measure_str_w(bfont_ex(words[0].font_size, words[0].bold), words[0].word, words[0].ls, words[0].font_size);
        fl_font = bfont_ex(fls.font_size, fls.bold);
        fl_w = measure_str_w(fl_font, fl_utf8, 0, fls.font_size) + fls.mar[1] + 4;
        fl_h = fl_font->line_height;
        fl_lines = (fl_h + lh - 1) / lh;
        if (fl_lines < 2) fl_lines = 2;
    }

    int max_w = (avail_w > 40) ? avail_w : 600;
    if (max_w > 1400) max_w = 1400;

    /* Layout words into lines */
    int line_start[48], line_end[48], line_w[48], line_x0[48];
    int nl = 0, wi = 0, max_lw = 0;
    while (wi < nw && nl < 48) {
        int indent = (has_fl && nl < fl_lines) ? fl_w : 0;
        int lim = max_w - indent;
        if (lim < 60) lim = 60;
        line_start[nl] = wi;
        line_x0[nl] = indent;
        int cur_w = 0;
        while (wi < nw) {
            int add = words[wi].w + ((wi > line_start[nl] && words[wi].space_before) ? sp_w : 0);
            if (wi > line_start[nl] && cur_w + add > lim) break;
            cur_w += add;
            wi++;
        }
        line_end[nl] = wi;
        line_w[nl] = cur_w;
        if (indent + cur_w > max_lw) max_lw = indent + cur_w;
        nl++;
    }
    int cw = allow_shrink ? (max_lw + 6) : max_w;
    if (cw < 16) cw = 16;
    if (cw > max_w) cw = max_w;
    int ch = nl * lh + 6;
    if (has_fl && fl_h + 6 > ch) ch = fl_h + 6;
    if (ch > 1200) ch = 1200;

    uint32_t *px = img_alloc_extra(cw, ch);
    if (!px) return 0;
    for (int i = 0; i < cw * ch; i++) px[i] = 0;

    if (has_fl && fl_font) {
        const css_grad_t *fg = (fls.bg_clip_text && fls.ngrads > 0) ? &fls.grads[0] : 0;
        blit_str_run(px, cw, ch, 0, 0, fl_font, fl_utf8, fls.font_size, 0,
                     fls.color, (uint8_t)fls.color_a, fg, fl_w, fl_h, 0, 0);
    }

    for (int li = 0; li < nl; li++) {
        int pen = line_x0[li];
        int rem_w = cw - line_x0[li] - line_w[li];
        if (s->align == 1 && rem_w > 0) pen += rem_w / 2;
        else if (s->align == 2 && rem_w > 0) pen += rem_w;
        int ly = li * lh + 2;
        for (int k = line_start[li]; k < line_end[li]; k++) {
            if (k > line_start[li] && words[k].space_before) pen += sp_w;
            if (!words[k].word[0]) continue;
            lv_font_t *wf = bfont_ex(words[k].font_size, words[k].bold);
            css_grad_t wg_local;
            const css_grad_t *wg = 0;
            if (words[k].has_grad) {
                wg_local = words[k].grad;
                /* For inline gradient spans (e.g. 45deg), flow smoothly from left/top-left to right/bottom-right */
                if (wg_local.kind == GRAD_LINEAR && wg_local.angle > 0.0f && wg_local.angle < 90.0f)
                    wg_local.angle = 135.0f;
                wg = &wg_local;
            }
            pen = blit_str_run(px, cw, ch, pen, ly, wf, words[k].word, words[k].font_size, words[k].ls,
                               words[k].color, words[k].color_a, wg, cw, ch, 0, 0);
        }
    }

    lv_obj_t *c = lv_canvas_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_canvas_set_buffer(c, px, cw, ch, LV_COLOR_FORMAT_ARGB8888);
    return c;
}

/* ---------------- Inline SVG Preprocessor (<polyline> + <linearGradient>) ---------------- */
static void svg_extract_grad_color(const char *svg, const char *gid, css_grad_t *g) {
    g->kind = GRAD_LINEAR; g->angle = 135.0f; g->nstops = 0;
    char needle[64]; bcopy_str(needle, "id=\"", sizeof(needle)); bcat(needle, gid, sizeof(needle)); bcat(needle, "\"", sizeof(needle));
    const char *p = find_substr_ci(svg, needle);
    if (!p) return;
    const char *end = find_substr_ci(p, "</linearGradient");
    if (!end) return;
    while (p < end && g->nstops < 4) {
        const char *st = find_substr_ci(p, "<stop");
        if (!st || st >= end) break;
        const char *se = st; while (*se && *se != '>') se++;
        char col[32], off[16];
        if (find_attr(st, se, "stop-color", col, sizeof(col))) {
            uint32_t rgb = 0;
            if (css_color(col, &rgb)) {
                g->stops[g->nstops].rgb = rgb;
                g->stops[g->nstops].a = 255;
                g->stops[g->nstops].pos = find_attr(st, se, "offset", off, sizeof(off)) ? (float)(css_f(off) / 100.0) : (g->nstops ? 1.0f : 0.0f);
                g->stops[g->nstops].has_pos = 1;
                g->nstops++;
            }
        }
        p = se + 1;
    }
}

/* Render inline SVG with <polyline> conversion, currentColor, and <linearGradient> url(#id) fills! */
static uint32_t *render_inline_svg_enhanced(const char *svg, int rw, int rh, uint32_t cur_color) {
    if (!svg || !g_basi || !g_basi->mem) return 0;
    uint32_t *out = img_alloc_extra(rw, rh);
    if (!out) return 0;
    for (int i = 0; i < rw * rh; i++) out[i] = 0;

    int slen = b_len(svg);
    char *fixed = (char *)g_basi->mem->alloc((size_t)slen + 1024);
    if (!fixed) return 0;

    static const char HEXC[] = "0123456789abcdef";
    char ccol[8];
    ccol[0] = '#';
    ccol[1] = HEXC[(cur_color >> 20) & 0xF]; ccol[2] = HEXC[(cur_color >> 16) & 0xF];
    ccol[3] = HEXC[(cur_color >> 12) & 0xF]; ccol[4] = HEXC[(cur_color >> 8) & 0xF];
    ccol[5] = HEXC[(cur_color >> 4) & 0xF];  ccol[6] = HEXC[cur_color & 0xF];
    ccol[7] = 0;

    /* Pass 1: Convert <polyline points="..."> -> <path d="M ..."/>, currentColor -> #rrggbb, and url(#id) -> sentinels */
    css_grad_t sgrads[4];
    uint32_t   sentinels[4] = { 0xFE0102u, 0xFE0304u, 0xFE0506u, 0xFE0708u };
    int ng = 0;

    const char *p = svg;
    int o = 0, cap = slen + 1000;
    while (*p && o < cap - 32) {
        if (strn_ci_eq(p, "currentColor", 12)) {
            for (int k = 0; k < 7 && o < cap - 4; k++) fixed[o++] = ccol[k];
            p += 12;
            continue;
        }
        if (strn_ci_eq(p, "<polyline", 9)) {
            const char *te = p; while (*te && *te != '>') te++;
            char pts[256];
            if (find_attr(p, te, "points", pts, sizeof(pts))) {
                const char *hd = "<path d=\"M";
                for (int k = 0; hd[k] && o < cap - 16; k++) fixed[o++] = hd[k];
                for (int k = 0; pts[k] && o < cap - 16; k++) fixed[o++] = pts[k];
                const char *tl = "\"/>";
                for (int k = 0; tl[k] && o < cap - 4; k++) fixed[o++] = tl[k];
            }
            p = (*te == '>') ? te + 1 : te;
            const char *cl = find_substr_ci(p, "</polyline>");
            if (cl && cl - p < 4) p = cl + 11;
            continue;
        }
        if (strn_ci_eq(p, "url(#", 5)) {
            p += 5;
            char gid[48]; int gi = 0;
            while (*p && *p != ')' && gi < 47) gid[gi++] = *p++;
            gid[gi] = 0;
            if (*p == ')') p++;
            int slot = ng;
            if (ng < 4) { svg_extract_grad_color(svg, gid, &sgrads[ng]); ng++; }
            else slot = 0;
            /* Emit #FE0102 etc. */
            static const char *hexcols[4] = { "#fe0102", "#fe0304", "#fe0506", "#fe0708" };
            const char *hc = hexcols[slot];
            for (int k = 0; hc[k] && o < cap - 4; k++) fixed[o++] = hc[k];
            continue;
        }
        fixed[o++] = *p++;
    }
    fixed[o] = 0;

    svg_render(fixed, rw, rh, out);
    g_basi->mem->free(fixed);

    /* Pass 2: Replace sentinel pixels with real smooth linearGradient colors! */
    if (ng > 0) {
        for (int y = 0; y < rh; y++) {
            for (int x = 0; x < rw; x++) {
                uint32_t p0 = out[y * rw + x];
                uint32_t a = (p0 >> 24) & 0xFF;
                if (!a) continue;
                uint32_t rgb = p0 & 0x00FFFFFFu;
                for (int gi = 0; gi < ng; gi++) {
                    if (rgb == sentinels[gi] && sgrads[gi].nstops >= 2) {
                        uint32_t gc = sample_grad(&sgrads[gi], (float)x, (float)y, rw, rh);
                        out[y * rw + x] = (a << 24) | (gc & 0x00FFFFFFu);
                        break;
                    }
                }
            }
        }
    }
    return out;
}

/* ---------------- Apply Box & Position to LVGL ---------------- */
static void apply_box(lv_obj_t *o, css_style_t *s, int box_w, int box_h) {
    int use_surface = style_needs_custom_surface(s);
    if (!use_surface && !s->bg_clip_text) {
        if (s->has_bg) {
            if (s->bg_transparent) lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
            else {
                lv_obj_set_style_bg_color(o, lv_color_hex(s->bg), 0);
                lv_obj_set_style_bg_opa(o, (lv_opa_t)s->bg_a, 0);
            }
        }
    }
    /* Borders (per-side or uniform solid) */
    if (s->border_w > 0 && s->border_style == 0) {
        lv_obj_set_style_border_width(o, s->border_w, 0);
        lv_obj_set_style_border_color(o, lv_color_hex(s->border_c), 0);
        lv_obj_set_style_border_opa(o, (lv_opa_t)s->border_a, 0);
        if (s->border_sides && s->border_sides != 15) {
            lv_border_side_t bs = LV_BORDER_SIDE_NONE;
            if (s->border_sides & 1) bs = (lv_border_side_t)(bs | LV_BORDER_SIDE_TOP);
            if (s->border_sides & 2) bs = (lv_border_side_t)(bs | LV_BORDER_SIDE_RIGHT);
            if (s->border_sides & 4) bs = (lv_border_side_t)(bs | LV_BORDER_SIDE_BOTTOM);
            if (s->border_sides & 8) bs = (lv_border_side_t)(bs | LV_BORDER_SIDE_LEFT);
            lv_obj_set_style_border_side(o, bs, 0);
        }
    }
    /* Radius */
    if (!s->has_corner_radius && s->radius > 0) {
        if (s->radius_pct && s->radius >= 50) lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
        else lv_obj_set_style_radius(o, s->radius, 0);
    }
    /* Box shadows (pick strongest non-inset shadow for LVGL) */
    if (s->nshadows > 0) {
        int best = -1;
        for (int i = 0; i < s->nshadows; i++) {
            if (!s->shadows[i].inset) { best = i; if (s->shadows[i].rgb != 0) break; }
        }
        if (best >= 0) {
            const css_shadow_t *sh = &s->shadows[best];
            lv_obj_set_style_shadow_width(o, sh->blur > 0 ? sh->blur : 4, 0);
            lv_obj_set_style_shadow_color(o, lv_color_hex(sh->rgb), 0);
            lv_obj_set_style_shadow_opa(o, (lv_opa_t)sh->a, 0);
            lv_obj_set_style_shadow_ofs_x(o, sh->x, 0);
            lv_obj_set_style_shadow_ofs_y(o, sh->y, 0);
            lv_obj_set_style_shadow_spread(o, sh->spread, 0);
        }
    }
    /* Outline */
    if (s->has_outline && s->outline_w > 0) {
        lv_obj_set_style_outline_width(o, s->outline_w, 0);
        lv_obj_set_style_outline_color(o, lv_color_hex(s->outline_c), 0);
        lv_obj_set_style_outline_opa(o, (lv_opa_t)s->outline_a, 0);
        lv_obj_set_style_outline_pad(o, s->outline_offset, 0);
    }
    lv_obj_set_style_pad_top(o, s->pad[0], 0);
    lv_obj_set_style_pad_right(o, s->pad[1], 0);
    lv_obj_set_style_pad_bottom(o, s->pad[2], 0);
    lv_obj_set_style_pad_left(o, s->pad[3], 0);
    if (s->has_mar || s->mar[0] || s->mar[1] || s->mar[2] || s->mar[3]) {
        lv_obj_set_style_margin_top(o, s->mar[0], 0);
        lv_obj_set_style_margin_right(o, s->mar[1], 0);
        lv_obj_set_style_margin_bottom(o, s->mar[2], 0);
        lv_obj_set_style_margin_left(o, s->mar[3], 0);
    }
    if (s->has_minw) lv_obj_set_style_min_width(o, s->minw_pct ? LV_PCT(s->minw) : s->minw, 0);
    if (s->has_maxw) lv_obj_set_style_max_width(o, s->maxw_pct ? LV_PCT(s->maxw) : s->maxw, 0);
    if (s->has_minh) lv_obj_set_style_min_height(o, s->minh_pct ? LV_PCT(s->minh) : s->minh, 0);
    if (s->has_maxh) lv_obj_set_style_max_height(o, s->maxh_pct ? LV_PCT(s->maxh) : s->maxh, 0);
    if (s->has_opacity && s->opacity < 100)
        lv_obj_set_style_opa(o, (lv_opa_t)(s->opacity * 255 / 100), 0);
    if (s->has_translate && s->position != 1) {
        int tx = s->tx_pct ? (box_w * s->tx / 100) : s->tx;
        int ty = s->ty_pct ? (box_h * s->ty / 100) : s->ty;
        if (tx) lv_obj_set_style_translate_x(o, tx, 0);
        if (ty) lv_obj_set_style_translate_y(o, ty, 0);
    }
    if (s->has_scale) {
        lv_obj_set_style_transform_scale_x(o, s->tscalex, 0);
        lv_obj_set_style_transform_scale_y(o, s->tscaley, 0);
    }
    if (s->has_rot && !s->writing_vert) {
        lv_obj_set_style_transform_rotation(o, s->trot, 0);
        lv_obj_set_style_transform_pivot_x(o, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(o, LV_PCT(50), 0);
    }
    if (s->overflow_hidden) lv_obj_set_style_clip_corner(o, true, 0);
}

static void apply_position(lv_obj_t *o, css_style_t *s, int par_w, int par_h) {
    if (s->position == 2 || s->position == 3) {
        lv_obj_add_flag(o, LV_OBJ_FLAG_IGNORE_LAYOUT);
        int right = s->has_right && !s->has_left;
        int bottom = s->has_bottom && !s->has_top;
        int mid_y = (s->has_top && s->top_pct && s->top == 50 && s->has_translate && s->ty_pct && s->ty == -50);
        int mid_x = (s->has_left && s->left_pct && s->left == 50 && s->has_translate && s->tx_pct && s->tx == -50);
        int xo = s->has_left ? (s->left_pct ? par_w * s->left / 100 : s->left)
                             : (s->has_right ? -(s->right_pct ? par_w * s->right / 100 : s->right) : 0);
        int yo = s->has_top ? (s->top_pct ? par_h * s->top / 100 : s->top)
                            : (s->has_bottom ? -(s->bottom_pct ? par_h * s->bottom / 100 : s->bottom) : 0);
        if (mid_x && mid_y) {
            lv_obj_set_style_translate_x(o, 0, 0);
            lv_obj_set_style_translate_y(o, 0, 0);
            lv_obj_align(o, LV_ALIGN_CENTER, 0, 0);
        } else if (mid_y && right) {
            lv_obj_set_style_translate_y(o, 0, 0);
            lv_obj_align(o, LV_ALIGN_RIGHT_MID, xo, 0);
        } else if (mid_y) {
            lv_obj_set_style_translate_y(o, 0, 0);
            lv_obj_align(o, LV_ALIGN_LEFT_MID, xo, 0);
        } else if (mid_x && bottom) {
            lv_obj_set_style_translate_x(o, 0, 0);
            lv_obj_align(o, LV_ALIGN_BOTTOM_MID, 0, yo);
        } else if (mid_x) {
            lv_obj_set_style_translate_x(o, 0, 0);
            lv_obj_align(o, LV_ALIGN_TOP_MID, 0, yo);
        } else if (right && bottom) lv_obj_align(o, LV_ALIGN_BOTTOM_RIGHT, xo, yo);
        else if (right) lv_obj_align(o, LV_ALIGN_TOP_RIGHT, xo, yo);
        else if (bottom) lv_obj_align(o, LV_ALIGN_BOTTOM_LEFT, xo, yo);
        else if (s->left_pct && s->left == 50 && s->has_bottom) lv_obj_align(o, LV_ALIGN_BOTTOM_MID, 0, yo);
        else lv_obj_set_pos(o, xo, yo);
    } else if (s->position == 1 && (s->has_left || s->has_top || s->has_translate)) {
        int xx = (s->has_left ? s->left : 0) + (s->has_translate && !s->tx_pct ? s->tx : 0);
        int yy = (s->has_top ? s->top : 0) + (s->has_translate && !s->ty_pct ? s->ty : 0);
        if (xx) lv_obj_set_style_translate_x(o, xx, 0);
        if (yy) lv_obj_set_style_translate_y(o, yy, 0);
    }
    if (s->has_z && s->z_index < 0) lv_obj_move_background(o);
    else if (s->has_z && s->z_index > 0) lv_obj_move_foreground(o);
}

/* ---------------- Interactive Callbacks (CSS3 :checked, :target, <details>, :focus, :valid/:invalid, :hover) ---------------- */
static void page_submit(int form_id) {
    if (form_id < 0 || form_id >= g_pform_count) return;
    if (g_pforms[form_id].action[0] == '#') {
        bcopy_str(g_cur_hash, g_pforms[form_id].action + 1, sizeof(g_cur_hash));
        restyle_page();
        return;
    }
    char action[220];
    if (g_pforms[form_id].action[0])
        resolve_url(g_cur_scheme, g_cur_host, g_pforms[form_id].action, action, sizeof(action));
    else bcopy_str(action, g_cur_url, sizeof(action));
    if (!action[0]) bcopy_str(action, g_cur_url, sizeof(action));
    int is_post = str_eq_ci(g_pforms[form_id].method, "post") &&
                  g_basi && g_basi->net && g_basi->net->http_request;
    char body[2048]; body[0] = 0; int first = 1;
    for (int i = 0; i < g_dn_count; i++) {
        dnode_t *d = &g_dn[i];
        if (d->form_id != form_id || !d->name[0]) continue;
        const char *value = 0;
        if (str_eq(d->tag, "input")) {
            if (str_eq(d->type, "submit") || str_eq(d->type, "button") || str_eq(d->type, "reset")) continue;
            if (str_eq(d->type, "checkbox") || str_eq(d->type, "radio")) {
                if (!d->checked) continue;
                value = d->value[0] ? d->value : "on";
            } else if (str_eq(d->type, "hidden")) value = d->value;
            else if (d->widget && d->is_ta)
                value = lv_textarea_get_text(d->widget);
            else value = d->value;
        } else if (str_eq(d->tag, "textarea")) {
            value = (d->widget && d->is_ta) ? lv_textarea_get_text(d->widget) : d->value;
        } else continue;
        if (!value) continue;
        if (!first) bcat(body, "&", sizeof(body));
        first = 0;
        url_encode_append(body, sizeof(body), d->name);
        bcat(body, "=", sizeof(body));
        url_encode_append(body, sizeof(body), value);
    }
    if (is_post) {
        if (g_addr) lv_textarea_set_text(g_addr, action);
        navigate_post(action, body, 1);
    } else {
        char full[URL_MAX]; bcopy_str(full, action, sizeof(full));
        if (body[0]) {
            bcat(full, find_substr_ci(action, "?") ? "&" : "?", sizeof(full));
            bcat(full, body, sizeof(full));
        }
        if (g_addr) lv_textarea_set_text(g_addr, full);
        navigate(full, 1);
    }
}

static void toggle_input_node(int target) {
    if (target < 0 || target >= g_dn_count) return;
    dnode_t *td = &g_dn[target];
    if (str_eq(td->type, "radio")) {
        if (td->name[0]) {
            for (int i = 0; i < g_dn_count; i++)
                if (str_eq(g_dn[i].tag, "input") && str_eq(g_dn[i].type, "radio") && str_eq(g_dn[i].name, td->name))
                    g_dn[i].checked = 0;
        }
        td->checked = 1;
    } else {
        td->checked = td->checked ? 0 : 1;
    }
    g_restyle_pending = 1;
}

static void page_label_cb(lv_event_t *e) {
    int n = (int)(intptr_t)lv_event_get_user_data(e);
    if (n < 0 || n >= g_dn_count) return;
    int target = -1;
    if (g_dn[n].for_attr[0]) {
        for (int i = 0; i < g_dn_count; i++)
            if (g_dn[i].id[0] && str_eq(g_dn[i].id, g_dn[n].for_attr)) { target = i; break; }
    } else {
        for (int c = g_dn[n].first; c >= 0; c = g_dn[c].next)
            if (str_eq(g_dn[c].tag, "input")) { target = c; break; }
    }
    if (target >= 0) {
        if (str_eq(g_dn[target].type, "checkbox") || str_eq(g_dn[target].type, "radio"))
            toggle_input_node(target);
        else if (g_dn[target].widget && g_group) {
            g_focus_node = target;
            lv_group_focus_obj(g_dn[target].widget);
        }
    }
}

static void page_file_click_cb(lv_event_t *e) {
    int n = (int)(intptr_t)lv_event_get_user_data(e);
    file_picker_open(n);
}

static void page_checkbox_cb(lv_event_t *e) {
    int n = (int)(intptr_t)lv_event_get_user_data(e);
    toggle_input_node(n);
}

static void page_summary_cb(lv_event_t *e) {
    int n = (int)(intptr_t)lv_event_get_user_data(e);
    if (n < 0 || n >= g_dn_count) return;
    int par = g_dn[n].parent;
    if (par >= 0 && str_eq(g_dn[par].tag, "details")) {
        g_dn[par].is_open = g_dn[par].is_open ? 0 : 1;
        g_restyle_pending = 1;
    }
}

static void page_input_update_style(int n) {
    if (n < 0 || n >= g_dn_count || !g_dn[n].widget || !g_dn[n].is_ta) return;
    lv_obj_t *ta = g_dn[n].widget;
    const char *v = lv_textarea_get_text(ta);
    if (v) bcopy_str(g_dn[n].value, v, sizeof(g_dn[n].value));
    /* Recompute style for dynamic :focus / :valid / :invalid / :placeholder-shown */
    css_style_t s;
    g_vstk_n = 0;
    /* Re-push :root vars quickly */
    css_style_t root_s;
    compute_style(0, 0, &root_s, 0, 0);
    /* Also check theme-toggle override on .app-wrapper */
    for (int i = 1; i < g_dn_count; i++) {
        if (class_has(g_dn[i].cls, "app-wrapper", 11)) {
            css_style_t aw; compute_style(i, &root_s, &aw, 0, 1); break;
        }
    }
    compute_style(n, 0, &s, 0, 2);
    css_var_pop(0);
    if (s.border_w > 0) {
        lv_obj_set_style_border_width(ta, s.border_w, 0);
        lv_obj_set_style_border_color(ta, lv_color_hex(s.border_c), 0);
        lv_obj_set_style_border_opa(ta, (lv_opa_t)s.border_a, 0);
    }
    if (s.has_outline && s.outline_w > 0) {
        lv_obj_set_style_outline_width(ta, s.outline_w, 0);
        lv_obj_set_style_outline_color(ta, lv_color_hex(s.outline_c), 0);
        lv_obj_set_style_outline_opa(ta, (lv_opa_t)s.outline_a, 0);
        lv_obj_set_style_outline_pad(ta, s.outline_offset, 0);
    } else {
        lv_obj_set_style_outline_width(ta, 0, 0);
    }
}

static void page_input_focus_cb(lv_event_t *e) {
    int n = (int)(intptr_t)lv_event_get_user_data(e);
    if (n < 0 || n >= g_dn_count) return;
    g_focus_node = n;
    if (g_group && g_dn[n].widget) lv_group_focus_obj(g_dn[n].widget);
    page_input_update_style(n);
}

static void page_input_blur_cb(lv_event_t *e) {
    int n = (int)(intptr_t)lv_event_get_user_data(e);
    if (g_focus_node == n) g_focus_node = -1;
    page_input_update_style(n);
}

static void page_input_changed_cb(lv_event_t *e) {
    int n = (int)(intptr_t)lv_event_get_user_data(e);
    page_input_update_style(n);
    js_dispatch_dom_event(n, "input");
}

static void page_input_ready_cb(lv_event_t *e) {
    int n = (int)(intptr_t)lv_event_get_user_data(e);
    if (n < 0 || n >= g_dn_count) return;
    if (!js_dispatch_dom_event(n, "submit"))
        page_submit(g_dn[n].form_id);
}

static void page_submit_cb(lv_event_t *e) {
    int n = (int)(intptr_t)lv_event_get_user_data(e);
    if (n < 0 || n >= g_dn_count) return;
    if (js_dispatch_dom_event(n, "click")) return;
    if (g_dn[n].form_id >= 0 && !str_eq(g_dn[n].type, "button")) {
        int fn = g_pforms[g_dn[n].form_id].node;
        if (!js_dispatch_dom_event(fn, "submit"))
            page_submit(g_dn[n].form_id);
    }
}

static void page_js_click_cb(lv_event_t *e) {
    int n = (int)(intptr_t)lv_event_get_user_data(e);
    if (n < 0 || n >= g_dn_count) return;
    js_dispatch_dom_event(n, "click");
}

static void page_link_cb(lv_event_t *e) {
    int n = (int)(intptr_t)lv_event_get_user_data(e);
    if (n < 0 || n >= g_dn_count) return;
    if (js_dispatch_dom_event(n, "click")) return;
    if (g_dn[n].href[0] == '#') {
        bcopy_str(g_cur_hash, g_dn[n].href + 1, sizeof(g_cur_hash));
        g_restyle_pending = 1;
        /* Also scroll to non-modal anchor target if present */
        for (int i = 0; i < g_dn_count; i++) {
            if (g_dn[i].id[0] && str_eq(g_dn[i].id, g_cur_hash) &&
                !class_has(g_dn[i].cls, "modal-overlay", 13) && g_dn[i].widget && g_content) {
                lv_obj_scroll_to_view_recursive(g_dn[i].widget, LV_ANIM_OFF);
                break;
            }
        }
        return;
    }
    char dl_a[8];
    if (dn_attr(n, "download", dl_a, sizeof(dl_a))) g_nav_force_dl = 1;
    char abs[220];
    resolve_url(g_cur_scheme, g_cur_host, g_dn[n].href, abs, sizeof(abs));
    if (abs[0]) navigate(abs, 1);
}

/* Sayfa kaydirmasini ust (0) ve alt sinir icinde sıkıca kilitle (overscroll yok) */
static void content_scroll_clamp_cb(lv_event_t *e) {
    static int in_clamp = 0;
    if (in_clamp) return;
    lv_obj_t *obj = lv_event_get_target(e);
    if (!obj) return;
    int32_t sy = lv_obj_get_scroll_y(obj);
    int32_t sb = lv_obj_get_scroll_bottom(obj);
    if (sy < 0) {
        in_clamp = 1;
        lv_obj_scroll_to_y(obj, 0, LV_ANIM_OFF);
        in_clamp = 0;
    } else if (sy > 0 && sb < 0) {
        int32_t ny = sy + sb;
        if (ny < 0) ny = 0;
        in_clamp = 1;
        lv_obj_scroll_to_y(obj, ny, LV_ANIM_OFF);
        in_clamp = 0;
    }
}

static void scroll_content_clamped(int32_t dy) {
    if (!g_content || dy == 0) return;
    lv_obj_update_layout(g_content);
    if (dy > 0) {
        int32_t top = lv_obj_get_scroll_top(g_content);
        if (top <= 0) return;
        if (dy > top) dy = top;
    } else {
        int32_t bot = lv_obj_get_scroll_bottom(g_content);
        if (bot <= 0) return;
        if (-dy > bot) dy = -bot;
    }
    if (dy != 0) lv_obj_scroll_by(g_content, 0, dy, LV_ANIM_OFF);
}

static void page_wheel_cb(lv_timer_t *t) {
    (void)t;
    js_tick();
    ws_poll_all();
    /* Pencere yeniden boyutlandirildiginda (orn. tam ekran) @media ve yerlesimi guncelle */
    if (g_content && g_dn_count > 1 && !g_nav_in_flight) {
        int cw = (int)lv_obj_get_width(g_content);
        int ch = (int)lv_obj_get_height(g_content);
        if (cw > 40 && ch > 40 && (cw != g_view_w || ch != g_view_h)) {
            g_view_w = cw;
            g_view_h = ch;
            if (g_raw && g_raw[0]) collect_css_from_html(g_raw);
            g_restyle_pending = 1;
        }
    }
    if (g_restyle_pending) {
        g_restyle_pending = 0;
        restyle_page();
    }
    /* Sticky header + scroll wheel (sinirlar disina tasmaz) */
    if (g_basi && g_basi->input && g_basi->input->mouse_scroll && g_content) {
        int sc = g_basi->input->mouse_scroll();
        if (sc != 0) {
            int mx = g_basi->input->mouse_x();
            int my = g_basi->input->mouse_y();
            lv_area_t a; lv_obj_get_coords(g_content, &a);
            if (mx >= a.x1 && mx <= a.x2 && my >= a.y1 && my <= a.y2)
                scroll_content_clamped(-sc * 48);
        }
    }
    if (g_sticky_obj && g_content) {
        int32_t sy = lv_obj_get_scroll_y(g_content);
        if (sy < 0) sy = 0;
        lv_obj_set_style_translate_y(g_sticky_obj, sy, 0);
    }
    /* Live CSS :hover hit-testing (triggers restyle only when entering/leaving hover-sensitive nodes) */
    if (g_basi && g_basi->input && g_basi->input->mouse_x && g_content && g_dn_count > 1) {
        int mx = g_basi->input->mouse_x();
        int my = g_basi->input->mouse_y();
        if (mx != g_last_mx || my != g_last_my) {
            g_last_mx = mx; g_last_my = my;
            int hit = -1;
            /* Walk DOM backwards (later/deeper nodes sit on top) for hover-sensitive elements */
            for (int i = g_dn_count - 1; i >= 0; i--) {
                if (!g_dn[i].hover_sens || !g_dn[i].widget) continue;
                lv_area_t a; lv_obj_get_coords(g_dn[i].widget, &a);
                if (mx >= a.x1 && mx <= a.x2 && my >= a.y1 && my <= a.y2) {
                    hit = i; break;
                }
            }
            if (hit != g_hover_node) {
                g_hover_node = hit;
                restyle_page();
            }
        }
    }
    /* Live CSS @keyframes animations */
    if (g_anim_n > 0 && g_basi && g_basi->time && g_basi->time->ticks_ms) {
        uint32_t now = (uint32_t)g_basi->time->ticks_ms();
        for (int i = 0; i < g_anim_n; i++) {
            css_anim_ent_t *ae = &g_anims[i];
            if (!ae->obj || ae->paused || ae->dur_ms <= 0) continue;
            uint32_t phase = now % (uint32_t)ae->dur_ms;
            float t = (float)phase / (float)ae->dur_ms;
            if (ae->kind == 1) { /* pulse-dot opacity */
                float tri = t < 0.5f ? (1.0f - t * 1.4f) : (0.3f + (t - 0.5f) * 1.4f);
                if (tri < 0.3f) tri = 0.3f; if (tri > 1.0f) tri = 1.0f;
                lv_obj_set_style_opa(ae->obj, (lv_opa_t)(tri * 255.0f), 0);
            } else if (ae->kind == 2 && ae->param > 20) { /* marquee-scroll */
                int tx = -(int)(t * (float)ae->param);
                lv_obj_set_style_translate_x(ae->obj, tx, 0);
            }
        }
    }
}

/* ---------------- Standard Label Helper ---------------- */
static int text_has_emoji(const char *s) {
    const char *p = s;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        if (cp == 0x1F319 || cp == 0x2600 || cp == 0x1F504 || cp == 0x26A1) return 1;
    }
    return 0;
}

static void text_label(lv_obj_t *o, const char *txt, const css_style_t *s, const css_style_t *ps, int max_w, int wrap) {
    if (!txt || !txt[0]) return;
    int need_fancy = s->bg_clip_text || s->writing_vert || s->blend_mode == 1 || text_has_emoji(txt) ||
                     (ps && ps->bg_clip_text && !s->has_color && !s->has_bg);
    if (need_fancy) {
        create_fancy_text_canvas(o, txt, s, ps, max_w, wrap);
        return;
    }
    char tb[CSS_NTEXT];
    apply_text_transform(txt, tb, sizeof(tb), s->transform);
    css_trim(tb);
    if (!tb[0]) return;
    lv_obj_t *l = lv_label_create(o);
    lv_label_set_text(l, tb);
    int fsz = s->font_size * g_zoom_pct / 100;
    if (fsz < 8) fsz = 8;
    if (fsz > 72) fsz = 72;
    lv_font_t *f = bfont_ex(fsz, s->bold);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(s->color), 0);
    lv_obj_set_style_text_opa(l, (lv_opa_t)s->color_a, 0);
    lv_obj_set_style_text_align(l, s->align == 1 ? LV_TEXT_ALIGN_CENTER :
                                    s->align == 2 ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    lv_text_decor_t decor = LV_TEXT_DECOR_NONE;
    if (s->underline) decor = (lv_text_decor_t)(decor | LV_TEXT_DECOR_UNDERLINE);
    if (s->strike) decor = (lv_text_decor_t)(decor | LV_TEXT_DECOR_STRIKETHROUGH);
    if (decor) lv_obj_set_style_text_decor(l, decor, 0);
    if (s->letter_space) lv_obj_set_style_text_letter_space(l, s->letter_space, 0);
    if (s->line_height > f->line_height) lv_obj_set_style_text_line_space(l, s->line_height - f->line_height, 0);
    if (s->nowrap) {
        if (s->ellipsis) { lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS); lv_obj_set_width(l, LV_PCT(100)); }
        else lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
    } else if (wrap) {
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        if (max_w > 20) lv_obj_set_width(l, max_w);
        else lv_obj_set_width(l, LV_PCT(100));
    }
}

static lv_obj_t *make_image(lv_obj_t *parent, const char *src, int wa, int wa_pct, int ha, int ha_pct, int fit) {
    (void)fit;
    if (!src[0]) return 0;
    int w = 0, h = 0;
    uint32_t *px = 0;
    if (starts_with(src, "data:")) {
        px = img_from_data_uri(src, &w, &h);
        if (px && g_img_extra_n < IMG_EXTRA) g_img_extra[g_img_extra_n++] = px;
    } else {
        char abs[440]; resolve_url(g_cur_scheme, g_cur_host, src, abs, sizeof(abs));
        if (!abs[0]) return 0;
        px = img_get(abs, &w, &h);
    }
    if (!px || w <= 0 || h <= 0) return 0;
    uint32_t *buf = px; int bw = w, bh = h;
    int dw = (wa > 0 && !wa_pct) ? wa : 0, dh = (ha > 0 && !ha_pct) ? ha : 0;
    if (dw > 0 && dh > 0 && (dw != w || dh != h)) {
        uint32_t *sc = img_scale(px, w, h, dw, dh);
        if (sc) { buf = sc; bw = dw; bh = dh; }
    }
    lv_obj_t *c = lv_canvas_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_canvas_set_buffer(c, buf, bw, bh, LV_COLOR_FORMAT_ARGB8888);
    return c;
}

/* ---------------- Grid & Pseudo-Elements & DOM Render ---------------- */
#define GRID_SLOTS 192
#define GRID_ROWS  34
static int32_t g_grid_col[GRID_SLOTS][18];
static int32_t g_grid_row[GRID_SLOTS][GRID_ROWS];
static int     g_grid_slot;
static int     g_in_shrink_w;

static void build_pseudo_elem(lv_obj_t *o, int n, css_style_t *ps, int which, int par_w, int par_h, int depth) {
    css_style_t sb;
    compute_style(n, ps, &sb, which, depth + 1);
    css_var_pop(depth + 1);
    if (!sb.has_content || sb.hide) return;
    if (sb.has_opacity && sb.opacity == 0) return;

    char txt[200]; txt[0] = 0;
    if (sb.content_url && sb.content_src[0]) {
        make_image(o, sb.content_src, -1, 0, -1, 0, sb.object_fit);
        return;
    }
    if (sb.content[0] == 1) dn_attr(n, sb.content + 1, txt, sizeof(txt));
    else bcopy_str(txt, sb.content, sizeof(txt));

    int bw = sb.has_w ? (sb.w_pct ? par_w * sb.width / 100 : sb.width) : 0;
    int bh = sb.has_h ? (sb.h_pct ? par_h * sb.height / 100 : sb.height) : 0;

    /* Empty-content decorative pseudo-element (e.g. .conic-ring::before, .nav-links a::after) */
    if (!txt[0]) {
        if (bw <= 0 && bh <= 0) return;
        lv_obj_t *pe = lv_obj_create(o);
        lv_obj_remove_style_all(pe);
        lv_obj_remove_flag(pe, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(pe, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(pe, bw > 0 ? bw : LV_SIZE_CONTENT, bh > 0 ? bh : LV_SIZE_CONTENT);
        apply_box(pe, &sb, bw, bh);
        if (style_needs_custom_surface(&sb) && bw > 0 && bh > 0) paint_css_surface(pe, bw, bh, &sb);
        if (sb.position == 2) {
            lv_obj_add_flag(pe, LV_OBJ_FLAG_IGNORE_LAYOUT);
            if (!sb.has_top && !sb.has_bottom && !sb.has_left && !sb.has_right)
                lv_obj_center(pe);
            else
                apply_position(pe, &sb, par_w, par_h);
        }
        return;
    }

    /* Styled text pseudo-element (e.g. summary::after '+'/'-', [data-tooltip]::after) */
    if (sb.position == 2 || sb.has_bg || sb.border_w > 0 || sb.has_pad) {
        lv_obj_t *pe = lv_obj_create(o);
        lv_obj_remove_style_all(pe);
        lv_obj_remove_flag(pe, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(pe, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(pe, bw > 0 ? bw : LV_SIZE_CONTENT, bh > 0 ? bh : LV_SIZE_CONTENT);
        apply_box(pe, &sb, bw, bh);
        text_label(pe, txt, &sb, ps, 0, 0);
        if (sb.position == 2) {
            apply_position(pe, &sb, par_w, par_h);
            lv_obj_move_foreground(pe);
        }
    } else {
        text_label(o, txt, &sb, ps, 0, 0);
    }
}

static void mark_hover_sensitivity(void) {
    for (int i = 0; i < g_dn_count; i++) {
        g_dn[i].hover_sens = 0;
        g_dn[i].focus_sens = 0;
    }
    /* For every rule with :hover, pre-filter by rightmost key to avoid O(R*N) sel_match! */
    for (int r = 0; r < g_css_count; r++) {
        if (!(g_css[r].flags & RULE_F_HOVER)) continue;
        const css_rule_t *cr = &g_css[r];
        char sel[CSS_SEL]; bcopy_str(sel, cr->sel, sizeof(sel));
        const char *hv = find_substr_ci(sel, ":hover");
        if (!hv) continue;
        /* Check if :hover is in rightmost compound (no combinator after :hover) */
        int is_rightmost = 1;
        for (const char *q = hv + 6; *q; q++) {
            if (*q == ' ' || *q == '>' || *q == '+' || *q == '~') { is_rightmost = 0; break; }
        }
        /* Extract prefix up to :hover */
        char prefix[CSS_SEL];
        int pl = (int)(hv - sel);
        if (pl >= CSS_SEL) pl = CSS_SEL - 1;
        for (int k = 0; k < pl; k++) prefix[k] = sel[k];
        prefix[pl] = 0;
        css_trim(prefix);
        if (!prefix[0]) continue;
        /* On huge DOMs (>800 nodes), skip expensive unkeyed universal hover rules */
        if (g_dn_count > 800 && (!is_rightmost || (!cr->key_id[0] && !cr->key_cls[0] && !cr->key_tag[0])))
            continue;
        int save_pe = g_pe; g_pe = 0;
        for (int i = 0; i < g_dn_count; i++) {
            if (g_dn[i].is_text || g_dn[i].hover_sens) continue;
            if (is_rightmost) {
                if (cr->key_id[0] && !str_eq(g_dn[i].id, cr->key_id)) continue;
                if (cr->key_cls[0] && !class_has(g_dn[i].cls, cr->key_cls, cr->key_cls_len)) continue;
                if (cr->key_tag[0] && !str_eq(g_dn[i].tag, cr->key_tag)) continue;
            }
            if (sel_match(i, prefix)) g_dn[i].hover_sens = 1;
        }
        g_pe = save_pe;
    }
}

static void register_anim(lv_obj_t *obj, int n, const css_style_t *s, int param) {
    if (!s->anim_name[0] || g_anim_n >= CSS_ANIM_MAX) return;
    int kind = 0;
    if (find_substr_ci(s->anim_name, "pulse-dot")) kind = 1;
    else if (find_substr_ci(s->anim_name, "marquee")) kind = 2;
    else if (find_substr_ci(s->anim_name, "pulse-glow")) kind = 3;
    if (!kind) return;
    g_anims[g_anim_n].obj = obj;
    g_anims[g_anim_n].node = n;
    g_anims[g_anim_n].kind = kind;
    g_anims[g_anim_n].dur_ms = s->anim_dur_ms > 100 ? s->anim_dur_ms : 2000;
    g_anims[g_anim_n].param = param;
    g_anims[g_anim_n].paused = s->anim_paused;
    g_anim_n++;
}

static void build_node(int n, lv_obj_t *parent, css_style_t *ps, int avail_w, int avail_h, int depth) {
    if (n < 0 || n >= g_dn_count || depth > 40) return;
    dnode_t *d = &g_dn[n];
    d->widget = 0;

    if (d->is_text) {
        if (d->text[0]) text_label(parent, d->text, ps, ps, avail_w, 0);
        return;
    }

    int save_avail = g_cur_avail_w;
    int save_cont = g_cur_cont_w;
    g_cur_avail_w = avail_w;
    if (class_has(d->cls, "projects-container-query", 24)) g_cur_cont_w = avail_w;

    css_style_t s;
    compute_style(n, ps, &s, 0, depth);

    /* 3D preserve-3d & backface-visibility handling */
    int par_rot_y = (d->parent >= 0) ? g_dn[d->parent].eff_rot_y : 0;
    int my_rot_y = par_rot_y + (s.has_rot_y ? s.trot_y : 0);
    d->eff_rot_y = (int16_t)my_rot_y;
    int norm_ry = my_rot_y % 360;
    if (norm_ry < 0) norm_ry += 360;
    if (s.backface_hidden && norm_ry > 90 && norm_ry < 270) {
        css_var_pop(depth); g_cur_avail_w = save_avail; g_cur_cont_w = save_cont;
        return;
    }

    /* Hidden / sr-only / opacity:0 fixed modal */
    if (s.hide || s.vis_hidden || s.sr_clip || (s.position == 3 && s.has_opacity && s.opacity == 0)) {
        css_var_pop(depth); g_cur_avail_w = save_avail; g_cur_cont_w = save_cont;
        return;
    }
    /* SSR scroll-reveal placeholder (orn. GitHub React Intro__content style="opacity:0"): in-flow konteyneri gorunur kil */
    if (s.has_opacity && s.opacity == 0 && s.position <= 1 && !s.pointer_none && d->first >= 0) {
        s.opacity = 100;
        s.has_opacity = 0;
    }

    /* display: contents -> no box generated, children participate directly in parent's flex/grid! */
    if (s.display == 8) {
        for (int c = d->first; c >= 0; c = g_dn[c].next)
            build_node(c, parent, ps ? ps : &s, avail_w, avail_h, depth + 1);
        css_var_pop(depth); g_cur_avail_w = save_avail; g_cur_cont_w = save_cont;
        return;
    }

    /* <html> and <body>: set page background on g_content, and apply body flex/grid/centering/min-height */
    if (str_eq(d->tag, "html") || str_eq(d->tag, "body")) {
        lv_obj_t *child_par = parent;
        lv_obj_t *body_box = 0;
        int body_mh = 0;
        int child_w = avail_w, child_h = avail_h;
        if (str_eq(d->tag, "body")) {
            if (s.has_bg && !s.bg_transparent && g_content) {
                lv_obj_set_style_bg_color(g_content, lv_color_hex(s.bg), 0);
                lv_obj_set_style_bg_opa(g_content, LV_OPA_COVER, 0);
            }
            if (s.is_flex || s.is_grid || s.has_pad || s.has_minh || s.align != 0) {
                lv_obj_t *bo = lv_obj_create(parent);
                lv_obj_remove_style_all(bo);
                lv_obj_remove_flag(bo, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_clear_flag(bo, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_width(bo, LV_PCT(100));
                int mh = 0;
                if (s.has_minh) mh = s.minh_pct ? (g_view_h * s.minh / 100) : s.minh;
                else if (s.has_h) mh = s.h_pct ? (g_view_h * s.height / 100) : s.height;
                lv_obj_set_height(bo, LV_SIZE_CONTENT);
                if (mh > 0) {
                    lv_obj_set_style_min_height(bo, mh, 0);
                    child_h = mh;
                    body_mh = mh;
                }
                if (s.has_pad) {
                    lv_obj_set_style_pad_top(bo, s.pad[0], 0);
                    lv_obj_set_style_pad_right(bo, s.pad[1], 0);
                    lv_obj_set_style_pad_bottom(bo, s.pad[2], 0);
                    lv_obj_set_style_pad_left(bo, s.pad[3], 0);
                    child_w = avail_w - s.pad[1] - s.pad[3];
                    if (child_w < 40) child_w = 40;
                }
                if (s.is_flex || s.is_grid) {
                    int is_row = s.is_flex ? s.row : 0;
                    lv_obj_set_flex_flow(bo, is_row ? (s.wrap ? LV_FLEX_FLOW_ROW_WRAP : LV_FLEX_FLOW_ROW) : LV_FLEX_FLOW_COLUMN);
                    lv_flex_align_t ma = s.justify==1?LV_FLEX_ALIGN_CENTER:s.justify==2?LV_FLEX_ALIGN_END:
                                         s.justify==3?LV_FLEX_ALIGN_SPACE_BETWEEN:s.justify==4?LV_FLEX_ALIGN_SPACE_AROUND:LV_FLEX_ALIGN_START;
                    lv_flex_align_t ca = s.align_items==1?LV_FLEX_ALIGN_CENTER:s.align_items==2?LV_FLEX_ALIGN_END:LV_FLEX_ALIGN_START;
                    lv_obj_set_flex_align(bo, ma, ca, ca);
                    lv_obj_set_style_pad_column(bo, s.col_gap > 0 ? s.col_gap : s.gap, 0);
                    lv_obj_set_style_pad_row(bo, s.row_gap > 0 ? s.row_gap : s.gap, 0);
                } else {
                    lv_obj_set_flex_flow(bo, LV_FLEX_FLOW_COLUMN);
                    lv_flex_align_t ca = s.align==1?LV_FLEX_ALIGN_CENTER:s.align==2?LV_FLEX_ALIGN_END:LV_FLEX_ALIGN_START;
                    lv_obj_set_flex_align(bo, LV_FLEX_ALIGN_START, ca, ca);
                }
                d->widget = bo;
                body_box = bo;
                child_par = bo;
            }
        }
        for (int c = d->first; c >= 0; c = g_dn[c].next)
            build_node(c, child_par, &s, child_w, child_h, depth + 1);
        if (body_box && body_mh > 0) {
            lv_obj_update_layout(body_box);
            if ((int)lv_obj_get_height(body_box) <= body_mh)
                lv_obj_set_height(body_box, body_mh);
        }
        css_var_pop(depth); g_cur_avail_w = save_avail; g_cur_cont_w = save_cont;
        return;
    }

    /* Choose parent container (position: fixed goes to g_fixed_layer!) */
    lv_obj_t *target_parent = (s.position == 3 && g_fixed_layer) ? g_fixed_layer : parent;
    int my_avail = (s.position == 3) ? g_view_w : avail_w;
    int my_avail_h = (s.position == 3) ? g_view_h : avail_h;

    /* Resolve concrete box width & height for layout and custom canvas painting */
    int is_inline_el = (s.display == 1 || s.display == 2 || s.display == 4 || s.display == 6);
    int in_flex_row = (ps && ps->is_flex && ps->row);
    int in_centered_col = (ps && ((ps->is_flex && !ps->row && ps->align_items == 1) ||
                                  (ps->is_grid && (ps->justify_items == 1 || ps->align_items == 1))));
    int shrink_w = is_inline_el || in_flex_row || g_in_shrink_w ||
                   ((s.position == 2 || s.position == 3) && !(s.has_left && s.has_right)) ||
                   (in_centered_col && !s.has_w);
    int box_w = -1, box_h = -1;
    if (s.has_w) {
        if (in_flex_row && s.w_pct && s.width >= 95 && d->parent >= 0 && g_dn[d->parent].first != g_dn[d->parent].last) {
            /* width: 100% sibling inside a flex row (e.g. .marquee-container or .ctaContainer next to <nav>) */
            if (!g_in_shrink_w) {
                s.flex_grow = 1;
                box_w = -1;
            } else {
                box_w = my_avail > 120 ? (my_avail - 64) : my_avail;
                s.flex_grow = 0;
            }
        } else if (g_in_shrink_w && s.w_pct) {
            /* CSS spec: % width inside an indefinite shrink-to-fit container must NOT resolve against ancestor my_avail and blow up the container! */
            box_w = -1;
        } else {
            box_w = s.w_pct ? (my_avail * s.width / 100) : s.width;
        }
    }
    else if ((s.position == 2 || s.position == 3) && s.has_left && s.has_right) {
        if (s.position == 2 && g_in_shrink_w) box_w = -1;
        else box_w = my_avail - s.left - s.right;
    }
    else if (!shrink_w) box_w = my_avail;
    if (s.has_maxw) {
        int mw = s.maxw_pct ? (my_avail * s.maxw / 100) : s.maxw;
        if (s.maxw_pct && g_in_shrink_w) {
            /* LVGL resolves LV_PCT max-width against 0 inside LV_SIZE_CONTENT parents! */
            s.maxw = mw; s.maxw_pct = 0;
        }
        if ((!(is_inline_el || (in_flex_row && s.maxw_pct)) && box_w < 0) || (box_w > 0 && box_w > mw)) box_w = mw;
    }
    if (s.has_minw) {
        int mn = s.minw_pct ? (my_avail * s.minw / 100) : s.minw;
        if (box_w >= 0 && box_w < mn) box_w = mn;
    }
    if (s.has_h) {
        if (s.h_pct) {
            /* CSS spec: % height against auto-height parent computes to auto! */
            if (my_avail_h > 0) box_h = my_avail_h * s.height / 100;
            else s.has_h = 0;
        } else {
            box_h = s.height;
        }
    }
    else if (s.position == 3 && s.has_top && s.has_bottom) box_h = g_view_h - s.top - s.bottom;
    if (s.has_maxh) {
        if (s.maxh_pct) {
            if (my_avail_h > 0) {
                int mh = my_avail_h * s.maxh / 100;
                if (box_h > 0 && box_h > mh) box_h = mh;
                s.maxh = mh; s.maxh_pct = 0;
            } else {
                /* CSS spec: max-height: % against auto-height parent is none (prevents LVGL 0px collapse!) */
                s.has_maxh = 0;
            }
        } else if (box_h > 0 && box_h > s.maxh) {
            box_h = s.maxh;
        }
    }
    if (s.has_minh) {
        int mnh = s.minh_pct ? (my_avail_h > 0 ? (my_avail_h * s.minh / 100) : 0) : s.minh;
        if (box_h >= 0 && box_h < mnh) box_h = mnh;
    }
    if (s.has_aspect && s.aspect > 0 && box_w > 0 && box_h < 0)
        box_h = box_w * 1000 / s.aspect;

    /* Inline <svg> */
    if (str_eq(d->tag, "svg")) {
        int rw = 24, rh = 24;
        if (d->svg_len > 0) svg_intrinsic_size(g_svg_blob + d->svg_off, &rw, &rh);
        if (d->wa > 0 && !d->wa_pct) rw = d->wa;
        if (d->ha > 0 && !d->ha_pct) rh = d->ha;
        if (s.has_w) rw = s.w_pct ? (my_avail * s.width / 100) : s.width;
        if (s.has_h) rh = s.h_pct ? ((my_avail_h > 0 ? my_avail_h : my_avail) * s.height / 100) : s.height;
        if (rw < 8) rw = 24; if (rh < 8) rh = 24;
        if (rw > 400) rw = 400; if (rh > 400) rh = 400;
        lv_obj_t *o = lv_obj_create(target_parent);
        lv_obj_remove_style_all(o);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(o, rw, rh);
        apply_box(o, &s, rw, rh);
        if (d->svg_len > 0) {
            uint32_t *px = render_inline_svg_enhanced(g_svg_blob + d->svg_off, rw, rh, s.color);
            if (px) {
                lv_obj_t *c = lv_canvas_create(o);
                lv_obj_remove_style_all(c);
                lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
                lv_canvas_set_buffer(c, px, rw, rh, LV_COLOR_FORMAT_ARGB8888);
                lv_obj_center(c);
            }
        }
        d->widget = o;
        css_var_pop(depth); g_cur_avail_w = save_avail; g_cur_cont_w = save_cont;
        return;
    }

    /* Gomulu <iframe> (srcdoc veya src icerigini izole cercevede cizer) */
    if (str_eq(d->tag, "iframe")) {
        int fw = d->wa > 0 && !d->wa_pct ? d->wa : (box_w > 0 ? box_w : (my_avail > 520 ? 520 : my_avail));
        int fh = d->ha > 0 && !d->ha_pct ? d->ha : (box_h > 0 ? box_h : 260);
        if (fw < 80) fw = 320; if (fh < 48) fh = 200;
        lv_obj_t *fr = lv_obj_create(target_parent);
        lv_obj_remove_style_all(fr);
        lv_obj_set_size(fr, fw, fh);
        apply_box(fr, &s, fw, fh);
        lv_obj_set_style_bg_color(fr, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(fr, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(fr, 1, 0);
        lv_obj_set_style_border_color(fr, lv_color_hex(0xCBD5E1), 0);
        lv_obj_set_style_radius(fr, 6, 0);
        lv_obj_set_flex_flow(fr, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(fr, 0, 0);
        lv_obj_set_style_pad_row(fr, 0, 0);
        lv_obj_remove_flag(fr, LV_OBJ_FLAG_SCROLLABLE);

        char title_a[96]; title_a[0] = 0;
        dn_attr(n, "title", title_a, sizeof(title_a));
        lv_obj_t *hdr = lv_obj_create(fr);
        lv_obj_remove_style_all(hdr);
        lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(hdr, LV_PCT(100), 24);
        lv_obj_set_style_bg_color(hdr, lv_color_hex(0xF1F5F9), 0);
        lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(hdr, 1, 0);
        lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(hdr, lv_color_hex(0xE2E8F0), 0);
        lv_obj_set_style_pad_hor(hdr, 8, 0);
        lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *hl = lv_label_create(hdr);
        lv_label_set_text(hl, title_a[0] ? title_a : (d->src[0] ? d->src : "iframe"));
        lv_label_set_long_mode(hl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(hl, 10); lv_obj_set_flex_grow(hl, 1);
        lv_obj_set_style_text_color(hl, lv_color_hex(0x475569), 0);
        { lv_font_t *hf = bfont(11); if (hf) lv_obj_set_style_text_font(hl, hf, 0); }

        lv_obj_t *body = lv_obj_create(fr);
        lv_obj_remove_style_all(body);
        lv_obj_set_size(body, LV_PCT(100), 10);
        lv_obj_set_flex_grow(body, 1);
        lv_obj_set_style_pad_all(body, 10, 0);
        lv_obj_set_style_pad_row(body, 6, 0);
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scroll_dir(body, LV_DIR_VER);
        lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLL_ELASTIC);

        char sd[512]; sd[0] = 0;
        const char *ih = 0;
        if (dn_attr(n, "srcdoc", sd, sizeof(sd)) && sd[0]) {
            ih = sd;
        } else if (d->src[0] && !starts_with(d->src, "about:") && !starts_with(d->src, "javascript:")) {
            char abs[440]; resolve_url(g_cur_scheme, g_cur_host, d->src, abs, sizeof(abs));
            int ri = abs[0] ? res_find(abs) : -1;
            if (ri >= 0 && g_res[ri].data && g_res[ri].len > 0) ih = (const char *)g_res[ri].data;
            else if (abs[0] && g_basi && g_basi->net && !g_nav_in_flight) {
                uint32_t fl = 0;
                const char *fb = starts_with(abs, "https://")
                    ? (g_basi->net->https_get ? g_basi->net->https_get(abs, &fl) : 0)
                    : (g_basi->net->http_get ? g_basi->net->http_get(abs, &fl) : 0);
                if (fb && fl > 0) {
                    int nri = res_add(abs, fb, fl > 65536u ? 65536u : fl, 0);
                    if (nri >= 0 && g_res[nri].data) ih = (const char *)g_res[nri].data;
                }
            }
        }
        if (ih && ih[0]) {
            /* Izole HTML metin/baslik ayristirmasi (ebeveyn DOM'a dokunmadan) */
            const char *p = ih;
            int lines = 0;
            while (*p && lines < 32) {
                if (*p == '<') {
                    if (strn_ci_eq(p, "<script", 7)) {
                        const char *e = find_substr_ci(p, "</script>");
                        p = e ? e + 9 : p + 7; continue;
                    }
                    if (strn_ci_eq(p, "<style", 6)) {
                        const char *e = find_substr_ci(p, "</style>");
                        p = e ? e + 8 : p + 6; continue;
                    }
                    int is_h = (p[1] == 'h' || p[1] == 'H') && p[2] >= '1' && p[2] <= '3';
                    while (*p && *p != '>') p++;
                    if (*p == '>') p++;
                    if (is_h) {
                        char tb[160]; int ti = 0;
                        while (*p && *p != '<' && ti < 158) tb[ti++] = *p++;
                        tb[ti] = 0; css_trim(tb);
                        if (tb[0]) {
                            lv_obj_t *l = lv_label_create(body);
                            lv_label_set_text(l, tb);
                            lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
                            lv_obj_set_width(l, LV_PCT(100));
                            lv_obj_set_style_text_color(l, lv_color_hex(0x0F172A), 0);
                            { lv_font_t *f = bfont_ex(16, 1); if (f) lv_obj_set_style_text_font(l, f, 0); }
                            lines++;
                        }
                    }
                    continue;
                }
                char tb[220]; int ti = 0;
                while (*p && *p != '<' && ti < 218) {
                    char c = *p++;
                    if (c == '\r' || c == '\n' || c == '\t') c = ' ';
                    if (c == ' ' && (ti == 0 || tb[ti - 1] == ' ')) continue;
                    tb[ti++] = c;
                }
                while (ti > 0 && tb[ti - 1] == ' ') ti--;
                tb[ti] = 0;
                if (tb[0]) {
                    lv_obj_t *l = lv_label_create(body);
                    lv_label_set_text(l, tb);
                    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
                    lv_obj_set_width(l, LV_PCT(100));
                    lv_obj_set_style_text_color(l, lv_color_hex(0x334155), 0);
                    { lv_font_t *f = bfont(13); if (f) lv_obj_set_style_text_font(l, f, 0); }
                    lines++;
                }
            }
        }
        d->widget = fr;
        css_var_pop(depth); g_cur_avail_w = save_avail; g_cur_cont_w = save_cont;
        return;
    }

    /* <img> */
    if (str_eq(d->tag, "img")) {
        lv_obj_t *o = lv_obj_create(target_parent);
        lv_obj_remove_style_all(o);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
        apply_box(o, &s, box_w, box_h);
        lv_obj_t *c = make_image(o, d->src, d->wa, d->wa_pct, d->ha, d->ha_pct, s.object_fit);
        if (c) lv_obj_center(c);
        else if (d->alt[0]) text_label(o, d->alt, &s, ps, 0, 0);
        d->widget = o;
        css_var_pop(depth); g_cur_avail_w = save_avail; g_cur_cont_w = save_cont;
        return;
    }



    /* HTML5 <canvas> (gercek ARGB8888 2D yazilim rasterizer yuzeyi) */
    if (str_eq(d->tag, "canvas")) {
        int cw = d->wa > 0 && !d->wa_pct ? d->wa : (box_w > 0 ? box_w : 300);
        int ch = d->ha > 0 && !d->ha_pct ? d->ha : (box_h > 0 ? box_h : 150);
        if (cw < 1) cw = 300; if (ch < 1) ch = 150;
        if (cw > 1024) cw = 1024; if (ch > 1024) ch = 1024;
        lv_obj_t *o = lv_obj_create(target_parent);
        lv_obj_remove_style_all(o);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(o, cw, ch);
        apply_box(o, &s, cw, ch);
        bcanvas_slot_t *sl = bcanvas_get(n, cw, ch);
        if (sl && sl->bc.px) {
            lv_obj_t *c = lv_canvas_create(o);
            lv_obj_remove_style_all(c);
            lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
            lv_canvas_set_buffer(c, sl->bc.px, sl->bc.w, sl->bc.h, LV_COLOR_FORMAT_ARGB8888);
            lv_obj_center(c);
            sl->lv_c = c;
        }
        d->widget = o;
        css_var_pop(depth); g_cur_avail_w = save_avail; g_cur_cont_w = save_cont;
        return;
    }



    /* <input type="file"> (gercek Arctian dosya secici) */
    if (str_eq(d->tag, "input") && str_eq(d->type, "file")) {
        lv_obj_t *row = lv_obj_create(target_parent);
        lv_obj_remove_style_all(row);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(row, LV_SIZE_CONTENT, 32);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);

        lv_obj_t *btn = lv_button_create(row);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, LV_SIZE_CONTENT, 28);
        lv_obj_set_style_pad_hor(btn, 12, 0);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2563EB), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(btn, page_file_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)n);
        lv_obj_t *bl = lv_label_create(btn);
        lv_label_set_text(bl, "Dosya Sec...");
        lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), 0);
        { lv_font_t *ff = bfont(12); if (ff) lv_obj_set_style_text_font(bl, ff, 0); }
        lv_obj_center(bl);

        lv_obj_t *fl = lv_label_create(row);
        lv_label_set_text(fl, d->value[0] ? d->value : "Dosya secilmedi");
        lv_obj_set_style_text_color(fl, lv_color_hex(s.has_color ? s.color : 0x64748B), 0);
        { lv_font_t *ff = bfont(12); if (ff) lv_obj_set_style_text_font(fl, ff, 0); }

        d->widget = row;
        css_var_pop(depth); g_cur_avail_w = save_avail; g_cur_cont_w = save_cont;
        return;
    }

    /* <input type="checkbox"> (visible) */
    if (str_eq(d->tag, "input") && str_eq(d->type, "checkbox")) {
        int cw = box_w > 0 ? box_w : 20, ch = box_h > 0 ? box_h : 20;
        lv_obj_t *cb = lv_obj_create(target_parent);
        lv_obj_remove_style_all(cb);
        lv_obj_remove_flag(cb, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(cb, cw, ch);
        lv_obj_set_style_radius(cb, 4, 0);
        uint32_t acc = s.has_accent ? s.accent_c : 0x6366F1;
        lv_obj_set_style_bg_color(cb, lv_color_hex(d->checked ? acc : 0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(cb, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(cb, 1, 0);
        lv_obj_set_style_border_color(cb, lv_color_hex(d->checked ? acc : 0x94A3B8), 0);
        if (d->checked) {
            lv_obj_t *ck = lv_label_create(cb);
            lv_label_set_text(ck, LV_SYMBOL_OK);
            lv_obj_set_style_text_color(ck, lv_color_hex(0xFFFFFF), 0);
            lv_obj_center(ck);
        }
        lv_obj_add_flag(cb, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cb, page_checkbox_cb, LV_EVENT_CLICKED, (void *)(intptr_t)n);
        d->widget = cb;
        css_var_pop(depth); g_cur_avail_w = save_avail; g_cur_cont_w = save_cont;
        return;
    }

    /* <input type="text|email|search|..."> and <textarea> */
    if ((str_eq(d->tag, "input") &&
        (d->type[0] == 0 || str_eq(d->type, "text") || str_eq(d->type, "search") ||
         str_eq(d->type, "email") || str_eq(d->type, "url") || str_eq(d->type, "tel") ||
         str_eq(d->type, "password") || str_eq(d->type, "number"))) ||
        str_eq(d->tag, "textarea")) {
        int is_ta = str_eq(d->tag, "textarea");
        int one_line = (!is_ta || d->rows == 1) ? 1 : 0;
        lv_obj_t *ta = lv_textarea_create(target_parent);
        lv_obj_remove_style_all(ta);
        lv_textarea_set_one_line(ta, one_line);
        int ta_fg = (s.flex_grow > 0 && ps && ps->is_flex && (ps->row ? !g_in_shrink_w : (my_avail_h > 0)));
        int tw = box_w > 0 ? box_w : (ta_fg && ps->row ? 10 : (shrink_w ? (my_avail < 260 ? my_avail : 260) : my_avail));
        int th = box_h > 0 ? box_h : (one_line ? (s.font_size + s.pad[0] + s.pad[2] + 10) : ((d->rows > 0 ? d->rows : 4) * 24 + s.pad[0] + s.pad[2]));
        lv_obj_set_size(ta, tw, th);
        apply_box(ta, &s, tw, th);
        if (ta_fg) lv_obj_set_flex_grow(ta, s.flex_grow);
        if (!s.has_bg) { lv_obj_set_style_bg_color(ta, lv_color_hex(0x1F293D), 0); lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0); }
        if (s.border_w == 0) { lv_obj_set_style_border_width(ta, 1, 0); lv_obj_set_style_border_color(ta, lv_color_hex(0x374151), 0); }
        lv_font_t *tf = bfont_ex(s.font_size, 0);
        lv_obj_set_style_text_font(ta, tf, 0);
        lv_obj_set_style_text_color(ta, lv_color_hex(s.has_color ? s.color : 0xF3F4F6), 0);
        lv_obj_set_style_text_font(ta, tf, LV_PART_TEXTAREA_PLACEHOLDER);
        lv_obj_set_style_text_color(ta, lv_color_hex(0x6B7280), LV_PART_TEXTAREA_PLACEHOLDER);
        if (d->ph[0]) lv_textarea_set_placeholder_text(ta, d->ph);
        if (d->value[0]) lv_textarea_set_text(ta, d->value);
        d->widget = ta;
        d->is_ta = 1;
        if (g_group) {
            lv_group_add_obj(g_group, ta);
            lv_obj_add_flag(ta, LV_OBJ_FLAG_CLICK_FOCUSABLE);
            if (g_focus_node == n) lv_group_focus_obj(ta);
        }
        lv_obj_add_event_cb(ta, page_input_ready_cb, LV_EVENT_READY, (void *)(intptr_t)n);
        lv_obj_add_event_cb(ta, page_input_focus_cb, LV_EVENT_CLICKED, (void *)(intptr_t)n);
        lv_obj_add_event_cb(ta, page_input_focus_cb, LV_EVENT_FOCUSED, (void *)(intptr_t)n);
        lv_obj_add_event_cb(ta, page_input_blur_cb, LV_EVENT_DEFOCUSED, (void *)(intptr_t)n);
        lv_obj_add_event_cb(ta, page_input_changed_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)n);
        css_var_pop(depth); g_cur_avail_w = save_avail; g_cur_cont_w = save_cont;
        return;
    }

    if (str_eq(d->tag, "input") && (str_eq(d->type, "hidden") || str_eq(d->type, "radio"))) {
        css_var_pop(depth); g_cur_avail_w = save_avail; g_cur_cont_w = save_cont;
        return;
    }

    if (str_eq(d->tag, "hr")) {
        lv_obj_t *o = lv_obj_create(target_parent);
        lv_obj_remove_style_all(o);
        lv_obj_set_size(o, box_w > 0 ? box_w : LV_PCT(100), 1);
        lv_obj_set_style_bg_color(o, lv_color_hex(s.border_c), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        d->widget = o;
        css_var_pop(depth); g_cur_avail_w = save_avail; g_cur_cont_w = save_cont;
        return;
    }



    /* Regular container / box element */
    lv_obj_t *o = lv_obj_create(target_parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    d->widget = o;

    if (s.position == 4) g_sticky_obj = o;

    /* LVGL flex_grow MUST only apply when parent has definite main-axis size (otherwise LVGL collapses LV_SIZE_CONTENT to 0px!) */
    int use_fg = (s.flex_grow > 0 && ps && ps->is_flex && (ps->row ? !g_in_shrink_w : (my_avail_h > 0)));
    /* Set width & height (prefer LV_PCT(100) for width:100% inside flex_grow parents so live parent width is respected!) */
    int use_pct_w = (s.has_w && s.w_pct && s.width == 100 && !s.has_maxw && !s.has_minw && !in_flex_row && !g_in_shrink_w && !style_needs_custom_surface(&s));
    if (use_pct_w) lv_obj_set_width(o, LV_PCT(100));
    else if (box_w > 0) lv_obj_set_width(o, box_w);
    else if (use_fg && ps->row) lv_obj_set_width(o, 10);
    else if (s.has_w && s.w_pct && (!g_in_shrink_w || s.position >= 2)) lv_obj_set_width(o, LV_PCT(s.width));
    else if (s.position == 2 && s.has_left && s.has_right && g_in_shrink_w) lv_obj_set_width(o, LV_PCT(100));
    else lv_obj_set_width(o, shrink_w ? LV_SIZE_CONTENT : LV_PCT(100));

    if (box_h > 0) lv_obj_set_height(o, box_h);
    else if (s.has_h && s.h_pct) lv_obj_set_height(o, LV_PCT(s.height));
    else lv_obj_set_height(o, LV_SIZE_CONTENT);

    /* Center block horizontally if margin-inline: auto / margin: 0 auto (symmetric margins prevent double-shift in centered parents!) */
    if (s.mar_auto_l && s.mar_auto_r && box_w > 0 && box_w < my_avail) {
        int rem = my_avail - box_w;
        s.mar[3] = rem / 2;
        s.mar[1] = rem - s.mar[3];
        s.has_mar = 1;
    }

    apply_box(o, &s, box_w > 0 ? box_w : my_avail, box_h > 0 ? box_h : 60);
    if (use_fg) lv_obj_set_flex_grow(o, s.flex_grow);

    /* Interactive click handlers */
    if (!s.pointer_none) {
        if (str_eq(d->tag, "a") && d->href[0]) {
            lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(o, page_link_cb, LV_EVENT_CLICKED, (void *)(intptr_t)n);
        } else if (str_eq(d->tag, "label")) {
            lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(o, page_label_cb, LV_EVENT_CLICKED, (void *)(intptr_t)n);
        } else if (str_eq(d->tag, "summary")) {
            lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(o, page_summary_cb, LV_EVENT_CLICKED, (void *)(intptr_t)n);
        } else if (str_eq(d->tag, "button") ||
                   (str_eq(d->tag, "input") && (str_eq(d->type, "submit") || str_eq(d->type, "button") || str_eq(d->type, "reset")))) {
            lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(o, page_submit_cb, LV_EVENT_CLICKED, (void *)(intptr_t)n);
        } else if (js_node_has_click_handler(n)) {
            lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(o, page_js_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)n);
        }
    }

    int inner_w = (box_w > 0 ? box_w : my_avail) - s.pad[1] - s.pad[3] - s.border_w * 2;
    if (inner_w < 20) inner_w = 20;
    int inner_h = (box_h > 0) ? (box_h - s.pad[0] - s.pad[2] - s.border_w * 2) : -1;
    if (inner_h >= 0 && inner_h < 8) inner_h = 8;

    /* Special Case A: Multi-column text layout (column-count > 1, e.g. .multi-column-text) */
    if (s.col_count >= 2 && d->first >= 0) {
        int col_gap = s.col_gap > 0 ? s.col_gap : 32;
        int col_w = (inner_w - col_gap * (s.col_count - 1)) / s.col_count;
        lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(o, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_column(o, 0, 0);

        /* Collect child paragraphs */
        int pnodes[16]; int pn = 0;
        for (int c = d->first; c >= 0 && pn < 16; c = g_dn[c].next)
            if (!g_dn[c].is_text) pnodes[pn++] = c;

        int per = (pn + s.col_count - 1) / s.col_count;
        lv_obj_t *rule_obj = 0;
        int max_col_h = 120;
        for (int col = 0; col < s.col_count; col++) {
            if (col > 0 && s.col_rule_w > 0) {
                rule_obj = lv_obj_create(o);
                lv_obj_remove_style_all(rule_obj);
                lv_obj_remove_flag(rule_obj, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_set_size(rule_obj, s.col_rule_w < 2 ? 2 : s.col_rule_w, 100);
            }
            lv_obj_t *col_box = lv_obj_create(o);
            lv_obj_remove_style_all(col_box);
            lv_obj_remove_flag(col_box, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_size(col_box, col_w, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(col_box, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(col_box, 12, 0);
            int st = col * per, en = st + per;
            if (en > pn) en = pn;
            for (int k = st; k < en; k++) build_node(pnodes[k], col_box, &s, col_w, -1, depth + 1);
            lv_obj_update_layout(col_box);
            int ch = (int)lv_obj_get_height(col_box);
            if (ch > max_col_h) max_col_h = ch;
        }
        if (rule_obj && s.col_rule_w > 0) {
            int rw = s.col_rule_w < 2 ? 2 : s.col_rule_w;
            lv_obj_set_size(rule_obj, rw, max_col_h);
            uint32_t *rpx = img_alloc_extra(rw, max_col_h);
            if (rpx) {
                uint32_t rc = ((uint32_t)(s.col_rule_a > 20 ? s.col_rule_a : 48) << 24) | s.col_rule_c;
                for (int y = 0; y < max_col_h; y++) {
                    int on = (s.col_rule_style == 2) ? ((y % 12) < 6) : 1;
                    for (int x = 0; x < rw; x++) rpx[y * rw + x] = on ? rc : 0;
                }
                lv_image_dsc_t *rdsc = img_make_dsc(rpx, rw, max_col_h);
                if (rdsc) { lv_obj_set_style_bg_image_src(rule_obj, rdsc, 0); lv_obj_set_style_bg_image_opa(rule_obj, LV_OPA_COVER, 0); }
            }
        }
        apply_position(o, &s, my_avail, my_avail_h > 0 ? my_avail_h : g_view_h);
        css_var_pop(depth); g_cur_avail_w = save_avail; g_cur_cont_w = save_cont;
        return;
    }

    /* Special Case B: Rich mixed-inline block (e.g. <h1>SÄ±fÄ±rdan <span class="gradient-text">...</span>...</h1>,
     * <p class="lead-text">, <span>KaanDemir<span>.css</span></span>, <footer><p><strong>...</strong>...</p>) */
    int has_elem_child = 0;
    for (int c = d->first; c >= 0; c = g_dn[c].next) if (!g_dn[c].is_text) { has_elem_child = 1; break; }
    int has_fl_rule = class_has(d->cls, "lead-text", 9);

    if ((has_elem_child || has_fl_rule) && !s.is_flex && !s.is_grid && is_inline_text_only_subtree(n)) {
        lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START,
            s.align == 1 ? LV_FLEX_ALIGN_CENTER : (s.align == 2 ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START),
            LV_FLEX_ALIGN_START);
        build_pseudo_elem(o, n, &s, 1, box_w > 0 ? box_w : inner_w, box_h > 0 ? box_h : 40, depth);
        render_rich_inline_block(o, n, &s, inner_w, depth, is_inline_el);
        build_pseudo_elem(o, n, &s, 2, box_w > 0 ? box_w : inner_w, box_h > 0 ? box_h : 40, depth);
        apply_position(o, &s, my_avail, my_avail_h > 0 ? my_avail_h : (s.position == 2 ? (int)lv_obj_get_height(target_parent) : g_view_h));
        if (style_needs_custom_surface(&s)) {
            lv_obj_update_layout(o);
            int rw = box_w > 0 ? box_w : (int)lv_obj_get_width(o);
            int rh = box_h > 0 ? box_h : (int)lv_obj_get_height(o);
            if (rw > 1 && rh > 1) paint_css_surface(o, rw, rh, &s);
        }
        css_var_pop(depth); g_cur_avail_w = save_avail; g_cur_cont_w = save_cont;
        return;
    }

    /* Special Case C: Leaf element with only text (no element children) */
    if (!has_elem_child) {
        if (s.is_flex || s.is_grid) {
            lv_obj_set_flex_flow(o, (s.is_flex && s.row) ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
            lv_flex_align_t ma = s.justify==1?LV_FLEX_ALIGN_CENTER:s.justify==2?LV_FLEX_ALIGN_END:s.justify==3?LV_FLEX_ALIGN_SPACE_BETWEEN:LV_FLEX_ALIGN_START;
            lv_flex_align_t ca = s.align_items==1?LV_FLEX_ALIGN_CENTER:s.align_items==2?LV_FLEX_ALIGN_END:LV_FLEX_ALIGN_START;
            lv_obj_set_flex_align(o, ma, ca, ca);
        } else {
            lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
            lv_flex_align_t ca = s.align==1?LV_FLEX_ALIGN_CENTER:s.align==2?LV_FLEX_ALIGN_END:LV_FLEX_ALIGN_START;
            int is_btn_el = str_eq(d->tag, "button") ||
                            (str_eq(d->tag, "input") && (str_eq(d->type, "submit") || str_eq(d->type, "button") || str_eq(d->type, "reset")));
            lv_obj_set_flex_align(o, is_btn_el ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START, ca, ca);
        }
        build_pseudo_elem(o, n, &s, 1, box_w > 0 ? box_w : inner_w, box_h > 0 ? box_h : 40, depth);
        const char *leaf_txt = d->text;
        if (!leaf_txt[0] && str_eq(d->tag, "input") &&
            (str_eq(d->type, "submit") || str_eq(d->type, "button") || str_eq(d->type, "reset"))) {
            leaf_txt = d->value[0] ? d->value : (str_eq(d->type, "reset") ? "Reset" : "Submit");
        }
        if (leaf_txt[0]) {
            /* Inside a space-between flex row (e.g. .test-card-title), allow non-nowrap text to wrap instead of colliding with badge */
            int row_wrap = (ps && ps->is_flex && ps->row && ps->justify == 3 && !s.nowrap && !s.has_bg && !s.has_w && d->next >= 0);
            int wrap = row_wrap || !(is_inline_el || s.nowrap || shrink_w || (ps && ps->is_flex && ps->row && !s.has_w));
            if (row_wrap) {
                lv_obj_set_flex_grow(o, 1);
                lv_obj_set_width(o, 10);
                lv_obj_set_style_pad_right(o, 6, 0);
                text_label(o, leaf_txt, &s, ps, 0, 1);
            } else {
                text_label(o, leaf_txt, &s, ps, inner_w, wrap);
            }
        }
        build_pseudo_elem(o, n, &s, 2, box_w > 0 ? box_w : inner_w, box_h > 0 ? box_h : 40, depth);
        apply_position(o, &s, my_avail, my_avail_h > 0 ? my_avail_h : (s.position == 2 ? (int)lv_obj_get_height(target_parent) : g_view_h));
        if (style_needs_custom_surface(&s)) {
            lv_obj_update_layout(o);
            int rw = box_w > 0 ? box_w : (int)lv_obj_get_width(o);
            int rh = box_h > 0 ? box_h : (int)lv_obj_get_height(o);
            if (rw > 1 && rh > 1) paint_css_surface(o, rw, rh, &s);
        }
        register_anim(o, n, &s, 0);
        css_var_pop(depth); g_cur_avail_w = save_avail; g_cur_cont_w = save_cont;
        return;
    }

    /* Special Case D: Real CSS Grid container with tracks or auto-fit */
    int use_grid = s.is_grid && (s.grid_cols > 0 || s.grid_auto_fit) && g_grid_slot < GRID_SLOTS;
    if (use_grid) {
        int cgap = s.col_gap > 0 ? s.col_gap : s.gap;
        int rgap = s.row_gap > 0 ? s.row_gap : s.gap;
        int ncols = s.grid_cols;
        if (s.grid_auto_fit) {
            int mw = s.grid_min_w > 60 ? s.grid_min_w : 280;
            ncols = (inner_w + cgap) / (mw + cgap);
            if (ncols < 1) ncols = 1;
            if (ncols > 6) ncols = 6;
        }
        if (ncols < 1) ncols = 1;
        if (ncols > 16) ncols = 16;

        /* Compute exact pixel width of each grid track */
        int track_px[16];
        int fixed_sum = 0, fr_sum = 0;
        for (int i = 0; i < ncols; i++) {
            if (!s.grid_auto_fit && i < s.gtrack_n && s.gtracks[i].type == 1)
                fixed_sum += s.gtracks[i].val;
            else
                fr_sum += (!s.grid_auto_fit && i < s.gtrack_n && s.gtracks[i].val > 0) ? s.gtracks[i].val : 1;
        }
        int rem_px = inner_w - fixed_sum - cgap * (ncols - 1);
        if (rem_px < 40) rem_px = 40;
        int32_t *cd = g_grid_col[g_grid_slot];
        int32_t *rd = g_grid_row[g_grid_slot];
        for (int i = 0; i < ncols; i++) {
            if (!s.grid_auto_fit && i < s.gtrack_n && s.gtracks[i].type == 1) {
                track_px[i] = s.gtracks[i].val;
                cd[i] = track_px[i];
            } else {
                int fv = (!s.grid_auto_fit && i < s.gtrack_n && s.gtracks[i].val > 0) ? s.gtracks[i].val : 1;
                track_px[i] = rem_px * fv / (fr_sum > 0 ? fr_sum : 1);
                cd[i] = LV_GRID_FR(fv);
            }
        }
        cd[ncols] = LV_GRID_TEMPLATE_LAST;
        int my_slot = g_grid_slot++;

        /* Build visible children and measure exact row heights so stretch never explodes! */
        lv_obj_t *vis_ch[64]; int vis_dn[64]; int vis_span[64]; int vis_rspan[64]; int vis_cw[64]; int vis_row[64]; int vis_col[64]; int nvis = 0;
        int row_max_h[GRID_ROWS];
        for (int i = 0; i < GRID_ROWS; i++) row_max_h[i] = 0;
        int col = 0, row = 0;
        for (int c = d->first; c >= 0 && nvis < 64; c = g_dn[c].next) {
            if (g_dn[c].is_text) continue;
            css_style_t cs; compute_style(c, &s, &cs, 0, depth + 1); css_var_pop(depth + 1);
            if (cs.hide || cs.sr_clip) continue;
            int sp = cs.has_col_span ? cs.col_span : 1;
            if (sp > ncols) sp = ncols;
            if (col + sp > ncols) { col = 0; row++; }
            int cell_w = cgap * (sp - 1);
            for (int k = 0; k < sp && col + k < ncols; k++) cell_w += track_px[col + k];
            int sv_sh = g_in_shrink_w; g_in_shrink_w = 0;
            build_node(c, o, &s, cell_w, -1, depth + 1);
            g_in_shrink_w = sv_sh;
            if (g_dn[c].widget) {
                lv_obj_update_layout(g_dn[c].widget);
                int ch_h = (int)lv_obj_get_height(g_dn[c].widget);
                if (row < GRID_ROWS && ch_h > row_max_h[row]) row_max_h[row] = ch_h;
                vis_ch[nvis] = g_dn[c].widget;
                vis_dn[nvis] = c;
                vis_span[nvis] = sp;
                vis_rspan[nvis] = cs.has_row_span ? cs.row_span : 1;
                vis_cw[nvis] = cell_w;
                vis_row[nvis] = row;
                vis_col[nvis] = col;
                nvis++;
            }
            col += sp;
        }
        int nrows = row + 1;
        if (nrows < 1) nrows = 1;
        if (nrows > GRID_ROWS - 1) nrows = GRID_ROWS - 1;
        int align_start = (s.align_items == 0 && !s.grid_auto_fit && ncols == 2);
        for (int i = 0; i < nrows; i++) rd[i] = (row_max_h[i] > 10 && !align_start) ? row_max_h[i] : LV_GRID_CONTENT;
        rd[nrows] = LV_GRID_TEMPLATE_LAST;
        lv_obj_set_grid_dsc_array(o, cd, rd);
        lv_obj_set_style_pad_column(o, cgap, 0);
        lv_obj_set_style_pad_row(o, rgap, 0);

        for (int i = 0; i < nvis; i++) {
            int r = vis_row[i];
            if (r >= nrows) break;
            lv_grid_align_t va = align_start ? LV_GRID_ALIGN_START : LV_GRID_ALIGN_STRETCH;
            if (!align_start && row_max_h[r] > 10) {
                int old_h = (int)lv_obj_get_height(vis_ch[i]);
                if (old_h < row_max_h[r]) {
                    lv_obj_set_height(vis_ch[i], row_max_h[r]);
                    css_style_t cs; compute_style(vis_dn[i], &s, &cs, 0, depth + 1); css_var_pop(depth + 1);
                    if (style_needs_custom_surface(&cs))
                        paint_css_surface(vis_ch[i], vis_cw[i], row_max_h[r], &cs);
                }
            }
            lv_obj_set_grid_cell(vis_ch[i], LV_GRID_ALIGN_STRETCH, vis_col[i], vis_span[i], va, r, vis_rspan[i]);
        }
        apply_position(o, &s, my_avail, my_avail_h > 0 ? my_avail_h : (s.position == 2 ? (int)lv_obj_get_height(target_parent) : g_view_h));
        if (style_needs_custom_surface(&s)) {
            lv_obj_update_layout(o);
            int rw = box_w > 0 ? box_w : (int)lv_obj_get_width(o);
            int rh = box_h > 0 ? box_h : (int)lv_obj_get_height(o);
            if (rw > 1 && rh > 1) paint_css_surface(o, rw, rh, &s);
        }
        css_var_pop(depth); g_cur_avail_w = save_avail; g_cur_cont_w = save_cont;
        (void)my_slot;
        return;
    }

    /* Regular Flex / Grid-Place-Center / Block Container */
    if (s.is_flex || s.is_grid) {
        int is_row = s.is_flex ? s.row : 0;
        lv_obj_set_flex_flow(o, is_row ? (s.wrap ? LV_FLEX_FLOW_ROW_WRAP : LV_FLEX_FLOW_ROW) : LV_FLEX_FLOW_COLUMN);
        int mv = (s.is_grid && !s.is_flex) ? (s.align_items ? s.align_items : s.justify) : s.justify;
        int cv = (s.is_grid && !s.is_flex) ? (s.justify_items ? s.justify_items : s.align_items) : s.align_items;
        lv_flex_align_t ma = mv==1?LV_FLEX_ALIGN_CENTER:mv==2?LV_FLEX_ALIGN_END:
                             mv==3?LV_FLEX_ALIGN_SPACE_BETWEEN:mv==4?LV_FLEX_ALIGN_SPACE_AROUND:LV_FLEX_ALIGN_START;
        lv_flex_align_t ca = (cv==1 || (cv==3 && is_row && box_h > 0 && box_h <= 80))?LV_FLEX_ALIGN_CENTER:cv==2?LV_FLEX_ALIGN_END:LV_FLEX_ALIGN_START;
        lv_obj_set_flex_align(o, ma, ca, ca);
        int cg = s.col_gap > 0 ? s.col_gap : s.gap;
        int rg = s.row_gap > 0 ? s.row_gap : s.gap;
        lv_obj_set_style_pad_column(o, cg, 0);
        lv_obj_set_style_pad_row(o, rg, 0);
    } else {
        /* Inline-only block container (e.g. <center><input type="submit"><input type="submit"></center>): lay out inline children in a centered/aligned horizontal row */
        int inl_cnt = 0, blk_cnt = 0;
        for (int c = d->first; c >= 0; c = g_dn[c].next) {
            if (g_dn[c].is_text) continue;
            css_style_t cs; compute_style(c, &s, &cs, 0, depth + 1); css_var_pop(depth + 1);
            if (cs.hide || cs.vis_hidden || cs.sr_clip) continue;
            if (cs.display == 1 || cs.display == 2 || cs.display == 4 || cs.display == 6) inl_cnt++;
            else blk_cnt++;
        }
        if (inl_cnt >= 2 && blk_cnt == 0) {
            lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW_WRAP);
            lv_flex_align_t ma = s.align==1?LV_FLEX_ALIGN_CENTER:s.align==2?LV_FLEX_ALIGN_END:LV_FLEX_ALIGN_START;
            lv_obj_set_flex_align(o, ma, LV_FLEX_ALIGN_CENTER, ma);
            lv_obj_set_style_pad_column(o, s.col_gap > 0 ? s.col_gap : 8, 0);
            lv_obj_set_style_pad_row(o, s.row_gap > 0 ? s.row_gap : 6, 0);
        } else {
            lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
            lv_flex_align_t ca = s.align==1?LV_FLEX_ALIGN_CENTER:s.align==2?LV_FLEX_ALIGN_END:LV_FLEX_ALIGN_START;
            lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, ca, ca);
            lv_obj_set_style_pad_row(o, s.row_gap > 0 ? s.row_gap : s.gap, 0);
        }
    }

    /* ::before pseudo-element */
    build_pseudo_elem(o, n, &s, 1, box_w > 0 ? box_w : inner_w, box_h > 0 ? box_h : 60, depth);

    /* Children (<details> hides non-<summary> children when !is_open!) */
    int is_details = str_eq(d->tag, "details");
    int sv_sh = g_in_shrink_w;
    g_in_shrink_w = (box_w < 0 && s.flex_grow == 0 && !(s.has_w && s.w_pct)) ? 1 : 0;
    for (int c = d->first; c >= 0; c = g_dn[c].next) {
        if (is_details && !d->is_open && !str_eq(g_dn[c].tag, "summary")) continue;
        build_node(c, o, &s, inner_w, inner_h, depth + 1);
    }
    g_in_shrink_w = sv_sh;

    /* ::after pseudo-element */
    build_pseudo_elem(o, n, &s, 2, box_w > 0 ? box_w : inner_w, box_h > 0 ? box_h : 60, depth);

    /* Position & Z-index */
    apply_position(o, &s, my_avail, g_view_h);

    /* Paint custom CSS3 surface (gradients, radial, conic, filter, clip-path, elliptical radius, dashed border) */
    if (style_needs_custom_surface(&s)) {
        lv_obj_update_layout(o);
        int rw = box_w > 0 ? box_w : (int)lv_obj_get_width(o);
        int rh = box_h > 0 ? box_h : (int)lv_obj_get_height(o);
        if (rw > 1 && rh > 1) paint_css_surface(o, rw, rh, &s);
    }

    /* Register live @keyframes animation */
    if (s.anim_name[0]) {
        int param = 0;
        if (find_substr_ci(s.anim_name, "marquee")) {
            lv_obj_update_layout(o);
            param = (int)lv_obj_get_width(o) / 2;
            if (param < 200) param = 600;
        }
        register_anim(o, n, &s, param);
    }

    /* Soft horizontal edge fade for mask-image: linear-gradient(90deg, transparent, #000 15%, #000 85%, transparent) */
    if (s.has_mask_fade && ps && ps->has_bg) {
        lv_obj_update_layout(o);
        int mh = (int)lv_obj_get_height(o);
        if (mh < 24) mh = 32;
        int fw = 84;
        uint32_t bgc = ps->bg;
        for (int side = 0; side < 2; side++) {
            uint32_t *fpx = img_alloc_extra(fw, mh);
            if (!fpx) break;
            for (int x = 0; x < fw; x++) {
                float f = side == 0 ? (1.0f - (float)x / (float)(fw - 1)) : ((float)x / (float)(fw - 1));
                uint32_t a = (uint32_t)(f * f * 245.0f);
                uint32_t col = (a << 24) | bgc;
                for (int y = 0; y < mh; y++) fpx[y * fw + x] = col;
            }
            lv_obj_t *ov = lv_canvas_create(o);
            lv_obj_remove_style_all(ov);
            lv_obj_remove_flag(ov, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(ov, LV_OBJ_FLAG_IGNORE_LAYOUT);
            lv_canvas_set_buffer(ov, fpx, fw, mh, LV_COLOR_FORMAT_ARGB8888);
            lv_obj_align(ov, side == 0 ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, 0, 0);
            lv_obj_move_foreground(ov);
        }
    }

    css_var_pop(depth);
    g_cur_avail_w = save_avail;
    g_cur_cont_w = save_cont;
}

static int css_pos_inert(const char *html, const char *pos) {
    const char *opens[2] = { "<noscript", "<template" };
    const char *closes[2] = { "</noscript", "</template" };
    for (int t = 0; t < 2; t++) {
        const char *ns = html;
        for (;;) {
            const char *a = find_substr_ci(ns, opens[t]);
            if (!a || a >= pos) break;
            const char *b = find_substr_ci(a, closes[t]);
            if (!b || b > pos) return 1;
            ns = b + 1;
        }
    }
    return 0;
}

static void collect_css_from_html(const char *html) {
    g_css_count = 0;
    g_css_pool_len = 0;
    g_kf_count = 0;
    g_css_idx_built = 0;
    const char *p = html;
    while (*p) {
        const char *lt = find_substr_ci(p, "<style");
        const char *lk = find_substr_ci(p, "<link");
        const char *next = (lt && (!lk || lt < lk)) ? lt : lk;
        if (!next) break;
        if (css_pos_inert(html, next)) { p = next + 1; continue; }
        const char *q = next; while (*q && *q != '>') q++;
        if (next == lt) {
            const char *cs = (*q == '>') ? q + 1 : q;
            const char *ce = find_substr_ci(cs, "</style");
            if (!ce) break;
            int len = (int)(ce - cs);
            if (len > 0 && g_basi && g_basi->mem) {
                char *buf = g_basi->mem->alloc((size_t)len + 1);
                if (buf) {
                    for (int i = 0; i < len; i++) buf[i] = cs[i];
                    buf[len] = 0;
                    css_parse(buf);
                    g_basi->mem->free(buf);
                }
            }
            const char *end = ce; while (*end && *end != '>') end++;
            p = (*end == '>') ? end + 1 : end;
        } else {
            char href[420], rel[64];
            if (find_attr(next, q, "href", href, sizeof(href)) &&
                find_attr(next, q, "rel", rel, sizeof(rel)) && find_substr_ci(rel, "stylesheet")) {
                char abs[440]; resolve_url(g_cur_scheme, g_cur_host, href, abs, sizeof(abs));
                int ri = res_find(abs);
                if (ri >= 0 && g_res[ri].is_css == 1 && g_res[ri].data) css_parse((const char *)g_res[ri].data);
            }
            p = (*q == '>') ? q + 1 : q;
        }
    }
}

/* Rebuilds visual LVGL widgets from current DOM + interactive state (:checked, :target, :hover, <details open>) */
static void restyle_page(void) {
    if (!g_content || g_dn_count < 1) return;
    int32_t saved_sy = lv_obj_get_scroll_y(g_content);
    /* Sync text from live form inputs before clean */
    for (int i = 0; i < g_dn_count; i++) {
        if (g_dn[i].widget && g_dn[i].is_ta) {
            const char *v = lv_textarea_get_text(g_dn[i].widget);
            if (v) bcopy_str(g_dn[i].value, v, sizeof(g_dn[i].value));
        }
        g_dn[i].widget = 0;
        g_dn[i].is_ta = 0;
    }
    lv_obj_clean(g_content);
    if (g_fixed_layer) lv_obj_clean(g_fixed_layer);
    images_clear();
    g_vstk_n = 0;
    g_vcount = 0;
    g_grid_slot = 0;
    g_root_font = 16;

    int vw = (int)lv_obj_get_width(g_content);
    int vh = (int)lv_obj_get_height(g_content);
    g_view_w = (vw > 40) ? vw : 1280;
    g_view_h = (vh > 40) ? vh : 800;

    if (g_group && g_addr) {
        lv_group_remove_all_objs(g_group);
        lv_group_add_obj(g_group, g_addr);
        if (s_find_ta) lv_group_add_obj(g_group, s_find_ta);
    }

    css_style_t base;
    style_defaults(&base);
    build_node(0, g_content, &base, g_view_w, g_view_h, 0);
    if (saved_sy > 0) lv_obj_scroll_to_y(g_content, saved_sy, LV_ANIM_OFF);
}

/* Agdan gelen gercek sayfayi CSS motoruyla cizer. */
/* ==========================================================================
 * ArctianJS - Sifirdan, Freestanding Modern JavaScript Motoru (ES6 - ES2024+)
 * + Tam Tarayici DOM / BOM / Events / Timers / Fetch Koprusu
 * Bagimlilik: SIFIR libc, SIFIR libm, SIFIR QuickJS, SIFIR Wolf Engine.
 * ========================================================================== */

/* --- JS Bellek Yoneticisi (sayfa degisince %100 temizlenir) --- */
typedef struct js_alloc_hdr {
    struct js_alloc_hdr *prev, *next;
    size_t size;
} js_alloc_hdr_t;

static js_alloc_hdr_t *g_js_alloc_head;
static size_t g_js_alloc_bytes;
#define JS_MEM_LIMIT (24u * 1024u * 1024u)

static void *js_malloc(size_t sz) {
    if (!g_basi || !g_basi->mem || !g_basi->mem->alloc) return 0;
    if (sz == 0) sz = 1;
    if (g_js_alloc_bytes + sz > g_js_mem_limit) return 0;
    js_alloc_hdr_t *h = (js_alloc_hdr_t *)g_basi->mem->alloc(sizeof(js_alloc_hdr_t) + sz);
    if (!h) return 0;
    h->prev = 0;
    h->next = g_js_alloc_head;
    h->size = sz;
    if (g_js_alloc_head) g_js_alloc_head->prev = h;
    g_js_alloc_head = h;
    g_js_alloc_bytes += sz;
    return (void *)(h + 1);
}

static void *js_calloc(size_t n, size_t sz) {
    size_t tot = n * sz;
    void *p = js_malloc(tot);
    if (!p) return 0;
    uint8_t *b = (uint8_t *)p;
    for (size_t i = 0; i < tot; i++) b[i] = 0;
    return p;
}

static void js_free(void *p) {
    if (!p || !g_basi || !g_basi->mem || !g_basi->mem->free) return;
    js_alloc_hdr_t *h = ((js_alloc_hdr_t *)p) - 1;
    if (h->prev) h->prev->next = h->next;
    else g_js_alloc_head = h->next;
    if (h->next) h->next->prev = h->prev;
    if (g_js_alloc_bytes >= h->size) g_js_alloc_bytes -= h->size;
    else g_js_alloc_bytes = 0;
    g_basi->mem->free(h);
}

static void *js_realloc(void *p, size_t nsz) {
    if (!p) return js_malloc(nsz);
    js_alloc_hdr_t *h = ((js_alloc_hdr_t *)p) - 1;
    if (nsz <= h->size) return p;
    void *np = js_malloc(nsz);
    if (!np) return 0;
    uint8_t *d = (uint8_t *)np, *s = (uint8_t *)p;
    for (size_t i = 0; i < h->size; i++) d[i] = s[i];
    js_free(p);
    return np;
}

static void js_free_all(void) {
    if (!g_basi || !g_basi->mem || !g_basi->mem->free) {
        g_js_alloc_head = 0; g_js_alloc_bytes = 0; return;
    }
    js_alloc_hdr_t *h = g_js_alloc_head;
    while (h) {
        js_alloc_hdr_t *nx = h->next;
        g_basi->mem->free(h);
        h = nx;
    }
    g_js_alloc_head = 0;
    g_js_alloc_bytes = 0;
}

/* --- Freestanding Yardimci Bellek/String/Matematik Fonksiyonlari --- */
static void js_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst; const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}
static void js_memset(void *dst, int v, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)v;
}
static int js_memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}
static size_t js_strlen(const char *s) {
    size_t n = 0; if (!s) return 0; while (s[n]) n++; return n;
}
static int js_strcmp(const char *a, const char *b) {
    if (!a) a = ""; if (!b) b = "";
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static int js_strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i]) return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}

/* IEEE-754 Freestanding Matematik */
static inline uint64_t js_d2u(double d) { union { double d; uint64_t u; } x; x.d = d; return x.u; }
static inline double js_u2d(uint64_t u) { union { double d; uint64_t u; } x; x.u = u; return x.d; }
static inline double js_nan(void) { return js_u2d(0x7FF8000000000000ULL); }
static inline double js_inf(void) { return js_u2d(0x7FF0000000000000ULL); }
static inline int js_isnan(double d) { return d != d; }
static inline int js_isinf(double d) { uint64_t u = js_d2u(d) & 0x7FFFFFFFFFFFFFFFULL; return u == 0x7FF0000000000000ULL; }
static inline int js_isfinite(double d) { return !js_isnan(d) && !js_isinf(d); }
static inline double js_fabs(double d) { return js_u2d(js_d2u(d) & 0x7FFFFFFFFFFFFFFFULL); }
static inline int js_signbit(double d) { return (int)(js_d2u(d) >> 63); }

static double js_trunc(double x) {
    if (!js_isfinite(x)) return x;
    if (js_fabs(x) >= 4503599627370496.0) return x;
    int64_t i = (int64_t)x;
    double r = (double)i;
    if (r == 0.0 && js_signbit(x)) return -0.0;
    return r;
}
static double js_floor(double x) {
    if (!js_isfinite(x)) return x;
    double t = js_trunc(x);
    return (x < t) ? (t - 1.0) : t;
}
static double js_ceil(double x) {
    if (!js_isfinite(x)) return x;
    double t = js_trunc(x);
    return (x > t) ? (t + 1.0) : t;
}
static double js_round_js(double x) {
    if (!js_isfinite(x) || x == 0.0) return x;
    if (x >= -0.5 && x < 0.0) return -0.0;
    return js_floor(x + 0.5);
}
static double js_fmod(double x, double y) {
    if (!js_isfinite(x) || js_isnan(y) || y == 0.0) return js_nan();
    if (js_isinf(y)) return x;
    double q = js_trunc(x / y);
    double r = x - q * y;
    if (r == 0.0 && js_signbit(x)) return -0.0;
    return r;
}
static double js_sqrt(double x) {
    if (js_isnan(x) || x < 0.0) return js_nan();
    if (x == 0.0 || js_isinf(x)) return x;
    double g = x > 1.0 ? x * 0.5 : 1.0;
    /* Better initial guess from exponent */
    uint64_t u = js_d2u(x);
    u = ((u - (1023ULL << 52)) >> 1) + (1023ULL << 52);
    g = js_u2d(u);
    for (int i = 0; i < 8; i++) g = 0.5 * (g + x / g);
    return g;
}
static double js_exp(double x) {
    if (js_isnan(x)) return js_nan();
    if (x > 709.78) return js_inf();
    if (x < -745.13) return 0.0;
    const double LN2 = 0.693147180559945309417;
    int k = (int)js_floor(x / LN2 + 0.5);
    double r = x - (double)k * LN2;
    double term = 1.0, sum = 1.0;
    for (int i = 1; i <= 22; i++) {
        term *= r / (double)i;
        sum += term;
    }
    /* Scale by 2^k */
    if (k >= -1022 && k <= 1023) {
        uint64_t scale = ((uint64_t)(k + 1023)) << 52;
        return sum * js_u2d(scale);
    }
    while (k > 0) { sum *= 2.0; k--; }
    while (k < 0) { sum *= 0.5; k++; }
    return sum;
}
static double js_log(double x) {
    if (js_isnan(x) || x < 0.0) return js_nan();
    if (x == 0.0) return -js_inf();
    if (js_isinf(x)) return js_inf();
    if (x == 1.0) return 0.0;
    const double LN2 = 0.693147180559945309417;
    int e = 0;
    double m = x;
    while (m >= 2.0) { m *= 0.5; e++; }
    while (m < 1.0) { m *= 2.0; e--; }
    /* Center around sqrt(2) for fast convergence */
    if (m > 1.4142135623730951) { m *= 0.5; e++; }
    double t = (m - 1.0) / (m + 1.0);
    double t2 = t * t;
    double term = t, sum = 0.0;
    for (int i = 1; i <= 29; i += 2) {
        sum += term / (double)i;
        term *= t2;
    }
    return 2.0 * sum + (double)e * LN2;
}
static double js_pow(double a, double b) {
    if (js_isnan(b)) return js_nan();
    if (b == 0.0) return 1.0;
    if (js_isnan(a)) return js_nan();
    if (a == 1.0) return js_isinf(b) ? js_nan() : 1.0;
    if (a == -1.0 && js_isinf(b)) return js_nan();
    if (a == 0.0) {
        int odd = (b == js_trunc(b) && js_fmod(b, 2.0) != 0.0);
        if (b > 0.0) return (js_signbit(a) && odd) ? -0.0 : 0.0;
        return (js_signbit(a) && odd) ? -js_inf() : js_inf();
    }
    if (b == js_trunc(b) && js_fabs(b) <= 1e9) {
        int64_t n = (int64_t)b;
        int neg = n < 0;
        uint64_t un = neg ? (uint64_t)(-n) : (uint64_t)n;
        double res = 1.0, base = a;
        while (un) {
            if (un & 1) res *= base;
            base *= base;
            un >>= 1;
        }
        return neg ? (1.0 / res) : res;
    }
    if (a < 0.0) return js_nan();
    return js_exp(b * js_log(a));
}
static double js_cbrt(double x) {
    if (!js_isfinite(x) || x == 0.0) return x;
    int neg = x < 0.0;
    double r = js_pow(neg ? -x : x, 1.0 / 3.0);
    return neg ? -r : r;
}
static double js_sin(double x) {
    if (!js_isfinite(x)) return js_nan();
    const double PI = 3.14159265358979323846;
    const double TWO_PI = 6.28318530717958647692;
    x = js_fmod(x, TWO_PI);
    if (x > PI) x -= TWO_PI;
    if (x < -PI) x += TWO_PI;
    if (x > PI * 0.5) x = PI - x;
    if (x < -PI * 0.5) x = -PI - x;
    double x2 = x * x, term = x, sum = x;
    for (int i = 1; i <= 11; i++) {
        term = -term * x2 / (double)((2 * i) * (2 * i + 1));
        sum += term;
    }
    return sum;
}
static double js_cos(double x) {
    return js_sin(x + 1.57079632679489661923);
}
static double js_tan(double x) {
    double c = js_cos(x);
    return js_sin(x) / c;
}
static double js_atan(double x) {
    if (js_isnan(x)) return js_nan();
    const double PI2 = 1.57079632679489661923;
    if (js_isinf(x)) return x > 0 ? PI2 : -PI2;
    int neg = x < 0.0; if (neg) x = -x;
    int inv = 0; if (x > 1.0) { x = 1.0 / x; inv = 1; }
    /* Half-angle reduction so |t| <= tan(pi/8) ~ 0.414 */
    double t = x / (1.0 + js_sqrt(1.0 + x * x));
    double t2 = t * t, term = t, sum = t;
    for (int i = 1; i <= 16; i++) {
        term = -term * t2;
        sum += term / (double)(2 * i + 1);
    }
    double r = 2.0 * sum;
    if (inv) r = PI2 - r;
    return neg ? -r : r;
}
static double js_atan2(double y, double x) {
    if (js_isnan(x) || js_isnan(y)) return js_nan();
    const double PI = 3.14159265358979323846;
    if (x > 0.0) return js_atan(y / x);
    if (x < 0.0) return y >= 0.0 ? js_atan(y / x) + PI : js_atan(y / x) - PI;
    if (y > 0.0) return PI * 0.5;
    if (y < 0.0) return -PI * 0.5;
    return 0.0;
}
static double js_asin(double x) {
    if (js_isnan(x) || x < -1.0 || x > 1.0) return js_nan();
    return js_atan2(x, js_sqrt(1.0 - x * x));
}
static double js_acos(double x) {
    if (js_isnan(x) || x < -1.0 || x > 1.0) return js_nan();
    return js_atan2(js_sqrt(1.0 - x * x), x);
}

/* PRNG (xorshift64*) */
static uint64_t g_js_rng = 0x9E3779B97F4A7C15ULL;
static double js_random_num(void) {
    if (g_basi && g_basi->time && g_basi->time->ticks_ms)
        g_js_rng ^= (uint64_t)g_basi->time->ticks_ms() * 0xBF58476D1CE4E5B9ULL;
    uint64_t x = g_js_rng;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    g_js_rng = x;
    return (double)((x * 0x2545F4914F6CDD1DULL) >> 11) * (1.0 / 9007199254740992.0);
}

/* Sayi -> String (ECMAScript bicimi) */
static int js_dtoa(double v, char *buf, int cap) {
    if (cap < 24) return 0;
    if (js_isnan(v)) { bcopy_str(buf, "NaN", cap); return 3; }
    if (js_isinf(v)) {
        if (v < 0) { bcopy_str(buf, "-Infinity", cap); return 9; }
        bcopy_str(buf, "Infinity", cap); return 8;
    }
    if (v == 0.0) { buf[0] = '0'; buf[1] = 0; return 1; }
    int pos = 0;
    if (v < 0.0) { buf[pos++] = '-'; v = -v; }
    /* Exact integer fast path up to 1e15 */
    if (v < 1e15 && v == js_trunc(v)) {
        uint64_t iv = (uint64_t)v;
        char tmp[24]; int tn = 0;
        while (iv > 0) { tmp[tn++] = (char)('0' + (iv % 10)); iv /= 10; }
        while (tn > 0 && pos < cap - 1) buf[pos++] = tmp[--tn];
        buf[pos] = 0;
        return pos;
    }
    /* Scientific notation for very large or very small numbers */
    if (v >= 1e21 || v < 1e-6) {
        int exp = 0;
        double m = v;
        while (m >= 10.0) { m /= 10.0; exp++; }
        while (m < 1.0) { m *= 10.0; exp--; }
        int d0 = (int)m;
        buf[pos++] = (char)('0' + d0);
        double frac = m - (double)d0;
        char fbuf[18]; int fn = 0;
        for (int i = 0; i < 12; i++) {
            frac *= 10.0;
            int di = (int)frac;
            if (di > 9) di = 9;
            fbuf[fn++] = (char)('0' + di);
            frac -= (double)di;
        }
        while (fn > 0 && fbuf[fn - 1] == '0') fn--;
        if (fn > 0 && pos < cap - 2) {
            buf[pos++] = '.';
            for (int i = 0; i < fn && pos < cap - 8; i++) buf[pos++] = fbuf[i];
        }
        buf[pos++] = 'e';
        buf[pos++] = exp >= 0 ? '+' : '-';
        int ae = exp >= 0 ? exp : -exp;
        char et[8]; int en = 0;
        if (ae == 0) et[en++] = '0';
        while (ae > 0) { et[en++] = (char)('0' + (ae % 10)); ae /= 10; }
        while (en > 0 && pos < cap - 1) buf[pos++] = et[--en];
        buf[pos] = 0;
        return pos;
    }
    /* Standard decimal */
    uint64_t ip = (uint64_t)v;
    double fp = v - (double)ip;
    /* Round to 12 significant decimal digits cleanly */
    double scale = 1e10;
    uint64_t fscaled = (uint64_t)(fp * scale + 0.5);
    if (fscaled >= (uint64_t)scale) { ip++; fscaled = 0; }
    char tmp[24]; int tn = 0;
    if (ip == 0) tmp[tn++] = '0';
    while (ip > 0) { tmp[tn++] = (char)('0' + (ip % 10)); ip /= 10; }
    while (tn > 0 && pos < cap - 1) buf[pos++] = tmp[--tn];
    if (fscaled > 0 && pos < cap - 2) {
        buf[pos++] = '.';
        char fb[16]; int fn = 10;
        for (int i = fn - 1; i >= 0; i--) { fb[i] = (char)('0' + (fscaled % 10)); fscaled /= 10; }
        while (fn > 0 && fb[fn - 1] == '0') fn--;
        for (int i = 0; i < fn && pos < cap - 1; i++) buf[pos++] = fb[i];
    }
    buf[pos] = 0;
    return pos;
}

static int js_i64toa(int64_t v, char *buf, int cap) {
    if (cap < 2) return 0;
    int pos = 0;
    uint64_t uv;
    if (v < 0) { buf[pos++] = '-'; uv = (uint64_t)(-(v + 1)) + 1ULL; }
    else uv = (uint64_t)v;
    char tmp[24]; int tn = 0;
    if (uv == 0) tmp[tn++] = '0';
    while (uv > 0) { tmp[tn++] = (char)('0' + (uv % 10)); uv /= 10; }
    while (tn > 0 && pos < cap - 1) buf[pos++] = tmp[--tn];
    buf[pos] = 0;
    return pos;
}

static int js_is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}
static int js_is_digit(unsigned char c) { return c >= '0' && c <= '9'; }
static int js_is_hex(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static int js_hex_val(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int js_is_id_start(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || c >= 0x80;
}
static int js_is_id_part(unsigned char c) {
    return js_is_id_start(c) || js_is_digit(c);
}

/* String -> Number (ECMAScript ToNumber kurallari) */
static double js_str_to_num(const char *s, int len) {
    int i = 0;
    while (i < len && js_is_space((unsigned char)s[i])) i++;
    while (len > i && js_is_space((unsigned char)s[len - 1])) len--;
    if (i == len) return 0.0;
    int neg = 0;
    if (s[i] == '+') i++;
    else if (s[i] == '-') { neg = 1; i++; }
    if (len - i == 8 && js_strncmp(s + i, "Infinity", 8) == 0) return neg ? -js_inf() : js_inf();
    /* 0x / 0b / 0o */
    if (!neg && len - i > 2 && s[i] == '0') {
        if (s[i + 1] == 'x' || s[i + 1] == 'X') {
            double v = 0.0;
            for (int k = i + 2; k < len; k++) {
                int h = js_hex_val((unsigned char)s[k]);
                if (h < 0) return js_nan();
                v = v * 16.0 + (double)h;
            }
            return v;
        }
        if (s[i + 1] == 'b' || s[i + 1] == 'B') {
            double v = 0.0;
            for (int k = i + 2; k < len; k++) {
                if (s[k] != '0' && s[k] != '1') return js_nan();
                v = v * 2.0 + (double)(s[k] - '0');
            }
            return v;
        }
        if (s[i + 1] == 'o' || s[i + 1] == 'O') {
            double v = 0.0;
            for (int k = i + 2; k < len; k++) {
                if (s[k] < '0' || s[k] > '7') return js_nan();
                v = v * 8.0 + (double)(s[k] - '0');
            }
            return v;
        }
    }
    double v = 0.0;
    int has_dig = 0;
    while (i < len && js_is_digit((unsigned char)s[i])) { v = v * 10.0 + (double)(s[i++] - '0'); has_dig = 1; }
    if (i < len && s[i] == '.') {
        i++;
        double f = 0.1;
        while (i < len && js_is_digit((unsigned char)s[i])) { v += (double)(s[i++] - '0') * f; f *= 0.1; has_dig = 1; }
    }
    if (!has_dig) return js_nan();
    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        int eneg = 0;
        if (i < len && s[i] == '+') i++;
        else if (i < len && s[i] == '-') { eneg = 1; i++; }
        if (i >= len || !js_is_digit((unsigned char)s[i])) return js_nan();
        int e = 0;
        while (i < len && js_is_digit((unsigned char)s[i])) {
            if (e < 1000) e = e * 10 + (s[i] - '0');
            i++;
        }
        v *= js_pow(10.0, eneg ? (double)(-e) : (double)e);
    }
    if (i != len) return js_nan();
    return neg ? -v : v;
}

/* ==========================================================================
 * 1. JS DEGER, STRING, NESNE VE PROTOTIP MODELI
 * ========================================================================== */

typedef enum {
    JS_UNDEF = 0,
    JS_NULL,
    JS_BOOL,
    JS_NUM,
    JS_BIGINT,
    JS_STR,
    JS_SYM,
    JS_OBJ
} js_type_t;

typedef struct js_str {
    int len;
    uint32_t hash;
    char data[];
} js_str_t;

typedef struct js_obj js_obj_t;
typedef struct js_ctx js_ctx_t;
typedef struct js_ast js_ast_t;
typedef struct js_env js_env_t;

typedef struct {
    js_type_t type;
    union {
        int       b;
        double    n;
        int64_t   bi;
        js_str_t *s;
        int       sym;
        js_obj_t *o;
    } u;
} js_val_t;

static inline js_val_t jv_undef(void)          { js_val_t v; v.type = JS_UNDEF; v.u.bi = 0; return v; }
static inline js_val_t jv_null(void)           { js_val_t v; v.type = JS_NULL; v.u.bi = 0; return v; }
static inline js_val_t jv_bool(int b)          { js_val_t v; v.type = JS_BOOL; v.u.b = b ? 1 : 0; return v; }
static inline js_val_t jv_num(double n)        { js_val_t v; v.type = JS_NUM; v.u.n = n; return v; }
static inline js_val_t jv_bigint(int64_t bi)   { js_val_t v; v.type = JS_BIGINT; v.u.bi = bi; return v; }
static inline js_val_t jv_str(js_str_t *s)     { js_val_t v; v.type = JS_STR; v.u.s = s; return v; }
static inline js_val_t jv_sym(int id)          { js_val_t v; v.type = JS_SYM; v.u.sym = id; return v; }
static inline js_val_t jv_obj(js_obj_t *o)     { js_val_t v; if (!o) return jv_null(); v.type = JS_OBJ; v.u.o = o; return v; }

static uint32_t js_hash_bytes(const char *s, int len) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}

static js_str_t *js_str_new(const char *s, int len) {
    if (!s) { s = ""; len = 0; }
    if (len < 0) len = (int)js_strlen(s);
    js_str_t *st = (js_str_t *)js_malloc(sizeof(js_str_t) + (size_t)len + 1);
    if (!st) return 0;
    st->len = len;
    st->hash = js_hash_bytes(s, len);
    if (len > 0) js_memcpy(st->data, s, (size_t)len);
    st->data[len] = 0;
    return st;
}
static js_val_t jv_cstr(const char *s) { return jv_str(js_str_new(s ? s : "", -1)); }
static js_val_t jv_cstr_n(const char *s, int len) { return jv_str(js_str_new(s ? s : "", len)); }

static int js_str_eq(js_str_t *a, js_str_t *b) {
    if (a == b) return 1;
    if (!a || !b || a->len != b->len || a->hash != b->hash) return 0;
    return js_memcmp(a->data, b->data, (size_t)a->len) == 0;
}
static int js_str_eq_c(js_str_t *a, const char *c) {
    if (!a || !c) return 0;
    int cl = (int)js_strlen(c);
    return a->len == cl && js_memcmp(a->data, c, (size_t)cl) == 0;
}

/* Dinamik string tamponu */
typedef struct {
    char *buf;
    int len;
    int cap;
} js_sb_t;

static void js_sb_init(js_sb_t *sb) { sb->buf = 0; sb->len = 0; sb->cap = 0; }
static void js_sb_grow(js_sb_t *sb, int need) {
    if (sb->len + need + 1 <= sb->cap) return;
    int nc = sb->cap ? sb->cap * 2 : 64;
    while (nc < sb->len + need + 1) nc *= 2;
    char *nb = (char *)js_realloc(sb->buf, (size_t)nc);
    if (!nb) return;
    sb->buf = nb; sb->cap = nc;
}
static void js_sb_putn(js_sb_t *sb, const char *s, int n) {
    if (!s || n <= 0) return;
    js_sb_grow(sb, n);
    if (!sb->buf) return;
    js_memcpy(sb->buf + sb->len, s, (size_t)n);
    sb->len += n;
    sb->buf[sb->len] = 0;
}
static void js_sb_putc(js_sb_t *sb, char c) { js_sb_putn(sb, &c, 1); }
static void js_sb_puts(js_sb_t *sb, const char *s) { if (s) js_sb_putn(sb, s, (int)js_strlen(s)); }
static js_str_t *js_sb_finish(js_sb_t *sb) {
    js_str_t *r = js_str_new(sb->buf ? sb->buf : "", sb->len);
    if (sb->buf) js_free(sb->buf);
    sb->buf = 0; sb->len = 0; sb->cap = 0;
    return r;
}

/* Nesne turleri ve ozellikler */
typedef enum {
    OBJ_PLAIN = 0,
    OBJ_ARRAY,
    OBJ_FUNC,
    OBJ_NATIVE,
    OBJ_REGEXP,
    OBJ_MAP,
    OBJ_SET,
    OBJ_PROMISE,
    OBJ_DATE,
    OBJ_ERROR,
    OBJ_ARRAYBUFFER,
    OBJ_TYPEDARRAY,
    OBJ_PROXY,
    OBJ_DOM,
    OBJ_DOM_STYLE,
    OBJ_DOM_CLASSLIST,
    OBJ_DOM_DATASET,
    OBJ_GENERATOR,
    OBJ_URL,
    OBJ_URLPARAMS,
    OBJ_RESPONSE
} js_obj_kind_t;

#define PROP_ENUM  1
#define PROP_WRIT  2
#define PROP_CONF  4
#define PROP_ACC   8

typedef struct {
    char     *key;
    uint32_t  hash;
    int       sym_id;   /* >0 ise sembol anahtari */
    js_val_t  val;
    js_obj_t *getter;
    js_obj_t *setter;
    uint8_t   flags;
} js_prop_t;

typedef js_val_t (*js_native_fn)(js_ctx_t *ctx, js_val_t this_val, js_val_t *args, int argc, void *userdata);

typedef struct {
    js_val_t key;
    js_val_t val;
    uint8_t  used;
} js_map_ent_t;

typedef struct js_prom_cb {
    js_obj_t *on_fulfill;
    js_obj_t *on_reject;
    js_obj_t *next_prom;
    struct js_prom_cb *next;
} js_prom_cb_t;

struct js_obj {
    js_obj_kind_t kind;
    js_obj_t     *proto;
    js_prop_t    *props;
    int           prop_n;
    int           prop_cap;
    uint8_t       frozen;
    uint8_t       sealed;
    /* Array */
    js_val_t     *elems;
    int           elem_n;
    int           elem_cap;
    /* JS Function */
    js_ast_t     *fn_ast;
    js_env_t     *fn_env;
    js_val_t      bound_this;
    js_val_t     *bound_args;
    int           bound_argc;
    js_obj_t     *bound_target;
    js_obj_t     *super_proto;
    uint8_t       is_arrow;
    uint8_t       is_async;
    uint8_t       is_gen;
    uint8_t       is_class;
    /* Native Function */
    js_native_fn  native_fn;
    void         *native_ud;
    /* RegExp */
    char         *re_pat;
    char          re_flags[8];
    int           re_last_idx;
    /* Map / Set */
    js_map_ent_t *map_ents;
    int           map_n;
    int           map_cap;
    /* Promise */
    int           prom_state; /* 0=pending, 1=fulfilled, 2=rejected */
    js_val_t      prom_val;
    js_prom_cb_t *prom_cbs;
    /* Date */
    double        date_ms;
    /* ArrayBuffer / TypedArray */
    uint8_t      *buf;
    int           buf_len;
    int           ta_kind; /* 0=U8,1=I8,2=U8C,3=I16,4=U16,5=I32,6=U32,7=F32,8=F64 */
    int           ta_off;
    js_obj_t     *ta_ab;
    /* Proxy */
    js_obj_t     *proxy_target;
    js_obj_t     *proxy_handler;
    /* DOM node index (-1 = document, -2 = window, >=0 = g_dn[idx]) */
    int           dom_idx;
    /* Generator collected values */
    js_val_t     *gen_vals;
    int           gen_n;
    int           gen_pos;
    int           gen_done;
};

/* Olay dinleyicisi & Zamanlayici yapilari */
#define JS_MAX_LISTENERS 512
#define JS_MAX_TIMERS    128
#define JS_MAX_MICRO     512
#define JS_MAX_SYMS      256
#define JS_MAX_STORAGE   128
#define JS_CONSOLE_MAX   16384

typedef struct {
    int       dom_idx;   /* -2=window, -1=document, >=0=g_dn[idx] */
    char      type[32];
    js_obj_t *fn;
    uint8_t   once;
    uint8_t   active;
} js_listener_t;

typedef struct {
    int       id;
    js_obj_t *fn;
    js_str_t *code;
    js_val_t *args;
    int       argc;
    uint64_t  fire_ms;
    uint32_t  interval_ms;
    uint8_t   is_interval;
    uint8_t   is_raf;
    uint8_t   active;
} js_timer_t;

typedef struct {
    js_obj_t *fn;
    js_val_t  arg;
    int       has_arg;
} js_micro_t;

typedef struct {
    char *k;
    char *v;
} js_kv_t;

struct js_env {
    js_env_t  *parent;
    js_obj_t  *vars;      /* degisken tablosu */
    uint8_t   *is_const;  /* const bayraklari (prop index eslesmeli) */
    int        const_cap;
    js_val_t   this_val;
    int        has_this;
    js_obj_t  *super_proto;
    int        is_fn_scope;
};

struct js_ctx {
    js_env_t     *global_env;
    js_obj_t     *global_obj;
    js_obj_t     *obj_proto;
    js_obj_t     *arr_proto;
    js_obj_t     *str_proto;
    js_obj_t     *num_proto;
    js_obj_t     *bool_proto;
    js_obj_t     *fn_proto;
    js_obj_t     *re_proto;
    js_obj_t     *map_proto;
    js_obj_t     *set_proto;
    js_obj_t     *prom_proto;
    js_obj_t     *date_proto;
    js_obj_t     *err_proto;
    js_obj_t     *ta_proto;
    js_obj_t     *ab_proto;
    js_obj_t     *dom_proto;
    js_obj_t     *url_proto;
    js_obj_t     *urlp_proto;
    js_obj_t     *doc_obj;
    /* Semboller */
    char         *sym_desc[JS_MAX_SYMS];
    int           sym_n;
    int           sym_iterator;
    int           sym_to_prim;
    int           sym_to_strtag;
    /* Kontrol akisi / Hata */
    int           throwing;
    js_val_t      exc;
    int           returning;
    js_val_t      ret_val;
    int           breaking;
    int           continuing;
    char          jump_label[48];
    /* Generator yield toplama */
    int           in_gen;
    js_val_t     *gen_buf;
    int           gen_cnt;
    int           gen_cap;
    /* Adim & derinlik korumasi */
    uint32_t      steps;
    uint32_t      max_steps;
    int           depth;
    int           max_depth;
    /* Olaylar / Zamanlayicilar / Mikrogorevler */
    js_listener_t listeners[JS_MAX_LISTENERS];
    int           listener_n;
    js_timer_t    timers[JS_MAX_TIMERS];
    int           next_timer_id;
    js_micro_t    micros[JS_MAX_MICRO];
    int           micro_head, micro_tail;
    /* DOM wrapper onbellegi (dinamik heap) */
    js_obj_t    **dom_cache;
    int           dom_cache_cap;
    /* Depolama */
    js_kv_t       local_store[JS_MAX_STORAGE];
    int           local_n;
    js_kv_t       session_store[JS_MAX_STORAGE];
    int           session_n;
    /* Konsol cikti tamponu */
    char          console_buf[JS_CONSOLE_MAX];
    int           console_len;
    /* Event dispatch bayraklari */
    int           ev_prevented;
    int           ev_stopped;
    /* Zamanlayici/performans */
    uint64_t      time_origin;
};

static js_ctx_t *g_js;

/* Ileri bildirimler */
static js_val_t js_call_fn(js_ctx_t *ctx, js_val_t fnv, js_val_t this_val, js_val_t *args, int argc, int is_new);
static js_val_t js_get_prop(js_ctx_t *ctx, js_val_t obj, const char *key);
static js_val_t js_get_prop_v(js_ctx_t *ctx, js_val_t obj, js_val_t key);
static void     js_set_prop(js_ctx_t *ctx, js_val_t obj, const char *key, js_val_t val);
static void     js_set_prop_v(js_ctx_t *ctx, js_val_t obj, js_val_t key, js_val_t val);
static int      js_has_prop_v(js_ctx_t *ctx, js_val_t obj, js_val_t key);
static int      js_del_prop_v(js_ctx_t *ctx, js_val_t obj, js_val_t key);
static js_str_t *js_to_str(js_ctx_t *ctx, js_val_t v);
static double   js_to_num(js_ctx_t *ctx, js_val_t v);
static int      js_to_bool(js_val_t v);
static js_val_t js_eval_source(js_ctx_t *ctx, const char *src, int len, js_env_t *env);
static void     js_drain_microtasks(js_ctx_t *ctx);
static js_obj_t *js_dom_wrap(js_ctx_t *ctx, int dom_idx);
static js_val_t js_dom_get_prop(js_ctx_t *ctx, js_obj_t *o, const char *k);
static void     js_dom_set_prop(js_ctx_t *ctx, js_obj_t *o, const char *k, js_val_t v);
static void     js_throw_err(js_ctx_t *ctx, const char *kind, const char *msg);
static js_val_t js_prom_resolve_val(js_ctx_t *ctx, js_obj_t *p, js_val_t v);
static js_val_t js_prom_reject_val(js_ctx_t *ctx, js_obj_t *p, js_val_t v);

/* Nesne olusturucular */
static js_obj_t *js_obj_new(js_ctx_t *ctx, js_obj_kind_t kind, js_obj_t *proto) {
    js_obj_t *o = (js_obj_t *)js_calloc(1, sizeof(js_obj_t));
    if (!o) return 0;
    o->kind = kind;
    o->proto = proto ? proto : (ctx ? ctx->obj_proto : 0);
    o->dom_idx = -99;
    return o;
}

static js_obj_t *js_arr_new(js_ctx_t *ctx, int cap) {
    js_obj_t *o = js_obj_new(ctx, OBJ_ARRAY, ctx ? ctx->arr_proto : 0);
    if (!o) return 0;
    if (cap > 0) {
        o->elems = (js_val_t *)js_calloc((size_t)cap, sizeof(js_val_t));
        o->elem_cap = o->elems ? cap : 0;
    }
    return o;
}

static void js_arr_push(js_obj_t *a, js_val_t v) {
    if (!a) return;
    if (a->elem_n >= a->elem_cap) {
        int nc = a->elem_cap ? a->elem_cap * 2 : 8;
        js_val_t *ne = (js_val_t *)js_realloc(a->elems, (size_t)nc * sizeof(js_val_t));
        if (!ne) return;
        a->elems = ne; a->elem_cap = nc;
    }
    a->elems[a->elem_n++] = v;
}

static js_obj_t *js_native_new(js_ctx_t *ctx, js_native_fn fn, void *ud, const char *name) {
    js_obj_t *o = js_obj_new(ctx, OBJ_NATIVE, ctx ? ctx->fn_proto : 0);
    if (!o) return 0;
    o->native_fn = fn;
    o->native_ud = ud;
    if (name) {
        js_prop_t *p = (js_prop_t *)js_calloc(2, sizeof(js_prop_t));
        if (p) {
            o->props = p; o->prop_cap = 2; o->prop_n = 1;
            int nl = (int)js_strlen(name);
            p[0].key = (char *)js_malloc((size_t)nl + 1);
            if (p[0].key) { js_memcpy(p[0].key, name, (size_t)nl + 1); }
            p[0].hash = js_hash_bytes(name, nl);
            p[0].val = jv_cstr(name);
            p[0].flags = PROP_CONF;
        }
    }
    return o;
}

/* Ozellik yonetimi */
static js_prop_t *js_find_own_prop(js_obj_t *o, const char *key, uint32_t h, int sym_id) {
    if (!o) return 0;
    for (int i = 0; i < o->prop_n; i++) {
        if (sym_id > 0) {
            if (o->props[i].sym_id == sym_id) return &o->props[i];
        } else if (o->props[i].sym_id == 0 && o->props[i].hash == h &&
                   o->props[i].key && js_strcmp(o->props[i].key, key) == 0) {
            return &o->props[i];
        }
    }
    return 0;
}

static js_prop_t *js_add_own_prop(js_obj_t *o, const char *key, int sym_id) {
    if (!o) return 0;
    if (o->prop_n >= o->prop_cap) {
        int nc = o->prop_cap ? o->prop_cap * 2 : 8;
        js_prop_t *np = (js_prop_t *)js_realloc(o->props, (size_t)nc * sizeof(js_prop_t));
        if (!np) return 0;
        o->props = np; o->prop_cap = nc;
    }
    js_prop_t *p = &o->props[o->prop_n++];
    js_memset(p, 0, sizeof(*p));
    p->sym_id = sym_id;
    if (sym_id == 0 && key) {
        int kl = (int)js_strlen(key);
        p->key = (char *)js_malloc((size_t)kl + 1);
        if (p->key) js_memcpy(p->key, key, (size_t)kl + 1);
        p->hash = js_hash_bytes(key, kl);
    }
    p->flags = PROP_ENUM | PROP_WRIT | PROP_CONF;
    return p;
}

static void js_def_prop(js_obj_t *o, const char *key, js_val_t val, uint8_t flags) {
    if (!o || !key) return;
    uint32_t h = js_hash_bytes(key, (int)js_strlen(key));
    js_prop_t *p = js_find_own_prop(o, key, h, 0);
    if (!p) p = js_add_own_prop(o, key, 0);
    if (!p) return;
    p->val = val;
    p->flags = flags;
    p->getter = 0; p->setter = 0;
}

static void js_def_sym_prop(js_obj_t *o, int sym_id, js_val_t val) {
    if (!o || sym_id <= 0) return;
    js_prop_t *p = js_find_own_prop(o, 0, 0, sym_id);
    if (!p) p = js_add_own_prop(o, 0, sym_id);
    if (!p) return;
    p->val = val;
    p->flags = PROP_WRIT | PROP_CONF;
}

static void js_def_native(js_ctx_t *ctx, js_obj_t *o, const char *name, js_native_fn fn, void *ud) {
    js_def_prop(o, name, jv_obj(js_native_new(ctx, fn, ud, name)), PROP_WRIT | PROP_CONF);
}

static int js_key_is_index(const char *k, int *out_idx) {
    if (!k || !k[0]) return 0;
    if (k[0] == '0' && k[1] == 0) { *out_idx = 0; return 1; }
    if (k[0] < '1' || k[0] > '9') return 0;
    long v = 0;
    for (int i = 0; k[i]; i++) {
        if (k[i] < '0' || k[i] > '9') return 0;
        v = v * 10 + (k[i] - '0');
        if (v > 10000000L) return 0;
    }
    *out_idx = (int)v;
    return 1;
}

/* TypedArray yardimcilari */
static int js_ta_elem_size(int k) {
    switch (k) {
        case 0: case 1: case 2: return 1;
        case 3: case 4: return 2;
        case 5: case 6: case 7: return 4;
        case 8: return 8;
        default: return 1;
    }
}
static double js_ta_get(js_obj_t *o, int idx) {
    int es = js_ta_elem_size(o->ta_kind);
    if (!o->buf || idx < 0 || o->ta_off + (idx + 1) * es > o->buf_len) return js_nan();
    uint8_t *p = o->buf + o->ta_off + idx * es;
    switch (o->ta_kind) {
        case 0: case 2: return (double)p[0];
        case 1: return (double)(int8_t)p[0];
        case 3: { int16_t v; js_memcpy(&v, p, 2); return (double)v; }
        case 4: { uint16_t v; js_memcpy(&v, p, 2); return (double)v; }
        case 5: { int32_t v; js_memcpy(&v, p, 4); return (double)v; }
        case 6: { uint32_t v; js_memcpy(&v, p, 4); return (double)v; }
        case 7: { float v; js_memcpy(&v, p, 4); return (double)v; }
        case 8: { double v; js_memcpy(&v, p, 8); return v; }
    }
    return 0.0;
}
static void js_ta_set(js_obj_t *o, int idx, double val) {
    int es = js_ta_elem_size(o->ta_kind);
    if (!o->buf || idx < 0 || o->ta_off + (idx + 1) * es > o->buf_len) return;
    uint8_t *p = o->buf + o->ta_off + idx * es;
    switch (o->ta_kind) {
        case 0: p[0] = (uint8_t)(int64_t)val; break;
        case 1: p[0] = (uint8_t)(int8_t)(int64_t)val; break;
        case 2: { int64_t v = js_isnan(val) ? 0 : (int64_t)js_round_js(val); if (v < 0) v = 0; if (v > 255) v = 255; p[0] = (uint8_t)v; break; }
        case 3: { int16_t v = (int16_t)(int64_t)val; js_memcpy(p, &v, 2); break; }
        case 4: { uint16_t v = (uint16_t)(int64_t)val; js_memcpy(p, &v, 2); break; }
        case 5: { int32_t v = (int32_t)(int64_t)val; js_memcpy(p, &v, 4); break; }
        case 6: { uint32_t v = (uint32_t)(int64_t)val; js_memcpy(p, &v, 4); break; }
        case 7: { float v = (float)val; js_memcpy(p, &v, 4); break; }
        case 8: js_memcpy(p, &val, 8); break;
    }
}

/* Tip donusumleri */
static int js_to_bool(js_val_t v) {
    switch (v.type) {
        case JS_UNDEF: case JS_NULL: return 0;
        case JS_BOOL: return v.u.b;
        case JS_NUM: return !js_isnan(v.u.n) && v.u.n != 0.0;
        case JS_BIGINT: return v.u.bi != 0;
        case JS_STR: return v.u.s && v.u.s->len > 0;
        case JS_SYM: case JS_OBJ: return 1;
    }
    return 0;
}

static js_val_t js_to_primitive(js_ctx_t *ctx, js_val_t v, int hint_str) {
    if (v.type != JS_OBJ || !v.u.o) return v;
    if (ctx && ctx->sym_to_prim > 0) {
        js_val_t m = js_get_prop_v(ctx, v, jv_sym(ctx->sym_to_prim));
        if (m.type == JS_OBJ && (m.u.o->kind == OBJ_FUNC || m.u.o->kind == OBJ_NATIVE)) {
            js_val_t arg = jv_cstr(hint_str ? "string" : "number");
            return js_call_fn(ctx, m, v, &arg, 1, 0);
        }
    }
    const char *first = hint_str ? "toString" : "valueOf";
    const char *second = hint_str ? "valueOf" : "toString";
    js_val_t m1 = js_get_prop(ctx, v, first);
    if (m1.type == JS_OBJ && (m1.u.o->kind == OBJ_FUNC || m1.u.o->kind == OBJ_NATIVE)) {
        js_val_t r = js_call_fn(ctx, m1, v, 0, 0, 0);
        if (r.type != JS_OBJ) return r;
    }
    js_val_t m2 = js_get_prop(ctx, v, second);
    if (m2.type == JS_OBJ && (m2.u.o->kind == OBJ_FUNC || m2.u.o->kind == OBJ_NATIVE)) {
        js_val_t r = js_call_fn(ctx, m2, v, 0, 0, 0);
        if (r.type != JS_OBJ) return r;
    }
    return jv_cstr("[object Object]");
}

static double js_to_num(js_ctx_t *ctx, js_val_t v) {
    if (v.type == JS_OBJ) v = js_to_primitive(ctx, v, 0);
    switch (v.type) {
        case JS_UNDEF: return js_nan();
        case JS_NULL: return 0.0;
        case JS_BOOL: return v.u.b ? 1.0 : 0.0;
        case JS_NUM: return v.u.n;
        case JS_BIGINT: return (double)v.u.bi;
        case JS_STR: return v.u.s ? js_str_to_num(v.u.s->data, v.u.s->len) : 0.0;
        default: return js_nan();
    }
}

static int32_t js_to_i32(js_ctx_t *ctx, js_val_t v) {
    double n = js_to_num(ctx, v);
    if (!js_isfinite(n) || n == 0.0) return 0;
    double t = js_fmod(js_trunc(n), 4294967296.0);
    if (t < 0.0) t += 4294967296.0;
    uint32_t u = (uint32_t)t;
    return (int32_t)u;
}

static uint32_t js_to_u32(js_ctx_t *ctx, js_val_t v) {
    return (uint32_t)js_to_i32(ctx, v);
}

static js_str_t *js_to_str(js_ctx_t *ctx, js_val_t v) {
    if (v.type == JS_STR) return v.u.s ? v.u.s : js_str_new("", 0);
    if (v.type == JS_OBJ) v = js_to_primitive(ctx, v, 1);
    switch (v.type) {
        case JS_UNDEF: return js_str_new("undefined", 9);
        case JS_NULL:  return js_str_new("null", 4);
        case JS_BOOL:  return v.u.b ? js_str_new("true", 4) : js_str_new("false", 5);
        case JS_NUM: {
            char b[48]; int n = js_dtoa(v.u.n, b, sizeof(b));
            return js_str_new(b, n);
        }
        case JS_BIGINT: {
            char b[32]; int n = js_i64toa(v.u.bi, b, sizeof(b));
            return js_str_new(b, n);
        }
        case JS_STR: return v.u.s ? v.u.s : js_str_new("", 0);
        case JS_SYM: {
            char b[96]; bcopy_str(b, "Symbol(", sizeof(b));
            if (ctx && v.u.sym > 0 && v.u.sym <= ctx->sym_n && ctx->sym_desc[v.u.sym - 1])
                bcat(b, ctx->sym_desc[v.u.sym - 1], sizeof(b));
            bcat(b, ")", sizeof(b));
            return js_str_new(b, -1);
        }
        default: return js_str_new("[object Object]", 15);
    }
}

static int js_strict_eq(js_val_t a, js_val_t b) {
    if (a.type != b.type) return 0;
    switch (a.type) {
        case JS_UNDEF: case JS_NULL: return 1;
        case JS_BOOL: return a.u.b == b.u.b;
        case JS_NUM: return a.u.n == b.u.n; /* NaN != NaN, +0 == -0 */
        case JS_BIGINT: return a.u.bi == b.u.bi;
        case JS_STR: return js_str_eq(a.u.s, b.u.s);
        case JS_SYM: return a.u.sym == b.u.sym;
        case JS_OBJ: return a.u.o == b.u.o;
    }
    return 0;
}

static int js_same_value_zero(js_val_t a, js_val_t b) {
    if (a.type == JS_NUM && b.type == JS_NUM && js_isnan(a.u.n) && js_isnan(b.u.n)) return 1;
    return js_strict_eq(a, b);
}

static int js_same_value(js_val_t a, js_val_t b) {
    if (a.type == JS_NUM && b.type == JS_NUM) {
        if (js_isnan(a.u.n) && js_isnan(b.u.n)) return 1;
        if (a.u.n == 0.0 && b.u.n == 0.0) return js_signbit(a.u.n) == js_signbit(b.u.n);
    }
    return js_strict_eq(a, b);
}

static int js_loose_eq(js_ctx_t *ctx, js_val_t a, js_val_t b) {
    if (a.type == b.type) return js_strict_eq(a, b);
    if ((a.type == JS_NULL && b.type == JS_UNDEF) || (a.type == JS_UNDEF && b.type == JS_NULL)) return 1;
    if (a.type == JS_NUM && b.type == JS_STR) return a.u.n == js_to_num(ctx, b);
    if (a.type == JS_STR && b.type == JS_NUM) return js_to_num(ctx, a) == b.u.n;
    if (a.type == JS_BOOL) return js_loose_eq(ctx, jv_num(a.u.b ? 1.0 : 0.0), b);
    if (b.type == JS_BOOL) return js_loose_eq(ctx, a, jv_num(b.u.b ? 1.0 : 0.0));
    if (a.type == JS_BIGINT && b.type == JS_NUM) return (double)a.u.bi == b.u.n;
    if (a.type == JS_NUM && b.type == JS_BIGINT) return a.u.n == (double)b.u.bi;
    if (a.type == JS_OBJ && (b.type == JS_STR || b.type == JS_NUM || b.type == JS_SYM))
        return js_loose_eq(ctx, js_to_primitive(ctx, a, 0), b);
    if (b.type == JS_OBJ && (a.type == JS_STR || a.type == JS_NUM || a.type == JS_SYM))
        return js_loose_eq(ctx, a, js_to_primitive(ctx, b, 0));
    return 0;
}

/* Ozellik okuma / yazma (Proxy, Array, TypedArray, String, DOM dahil) */
static js_val_t js_get_prop_sym(js_ctx_t *ctx, js_val_t obj, const char *key, int sym_id) {
    if (obj.type == JS_UNDEF || obj.type == JS_NULL) {
        js_throw_err(ctx, "TypeError", "Cannot read properties of null or undefined");
        return jv_undef();
    }
    if (obj.type == JS_STR) {
        if (sym_id == 0 && key) {
            if (js_strcmp(key, "length") == 0) return jv_num(obj.u.s ? (double)obj.u.s->len : 0.0);
            int idx;
            if (js_key_is_index(key, &idx) && obj.u.s && idx >= 0 && idx < obj.u.s->len)
                return jv_cstr_n(obj.u.s->data + idx, 1);
        }
        js_obj_t *cur = ctx ? ctx->str_proto : 0;
        uint32_t h = key ? js_hash_bytes(key, (int)js_strlen(key)) : 0;
        while (cur) {
            js_prop_t *p = js_find_own_prop(cur, key, h, sym_id);
            if (p) return (p->flags & PROP_ACC) && p->getter ? js_call_fn(ctx, jv_obj(p->getter), obj, 0, 0, 0) : p->val;
            cur = cur->proto;
        }
        return jv_undef();
    }
    if (obj.type == JS_NUM || obj.type == JS_BOOL || obj.type == JS_BIGINT || obj.type == JS_SYM) {
        js_obj_t *cur = (obj.type == JS_NUM) ? ctx->num_proto :
                        (obj.type == JS_BOOL) ? ctx->bool_proto : ctx->obj_proto;
        uint32_t h = key ? js_hash_bytes(key, (int)js_strlen(key)) : 0;
        while (cur) {
            js_prop_t *p = js_find_own_prop(cur, key, h, sym_id);
            if (p) return p->val;
            cur = cur->proto;
        }
        return jv_undef();
    }
    js_obj_t *o = obj.u.o;
    if (!o) return jv_undef();
    /* Proxy get trap */
    if (o->kind == OBJ_PROXY && o->proxy_handler && o->proxy_target) {
        js_val_t trap = js_get_prop(ctx, jv_obj(o->proxy_handler), "get");
        if (trap.type == JS_OBJ && (trap.u.o->kind == OBJ_FUNC || trap.u.o->kind == OBJ_NATIVE)) {
            js_val_t args[3] = { jv_obj(o->proxy_target), sym_id > 0 ? jv_sym(sym_id) : jv_cstr(key), obj };
            return js_call_fn(ctx, trap, jv_obj(o->proxy_handler), args, 3, 0);
        }
        return js_get_prop_sym(ctx, jv_obj(o->proxy_target), key, sym_id);
    }
    if (sym_id == 0 && key) {
        if (js_strcmp(key, "__proto__") == 0) return o->proto ? jv_obj(o->proto) : jv_null();
        if (o->kind == OBJ_ARRAY) {
            if (js_strcmp(key, "length") == 0) return jv_num((double)o->elem_n);
            int idx;
            if (js_key_is_index(key, &idx)) return (idx >= 0 && idx < o->elem_n) ? o->elems[idx] : jv_undef();
        } else if (o->kind == OBJ_TYPEDARRAY) {
            int es = js_ta_elem_size(o->ta_kind);
            int len = (o->buf_len - o->ta_off) / es;
            if (js_strcmp(key, "length") == 0) return jv_num((double)len);
            if (js_strcmp(key, "byteLength") == 0) return jv_num((double)(len * es));
            if (js_strcmp(key, "byteOffset") == 0) return jv_num((double)o->ta_off);
            if (js_strcmp(key, "BYTES_PER_ELEMENT") == 0) return jv_num((double)es);
            if (js_strcmp(key, "buffer") == 0) return o->ta_ab ? jv_obj(o->ta_ab) : jv_null();
            int idx;
            if (js_key_is_index(key, &idx)) return (idx >= 0 && idx < len) ? jv_num(js_ta_get(o, idx)) : jv_undef();
        } else if (o->kind == OBJ_ARRAYBUFFER) {
            if (js_strcmp(key, "byteLength") == 0) return jv_num((double)o->buf_len);
        } else if (o->kind == OBJ_MAP || o->kind == OBJ_SET) {
            if (js_strcmp(key, "size") == 0) {
                int c = 0; for (int i = 0; i < o->map_n; i++) if (o->map_ents[i].used) c++;
                return jv_num((double)c);
            }
        } else if (o->kind == OBJ_REGEXP) {
            if (js_strcmp(key, "source") == 0) return jv_cstr(o->re_pat ? o->re_pat : "");
            if (js_strcmp(key, "flags") == 0) return jv_cstr(o->re_flags);
            if (js_strcmp(key, "lastIndex") == 0) return jv_num((double)o->re_last_idx);
            if (js_strcmp(key, "global") == 0) { for (int i=0; o->re_flags[i]; i++) if (o->re_flags[i]=='g') return jv_bool(1); return jv_bool(0); }
            if (js_strcmp(key, "ignoreCase") == 0) { for (int i=0; o->re_flags[i]; i++) if (o->re_flags[i]=='i') return jv_bool(1); return jv_bool(0); }
            if (js_strcmp(key, "multiline") == 0) { for (int i=0; o->re_flags[i]; i++) if (o->re_flags[i]=='m') return jv_bool(1); return jv_bool(0); }
        } else if (o->kind == OBJ_DOM || o->kind == OBJ_DOM_STYLE || o->kind == OBJ_DOM_CLASSLIST || o->kind == OBJ_DOM_DATASET) {
            js_val_t dv = js_dom_get_prop(ctx, o, key);
            if (dv.type != JS_SYM || dv.u.sym != -999) return dv;
        }
    }
    uint32_t h = (sym_id == 0 && key) ? js_hash_bytes(key, (int)js_strlen(key)) : 0;
    js_obj_t *cur = o;
    int guard = 0;
    while (cur && guard++ < 32) {
        js_prop_t *p = js_find_own_prop(cur, key, h, sym_id);
        if (p) {
            if ((p->flags & PROP_ACC) && p->getter)
                return js_call_fn(ctx, jv_obj(p->getter), obj, 0, 0, 0);
            return p->val;
        }
        cur = cur->proto;
    }
    return jv_undef();
}

static js_val_t js_get_prop(js_ctx_t *ctx, js_val_t obj, const char *key) {
    return js_get_prop_sym(ctx, obj, key, 0);
}

static js_val_t js_get_prop_v(js_ctx_t *ctx, js_val_t obj, js_val_t key) {
    if (key.type == JS_SYM) return js_get_prop_sym(ctx, obj, 0, key.u.sym);
    js_str_t *ks = js_to_str(ctx, key);
    return js_get_prop_sym(ctx, obj, ks ? ks->data : "", 0);
}

static void js_set_prop_sym(js_ctx_t *ctx, js_val_t obj, const char *key, int sym_id, js_val_t val) {
    if (obj.type != JS_OBJ || !obj.u.o) return;
    js_obj_t *o = obj.u.o;
    if (o->kind == OBJ_PROXY && o->proxy_handler && o->proxy_target) {
        js_val_t trap = js_get_prop(ctx, jv_obj(o->proxy_handler), "set");
        if (trap.type == JS_OBJ && (trap.u.o->kind == OBJ_FUNC || trap.u.o->kind == OBJ_NATIVE)) {
            js_val_t args[4] = { jv_obj(o->proxy_target), sym_id > 0 ? jv_sym(sym_id) : jv_cstr(key), val, obj };
            js_call_fn(ctx, trap, jv_obj(o->proxy_handler), args, 4, 0);
            return;
        }
        js_set_prop_sym(ctx, jv_obj(o->proxy_target), key, sym_id, val);
        return;
    }
    if (o->frozen) return;
    if (sym_id == 0 && key) {
        if (js_strcmp(key, "__proto__") == 0) {
            o->proto = (val.type == JS_OBJ) ? val.u.o : 0;
            return;
        }
        if (o->kind == OBJ_ARRAY) {
            if (js_strcmp(key, "length") == 0) {
                int nl = (int)js_to_num(ctx, val);
                if (nl < 0) nl = 0;
                if (nl < o->elem_n) o->elem_n = nl;
                else while (o->elem_n < nl) js_arr_push(o, jv_undef());
                return;
            }
            int idx;
            if (js_key_is_index(key, &idx)) {
                while (o->elem_n <= idx) js_arr_push(o, jv_undef());
                o->elems[idx] = val;
                return;
            }
        } else if (o->kind == OBJ_TYPEDARRAY) {
            int idx;
            if (js_key_is_index(key, &idx)) { js_ta_set(o, idx, js_to_num(ctx, val)); return; }
        } else if (o->kind == OBJ_REGEXP && js_strcmp(key, "lastIndex") == 0) {
            o->re_last_idx = (int)js_to_num(ctx, val);
            return;
        } else if (o->kind == OBJ_DOM || o->kind == OBJ_DOM_STYLE || o->kind == OBJ_DOM_CLASSLIST || o->kind == OBJ_DOM_DATASET) {
            js_dom_set_prop(ctx, o, key, val);
            return;
        }
    }
    uint32_t h = (sym_id == 0 && key) ? js_hash_bytes(key, (int)js_strlen(key)) : 0;
    /* Check prototype chain for setter */
    js_obj_t *cur = o;
    int guard = 0;
    while (cur && guard++ < 32) {
        js_prop_t *p = js_find_own_prop(cur, key, h, sym_id);
        if (p) {
            if (p->flags & PROP_ACC) {
                if (p->setter) js_call_fn(ctx, jv_obj(p->setter), obj, &val, 1, 0);
                return;
            }
            if (cur == o) {
                if (p->flags & PROP_WRIT) p->val = val;
                return;
            }
            break;
        }
        cur = cur->proto;
    }
    if (o->sealed) return;
    js_prop_t *np = js_add_own_prop(o, key, sym_id);
    if (np) np->val = val;
}

static void js_set_prop(js_ctx_t *ctx, js_val_t obj, const char *key, js_val_t val) {
    js_set_prop_sym(ctx, obj, key, 0, val);
}
static void js_set_prop_v(js_ctx_t *ctx, js_val_t obj, js_val_t key, js_val_t val) {
    if (key.type == JS_SYM) { js_set_prop_sym(ctx, obj, 0, key.u.sym, val); return; }
    js_str_t *ks = js_to_str(ctx, key);
    js_set_prop_sym(ctx, obj, ks ? ks->data : "", 0, val);
}

static int js_has_prop_v(js_ctx_t *ctx, js_val_t obj, js_val_t key) {
    if (obj.type != JS_OBJ || !obj.u.o) return 0;
    js_obj_t *o = obj.u.o;
    if (o->kind == OBJ_PROXY && o->proxy_handler && o->proxy_target) {
        js_val_t trap = js_get_prop(ctx, jv_obj(o->proxy_handler), "has");
        if (trap.type == JS_OBJ && (trap.u.o->kind == OBJ_FUNC || trap.u.o->kind == OBJ_NATIVE)) {
            js_val_t args[2] = { jv_obj(o->proxy_target), key };
            return js_to_bool(js_call_fn(ctx, trap, jv_obj(o->proxy_handler), args, 2, 0));
        }
        return js_has_prop_v(ctx, jv_obj(o->proxy_target), key);
    }
    int sym_id = (key.type == JS_SYM) ? key.u.sym : 0;
    js_str_t *ks = sym_id ? 0 : js_to_str(ctx, key);
    const char *k = ks ? ks->data : "";
    if (!sym_id && o->kind == OBJ_ARRAY) {
        if (js_strcmp(k, "length") == 0) return 1;
        int idx;
        if (js_key_is_index(k, &idx) && idx >= 0 && idx < o->elem_n) return 1;
    }
    uint32_t h = sym_id ? 0 : js_hash_bytes(k, (int)js_strlen(k));
    js_obj_t *cur = o; int g = 0;
    while (cur && g++ < 32) {
        if (js_find_own_prop(cur, k, h, sym_id)) return 1;
        cur = cur->proto;
    }
    return 0;
}

static int js_del_prop_v(js_ctx_t *ctx, js_val_t obj, js_val_t key) {
    if (obj.type != JS_OBJ || !obj.u.o) return 1;
    js_obj_t *o = obj.u.o;
    if (o->frozen || o->sealed) return 0;
    if (o->kind == OBJ_PROXY && o->proxy_handler && o->proxy_target) {
        js_val_t trap = js_get_prop(ctx, jv_obj(o->proxy_handler), "deleteProperty");
        if (trap.type == JS_OBJ && (trap.u.o->kind == OBJ_FUNC || trap.u.o->kind == OBJ_NATIVE)) {
            js_val_t args[2] = { jv_obj(o->proxy_target), key };
            return js_to_bool(js_call_fn(ctx, trap, jv_obj(o->proxy_handler), args, 2, 0));
        }
        return js_del_prop_v(ctx, jv_obj(o->proxy_target), key);
    }
    int sym_id = (key.type == JS_SYM) ? key.u.sym : 0;
    js_str_t *ks = sym_id ? 0 : js_to_str(ctx, key);
    const char *k = ks ? ks->data : "";
    if (!sym_id && o->kind == OBJ_ARRAY) {
        int idx;
        if (js_key_is_index(k, &idx) && idx >= 0 && idx < o->elem_n) {
            o->elems[idx] = jv_undef();
            return 1;
        }
    }
    uint32_t h = sym_id ? 0 : js_hash_bytes(k, (int)js_strlen(k));
    for (int i = 0; i < o->prop_n; i++) {
        int m = sym_id ? (o->props[i].sym_id == sym_id)
                       : (o->props[i].sym_id == 0 && o->props[i].hash == h && o->props[i].key && js_strcmp(o->props[i].key, k) == 0);
        if (m) {
            if (!(o->props[i].flags & PROP_CONF)) return 0;
            if (o->props[i].key) js_free(o->props[i].key);
            for (int j = i; j < o->prop_n - 1; j++) o->props[j] = o->props[j + 1];
            o->prop_n--;
            return 1;
        }
    }
    return 1;
}

/* ==========================================================================
 * 2. GERCEK GERI IZLEMELI (BACKTRACKING) REGEXP MOTORU
 * ========================================================================== */
#define RE_MAX_CAPS 16
typedef struct {
    const char *pat;
    int plen;
    const char *str;
    int slen;
    int fl_i, fl_m, fl_s;
    int cap_s[RE_MAX_CAPS];
    int cap_e[RE_MAX_CAPS];
    int ncaps;
    int steps;
} js_re_state_t;

static int re_ch_eq(unsigned char a, unsigned char b, int ci) {
    if (a == b) return 1;
    if (ci) {
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        return a == b;
    }
    return 0;
}
static int re_is_word(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static int re_match_class(const char *p, int plen, int *pi, unsigned char ch, int ci) {
    int i = *pi + 1;
    int neg = 0;
    if (i < plen && p[i] == '^') { neg = 1; i++; }
    int matched = 0;
    int first = 1;
    while (i < plen && (p[i] != ']' || first)) {
        first = 0;
        if (p[i] == '\\' && i + 1 < plen) {
            char e = p[i + 1];
            if (e == 'd') { if (js_is_digit(ch)) matched = 1; i += 2; continue; }
            if (e == 'D') { if (!js_is_digit(ch)) matched = 1; i += 2; continue; }
            if (e == 'w') { if (re_is_word(ch)) matched = 1; i += 2; continue; }
            if (e == 'W') { if (!re_is_word(ch)) matched = 1; i += 2; continue; }
            if (e == 's') { if (js_is_space(ch)) matched = 1; i += 2; continue; }
            if (e == 'S') { if (!js_is_space(ch)) matched = 1; i += 2; continue; }
            if (e == 'n') { if (ch == '\n') matched = 1; i += 2; continue; }
            if (e == 'r') { if (ch == '\r') matched = 1; i += 2; continue; }
            if (e == 't') { if (ch == '\t') matched = 1; i += 2; continue; }
            if (re_ch_eq((unsigned char)e, ch, ci)) matched = 1;
            i += 2;
            continue;
        }
        unsigned char c1 = (unsigned char)p[i];
        if (i + 2 < plen && p[i + 1] == '-' && p[i + 2] != ']') {
            unsigned char c2 = (unsigned char)p[i + 2];
            unsigned char tc = ch;
            if (ci) {
                if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
                if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
                if (tc >= 'A' && tc <= 'Z') tc += 32;
            }
            if (tc >= c1 && tc <= c2) matched = 1;
            i += 3;
        } else {
            if (re_ch_eq(c1, ch, ci)) matched = 1;
            i++;
        }
    }
    if (i < plen && p[i] == ']') i++;
    *pi = i;
    return neg ? !matched : matched;
}

static int re_find_group_end(const char *p, int plen, int open_i) {
    int depth = 1, in_cls = 0;
    for (int i = open_i + 1; i < plen; i++) {
        if (p[i] == '\\') { i++; continue; }
        if (in_cls) { if (p[i] == ']') in_cls = 0; continue; }
        if (p[i] == '[') { in_cls = 1; continue; }
        if (p[i] == '(') depth++;
        else if (p[i] == ')') { depth--; if (depth == 0) return i; }
    }
    return plen;
}

static int re_atom_end(const char *p, int plen, int i) {
    if (i >= plen) return i;
    if (p[i] == '\\') return (i + 2 <= plen) ? i + 2 : plen;
    if (p[i] == '[') {
        int j = i + 1; if (j < plen && p[j] == '^') j++;
        if (j < plen && p[j] == ']') j++;
        while (j < plen && p[j] != ']') { if (p[j] == '\\') j++; j++; }
        return (j < plen) ? j + 1 : plen;
    }
    if (p[i] == '(') {
        int ge = re_find_group_end(p, plen, i);
        return (ge < plen) ? ge + 1 : plen;
    }
    return i + 1;
}

static int re_parse_quant(const char *p, int plen, int i, int *qmin, int *qmax, int *lazy) {
    *qmin = 1; *qmax = 1; *lazy = 0;
    if (i >= plen) return i;
    if (p[i] == '*') { *qmin = 0; *qmax = 100000; i++; }
    else if (p[i] == '+') { *qmin = 1; *qmax = 100000; i++; }
    else if (p[i] == '?') { *qmin = 0; *qmax = 1; i++; }
    else if (p[i] == '{') {
        int j = i + 1, a = 0, b = -1, has_a = 0, has_comma = 0;
        while (j < plen && js_is_digit((unsigned char)p[j])) { a = a * 10 + (p[j++] - '0'); has_a = 1; }
        if (j < plen && p[j] == ',') {
            has_comma = 1; j++; b = 0; int has_b = 0;
            while (j < plen && js_is_digit((unsigned char)p[j])) { b = b * 10 + (p[j++] - '0'); has_b = 1; }
            if (!has_b) b = 100000;
        }
        if (has_a && j < plen && p[j] == '}') {
            *qmin = a; *qmax = has_comma ? b : a; i = j + 1;
        } else return i;
    } else return i;
    if (i < plen && p[i] == '?') { *lazy = 1; i++; }
    return i;
}

static int re_match_seq(js_re_state_t *st, int ps, int pe, int sp, int *out_sp);

static int re_match_alt(js_re_state_t *st, int ps, int pe, int sp, int *out_sp) {
    /* Split top-level '|' inside [ps, pe) */
    int bstart = ps, depth = 0, in_cls = 0;
    for (int i = ps; i <= pe; i++) {
        int at_bar = 0;
        if (i == pe) at_bar = 1;
        else if (st->pat[i] == '\\') { i++; continue; }
        else if (in_cls) { if (st->pat[i] == ']') in_cls = 0; continue; }
        else if (st->pat[i] == '[') { in_cls = 1; continue; }
        else if (st->pat[i] == '(') depth++;
        else if (st->pat[i] == ')') depth--;
        else if (st->pat[i] == '|' && depth == 0) at_bar = 1;
        if (at_bar) {
            int save_s[RE_MAX_CAPS], save_e[RE_MAX_CAPS];
            js_memcpy(save_s, st->cap_s, sizeof(save_s));
            js_memcpy(save_e, st->cap_e, sizeof(save_e));
            int nsp = sp;
            if (re_match_seq(st, bstart, i, sp, &nsp)) { *out_sp = nsp; return 1; }
            js_memcpy(st->cap_s, save_s, sizeof(save_s));
            js_memcpy(st->cap_e, save_e, sizeof(save_e));
            bstart = i + 1;
        }
    }
    return 0;
}

static int re_count_group_idx(const char *p, int open_i) {
    int idx = 1, in_cls = 0;
    for (int i = 0; i < open_i; i++) {
        if (p[i] == '\\') { i++; continue; }
        if (in_cls) { if (p[i] == ']') in_cls = 0; continue; }
        if (p[i] == '[') { in_cls = 1; continue; }
        if (p[i] == '(' && !(p[i + 1] == '?')) idx++;
    }
    return idx;
}

static int re_match_atom(js_re_state_t *st, int as, int ae, int sp, int *out_sp) {
    if (st->steps++ > 80000) return 0;
    char c = st->pat[as];
    if (c == '^') {
        if (sp == 0 || (st->fl_m && sp > 0 && st->str[sp - 1] == '\n')) { *out_sp = sp; return 1; }
        return 0;
    }
    if (c == '$') {
        if (sp == st->slen || (st->fl_m && st->str[sp] == '\n')) { *out_sp = sp; return 1; }
        return 0;
    }
    if (c == '.') {
        if (sp < st->slen && (st->fl_s || (st->str[sp] != '\n' && st->str[sp] != '\r'))) { *out_sp = sp + 1; return 1; }
        return 0;
    }
    if (c == '[') {
        if (sp >= st->slen) return 0;
        int ti = as;
        if (re_match_class(st->pat, st->plen, &ti, (unsigned char)st->str[sp], st->fl_i)) { *out_sp = sp + 1; return 1; }
        return 0;
    }
    if (c == '\\' && as + 1 < ae) {
        char e = st->pat[as + 1];
        if (e == 'b' || e == 'B') {
            int l = (sp > 0) ? re_is_word((unsigned char)st->str[sp - 1]) : 0;
            int r = (sp < st->slen) ? re_is_word((unsigned char)st->str[sp]) : 0;
            int b = (l != r);
            if ((e == 'b') ? b : !b) { *out_sp = sp; return 1; }
            return 0;
        }
        if (e >= '1' && e <= '9') {
            int gi = e - '0';
            if (gi < RE_MAX_CAPS && st->cap_s[gi] >= 0 && st->cap_e[gi] >= st->cap_s[gi]) {
                int gl = st->cap_e[gi] - st->cap_s[gi];
                if (sp + gl <= st->slen) {
                    int ok = 1;
                    for (int k = 0; k < gl; k++)
                        if (!re_ch_eq((unsigned char)st->str[st->cap_s[gi] + k], (unsigned char)st->str[sp + k], st->fl_i)) { ok = 0; break; }
                    if (ok) { *out_sp = sp + gl; return 1; }
                }
            }
            return 0;
        }
        if (sp >= st->slen) return 0;
        unsigned char ch = (unsigned char)st->str[sp];
        int ok = 0;
        if (e == 'd') ok = js_is_digit(ch);
        else if (e == 'D') ok = !js_is_digit(ch);
        else if (e == 'w') ok = re_is_word(ch);
        else if (e == 'W') ok = !re_is_word(ch);
        else if (e == 's') ok = js_is_space(ch);
        else if (e == 'S') ok = !js_is_space(ch);
        else if (e == 'n') ok = (ch == '\n');
        else if (e == 'r') ok = (ch == '\r');
        else if (e == 't') ok = (ch == '\t');
        else ok = re_ch_eq((unsigned char)e, ch, st->fl_i);
        if (ok) { *out_sp = sp + 1; return 1; }
        return 0;
    }
    if (c == '(') {
        int inner_s = as + 1, inner_e = ae - 1;
        if (inner_e < inner_s) inner_e = inner_s;
        if (inner_s + 1 < inner_e && st->pat[inner_s] == '?') {
            char kind = st->pat[inner_s + 1];
            if (kind == ':') return re_match_alt(st, inner_s + 2, inner_e, sp, out_sp);
            if (kind == '=' || kind == '!') {
                int tmp = sp;
                int m = re_match_alt(st, inner_s + 2, inner_e, sp, &tmp);
                if ((kind == '=') ? m : !m) { *out_sp = sp; return 1; }
                return 0;
            }
        }
        int gi = re_count_group_idx(st->pat, as);
        int nsp = sp;
        if (re_match_alt(st, inner_s, inner_e, sp, &nsp)) {
            if (gi < RE_MAX_CAPS) { st->cap_s[gi] = sp; st->cap_e[gi] = nsp; if (gi + 1 > st->ncaps) st->ncaps = gi + 1; }
            *out_sp = nsp;
            return 1;
        }
        return 0;
    }
    if (sp < st->slen && re_ch_eq((unsigned char)c, (unsigned char)st->str[sp], st->fl_i)) {
        *out_sp = sp + 1; return 1;
    }
    return 0;
}

static int re_match_seq(js_re_state_t *st, int ps, int pe, int sp, int *out_sp) {
    if (st->steps++ > 80000) return 0;
    if (ps >= pe) { *out_sp = sp; return 1; }
    int ae = re_atom_end(st->pat, pe, ps);
    int qmin, qmax, lazy;
    int next_p = re_parse_quant(st->pat, pe, ae, &qmin, &qmax, &lazy);
    if (qmin == 1 && qmax == 1) {
        int nsp = sp;
        if (!re_match_atom(st, ps, ae, sp, &nsp)) return 0;
        return re_match_seq(st, next_p, pe, nsp, out_sp);
    }
    /* Match between qmin..qmax repetitions */
    int positions[512];
    int npos = 0;
    positions[npos++] = sp;
    int cur = sp;
    for (int k = 0; k < qmax && npos < 511; k++) {
        int nsp = cur;
        if (!re_match_atom(st, ps, ae, cur, &nsp)) break;
        if (nsp == cur && k >= qmin) break; /* prevent infinite zero-width loop */
        cur = nsp;
        positions[npos++] = cur;
    }
    if (lazy) {
        for (int k = qmin; k < npos; k++) {
            int res = positions[k];
            if (re_match_seq(st, next_p, pe, positions[k], &res)) { *out_sp = res; return 1; }
        }
    } else {
        for (int k = npos - 1; k >= qmin; k--) {
            int res = positions[k];
            if (re_match_seq(st, next_p, pe, positions[k], &res)) { *out_sp = res; return 1; }
        }
    }
    return 0;
}

static int js_re_exec_at(const char *pat, const char *flags, const char *str, int slen, int start,
                         int cap_s[RE_MAX_CAPS], int cap_e[RE_MAX_CAPS], int *ncaps_out) {
    js_re_state_t st;
    js_memset(&st, 0, sizeof(st));
    st.pat = pat ? pat : "";
    st.plen = (int)js_strlen(st.pat);
    st.str = str ? str : "";
    st.slen = slen;
    for (int i = 0; flags && flags[i]; i++) {
        if (flags[i] == 'i') st.fl_i = 1;
        if (flags[i] == 'm') st.fl_m = 1;
        if (flags[i] == 's') st.fl_s = 1;
    }
    if (start < 0) start = 0;
    for (int sp = start; sp <= slen; sp++) {
        for (int k = 0; k < RE_MAX_CAPS; k++) { st.cap_s[k] = -1; st.cap_e[k] = -1; }
        st.ncaps = 1;
        st.steps = 0;
        int endp = sp;
        if (re_match_alt(&st, 0, st.plen, sp, &endp)) {
            st.cap_s[0] = sp; st.cap_e[0] = endp;
            if (cap_s) js_memcpy(cap_s, st.cap_s, sizeof(st.cap_s));
            if (cap_e) js_memcpy(cap_e, st.cap_e, sizeof(st.cap_e));
            if (ncaps_out) *ncaps_out = st.ncaps;
            return 1;
        }
    }
    return 0;
}

/* ==========================================================================
 * 3. ES6-ES2024 LEXER, PARSER VE AST
 * ========================================================================== */

typedef enum {
    TK_EOF = 0, TK_NUM, TK_BIGINT, TK_STR, TK_TMPL, TK_REGEX, TK_ID,
    /* Anahtar kelimeler */
    TK_VAR, TK_LET, TK_CONST, TK_FUNC, TK_RETURN, TK_IF, TK_ELSE,
    TK_WHILE, TK_DO, TK_FOR, TK_IN, TK_OF, TK_BREAK, TK_CONT,
    TK_SWITCH, TK_CASE, TK_DEFAULT, TK_TRY, TK_CATCH, TK_FINALLY, TK_THROW,
    TK_NEW, TK_THIS, TK_SUPER, TK_CLASS, TK_EXTENDS, TK_STATIC,
    TK_TYPEOF, TK_VOID, TK_DELETE, TK_INSTANCEOF,
    TK_TRUE, TK_FALSE, TK_NULL, TK_ASYNC, TK_AWAIT, TK_YIELD,
    /* Noktalama & Operatorler */
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE, TK_LBRACK, TK_RBRACK,
    TK_SEMI, TK_COMMA, TK_DOT, TK_ELLIPSIS, TK_QMARK, TK_QDOT, TK_COLON,
    TK_ARROW, TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT, TK_POW,
    TK_INC, TK_DEC, TK_NOT, TK_BNOT,
    TK_SHL, TK_SHR, TK_USHR,
    TK_LT, TK_LE, TK_GT, TK_GE,
    TK_EQ, TK_NE, TK_SEQ, TK_SNE,
    TK_BAND, TK_BXOR, TK_BOR, TK_AND, TK_OR, TK_NULLISH,
    TK_ASSIGN, TK_ADD_EQ, TK_SUB_EQ, TK_MUL_EQ, TK_DIV_EQ, TK_MOD_EQ, TK_POW_EQ,
    TK_SHL_EQ, TK_SHR_EQ, TK_USHR_EQ, TK_AND_EQ, TK_XOR_EQ, TK_OR_EQ,
    TK_LAND_EQ, TK_LOR_EQ, TK_NULLISH_EQ
} js_tok_kind_t;

typedef struct {
    js_tok_kind_t kind;
    const char   *pos;
    int           len;
    double        num;
    int64_t       bi;
    js_str_t     *str;
    char          re_flags[8];
    int           had_nl;
} js_tok_t;

typedef struct {
    const char *src;
    int         len;
    int         pos;
    js_tok_t    tok;
    js_tok_t    peek;
    int         has_peek;
    int         allow_re;
    int         err;
    char        err_msg[128];
} js_parser_t;

typedef enum {
    AST_PROG = 1, AST_BLOCK, AST_EMPTY, AST_EXPR_STMT,
    AST_VAR, AST_VAR_DECL,
    AST_IF, AST_WHILE, AST_DO_WHILE, AST_FOR, AST_FOR_IN, AST_FOR_OF,
    AST_SWITCH, AST_CASE, AST_BREAK, AST_CONT, AST_RETURN, AST_THROW, AST_TRY, AST_LABEL,
    AST_FUNC, AST_CLASS, AST_METHOD, AST_FIELD,
    AST_NUM, AST_BIGINT, AST_STR, AST_BOOL, AST_NULL, AST_REGEX, AST_TMPL,
    AST_ID, AST_THIS, AST_SUPER,
    AST_ARR, AST_OBJ, AST_PROP, AST_SPREAD,
    AST_MEMBER, AST_CALL, AST_NEW,
    AST_UNARY, AST_UPDATE, AST_BINARY, AST_LOGICAL, AST_ASSIGN, AST_COND, AST_SEQ,
    AST_AWAIT, AST_YIELD,
    AST_PAT_ARR, AST_PAT_OBJ, AST_PAT_ASSIGN, AST_PAT_REST
} js_ast_kind_t;

struct js_ast {
    js_ast_kind_t kind;
    int           op;         /* operator token kind or var kind (0=var,1=let,2=const) */
    int           flags;      /* optional/computed/prefix/async/gen/arrow/static/get=1/set=2/delegate */
    double        num;
    int64_t       bi;
    js_str_t     *str;        /* id name / string / regex pattern / label */
    char          extra[16];  /* regex flags / label */
    js_ast_t     *a, *b, *c, *d;
    js_ast_t    **list;
    int           list_n;
    int           list_cap;
};

static js_ast_t *ast_new(js_ast_kind_t k) {
    js_ast_t *n = (js_ast_t *)js_calloc(1, sizeof(js_ast_t));
    if (n) n->kind = k;
    return n;
}
static void ast_push(js_ast_t *parent, js_ast_t *child) {
    if (!parent) return;
    if (parent->list_n >= parent->list_cap) {
        int nc = parent->list_cap ? parent->list_cap * 2 : 4;
        js_ast_t **nl = (js_ast_t **)js_realloc(parent->list, (size_t)nc * sizeof(js_ast_t *));
        if (!nl) return;
        parent->list = nl; parent->list_cap = nc;
    }
    parent->list[parent->list_n++] = child;
}

static void jp_err(js_parser_t *p, const char *m) {
    if (!p->err) { p->err = 1; bcopy_str(p->err_msg, m, sizeof(p->err_msg)); }
}

static int jp_skip_ws(js_parser_t *p) {
    int nl = 0;
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\v' || c == '\f') { p->pos++; continue; }
        if (c == '\n' || c == '\r') { nl = 1; p->pos++; continue; }
        if (c == '/' && p->pos + 1 < p->len && p->src[p->pos + 1] == '/') {
            p->pos += 2;
            while (p->pos < p->len && p->src[p->pos] != '\n' && p->src[p->pos] != '\r') p->pos++;
            continue;
        }
        if (c == '/' && p->pos + 1 < p->len && p->src[p->pos + 1] == '*') {
            p->pos += 2;
            while (p->pos + 1 < p->len && !(p->src[p->pos] == '*' && p->src[p->pos + 1] == '/')) {
                if (p->src[p->pos] == '\n' || p->src[p->pos] == '\r') nl = 1;
                p->pos++;
            }
            if (p->pos + 1 < p->len) p->pos += 2;
            continue;
        }
        break;
    }
    return nl;
}

static void jp_encode_utf8(js_sb_t *sb, uint32_t cp) {
    if (cp < 0x80) js_sb_putc(sb, (char)cp);
    else if (cp < 0x800) {
        js_sb_putc(sb, (char)(0xC0 | (cp >> 6)));
        js_sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        js_sb_putc(sb, (char)(0xE0 | (cp >> 12)));
        js_sb_putc(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        js_sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        js_sb_putc(sb, (char)(0xF0 | (cp >> 18)));
        js_sb_putc(sb, (char)(0x80 | ((cp >> 12) & 0x3F)));
        js_sb_putc(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        js_sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    }
}

static void jp_read_escape(js_parser_t *p, js_sb_t *sb) {
    if (p->pos >= p->len) return;
    char e = p->src[p->pos++];
    switch (e) {
        case 'n': js_sb_putc(sb, '\n'); break;
        case 'r': js_sb_putc(sb, '\r'); break;
        case 't': js_sb_putc(sb, '\t'); break;
        case 'b': js_sb_putc(sb, '\b'); break;
        case 'f': js_sb_putc(sb, '\f'); break;
        case 'v': js_sb_putc(sb, '\v'); break;
        case '0': js_sb_putc(sb, '\0'); break;
        case '\\': case '\'': case '"': case '`': case '$': js_sb_putc(sb, e); break;
        case '\n': break;
        case '\r': if (p->pos < p->len && p->src[p->pos] == '\n') p->pos++; break;
        case 'x': {
            if (p->pos + 1 < p->len) {
                int h1 = js_hex_val((unsigned char)p->src[p->pos]), h2 = js_hex_val((unsigned char)p->src[p->pos + 1]);
                if (h1 >= 0 && h2 >= 0) { jp_encode_utf8(sb, (uint32_t)((h1 << 4) | h2)); p->pos += 2; break; }
            }
            js_sb_putc(sb, 'x'); break;
        }
        case 'u': {
            if (p->pos < p->len && p->src[p->pos] == '{') {
                p->pos++; uint32_t cp = 0;
                while (p->pos < p->len && p->src[p->pos] != '}') {
                    int h = js_hex_val((unsigned char)p->src[p->pos++]);
                    if (h >= 0) cp = (cp << 4) | (uint32_t)h;
                }
                if (p->pos < p->len && p->src[p->pos] == '}') p->pos++;
                jp_encode_utf8(sb, cp);
            } else if (p->pos + 3 < p->len) {
                uint32_t cp = 0; int ok = 1;
                for (int k = 0; k < 4; k++) {
                    int h = js_hex_val((unsigned char)p->src[p->pos + k]);
                    if (h < 0) { ok = 0; break; }
                    cp = (cp << 4) | (uint32_t)h;
                }
                if (ok) { p->pos += 4; jp_encode_utf8(sb, cp); }
                else js_sb_putc(sb, 'u');
            } else js_sb_putc(sb, 'u');
            break;
        }
        default: js_sb_putc(sb, e); break;
    }
}

static js_tok_kind_t jp_kw(const char *s, int n) {
    #define KW(lit, tk) if (n == (int)(sizeof(lit)-1) && js_memcmp(s, lit, (size_t)n) == 0) return tk
    KW("var", TK_VAR); KW("let", TK_LET); KW("const", TK_CONST);
    KW("function", TK_FUNC); KW("return", TK_RETURN);
    KW("if", TK_IF); KW("else", TK_ELSE);
    KW("while", TK_WHILE); KW("do", TK_DO); KW("for", TK_FOR);
    KW("in", TK_IN); KW("of", TK_OF);
    KW("break", TK_BREAK); KW("continue", TK_CONT);
    KW("switch", TK_SWITCH); KW("case", TK_CASE); KW("default", TK_DEFAULT);
    KW("try", TK_TRY); KW("catch", TK_CATCH); KW("finally", TK_FINALLY); KW("throw", TK_THROW);
    KW("new", TK_NEW); KW("this", TK_THIS); KW("super", TK_SUPER);
    KW("class", TK_CLASS); KW("extends", TK_EXTENDS); KW("static", TK_STATIC);
    KW("typeof", TK_TYPEOF); KW("void", TK_VOID); KW("delete", TK_DELETE);
    KW("instanceof", TK_INSTANCEOF);
    KW("true", TK_TRUE); KW("false", TK_FALSE); KW("null", TK_NULL);
    KW("async", TK_ASYNC); KW("await", TK_AWAIT); KW("yield", TK_YIELD);
    #undef KW
    return TK_ID;
}

static int jp_tok_allows_re(js_tok_kind_t k) {
    switch (k) {
        case TK_NUM: case TK_BIGINT: case TK_STR: case TK_TMPL: case TK_REGEX:
        case TK_ID: case TK_THIS: case TK_SUPER: case TK_TRUE: case TK_FALSE: case TK_NULL:
        case TK_RPAREN: case TK_RBRACK: case TK_INC: case TK_DEC:
            return 0;
        default: return 1;
    }
}

static js_tok_t jp_lex_one(js_parser_t *p, int allow_re) {
    js_tok_t t;
    js_memset(&t, 0, sizeof(t));
    t.had_nl = jp_skip_ws(p);
    t.pos = p->src + p->pos;
    if (p->pos >= p->len) { t.kind = TK_EOF; return t; }

    char c = p->src[p->pos];

    /* String */
    if (c == '\'' || c == '"') {
        char q = c; p->pos++;
        js_sb_t sb; js_sb_init(&sb);
        while (p->pos < p->len && p->src[p->pos] != q) {
            if (p->src[p->pos] == '\\') { p->pos++; jp_read_escape(p, &sb); }
            else js_sb_putc(&sb, p->src[p->pos++]);
        }
        if (p->pos < p->len && p->src[p->pos] == q) p->pos++;
        t.kind = TK_STR; t.str = js_sb_finish(&sb);
        t.len = (int)((p->src + p->pos) - t.pos);
        return t;
    }

    /* Template literal raw capture (parsed by jp_parse_template) */
    if (c == '`') {
        int start = ++p->pos;
        int depth = 0;
        while (p->pos < p->len) {
            char ch = p->src[p->pos];
            if (ch == '\\') { p->pos += 2; continue; }
            if (depth == 0) {
                if (ch == '`') break;
                if (ch == '$' && p->pos + 1 < p->len && p->src[p->pos + 1] == '{') {
                    depth = 1; p->pos += 2; continue;
                }
            } else {
                if (ch == '\'' || ch == '"') {
                    char q = ch; p->pos++;
                    while (p->pos < p->len && p->src[p->pos] != q) {
                        if (p->src[p->pos] == '\\') p->pos++;
                        if (p->pos < p->len) p->pos++;
                    }
                    if (p->pos < p->len) p->pos++;
                    continue;
                }
                if (ch == '`') {
                    p->pos++;
                    while (p->pos < p->len && p->src[p->pos] != '`') {
                        if (p->src[p->pos] == '\\') p->pos++;
                        if (p->pos < p->len) p->pos++;
                    }
                    if (p->pos < p->len) p->pos++;
                    continue;
                }
                if (ch == '{') depth++;
                else if (ch == '}') depth--;
            }
            p->pos++;
        }
        int end = p->pos;
        if (p->pos < p->len && p->src[p->pos] == '`') p->pos++;
        t.kind = TK_TMPL;
        t.str = js_str_new(p->src + start, end - start);
        t.len = (int)((p->src + p->pos) - t.pos);
        return t;
    }

    /* Number / BigInt */
    if (js_is_digit((unsigned char)c) || (c == '.' && p->pos + 1 < p->len && js_is_digit((unsigned char)p->src[p->pos + 1]))) {
        char nb[128]; int nn = 0;
        if (c == '0' && p->pos + 1 < p->len && (p->src[p->pos + 1] == 'x' || p->src[p->pos + 1] == 'X' ||
            p->src[p->pos + 1] == 'b' || p->src[p->pos + 1] == 'B' ||
            p->src[p->pos + 1] == 'o' || p->src[p->pos + 1] == 'O')) {
            nb[nn++] = p->src[p->pos++];
            nb[nn++] = p->src[p->pos++];
            while (p->pos < p->len && (js_is_hex((unsigned char)p->src[p->pos]) || p->src[p->pos] == '_')) {
                char ch = p->src[p->pos++];
                if (ch != '_' && nn < 120) nb[nn++] = ch;
            }
        } else {
            while (p->pos < p->len && (js_is_digit((unsigned char)p->src[p->pos]) || p->src[p->pos] == '_')) {
                char ch = p->src[p->pos++];
                if (ch != '_' && nn < 120) nb[nn++] = ch;
            }
            if (p->pos < p->len && p->src[p->pos] == '.' && !(p->pos + 1 < p->len && p->src[p->pos + 1] == '.')) {
                if (nn < 120) nb[nn++] = p->src[p->pos++];
                while (p->pos < p->len && (js_is_digit((unsigned char)p->src[p->pos]) || p->src[p->pos] == '_')) {
                    char ch = p->src[p->pos++];
                    if (ch != '_' && nn < 120) nb[nn++] = ch;
                }
            }
            if (p->pos < p->len && (p->src[p->pos] == 'e' || p->src[p->pos] == 'E')) {
                if (nn < 120) nb[nn++] = p->src[p->pos++];
                if (p->pos < p->len && (p->src[p->pos] == '+' || p->src[p->pos] == '-'))
                    if (nn < 120) nb[nn++] = p->src[p->pos++];
                while (p->pos < p->len && (js_is_digit((unsigned char)p->src[p->pos]) || p->src[p->pos] == '_')) {
                    char ch = p->src[p->pos++];
                    if (ch != '_' && nn < 120) nb[nn++] = ch;
                }
            }
        }
        nb[nn] = 0;
        if (p->pos < p->len && p->src[p->pos] == 'n') {
            p->pos++;
            t.kind = TK_BIGINT;
            int64_t bi = 0;
            if (nn > 2 && nb[0] == '0' && (nb[1] == 'x' || nb[1] == 'X')) {
                for (int k = 2; k < nn; k++) bi = (bi << 4) | js_hex_val((unsigned char)nb[k]);
            } else if (nn > 2 && nb[0] == '0' && (nb[1] == 'b' || nb[1] == 'B')) {
                for (int k = 2; k < nn; k++) bi = (bi << 1) | (nb[k] - '0');
            } else {
                for (int k = 0; k < nn; k++) if (js_is_digit((unsigned char)nb[k])) bi = bi * 10 + (nb[k] - '0');
            }
            t.bi = bi;
        } else {
            t.kind = TK_NUM;
            t.num = js_str_to_num(nb, nn);
        }
        t.len = (int)((p->src + p->pos) - t.pos);
        return t;
    }

    /* Identifier / Private #id / Keyword */
    if (js_is_id_start((unsigned char)c) || (c == '#' && p->pos + 1 < p->len && js_is_id_start((unsigned char)p->src[p->pos + 1]))) {
        int s = p->pos++;
        while (p->pos < p->len && js_is_id_part((unsigned char)p->src[p->pos])) p->pos++;
        t.len = p->pos - s;
        t.kind = (c == '#') ? TK_ID : jp_kw(p->src + s, t.len);
        t.str = js_str_new(p->src + s, t.len);
        return t;
    }

    /* RegExp literal when allowed */
    if (c == '/' && allow_re) {
        int s = ++p->pos, in_cls = 0;
        while (p->pos < p->len) {
            char ch = p->src[p->pos];
            if (ch == '\\') { p->pos += 2; continue; }
            if (in_cls) { if (ch == ']') in_cls = 0; p->pos++; continue; }
            if (ch == '[') { in_cls = 1; p->pos++; continue; }
            if (ch == '/' || ch == '\n') break;
            p->pos++;
        }
        int e = p->pos;
        if (p->pos < p->len && p->src[p->pos] == '/') p->pos++;
        int fn = 0;
        while (p->pos < p->len && ((p->src[p->pos] >= 'a' && p->src[p->pos] <= 'z') || (p->src[p->pos] >= 'A' && p->src[p->pos] <= 'Z'))) {
            if (fn < 7) t.re_flags[fn++] = p->src[p->pos];
            p->pos++;
        }
        t.re_flags[fn] = 0;
        t.kind = TK_REGEX;
        t.str = js_str_new(p->src + s, e - s);
        t.len = (int)((p->src + p->pos) - t.pos);
        return t;
    }

    /* Multi-character operators */
    char c1 = (p->pos + 1 < p->len) ? p->src[p->pos + 1] : 0;
    char c2 = (p->pos + 2 < p->len) ? p->src[p->pos + 2] : 0;
    char c3 = (p->pos + 3 < p->len) ? p->src[p->pos + 3] : 0;

    if (c == '>' && c1 == '>' && c2 == '>' && c3 == '=') { p->pos += 4; t.kind = TK_USHR_EQ; t.len = 4; return t; }
    if (c == '=' && c1 == '=' && c2 == '=') { p->pos += 3; t.kind = TK_SEQ; t.len = 3; return t; }
    if (c == '!' && c1 == '=' && c2 == '=') { p->pos += 3; t.kind = TK_SNE; t.len = 3; return t; }
    if (c == '>' && c1 == '>' && c2 == '>') { p->pos += 3; t.kind = TK_USHR; t.len = 3; return t; }
    if (c == '.' && c1 == '.' && c2 == '.') { p->pos += 3; t.kind = TK_ELLIPSIS; t.len = 3; return t; }
    if (c == '*' && c1 == '*' && c2 == '=') { p->pos += 3; t.kind = TK_POW_EQ; t.len = 3; return t; }
    if (c == '<' && c1 == '<' && c2 == '=') { p->pos += 3; t.kind = TK_SHL_EQ; t.len = 3; return t; }
    if (c == '>' && c1 == '>' && c2 == '=') { p->pos += 3; t.kind = TK_SHR_EQ; t.len = 3; return t; }
    if (c == '&' && c1 == '&' && c2 == '=') { p->pos += 3; t.kind = TK_LAND_EQ; t.len = 3; return t; }
    if (c == '|' && c1 == '|' && c2 == '=') { p->pos += 3; t.kind = TK_LOR_EQ; t.len = 3; return t; }
    if (c == '?' && c1 == '?' && c2 == '=') { p->pos += 3; t.kind = TK_NULLISH_EQ; t.len = 3; return t; }

    if (c == '=' && c1 == '>') { p->pos += 2; t.kind = TK_ARROW; t.len = 2; return t; }
    if (c == '=' && c1 == '=') { p->pos += 2; t.kind = TK_EQ; t.len = 2; return t; }
    if (c == '!' && c1 == '=') { p->pos += 2; t.kind = TK_NE; t.len = 2; return t; }
    if (c == '<' && c1 == '=') { p->pos += 2; t.kind = TK_LE; t.len = 2; return t; }
    if (c == '>' && c1 == '=') { p->pos += 2; t.kind = TK_GE; t.len = 2; return t; }
    if (c == '<' && c1 == '<') { p->pos += 2; t.kind = TK_SHL; t.len = 2; return t; }
    if (c == '>' && c1 == '>') { p->pos += 2; t.kind = TK_SHR; t.len = 2; return t; }
    if (c == '&' && c1 == '&') { p->pos += 2; t.kind = TK_AND; t.len = 2; return t; }
    if (c == '|' && c1 == '|') { p->pos += 2; t.kind = TK_OR; t.len = 2; return t; }
    if (c == '?' && c1 == '?') { p->pos += 2; t.kind = TK_NULLISH; t.len = 2; return t; }
    if (c == '?' && c1 == '.' && !js_is_digit((unsigned char)c2)) { p->pos += 2; t.kind = TK_QDOT; t.len = 2; return t; }
    if (c == '+' && c1 == '+') { p->pos += 2; t.kind = TK_INC; t.len = 2; return t; }
    if (c == '-' && c1 == '-') { p->pos += 2; t.kind = TK_DEC; t.len = 2; return t; }
    if (c == '*' && c1 == '*') { p->pos += 2; t.kind = TK_POW; t.len = 2; return t; }
    if (c == '+' && c1 == '=') { p->pos += 2; t.kind = TK_ADD_EQ; t.len = 2; return t; }
    if (c == '-' && c1 == '=') { p->pos += 2; t.kind = TK_SUB_EQ; t.len = 2; return t; }
    if (c == '*' && c1 == '=') { p->pos += 2; t.kind = TK_MUL_EQ; t.len = 2; return t; }
    if (c == '/' && c1 == '=') { p->pos += 2; t.kind = TK_DIV_EQ; t.len = 2; return t; }
    if (c == '%' && c1 == '=') { p->pos += 2; t.kind = TK_MOD_EQ; t.len = 2; return t; }
    if (c == '&' && c1 == '=') { p->pos += 2; t.kind = TK_AND_EQ; t.len = 2; return t; }
    if (c == '^' && c1 == '=') { p->pos += 2; t.kind = TK_XOR_EQ; t.len = 2; return t; }
    if (c == '|' && c1 == '=') { p->pos += 2; t.kind = TK_OR_EQ; t.len = 2; return t; }

    p->pos++; t.len = 1;
    switch (c) {
        case '(': t.kind = TK_LPAREN; break; case ')': t.kind = TK_RPAREN; break;
        case '{': t.kind = TK_LBRACE; break; case '}': t.kind = TK_RBRACE; break;
        case '[': t.kind = TK_LBRACK; break; case ']': t.kind = TK_RBRACK; break;
        case ';': t.kind = TK_SEMI; break;   case ',': t.kind = TK_COMMA; break;
        case '.': t.kind = TK_DOT; break;    case '?': t.kind = TK_QMARK; break;
        case ':': t.kind = TK_COLON; break;  case '+': t.kind = TK_PLUS; break;
        case '-': t.kind = TK_MINUS; break;  case '*': t.kind = TK_STAR; break;
        case '/': t.kind = TK_SLASH; break;  case '%': t.kind = TK_PERCENT; break;
        case '!': t.kind = TK_NOT; break;    case '~': t.kind = TK_BNOT; break;
        case '<': t.kind = TK_LT; break;     case '>': t.kind = TK_GT; break;
        case '&': t.kind = TK_BAND; break;   case '^': t.kind = TK_BXOR; break;
        case '|': t.kind = TK_BOR; break;    case '=': t.kind = TK_ASSIGN; break;
        default: t.kind = TK_EOF; break;
    }
    return t;
}

static void jp_next(js_parser_t *p) {
    if (p->has_peek) {
        p->tok = p->peek;
        p->has_peek = 0;
        p->allow_re = jp_tok_allows_re(p->tok.kind);
        return;
    }
    p->tok = jp_lex_one(p, p->allow_re);
    p->allow_re = jp_tok_allows_re(p->tok.kind);
}

static js_tok_t jp_peek_tok(js_parser_t *p) {
    if (!p->has_peek) {
        p->peek = jp_lex_one(p, p->allow_re);
        p->has_peek = 1;
    }
    return p->peek;
}

static int jp_eat(js_parser_t *p, js_tok_kind_t k) {
    if (p->tok.kind == k) { jp_next(p); return 1; }
    return 0;
}

static void jp_expect(js_parser_t *p, js_tok_kind_t k) {
    if (p->tok.kind == k) jp_next(p);
    else jp_err(p, "Unexpected token");
}

static void jp_eat_semi(js_parser_t *p) {
    if (p->tok.kind == TK_SEMI) jp_next(p);
}

static int jp_is_prop_name_tok(js_tok_kind_t k) {
    return k == TK_ID || (k >= TK_VAR && k <= TK_YIELD);
}

static js_str_t *jp_tok_name(js_tok_t *t) {
    if (t->str) return t->str;
    return js_str_new(t->pos, t->len);
}

/* Ileri bildirimler */
static js_ast_t *jp_parse_stmt(js_parser_t *p);
static js_ast_t *jp_parse_expr(js_parser_t *p);
static js_ast_t *jp_parse_assign(js_parser_t *p);
static js_ast_t *jp_parse_pattern(js_parser_t *p);
static js_ast_t *jp_parse_func(js_parser_t *p, int is_decl, int is_async);
static js_ast_t *jp_parse_class(js_parser_t *p, int is_decl);

static js_ast_t *jp_parse_template(js_parser_t *p, js_str_t *raw) {
    js_ast_t *node = ast_new(AST_TMPL);
    const char *s = raw ? raw->data : "";
    int len = raw ? raw->len : 0;
    int i = 0;
    js_sb_t sb; js_sb_init(&sb);
    while (i < len) {
        if (s[i] == '\\' && i + 1 < len) {
            js_parser_t sub; js_memset(&sub, 0, sizeof(sub));
            sub.src = s; sub.len = len; sub.pos = i + 1;
            jp_read_escape(&sub, &sb);
            i = sub.pos;
        } else if (s[i] == '$' && i + 1 < len && s[i + 1] == '{') {
            js_ast_t *lit = ast_new(AST_STR);
            lit->str = js_sb_finish(&sb);
            ast_push(node, lit);
            js_sb_init(&sb);
            i += 2;
            int es = i, depth = 1;
            while (i < len && depth > 0) {
                char ch = s[i];
                if (ch == '\'' || ch == '"') {
                    char q = ch; i++;
                    while (i < len && s[i] != q) { if (s[i] == '\\') i++; i++; }
                    if (i < len) i++;
                    continue;
                }
                if (ch == '`') {
                    i++;
                    while (i < len && s[i] != '`') { if (s[i] == '\\') i++; i++; }
                    if (i < len) i++;
                    continue;
                }
                if (ch == '{') depth++;
                else if (ch == '}') { depth--; if (depth == 0) break; }
                i++;
            }
            js_parser_t ep; js_memset(&ep, 0, sizeof(ep));
            ep.src = s + es; ep.len = i - es; ep.allow_re = 1;
            jp_next(&ep);
            js_ast_t *ex = jp_parse_expr(&ep);
            if (ep.err && !p->err) jp_err(p, ep.err_msg);
            ast_push(node, ex ? ex : ast_new(AST_STR));
            if (i < len && s[i] == '}') i++;
        } else {
            js_sb_putc(&sb, s[i++]);
        }
    }
    js_ast_t *tail = ast_new(AST_STR);
    tail->str = js_sb_finish(&sb);
    ast_push(node, tail);
    return node;
}

static js_ast_t *jp_parse_params(js_parser_t *p) {
    js_ast_t *params = ast_new(AST_ARR);
    jp_expect(p, TK_LPAREN);
    while (p->tok.kind != TK_RPAREN && p->tok.kind != TK_EOF && !p->err) {
        if (jp_eat(p, TK_ELLIPSIS)) {
            js_ast_t *r = ast_new(AST_PAT_REST);
            r->a = jp_parse_pattern(p);
            ast_push(params, r);
            break;
        }
        js_ast_t *pat = jp_parse_pattern(p);
        if (jp_eat(p, TK_ASSIGN)) {
            js_ast_t *ap = ast_new(AST_PAT_ASSIGN);
            ap->a = pat;
            ap->b = jp_parse_assign(p);
            pat = ap;
        }
        ast_push(params, pat);
        if (!jp_eat(p, TK_COMMA)) break;
    }
    jp_expect(p, TK_RPAREN);
    return params;
}

static js_ast_t *jp_parse_block(js_parser_t *p) {
    js_ast_t *blk = ast_new(AST_BLOCK);
    jp_expect(p, TK_LBRACE);
    while (p->tok.kind != TK_RBRACE && p->tok.kind != TK_EOF && !p->err) {
        js_ast_t *st = jp_parse_stmt(p);
        if (st) ast_push(blk, st);
    }
    jp_expect(p, TK_RBRACE);
    return blk;
}

static js_ast_t *jp_parse_func(js_parser_t *p, int is_decl, int is_async) {
    js_ast_t *fn = ast_new(AST_FUNC);
    if (is_async) fn->flags |= 1; /* async */
    jp_expect(p, TK_FUNC);
    if (jp_eat(p, TK_STAR)) fn->flags |= 2; /* generator */
    if (jp_is_prop_name_tok(p->tok.kind) && p->tok.kind != TK_LPAREN) {
        fn->str = jp_tok_name(&p->tok);
        jp_next(p);
    } else if (is_decl) {
        fn->str = js_str_new("", 0);
    }
    fn->a = jp_parse_params(p);
    fn->b = jp_parse_block(p);
    return fn;
}

static js_ast_t *jp_parse_class(js_parser_t *p, int is_decl) {
    (void)is_decl;
    js_ast_t *cls = ast_new(AST_CLASS);
    jp_expect(p, TK_CLASS);
    if (p->tok.kind == TK_ID) {
        cls->str = p->tok.str;
        jp_next(p);
    }
    if (jp_eat(p, TK_EXTENDS)) {
        cls->a = jp_parse_assign(p);
    }
    jp_expect(p, TK_LBRACE);
    while (p->tok.kind != TK_RBRACE && p->tok.kind != TK_EOF && !p->err) {
        if (jp_eat(p, TK_SEMI)) continue;
        int is_static = 0, acc = 0, is_async = 0, is_gen = 0, computed = 0;
        if (p->tok.kind == TK_STATIC) {
            js_tok_t pk = jp_peek_tok(p);
            if (pk.kind != TK_LPAREN && pk.kind != TK_ASSIGN && pk.kind != TK_SEMI) {
                is_static = 1; jp_next(p);
            }
        }
        if (p->tok.kind == TK_ASYNC && jp_peek_tok(p).kind != TK_LPAREN) { is_async = 1; jp_next(p); }
        if (p->tok.kind == TK_ID && p->tok.str && (js_str_eq_c(p->tok.str, "get") || js_str_eq_c(p->tok.str, "set"))) {
            js_tok_t pk = jp_peek_tok(p);
            if (pk.kind != TK_LPAREN && pk.kind != TK_ASSIGN && pk.kind != TK_SEMI) {
                acc = js_str_eq_c(p->tok.str, "get") ? 1 : 2;
                jp_next(p);
            }
        }
        if (jp_eat(p, TK_STAR)) is_gen = 1;
        js_ast_t *key_ast = 0;
        js_str_t *key_str = 0;
        if (jp_eat(p, TK_LBRACK)) {
            computed = 1;
            key_ast = jp_parse_assign(p);
            jp_expect(p, TK_RBRACK);
        } else if (p->tok.kind == TK_STR || p->tok.kind == TK_NUM) {
            key_str = (p->tok.kind == TK_STR) ? p->tok.str : js_str_new(p->tok.pos, p->tok.len);
            jp_next(p);
        } else {
            key_str = jp_tok_name(&p->tok);
            jp_next(p);
        }
        if (p->tok.kind == TK_LPAREN) {
            js_ast_t *m = ast_new(AST_METHOD);
            m->str = key_str;
            m->a = key_ast;
            m->op = acc;
            m->flags = (is_static ? 1 : 0) | (computed ? 2 : 0);
            js_ast_t *fn = ast_new(AST_FUNC);
            fn->str = key_str;
            fn->flags = (is_async ? 1 : 0) | (is_gen ? 2 : 0);
            fn->a = jp_parse_params(p);
            fn->b = jp_parse_block(p);
            m->b = fn;
            ast_push(cls, m);
        } else {
            js_ast_t *f = ast_new(AST_FIELD);
            f->str = key_str;
            f->a = key_ast;
            f->flags = (is_static ? 1 : 0) | (computed ? 2 : 0);
            if (jp_eat(p, TK_ASSIGN)) f->b = jp_parse_assign(p);
            jp_eat_semi(p);
            ast_push(cls, f);
        }
    }
    jp_expect(p, TK_RBRACE);
    return cls;
}

static js_ast_t *jp_parse_pattern(js_parser_t *p) {
    if (p->tok.kind == TK_LBRACK) {
        jp_next(p);
        js_ast_t *pat = ast_new(AST_PAT_ARR);
        while (p->tok.kind != TK_RBRACK && p->tok.kind != TK_EOF && !p->err) {
            if (p->tok.kind == TK_COMMA) { ast_push(pat, 0); jp_next(p); continue; }
            if (jp_eat(p, TK_ELLIPSIS)) {
                js_ast_t *r = ast_new(AST_PAT_REST);
                r->a = jp_parse_pattern(p);
                ast_push(pat, r);
                break;
            }
            js_ast_t *el = jp_parse_pattern(p);
            if (jp_eat(p, TK_ASSIGN)) {
                js_ast_t *ap = ast_new(AST_PAT_ASSIGN); ap->a = el; ap->b = jp_parse_assign(p); el = ap;
            }
            ast_push(pat, el);
            if (!jp_eat(p, TK_COMMA)) break;
        }
        jp_expect(p, TK_RBRACK);
        return pat;
    }
    if (p->tok.kind == TK_LBRACE) {
        jp_next(p);
        js_ast_t *pat = ast_new(AST_PAT_OBJ);
        while (p->tok.kind != TK_RBRACE && p->tok.kind != TK_EOF && !p->err) {
            if (jp_eat(p, TK_ELLIPSIS)) {
                js_ast_t *r = ast_new(AST_PAT_REST);
                r->a = jp_parse_pattern(p);
                ast_push(pat, r);
                break;
            }
            js_ast_t *pr = ast_new(AST_PROP);
            if (jp_eat(p, TK_LBRACK)) {
                pr->flags = 1; /* computed */
                pr->a = jp_parse_assign(p);
                jp_expect(p, TK_RBRACK);
                jp_expect(p, TK_COLON);
                pr->b = jp_parse_pattern(p);
            } else {
                pr->str = jp_tok_name(&p->tok);
                jp_next(p);
                if (jp_eat(p, TK_COLON)) {
                    pr->b = jp_parse_pattern(p);
                } else {
                    js_ast_t *id = ast_new(AST_ID); id->str = pr->str;
                    pr->b = id;
                }
            }
            if (jp_eat(p, TK_ASSIGN)) {
                js_ast_t *ap = ast_new(AST_PAT_ASSIGN); ap->a = pr->b; ap->b = jp_parse_assign(p); pr->b = ap;
            }
            ast_push(pat, pr);
            if (!jp_eat(p, TK_COMMA)) break;
        }
        jp_expect(p, TK_RBRACE);
        return pat;
    }
    js_ast_t *id = ast_new(AST_ID);
    id->str = jp_tok_name(&p->tok);
    jp_next(p);
    return id;
}

/* Ok fonksiyonu (arrow) kontrolu icin lookahead yardimcisi */
static int jp_looks_like_arrow(js_parser_t *p) {
    /* Current token is '(' : scan matching ')' and see if '=>' follows */
    js_parser_t sv = *p;
    int depth = 0;
    if (sv.tok.kind != TK_LPAREN) return 0;
    depth = 1;
    jp_next(&sv);
    while (sv.tok.kind != TK_EOF && depth > 0) {
        if (sv.tok.kind == TK_LPAREN) depth++;
        else if (sv.tok.kind == TK_RPAREN) depth--;
        if (depth > 0) jp_next(&sv);
    }
    if (depth != 0) return 0;
    jp_next(&sv);
    return sv.tok.kind == TK_ARROW && !sv.tok.had_nl;
}

static js_ast_t *jp_parse_arrow_body(js_parser_t *p, js_ast_t *params, int is_async) {
    jp_expect(p, TK_ARROW);
    js_ast_t *fn = ast_new(AST_FUNC);
    fn->flags = 4 | (is_async ? 1 : 0); /* 4 = arrow */
    fn->a = params;
    if (p->tok.kind == TK_LBRACE) {
        fn->b = jp_parse_block(p);
    } else {
        js_ast_t *blk = ast_new(AST_BLOCK);
        js_ast_t *ret = ast_new(AST_RETURN);
        ret->a = jp_parse_assign(p);
        ast_push(blk, ret);
        fn->b = blk;
    }
    return fn;
}

static js_ast_t *jp_parse_primary(js_parser_t *p) {
    js_tok_t t = p->tok;
    if (t.kind == TK_NUM) { js_ast_t *n = ast_new(AST_NUM); n->num = t.num; jp_next(p); return n; }
    if (t.kind == TK_BIGINT) { js_ast_t *n = ast_new(AST_BIGINT); n->bi = t.bi; jp_next(p); return n; }
    if (t.kind == TK_STR) { js_ast_t *n = ast_new(AST_STR); n->str = t.str; jp_next(p); return n; }
    if (t.kind == TK_TMPL) { js_str_t *raw = t.str; jp_next(p); return jp_parse_template(p, raw); }
    if (t.kind == TK_REGEX) {
        js_ast_t *n = ast_new(AST_REGEX); n->str = t.str; bcopy_str(n->extra, t.re_flags, sizeof(n->extra));
        jp_next(p); return n;
    }
    if (t.kind == TK_TRUE || t.kind == TK_FALSE) {
        js_ast_t *n = ast_new(AST_BOOL); n->op = (t.kind == TK_TRUE); jp_next(p); return n;
    }
    if (t.kind == TK_NULL) { jp_next(p); return ast_new(AST_NULL); }
    if (t.kind == TK_THIS) { jp_next(p); return ast_new(AST_THIS); }
    if (t.kind == TK_SUPER) { jp_next(p); return ast_new(AST_SUPER); }
    if (t.kind == TK_FUNC) return jp_parse_func(p, 0, 0);
    if (t.kind == TK_CLASS) return jp_parse_class(p, 0);
    if (t.kind == TK_ASYNC) {
        js_tok_t pk = jp_peek_tok(p);
        if (!pk.had_nl) {
            if (pk.kind == TK_FUNC) { jp_next(p); return jp_parse_func(p, 0, 1); }
            if (pk.kind == TK_LPAREN) {
                js_parser_t sv = *p; jp_next(&sv);
                if (jp_looks_like_arrow(&sv)) {
                    jp_next(p);
                    js_ast_t *params = jp_parse_params(p);
                    return jp_parse_arrow_body(p, params, 1);
                }
            }
            if (pk.kind == TK_ID) {
                js_parser_t sv = *p; jp_next(&sv); jp_next(&sv);
                if (sv.tok.kind == TK_ARROW && !sv.tok.had_nl) {
                    jp_next(p);
                    js_ast_t *params = ast_new(AST_ARR);
                    js_ast_t *id = ast_new(AST_ID); id->str = p->tok.str; jp_next(p);
                    ast_push(params, id);
                    return jp_parse_arrow_body(p, params, 1);
                }
            }
        }
    }
    /* Arrow function: id => ... */
    if (t.kind == TK_ID || t.kind == TK_OF || t.kind == TK_ASYNC) {
        js_tok_t pk = jp_peek_tok(p);
        if (pk.kind == TK_ARROW && !pk.had_nl) {
            js_ast_t *params = ast_new(AST_ARR);
            js_ast_t *id = ast_new(AST_ID); id->str = jp_tok_name(&p->tok); jp_next(p);
            ast_push(params, id);
            return jp_parse_arrow_body(p, params, 0);
        }
        js_ast_t *id = ast_new(AST_ID); id->str = jp_tok_name(&p->tok); jp_next(p); return id;
    }
    /* Parenthesized expr or Arrow params (...) => ... */
    if (t.kind == TK_LPAREN) {
        if (jp_looks_like_arrow(p)) {
            js_ast_t *params = jp_parse_params(p);
            return jp_parse_arrow_body(p, params, 0);
        }
        jp_next(p);
        js_ast_t *e = jp_parse_expr(p);
        jp_expect(p, TK_RPAREN);
        return e;
    }
    /* Array literal */
    if (t.kind == TK_LBRACK) {
        jp_next(p);
        js_ast_t *arr = ast_new(AST_ARR);
        while (p->tok.kind != TK_RBRACK && p->tok.kind != TK_EOF && !p->err) {
            if (p->tok.kind == TK_COMMA) { ast_push(arr, 0); jp_next(p); continue; }
            if (jp_eat(p, TK_ELLIPSIS)) {
                js_ast_t *sp = ast_new(AST_SPREAD); sp->a = jp_parse_assign(p); ast_push(arr, sp);
            } else {
                ast_push(arr, jp_parse_assign(p));
            }
            if (!jp_eat(p, TK_COMMA)) break;
        }
        jp_expect(p, TK_RBRACK);
        return arr;
    }
    /* Object literal */
    if (t.kind == TK_LBRACE) {
        jp_next(p);
        js_ast_t *obj = ast_new(AST_OBJ);
        while (p->tok.kind != TK_RBRACE && p->tok.kind != TK_EOF && !p->err) {
            if (jp_eat(p, TK_ELLIPSIS)) {
                js_ast_t *sp = ast_new(AST_SPREAD); sp->a = jp_parse_assign(p); ast_push(obj, sp);
                if (!jp_eat(p, TK_COMMA)) break;
                continue;
            }
            js_ast_t *pr = ast_new(AST_PROP);
            int acc = 0, is_async = 0, is_gen = 0;
            if (p->tok.kind == TK_ASYNC && jp_peek_tok(p).kind != TK_COLON && jp_peek_tok(p).kind != TK_COMMA && jp_peek_tok(p).kind != TK_RBRACE && jp_peek_tok(p).kind != TK_LPAREN) {
                is_async = 1; jp_next(p);
            }
            if (p->tok.kind == TK_ID && p->tok.str && (js_str_eq_c(p->tok.str, "get") || js_str_eq_c(p->tok.str, "set"))) {
                js_tok_t pk = jp_peek_tok(p);
                if (pk.kind != TK_COLON && pk.kind != TK_COMMA && pk.kind != TK_RBRACE && pk.kind != TK_LPAREN) {
                    acc = js_str_eq_c(p->tok.str, "get") ? 1 : 2;
                    jp_next(p);
                }
            }
            if (jp_eat(p, TK_STAR)) is_gen = 1;
            if (jp_eat(p, TK_LBRACK)) {
                pr->flags = 1; /* computed */
                pr->a = jp_parse_assign(p);
                jp_expect(p, TK_RBRACK);
            } else if (p->tok.kind == TK_STR) {
                pr->str = p->tok.str; jp_next(p);
            } else if (p->tok.kind == TK_NUM) {
                char nb[48]; int nn = js_dtoa(p->tok.num, nb, sizeof(nb));
                pr->str = js_str_new(nb, nn); jp_next(p);
            } else {
                pr->str = jp_tok_name(&p->tok); jp_next(p);
            }
            pr->op = acc;
            if (p->tok.kind == TK_LPAREN) {
                js_ast_t *fn = ast_new(AST_FUNC);
                fn->str = pr->str;
                fn->flags = (is_async ? 1 : 0) | (is_gen ? 2 : 0);
                fn->a = jp_parse_params(p);
                fn->b = jp_parse_block(p);
                pr->b = fn;
            } else if (jp_eat(p, TK_COLON)) {
                pr->b = jp_parse_assign(p);
            } else {
                /* Shorthand { a } */
                js_ast_t *id = ast_new(AST_ID); id->str = pr->str;
                pr->b = id;
            }
            ast_push(obj, pr);
            if (!jp_eat(p, TK_COMMA)) break;
        }
        jp_expect(p, TK_RBRACE);
        return obj;
    }
    jp_err(p, "Unexpected token in expression");
    jp_next(p);
    return ast_new(AST_NULL);
}

static void jp_parse_args(js_parser_t *p, js_ast_t *call) {
    jp_expect(p, TK_LPAREN);
    while (p->tok.kind != TK_RPAREN && p->tok.kind != TK_EOF && !p->err) {
        if (jp_eat(p, TK_ELLIPSIS)) {
            js_ast_t *sp = ast_new(AST_SPREAD); sp->a = jp_parse_assign(p); ast_push(call, sp);
        } else {
            ast_push(call, jp_parse_assign(p));
        }
        if (!jp_eat(p, TK_COMMA)) break;
    }
    jp_expect(p, TK_RPAREN);
}

static js_ast_t *jp_parse_member(js_parser_t *p, int allow_call) {
    js_ast_t *e;
    if (jp_eat(p, TK_NEW)) {
        if (p->tok.kind == TK_DOT) {
            /* new.target */
            jp_next(p); jp_next(p);
            js_ast_t *id = ast_new(AST_ID); id->str = js_str_new("new.target", 10);
            return id;
        }
        js_ast_t *callee = jp_parse_member(p, 0);
        js_ast_t *nw = ast_new(AST_NEW);
        nw->a = callee;
        if (p->tok.kind == TK_LPAREN) jp_parse_args(p, nw);
        e = nw;
    } else {
        e = jp_parse_primary(p);
    }
    while (!p->err) {
        if (p->tok.kind == TK_DOT || p->tok.kind == TK_QDOT) {
            int opt = (p->tok.kind == TK_QDOT);
            jp_next(p);
            if (opt && p->tok.kind == TK_LBRACK) {
                jp_next(p);
                js_ast_t *m = ast_new(AST_MEMBER);
                m->a = e; m->b = jp_parse_expr(p); m->flags = 3; /* computed + opt */
                jp_expect(p, TK_RBRACK);
                e = m;
                continue;
            }
            if (opt && allow_call && p->tok.kind == TK_LPAREN) {
                js_ast_t *c = ast_new(AST_CALL);
                c->a = e; c->flags = 2; /* opt */
                jp_parse_args(p, c);
                e = c;
                continue;
            }
            js_ast_t *m = ast_new(AST_MEMBER);
            m->a = e; m->str = jp_tok_name(&p->tok); m->flags = opt ? 2 : 0;
            jp_next(p);
            e = m;
        } else if (p->tok.kind == TK_LBRACK) {
            jp_next(p);
            js_ast_t *m = ast_new(AST_MEMBER);
            m->a = e; m->b = jp_parse_expr(p); m->flags = 1; /* computed */
            jp_expect(p, TK_RBRACK);
            e = m;
        } else if (allow_call && p->tok.kind == TK_LPAREN) {
            js_ast_t *c = ast_new(AST_CALL);
            c->a = e;
            jp_parse_args(p, c);
            e = c;
        } else if (allow_call && p->tok.kind == TK_TMPL) {
            /* Tagged template literal: tag`a${x}b` */
            js_ast_t *tm = jp_parse_template(p, p->tok.str);
            jp_next(p);
            js_ast_t *c = ast_new(AST_CALL);
            c->a = e; c->flags = 4; /* tagged template */
            c->b = tm;
            e = c;
        } else break;
    }
    return e;
}

static js_ast_t *jp_parse_unary(js_parser_t *p) {
    js_tok_kind_t k = p->tok.kind;
    if (k == TK_AWAIT) {
        jp_next(p);
        js_ast_t *n = ast_new(AST_AWAIT); n->a = jp_parse_unary(p); return n;
    }
    if (k == TK_INC || k == TK_DEC) {
        jp_next(p);
        js_ast_t *n = ast_new(AST_UPDATE); n->op = k; n->flags = 1; /* prefix */
        n->a = jp_parse_unary(p); return n;
    }
    if (k == TK_PLUS || k == TK_MINUS || k == TK_NOT || k == TK_BNOT ||
        k == TK_TYPEOF || k == TK_VOID || k == TK_DELETE) {
        jp_next(p);
        js_ast_t *n = ast_new(AST_UNARY); n->op = k;
        n->a = jp_parse_unary(p); return n;
    }
    js_ast_t *e = jp_parse_member(p, 1);
    if (!p->tok.had_nl && (p->tok.kind == TK_INC || p->tok.kind == TK_DEC)) {
        js_ast_t *n = ast_new(AST_UPDATE); n->op = p->tok.kind; n->flags = 0; /* postfix */
        n->a = e; jp_next(p); return n;
    }
    return e;
}

static int jp_bin_prec(js_tok_kind_t k) {
    switch (k) {
        case TK_NULLISH: return 3;
        case TK_OR: return 4;
        case TK_AND: return 5;
        case TK_BOR: return 6;
        case TK_BXOR: return 7;
        case TK_BAND: return 8;
        case TK_EQ: case TK_NE: case TK_SEQ: case TK_SNE: return 9;
        case TK_LT: case TK_LE: case TK_GT: case TK_GE: case TK_IN: case TK_INSTANCEOF: return 10;
        case TK_SHL: case TK_SHR: case TK_USHR: return 11;
        case TK_PLUS: case TK_MINUS: return 12;
        case TK_STAR: case TK_SLASH: case TK_PERCENT: return 13;
        case TK_POW: return 14;
        default: return 0;
    }
}

static js_ast_t *jp_parse_bin(js_parser_t *p, int min_prec) {
    js_ast_t *left = jp_parse_unary(p);
    while (!p->err) {
        js_tok_kind_t op = p->tok.kind;
        int prec = jp_bin_prec(op);
        if (prec < min_prec) break;
        jp_next(p);
        int next_min = (op == TK_POW) ? prec : prec + 1;
        js_ast_t *right = jp_parse_bin(p, next_min);
        js_ast_t *b = ast_new((op == TK_AND || op == TK_OR || op == TK_NULLISH) ? AST_LOGICAL : AST_BINARY);
        b->op = op; b->a = left; b->b = right;
        left = b;
    }
    return left;
}

static int jp_is_assign_op(js_tok_kind_t k) {
    return k >= TK_ASSIGN && k <= TK_NULLISH_EQ;
}

static js_ast_t *jp_parse_assign(js_parser_t *p) {
    if (p->tok.kind == TK_YIELD) {
        jp_next(p);
        js_ast_t *y = ast_new(AST_YIELD);
        if (!p->tok.had_nl && jp_eat(p, TK_STAR)) y->flags = 1; /* yield* */
        if (!p->tok.had_nl && p->tok.kind != TK_SEMI && p->tok.kind != TK_RBRACE &&
            p->tok.kind != TK_RPAREN && p->tok.kind != TK_RBRACK && p->tok.kind != TK_COMMA)
            y->a = jp_parse_assign(p);
        return y;
    }
    js_ast_t *left = jp_parse_bin(p, 1);
    if (p->tok.kind == TK_QMARK) {
        jp_next(p);
        js_ast_t *c = ast_new(AST_COND);
        c->a = left;
        c->b = jp_parse_assign(p);
        jp_expect(p, TK_COLON);
        c->c = jp_parse_assign(p);
        return c;
    }
    if (jp_is_assign_op(p->tok.kind)) {
        js_tok_kind_t op = p->tok.kind;
        jp_next(p);
        js_ast_t *a = ast_new(AST_ASSIGN);
        a->op = op; a->a = left; a->b = jp_parse_assign(p);
        return a;
    }
    return left;
}

static js_ast_t *jp_parse_expr(js_parser_t *p) {
    js_ast_t *e = jp_parse_assign(p);
    if (p->tok.kind != TK_COMMA) return e;
    js_ast_t *seq = ast_new(AST_SEQ);
    ast_push(seq, e);
    while (jp_eat(p, TK_COMMA)) ast_push(seq, jp_parse_assign(p));
    return seq;
}

static js_ast_t *jp_parse_var_list(js_parser_t *p, int vkind, int eat_semi) {
    js_ast_t *v = ast_new(AST_VAR);
    v->op = vkind; /* 0=var, 1=let, 2=const */
    while (!p->err) {
        js_ast_t *d = ast_new(AST_VAR_DECL);
        d->a = jp_parse_pattern(p);
        if (jp_eat(p, TK_ASSIGN)) d->b = jp_parse_assign(p);
        ast_push(v, d);
        if (!jp_eat(p, TK_COMMA)) break;
    }
    if (eat_semi) jp_eat_semi(p);
    return v;
}

static js_ast_t *jp_parse_stmt(js_parser_t *p) {
    js_tok_kind_t k = p->tok.kind;
    if (k == TK_SEMI) { jp_next(p); return ast_new(AST_EMPTY); }
    if (k == TK_LBRACE) return jp_parse_block(p);
    if (k == TK_VAR || k == TK_LET || k == TK_CONST) {
        int vk = (k == TK_VAR) ? 0 : (k == TK_LET) ? 1 : 2;
        jp_next(p);
        return jp_parse_var_list(p, vk, 1);
    }
    if (k == TK_FUNC) return jp_parse_func(p, 1, 0);
    if (k == TK_ASYNC && jp_peek_tok(p).kind == TK_FUNC && !jp_peek_tok(p).had_nl) {
        jp_next(p); return jp_parse_func(p, 1, 1);
    }
    if (k == TK_CLASS) return jp_parse_class(p, 1);
    if (k == TK_IF) {
        jp_next(p);
        js_ast_t *n = ast_new(AST_IF);
        jp_expect(p, TK_LPAREN); n->a = jp_parse_expr(p); jp_expect(p, TK_RPAREN);
        n->b = jp_parse_stmt(p);
        if (jp_eat(p, TK_ELSE)) n->c = jp_parse_stmt(p);
        return n;
    }
    if (k == TK_WHILE) {
        jp_next(p);
        js_ast_t *n = ast_new(AST_WHILE);
        jp_expect(p, TK_LPAREN); n->a = jp_parse_expr(p); jp_expect(p, TK_RPAREN);
        n->b = jp_parse_stmt(p);
        return n;
    }
    if (k == TK_DO) {
        jp_next(p);
        js_ast_t *n = ast_new(AST_DO_WHILE);
        n->b = jp_parse_stmt(p);
        jp_expect(p, TK_WHILE); jp_expect(p, TK_LPAREN); n->a = jp_parse_expr(p); jp_expect(p, TK_RPAREN);
        jp_eat_semi(p);
        return n;
    }
    if (k == TK_FOR) {
        jp_next(p);
        if (p->tok.kind == TK_AWAIT) jp_next(p);
        jp_expect(p, TK_LPAREN);
        js_ast_t *init = 0;
        int vkind = -1;
        if (p->tok.kind == TK_VAR || p->tok.kind == TK_LET || p->tok.kind == TK_CONST) {
            vkind = (p->tok.kind == TK_VAR) ? 0 : (p->tok.kind == TK_LET) ? 1 : 2;
            jp_next(p);
            js_ast_t *pat = jp_parse_pattern(p);
            if (p->tok.kind == TK_IN || p->tok.kind == TK_OF) {
                int is_of = (p->tok.kind == TK_OF);
                jp_next(p);
                js_ast_t *fn = ast_new(is_of ? AST_FOR_OF : AST_FOR_IN);
                fn->op = vkind; fn->a = pat; fn->b = jp_parse_expr(p);
                jp_expect(p, TK_RPAREN);
                fn->c = jp_parse_stmt(p);
                return fn;
            }
            js_ast_t *v = ast_new(AST_VAR); v->op = vkind;
            js_ast_t *d = ast_new(AST_VAR_DECL); d->a = pat;
            if (jp_eat(p, TK_ASSIGN)) d->b = jp_parse_assign(p);
            ast_push(v, d);
            while (jp_eat(p, TK_COMMA)) {
                js_ast_t *d2 = ast_new(AST_VAR_DECL);
                d2->a = jp_parse_pattern(p);
                if (jp_eat(p, TK_ASSIGN)) d2->b = jp_parse_assign(p);
                ast_push(v, d2);
            }
            init = v;
        } else if (p->tok.kind != TK_SEMI) {
            /* Could be for (x in obj) or for (x of iter) or normal init */
            js_parser_t sv = *p;
            js_ast_t *pat = jp_parse_pattern(&sv);
            if (!sv.err && (sv.tok.kind == TK_IN || sv.tok.kind == TK_OF)) {
                int is_of = (sv.tok.kind == TK_OF);
                *p = sv; jp_next(p);
                js_ast_t *fn = ast_new(is_of ? AST_FOR_OF : AST_FOR_IN);
                fn->op = -1; fn->a = pat; fn->b = jp_parse_expr(p);
                jp_expect(p, TK_RPAREN);
                fn->c = jp_parse_stmt(p);
                return fn;
            }
            init = jp_parse_expr(p);
        }
        jp_expect(p, TK_SEMI);
        js_ast_t *fn = ast_new(AST_FOR);
        fn->a = init;
        if (p->tok.kind != TK_SEMI) fn->b = jp_parse_expr(p);
        jp_expect(p, TK_SEMI);
        if (p->tok.kind != TK_RPAREN) fn->c = jp_parse_expr(p);
        jp_expect(p, TK_RPAREN);
        fn->d = jp_parse_stmt(p);
        return fn;
    }
    if (k == TK_SWITCH) {
        jp_next(p);
        js_ast_t *sw = ast_new(AST_SWITCH);
        jp_expect(p, TK_LPAREN); sw->a = jp_parse_expr(p); jp_expect(p, TK_RPAREN);
        jp_expect(p, TK_LBRACE);
        while (p->tok.kind != TK_RBRACE && p->tok.kind != TK_EOF && !p->err) {
            js_ast_t *cs = ast_new(AST_CASE);
            if (jp_eat(p, TK_CASE)) { cs->a = jp_parse_expr(p); jp_expect(p, TK_COLON); }
            else if (jp_eat(p, TK_DEFAULT)) { cs->a = 0; jp_expect(p, TK_COLON); }
            else { jp_err(p, "Expected case/default"); break; }
            while (p->tok.kind != TK_CASE && p->tok.kind != TK_DEFAULT &&
                   p->tok.kind != TK_RBRACE && p->tok.kind != TK_EOF && !p->err) {
                ast_push(cs, jp_parse_stmt(p));
            }
            ast_push(sw, cs);
        }
        jp_expect(p, TK_RBRACE);
        return sw;
    }
    if (k == TK_RETURN) {
        jp_next(p);
        js_ast_t *n = ast_new(AST_RETURN);
        if (!p->tok.had_nl && p->tok.kind != TK_SEMI && p->tok.kind != TK_RBRACE && p->tok.kind != TK_EOF)
            n->a = jp_parse_expr(p);
        jp_eat_semi(p);
        return n;
    }
    if (k == TK_THROW) {
        jp_next(p);
        js_ast_t *n = ast_new(AST_THROW);
        n->a = jp_parse_expr(p);
        jp_eat_semi(p);
        return n;
    }
    if (k == TK_BREAK || k == TK_CONT) {
        jp_next(p);
        js_ast_t *n = ast_new(k == TK_BREAK ? AST_BREAK : AST_CONT);
        if (!p->tok.had_nl && p->tok.kind == TK_ID) { n->str = p->tok.str; jp_next(p); }
        jp_eat_semi(p);
        return n;
    }
    if (k == TK_TRY) {
        jp_next(p);
        js_ast_t *n = ast_new(AST_TRY);
        n->a = jp_parse_block(p);
        if (jp_eat(p, TK_CATCH)) {
            if (jp_eat(p, TK_LPAREN)) {
                n->d = jp_parse_pattern(p);
                jp_expect(p, TK_RPAREN);
            }
            n->b = jp_parse_block(p);
        }
        if (jp_eat(p, TK_FINALLY)) n->c = jp_parse_block(p);
        return n;
    }
    /* Labeled statement */
    if (k == TK_ID && jp_peek_tok(p).kind == TK_COLON) {
        js_ast_t *lb = ast_new(AST_LABEL);
        lb->str = p->tok.str;
        jp_next(p); jp_next(p);
        lb->a = jp_parse_stmt(p);
        return lb;
    }
    js_ast_t *es = ast_new(AST_EXPR_STMT);
    es->a = jp_parse_expr(p);
    jp_eat_semi(p);
    return es;
}

static js_ast_t *jp_parse_program(const char *src, int len, char *err_out, int err_cap) {
    js_parser_t p;
    js_memset(&p, 0, sizeof(p));
    p.src = src ? src : "";
    p.len = len >= 0 ? len : (int)js_strlen(p.src);
    p.allow_re = 1;
    jp_next(&p);
    js_ast_t *prog = ast_new(AST_PROG);
    while (p.tok.kind != TK_EOF && !p.err) {
        js_ast_t *st = jp_parse_stmt(&p);
        if (st) ast_push(prog, st);
    }
    if (p.err) {
        if (err_out && err_cap > 0) bcopy_str(err_out, p.err_msg, err_cap);
        return 0;
    }
    return prog;
}

/* ==========================================================================
 * 4. EVALUATOR, CLOSURE/SCOPE, SINIFLAR, GENERATOR, PROMISE & ASYNC/AWAIT
 * ========================================================================== */

static js_env_t *js_env_new(js_ctx_t *ctx, js_env_t *parent, int is_fn) {
    js_env_t *e = (js_env_t *)js_calloc(1, sizeof(js_env_t));
    if (!e) return 0;
    e->parent = parent;
    e->vars = js_obj_new(ctx, OBJ_PLAIN, 0);
    e->is_fn_scope = is_fn;
    return e;
}

static void js_env_def(js_env_t *env, const char *name, js_val_t val, int is_const) {
    if (!env || !env->vars || !name) return;
    uint32_t h = js_hash_bytes(name, (int)js_strlen(name));
    js_prop_t *p = js_find_own_prop(env->vars, name, h, 0);
    if (!p) p = js_add_own_prop(env->vars, name, 0);
    if (!p) return;
    p->val = val;
    p->flags = PROP_ENUM | PROP_CONF | (is_const ? 0 : PROP_WRIT);
}

static js_env_t *js_env_fn_scope(js_env_t *env) {
    js_env_t *cur = env;
    while (cur && !cur->is_fn_scope && cur->parent) cur = cur->parent;
    return cur;
}

static int js_env_get(js_ctx_t *ctx, js_env_t *env, const char *name, js_val_t *out) {
    uint32_t h = js_hash_bytes(name, (int)js_strlen(name));
    js_env_t *cur = env;
    while (cur) {
        if (cur->vars) {
            js_prop_t *p = js_find_own_prop(cur->vars, name, h, 0);
            if (p) { *out = p->val; return 1; }
        }
        cur = cur->parent;
    }
    if (ctx && ctx->global_obj) {
        js_prop_t *p = js_find_own_prop(ctx->global_obj, name, h, 0);
        if (p) {
            if ((p->flags & PROP_ACC) && p->getter) *out = js_call_fn(ctx, jv_obj(p->getter), jv_obj(ctx->global_obj), 0, 0, 0);
            else *out = p->val;
            return 1;
        }
    }
    return 0;
}

static void js_env_set(js_ctx_t *ctx, js_env_t *env, const char *name, js_val_t val) {
    uint32_t h = js_hash_bytes(name, (int)js_strlen(name));
    js_env_t *cur = env;
    while (cur) {
        if (cur->vars) {
            js_prop_t *p = js_find_own_prop(cur->vars, name, h, 0);
            if (p) {
                if (!(p->flags & PROP_WRIT)) {
                    js_throw_err(ctx, "TypeError", "Assignment to constant variable");
                    return;
                }
                p->val = val;
                return;
            }
        }
        cur = cur->parent;
    }
    /* Implicit global property */
    if (ctx && ctx->global_obj) js_set_prop(ctx, jv_obj(ctx->global_obj), name, val);
}

static void js_throw_val(js_ctx_t *ctx, js_val_t v) {
    if (!ctx || ctx->throwing) return;
    ctx->throwing = 1;
    ctx->exc = v;
}

static void js_throw_err(js_ctx_t *ctx, const char *kind, const char *msg) {
    if (!ctx || ctx->throwing) return;
    js_obj_t *e = js_obj_new(ctx, OBJ_ERROR, ctx->err_proto);
    js_def_prop(e, "name", jv_cstr(kind ? kind : "Error"), PROP_WRIT | PROP_CONF);
    js_def_prop(e, "message", jv_cstr(msg ? msg : ""), PROP_WRIT | PROP_CONF);
    js_throw_val(ctx, jv_obj(e));
}

/* Mikrogorev kuyrugu & Promise cozumleme */
static void js_enqueue_micro(js_ctx_t *ctx, js_obj_t *fn, js_val_t arg, int has_arg) {
    if (!ctx || !fn) return;
    int next_tail = (ctx->micro_tail + 1) % JS_MAX_MICRO;
    if (next_tail == ctx->micro_head) return;
    ctx->micros[ctx->micro_tail].fn = fn;
    ctx->micros[ctx->micro_tail].arg = arg;
    ctx->micros[ctx->micro_tail].has_arg = has_arg;
    ctx->micro_tail = next_tail;
}

static void js_drain_microtasks(js_ctx_t *ctx) {
    int guard = 0;
    while (ctx && ctx->micro_head != ctx->micro_tail && guard++ < 512) {
        js_micro_t m = ctx->micros[ctx->micro_head];
        ctx->micro_head = (ctx->micro_head + 1) % JS_MAX_MICRO;
        if (m.fn) {
            int saved_t = ctx->throwing; js_val_t saved_e = ctx->exc;
            ctx->throwing = 0;
            js_call_fn(ctx, jv_obj(m.fn), jv_undef(), m.has_arg ? &m.arg : 0, m.has_arg ? 1 : 0, 0);
            if (saved_t) { ctx->throwing = saved_t; ctx->exc = saved_e; }
            else ctx->throwing = 0;
        }
    }
}

static js_val_t js_prom_run_cb_native(js_ctx_t *ctx, js_val_t this_val, js_val_t *args, int argc, void *ud) {
    (void)this_val; (void)args; (void)argc;
    js_prom_cb_t *cb = (js_prom_cb_t *)ud;
    if (!cb || !cb->next_prom) return jv_undef();
    /* Encoded in args[0] = value, and we know state from cb->next_prom->dom_idx (temp tag) */
    int state = cb->next_prom->dom_idx;
    cb->next_prom->dom_idx = -99;
    js_val_t val = argc > 0 ? args[0] : jv_undef();
    js_obj_t *handler = (state == 1) ? cb->on_fulfill : cb->on_reject;
    if (!handler) {
        if (state == 1) js_prom_resolve_val(ctx, cb->next_prom, val);
        else js_prom_reject_val(ctx, cb->next_prom, val);
        return jv_undef();
    }
    js_val_t res = js_call_fn(ctx, jv_obj(handler), jv_undef(), &val, 1, 0);
    if (ctx->throwing) {
        js_val_t err = ctx->exc; ctx->throwing = 0;
        js_prom_reject_val(ctx, cb->next_prom, err);
    } else {
        js_prom_resolve_val(ctx, cb->next_prom, res);
    }
    return jv_undef();
}

static void js_prom_flush(js_ctx_t *ctx, js_obj_t *p) {
    js_prom_cb_t *cb = p->prom_cbs;
    p->prom_cbs = 0;
    while (cb) {
        js_prom_cb_t *nx = cb->next;
        cb->next_prom->dom_idx = p->prom_state;
        js_obj_t *tramp = js_native_new(ctx, js_prom_run_cb_native, cb, "prom_cb");
        js_enqueue_micro(ctx, tramp, p->prom_val, 1);
        cb = nx;
    }
}

static js_val_t js_prom_resolve_val(js_ctx_t *ctx, js_obj_t *p, js_val_t v) {
    if (!p || p->prom_state != 0) return jv_undef();
    /* Unwrap thenable / Promise */
    if (v.type == JS_OBJ && v.u.o && v.u.o->kind == OBJ_PROMISE && v.u.o != p) {
        js_obj_t *inner = v.u.o;
        if (inner->prom_state == 1) return js_prom_resolve_val(ctx, p, inner->prom_val);
        if (inner->prom_state == 2) return js_prom_reject_val(ctx, p, inner->prom_val);
        js_prom_cb_t *cb = (js_prom_cb_t *)js_calloc(1, sizeof(js_prom_cb_t));
        cb->next_prom = p;
        cb->next = inner->prom_cbs;
        inner->prom_cbs = cb;
        return jv_undef();
    }
    p->prom_state = 1;
    p->prom_val = v;
    js_prom_flush(ctx, p);
    return jv_undef();
}

static js_val_t js_prom_reject_val(js_ctx_t *ctx, js_obj_t *p, js_val_t v) {
    if (!p || p->prom_state != 0) return jv_undef();
    p->prom_state = 2;
    p->prom_val = v;
    js_prom_flush(ctx, p);
    return jv_undef();
}

static js_val_t js_eval_node(js_ctx_t *ctx, js_ast_t *n, js_env_t *env);

/* Iterasyon yardimcisi: Array, String, Set, Map, TypedArray, Generator veya Symbol.iterator */
static js_obj_t *js_to_iter_array(js_ctx_t *ctx, js_val_t v) {
    js_obj_t *out = js_arr_new(ctx, 8);
    if (v.type == JS_STR && v.u.s) {
        for (int i = 0; i < v.u.s->len; i++) js_arr_push(out, jv_cstr_n(v.u.s->data + i, 1));
        return out;
    }
    if (v.type != JS_OBJ || !v.u.o) return out;
    js_obj_t *o = v.u.o;
    if (o->kind == OBJ_ARRAY) {
        for (int i = 0; i < o->elem_n; i++) js_arr_push(out, o->elems[i]);
        return out;
    }
    if (o->kind == OBJ_TYPEDARRAY) {
        int len = (o->buf_len - o->ta_off) / js_ta_elem_size(o->ta_kind);
        for (int i = 0; i < len; i++) js_arr_push(out, jv_num(js_ta_get(o, i)));
        return out;
    }
    if (o->kind == OBJ_SET) {
        for (int i = 0; i < o->map_n; i++) if (o->map_ents[i].used) js_arr_push(out, o->map_ents[i].key);
        return out;
    }
    if (o->kind == OBJ_MAP) {
        for (int i = 0; i < o->map_n; i++) if (o->map_ents[i].used) {
            js_obj_t *pair = js_arr_new(ctx, 2);
            js_arr_push(pair, o->map_ents[i].key);
            js_arr_push(pair, o->map_ents[i].val);
            js_arr_push(out, jv_obj(pair));
        }
        return out;
    }
    if (o->kind == OBJ_GENERATOR) {
        while (o->gen_pos < o->gen_n) js_arr_push(out, o->gen_vals[o->gen_pos++]);
        o->gen_done = 1;
        return out;
    }
    /* Custom Symbol.iterator */
    if (ctx->sym_iterator > 0) {
        js_val_t itfn = js_get_prop_v(ctx, v, jv_sym(ctx->sym_iterator));
        if (itfn.type == JS_OBJ && (itfn.u.o->kind == OBJ_FUNC || itfn.u.o->kind == OBJ_NATIVE)) {
            js_val_t iter = js_call_fn(ctx, itfn, v, 0, 0, 0);
            if (!ctx->throwing && iter.type == JS_OBJ) {
                js_val_t next_fn = js_get_prop(ctx, iter, "next");
                int guard = 0;
                while (next_fn.type == JS_OBJ && guard++ < 10000 && !ctx->throwing) {
                    js_val_t r = js_call_fn(ctx, next_fn, iter, 0, 0, 0);
                    if (ctx->throwing || r.type != JS_OBJ) break;
                    if (js_to_bool(js_get_prop(ctx, r, "done"))) break;
                    js_arr_push(out, js_get_prop(ctx, r, "value"));
                }
            }
        }
    }
    return out;
}

/* Destructuring pattern baglama (vkind: -1=assign, 0=var, 1=let, 2=const) */
static void js_bind_pattern(js_ctx_t *ctx, js_ast_t *pat, js_val_t val, js_env_t *env, int vkind) {
    if (!pat || ctx->throwing) return;
    if (pat->kind == AST_PAT_ASSIGN) {
        if (val.type == JS_UNDEF) val = js_eval_node(ctx, pat->b, env);
        js_bind_pattern(ctx, pat->a, val, env, vkind);
        return;
    }
    if (pat->kind == AST_ID && pat->str) {
        if (vkind == -1) js_env_set(ctx, env, pat->str->data, val);
        else if (vkind == 0) js_env_def(js_env_fn_scope(env), pat->str->data, val, 0);
        else js_env_def(env, pat->str->data, val, vkind == 2);
        return;
    }
    if (pat->kind == AST_MEMBER) {
        /* Assignment destructuring target like [a.x, a.y] = [1, 2] */
        js_val_t obj = js_eval_node(ctx, pat->a, env);
        if (pat->flags & 1) js_set_prop_v(ctx, obj, js_eval_node(ctx, pat->b, env), val);
        else js_set_prop(ctx, obj, pat->str ? pat->str->data : "", val);
        return;
    }
    if (pat->kind == AST_PAT_ARR || pat->kind == AST_ARR) {
        js_obj_t *arr = js_to_iter_array(ctx, val);
        for (int i = 0; i < pat->list_n && !ctx->throwing; i++) {
            js_ast_t *el = pat->list[i];
            if (!el) continue;
            if (el->kind == AST_PAT_REST || el->kind == AST_SPREAD) {
                js_obj_t *rest = js_arr_new(ctx, arr->elem_n > i ? arr->elem_n - i : 0);
                for (int k = i; k < arr->elem_n; k++) js_arr_push(rest, arr->elems[k]);
                js_bind_pattern(ctx, el->a, jv_obj(rest), env, vkind);
                break;
            }
            js_val_t ev = (i < arr->elem_n) ? arr->elems[i] : jv_undef();
            js_bind_pattern(ctx, el, ev, env, vkind);
        }
        return;
    }
    if (pat->kind == AST_PAT_OBJ || pat->kind == AST_OBJ) {
        char *used_keys[64]; int used_n = 0;
        for (int i = 0; i < pat->list_n && !ctx->throwing; i++) {
            js_ast_t *pr = pat->list[i];
            if (!pr) continue;
            if (pr->kind == AST_PAT_REST || pr->kind == AST_SPREAD) {
                js_obj_t *rest = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
                if (val.type == JS_OBJ && val.u.o) {
                    js_obj_t *so = val.u.o;
                    for (int k = 0; k < so->prop_n; k++) {
                        if (!(so->props[k].flags & PROP_ENUM) || !so->props[k].key) continue;
                        int skip = 0;
                        for (int u = 0; u < used_n; u++) if (js_strcmp(used_keys[u], so->props[k].key) == 0) { skip = 1; break; }
                        if (!skip) js_set_prop(ctx, jv_obj(rest), so->props[k].key, so->props[k].val);
                    }
                }
                js_bind_pattern(ctx, pr->a, jv_obj(rest), env, vkind);
                break;
            }
            js_val_t kv;
            const char *kname = "";
            if (pr->flags & 1) {
                kv = js_eval_node(ctx, pr->a, env);
                js_str_t *ks = js_to_str(ctx, kv);
                kname = ks ? ks->data : "";
            } else {
                kname = pr->str ? pr->str->data : "";
                kv = jv_cstr(kname);
            }
            if (used_n < 64) used_keys[used_n++] = (char *)kname;
            js_val_t pv = js_get_prop_v(ctx, val, kv);
            js_bind_pattern(ctx, pr->b, pv, env, vkind);
        }
    }
}

/* Generator .next() / .return() / [Symbol.iterator] */
static js_val_t js_gen_next_fn(js_ctx_t *ctx, js_val_t this_val, js_val_t *args, int argc, void *ud) {
    (void)args; (void)argc; (void)ud;
    js_obj_t *r = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    if (this_val.type != JS_OBJ || !this_val.u.o || this_val.u.o->kind != OBJ_GENERATOR) {
        js_def_prop(r, "value", jv_undef(), PROP_ENUM | PROP_WRIT | PROP_CONF);
        js_def_prop(r, "done", jv_bool(1), PROP_ENUM | PROP_WRIT | PROP_CONF);
        return jv_obj(r);
    }
    js_obj_t *g = this_val.u.o;
    if (!g->gen_done && g->gen_pos < g->gen_n) {
        js_def_prop(r, "value", g->gen_vals[g->gen_pos++], PROP_ENUM | PROP_WRIT | PROP_CONF);
        js_def_prop(r, "done", jv_bool(0), PROP_ENUM | PROP_WRIT | PROP_CONF);
    } else {
        g->gen_done = 1;
        js_def_prop(r, "value", jv_undef(), PROP_ENUM | PROP_WRIT | PROP_CONF);
        js_def_prop(r, "done", jv_bool(1), PROP_ENUM | PROP_WRIT | PROP_CONF);
    }
    return jv_obj(r);
}
static js_val_t js_gen_self_iter(js_ctx_t *ctx, js_val_t this_val, js_val_t *args, int argc, void *ud) {
    (void)ctx; (void)args; (void)argc; (void)ud;
    return this_val;
}

/* Hoisting: var ve function bildirimleri */
static void js_hoist(js_ctx_t *ctx, js_ast_t *blk, js_env_t *env) {
    if (!blk) return;
    for (int i = 0; i < blk->list_n; i++) {
        js_ast_t *s = blk->list[i];
        if (!s) continue;
        if (s->kind == AST_FUNC && s->str && s->str->len > 0) {
            js_obj_t *fn = js_obj_new(ctx, OBJ_FUNC, ctx->fn_proto);
            fn->fn_ast = s; fn->fn_env = env;
            fn->is_async = (s->flags & 1) ? 1 : 0;
            fn->is_gen = (s->flags & 2) ? 1 : 0;
            js_obj_t *fp = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
            js_def_prop(fp, "constructor", jv_obj(fn), PROP_WRIT | PROP_CONF);
            js_def_prop(fn, "prototype", jv_obj(fp), PROP_WRIT);
            js_def_prop(fn, "name", jv_str(s->str), PROP_CONF);
            js_def_prop(fn, "length", jv_num(s->a ? (double)s->a->list_n : 0.0), PROP_CONF);
            js_env_def(env, s->str->data, jv_obj(fn), 0);
        } else if (s->kind == AST_VAR && s->op == 0) {
            for (int k = 0; k < s->list_n; k++) {
                js_ast_t *d = s->list[k];
                if (d && d->a && d->a->kind == AST_ID && d->a->str)
                    js_env_def(js_env_fn_scope(env), d->a->str->data, jv_undef(), 0);
            }
        }
    }
}

/* Fonksiyon / Native / Sinif cagrisi */
static js_val_t js_call_fn(js_ctx_t *ctx, js_val_t fnv, js_val_t this_val, js_val_t *args, int argc, int is_new) {
    if (!ctx || ctx->throwing) return jv_undef();
    if (++ctx->depth > ctx->max_depth) {
        ctx->depth--;
        js_throw_err(ctx, "RangeError", "Maximum call stack size exceeded");
        return jv_undef();
    }
    if (fnv.type != JS_OBJ || !fnv.u.o) {
        ctx->depth--;
        js_throw_err(ctx, "TypeError", "Value is not a function");
        return jv_undef();
    }
    js_obj_t *fn = fnv.u.o;
    /* Proxy apply/construct */
    if (fn->kind == OBJ_PROXY && fn->proxy_handler && fn->proxy_target) {
        const char *tname = is_new ? "construct" : "apply";
        js_val_t trap = js_get_prop(ctx, jv_obj(fn->proxy_handler), tname);
        if (trap.type == JS_OBJ && (trap.u.o->kind == OBJ_FUNC || trap.u.o->kind == OBJ_NATIVE)) {
            js_obj_t *al = js_arr_new(ctx, argc);
            for (int i = 0; i < argc; i++) js_arr_push(al, args[i]);
            js_val_t r;
            if (is_new) {
                js_val_t ta[3] = { jv_obj(fn->proxy_target), jv_obj(al), fnv };
                r = js_call_fn(ctx, trap, jv_obj(fn->proxy_handler), ta, 3, 0);
            } else {
                js_val_t ta[3] = { jv_obj(fn->proxy_target), this_val, jv_obj(al) };
                r = js_call_fn(ctx, trap, jv_obj(fn->proxy_handler), ta, 3, 0);
            }
            ctx->depth--;
            return r;
        }
        js_val_t r = js_call_fn(ctx, jv_obj(fn->proxy_target), this_val, args, argc, is_new);
        ctx->depth--;
        return r;
    }
    /* Bound function (.bind) */
    if (fn->bound_target) {
        int tot = fn->bound_argc + argc;
        js_val_t *all = tot > 0 ? (js_val_t *)js_malloc((size_t)tot * sizeof(js_val_t)) : 0;
        for (int i = 0; i < fn->bound_argc; i++) all[i] = fn->bound_args[i];
        for (int i = 0; i < argc; i++) all[fn->bound_argc + i] = args[i];
        js_val_t r = js_call_fn(ctx, jv_obj(fn->bound_target), is_new ? this_val : fn->bound_this, all, tot, is_new);
        if (all) js_free(all);
        ctx->depth--;
        return r;
    }
    if (fn->kind == OBJ_NATIVE && fn->native_fn) {
        js_val_t r = fn->native_fn(ctx, this_val, args, argc, fn->native_ud);
        ctx->depth--;
        return r;
    }
    if (fn->kind != OBJ_FUNC || !fn->fn_ast) {
        ctx->depth--;
        js_throw_err(ctx, "TypeError", "Not a function");
        return jv_undef();
    }

    js_val_t new_inst = jv_undef();
    if (is_new) {
        js_val_t pv = js_get_prop(ctx, fnv, "prototype");
        js_obj_t *inst = js_obj_new(ctx, OBJ_PLAIN, (pv.type == JS_OBJ) ? pv.u.o : ctx->obj_proto);
        new_inst = jv_obj(inst);
        this_val = new_inst;
        /* Initialize class instance fields if stored on constructor */
        js_val_t finit = js_get_prop(ctx, fnv, "__fields_init__");
        if (finit.type == JS_OBJ && finit.u.o->kind == OBJ_FUNC) {
            js_call_fn(ctx, finit, new_inst, 0, 0, 0);
        }
    }

    js_env_t *fenv = js_env_new(ctx, fn->fn_env ? fn->fn_env : ctx->global_env, 1);
    if (!fn->is_arrow) {
        fenv->has_this = 1;
        fenv->this_val = (this_val.type == JS_UNDEF || this_val.type == JS_NULL) ? jv_obj(ctx->global_obj) : this_val;
        if (is_new) fenv->this_val = new_inst;
        fenv->super_proto = fn->super_proto;
        /* arguments object */
        js_obj_t *arg_obj = js_arr_new(ctx, argc);
        for (int i = 0; i < argc; i++) js_arr_push(arg_obj, args[i]);
        js_env_def(fenv, "arguments", jv_obj(arg_obj), 0);
    } else {
        fenv->has_this = 0;
        fenv->super_proto = fn->super_proto;
    }
    if (fn->fn_ast->str && fn->fn_ast->str->len > 0)
        js_env_def(fenv, fn->fn_ast->str->data, fnv, 0);

    /* Bind parameters */
    js_ast_t *params = fn->fn_ast->a;
    if (params) {
        for (int i = 0; i < params->list_n && !ctx->throwing; i++) {
            js_ast_t *pa = params->list[i];
            if (!pa) continue;
            if (pa->kind == AST_PAT_REST) {
                js_obj_t *rest = js_arr_new(ctx, argc > i ? argc - i : 0);
                for (int k = i; k < argc; k++) js_arr_push(rest, args[k]);
                js_bind_pattern(ctx, pa->a, jv_obj(rest), fenv, 1);
                break;
            }
            js_val_t av = (i < argc) ? args[i] : jv_undef();
            js_bind_pattern(ctx, pa, av, fenv, 1);
        }
    }

    /* Generator function: collect yields and return generator object */
    if (fn->is_gen) {
        int prev_in_gen = ctx->in_gen;
        js_val_t *prev_buf = ctx->gen_buf; int prev_cnt = ctx->gen_cnt, prev_cap = ctx->gen_cap;
        ctx->in_gen = 1; ctx->gen_buf = 0; ctx->gen_cnt = 0; ctx->gen_cap = 0;
        int prev_ret = ctx->returning; js_val_t prev_rv = ctx->ret_val;
        ctx->returning = 0; ctx->ret_val = jv_undef();
        js_hoist(ctx, fn->fn_ast->b, fenv);
        js_eval_node(ctx, fn->fn_ast->b, fenv);
        js_obj_t *go = js_obj_new(ctx, OBJ_GENERATOR, ctx->obj_proto);
        go->gen_vals = ctx->gen_buf;
        go->gen_n = ctx->gen_cnt;
        go->gen_pos = 0;
        go->gen_done = 0;
        js_def_native(ctx, go, "next", js_gen_next_fn, 0);
        js_def_sym_prop(go, ctx->sym_iterator, jv_obj(js_native_new(ctx, js_gen_self_iter, 0, "[Symbol.iterator]")));
        ctx->in_gen = prev_in_gen; ctx->gen_buf = prev_buf; ctx->gen_cnt = prev_cnt; ctx->gen_cap = prev_cap;
        ctx->returning = prev_ret; ctx->ret_val = prev_rv;
        ctx->depth--;
        return jv_obj(go);
    }

    int prev_ret = ctx->returning; js_val_t prev_rv = ctx->ret_val;
    int prev_brk = ctx->breaking, prev_cnt = ctx->continuing;
    ctx->returning = 0; ctx->ret_val = jv_undef();
    ctx->breaking = 0; ctx->continuing = 0;

    js_hoist(ctx, fn->fn_ast->b, fenv);
    js_eval_node(ctx, fn->fn_ast->b, fenv);

    js_val_t res = ctx->returning ? ctx->ret_val : jv_undef();
    ctx->returning = prev_ret; ctx->ret_val = prev_rv;
    ctx->breaking = prev_brk; ctx->continuing = prev_cnt;
    ctx->depth--;

    if (fn->is_async) {
        js_obj_t *pr = js_obj_new(ctx, OBJ_PROMISE, ctx->prom_proto);
        if (ctx->throwing) {
            js_val_t ex = ctx->exc; ctx->throwing = 0;
            js_prom_reject_val(ctx, pr, ex);
        } else {
            js_prom_resolve_val(ctx, pr, res);
        }
        return jv_obj(pr);
    }

    if (is_new && !ctx->throwing) {
        if (res.type == JS_OBJ && res.u.o) return res;
        return new_inst;
    }
    return res;
}

static js_val_t js_find_this(js_ctx_t *ctx, js_env_t *env) {
    js_env_t *c = env;
    while (c) {
        if (c->has_this) return c->this_val;
        c = c->parent;
    }
    return ctx && ctx->global_obj ? jv_obj(ctx->global_obj) : jv_undef();
}

static js_obj_t *js_find_super(js_env_t *env) {
    js_env_t *c = env;
    while (c) {
        if (c->super_proto) return c->super_proto;
        c = c->parent;
    }
    return 0;
}

static js_val_t js_apply_binop(js_ctx_t *ctx, int op, js_val_t a, js_val_t b) {
    if (a.type == JS_BIGINT && b.type == JS_BIGINT) {
        switch (op) {
            case TK_PLUS: return jv_bigint(a.u.bi + b.u.bi);
            case TK_MINUS: return jv_bigint(a.u.bi - b.u.bi);
            case TK_STAR: return jv_bigint(a.u.bi * b.u.bi);
            case TK_SLASH: return b.u.bi ? jv_bigint(a.u.bi / b.u.bi) : (js_throw_err(ctx, "RangeError", "Division by zero"), jv_undef());
            case TK_PERCENT: return b.u.bi ? jv_bigint(a.u.bi % b.u.bi) : (js_throw_err(ctx, "RangeError", "Division by zero"), jv_undef());
            case TK_POW: return jv_bigint((int64_t)js_pow((double)a.u.bi, (double)b.u.bi));
            case TK_LT: return jv_bool(a.u.bi < b.u.bi);
            case TK_LE: return jv_bool(a.u.bi <= b.u.bi);
            case TK_GT: return jv_bool(a.u.bi > b.u.bi);
            case TK_GE: return jv_bool(a.u.bi >= b.u.bi);
            case TK_BAND: return jv_bigint(a.u.bi & b.u.bi);
            case TK_BOR: return jv_bigint(a.u.bi | b.u.bi);
            case TK_BXOR: return jv_bigint(a.u.bi ^ b.u.bi);
            case TK_SHL: return jv_bigint(a.u.bi << b.u.bi);
            case TK_SHR: return jv_bigint(a.u.bi >> b.u.bi);
        }
    }
    switch (op) {
        case TK_PLUS: {
            js_val_t pa = (a.type == JS_OBJ) ? js_to_primitive(ctx, a, 0) : a;
            js_val_t pb = (b.type == JS_OBJ) ? js_to_primitive(ctx, b, 0) : b;
            if (pa.type == JS_STR || pb.type == JS_STR) {
                js_str_t *sa = js_to_str(ctx, pa), *sb = js_to_str(ctx, pb);
                js_sb_t buf; js_sb_init(&buf);
                if (sa) js_sb_putn(&buf, sa->data, sa->len);
                if (sb) js_sb_putn(&buf, sb->data, sb->len);
                return jv_str(js_sb_finish(&buf));
            }
            return jv_num(js_to_num(ctx, pa) + js_to_num(ctx, pb));
        }
        case TK_MINUS:   return jv_num(js_to_num(ctx, a) - js_to_num(ctx, b));
        case TK_STAR:    return jv_num(js_to_num(ctx, a) * js_to_num(ctx, b));
        case TK_SLASH:   return jv_num(js_to_num(ctx, a) / js_to_num(ctx, b));
        case TK_PERCENT: return jv_num(js_fmod(js_to_num(ctx, a), js_to_num(ctx, b)));
        case TK_POW:     return jv_num(js_pow(js_to_num(ctx, a), js_to_num(ctx, b)));
        case TK_BAND:    return jv_num((double)(js_to_i32(ctx, a) & js_to_i32(ctx, b)));
        case TK_BOR:     return jv_num((double)(js_to_i32(ctx, a) | js_to_i32(ctx, b)));
        case TK_BXOR:    return jv_num((double)(js_to_i32(ctx, a) ^ js_to_i32(ctx, b)));
        case TK_SHL:     return jv_num((double)(int32_t)((uint32_t)js_to_i32(ctx, a) << (js_to_u32(ctx, b) & 31)));
        case TK_SHR:     return jv_num((double)(js_to_i32(ctx, a) >> (js_to_u32(ctx, b) & 31)));
        case TK_USHR:    return jv_num((double)(js_to_u32(ctx, a) >> (js_to_u32(ctx, b) & 31)));
        case TK_SEQ:     return jv_bool(js_strict_eq(a, b));
        case TK_SNE:     return jv_bool(!js_strict_eq(a, b));
        case TK_EQ:      return jv_bool(js_loose_eq(ctx, a, b));
        case TK_NE:      return jv_bool(!js_loose_eq(ctx, a, b));
        case TK_LT: case TK_LE: case TK_GT: case TK_GE: {
            js_val_t pa = (a.type == JS_OBJ) ? js_to_primitive(ctx, a, 0) : a;
            js_val_t pb = (b.type == JS_OBJ) ? js_to_primitive(ctx, b, 0) : b;
            if (pa.type == JS_STR && pb.type == JS_STR) {
                int c = js_strcmp(pa.u.s ? pa.u.s->data : "", pb.u.s ? pb.u.s->data : "");
                if (op == TK_LT) return jv_bool(c < 0);
                if (op == TK_LE) return jv_bool(c <= 0);
                if (op == TK_GT) return jv_bool(c > 0);
                return jv_bool(c >= 0);
            }
            double na = js_to_num(ctx, pa), nb = js_to_num(ctx, pb);
            if (op == TK_LT) return jv_bool(na < nb);
            if (op == TK_LE) return jv_bool(na <= nb);
            if (op == TK_GT) return jv_bool(na > nb);
            return jv_bool(na >= nb);
        }
        case TK_IN: return jv_bool(js_has_prop_v(ctx, b, a));
        case TK_INSTANCEOF: {
            if (a.type != JS_OBJ || !a.u.o || b.type != JS_OBJ || !b.u.o) return jv_bool(0);
            js_val_t bp = js_get_prop(ctx, b, "prototype");
            if (bp.type != JS_OBJ || !bp.u.o) return jv_bool(0);
            js_obj_t *cur = a.u.o->proto;
            int g = 0;
            while (cur && g++ < 32) {
                if (cur == bp.u.o) return jv_bool(1);
                cur = cur->proto;
            }
            return jv_bool(0);
        }
    }
    return jv_undef();
}

static int js_compound_to_bin(int op) {
    switch (op) {
        case TK_ADD_EQ: return TK_PLUS; case TK_SUB_EQ: return TK_MINUS;
        case TK_MUL_EQ: return TK_STAR; case TK_DIV_EQ: return TK_SLASH;
        case TK_MOD_EQ: return TK_PERCENT; case TK_POW_EQ: return TK_POW;
        case TK_SHL_EQ: return TK_SHL; case TK_SHR_EQ: return TK_SHR;
        case TK_USHR_EQ: return TK_USHR; case TK_AND_EQ: return TK_BAND;
        case TK_XOR_EQ: return TK_BXOR; case TK_OR_EQ: return TK_BOR;
        default: return 0;
    }
}

static js_val_t js_eval_node(js_ctx_t *ctx, js_ast_t *n, js_env_t *env) {
    if (!n || !ctx || ctx->throwing || ctx->returning || ctx->breaking || ctx->continuing)
        return jv_undef();
    if (++ctx->steps > ctx->max_steps) {
        js_throw_err(ctx, "RangeError", "Script execution step limit exceeded");
        return jv_undef();
    }

    switch (n->kind) {
        case AST_EMPTY: return jv_undef();
        case AST_NUM: return jv_num(n->num);
        case AST_BIGINT: return jv_bigint(n->bi);
        case AST_STR: return jv_str(n->str ? n->str : js_str_new("", 0));
        case AST_BOOL: return jv_bool(n->op);
        case AST_NULL: return jv_null();
        case AST_THIS: return js_find_this(ctx, env);
        case AST_SUPER: {
            js_obj_t *sp = js_find_super(env);
            return sp ? jv_obj(sp) : jv_undef();
        }
        case AST_REGEX: {
            js_obj_t *ro = js_obj_new(ctx, OBJ_REGEXP, ctx->re_proto);
            int pl = n->str ? n->str->len : 0;
            ro->re_pat = (char *)js_malloc((size_t)pl + 1);
            if (ro->re_pat) { if (pl) js_memcpy(ro->re_pat, n->str->data, (size_t)pl); ro->re_pat[pl] = 0; }
            bcopy_str(ro->re_flags, n->extra, sizeof(ro->re_flags));
            return jv_obj(ro);
        }
        case AST_TMPL: {
            js_sb_t sb; js_sb_init(&sb);
            for (int i = 0; i < n->list_n && !ctx->throwing; i++) {
                js_val_t v = js_eval_node(ctx, n->list[i], env);
                js_str_t *s = js_to_str(ctx, v);
                if (s) js_sb_putn(&sb, s->data, s->len);
            }
            return jv_str(js_sb_finish(&sb));
        }
        case AST_ID: {
            const char *name = n->str ? n->str->data : "";
            if (js_strcmp(name, "undefined") == 0) return jv_undef();
            js_val_t v;
            if (js_env_get(ctx, env, name, &v)) return v;
            js_throw_err(ctx, "ReferenceError", name);
            return jv_undef();
        }
        case AST_PROG: {
            js_hoist(ctx, n, env);
            js_val_t last = jv_undef();
            for (int i = 0; i < n->list_n; i++) {
                last = js_eval_node(ctx, n->list[i], env);
                if (ctx->throwing || ctx->returning || ctx->breaking || ctx->continuing) break;
            }
            return last;
        }
        case AST_BLOCK: {
            js_env_t *benv = js_env_new(ctx, env, 0);
            js_hoist(ctx, n, benv);
            js_val_t last = jv_undef();
            for (int i = 0; i < n->list_n; i++) {
                last = js_eval_node(ctx, n->list[i], benv);
                if (ctx->throwing || ctx->returning || ctx->breaking || ctx->continuing) break;
            }
            return last;
        }
        case AST_EXPR_STMT: return js_eval_node(ctx, n->a, env);
        case AST_SEQ: {
            js_val_t r = jv_undef();
            for (int i = 0; i < n->list_n && !ctx->throwing; i++) r = js_eval_node(ctx, n->list[i], env);
            return r;
        }
        case AST_VAR: {
            for (int i = 0; i < n->list_n && !ctx->throwing; i++) {
                js_ast_t *d = n->list[i];
                js_val_t init = d->b ? js_eval_node(ctx, d->b, env) : jv_undef();
                if (!ctx->throwing) js_bind_pattern(ctx, d->a, init, env, n->op);
            }
            return jv_undef();
        }
        case AST_FUNC: {
            js_obj_t *fn = js_obj_new(ctx, OBJ_FUNC, ctx->fn_proto);
            fn->fn_ast = n; fn->fn_env = env;
            fn->is_async = (n->flags & 1) ? 1 : 0;
            fn->is_gen = (n->flags & 2) ? 1 : 0;
            fn->is_arrow = (n->flags & 4) ? 1 : 0;
            if (!fn->is_arrow) {
                js_obj_t *fp = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
                js_def_prop(fp, "constructor", jv_obj(fn), PROP_WRIT | PROP_CONF);
                js_def_prop(fn, "prototype", jv_obj(fp), PROP_WRIT);
            }
            if (n->str && n->str->len > 0) js_def_prop(fn, "name", jv_str(n->str), PROP_CONF);
            js_def_prop(fn, "length", jv_num(n->a ? (double)n->a->list_n : 0.0), PROP_CONF);
            return jv_obj(fn);
        }
        case AST_CLASS: {
            js_obj_t *super_ctor = 0, *super_proto = ctx->obj_proto;
            if (n->a) {
                js_val_t sv = js_eval_node(ctx, n->a, env);
                if (ctx->throwing) return jv_undef();
                if (sv.type == JS_OBJ && sv.u.o) {
                    super_ctor = sv.u.o;
                    js_val_t spv = js_get_prop(ctx, sv, "prototype");
                    if (spv.type == JS_OBJ && spv.u.o) super_proto = spv.u.o;
                }
            }
            js_obj_t *proto = js_obj_new(ctx, OBJ_PLAIN, super_proto);
            js_obj_t *ctor = 0;
            /* Find constructor method or synthesize default */
            for (int i = 0; i < n->list_n; i++) {
                js_ast_t *m = n->list[i];
                if (m->kind == AST_METHOD && !(m->flags & 1) && m->str && js_str_eq_c(m->str, "constructor")) {
                    js_val_t cv = js_eval_node(ctx, m->b, env);
                    ctor = cv.u.o;
                    break;
                }
            }
            if (!ctor) {
                /* Default constructor calling super if present */
                js_ast_t *df = ast_new(AST_FUNC);
                df->a = ast_new(AST_ARR);
                df->b = ast_new(AST_BLOCK);
                if (super_ctor) {
                    js_ast_t *sc = ast_new(AST_CALL);
                    sc->a = ast_new(AST_SUPER);
                    js_ast_t *sp = ast_new(AST_SPREAD);
                    js_ast_t *ag = ast_new(AST_ID); ag->str = js_str_new("arguments", 9);
                    sp->a = ag; ast_push(sc, sp);
                    js_ast_t *es = ast_new(AST_EXPR_STMT); es->a = sc;
                    ast_push(df->b, es);
                }
                ctor = js_obj_new(ctx, OBJ_FUNC, super_ctor ? super_ctor : ctx->fn_proto);
                ctor->fn_ast = df; ctor->fn_env = env;
            }
            ctor->is_class = 1;
            ctor->super_proto = super_ctor;
            if (super_ctor) ctor->proto = super_ctor;
            js_def_prop(ctor, "prototype", jv_obj(proto), PROP_WRIT);
            js_def_prop(proto, "constructor", jv_obj(ctor), PROP_WRIT | PROP_CONF);
            if (n->str && n->str->len > 0) js_def_prop(ctor, "name", jv_str(n->str), PROP_CONF);

            /* Instance fields initializer block */
            js_ast_t *fblk = ast_new(AST_BLOCK);
            int has_inst_fields = 0;
            for (int i = 0; i < n->list_n; i++) {
                js_ast_t *m = n->list[i];
                if (m->kind == AST_METHOD) {
                    if (!(m->flags & 1) && m->str && js_str_eq_c(m->str, "constructor")) continue;
                    js_obj_t *target = (m->flags & 1) ? ctor : proto;
                    js_val_t mfv = js_eval_node(ctx, m->b, env);
                    if (mfv.type == JS_OBJ && mfv.u.o) mfv.u.o->super_proto = (m->flags & 1) ? super_ctor : super_proto;
                    const char *kname = m->str ? m->str->data : "";
                    int sym_id = 0;
                    if (m->flags & 2) {
                        js_val_t kv = js_eval_node(ctx, m->a, env);
                        if (kv.type == JS_SYM) sym_id = kv.u.sym;
                        else { js_str_t *ks = js_to_str(ctx, kv); kname = ks ? ks->data : ""; }
                    }
                    if (m->op == 1 || m->op == 2) {
                        uint32_t h = sym_id ? 0 : js_hash_bytes(kname, (int)js_strlen(kname));
                        js_prop_t *p = js_find_own_prop(target, kname, h, sym_id);
                        if (!p) p = js_add_own_prop(target, kname, sym_id);
                        if (p) {
                            p->flags = PROP_CONF | PROP_ACC;
                            if (m->op == 1) p->getter = mfv.u.o;
                            else p->setter = mfv.u.o;
                        }
                    } else if (sym_id > 0) {
                        js_def_sym_prop(target, sym_id, mfv);
                    } else {
                        js_def_prop(target, kname, mfv, PROP_WRIT | PROP_CONF);
                    }
                } else if (m->kind == AST_FIELD) {
                    if (m->flags & 1) {
                        js_val_t fv = m->b ? js_eval_node(ctx, m->b, env) : jv_undef();
                        if (m->flags & 2) js_set_prop_v(ctx, jv_obj(ctor), js_eval_node(ctx, m->a, env), fv);
                        else js_set_prop(ctx, jv_obj(ctor), m->str ? m->str->data : "", fv);
                    } else {
                        has_inst_fields = 1;
                        js_ast_t *asg = ast_new(AST_ASSIGN); asg->op = TK_ASSIGN;
                        js_ast_t *mem = ast_new(AST_MEMBER); mem->a = ast_new(AST_THIS);
                        if (m->flags & 2) { mem->flags = 1; mem->b = m->a; }
                        else mem->str = m->str;
                        asg->a = mem;
                        asg->b = m->b ? m->b : ast_new(AST_NULL);
                        if (!m->b) { js_ast_t *u = ast_new(AST_ID); u->str = js_str_new("undefined", 9); asg->b = u; }
                        js_ast_t *es = ast_new(AST_EXPR_STMT); es->a = asg;
                        ast_push(fblk, es);
                    }
                }
            }
            if (has_inst_fields) {
                js_ast_t *ff = ast_new(AST_FUNC); ff->a = ast_new(AST_ARR); ff->b = fblk;
                js_obj_t *fo = js_obj_new(ctx, OBJ_FUNC, ctx->fn_proto);
                fo->fn_ast = ff; fo->fn_env = env;
                js_def_prop(ctor, "__fields_init__", jv_obj(fo), 0);
            }
            if (n->str && n->str->len > 0) js_env_def(env, n->str->data, jv_obj(ctor), 0);
            return jv_obj(ctor);
        }
        case AST_ARR: {
            js_obj_t *a = js_arr_new(ctx, n->list_n);
            for (int i = 0; i < n->list_n && !ctx->throwing; i++) {
                js_ast_t *el = n->list[i];
                if (!el) { js_arr_push(a, jv_undef()); continue; }
                if (el->kind == AST_SPREAD) {
                    js_val_t sv = js_eval_node(ctx, el->a, env);
                    js_obj_t *it = js_to_iter_array(ctx, sv);
                    for (int k = 0; k < it->elem_n; k++) js_arr_push(a, it->elems[k]);
                } else {
                    js_arr_push(a, js_eval_node(ctx, el, env));
                }
            }
            return jv_obj(a);
        }
        case AST_OBJ: {
            js_obj_t *o = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
            for (int i = 0; i < n->list_n && !ctx->throwing; i++) {
                js_ast_t *pr = n->list[i];
                if (pr->kind == AST_SPREAD) {
                    js_val_t sv = js_eval_node(ctx, pr->a, env);
                    if (sv.type == JS_OBJ && sv.u.o) {
                        js_obj_t *so = sv.u.o;
                        if (so->kind == OBJ_ARRAY) {
                            for (int k = 0; k < so->elem_n; k++) {
                                char kb[16]; js_i64toa(k, kb, sizeof(kb));
                                js_set_prop(ctx, jv_obj(o), kb, so->elems[k]);
                            }
                        } else {
                            for (int k = 0; k < so->prop_n; k++) {
                                if (!(so->props[k].flags & PROP_ENUM)) continue;
                                if (so->props[k].sym_id > 0) js_def_sym_prop(o, so->props[k].sym_id, so->props[k].val);
                                else if (so->props[k].key) {
                                    js_val_t v = (so->props[k].flags & PROP_ACC) && so->props[k].getter
                                        ? js_call_fn(ctx, jv_obj(so->props[k].getter), sv, 0, 0, 0)
                                        : so->props[k].val;
                                    js_set_prop(ctx, jv_obj(o), so->props[k].key, v);
                                }
                            }
                        }
                    }
                    continue;
                }
                const char *kname = pr->str ? pr->str->data : "";
                int sym_id = 0;
                if (pr->flags & 1) {
                    js_val_t kv = js_eval_node(ctx, pr->a, env);
                    if (kv.type == JS_SYM) sym_id = kv.u.sym;
                    else { js_str_t *ks = js_to_str(ctx, kv); kname = ks ? ks->data : ""; }
                }
                js_val_t vv = js_eval_node(ctx, pr->b, env);
                if (pr->op == 1 || pr->op == 2) {
                    uint32_t h = sym_id ? 0 : js_hash_bytes(kname, (int)js_strlen(kname));
                    js_prop_t *p = js_find_own_prop(o, kname, h, sym_id);
                    if (!p) p = js_add_own_prop(o, kname, sym_id);
                    if (p) {
                        p->flags = PROP_ENUM | PROP_CONF | PROP_ACC;
                        if (pr->op == 1) p->getter = vv.u.o;
                        else p->setter = vv.u.o;
                    }
                } else if (sym_id > 0) {
                    js_def_sym_prop(o, sym_id, vv);
                } else {
                    js_set_prop(ctx, jv_obj(o), kname, vv);
                }
            }
            return jv_obj(o);
        }
        case AST_MEMBER: {
            js_val_t obj = js_eval_node(ctx, n->a, env);
            if (ctx->throwing) return jv_undef();
            if ((n->flags & 2) && (obj.type == JS_UNDEF || obj.type == JS_NULL)) return jv_undef();
            if (n->flags & 1) {
                js_val_t key = js_eval_node(ctx, n->b, env);
                if (ctx->throwing) return jv_undef();
                return js_get_prop_v(ctx, obj, key);
            }
            return js_get_prop(ctx, obj, n->str ? n->str->data : "");
        }
        case AST_CALL:
        case AST_NEW: {
            int is_new = (n->kind == AST_NEW);
            js_val_t callee = jv_undef(), this_v = jv_undef();
            if (!is_new && n->a->kind == AST_SUPER) {
                js_obj_t *sc = js_find_super(env);
                if (!sc) { js_throw_err(ctx, "ReferenceError", "super is not available"); return jv_undef(); }
                callee = jv_obj(sc);
                this_v = js_find_this(ctx, env);
            } else if (!is_new && n->a->kind == AST_MEMBER) {
                js_ast_t *m = n->a;
                int is_super_m = (m->a->kind == AST_SUPER);
                js_val_t obj = js_eval_node(ctx, m->a, env);
                if (ctx->throwing) return jv_undef();
                if ((m->flags & 2) && (obj.type == JS_UNDEF || obj.type == JS_NULL)) return jv_undef();
                this_v = is_super_m ? js_find_this(ctx, env) : obj;
                if (m->flags & 1) callee = js_get_prop_v(ctx, obj, js_eval_node(ctx, m->b, env));
                else callee = js_get_prop(ctx, obj, m->str ? m->str->data : "");
            } else {
                callee = js_eval_node(ctx, n->a, env);
            }
            if (ctx->throwing) return jv_undef();
            if ((n->flags & 2) && (callee.type == JS_UNDEF || callee.type == JS_NULL)) return jv_undef();
            /* Tagged template literal */
            if (n->flags & 4) {
                js_ast_t *tm = n->b;
                js_obj_t *strs = js_arr_new(ctx, tm->list_n);
                js_val_t targs[32]; int targc = 0;
                targs[targc++] = jv_obj(strs);
                for (int i = 0; i < tm->list_n && !ctx->throwing; i++) {
                    if (i % 2 == 0) js_arr_push(strs, js_eval_node(ctx, tm->list[i], env));
                    else if (targc < 32) targs[targc++] = js_eval_node(ctx, tm->list[i], env);
                }
                return js_call_fn(ctx, callee, this_v, targs, targc, 0);
            }
            js_obj_t *av = js_arr_new(ctx, n->list_n);
            for (int i = 0; i < n->list_n && !ctx->throwing; i++) {
                js_ast_t *a = n->list[i];
                if (a->kind == AST_SPREAD) {
                    js_obj_t *it = js_to_iter_array(ctx, js_eval_node(ctx, a->a, env));
                    for (int k = 0; k < it->elem_n; k++) js_arr_push(av, it->elems[k]);
                } else {
                    js_arr_push(av, js_eval_node(ctx, a, env));
                }
            }
            if (ctx->throwing) return jv_undef();
            return js_call_fn(ctx, callee, this_v, av->elems, av->elem_n, is_new);
        }
        case AST_UNARY: {
            if (n->op == TK_TYPEOF) {
                if (n->a->kind == AST_ID && n->a->str) {
                    js_val_t v;
                    if (!js_env_get(ctx, env, n->a->str->data, &v)) return jv_cstr("undefined");
                    switch (v.type) {
                        case JS_UNDEF: return jv_cstr("undefined");
                        case JS_NULL: return jv_cstr("object");
                        case JS_BOOL: return jv_cstr("boolean");
                        case JS_NUM: return jv_cstr("number");
                        case JS_BIGINT: return jv_cstr("bigint");
                        case JS_STR: return jv_cstr("string");
                        case JS_SYM: return jv_cstr("symbol");
                        case JS_OBJ: return (v.u.o && (v.u.o->kind == OBJ_FUNC || v.u.o->kind == OBJ_NATIVE))
                                            ? jv_cstr("function") : jv_cstr("object");
                    }
                }
                js_val_t v = js_eval_node(ctx, n->a, env);
                switch (v.type) {
                    case JS_UNDEF: return jv_cstr("undefined");
                    case JS_NULL: return jv_cstr("object");
                    case JS_BOOL: return jv_cstr("boolean");
                    case JS_NUM: return jv_cstr("number");
                    case JS_BIGINT: return jv_cstr("bigint");
                    case JS_STR: return jv_cstr("string");
                    case JS_SYM: return jv_cstr("symbol");
                    case JS_OBJ: return (v.u.o && (v.u.o->kind == OBJ_FUNC || v.u.o->kind == OBJ_NATIVE))
                                        ? jv_cstr("function") : jv_cstr("object");
                }
            }
            if (n->op == TK_DELETE) {
                if (n->a->kind == AST_MEMBER) {
                    js_val_t o = js_eval_node(ctx, n->a->a, env);
                    js_val_t k = (n->a->flags & 1) ? js_eval_node(ctx, n->a->b, env) : jv_cstr(n->a->str ? n->a->str->data : "");
                    return jv_bool(js_del_prop_v(ctx, o, k));
                }
                return jv_bool(1);
            }
            js_val_t v = js_eval_node(ctx, n->a, env);
            if (ctx->throwing) return jv_undef();
            switch (n->op) {
                case TK_PLUS:  return jv_num(js_to_num(ctx, v));
                case TK_MINUS: return (v.type == JS_BIGINT) ? jv_bigint(-v.u.bi) : jv_num(-js_to_num(ctx, v));
                case TK_NOT:   return jv_bool(!js_to_bool(v));
                case TK_BNOT:  return (v.type == JS_BIGINT) ? jv_bigint(~v.u.bi) : jv_num((double)(~js_to_i32(ctx, v)));
                case TK_VOID:  return jv_undef();
            }
            return jv_undef();
        }
        case AST_UPDATE: {
            int delta = (n->op == TK_INC) ? 1 : -1;
            if (n->a->kind == AST_ID && n->a->str) {
                js_val_t cur;
                if (!js_env_get(ctx, env, n->a->str->data, &cur)) {
                    js_throw_err(ctx, "ReferenceError", n->a->str->data); return jv_undef();
                }
                double ov = js_to_num(ctx, cur);
                double nv = ov + (double)delta;
                js_env_set(ctx, env, n->a->str->data, jv_num(nv));
                return jv_num((n->flags & 1) ? nv : ov);
            }
            if (n->a->kind == AST_MEMBER) {
                js_val_t o = js_eval_node(ctx, n->a->a, env);
                js_val_t k = (n->a->flags & 1) ? js_eval_node(ctx, n->a->b, env) : jv_cstr(n->a->str ? n->a->str->data : "");
                double ov = js_to_num(ctx, js_get_prop_v(ctx, o, k));
                double nv = ov + (double)delta;
                js_set_prop_v(ctx, o, k, jv_num(nv));
                return jv_num((n->flags & 1) ? nv : ov);
            }
            return jv_num(js_nan());
        }
        case AST_BINARY: {
            js_val_t a = js_eval_node(ctx, n->a, env);
            if (ctx->throwing) return jv_undef();
            js_val_t b = js_eval_node(ctx, n->b, env);
            if (ctx->throwing) return jv_undef();
            return js_apply_binop(ctx, n->op, a, b);
        }
        case AST_LOGICAL: {
            js_val_t a = js_eval_node(ctx, n->a, env);
            if (ctx->throwing) return jv_undef();
            if (n->op == TK_AND) return js_to_bool(a) ? js_eval_node(ctx, n->b, env) : a;
            if (n->op == TK_OR) return js_to_bool(a) ? a : js_eval_node(ctx, n->b, env);
            if (n->op == TK_NULLISH) return (a.type == JS_NULL || a.type == JS_UNDEF) ? js_eval_node(ctx, n->b, env) : a;
            return jv_undef();
        }
        case AST_COND: {
            js_val_t c = js_eval_node(ctx, n->a, env);
            if (ctx->throwing) return jv_undef();
            return js_to_bool(c) ? js_eval_node(ctx, n->b, env) : js_eval_node(ctx, n->c, env);
        }
        case AST_ASSIGN: {
            if (n->op == TK_ASSIGN && (n->a->kind == AST_ARR || n->a->kind == AST_OBJ ||
                n->a->kind == AST_PAT_ARR || n->a->kind == AST_PAT_OBJ)) {
                js_val_t rv = js_eval_node(ctx, n->b, env);
                if (!ctx->throwing) js_bind_pattern(ctx, n->a, rv, env, -1);
                return rv;
            }
            if (n->a->kind == AST_ID && n->a->str) {
                const char *nm = n->a->str->data;
                if (n->op == TK_ASSIGN) {
                    js_val_t rv = js_eval_node(ctx, n->b, env);
                    if (!ctx->throwing) js_env_set(ctx, env, nm, rv);
                    return rv;
                }
                js_val_t cur;
                if (!js_env_get(ctx, env, nm, &cur)) cur = jv_undef();
                if (n->op == TK_LAND_EQ) { if (!js_to_bool(cur)) return cur; js_val_t rv = js_eval_node(ctx, n->b, env); js_env_set(ctx, env, nm, rv); return rv; }
                if (n->op == TK_LOR_EQ)  { if (js_to_bool(cur)) return cur; js_val_t rv = js_eval_node(ctx, n->b, env); js_env_set(ctx, env, nm, rv); return rv; }
                if (n->op == TK_NULLISH_EQ) { if (cur.type != JS_NULL && cur.type != JS_UNDEF) return cur; js_val_t rv = js_eval_node(ctx, n->b, env); js_env_set(ctx, env, nm, rv); return rv; }
                js_val_t rv = js_eval_node(ctx, n->b, env);
                js_val_t res = js_apply_binop(ctx, js_compound_to_bin(n->op), cur, rv);
                if (!ctx->throwing) js_env_set(ctx, env, nm, res);
                return res;
            }
            if (n->a->kind == AST_MEMBER) {
                js_val_t o = js_eval_node(ctx, n->a->a, env);
                js_val_t k = (n->a->flags & 1) ? js_eval_node(ctx, n->a->b, env) : jv_cstr(n->a->str ? n->a->str->data : "");
                if (ctx->throwing) return jv_undef();
                if (n->op == TK_ASSIGN) {
                    js_val_t rv = js_eval_node(ctx, n->b, env);
                    if (!ctx->throwing) js_set_prop_v(ctx, o, k, rv);
                    return rv;
                }
                js_val_t cur = js_get_prop_v(ctx, o, k);
                if (n->op == TK_LAND_EQ) { if (!js_to_bool(cur)) return cur; js_val_t rv = js_eval_node(ctx, n->b, env); js_set_prop_v(ctx, o, k, rv); return rv; }
                if (n->op == TK_LOR_EQ)  { if (js_to_bool(cur)) return cur; js_val_t rv = js_eval_node(ctx, n->b, env); js_set_prop_v(ctx, o, k, rv); return rv; }
                if (n->op == TK_NULLISH_EQ) { if (cur.type != JS_NULL && cur.type != JS_UNDEF) return cur; js_val_t rv = js_eval_node(ctx, n->b, env); js_set_prop_v(ctx, o, k, rv); return rv; }
                js_val_t rv = js_eval_node(ctx, n->b, env);
                js_val_t res = js_apply_binop(ctx, js_compound_to_bin(n->op), cur, rv);
                if (!ctx->throwing) js_set_prop_v(ctx, o, k, res);
                return res;
            }
            return jv_undef();
        }
        case AST_AWAIT: {
            js_val_t v = js_eval_node(ctx, n->a, env);
            if (ctx->throwing) return jv_undef();
            js_drain_microtasks(ctx);
            if (v.type == JS_OBJ && v.u.o && v.u.o->kind == OBJ_PROMISE) {
                if (v.u.o->prom_state == 1) return v.u.o->prom_val;
                if (v.u.o->prom_state == 2) { js_throw_val(ctx, v.u.o->prom_val); return jv_undef(); }
            }
            return v;
        }
        case AST_YIELD: {
            js_val_t v = n->a ? js_eval_node(ctx, n->a, env) : jv_undef();
            if (!ctx->in_gen) return v;
            if (n->flags & 1) { /* yield* */
                js_obj_t *it = js_to_iter_array(ctx, v);
                for (int k = 0; k < it->elem_n; k++) {
                    if (ctx->gen_cnt >= ctx->gen_cap) {
                        int nc = ctx->gen_cap ? ctx->gen_cap * 2 : 16;
                        ctx->gen_buf = (js_val_t *)js_realloc(ctx->gen_buf, (size_t)nc * sizeof(js_val_t));
                        ctx->gen_cap = ctx->gen_buf ? nc : 0;
                    }
                    if (ctx->gen_buf) ctx->gen_buf[ctx->gen_cnt++] = it->elems[k];
                }
            } else {
                if (ctx->gen_cnt >= ctx->gen_cap) {
                    int nc = ctx->gen_cap ? ctx->gen_cap * 2 : 16;
                    ctx->gen_buf = (js_val_t *)js_realloc(ctx->gen_buf, (size_t)nc * sizeof(js_val_t));
                    ctx->gen_cap = ctx->gen_buf ? nc : 0;
                }
                if (ctx->gen_buf) ctx->gen_buf[ctx->gen_cnt++] = v;
            }
            return jv_undef();
        }
        case AST_IF: {
            if (js_to_bool(js_eval_node(ctx, n->a, env))) return js_eval_node(ctx, n->b, env);
            else if (n->c) return js_eval_node(ctx, n->c, env);
            return jv_undef();
        }
        case AST_WHILE: {
            while (!ctx->throwing && !ctx->returning && js_to_bool(js_eval_node(ctx, n->a, env))) {
                js_eval_node(ctx, n->b, env);
                if (ctx->breaking) { if (!ctx->jump_label[0]) ctx->breaking = 0; break; }
                if (ctx->continuing) { if (!ctx->jump_label[0]) ctx->continuing = 0; else break; }
            }
            return jv_undef();
        }
        case AST_DO_WHILE: {
            do {
                js_eval_node(ctx, n->b, env);
                if (ctx->breaking) { if (!ctx->jump_label[0]) ctx->breaking = 0; break; }
                if (ctx->continuing) { if (!ctx->jump_label[0]) ctx->continuing = 0; else break; }
            } while (!ctx->throwing && !ctx->returning && js_to_bool(js_eval_node(ctx, n->a, env)));
            return jv_undef();
        }
        case AST_FOR: {
            js_env_t *fenv = js_env_new(ctx, env, 0);
            if (n->a) js_eval_node(ctx, n->a, fenv);
            int has_let = (n->a && n->a->kind == AST_VAR && n->a->op >= 1);
            while (!ctx->throwing && !ctx->returning) {
                if (n->b && !js_to_bool(js_eval_node(ctx, n->b, fenv))) break;
                js_env_t *iter_env = fenv;
                if (has_let) {
                    /* Fresh closure copy of loop let variables per iteration */
                    iter_env = js_env_new(ctx, fenv, 0);
                    for (int k = 0; k < fenv->vars->prop_n; k++) {
                        js_prop_t *p = &fenv->vars->props[k];
                        if (p->key) js_env_def(iter_env, p->key, p->val, !(p->flags & PROP_WRIT));
                    }
                }
                js_eval_node(ctx, n->d, iter_env);
                if (has_let) {
                    /* Copy back mutations to fenv for update/condition */
                    for (int k = 0; k < iter_env->vars->prop_n; k++) {
                        js_prop_t *p = &iter_env->vars->props[k];
                        if (p->key && fenv->vars) {
                            uint32_t h = js_hash_bytes(p->key, (int)js_strlen(p->key));
                            js_prop_t *fp = js_find_own_prop(fenv->vars, p->key, h, 0);
                            if (fp && (fp->flags & PROP_WRIT)) fp->val = p->val;
                        }
                    }
                }
                if (ctx->breaking) { if (!ctx->jump_label[0]) ctx->breaking = 0; break; }
                if (ctx->continuing) { if (!ctx->jump_label[0]) ctx->continuing = 0; else break; }
                if (n->c) js_eval_node(ctx, n->c, fenv);
            }
            return jv_undef();
        }
        case AST_FOR_IN: {
            js_val_t obj = js_eval_node(ctx, n->b, env);
            if (ctx->throwing || obj.type != JS_OBJ || !obj.u.o) return jv_undef();
            js_obj_t *keys = js_arr_new(ctx, 8);
            js_obj_t *o = obj.u.o;
            if (o->kind == OBJ_ARRAY) {
                for (int i = 0; i < o->elem_n; i++) { char kb[16]; js_i64toa(i, kb, sizeof(kb)); js_arr_push(keys, jv_cstr(kb)); }
            }
            for (int i = 0; i < o->prop_n; i++) {
                if ((o->props[i].flags & PROP_ENUM) && o->props[i].key)
                    js_arr_push(keys, jv_cstr(o->props[i].key));
            }
            for (int i = 0; i < keys->elem_n && !ctx->throwing && !ctx->returning; i++) {
                js_env_t *ienv = js_env_new(ctx, env, 0);
                js_bind_pattern(ctx, n->a, keys->elems[i], ienv, n->op);
                js_eval_node(ctx, n->c, ienv);
                if (ctx->breaking) { if (!ctx->jump_label[0]) ctx->breaking = 0; break; }
                if (ctx->continuing) { if (!ctx->jump_label[0]) ctx->continuing = 0; else break; }
            }
            return jv_undef();
        }
        case AST_FOR_OF: {
            js_val_t itv = js_eval_node(ctx, n->b, env);
            if (ctx->throwing) return jv_undef();
            js_obj_t *arr = js_to_iter_array(ctx, itv);
            for (int i = 0; i < arr->elem_n && !ctx->throwing && !ctx->returning; i++) {
                js_env_t *ienv = js_env_new(ctx, env, 0);
                js_bind_pattern(ctx, n->a, arr->elems[i], ienv, n->op);
                js_eval_node(ctx, n->c, ienv);
                if (ctx->breaking) { if (!ctx->jump_label[0]) ctx->breaking = 0; break; }
                if (ctx->continuing) { if (!ctx->jump_label[0]) ctx->continuing = 0; else break; }
            }
            return jv_undef();
        }
        case AST_SWITCH: {
            js_val_t disc = js_eval_node(ctx, n->a, env);
            if (ctx->throwing) return jv_undef();
            int matched = 0, def_idx = -1;
            for (int i = 0; i < n->list_n; i++) {
                js_ast_t *cs = n->list[i];
                if (!cs->a) { def_idx = i; continue; }
                if (!matched && js_strict_eq(disc, js_eval_node(ctx, cs->a, env))) matched = 1;
                if (matched) {
                    for (int k = 0; k < cs->list_n; k++) {
                        js_eval_node(ctx, cs->list[k], env);
                        if (ctx->throwing || ctx->returning || ctx->breaking || ctx->continuing) break;
                    }
                    if (ctx->breaking) { if (!ctx->jump_label[0]) ctx->breaking = 0; return jv_undef(); }
                    if (ctx->throwing || ctx->returning || ctx->continuing) return jv_undef();
                }
            }
            if (!matched && def_idx >= 0) {
                for (int i = def_idx; i < n->list_n; i++) {
                    js_ast_t *cs = n->list[i];
                    for (int k = 0; k < cs->list_n; k++) {
                        js_eval_node(ctx, cs->list[k], env);
                        if (ctx->throwing || ctx->returning || ctx->breaking || ctx->continuing) break;
                    }
                    if (ctx->breaking) { if (!ctx->jump_label[0]) ctx->breaking = 0; return jv_undef(); }
                    if (ctx->throwing || ctx->returning || ctx->continuing) return jv_undef();
                }
            }
            return jv_undef();
        }
        case AST_LABEL: {
            js_eval_node(ctx, n->a, env);
            if (ctx->breaking && n->str && js_strcmp(ctx->jump_label, n->str->data) == 0) {
                ctx->breaking = 0; ctx->jump_label[0] = 0;
            }
            if (ctx->continuing && n->str && js_strcmp(ctx->jump_label, n->str->data) == 0) {
                ctx->continuing = 0; ctx->jump_label[0] = 0;
            }
            return jv_undef();
        }
        case AST_RETURN: {
            js_val_t v = n->a ? js_eval_node(ctx, n->a, env) : jv_undef();
            if (!ctx->throwing) { ctx->returning = 1; ctx->ret_val = v; }
            return v;
        }
        case AST_THROW: {
            js_val_t v = js_eval_node(ctx, n->a, env);
            if (!ctx->throwing) js_throw_val(ctx, v);
            return jv_undef();
        }
        case AST_BREAK: {
            ctx->breaking = 1;
            bcopy_str(ctx->jump_label, n->str ? n->str->data : "", sizeof(ctx->jump_label));
            return jv_undef();
        }
        case AST_CONT: {
            ctx->continuing = 1;
            bcopy_str(ctx->jump_label, n->str ? n->str->data : "", sizeof(ctx->jump_label));
            return jv_undef();
        }
        case AST_TRY: {
            js_eval_node(ctx, n->a, env);
            if (ctx->throwing && n->b) {
                js_val_t ex = ctx->exc;
                ctx->throwing = 0; ctx->exc = jv_undef();
                js_env_t *cenv = js_env_new(ctx, env, 0);
                if (n->d) js_bind_pattern(ctx, n->d, ex, cenv, 1);
                js_eval_node(ctx, n->b, cenv);
            }
            if (n->c) {
                int st = ctx->throwing, sr = ctx->returning, sb = ctx->breaking, sc = ctx->continuing;
                js_val_t se = ctx->exc, srv = ctx->ret_val;
                ctx->throwing = 0; ctx->returning = 0; ctx->breaking = 0; ctx->continuing = 0;
                js_eval_node(ctx, n->c, env);
                if (!ctx->throwing && !ctx->returning) {
                    ctx->throwing = st; ctx->exc = se;
                    ctx->returning = sr; ctx->ret_val = srv;
                    ctx->breaking = sb; ctx->continuing = sc;
                }
            }
            return jv_undef();
        }
        default: return jv_undef();
    }
}

static js_val_t js_eval_source(js_ctx_t *ctx, const char *src, int len, js_env_t *env) {
    char err[128]; err[0] = 0;
    js_ast_t *prog = jp_parse_program(src, len, err, sizeof(err));
    if (!prog) {
        js_throw_err(ctx, "SyntaxError", err[0] ? err : "Syntax error");
        return jv_undef();
    }
    js_val_t r = js_eval_node(ctx, prog, env ? env : ctx->global_env);
    js_drain_microtasks(ctx);
    return r;
}

/* ==========================================================================
 * 5. MODERN ECMASCRIPT (ES6 - ES2024+) STANDART KUTUPHANE
 * ========================================================================== */

/* --- Global Yardimcilar: parseInt, parseFloat, URI, Base64, structuredClone --- */
static js_val_t jn_isNaN(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u; return jv_bool(n < 1 || js_isnan(js_to_num(ctx, a[0])));
}
static js_val_t jn_isFinite(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u; return jv_bool(n >= 1 && js_isfinite(js_to_num(ctx, a[0])));
}
static js_val_t jn_parseInt(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    if (n < 1) return jv_num(js_nan());
    js_str_t *s = js_to_str(ctx, a[0]);
    const char *p = s ? s->data : "";
    while (*p && js_is_space((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '+') p++;
    else if (*p == '-') { neg = 1; p++; }
    int radix = (n >= 2) ? js_to_i32(ctx, a[1]) : 0;
    if (radix == 0) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { radix = 16; p += 2; }
        else radix = 10;
    } else if (radix == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }
    if (radix < 2 || radix > 36) return jv_num(js_nan());
    double res = 0.0; int got = 0;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        int d = -1;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        if (d < 0 || d >= radix) break;
        res = res * (double)radix + (double)d;
        got = 1; p++;
    }
    if (!got) return jv_num(js_nan());
    return jv_num(neg ? -res : res);
}
static js_val_t jn_parseFloat(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    if (n < 1) return jv_num(js_nan());
    js_str_t *s = js_to_str(ctx, a[0]);
    const char *p = s ? s->data : "";
    while (*p && js_is_space((unsigned char)*p)) p++;
    const char *st = p;
    if (*p == '+' || *p == '-') p++;
    if (js_strncmp(p, "Infinity", 8) == 0) return jv_num(*st == '-' ? -js_inf() : js_inf());
    int has = 0;
    while (js_is_digit((unsigned char)*p)) { p++; has = 1; }
    if (*p == '.') { p++; while (js_is_digit((unsigned char)*p)) { p++; has = 1; } }
    if (!has) return jv_num(js_nan());
    if (*p == 'e' || *p == 'E') {
        const char *ep = p + 1;
        if (*ep == '+' || *ep == '-') ep++;
        if (js_is_digit((unsigned char)*ep)) { while (js_is_digit((unsigned char)*ep)) ep++; p = ep; }
    }
    return jv_num(js_str_to_num(st, (int)(p - st)));
}
static js_val_t jn_encodeURI(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; int comp = (int)(intptr_t)u;
    if (n < 1) return jv_cstr("undefined");
    js_str_t *s = js_to_str(ctx, a[0]);
    js_sb_t sb; js_sb_init(&sb);
    const char *hx = "0123456789ABCDEF";
    for (int i = 0; s && i < s->len; i++) {
        unsigned char c = (unsigned char)s->data[i];
        int safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '-' || c == '_' || c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' || c == ')';
        if (!comp && (c == ';' || c == ',' || c == '/' || c == '?' || c == ':' || c == '@' || c == '&' || c == '=' || c == '+' || c == '$' || c == '#'))
            safe = 1;
        if (safe) js_sb_putc(&sb, (char)c);
        else { js_sb_putc(&sb, '%'); js_sb_putc(&sb, hx[c >> 4]); js_sb_putc(&sb, hx[c & 15]); }
    }
    return jv_str(js_sb_finish(&sb));
}
static js_val_t jn_decodeURI(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    if (n < 1) return jv_cstr("undefined");
    js_str_t *s = js_to_str(ctx, a[0]);
    js_sb_t sb; js_sb_init(&sb);
    for (int i = 0; s && i < s->len; i++) {
        if (s->data[i] == '%' && i + 2 < s->len) {
            int h1 = js_hex_val((unsigned char)s->data[i + 1]), h2 = js_hex_val((unsigned char)s->data[i + 2]);
            if (h1 >= 0 && h2 >= 0) { js_sb_putc(&sb, (char)((h1 << 4) | h2)); i += 2; continue; }
        }
        js_sb_putc(&sb, s->data[i]);
    }
    return jv_str(js_sb_finish(&sb));
}
static js_val_t jn_btoa(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    js_str_t *s = (n >= 1) ? js_to_str(ctx, a[0]) : js_str_new("", 0);
    js_sb_t sb; js_sb_init(&sb);
    for (int i = 0; i < s->len; i += 3) {
        uint32_t v = ((uint32_t)(unsigned char)s->data[i]) << 16;
        if (i + 1 < s->len) v |= ((uint32_t)(unsigned char)s->data[i + 1]) << 8;
        if (i + 2 < s->len) v |= (uint32_t)(unsigned char)s->data[i + 2];
        js_sb_putc(&sb, b64[(v >> 18) & 63]);
        js_sb_putc(&sb, b64[(v >> 12) & 63]);
        js_sb_putc(&sb, (i + 1 < s->len) ? b64[(v >> 6) & 63] : '=');
        js_sb_putc(&sb, (i + 2 < s->len) ? b64[v & 63] : '=');
    }
    return jv_str(js_sb_finish(&sb));
}
static js_val_t jn_atob(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    js_str_t *s = (n >= 1) ? js_to_str(ctx, a[0]) : js_str_new("", 0);
    js_sb_t sb; js_sb_init(&sb);
    uint32_t buf = 0; int bits = 0;
    for (int i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->data[i];
        int v = -1;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') break;
        else continue;
        buf = (buf << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; js_sb_putc(&sb, (char)((buf >> bits) & 0xFF)); }
    }
    return jv_str(js_sb_finish(&sb));
}
static js_val_t jn_eval(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    if (n < 1) return jv_undef();
    if (a[0].type != JS_STR) return a[0];
    return js_eval_source(ctx, a[0].u.s->data, a[0].u.s->len, ctx->global_env);
}
static js_val_t jn_queueMicrotask(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    if (n >= 1 && a[0].type == JS_OBJ) js_enqueue_micro(ctx, a[0].u.o, jv_undef(), 0);
    return jv_undef();
}
static js_val_t js_deep_clone(js_ctx_t *ctx, js_val_t v, int depth) {
    if (depth > 32 || v.type != JS_OBJ || !v.u.o) return v;
    js_obj_t *o = v.u.o;
    if (o->kind == OBJ_ARRAY) {
        js_obj_t *na = js_arr_new(ctx, o->elem_n);
        for (int i = 0; i < o->elem_n; i++) js_arr_push(na, js_deep_clone(ctx, o->elems[i], depth + 1));
        return jv_obj(na);
    }
    if (o->kind == OBJ_DATE) {
        js_obj_t *nd = js_obj_new(ctx, OBJ_DATE, ctx->date_proto); nd->date_ms = o->date_ms; return jv_obj(nd);
    }
    if (o->kind == OBJ_PLAIN) {
        js_obj_t *no = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
        for (int i = 0; i < o->prop_n; i++)
            if ((o->props[i].flags & PROP_ENUM) && o->props[i].key)
                js_set_prop(ctx, jv_obj(no), o->props[i].key, js_deep_clone(ctx, o->props[i].val, depth + 1));
        return jv_obj(no);
    }
    return v;
}
static js_val_t jn_structuredClone(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    return n >= 1 ? js_deep_clone(ctx, a[0], 0) : jv_undef();
}

/* --- Object --- */
static js_val_t jn_obj_ctor(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    if (n >= 1 && a[0].type == JS_OBJ) return a[0];
    return jv_obj(js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto));
}
static js_val_t jn_obj_keys_vals_ent(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; int mode = (int)(intptr_t)u; /* 0=keys, 1=values, 2=entries, 3=ownPropNames */
    js_obj_t *res = js_arr_new(ctx, 8);
    if (n < 1) return jv_obj(res);
    if (a[0].type == JS_STR && a[0].u.s) {
        for (int i = 0; i < a[0].u.s->len; i++) {
            char kb[16]; js_i64toa(i, kb, sizeof(kb));
            js_val_t k = jv_cstr(kb), v = jv_cstr_n(a[0].u.s->data + i, 1);
            if (mode == 0 || mode == 3) js_arr_push(res, k);
            else if (mode == 1) js_arr_push(res, v);
            else { js_obj_t *pr = js_arr_new(ctx, 2); js_arr_push(pr, k); js_arr_push(pr, v); js_arr_push(res, jv_obj(pr)); }
        }
        return jv_obj(res);
    }
    if (a[0].type != JS_OBJ || !a[0].u.o) return jv_obj(res);
    js_obj_t *o = a[0].u.o;
    if (o->kind == OBJ_ARRAY) {
        for (int i = 0; i < o->elem_n; i++) {
            char kb[16]; js_i64toa(i, kb, sizeof(kb));
            js_val_t k = jv_cstr(kb), v = o->elems[i];
            if (mode == 0 || mode == 3) js_arr_push(res, k);
            else if (mode == 1) js_arr_push(res, v);
            else { js_obj_t *pr = js_arr_new(ctx, 2); js_arr_push(pr, k); js_arr_push(pr, v); js_arr_push(res, jv_obj(pr)); }
        }
        if (mode == 3) js_arr_push(res, jv_cstr("length"));
    }
    for (int i = 0; i < o->prop_n; i++) {
        if (!o->props[i].key || o->props[i].sym_id > 0) continue;
        if (mode != 3 && !(o->props[i].flags & PROP_ENUM)) continue;
        js_val_t k = jv_cstr(o->props[i].key);
        js_val_t v = ((o->props[i].flags & PROP_ACC) && o->props[i].getter)
            ? js_call_fn(ctx, jv_obj(o->props[i].getter), a[0], 0, 0, 0) : o->props[i].val;
        if (mode == 0 || mode == 3) js_arr_push(res, k);
        else if (mode == 1) js_arr_push(res, v);
        else { js_obj_t *pr = js_arr_new(ctx, 2); js_arr_push(pr, k); js_arr_push(pr, v); js_arr_push(res, jv_obj(pr)); }
    }
    return jv_obj(res);
}
static js_val_t jn_obj_fromEntries(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    js_obj_t *o = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    if (n < 1) return jv_obj(o);
    js_obj_t *it = js_to_iter_array(ctx, a[0]);
    for (int i = 0; i < it->elem_n; i++) {
        js_val_t k = js_get_prop(ctx, it->elems[i], "0");
        js_val_t v = js_get_prop(ctx, it->elems[i], "1");
        js_set_prop_v(ctx, jv_obj(o), k, v);
    }
    return jv_obj(o);
}
static js_val_t jn_obj_assign(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    if (n < 1 || a[0].type != JS_OBJ) return n >= 1 ? a[0] : jv_undef();
    for (int i = 1; i < n; i++) {
        if (a[i].type != JS_OBJ || !a[i].u.o) continue;
        js_obj_t *s = a[i].u.o;
        for (int k = 0; k < s->prop_n; k++) {
            if ((s->props[k].flags & PROP_ENUM) && s->props[k].key)
                js_set_prop(ctx, a[0], s->props[k].key, s->props[k].val);
        }
    }
    return a[0];
}
static js_val_t jn_obj_create(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    js_obj_t *pr = (n >= 1 && a[0].type == JS_OBJ) ? a[0].u.o : 0;
    js_obj_t *o = js_obj_new(ctx, OBJ_PLAIN, pr);
    if (n >= 1 && a[0].type == JS_NULL) o->proto = 0;
    return jv_obj(o);
}
static js_val_t jn_obj_freeze_seal(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)ctx; (void)th; int mode = (int)(intptr_t)u;
    if (n < 1 || a[0].type != JS_OBJ || !a[0].u.o) return (mode >= 2) ? jv_bool(1) : (n >= 1 ? a[0] : jv_undef());
    if (mode == 0) { a[0].u.o->frozen = 1; a[0].u.o->sealed = 1; return a[0]; }
    if (mode == 1) { a[0].u.o->sealed = 1; return a[0]; }
    if (mode == 2) return jv_bool(a[0].u.o->frozen);
    return jv_bool(a[0].u.o->sealed);
}
static js_val_t jn_obj_is(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)ctx; (void)th; (void)u;
    return jv_bool(js_same_value(n >= 1 ? a[0] : jv_undef(), n >= 2 ? a[1] : jv_undef()));
}
static js_val_t jn_obj_hasOwn(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    if (n < 2 || a[0].type != JS_OBJ || !a[0].u.o) return jv_bool(0);
    int sid = (a[1].type == JS_SYM) ? a[1].u.sym : 0;
    js_str_t *ks = sid ? 0 : js_to_str(ctx, a[1]);
    const char *k = ks ? ks->data : "";
    if (!sid && a[0].u.o->kind == OBJ_ARRAY) {
        int idx; if (js_key_is_index(k, &idx)) return jv_bool(idx >= 0 && idx < a[0].u.o->elem_n);
        if (js_strcmp(k, "length") == 0) return jv_bool(1);
    }
    uint32_t h = sid ? 0 : js_hash_bytes(k, (int)js_strlen(k));
    return jv_bool(js_find_own_prop(a[0].u.o, k, h, sid) != 0);
}
static js_val_t jn_obj_defineProperty(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    if (n < 3 || a[0].type != JS_OBJ || a[2].type != JS_OBJ) return n >= 1 ? a[0] : jv_undef();
    js_obj_t *o = a[0].u.o;
    int sid = (a[1].type == JS_SYM) ? a[1].u.sym : 0;
    js_str_t *ks = sid ? 0 : js_to_str(ctx, a[1]);
    const char *k = ks ? ks->data : "";
    uint32_t h = sid ? 0 : js_hash_bytes(k, (int)js_strlen(k));
    js_prop_t *p = js_find_own_prop(o, k, h, sid);
    if (!p) p = js_add_own_prop(o, k, sid);
    if (!p) return a[0];
    js_val_t g = js_get_prop(ctx, a[2], "get");
    js_val_t s = js_get_prop(ctx, a[2], "set");
    uint8_t fl = 0;
    if (js_to_bool(js_get_prop(ctx, a[2], "enumerable"))) fl |= PROP_ENUM;
    if (js_to_bool(js_get_prop(ctx, a[2], "configurable"))) fl |= PROP_CONF;
    if (g.type == JS_OBJ || s.type == JS_OBJ) {
        fl |= PROP_ACC;
        p->getter = (g.type == JS_OBJ) ? g.u.o : 0;
        p->setter = (s.type == JS_OBJ) ? s.u.o : 0;
    } else {
        if (js_to_bool(js_get_prop(ctx, a[2], "writable"))) fl |= PROP_WRIT;
        if (js_has_prop_v(ctx, a[2], jv_cstr("value"))) p->val = js_get_prop(ctx, a[2], "value");
    }
    p->flags = fl;
    return a[0];
}
static js_val_t jn_obj_getOwnPropDesc(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    if (n < 2 || a[0].type != JS_OBJ || !a[0].u.o) return jv_undef();
    int sid = (a[1].type == JS_SYM) ? a[1].u.sym : 0;
    js_str_t *ks = sid ? 0 : js_to_str(ctx, a[1]);
    const char *k = ks ? ks->data : "";
    uint32_t h = sid ? 0 : js_hash_bytes(k, (int)js_strlen(k));
    js_prop_t *p = js_find_own_prop(a[0].u.o, k, h, sid);
    if (!p) return jv_undef();
    js_obj_t *d = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    if (p->flags & PROP_ACC) {
        js_set_prop(ctx, jv_obj(d), "get", p->getter ? jv_obj(p->getter) : jv_undef());
        js_set_prop(ctx, jv_obj(d), "set", p->setter ? jv_obj(p->setter) : jv_undef());
    } else {
        js_set_prop(ctx, jv_obj(d), "value", p->val);
        js_set_prop(ctx, jv_obj(d), "writable", jv_bool((p->flags & PROP_WRIT) != 0));
    }
    js_set_prop(ctx, jv_obj(d), "enumerable", jv_bool((p->flags & PROP_ENUM) != 0));
    js_set_prop(ctx, jv_obj(d), "configurable", jv_bool((p->flags & PROP_CONF) != 0));
    return jv_obj(d);
}
static js_val_t jn_obj_getProto(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)ctx; (void)th; (void)u;
    if (n < 1 || a[0].type != JS_OBJ || !a[0].u.o) return jv_null();
    return a[0].u.o->proto ? jv_obj(a[0].u.o->proto) : jv_null();
}
static js_val_t jn_obj_setProto(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)ctx; (void)th; (void)u;
    if (n >= 2 && a[0].type == JS_OBJ && a[0].u.o)
        a[0].u.o->proto = (a[1].type == JS_OBJ) ? a[1].u.o : 0;
    return n >= 1 ? a[0] : jv_undef();
}
static js_val_t jn_obj_groupBy(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    js_obj_t *out = js_obj_new(ctx, OBJ_PLAIN, 0);
    if (n < 2) return jv_obj(out);
    js_obj_t *it = js_to_iter_array(ctx, a[0]);
    for (int i = 0; i < it->elem_n && !ctx->throwing; i++) {
        js_val_t ca[2] = { it->elems[i], jv_num((double)i) };
        js_val_t k = js_call_fn(ctx, a[1], jv_undef(), ca, 2, 0);
        js_str_t *ks = js_to_str(ctx, k);
        const char *kn = ks ? ks->data : "";
        js_val_t grp = js_get_prop(ctx, jv_obj(out), kn);
        if (grp.type != JS_OBJ || grp.u.o->kind != OBJ_ARRAY) {
            grp = jv_obj(js_arr_new(ctx, 4));
            js_set_prop(ctx, jv_obj(out), kn, grp);
        }
        js_arr_push(grp.u.o, it->elems[i]);
    }
    return jv_obj(out);
}
static js_val_t jn_obj_proto_hasOwn(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    js_val_t ca[2] = { th, n >= 1 ? a[0] : jv_undef() };
    return jn_obj_hasOwn(ctx, jv_undef(), ca, 2, u);
}
static js_val_t jn_obj_proto_toString(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)a; (void)n; (void)u;
    if (th.type == JS_UNDEF) return jv_cstr("[object Undefined]");
    if (th.type == JS_NULL) return jv_cstr("[object Null]");
    if (th.type == JS_BOOL) return jv_cstr("[object Boolean]");
    if (th.type == JS_NUM) return jv_cstr("[object Number]");
    if (th.type == JS_STR) return jv_cstr("[object String]");
    if (th.type == JS_OBJ && th.u.o) {
        if (ctx->sym_to_strtag > 0) {
            js_val_t tag = js_get_prop_v(ctx, th, jv_sym(ctx->sym_to_strtag));
            if (tag.type == JS_STR && tag.u.s) {
                char b[64]; bcopy_str(b, "[object ", sizeof(b)); bcat(b, tag.u.s->data, sizeof(b)); bcat(b, "]", sizeof(b));
                return jv_cstr(b);
            }
        }
        switch (th.u.o->kind) {
            case OBJ_ARRAY: return jv_cstr("[object Array]");
            case OBJ_FUNC: case OBJ_NATIVE: return jv_cstr("[object Function]");
            case OBJ_REGEXP: return jv_cstr("[object RegExp]");
            case OBJ_DATE: return jv_cstr("[object Date]");
            case OBJ_MAP: return jv_cstr("[object Map]");
            case OBJ_SET: return jv_cstr("[object Set]");
            case OBJ_PROMISE: return jv_cstr("[object Promise]");
            case OBJ_ERROR: return jv_cstr("[object Error]");
            default: return jv_cstr("[object Object]");
        }
    }
    return jv_cstr("[object Object]");
}
static js_val_t jn_obj_proto_valueOf(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)ctx; (void)a; (void)n; (void)u; return th;
}

/* --- Array (Tum ES2023/ES2024 metodlari dahil!) --- */
static js_val_t jn_arr_ctor(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    if (n == 1 && a[0].type == JS_NUM) {
        int len = (int)a[0].u.n; if (len < 0) len = 0; if (len > 100000) len = 100000;
        js_obj_t *r = js_arr_new(ctx, len);
        for (int i = 0; i < len; i++) js_arr_push(r, jv_undef());
        return jv_obj(r);
    }
    js_obj_t *r = js_arr_new(ctx, n);
    for (int i = 0; i < n; i++) js_arr_push(r, a[i]);
    return jv_obj(r);
}
static js_val_t jn_arr_isArray(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)ctx; (void)th; (void)u;
    return jv_bool(n >= 1 && a[0].type == JS_OBJ && a[0].u.o && a[0].u.o->kind == OBJ_ARRAY);
}
static js_val_t jn_arr_from(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    if (n < 1) return jv_obj(js_arr_new(ctx, 0));
    js_obj_t *r;
    if (a[0].type == JS_OBJ && a[0].u.o && a[0].u.o->kind == OBJ_PLAIN && js_has_prop_v(ctx, a[0], jv_cstr("length"))) {
        int len = (int)js_to_num(ctx, js_get_prop(ctx, a[0], "length"));
        if (len < 0) len = 0; if (len > 100000) len = 100000;
        r = js_arr_new(ctx, len);
        for (int i = 0; i < len; i++) {
            char kb[16]; js_i64toa(i, kb, sizeof(kb));
            js_arr_push(r, js_get_prop(ctx, a[0], kb));
        }
    } else {
        r = js_to_iter_array(ctx, a[0]);
    }
    if (n >= 2 && a[1].type == JS_OBJ) {
        js_val_t map_th = (n >= 3) ? a[2] : jv_undef();
        for (int i = 0; i < r->elem_n && !ctx->throwing; i++) {
            js_val_t ca[2] = { r->elems[i], jv_num((double)i) };
            r->elems[i] = js_call_fn(ctx, a[1], map_th, ca, 2, 0);
        }
    }
    return jv_obj(r);
}
static js_val_t jn_arr_of(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    js_obj_t *r = js_arr_new(ctx, n);
    for (int i = 0; i < n; i++) js_arr_push(r, a[i]);
    return jv_obj(r);
}
static js_val_t jn_arr_push(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)ctx; (void)u;
    if (th.type != JS_OBJ || th.u.o->kind != OBJ_ARRAY) return jv_num(0);
    for (int i = 0; i < n; i++) js_arr_push(th.u.o, a[i]);
    return jv_num((double)th.u.o->elem_n);
}
static js_val_t jn_arr_pop(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)ctx; (void)a; (void)n; (void)u;
    if (th.type != JS_OBJ || th.u.o->kind != OBJ_ARRAY || th.u.o->elem_n <= 0) return jv_undef();
    return th.u.o->elems[--th.u.o->elem_n];
}
static js_val_t jn_arr_shift(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)ctx; (void)a; (void)n; (void)u;
    if (th.type != JS_OBJ || th.u.o->kind != OBJ_ARRAY || th.u.o->elem_n <= 0) return jv_undef();
    js_obj_t *o = th.u.o;
    js_val_t first = o->elems[0];
    for (int i = 0; i < o->elem_n - 1; i++) o->elems[i] = o->elems[i + 1];
    o->elem_n--;
    return first;
}
static js_val_t jn_arr_unshift(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)ctx; (void)u;
    if (th.type != JS_OBJ || th.u.o->kind != OBJ_ARRAY) return jv_num(0);
    js_obj_t *o = th.u.o;
    for (int i = 0; i < n; i++) js_arr_push(o, jv_undef());
    for (int i = o->elem_n - 1; i >= n; i--) o->elems[i] = o->elems[i - n];
    for (int i = 0; i < n; i++) o->elems[i] = a[i];
    return jv_num((double)o->elem_n);
}
static js_val_t jn_arr_join(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)u;
    if (th.type != JS_OBJ || th.u.o->kind != OBJ_ARRAY) return jv_cstr("");
    js_str_t *sep = (n >= 1 && a[0].type != JS_UNDEF) ? js_to_str(ctx, a[0]) : js_str_new(",", 1);
    js_sb_t sb; js_sb_init(&sb);
    js_obj_t *o = th.u.o;
    for (int i = 0; i < o->elem_n; i++) {
        if (i > 0 && sep) js_sb_putn(&sb, sep->data, sep->len);
        if (o->elems[i].type != JS_UNDEF && o->elems[i].type != JS_NULL) {
            js_str_t *es = js_to_str(ctx, o->elems[i]);
            if (es) js_sb_putn(&sb, es->data, es->len);
        }
    }
    return jv_str(js_sb_finish(&sb));
}
static js_val_t jn_arr_slice(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)u;
    js_obj_t *r = js_arr_new(ctx, 4);
    if (th.type != JS_OBJ || th.u.o->kind != OBJ_ARRAY) return jv_obj(r);
    int len = th.u.o->elem_n;
    int s = (n >= 1 && a[0].type != JS_UNDEF) ? (int)js_to_num(ctx, a[0]) : 0;
    int e = (n >= 2 && a[1].type != JS_UNDEF) ? (int)js_to_num(ctx, a[1]) : len;
    if (s < 0) s = len + s; if (s < 0) s = 0; if (s > len) s = len;
    if (e < 0) e = len + e; if (e < 0) e = 0; if (e > len) e = len;
    for (int i = s; i < e; i++) js_arr_push(r, th.u.o->elems[i]);
    return jv_obj(r);
}
static js_val_t jn_arr_splice(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    int immut = (int)(intptr_t)u; /* 1 = toSpliced */
    if (th.type != JS_OBJ || th.u.o->kind != OBJ_ARRAY) return jv_obj(js_arr_new(ctx, 0));
    js_obj_t *o = th.u.o;
    int len = o->elem_n;
    int start = (n >= 1) ? (int)js_to_num(ctx, a[0]) : 0;
    if (start < 0) start = len + start; if (start < 0) start = 0; if (start > len) start = len;
    int del = (n >= 2) ? (int)js_to_num(ctx, a[1]) : (len - start);
    if (del < 0) del = 0; if (start + del > len) del = len - start;
    int ins = (n > 2) ? (n - 2) : 0;
    js_obj_t *removed = js_arr_new(ctx, del);
    for (int i = 0; i < del; i++) js_arr_push(removed, o->elems[start + i]);
    js_obj_t *nw = js_arr_new(ctx, len - del + ins);
    for (int i = 0; i < start; i++) js_arr_push(nw, o->elems[i]);
    for (int i = 0; i < ins; i++) js_arr_push(nw, a[2 + i]);
    for (int i = start + del; i < len; i++) js_arr_push(nw, o->elems[i]);
    if (immut) return jv_obj(nw);
    o->elem_n = 0;
    for (int i = 0; i < nw->elem_n; i++) js_arr_push(o, nw->elems[i]);
    return jv_obj(removed);
}
static js_val_t jn_arr_concat(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)u;
    js_obj_t *r = js_arr_new(ctx, 8);
    if (th.type == JS_OBJ && th.u.o->kind == OBJ_ARRAY)
        for (int i = 0; i < th.u.o->elem_n; i++) js_arr_push(r, th.u.o->elems[i]);
    else js_arr_push(r, th);
    for (int i = 0; i < n; i++) {
        if (a[i].type == JS_OBJ && a[i].u.o && a[i].u.o->kind == OBJ_ARRAY)
            for (int k = 0; k < a[i].u.o->elem_n; k++) js_arr_push(r, a[i].u.o->elems[k]);
        else js_arr_push(r, a[i]);
    }
    return jv_obj(r);
}
static js_val_t jn_arr_reverse(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)a; (void)n; int immut = (int)(intptr_t)u;
    if (th.type != JS_OBJ || th.u.o->kind != OBJ_ARRAY) return th;
    js_obj_t *o = th.u.o;
    if (immut) {
        js_obj_t *r = js_arr_new(ctx, o->elem_n);
        for (int i = o->elem_n - 1; i >= 0; i--) js_arr_push(r, o->elems[i]);
        return jv_obj(r);
    }
    for (int i = 0, j = o->elem_n - 1; i < j; i++, j--) {
        js_val_t t = o->elems[i]; o->elems[i] = o->elems[j]; o->elems[j] = t;
    }
    return th;
}
static js_val_t jn_arr_sort(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    int immut = (int)(intptr_t)u;
    if (th.type != JS_OBJ || th.u.o->kind != OBJ_ARRAY) return th;
    js_obj_t *o = immut ? js_arr_new(ctx, th.u.o->elem_n) : th.u.o;
    if (immut) for (int i = 0; i < th.u.o->elem_n; i++) js_arr_push(o, th.u.o->elems[i]);
    int has_cmp = (n >= 1 && a[0].type == JS_OBJ);
    /* Stable insertion/merge sort */
    for (int i = 1; i < o->elem_n && !ctx->throwing; i++) {
        js_val_t key = o->elems[i];
        int j = i - 1;
        while (j >= 0 && !ctx->throwing) {
            double c;
            if (has_cmp) {
                js_val_t ca[2] = { o->elems[j], key };
                c = js_to_num(ctx, js_call_fn(ctx, a[0], jv_undef(), ca, 2, 0));
            } else {
                js_str_t *sa = js_to_str(ctx, o->elems[j]), *sb = js_to_str(ctx, key);
                c = (double)js_strcmp(sa ? sa->data : "", sb ? sb->data : "");
            }
            if (c > 0.0) { o->elems[j + 1] = o->elems[j]; j--; }
            else break;
        }
        o->elems[j + 1] = key;
    }
    return jv_obj(o);
}
static js_val_t jn_arr_with(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)u;
    if (th.type != JS_OBJ || th.u.o->kind != OBJ_ARRAY) return th;
    js_obj_t *r = js_arr_new(ctx, th.u.o->elem_n);
    for (int i = 0; i < th.u.o->elem_n; i++) js_arr_push(r, th.u.o->elems[i]);
    int idx = (n >= 1) ? (int)js_to_num(ctx, a[0]) : 0;
    if (idx < 0) idx += r->elem_n;
    if (idx < 0 || idx >= r->elem_n) { js_throw_err(ctx, "RangeError", "Invalid index"); return jv_undef(); }
    r->elems[idx] = (n >= 2) ? a[1] : jv_undef();
    return jv_obj(r);
}
static js_val_t jn_arr_at(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)u;
    if (th.type != JS_OBJ || th.u.o->kind != OBJ_ARRAY) return jv_undef();
    int idx = (n >= 1) ? (int)js_to_num(ctx, a[0]) : 0;
    if (idx < 0) idx += th.u.o->elem_n;
    return (idx >= 0 && idx < th.u.o->elem_n) ? th.u.o->elems[idx] : jv_undef();
}
static js_val_t jn_arr_index(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    int mode = (int)(intptr_t)u; /* 0=indexOf, 1=lastIndexOf, 2=includes */
    if (th.type != JS_OBJ || th.u.o->kind != OBJ_ARRAY) return mode == 2 ? jv_bool(0) : jv_num(-1);
    js_val_t needle = (n >= 1) ? a[0] : jv_undef();
    int len = th.u.o->elem_n;
    if (mode == 1) {
        int from = (n >= 2) ? (int)js_to_num(ctx, a[1]) : len - 1;
        if (from < 0) from += len; if (from >= len) from = len - 1;
        for (int i = from; i >= 0; i--) if (js_strict_eq(th.u.o->elems[i], needle)) return jv_num((double)i);
        return jv_num(-1);
    }
    int from = (n >= 2) ? (int)js_to_num(ctx, a[1]) : 0;
    if (from < 0) from += len; if (from < 0) from = 0;
    for (int i = from; i < len; i++) {
        int eq = (mode == 2) ? js_same_value_zero(th.u.o->elems[i], needle) : js_strict_eq(th.u.o->elems[i], needle);
        if (eq) return mode == 2 ? jv_bool(1) : jv_num((double)i);
    }
    return mode == 2 ? jv_bool(0) : jv_num(-1);
}
static js_val_t jn_arr_hof(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    /* 0=forEach, 1=map, 2=filter, 3=find, 4=findIndex, 5=every, 6=some, 7=findLast, 8=findLastIndex, 9=flatMap */
    int mode = (int)(intptr_t)u;
    if (th.type != JS_OBJ || th.u.o->kind != OBJ_ARRAY || n < 1) return jv_undef();
    js_obj_t *o = th.u.o;
    js_val_t cb = a[0], cth = (n >= 2) ? a[1] : jv_undef();
    js_obj_t *res = (mode == 1 || mode == 2 || mode == 9) ? js_arr_new(ctx, o->elem_n) : 0;
    if (mode == 7 || mode == 8) {
        for (int i = o->elem_n - 1; i >= 0 && !ctx->throwing; i--) {
            js_val_t ca[3] = { o->elems[i], jv_num((double)i), th };
            if (js_to_bool(js_call_fn(ctx, cb, cth, ca, 3, 0))) return mode == 7 ? o->elems[i] : jv_num((double)i);
        }
        return mode == 7 ? jv_undef() : jv_num(-1);
    }
    for (int i = 0; i < o->elem_n && !ctx->throwing; i++) {
        js_val_t ca[3] = { o->elems[i], jv_num((double)i), th };
        js_val_t rv = js_call_fn(ctx, cb, cth, ca, 3, 0);
        if (ctx->throwing) return jv_undef();
        if (mode == 1) js_arr_push(res, rv);
        else if (mode == 2) { if (js_to_bool(rv)) js_arr_push(res, o->elems[i]); }
        else if (mode == 3) { if (js_to_bool(rv)) return o->elems[i]; }
        else if (mode == 4) { if (js_to_bool(rv)) return jv_num((double)i); }
        else if (mode == 5) { if (!js_to_bool(rv)) return jv_bool(0); }
        else if (mode == 6) { if (js_to_bool(rv)) return jv_bool(1); }
        else if (mode == 9) {
            if (rv.type == JS_OBJ && rv.u.o && rv.u.o->kind == OBJ_ARRAY)
                for (int k = 0; k < rv.u.o->elem_n; k++) js_arr_push(res, rv.u.o->elems[k]);
            else js_arr_push(res, rv);
        }
    }
    if (mode == 1 || mode == 2 || mode == 9) return jv_obj(res);
    if (mode == 4) return jv_num(-1);
    if (mode == 5) return jv_bool(1);
    if (mode == 6) return jv_bool(0);
    return jv_undef();
}
static js_val_t jn_arr_reduce(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    int right = (int)(intptr_t)u;
    if (th.type != JS_OBJ || th.u.o->kind != OBJ_ARRAY || n < 1) return jv_undef();
    js_obj_t *o = th.u.o;
    int has_init = (n >= 2);
    if (!has_init && o->elem_n == 0) { js_throw_err(ctx, "TypeError", "Reduce of empty array"); return jv_undef(); }
    js_val_t acc = has_init ? a[1] : (right ? o->elems[o->elem_n - 1] : o->elems[0]);
    if (right) {
        for (int i = has_init ? o->elem_n - 1 : o->elem_n - 2; i >= 0 && !ctx->throwing; i--) {
            js_val_t ca[4] = { acc, o->elems[i], jv_num((double)i), th };
            acc = js_call_fn(ctx, a[0], jv_undef(), ca, 4, 0);
        }
    } else {
        for (int i = has_init ? 0 : 1; i < o->elem_n && !ctx->throwing; i++) {
            js_val_t ca[4] = { acc, o->elems[i], jv_num((double)i), th };
            acc = js_call_fn(ctx, a[0], jv_undef(), ca, 4, 0);
        }
    }
    return acc;
}
static void js_flat_rec(js_obj_t *dst, js_obj_t *src, int depth) {
    for (int i = 0; i < src->elem_n; i++) {
        js_val_t v = src->elems[i];
        if (depth > 0 && v.type == JS_OBJ && v.u.o && v.u.o->kind == OBJ_ARRAY)
            js_flat_rec(dst, v.u.o, depth - 1);
        else js_arr_push(dst, v);
    }
}
static js_val_t jn_arr_flat(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)u;
    js_obj_t *r = js_arr_new(ctx, 8);
    if (th.type != JS_OBJ || th.u.o->kind != OBJ_ARRAY) return jv_obj(r);
    int d = (n >= 1 && a[0].type != JS_UNDEF) ? (int)js_to_num(ctx, a[0]) : 1;
    if (n >= 1 && a[0].type == JS_NUM && js_isinf(a[0].u.n)) d = 64;
    js_flat_rec(r, th.u.o, d);
    return jv_obj(r);
}
static js_val_t jn_arr_fill(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)u;
    if (th.type != JS_OBJ || th.u.o->kind != OBJ_ARRAY) return th;
    int len = th.u.o->elem_n;
    js_val_t v = n >= 1 ? a[0] : jv_undef();
    int s = (n >= 2 && a[1].type != JS_UNDEF) ? (int)js_to_num(ctx, a[1]) : 0;
    int e = (n >= 3 && a[2].type != JS_UNDEF) ? (int)js_to_num(ctx, a[2]) : len;
    if (s < 0) s += len; if (s < 0) s = 0; if (s > len) s = len;
    if (e < 0) e += len; if (e < 0) e = 0; if (e > len) e = len;
    for (int i = s; i < e; i++) th.u.o->elems[i] = v;
    return th;
}
static js_val_t jn_arr_kvent(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)a; (void)n; int mode = (int)(intptr_t)u; /* 0=keys, 1=values, 2=entries */
    js_obj_t *r = js_arr_new(ctx, 8);
    if (th.type != JS_OBJ || th.u.o->kind != OBJ_ARRAY) return jv_obj(r);
    for (int i = 0; i < th.u.o->elem_n; i++) {
        if (mode == 0) js_arr_push(r, jv_num((double)i));
        else if (mode == 1) js_arr_push(r, th.u.o->elems[i]);
        else { js_obj_t *pr = js_arr_new(ctx, 2); js_arr_push(pr, jv_num((double)i)); js_arr_push(pr, th.u.o->elems[i]); js_arr_push(r, jv_obj(pr)); }
    }
    return jv_obj(r);
}

/* --- String (Tum modern metodlar) --- */
static js_val_t jn_str_ctor(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u; return n >= 1 ? jv_str(js_to_str(ctx, a[0])) : jv_cstr("");
}
static js_val_t jn_str_fromCharCode(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    js_sb_t sb; js_sb_init(&sb);
    for (int i = 0; i < n; i++) jp_encode_utf8(&sb, (uint32_t)js_to_i32(ctx, a[i]));
    return jv_str(js_sb_finish(&sb));
}
static js_val_t jn_str_charAt(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    int mode = (int)(intptr_t)u; /* 0=charAt, 1=charCodeAt, 2=at */
    js_str_t *s = js_to_str(ctx, th);
    int idx = (n >= 1) ? (int)js_to_num(ctx, a[0]) : 0;
    if (mode == 2 && idx < 0) idx += s->len;
    if (idx < 0 || idx >= s->len) return (mode == 1) ? jv_num(js_nan()) : (mode == 2 ? jv_undef() : jv_cstr(""));
    if (mode == 1) return jv_num((double)(unsigned char)s->data[idx]);
    return jv_cstr_n(s->data + idx, 1);
}
static js_val_t jn_str_indexOf(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    int mode = (int)(intptr_t)u; /* 0=indexOf, 1=lastIndexOf, 2=includes, 3=startsWith, 4=endsWith */
    js_str_t *s = js_to_str(ctx, th);
    js_str_t *q = (n >= 1) ? js_to_str(ctx, a[0]) : js_str_new("undefined", 9);
    if (mode == 3) {
        int pos = (n >= 2) ? (int)js_to_num(ctx, a[1]) : 0;
        if (pos < 0) pos = 0;
        return jv_bool(pos + q->len <= s->len && js_memcmp(s->data + pos, q->data, (size_t)q->len) == 0);
    }
    if (mode == 4) {
        int end = (n >= 2 && a[1].type != JS_UNDEF) ? (int)js_to_num(ctx, a[1]) : s->len;
        if (end > s->len) end = s->len; if (end < 0) end = 0;
        return jv_bool(end >= q->len && js_memcmp(s->data + end - q->len, q->data, (size_t)q->len) == 0);
    }
    if (mode == 1) {
        int pos = (n >= 2 && a[1].type != JS_UNDEF) ? (int)js_to_num(ctx, a[1]) : s->len;
        if (pos > s->len - q->len) pos = s->len - q->len;
        for (int i = pos; i >= 0; i--)
            if (js_memcmp(s->data + i, q->data, (size_t)q->len) == 0) return jv_num((double)i);
        return jv_num(-1);
    }
    int pos = (n >= 2) ? (int)js_to_num(ctx, a[1]) : 0;
    if (pos < 0) pos = 0;
    for (int i = pos; i <= s->len - q->len; i++) {
        if (js_memcmp(s->data + i, q->data, (size_t)q->len) == 0)
            return mode == 2 ? jv_bool(1) : jv_num((double)i);
    }
    return mode == 2 ? jv_bool(0) : jv_num(-1);
}
static js_val_t jn_str_slice(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    int mode = (int)(intptr_t)u; /* 0=slice, 1=substring, 2=substr */
    js_str_t *s = js_to_str(ctx, th);
    int len = s->len;
    int st = (n >= 1 && a[0].type != JS_UNDEF) ? (int)js_to_num(ctx, a[0]) : 0;
    int en = (n >= 2 && a[1].type != JS_UNDEF) ? (int)js_to_num(ctx, a[1]) : len;
    if (mode == 0) {
        if (st < 0) st += len; if (st < 0) st = 0; if (st > len) st = len;
        if (en < 0) en += len; if (en < 0) en = 0; if (en > len) en = len;
    } else if (mode == 1) {
        if (st < 0) st = 0; if (st > len) st = len;
        if (en < 0) en = 0; if (en > len) en = len;
        if (st > en) { int t = st; st = en; en = t; }
    } else {
        if (st < 0) st += len; if (st < 0) st = 0; if (st > len) st = len;
        en = (n >= 2) ? st + en : len;
        if (en > len) en = len;
    }
    if (en <= st) return jv_cstr("");
    return jv_cstr_n(s->data + st, en - st);
}
static js_val_t jn_str_case(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)a; (void)n; int up = (int)(intptr_t)u;
    js_str_t *s = js_to_str(ctx, th);
    js_sb_t sb; js_sb_init(&sb);
    for (int i = 0; i < s->len; i++) {
        char c = s->data[i];
        if (up && c >= 'a' && c <= 'z') c = (char)(c - 32);
        else if (!up && c >= 'A' && c <= 'Z') c = (char)(c + 32);
        js_sb_putc(&sb, c);
    }
    return jv_str(js_sb_finish(&sb));
}
static js_val_t jn_str_trim(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)a; (void)n; int mode = (int)(intptr_t)u; /* 0=both, 1=start, 2=end */
    js_str_t *s = js_to_str(ctx, th);
    int st = 0, en = s->len;
    if (mode != 2) while (st < en && js_is_space((unsigned char)s->data[st])) st++;
    if (mode != 1) while (en > st && js_is_space((unsigned char)s->data[en - 1])) en--;
    return jv_cstr_n(s->data + st, en - st);
}
static js_val_t jn_str_pad(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    int end_pad = (int)(intptr_t)u;
    js_str_t *s = js_to_str(ctx, th);
    int target = (n >= 1) ? (int)js_to_num(ctx, a[0]) : 0;
    if (target <= s->len) return jv_str(s);
    js_str_t *pad = (n >= 2 && a[1].type != JS_UNDEF) ? js_to_str(ctx, a[1]) : js_str_new(" ", 1);
    if (!pad || pad->len == 0) return jv_str(s);
    int need = target - s->len;
    js_sb_t sb; js_sb_init(&sb);
    if (end_pad) js_sb_putn(&sb, s->data, s->len);
    for (int i = 0; i < need; i++) js_sb_putc(&sb, pad->data[i % pad->len]);
    if (!end_pad) js_sb_putn(&sb, s->data, s->len);
    return jv_str(js_sb_finish(&sb));
}
static js_val_t jn_str_repeat(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)u;
    js_str_t *s = js_to_str(ctx, th);
    int cnt = (n >= 1) ? (int)js_to_num(ctx, a[0]) : 0;
    if (cnt < 0 || cnt > 100000) { js_throw_err(ctx, "RangeError", "Invalid count"); return jv_undef(); }
    js_sb_t sb; js_sb_init(&sb);
    for (int i = 0; i < cnt; i++) js_sb_putn(&sb, s->data, s->len);
    return jv_str(js_sb_finish(&sb));
}
static js_val_t jn_str_concat(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)u;
    js_str_t *s = js_to_str(ctx, th);
    js_sb_t sb; js_sb_init(&sb);
    js_sb_putn(&sb, s->data, s->len);
    for (int i = 0; i < n; i++) { js_str_t *as = js_to_str(ctx, a[i]); js_sb_putn(&sb, as->data, as->len); }
    return jv_str(js_sb_finish(&sb));
}
static js_val_t jn_str_split(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)u;
    js_str_t *s = js_to_str(ctx, th);
    js_obj_t *r = js_arr_new(ctx, 8);
    int lim = (n >= 2 && a[1].type != JS_UNDEF) ? (int)js_to_num(ctx, a[1]) : 100000;
    if (n < 1 || a[0].type == JS_UNDEF) { js_arr_push(r, jv_str(s)); return jv_obj(r); }
    if (a[0].type == JS_OBJ && a[0].u.o && a[0].u.o->kind == OBJ_REGEXP) {
        js_obj_t *re = a[0].u.o;
        int pos = 0, cs[RE_MAX_CAPS], ce[RE_MAX_CAPS], nc = 0;
        while (pos <= s->len && r->elem_n < lim) {
            if (!js_re_exec_at(re->re_pat, re->re_flags, s->data, s->len, pos, cs, ce, &nc)) {
                js_arr_push(r, jv_cstr_n(s->data + pos, s->len - pos));
                break;
            }
            js_arr_push(r, jv_cstr_n(s->data + pos, cs[0] - pos));
            pos = (ce[0] > pos) ? ce[0] : (pos + 1);
            if (pos > s->len) break;
        }
        return jv_obj(r);
    }
    js_str_t *sep = js_to_str(ctx, a[0]);
    if (sep->len == 0) {
        for (int i = 0; i < s->len && r->elem_n < lim; i++) js_arr_push(r, jv_cstr_n(s->data + i, 1));
        return jv_obj(r);
    }
    int pos = 0;
    while (pos <= s->len && r->elem_n < lim) {
        int found = -1;
        for (int i = pos; i <= s->len - sep->len; i++) {
            if (js_memcmp(s->data + i, sep->data, (size_t)sep->len) == 0) { found = i; break; }
        }
        if (found < 0) { js_arr_push(r, jv_cstr_n(s->data + pos, s->len - pos)); break; }
        js_arr_push(r, jv_cstr_n(s->data + pos, found - pos));
        pos = found + sep->len;
    }
    return jv_obj(r);
}
static js_val_t jn_str_replace(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    int all = (int)(intptr_t)u;
    js_str_t *s = js_to_str(ctx, th);
    if (n < 2) return jv_str(s);
    int is_fn = (a[1].type == JS_OBJ && (a[1].u.o->kind == OBJ_FUNC || a[1].u.o->kind == OBJ_NATIVE));
    js_str_t *rep_s = is_fn ? 0 : js_to_str(ctx, a[1]);
    js_sb_t sb; js_sb_init(&sb);
    if (a[0].type == JS_OBJ && a[0].u.o && a[0].u.o->kind == OBJ_REGEXP) {
        js_obj_t *re = a[0].u.o;
        int is_g = all;
        for (int i = 0; re->re_flags[i]; i++) if (re->re_flags[i] == 'g') is_g = 1;
        int pos = 0, cs[RE_MAX_CAPS], ce[RE_MAX_CAPS], nc = 0;
        while (pos <= s->len) {
            if (!js_re_exec_at(re->re_pat, re->re_flags, s->data, s->len, pos, cs, ce, &nc)) {
                js_sb_putn(&sb, s->data + pos, s->len - pos);
                break;
            }
            js_sb_putn(&sb, s->data + pos, cs[0] - pos);
            if (is_fn) {
                js_val_t ca[RE_MAX_CAPS + 2]; int cn = 0;
                for (int k = 0; k < nc && cn < RE_MAX_CAPS; k++)
                    ca[cn++] = (cs[k] >= 0) ? jv_cstr_n(s->data + cs[k], ce[k] - cs[k]) : jv_undef();
                ca[cn++] = jv_num((double)cs[0]);
                ca[cn++] = jv_str(s);
                js_str_t *rv = js_to_str(ctx, js_call_fn(ctx, a[1], jv_undef(), ca, cn, 0));
                if (rv) js_sb_putn(&sb, rv->data, rv->len);
            } else if (rep_s) {
                for (int k = 0; k < rep_s->len; k++) {
                    if (rep_s->data[k] == '$' && k + 1 < rep_s->len) {
                        char nx = rep_s->data[k + 1];
                        if (nx == '$') { js_sb_putc(&sb, '$'); k++; continue; }
                        if (nx == '&') { js_sb_putn(&sb, s->data + cs[0], ce[0] - cs[0]); k++; continue; }
                        if (nx >= '1' && nx <= '9' && (nx - '0') < nc) {
                            int gi = nx - '0';
                            if (cs[gi] >= 0) js_sb_putn(&sb, s->data + cs[gi], ce[gi] - cs[gi]);
                            k++; continue;
                        }
                    }
                    js_sb_putc(&sb, rep_s->data[k]);
                }
            }
            pos = ce[0];
            if (!is_g) { js_sb_putn(&sb, s->data + pos, s->len - pos); break; }
            if (ce[0] == cs[0]) { if (pos < s->len) js_sb_putc(&sb, s->data[pos]); pos++; }
        }
        return jv_str(js_sb_finish(&sb));
    }
    js_str_t *pat = js_to_str(ctx, a[0]);
    if (pat->len == 0) {
        if (rep_s) js_sb_putn(&sb, rep_s->data, rep_s->len);
        js_sb_putn(&sb, s->data, s->len);
        return jv_str(js_sb_finish(&sb));
    }
    int pos = 0;
    while (pos <= s->len) {
        int found = -1;
        for (int i = pos; i <= s->len - pat->len; i++)
            if (js_memcmp(s->data + i, pat->data, (size_t)pat->len) == 0) { found = i; break; }
        if (found < 0) { js_sb_putn(&sb, s->data + pos, s->len - pos); break; }
        js_sb_putn(&sb, s->data + pos, found - pos);
        if (is_fn) {
            js_val_t ca[3] = { jv_str(pat), jv_num((double)found), jv_str(s) };
            js_str_t *rv = js_to_str(ctx, js_call_fn(ctx, a[1], jv_undef(), ca, 3, 0));
            if (rv) js_sb_putn(&sb, rv->data, rv->len);
        } else if (rep_s) js_sb_putn(&sb, rep_s->data, rep_s->len);
        pos = found + pat->len;
        if (!all) { js_sb_putn(&sb, s->data + pos, s->len - pos); break; }
    }
    return jv_str(js_sb_finish(&sb));
}
static js_val_t jn_str_match(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    int mode = (int)(intptr_t)u; /* 0=match, 1=search, 2=matchAll */
    js_str_t *s = js_to_str(ctx, th);
    const char *pat = "", *fl = "";
    if (n >= 1 && a[0].type == JS_OBJ && a[0].u.o->kind == OBJ_REGEXP) {
        pat = a[0].u.o->re_pat ? a[0].u.o->re_pat : "";
        fl = a[0].u.o->re_flags;
    } else if (n >= 1 && a[0].type != JS_UNDEF) {
        js_str_t *ps = js_to_str(ctx, a[0]); pat = ps ? ps->data : "";
    }
    int cs[RE_MAX_CAPS], ce[RE_MAX_CAPS], nc = 0;
    if (mode == 1) {
        return js_re_exec_at(pat, fl, s->data, s->len, 0, cs, ce, &nc) ? jv_num((double)cs[0]) : jv_num(-1);
    }
    int is_g = (mode == 2);
    for (int i = 0; fl[i]; i++) if (fl[i] == 'g') is_g = 1;
    if (is_g) {
        js_obj_t *r = js_arr_new(ctx, 4);
        int pos = 0;
        while (pos <= s->len && js_re_exec_at(pat, fl, s->data, s->len, pos, cs, ce, &nc)) {
            if (mode == 2) {
                js_obj_t *m = js_arr_new(ctx, nc);
                for (int k = 0; k < nc; k++)
                    js_arr_push(m, cs[k] >= 0 ? jv_cstr_n(s->data + cs[k], ce[k] - cs[k]) : jv_undef());
                js_set_prop(ctx, jv_obj(m), "index", jv_num((double)cs[0]));
                js_arr_push(r, jv_obj(m));
            } else {
                js_arr_push(r, jv_cstr_n(s->data + cs[0], ce[0] - cs[0]));
            }
            pos = (ce[0] > cs[0]) ? ce[0] : (ce[0] + 1);
        }
        if (mode == 0 && r->elem_n == 0) return jv_null();
        return jv_obj(r);
    }
    if (!js_re_exec_at(pat, fl, s->data, s->len, 0, cs, ce, &nc)) return jv_null();
    js_obj_t *m = js_arr_new(ctx, nc);
    for (int k = 0; k < nc; k++)
        js_arr_push(m, cs[k] >= 0 ? jv_cstr_n(s->data + cs[k], ce[k] - cs[k]) : jv_undef());
    js_set_prop(ctx, jv_obj(m), "index", jv_num((double)cs[0]));
    js_set_prop(ctx, jv_obj(m), "input", jv_str(s));
    return jv_obj(m);
}
static js_val_t jn_str_localeCompare(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)u;
    js_str_t *s1 = js_to_str(ctx, th), *s2 = (n >= 1) ? js_to_str(ctx, a[0]) : js_str_new("", 0);
    int c = js_strcmp(s1->data, s2->data);
    return jv_num(c < 0 ? -1.0 : (c > 0 ? 1.0 : 0.0));
}

/* --- Number, Boolean, BigInt, Symbol, Function --- */
static js_val_t jn_num_ctor(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u; return jv_num(n >= 1 ? js_to_num(ctx, a[0]) : 0.0);
}
static js_val_t jn_num_isInt(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)ctx; (void)th; int safe = (int)(intptr_t)u;
    if (n < 1 || a[0].type != JS_NUM || !js_isfinite(a[0].u.n)) return jv_bool(0);
    if (a[0].u.n != js_trunc(a[0].u.n)) return jv_bool(0);
    if (safe) return jv_bool(js_fabs(a[0].u.n) <= 9007199254740991.0);
    return jv_bool(1);
}
static js_val_t jn_num_isNaN_s(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)ctx; (void)th; (void)u; return jv_bool(n >= 1 && a[0].type == JS_NUM && js_isnan(a[0].u.n));
}
static js_val_t jn_num_isFin_s(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)ctx; (void)th; (void)u; return jv_bool(n >= 1 && a[0].type == JS_NUM && js_isfinite(a[0].u.n));
}
static js_val_t jn_num_toFixed(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)u;
    double v = js_to_num(ctx, th);
    if (!js_isfinite(v)) return jv_str(js_to_str(ctx, jv_num(v)));
    int d = (n >= 1) ? (int)js_to_num(ctx, a[0]) : 0;
    if (d < 0) d = 0; if (d > 15) d = 15;
    js_sb_t sb; js_sb_init(&sb);
    if (v < 0.0) { js_sb_putc(&sb, '-'); v = -v; }
    double p10 = js_pow(10.0, (double)d);
    uint64_t scaled = (uint64_t)(v * p10 + 0.5);
    uint64_t ip = (d > 0) ? (scaled / (uint64_t)p10) : scaled;
    uint64_t fp = (d > 0) ? (scaled % (uint64_t)p10) : 0;
    char ib[32]; int il = js_i64toa((int64_t)ip, ib, sizeof(ib));
    js_sb_putn(&sb, ib, il);
    if (d > 0) {
        js_sb_putc(&sb, '.');
        char fb[20];
        for (int i = d - 1; i >= 0; i--) { fb[i] = (char)('0' + (fp % 10)); fp /= 10; }
        js_sb_putn(&sb, fb, d);
    }
    return jv_str(js_sb_finish(&sb));
}
static js_val_t jn_num_toString(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)u;
    double v = js_to_num(ctx, th);
    int radix = (n >= 1 && a[0].type != JS_UNDEF) ? (int)js_to_num(ctx, a[0]) : 10;
    if (radix == 10 || !js_isfinite(v)) return jv_str(js_to_str(ctx, jv_num(v)));
    if (radix < 2 || radix > 36) { js_throw_err(ctx, "RangeError", "Invalid radix"); return jv_undef(); }
    static const char dg[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    int neg = v < 0.0; if (neg) v = -v;
    uint64_t iv = (uint64_t)v;
    char tmp[70]; int tn = 0;
    if (iv == 0) tmp[tn++] = '0';
    while (iv > 0 && tn < 68) { tmp[tn++] = dg[iv % (uint64_t)radix]; iv /= (uint64_t)radix; }
    js_sb_t sb; js_sb_init(&sb);
    if (neg) js_sb_putc(&sb, '-');
    while (tn > 0) js_sb_putc(&sb, tmp[--tn]);
    return jv_str(js_sb_finish(&sb));
}
static js_val_t jn_bool_ctor(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)ctx; (void)th; (void)u; return jv_bool(n >= 1 ? js_to_bool(a[0]) : 0);
}
static js_val_t jn_bigint_ctor(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    if (n < 1) return jv_bigint(0);
    if (a[0].type == JS_BIGINT) return a[0];
    return jv_bigint((int64_t)js_to_num(ctx, a[0]));
}
static js_val_t jn_sym_ctor(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    if (ctx->sym_n >= JS_MAX_SYMS) return jv_sym(ctx->sym_n);
    char *desc = 0;
    if (n >= 1 && a[0].type != JS_UNDEF) {
        js_str_t *s = js_to_str(ctx, a[0]);
        desc = (char *)js_malloc((size_t)s->len + 1);
        if (desc) { js_memcpy(desc, s->data, (size_t)s->len + 1); }
    }
    ctx->sym_desc[ctx->sym_n++] = desc;
    return jv_sym(ctx->sym_n);
}
static js_val_t jn_sym_for(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    js_str_t *s = (n >= 1) ? js_to_str(ctx, a[0]) : js_str_new("undefined", 9);
    for (int i = 0; i < ctx->sym_n; i++)
        if (ctx->sym_desc[i] && js_strcmp(ctx->sym_desc[i], s->data) == 0) return jv_sym(i + 1);
    return jn_sym_ctor(ctx, th, a, n, u);
}
static js_val_t jn_fn_call_apply_bind(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    int mode = (int)(intptr_t)u; /* 0=call, 1=apply, 2=bind */
    js_val_t this_v = (n >= 1) ? a[0] : jv_undef();
    if (mode == 0) return js_call_fn(ctx, th, this_v, n > 1 ? a + 1 : 0, n > 1 ? n - 1 : 0, 0);
    if (mode == 1) {
        if (n >= 2 && a[1].type == JS_OBJ && a[1].u.o) {
            js_obj_t *al = js_to_iter_array(ctx, a[1]);
            return js_call_fn(ctx, th, this_v, al->elems, al->elem_n, 0);
        }
        return js_call_fn(ctx, th, this_v, 0, 0, 0);
    }
    js_obj_t *bf = js_obj_new(ctx, OBJ_FUNC, ctx->fn_proto);
    bf->bound_target = (th.type == JS_OBJ) ? th.u.o : 0;
    bf->bound_this = this_v;
    if (n > 1) {
        bf->bound_argc = n - 1;
        bf->bound_args = (js_val_t *)js_malloc((size_t)(n - 1) * sizeof(js_val_t));
        for (int i = 0; i < n - 1; i++) bf->bound_args[i] = a[i + 1];
    }
    return jv_obj(bf);
}

/* --- Math (35+ metod + 8 sabit) --- */
static js_val_t jn_math_fn(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; int op = (int)(intptr_t)u;
    double x = (n >= 1) ? js_to_num(ctx, a[0]) : js_nan();
    double y = (n >= 2) ? js_to_num(ctx, a[1]) : js_nan();
    switch (op) {
        case 1: return jv_num(js_fabs(x));
        case 2: return jv_num(js_floor(x));
        case 3: return jv_num(js_ceil(x));
        case 4: return jv_num(js_round_js(x));
        case 5: return jv_num(js_trunc(x));
        case 6: return jv_num(js_isnan(x) ? js_nan() : (x > 0 ? 1.0 : (x < 0 ? -1.0 : x)));
        case 7: return jv_num(js_sqrt(x));
        case 8: return jv_num(js_cbrt(x));
        case 9: return jv_num(js_pow(x, y));
        case 10: return jv_num(js_exp(x));
        case 11: return jv_num(js_exp(x) - 1.0);
        case 12: return jv_num(js_log(x));
        case 13: return jv_num(js_log(x) / 2.30258509299404568402);
        case 14: return jv_num(js_log(x) / 0.693147180559945309417);
        case 15: return jv_num(js_log(1.0 + x));
        case 16: return jv_num(js_sin(x));
        case 17: return jv_num(js_cos(x));
        case 18: return jv_num(js_tan(x));
        case 19: return jv_num(js_asin(x));
        case 20: return jv_num(js_acos(x));
        case 21: return jv_num(js_atan(x));
        case 22: return jv_num(js_atan2(x, y));
        case 23: return jv_num(0.5 * (js_exp(x) - js_exp(-x)));
        case 24: return jv_num(0.5 * (js_exp(x) + js_exp(-x)));
        case 25: { double ep = js_exp(x), em = js_exp(-x); return jv_num((ep - em) / (ep + em)); }
        case 26: return jv_num(js_log(x + js_sqrt(x * x + 1.0)));
        case 27: return jv_num(x < 1.0 ? js_nan() : js_log(x + js_sqrt(x * x - 1.0)));
        case 28: return jv_num(0.5 * js_log((1.0 + x) / (1.0 - x)));
        case 29: return jv_num(js_random_num());
        case 30: { /* min */
            if (n == 0) return jv_num(js_inf());
            double m = js_to_num(ctx, a[0]);
            for (int i = 1; i < n; i++) { double v = js_to_num(ctx, a[i]); if (js_isnan(v)) return jv_num(js_nan()); if (v < m) m = v; }
            return jv_num(m);
        }
        case 31: { /* max */
            if (n == 0) return jv_num(-js_inf());
            double m = js_to_num(ctx, a[0]);
            for (int i = 1; i < n; i++) { double v = js_to_num(ctx, a[i]); if (js_isnan(v)) return jv_num(js_nan()); if (v > m) m = v; }
            return jv_num(m);
        }
        case 32: { /* hypot */
            double s = 0.0;
            for (int i = 0; i < n; i++) { double v = js_to_num(ctx, a[i]); s += v * v; }
            return jv_num(js_sqrt(s));
        }
        case 33: { /* imul */
            int32_t ia = js_to_i32(ctx, n >= 1 ? a[0] : jv_num(0));
            int32_t ib = js_to_i32(ctx, n >= 2 ? a[1] : jv_num(0));
            return jv_num((double)(int32_t)((uint32_t)ia * (uint32_t)ib));
        }
        case 34: { /* clz32 */
            uint32_t u32 = js_to_u32(ctx, n >= 1 ? a[0] : jv_num(0));
            if (u32 == 0) return jv_num(32.0);
            int c = 0; while (!(u32 & 0x80000000u)) { u32 <<= 1; c++; }
            return jv_num((double)c);
        }
        case 35: return jv_num((double)(float)x);
    }
    return jv_num(js_nan());
}

/* --- JSON (RFC 8259 parse + stringify) --- */
typedef struct { const char *s; int len; int pos; int err; } js_json_p_t;
static void json_ws(js_json_p_t *p) { while (p->pos < p->len && js_is_space((unsigned char)p->s[p->pos])) p->pos++; }
static js_val_t json_parse_val(js_ctx_t *ctx, js_json_p_t *p, int depth);
static js_str_t *json_parse_str(js_json_p_t *p) {
    if (p->pos >= p->len || p->s[p->pos] != '"') { p->err = 1; return 0; }
    p->pos++;
    js_sb_t sb; js_sb_init(&sb);
    while (p->pos < p->len && p->s[p->pos] != '"') {
        if (p->s[p->pos] == '\\' && p->pos + 1 < p->len) {
            p->pos++;
            js_parser_t tmp; js_memset(&tmp, 0, sizeof(tmp));
            tmp.src = p->s; tmp.len = p->len; tmp.pos = p->pos;
            jp_read_escape(&tmp, &sb);
            p->pos = tmp.pos;
        } else js_sb_putc(&sb, p->s[p->pos++]);
    }
    if (p->pos >= p->len || p->s[p->pos] != '"') { p->err = 1; return 0; }
    p->pos++;
    return js_sb_finish(&sb);
}
static js_val_t json_parse_val(js_ctx_t *ctx, js_json_p_t *p, int depth) {
    if (depth > 64) { p->err = 1; return jv_undef(); }
    json_ws(p);
    if (p->pos >= p->len) { p->err = 1; return jv_undef(); }
    char c = p->s[p->pos];
    if (c == '"') { js_str_t *s = json_parse_str(p); return s ? jv_str(s) : jv_undef(); }
    if (c == '{') {
        p->pos++; json_ws(p);
        js_obj_t *o = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
        if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; return jv_obj(o); }
        while (p->pos < p->len && !p->err) {
            json_ws(p);
            js_str_t *k = json_parse_str(p);
            if (!k) { p->err = 1; return jv_undef(); }
            json_ws(p);
            if (p->pos >= p->len || p->s[p->pos] != ':') { p->err = 1; return jv_undef(); }
            p->pos++;
            js_val_t v = json_parse_val(ctx, p, depth + 1);
            js_set_prop(ctx, jv_obj(o), k->data, v);
            json_ws(p);
            if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
            if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; break; }
            p->err = 1; break;
        }
        return jv_obj(o);
    }
    if (c == '[') {
        p->pos++; json_ws(p);
        js_obj_t *a = js_arr_new(ctx, 4);
        if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return jv_obj(a); }
        while (p->pos < p->len && !p->err) {
            js_arr_push(a, json_parse_val(ctx, p, depth + 1));
            json_ws(p);
            if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
            if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; break; }
            p->err = 1; break;
        }
        return jv_obj(a);
    }
    if (p->pos + 4 <= p->len && js_memcmp(p->s + p->pos, "true", 4) == 0) { p->pos += 4; return jv_bool(1); }
    if (p->pos + 5 <= p->len && js_memcmp(p->s + p->pos, "false", 5) == 0) { p->pos += 5; return jv_bool(0); }
    if (p->pos + 4 <= p->len && js_memcmp(p->s + p->pos, "null", 4) == 0) { p->pos += 4; return jv_null(); }
    int st = p->pos;
    if (p->s[p->pos] == '-') p->pos++;
    while (p->pos < p->len && js_is_digit((unsigned char)p->s[p->pos])) p->pos++;
    if (p->pos < p->len && p->s[p->pos] == '.') { p->pos++; while (p->pos < p->len && js_is_digit((unsigned char)p->s[p->pos])) p->pos++; }
    if (p->pos < p->len && (p->s[p->pos] == 'e' || p->s[p->pos] == 'E')) {
        p->pos++; if (p->pos < p->len && (p->s[p->pos] == '+' || p->s[p->pos] == '-')) p->pos++;
        while (p->pos < p->len && js_is_digit((unsigned char)p->s[p->pos])) p->pos++;
    }
    if (p->pos == st) { p->err = 1; return jv_undef(); }
    return jv_num(js_str_to_num(p->s + st, p->pos - st));
}
static js_val_t jn_json_parse(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    if (n < 1) { js_throw_err(ctx, "SyntaxError", "Unexpected end of JSON"); return jv_undef(); }
    js_str_t *s = js_to_str(ctx, a[0]);
    js_json_p_t p = { s->data, s->len, 0, 0 };
    js_val_t v = json_parse_val(ctx, &p, 0);
    json_ws(&p);
    if (p.err || p.pos != p.len) { js_throw_err(ctx, "SyntaxError", "Invalid JSON"); return jv_undef(); }
    return v;
}
static void json_esc_str(js_sb_t *sb, const char *s, int len) {
    js_sb_putc(sb, '"');
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"') js_sb_puts(sb, "\\\"");
        else if (c == '\\') js_sb_puts(sb, "\\\\");
        else if (c == '\n') js_sb_puts(sb, "\\n");
        else if (c == '\r') js_sb_puts(sb, "\\r");
        else if (c == '\t') js_sb_puts(sb, "\\t");
        else if (c < 0x20) {
            const char *hx = "0123456789abcdef";
            char u[7] = { '\\', 'u', '0', '0', hx[c >> 4], hx[c & 15], 0 };
            js_sb_putn(sb, u, 6);
        } else js_sb_putc(sb, (char)c);
    }
    js_sb_putc(sb, '"');
}
static void json_stringify_rec(js_ctx_t *ctx, js_val_t v, js_sb_t *sb, int indent, int depth) {
    if (depth > 32) { js_sb_puts(sb, "null"); return; }
    if (v.type == JS_OBJ && v.u.o) {
        js_val_t tj = js_get_prop(ctx, v, "toJSON");
        if (tj.type == JS_OBJ && (tj.u.o->kind == OBJ_FUNC || tj.u.o->kind == OBJ_NATIVE))
            v = js_call_fn(ctx, tj, v, 0, 0, 0);
    }
    switch (v.type) {
        case JS_NULL: case JS_UNDEF: js_sb_puts(sb, "null"); break;
        case JS_BOOL: js_sb_puts(sb, v.u.b ? "true" : "false"); break;
        case JS_NUM: {
            if (!js_isfinite(v.u.n)) { js_sb_puts(sb, "null"); break; }
            char b[48]; int n = js_dtoa(v.u.n, b, sizeof(b)); js_sb_putn(sb, b, n); break;
        }
        case JS_STR: json_esc_str(sb, v.u.s ? v.u.s->data : "", v.u.s ? v.u.s->len : 0); break;
        case JS_OBJ: {
            js_obj_t *o = v.u.o;
            if (o->kind == OBJ_ARRAY) {
                js_sb_putc(sb, '[');
                for (int i = 0; i < o->elem_n; i++) {
                    if (i > 0) js_sb_putc(sb, ',');
                    if (indent > 0) { js_sb_putc(sb, '\n'); for (int k = 0; k < (depth + 1) * indent; k++) js_sb_putc(sb, ' '); }
                    json_stringify_rec(ctx, o->elems[i], sb, indent, depth + 1);
                }
                if (indent > 0 && o->elem_n > 0) { js_sb_putc(sb, '\n'); for (int k = 0; k < depth * indent; k++) js_sb_putc(sb, ' '); }
                js_sb_putc(sb, ']');
            } else {
                js_sb_putc(sb, '{');
                int first = 1;
                for (int i = 0; i < o->prop_n; i++) {
                    if (!(o->props[i].flags & PROP_ENUM) || !o->props[i].key || o->props[i].sym_id > 0) continue;
                    js_val_t pv = o->props[i].val;
                    if (pv.type == JS_UNDEF || pv.type == JS_SYM || (pv.type == JS_OBJ && (pv.u.o->kind == OBJ_FUNC || pv.u.o->kind == OBJ_NATIVE))) continue;
                    if (!first) js_sb_putc(sb, ',');
                    first = 0;
                    if (indent > 0) { js_sb_putc(sb, '\n'); for (int k = 0; k < (depth + 1) * indent; k++) js_sb_putc(sb, ' '); }
                    json_esc_str(sb, o->props[i].key, (int)js_strlen(o->props[i].key));
                    js_sb_putc(sb, ':');
                    if (indent > 0) js_sb_putc(sb, ' ');
                    json_stringify_rec(ctx, pv, sb, indent, depth + 1);
                }
                if (indent > 0 && !first) { js_sb_putc(sb, '\n'); for (int k = 0; k < depth * indent; k++) js_sb_putc(sb, ' '); }
                js_sb_putc(sb, '}');
            }
            break;
        }
        default: js_sb_puts(sb, "null"); break;
    }
}
static js_val_t jn_json_stringify(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    if (n < 1 || a[0].type == JS_UNDEF) return jv_undef();
    int indent = (n >= 3 && a[2].type == JS_NUM) ? (int)a[2].u.n : 0;
    if (indent < 0) indent = 0; if (indent > 10) indent = 10;
    js_sb_t sb; js_sb_init(&sb);
    json_stringify_rec(ctx, a[0], &sb, indent, 0);
    return jv_str(js_sb_finish(&sb));
}

/* --- RegExp --- */
static js_val_t jn_re_ctor(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    js_obj_t *ro = js_obj_new(ctx, OBJ_REGEXP, ctx->re_proto);
    js_str_t *ps = (n >= 1 && a[0].type != JS_UNDEF) ? js_to_str(ctx, a[0]) : js_str_new("", 0);
    ro->re_pat = (char *)js_malloc((size_t)ps->len + 1);
    if (ro->re_pat) js_memcpy(ro->re_pat, ps->data, (size_t)ps->len + 1);
    if (n >= 2 && a[1].type != JS_UNDEF) {
        js_str_t *fs = js_to_str(ctx, a[1]);
        bcopy_str(ro->re_flags, fs ? fs->data : "", sizeof(ro->re_flags));
    }
    return jv_obj(ro);
}
static js_val_t jn_re_exec_test(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    int is_test = (int)(intptr_t)u;
    if (th.type != JS_OBJ || th.u.o->kind != OBJ_REGEXP) return is_test ? jv_bool(0) : jv_null();
    js_obj_t *ro = th.u.o;
    js_str_t *s = (n >= 1) ? js_to_str(ctx, a[0]) : js_str_new("", 0);
    int is_g = 0; for (int i = 0; ro->re_flags[i]; i++) if (ro->re_flags[i] == 'g' || ro->re_flags[i] == 'y') is_g = 1;
    int start = is_g ? ro->re_last_idx : 0;
    int cs[RE_MAX_CAPS], ce[RE_MAX_CAPS], nc = 0;
    if (!js_re_exec_at(ro->re_pat, ro->re_flags, s->data, s->len, start, cs, ce, &nc)) {
        if (is_g) ro->re_last_idx = 0;
        return is_test ? jv_bool(0) : jv_null();
    }
    if (is_g) ro->re_last_idx = ce[0];
    if (is_test) return jv_bool(1);
    js_obj_t *m = js_arr_new(ctx, nc);
    for (int k = 0; k < nc; k++)
        js_arr_push(m, cs[k] >= 0 ? jv_cstr_n(s->data + cs[k], ce[k] - cs[k]) : jv_undef());
    js_set_prop(ctx, jv_obj(m), "index", jv_num((double)cs[0]));
    js_set_prop(ctx, jv_obj(m), "input", jv_str(s));
    return jv_obj(m);
}

/* --- Map / Set (ES2024 Set metodlari + Map.groupBy dahil!) --- */
static int js_map_find(js_obj_t *o, js_val_t k) {
    for (int i = 0; i < o->map_n; i++)
        if (o->map_ents[i].used && js_same_value_zero(o->map_ents[i].key, k)) return i;
    return -1;
}
static void js_map_put(js_obj_t *o, js_val_t k, js_val_t v) {
    int idx = js_map_find(o, k);
    if (idx >= 0) { o->map_ents[idx].val = v; return; }
    if (o->map_n >= o->map_cap) {
        int nc = o->map_cap ? o->map_cap * 2 : 8;
        js_map_ent_t *ne = (js_map_ent_t *)js_realloc(o->map_ents, (size_t)nc * sizeof(js_map_ent_t));
        if (!ne) return;
        o->map_ents = ne; o->map_cap = nc;
    }
    o->map_ents[o->map_n].key = k;
    o->map_ents[o->map_n].val = v;
    o->map_ents[o->map_n].used = 1;
    o->map_n++;
}
static js_val_t jn_map_ctor(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    js_obj_t *m = js_obj_new(ctx, OBJ_MAP, ctx->map_proto);
    if (n >= 1 && a[0].type != JS_UNDEF && a[0].type != JS_NULL) {
        js_obj_t *it = js_to_iter_array(ctx, a[0]);
        for (int i = 0; i < it->elem_n; i++)
            js_map_put(m, js_get_prop(ctx, it->elems[i], "0"), js_get_prop(ctx, it->elems[i], "1"));
    }
    return jv_obj(m);
}
static js_val_t jn_map_ops(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    int op = (int)(intptr_t)u; /* 0=set,1=get,2=has,3=delete,4=clear,5=forEach,6=keys,7=values,8=entries */
    if (th.type != JS_OBJ || th.u.o->kind != OBJ_MAP) return jv_undef();
    js_obj_t *m = th.u.o;
    js_val_t k = n >= 1 ? a[0] : jv_undef();
    if (op == 0) { js_map_put(m, k, n >= 2 ? a[1] : jv_undef()); return th; }
    if (op == 1) { int i = js_map_find(m, k); return i >= 0 ? m->map_ents[i].val : jv_undef(); }
    if (op == 2) return jv_bool(js_map_find(m, k) >= 0);
    if (op == 3) { int i = js_map_find(m, k); if (i >= 0) { m->map_ents[i].used = 0; return jv_bool(1); } return jv_bool(0); }
    if (op == 4) { m->map_n = 0; return jv_undef(); }
    if (op == 5 && n >= 1) {
        for (int i = 0; i < m->map_n && !ctx->throwing; i++) {
            if (!m->map_ents[i].used) continue;
            js_val_t ca[3] = { m->map_ents[i].val, m->map_ents[i].key, th };
            js_call_fn(ctx, a[0], n >= 2 ? a[1] : jv_undef(), ca, 3, 0);
        }
        return jv_undef();
    }
    js_obj_t *r = js_arr_new(ctx, m->map_n);
    for (int i = 0; i < m->map_n; i++) {
        if (!m->map_ents[i].used) continue;
        if (op == 6) js_arr_push(r, m->map_ents[i].key);
        else if (op == 7) js_arr_push(r, m->map_ents[i].val);
        else { js_obj_t *pr = js_arr_new(ctx, 2); js_arr_push(pr, m->map_ents[i].key); js_arr_push(pr, m->map_ents[i].val); js_arr_push(r, jv_obj(pr)); }
    }
    return jv_obj(r);
}
static js_val_t jn_map_groupBy(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    js_obj_t *m = js_obj_new(ctx, OBJ_MAP, ctx->map_proto);
    if (n < 2) return jv_obj(m);
    js_obj_t *it = js_to_iter_array(ctx, a[0]);
    for (int i = 0; i < it->elem_n && !ctx->throwing; i++) {
        js_val_t ca[2] = { it->elems[i], jv_num((double)i) };
        js_val_t k = js_call_fn(ctx, a[1], jv_undef(), ca, 2, 0);
        int idx = js_map_find(m, k);
        js_obj_t *grp;
        if (idx >= 0) grp = m->map_ents[idx].val.u.o;
        else { grp = js_arr_new(ctx, 4); js_map_put(m, k, jv_obj(grp)); }
        js_arr_push(grp, it->elems[i]);
    }
    return jv_obj(m);
}
static js_val_t jn_set_ctor(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    js_obj_t *s = js_obj_new(ctx, OBJ_SET, ctx->set_proto);
    if (n >= 1 && a[0].type != JS_UNDEF && a[0].type != JS_NULL) {
        js_obj_t *it = js_to_iter_array(ctx, a[0]);
        for (int i = 0; i < it->elem_n; i++) js_map_put(s, it->elems[i], jv_bool(1));
    }
    return jv_obj(s);
}
static js_val_t jn_set_ops(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    /* 0=add,1=has,2=delete,3=clear,4=forEach,5=values,6=entries,7=union,8=intersection,9=difference,10=symDiff,11=isSubsetOf,12=isSupersetOf,13=isDisjointFrom */
    int op = (int)(intptr_t)u;
    if (th.type != JS_OBJ || th.u.o->kind != OBJ_SET) return jv_undef();
    js_obj_t *s = th.u.o;
    js_val_t k = n >= 1 ? a[0] : jv_undef();
    if (op == 0) { js_map_put(s, k, jv_bool(1)); return th; }
    if (op == 1) return jv_bool(js_map_find(s, k) >= 0);
    if (op == 2) { int i = js_map_find(s, k); if (i >= 0) { s->map_ents[i].used = 0; return jv_bool(1); } return jv_bool(0); }
    if (op == 3) { s->map_n = 0; return jv_undef(); }
    if (op == 4 && n >= 1) {
        for (int i = 0; i < s->map_n && !ctx->throwing; i++) {
            if (!s->map_ents[i].used) continue;
            js_val_t ca[3] = { s->map_ents[i].key, s->map_ents[i].key, th };
            js_call_fn(ctx, a[0], n >= 2 ? a[1] : jv_undef(), ca, 3, 0);
        }
        return jv_undef();
    }
    if (op == 5 || op == 6) {
        js_obj_t *r = js_arr_new(ctx, s->map_n);
        for (int i = 0; i < s->map_n; i++) {
            if (!s->map_ents[i].used) continue;
            if (op == 5) js_arr_push(r, s->map_ents[i].key);
            else { js_obj_t *pr = js_arr_new(ctx, 2); js_arr_push(pr, s->map_ents[i].key); js_arr_push(pr, s->map_ents[i].key); js_arr_push(r, jv_obj(pr)); }
        }
        return jv_obj(r);
    }
    /* ES2024 Set math operations */
    js_obj_t *other = (k.type == JS_OBJ && k.u.o && k.u.o->kind == OBJ_SET) ? k.u.o : 0;
    if (!other) {
        other = js_obj_new(ctx, OBJ_SET, ctx->set_proto);
        js_obj_t *it = js_to_iter_array(ctx, k);
        for (int i = 0; i < it->elem_n; i++) js_map_put(other, it->elems[i], jv_bool(1));
    }
    if (op == 7) {
        js_obj_t *r = js_obj_new(ctx, OBJ_SET, ctx->set_proto);
        for (int i = 0; i < s->map_n; i++) if (s->map_ents[i].used) js_map_put(r, s->map_ents[i].key, jv_bool(1));
        for (int i = 0; i < other->map_n; i++) if (other->map_ents[i].used) js_map_put(r, other->map_ents[i].key, jv_bool(1));
        return jv_obj(r);
    }
    if (op == 8) {
        js_obj_t *r = js_obj_new(ctx, OBJ_SET, ctx->set_proto);
        for (int i = 0; i < s->map_n; i++) if (s->map_ents[i].used && js_map_find(other, s->map_ents[i].key) >= 0) js_map_put(r, s->map_ents[i].key, jv_bool(1));
        return jv_obj(r);
    }
    if (op == 9) {
        js_obj_t *r = js_obj_new(ctx, OBJ_SET, ctx->set_proto);
        for (int i = 0; i < s->map_n; i++) if (s->map_ents[i].used && js_map_find(other, s->map_ents[i].key) < 0) js_map_put(r, s->map_ents[i].key, jv_bool(1));
        return jv_obj(r);
    }
    if (op == 10) {
        js_obj_t *r = js_obj_new(ctx, OBJ_SET, ctx->set_proto);
        for (int i = 0; i < s->map_n; i++) if (s->map_ents[i].used && js_map_find(other, s->map_ents[i].key) < 0) js_map_put(r, s->map_ents[i].key, jv_bool(1));
        for (int i = 0; i < other->map_n; i++) if (other->map_ents[i].used && js_map_find(s, other->map_ents[i].key) < 0) js_map_put(r, other->map_ents[i].key, jv_bool(1));
        return jv_obj(r);
    }
    if (op == 11) {
        for (int i = 0; i < s->map_n; i++) if (s->map_ents[i].used && js_map_find(other, s->map_ents[i].key) < 0) return jv_bool(0);
        return jv_bool(1);
    }
    if (op == 12) {
        for (int i = 0; i < other->map_n; i++) if (other->map_ents[i].used && js_map_find(s, other->map_ents[i].key) < 0) return jv_bool(0);
        return jv_bool(1);
    }
    for (int i = 0; i < s->map_n; i++) if (s->map_ents[i].used && js_map_find(other, s->map_ents[i].key) >= 0) return jv_bool(0);
    return jv_bool(1);
}

/* --- Promise (ES6 - ES2024 withResolvers dahil!) --- */
static js_val_t jn_prom_res_cb(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; return js_prom_resolve_val(ctx, (js_obj_t *)u, n >= 1 ? a[0] : jv_undef());
}
static js_val_t jn_prom_rej_cb(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; return js_prom_reject_val(ctx, (js_obj_t *)u, n >= 1 ? a[0] : jv_undef());
}
static js_val_t jn_prom_ctor(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    js_obj_t *p = js_obj_new(ctx, OBJ_PROMISE, ctx->prom_proto);
    if (n >= 1 && a[0].type == JS_OBJ) {
        js_val_t ca[2] = { jv_obj(js_native_new(ctx, jn_prom_res_cb, p, "resolve")),
                           jv_obj(js_native_new(ctx, jn_prom_rej_cb, p, "reject")) };
        js_call_fn(ctx, a[0], jv_undef(), ca, 2, 0);
        if (ctx->throwing) { js_val_t ex = ctx->exc; ctx->throwing = 0; js_prom_reject_val(ctx, p, ex); }
    }
    return jv_obj(p);
}
static js_val_t jn_prom_then(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    int mode = (int)(intptr_t)u; /* 0=then, 1=catch, 2=finally */
    js_obj_t *next = js_obj_new(ctx, OBJ_PROMISE, ctx->prom_proto);
    if (th.type != JS_OBJ || th.u.o->kind != OBJ_PROMISE) return jv_obj(next);
    js_obj_t *p = th.u.o;
    js_prom_cb_t *cb = (js_prom_cb_t *)js_calloc(1, sizeof(js_prom_cb_t));
    cb->next_prom = next;
    if (mode == 0) {
        if (n >= 1 && a[0].type == JS_OBJ) cb->on_fulfill = a[0].u.o;
        if (n >= 2 && a[1].type == JS_OBJ) cb->on_reject = a[1].u.o;
    } else if (mode == 1) {
        if (n >= 1 && a[0].type == JS_OBJ) cb->on_reject = a[0].u.o;
    } else {
        if (n >= 1 && a[0].type == JS_OBJ) { cb->on_fulfill = a[0].u.o; cb->on_reject = a[0].u.o; }
    }
    if (p->prom_state == 0) {
        /* Append to tail */
        js_prom_cb_t **pp = &p->prom_cbs;
        while (*pp) pp = &(*pp)->next;
        *pp = cb;
    } else {
        next->dom_idx = p->prom_state;
        js_obj_t *tramp = js_native_new(ctx, js_prom_run_cb_native, cb, "prom_cb");
        js_enqueue_micro(ctx, tramp, p->prom_val, 1);
    }
    return jv_obj(next);
}
static js_val_t jn_prom_static(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; int op = (int)(intptr_t)u; /* 0=resolve,1=reject,2=all,3=race,4=allSettled,5=any,6=withResolvers */
    if (op == 0) {
        if (n >= 1 && a[0].type == JS_OBJ && a[0].u.o->kind == OBJ_PROMISE) return a[0];
        js_obj_t *p = js_obj_new(ctx, OBJ_PROMISE, ctx->prom_proto);
        js_prom_resolve_val(ctx, p, n >= 1 ? a[0] : jv_undef());
        return jv_obj(p);
    }
    if (op == 1) {
        js_obj_t *p = js_obj_new(ctx, OBJ_PROMISE, ctx->prom_proto);
        js_prom_reject_val(ctx, p, n >= 1 ? a[0] : jv_undef());
        return jv_obj(p);
    }
    if (op == 6) {
        js_obj_t *p = js_obj_new(ctx, OBJ_PROMISE, ctx->prom_proto);
        js_obj_t *o = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
        js_set_prop(ctx, jv_obj(o), "promise", jv_obj(p));
        js_set_prop(ctx, jv_obj(o), "resolve", jv_obj(js_native_new(ctx, jn_prom_res_cb, p, "resolve")));
        js_set_prop(ctx, jv_obj(o), "reject", jv_obj(js_native_new(ctx, jn_prom_rej_cb, p, "reject")));
        return jv_obj(o);
    }
    js_obj_t *out_p = js_obj_new(ctx, OBJ_PROMISE, ctx->prom_proto);
    js_obj_t *it = (n >= 1) ? js_to_iter_array(ctx, a[0]) : js_arr_new(ctx, 0);
    js_drain_microtasks(ctx);
    if (op == 2) {
        js_obj_t *res = js_arr_new(ctx, it->elem_n);
        for (int i = 0; i < it->elem_n; i++) {
            js_val_t v = it->elems[i];
            if (v.type == JS_OBJ && v.u.o->kind == OBJ_PROMISE) {
                if (v.u.o->prom_state == 2) { js_prom_reject_val(ctx, out_p, v.u.o->prom_val); return jv_obj(out_p); }
                v = v.u.o->prom_val;
            }
            js_arr_push(res, v);
        }
        js_prom_resolve_val(ctx, out_p, jv_obj(res));
    } else if (op == 3) {
        if (it->elem_n > 0) {
            js_val_t v = it->elems[0];
            if (v.type == JS_OBJ && v.u.o->kind == OBJ_PROMISE && v.u.o->prom_state == 2) js_prom_reject_val(ctx, out_p, v.u.o->prom_val);
            else js_prom_resolve_val(ctx, out_p, (v.type == JS_OBJ && v.u.o->kind == OBJ_PROMISE) ? v.u.o->prom_val : v);
        }
    } else if (op == 4) {
        js_obj_t *res = js_arr_new(ctx, it->elem_n);
        for (int i = 0; i < it->elem_n; i++) {
            js_obj_t *st = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
            js_val_t v = it->elems[i];
            if (v.type == JS_OBJ && v.u.o->kind == OBJ_PROMISE && v.u.o->prom_state == 2) {
                js_set_prop(ctx, jv_obj(st), "status", jv_cstr("rejected"));
                js_set_prop(ctx, jv_obj(st), "reason", v.u.o->prom_val);
            } else {
                js_set_prop(ctx, jv_obj(st), "status", jv_cstr("fulfilled"));
                js_set_prop(ctx, jv_obj(st), "value", (v.type == JS_OBJ && v.u.o->kind == OBJ_PROMISE) ? v.u.o->prom_val : v);
            }
            js_arr_push(res, jv_obj(st));
        }
        js_prom_resolve_val(ctx, out_p, jv_obj(res));
    } else if (op == 5) {
        for (int i = 0; i < it->elem_n; i++) {
            js_val_t v = it->elems[i];
            if (v.type == JS_OBJ && v.u.o->kind == OBJ_PROMISE) {
                if (v.u.o->prom_state == 1) { js_prom_resolve_val(ctx, out_p, v.u.o->prom_val); return jv_obj(out_p); }
            } else { js_prom_resolve_val(ctx, out_p, v); return jv_obj(out_p); }
        }
        js_prom_reject_val(ctx, out_p, jv_cstr("All promises were rejected"));
    }
    return jv_obj(out_p);
}

/* --- Date --- */
static double js_now_ms(void) {
    if (g_basi && g_basi->time) {
        if (g_basi->time->unix_now) {
            uint64_t u = g_basi->time->unix_now();
            if (u > 1000000000ULL) return (double)u * 1000.0 + (double)(g_basi->time->ticks_ms ? (g_basi->time->ticks_ms() % 1000) : 0);
        }
        if (g_basi->time->ticks_ms) return 1763800000000.0 + (double)g_basi->time->ticks_ms();
    }
    return 1763800000000.0;
}
static void js_ms_to_ymd(double ms, int *Y, int *M, int *D, int *wd, int *h, int *mi, int *s, int *ms_r) {
    int64_t tot_ms = (int64_t)js_floor(ms);
    int rem = (int)(tot_ms % 1000); if (rem < 0) rem += 1000;
    int64_t sec = (tot_ms - rem) / 1000;
    int64_t days = sec >= 0 ? (sec / 86400) : ((sec - 86399) / 86400);
    int64_t sod = sec - days * 86400;
    *h = (int)(sod / 3600); *mi = (int)((sod % 3600) / 60); *s = (int)(sod % 60); *ms_r = rem;
    *wd = (int)((days + 4) % 7); if (*wd < 0) *wd += 7;
    /* Civil from days (Howard Hinnant algorithm) */
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *D = (int)(doy - (153 * mp + 2) / 5 + 1);
    *M = (int)(mp + (mp < 10 ? 3 : -9));
    *Y = (int)(y + (*M <= 2));
}
static double js_ymd_to_ms(int Y, int M, int D, int h, int mi, int s, int ms) {
    int64_t y = Y - (M <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;
    return (double)(days * 86400LL + h * 3600LL + mi * 60LL + s) * 1000.0 + (double)ms;
}
static js_val_t jn_date_ctor(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    js_obj_t *d = js_obj_new(ctx, OBJ_DATE, ctx->date_proto);
    if (n == 0) d->date_ms = js_now_ms();
    else if (n == 1) {
        if (a[0].type == JS_NUM) d->date_ms = a[0].u.n;
        else if (a[0].type == JS_OBJ && a[0].u.o->kind == OBJ_DATE) d->date_ms = a[0].u.o->date_ms;
        else {
            /* Parse ISO YYYY-MM-DD[THH:MM:SS[.mmm]Z] */
            js_str_t *s = js_to_str(ctx, a[0]);
            const char *p = s->data;
            int Y = 1970, M = 1, D = 1, h = 0, mi = 0, sc = 0, ms = 0;
            if (s->len >= 10 && p[4] == '-' && p[7] == '-') {
                Y = (int)js_str_to_num(p, 4); M = (int)js_str_to_num(p + 5, 2); D = (int)js_str_to_num(p + 8, 2);
                if (s->len >= 19 && (p[10] == 'T' || p[10] == ' ')) {
                    h = (int)js_str_to_num(p + 11, 2); mi = (int)js_str_to_num(p + 14, 2); sc = (int)js_str_to_num(p + 17, 2);
                    if (s->len >= 23 && p[19] == '.') ms = (int)js_str_to_num(p + 20, 3);
                }
                d->date_ms = js_ymd_to_ms(Y, M, D, h, mi, sc, ms);
            } else d->date_ms = js_to_num(ctx, a[0]);
        }
    } else {
        int Y = (int)js_to_num(ctx, a[0]); if (Y >= 0 && Y < 100) Y += 1900;
        int M = (int)js_to_num(ctx, a[1]) + 1;
        int D = (n >= 3) ? (int)js_to_num(ctx, a[2]) : 1;
        int h = (n >= 4) ? (int)js_to_num(ctx, a[3]) : 0;
        int mi = (n >= 5) ? (int)js_to_num(ctx, a[4]) : 0;
        int sc = (n >= 6) ? (int)js_to_num(ctx, a[5]) : 0;
        int ms = (n >= 7) ? (int)js_to_num(ctx, a[6]) : 0;
        d->date_ms = js_ymd_to_ms(Y, M, D, h, mi, sc, ms);
    }
    return jv_obj(d);
}
static js_val_t jn_date_now(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)ctx; (void)th; (void)a; (void)n; (void)u; return jv_num(js_now_ms());
}
static js_val_t jn_date_utc(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    js_val_t d = jn_date_ctor(ctx, th, a, n, u);
    return (d.type == JS_OBJ) ? jv_num(d.u.o->date_ms) : jv_num(js_nan());
}
static js_val_t jn_date_ops(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    int op = (int)(intptr_t)u;
    if (th.type != JS_OBJ || th.u.o->kind != OBJ_DATE) return jv_num(js_nan());
    js_obj_t *d = th.u.o;
    if (op == 0) return jv_num(d->date_ms);
    if (op == 10) { d->date_ms = (n >= 1) ? js_to_num(ctx, a[0]) : js_nan(); return jv_num(d->date_ms); }
    int Y, M, D, wd, h, mi, s, ms;
    js_ms_to_ymd(d->date_ms, &Y, &M, &D, &wd, &h, &mi, &s, &ms);
    switch (op) {
        case 1: return jv_num((double)Y);
        case 2: return jv_num((double)(M - 1));
        case 3: return jv_num((double)D);
        case 4: return jv_num((double)wd);
        case 5: return jv_num((double)h);
        case 6: return jv_num((double)mi);
        case 7: return jv_num((double)s);
        case 8: return jv_num((double)ms);
        case 9: {
            char b[32];
            int p = 0;
            b[p++] = (char)('0' + (Y / 1000) % 10); b[p++] = (char)('0' + (Y / 100) % 10);
            b[p++] = (char)('0' + (Y / 10) % 10); b[p++] = (char)('0' + Y % 10); b[p++] = '-';
            b[p++] = (char)('0' + M / 10); b[p++] = (char)('0' + M % 10); b[p++] = '-';
            b[p++] = (char)('0' + D / 10); b[p++] = (char)('0' + D % 10); b[p++] = 'T';
            b[p++] = (char)('0' + h / 10); b[p++] = (char)('0' + h % 10); b[p++] = ':';
            b[p++] = (char)('0' + mi / 10); b[p++] = (char)('0' + mi % 10); b[p++] = ':';
            b[p++] = (char)('0' + s / 10); b[p++] = (char)('0' + s % 10); b[p++] = '.';
            b[p++] = (char)('0' + (ms / 100) % 10); b[p++] = (char)('0' + (ms / 10) % 10); b[p++] = (char)('0' + ms % 10);
            b[p++] = 'Z'; b[p] = 0;
            return jv_cstr_n(b, p);
        }
    }
    return jv_num(0);
}

/* --- Error, Proxy, Reflect, ArrayBuffer, TypedArray, TextEncoder/Decoder, URL --- */
static js_val_t jn_err_ctor(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; const char *kind = (const char *)u;
    js_obj_t *e = js_obj_new(ctx, OBJ_ERROR, ctx->err_proto);
    js_def_prop(e, "name", jv_cstr(kind ? kind : "Error"), PROP_WRIT | PROP_CONF);
    js_def_prop(e, "message", n >= 1 ? jv_str(js_to_str(ctx, a[0])) : jv_cstr(""), PROP_WRIT | PROP_CONF);
    return jv_obj(e);
}
static js_val_t jn_err_toString(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)a; (void)n; (void)u;
    js_str_t *nm = js_to_str(ctx, js_get_prop(ctx, th, "name"));
    js_str_t *ms = js_to_str(ctx, js_get_prop(ctx, th, "message"));
    js_sb_t sb; js_sb_init(&sb);
    js_sb_putn(&sb, nm->data, nm->len);
    if (ms && ms->len > 0) { js_sb_puts(&sb, ": "); js_sb_putn(&sb, ms->data, ms->len); }
    return jv_str(js_sb_finish(&sb));
}
static js_val_t jn_proxy_ctor(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    if (n < 2 || a[0].type != JS_OBJ || a[1].type != JS_OBJ) {
        js_throw_err(ctx, "TypeError", "Proxy requires target and handler objects");
        return jv_undef();
    }
    js_obj_t *p = js_obj_new(ctx, OBJ_PROXY, a[0].u.o->proto);
    p->proxy_target = a[0].u.o;
    p->proxy_handler = a[1].u.o;
    return jv_obj(p);
}
static js_val_t jn_reflect_ops(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; int op = (int)(intptr_t)u; /* 0=get,1=set,2=has,3=deleteProperty,4=apply,5=construct,6=ownKeys */
    if (op == 0) return n >= 2 ? js_get_prop_v(ctx, a[0], a[1]) : jv_undef();
    if (op == 1) { if (n >= 3) js_set_prop_v(ctx, a[0], a[1], a[2]); return jv_bool(1); }
    if (op == 2) return jv_bool(n >= 2 && js_has_prop_v(ctx, a[0], a[1]));
    if (op == 3) return jv_bool(n >= 2 && js_del_prop_v(ctx, a[0], a[1]));
    if (op == 4 && n >= 3) {
        js_obj_t *al = js_to_iter_array(ctx, a[2]);
        return js_call_fn(ctx, a[0], a[1], al->elems, al->elem_n, 0);
    }
    if (op == 5 && n >= 2) {
        js_obj_t *al = js_to_iter_array(ctx, a[1]);
        return js_call_fn(ctx, a[0], jv_undef(), al->elems, al->elem_n, 1);
    }
    if (op == 6 && n >= 1) return jn_obj_keys_vals_ent(ctx, jv_undef(), a, 1, (void *)3);
    return jv_undef();
}
static js_val_t jn_ab_ctor(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    int len = (n >= 1) ? (int)js_to_num(ctx, a[0]) : 0;
    if (len < 0) len = 0; if (len > 4 * 1024 * 1024) len = 4 * 1024 * 1024;
    js_obj_t *ab = js_obj_new(ctx, OBJ_ARRAYBUFFER, ctx->ab_proto);
    ab->buf = (uint8_t *)js_calloc(1, (size_t)len + 1);
    ab->buf_len = ab->buf ? len : 0;
    return jv_obj(ab);
}
static js_val_t jn_ta_ctor(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; int kind = (int)(intptr_t)u;
    int es = js_ta_elem_size(kind);
    js_obj_t *ta = js_obj_new(ctx, OBJ_TYPEDARRAY, ctx->ta_proto);
    ta->ta_kind = kind;
    if (n >= 1 && a[0].type == JS_OBJ && a[0].u.o->kind == OBJ_ARRAYBUFFER) {
        ta->ta_ab = a[0].u.o;
        ta->buf = a[0].u.o->buf;
        ta->ta_off = (n >= 2) ? (int)js_to_num(ctx, a[1]) : 0;
        int bl = (n >= 3) ? (int)js_to_num(ctx, a[2]) * es : (a[0].u.o->buf_len - ta->ta_off);
        ta->buf_len = ta->ta_off + bl;
    } else if (n >= 1 && a[0].type == JS_OBJ) {
        js_obj_t *it = js_to_iter_array(ctx, a[0]);
        js_obj_t *ab = js_obj_new(ctx, OBJ_ARRAYBUFFER, ctx->ab_proto);
        ab->buf_len = it->elem_n * es;
        ab->buf = (uint8_t *)js_calloc(1, (size_t)ab->buf_len + 1);
        ta->ta_ab = ab; ta->buf = ab->buf; ta->buf_len = ab->buf_len;
        for (int i = 0; i < it->elem_n; i++) js_ta_set(ta, i, js_to_num(ctx, it->elems[i]));
    } else {
        int len = (n >= 1) ? (int)js_to_num(ctx, a[0]) : 0;
        if (len < 0) len = 0; if (len > 500000) len = 500000;
        js_obj_t *ab = js_obj_new(ctx, OBJ_ARRAYBUFFER, ctx->ab_proto);
        ab->buf_len = len * es;
        ab->buf = (uint8_t *)js_calloc(1, (size_t)ab->buf_len + 1);
        ta->ta_ab = ab; ta->buf = ab->buf; ta->buf_len = ab->buf_len;
    }
    return jv_obj(ta);
}
static js_val_t jn_ta_subarray_slice(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    int copy = (int)(intptr_t)u;
    if (th.type != JS_OBJ || th.u.o->kind != OBJ_TYPEDARRAY) return jv_undef();
    js_obj_t *o = th.u.o; int es = js_ta_elem_size(o->ta_kind);
    int len = (o->buf_len - o->ta_off) / es;
    int s = (n >= 1) ? (int)js_to_num(ctx, a[0]) : 0;
    int e = (n >= 2) ? (int)js_to_num(ctx, a[1]) : len;
    if (s < 0) s += len; if (s < 0) s = 0; if (s > len) s = len;
    if (e < 0) e += len; if (e < 0) e = 0; if (e > len) e = len;
    int nl = (e > s) ? (e - s) : 0;
    js_obj_t *r = js_obj_new(ctx, OBJ_TYPEDARRAY, ctx->ta_proto);
    r->ta_kind = o->ta_kind;
    if (copy) {
        js_obj_t *ab = js_obj_new(ctx, OBJ_ARRAYBUFFER, ctx->ab_proto);
        ab->buf_len = nl * es; ab->buf = (uint8_t *)js_calloc(1, (size_t)ab->buf_len + 1);
        if (ab->buf && nl > 0) js_memcpy(ab->buf, o->buf + o->ta_off + s * es, (size_t)(nl * es));
        r->ta_ab = ab; r->buf = ab->buf; r->buf_len = ab->buf_len;
    } else {
        r->ta_ab = o->ta_ab; r->buf = o->buf; r->ta_off = o->ta_off + s * es; r->buf_len = o->ta_off + e * es;
    }
    return jv_obj(r);
}
static js_val_t jn_text_encode(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    js_str_t *s = (n >= 1) ? js_to_str(ctx, a[0]) : js_str_new("", 0);
    js_obj_t *ab = js_obj_new(ctx, OBJ_ARRAYBUFFER, ctx->ab_proto);
    ab->buf_len = s->len; ab->buf = (uint8_t *)js_malloc((size_t)s->len + 1);
    if (ab->buf && s->len > 0) js_memcpy(ab->buf, s->data, (size_t)s->len);
    js_obj_t *ta = js_obj_new(ctx, OBJ_TYPEDARRAY, ctx->ta_proto);
    ta->ta_kind = 0; ta->ta_ab = ab; ta->buf = ab->buf; ta->buf_len = ab->buf_len;
    return jv_obj(ta);
}
static js_val_t jn_text_decode(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)ctx; (void)th; (void)u;
    if (n < 1 || a[0].type != JS_OBJ || !a[0].u.o) return jv_cstr("");
    js_obj_t *o = a[0].u.o;
    if ((o->kind == OBJ_TYPEDARRAY || o->kind == OBJ_ARRAYBUFFER) && o->buf)
        return jv_cstr_n((const char *)(o->buf + o->ta_off), o->buf_len - o->ta_off);
    return jv_cstr("");
}
static js_val_t jn_te_ctor(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)a; (void)n; int dec = (int)(intptr_t)u;
    js_obj_t *o = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    if (dec) js_def_native(ctx, o, "decode", jn_text_decode, 0);
    else js_def_native(ctx, o, "encode", jn_text_encode, 0);
    return jv_obj(o);
}

/* --- URL & URLSearchParams --- */
static js_val_t jn_urlp_ctor(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    js_obj_t *o = js_obj_new(ctx, OBJ_URLPARAMS, ctx->urlp_proto);
    if (n >= 1 && a[0].type == JS_STR && a[0].u.s) {
        const char *p = a[0].u.s->data;
        if (*p == '?') p++;
        while (*p) {
            const char *ks = p;
            while (*p && *p != '=' && *p != '&') p++;
            js_val_t kv = jv_cstr_n(ks, (int)(p - ks));
            js_val_t vv = jv_cstr("");
            if (*p == '=') {
                p++; const char *vs = p;
                while (*p && *p != '&') p++;
                vv = jv_cstr_n(vs, (int)(p - vs));
            }
            js_map_put(o, kv, vv);
            if (*p == '&') p++;
        }
    }
    return jv_obj(o);
}
static js_val_t jn_urlp_ops(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    int op = (int)(intptr_t)u; /* 0=get,1=set,2=has,3=delete,4=toString */
    if (th.type != JS_OBJ || th.u.o->kind != OBJ_URLPARAMS) return jv_null();
    js_obj_t *o = th.u.o;
    js_val_t k = (n >= 1) ? jv_str(js_to_str(ctx, a[0])) : jv_cstr("");
    if (op == 0) { int i = js_map_find(o, k); return i >= 0 ? o->map_ents[i].val : jv_null(); }
    if (op == 1) { js_map_put(o, k, n >= 2 ? jv_str(js_to_str(ctx, a[1])) : jv_cstr("")); return jv_undef(); }
    if (op == 2) return jv_bool(js_map_find(o, k) >= 0);
    if (op == 3) { int i = js_map_find(o, k); if (i >= 0) o->map_ents[i].used = 0; return jv_undef(); }
    js_sb_t sb; js_sb_init(&sb); int first = 1;
    for (int i = 0; i < o->map_n; i++) {
        if (!o->map_ents[i].used) continue;
        if (!first) js_sb_putc(&sb, '&');
        first = 0;
        js_str_t *ks = js_to_str(ctx, o->map_ents[i].key), *vs = js_to_str(ctx, o->map_ents[i].val);
        js_sb_putn(&sb, ks->data, ks->len); js_sb_putc(&sb, '='); js_sb_putn(&sb, vs->data, vs->len);
    }
    return jv_str(js_sb_finish(&sb));
}
static js_val_t jn_url_ctor(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    js_obj_t *o = js_obj_new(ctx, OBJ_URL, ctx->url_proto);
    js_str_t *us = (n >= 1) ? js_to_str(ctx, a[0]) : js_str_new("", 0);
    const char *s = us->data;
    char proto[16] = "https:", host[128] = "", hostname[128] = "", port[16] = "", path[256] = "/", search[256] = "", hash[128] = "";
    const char *p = s;
    const char *col = find_substr_ci(p, "://");
    if (col) {
        int pl = (int)(col - p) + 1; if (pl > 15) pl = 15;
        js_memcpy(proto, p, (size_t)pl); proto[pl] = 0;
        p = col + 3;
        const char *he = p;
        while (*he && *he != '/' && *he != '?' && *he != '#') he++;
        int hl = (int)(he - p); if (hl > 127) hl = 127;
        js_memcpy(host, p, (size_t)hl); host[hl] = 0;
        int cpos = -1;
        for (int i = 0; i < hl; i++) if (host[i] == ':') { cpos = i; break; }
        if (cpos >= 0) {
            js_memcpy(hostname, host, (size_t)cpos); hostname[cpos] = 0;
            bcopy_str(port, host + cpos + 1, sizeof(port));
        } else bcopy_str(hostname, host, sizeof(hostname));
        p = he;
    }
    if (*p == '/') {
        const char *pe = p; while (*pe && *pe != '?' && *pe != '#') pe++;
        int pl = (int)(pe - p); if (pl > 255) pl = 255;
        js_memcpy(path, p, (size_t)pl); path[pl] = 0; p = pe;
    }
    if (*p == '?') {
        const char *qe = p; while (*qe && *qe != '#') qe++;
        int ql = (int)(qe - p); if (ql > 255) ql = 255;
        js_memcpy(search, p, (size_t)ql); search[ql] = 0; p = qe;
    }
    if (*p == '#') bcopy_str(hash, p, sizeof(hash));
    char origin[160]; bcopy_str(origin, proto, sizeof(origin)); bcat(origin, "//", sizeof(origin)); bcat(origin, host, sizeof(origin));
    js_set_prop(ctx, jv_obj(o), "href", jv_str(us));
    js_set_prop(ctx, jv_obj(o), "protocol", jv_cstr(proto));
    js_set_prop(ctx, jv_obj(o), "host", jv_cstr(host));
    js_set_prop(ctx, jv_obj(o), "hostname", jv_cstr(hostname));
    js_set_prop(ctx, jv_obj(o), "port", jv_cstr(port));
    js_set_prop(ctx, jv_obj(o), "pathname", jv_cstr(path));
    js_set_prop(ctx, jv_obj(o), "search", jv_cstr(search));
    js_set_prop(ctx, jv_obj(o), "hash", jv_cstr(hash));
    js_set_prop(ctx, jv_obj(o), "origin", jv_cstr(origin));
    js_val_t sa = jv_cstr(search);
    js_set_prop(ctx, jv_obj(o), "searchParams", jn_urlp_ctor(ctx, jv_undef(), &sa, 1, 0));
    return jv_obj(o);
}

/* ==========================================================================
 * 6. TARAYICI DOM / BOM / EVENTS / TIMERS / FETCH / CONSOLE KOPRUSU
 * ========================================================================== */

static int js_dispatch_dom_event(int dom_idx, const char *type);

static int js_sel_list_match(int n, const char *sel_list) {
    if (!sel_list || !sel_list[0]) return 0;
    const char *p = sel_list;
    while (*p) {
        const char *s = p; int depth = 0;
        while (*p && (*p != ',' || depth > 0)) {
            if (*p == '(' || *p == '[') depth++;
            else if ((*p == ')' || *p == ']') && depth > 0) depth--;
            p++;
        }
        char sub[160]; int l = (int)(p - s); if (l > 159) l = 159;
        js_memcpy(sub, s, (size_t)l); sub[l] = 0; css_trim(sub);
        if (sub[0] && sel_match(n, sub)) return 1;
        if (*p == ',') p++;
    }
    return 0;
}

/* --- Console (test ve debug tamponuna yazar) --- */
static void js_console_append(js_ctx_t *ctx, const char *s, int len) {
    if (!ctx || !s || len <= 0) return;
    for (int i = 0; i < len && ctx->console_len < JS_CONSOLE_MAX - 1; i++)
        ctx->console_buf[ctx->console_len++] = s[i];
    ctx->console_buf[ctx->console_len] = 0;
}
static js_val_t jn_console_log(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; const char *pfx = (const char *)u;
    if (pfx && pfx[0]) js_console_append(ctx, pfx, (int)js_strlen(pfx));
    for (int i = 0; i < n; i++) {
        if (i > 0) js_console_append(ctx, " ", 1);
        js_str_t *s = js_to_str(ctx, a[i]);
        if (s) js_console_append(ctx, s->data, s->len);
    }
    js_console_append(ctx, "\n", 1);
    return jv_undef();
}
static js_val_t jn_console_assert(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)u;
    if (n < 1 || !js_to_bool(a[0]))
        return jn_console_log(ctx, th, n > 1 ? a + 1 : 0, n > 1 ? n - 1 : 0, "[assert] ");
    return jv_undef();
}
static js_val_t jn_console_clear(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)a; (void)n; (void)u;
    if (ctx) { ctx->console_len = 0; ctx->console_buf[0] = 0; }
    return jv_undef();
}

/* --- DOM Wrapper & Yardimcilar --- */
static js_obj_t *js_dom_wrap(js_ctx_t *ctx, int dom_idx) {
    if (!ctx) return 0;
    if (dom_idx == -1) return ctx->doc_obj;
    if (dom_idx == -2) return ctx->global_obj;
    if (dom_idx < 0 || dom_idx >= g_dn_count) return 0;
    if (dom_idx >= ctx->dom_cache_cap) {
        int nc = ctx->dom_cache_cap ? ctx->dom_cache_cap * 2 : 512;
        while (nc <= dom_idx) nc *= 2;
        js_obj_t **na = (js_obj_t **)js_calloc((size_t)nc, sizeof(js_obj_t *));
        if (!na) return 0;
        for (int i = 0; i < ctx->dom_cache_cap; i++) na[i] = ctx->dom_cache[i];
        if (ctx->dom_cache) js_free(ctx->dom_cache);
        ctx->dom_cache = na;
        ctx->dom_cache_cap = nc;
    }
    if (ctx->dom_cache[dom_idx]) return ctx->dom_cache[dom_idx];
    js_obj_t *o = js_obj_new(ctx, OBJ_DOM, ctx->dom_proto);
    o->dom_idx = dom_idx;
    ctx->dom_cache[dom_idx] = o;
    return o;
}

static void js_dom_collect_text(int idx, js_sb_t *sb) {
    if (idx < 0 || idx >= g_dn_count) return;
    if (g_dn[idx].text[0]) js_sb_puts(sb, g_dn[idx].text);
    for (int c = g_dn[idx].first; c >= 0; c = g_dn[c].next) js_dom_collect_text(c, sb);
}

static void js_dom_set_text_content(int idx, const char *txt) {
    if (idx < 0 || idx >= g_dn_count) return;
    /* Clear children */
    g_dn[idx].first = -1;
    g_dn[idx].last = -1;
    bcopy_str(g_dn[idx].text, txt ? txt : "", sizeof(g_dn[idx].text));
    g_restyle_pending = 1;
}

/* HTML fragment -> g_dn[] alt dugumleri olarak ayristir (innerHTML icin) */
static void js_dom_parse_fragment(int parent_idx, const char *html) {
    if (parent_idx < 0 || parent_idx >= g_dn_count || !html) return;
    g_dn[parent_idx].first = -1;
    g_dn[parent_idx].last = -1;
    g_dn[parent_idx].text[0] = 0;
    int st[48]; int sp = 1; st[0] = parent_idx;
    const char *p = html;
    while (*p) {
        if (*p == '<') {
            const char *ts = p;
            if (p[1] == '!' && p[2] == '-' && p[3] == '-') {
                const char *ec = find_substr_ci(p + 4, "-->");
                p = ec ? ec + 3 : p + 4; continue;
            }
            const char *q = p + 1; char inq = 0;
            while (*q) {
                if (inq) { if (*q == inq) inq = 0; }
                else { if (*q == '"' || *q == '\'') inq = *q; else if (*q == '>') break; }
                q++;
            }
            if (p[1] == '/') {
                char nm[16]; get_tag_name(p + 1, nm, sizeof(nm));
                for (int i = sp - 1; i >= 1; i--)
                    if (str_eq(g_dn[st[i]].tag, nm)) { sp = i; break; }
                p = (*q == '>') ? q + 1 : q; continue;
            }
            if (p[1] == '!' || p[1] == '?') { p = (*q == '>') ? q + 1 : q; continue; }
            char nm[16]; get_tag_name(p + 1, nm, sizeof(nm));
            if (nm[0]) {
                int ni = dn_new(nm);
                if (ni >= 0) {
                    dn_add(st[sp - 1], ni);
                    dnode_t *dn = &g_dn[ni];
                    char a[260];
                    if (find_attr(ts, q, "id", a, sizeof(a))) bcopy_str(dn->id, a, sizeof(dn->id));
                    if (find_attr(ts, q, "class", a, sizeof(a))) bcopy_str(dn->cls, a, sizeof(dn->cls));
                    if (find_attr(ts, q, "style", a, sizeof(a))) bcopy_str(dn->istyle, a, sizeof(dn->istyle));
                    if (find_attr(ts, q, "href", a, sizeof(a))) bcopy_str(dn->href, a, sizeof(dn->href));
                    if (find_attr(ts, q, "src", a, sizeof(a))) bcopy_str(dn->src, a, sizeof(dn->src));
                    if (find_attr(ts, q, "name", a, sizeof(a))) bcopy_str(dn->name, a, sizeof(dn->name));
                    if (find_attr(ts, q, "type", a, sizeof(a))) bcopy_str(dn->type, a, sizeof(dn->type));
                    if (find_attr(ts, q, "value", a, sizeof(a))) dn_decode(a, dn->value, sizeof(dn->value));
                    if (find_attr(ts, q, "placeholder", a, sizeof(a))) dn_decode(a, dn->ph, sizeof(dn->ph));
                    if (find_attr(ts, q, "data-category", a, sizeof(a))) bcopy_str(dn->data_cat, a, sizeof(dn->data_cat));
                    if (find_attr(ts, q, "data-tooltip", a, sizeof(a))) dn_decode(a, dn->data_tip, sizeof(dn->data_tip));
                    if (find_bool_attr(ts, q, "checked")) dn->checked = 1;
                    if (find_bool_attr(ts, q, "open")) dn->is_open = 1;
                    if (find_bool_attr(ts, q, "required")) dn->required = 1;
                    int self_close = (q > ts && q[-1] == '/');
                    if (!self_close && !dn_void(nm) && sp < 47) st[sp++] = ni;
                }
            }
            p = (*q == '>') ? q + 1 : q;
        } else {
            const char *s = p;
            while (*p && *p != '<') p++;
            char dec[CSS_NTEXT];
            int raw_n = (int)(p - s);
            if (raw_n >= (int)sizeof(dec)) raw_n = (int)sizeof(dec) - 1;
            char tmp[CSS_NTEXT]; js_memcpy(tmp, s, (size_t)raw_n); tmp[raw_n] = 0;
            dn_decode(tmp, dec, sizeof(dec));
            if (dec[0]) {
                int tn = dn_new("#text");
                if (tn >= 0) {
                    g_dn[tn].is_text = 1;
                    bcopy_str(g_dn[tn].text, dec, sizeof(g_dn[tn].text));
                    dn_add(st[sp - 1], tn);
                }
            }
        }
    }
    g_restyle_pending = 1;
}

static void js_dom_serialize_html(int idx, js_sb_t *sb, int outer) {
    if (idx < 0 || idx >= g_dn_count) return;
    dnode_t *d = &g_dn[idx];
    if (d->is_text) { js_sb_puts(sb, d->text); return; }
    if (outer) {
        js_sb_putc(sb, '<'); js_sb_puts(sb, d->tag);
        if (d->id[0]) { js_sb_puts(sb, " id=\""); js_sb_puts(sb, d->id); js_sb_putc(sb, '"'); }
        if (d->cls[0]) { js_sb_puts(sb, " class=\""); js_sb_puts(sb, d->cls); js_sb_putc(sb, '"'); }
        if (d->istyle[0]) { js_sb_puts(sb, " style=\""); js_sb_puts(sb, d->istyle); js_sb_putc(sb, '"'); }
        js_sb_putc(sb, '>');
    }
    if (d->text[0]) js_sb_puts(sb, d->text);
    for (int c = d->first; c >= 0; c = g_dn[c].next) js_dom_serialize_html(c, sb, 1);
    if (outer && !dn_void(d->tag)) { js_sb_puts(sb, "</"); js_sb_puts(sb, d->tag); js_sb_putc(sb, '>'); }
}

/* camelCase -> kebab-case (style.backgroundColor -> background-color) */
static void js_camel_to_kebab(const char *in, char *out, int cap) {
    int o = 0;
    for (int i = 0; in[i] && o < cap - 2; i++) {
        if (in[i] >= 'A' && in[i] <= 'Z') {
            if (o > 0) out[o++] = '-';
            out[o++] = (char)(in[i] + 32);
        } else out[o++] = in[i];
    }
    out[o] = 0;
}

/* Inline style ozelligi okuma/yazma */
static void js_istyle_set(int idx, const char *prop_in, const char *val) {
    if (idx < 0 || idx >= g_dn_count || !prop_in) return;
    char prop[64];
    js_camel_to_kebab(prop_in, prop, sizeof(prop));
    /* Remove existing declaration of same property if any, then append new */
    char nb[sizeof(g_dn[idx].istyle)]; int no = 0;
    const char *p = g_dn[idx].istyle;
    while (*p) {
        while (*p == ' ' || *p == ';') p++;
        if (!*p) break;
        const char *ds = p;
        while (*p && *p != ';') p++;
        const char *de = p;
        const char *col = ds;
        while (col < de && *col != ':') col++;
        char k[64]; int kl = (int)(col - ds); if (kl > 63) kl = 63;
        js_memcpy(k, ds, (size_t)kl); k[kl] = 0; css_trim(k);
        if (!str_eq_ci(k, prop) && col < de) {
            if (no > 0 && no < (int)sizeof(nb) - 2) nb[no++] = ';';
            int dl = (int)(de - ds);
            if (no + dl < (int)sizeof(nb) - 2) { js_memcpy(nb + no, ds, (size_t)dl); no += dl; }
        }
    }
    if (val && val[0]) {
        if (no > 0 && no < (int)sizeof(nb) - 2) nb[no++] = ';';
        int pl = (int)js_strlen(prop), vl = (int)js_strlen(val);
        if (no + pl + vl + 2 < (int)sizeof(nb)) {
            js_memcpy(nb + no, prop, (size_t)pl); no += pl;
            nb[no++] = ':';
            js_memcpy(nb + no, val, (size_t)vl); no += vl;
        }
    }
    nb[no] = 0;
    bcopy_str(g_dn[idx].istyle, nb, sizeof(g_dn[idx].istyle));
    g_restyle_pending = 1;
}

static void js_istyle_get(int idx, const char *prop_in, char *out, int cap) {
    out[0] = 0;
    if (idx < 0 || idx >= g_dn_count || !prop_in) return;
    char prop[64]; js_camel_to_kebab(prop_in, prop, sizeof(prop));
    const char *p = g_dn[idx].istyle;
    while (*p) {
        while (*p == ' ' || *p == ';') p++;
        if (!*p) break;
        const char *ds = p; while (*p && *p != ';') p++;
        const char *de = p;
        const char *col = ds; while (col < de && *col != ':') col++;
        if (col < de) {
            char k[64]; int kl = (int)(col - ds); if (kl > 63) kl = 63;
            js_memcpy(k, ds, (size_t)kl); k[kl] = 0; css_trim(k);
            if (str_eq_ci(k, prop)) {
                int vl = (int)(de - (col + 1)); if (vl >= cap) vl = cap - 1;
                js_memcpy(out, col + 1, (size_t)vl); out[vl] = 0; css_trim(out);
            }
        }
    }
}

/* extra_attrs yazma yardimcisi */
static void js_dn_set_extra_attr(int idx, const char *name, const char *val) {
    if (idx < 0 || idx >= g_dn_count || !name || !name[0]) return;
    dnode_t *d = &g_dn[idx];
    if (str_eq_ci(name, "id")) bcopy_str(d->id, val, sizeof(d->id));
    else if (str_eq_ci(name, "class")) bcopy_str(d->cls, val, sizeof(d->cls));
    else if (str_eq_ci(name, "style")) bcopy_str(d->istyle, val, sizeof(d->istyle));
    else if (str_eq_ci(name, "href")) bcopy_str(d->href, val, sizeof(d->href));
    else if (str_eq_ci(name, "src")) bcopy_str(d->src, val, sizeof(d->src));
    else if (str_eq_ci(name, "name")) bcopy_str(d->name, val, sizeof(d->name));
    else if (str_eq_ci(name, "type")) bcopy_str(d->type, val, sizeof(d->type));
    else if (str_eq_ci(name, "value")) { bcopy_str(d->value, val, sizeof(d->value)); if (d->widget && d->is_ta) lv_textarea_set_text(d->widget, val); }
    else if (str_eq_ci(name, "placeholder") || str_eq_ci(name, "title")) bcopy_str(d->ph, val, sizeof(d->ph));
    else if (str_eq_ci(name, "alt")) bcopy_str(d->alt, val, sizeof(d->alt));
    else if (str_eq_ci(name, "for")) bcopy_str(d->for_attr, val, sizeof(d->for_attr));
    else if (str_eq_ci(name, "data-category")) bcopy_str(d->data_cat, val, sizeof(d->data_cat));
    else if (str_eq_ci(name, "data-tooltip")) bcopy_str(d->data_tip, val, sizeof(d->data_tip));
    else if (str_eq_ci(name, "checked")) d->checked = 1;
    else if (str_eq_ci(name, "open")) d->is_open = 1;
    else if (str_eq_ci(name, "required")) d->required = 1;
    else {
        char nb[sizeof(d->extra_attrs)]; int no = 0;
        const char *p = d->extra_attrs; int nl = (int)js_strlen(name);
        while (*p) {
            const char *es = p; while (*p && *p != '\x1f') p++;
            const char *eq = es; while (eq < p && *eq != '=') eq++;
            if (!((int)(eq - es) == nl && strn_ci_eq(es, name, nl))) {
                if (no > 0 && no < (int)sizeof(nb) - 2) nb[no++] = '\x1f';
                int el = (int)(p - es);
                if (no + el < (int)sizeof(nb) - 2) { js_memcpy(nb + no, es, (size_t)el); no += el; }
            }
            if (*p == '\x1f') p++;
        }
        if (val) {
            if (no > 0 && no < (int)sizeof(nb) - 2) nb[no++] = '\x1f';
            int vl = (int)js_strlen(val);
            if (no + nl + vl + 2 < (int)sizeof(nb)) {
                js_memcpy(nb + no, name, (size_t)nl); no += nl;
                nb[no++] = '=';
                js_memcpy(nb + no, val, (size_t)vl); no += vl;
            }
        }
        nb[no] = 0;
        bcopy_str(d->extra_attrs, nb, sizeof(d->extra_attrs));
    }
    g_restyle_pending = 1;
}

/* classList metodlari */
static js_val_t jn_cl_ops(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    int op = (int)(intptr_t)u; /* 0=add,1=remove,2=toggle,3=contains,4=replace */
    if (th.type != JS_OBJ || th.u.o->dom_idx < 0 || th.u.o->dom_idx >= g_dn_count) return jv_bool(0);
    int idx = th.u.o->dom_idx;
    if (op == 3) {
        if (n < 1) return jv_bool(0);
        js_str_t *s = js_to_str(ctx, a[0]);
        return jv_bool(class_has(g_dn[idx].cls, s->data, s->len));
    }
    if (op == 0) {
        for (int i = 0; i < n; i++) {
            js_str_t *s = js_to_str(ctx, a[i]);
            if (s->len > 0 && !class_has(g_dn[idx].cls, s->data, s->len)) {
                if (g_dn[idx].cls[0]) bcat(g_dn[idx].cls, " ", sizeof(g_dn[idx].cls));
                bcat(g_dn[idx].cls, s->data, sizeof(g_dn[idx].cls));
                g_restyle_pending = 1;
            }
        }
        return jv_undef();
    }
    if (op == 1 || op == 2 || op == 4) {
        if (n < 1) return jv_bool(0);
        js_str_t *s = js_to_str(ctx, a[0]);
        int has = class_has(g_dn[idx].cls, s->data, s->len);
        if (op == 2) {
            int want = (n >= 2) ? js_to_bool(a[1]) : !has;
            if (want && !has) {
                if (g_dn[idx].cls[0]) bcat(g_dn[idx].cls, " ", sizeof(g_dn[idx].cls));
                bcat(g_dn[idx].cls, s->data, sizeof(g_dn[idx].cls));
                g_restyle_pending = 1; return jv_bool(1);
            } else if (!want && has) {
                /* fall through to remove */
            } else return jv_bool(want);
        }
        if (op == 4 && !has) return jv_bool(0);
        char nb[sizeof(g_dn[idx].cls)]; nb[0] = 0;
        const char *p = g_dn[idx].cls;
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            const char *q = p; while (*q && *q != ' ' && *q != '\t') q++;
            if (q > p) {
                int match = ((int)(q - p) == s->len && js_memcmp(p, s->data, (size_t)s->len) == 0);
                if (!match) {
                    if (nb[0]) bcat(nb, " ", sizeof(nb));
                    char tok[64]; int tl = (int)(q - p); if (tl > 63) tl = 63;
                    js_memcpy(tok, p, (size_t)tl); tok[tl] = 0;
                    bcat(nb, tok, sizeof(nb));
                } else if (op == 4 && n >= 2) {
                    js_str_t *ns = js_to_str(ctx, a[1]);
                    if (nb[0]) bcat(nb, " ", sizeof(nb));
                    bcat(nb, ns->data, sizeof(nb));
                }
            }
            p = q;
        }
        bcopy_str(g_dn[idx].cls, nb, sizeof(g_dn[idx].cls));
        g_restyle_pending = 1;
        return jv_bool(op == 4 ? 1 : 0);
    }
    return jv_undef();
}

static js_val_t jn_style_ops(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    int op = (int)(intptr_t)u; /* 0=setProperty, 1=getPropertyValue, 2=removeProperty */
    if (th.type != JS_OBJ || th.u.o->dom_idx < 0) return jv_cstr("");
    if (n < 1) return jv_cstr("");
    js_str_t *k = js_to_str(ctx, a[0]);
    if (op == 0) {
        js_str_t *v = (n >= 2) ? js_to_str(ctx, a[1]) : js_str_new("", 0);
        js_istyle_set(th.u.o->dom_idx, k->data, v->data);
        return jv_undef();
    }
    if (op == 1) {
        char buf[128]; js_istyle_get(th.u.o->dom_idx, k->data, buf, sizeof(buf));
        return jv_cstr(buf);
    }
    char old[128]; js_istyle_get(th.u.o->dom_idx, k->data, old, sizeof(old));
    js_istyle_set(th.u.o->dom_idx, k->data, "");
    return jv_cstr(old);
}

/* DOM property getter/setter */
static js_val_t js_dom_get_prop(js_ctx_t *ctx, js_obj_t *o, const char *k) {
    if (o->kind == OBJ_DOM_STYLE) {
        if (js_strcmp(k, "setProperty") == 0 || js_strcmp(k, "getPropertyValue") == 0 || js_strcmp(k, "removeProperty") == 0)
            return jv_sym(-999);
        if (js_strcmp(k, "cssText") == 0 && o->dom_idx >= 0 && o->dom_idx < g_dn_count)
            return jv_cstr(g_dn[o->dom_idx].istyle);
        char buf[128]; js_istyle_get(o->dom_idx, k, buf, sizeof(buf));
        return jv_cstr(buf);
    }
    if (o->kind == OBJ_DOM_CLASSLIST) {
        if (js_strcmp(k, "value") == 0 && o->dom_idx >= 0 && o->dom_idx < g_dn_count)
            return jv_cstr(g_dn[o->dom_idx].cls);
        if (js_strcmp(k, "length") == 0 && o->dom_idx >= 0 && o->dom_idx < g_dn_count) {
            int c = 0; const char *p = g_dn[o->dom_idx].cls;
            while (*p) { while (*p == ' ') p++; if (*p) { c++; while (*p && *p != ' ') p++; } }
            return jv_num((double)c);
        }
        return jv_sym(-999);
    }
    if (o->kind == OBJ_DOM_DATASET) {
        if (o->dom_idx < 0 || o->dom_idx >= g_dn_count) return jv_undef();
        char ak[72] = "data-";
        js_camel_to_kebab(k, ak + 5, sizeof(ak) - 5);
        char val[128];
        if (dn_attr(o->dom_idx, ak, val, sizeof(val))) return jv_cstr(val);
        return jv_undef();
    }
    /* OBJ_DOM */
    int idx = o->dom_idx;
    if (idx < 0 || idx >= g_dn_count) return jv_sym(-999);
    dnode_t *d = &g_dn[idx];
    if (js_strcmp(k, "tagName") == 0 || js_strcmp(k, "nodeName") == 0) {
        char up[24]; int i = 0;
        for (; d->tag[i] && i < 23; i++) up[i] = (d->tag[i] >= 'a' && d->tag[i] <= 'z') ? (char)(d->tag[i] - 32) : d->tag[i];
        up[i] = 0; return jv_cstr(up);
    }
    if (js_strcmp(k, "nodeType") == 0) return jv_num(d->is_text ? 3.0 : 1.0);
    if (js_strcmp(k, "id") == 0) return jv_cstr(d->id);
    if (js_strcmp(k, "className") == 0) return jv_cstr(d->cls);
    if (js_strcmp(k, "href") == 0) return jv_cstr(d->href);
    if (js_strcmp(k, "src") == 0) return jv_cstr(d->src);
    if (js_strcmp(k, "name") == 0) return jv_cstr(d->name);
    if (js_strcmp(k, "type") == 0) return jv_cstr(d->type);
    if (js_strcmp(k, "placeholder") == 0) return jv_cstr(d->ph);
    if (js_strcmp(k, "value") == 0) {
        if (d->widget && d->is_ta) {
            const char *t = lv_textarea_get_text(d->widget);
            if (t) bcopy_str(d->value, t, sizeof(d->value));
        }
        return jv_cstr(d->value);
    }
    if (js_strcmp(k, "checked") == 0) return jv_bool(d->checked);
    if (js_strcmp(k, "open") == 0) return jv_bool(d->is_open);
    if (js_strcmp(k, "required") == 0) return jv_bool(d->required);
    if (js_strcmp(k, "textContent") == 0 || js_strcmp(k, "innerText") == 0) {
        js_sb_t sb; js_sb_init(&sb); js_dom_collect_text(idx, &sb);
        return jv_str(js_sb_finish(&sb));
    }
    if (js_strcmp(k, "innerHTML") == 0) {
        js_sb_t sb; js_sb_init(&sb); js_dom_serialize_html(idx, &sb, 0);
        return jv_str(js_sb_finish(&sb));
    }
    if (js_strcmp(k, "outerHTML") == 0) {
        js_sb_t sb; js_sb_init(&sb); js_dom_serialize_html(idx, &sb, 1);
        return jv_str(js_sb_finish(&sb));
    }
    if (js_strcmp(k, "style") == 0) {
        js_obj_t *so = js_obj_new(ctx, OBJ_DOM_STYLE, ctx->obj_proto); so->dom_idx = idx;
        js_def_native(ctx, so, "setProperty", jn_style_ops, (void *)0);
        js_def_native(ctx, so, "getPropertyValue", jn_style_ops, (void *)1);
        js_def_native(ctx, so, "removeProperty", jn_style_ops, (void *)2);
        return jv_obj(so);
    }
    if (js_strcmp(k, "classList") == 0) {
        js_obj_t *co = js_obj_new(ctx, OBJ_DOM_CLASSLIST, ctx->obj_proto); co->dom_idx = idx;
        js_def_native(ctx, co, "add", jn_cl_ops, (void *)0);
        js_def_native(ctx, co, "remove", jn_cl_ops, (void *)1);
        js_def_native(ctx, co, "toggle", jn_cl_ops, (void *)2);
        js_def_native(ctx, co, "contains", jn_cl_ops, (void *)3);
        js_def_native(ctx, co, "replace", jn_cl_ops, (void *)4);
        return jv_obj(co);
    }
    if (js_strcmp(k, "dataset") == 0) {
        js_obj_t *ds = js_obj_new(ctx, OBJ_DOM_DATASET, ctx->obj_proto); ds->dom_idx = idx;
        return jv_obj(ds);
    }
    if (js_strcmp(k, "parentNode") == 0 || js_strcmp(k, "parentElement") == 0)
        return d->parent >= 0 ? jv_obj(js_dom_wrap(ctx, d->parent)) : jv_null();
    if (js_strcmp(k, "children") == 0 || js_strcmp(k, "childNodes") == 0) {
        int elem_only = (js_strcmp(k, "children") == 0);
        js_obj_t *a = js_arr_new(ctx, 4);
        for (int c = d->first; c >= 0; c = g_dn[c].next)
            if (!elem_only || !g_dn[c].is_text) js_arr_push(a, jv_obj(js_dom_wrap(ctx, c)));
        return jv_obj(a);
    }
    if (js_strcmp(k, "childElementCount") == 0) {
        int cnt = 0; for (int c = d->first; c >= 0; c = g_dn[c].next) if (!g_dn[c].is_text) cnt++;
        return jv_num((double)cnt);
    }
    if (js_strcmp(k, "firstChild") == 0) return d->first >= 0 ? jv_obj(js_dom_wrap(ctx, d->first)) : jv_null();
    if (js_strcmp(k, "lastChild") == 0) return d->last >= 0 ? jv_obj(js_dom_wrap(ctx, d->last)) : jv_null();
    if (js_strcmp(k, "firstElementChild") == 0) {
        for (int c = d->first; c >= 0; c = g_dn[c].next) if (!g_dn[c].is_text) return jv_obj(js_dom_wrap(ctx, c));
        return jv_null();
    }
    if (js_strcmp(k, "nextSibling") == 0) return d->next >= 0 ? jv_obj(js_dom_wrap(ctx, d->next)) : jv_null();
    if (js_strcmp(k, "nextElementSibling") == 0) {
        for (int c = d->next; c >= 0; c = g_dn[c].next) if (!g_dn[c].is_text) return jv_obj(js_dom_wrap(ctx, c));
        return jv_null();
    }
    if (js_strcmp(k, "previousElementSibling") == 0 || js_strcmp(k, "previousSibling") == 0) {
        int elem_only = (js_strcmp(k, "previousElementSibling") == 0);
        int prev = -1;
        if (d->parent >= 0) {
            for (int c = g_dn[d->parent].first; c >= 0 && c != idx; c = g_dn[c].next)
                if (!elem_only || !g_dn[c].is_text) prev = c;
        }
        return prev >= 0 ? jv_obj(js_dom_wrap(ctx, prev)) : jv_null();
    }
    if (js_strcmp(k, "offsetWidth") == 0 || js_strcmp(k, "clientWidth") == 0)
        return jv_num(d->widget ? (double)lv_obj_get_width(d->widget) : 0.0);
    if (js_strcmp(k, "offsetHeight") == 0 || js_strcmp(k, "clientHeight") == 0)
        return jv_num(d->widget ? (double)lv_obj_get_height(d->widget) : 0.0);
    return jv_sym(-999);
}

static void js_dom_set_prop(js_ctx_t *ctx, js_obj_t *o, const char *k, js_val_t v) {
    if (o->kind == OBJ_DOM_STYLE) {
        js_str_t *vs = js_to_str(ctx, v);
        if (js_strcmp(k, "cssText") == 0 && o->dom_idx >= 0 && o->dom_idx < g_dn_count) {
            bcopy_str(g_dn[o->dom_idx].istyle, vs->data, sizeof(g_dn[o->dom_idx].istyle));
            g_restyle_pending = 1;
        } else {
            js_istyle_set(o->dom_idx, k, vs->data);
        }
        return;
    }
    if (o->kind == OBJ_DOM_DATASET) {
        char ak[72] = "data-"; js_camel_to_kebab(k, ak + 5, sizeof(ak) - 5);
        js_str_t *vs = js_to_str(ctx, v);
        js_dn_set_extra_attr(o->dom_idx, ak, vs->data);
        return;
    }
    if (o->kind == OBJ_DOM && o->dom_idx >= 0 && o->dom_idx < g_dn_count) {
        int idx = o->dom_idx;
        if (js_strcmp(k, "textContent") == 0 || js_strcmp(k, "innerText") == 0) {
            js_dom_set_text_content(idx, js_to_str(ctx, v)->data); return;
        }
        if (js_strcmp(k, "innerHTML") == 0) {
            js_dom_parse_fragment(idx, js_to_str(ctx, v)->data); return;
        }
        if (js_strcmp(k, "className") == 0) { js_dn_set_extra_attr(idx, "class", js_to_str(ctx, v)->data); return; }
        if (js_strcmp(k, "id") == 0 || js_strcmp(k, "href") == 0 || js_strcmp(k, "src") == 0 ||
            js_strcmp(k, "value") == 0 || js_strcmp(k, "name") == 0 || js_strcmp(k, "type") == 0 ||
            js_strcmp(k, "placeholder") == 0) {
            js_dn_set_extra_attr(idx, k, js_to_str(ctx, v)->data); return;
        }
        if (js_strcmp(k, "checked") == 0) { g_dn[idx].checked = js_to_bool(v) ? 1 : 0; g_restyle_pending = 1; return; }
        if (js_strcmp(k, "open") == 0) { g_dn[idx].is_open = js_to_bool(v) ? 1 : 0; g_restyle_pending = 1; return; }
        if (js_strcmp(k, "hidden") == 0) { js_istyle_set(idx, "display", js_to_bool(v) ? "none" : ""); return; }
    }
    /* Custom property or event handler like el.onclick = fn */
    js_def_prop(o, k, v, PROP_ENUM | PROP_WRIT | PROP_CONF);
}

/* DOM / Document / Element Metodlari */
static int js_dom_is_descendant(int root, int node) {
    if (root < 0) return 1; /* document */
    int c = g_dn[node].parent;
    while (c >= 0) { if (c == root) return 1; c = g_dn[c].parent; }
    return 0;
}
static js_val_t jn_dom_qs(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    int all = (int)(intptr_t)u;
    if (n < 1) return all ? jv_obj(js_arr_new(ctx, 0)) : jv_null();
    js_str_t *sel = js_to_str(ctx, a[0]);
    int root = (th.type == JS_OBJ && th.u.o->kind == OBJ_DOM) ? th.u.o->dom_idx : -1;
    js_obj_t *arr = all ? js_arr_new(ctx, 8) : 0;
    for (int i = 0; i < g_dn_count; i++) {
        if (g_dn[i].is_text) continue;
        if (root >= 0 && !js_dom_is_descendant(root, i)) continue;
        if (js_sel_list_match(i, sel->data)) {
            if (!all) return jv_obj(js_dom_wrap(ctx, i));
            js_arr_push(arr, jv_obj(js_dom_wrap(ctx, i)));
        }
    }
    return all ? jv_obj(arr) : jv_null();
}
static js_val_t jn_doc_getById(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    if (n < 1) return jv_null();
    js_str_t *id = js_to_str(ctx, a[0]);
    for (int i = 0; i < g_dn_count; i++)
        if (g_dn[i].id[0] && str_eq(g_dn[i].id, id->data)) return jv_obj(js_dom_wrap(ctx, i));
    return jv_null();
}
static js_val_t jn_dom_getByTagCls(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    int by_cls = (int)(intptr_t)u;
    js_obj_t *arr = js_arr_new(ctx, 8);
    if (n < 1) return jv_obj(arr);
    js_str_t *q = js_to_str(ctx, a[0]);
    int root = (th.type == JS_OBJ && th.u.o->kind == OBJ_DOM) ? th.u.o->dom_idx : -1;
    for (int i = 0; i < g_dn_count; i++) {
        if (g_dn[i].is_text) continue;
        if (root >= 0 && !js_dom_is_descendant(root, i)) continue;
        if (by_cls) { if (class_has(g_dn[i].cls, q->data, q->len)) js_arr_push(arr, jv_obj(js_dom_wrap(ctx, i))); }
        else { if (str_eq(q->data, "*") || str_eq_ci(g_dn[i].tag, q->data)) js_arr_push(arr, jv_obj(js_dom_wrap(ctx, i))); }
    }
    return jv_obj(arr);
}
static js_val_t jn_doc_createEl(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; int kind = (int)(intptr_t)u; /* 0=el, 1=text, 2=frag */
    if (kind == 1) {
        int ni = dn_new("#text");
        if (ni < 0) return jv_null();
        g_dn[ni].is_text = 1;
        if (n >= 1) bcopy_str(g_dn[ni].text, js_to_str(ctx, a[0])->data, sizeof(g_dn[ni].text));
        return jv_obj(js_dom_wrap(ctx, ni));
    }
    char tag[16];
    if (kind == 2) bcopy_str(tag, "div", sizeof(tag));
    else {
        js_str_t *ts = (n >= 1) ? js_to_str(ctx, a[0]) : js_str_new("div", 3);
        int i = 0; for (; i < ts->len && i < 15; i++) {
            char c = ts->data[i]; if (c >= 'A' && c <= 'Z') c += 32; tag[i] = c;
        }
        tag[i] = 0;
    }
    int ni = dn_new(tag);
    return ni >= 0 ? jv_obj(js_dom_wrap(ctx, ni)) : jv_null();
}
static js_val_t jn_dom_attr(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    int op = (int)(intptr_t)u; /* 0=get, 1=set, 2=remove, 3=has */
    if (th.type != JS_OBJ || th.u.o->kind != OBJ_DOM || n < 1) return op == 3 ? jv_bool(0) : jv_null();
    int idx = th.u.o->dom_idx;
    js_str_t *nm = js_to_str(ctx, a[0]);
    if (op == 0 || op == 3) {
        char buf[260]; int has = dn_attr(idx, nm->data, buf, sizeof(buf));
        if (op == 3) return jv_bool(has);
        return has ? jv_cstr(buf) : jv_null();
    }
    if (op == 1) {
        js_str_t *val = (n >= 2) ? js_to_str(ctx, a[1]) : js_str_new("", 0);
        js_dn_set_extra_attr(idx, nm->data, val->data);
        return jv_undef();
    }
    js_dn_set_extra_attr(idx, nm->data, "");
    if (str_eq_ci(nm->data, "open")) g_dn[idx].is_open = 0;
    if (str_eq_ci(nm->data, "checked")) g_dn[idx].checked = 0;
    return jv_undef();
}
static js_val_t jn_dom_tree(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    int op = (int)(intptr_t)u; /* 0=appendChild, 1=removeChild, 2=remove, 3=append, 4=prepend, 5=insertBefore, 6=cloneNode, 7=matches, 8=closest, 9=contains */
    if (th.type != JS_OBJ || th.u.o->kind != OBJ_DOM) return jv_undef();
    int p = th.u.o->dom_idx;
    if (op == 2) { dn_unlink(p); g_restyle_pending = 1; return jv_undef(); }
    if (op == 6) {
        int ni = dn_new(g_dn[p].tag);
        if (ni < 0) return jv_null();
        g_dn[ni] = g_dn[p];
        g_dn[ni].parent = -1; g_dn[ni].first = -1; g_dn[ni].last = -1; g_dn[ni].next = -1; g_dn[ni].widget = 0;
        return jv_obj(js_dom_wrap(ctx, ni));
    }
    if (op == 7 && n >= 1) return jv_bool(js_sel_list_match(p, js_to_str(ctx, a[0])->data));
    if (op == 8 && n >= 1) {
        js_str_t *sel = js_to_str(ctx, a[0]);
        for (int c = p; c >= 0; c = g_dn[c].parent)
            if (js_sel_list_match(c, sel->data)) return jv_obj(js_dom_wrap(ctx, c));
        return jv_null();
    }
    if (op == 9 && n >= 1 && a[0].type == JS_OBJ && a[0].u.o->kind == OBJ_DOM) {
        int t = a[0].u.o->dom_idx;
        return jv_bool(t == p || js_dom_is_descendant(p, t));
    }
    if (op == 0 || op == 3 || op == 4) {
        for (int i = 0; i < n; i++) {
            int ch = -1;
            if (a[i].type == JS_OBJ && a[i].u.o->kind == OBJ_DOM) ch = a[i].u.o->dom_idx;
            else if (op != 0) {
                ch = dn_new("#text");
                if (ch >= 0) { g_dn[ch].is_text = 1; bcopy_str(g_dn[ch].text, js_to_str(ctx, a[i])->data, sizeof(g_dn[ch].text)); }
            }
            if (ch >= 0 && ch != p && !dn_is_ancestor(ch, p)) {
                dn_unlink(ch);
                if (op == 4) {
                    g_dn[ch].parent = p; g_dn[ch].next = g_dn[p].first;
                    g_dn[p].first = ch; if (g_dn[p].last < 0) g_dn[p].last = ch;
                } else dn_add(p, ch);
                g_restyle_pending = 1;
            }
        }
        return (op == 0 && n >= 1) ? a[0] : jv_undef();
    }
    if (op == 1 && n >= 1 && a[0].type == JS_OBJ && a[0].u.o->kind == OBJ_DOM) {
        dn_unlink(a[0].u.o->dom_idx); g_restyle_pending = 1; return a[0];
    }
    if (op == 5 && n >= 1 && a[0].type == JS_OBJ && a[0].u.o->kind == OBJ_DOM) {
        int ch = a[0].u.o->dom_idx;
        int ref = (n >= 2 && a[1].type == JS_OBJ && a[1].u.o->kind == OBJ_DOM) ? a[1].u.o->dom_idx : -1;
        if (ch != p && !dn_is_ancestor(ch, p)) {
            dn_unlink(ch);
            if (ref < 0 || g_dn[ref].parent != p) dn_add(p, ch);
            else {
                g_dn[ch].parent = p;
                if (g_dn[p].first == ref) { g_dn[ch].next = ref; g_dn[p].first = ch; }
                else {
                    for (int cur = g_dn[p].first; cur >= 0; cur = g_dn[cur].next)
                        if (g_dn[cur].next == ref) { g_dn[ch].next = ref; g_dn[cur].next = ch; break; }
                }
            }
            g_restyle_pending = 1;
        }
        return a[0];
    }
    return jv_undef();
}
static js_val_t jn_dom_rect(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)a; (void)n; (void)u;
    js_obj_t *r = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    int x = 0, y = 0, w = 0, h = 0;
    if (th.type == JS_OBJ && th.u.o->kind == OBJ_DOM && th.u.o->dom_idx >= 0 && g_dn[th.u.o->dom_idx].widget) {
        lv_area_t ar; lv_obj_get_coords(g_dn[th.u.o->dom_idx].widget, &ar);
        x = ar.x1; y = ar.y1; w = ar.x2 - ar.x1 + 1; h = ar.y2 - ar.y1 + 1;
    }
    js_set_prop(ctx, jv_obj(r), "x", jv_num((double)x)); js_set_prop(ctx, jv_obj(r), "y", jv_num((double)y));
    js_set_prop(ctx, jv_obj(r), "left", jv_num((double)x)); js_set_prop(ctx, jv_obj(r), "top", jv_num((double)y));
    js_set_prop(ctx, jv_obj(r), "right", jv_num((double)(x + w))); js_set_prop(ctx, jv_obj(r), "bottom", jv_num((double)(y + h)));
    js_set_prop(ctx, jv_obj(r), "width", jv_num((double)w)); js_set_prop(ctx, jv_obj(r), "height", jv_num((double)h));
    return jv_obj(r);
}
/* --- HTML5 <canvas> 2D Context JS Baglantisi (browser_canvas.h) --- */
static uint32_t c2d_parse_color(const char *s, uint32_t def_c) {
    uint32_t c = def_c;
    if (s && s[0] && css_color(s, &c)) return 0xFF000000u | (c & 0x00FFFFFFu);
    return 0xFF000000u | (def_c & 0x00FFFFFFu);
}
static bcanvas_slot_t *c2d_sync_slot(js_ctx_t *ctx, js_val_t th) {
    if (th.type != JS_OBJ || !th.u.o) return 0;
    js_val_t di = js_get_prop(ctx, th, "__cidx__");
    if (di.type != JS_NUM) return 0;
    int slot = (int)di.u.n;
    if (slot < 0 || slot >= g_canvas_n) return 0;
    bcanvas_slot_t *sl = &g_canvases[slot];
    js_val_t fs = js_get_prop(ctx, th, "fillStyle");
    if (fs.type == JS_STR) {
        sl->bc.st.fill_grad.active = 0;
        sl->bc.st.fill_col = c2d_parse_color(fs.u.s->data, 0x000000);
    } else if (fs.type == JS_OBJ && fs.u.o) {
        js_val_t gx0 = js_get_prop(ctx, fs, "__x0__");
        if (gx0.type == JS_NUM) {
            sl->bc.st.fill_grad.active = 1;
            sl->bc.st.fill_grad.x0 = gx0.u.n;
            sl->bc.st.fill_grad.y0 = js_to_num(ctx, js_get_prop(ctx, fs, "__y0__"));
            sl->bc.st.fill_grad.x1 = js_to_num(ctx, js_get_prop(ctx, fs, "__x1__"));
            sl->bc.st.fill_grad.y1 = js_to_num(ctx, js_get_prop(ctx, fs, "__y1__"));
            js_val_t st_arr = js_get_prop(ctx, fs, "__stops__");
            sl->bc.st.fill_grad.nstops = 0;
            if (st_arr.type == JS_OBJ && st_arr.u.o) {
                for (int i = 0; i < st_arr.u.o->elem_n && sl->bc.st.fill_grad.nstops < BC_MAX_STOPS; i += 2) {
                    if (i + 1 >= st_arr.u.o->elem_n) break;
                    int k = sl->bc.st.fill_grad.nstops++;
                    sl->bc.st.fill_grad.stops[k].off = js_to_num(ctx, st_arr.u.o->elems[i]);
                    js_str_t *cs = js_to_str(ctx, st_arr.u.o->elems[i + 1]);
                    sl->bc.st.fill_grad.stops[k].col = c2d_parse_color(cs->data, 0x000000);
                }
            }
        }
    }
    js_val_t ss = js_get_prop(ctx, th, "strokeStyle");
    if (ss.type == JS_STR) sl->bc.st.stroke_col = c2d_parse_color(ss.u.s->data, 0x000000);
    js_val_t lw = js_get_prop(ctx, th, "lineWidth");
    if (lw.type == JS_NUM && lw.u.n > 0) sl->bc.st.line_width = lw.u.n;
    js_val_t ga = js_get_prop(ctx, th, "globalAlpha");
    if (ga.type == JS_NUM) {
        double a = ga.u.n; if (a < 0) a = 0; if (a > 1) a = 1;
        sl->bc.st.alpha = a;
    }
    js_val_t fn = js_get_prop(ctx, th, "font");
    if (fn.type == JS_STR) {
        int px = 0;
        for (int i = 0; fn.u.s->data[i]; i++) {
            if (fn.u.s->data[i] >= '0' && fn.u.s->data[i] <= '9') {
                while (fn.u.s->data[i] >= '0' && fn.u.s->data[i] <= '9')
                    px = px * 10 + (fn.u.s->data[i++] - '0');
                break;
            }
        }
        if (px >= 6 && px <= 96) sl->bc.st.font_px = px;
    }
    return sl;
}
static js_val_t jn_c2d_grad_add(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)u;
    if (th.type != JS_OBJ || !th.u.o || n < 2) return jv_undef();
    js_val_t arr = js_get_prop(ctx, th, "__stops__");
    if (arr.type == JS_OBJ && arr.u.o) {
        js_arr_push(arr.u.o, jv_num(js_to_num(ctx, a[0])));
        js_arr_push(arr.u.o, jv_str(js_to_str(ctx, a[1])));
    }
    return jv_undef();
}
static js_val_t jn_c2d_op(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    int op = (int)(intptr_t)u;
    bcanvas_slot_t *sl = c2d_sync_slot(ctx, th);
    if (!sl) return jv_undef();
    bc_ctx_t *bc = &sl->bc;
    int dirty = 0;
    if (op == 0 && n >= 4) { bc_fill_rect(bc, js_to_num(ctx, a[0]), js_to_num(ctx, a[1]), js_to_num(ctx, a[2]), js_to_num(ctx, a[3])); dirty = 1; }
    else if (op == 1 && n >= 4) { bc_stroke_rect(bc, js_to_num(ctx, a[0]), js_to_num(ctx, a[1]), js_to_num(ctx, a[2]), js_to_num(ctx, a[3])); dirty = 1; }
    else if (op == 2 && n >= 4) { bc_clear_rect(bc, js_to_num(ctx, a[0]), js_to_num(ctx, a[1]), js_to_num(ctx, a[2]), js_to_num(ctx, a[3])); dirty = 1; }
    else if (op == 3) bc_begin_path(bc);
    else if (op == 4) bc_close_path(bc);
    else if (op == 5 && n >= 2) bc_move_to(bc, js_to_num(ctx, a[0]), js_to_num(ctx, a[1]));
    else if (op == 6 && n >= 2) bc_line_to(bc, js_to_num(ctx, a[0]), js_to_num(ctx, a[1]));
    else if (op == 7 && n >= 4) bc_rect(bc, js_to_num(ctx, a[0]), js_to_num(ctx, a[1]), js_to_num(ctx, a[2]), js_to_num(ctx, a[3]));
    else if (op == 8 && n >= 5) bc_arc(bc, js_to_num(ctx, a[0]), js_to_num(ctx, a[1]), js_to_num(ctx, a[2]), js_to_num(ctx, a[3]), js_to_num(ctx, a[4]), n >= 6 ? js_to_bool(a[5]) : 0);
    else if (op == 9 && n >= 4) bc_quad_to(bc, js_to_num(ctx, a[0]), js_to_num(ctx, a[1]), js_to_num(ctx, a[2]), js_to_num(ctx, a[3]));
    else if (op == 10 && n >= 6) bc_bezier_to(bc, js_to_num(ctx, a[0]), js_to_num(ctx, a[1]), js_to_num(ctx, a[2]), js_to_num(ctx, a[3]), js_to_num(ctx, a[4]), js_to_num(ctx, a[5]));
    else if (op == 11) { bc_fill(bc); dirty = 1; }
    else if (op == 12) { bc_stroke(bc); dirty = 1; }
    else if (op == 13 && n >= 3) { bc_fill_text(bc, js_to_str(ctx, a[0])->data, js_to_num(ctx, a[1]), js_to_num(ctx, a[2])); dirty = 1; }
    else if (op == 14) bc_save(bc);
    else if (op == 15) bc_restore(bc);
    else if (op == 16 && n >= 2) bc_translate(bc, js_to_num(ctx, a[0]), js_to_num(ctx, a[1]));
    else if (op == 17 && n >= 2) bc_scale(bc, js_to_num(ctx, a[0]), js_to_num(ctx, a[1]));
    else if (op == 18 && n >= 1) bc_rotate(bc, js_to_num(ctx, a[0]));
    else if (op == 19 && n >= 4) {
        js_obj_t *g = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
        js_set_prop(ctx, jv_obj(g), "__x0__", jv_num(js_to_num(ctx, a[0])));
        js_set_prop(ctx, jv_obj(g), "__y0__", jv_num(js_to_num(ctx, a[1])));
        js_set_prop(ctx, jv_obj(g), "__x1__", jv_num(js_to_num(ctx, a[2])));
        js_set_prop(ctx, jv_obj(g), "__y1__", jv_num(js_to_num(ctx, a[3])));
        js_set_prop(ctx, jv_obj(g), "__stops__", jv_obj(js_arr_new(ctx, 4)));
        js_def_native(ctx, g, "addColorStop", jn_c2d_grad_add, 0);
        return jv_obj(g);
    } else if (op == 20 && n >= 1) {
        js_str_t *ts = js_to_str(ctx, a[0]);
        int sc = bc->st.font_px / 7; if (sc < 1) sc = 1;
        js_obj_t *m = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
        js_set_prop(ctx, jv_obj(m), "width", jv_num((double)(ts->len * 6 * sc)));
        return jv_obj(m);
    }
    if (dirty && sl->lv_c) lv_obj_invalidate(sl->lv_c);
    return jv_undef();
}
static js_val_t jn_canvas_getContext(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)u;
    if (th.type != JS_OBJ || !th.u.o || th.u.o->kind != OBJ_DOM) return jv_null();
    if (n >= 1 && !str_eq_ci(js_to_str(ctx, a[0])->data, "2d")) return jv_null();
    int di = th.u.o->dom_idx;
    if (di < 0 || di >= g_dn_count) return jv_null();
    int cw = g_dn[di].wa > 0 ? g_dn[di].wa : 300;
    int ch = g_dn[di].ha > 0 ? g_dn[di].ha : 150;
    bcanvas_slot_t *sl = bcanvas_get(di, cw, ch);
    if (!sl) return jv_null();
    int slot_idx = (int)(sl - g_canvases);
    js_obj_t *o = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    js_set_prop(ctx, jv_obj(o), "__cidx__", jv_num((double)slot_idx));
    js_set_prop(ctx, jv_obj(o), "canvas", th);
    js_set_prop(ctx, jv_obj(o), "fillStyle", jv_cstr("#000000"));
    js_set_prop(ctx, jv_obj(o), "strokeStyle", jv_cstr("#000000"));
    js_set_prop(ctx, jv_obj(o), "lineWidth", jv_num(1.0));
    js_set_prop(ctx, jv_obj(o), "globalAlpha", jv_num(1.0));
    js_set_prop(ctx, jv_obj(o), "font", jv_cstr("14px sans-serif"));
    static const char *ops[] = {
        "fillRect","strokeRect","clearRect","beginPath","closePath","moveTo","lineTo",
        "rect","arc","quadraticCurveTo","bezierCurveTo","fill","stroke","fillText",
        "save","restore","translate","scale","rotate","createLinearGradient","measureText"
    };
    for (int i = 0; i < 21; i++) js_def_native(ctx, o, ops[i], jn_c2d_op, (void *)(intptr_t)i);
    return jv_obj(o);
}

static js_val_t jn_dom_focus_click(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)ctx; (void)a; (void)n; int op = (int)(intptr_t)u; /* 0=focus, 1=blur, 2=click, 3=scrollIntoView */
    if (th.type != JS_OBJ || th.u.o->kind != OBJ_DOM) return jv_undef();
    int idx = th.u.o->dom_idx;
    if (op == 0) { g_focus_node = idx; if (g_dn[idx].widget && g_group) lv_group_focus_obj(g_dn[idx].widget); }
    else if (op == 1) { if (g_focus_node == idx) g_focus_node = -1; }
    else if (op == 2) {
        if (str_eq(g_dn[idx].tag, "input") && (str_eq(g_dn[idx].type, "checkbox") || str_eq(g_dn[idx].type, "radio")))
            toggle_input_node(idx);
        /* Dispatch click event */
        js_dispatch_dom_event(idx, "click");
    } else if (op == 3 && g_dn[idx].widget) {
        lv_obj_scroll_to_view_recursive(g_dn[idx].widget, LV_ANIM_OFF);
    }
    return jv_undef();
}

/* --- Olay Sistemi (addEventListener / removeEventListener / dispatchEvent) --- */
static js_val_t jn_ev_prevent(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)a; (void)n; int op = (int)(intptr_t)u;
    if (op == 0) { ctx->ev_prevented = 1; js_set_prop(ctx, th, "defaultPrevented", jv_bool(1)); }
    else ctx->ev_stopped = 1;
    return jv_undef();
}
static js_val_t jn_event_ctor(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; int custom = (int)(intptr_t)u;
    js_obj_t *ev = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    js_set_prop(ctx, jv_obj(ev), "type", n >= 1 ? jv_str(js_to_str(ctx, a[0])) : jv_cstr(""));
    js_set_prop(ctx, jv_obj(ev), "bubbles", jv_bool(n >= 2 ? js_to_bool(js_get_prop(ctx, a[1], "bubbles")) : 0));
    js_set_prop(ctx, jv_obj(ev), "defaultPrevented", jv_bool(0));
    if (custom) js_set_prop(ctx, jv_obj(ev), "detail", n >= 2 ? js_get_prop(ctx, a[1], "detail") : jv_null());
    js_def_native(ctx, ev, "preventDefault", jn_ev_prevent, (void *)0);
    js_def_native(ctx, ev, "stopPropagation", jn_ev_prevent, (void *)1);
    return jv_obj(ev);
}
static int js_target_dom_idx(js_ctx_t *ctx, js_val_t th) {
    if (th.type != JS_OBJ || !th.u.o) return -2;
    if (th.u.o == ctx->global_obj) return -2;
    if (th.u.o == ctx->doc_obj) return -1;
    if (th.u.o->kind == OBJ_DOM) return th.u.o->dom_idx;
    return -99;
}
static js_val_t jn_addEvent(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)u;
    if (n < 2 || a[1].type != JS_OBJ) return jv_undef();
    int did = js_target_dom_idx(ctx, th);
    if (did == -99 || ctx->listener_n >= JS_MAX_LISTENERS) return jv_undef();
    js_str_t *tp = js_to_str(ctx, a[0]);
    js_listener_t *l = &ctx->listeners[ctx->listener_n++];
    l->dom_idx = did;
    bcopy_str(l->type, tp->data, sizeof(l->type));
    l->fn = a[1].u.o;
    l->once = (n >= 3 && a[2].type == JS_OBJ && js_to_bool(js_get_prop(ctx, a[2], "once"))) ? 1 : 0;
    l->active = 1;
    return jv_undef();
}
static js_val_t jn_remEvent(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)u;
    if (n < 2 || a[1].type != JS_OBJ) return jv_undef();
    int did = js_target_dom_idx(ctx, th);
    js_str_t *tp = js_to_str(ctx, a[0]);
    for (int i = 0; i < ctx->listener_n; i++) {
        if (ctx->listeners[i].active && ctx->listeners[i].dom_idx == did &&
            ctx->listeners[i].fn == a[1].u.o && str_eq(ctx->listeners[i].type, tp->data))
            ctx->listeners[i].active = 0;
    }
    return jv_undef();
}
static int js_dispatch_event_obj(js_ctx_t *ctx, int dom_idx, js_obj_t *ev) {
    if (!ctx || !ev) return 0;
    js_str_t *tp = js_to_str(ctx, js_get_prop(ctx, jv_obj(ev), "type"));
    js_val_t tgt = (dom_idx >= 0) ? jv_obj(js_dom_wrap(ctx, dom_idx)) :
                   (dom_idx == -1 ? jv_obj(ctx->doc_obj) : jv_obj(ctx->global_obj));
    js_set_prop(ctx, jv_obj(ev), "target", tgt);
    ctx->ev_prevented = 0;
    ctx->ev_stopped = 0;
    /* Build bubble path: dom_idx -> parent -> ... -> -1 (document) -> -2 (window) */
    int path[64]; int pn = 0;
    if (dom_idx >= 0) {
        for (int c = dom_idx; c >= 0 && pn < 60; c = g_dn[c].parent) path[pn++] = c;
        path[pn++] = -1;
        path[pn++] = -2;
    } else if (dom_idx == -1) { path[pn++] = -1; path[pn++] = -2; }
    else { path[pn++] = -2; }

    char onprop[40]; bcopy_str(onprop, "on", sizeof(onprop)); bcat(onprop, tp->data, sizeof(onprop));
    for (int pi = 0; pi < pn && !ctx->ev_stopped; pi++) {
        int cur = path[pi];
        js_val_t cur_v = (cur >= 0) ? jv_obj(js_dom_wrap(ctx, cur)) :
                         (cur == -1 ? jv_obj(ctx->doc_obj) : jv_obj(ctx->global_obj));
        js_set_prop(ctx, jv_obj(ev), "currentTarget", cur_v);
        /* Inline HTML attribute handler (e.g., onclick="...") */
        if (cur >= 0) {
            char code[160];
            if (dn_attr(cur, onprop, code, sizeof(code)) && code[0]) {
                js_env_t *henv = js_env_new(ctx, ctx->global_env, 1);
                henv->has_this = 1; henv->this_val = cur_v;
                js_env_def(henv, "event", jv_obj(ev), 0);
                js_eval_source(ctx, code, -1, henv);
                ctx->throwing = 0;
            }
        }
        /* Property handler el.onclick = fn */
        js_val_t oh = js_get_prop(ctx, cur_v, onprop);
        if (oh.type == JS_OBJ && (oh.u.o->kind == OBJ_FUNC || oh.u.o->kind == OBJ_NATIVE)) {
            js_val_t ea = jv_obj(ev);
            js_call_fn(ctx, oh, cur_v, &ea, 1, 0);
            ctx->throwing = 0;
        }
        /* Registered addEventListener callbacks */
        for (int i = 0; i < ctx->listener_n; i++) {
            js_listener_t *l = &ctx->listeners[i];
            if (!l->active || l->dom_idx != cur || !str_eq(l->type, tp->data)) continue;
            if (l->once) l->active = 0;
            js_val_t ea = jv_obj(ev);
            js_call_fn(ctx, jv_obj(l->fn), cur_v, &ea, 1, 0);
            ctx->throwing = 0;
        }
        if (pi == 0 && !js_to_bool(js_get_prop(ctx, jv_obj(ev), "bubbles"))) break;
    }
    js_drain_microtasks(ctx);
    return ctx->ev_prevented;
}
static int js_dispatch_dom_event(int dom_idx, const char *type) {
    if (!g_js || !type) return 0;
    js_val_t ta = jv_cstr(type);
    js_obj_t *opts = js_obj_new(g_js, OBJ_PLAIN, g_js->obj_proto);
    js_set_prop(g_js, jv_obj(opts), "bubbles", jv_bool(1));
    js_val_t ca[2] = { ta, jv_obj(opts) };
    js_val_t ev = jn_event_ctor(g_js, jv_undef(), ca, 2, (void *)0);
    return js_dispatch_event_obj(g_js, dom_idx, ev.u.o);
}
static js_val_t jn_dispatchEvent(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)u;
    if (n < 1 || a[0].type != JS_OBJ) return jv_bool(1);
    int did = js_target_dom_idx(ctx, th);
    int prevented = js_dispatch_event_obj(ctx, did, a[0].u.o);
    return jv_bool(!prevented);
}

/* --- Zamanlayicilar (setTimeout / setInterval / requestAnimationFrame) --- */
static uint64_t js_ticks_now(void) {
    if (g_basi && g_basi->time && g_basi->time->ticks_ms) return g_basi->time->ticks_ms();
    return 0;
}
static js_val_t jn_set_timer(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; int mode = (int)(intptr_t)u; /* 0=timeout, 1=interval, 2=raf */
    if (n < 1) return jv_num(0);
    int slot = -1;
    for (int i = 0; i < JS_MAX_TIMERS; i++) if (!ctx->timers[i].active) { slot = i; break; }
    if (slot < 0) return jv_num(0);
    js_timer_t *tm = &ctx->timers[slot];
    js_memset(tm, 0, sizeof(*tm));
    tm->id = ++ctx->next_timer_id;
    tm->active = 1;
    tm->is_interval = (mode == 1) ? 1 : 0;
    tm->is_raf = (mode == 2) ? 1 : 0;
    if (a[0].type == JS_OBJ) tm->fn = a[0].u.o;
    else tm->code = js_to_str(ctx, a[0]);
    uint32_t delay = (mode == 2) ? 16u : ((n >= 2) ? (uint32_t)(js_to_num(ctx, a[1]) > 0 ? js_to_num(ctx, a[1]) : 0) : 0u);
    tm->interval_ms = delay;
    tm->fire_ms = js_ticks_now() + (uint64_t)delay;
    if (mode != 2 && n > 2) {
        tm->argc = n - 2;
        tm->args = (js_val_t *)js_malloc((size_t)tm->argc * sizeof(js_val_t));
        for (int i = 0; i < tm->argc; i++) tm->args[i] = a[2 + i];
    }
    return jv_num((double)tm->id);
}
static js_val_t jn_clear_timer(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    if (n < 1) return jv_undef();
    int id = (int)js_to_num(ctx, a[0]);
    for (int i = 0; i < JS_MAX_TIMERS; i++)
        if (ctx->timers[i].active && ctx->timers[i].id == id) ctx->timers[i].active = 0;
    return jv_undef();
}

static void js_tick(void) {
    if (!g_js) return;
    js_ctx_t *ctx = g_js;
    js_drain_microtasks(ctx);
    uint64_t now = js_ticks_now();
    for (int i = 0; i < JS_MAX_TIMERS; i++) {
        js_timer_t *tm = &ctx->timers[i];
        if (!tm->active || now < tm->fire_ms) continue;
        if (tm->is_interval) tm->fire_ms = now + (uint64_t)(tm->interval_ms ? tm->interval_ms : 16u);
        else tm->active = 0;
        ctx->steps = 0; ctx->throwing = 0;
        if (tm->fn) {
            if (tm->is_raf) {
                js_val_t ts = jv_num((double)(now - ctx->time_origin));
                js_call_fn(ctx, jv_obj(tm->fn), jv_obj(ctx->global_obj), &ts, 1, 0);
            } else {
                js_call_fn(ctx, jv_obj(tm->fn), jv_obj(ctx->global_obj), tm->args, tm->argc, 0);
            }
        } else if (tm->code) {
            js_eval_source(ctx, tm->code->data, tm->code->len, ctx->global_env);
        }
        ctx->throwing = 0;
        js_drain_microtasks(ctx);
    }
}

/* --- Storage (kalici C:/etc/localstorage.cfg), Performance, Crypto, getComputedStyle, matchMedia, fetch, XHR --- */
#define LS_FILE "C:/etc/localstorage.cfg"
static void ls_persist_save(js_ctx_t *ctx) {
    if (!ctx || !g_basi || !g_basi->fs || !g_basi->fs->create_in || !g_basi->fs->write) return;
    if (g_basi->fs->mkdir) g_basi->fs->mkdir("C:/etc");
    void *f = g_basi->fs->create_in("C:/etc", "localstorage.cfg", 0);
    if (!f) return;
    static char lbuf[8192];
    int n = 0;
    for (int i = 0; i < ctx->local_n; i++) {
        if (!ctx->local_store[i].k) continue;
        const char *k = ctx->local_store[i].k;
        const char *v = ctx->local_store[i].v ? ctx->local_store[i].v : "";
        int kl = b_len(k), vl = b_len(v);
        if (n + kl + vl + 3 >= (int)sizeof(lbuf)) break;
        for (int j = 0; j < kl; j++) lbuf[n++] = (k[j] == '\t' || k[j] == '\n') ? ' ' : k[j];
        lbuf[n++] = '\t';
        for (int j = 0; j < vl; j++) lbuf[n++] = (v[j] == '\n' || v[j] == '\r') ? ' ' : v[j];
        lbuf[n++] = '\n';
    }
    g_basi->fs->write(f, lbuf, (uint32_t)n);
}
static void ls_persist_load(js_ctx_t *ctx) {
    if (!ctx || !g_basi || !g_basi->fs || !g_basi->fs->read_path) return;
    static char lbuf[8192];
    uint32_t n = g_basi->fs->read_path(LS_FILE, lbuf, sizeof(lbuf) - 1);
    if (!n) return;
    lbuf[n] = 0;
    uint32_t i = 0;
    while (i < n && ctx->local_n < JS_MAX_STORAGE) {
        uint32_t ks = i;
        while (i < n && lbuf[i] != '\t' && lbuf[i] != '\n') i++;
        if (i >= n || lbuf[i] != '\t') { while (i < n && lbuf[i] != '\n') i++; if (i < n) i++; continue; }
        uint32_t ke = i++;
        uint32_t vs = i;
        while (i < n && lbuf[i] != '\n' && lbuf[i] != '\r') i++;
        uint32_t ve = i;
        while (i < n && (lbuf[i] == '\n' || lbuf[i] == '\r')) i++;
        if (ke > ks) {
            char *kp = (char *)js_malloc((size_t)(ke - ks) + 1);
            char *vp = (char *)js_malloc((size_t)(ve - vs) + 1);
            if (kp && vp) {
                js_memcpy(kp, lbuf + ks, (size_t)(ke - ks)); kp[ke - ks] = 0;
                js_memcpy(vp, lbuf + vs, (size_t)(ve - vs)); vp[ve - vs] = 0;
                ctx->local_store[ctx->local_n].k = kp;
                ctx->local_store[ctx->local_n].v = vp;
                ctx->local_n++;
            }
        }
    }
}
static js_val_t jn_storage_ops(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; int code = (int)(intptr_t)u;
    int is_sess = code / 10, op = code % 10; /* 0=get,1=set,2=remove,3=clear,4=key */
    js_kv_t *arr = is_sess ? ctx->session_store : ctx->local_store;
    int *cnt = is_sess ? &ctx->session_n : &ctx->local_n;
    if (op == 3) { *cnt = 0; if (!is_sess) ls_persist_save(ctx); return jv_undef(); }
    if (op == 4) {
        int i = (n >= 1) ? (int)js_to_num(ctx, a[0]) : -1;
        return (i >= 0 && i < *cnt) ? jv_cstr(arr[i].k) : jv_null();
    }
    if (n < 1) return jv_null();
    js_str_t *ks = js_to_str(ctx, a[0]);
    for (int i = 0; i < *cnt; i++) {
        if (js_strcmp(arr[i].k, ks->data) == 0) {
            if (op == 0) return jv_cstr(arr[i].v);
            if (op == 1) {
                js_str_t *vs = (n >= 2) ? js_to_str(ctx, a[1]) : js_str_new("undefined", 9);
                arr[i].v = (char *)js_malloc((size_t)vs->len + 1);
                if (arr[i].v) js_memcpy(arr[i].v, vs->data, (size_t)vs->len + 1);
                if (!is_sess) ls_persist_save(ctx);
                return jv_undef();
            }
            if (op == 2) {
                for (int j = i; j < *cnt - 1; j++) arr[j] = arr[j + 1];
                (*cnt)--; if (!is_sess) ls_persist_save(ctx); return jv_undef();
            }
        }
    }
    if (op == 1 && *cnt < JS_MAX_STORAGE) {
        js_str_t *vs = (n >= 2) ? js_to_str(ctx, a[1]) : js_str_new("undefined", 9);
        arr[*cnt].k = (char *)js_malloc((size_t)ks->len + 1);
        arr[*cnt].v = (char *)js_malloc((size_t)vs->len + 1);
        if (arr[*cnt].k) js_memcpy(arr[*cnt].k, ks->data, (size_t)ks->len + 1);
        if (arr[*cnt].v) js_memcpy(arr[*cnt].v, vs->data, (size_t)vs->len + 1);
        (*cnt)++;
        if (!is_sess) ls_persist_save(ctx);
    }
    return op == 0 ? jv_null() : jv_undef();
}
static js_val_t jn_perf_now(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)a; (void)n; (void)u;
    return jv_num((double)(js_ticks_now() - ctx->time_origin));
}
static js_val_t jn_crypto_ops(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)ctx; (void)th; int op = (int)(intptr_t)u; /* 0=getRandomValues, 1=randomUUID */
    if (op == 0 && n >= 1 && a[0].type == JS_OBJ && a[0].u.o->kind == OBJ_TYPEDARRAY) {
        js_obj_t *ta = a[0].u.o;
        for (int i = ta->ta_off; i < ta->buf_len; i++)
            ta->buf[i] = (uint8_t)((uint32_t)(js_random_num() * 256.0) & 0xFF);
        return a[0];
    }
    static const char hx[] = "0123456789abcdef";
    char uuid[37];
    uint8_t b[16];
    for (int i = 0; i < 16; i++) b[i] = (uint8_t)(js_random_num() * 256.0);
    b[6] = (uint8_t)((b[6] & 0x0F) | 0x40);
    b[8] = (uint8_t)((b[8] & 0x3F) | 0x80);
    int p = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) uuid[p++] = '-';
        uuid[p++] = hx[b[i] >> 4]; uuid[p++] = hx[b[i] & 15];
    }
    uuid[36] = 0;
    return jv_cstr(uuid);
}
static js_val_t jn_getComputedStyle(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    js_obj_t *o = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    if (n < 1 || a[0].type != JS_OBJ || a[0].u.o->kind != OBJ_DOM) return jv_obj(o);
    int idx = a[0].u.o->dom_idx;
    if (idx < 0 || idx >= g_dn_count) return jv_obj(o);
    css_style_t base, st; style_defaults(&base);
    compute_style(idx, &base, &st, 0, 0);
    char cb[32];
    int r = (st.color >> 16) & 255, g = (st.color >> 8) & 255, b = st.color & 255;
    bcopy_str(cb, "rgb(", sizeof(cb));
    char nb[16]; js_i64toa(r, nb, sizeof(nb)); bcat(cb, nb, sizeof(cb)); bcat(cb, ", ", sizeof(cb));
    js_i64toa(g, nb, sizeof(nb)); bcat(cb, nb, sizeof(cb)); bcat(cb, ", ", sizeof(cb));
    js_i64toa(b, nb, sizeof(nb)); bcat(cb, nb, sizeof(cb)); bcat(cb, ")", sizeof(cb));
    js_set_prop(ctx, jv_obj(o), "color", jv_cstr(cb));
    const char *disp = st.display == 2 ? "none" : st.display == 3 ? "flex" : st.display == 4 ? "grid" : st.display == 1 ? "inline" : "block";
    js_set_prop(ctx, jv_obj(o), "display", jv_cstr(disp));
    js_i64toa(st.font_size, nb, sizeof(nb)); bcat(nb, "px", sizeof(nb));
    js_set_prop(ctx, jv_obj(o), "fontSize", jv_cstr(nb));
    js_dtoa((double)st.opacity / 100.0, nb, sizeof(nb));
    js_set_prop(ctx, jv_obj(o), "opacity", jv_cstr(nb));
    return jv_obj(o);
}
static js_val_t jn_matchMedia(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    js_str_t *q = (n >= 1) ? js_to_str(ctx, a[0]) : js_str_new("", 0);
    js_obj_t *o = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    js_set_prop(ctx, jv_obj(o), "media", jv_str(q));
    js_set_prop(ctx, jv_obj(o), "matches", jv_bool(media_matches(q->data)));
    return jv_obj(o);
}
static js_val_t jn_resp_text(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)a; (void)n; int mode = (int)(intptr_t)u; /* 0=text, 1=json */
    js_obj_t *p = js_obj_new(ctx, OBJ_PROMISE, ctx->prom_proto);
    js_val_t bv = js_get_prop(ctx, th, "__body__");
    if (mode == 1) {
        js_val_t j = jn_json_parse(ctx, jv_undef(), &bv, 1, 0);
        if (ctx->throwing) { js_val_t e = ctx->exc; ctx->throwing = 0; js_prom_reject_val(ctx, p, e); }
        else js_prom_resolve_val(ctx, p, j);
    } else js_prom_resolve_val(ctx, p, bv);
    return jv_obj(p);
}
static js_val_t jn_fetch(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    js_obj_t *p = js_obj_new(ctx, OBJ_PROMISE, ctx->prom_proto);
    if (n < 1 || !g_basi || !g_basi->net || (!g_basi->net->http_request && !g_basi->net->download)) {
        js_prom_reject_val(ctx, p, jv_cstr("fetch unavailable"));
        return jv_obj(p);
    }
    js_str_t *us = js_to_str(ctx, a[0]);
    char abs[URL_MAX];
    resolve_url(g_cur_scheme, g_cur_host, us->data, abs, sizeof(abs));
    const char *target = abs[0] ? abs : us->data;

    char method[16]; bcopy_str(method, "GET", sizeof(method));
    char ctype[96]; ctype[0] = 0;
    char hdrs[512]; hdrs[0] = 0;
    const char *rbody = 0; uint32_t rblen = 0;
    if (n >= 2 && a[1].type == JS_OBJ && a[1].u.o) {
        js_val_t mv = js_get_prop(ctx, a[1], "method");
        if (mv.type == JS_STR && mv.u.s->len > 0) {
            bcopy_str(method, mv.u.s->data, sizeof(method));
            for (int i = 0; method[i]; i++) if (method[i] >= 'a' && method[i] <= 'z') method[i] = (char)(method[i] - 32);
        }
        js_val_t bv = js_get_prop(ctx, a[1], "body");
        if (bv.type != JS_UNDEF && bv.type != JS_NULL) {
            js_str_t *bs = js_to_str(ctx, bv);
            rbody = bs->data; rblen = (uint32_t)bs->len;
        }
        js_val_t hv = js_get_prop(ctx, a[1], "headers");
        if (hv.type == JS_OBJ && hv.u.o) {
            for (int pi = 0; pi < hv.u.o->prop_n; pi++) {
                js_prop_t *pr = &hv.u.o->props[pi];
                if (!pr->key || !pr->key[0]) continue;
                js_str_t *vs = js_to_str(ctx, pr->val);
                if (str_eq_ci(pr->key, "content-type")) {
                    bcopy_str(ctype, vs->data, sizeof(ctype));
                } else {
                    if (hdrs[0]) bcat(hdrs, "\r\n", sizeof(hdrs));
                    bcat(hdrs, pr->key, sizeof(hdrs));
                    bcat(hdrs, ": ", sizeof(hdrs));
                    bcat(hdrs, vs->data, sizeof(hdrs));
                }
            }
        }
    }

    int st_code = 200;
    uint32_t got = 0;
    const char *resp_data = 0;
    char *dl_tmp = 0;
    if (g_basi->net->http_request) {
        resp_data = g_basi->net->http_request(method, target, ctype[0] ? ctype : 0,
                                              hdrs[0] ? hdrs : 0, rbody, rblen, &st_code, &got);
        if (!resp_data) {
            js_prom_reject_val(ctx, p, jv_cstr("Network error"));
            return jv_obj(p);
        }
    } else {
        const uint32_t cap = 262144u;
        dl_tmp = (char *)js_malloc(cap);
        if (!dl_tmp) { js_prom_reject_val(ctx, p, jv_cstr("Out of memory")); return jv_obj(p); }
        int rc = g_basi->net->download(target, dl_tmp, cap - 1, &got);
        if (rc != 0) {
            js_free(dl_tmp);
            js_prom_reject_val(ctx, p, jv_cstr("Network error"));
            return jv_obj(p);
        }
        dl_tmp[got] = 0;
        resp_data = dl_tmp;
    }

    js_obj_t *resp = js_obj_new(ctx, OBJ_RESPONSE, ctx->obj_proto);
    js_set_prop(ctx, jv_obj(resp), "ok", jv_bool(st_code >= 200 && st_code < 300));
    js_set_prop(ctx, jv_obj(resp), "status", jv_num((double)st_code));
    js_set_prop(ctx, jv_obj(resp), "url", jv_cstr(target));
    js_set_prop(ctx, jv_obj(resp), "__body__", jv_cstr_n(resp_data, (int)got));
    js_def_native(ctx, resp, "text", jn_resp_text, (void *)0);
    js_def_native(ctx, resp, "json", jn_resp_text, (void *)1);
    if (dl_tmp) js_free(dl_tmp);
    js_prom_resolve_val(ctx, p, jv_obj(resp));
    return jv_obj(p);
}

/* XMLHttpRequest (tam GET/POST/PUT/DELETE + custom headers + callbacks) */
static js_val_t jn_xhr_open(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)u;
    if (th.type != JS_OBJ || !th.u.o || n < 2) return jv_undef();
    js_str_t *ms = js_to_str(ctx, a[0]);
    js_str_t *us = js_to_str(ctx, a[1]);
    js_set_prop(ctx, th, "__method__", jv_str(ms));
    js_set_prop(ctx, th, "__url__", jv_str(us));
    js_set_prop(ctx, th, "__hdrs__", jv_obj(js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto)));
    js_set_prop(ctx, th, "readyState", jv_num(1));
    return jv_undef();
}
static js_val_t jn_xhr_set_hdr(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)u;
    if (th.type != JS_OBJ || !th.u.o || n < 2) return jv_undef();
    js_val_t ho = js_get_prop(ctx, th, "__hdrs__");
    if (ho.type == JS_OBJ && ho.u.o) {
        js_str_t *k = js_to_str(ctx, a[0]);
        js_str_t *v = js_to_str(ctx, a[1]);
        js_set_prop(ctx, ho, k->data, jv_str(v));
    }
    return jv_undef();
}
static js_val_t jn_xhr_send(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)u;
    if (th.type != JS_OBJ || !th.u.o) return jv_undef();
    js_val_t uv = js_get_prop(ctx, th, "__url__");
    if (uv.type != JS_STR) return jv_undef();
    js_obj_t *opts = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    js_set_prop(ctx, jv_obj(opts), "method", js_get_prop(ctx, th, "__method__"));
    js_set_prop(ctx, jv_obj(opts), "headers", js_get_prop(ctx, th, "__hdrs__"));
    if (n >= 1 && a[0].type != JS_UNDEF && a[0].type != JS_NULL)
        js_set_prop(ctx, jv_obj(opts), "body", a[0]);
    js_val_t fargs[2] = { uv, jv_obj(opts) };
    js_val_t pr = jn_fetch(ctx, jv_undef(), fargs, 2, 0);
    if (pr.type == JS_OBJ && pr.u.o && pr.u.o->prom_state == 1 && pr.u.o->prom_val.type == JS_OBJ) {
        js_val_t rv = pr.u.o->prom_val;
        js_set_prop(ctx, th, "status", js_get_prop(ctx, rv, "status"));
        js_set_prop(ctx, th, "responseText", js_get_prop(ctx, rv, "__body__"));
        js_set_prop(ctx, th, "response", js_get_prop(ctx, rv, "__body__"));
        js_set_prop(ctx, th, "readyState", jv_num(4));
        js_val_t cb = js_get_prop(ctx, th, "onreadystatechange");
        if (cb.type == JS_OBJ) js_call_fn(ctx, cb, th, 0, 0, 0);
        js_val_t ol = js_get_prop(ctx, th, "onload");
        if (ol.type == JS_OBJ) js_call_fn(ctx, ol, th, 0, 0, 0);
    } else {
        js_set_prop(ctx, th, "status", jv_num(0));
        js_set_prop(ctx, th, "readyState", jv_num(4));
        js_val_t oe = js_get_prop(ctx, th, "onerror");
        if (oe.type == JS_OBJ) js_call_fn(ctx, oe, th, 0, 0, 0);
    }
    return jv_undef();
}
static js_val_t jn_xhr_ctor(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)a; (void)n; (void)u;
    js_obj_t *x = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    js_def_prop(x, "readyState", jv_num(0), PROP_ENUM | PROP_WRIT | PROP_CONF);
    js_def_prop(x, "status", jv_num(0), PROP_ENUM | PROP_WRIT | PROP_CONF);
    js_def_prop(x, "responseText", jv_cstr(""), PROP_ENUM | PROP_WRIT | PROP_CONF);
    js_def_prop(x, "response", jv_cstr(""), PROP_ENUM | PROP_WRIT | PROP_CONF);
    js_def_native(ctx, x, "open", jn_xhr_open, 0);
    js_def_native(ctx, x, "setRequestHeader", jn_xhr_set_hdr, 0);
    js_def_native(ctx, x, "send", jn_xhr_send, 0);
    return jv_obj(x);
}
static js_val_t jn_win_scroll(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; int rel = (int)(intptr_t)u;
    if (!g_content) return jv_undef();
    int y = (n >= 2) ? (int)js_to_num(ctx, a[1]) : (n == 1 && a[0].type == JS_OBJ ? (int)js_to_num(ctx, js_get_prop(ctx, a[0], "top")) : 0);
    if (rel) scroll_content_clamped(-y);
    else {
        if (y < 0) y = 0;
        int32_t max_y = lv_obj_get_scroll_y(g_content) + lv_obj_get_scroll_bottom(g_content);
        if (max_y < 0) max_y = 0;
        if (y > max_y) y = max_y;
        lv_obj_scroll_to_y(g_content, y, LV_ANIM_OFF);
    }
    return jv_undef();
}
static js_val_t jn_win_alert(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; int kind = (int)(intptr_t)u;
    js_str_t *m = (n >= 1) ? js_to_str(ctx, a[0]) : js_str_new("", 0);
    js_console_append(ctx, "[alert] ", 8); js_console_append(ctx, m->data, m->len); js_console_append(ctx, "\n", 1);
    if (g_status) set_status(m->data, ADL_WARN);
    if (kind == 1) return jv_bool(1);
    if (kind == 2) return (n >= 2) ? jv_str(js_to_str(ctx, a[1])) : jv_cstr("");
    return jv_undef();
}
static js_val_t jn_loc_assign(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    if (n >= 1) {
        char abs[URL_MAX]; resolve_url(g_cur_scheme, g_cur_host, js_to_str(ctx, a[0])->data, abs, sizeof(abs));
        if (abs[0]) navigate(abs, 1);
    }
    return jv_undef();
}
static js_val_t jn_doc_body_get(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)a; (void)n; int which = (int)(intptr_t)u; /* 0=body, 1=documentElement, 2=title, 3=cookie */
    if (which == 1) return g_dn_count > 0 ? jv_obj(js_dom_wrap(ctx, 0)) : jv_null();
    if (which == 0) {
        for (int i = 0; i < g_dn_count; i++) if (str_eq(g_dn[i].tag, "body")) return jv_obj(js_dom_wrap(ctx, i));
        return g_dn_count > 0 ? jv_obj(js_dom_wrap(ctx, 0)) : jv_null();
    }
    if (which == 2) {
        char t[120]; if (g_raw && extract_title(g_raw, t, sizeof(t))) return jv_cstr(t);
        return jv_cstr("");
    }
    return jv_cstr("");
}
static js_val_t jn_doc_title_set(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    if (n >= 1) set_tab_title(js_to_str(ctx, a[0])->data);
    return jv_undef();
}
static js_val_t jn_doc_cookie_set(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    if (n < 1 || !g_basi || !g_basi->net || !g_cur_host[0]) return jv_undef();
    js_str_t *s = js_to_str(ctx, a[0]);
    const char *eq = s->data; while (*eq && *eq != '=' && *eq != ';') eq++;
    if (*eq == '=') {
        char nm[64], vl[256];
        int nl = (int)(eq - s->data); if (nl > 63) nl = 63;
        js_memcpy(nm, s->data, (size_t)nl); nm[nl] = 0; css_trim(nm);
        const char *ve = eq + 1; while (*ve && *ve != ';') ve++;
        int vlen = (int)(ve - (eq + 1)); if (vlen > 255) vlen = 255;
        js_memcpy(vl, eq + 1, (size_t)vlen); vl[vlen] = 0;
        if (g_basi->net->cookie_set2)
            g_basi->net->cookie_set2(g_cur_host, nm, vl, str_eq(g_cur_scheme, "https"));
        else if (g_basi->net->cookie_set)
            g_basi->net->cookie_set(g_cur_host, nm, vl);
        cookies_persist_save();
    }
    return jv_undef();
}

/* --- RFC 6455 WebSocket (ws:// ve wss://) JS API --- */
typedef struct {
    int used;
    int ws_id;
    js_obj_t *obj;
} js_ws_ent_t;
static js_ws_ent_t g_js_ws[4];

static void ws_close_all(void) {
    for (int i = 0; i < 4; i++) {
        if (g_js_ws[i].used) {
            if (g_js_ws[i].ws_id >= 0 && g_basi && g_basi->net && g_basi->net->ws_close)
                g_basi->net->ws_close(g_js_ws[i].ws_id);
            g_js_ws[i].used = 0; g_js_ws[i].ws_id = -1; g_js_ws[i].obj = 0;
        }
    }
}
static void ws_poll_all(void) {
    if (!g_js || !g_basi || !g_basi->net || !g_basi->net->ws_recv) return;
    static char ws_rx[4096];
    for (int i = 0; i < 4; i++) {
        if (!g_js_ws[i].used || g_js_ws[i].ws_id < 0 || !g_js_ws[i].obj) continue;
        int is_bin = 0;
        int n = g_basi->net->ws_recv(g_js_ws[i].ws_id, ws_rx, sizeof(ws_rx) - 1, &is_bin);
        if (n > 0) {
            ws_rx[n] = 0;
            js_obj_t *ev = js_obj_new(g_js, OBJ_PLAIN, g_js->obj_proto);
            js_set_prop(g_js, jv_obj(ev), "data", jv_cstr_n(ws_rx, n));
            js_val_t cb = js_get_prop(g_js, jv_obj(g_js_ws[i].obj), "onmessage");
            if (cb.type == JS_OBJ) {
                js_val_t arg = jv_obj(ev);
                js_call_fn(g_js, cb, jv_obj(g_js_ws[i].obj), &arg, 1, 0);
                js_drain_microtasks(g_js);
            }
        } else if (n < 0) {
            js_set_prop(g_js, jv_obj(g_js_ws[i].obj), "readyState", jv_num(3));
            js_val_t cb = js_get_prop(g_js, jv_obj(g_js_ws[i].obj), "onclose");
            g_js_ws[i].used = 0; g_js_ws[i].ws_id = -1;
            if (cb.type == JS_OBJ) {
                js_call_fn(g_js, cb, jv_obj(g_js_ws[i].obj), 0, 0, 0);
                js_drain_microtasks(g_js);
            }
        }
    }
}
static js_val_t jn_ws_send(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)u;
    if (th.type != JS_OBJ || !th.u.o || n < 1 || !g_basi || !g_basi->net || !g_basi->net->ws_send) return jv_undef();
    js_val_t idv = js_get_prop(ctx, th, "__wsid__");
    if (idv.type != JS_NUM || (int)idv.u.n < 0) return jv_undef();
    js_str_t *s = js_to_str(ctx, a[0]);
    g_basi->net->ws_send((int)idv.u.n, s->data, (uint32_t)s->len, 0);
    return jv_undef();
}
static js_val_t jn_ws_close(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)a; (void)n; (void)u;
    if (th.type != JS_OBJ || !th.u.o) return jv_undef();
    js_val_t idv = js_get_prop(ctx, th, "__wsid__");
    int id = (idv.type == JS_NUM) ? (int)idv.u.n : -1;
    if (id >= 0 && g_basi && g_basi->net && g_basi->net->ws_close) g_basi->net->ws_close(id);
    for (int i = 0; i < 4; i++) if (g_js_ws[i].used && g_js_ws[i].ws_id == id) { g_js_ws[i].used = 0; g_js_ws[i].ws_id = -1; }
    js_set_prop(ctx, th, "__wsid__", jv_num(-1));
    js_set_prop(ctx, th, "readyState", jv_num(3));
    return jv_undef();
}
static js_val_t jn_ws_ctor(js_ctx_t *ctx, js_val_t th, js_val_t *a, int n, void *u) {
    (void)th; (void)u;
    js_obj_t *o = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    js_str_t *us = (n >= 1) ? js_to_str(ctx, a[0]) : js_str_new("", 0);
    js_def_prop(o, "url", jv_str(us), PROP_ENUM);
    js_def_prop(o, "readyState", jv_num(3), PROP_ENUM | PROP_WRIT | PROP_CONF);
    js_def_prop(o, "__wsid__", jv_num(-1), PROP_WRIT | PROP_CONF);
    js_def_native(ctx, o, "send", jn_ws_send, 0);
    js_def_native(ctx, o, "close", jn_ws_close, 0);
    if (g_basi && g_basi->net && g_basi->net->ws_open && us->len > 0) {
        int id = g_basi->net->ws_open(us->data);
        if (id >= 0) {
            js_set_prop(ctx, jv_obj(o), "__wsid__", jv_num((double)id));
            js_set_prop(ctx, jv_obj(o), "readyState", jv_num(1));
            for (int i = 0; i < 4; i++) {
                if (!g_js_ws[i].used) { g_js_ws[i].used = 1; g_js_ws[i].ws_id = id; g_js_ws[i].obj = o; break; }
            }
        }
    }
    return jv_obj(o);
}

/* --- Motor Kurulumu & Reset --- */
static void js_reset(void) {
    ws_close_all();
    g_js = 0;
    js_free_all();
}

static js_ctx_t *js_init(void) {
    js_reset();
    js_ctx_t *ctx = (js_ctx_t *)js_calloc(1, sizeof(js_ctx_t));
    if (!ctx) return 0;
    g_js = ctx;
    ctx->max_steps = 400000;
    ctx->max_depth = 96;
    ctx->time_origin = js_ticks_now();

    /* Prototipler */
    ctx->obj_proto  = js_obj_new(ctx, OBJ_PLAIN, 0); ctx->obj_proto->proto = 0;
    ctx->fn_proto   = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    ctx->arr_proto  = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    ctx->str_proto  = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    ctx->num_proto  = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    ctx->bool_proto = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    ctx->re_proto   = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    ctx->map_proto  = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    ctx->set_proto  = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    ctx->prom_proto = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    ctx->date_proto = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    ctx->err_proto  = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    ctx->ta_proto   = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    ctx->ab_proto   = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    ctx->dom_proto  = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    ctx->url_proto  = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    ctx->urlp_proto = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);

    /* Yerlesik semboller */
    js_val_t s1 = jv_cstr("Symbol.iterator"), s2 = jv_cstr("Symbol.toPrimitive"), s3 = jv_cstr("Symbol.toStringTag");
    ctx->sym_iterator  = jn_sym_ctor(ctx, jv_undef(), &s1, 1, 0).u.sym;
    ctx->sym_to_prim   = jn_sym_ctor(ctx, jv_undef(), &s2, 1, 0).u.sym;
    ctx->sym_to_strtag = jn_sym_ctor(ctx, jv_undef(), &s3, 1, 0).u.sym;

    /* Global ortam & nesne */
    ctx->global_obj = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    ctx->global_env = js_env_new(ctx, 0, 1);
    ctx->global_env->has_this = 1;
    ctx->global_env->this_val = jv_obj(ctx->global_obj);
    js_obj_t *G = ctx->global_obj;

    /* Object.prototype & Object */
    js_def_native(ctx, ctx->obj_proto, "hasOwnProperty", jn_obj_proto_hasOwn, 0);
    js_def_native(ctx, ctx->obj_proto, "toString", jn_obj_proto_toString, 0);
    js_def_native(ctx, ctx->obj_proto, "valueOf", jn_obj_proto_valueOf, 0);
    js_obj_t *Obj = js_native_new(ctx, jn_obj_ctor, 0, "Object");
    js_def_prop(Obj, "prototype", jv_obj(ctx->obj_proto), 0);
    js_def_native(ctx, Obj, "keys", jn_obj_keys_vals_ent, (void *)0);
    js_def_native(ctx, Obj, "values", jn_obj_keys_vals_ent, (void *)1);
    js_def_native(ctx, Obj, "entries", jn_obj_keys_vals_ent, (void *)2);
    js_def_native(ctx, Obj, "getOwnPropertyNames", jn_obj_keys_vals_ent, (void *)3);
    js_def_native(ctx, Obj, "fromEntries", jn_obj_fromEntries, 0);
    js_def_native(ctx, Obj, "assign", jn_obj_assign, 0);
    js_def_native(ctx, Obj, "create", jn_obj_create, 0);
    js_def_native(ctx, Obj, "freeze", jn_obj_freeze_seal, (void *)0);
    js_def_native(ctx, Obj, "seal", jn_obj_freeze_seal, (void *)1);
    js_def_native(ctx, Obj, "isFrozen", jn_obj_freeze_seal, (void *)2);
    js_def_native(ctx, Obj, "isSealed", jn_obj_freeze_seal, (void *)3);
    js_def_native(ctx, Obj, "is", jn_obj_is, 0);
    js_def_native(ctx, Obj, "hasOwn", jn_obj_hasOwn, 0);
    js_def_native(ctx, Obj, "defineProperty", jn_obj_defineProperty, 0);
    js_def_native(ctx, Obj, "getOwnPropertyDescriptor", jn_obj_getOwnPropDesc, 0);
    js_def_native(ctx, Obj, "getPrototypeOf", jn_obj_getProto, 0);
    js_def_native(ctx, Obj, "setPrototypeOf", jn_obj_setProto, 0);
    js_def_native(ctx, Obj, "groupBy", jn_obj_groupBy, 0);
    js_def_prop(G, "Object", jv_obj(Obj), PROP_WRIT | PROP_CONF);

    /* Function.prototype */
    js_def_native(ctx, ctx->fn_proto, "call", jn_fn_call_apply_bind, (void *)0);
    js_def_native(ctx, ctx->fn_proto, "apply", jn_fn_call_apply_bind, (void *)1);
    js_def_native(ctx, ctx->fn_proto, "bind", jn_fn_call_apply_bind, (void *)2);
    js_obj_t *Fn = js_native_new(ctx, jn_eval, 0, "Function");
    js_def_prop(Fn, "prototype", jv_obj(ctx->fn_proto), 0);
    js_def_prop(G, "Function", jv_obj(Fn), PROP_WRIT | PROP_CONF);

    /* Array.prototype & Array */
    js_def_native(ctx, ctx->arr_proto, "push", jn_arr_push, 0);
    js_def_native(ctx, ctx->arr_proto, "pop", jn_arr_pop, 0);
    js_def_native(ctx, ctx->arr_proto, "shift", jn_arr_shift, 0);
    js_def_native(ctx, ctx->arr_proto, "unshift", jn_arr_unshift, 0);
    js_def_native(ctx, ctx->arr_proto, "join", jn_arr_join, 0);
    js_def_native(ctx, ctx->arr_proto, "toString", jn_arr_join, 0);
    js_def_native(ctx, ctx->arr_proto, "slice", jn_arr_slice, 0);
    js_def_native(ctx, ctx->arr_proto, "splice", jn_arr_splice, (void *)0);
    js_def_native(ctx, ctx->arr_proto, "toSpliced", jn_arr_splice, (void *)1);
    js_def_native(ctx, ctx->arr_proto, "concat", jn_arr_concat, 0);
    js_def_native(ctx, ctx->arr_proto, "reverse", jn_arr_reverse, (void *)0);
    js_def_native(ctx, ctx->arr_proto, "toReversed", jn_arr_reverse, (void *)1);
    js_def_native(ctx, ctx->arr_proto, "sort", jn_arr_sort, (void *)0);
    js_def_native(ctx, ctx->arr_proto, "toSorted", jn_arr_sort, (void *)1);
    js_def_native(ctx, ctx->arr_proto, "with", jn_arr_with, 0);
    js_def_native(ctx, ctx->arr_proto, "at", jn_arr_at, 0);
    js_def_native(ctx, ctx->arr_proto, "indexOf", jn_arr_index, (void *)0);
    js_def_native(ctx, ctx->arr_proto, "lastIndexOf", jn_arr_index, (void *)1);
    js_def_native(ctx, ctx->arr_proto, "includes", jn_arr_index, (void *)2);
    js_def_native(ctx, ctx->arr_proto, "forEach", jn_arr_hof, (void *)0);
    js_def_native(ctx, ctx->arr_proto, "map", jn_arr_hof, (void *)1);
    js_def_native(ctx, ctx->arr_proto, "filter", jn_arr_hof, (void *)2);
    js_def_native(ctx, ctx->arr_proto, "find", jn_arr_hof, (void *)3);
    js_def_native(ctx, ctx->arr_proto, "findIndex", jn_arr_hof, (void *)4);
    js_def_native(ctx, ctx->arr_proto, "every", jn_arr_hof, (void *)5);
    js_def_native(ctx, ctx->arr_proto, "some", jn_arr_hof, (void *)6);
    js_def_native(ctx, ctx->arr_proto, "findLast", jn_arr_hof, (void *)7);
    js_def_native(ctx, ctx->arr_proto, "findLastIndex", jn_arr_hof, (void *)8);
    js_def_native(ctx, ctx->arr_proto, "flatMap", jn_arr_hof, (void *)9);
    js_def_native(ctx, ctx->arr_proto, "reduce", jn_arr_reduce, (void *)0);
    js_def_native(ctx, ctx->arr_proto, "reduceRight", jn_arr_reduce, (void *)1);
    js_def_native(ctx, ctx->arr_proto, "flat", jn_arr_flat, 0);
    js_def_native(ctx, ctx->arr_proto, "fill", jn_arr_fill, 0);
    js_def_native(ctx, ctx->arr_proto, "keys", jn_arr_kvent, (void *)0);
    js_def_native(ctx, ctx->arr_proto, "values", jn_arr_kvent, (void *)1);
    js_def_native(ctx, ctx->arr_proto, "entries", jn_arr_kvent, (void *)2);
    js_obj_t *Arr = js_native_new(ctx, jn_arr_ctor, 0, "Array");
    js_def_prop(Arr, "prototype", jv_obj(ctx->arr_proto), 0);
    js_def_native(ctx, Arr, "isArray", jn_arr_isArray, 0);
    js_def_native(ctx, Arr, "from", jn_arr_from, 0);
    js_def_native(ctx, Arr, "of", jn_arr_of, 0);
    js_def_prop(G, "Array", jv_obj(Arr), PROP_WRIT | PROP_CONF);

    /* String.prototype & String */
    js_def_native(ctx, ctx->str_proto, "charAt", jn_str_charAt, (void *)0);
    js_def_native(ctx, ctx->str_proto, "charCodeAt", jn_str_charAt, (void *)1);
    js_def_native(ctx, ctx->str_proto, "codePointAt", jn_str_charAt, (void *)1);
    js_def_native(ctx, ctx->str_proto, "at", jn_str_charAt, (void *)2);
    js_def_native(ctx, ctx->str_proto, "indexOf", jn_str_indexOf, (void *)0);
    js_def_native(ctx, ctx->str_proto, "lastIndexOf", jn_str_indexOf, (void *)1);
    js_def_native(ctx, ctx->str_proto, "includes", jn_str_indexOf, (void *)2);
    js_def_native(ctx, ctx->str_proto, "startsWith", jn_str_indexOf, (void *)3);
    js_def_native(ctx, ctx->str_proto, "endsWith", jn_str_indexOf, (void *)4);
    js_def_native(ctx, ctx->str_proto, "slice", jn_str_slice, (void *)0);
    js_def_native(ctx, ctx->str_proto, "substring", jn_str_slice, (void *)1);
    js_def_native(ctx, ctx->str_proto, "substr", jn_str_slice, (void *)2);
    js_def_native(ctx, ctx->str_proto, "toLowerCase", jn_str_case, (void *)0);
    js_def_native(ctx, ctx->str_proto, "toUpperCase", jn_str_case, (void *)1);
    js_def_native(ctx, ctx->str_proto, "trim", jn_str_trim, (void *)0);
    js_def_native(ctx, ctx->str_proto, "trimStart", jn_str_trim, (void *)1);
    js_def_native(ctx, ctx->str_proto, "trimLeft", jn_str_trim, (void *)1);
    js_def_native(ctx, ctx->str_proto, "trimEnd", jn_str_trim, (void *)2);
    js_def_native(ctx, ctx->str_proto, "trimRight", jn_str_trim, (void *)2);
    js_def_native(ctx, ctx->str_proto, "padStart", jn_str_pad, (void *)0);
    js_def_native(ctx, ctx->str_proto, "padEnd", jn_str_pad, (void *)1);
    js_def_native(ctx, ctx->str_proto, "repeat", jn_str_repeat, 0);
    js_def_native(ctx, ctx->str_proto, "concat", jn_str_concat, 0);
    js_def_native(ctx, ctx->str_proto, "split", jn_str_split, 0);
    js_def_native(ctx, ctx->str_proto, "replace", jn_str_replace, (void *)0);
    js_def_native(ctx, ctx->str_proto, "replaceAll", jn_str_replace, (void *)1);
    js_def_native(ctx, ctx->str_proto, "match", jn_str_match, (void *)0);
    js_def_native(ctx, ctx->str_proto, "search", jn_str_match, (void *)1);
    js_def_native(ctx, ctx->str_proto, "matchAll", jn_str_match, (void *)2);
    js_def_native(ctx, ctx->str_proto, "localeCompare", jn_str_localeCompare, 0);
    js_obj_t *Str = js_native_new(ctx, jn_str_ctor, 0, "String");
    js_def_prop(Str, "prototype", jv_obj(ctx->str_proto), 0);
    js_def_native(ctx, Str, "fromCharCode", jn_str_fromCharCode, 0);
    js_def_native(ctx, Str, "fromCodePoint", jn_str_fromCharCode, 0);
    js_def_prop(G, "String", jv_obj(Str), PROP_WRIT | PROP_CONF);

    /* Number, Boolean, BigInt, Symbol */
    js_def_native(ctx, ctx->num_proto, "toFixed", jn_num_toFixed, 0);
    js_def_native(ctx, ctx->num_proto, "toString", jn_num_toString, 0);
    js_obj_t *Num = js_native_new(ctx, jn_num_ctor, 0, "Number");
    js_def_prop(Num, "prototype", jv_obj(ctx->num_proto), 0);
    js_def_native(ctx, Num, "isNaN", jn_num_isNaN_s, 0);
    js_def_native(ctx, Num, "isFinite", jn_num_isFin_s, 0);
    js_def_native(ctx, Num, "isInteger", jn_num_isInt, (void *)0);
    js_def_native(ctx, Num, "isSafeInteger", jn_num_isInt, (void *)1);
    js_def_native(ctx, Num, "parseInt", jn_parseInt, 0);
    js_def_native(ctx, Num, "parseFloat", jn_parseFloat, 0);
    js_def_prop(Num, "MAX_SAFE_INTEGER", jv_num(9007199254740991.0), 0);
    js_def_prop(Num, "MIN_SAFE_INTEGER", jv_num(-9007199254740991.0), 0);
    js_def_prop(Num, "MAX_VALUE", jv_num(1.7976931348623157e308), 0);
    js_def_prop(Num, "MIN_VALUE", jv_num(5e-324), 0);
    js_def_prop(Num, "EPSILON", jv_num(2.220446049250313e-16), 0);
    js_def_prop(Num, "POSITIVE_INFINITY", jv_num(js_inf()), 0);
    js_def_prop(Num, "NEGATIVE_INFINITY", jv_num(-js_inf()), 0);
    js_def_prop(Num, "NaN", jv_num(js_nan()), 0);
    js_def_prop(G, "Number", jv_obj(Num), PROP_WRIT | PROP_CONF);
    js_def_prop(G, "Boolean", jv_obj(js_native_new(ctx, jn_bool_ctor, 0, "Boolean")), PROP_WRIT | PROP_CONF);
    js_def_prop(G, "BigInt", jv_obj(js_native_new(ctx, jn_bigint_ctor, 0, "BigInt")), PROP_WRIT | PROP_CONF);
    js_obj_t *Sym = js_native_new(ctx, jn_sym_ctor, 0, "Symbol");
    js_def_native(ctx, Sym, "for", jn_sym_for, 0);
    js_def_prop(Sym, "iterator", jv_sym(ctx->sym_iterator), 0);
    js_def_prop(Sym, "toPrimitive", jv_sym(ctx->sym_to_prim), 0);
    js_def_prop(Sym, "toStringTag", jv_sym(ctx->sym_to_strtag), 0);
    js_def_prop(G, "Symbol", jv_obj(Sym), PROP_WRIT | PROP_CONF);

    /* Math */
    js_obj_t *M = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    js_def_prop(M, "PI", jv_num(3.14159265358979323846), 0);
    js_def_prop(M, "E", jv_num(2.71828182845904523536), 0);
    js_def_prop(M, "LN2", jv_num(0.69314718055994530942), 0);
    js_def_prop(M, "LN10", jv_num(2.30258509299404568402), 0);
    js_def_prop(M, "LOG2E", jv_num(1.44269504088896340736), 0);
    js_def_prop(M, "LOG10E", jv_num(0.43429448190325182765), 0);
    js_def_prop(M, "SQRT2", jv_num(1.41421356237309504880), 0);
    js_def_prop(M, "SQRT1_2", jv_num(0.70710678118654752440), 0);
    static const char *mnames[] = {
        "abs","floor","ceil","round","trunc","sign","sqrt","cbrt","pow","exp","expm1",
        "log","log10","log2","log1p","sin","cos","tan","asin","acos","atan","atan2",
        "sinh","cosh","tanh","asinh","acosh","atanh","random","min","max","hypot","imul","clz32","fround"
    };
    for (int i = 0; i < 35; i++) js_def_native(ctx, M, mnames[i], jn_math_fn, (void *)(intptr_t)(i + 1));
    js_def_prop(G, "Math", jv_obj(M), PROP_WRIT | PROP_CONF);

    /* JSON */
    js_obj_t *J = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    js_def_native(ctx, J, "parse", jn_json_parse, 0);
    js_def_native(ctx, J, "stringify", jn_json_stringify, 0);
    js_def_prop(G, "JSON", jv_obj(J), PROP_WRIT | PROP_CONF);

    /* RegExp */
    js_def_native(ctx, ctx->re_proto, "exec", jn_re_exec_test, (void *)0);
    js_def_native(ctx, ctx->re_proto, "test", jn_re_exec_test, (void *)1);
    js_obj_t *Re = js_native_new(ctx, jn_re_ctor, 0, "RegExp");
    js_def_prop(Re, "prototype", jv_obj(ctx->re_proto), 0);
    js_def_prop(G, "RegExp", jv_obj(Re), PROP_WRIT | PROP_CONF);

    /* Map / WeakMap / Set / WeakSet */
    js_def_native(ctx, ctx->map_proto, "set", jn_map_ops, (void *)0);
    js_def_native(ctx, ctx->map_proto, "get", jn_map_ops, (void *)1);
    js_def_native(ctx, ctx->map_proto, "has", jn_map_ops, (void *)2);
    js_def_native(ctx, ctx->map_proto, "delete", jn_map_ops, (void *)3);
    js_def_native(ctx, ctx->map_proto, "clear", jn_map_ops, (void *)4);
    js_def_native(ctx, ctx->map_proto, "forEach", jn_map_ops, (void *)5);
    js_def_native(ctx, ctx->map_proto, "keys", jn_map_ops, (void *)6);
    js_def_native(ctx, ctx->map_proto, "values", jn_map_ops, (void *)7);
    js_def_native(ctx, ctx->map_proto, "entries", jn_map_ops, (void *)8);
    js_obj_t *Mp = js_native_new(ctx, jn_map_ctor, 0, "Map");
    js_def_prop(Mp, "prototype", jv_obj(ctx->map_proto), 0);
    js_def_native(ctx, Mp, "groupBy", jn_map_groupBy, 0);
    js_def_prop(G, "Map", jv_obj(Mp), PROP_WRIT | PROP_CONF);
    js_def_prop(G, "WeakMap", jv_obj(Mp), PROP_WRIT | PROP_CONF);

    js_def_native(ctx, ctx->set_proto, "add", jn_set_ops, (void *)0);
    js_def_native(ctx, ctx->set_proto, "has", jn_set_ops, (void *)1);
    js_def_native(ctx, ctx->set_proto, "delete", jn_set_ops, (void *)2);
    js_def_native(ctx, ctx->set_proto, "clear", jn_set_ops, (void *)3);
    js_def_native(ctx, ctx->set_proto, "forEach", jn_set_ops, (void *)4);
    js_def_native(ctx, ctx->set_proto, "values", jn_set_ops, (void *)5);
    js_def_native(ctx, ctx->set_proto, "keys", jn_set_ops, (void *)5);
    js_def_native(ctx, ctx->set_proto, "entries", jn_set_ops, (void *)6);
    js_def_native(ctx, ctx->set_proto, "union", jn_set_ops, (void *)7);
    js_def_native(ctx, ctx->set_proto, "intersection", jn_set_ops, (void *)8);
    js_def_native(ctx, ctx->set_proto, "difference", jn_set_ops, (void *)9);
    js_def_native(ctx, ctx->set_proto, "symmetricDifference", jn_set_ops, (void *)10);
    js_def_native(ctx, ctx->set_proto, "isSubsetOf", jn_set_ops, (void *)11);
    js_def_native(ctx, ctx->set_proto, "isSupersetOf", jn_set_ops, (void *)12);
    js_def_native(ctx, ctx->set_proto, "isDisjointFrom", jn_set_ops, (void *)13);
    js_obj_t *St = js_native_new(ctx, jn_set_ctor, 0, "Set");
    js_def_prop(St, "prototype", jv_obj(ctx->set_proto), 0);
    js_def_prop(G, "Set", jv_obj(St), PROP_WRIT | PROP_CONF);
    js_def_prop(G, "WeakSet", jv_obj(St), PROP_WRIT | PROP_CONF);

    /* Promise */
    js_def_native(ctx, ctx->prom_proto, "then", jn_prom_then, (void *)0);
    js_def_native(ctx, ctx->prom_proto, "catch", jn_prom_then, (void *)1);
    js_def_native(ctx, ctx->prom_proto, "finally", jn_prom_then, (void *)2);
    js_obj_t *Pr = js_native_new(ctx, jn_prom_ctor, 0, "Promise");
    js_def_prop(Pr, "prototype", jv_obj(ctx->prom_proto), 0);
    js_def_native(ctx, Pr, "resolve", jn_prom_static, (void *)0);
    js_def_native(ctx, Pr, "reject", jn_prom_static, (void *)1);
    js_def_native(ctx, Pr, "all", jn_prom_static, (void *)2);
    js_def_native(ctx, Pr, "race", jn_prom_static, (void *)3);
    js_def_native(ctx, Pr, "allSettled", jn_prom_static, (void *)4);
    js_def_native(ctx, Pr, "any", jn_prom_static, (void *)5);
    js_def_native(ctx, Pr, "withResolvers", jn_prom_static, (void *)6);
    js_def_prop(G, "Promise", jv_obj(Pr), PROP_WRIT | PROP_CONF);

    /* Date */
    js_def_native(ctx, ctx->date_proto, "getTime", jn_date_ops, (void *)0);
    js_def_native(ctx, ctx->date_proto, "valueOf", jn_date_ops, (void *)0);
    js_def_native(ctx, ctx->date_proto, "getFullYear", jn_date_ops, (void *)1);
    js_def_native(ctx, ctx->date_proto, "getUTCFullYear", jn_date_ops, (void *)1);
    js_def_native(ctx, ctx->date_proto, "getMonth", jn_date_ops, (void *)2);
    js_def_native(ctx, ctx->date_proto, "getUTCMonth", jn_date_ops, (void *)2);
    js_def_native(ctx, ctx->date_proto, "getDate", jn_date_ops, (void *)3);
    js_def_native(ctx, ctx->date_proto, "getUTCDate", jn_date_ops, (void *)3);
    js_def_native(ctx, ctx->date_proto, "getDay", jn_date_ops, (void *)4);
    js_def_native(ctx, ctx->date_proto, "getHours", jn_date_ops, (void *)5);
    js_def_native(ctx, ctx->date_proto, "getUTCHours", jn_date_ops, (void *)5);
    js_def_native(ctx, ctx->date_proto, "getMinutes", jn_date_ops, (void *)6);
    js_def_native(ctx, ctx->date_proto, "getUTCMinutes", jn_date_ops, (void *)6);
    js_def_native(ctx, ctx->date_proto, "getSeconds", jn_date_ops, (void *)7);
    js_def_native(ctx, ctx->date_proto, "getUTCSeconds", jn_date_ops, (void *)7);
    js_def_native(ctx, ctx->date_proto, "getMilliseconds", jn_date_ops, (void *)8);
    js_def_native(ctx, ctx->date_proto, "toISOString", jn_date_ops, (void *)9);
    js_def_native(ctx, ctx->date_proto, "toJSON", jn_date_ops, (void *)9);
    js_def_native(ctx, ctx->date_proto, "toString", jn_date_ops, (void *)9);
    js_def_native(ctx, ctx->date_proto, "setTime", jn_date_ops, (void *)10);
    js_obj_t *Dt = js_native_new(ctx, jn_date_ctor, 0, "Date");
    js_def_prop(Dt, "prototype", jv_obj(ctx->date_proto), 0);
    js_def_native(ctx, Dt, "now", jn_date_now, 0);
    js_def_native(ctx, Dt, "UTC", jn_date_utc, 0);
    js_def_native(ctx, Dt, "parse", jn_date_utc, 0);
    js_def_prop(G, "Date", jv_obj(Dt), PROP_WRIT | PROP_CONF);

    /* Errors */
    js_def_native(ctx, ctx->err_proto, "toString", jn_err_toString, 0);
    static const char *enames[] = { "Error","TypeError","ReferenceError","SyntaxError","RangeError","URIError","EvalError","AggregateError" };
    for (int i = 0; i < 8; i++) {
        js_obj_t *ec = js_native_new(ctx, jn_err_ctor, (void *)enames[i], enames[i]);
        js_def_prop(ec, "prototype", jv_obj(ctx->err_proto), 0);
        js_def_prop(G, enames[i], jv_obj(ec), PROP_WRIT | PROP_CONF);
    }

    /* Proxy & Reflect */
    js_def_prop(G, "Proxy", jv_obj(js_native_new(ctx, jn_proxy_ctor, 0, "Proxy")), PROP_WRIT | PROP_CONF);
    js_obj_t *Ref = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    js_def_native(ctx, Ref, "get", jn_reflect_ops, (void *)0);
    js_def_native(ctx, Ref, "set", jn_reflect_ops, (void *)1);
    js_def_native(ctx, Ref, "has", jn_reflect_ops, (void *)2);
    js_def_native(ctx, Ref, "deleteProperty", jn_reflect_ops, (void *)3);
    js_def_native(ctx, Ref, "apply", jn_reflect_ops, (void *)4);
    js_def_native(ctx, Ref, "construct", jn_reflect_ops, (void *)5);
    js_def_native(ctx, Ref, "ownKeys", jn_reflect_ops, (void *)6);
    js_def_prop(G, "Reflect", jv_obj(Ref), PROP_WRIT | PROP_CONF);

    /* ArrayBuffer, TypedArrays, TextEncoder/Decoder */
    js_def_native(ctx, ctx->ta_proto, "subarray", jn_ta_subarray_slice, (void *)0);
    js_def_native(ctx, ctx->ta_proto, "slice", jn_ta_subarray_slice, (void *)1);
    js_def_prop(G, "ArrayBuffer", jv_obj(js_native_new(ctx, jn_ab_ctor, 0, "ArrayBuffer")), PROP_WRIT | PROP_CONF);
    static const char *tanames[] = { "Uint8Array","Int8Array","Uint8ClampedArray","Int16Array","Uint16Array","Int32Array","Uint32Array","Float32Array","Float64Array" };
    for (int i = 0; i < 9; i++) {
        js_obj_t *tc = js_native_new(ctx, jn_ta_ctor, (void *)(intptr_t)i, tanames[i]);
        js_def_prop(tc, "prototype", jv_obj(ctx->ta_proto), 0);
        js_def_prop(G, tanames[i], jv_obj(tc), PROP_WRIT | PROP_CONF);
    }
    js_def_prop(G, "TextEncoder", jv_obj(js_native_new(ctx, jn_te_ctor, (void *)0, "TextEncoder")), PROP_WRIT | PROP_CONF);
    js_def_prop(G, "TextDecoder", jv_obj(js_native_new(ctx, jn_te_ctor, (void *)1, "TextDecoder")), PROP_WRIT | PROP_CONF);

    /* URL & URLSearchParams */
    js_def_native(ctx, ctx->urlp_proto, "get", jn_urlp_ops, (void *)0);
    js_def_native(ctx, ctx->urlp_proto, "set", jn_urlp_ops, (void *)1);
    js_def_native(ctx, ctx->urlp_proto, "append", jn_urlp_ops, (void *)1);
    js_def_native(ctx, ctx->urlp_proto, "has", jn_urlp_ops, (void *)2);
    js_def_native(ctx, ctx->urlp_proto, "delete", jn_urlp_ops, (void *)3);
    js_def_native(ctx, ctx->urlp_proto, "toString", jn_urlp_ops, (void *)4);
    js_def_prop(G, "URLSearchParams", jv_obj(js_native_new(ctx, jn_urlp_ctor, 0, "URLSearchParams")), PROP_WRIT | PROP_CONF);
    js_def_prop(G, "URL", jv_obj(js_native_new(ctx, jn_url_ctor, 0, "URL")), PROP_WRIT | PROP_CONF);

    /* Global fonksiyonlar ve sabitler */
    js_def_prop(G, "undefined", jv_undef(), 0);
    js_def_prop(G, "NaN", jv_num(js_nan()), 0);
    js_def_prop(G, "Infinity", jv_num(js_inf()), 0);
    js_def_prop(G, "globalThis", jv_obj(G), PROP_WRIT | PROP_CONF);
    js_def_prop(G, "window", jv_obj(G), PROP_WRIT | PROP_CONF);
    js_def_prop(G, "self", jv_obj(G), PROP_WRIT | PROP_CONF);
    js_def_native(ctx, G, "isNaN", jn_isNaN, 0);
    js_def_native(ctx, G, "isFinite", jn_isFinite, 0);
    js_def_native(ctx, G, "parseInt", jn_parseInt, 0);
    js_def_native(ctx, G, "parseFloat", jn_parseFloat, 0);
    js_def_native(ctx, G, "encodeURI", jn_encodeURI, (void *)0);
    js_def_native(ctx, G, "encodeURIComponent", jn_encodeURI, (void *)1);
    js_def_native(ctx, G, "decodeURI", jn_decodeURI, 0);
    js_def_native(ctx, G, "decodeURIComponent", jn_decodeURI, 0);
    js_def_native(ctx, G, "btoa", jn_btoa, 0);
    js_def_native(ctx, G, "atob", jn_atob, 0);
    js_def_native(ctx, G, "eval", jn_eval, 0);
    js_def_native(ctx, G, "queueMicrotask", jn_queueMicrotask, 0);
    js_def_native(ctx, G, "structuredClone", jn_structuredClone, 0);

    /* Console */
    js_obj_t *Con = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    js_def_native(ctx, Con, "log", jn_console_log, (void *)"");
    js_def_native(ctx, Con, "info", jn_console_log, (void *)"[info] ");
    js_def_native(ctx, Con, "warn", jn_console_log, (void *)"[warn] ");
    js_def_native(ctx, Con, "error", jn_console_log, (void *)"[error] ");
    js_def_native(ctx, Con, "debug", jn_console_log, (void *)"[debug] ");
    js_def_native(ctx, Con, "assert", jn_console_assert, 0);
    js_def_native(ctx, Con, "clear", jn_console_clear, 0);
    js_def_prop(G, "console", jv_obj(Con), PROP_WRIT | PROP_CONF);

    /* DOM Element prototype */
    js_def_native(ctx, ctx->dom_proto, "querySelector", jn_dom_qs, (void *)0);
    js_def_native(ctx, ctx->dom_proto, "querySelectorAll", jn_dom_qs, (void *)1);
    js_def_native(ctx, ctx->dom_proto, "getElementsByTagName", jn_dom_getByTagCls, (void *)0);
    js_def_native(ctx, ctx->dom_proto, "getElementsByClassName", jn_dom_getByTagCls, (void *)1);
    js_def_native(ctx, ctx->dom_proto, "getAttribute", jn_dom_attr, (void *)0);
    js_def_native(ctx, ctx->dom_proto, "setAttribute", jn_dom_attr, (void *)1);
    js_def_native(ctx, ctx->dom_proto, "removeAttribute", jn_dom_attr, (void *)2);
    js_def_native(ctx, ctx->dom_proto, "hasAttribute", jn_dom_attr, (void *)3);
    js_def_native(ctx, ctx->dom_proto, "appendChild", jn_dom_tree, (void *)0);
    js_def_native(ctx, ctx->dom_proto, "removeChild", jn_dom_tree, (void *)1);
    js_def_native(ctx, ctx->dom_proto, "remove", jn_dom_tree, (void *)2);
    js_def_native(ctx, ctx->dom_proto, "append", jn_dom_tree, (void *)3);
    js_def_native(ctx, ctx->dom_proto, "prepend", jn_dom_tree, (void *)4);
    js_def_native(ctx, ctx->dom_proto, "insertBefore", jn_dom_tree, (void *)5);
    js_def_native(ctx, ctx->dom_proto, "cloneNode", jn_dom_tree, (void *)6);
    js_def_native(ctx, ctx->dom_proto, "matches", jn_dom_tree, (void *)7);
    js_def_native(ctx, ctx->dom_proto, "closest", jn_dom_tree, (void *)8);
    js_def_native(ctx, ctx->dom_proto, "contains", jn_dom_tree, (void *)9);
    js_def_native(ctx, ctx->dom_proto, "getBoundingClientRect", jn_dom_rect, 0);
    js_def_native(ctx, ctx->dom_proto, "focus", jn_dom_focus_click, (void *)0);
    js_def_native(ctx, ctx->dom_proto, "blur", jn_dom_focus_click, (void *)1);
    js_def_native(ctx, ctx->dom_proto, "click", jn_dom_focus_click, (void *)2);
    js_def_native(ctx, ctx->dom_proto, "scrollIntoView", jn_dom_focus_click, (void *)3);
    js_def_native(ctx, ctx->dom_proto, "addEventListener", jn_addEvent, 0);
    js_def_native(ctx, ctx->dom_proto, "removeEventListener", jn_remEvent, 0);
    js_def_native(ctx, ctx->dom_proto, "dispatchEvent", jn_dispatchEvent, 0);
    js_def_native(ctx, ctx->dom_proto, "getContext", jn_canvas_getContext, 0);

    /* Document */
    js_obj_t *Doc = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    ctx->doc_obj = Doc;
    js_def_native(ctx, Doc, "getElementById", jn_doc_getById, 0);
    js_def_native(ctx, Doc, "querySelector", jn_dom_qs, (void *)0);
    js_def_native(ctx, Doc, "querySelectorAll", jn_dom_qs, (void *)1);
    js_def_native(ctx, Doc, "getElementsByTagName", jn_dom_getByTagCls, (void *)0);
    js_def_native(ctx, Doc, "getElementsByClassName", jn_dom_getByTagCls, (void *)1);
    js_def_native(ctx, Doc, "createElement", jn_doc_createEl, (void *)0);
    js_def_native(ctx, Doc, "createTextNode", jn_doc_createEl, (void *)1);
    js_def_native(ctx, Doc, "createDocumentFragment", jn_doc_createEl, (void *)2);
    js_def_native(ctx, Doc, "addEventListener", jn_addEvent, 0);
    js_def_native(ctx, Doc, "removeEventListener", jn_remEvent, 0);
    js_def_native(ctx, Doc, "dispatchEvent", jn_dispatchEvent, 0);
    /* Accessors on document */
    js_prop_t *pb = js_add_own_prop(Doc, "body", 0);
    pb->flags = PROP_ENUM | PROP_CONF | PROP_ACC; pb->getter = js_native_new(ctx, jn_doc_body_get, (void *)0, "get body");
    js_prop_t *pde = js_add_own_prop(Doc, "documentElement", 0);
    pde->flags = PROP_ENUM | PROP_CONF | PROP_ACC; pde->getter = js_native_new(ctx, jn_doc_body_get, (void *)1, "get documentElement");
    js_prop_t *pt = js_add_own_prop(Doc, "title", 0);
    pt->flags = PROP_ENUM | PROP_CONF | PROP_ACC;
    pt->getter = js_native_new(ctx, jn_doc_body_get, (void *)2, "get title");
    pt->setter = js_native_new(ctx, jn_doc_title_set, 0, "set title");
    js_prop_t *pc = js_add_own_prop(Doc, "cookie", 0);
    pc->flags = PROP_ENUM | PROP_CONF | PROP_ACC;
    pc->getter = js_native_new(ctx, jn_doc_body_get, (void *)3, "get cookie");
    pc->setter = js_native_new(ctx, jn_doc_cookie_set, 0, "set cookie");
    js_def_prop(Doc, "readyState", jv_cstr("interactive"), PROP_ENUM | PROP_WRIT | PROP_CONF);
    js_def_prop(Doc, "URL", jv_cstr(g_cur_url), PROP_ENUM | PROP_CONF);
    js_def_prop(G, "document", jv_obj(Doc), PROP_WRIT | PROP_CONF);

    /* Window / BOM: location, navigator, history, performance, storage, crypto, timers, fetch, events */
    js_val_t cu = jv_cstr(g_cur_url[0] ? g_cur_url : "about:blank");
    js_val_t loc = jn_url_ctor(ctx, jv_undef(), &cu, 1, 0);
    js_def_native(ctx, loc.u.o, "assign", jn_loc_assign, 0);
    js_def_native(ctx, loc.u.o, "replace", jn_loc_assign, 0);
    js_def_prop(G, "location", loc, PROP_WRIT | PROP_CONF);
    js_def_prop(Doc, "location", loc, PROP_WRIT | PROP_CONF);

    js_obj_t *Nav = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    js_def_prop(Nav, "userAgent", jv_cstr("Mozilla/5.0 (Arctian OS; x86_64) ArctianBrowser/1.0 ArctianJS/1.0"), PROP_ENUM);
    js_def_prop(Nav, "language", jv_cstr("tr-TR"), PROP_ENUM);
    js_def_prop(Nav, "platform", jv_cstr("Arctian x86_64"), PROP_ENUM);
    js_def_prop(Nav, "onLine", jv_bool(1), PROP_ENUM);
    js_def_prop(Nav, "hardwareConcurrency", jv_num(2), PROP_ENUM);
    js_def_prop(G, "navigator", jv_obj(Nav), PROP_WRIT | PROP_CONF);

    js_obj_t *Perf = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    js_def_native(ctx, Perf, "now", jn_perf_now, 0);
    js_def_prop(Perf, "timeOrigin", jv_num((double)ctx->time_origin), PROP_ENUM);
    js_def_prop(G, "performance", jv_obj(Perf), PROP_WRIT | PROP_CONF);

    js_obj_t *Cry = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
    js_def_native(ctx, Cry, "getRandomValues", jn_crypto_ops, (void *)0);
    js_def_native(ctx, Cry, "randomUUID", jn_crypto_ops, (void *)1);
    js_def_prop(G, "crypto", jv_obj(Cry), PROP_WRIT | PROP_CONF);

    ls_persist_load(ctx);
    for (int st = 0; st < 2; st++) {
        js_obj_t *S = js_obj_new(ctx, OBJ_PLAIN, ctx->obj_proto);
        js_def_native(ctx, S, "getItem", jn_storage_ops, (void *)(intptr_t)(st * 10 + 0));
        js_def_native(ctx, S, "setItem", jn_storage_ops, (void *)(intptr_t)(st * 10 + 1));
        js_def_native(ctx, S, "removeItem", jn_storage_ops, (void *)(intptr_t)(st * 10 + 2));
        js_def_native(ctx, S, "clear", jn_storage_ops, (void *)(intptr_t)(st * 10 + 3));
        js_def_native(ctx, S, "key", jn_storage_ops, (void *)(intptr_t)(st * 10 + 4));
        js_def_prop(G, st == 0 ? "localStorage" : "sessionStorage", jv_obj(S), PROP_WRIT | PROP_CONF);
    }
    js_def_prop(G, "XMLHttpRequest", jv_obj(js_native_new(ctx, jn_xhr_ctor, 0, "XMLHttpRequest")), PROP_WRIT | PROP_CONF);
    js_def_prop(G, "WebSocket", jv_obj(js_native_new(ctx, jn_ws_ctor, 0, "WebSocket")), PROP_WRIT | PROP_CONF);

    js_def_prop(G, "innerWidth", jv_num((double)g_view_w), PROP_ENUM | PROP_WRIT | PROP_CONF);
    js_def_prop(G, "innerHeight", jv_num((double)g_view_h), PROP_ENUM | PROP_WRIT | PROP_CONF);
    js_def_prop(G, "devicePixelRatio", jv_num(1.0), PROP_ENUM | PROP_WRIT | PROP_CONF);
    js_def_native(ctx, G, "setTimeout", jn_set_timer, (void *)0);
    js_def_native(ctx, G, "setInterval", jn_set_timer, (void *)1);
    js_def_native(ctx, G, "requestAnimationFrame", jn_set_timer, (void *)2);
    js_def_native(ctx, G, "clearTimeout", jn_clear_timer, 0);
    js_def_native(ctx, G, "clearInterval", jn_clear_timer, 0);
    js_def_native(ctx, G, "cancelAnimationFrame", jn_clear_timer, 0);
    js_def_native(ctx, G, "getComputedStyle", jn_getComputedStyle, 0);
    js_def_native(ctx, G, "matchMedia", jn_matchMedia, 0);
    js_def_native(ctx, G, "fetch", jn_fetch, 0);
    js_def_native(ctx, G, "scrollTo", jn_win_scroll, (void *)0);
    js_def_native(ctx, G, "scrollBy", jn_win_scroll, (void *)1);
    js_def_native(ctx, G, "alert", jn_win_alert, (void *)0);
    js_def_native(ctx, G, "confirm", jn_win_alert, (void *)1);
    js_def_native(ctx, G, "prompt", jn_win_alert, (void *)2);
    js_def_native(ctx, G, "addEventListener", jn_addEvent, 0);
    js_def_native(ctx, G, "removeEventListener", jn_remEvent, 0);
    js_def_native(ctx, G, "dispatchEvent", jn_dispatchEvent, 0);
    js_def_prop(G, "Event", jv_obj(js_native_new(ctx, jn_event_ctor, (void *)0, "Event")), PROP_WRIT | PROP_CONF);
    js_def_prop(G, "CustomEvent", jv_obj(js_native_new(ctx, jn_event_ctor, (void *)1, "CustomEvent")), PROP_WRIT | PROP_CONF);
    return ctx;
}

static int js_node_has_click_handler(int dom_idx) {
    if (dom_idx < 0 || dom_idx >= g_dn_count) return 0;
    char tmp[8];
    if (dn_attr(dom_idx, "onclick", tmp, sizeof(tmp))) return 1;
    if (!g_js) return 0;
    if (g_js->dom_cache && dom_idx < g_js->dom_cache_cap && g_js->dom_cache[dom_idx]) {
        uint32_t h = js_hash_bytes("onclick", 7);
        if (js_find_own_prop(g_js->dom_cache[dom_idx], "onclick", h, 0)) return 1;
    }
    for (int i = 0; i < g_js->listener_n; i++)
        if (g_js->listeners[i].active && g_js->listeners[i].dom_idx == dom_idx && str_eq(g_js->listeners[i].type, "click"))
            return 1;
    return 0;
}

static void run_page_scripts(void) {
    js_ctx_t *ctx = js_init();
    if (!ctx) return;
    if (g_script_len > 0) {
        g_script_src[g_script_len] = 0;
        ctx->steps = 0; ctx->throwing = 0;
        js_eval_source(ctx, g_script_src, g_script_len, ctx->global_env);
        ctx->throwing = 0;
    }
    js_def_prop(ctx->doc_obj, "readyState", jv_cstr("complete"), PROP_ENUM | PROP_WRIT | PROP_CONF);
    js_dispatch_dom_event(-1, "DOMContentLoaded");
    js_dispatch_dom_event(-2, "load");
    js_drain_microtasks(ctx);
}


static void render_page(const char *html) {
    if (!g_content) return;
    lv_obj_clean(g_content);
    if (g_fixed_layer) lv_obj_clean(g_fixed_layer);
    images_clear();
    g_css_count = 0;
    g_css_pool_len = 0;
    g_kf_count = 0;
    g_vstk_n = 0;
    g_vcount = 0;
    g_pform_count = 0;
    g_grid_slot = 0;
    g_cur_hash[0] = 0;
    g_hover_node = -1;
    g_focus_node = -1;
    g_root_font = 16;

    int vw = lv_obj_get_width(g_content);
    int vh = lv_obj_get_height(g_content);
    g_view_w = (vw > 40) ? vw : 1280;
    g_view_h = (vh > 40) ? vh : 800;

    lv_obj_set_style_bg_color(g_content, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(g_content, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_content, 0, 0);
    lv_obj_set_style_pad_row(g_content, 0, 0);
    lv_obj_set_flex_flow(g_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(g_content, LV_DIR_VER);
    lv_obj_remove_flag(g_content, LV_OBJ_FLAG_SCROLL_ELASTIC);

    if (g_group && g_addr) {
        lv_group_remove_all_objs(g_group);
        lv_group_add_obj(g_group, g_addr);
        if (s_find_ta) lv_group_add_obj(g_group, s_find_ta);
    }

    collect_css_from_html(html);
    dom_parse(html);
    run_page_scripts();
    mark_hover_sensitivity();

    css_style_t base;
    style_defaults(&base);
    if (g_dn_count >= 1) build_node(0, g_content, &base, g_view_w, g_view_h, 0);
    if (g_basi && g_basi->console && g_basi->console->puts)
        g_basi->console->puts("[browser] render ok\n");
}

/* ---------------- Asamali (Progressive) Kaynak Yukleme Kuyrugu ----------------
 * HTML indirildigi anda sayfa hemen cizilir; ardindan harici CSS, JS ve gorsel
 * kaynaklari netbg uzerinde tek tek indirilip her biri geldigi anda canli
 * sayfaya yansitilir. Kullanici baska bir sayfaya gecerse eski kuyruk aninda
 * iptal edilir. */
#define PQ_MAX 96
typedef struct {
    char url[440];
    int  is_css; /* 1=CSS, 2=JS, 0=IMG */
} pq_item_t;

static pq_item_t *g_pq;
static int        g_pq_count;
static int        g_pq_idx;
static int        g_pq_css_rem;
static int        g_pq_css_dirty;
static uint64_t   g_pq_last_restyle;
static int        g_pq_gen;
static int        g_pq_job_gen;
static char       g_pq_cur_url[440];
static int        g_pq_cur_kind;
static uint8_t   *g_pq_stage;
static uint32_t   g_pq_stage_len;

static void pq_cancel(void) {
    g_pq_gen++;
    g_pq_count = 0;
    g_pq_idx = 0;
    g_pq_css_rem = 0;
    g_pq_css_dirty = 0;
    g_pq_last_restyle = 0;
}

static int pq_has_url(const char *url) {
    if (res_find(url) >= 0) return 1;
    for (int i = 0; i < g_pq_count; i++)
        if (str_eq(g_pq[i].url, url)) return 1;
    return 0;
}

static void pq_push(const char *url, int is_css) {
    if (!url || !url[0] || !g_pq || g_pq_count >= PQ_MAX) return;
    if (!starts_with(url, "https://") && !starts_with(url, "http://")) return;
    if (pq_has_url(url)) return;
    bcopy_str(g_pq[g_pq_count].url, url, sizeof(g_pq[0].url));
    g_pq[g_pq_count].is_css = is_css;
    g_pq_count++;
    if (is_css == 1) g_pq_css_rem++;
}

static void pq_fmt_status(char *out, int cap, int cur, int tot, int kind) {
    const char *lbl = (kind == 1) ? "Yukleniyor: stil (" : (kind == 2) ? "Yukleniyor: betik (" : "Yukleniyor: gorsel (";
    bcopy_str(out, lbl, cap);
    char n1[16], n2[16]; int k1 = 0, k2 = 0;
    int v = cur; if (v <= 0) n1[k1++] = '0'; else { char t[16]; int ti = 0; while (v > 0 && ti < 15) { t[ti++] = (char)('0' + (v % 10)); v /= 10; } while (ti) n1[k1++] = t[--ti]; }
    n1[k1] = 0;
    v = tot; if (v <= 0) n2[k2++] = '0'; else { char t[16]; int ti = 0; while (v > 0 && ti < 15) { t[ti++] = (char)('0' + (v % 10)); v /= 10; } while (ti) n2[k2++] = t[--ti]; }
    n2[k2] = 0;
    bcat(out, n1, cap); bcat(out, "/", cap); bcat(out, n2, cap); bcat(out, ")...", cap);
}

static void browser_res_work(void *u);
static void browser_res_done(int status, void *u);

static int pq_submit_next(void) {
    if (!g_basi || !g_basi->net || !g_basi->net->submit || !g_pq) return 0;
    while (g_pq_idx < g_pq_count && res_find(g_pq[g_pq_idx].url) >= 0) {
        if (g_pq[g_pq_idx].is_css == 1 && g_pq_css_rem > 0) g_pq_css_rem--;
        g_pq_idx++;
    }
    if (g_pq_css_rem == 0 && g_pq_css_dirty > 0) {
        g_pq_css_dirty = 0;
        if (g_raw && find_substr_ci(g_raw, "<style")) collect_css_from_html(g_raw);
        restyle_page();
    }
    if (g_pq_idx >= g_pq_count) {
        mark_hover_sensitivity();
        set_status(g_cur_url, ADL_TEXT_DIM);
        if (g_basi->console && g_basi->console->puts)
            g_basi->console->puts("[browser] progressive done\n");
        return 0;
    }
    bcopy_str(g_pq_cur_url, g_pq[g_pq_idx].url, sizeof(g_pq_cur_url));
    g_pq_cur_kind = g_pq[g_pq_idx].is_css;
    g_pq_job_gen = g_pq_gen;
    char st[80];
    pq_fmt_status(st, sizeof(st), g_pq_idx + 1, g_pq_count, g_pq_cur_kind);
    set_status(st, ADL_WARN);
    if (g_basi->net->submit(browser_res_work, browser_res_done, 0) != 0) {
        set_status(g_cur_url, ADL_TEXT_DIM);
        return 0;
    }
    return 1;
}

static void browser_res_work(void *u) {
    (void)u;
    g_pq_stage = 0;
    g_pq_stage_len = 0;
    if (!g_basi || !g_basi->net || !g_basi->mem || !g_basi->mem->alloc || !g_pq_cur_url[0]) return;
    uint32_t len = 0;
    const char *b = 0;
    if (starts_with(g_pq_cur_url, "https://")) b = g_basi->net->https_get(g_pq_cur_url, &len);
    else if (starts_with(g_pq_cur_url, "http://")) b = g_basi->net->http_get(g_pq_cur_url, &len);
    if (!b || len == 0) return;
    if (len > g_res_item_cap) len = g_res_item_cap;
    uint8_t *d = (uint8_t *)g_basi->mem->alloc((size_t)len + 1);
    if (!d) return;
    for (uint32_t i = 0; i < len; i++) d[i] = (uint8_t)b[i];
    d[len] = 0;
    g_pq_stage = d;
    g_pq_stage_len = len;
}

static void browser_res_done(int status, void *u) {
    (void)status; (void)u;
    uint8_t *buf = g_pq_stage;
    uint32_t len = g_pq_stage_len;
    g_pq_stage = 0;
    g_pq_stage_len = 0;

    if (g_pq_job_gen != g_pq_gen || !g_content) {
        if (buf && g_basi && g_basi->mem && g_basi->mem->free) g_basi->mem->free(buf);
        if (g_nav_have_pending) {
            char next[URL_MAX];
            bcopy_str(next, g_nav_pending, sizeof(next));
            g_nav_have_pending = 0;
            navigate(next, 1);
        }
        return;
    }
    if (g_nav_have_pending) {
        if (buf && g_basi && g_basi->mem && g_basi->mem->free) g_basi->mem->free(buf);
        pq_cancel();
        char next[URL_MAX];
        bcopy_str(next, g_nav_pending, sizeof(next));
        g_nav_have_pending = 0;
        navigate(next, 1);
        return;
    }

    int kind = g_pq_cur_kind;
    if (kind == 1 && g_pq_css_rem > 0) g_pq_css_rem--;
    if (buf && len > 0) {
        int ri = res_add_owned(g_pq_cur_url, buf, len, kind);
        if (ri >= 0) {
            if (kind == 1) {
                /* Yeni gelen CSS kuralini ayristir; coklu CSS sitelerinde (orn. 20 dosya)
                 * her dosya icin tam LVGL yeniden kurulumu yerine akilli esiklerle ciz! */
                if (g_pq_css_rem == 0 && g_raw && find_substr_ci(g_raw, "<style"))
                    collect_css_from_html(g_raw);
                else
                    css_parse((const char *)g_res[ri].data);
                g_pq_css_dirty++;
                uint64_t now = (g_basi && g_basi->time && g_basi->time->ticks_ms) ? g_basi->time->ticks_ms() : 0;
                int big_css = (len >= 350u * 1024u) ? 1 : 0;
                int should_paint = (g_pq_css_rem == 0) ||
                                   (g_pq_count <= 4) ||
                                   (big_css && (now == 0 || now - g_pq_last_restyle >= 4000)) ||
                                   (g_pq_css_dirty >= 6 && (now == 0 || now - g_pq_last_restyle >= 6000));
                if (should_paint) {
                    g_pq_css_dirty = 0;
                    g_pq_last_restyle = now;
                    restyle_page();
                }
            } else if (kind == 2) {
                if (g_js && g_res[ri].data) {
                    g_js->steps = 0; g_js->throwing = 0;
                    js_eval_source(g_js, (const char *)g_res[ri].data, (int)g_res[ri].len, g_js->global_env);
                    g_js->throwing = 0;
                    js_drain_microtasks(g_js);
                    if (g_restyle_pending) { g_restyle_pending = 0; restyle_page(); }
                }
            } else if (kind == 0) {
                /* Gorsel geldiginde ilgili <img> kutularini yerinde (yeniden cizmeden) guncelle! */
                int iw = 0, ih = 0;
                uint32_t *px = img_get(g_pq_cur_url, &iw, &ih);
                if (px && iw > 0 && ih > 0) {
                    int patched = 0;
                    for (int i = 0; i < g_dn_count; i++) {
                        dnode_t *dn = &g_dn[i];
                        if (!dn->widget || !str_eq(dn->tag, "img") || !dn->src[0]) continue;
                        char abs[440]; resolve_url(g_cur_scheme, g_cur_host, dn->src, abs, sizeof(abs));
                        if (!str_eq(abs, g_pq_cur_url)) continue;
                        lv_obj_clean(dn->widget);
                        lv_obj_t *c = make_image(dn->widget, dn->src, dn->wa, dn->wa_pct, dn->ha, dn->ha_pct, 0);
                        if (c) lv_obj_center(c);
                        patched = 1;
                    }
                    if (!patched) restyle_page();
                }
            }
        }
    }

    g_pq_idx++;
    pq_submit_next();
}

static int pq_start_from_html(const char *html, const char *scheme, const char *host) {
    if (!html || !g_basi || !g_basi->mem || !g_basi->mem->alloc) return 0;
    if (!g_pq) {
        g_pq = (pq_item_t *)g_basi->mem->alloc((size_t)PQ_MAX * sizeof(pq_item_t));
        if (!g_pq) return 0;
    }
    g_pq_count = 0;
    g_pq_idx = 0;
    g_pq_css_rem = 0;

    /* 1. Oncelik: Harici CSS dosyalari (belge sirasinda) */
    const char *p = html;
    while (*p) {
        const char *tl = find_substr_ci(p, "<link");
        if (!tl) break;
        const char *q = tl; while (*q && *q != '>') q++;
        char rel[64], href[420];
        if (find_attr(tl, q, "rel", rel, sizeof(rel)) && find_substr_ci(rel, "stylesheet") &&
            find_attr(tl, q, "href", href, sizeof(href)) && href[0]) {
            char abs[440]; resolve_url(scheme, host, href, abs, sizeof(abs));
            pq_push(abs, 1);
        }
        p = (*q == '>') ? q + 1 : q;
    }

    /* 2. Oncelik: Harici JS betikleri (module chunk'lari haric) ve <img> gorselleri */
    p = html;
    int imgs = 0, jsn = 0;
    while (*p && g_pq_count < PQ_MAX) {
        const char *ti = find_substr_ci(p, "<img");
        const char *ts = find_substr_ci(p, "<script");
        const char *next = ti;
        if (ts && (!next || ts < next)) next = ts;
        if (!next) break;
        const char *q = next; while (*q && *q != '>') q++;
        char a[420];
        if (next == ti && imgs < 32) {
            if (find_attr(next, q, "src", a, sizeof(a)) && !starts_with(a, "data:") &&
                !find_substr_ci(a, ".webp") && !find_substr_ci(a, ".avif")) {
                char abs[440]; resolve_url(scheme, host, a, abs, sizeof(abs));
                pq_push(abs, 0);
                imgs++;
            }
        } else if (next == ts && jsn < 24) {
            char stype[48];
            if (!(find_attr(next, q, "type", stype, sizeof(stype)) && str_eq_ci(stype, "module"))) {
                if (find_attr(next, q, "src", a, sizeof(a)) && a[0]) {
                    char abs[440]; resolve_url(scheme, host, a, abs, sizeof(abs));
                    pq_push(abs, 2);
                    jsn++;
                }
            }
        }
        p = (*q == '>') ? q + 1 : q;
    }

    if (g_pq_count == 0) return 0;
    return pq_submit_next();
}

/* ---------------- Ust bar olaylari ---------------- */
#define HOME_URL   "https://lite.duckduckgo.com/lite/"
#define SEARCH_URL "https://lite.duckduckgo.com/lite/?q="

static int looks_like_query(const char *s) {
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    int len = b_len(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) len--;
    if (len == 0) return 0;
    for (int i = 0; i < len; i++) {
        if (s[i] == ' ' || s[i] == '\t') return 1;
    }
    if (starts_with(s, "http://") || starts_with(s, "https://")) return 0;
    if (str_eq(s, "localhost") || starts_with(s, "localhost:")) return 0;
    for (int i = 0; i < len; i++) {
        if (s[i] == '.' && i > 0 && i < len - 1) return 0;
        if (s[i] == ':' || s[i] == '/') return 0;
    }
    return 1;
}

static void go_cb(lv_event_t *e) {
    (void)e;
    const char *raw = lv_textarea_get_text(g_addr);
    if (!raw) return;
    while (*raw == ' ' || *raw == '\t') raw++;
    char trimmed[URL_MAX];
    bcopy_str(trimmed, raw, sizeof(trimmed));
    int tl = b_len(trimmed);
    while (tl > 0 && (trimmed[tl - 1] == ' ' || trimmed[tl - 1] == '\t')) trimmed[--tl] = 0;
    if (tl == 0) return;

    char url[URL_MAX];
    if (looks_like_query(trimmed)) {
        bcopy_str(url, SEARCH_URL, sizeof(url));
        int bl = b_len(url);
        search_url_encode(trimmed, url + bl, URL_MAX - bl);
    } else {
        bcopy_str(url, trimmed, sizeof(url));
    }
    navigate(url, 1);
}

/* ================= Yer imleri + Gecmis + Indirme Yoneticisi + Coklu Sekme =================
 * Yer imleri : C:/etc/yerimleri.cfg
 * Gecmis     : C:/etc/gecmis.cfg (tum sekmeler arasi kalici)
 * Indirmeler : C:/indirilenler + C:/etc/indirmeler.log */
#define BM_FILE    "C:/etc/yerimleri.cfg"
#define GHIST_FILE "C:/etc/gecmis.cfg"
#define CK_FILE    "C:/etc/cookies.cfg"
static int s_file_pick_node = -1;
static char s_file_pick_paths[32][160];
static int  s_file_pick_n;

/* Kalici Cerez Deposu (C:/etc/cookies.cfg) */
static void cookies_persist_load(void) {
    if (!g_basi || !g_basi->fs || !g_basi->fs->read_path || !g_basi->net) return;
    if (!g_basi->net->cookie_set2 && !g_basi->net->cookie_set) return;
    static char cbuf[16384];
    uint32_t n = g_basi->fs->read_path(CK_FILE, cbuf, sizeof(cbuf) - 1);
    if (!n) return;
    cbuf[n] = 0;
    uint32_t i = 0;
    while (i < n) {
        char host[128], nm[64], val[512]; int sec = 0;
        int hi = 0, ni = 0, vi = 0;
        while (i < n && cbuf[i] != '\t' && cbuf[i] != '\n') { if (hi < 127) host[hi++] = cbuf[i]; i++; }
        if (i >= n || cbuf[i] != '\t') { while (i < n && cbuf[i] != '\n') i++; if (i < n) i++; continue; }
        i++;
        while (i < n && cbuf[i] != '\t' && cbuf[i] != '\n') { if (ni < 63) nm[ni++] = cbuf[i]; i++; }
        if (i >= n || cbuf[i] != '\t') { while (i < n && cbuf[i] != '\n') i++; if (i < n) i++; continue; }
        i++;
        while (i < n && cbuf[i] != '\t' && cbuf[i] != '\n') { if (vi < 511) val[vi++] = cbuf[i]; i++; }
        if (i < n && cbuf[i] == '\t') { i++; if (i < n && cbuf[i] == '1') sec = 1; }
        while (i < n && cbuf[i] != '\n') i++;
        if (i < n) i++;
        host[hi] = 0; nm[ni] = 0; val[vi] = 0;
        if (host[0] && nm[0]) {
            if (g_basi->net->cookie_set2) g_basi->net->cookie_set2(host, nm, val, sec);
            else g_basi->net->cookie_set(host, nm, val);
        }
    }
}

static void cookies_persist_save(void) {
    if (!g_basi || !g_basi->net || !g_basi->net->cookie_export) return;
    if (!g_basi->fs || !g_basi->fs->create_in || !g_basi->fs->write) return;
    static char cbuf[16384];
    uint32_t n = g_basi->net->cookie_export(cbuf, sizeof(cbuf));
    if (n == 0) return;
    if (g_basi->fs->mkdir) g_basi->fs->mkdir("C:/etc");
    void *f = g_basi->fs->create_in("C:/etc", "cookies.cfg", 0);
    if (!f) return;
    g_basi->fs->write(f, cbuf, n);
}

/* Dahili PDF Goruntuleyici (browser_pdf.h) */
static void pdf_save_btn_cb(lv_event_t *e) {
    (void)e;
    if (g_nav_dl_buf && g_nav_dl_len > 0)
        save_non_html_download(g_cur_url[0] ? g_cur_url : g_nav_final, g_nav_dl_buf, g_nav_dl_len, ".pdf");
}

static void render_pdf_viewer(const char *url, const uint8_t *data, uint32_t len) {
    if (!g_content || !g_basi || !g_basi->mem || !g_basi->mem->alloc || !g_basi->mem->free) return;
    bcopy_str(g_cur_url, url ? url : "", URL_MAX);
    parse_url(g_cur_url, g_cur_scheme, sizeof(g_cur_scheme), g_cur_host, sizeof(g_cur_host));
    if (g_addr) lv_textarea_set_text(g_addr, g_cur_url);
    ghist_record(g_cur_url);

    lv_obj_clean(g_content);
    if (g_fixed_layer) lv_obj_clean(g_fixed_layer);
    images_clear();
    js_reset();
    g_dn_count = 0;

    lv_obj_set_style_bg_color(g_content, lv_color_hex(0x1E222B), 0);
    lv_obj_set_style_bg_opa(g_content, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_content, 18, 0);
    lv_obj_set_style_pad_row(g_content, 16, 0);
    lv_obj_set_flex_flow(g_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_scroll_to_y(g_content, 0, LV_ANIM_OFF);

    bpdf_doc_t doc;
    bpdf_parse(data, len, g_basi->mem->alloc, g_basi->mem->free, &doc);

    /* Ust PDF bilgi & kaydetme cubugu */
    lv_obj_t *bar = lv_obj_create(g_content);
    lv_obj_remove_style_all(bar);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bar, LV_PCT(100), 46);
    lv_obj_set_style_max_width(bar, 780, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x14171D), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 10, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(0x2C313B), 0);
    lv_obj_set_style_pad_hor(bar, 14, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    char hdr_t[200], nb[16];
    bcopy_str(hdr_t, "PDF ", sizeof(hdr_t));
    bcat(hdr_t, doc.version[0] ? doc.version : "1.4", sizeof(hdr_t));
    bcat(hdr_t, "  |  ", sizeof(hdr_t));
    bcat(hdr_t, doc.title[0] ? doc.title : "Belge", sizeof(hdr_t));
    bcat(hdr_t, "  (", sizeof(hdr_t));
    js_i64toa(doc.page_count, nb, sizeof(nb)); bcat(hdr_t, nb, sizeof(hdr_t));
    bcat(hdr_t, " sayfa)", sizeof(hdr_t));
    lv_obj_t *tl = lv_label_create(bar);
    lv_label_set_text(tl, hdr_t);
    lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(tl, 10); lv_obj_set_flex_grow(tl, 1);
    lv_obj_set_style_text_color(tl, lv_color_hex(0xE8EAEE), 0);
    { lv_font_t *f = bfont_ex(13, 1); if (f) lv_obj_set_style_text_font(tl, f, 0); }

    lv_obj_t *sb = lv_button_create(bar);
    lv_obj_remove_style_all(sb);
    lv_obj_set_size(sb, LV_SIZE_CONTENT, 28);
    lv_obj_set_style_pad_hor(sb, 12, 0);
    lv_obj_set_style_radius(sb, 6, 0);
    lv_obj_set_style_bg_color(sb, lv_color_hex(0x2563EB), 0);
    lv_obj_set_style_bg_opa(sb, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(sb, pdf_save_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(sb);
    lv_label_set_text(sl, "PDF Indir");
    lv_obj_set_style_text_color(sl, lv_color_hex(0xFFFFFF), 0);
    { lv_font_t *f = bfont(12); if (f) lv_obj_set_style_text_font(sl, f, 0); }
    lv_obj_center(sl);

    if (doc.page_count == 0) {
        lv_obj_t *pg = lv_obj_create(g_content);
        lv_obj_remove_style_all(pg);
        lv_obj_remove_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(pg, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(pg, 780, 0);
        lv_obj_set_style_bg_color(pg, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(pg, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(pg, 8, 0);
        lv_obj_set_style_pad_all(pg, 28, 0);
        lv_obj_t *pl = lv_label_create(pg);
        lv_label_set_text(pl, "Bu PDF dosyasinda metin akisi bulunamadi (taranmis gorsel PDF olabilir). Ustteki 'PDF Indir' ile kaydedebilirsiniz.");
        lv_label_set_long_mode(pl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(pl, LV_PCT(100));
        lv_obj_set_style_text_color(pl, lv_color_hex(0x334155), 0);
        { lv_font_t *f = bfont(14); if (f) lv_obj_set_style_text_font(pl, f, 0); }
    } else {
        for (int i = 0; i < doc.page_count; i++) {
            lv_obj_t *sheet = lv_obj_create(g_content);
            lv_obj_remove_style_all(sheet);
            lv_obj_remove_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_size(sheet, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_style_max_width(sheet, 780, 0);
            lv_obj_set_style_bg_color(sheet, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(sheet, 6, 0);
            lv_obj_set_style_shadow_width(sheet, 16, 0);
            lv_obj_set_style_shadow_opa(sheet, 70, 0);
            lv_obj_set_style_pad_all(sheet, 28, 0);
            lv_obj_set_style_pad_row(sheet, 10, 0);
            lv_obj_set_flex_flow(sheet, LV_FLEX_FLOW_COLUMN);

            char ptag[32]; bcopy_str(ptag, "Sayfa ", sizeof(ptag));
            js_i64toa(i + 1, nb, sizeof(nb)); bcat(ptag, nb, sizeof(ptag));
            lv_obj_t *badge = lv_label_create(sheet);
            lv_label_set_text(badge, ptag);
            lv_obj_set_style_text_color(badge, lv_color_hex(0x64748B), 0);
            { lv_font_t *f = bfont_ex(11, 1); if (f) lv_obj_set_style_text_font(badge, f, 0); }

            lv_obj_t *body = lv_label_create(sheet);
            lv_label_set_text(body, doc.pages[i].text ? doc.pages[i].text : "");
            lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(body, LV_PCT(100));
            lv_obj_set_style_text_color(body, lv_color_hex(0x0F172A), 0);
            { lv_font_t *f = bfont(14); if (f) lv_obj_set_style_text_font(body, f, 0); }
        }
    }

    set_tab_title(doc.title[0] ? doc.title : "PDF Belgesi");
    tab_sync_active_meta();
    set_status(hdr_t, 0x10B981);
    bpdf_free(&doc, g_basi->mem->free);
}
#define BM_MAX     24
#define GHIST_MAX  48
#define BM_URLLEN  200
#define TAB_MAX    8

typedef struct {
    char    url[URL_MAX];
    char    host[160];
    char    scheme[8];
    char    title[120];
    char    hist[HIST_MAX][URL_MAX];
    int     hist_count;
    char    fwd[HIST_MAX][URL_MAX];
    int     fwd_count;
    int32_t scroll_y;
    char   *html;
    uint32_t html_len;
    int     used;
} browser_tab_t;

static browser_tab_t *g_tabs;
static int            g_tab_count;
static int            g_tab_active;
static lv_obj_t      *g_tab_bar;

static lv_obj_t *s_panel;
static lv_obj_t *s_panel_title;
static lv_obj_t *s_panel_list;
static int       s_panel_mode;      /* 0 kapali, 1 yer imleri, 2 indirmeler, 3 gecmis, 4 araclar/konsol */
static char      s_bm[BM_MAX][BM_URLLEN];
static int       s_bm_n;
static char      s_ghist[GHIST_MAX][BM_URLLEN];
static int       s_ghist_n;
static int       s_ghist_loaded;

static void show_start_page(void);
static void tab_bar_rebuild(void);

static int bm_load(void) {
    s_bm_n = 0;
    if (!g_basi || !g_basi->fs || !g_basi->fs->read_path) return 0;
    static char buf[BM_MAX * (BM_URLLEN + 2)];
    uint32_t n = g_basi->fs->read_path(BM_FILE, buf, sizeof(buf) - 1);
    if (!n) return 0;
    buf[n] = 0;
    int k = 0;
    for (uint32_t i = 0; i <= n; i++) {
        char c = (i < n) ? buf[i] : '\n';
        if (c == '\n' || c == '\r') {
            if (k > 0 && s_bm_n < BM_MAX) {
                s_bm[s_bm_n][k] = 0;
                s_bm_n++;
            }
            k = 0;
            continue;
        }
        if (k < BM_URLLEN - 1) s_bm[s_bm_n < BM_MAX ? s_bm_n : BM_MAX - 1][k++] = c;
    }
    return s_bm_n;
}

static void bm_save(void) {
    if (!g_basi || !g_basi->fs || !g_basi->fs->create_in || !g_basi->fs->write) return;
    if (g_basi->fs->mkdir) g_basi->fs->mkdir("C:/etc");
    static char buf[BM_MAX * (BM_URLLEN + 2)];
    uint32_t n = 0, cap = sizeof(buf) - 1;
    for (int i = 0; i < s_bm_n; i++) {
        uint32_t l = (uint32_t)b_len(s_bm[i]);
        if (l == 0) continue;
        if (l + 1 > cap - n) break;
        for (uint32_t k = 0; k < l; k++) buf[n++] = s_bm[i][k];
        buf[n++] = '\n';
    }
    void *h = g_basi->fs->create_in("C:/etc", "yerimleri.cfg", 0);
    if (!h) return;
    g_basi->fs->write(h, buf, n);
}

static int bm_find(const char *url) {
    for (int i = 0; i < s_bm_n; i++)
        if (str_eq(s_bm[i], url)) return i;
    return -1;
}

static void ghist_load(void) {
    if (s_ghist_loaded) return;
    s_ghist_loaded = 1;
    s_ghist_n = 0;
    if (!g_basi || !g_basi->fs || !g_basi->fs->read_path) return;
    static char buf[GHIST_MAX * (BM_URLLEN + 2)];
    uint32_t n = g_basi->fs->read_path(GHIST_FILE, buf, sizeof(buf) - 1);
    if (!n) return;
    buf[n] = 0;
    int k = 0;
    for (uint32_t i = 0; i <= n; i++) {
        char c = (i < n) ? buf[i] : '\n';
        if (c == '\n' || c == '\r') {
            if (k > 0 && s_ghist_n < GHIST_MAX) {
                s_ghist[s_ghist_n][k] = 0;
                s_ghist_n++;
            }
            k = 0;
            continue;
        }
        if (k < BM_URLLEN - 1) s_ghist[s_ghist_n < GHIST_MAX ? s_ghist_n : GHIST_MAX - 1][k++] = c;
    }
}

static void ghist_save(void) {
    if (!g_basi || !g_basi->fs || !g_basi->fs->create_in || !g_basi->fs->write) return;
    if (g_basi->fs->mkdir) g_basi->fs->mkdir("C:/etc");
    static char buf[GHIST_MAX * (BM_URLLEN + 2)];
    uint32_t n = 0, cap = sizeof(buf) - 1;
    for (int i = 0; i < s_ghist_n; i++) {
        uint32_t l = (uint32_t)b_len(s_ghist[i]);
        if (l == 0) continue;
        if (l + 1 > cap - n) break;
        for (uint32_t k = 0; k < l; k++) buf[n++] = s_ghist[i][k];
        buf[n++] = '\n';
    }
    void *h = g_basi->fs->create_in("C:/etc", "gecmis.cfg", 0);
    if (!h) return;
    g_basi->fs->write(h, buf, n);
}

static void ghist_record(const char *url) {
    if (!url || !url[0]) return;
    ghist_load();
    int found = -1;
    for (int i = 0; i < s_ghist_n; i++)
        if (str_eq(s_ghist[i], url)) { found = i; break; }
    if (found >= 0) {
        for (int i = found; i < s_ghist_n - 1; i++)
            bcopy_str(s_ghist[i], s_ghist[i + 1], BM_URLLEN);
        bcopy_str(s_ghist[s_ghist_n - 1], url, BM_URLLEN);
    } else if (s_ghist_n < GHIST_MAX) {
        bcopy_str(s_ghist[s_ghist_n++], url, BM_URLLEN);
    } else {
        for (int i = 1; i < GHIST_MAX; i++)
            bcopy_str(s_ghist[i - 1], s_ghist[i], BM_URLLEN);
        bcopy_str(s_ghist[GHIST_MAX - 1], url, BM_URLLEN);
    }
    ghist_save();
}

static void ghist_clear(void) {
    s_ghist_n = 0;
    s_ghist_loaded = 1;
    g_hist_count = 0;
    g_fwd_count = 0;
    if (g_tabs) {
        for (int t = 0; t < g_tab_count; t++) {
            g_tabs[t].hist_count = 0;
            g_tabs[t].fwd_count = 0;
        }
    }
    ghist_save();
}

/* Indirme kaydi (ad + boyut + url) */
static void dl_log(const char *name, uint32_t size, const char *url) {
    if (!g_basi || !g_basi->fs || !g_basi->fs->create_in || !g_basi->fs->write) return;
    if (g_basi->fs->mkdir) g_basi->fs->mkdir("C:/etc");
    char line[512];
    int n = 0;
    const char *p = name ? name : "?";
    while (*p && n < 40) line[n++] = *p++;
    line[n++] = '\t';
    uint32_t v = size;
    char tmp[12]; int k = 0;
    if (!v) tmp[k++] = '0';
    while (v && k < 12) { tmp[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) line[n++] = tmp[--k];
    line[n++] = '\t';
    p = url ? url : "";
    while (*p && n < 500) line[n++] = *p++;
    if (n < 511) line[n++] = '\n';

    static char buf[4096];
    uint32_t cap = sizeof(buf) - 1, bn = 0;
    if (g_basi->fs->read_path)
        bn = g_basi->fs->read_path("C:/etc/indirmeler.log", buf, cap - 512);
    if (bn > cap - 512) bn = cap - 512;
    if (bn + (uint32_t)n > cap) bn = 0;
    for (int i = 0; i < n; i++) buf[bn++] = line[i];

    void *h = g_basi->fs->create_in("C:/etc", "indirmeler.log", 0);
    if (!h) return;
    g_basi->fs->write(h, buf, bn);
}

static void panel_close(void) {
    if (s_panel) lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    s_panel_mode = 0;
}
static void panel_close_cb(lv_event_t *e) { (void)e; panel_close(); }
static void panel_fill(void);

static void panel_item_cb(lv_event_t *e) {
    int code = (int)(intptr_t)lv_event_get_user_data(e);
    int mode = code / 1000;
    int idx = code % 1000;
    if (mode == 1) {
        if (idx >= 0 && idx < s_bm_n) {
            if (g_addr) lv_textarea_set_text(g_addr, s_bm[idx]);
            panel_close();
            navigate(s_bm[idx], 1);
            return;
        }
    } else if (mode == 4) {
        /* Yer imi sil */
        if (idx >= 0 && idx < s_bm_n) {
            for (int k = idx; k < s_bm_n - 1; k++) bcopy_str(s_bm[k], s_bm[k + 1], BM_URLLEN);
            s_bm_n--;
            bm_save();
            set_status("Yer imi kaldirildi", ADL_TEXT_DIM);
            panel_fill();
            return;
        }
    } else if (mode == 2) {
        char path[200];
        int n = 0;
        const char *pre = "C:/indirilenler/";
        while (pre[n]) { path[n] = pre[n]; n++; }
        char nm[120]; int isd = 0;
        if (g_basi && g_basi->fs && g_basi->fs->list_dir &&
            g_basi->fs->list_dir("C:/indirilenler", idx, nm, sizeof(nm), &isd) == 0) {
            for (int i = 0; nm[i] && n < (int)sizeof(path) - 1; i++) path[n++] = nm[i];
            path[n] = 0;
            arctian_shell_open_file(path);
        }
    } else if (mode == 3) {
        /* Kalici genel gecmis: en yeniden en eskiye */
        int k = s_ghist_n - 1 - idx;
        if (k >= 0 && k < s_ghist_n && s_ghist[k][0]) {
            char u[URL_MAX];
            bcopy_str(u, s_ghist[k], sizeof(u));
            if (g_addr) lv_textarea_set_text(g_addr, u);
            panel_close();
            navigate(u, 1);
            return;
        }
    } else if (mode == 5) {
        ghist_clear();
        set_status("Gecmis temizlendi", ADL_TEXT_DIM);
        panel_fill();
        return;
    } else if (mode == 6) {
        /* Yakinlastirma (Zoom) + DevTools Konsol kontrolleri */
        if (idx == 0) g_zoom_pct = 100;
        else if (idx == 1) { g_zoom_pct -= 15; if (g_zoom_pct < 70) g_zoom_pct = 70; }
        else if (idx == 2) { g_zoom_pct += 15; if (g_zoom_pct > 160) g_zoom_pct = 160; }
        else if (idx == 3) {
            if (g_js) { g_js->console_len = 0; g_js->console_buf[0] = 0; }
            set_status("Konsol temizlendi", ADL_TEXT_DIM);
            panel_fill();
            return;
        }
        if (g_dn_count > 0) restyle_page();
        char zm[48]; bcopy_str(zm, "Yakinlastirma: %", sizeof(zm));
        char nb[12]; js_i64toa(g_zoom_pct, nb, sizeof(nb)); bcat(zm, nb, sizeof(zm));
        set_status(zm, ADL_TEXT_DIM);
        panel_fill();
        return;
    } else if (mode == 7) {
        /* <input type="file"> dosya secildi */
        if (idx >= 0 && idx < s_file_pick_n && s_file_pick_node >= 0 && s_file_pick_node < g_dn_count) {
            bcopy_str(g_dn[s_file_pick_node].value, s_file_pick_paths[idx], sizeof(g_dn[0].value));
            if (g_dn[s_file_pick_node].widget && lv_obj_get_child_count(g_dn[s_file_pick_node].widget) >= 2) {
                lv_obj_t *lbl = lv_obj_get_child(g_dn[s_file_pick_node].widget, 1);
                if (lbl) lv_label_set_text(lbl, s_file_pick_paths[idx]);
            }
            set_status(s_file_pick_paths[idx], 0x10B981);
            if (g_js) js_dispatch_dom_event(s_file_pick_node, "change");
        }
        s_file_pick_node = -1;
    }
    panel_close();
}

static void panel_row_ex(const char *text, int code, int del_code) {
    lv_obj_t *r = lv_obj_create(s_panel_list);
    lv_obj_remove_style_all(r);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(r, LV_PCT(100), 34);
    lv_obj_set_style_radius(r, 8, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(ADL_SURFACE_2), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(ADL_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(r, 8, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r, panel_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)code);

    lv_obj_t *l = lv_label_create(r);
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, 10);
    lv_obj_set_flex_grow(l, 1);
    lv_obj_set_style_text_color(l, lv_color_hex(ADL_TEXT), 0);
    { lv_font_t *pf = bfont(13); if (pf) lv_obj_set_style_text_font(l, pf, 0); }

    if (del_code >= 0) {
        lv_obj_t *xb = lv_button_create(r);
        lv_obj_remove_style_all(xb);
        lv_obj_set_size(xb, 24, 24);
        lv_obj_set_style_radius(xb, 6, 0);
        lv_obj_set_style_bg_color(xb, lv_color_hex(0x3B2228), LV_STATE_HOVERED);
        lv_obj_set_style_bg_opa(xb, LV_OPA_COVER, LV_STATE_HOVERED);
        lv_obj_add_event_cb(xb, panel_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)del_code);
        lv_obj_t *xl = lv_label_create(xb);
        lv_label_set_text(xl, "x");
        lv_obj_set_style_text_color(xl, lv_color_hex(0xF87171), 0);
        lv_obj_center(xl);
    }
}

static void panel_row(const char *text, int code) { panel_row_ex(text, code, -1); }

static void panel_fill(void) {
    if (!s_panel_list) return;
    lv_obj_clean(s_panel_list);
    if (s_panel_title) {
        const char *ht = s_panel_mode == 1 ? "Yer Imleri"
                       : s_panel_mode == 2 ? "Indirmeler"
                       : s_panel_mode == 3 ? "Gecmis"
                       : s_panel_mode == 5 ? "Dosya Sec" : "Araclar & Konsol";
        lv_label_set_text(s_panel_title, ht);
    }
    if (s_panel_mode == 1) {
        bm_load();
        if (s_bm_n == 0) { panel_row("(yer imi yok)", 1999); return; }
        for (int i = 0; i < s_bm_n; i++) panel_row_ex(s_bm[i], 1000 + i, 4000 + i);
    } else if (s_panel_mode == 2) {
        if (!g_basi || !g_basi->fs || !g_basi->fs->list_dir) { panel_row("(dosya sistemi yok)", 2999); return; }
        int n = g_basi->fs->count_dir ? g_basi->fs->count_dir("C:/indirilenler") : 0;
        if (n <= 0) { panel_row("(indirme yok)", 2999); return; }
        int shown = 0;
        for (int i = 0; i < n && shown < 24; i++) {
            char nm[120]; int isd = 0;
            if (g_basi->fs->list_dir("C:/indirilenler", i, nm, sizeof(nm), &isd) != 0) continue;
            if (isd) continue;
            panel_row(nm, 2000 + i);
            shown++;
        }
        if (shown == 0) panel_row("(indirme yok)", 2999);
    } else if (s_panel_mode == 3) {
        ghist_load();
        if (s_ghist_n == 0) { panel_row("(gecmis bos)", 3999); return; }
        panel_row("Gecmisi Temizle", 5000);
        for (int i = s_ghist_n - 1, shown = 0; i >= 0 && shown < 32; i--, shown++) {
            if (s_ghist[i][0]) panel_row(s_ghist[i], 3000 + (s_ghist_n - 1 - i));
        }
    } else if (s_panel_mode == 4) {
        char zb[64], nb[16];
        bcopy_str(zb, "Yakinlastir (+)  [Su an: %", sizeof(zb));
        js_i64toa(g_zoom_pct, nb, sizeof(nb)); bcat(zb, nb, sizeof(zb)); bcat(zb, "]", sizeof(zb));
        panel_row(zb, 6002);
        panel_row("Uzaklastir (-)   |   Sifirla (%100)", 6001);
        panel_row("Yakinlastirmayi Sifirla (%100)", 6000);
        char stat_s[96];
        bcopy_str(stat_s, "DOM: ", sizeof(stat_s));
        js_i64toa(g_dn_count, nb, sizeof(nb)); bcat(stat_s, nb, sizeof(stat_s));
        bcat(stat_s, " | CSS: ", sizeof(stat_s));
        js_i64toa(g_css_count, nb, sizeof(nb)); bcat(stat_s, nb, sizeof(stat_s));
        bcat(stat_s, " | Canvas: ", sizeof(stat_s));
        js_i64toa(g_canvas_n, nb, sizeof(nb)); bcat(stat_s, nb, sizeof(stat_s));
        panel_row(stat_s, 6999);
        panel_row("JS Konsolunu Temizle", 6003);
        if (g_js && g_js->console_len > 0) {
            const char *p = g_js->console_buf;
            int shown = 0;
            while (*p && shown < 16) {
                char line[120]; int li = 0;
                while (*p && *p != '\n' && li < 118) line[li++] = *p++;
                if (*p == '\n') p++;
                line[li] = 0;
                if (li > 0) { panel_row(line, 6999); shown++; }
            }
        } else {
            panel_row("(JS konsol ciktisi yok)", 6999);
        }
    } else if (s_panel_mode == 5) {
        s_file_pick_n = 0;
        static const char *dirs[2] = { "C:/belgeler", "C:/indirilenler" };
        if (g_basi && g_basi->fs && g_basi->fs->list_dir && g_basi->fs->count_dir) {
            for (int di = 0; di < 2; di++) {
                int cnt = g_basi->fs->count_dir(dirs[di]);
                for (int i = 0; i < cnt && s_file_pick_n < 32; i++) {
                    char nm[120]; int isd = 0;
                    if (g_basi->fs->list_dir(dirs[di], i, nm, sizeof(nm), &isd) != 0 || isd) continue;
                    bcopy_str(s_file_pick_paths[s_file_pick_n], dirs[di], sizeof(s_file_pick_paths[0]));
                    bcat(s_file_pick_paths[s_file_pick_n], "/", sizeof(s_file_pick_paths[0]));
                    bcat(s_file_pick_paths[s_file_pick_n], nm, sizeof(s_file_pick_paths[0]));
                    panel_row(s_file_pick_paths[s_file_pick_n], 7000 + s_file_pick_n);
                    s_file_pick_n++;
                }
            }
        }
        if (s_file_pick_n == 0) panel_row("(C:/belgeler veya C:/indirilenler bos)", 7999);
    }
}

static void file_picker_open(int dom_idx) {
    s_file_pick_node = dom_idx;
    if (!s_panel) return;
    s_panel_mode = 5;
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_panel);
    panel_fill();
}

static void panel_toggle(int mode) {
    if (!s_panel) return;
    if (s_panel_mode == mode) { panel_close(); return; }
    s_panel_mode = mode;
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_panel);
    panel_fill();
}

static void bm_btn_cb(lv_event_t *e) { (void)e; panel_toggle(1); }
static void dl_btn_cb(lv_event_t *e) { (void)e; panel_toggle(2); }
static void hist_btn_cb(lv_event_t *e) { (void)e; panel_toggle(3); }
static void devtools_btn_cb(lv_event_t *e) { (void)e; panel_toggle(4); }

/* ---------------- Sayfada Bul (Find-in-Page) ---------------- */
static void find_clear_hl(void) {
    for (int i = 0; i < s_find_n; i++) {
        if (s_find_hits[i]) lv_obj_set_style_outline_width(s_find_hits[i], 0, 0);
    }
    s_find_n = 0; s_find_cur = -1;
}
static void find_walk(lv_obj_t *obj, const char *needle) {
    if (!obj || s_find_n >= 64) return;
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *t = lv_label_get_text(obj);
        if (t && t[0] && find_substr_ci(t, needle)) {
            s_find_hits[s_find_n++] = obj;
            lv_obj_set_style_outline_width(obj, 2, 0);
            lv_obj_set_style_outline_color(obj, lv_color_hex(0xF59E0B), 0);
        }
    }
    uint32_t cc = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < cc && s_find_n < 64; i++) find_walk(lv_obj_get_child(obj, (int32_t)i), needle);
}
static void find_focus_cur(void) {
    char buf[24];
    if (s_find_n <= 0 || s_find_cur < 0) {
        bcopy_str(buf, "0/0", sizeof(buf));
    } else {
        char n1[10], n2[10];
        js_i64toa(s_find_cur + 1, n1, sizeof(n1));
        js_i64toa(s_find_n, n2, sizeof(n2));
        bcopy_str(buf, n1, sizeof(buf)); bcat(buf, "/", sizeof(buf)); bcat(buf, n2, sizeof(buf));
        for (int i = 0; i < s_find_n; i++) {
            lv_obj_set_style_outline_color(s_find_hits[i],
                lv_color_hex(i == s_find_cur ? 0x38BDF8 : 0xF59E0B), 0);
        }
        lv_obj_scroll_to_view_recursive(s_find_hits[s_find_cur], LV_ANIM_OFF);
    }
    if (s_find_lbl) lv_label_set_text(s_find_lbl, buf);
}
static void find_run(const char *q) {
    find_clear_hl();
    if (q && q[0] && g_content) {
        find_walk(g_content, q);
        if (s_find_n > 0) s_find_cur = 0;
    }
    find_focus_cur();
}
static void find_changed_cb(lv_event_t *e) {
    (void)e;
    if (s_find_ta) find_run(lv_textarea_get_text(s_find_ta));
}
static void find_step_cb(lv_event_t *e) {
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_find_n <= 0) return;
    s_find_cur = (s_find_cur + dir + s_find_n) % s_find_n;
    find_focus_cur();
}
static void find_close_cb(lv_event_t *e) {
    (void)e;
    find_clear_hl();
    if (s_find_bar) lv_obj_add_flag(s_find_bar, LV_OBJ_FLAG_HIDDEN);
    if (g_group && g_addr) lv_group_focus_obj(g_addr);
}
static void find_toggle_cb(lv_event_t *e) {
    (void)e;
    if (!s_find_bar) return;
    if (!lv_obj_has_flag(s_find_bar, LV_OBJ_FLAG_HIDDEN)) { find_close_cb(0); return; }
    panel_close();
    lv_obj_remove_flag(s_find_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_find_bar);
    if (g_group && s_find_ta) lv_group_focus_obj(s_find_ta);
    if (s_find_ta) find_run(lv_textarea_get_text(s_find_ta));
}

static void resolve_active_url(char *out, int cap) {
    out[0] = 0;
    if (g_cur_url[0]) { bcopy_str(out, g_cur_url, cap); return; }
    const char *t = g_addr ? lv_textarea_get_text(g_addr) : "";
    if (!t || !t[0]) {
        if (g_basi && g_basi->net && g_basi->net->last_url) t = g_basi->net->last_url();
    }
    if (!t || !t[0]) return;
    if (!starts_with(t, "http://") && !starts_with(t, "https://")) {
        bcopy_str(out, "https://", cap);
        bcat(out, t, cap);
    } else {
        bcopy_str(out, t, cap);
    }
}

/* Gecerli adresi yer imlerine ekler/cikarir. */
static void bm_star_cb(lv_event_t *e) {
    (void)e;
    char u[URL_MAX];
    resolve_active_url(u, sizeof(u));
    if (!u[0]) { set_status("Once bir sayfa acin", ADL_WARN); return; }
    bm_load();
    int i = bm_find(u);
    if (i >= 0) {
        for (int k = i; k < s_bm_n - 1; k++) bcopy_str(s_bm[k], s_bm[k + 1], BM_URLLEN);
        s_bm_n--;
        bm_save();
        set_status("Yer imi kaldirildi", ADL_TEXT_DIM);
    } else {
        if (s_bm_n >= BM_MAX) { set_status("Yer imi listesi dolu", ADL_WARN); return; }
        bcopy_str(s_bm[s_bm_n], u, BM_URLLEN);
        s_bm_n++;
        bm_save();
        set_status("Yer imlerine eklendi", 0x10B981);
    }
    if (s_panel_mode == 1 && s_panel) panel_fill();
}

static void download_cb(lv_event_t *e) {
    (void)e;
    if (!g_basi || !g_basi->net || !g_basi->net->download) return;
    char u[URL_MAX];
    resolve_active_url(u, sizeof(u));
    if (!u[0]) { set_status("Indirilecek adres yok", ADL_WARN); return; }

    char name[80];
    build_download_filename(u, ".html", name, sizeof(name));

    if (!g_nav_dl_buf && g_basi->mem && g_basi->mem->alloc)
        g_nav_dl_buf = (uint8_t *)g_basi->mem->alloc(g_dl_cap);
    if (!g_nav_dl_buf) { set_status("Bellek yetersiz", ADL_DANGER); return; }

    set_status("Indiriliyor...", ADL_WARN);
    uint32_t len = 0;
    int rc = g_basi->net->download(u, g_nav_dl_buf, g_dl_cap, &len);
    if (rc != 0) {
        set_status("Indirme basarisiz (ag/TLS)", ADL_DANGER);
        arctian_notify("Indirme", "Basarisiz: ag veya TLS hatasi");
        return;
    }
    char ext[16]; ext[0] = 0;
    sniff_non_html(g_nav_dl_buf, len, u, ext);
    if (ext[0]) build_download_filename(u, ext, name, sizeof(name));
    asi_fs_t *fs = g_basi->fs;
    if (fs && fs->mkdir) fs->mkdir("C:/indirilenler");
    void *h = (fs && fs->create_in) ? fs->create_in("C:/indirilenler", name, 0) : 0;
    if (!h) { set_status("Dosya olusturulamadi", ADL_DANGER); return; }
    fs->write(h, g_nav_dl_buf, len);
    char msg[140];
    bcopy_str(msg, "Kaydedildi: C:/indirilenler/", sizeof(msg));
    bcat(msg, name, sizeof(msg));
    set_status(msg, 0x10B981);
    dl_log(name, len, u);
    arctian_notify("Indirme tamam", name);
    if (s_panel_mode == 2 && s_panel) panel_fill();
}

static void addr_ready_cb(lv_event_t *e) { go_cb(e); }

static void back_cb(lv_event_t *e) {
    (void)e;
    panel_close();
    if (g_hist_count <= 0) return;
    if (g_cur_url[0] && g_fwd_count < HIST_MAX)
        bcopy_str(g_fwd[g_fwd_count++], g_cur_url, URL_MAX);
    g_hist_count--;
    char url[URL_MAX];
    bcopy_str(url, g_history[g_hist_count], sizeof(url));
    if (g_addr) lv_textarea_set_text(g_addr, url);
    navigate(url, 0);
}

static void forward_cb(lv_event_t *e) {
    (void)e;
    panel_close();
    if (g_fwd_count <= 0) return;
    g_fwd_count--;
    char url[URL_MAX];
    bcopy_str(url, g_fwd[g_fwd_count], sizeof(url));
    if (g_cur_url[0] && g_hist_count < HIST_MAX)
        bcopy_str(g_history[g_hist_count++], g_cur_url, URL_MAX);
    if (g_addr) lv_textarea_set_text(g_addr, url);
    navigate(url, 0);
}

static void reload_cb(lv_event_t *e) {
    (void)e;
    panel_close();
    char u[URL_MAX];
    resolve_active_url(u, sizeof(u));
    if (u[0]) navigate(u, 0);
}

/* Karsilama ekrani (bos sekme hali) — onceki sayfanin fixed/beyaz stilini tamamen temizler. */
static void show_start_page(void) {
    if (g_fixed_layer) lv_obj_clean(g_fixed_layer);
    g_sticky_obj = 0;
    images_clear();
    js_reset();
    g_dn_count = 0;
    if (g_content) {
        lv_obj_set_flex_flow(g_content, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(g_content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_scroll_to_y(g_content, 0, LV_ANIM_OFF);
    }
    g_block_count = 0;
    push_block(BLK_H1, "Arctian Tarayici", 0);
    push_block(BLK_TEXT, "Adres yazip Enter'a basin (ornek: google.com, github.com, kerem.site)", 0);
    push_block(BLK_TEXT, "Ustteki '+' ile coklu sekme acabilir, 'x' ile kapatabilir, sekmeler arasi hizla gecebilirsiniz.", 0);
    push_block(BLK_TEXT, "Geri/Ileri/Yenile, Yer Imleri, Indirmeler ve Gecmis dugmeleriyle tam kontrol saglayabilirsiniz.", 0);
    render_blocks();
    if (g_secure_dot)
        lv_obj_set_style_text_color(g_secure_dot, lv_color_hex(0x6B717D), 0);
}

/* ---------------- Gercek Coklu Sekme Yonetimi ---------------- */
static void tab_free_html(browser_tab_t *t) {
    if (t && t->html && g_basi && g_basi->mem && g_basi->mem->free) {
        g_basi->mem->free(t->html);
    }
    if (t) { t->html = 0; t->html_len = 0; }
}

static void tab_save_active(void) {
    if (!g_tabs || g_tab_active < 0 || g_tab_active >= g_tab_count) return;
    browser_tab_t *t = &g_tabs[g_tab_active];
    bcopy_str(t->url, g_cur_url, sizeof(t->url));
    bcopy_str(t->host, g_cur_host, sizeof(t->host));
    bcopy_str(t->scheme, g_cur_scheme, sizeof(t->scheme));
    t->hist_count = g_hist_count;
    for (int i = 0; i < g_hist_count && i < HIST_MAX; i++)
        bcopy_str(t->hist[i], g_history[i], URL_MAX);
    t->fwd_count = g_fwd_count;
    for (int i = 0; i < g_fwd_count && i < HIST_MAX; i++)
        bcopy_str(t->fwd[i], g_fwd[i], URL_MAX);
    t->scroll_y = g_content ? lv_obj_get_scroll_y(g_content) : 0;
    tab_free_html(t);
    if (t->url[0] && g_raw && g_raw[0] && g_basi && g_basi->mem && g_basi->mem->alloc) {
        uint32_t hl = (uint32_t)b_len(g_raw);
        if (hl > 0 && hl < g_raw_cap) {
            char *cp = (char *)g_basi->mem->alloc((size_t)hl + 1);
            if (cp) {
                for (uint32_t i = 0; i <= hl; i++) cp[i] = g_raw[i];
                t->html = cp;
                t->html_len = hl;
            }
        }
    }
}

static void tab_sync_active_meta(void) {
    if (!g_tabs || g_tab_active < 0 || g_tab_active >= g_tab_count) return;
    browser_tab_t *t = &g_tabs[g_tab_active];
    bcopy_str(t->url, g_cur_url, sizeof(t->url));
    bcopy_str(t->host, g_cur_host, sizeof(t->host));
    bcopy_str(t->scheme, g_cur_scheme, sizeof(t->scheme));
    t->hist_count = g_hist_count;
    for (int i = 0; i < g_hist_count && i < HIST_MAX; i++)
        bcopy_str(t->hist[i], g_history[i], URL_MAX);
    t->fwd_count = g_fwd_count;
    for (int i = 0; i < g_fwd_count && i < HIST_MAX; i++)
        bcopy_str(t->fwd[i], g_fwd[i], URL_MAX);
}

static void tab_set_active_title(const char *title) {
    if (!g_tabs || g_tab_active < 0 || g_tab_active >= g_tab_count) return;
    bcopy_str(g_tabs[g_tab_active].title, (title && title[0]) ? title : "Yeni sekme",
              sizeof(g_tabs[g_tab_active].title));
    tab_bar_rebuild();
}

static void tab_restore_active(void) {
    if (!g_tabs || g_tab_active < 0 || g_tab_active >= g_tab_count) return;
    browser_tab_t *t = &g_tabs[g_tab_active];
    bcopy_str(g_cur_url, t->url, sizeof(g_cur_url));
    bcopy_str(g_cur_host, t->host, sizeof(g_cur_host));
    bcopy_str(g_cur_scheme, t->scheme[0] ? t->scheme : "https", sizeof(g_cur_scheme));
    g_hist_count = t->hist_count;
    for (int i = 0; i < g_hist_count && i < HIST_MAX; i++)
        bcopy_str(g_history[i], t->hist[i], URL_MAX);
    g_fwd_count = t->fwd_count;
    for (int i = 0; i < g_fwd_count && i < HIST_MAX; i++)
        bcopy_str(g_fwd[i], t->fwd[i], URL_MAX);
    if (g_addr) lv_textarea_set_text(g_addr, g_cur_url);
    if (g_secure_dot)
        lv_obj_set_style_text_color(g_secure_dot,
            lv_color_hex((g_cur_url[0] && str_eq(g_cur_scheme, "https")) ? 0x4ADE80 : 0x6B717D), 0);
    tab_bar_rebuild();

    if (t->url[0] && t->html && t->html_len > 0 && ensure_raw_buf()) {
        uint32_t cp = t->html_len < (g_raw_cap - 1u) ? t->html_len : (g_raw_cap - 1u);
        for (uint32_t i = 0; i < cp; i++) g_raw[i] = t->html[i];
        g_raw[cp] = 0;
        render_page(g_raw);
        if (t->scroll_y > 0 && g_content) lv_obj_scroll_to_y(g_content, t->scroll_y, LV_ANIM_OFF);
        if (g_basi && g_basi->net && g_basi->net->submit && !g_in_submit) {
            if (!pq_start_from_html(g_raw, g_cur_scheme, g_cur_host))
                set_status(g_cur_url, ADL_TEXT_DIM);
        } else {
            set_status(g_cur_url, ADL_TEXT_DIM);
        }
    } else if (t->url[0]) {
        navigate(t->url, 0);
    } else {
        show_start_page();
        set_status("Hazir", ADL_TEXT_DIM);
        if (g_group && g_addr) lv_group_focus_obj(g_addr);
    }
}

static void tab_switch(int idx) {
    if (!g_tabs || idx < 0 || idx >= g_tab_count || idx == g_tab_active) return;
    panel_close();
    pq_cancel();
    tab_save_active();
    g_tab_active = idx;
    tab_restore_active();
}

static void tab_close(int idx) {
    if (!g_tabs || idx < 0 || idx >= g_tab_count) return;
    panel_close();
    pq_cancel();
    if (g_tab_count <= 1) {
        tab_free_html(&g_tabs[0]);
        g_tabs[0].url[0] = 0; g_tabs[0].host[0] = 0;
        bcopy_str(g_tabs[0].scheme, "https", sizeof(g_tabs[0].scheme));
        bcopy_str(g_tabs[0].title, "Yeni sekme", sizeof(g_tabs[0].title));
        g_tabs[0].hist_count = 0; g_tabs[0].fwd_count = 0; g_tabs[0].scroll_y = 0;
        g_tab_active = 0;
        if (g_basi && g_basi->net && (!g_basi->net->link_type || g_basi->net->link_type() != ASI_LINK_NONE)) {
            bcopy_str(g_tabs[0].url, HOME_URL, sizeof(g_tabs[0].url));
        }
        tab_restore_active();
        return;
    }
    if (idx == g_tab_active) {
        tab_free_html(&g_tabs[idx]);
    } else {
        tab_save_active();
        tab_free_html(&g_tabs[idx]);
    }
    for (int i = idx; i < g_tab_count - 1; i++) g_tabs[i] = g_tabs[i + 1];
    g_tab_count--;
    if (g_tab_active >= g_tab_count) g_tab_active = g_tab_count - 1;
    else if (idx < g_tab_active) g_tab_active--;
    tab_restore_active();
}

static void tab_click_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    tab_switch(idx);
}

static void tab_close_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    lv_event_stop_bubbling(e);
    tab_close(idx);
}

static void newtab_cb(lv_event_t *e) {
    (void)e;
    panel_close();
    if (!g_tabs) return;
    if (g_tab_count >= TAB_MAX) {
        set_status("En fazla 8 sekme acilabilir", ADL_WARN);
        return;
    }
    pq_cancel();
    tab_save_active();
    int ni = g_tab_count++;
    browser_tab_t *nt = &g_tabs[ni];
    uint8_t *z = (uint8_t *)nt;
    for (unsigned k = 0; k < sizeof(*nt); k++) z[k] = 0;
    bcopy_str(nt->scheme, "https", sizeof(nt->scheme));
    bcopy_str(nt->title, "Yeni sekme", sizeof(nt->title));
    if (g_basi && g_basi->net && (!g_basi->net->link_type || g_basi->net->link_type() != ASI_LINK_NONE)) {
        bcopy_str(nt->url, HOME_URL, sizeof(nt->url));
    }
    nt->used = 1;
    g_tab_active = ni;
    tab_restore_active();
}

static void tab_bar_rebuild(void) {
    if (!g_tab_bar || !g_tabs) return;
    lv_obj_clean(g_tab_bar);
    g_tab_label = 0;
    int tw = g_tab_count <= 2 ? 220 : g_tab_count <= 4 ? 170 : 125;
    lv_font_t *tf = bfont(13);
    for (int i = 0; i < g_tab_count; i++) {
        int active = (i == g_tab_active);
        lv_obj_t *tb = lv_obj_create(g_tab_bar);
        lv_obj_remove_style_all(tb);
        lv_obj_set_size(tb, tw, 31);
        lv_obj_set_style_radius(tb, 8, 0);
        lv_obj_set_style_bg_color(tb, lv_color_hex(active ? 0x1F232B : 0x14171D), 0);
        lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
        if (active) {
            lv_obj_set_style_border_width(tb, 2, 0);
            lv_obj_set_style_border_side(tb, LV_BORDER_SIDE_TOP, 0);
            lv_obj_set_style_border_color(tb, lv_color_hex(0x3B82F6), 0);
        }
        lv_obj_remove_flag(tb, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_left(tb, 10, 0);
        lv_obj_set_style_pad_right(tb, 6, 0);
        lv_obj_set_style_pad_column(tb, 6, 0);
        lv_obj_set_flex_flow(tb, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(tb, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(tb, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tb, tab_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(tb);
        lv_label_set_text(lbl, g_tabs[i].title[0] ? g_tabs[i].title : "Yeni sekme");
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_size(lbl, 10, 18);
        lv_obj_set_flex_grow(lbl, 1);
        lv_obj_set_style_text_color(lbl, lv_color_hex(active ? 0xE8EAEE : 0x8B919C), 0);
        if (tf) lv_obj_set_style_text_font(lbl, tf, 0);
        if (active) g_tab_label = lbl;

        lv_obj_t *xb = lv_button_create(tb);
        lv_obj_remove_style_all(xb);
        lv_obj_set_size(xb, 20, 20);
        lv_obj_set_style_radius(xb, 5, 0);
        lv_obj_set_style_bg_color(xb, lv_color_hex(0x2F3542), LV_STATE_HOVERED);
        lv_obj_set_style_bg_opa(xb, LV_OPA_COVER, LV_STATE_HOVERED);
        lv_obj_add_event_cb(xb, tab_close_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *xl = lv_label_create(xb);
        lv_label_set_text(xl, "x");
        lv_obj_set_style_text_color(xl, lv_color_hex(0x8B919C), 0);
        if (tf) lv_obj_set_style_text_font(xl, tf, 0);
        lv_obj_center(xl);
    }

    lv_obj_t *tplus = lv_button_create(g_tab_bar);
    lv_obj_remove_style_all(tplus);
    lv_obj_set_size(tplus, 28, 28);
    lv_obj_set_style_radius(tplus, 6, 0);
    lv_obj_set_style_bg_color(tplus, lv_color_hex(0x1F232B), LV_STATE_HOVERED);
    lv_obj_set_style_bg_opa(tplus, LV_OPA_COVER, LV_STATE_HOVERED);
    lv_obj_add_event_cb(tplus, newtab_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pl = lv_label_create(tplus);
    lv_label_set_text(pl, "+");
    lv_obj_set_style_text_color(pl, lv_color_hex(0xC0C6D0), 0);
    if (tf) lv_obj_set_style_text_font(pl, tf, 0);
    lv_obj_center(pl);
}

static void browser_del_cb(lv_event_t *e) {
    (void)e;
    pq_cancel();
    lv_indev_t *kp = lv_port_get_keypad();
    if (kp) lv_indev_set_group(kp, NULL);
    if (g_wheel_timer) { lv_timer_delete(g_wheel_timer); g_wheel_timer = 0; }
    js_reset();
    if (g_nav_dl_buf && g_basi && g_basi->mem) { g_basi->mem->free(g_nav_dl_buf); g_nav_dl_buf = 0; }
    images_clear();
    res_clear();
    if (g_tabs) {
        for (int i = 0; i < g_tab_count; i++) tab_free_html(&g_tabs[i]);
        if (g_basi && g_basi->mem && g_basi->mem->free) g_basi->mem->free(g_tabs);
        g_tabs = 0; g_tab_count = 0; g_tab_active = 0;
    }
    if (g_basi && g_basi->mem && g_basi->mem->free) {
        if (g_pq) { g_basi->mem->free(g_pq); g_pq = 0; }
        if (g_res) { g_basi->mem->free(g_res); g_res = 0; g_res_cap = 0; }
        if (g_css) { g_basi->mem->free(g_css); g_css = 0; g_css_cap = 0; g_css_count = 0; }
        if (g_css_pool) { g_basi->mem->free(g_css_pool); g_css_pool = 0; g_css_pool_cap = 0; g_css_pool_len = 0; }
        if (g_vstk) { g_basi->mem->free(g_vstk); g_vstk = 0; g_vstk_n = 0; g_vstk_cap = 0; g_vcount = 0; }
        if (g_dn) { g_basi->mem->free(g_dn); g_dn = 0; g_dn_cap = 0; g_dn_count = 0; }
        if (g_raw) { g_basi->mem->free(g_raw); g_raw = 0; }
    }
    for (int i = 0; i < g_chrome_n; i++)
        if (g_chrome_buf[i] && g_basi && g_basi->mem) g_basi->mem->free(g_chrome_buf[i]);
    g_chrome_n = 0;
    g_addr = 0; g_status = 0; g_content = 0; g_fixed_layer = 0; g_sticky_obj = 0;
    g_tab_bar = 0; g_tab_label = 0; g_secure_dot = 0;
    s_panel = 0; s_panel_title = 0; s_panel_list = 0; s_panel_mode = 0;
    s_find_bar = 0; s_find_ta = 0; s_find_lbl = 0; s_find_n = 0; s_find_cur = -1;
    s_ghist_loaded = 0;
    g_fwd_count = 0;
}

/* ---------------- Giris noktasi ---------------- */
void app_entry(lv_obj_t *win, asi_t *asi) {
    g_basi = asi;
    browser_init_mem_limits();
    g_block_count = 0;
    g_form_count = 0;
    g_hist_count = 0;
    g_fwd_count = 0;
    g_cur_url[0] = 0;
    g_cur_host[0] = 0;
    s_ghist_loaded = 0;
    bcopy_str(g_cur_scheme, "https", sizeof(g_cur_scheme));

    if (!g_tabs && g_basi && g_basi->mem && g_basi->mem->alloc) {
        g_tabs = (browser_tab_t *)g_basi->mem->alloc((size_t)TAB_MAX * sizeof(browser_tab_t));
        if (g_tabs) {
            uint8_t *z = (uint8_t *)g_tabs;
            for (unsigned k = 0; k < (unsigned)(TAB_MAX * sizeof(browser_tab_t)); k++) z[k] = 0;
        }
    }
    g_tab_count = 1;
    g_tab_active = 0;
    if (g_tabs) {
        bcopy_str(g_tabs[0].scheme, "https", sizeof(g_tabs[0].scheme));
        bcopy_str(g_tabs[0].title, "Yeni sekme", sizeof(g_tabs[0].title));
        g_tabs[0].used = 1;
    }

    lv_obj_set_flex_flow(win, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(win, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(win, 0, 0);
    lv_obj_set_style_pad_row(win, 0, 0);

    /* ---- Gercek Coklu Sekme Seridi (36 px, max 8 sekme, kapat + yeni sekme) ---- */
    g_tab_bar = lv_obj_create(win);
    lv_obj_remove_style_all(g_tab_bar);
    lv_obj_set_size(g_tab_bar, LV_PCT(100), 36);
    lv_obj_set_style_border_width(g_tab_bar, 1, 0);
    lv_obj_set_style_border_side(g_tab_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(g_tab_bar, lv_color_hex(0x2C313B), 0);
    lv_obj_remove_flag(g_tab_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_left(g_tab_bar, 10, 0);
    lv_obj_set_style_pad_right(g_tab_bar, 10, 0);
    lv_obj_set_style_pad_column(g_tab_bar, 4, 0);
    lv_obj_set_flex_flow(g_tab_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_tab_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    tab_bar_rebuild();

    /* ---- Adres satiri 52 px: geri / ileri / yenile + pill adres + dugmeler ---- */
    lv_obj_t *topbar = lv_obj_create(win);
    lv_obj_remove_style_all(topbar);
    lv_obj_set_size(topbar, LV_PCT(100), 52);
    lv_obj_set_style_border_width(topbar, 1, 0);
    lv_obj_set_style_border_side(topbar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(topbar, lv_color_hex(0x2A2E37), 0);
    lv_obj_remove_flag(topbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_hor(topbar, 14, 0);
    lv_obj_set_style_pad_column(topbar, 8, 0);
    lv_obj_set_flex_flow(topbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(topbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    chrome_btn(topbar, SVG_BACK, back_cb, 1);
    chrome_btn(topbar, SVG_FWD, forward_cb, 1);
    chrome_btn(topbar, SVG_RELOAD, reload_cb, 1);

    /* Adres pill'i: 32 px yukseklik, 16 px yariicap, #0f1115 zemin. */
    lv_obj_t *pill = lv_obj_create(topbar);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, 100, 32);
    lv_obj_set_flex_grow(pill, 1);
    lv_obj_set_style_radius(pill, 16, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(0x0F1115), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pill, 1, 0);
    lv_obj_set_style_border_color(pill, lv_color_hex(0x2C313B), 0);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_hor(pill, 14, 0);
    lv_obj_set_style_pad_column(pill, 6, 0);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_secure_dot = lv_label_create(pill);
    lv_label_set_text(g_secure_dot, "\xe2\x97\x8f");
    lv_obj_set_style_text_color(g_secure_dot, lv_color_hex(0x6B717D), 0);

    g_addr = lv_textarea_create(pill);
    lv_textarea_set_one_line(g_addr, 1);
    lv_textarea_set_placeholder_text(g_addr, "DuckDuckGo Lite'da ara veya URL yaz...");
    lv_obj_set_size(g_addr, 100, 30);
    lv_obj_set_flex_grow(g_addr, 1);
    lv_obj_set_style_bg_opa(g_addr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_addr, 0, 0);
    lv_obj_set_style_shadow_width(g_addr, 0, 0);
    lv_obj_set_style_pad_all(g_addr, 0, 0);
    lv_obj_set_style_pad_top(g_addr, 5, 0);
    {
        lv_font_t *af = bfont(13);
        if (af) {
            lv_obj_set_style_text_font(g_addr, af, 0);
            lv_obj_set_style_text_font(g_addr, af, LV_PART_TEXTAREA_PLACEHOLDER);
        }
    }
    lv_obj_set_style_text_color(g_addr, lv_color_hex(0xE8EAEE), 0);
    lv_obj_set_style_text_color(g_addr, lv_color_hex(0x6B717D), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_event_cb(g_addr, addr_ready_cb, LV_EVENT_READY, NULL);

    /* Islevsel dugmeler: git / indir / yildiz / indirmeler / yer imleri / gecmis / bul / araclar */
    chrome_btn(topbar, SVG_GO, go_cb, 0);
    chrome_btn(topbar, SVG_DL, download_cb, 0);
    chrome_btn(topbar, SVG_STAR, bm_star_cb, 0);
    chrome_btn(topbar, SVG_DLM, dl_btn_cb, 0);
    chrome_btn(topbar, SVG_BM, bm_btn_cb, 0);
    chrome_btn(topbar, SVG_HIST, hist_btn_cb, 0);
    chrome_btn(topbar, SVG_FIND, find_toggle_cb, 0);
    chrome_btn(topbar, SVG_DEVTOOLS, devtools_btn_cb, 0);

    /* Icerik alani */
    lv_obj_t *viewport = lv_obj_create(win);
    lv_obj_remove_style_all(viewport);
    lv_obj_remove_flag(viewport, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(viewport, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(viewport, 1);

    g_content = lv_obj_create(viewport);
    lv_obj_set_size(g_content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_content, lv_color_hex(ADL_BG), 0);
    lv_obj_set_style_bg_opa(g_content, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_content, 0, 0);
    lv_obj_set_style_pad_top(g_content, 28, 0);
    lv_obj_set_style_pad_bottom(g_content, 16, 0);
    lv_obj_set_style_pad_hor(g_content, 40, 0);
    lv_obj_set_style_pad_row(g_content, 6, 0);
    lv_obj_set_flex_flow(g_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(g_content, LV_DIR_VER);
    lv_obj_remove_flag(g_content, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_event_cb(g_content, content_scroll_clamp_cb, LV_EVENT_SCROLL, NULL);

    /* position: fixed katmani */
    g_fixed_layer = lv_obj_create(viewport);
    lv_obj_remove_style_all(g_fixed_layer);
    lv_obj_remove_flag(g_fixed_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(g_fixed_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(g_fixed_layer, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(g_fixed_layer, 0, 0);

    /* Alt durum cubugu 28 px */
    lv_obj_t *sbar = lv_obj_create(win);
    lv_obj_remove_style_all(sbar);
    lv_obj_set_size(sbar, LV_PCT(100), 28);
    lv_obj_set_style_border_width(sbar, 1, 0);
    lv_obj_set_style_border_side(sbar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(sbar, lv_color_hex(0x2A2E37), 0);
    lv_obj_remove_flag(sbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_hor(sbar, 16, 0);
    g_status = lv_label_create(sbar);
    lv_label_set_long_mode(g_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g_status, LV_PCT(100));
    lv_label_set_text(g_status, "Hazir");
    lv_obj_set_style_text_color(g_status, lv_color_hex(0x8B919C), 0);
    {
        lv_font_t *sf = bfont(12);
        if (sf) lv_obj_set_style_text_font(g_status, sf, 0);
    }
    lv_obj_align(g_status, LV_ALIGN_LEFT_MID, 0, 0);

    /* Yuzen sag ust panel (yer imleri / indirmeler / gecmis) — IGNORE_LAYOUT + FLOATING! */
    s_panel = lv_obj_create(win);
    lv_obj_remove_style_all(s_panel);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(s_panel, 360, 320);
    lv_obj_align(s_panel, LV_ALIGN_TOP_RIGHT, -8, 92);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(ADL_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_panel, 12, 0);
    lv_obj_set_style_border_width(s_panel, 1, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(ADL_BORDER), 0);
    lv_obj_set_style_shadow_width(s_panel, 20, 0);
    lv_obj_set_style_shadow_color(s_panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(s_panel, 120, 0);
    lv_obj_set_style_pad_all(s_panel, 10, 0);
    lv_obj_set_style_pad_row(s_panel, 8, 0);
    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *phdr = lv_obj_create(s_panel);
    lv_obj_remove_style_all(phdr);
    lv_obj_remove_flag(phdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(phdr, LV_PCT(100), 26);
    lv_obj_set_flex_flow(phdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(phdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_panel_title = lv_label_create(phdr);
    lv_label_set_text(s_panel_title, "Panel");
    lv_obj_set_style_text_color(s_panel_title, lv_color_hex(0xE8EAEE), 0);
    { lv_font_t *pf = bfont_ex(14, 1); if (pf) lv_obj_set_style_text_font(s_panel_title, pf, 0); }
    lv_obj_t *pxb = lv_button_create(phdr);
    lv_obj_remove_style_all(pxb);
    lv_obj_set_size(pxb, 24, 24);
    lv_obj_set_style_radius(pxb, 6, 0);
    lv_obj_set_style_bg_color(pxb, lv_color_hex(0x2A2E37), LV_STATE_HOVERED);
    lv_obj_set_style_bg_opa(pxb, LV_OPA_COVER, LV_STATE_HOVERED);
    lv_obj_add_event_cb(pxb, panel_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pxl = lv_label_create(pxb);
    lv_label_set_text(pxl, "x");
    lv_obj_set_style_text_color(pxl, lv_color_hex(0xA3A8B3), 0);
    lv_obj_center(pxl);

    s_panel_list = lv_obj_create(s_panel);
    lv_obj_remove_style_all(s_panel_list);
    lv_obj_set_size(s_panel_list, LV_PCT(100), 10);
    lv_obj_set_flex_grow(s_panel_list, 1);
    lv_obj_set_flex_flow(s_panel_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_panel_list, 6, 0);
    lv_obj_set_scroll_dir(s_panel_list, LV_DIR_VER);
    lv_obj_remove_flag(s_panel_list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scrollbar_mode(s_panel_list, LV_SCROLLBAR_MODE_AUTO);
    s_panel_mode = 0;

    /* Yuzen Sayfada Bul cubugu (IGNORE_LAYOUT | FLOATING) */
    s_find_bar = lv_obj_create(win);
    lv_obj_remove_style_all(s_find_bar);
    lv_obj_remove_flag(s_find_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_find_bar, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(s_find_bar, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(s_find_bar, 360, 42);
    lv_obj_align(s_find_bar, LV_ALIGN_TOP_RIGHT, -8, 92);
    lv_obj_set_style_bg_color(s_find_bar, lv_color_hex(ADL_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_find_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_find_bar, 10, 0);
    lv_obj_set_style_border_width(s_find_bar, 1, 0);
    lv_obj_set_style_border_color(s_find_bar, lv_color_hex(ADL_BORDER), 0);
    lv_obj_set_style_shadow_width(s_find_bar, 16, 0);
    lv_obj_set_style_shadow_opa(s_find_bar, 110, 0);
    lv_obj_set_style_pad_hor(s_find_bar, 8, 0);
    lv_obj_set_style_pad_column(s_find_bar, 6, 0);
    lv_obj_set_flex_flow(s_find_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_find_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(s_find_bar, LV_OBJ_FLAG_HIDDEN);

    s_find_ta = lv_textarea_create(s_find_bar);
    lv_textarea_set_one_line(s_find_ta, 1);
    lv_textarea_set_placeholder_text(s_find_ta, "Sayfada bul...");
    lv_obj_set_size(s_find_ta, 100, 28);
    lv_obj_set_flex_grow(s_find_ta, 1);
    lv_obj_set_style_bg_color(s_find_ta, lv_color_hex(0x0F1115), 0);
    lv_obj_set_style_bg_opa(s_find_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_find_ta, 6, 0);
    lv_obj_set_style_border_width(s_find_ta, 1, 0);
    lv_obj_set_style_border_color(s_find_ta, lv_color_hex(0x2C313B), 0);
    lv_obj_set_style_pad_hor(s_find_ta, 8, 0);
    lv_obj_set_style_pad_top(s_find_ta, 4, 0);
    lv_obj_set_style_text_color(s_find_ta, lv_color_hex(0xE8EAEE), 0);
    { lv_font_t *ff = bfont(13); if (ff) lv_obj_set_style_text_font(s_find_ta, ff, 0); }
    lv_obj_add_event_cb(s_find_ta, find_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_find_ta, find_step_cb, LV_EVENT_READY, (void *)(intptr_t)1);

    s_find_lbl = lv_label_create(s_find_bar);
    lv_label_set_text(s_find_lbl, "0/0");
    lv_obj_set_style_text_color(s_find_lbl, lv_color_hex(0x9CA3AF), 0);
    { lv_font_t *ff = bfont(12); if (ff) lv_obj_set_style_text_font(s_find_lbl, ff, 0); }

    static const char *fbtns[3] = { "<", ">", "x" };
    for (int bi = 0; bi < 3; bi++) {
        lv_obj_t *fb = lv_button_create(s_find_bar);
        lv_obj_remove_style_all(fb);
        lv_obj_set_size(fb, 24, 24);
        lv_obj_set_style_radius(fb, 6, 0);
        lv_obj_set_style_bg_color(fb, lv_color_hex(0x2A2E37), LV_STATE_HOVERED);
        lv_obj_set_style_bg_opa(fb, LV_OPA_COVER, LV_STATE_HOVERED);
        if (bi < 2) lv_obj_add_event_cb(fb, find_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(bi == 0 ? -1 : 1));
        else lv_obj_add_event_cb(fb, find_close_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *fl = lv_label_create(fb);
        lv_label_set_text(fl, fbtns[bi]);
        lv_obj_set_style_text_color(fl, lv_color_hex(0xD1D5DB), 0);
        lv_obj_center(fl);
    }

    lv_obj_add_event_cb(win, browser_del_cb, LV_EVENT_DELETE, NULL);

    if (!g_group) g_group = lv_group_create();
    lv_group_remove_all_objs(g_group);
    lv_group_add_obj(g_group, g_addr);
    if (s_find_ta) lv_group_add_obj(g_group, s_find_ta);
    lv_indev_t *kp = lv_port_get_keypad();
    if (kp) lv_indev_set_group(kp, g_group);
    if (g_wheel_timer) { lv_timer_delete(g_wheel_timer); g_wheel_timer = 0; }
    g_wheel_timer = lv_timer_create(page_wheel_cb, 30, NULL);

    /* Varsayilan acilis sayfasi: kayitli cerezleri yukle, ag bagliysa DuckDuckGo Lite ac */
    cookies_persist_load();
    if (g_basi && g_basi->net && (!g_basi->net->link_type || g_basi->net->link_type() != ASI_LINK_NONE)) {
        navigate(HOME_URL, 0);
    } else {
        show_start_page();
    }
}
