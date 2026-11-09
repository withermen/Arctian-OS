/**
 * Abakus — hucreli tablo (tam ekran, 0.8.4)
 *
 * Spec bolum 3.3: 36 baslik, 34 sekme, 70 ribon, 30 formul cubugu
 * (ad kutusu + fx), izgara A..G, sayfa sekmeleri, durum satiri.
 *
 * 0.8.4'te icerik gercek: hucreye tikla, fx cubuguna deger ya da formil
 * yaz, Enter (veya Uygula) hucreye yazar; izgara yeniden hesaplanir.
 * Formul ayristirici: dort islem, parantez, hucre adresi, TOPLA/ORT
 * (aralik ve liste). Dongusel gonderim "#CEVRE!" ile durur. Ac/Kaydet
 * dugmeleri CSV'yi C:/belgeler altinda okur/yazar; dosyayla acilirsa
 * (Safak listesi) o dosya yuklenir.
 *
 * Slot butcesi 512: hucreler panel+etiket cifti (70 hucre = 140).
 */
#include "wolf.h"

#define C_GROUND 0x14161B
#define C_LINE   0x2C313B
#define C_PLINE  0x262B35
#define C_CELL   0x101216   /* hucre zemini */
#define C_HDR    0x181B22   /* baslik gozleri */
#define C_ACC    0x3B82F6
#define C_TXT    0xE8EAEE
#define C_TXT2   0xC9CED8
#define C_META   0x8B919C
#define C_TAB    0xA3A8B3
#define C_FIELD  0x0F1115
#define C_INK    0xC9CED8
#define C_ERR    0xE06C6C

#define COLW 172           /* A..G sabit genislik (1204 = 44 + 7*172) */
#define ROWH 26
#define NROW 10
#define NCOL 7
#define CELLW 64           /* ham icerik siniri (fx girdisi de bu kadar) */

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

/* ---- kucuk yardimcilar (freestanding; libc yok) ---- */
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void scpy(char *d, int cap, const char *s)
{
    int i = 0;
    for (; s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}
static char up1(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static int ci_pre(const char *s, const char *word) /* on ek eslesmesi */
{
    int i = 0;
    for (; word[i]; i++) if (up1(s[i]) != up1(word[i])) return 0;
    return 1;
}

/* Metin olarak girilen sayiyi cozer. Turkce bicim iki yuzu var:
 * "8.500" binlik ayiraci, "8.5" ondalik. Kural: virgul tek ise ondaliyi
 * acar; nokta ancak ondalik henuz acilmamissa ve sagindaki grup 3 haneden
 * FARKLIYSA ondaliktir, degilse binlik ayiracidir ("1.234,56" calisir). */
static int pnum_loc(const char *s, double *out)
{
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return 0;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    if (*s < '0' || *s > '9') return 0;

    char dig[40];
    int dn = 0, dsep = -1, commas = 0;
    while (*s) {
        char ch = *s;
        if (ch >= '0' && ch <= '9') {
            if (dn >= (int)sizeof(dig) - 1) return 0;
            dig[dn++] = ch;
            s++;
            continue;
        }
        if (ch == '.') {
            const char *p = s + 1;
            int g = 0;
            while (*p >= '0' && *p <= '9') { g++; p++; }
            if (dsep < 0 && !commas && g != 3) dsep = dn;   /* ondalik */
            else if (dsep >= 0 || commas) return 0;         /* tekrar eden ayirac */
            s++;                                            /* nokta duser */
            continue;
        }
        if (ch == ',') {
            if (commas || dsep >= 0) return 0;
            dsep = dn;
            commas = 1;
            s++;
            continue;
        }
        return 0;                                  /* baska karakter: metin say */
    }
    if (dn == 0) return 0;
    if (dsep < 0) dsep = dn;
    double v = 0;
    for (int i = 0; i < dsep; i++) v = v * 10 + (dig[i] - '0');
    double fr = 0, div = 1;
    for (int i = dsep; i < dn; i++) { fr = fr * 10 + (dig[i] - '0'); div *= 10; }
    v += fr / div;
    *out = neg ? -v : v;
    return 1;
}

/* Sayiyi "102.000" / "1.250,5" biciminde yazar. */
static void fmt_num(double v, char *out, int cap)
{
    int o = 0;
    if (v > 1e15 || v < -1e15) { scpy(out, cap, "#Buyuk"); return; }
    long long c = (long long)(v * 100.0 + (v < 0 ? -0.5 : 0.5));
    if (c < 0) { out[o++] = '-'; c = -c; }
    long long ip = c / 100, fp = c % 100;

    char ib[24];
    int n = 0;
    if (ip == 0) ib[n++] = '0';
    while (ip > 0) { ib[n++] = (char)('0' + (int)(ip % 10)); ip /= 10; }
    for (int i = n - 1; i >= 0; i--) {
        if (o < cap - 1) out[o++] = ib[i];
        int left = i;                        /* solda kaç hane kaldi */
        if (left > 0 && left % 3 == 0 && o < cap - 1) out[o++] = '.';
    }
    if (fp) {
        while (fp % 10 == 0) fp /= 10;       /* 50 -> 5, 500 yok (max 99) */
        if (o < cap - 1) out[o++] = ',';
        char fb[3];
        int m = 0;
        while (fp > 0) { fb[m++] = (char)('0' + (int)(fp % 10)); fp /= 10; }
        for (int i = m - 1; i >= 0; i--) if (o < cap - 1) out[o++] = fb[i];
    }
    out[o] = 0;
}

/* ---- kagit uzerindeki veri ---- */
static char g_raw[NROW][NCOL][CELLW];      /* kullanicinin yazdigi (formul ham) */
static char g_disp[NROW][NCOL][CELLW];     /* izgarada gorunen */
static double g_num[NROW][NCOL];
static char g_isnum[NROW][NCOL];
static char g_errc[NROW][NCOL];

/* cozumleme durum(küresel; tek izgara icin yeterli) */
static const char *g_cp;
static int g_perr, g_depth;
static int g_done[NROW][NCOL], g_mvis[NROW][NCOL], g_cyc;

static double cell_val(int r, int c);

static void skip_ws(void) { while (*g_cp == ' ' || *g_cp == '\t') g_cp++; }

/* karsilikli cagri (p_primary <-> p_expr) */
static double p_expr(void);

static int p_num_simple(double *out)
{
    skip_ws();
    if (*g_cp < '0' || *g_cp > '9') return 0;
    double v = 0;
    while (*g_cp >= '0' && *g_cp <= '9') v = v * 10 + (*g_cp++ - '0');
    if (*g_cp == '.') {
        g_cp++;
        double fr = 0, div = 1;
        while (*g_cp >= '0' && *g_cp <= '9') { fr = fr * 10 + (*g_cp++ - '0'); div *= 10; }
        v += fr / div;
    }
    *out = v;
    return 1;
}

/* A1 / a1 -> hucre; 0 = hucre degil (imlec ilerlemez). */
static int p_ref(int *rr, int *cc)
{
    skip_ws();
    char ch = up1(*g_cp);
    if (ch < 'A' || ch > 'A' + NCOL - 1) return 0;
    const char *save = g_cp;
    g_cp++;
    if (*g_cp < '1' || *g_cp > '9') { g_cp = save; return 0; }
    int row = *g_cp++ - '0';
    if (*g_cp >= '0' && *g_cp <= '9') row = row * 10 + (*g_cp++ - '0');
    if (row < 1 || row > NROW) { g_cp = save; return 0; }
    *rr = row - 1; *cc = ch - 'A';
    return 1;
}

static double p_primary(void)
{
    skip_ws();
    if (g_depth++ > 24) { g_perr = 1; return 0; }
    double v = 0;
    if (*g_cp == '(') {
        g_cp++;
        v = p_expr(); /* tanim icin asagida */
        skip_ws();
        if (*g_cp == ')') g_cp++; else g_perr = 1;
    } else if (p_num_simple(&v)) {
        /* sayi */
    } else if (ci_pre(g_cp, "TOPLA") || ci_pre(g_cp, "ORT")) {
        int summode = up1(*g_cp) == 'T';
        g_cp += summode ? 5 : 3;
        if (!summode && ci_pre(g_cp, "ALAMA")) g_cp += 5;   /* ORTALAMA */
        skip_ws();
        if (*g_cp != '(') { g_perr = 1; g_depth--; return 0; }
        g_cp++;
        double sum = 0;
        int cnt = 0;
        for (;;) {
            int r1, c1;
            const char *bk = g_cp;
            if (p_ref(&r1, &c1)) {
                skip_ws();
                if (*g_cp == ':') {
                    g_cp++;
                    int r2, c2;
                    if (!p_ref(&r2, &c2)) { g_perr = 1; break; }
                    if (r1 > r2) { int t = r1; r1 = r2; r2 = t; }
                    if (c1 > c2) { int t = c1; c1 = c2; c2 = t; }
                    for (int r = r1; r <= r2; r++)
                        for (int c = c1; c <= c2; c++) {
                            double x = cell_val(r, c);
                            if (g_isnum[r][c]) { sum += x; cnt++; }
                        }
                } else {
                    double x = cell_val(r1, c1);
                    if (g_isnum[r1][c1]) { sum += x; cnt++; }
                }
            } else {
                g_cp = bk;
                double x = p_expr();
                if (g_perr) break;
                sum += x; cnt++;
            }
            skip_ws();
            if (*g_cp == ',') { g_cp++; continue; }
            break;
        }
        skip_ws();
        if (*g_cp == ')') g_cp++; else g_perr = 1;
        if (summode) v = sum;
        else v = cnt ? sum / (double)cnt : 0;
    } else {
        int r, c;
        if (p_ref(&r, &c)) v = cell_val(r, c);
        else g_perr = 1;
    }
    g_depth--;
    return v;
}

static double p_factor(void)
{
    skip_ws();
    if (*g_cp == '-') { g_cp++; return -p_factor(); }
    if (*g_cp == '+') { g_cp++; return p_factor(); }
    return p_primary();
}

static double p_term(void)
{
    double v = p_factor();
    for (;;) {
        skip_ws();
        if (*g_cp != '*' && *g_cp != '/') break;
        char op = *g_cp++;
        double x = p_factor();
        if (op == '*') v *= x;
        else if (x == 0) { g_perr = 1; return 0; }
        else v /= x;
    }
    return v;
}

static double p_expr(void)
{
    double v = p_term();
    for (;;) {
        skip_ws();
        if (*g_cp != '+' && *g_cp != '-') break;
        char op = *g_cp++;
        double x = p_term();
        v = op == '+' ? v + x : v - x;
    }
    return v;
}

/* hucreyi (tembel) cozer; g_num/g_isnum/g_disp bir kez doldurulur. */
static double cell_val(int r, int c)
{
    if (g_done[r][c]) return g_num[r][c];
    if (g_mvis[r][c]) { g_cyc = 1; return 0; }
    g_mvis[r][c] = 1;

    const char *raw = g_raw[r][c];
    double v = 0;
    int isnum = 0, err = 0;
    g_disp[r][c][0] = 0;
    if (raw[0]) {
        if (raw[0] == '=') {
            g_cp = raw + 1;
            g_perr = 0;
            g_depth = 0;
            v = p_expr();
            skip_ws();
            if (g_cyc || g_perr || *g_cp) err = g_cyc ? 2 : 1;
            else isnum = 1;
        } else if (pnum_loc(raw, &v)) {
            isnum = 1;
        }
    }
    g_mvis[r][c] = 0;
    if (err) {
        g_errc[r][c] = (char)err;
        g_isnum[r][c] = 0;
        scpy(g_disp[r][c], CELLW, err == 2 ? "#CEVRE!" : "#HATA!");
    } else if (isnum) {
        g_errc[r][c] = 0;
        g_num[r][c] = v;
        g_isnum[r][c] = 1;
        fmt_num(v, g_disp[r][c], CELLW);
    } else {
        g_errc[r][c] = 0;
        g_num[r][c] = 0;
        g_isnum[r][c] = 0;
        scpy(g_disp[r][c], CELLW, raw);
    }
    g_done[r][c] = 1;
    return v;
}

static void recompute(void)
{
    for (int r = 0; r < NROW; r++)
        for (int c = 0; c < NCOL; c++) {
            g_done[r][c] = 0;
            g_mvis[r][c] = 0;
        }
    g_cyc = 0;
    for (int r = 0; r < NROW; r++)
        for (int c = 0; c < NCOL; c++) cell_val(r, c);
}

/* ---- ornek icerik (specten; D sutunu artik gercek formul) ---- */
static void seed_default(void)
{
    static const char *init[NROW][NCOL] = {
        { "Kalem", "Adet", "Birim fiyat", "Tutar", "Not", "", "" },
        { "Sunucu kirasi", "12", "8.500", "=B2*C2", "", "", "" },
        { "Lisanslar", "25", "4.200", "=B3*C3", "", "", "" },
        { "Danismanlik", "40", "1.500", "=B4*C4", "", "", "" },
        { "Egitim", "8", "3.750", "=B5*C5", "", "", "" },
        { "Donanim", "5", "22.000", "=B6*C6", "", "", "" },
        { "Pazarlama", "1", "98.000", "=B7*C7", "-", "", "" },
        { "", "", "", "", "", "", "" },
        { "", "", "", "", "", "", "" },
        { "Toplam", "", "", "=TOPLA(D2:D7)", "", "", "" },
    };
    for (int r = 0; r < NROW; r++)
        for (int c = 0; c < NCOL; c++) scpy(g_raw[r][c], CELLW, init[r][c]);
}

/* ---- secim durumu + kabuk widget'lari ---- */
static wolf_id g_cell[NROW][NCOL];
static wolf_id g_lab[NROW][NCOL];
static wolf_id g_frame;
static wolf_id g_name, g_fx, g_status, g_title_lab;
static int g_sel = NCOL + 3;               /* D2 */
static int g_dirty;                        /* fx'te yazilmamis icerik var mi */
static char g_file[140] = "C:/belgeler/tablo.csv";
static char g_title[80] = "tablo.csv";

static const char *base_name(const char *p)
{
    const char *b = p;
    for (const char *s = p; *s; s++) if (*s == '/') b = s + 1;
    return b;
}

static void soon(wolf_id w, void *u)
{
    (void)w; (void)u;
    wolf_notify("Abaküs", "Bu ozellik simdilik yok");
}

/* ---- baslik (36): kompakt, kenar dugmeleri metin ---- */
static void cb_min(wolf_id w, void *u)   { (void)w; (void)u; wolf_win_min(); }
static void cb_close(wolf_id w, void *u) { (void)w; (void)u; wolf_win_close(); }

static void titlebar(void)
{
    wolf_id tb = wolf_panel(g_root, 0, 0, g_w, S(36));
    bgc(tb, C_GROUND); rad(tb, 0); bgo(tb, 100); brd_c(tb, 2, C_LINE);
    char t[128];
    int o = 0;
    for (const char *s = g_title; *s && o < (int)sizeof(t) - 20; s++) t[o++] = *s;
    const char *suf = " — Abaküs";
    for (const char *s = suf; *s; s++) t[o++] = *s;
    t[o] = 0;
    g_title_lab = lab(tb, 12, (36 - 12) / 2, 520, t, 12, C_TXT2, -1);
    wolf_id mn = lab(tb, 0, (36 - 13) / 2 - 1, 46, "—", 13, C_META, 1);
    wolf_move(mn, g_w - S(92), (S(36) - F(13) - 6) / 2);
    wolf_set_click(mn, 1); hov(mn, 0x262B35); wolf_on_click(mn, cb_min);
    wolf_id cl = lab(tb, 0, (36 - 13) / 2 - 1, 46, "×", 13, C_META, 1);
    wolf_move(cl, g_w - S(46), (S(36) - F(13) - 6) / 2);
    wolf_set_click(cl, 1); hov(cl, 0xC42B1C); wolf_on_click(cl, cb_close);
}

/* ---- sekme seridi (34) ---- */
static void tabstrip(void)
{
    wolf_id st = wolf_panel(g_root, 0, S(36), g_w, S(34));
    bgc(st, C_GROUND); rad(st, 0); bgo(st, 100); brd_c(st, 2, C_LINE);
    const char *tabs[4] = { "Dosya", "Giris", "Ekle", "Gorunum" };
    int on = 1, x = 12;
    for (int i = 0; i < 4; i++) {
        int tw = slen(tabs[i]) * 7 + 4;
        wolf_id t = lab(st, x, (34 - 13) / 2 - 1, tw + 16, tabs[i], 13,
                        i == on ? C_TXT : C_TAB, 1);
        wolf_set_click(t, 1); wolf_on_click(t, soon);
        if (i == on) {
            wolf_id u = wolf_panel(st, x, S(32), S(tw + 16), 2);
            bgc(u, C_ACC); bgo(u, 100); rad(u, 0);
        }
        x += S(tw + 16);
    }
}

/* ---- CSV al/yuk ---- */
static int g_last_r, g_last_c;              /* en son dolu hucre */

static void csv_store(int *r, int *ci, const char *f, int flen)
{
    if (*r >= NROW || *ci >= NCOL) return;
    char tmp[CELLW];
    int n = 0;
    while (flen > 0 && n < CELLW - 1) { tmp[n++] = *f++; flen--; }
    tmp[n] = 0;
    scpy(g_raw[*r][*ci], CELLW, tmp);
    if (*r > g_last_r) g_last_r = *r;
    if (*ci > g_last_c) g_last_c = *ci;
}

static void csv_parse(char *buf, int len)
{
    for (int r = 0; r < NROW; r++)
        for (int c = 0; c < NCOL; c++) g_raw[r][c][0] = 0;
    g_last_r = 0; g_last_c = 0;

    char sep = ',';
    for (int i = 0; i < len && buf[i] != '\n'; i++)
        if (buf[i] == ';') { sep = ';'; break; }

    int r = 0, c = 0, i = 0;
    while (i < len && r < NROW) {
        /* bir alan oku */
        char tmp[CELLW];
        int n = 0;
        if (buf[i] == '"') {
            i++;
            while (i < len && n < CELLW - 1) {
                if (buf[i] == '"') {
                    if (i + 1 < len && buf[i + 1] == '"') { tmp[n++] = '"'; i += 2; continue; }
                    i++;
                    break;
                }
                tmp[n++] = buf[i++];
            }
            while (i < len && buf[i] != sep && buf[i] != '\n') i++;
        } else {
            while (i < len && buf[i] != sep && buf[i] != '\n' && n < CELLW - 1)
                tmp[n++] = buf[i++];
            while (i < len && buf[i] != sep && buf[i] != '\n') i++;
        }
        while (i < len && buf[i] != sep && buf[i] != '\n') i++; /* tasma payi */
        tmp[n] = 0;
        if (n > 0 && tmp[n - 1] == '\r') tmp[--n] = 0;
        csv_store(&r, &c, tmp, n);
        if (i < len && buf[i] == sep) { i++; c++; continue; }
        if (i < len && buf[i] == '\n') { i++; r++; c = 0; }
    }
}

static void csv_build(char *out, int cap, int *len)
{
    int o = 0;
    recompute();
    int last = -1;
    for (int r = 0; r < NROW; r++)
        for (int c = 0; c < NCOL; c++)
            if (g_raw[r][c][0]) last = r;
    for (int r = 0; r <= last; r++) {
        for (int c = 0; c < NCOL; c++) {
            const char *v = g_raw[r][c];
            int q = 0;
            for (int i = 0; v[i]; i++)
                if (v[i] == ',' || v[i] == '"' || v[i] == '\n') { q = 1; break; }
            if (q && o < cap - 1) out[o++] = '"';
            for (int i = 0; v[i]; i++) {
                if (v[i] == '"' && o < cap - 2) out[o++] = '"';
                if (o < cap - 1) out[o++] = v[i];
            }
            if (q && o < cap - 1) out[o++] = '"';
            if (c < NCOL - 1 && o < cap - 1) out[o++] = ',';
        }
        if (o < cap - 2) out[o++] = '\n';
    }
    out[o] = 0;
    *len = o;
}

/* ---- durum satiri: secili sutunun sayilari ---- */
static void status_update(void)
{
    if (!g_status) return;
    int c = g_sel % NCOL;
    int cnt = 0;
    double sum = 0;
    for (int r = 0; r < NROW; r++)
        if (g_isnum[r][c]) { sum += g_num[r][c]; cnt++; }
    char sb[200];
    char nb[40];
    fmt_num(sum, nb, sizeof nb);
    int o = 0;
    const char *pfx = "Sayim: ";
    sb[o] = (char)('A' + c); o++;
    sb[o++] = ':'; sb[o++] = ' ';
    for (const char *s = pfx; *s && o < (int)sizeof(sb) - 90; s++) sb[o++] = *s;
    char cb2[12];
    int m = 0;
    int x = cnt;
    if (x == 0) cb2[m++] = '0';
    while (x > 0) { cb2[m++] = (char)('0' + x % 10); x /= 10; }
    for (int i = m - 1; i >= 0; i--) sb[o++] = cb2[i];
    const char *mid = "   Toplam: ";
    for (const char *s = mid; *s && o < (int)sizeof(sb) - 60; s++) sb[o++] = *s;
    for (const char *s = nb; *s && o < (int)sizeof(sb) - 40; s++) sb[o++] = *s;
    if (cnt) {
        const char *mid2 = "   Ortalama: ";
        for (const char *s = mid2; *s && o < (int)sizeof(sb) - 40; s++) sb[o++] = *s;
        fmt_num(sum / (double)cnt, nb, sizeof nb);
        for (const char *s = nb; *s && o < (int)sizeof(sb) - 2; s++) sb[o++] = *s;
    }
    sb[o] = 0;
    wolf_text(g_status, sb);
}

/* ---- izgara tazeleme ---- */
static void refresh_grid(void)
{
    for (int r = 0; r < NROW; r++) {
        for (int c = 0; c < NCOL; c++) {
            if (!g_lab[r][c]) continue;
            wolf_text(g_lab[r][c], g_disp[r][c]);
            int al = (g_isnum[r][c] || (c >= 1 && c <= 3)) ? 2 : 0;
            wolf_style(g_lab[r][c], WOLF_SP_TEXT_ALIGN, al, 0);
            uint32_t col = r == 0 ? C_META : (r == NROW - 1 ? C_TXT : C_INK);
            if (g_errc[r][c]) col = C_ERR;
            wolf_style(g_lab[r][c], WOLF_SP_TEXT_COLOR, (int)col, 0);
        }
    }
}

static void frame_place(void)
{
    int gx = (g_w - S(1204)) / 2;
    int r = g_sel / NCOL, c = g_sel % NCOL;
    wolf_move(g_frame, gx + S(44) + c * S(COLW), S(170) + S(24) + r * S(ROWH));
}

static void select_cell(int r, int c);

static void commit_cur(void)
{
    char txt[CELLW];
    wolf_text_of(g_fx, txt, sizeof txt);
    scpy(g_raw[g_sel / NCOL][g_sel % NCOL], CELLW, txt);
    g_dirty = 0;
    recompute();
    refresh_grid();
    status_update();
}

static void fx_change_cb(wolf_id w, void *u) { (void)w; (void)u; g_dirty = 1; }

static void fx_apply(wolf_id w, void *u)
{
    (void)w; (void)u;
    commit_cur();
    int r = g_sel / NCOL + 1;
    if (r >= NROW) r = NROW - 1;
    select_cell(r, g_sel % NCOL);
}

static void fx_clear_cb(wolf_id w, void *u)
{
    (void)w; (void)u;
    wolf_text(g_fx, "");
    g_dirty = 1;
}

/* dosya islari asagida (do_open/do_save) */

static void title_update(void)
{
    scpy(g_title, sizeof g_title, base_name(g_file));
    if (g_title_lab) {
        char t[128];
        scpy(t, sizeof t, g_title);
        int n = slen(t);
        const char *suf = " — Abaküs";
        for (const char *s = suf; *s && n < (int)sizeof(t) - 1; s++) t[n++] = *s;
        t[n] = 0;
        wolf_text(g_title_lab, t);
    }
}

static void do_open(wolf_id w, void *u);
static void do_save(wolf_id w, void *u);

/* acilis argumani ya da varsayilan dosyayi yukler; 0 = yuklenmedi. */
static int try_open(int notify)
{
    static char buf[4096];
    int n = wolf_fs_read(g_file, buf, (int)sizeof buf - 1);
    if (n <= 0) {
        if (notify) {
            char m[180];
            scpy(m, sizeof m, "Dosya okunamadi: ");
            int l = slen(m);
            scpy(m + l, (int)sizeof m - l, g_file);
            wolf_notify("Abaküs", m);
        }
        return 0;
    }
    buf[n] = 0;
    csv_parse(buf, n);
    return 1;
}

/* ---- hucre tiklama: yazilmamiş fx varsa once onu hucreye dok ---- */
static void cell_cb(wolf_id w, void *u)
{
    (void)w;
    int k = (int)(long)u;
    if (g_dirty) commit_cur();
    select_cell(k / NCOL, k % NCOL);
}

static void select_cell(int r, int c)
{
    g_sel = r * NCOL + c;
    char nm[8];
    int d = 0;
    nm[d++] = (char)('A' + c);
    int rr = r + 1;
    if (rr >= 10) nm[d++] = (char)('0' + rr / 10);
    nm[d++] = (char)('0' + rr % 10);
    nm[d] = 0;
    wolf_text(g_name, nm);
    wolf_text(g_fx, g_raw[r][c]);
    g_dirty = 0;
    frame_place();
    status_update();
}

/* izgara govdesi: baslik gozleri + hucreler + secim cercevesi */
static void grid(void)
{
    int gx = (g_w - S(1204)) / 2;
    int gy = S(170);

    wolf_id gp = wolf_panel(g_root, gx, gy, S(1204), S(24) + NROW * S(ROWH));
    bgc(gp, C_CELL); rad(gp, 0); bgo(gp, 100); brd_c(gp, 15, C_PLINE);
    wolf_scrollable(gp, 2);

    /* ust bant: bos solat + A..G */
    wolf_id cn = wolf_panel(gp, 0, 0, S(44), S(24));
    bgc(cn, C_HDR); bgo(cn, 100); rad(cn, 0);
    wolf_style(cn, WOLF_SP_BORDER_WIDTH, 1, 0);
    wolf_style(cn, WOLF_SP_BORDER_COLOR, C_PLINE, 0);
    for (int c = 0; c < NCOL; c++) {
        wolf_id h = wolf_panel(gp, S(44) + c * S(COLW), 0, S(COLW), S(24));
        bgc(h, C_HDR); bgo(h, 100); rad(h, 0);
        wolf_style(h, WOLF_SP_BORDER_WIDTH, 1, 0);
        wolf_style(h, WOLF_SP_BORDER_COLOR, C_PLINE, 0);
        char t[2] = { (char)('A' + c), 0 };
        lab(h, 0, (S(24) - F(12) - 6) / 2, COLW, t, 12, C_META, 1);
    }
    /* satir basliklari: tek etiket, iki satirlik paneller (slot icin) */
    for (int r = 0; r < NROW; r += 2) {
        char t[8];
        int n0 = r + 1, n1 = r + 2;
        int l = 0;
        if (n0 >= 10) t[l++] = (char)('0' + n0 / 10);
        t[l++] = (char)('0' + n0 % 10);
        t[l++] = '\n';
        if (n1 <= NROW) {
            if (n1 >= 10) t[l++] = (char)('0' + n1 / 10);
            t[l++] = (char)('0' + n1 % 10);
        }
        t[l] = 0;
        wolf_id t1 = wolf_label(gp, 0, S(24) + r * S(ROWH) - 1, t);
        wolf_resize(t1, S(44), 2 * S(ROWH));
        wolf_style(t1, WOLF_SP_TEXT_ALIGN, 1, 0);
        wolf_style(t1, WOLF_SP_TEXT_LONG, 0, 0);
        wolf_style(t1, WOLF_SP_TEXT_LINE, S(ROWH) - F(12) - 6, 0);
        fnt(t1, 12); tcol(t1, C_META);
    }

    /* hucreler: panel (tiklanabilir) + icerik etiketi (bosken de var ki
     * hesaptan sonra dogrudan uzerine yazabilelim) */
    for (int r = 0; r < NROW; r++) {
        for (int c = 0; c < NCOL; c++) {
            int x = S(44) + c * S(COLW);
            int y = S(24) + r * S(ROWH);
            wolf_id p = wolf_panel(gp, x, y, S(COLW), S(ROWH));
            bgc(p, C_CELL); bgo(p, 100); rad(p, 0);
            wolf_style(p, WOLF_SP_BORDER_WIDTH, 1, 0);
            wolf_style(p, WOLF_SP_BORDER_COLOR, C_PLINE, 0);
            wolf_set_click(p, 1); hov(p, 0x1A1E26);
            wolf_on_click_ex(p, cell_cb, (void *)(long)(r * NCOL + c));
            g_cell[r][c] = p;

            int al = (g_isnum[r][c] || (c >= 1 && c <= 3)) ? 2 : 0;
            uint32_t col = r == 0 ? C_META : (r == NROW - 1 ? C_TXT : C_INK);
            if (g_errc[r][c]) col = C_ERR;
            g_lab[r][c] = lab(p, S(8), (S(ROWH) - F(12) - 6) / 2, COLW - 16,
                              g_disp[r][c], 12, col, al);
        }
    }

    /* hucrelerden footer'a kadar bos izgara zemini (Excel gri alani) */
    int gbot = gy + S(24) + NROW * S(ROWH);
    wolf_id fill = wolf_panel(g_root, gx, gbot, S(1204),
                              g_h - S(28) - S(32) - gbot);
    bgc(fill, C_CELL); bgo(fill, 100); rad(fill, 0);

    /* secim cercevesi: en ustte, bos zemin, 2 px mavi */
    g_frame = wolf_panel(g_root, 0, 0, S(COLW), S(ROWH));
    bgo(g_frame, 0); rad(g_frame, 0);
    wolf_style(g_frame, WOLF_SP_BORDER_WIDTH, 2, 0);
    wolf_style(g_frame, WOLF_SP_BORDER_COLOR, C_ACC, 0);
    frame_place();
}

/* ---- formul cubugu (30): ad kutusu + fx + girdi + Uygula/temizle ---- */
static void fbar(void)
{
    int y = S(140), h = S(30);
    wolf_id fb = wolf_panel(g_root, 0, y, g_w, h);
    bgc(fb, C_GROUND); rad(fb, 0); bgo(fb, 100); brd_c(fb, 2, C_LINE);

    wolf_id nb = wolf_panel(fb, S(8), S(4), S(56), S(22));
    bgc(nb, C_FIELD); rad(nb, S(4)); bgo(nb, 100); brd_c(nb, 15, 0x333A47);
    g_name = lab(nb, 0, (S(22) - F(12) - 6) / 2, 56, "D2", 12, C_TXT, 1);

    lab(fb, S(72), (h - F(12) - 6) / 2, 26, "fx", 12, C_META, 1);

    /* Uygula ve temizle sagda; aradaki alan girdiye kalir */
    wolf_id ap = wolf_panel(fb, g_w - S(104), S(4), S(70), S(22));
    bgc(ap, 0x24406B); rad(ap, S(4)); bgo(ap, 100); hov(ap, 0x2C4E82);
    wolf_set_click(ap, 1); wolf_on_click(ap, fx_apply);
    lab(ap, 0, (S(22) - F(11) - 6) / 2, 70, "Uygula", 11, 0xDCE6F5, 1);

    wolf_id xr = lab(fb, g_w - S(30), (h - F(13) - 6) / 2, 24, "×", 13, C_META, 1);
    wolf_set_click(xr, 1); hov(xr, 0x262B35); wolf_on_click(xr, fx_clear_cb);

    g_fx = wolf_input(fb, S(102), S(4), g_w - S(102) - S(112), S(22),
                      "deger ya da =formil (Enter ile yazilir)");
    wolf_style(g_fx, WOLF_SP_FONT_PX, F(12), 0);
    wolf_on_change(g_fx, fx_change_cb);
    wolf_on_enter(g_fx, fx_apply);

    wolf_text(g_fx, g_raw[g_sel / NCOL][g_sel % NCOL]);
    g_dirty = 0;
}

/* ---- ribon (70): Dosya (Ac/Kaydet) grubu ---- */
static void ribbon(void)
{
    int y = S(70), h = S(70);
    wolf_id rb = wolf_panel(g_root, 0, y, g_w, h);
    bgc(rb, C_GROUND); rad(rb, 0); bgo(rb, 100); brd_c(rb, 2, C_LINE);

    struct { int x, w; const char *t; wolf_event_fn cb; } btns[2] = {
        { 14, 74, "Ac", do_open }, { 100, 74, "Kaydet", do_save },
    };
    for (int i = 0; i < 2; i++) {
        wolf_id b = wolf_panel(rb, S(btns[i].x), S(10), S(btns[i].w), S(30));
        bgc(b, C_FIELD); rad(b, S(6)); bgo(b, 100);
        brd_c(b, 15, 0x333A47);
        wolf_set_click(b, 1); hov(b, 0x1A1E26); wolf_on_click(b, btns[i].cb);
        lab(b, 0, (S(30) - F(12) - 6) / 2, btns[i].w, btns[i].t, 12, C_TXT, 1);
    }
    lab(rb, 14, h - S(16), 560,
        "Dosya · Duzen · Yazı tipi · Sayı · Hizala · Hucre  (CSV: C:/belgeler)",
        11, 0x7D8491, -1);
}

/* ---- sayfa sekmeleri (32) + durum (28) ---- */
static void footbar(void)
{
    int fy = g_h - S(28) - S(32);
    wolf_id fb = wolf_panel(g_root, 0, fy, g_w, S(32));
    bgc(fb, C_GROUND); rad(fb, 0); bgo(fb, 100);
    lab(fb, 12, (S(32) - F(13) - 6) / 2, 16, "<", 13, C_META, 1);
    const char *sheets[2] = { "Sayfa 1", "Sayfa 2" };
    int x = 40;
    for (int i = 0; i < 2; i++) {
        wolf_id t = lab(fb, x, (S(32) - F(13) - 6) / 2, 90, sheets[i], 13,
                        i == 0 ? C_ACC : C_TAB, 1);
        wolf_set_click(t, 1); wolf_on_click(t, soon);
        if (i == 0) {
            wolf_id u = wolf_panel(fb, x - S(4), S(30), S(90) + S(8), 2);
            bgc(u, C_ACC); bgo(u, 100); rad(u, 0);
        }
        x += S(104);
    }

    wolf_id sb = wolf_panel(g_root, 0, g_h - S(28), g_w, S(28));
    bgc(sb, C_GROUND); rad(sb, 0); bgo(sb, 100); brd_c(sb, 1, C_LINE);
    int ly = (S(28) - F(12) - 6) / 2;
    g_status = lab(sb, 14, ly, 640, "", 12, C_META, -1);
    lab(sb, g_w - S(124), ly, 110, "Hazır", 12, C_META, 2);
    status_update();
}

/* ---- dosya eylemleri ---- */
static void do_open(wolf_id w, void *u)
{
    (void)w; (void)u;
    if (try_open(1)) {
        title_update();
        int r = g_sel / NCOL, c = g_sel % NCOL;
        select_cell(r > g_last_r ? g_last_r : r, c > g_last_c ? g_last_c : c);
        refresh_grid();
    }
}

static void do_save(wolf_id w, void *u)
{
    (void)w; (void)u;
    if (g_dirty) commit_cur();
    static char buf[8192];
    int len = 0;
    csv_build(buf, sizeof buf, &len);
    if (wolf_fs_write(g_file, buf, len) == 0) {
        title_update();
        wolf_notify("Abaküs", g_file);
    } else {
        wolf_notify("Abaküs", "Kaydedilemedi");
    }
}

static void build(void);

static void on_resz(wolf_id w, void *u)
{
    (void)w; (void)u;
    int w0 = g_w, h0 = g_h;
    if (wolf_screen(&g_w, &g_h) != 0) return;
    if (g_w == w0 && g_h == h0) return;   /* boyut ayni: yeniden kurmayiz */
    int a = g_w * 1000 / 1280, b = g_h * 1000 / 720;
    g_s = a < b ? a : b;
    if (g_s < 300) g_s = 300;
    if (g_dirty) commit_cur();            /* yazilmamis fx kaybolmasin */
    wolf_clear(g_root);
    build();
}

static void build(void)
{
    wolf_style(g_root, WOLF_SP_BG_COLOR, C_GROUND, 0);
    wolf_style(g_root, WOLF_SP_BG_OPA, 100, 0);

    titlebar();
    tabstrip();
    ribbon();
    fbar();
    grid();
    footbar();
}

int wolf_main(const wolf_api_t *api)
{
    wolf_bind(api);
    g_root = wolf_root();
    if (wolf_screen(&g_w, &g_h) != 0) { g_w = 1280; g_h = 720; }
    int a = g_w * 1000 / 1280, b = g_h * 1000 / 720;
    g_s = a < b ? a : b;
    if (g_s < 300) g_s = 300;

    const char *arg = wolf_arg();
    int with_file = 0;
    if (arg && arg[0]) {
        scpy(g_file, sizeof g_file, arg);
        with_file = try_open(0);
        if (with_file) scpy(g_title, sizeof g_title, base_name(g_file));
    }
    if (!with_file) {
        seed_default();
        scpy(g_title, sizeof g_title, "Butce 2026 (ornek)");
    }
    recompute();

    wolf_on_resize(on_resz, 0);
    build();
    if (g_sel >= NROW * NCOL) g_sel = 0;
    select_cell(g_sel / NCOL, g_sel % NCOL);
    return 0;
}
