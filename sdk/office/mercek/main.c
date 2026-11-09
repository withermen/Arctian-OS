/**
 * Mercek — PDF ve PNG goruntuleyici (tam ekran, 0.8.6)
 *
 * Dosya gercekten diskten okunur:
 *  - PNG: wolf_image host cozucusuyla cizilir (canvas kaydirilabilir).
 *  - PDF: sade alt kume. Sayfalar "/Type /Page" ayrica sayilir, icerik
 *    akisi (FlateDecode ise app icindeki tiny-inflate ile) cozulur,
 *    BT/Td/Tm/Tj/TJ/re isleyicisi metni konumuyla beyaz sayfaya cizer.
 *    MediaBox okunur; A4 oranli panelde orantilidir. Sifreli PDF ve
 *    render gerektiren icerik icin durust uyari verilir — bos sayfa yok.
 * Acilir: wolf_arg (Safak'tan gelen yol); arg yoksa C:/belgeler,
 * C:/resimler, C:/masaustu listelenen secme penceresi acilir.
 * Gercek render motoru bu planin disinda; hedef kendi urettigimiz ve
 * sade metinli PDF'lerdir.
 */
#include "wolf.h"

#define C_GROUND 0x14161B
#define C_LINE   0x2C313B
#define C_PLINE  0x262B35
#define C_CANV   0x0B0C0F
#define C_PANEL  0x181B22
#define C_FIELD  0x0F1115
#define C_ACC    0x3B82F6
#define C_ACC_D  0x2E6FD6
#define C_TXT    0xE8EAEE
#define C_TXT2   0xC9CED8
#define C_META   0x8B919C
#define C_PAGE   0xFBFBFC
#define C_INK    0x1A1D24
#define C_SUB    0x6B717D

#define PDF_CAP  (160 * 1024)   /* okunacak en buyuk PDF */
#define FLAT_CAP (96 * 1024)    /* bir sayfanin cozulmus icerigi */
#define MAX_PAGE 128
#define MAX_TX   80             /* sayfada cizilecek etiket */
#define MAX_RC   40             /* sayfada cizilecek dikdortgen */
#define MAX_TH   16             /* sol listede gosterilen kucuk resim */
#define PICK_MAX 24

static int g_w, g_h, g_s;
static wolf_id g_root;

static int S(int v) { return (v * g_s + 500) / 1000; }
static int F(int px)
{
    int v = (px * g_s + 500) / 1000;
    if (v < 6) v = 6;
    return (v + 1) / 2 * 2;
}

static void bgc(wolf_id w, uint32_t c)  { wolf_style(w, WOLF_SP_BG_COLOR, (int)c, 0); }
static void bgo(wolf_id w, int o)       { wolf_style(w, WOLF_SP_BG_OPA, o, 0); }
static void rad(wolf_id w, int rp)      { wolf_style(w, WOLF_SP_RADIUS, rp, 0); }
static void fnt(wolf_id w, int px)      { wolf_style(w, WOLF_SP_FONT_PX, F(px), 0); }
static void tcol(wolf_id w, uint32_t c) { wolf_style(w, WOLF_SP_TEXT_COLOR, (int)c, 0); }
static void hov(wolf_id w, uint32_t c)
{
    wolf_style(w, WOLF_SP_BG_COLOR, (int)c, WOLF_SEL(WOLF_SF_HOVER, 0));
    wolf_style(w, WOLF_SP_BG_OPA, 100, WOLF_SEL(WOLF_SF_HOVER, 0));
}
static void brd_c(wolf_id w, int side, uint32_t c)
{
    wolf_style(w, WOLF_SP_BORDER_WIDTH, 1, 0);
    wolf_style(w, WOLF_SP_BORDER_COLOR, (int)c, 0);
    wolf_style(w, WOLF_SP_BORDER_SIDE, side, 0);
}

static wolf_id lab(wolf_id p, int x, int y, int wpx, const char *t,
                   int px, uint32_t col, int al)
{
    wolf_id l = wolf_label(p, x, y, t);
    wolf_resize(l, S(wpx), F(px) + 6);
    wolf_style(l, WOLF_SP_TEXT_LONG, 2, 0);
    if (al >= 0) wolf_style(l, WOLF_SP_TEXT_ALIGN, al, 0);
    fnt(l, px); tcol(l, col);
    return l;
}

/* ---- kucuk yardimcilar (freestanding) ---- */
static void scpy(char *d, int cap, const char *s)
{
    int i = 0;
    if (cap > 0) {
        while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
        d[i] = 0;
    }
}
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int c_low(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
/* yol uzantisi .png/.jpg/.pdf seklinde mi? (buyuk/kucuk harf duyarsiz) */
static int ext_is(const char *path, const char *ext)
{
    const char *dot = 0;
    for (const char *p = path; *p; p++) if (*p == '.') dot = p;
    if (!dot) return 0;
    int i = 0;
    while (ext[i] && dot[i]) {
        if (c_low(dot[i]) != ext[i]) return 0;
        i++;
    }
    return ext[i] == 0 && dot[i] == 0;
}
static const char *base_name(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/') b = p + 1;
    return b;
}
static int is_ws(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == 0; }
/* tam sayi ayractir (isaret/ondalik kabul eder); *pp girdi-çıkti pointer */
static int aton(const char **pp, double *out)
{
    const char *p = *pp;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
    int any = 0;
    double v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; any = 1; }
    if (*p == '.') {
        p++;
        double f = 0.1;
        while (*p >= '0' && *p <= '9') { v += (*p - '0') * f; f *= 0.1; p++; any = 1; }
    }
    if (!any) return 0;
    *out = neg ? -v : v;
    *pp = p;
    return 1;
}
/* bellekte alt dize ara (sonlu n, NUL'a bakmaz) */
static int bfind(const char *b, int n, int from, const char *pat)
{
    int pl = slen(pat);
    if (pl > n) return -1;
    for (int i = from; i + pl <= n; i++) {
        int ok = 1;
        for (int k = 0; k < pl; k++) if (b[i + k] != pat[k]) { ok = 0; break; }
        if (ok) return i;
    }
    return -1;
}

/* ---- tiny-inflate (zlib govdesi; PDF FlateDecode icin) ----
 * LSB-first bit sirasi, sabit + dinamik Huffman, saklanan blok.
 * zlib basligi (2 bayt) cagiran taraf atlar; adler32 yok sayilir. */
typedef struct { const uint8_t *s; int n; long pos; int bad; } IB;
typedef struct { uint16_t cnt[16]; uint16_t sym[320]; } HC;

static uint32_t ibits(IB *z, int k)
{
    uint32_t v = 0;
    for (int i = 0; i < k; i++) {
        long b = z->pos++;
        if ((b >> 3) >= z->n) { z->bad = 1; return 0; }
        v |= (uint32_t)((z->s[b >> 3] >> (b & 7)) & 1) << i;
    }
    return v;
}
static void hbuild(HC *h, const uint8_t *lens, int n)
{
    int off[16], idx[16];
    for (int i = 0; i < 16; i++) { h->cnt[i] = 0; off[i] = 0; idx[i] = 0; }
    for (int i = 0; i < n; i++) if (lens[i] < 16) h->cnt[lens[i]]++;
    h->cnt[0] = 0;
    /* gruplar uzunluga gore kiristirilmis sirada yan yana durur */
    for (int len = 1; len < 15; len++) off[len + 1] = off[len] + h->cnt[len];
    for (int len = 1; len <= 15; len++) idx[len] = off[len];
    for (int i = 0; i < n; i++)
        if (lens[i]) h->sym[idx[lens[i]]++] = (uint16_t)i;
}
static int hdec(HC *h, IB *z)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= (int)ibits(z, 1);
        int cnt = h->cnt[len];
        if (code - cnt < first) return z->bad ? -1 : h->sym[index + code - first];
        index += cnt;
        first += cnt;
        first <<= 1;
        code <<= 1;
    }
    z->bad = 1;
    return -1;
}
static const int L_BASE[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23,
                                27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163,
                                195, 227, 258 };
static const int L_EXTRA[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const int D_BASE[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97,
                                129, 193, 257, 385, 513, 769, 1025, 1537, 2049,
                                3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const int D_EXTRA[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static const uint8_t CL_ORDER[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4,
                                      12, 3, 13, 2, 14, 1, 15 };

/* src'yi dst'e cozer; üretilen bayt sayisi, hata -1. Eksik girdide
 * uretileni dondurur (PDF akislari "endstream"de kesilir). */
static int tinflate(const uint8_t *src, int n, uint8_t *dst, int dcap)
{
    IB z; z.s = src; z.n = n; z.pos = 0; z.bad = 0;
    int out = 0, last = 0;
    uint8_t lens[320];

    while (!last && !z.bad) {
        last = (int)ibits(&z, 1);
        int type = (int)ibits(&z, 2);
        if (type == 0) {                    /* saklanan blok */
            z.pos = (z.pos + 7) & ~7L;
            uint32_t l0 = ibits(&z, 8), l1 = ibits(&z, 8);
            uint32_t m0 = ibits(&z, 8), m1 = ibits(&z, 8);
            (void)m0; (void)m1;
            uint32_t len = l0 | (l1 << 8);
            for (uint32_t i = 0; i < len && !z.bad; i++) {
                uint32_t by = ibits(&z, 8);
                if (out < dcap) dst[out++] = (uint8_t)by;
            }
        } else {
            HC hl, hd;
            if (type == 1) {
                uint8_t ll[288], dl[30];
                for (int i = 0; i < 144; i++) ll[i] = 8;
                for (int i = 144; i < 256; i++) ll[i] = 9;
                for (int i = 256; i < 280; i++) ll[i] = 7;
                for (int i = 280; i < 288; i++) ll[i] = 8;
                for (int i = 0; i < 30; i++) dl[i] = 5;
                hbuild(&hl, ll, 288);
                hbuild(&hd, dl, 30);
            } else if (type == 2) {
                int hlit = (int)ibits(&z, 5) + 257;
                int hdist = (int)ibits(&z, 5) + 1;
                int hclen = (int)ibits(&z, 4) + 4;
                uint8_t clen[19];
                for (int i = 0; i < 19; i++) clen[i] = 0;
                for (int i = 0; i < hclen; i++)
                    clen[CL_ORDER[i]] = (uint8_t)ibits(&z, 3);
                HC hc;
                hbuild(&hc, clen, 19);
                int nlen = 0;
                while (nlen < hlit + hdist && !z.bad) {
                    int sy = hdec(&hc, &z);
                    if (sy < 0) break;
                    if (sy < 16) lens[nlen++] = (uint8_t)sy;
                    else {
                        int rep = 0, val = 0;
                        if (sy == 16) { val = nlen ? lens[nlen - 1] : 0; rep = 3 + (int)ibits(&z, 2); }
                        else if (sy == 17) rep = 3 + (int)ibits(&z, 3);
                        else rep = 11 + (int)ibits(&z, 7);
                        while (rep-- && nlen < hlit + hdist) lens[nlen++] = (uint8_t)val;
                    }
                }
                hbuild(&hl, lens, hlit);
                hbuild(&hd, lens + hlit, hdist);
            } else { z.bad = 1; break; }

            for (;;) {
                int sy = hdec(&hl, &z);
                if (z.bad) break;
                if (sy < 0) { z.bad = 1; break; }
                if (sy < 256) { if (out < dcap) dst[out++] = (uint8_t)sy; else z.bad = 1; continue; }
                if (sy == 256) break;
                sy -= 257;
                if (sy >= 29) { z.bad = 1; break; }
                int len = L_BASE[sy] + (int)ibits(&z, L_EXTRA[sy]);
                int ds = hdec(&hd, &z);
                if (z.bad || ds < 0 || ds >= 30) { z.bad = 1; break; }
                int dist = D_BASE[ds] + (int)ibits(&z, D_EXTRA[ds]);
                if (dist > out) { z.bad = 1; break; }
                for (int i = 0; i < len; i++) {
                    uint8_t by = dst[out - dist];
                    if (out < dcap) dst[out++] = by;
                    else { z.bad = 1; break; }
                }
            }
        }
    }
    if (z.bad && out == 0) return -1;
    return out;
}

/* ---- belge durumu ---- */
static int  g_kind;                 /* 0 yok, 1 png, 2 pdf, 3 desteklenmiyor */
static char g_path[200];
static char g_name[72];
static int  g_size = -1;
static int  g_npage;                /* pdf: sayfa; png: 1 */
static int  g_page = 1;
static int  g_poff[MAX_PAGE];       /* "/Type /Page" iz dusum adresleri */
static int  g_media_w = 595, g_media_h = 842;
static int  g_has_xobj;             /* içerikte gorsel var mi (durust bilgi) */
static char g_err[96];              /* acilista kullaniciya soylenecek */

static char g_pdf[PDF_CAP];
static char g_flat[FLAT_CAP];

/* ---- PDF sozdizimi ---- */
/* /Type sonrasi token "/Page" mu (Pages elenir)? */
static int is_page_type(const char *b, int n, int at)
{
    int p = at + 5;                    /* "/Type" */
    while (p < n && is_ws(b[p])) p++;
    if (p < n && b[p] == '/') p++;
    if (p + 4 > n) return 0;
    if (b[p] != 'P' || b[p + 1] != 'a' || b[p + 2] != 'g' || b[p + 3] != 'e') return 0;
    int e = p + 4;
    if (e < n && ((b[e] >= 'A' && b[e] <= 'Z') || (b[e] >= 'a' && b[e] <= 'z'))) return 0;
    return 1;
}

static void pdf_scan_pages(void)
{
    g_npage = 0;
    int p = 0;
    while (p < g_size) {
        int at = bfind(g_pdf, g_size, p, "/Type");
        if (at < 0) break;
        if (is_page_type(g_pdf, g_size, at)) {
            if (g_npage < MAX_PAGE) g_poff[g_npage++] = at;
        }
        p = at + 5;
    }
}

/* "N 0 obj" baslangicini bul; bulamazsa -1 */
static int find_obj(const char *b, int n, int num)
{
    char pat[24];
    int l = 0;
    char tmp[12];
    wolf_itoa(num, tmp, sizeof tmp);
    for (const char *t = tmp; *t; t++) pat[l++] = *t;
    pat[l++] = ' '; pat[l++] = '0'; pat[l++] = ' ';
    pat[l++] = 'o'; pat[l++] = 'b'; pat[l++] = 'j'; pat[l] = 0;
    int p = 0;
    while (p < n) {
        int at = bfind(b, n, p, pat);
        if (at < 0) return -1;
        if (at == 0 || is_ws(b[at - 1])) return at;
        p = at + 1;
    }
    return -1;
}

/* [a,b) arasindaki akisin govdesini g_flat'a koy; uzunluk veya -1.
 * flate ise cozer. header: "/FlateDecode" arayacagimiz alanin ucu. */
static int stream_body(int a, int b)
{
    int st = bfind(g_pdf, b, a, "stream");
    if (st < 0 || st >= b) return -1;
    int flat = bfind(g_pdf, st, a, "FlateDecode") >= 0;
    int d = st + 6;
    if (d < g_size && g_pdf[d] == '\r') d++;
    if (d < g_size && g_pdf[d] == '\n') d++;
    int en = bfind(g_pdf, g_size, d, "endstream");
    int len = (en < 0 ? g_size : en) - d;
    if (len < 0) return -1;
    if (!flat) {
        if (len > FLAT_CAP - 1) len = FLAT_CAP - 1;
        for (int i = 0; i < len; i++) g_flat[i] = g_pdf[d + i];
        g_flat[len] = 0;
        return len;
    }
    /* zlib basligi (CM=8, %31 kontrolu) varsa iki bayt atla; yoksa
     * ham deflate varsayilir. */
    if (len > 2 && (((uint8_t)g_pdf[d] & 0x0F) == 8) &&
        ((((uint8_t)g_pdf[d] << 8) | (uint8_t)g_pdf[d + 1]) % 31) == 0) {
        d += 2; len -= 2;
    }
    int out = tinflate((const uint8_t *)g_pdf + d, len, (uint8_t *)g_flat, FLAT_CAP - 1);
    if (out < 0) return -1;
    g_flat[out] = 0;
    return out;
}

/* sayfanin icerik akisini hazirla; -1 yok/cozulemedi */
static int page_stream(int idx)
{
    int a = g_poff[idx];
    int b = (idx + 1 < g_npage) ? g_poff[idx + 1] : g_size;

    /* MediaBox: /MediaBox [x0 y0 x1 y1] */
    int mb = bfind(g_pdf, b, a, "/MediaBox");
    if (mb >= 0) {
        const char *p = g_pdf + mb + 9;
        while (p < g_pdf + b && *p != '[' && *p != 0) p++;
        if (p < g_pdf + b && *p == '[') {
            p++;
            double x0, y0, x1, y1;
            const char *pp = p;
            if (aton(&pp, &x0) && aton(&pp, &y0) && aton(&pp, &x1) && aton(&pp, &y1)) {
                int w = (int)(x1 - x0), h = (int)(y1 - y0);
                if (w > 40 && w < 4000 && h > 40 && h < 4000) {
                    g_media_w = w; g_media_h = h;
                }
            }
        }
    }

    int c = bfind(g_pdf, b, a, "/Contents");
    if (c >= 0) {
        const char *p = g_pdf + c + 9;
        while (p < g_pdf + b && (*p == ' ' || *p == '[')) p++;
        double num = 0;
        const char *pp = p;
        if (aton(&pp, &num) && num >= 1 && num < 1000000) {
            int obj = find_obj(g_pdf, g_size, (int)num);
            if (obj >= 0) {
                int st = bfind(g_pdf, g_size, obj, "stream");
                if (st >= 0)
                    return stream_body(obj, st + 6 < g_size ? st + 6 : g_size);
            }
        }
    }
    return stream_body(a, b);
}

/* ---- icerik isleyici: metin ve dikdortgen topla ---- */
typedef struct { int x, y, fs; char t[80]; } Tx;
static Tx g_tx[MAX_TX];
static int g_ntx;
typedef struct { int x, y, w, h, fill; } Rc;
static Rc g_rc[MAX_RC];
static int g_nrc;

static char g_str[80];              /* son okunan (..)/<..> dizesi */
static int  g_strlen_;

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}
static void str_put(int c)
{
    if (g_strlen_ < (int)sizeof g_str - 1) {
        if (c < 32 || c == 127) c = ' ';
        g_str[g_strlen_++] = (char)c;
    }
}
/* p'yi '(' sonrasi baslat; kapanis sonrasini dondurur */
static const char *rd_lit(const char *p, const char *end)
{
    int depth = 1;
    while (p < end && depth) {
        int c = (uint8_t)*p++;
        if (c == '(') depth++;
        else if (c == ')') depth--;
        else if (c == '\\' && p < end) {
            int e = (uint8_t)*p++;
            if (e == 'n') str_put('\n');
            else if (e == 'r') str_put(' ');
            else if (e == 't') str_put(' ');
            else if (e >= '0' && e <= '7') {
                int v = e - '0';
                for (int k = 0; k < 2 && p < end && *p >= '0' && *p <= '7'; k++)
                    v = v * 8 + (*p++ - '0');
                str_put(v ? v : ' ');
            } else if (e == '\n') { /* satir devami */ }
            else str_put(e);
        } else if (c == '\r') { /* atla */ }
        else str_put(c);
    }
    return p;
}
static const char *rd_hex(const char *p, const char *end)
{
    int hi = -1;
    while (p < end && *p != '>') {
        int c = (uint8_t)*p++;
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
        int v = hexv(c);
        if (hi < 0) hi = v;
        else { str_put(hi * 16 + v); hi = -1; }
    }
    if (p < end) p++;
    return p;
}

/* ayrac sonrasi operator sozcugunu kucuk harfle al */
static int rd_op(const char *p, const char *end, char *op, int cap)
{
    int l = 0;
    while (p < end && *p) {
        int c = c_low((uint8_t)*p);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '*' || c == '"' || c == '\'' || c == '\\') {
            if (l < cap - 1) op[l++] = (char)c;
            p++;
        } else break;
    }
    op[l] = 0;
    return l;
}

static void run_content(const char *c, int n)
{
    g_ntx = 0; g_nrc = 0;
    g_strlen_ = 0;
    double st[8]; int ns = 0;
    double lx = 0, ly = 0, tl = 0, fs = 11;
    double rx = 0, ry = 0, rw = 0, rh = 0; int pend = 0;
    const char *p = c, *end = c + n;

    while (p < end && *p) {
        int ch = (uint8_t)*p;
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == 0) { p++; continue; }
        if (ch == '%') { while (p < end && *p != '\n') p++; continue; }
        if (ch == '(') { g_strlen_ = 0; p = rd_lit(p + 1, end); continue; }
        if (ch == '<') {
            if (p + 1 < end && p[1] == '<') { p += 2; continue; }   /* sozluk */
            p = rd_hex(p + 1, end); continue;
        }
        if (ch == '>') { p++; if (p < end && *p == '>') p++; continue; }
        if (ch == ']') { p++; continue; }
        if (ch == '[') {
            /* TJ dizisi: dizideki dizeleri birlestir */
            g_strlen_ = 0;
            p++;
            while (p < end && *p && *p != ']') {
                if (*p == '(') p = rd_lit(p + 1, end);
                else if (*p == '<' && !(p + 1 < end && p[1] == '<')) p = rd_hex(p + 1, end);
                else if (*p == '-' || *p == '.' || (*p >= '0' && *p <= '9')) {
                    double v; const char *q = p;
                    aton(&q, &v);
                    if (q == p) p++; else p = q;
                } else p++;
            }
            if (p < end && *p == ']') p++;
            continue;                                   /* operator asagida */
        }
        if (ch == '/') {                                 /* isim: atla */
            p++;
            while (p < end && *p && !is_ws((uint8_t)*p) && *p != '/' &&
                   *p != '(' && *p != '<' && *p != '[') p++;
            continue;
        }
        if (ch == '-' || ch == '+' || ch == '.' || (ch >= '0' && ch <= '9')) {
            double v; const char *q = p;
            if (aton(&q, &v)) {
                if (ns >= 8) {
                    for (int k = 0; k < 7; k++) st[k] = st[k + 1];
                    ns = 7;
                }
                st[ns++] = v;
                p = q;
            } else p++;
            continue;
        }
        /* operator */
        char op[16];
        int ol = rd_op(p, end, op, sizeof op);
        if (ol == 0) { p++; continue; }
        p += ol;

        if (op[0] == 'b' && op[1] == 't' && op[2] == 0) { lx = ly = 0; g_strlen_ = 0; }
        else if (op[0] == 't' && op[1] == 'd' && op[2] == 0) {
            if (ns >= 2) { lx += st[ns - 2]; ly += st[ns - 1]; }
        } else if (op[0] == 't' && op[1] == 'd' && op[2] == '0') {
            if (ns >= 2) { lx += st[ns - 2]; ly -= st[ns - 1]; tl = st[ns - 1]; }
        } else if (op[0] == 't' && op[1] == 'm' && op[2] == 0) {
            if (ns >= 6) { lx = st[ns - 2]; ly = st[ns - 1]; }
        } else if (op[0] == 't' && op[1] == 'l' && op[2] == 0) {
            if (ns >= 1) tl = st[ns - 1];
        } else if (op[0] == 't' && op[1] == 'f' && op[2] == 0) {
            if (ns >= 1 && st[ns - 1] > 2 && st[ns - 1] < 200) fs = st[ns - 1];
        } else if ((op[0] == 't' && op[1] == 'j') || op[0] == '\'' || op[0] == '"' ||
                   (op[0] == 'T' && op[1] == 'j')) {
            if (op[0] == '\'') ly -= tl;
            if (g_strlen_) {
                if (g_ntx < MAX_TX) {
                    Tx *t = &g_tx[g_ntx];
                    t->x = (int)lx; t->y = (int)ly; t->fs = (int)fs;
                    int i = 0;
                    while (i < g_strlen_ && i < 79) { t->t[i] = g_str[i]; i++; }
                    t->t[i] = 0;
                    g_ntx++;
                }
                g_strlen_ = 0;
            }
        } else if (op[0] == 'r' && op[1] == 'e' && op[2] == 0) {
            if (ns >= 4) { rx = st[ns - 4]; ry = st[ns - 3]; rw = st[ns - 2]; rh = st[ns - 1]; pend = 1; }
        } else if (pend && (op[0] == 'f' || op[0] == 's' ||
                            (op[0] == 'b' && (op[1] == 0 || op[1] == 'm')))) {
            if (g_nrc < MAX_RC) {
                Rc *r = &g_rc[g_nrc];
                r->x = (int)rx; r->y = (int)ry; r->w = (int)rw; r->h = (int)rh;
                r->fill = (op[0] != 's');
                g_nrc++;
            }
            pend = 0;
        } else if (op[0] == 'b' && op[1] == 'i' && op[2] == 0) {
            /* goruntu govdesi: EI ayracina kadar atla (render edilmez) */
            int at = bfind(c, n, (int)(p - c), "EI");
            p = (at < 0 ? end : c + at + 2);
        }
        ns = 0;
    }
}

/* ---- durum ---- */
static int g_zi = 3;
static const int g_zooms[6] = { 50, 67, 80, 100, 125, 150 };
static wolf_id g_thumb[MAX_TH];
static int g_nth;
static wolf_id g_canvas, g_pbox, g_zbox;
static int g_pick;                          /* acma penceresi acik mi */
static char g_pickv[PICK_MAX][200];
static int g_pickn;

static void soon(wolf_id w, void *u)
{
    (void)w; (void)u;
    wolf_notify("Mercek", "Bu ozellik 0.8.6'da yok");
}

static void utoa2(int v, char *t)
{
    int l = 0;
    if (v >= 100) t[l++] = (char)('0' + v / 100);
    if (v >= 10) t[l++] = (char)('0' + (v / 10) % 10);
    t[l++] = (char)('0' + v % 10);
    t[l] = 0;
}

static void set_page_label(void)
{
    char t[16], n[8];
    int l = 0;
    utoa2(g_page, n);
    const char *p = n;
    while (*p) t[l++] = *p++;
    t[l++] = ' '; t[l++] = '/'; t[l++] = ' ';
    utoa2(g_npage ? g_npage : 1, n);
    p = n;
    while (*p) t[l++] = *p++;
    t[l] = 0;
    wolf_text(g_pbox, t);
}

static void refresh_thumbs(void)
{
    for (int k = 0; k < g_nth; k++) {
        int on = (k + 1 == g_page);
        wolf_style(g_thumb[k], WOLF_SP_BORDER_COLOR, on ? C_ACC : C_PLINE, 0);
        wolf_style(g_thumb[k], WOLF_SP_BORDER_WIDTH, on ? 2 : 1, 0);
    }
}

static void rebuild(void);

/* ---- sayfa govdesi ---- */
static void paint_page(void)
{
    wolf_clear(g_canvas);

    if (g_kind == 1) {                      /* PNG */
        wolf_id im = wolf_image(g_canvas, S(24), S(24), 0, 0, g_path);
        if (!im) lab(g_canvas, 24, 24, 300, "Goruntu acilamadi (bozuk ya da PNG degil)",
                      14, C_SUB, -1);
        return;
    }
    if (g_kind != 2) {
        lab(g_canvas, 0, 0, g_w - S(150) - S(250),
            g_err[0] ? g_err : "Goruntulenecek dosya secilmedi", 15, C_SUB, 1);
        return;
    }

    int z = g_zooms[g_zi];
    int pw = S(560) * z / 100;
    int ph = S(792) * z / 100;
    int avail_w = g_w - S(150) - S(250);
    int sx = pw + S(48) < avail_w ? (avail_w - pw) / 2 : S(24);

    wolf_id pg = wolf_panel(g_canvas, sx, S(24), pw, ph);
    bgc(pg, C_PAGE); bgo(pg, 100); rad(pg, 0);
    wolf_style(pg, WOLF_SP_SHADOW_WIDTH, S(28), 0);
    wolf_style(pg, WOLF_SP_SHADOW_COLOR, 0x000000, 0);
    wolf_style(pg, WOLF_SP_SHADOW_OPA, 45, 0);
    wolf_style(pg, WOLF_SP_SHADOW_OFS_Y, S(8), 0);

    int flen = page_stream(g_page - 1);
    if (flen < 0) {
        lab(pg, 0, ph / 2 - S(12), 560,
            "Sayfa icerigi cozulemedi (basit PDF alt kumesi)", 14, C_SUB, 1);
        return;
    }
    run_content(g_flat, flen);

    /* nokta -> sayfa olcegi (genislik uzerinden) */
    int sc100 = pw * 100 / (g_media_w > 0 ? g_media_w : 595);
    for (int i = 0; i < g_nrc; i++) {
        Rc *r = &g_rc[i];
        int x = r->x * sc100 / 100;
        int w = r->w * sc100 / 100;
        int h = r->h * sc100 / 100;
        int y = ph - (r->y * sc100 / 100) - h;
        if (w < 1 || h < 1 || y > ph || y + h < 0) continue;
        wolf_id b = wolf_panel(pg, x, y, w, h);
        bgc(b, r->fill ? 0xD8DBE0 : C_PAGE); bgo(b, 100); rad(b, 0);
        if (!r->fill) brd_c(b, 15, 0x9AA1AC);
    }
    for (int i = 0; i < g_ntx; i++) {
        Tx *t = &g_tx[i];
        int x = t->x * sc100 / 100;
        int lh = (t->fs * sc100 / 100) + 4;
        int y = ph - (t->y * sc100 / 100) - lh;
        if (y > ph || y + lh < 0 || x > pw) continue;
        int tw = pw - x - S(8);
        if (tw < S(40)) tw = S(40);
        lab(pg, x, y, tw * 1000 / g_s, t->t, t->fs * sc100 / 100, C_INK, -1);
    }
    if (!g_ntx && !g_nrc)
        lab(pg, 0, ph / 2 - S(12), 560,
            "Bu sayfada metin/dikdortgen yok (gorsel icerik render edilmez)",
            14, C_SUB, 1);
}

/* ---- olaylar ---- */
static void go_page(int p)
{
    if (p < 1) p = 1;
    if (p > g_npage) p = g_npage;
    if (p == g_page) return;
    g_page = p;
    set_page_label();
    refresh_thumbs();
    paint_page();
}

static void cb_prev(wolf_id w, void *u) { (void)w; (void)u; go_page(g_page - 1); }
static void cb_next(wolf_id w, void *u) { (void)w; (void)u; go_page(g_page + 1); }

static void set_zoom_label(void)
{
    char t[12];
    int l = 0;
    t[l++] = '%';
    int z = g_zooms[g_zi];
    char n[8]; utoa2(z, n);
    const char *p = n;
    while (*p) t[l++] = *p++;
    t[l] = 0;
    wolf_text(g_zbox, t);
}

static void cb_zin(wolf_id w, void *u)
{
    (void)w; (void)u;
    if (g_kind != 2) { soon(0, 0); return; }
    if (g_zi >= 5) return;
    g_zi++;
    set_zoom_label();
    paint_page();
}
static void cb_zout(wolf_id w, void *u)
{
    (void)w; (void)u;
    if (g_kind != 2) { soon(0, 0); return; }
    if (g_zi <= 0) return;
    g_zi--;
    set_zoom_label();
    paint_page();
}

static void thumb_cb(wolf_id w, void *u)
{
    (void)w;
    go_page(1 + (int)(long)u);
}

/* ---- dosya acma ---- */
static void pick_scan(void)
{
    g_pickn = 0;
    static const char *dirs[3] = { "C:/belgeler", "C:/resimler", "C:/masaustu" };
    char nm[96];
    for (int d = 0; d < 3 && g_pickn < PICK_MAX; d++) {
        int cnt = wolf_fs_count(dirs[d]);
        for (int i = 0; i < cnt && g_pickn < PICK_MAX; i++) {
            int isdir = 0;
            if (wolf_fs_list(dirs[d], i, nm, sizeof nm, &isdir) != 0) break;
            if (isdir || !nm[0] || nm[0] == '.') continue;
            if (!ext_is(nm, ".pdf") && !ext_is(nm, ".png") &&
                !ext_is(nm, ".jpg") && !ext_is(nm, ".jpeg")) continue;
            scpy(g_pickv[g_pickn], 200, dirs[d]);
            wolf_strcat(g_pickv[g_pickn], 200, "/");
            wolf_strcat(g_pickv[g_pickn], 200, nm);
            g_pickn++;
        }
    }
}

/* pdf: yukle ve tara. 0 OK, degilse g_err doldurulur */
static int pdf_load(void)
{
    g_err[0] = 0;
    int sz = wolf_fs_size(g_path);
    if (sz < 0) { scpy(g_err, sizeof g_err, "Dosya okunamadi"); return -1; }
    if (sz > PDF_CAP - 64) {
        scpy(g_err, sizeof g_err, "Dosya buyuk (en fazla 160 KB PDF)");
        return -1;
    }
    int n = wolf_fs_read(g_path, g_pdf, sz + 1);
    if (n < 8) { scpy(g_err, sizeof g_err, "Dosya okunamadi"); return -1; }
    g_size = n;
    if (!(g_pdf[0] == '%' && g_pdf[1] == 'P' && g_pdf[2] == 'D' && g_pdf[3] == 'F')) {
        scpy(g_err, sizeof g_err, "PDF imzasi yok — bu bir PDF degil");
        return -1;
    }
    if (bfind(g_pdf, g_size, 0, "/Encrypt") >= 0) {
        scpy(g_err, sizeof g_err, "Sifreli PDF desteklenmiyor");
        return -1;
    }
    g_media_w = 595; g_media_h = 842;
    g_has_xobj = bfind(g_pdf, g_size, 0, "/XObject") >= 0;
    pdf_scan_pages();
    if (!g_npage) {
        scpy(g_err, sizeof g_err, "Sayfa bulunamadi (basit PDF alt kumesi)");
        return -1;
    }
    return 0;
}

static void open_path(const char *path)
{
    scpy(g_path, sizeof g_path, path);
    scpy(g_name, sizeof g_name, base_name(path));
    g_size = wolf_fs_size(path);
    g_page = 1; g_zi = 3; g_err[0] = 0;
    g_npage = 0;
    if (ext_is(path, ".png")) {
        g_kind = 1;
        g_npage = 1;
    } else if (ext_is(path, ".pdf")) {
        g_kind = 2;
        if (pdf_load() != 0) {
            g_kind = 3;
            g_npage = 0;
            wolf_notify("Mercek", g_err);
        }
    } else {
        g_kind = 3;
        scpy(g_err, sizeof g_err, "Bu dosya turu desteklenmiyor (yalniz PDF, PNG)");
        wolf_notify("Mercek", g_err);
    }
    g_pick = 0;
    rebuild();
}

static void pick_cb(wolf_id w, void *u)
{
    (void)w;
    int i = (int)(long)u;
    if (i >= 0 && i < g_pickn) open_path(g_pickv[i]);
}

static void cb_pick_open(wolf_id w, void *u) { (void)w; (void)u; g_pick = 1; rebuild(); }
static void cb_pick_close(wolf_id w, void *u) { (void)w; (void)u; g_pick = 0; rebuild(); }
static void cb_reload(wolf_id w, void *u)
{
    (void)w; (void)u;
    if (g_path[0]) open_path(g_path);
}

/* ---- baslik cubugu ---- */
static void cb_min(wolf_id w, void *u)   { (void)w; (void)u; wolf_win_min(); }
static void cb_close(wolf_id w, void *u) { (void)w; (void)u; wolf_win_close(); }

static void winbtn(int x, const char *svg, wolf_event_fn cb, uint32_t hover)
{
    wolf_id b = wolf_panel(g_root, x, 0, S(46), S(36));
    bgo(b, 0); rad(b, 0); wolf_set_click(b, 1); hov(b, hover);
    wolf_icon(b, (S(46) - S(10)) / 2, (S(36) - S(10)) / 2, S(10), svg);
    wolf_on_click(b, cb);
}

#define SVG_MIN   "<svg viewBox=\"0 0 10 10\"><path d=\"M1 5h8\" stroke=\"#8b919c\" stroke-width=\"1\" fill=\"none\"/></svg>"
#define SVG_CLOSE "<svg viewBox=\"0 0 10 10\"><path d=\"M1.5 1.5l7 7M8.5 1.5l-7 7\" stroke=\"#8b919c\" stroke-width=\"1.1\" fill=\"none\"/></svg>"

static void titlebar(void)
{
    wolf_id tb = wolf_panel(g_root, 0, 0, g_w, S(36));
    bgc(tb, C_GROUND); rad(tb, 0); bgo(tb, 100); brd_c(tb, 2, C_LINE);
    char t[120];
    scpy(t, sizeof t, g_name[0] ? g_name : "Dosya secilmedi");
    wolf_strcat(t, sizeof t, " — Mercek");
    lab(tb, 12, (36 - 12) / 2, 560, t, 12, C_TXT2, -1);
    winbtn(g_w - S(92),  SVG_MIN,   cb_min,   0x262B35);
    winbtn(g_w - S(46),  SVG_CLOSE, cb_close, 0xC42B1C);
}

/* arac cubugu dugmesi; x mantiksal (olceklenmemis) px */
static wolf_id tbtn(int x, int w, const char *t, int primary, wolf_event_fn cb)
{
    wolf_id b = wolf_panel(g_root, S(x), S(36) + S(8), S(w), S(30));
    if (primary) { bgc(b, C_ACC); hov(b, C_ACC_D); }
    else { bgc(b, C_FIELD); brd_c(b, 15, 0x333A47); hov(b, 0x1A1E26); }
    rad(b, S(6)); bgo(b, 100);
    wolf_set_click(b, 1);
    wolf_on_click(b, cb);
    lab(b, 0, (S(30) - F(13) - 6) / 2, w, t, 13,
        primary ? 0xFFFFFF : C_TXT, 1);
    return b;
}

static void toolbar(void)
{
    wolf_id tb = wolf_panel(g_root, 0, S(36), g_w, S(46));
    bgc(tb, C_GROUND); rad(tb, 0); bgo(tb, 100); brd_c(tb, 2, C_LINE);

    tbtn(12, 30, "<", 0, cb_prev);
    tbtn(46, 30, ">", 0, cb_next);
    wolf_id pb = wolf_panel(g_root, S(84), S(36) + S(8), S(70), S(30));
    bgc(pb, C_FIELD); brd_c(pb, 15, 0x333A47); rad(pb, S(6)); bgo(pb, 100);
    g_pbox = lab(pb, 0, (S(30) - F(13) - 6) / 2, 70, "-", 13, C_TXT, 1);

    tbtn(166, 28, "-", 0, cb_zout);
    wolf_id zb = wolf_panel(g_root, S(200), S(36) + S(8), S(54), S(30));
    bgc(zb, C_GROUND); rad(zb, 0); bgo(zb, 0);
    g_zbox = lab(zb, 0, (S(30) - F(13) - 6) / 2, 54, "%100", 13, C_TXT2, 1);
    tbtn(260, 28, "+", 0, cb_zin);

    /* sag kume */
    int R = (g_w * 1000 + 500) / g_s;
    int x = R - 12;
    x -= 60; tbtn(x, 60, "Aç", 1, cb_pick_open);
    x -= 8 + 70; tbtn(x, 70, "Yenile", 0, cb_reload);
}

static void leftpanel(void)
{
    wolf_id lp = wolf_panel(g_root, 0, S(82), S(150), g_h - S(82) - S(28));
    bgc(lp, C_PANEL); rad(lp, 0); bgo(lp, 100); brd_c(lp, 8, C_PLINE);
    wolf_scrollable(lp, 1);

    if (g_kind != 2 && g_kind != 1) {
        lab(lp, 10, S(12), 130, "Dosya yok", 12, C_SUB, 1);
        return;
    }
    g_nth = g_npage < MAX_TH ? g_npage : MAX_TH;
    for (int i = 0; i < g_nth; i++) {
        int y = S(12) + i * (S(136) + S(12));
        wolf_id t = wolf_panel(lp, (S(150) - S(100)) / 2, y, S(100), S(136));
        bgc(t, C_PAGE); bgo(t, 100); rad(t, S(2));
        int on = (i + 1 == g_page);
        wolf_style(t, WOLF_SP_BORDER_WIDTH, on ? 2 : 1, 0);
        wolf_style(t, WOLF_SP_BORDER_COLOR, on ? C_ACC : C_PLINE, 0);
        wolf_set_click(t, 1); hov(t, 0xF0F1F4);
        wolf_on_click_ex(t, thumb_cb, (void *)(long)i);
        g_thumb[i] = t;
        for (int k = 0; k < 3; k++) {
            wolf_id ln = wolf_panel(t, S(12), S(16) + k * S(10), S(76), 1);
            bgc(ln, 0xC9CED8); bgo(ln, 100); rad(ln, 0);
        }
        char n[8]; utoa2(i + 1, n);
        lab(t, 0, S(136) - S(20), 100, n, 11, C_SUB, 1);
    }
}

static void rightpanel(void)
{
    int rx = g_w - S(250);
    wolf_id rp = wolf_panel(g_root, rx, S(82), S(250), g_h - S(82) - S(28));
    bgc(rp, C_PANEL); rad(rp, 0); bgo(rp, 100); brd_c(rp, 4, C_PLINE);

    lab(rp, 16, S(16), 218, "Belge", 14, C_TXT, -1);

    const char *kind_s = "—";
    if (g_kind == 1) kind_s = "PNG goruntu";
    if (g_kind == 2) kind_s = "PDF (basit alt kume)";
    if (g_kind == 3) kind_s = "Desteklenmiyor";

    char szs[24] = "—";
    if (g_size > 0) {
        char nb[12];
        wolf_itoa(g_size / 1024 + 1, nb, sizeof nb);
        scpy(szs, sizeof szs, nb);
        wolf_strcat(szs, sizeof szs, " KB");
    }
    char pgs[24] = "—";
    if (g_kind == 2) {
        char nb[8];
        wolf_itoa(g_npage, nb, sizeof nb);
        scpy(pgs, sizeof pgs, nb);
        wolf_strcat(pgs, sizeof pgs, " sayfa");
    } else if (g_kind == 1) scpy(pgs, sizeof pgs, "1 goruntu");

    char mbs[40] = "—";
    if (g_kind == 2) {
        char nb[8];
        wolf_itoa(g_media_w, nb, sizeof nb);
        scpy(mbs, sizeof mbs, nb);
        wolf_strcat(mbs, sizeof mbs, " x ");
        wolf_itoa(g_media_h, nb, sizeof nb);
        wolf_strcat(mbs, sizeof mbs, nb);
        wolf_strcat(mbs, sizeof mbs, " pt");
    }

    const char *rows[5][2] = {
        { "Tur", kind_s }, { "Boyut", szs }, { "Icerik", pgs },
        { "Sayfa boyu", mbs }, { "", "" },
    };
    for (int i = 0; i < 4; i++) {
        int y = S(16) + S(34) + i * (S(46) + S(10));
        wolf_id c = wolf_panel(rp, 16, y, S(218), S(46));
        bgc(c, C_FIELD); bgo(c, 100); rad(c, S(8));
        brd_c(c, 15, C_PLINE);
        lab(c, S(12), S(6), 194, rows[i][0], 11, C_META, -1);
        lab(c, S(12), S(24), 194, rows[i][1], 12, C_TXT2, -1);
    }
    if (g_kind == 2) {
        wolf_id c = wolf_panel(rp, 16, S(16) + S(34) + 4 * (S(46) + S(10)), S(218), S(64));
        bgc(c, C_FIELD); bgo(c, 100); rad(c, S(8));
        brd_c(c, 15, C_PLINE);
        lab(c, S(12), S(6), 194, "Sinirlar", 11, C_META, -1);
        wolf_id body = wolf_label(c, S(12), S(24),
            g_has_xobj
                ? "Gorsel/render edilmez; metin+celeri cizilir."
                : "Metin ve cizimler konumlu cizilir; font gorseli birebir degil.");
        wolf_resize(body, S(194), 2 * (F(12) + 6));
        wolf_style(body, WOLF_SP_TEXT_LONG, 0, 0);
        fnt(body, 12); tcol(body, C_META);
    }
}

static void statusbar(void)
{
    wolf_id sb = wolf_panel(g_root, 0, g_h - S(28), g_w, S(28));
    bgc(sb, C_GROUND); rad(sb, 0); bgo(sb, 100); brd_c(sb, 1, C_LINE);
    int ly = (S(28) - F(12) - 6) / 2;
    char t[96];
    t[0] = 0;
    if (g_kind == 2) {
        char nb[8];
        wolf_itoa(g_npage, nb, sizeof nb);
        scpy(t, sizeof t, nb);
        wolf_strcat(t, sizeof t, " sayfa · PDF");
        if (g_size > 0) {
            wolf_strcat(t, sizeof t, " · ");
            wolf_itoa(g_size / 1024 + 1, nb, sizeof nb);
            wolf_strcat(t, sizeof t, nb);
            wolf_strcat(t, sizeof t, " KB");
        }
    } else if (g_kind == 1) scpy(t, sizeof t, "PNG goruntu");
    else scpy(t, sizeof t, g_err[0] ? g_err : "Dosya secilmedi — Ac ile listeyin");
    lab(sb, 14, ly, 480, t, 12, C_META, -1);

    t[0] = 0;
    if (g_path[0]) { scpy(t, sizeof t, g_path); }
    else scpy(t, sizeof t, "C:/belgeler · C:/resimler");
    lab(sb, g_w - S(424), ly, 410, t, 12, C_META, 2);
}

/* ---- dosya secme penceresi ---- */
static void picker(void)
{
    wolf_id ov = wolf_panel(g_root, 0, 0, g_w, g_h);
    bgc(ov, 0x0A0B0E); bgo(ov, 82); rad(ov, 0);

    int pw2 = S(560), ph2 = g_h - S(120);
    wolf_id box = wolf_panel(ov, (g_w - pw2) / 2, S(60), pw2, ph2);
    bgc(box, C_PANEL); bgo(box, 100); rad(box, S(12));
    brd_c(box, 15, C_LINE);
    lab(box, 20, S(16), 360, "Ac — PDF ve PNG", 16, C_TXT, -1);
    lab(box, 20, S(44), 460, "C:/belgeler, C:/resimler, C:/masaustu", 11, C_META, -1);

    wolf_id cb_ = wolf_panel(box, pw2 - S(84), S(16), S(64), S(30));
    bgc(cb_, C_FIELD); brd_c(cb_, 15, 0x333A47); rad(cb_, S(6)); bgo(cb_, 100);
    wolf_set_click(cb_, 1); hov(cb_, 0x1A1E26);
    wolf_on_click(cb_, cb_pick_close);
    lab(cb_, 0, (S(30) - F(13) - 6) / 2, 64, "Kapat", 13, C_TXT, 1);

    wolf_id lst = wolf_panel(box, S(16), S(84), pw2 - S(32), ph2 - S(100));
    bgo(lst, 0); rad(lst, 0);
    wolf_scrollable(lst, 1);

    if (!g_pickn) {
        lab(lst, 10, S(10), 480,
            "Klasorlerde desteklenen dosya yok (C:/belgeler, C:/resimler, C:/masaustu)",
            12, C_SUB, -1);
        return;
    }
    for (int i = 0; i < g_pickn; i++) {
        int y = i * (S(44) + S(8));
        wolf_id row = wolf_panel(lst, 0, y, pw2 - S(48), S(44));
        bgc(row, C_FIELD); bgo(row, 100); rad(row, S(8));
        brd_c(row, 15, C_PLINE);
        wolf_set_click(row, 1); hov(row, 0x1A1E26);
        wolf_on_click_ex(row, pick_cb, (void *)(long)i);
        const char *b = base_name(g_pickv[i]);
        char line[220];
        scpy(line, sizeof line, b);
        wolf_strcat(line, sizeof line, "   ·   ");
        wolf_strcat(line, sizeof line, g_pickv[i]);
        lab(row, S(12), S(5), (int)((pw2 - S(72)) * 1000 / g_s), line, 12, C_TXT, -1);
        lab(row, S(12), S(24), (int)((pw2 - S(72)) * 1000 / g_s),
            ext_is(b, ".pdf") ? "PDF" : (ext_is(b, ".png") ? "PNG" : "JPG (desteklenmiyor)"),
            10, C_META, -1);
    }
}

/* ---- derleme ---- */
static void build(void)
{
    wolf_style(g_root, WOLF_SP_BG_COLOR, C_GROUND, 0);
    wolf_style(g_root, WOLF_SP_BG_OPA, 100, 0);

    titlebar();
    toolbar();
    set_page_label();
    set_zoom_label();
    leftpanel();

    g_canvas = wolf_panel(g_root, S(150), S(82),
                          g_w - S(150) - S(250), g_h - S(82) - S(28));
    bgc(g_canvas, C_CANV); rad(g_canvas, 0); bgo(g_canvas, 100);
    wolf_scrollable(g_canvas, 2);
    paint_page();

    rightpanel();
    statusbar();
    if (g_pick) { pick_scan(); picker(); }
}

static void rebuild(void)
{
    wolf_clear(g_root);
    build();
}

static void on_resz(wolf_id w, void *u)
{
    (void)w; (void)u;
    if (wolf_screen(&g_w, &g_h) != 0) return;
    int a = g_w * 1000 / 1280, b = g_h * 1000 / 720;
    g_s = a < b ? a : b;
    if (g_s < 300) g_s = 300;
    rebuild();
}

int wolf_main(const wolf_api_t *api)
{
    wolf_bind(api);
    g_root = wolf_root();
    if (wolf_screen(&g_w, &g_h) != 0) { g_w = 1280; g_h = 720; }
    int a = g_w * 1000 / 1280, b = g_h * 1000 / 720;
    g_s = a < b ? a : b;
    if (g_s < 300) g_s = 300;
    wolf_on_resize(on_resz, 0);

    const char *arg = wolf_arg();
    if (arg && arg[0]) {
        scpy(g_path, sizeof g_path, arg);
        scpy(g_name, sizeof g_name, base_name(arg));
        g_size = wolf_fs_size(arg);
        if (ext_is(arg, ".png")) { g_kind = 1; g_npage = 1; }
        else if (ext_is(arg, ".pdf")) {
            g_kind = 2;
            if (pdf_load() != 0) { g_kind = 3; g_npage = 0; }
        } else {
            g_kind = 3;
            scpy(g_err, sizeof g_err, "Bu dosya turu desteklenmiyor (yalniz PDF, PNG)");
        }
        if (g_err[0]) wolf_notify("Mercek", g_err);
    } else {
        g_pick = 1;             /* arg yoksa gercek klasorleri listele */
    }
    build();
    return 0;
}
