/*
 * Arctian Web Engine — Freestanding Built-in PDF Viewer Engine
 * Zero 3rd-party dependencies, zero libc, zero libm.
 * Parses %PDF- headers, /Title & /Author metadata, /FlateDecode streams
 * (self-contained RFC 1950/1951 inflate), and BT..ET text operators
 * (Tj, TJ, ', ", Td, TD, T*, Tm, Tf) into structured pages for LVGL rendering.
 */
#ifndef ARCTIAN_BROWSER_PDF_H
#define ARCTIAN_BROWSER_PDF_H

#include <stdint.h>
#include <stddef.h>

#define BPDF_MAX_PAGES 32

typedef struct {
    char  *text;
    uint32_t len;
} bpdf_page_t;

typedef struct {
    char title[128];
    char author[96];
    char version[16];
    bpdf_page_t pages[BPDF_MAX_PAGES];
    int page_count;
} bpdf_doc_t;

/* Self-contained RFC 1951/1950 inflate for PDF /FlateDecode streams */
typedef struct { uint16_t cnt[16]; uint16_t sym[288]; } bpdf_ht_t;
typedef struct {
    const uint8_t *s; uint32_t sl, p, bb; int bc;
    uint8_t *d; uint32_t dl, dc, maxo;
    void *(*alloc_fn)(size_t); void (*free_fn)(void *);
} bpdf_inf_t;

static inline uint32_t bpdf_bits(bpdf_inf_t *c, int n) {
    while (c->bc < n) {
        if (c->p >= c->sl) { c->bc += 8; continue; }
        c->bb |= ((uint32_t)c->s[c->p++]) << c->bc;
        c->bc += 8;
    }
    uint32_t v = c->bb & ((1u << n) - 1u);
    c->bb >>= n; c->bc -= n;
    return v;
}
static void bpdf_ht_build(bpdf_ht_t *t, const uint8_t *l, int n) {
    uint16_t o[16];
    for (int i = 0; i < 16; i++) t->cnt[i] = 0;
    for (int i = 0; i < n; i++) if (l[i] <= 15) t->cnt[l[i]]++;
    t->cnt[0] = 0;
    uint16_t sum = 0;
    for (int i = 1; i < 16; i++) { o[i] = sum; sum = (uint16_t)(sum + t->cnt[i]); }
    for (int i = 0; i < n; i++) if (l[i] && l[i] <= 15) t->sym[o[l[i]]++] = (uint16_t)i;
}
static int bpdf_ht_dec(bpdf_inf_t *c, const bpdf_ht_t *t) {
    int code = 0, first = 0, idx = 0;
    for (int len = 1; len <= 15; len++) {
        code |= (int)bpdf_bits(c, 1);
        int cnt = t->cnt[len];
        if (code - cnt < first) return t->sym[idx + (code - first)];
        idx += cnt; first = (first + cnt) << 1; code <<= 1;
    }
    return -1;
}
static int bpdf_inf_grow(bpdf_inf_t *c, uint32_t need) {
    if (c->dl + need + 1u <= c->dc) return 0;
    uint32_t nc = c->dc ? c->dc * 2u : 8192u;
    while (nc < c->dl + need + 1u) nc *= 2u;
    if (nc > c->maxo + 1u) nc = c->maxo + 1u;
    if (c->dl + need + 1u > nc) return -1;
    uint8_t *nb = (uint8_t *)c->alloc_fn(nc);
    if (!nb) return -1;
    for (uint32_t i = 0; i < c->dl; i++) nb[i] = c->d[i];
    if (c->d) c->free_fn(c->d);
    c->d = nb; c->dc = nc;
    return 0;
}
static int bpdf_inf_blk(bpdf_inf_t *c, const bpdf_ht_t *lt, const bpdf_ht_t *dt) {
    static const uint16_t lb[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const uint8_t  le[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const uint16_t db[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const uint8_t  de[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    for (;;) {
        int sym = bpdf_ht_dec(c, lt);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (bpdf_inf_grow(c, 1) != 0) return -2;
            c->d[c->dl++] = (uint8_t)sym;
        } else if (sym == 256) return 0;
        else {
            int li = sym - 257; if (li < 0 || li >= 29) return -3;
            uint32_t ln = lb[li] + bpdf_bits(c, le[li]);
            int ds = bpdf_ht_dec(c, dt); if (ds < 0 || ds >= 30) return -4;
            uint32_t dist = db[ds] + bpdf_bits(c, de[ds]);
            if (dist == 0 || dist > c->dl) return -5;
            if (bpdf_inf_grow(c, ln) != 0) return -2;
            uint32_t so = c->dl - dist;
            for (uint32_t k = 0; k < ln; k++) c->d[c->dl++] = c->d[so + k];
        }
    }
}
static int bpdf_inflate_zlib(const uint8_t *src, uint32_t slen,
                             void *(*alloc_fn)(size_t), void (*free_fn)(void *),
                             uint8_t **out_b, uint32_t *out_l, uint32_t maxo) {
    if (!src || slen < 2) return -1;
    uint32_t off = 0;
    if ((src[0] & 0x0F) == 8 && (src[0] >> 4) <= 7 && ((((uint32_t)src[0] << 8) | src[1]) % 31u) == 0)
        off = (src[1] & 0x20) ? 6u : 2u;
    if (off >= slen) return -1;
    bpdf_inf_t c;
    c.s = src + off; c.sl = slen - off; c.p = 0; c.bb = 0; c.bc = 0;
    c.d = 0; c.dl = 0; c.dc = 0; c.maxo = maxo;
    c.alloc_fn = alloc_fn; c.free_fn = free_fn;
    bpdf_ht_t *tr = (bpdf_ht_t *)alloc_fn(sizeof(bpdf_ht_t) * 2u);
    if (!tr) return -2;
    int err = 0, bf = 0;
    while (!bf && !err) {
        bf = (int)bpdf_bits(&c, 1);
        int bt = (int)bpdf_bits(&c, 2);
        if (bt == 0) {
            c.bb = 0; c.bc = 0;
            if (c.p + 4 > c.sl) { err = -3; break; }
            uint16_t bl = (uint16_t)(c.s[c.p] | ((uint16_t)c.s[c.p + 1] << 8));
            c.p += 4;
            if (c.p + bl > c.sl || bpdf_inf_grow(&c, bl) != 0) { err = -4; break; }
            for (uint32_t k = 0; k < bl; k++) c.d[c.dl++] = c.s[c.p++];
        } else if (bt == 1) {
            uint8_t ls[288]; int i = 0;
            for (; i < 144; i++) ls[i] = 8;
            for (; i < 256; i++) ls[i] = 9;
            for (; i < 280; i++) ls[i] = 7;
            for (; i < 288; i++) ls[i] = 8;
            bpdf_ht_build(&tr[0], ls, 288);
            for (i = 0; i < 32; i++) ls[i] = 5;
            bpdf_ht_build(&tr[1], ls, 32);
            err = bpdf_inf_blk(&c, &tr[0], &tr[1]);
        } else if (bt == 2) {
            static const uint8_t co[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            int hl = (int)bpdf_bits(&c, 5) + 257, hd = (int)bpdf_bits(&c, 5) + 1, hc = (int)bpdf_bits(&c, 4) + 4;
            if (hl > 286 || hd > 30) { err = -5; break; }
            uint8_t cl[19]; for (int i = 0; i < 19; i++) cl[i] = 0;
            for (int i = 0; i < hc; i++) cl[co[i]] = (uint8_t)bpdf_bits(&c, 3);
            bpdf_ht_build(&tr[0], cl, 19);
            uint8_t ls[320]; int tot = hl + hd, idx = 0;
            while (idx < tot) {
                int s = bpdf_ht_dec(&c, &tr[0]);
                if (s < 0) { err = -6; break; }
                if (s < 16) ls[idx++] = (uint8_t)s;
                else {
                    uint8_t v = (s == 16 && idx > 0) ? ls[idx - 1] : 0;
                    int r = (s == 16) ? (3 + (int)bpdf_bits(&c, 2))
                          : (s == 17) ? (3 + (int)bpdf_bits(&c, 3)) : (11 + (int)bpdf_bits(&c, 7));
                    if (idx + r > tot) { err = -7; break; }
                    while (r--) ls[idx++] = v;
                }
            }
            if (!err) {
                bpdf_ht_build(&tr[0], ls, hl);
                bpdf_ht_build(&tr[1], ls + hl, hd);
                err = bpdf_inf_blk(&c, &tr[0], &tr[1]);
            }
        } else err = -8;
    }
    free_fn(tr);
    if (err || !c.d) { if (c.d) free_fn(c.d); return err ? err : -9; }
    c.d[c.dl] = 0; *out_b = c.d; *out_l = c.dl;
    return 0;
}

static inline int bpdf_memeq(const uint8_t *a, const char *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) if (a[i] != (uint8_t)b[i]) return 0;
    return 1;
}
static int bpdf_find_seq(const uint8_t *d, uint32_t len, uint32_t from, const char *pat) {
    uint32_t pl = 0; while (pat[pl]) pl++;
    if (pl == 0 || from + pl > len) return -1;
    for (uint32_t i = from; i + pl <= len; i++)
        if (bpdf_memeq(d + i, pat, pl)) return (int)i;
    return -1;
}
static int bpdf_hexval(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void bpdf_append_ch(char *out, uint32_t *ol, uint32_t cap, char ch) {
    if (*ol + 1 >= cap) return;
    if (ch == '\r') return;
    if ((unsigned char)ch >= 32 || ch == '\n' || ch == '\t') out[(*ol)++] = ch;
}

/* Parse a PDF literal string (...) starting at pos '(' ; returns index after ')' */
static uint32_t bpdf_read_lit(const uint8_t *s, uint32_t len, uint32_t p,
                              char *out, uint32_t *ol, uint32_t cap) {
    if (p >= len || s[p] != '(') return p;
    p++;
    int depth = 1;
    while (p < len && depth > 0) {
        uint8_t c = s[p++];
        if (c == '\\' && p < len) {
            uint8_t e = s[p++];
            if (e == 'n') bpdf_append_ch(out, ol, cap, '\n');
            else if (e == 'r') bpdf_append_ch(out, ol, cap, '\n');
            else if (e == 't') bpdf_append_ch(out, ol, cap, ' ');
            else if (e == '(' || e == ')' || e == '\\') bpdf_append_ch(out, ol, cap, (char)e);
            else if (e >= '0' && e <= '7') {
                int v = e - '0';
                if (p < len && s[p] >= '0' && s[p] <= '7') v = v * 8 + (s[p++] - '0');
                if (p < len && s[p] >= '0' && s[p] <= '7') v = v * 8 + (s[p++] - '0');
                if (v >= 32 && v < 127) bpdf_append_ch(out, ol, cap, (char)v);
            }
        } else if (c == '(') { depth++; bpdf_append_ch(out, ol, cap, '('); }
        else if (c == ')') { depth--; if (depth > 0) bpdf_append_ch(out, ol, cap, ')'); }
        else { bpdf_append_ch(out, ol, cap, (char)c); }
    }
    return p;
}

/* Parse a PDF hex string <...> starting at '<' */
static uint32_t bpdf_read_hex(const uint8_t *s, uint32_t len, uint32_t p,
                              char *out, uint32_t *ol, uint32_t cap) {
    if (p >= len || s[p] != '<') return p;
    p++;
    int hi = -1;
    while (p < len && s[p] != '>') {
        int v = bpdf_hexval(s[p++]);
        if (v < 0) continue;
        if (hi < 0) hi = v;
        else {
            uint8_t byte = (uint8_t)((hi << 4) | v);
            if (byte >= 32 && byte < 127) bpdf_append_ch(out, ol, cap, (char)byte);
            hi = -1;
        }
    }
    if (p < len && s[p] == '>') p++;
    return p;
}

/* Extract human-readable text from a PDF content stream (BT ... ET) */
static uint32_t bpdf_extract_stream_text(const uint8_t *s, uint32_t len,
                                         char *out, uint32_t cap) {
    uint32_t ol = 0;
    int in_bt = 0;
    uint32_t p = 0;
    while (p < len && ol + 4 < cap) {
        while (p < len && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) p++;
        if (p >= len) break;
        if (s[p] == '%') { while (p < len && s[p] != '\n') p++; continue; }

        if (!in_bt) {
            if (p + 2 <= len && s[p] == 'B' && s[p + 1] == 'T' &&
                (p + 2 == len || s[p + 2] <= ' ' || s[p + 2] == '/')) {
                in_bt = 1; p += 2;
                if (ol > 0 && out[ol - 1] != '\n') bpdf_append_ch(out, &ol, cap, '\n');
                continue;
            }
            p++;
            continue;
        }

        if (p + 2 <= len && s[p] == 'E' && s[p + 1] == 'T' &&
            (p + 2 == len || s[p + 2] <= ' ')) {
            in_bt = 0; p += 2;
            if (ol > 0 && out[ol - 1] != '\n') bpdf_append_ch(out, &ol, cap, '\n');
            continue;
        }

        if (s[p] == '(') {
            p = bpdf_read_lit(s, len, p, out, &ol, cap);
            continue;
        }
        if (s[p] == '<' && (p + 1 >= len || s[p + 1] != '<')) {
            p = bpdf_read_hex(s, len, p, out, &ol, cap);
            continue;
        }
        if (s[p] == '[') {
            p++;
            while (p < len && s[p] != ']') {
                if (s[p] == '(') p = bpdf_read_lit(s, len, p, out, &ol, cap);
                else if (s[p] == '<') p = bpdf_read_hex(s, len, p, out, &ol, cap);
                else if (s[p] == '-' && p + 1 < len && s[p + 1] >= '1' && s[p + 1] <= '9') {
                    p++;
                    int num = 0;
                    while (p < len && s[p] >= '0' && s[p] <= '9') num = num * 10 + (s[p++] - '0');
                    if (num >= 140 && ol > 0 && out[ol - 1] != ' ' && out[ol - 1] != '\n')
                        bpdf_append_ch(out, &ol, cap, ' ');
                } else p++;
            }
            if (p < len && s[p] == ']') p++;
            continue;
        }
        /* Detect line-move operators: Td, TD, T*, Tm, ', " */
        if (s[p] == 'T' && p + 1 < len && (s[p + 1] == 'd' || s[p + 1] == 'D' || s[p + 1] == '*' || s[p + 1] == 'm')) {
            if (ol > 0 && out[ol - 1] != '\n') bpdf_append_ch(out, &ol, cap, '\n');
            p += 2;
            continue;
        }
        p++;
    }
    out[ol] = 0;
    return ol;
}

static void bpdf_find_meta(const uint8_t *d, uint32_t len, const char *key, char *out, uint32_t cap) {
    out[0] = 0;
    int pos = bpdf_find_seq(d, len, 0, key);
    if (pos < 0) return;
    uint32_t p = (uint32_t)pos;
    while (key[p - (uint32_t)pos]) p++;
    while (p < len && (d[p] == ' ' || d[p] == '\t' || d[p] == '\r' || d[p] == '\n')) p++;
    uint32_t ol = 0;
    if (p < len && d[p] == '(') bpdf_read_lit(d, len, p, out, &ol, cap);
    else if (p < len && d[p] == '<') bpdf_read_hex(d, len, p, out, &ol, cap);
    out[ol] = 0;
}

static void bpdf_free(bpdf_doc_t *doc, void (*free_fn)(void *)) {
    if (!doc || !free_fn) return;
    for (int i = 0; i < doc->page_count; i++) {
        if (doc->pages[i].text) free_fn(doc->pages[i].text);
        doc->pages[i].text = 0; doc->pages[i].len = 0;
    }
    doc->page_count = 0;
}

static int bpdf_parse(const uint8_t *d, uint32_t len,
                      void *(*alloc_fn)(size_t), void (*free_fn)(void *),
                      bpdf_doc_t *doc) {
    if (!d || len < 8 || !alloc_fn || !free_fn || !doc) return -1;
    if (!bpdf_memeq(d, "%PDF-", 5)) return -1;
    doc->title[0] = 0; doc->author[0] = 0; doc->page_count = 0;
    uint32_t vl = 0;
    while (vl < 12 && 5 + vl < len && d[5 + vl] != '\r' && d[5 + vl] != '\n') {
        doc->version[vl] = (char)d[5 + vl]; vl++;
    }
    doc->version[vl] = 0;
    bpdf_find_meta(d, len, "/Title", doc->title, sizeof(doc->title));
    bpdf_find_meta(d, len, "/Author", doc->author, sizeof(doc->author));

    uint32_t pos = 0;
    while (pos < len && doc->page_count < BPDF_MAX_PAGES) {
        int st = bpdf_find_seq(d, len, pos, "stream");
        if (st < 0) break;
        /* Ignore "endstream" false match */
        if (st >= 3 && bpdf_memeq(d + st - 3, "end", 3)) { pos = (uint32_t)st + 6; continue; }
        uint32_t sdata = (uint32_t)st + 6;
        if (sdata < len && d[sdata] == '\r') sdata++;
        if (sdata < len && d[sdata] == '\n') sdata++;
        int en = bpdf_find_seq(d, len, sdata, "endstream");
        if (en < 0) break;
        uint32_t slen = (uint32_t)en - sdata;
        while (slen > 0 && (d[sdata + slen - 1] == '\n' || d[sdata + slen - 1] == '\r')) slen--;

        /* Check dictionary before "stream" for /FlateDecode or /Image */
        uint32_t dstart = ((uint32_t)st > 256) ? ((uint32_t)st - 256) : 0;
        int is_img = bpdf_find_seq(d, (uint32_t)st, dstart, "/Image") >= 0;
        int is_flate = bpdf_find_seq(d, (uint32_t)st, dstart, "/FlateDecode") >= 0;
        pos = (uint32_t)en + 9;
        if (is_img || slen == 0) continue;

        const uint8_t *content = d + sdata;
        uint32_t clen = slen;
        uint8_t *dec = 0;
        if (is_flate || (slen >= 2 && (content[0] & 0x0F) == 8 && ((((uint32_t)content[0] << 8) | content[1]) % 31u) == 0)) {
            uint32_t dl = 0;
            if (bpdf_inflate_zlib(content, slen, alloc_fn, free_fn, &dec, &dl, 512u * 1024u) == 0 && dec) {
                content = dec; clen = dl;
            }
        }

        char *txt = (char *)alloc_fn(16384u);
        if (txt) {
            uint32_t tl = bpdf_extract_stream_text(content, clen, txt, 16384u);
            /* Trim leading/trailing whitespace */
            uint32_t vis = 0;
            for (uint32_t k = 0; k < tl; k++) if (txt[k] > ' ') vis++;
            if (vis >= 2) {
                doc->pages[doc->page_count].text = txt;
                doc->pages[doc->page_count].len = tl;
                doc->page_count++;
            } else {
                free_fn(txt);
            }
        }
        if (dec) free_fn(dec);
    }
    return 0;
}

#endif /* ARCTIAN_BROWSER_PDF_H */
