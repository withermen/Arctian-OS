/**
 * Safak — ofis baslangic ekrani (tam ekran, 0.9)
 *
 * Spec bolum 3.1: 36 px baslik, 72 px sol ray, karsilama + arama, dort kart,
 * son kullanilanlar tablosu. Olcek 1280x720 referansli; cozunurluk degisince
 * her sey bastan cizilir (wolf_on_resize).
 *
 * 0.9 gerceklik: son kullanilanlar listesi artik sabit degil. Acilan her
 * belge kendi listemizde (C:/belgeler/son.txt, yoksa C:/son.txt) tutulur;
 * AFS'de degisim tarihi olmadigi icin satir "zaman|yol" bicimindedir ve
 * zaman kullanim anindaki saatten uretilir. Liste bosken
 * Belgeler/Masaustu/Indirilenler taranir. Satir tiklaninca belge ilgili
 * ofis uygulamasiyla acilir (wolf_open_app_file). Arama kutusu gercek
 * girdidir; yazildikca listeyi suzer.
 */
#include "wolf.h"

/* spec paleti */
#define C_GROUND 0x14161B   /* uygulama zemini */
#define C_LINE   0x2C313B   /* ana kenarlik */
#define C_PLINE  0x262B35   /* panel ici kenarlik */
#define C_DIV    0x1F232B   /* ayirici */
#define C_CARD   0x181B22   /* kart zemini */
#define C_FIELD  0x0F1115   /* girdi zemini */
#define C_SEL    0x1E2129   /* secili zemin */
#define C_ACC    0x3B82F6   /* vurgu */
#define C_TXT    0xE8EAEE
#define C_TXT2   0xC9CED8
#define C_META   0x8B919C
#define C_PAS    0x7D8491
#define C_BLUE   0x2B5FD9   /* belge */
#define C_GREEN  0x1F7A4D   /* tablo */
#define C_ORANGE 0xC2410C   /* sunum */
#define C_RED    0xDC2626   /* pdf */
#define C_PURP   0x6D28D9   /* ofis rayi */

/* pencere kenar butonlari (10 px) */
#define SVG_MIN   "<svg viewBox=\"0 0 10 10\"><path d=\"M1 5h8\" stroke=\"#8b919c\" stroke-width=\"1\" fill=\"none\"/></svg>"
#define SVG_MAX   "<svg viewBox=\"0 0 10 10\"><rect x=\"1.5\" y=\"1.5\" width=\"7\" height=\"7\" stroke=\"#8b919c\" stroke-width=\"1\" fill=\"none\"/></svg>"
#define SVG_CLOSE "<svg viewBox=\"0 0 10 10\"><path d=\"M1.5 1.5l7 7M8.5 1.5l-7 7\" stroke=\"#8b919c\" stroke-width=\"1.1\" fill=\"none\"/></svg>"

#define MAXR 8
#define PLEN 160            /* yol tamponu */
#define RBUF (MAXR * (PLEN + 24) + 16)

static int g_w, g_h, g_s;   /* tuval + permille olcek (1280x720 = 1000) */
static wolf_id g_root;

static int S(int v) { return (v * g_s + 500) / 1000; }
static int F(int px)
{
    int v = (px * g_s + 500) / 1000;
    if (v < 6) v = 6;
    return (v + 1) / 2 * 2;   /* 2 px kademe: font onbellegi dolmasin */
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

/* sabit genislikli etiket; al: 0 sol 1 orta 2 sag (-1 = dokunma) */
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

/* renk kutusu + ortali harf (W/X/P/S ikonlari); icerik ortalama elle */
static wolf_id letter_box(wolf_id p, int x, int y, int box, const char *ch,
                          uint32_t bg, int fpx)
{
    wolf_id b = wolf_panel(p, x, y, S(box), S(box));
    bgc(b, bg); rad(b, 8); bgo(b, 100);
    int lh = F(fpx) + 4;
    wolf_id l = wolf_label(b, 0, (S(box) - lh) / 2, ch);
    wolf_resize(l, S(box), lh);
    wolf_style(l, WOLF_SP_TEXT_ALIGN, 1, 0);
    wolf_style(l, WOLF_SP_TEXT_LONG, 1, 0);
    fnt(l, fpx); tcol(l, 0xFFFFFF);
    return b;
}

/* ---- baslik cubugu (36 px) + kenar dugmeleri ---- */
static void cb_min(wolf_id w, void *u)   { (void)w; (void)u; wolf_win_min(); }
static void cb_max(wolf_id w, void *u)   { (void)w; (void)u; wolf_notify("Ofis", "Zaten tam ekrandasin"); }
static void cb_close(wolf_id w, void *u) { (void)w; (void)u; wolf_win_close(); }

static void winbtn(int x, const char *svg, wolf_event_fn cb, uint32_t hover)
{
    wolf_id b = wolf_panel(g_root, x, 0, S(46), S(36));
    bgo(b, 0); rad(b, 0); wolf_set_click(b, 1); hov(b, hover);
    wolf_icon(b, (S(46) - S(10)) / 2, (S(36) - S(10)) / 2, S(10), svg);
    wolf_on_click(b, cb);
}

static void titlebar(const char *text)
{
    wolf_id tb = wolf_panel(g_root, 0, 0, g_w, S(36));
    bgc(tb, C_GROUND); rad(tb, 0); bgo(tb, 100);
    brd_c(tb, 2, C_LINE);
    lab(tb, 12, (S(36) - F(12) - 6) / 2, 480, text, 12, C_TXT2, -1);
    winbtn(g_w - S(46) * 3,      SVG_MIN,   cb_min,   C_PLINE);
    winbtn(g_w - S(46) * 2,      SVG_MAX,   cb_max,   C_PLINE);
    winbtn(g_w - S(46),          SVG_CLOSE, cb_close, 0xC42B1C);
}

/* ---- uygulama gecisleri ---- */
static void open_murekkep(wolf_id w, void *u) { (void)w; (void)u; wolf_open_app("murekkep"); }
static void open_abakus(wolf_id w, void *u)   { (void)w; (void)u; wolf_open_app("abakus"); }
static void open_sahne(wolf_id w, void *u)    { (void)w; (void)u; wolf_open_app("sahne"); }
static void open_mercek(wolf_id w, void *u)   { (void)w; (void)u; wolf_open_app("mercek"); }

/* ---- tur sozlesmesi: uzanti -> uygulama / etiket / renk ---- */
enum { K_DOC = 0, K_TAB, K_PPT, K_PDF, K_NUM };
static const char *K_APP[K_NUM]    = { "murekkep", "abakus", "sahne", "mercek" };
static const char *K_LET[K_NUM]    = { "W", "X", "S", "P" };
static const char *K_NAME[K_NUM]   = { "Belge", "Tablo", "Sunum", "PDF" };
static const uint32_t K_COL[K_NUM] = { C_BLUE, C_GREEN, C_ORANGE, C_RED };

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int ci_eq(const char *a, const char *b)
{
    while (*a && *b) { if (lc(*a) != lc(*b)) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}
/* yolun sonundaki uzantiya gore tur (bilinmeyen -> K_DOC) */
static int kind_of(const char *p)
{
    const char *base = p, *dot = 0;
    for (const char *s = p; *s; s++) if (*s == '/') base = s + 1;
    for (const char *s = base; *s; s++) if (*s == '.') dot = s;
    if (!dot) return K_DOC;
    if (ci_eq(dot, ".doc") || ci_eq(dot, ".txt") || ci_eq(dot, ".md") ||
        ci_eq(dot, ".rtf")) return K_DOC;
    if (ci_eq(dot, ".xls") || ci_eq(dot, ".csv")) return K_TAB;
    if (ci_eq(dot, ".ppt")) return K_PPT;
    if (ci_eq(dot, ".pdf")) return K_PDF;
    return K_DOC;
}
static const char *base_of(const char *p)
{
    const char *b = p;
    for (const char *s = p; *s; s++) if (*s == '/') b = s + 1;
    return b;
}
/* C:/belgeler/x -> "belgeler"; C:/x -> "C:" */
static void place_of(const char *p, char *out, int cap)
{
    const char *c = p;
    if (c[0] && c[1] == ':') c += 2;
    while (*c == '/') c++;
    int i = 0;
    while (c[i] && c[i] != '/') i++;
    if (c[i] == '/' && c[i + 1] && i < cap - 1) {   /* altina indigimiz ad var */
        for (int k = 0; k < i; k++) out[k] = c[k];
        out[i] = 0;
    } else {
        wolf_strcpy(out, cap, "C:");
    }
}

/* ---- son kullanilanlar listesi ----
 * AFS'de degisim zamani yok; kullanim anini kendimiz yazariz.
 * Satir bicimi: "<GG.AA SS:MM>|<yol>" (zamansiz satir da kabul). */
static char g_path[MAXR][PLEN];
static char g_when[MAXR][20];
static int  g_kind[MAXR];
static int  g_n;
static int  g_view[MAXR];       /* suzme sonrasi gorunen satirlar */
static int  g_vn;
static int  g_showall;
static char g_query[64];
static wolf_id g_box, g_list;   /* kusruk: g_box zemin+baslik, g_list satirlar */
static int g_list_w;

/* "C:/belgeler/son.txt" once; yazma ilk yazisinda dizini kendisi yaratir.
 * Dirus "C:/son.txt". Okuma hangisini bulduysa yazma da oraya gider. */
#define REC1 "C:/belgeler/son.txt"
#define REC2 "C:/son.txt"
static const char *s_recpath = REC1;
static int s_rec2 = 0;              /* dusus yolu kullaniliyor mu */

static void two(int v, char *d)
{
    d[0] = (char)('0' + (v / 10) % 10);
    d[1] = (char)('0' + v % 10);
    d[2] = 0;
}
/* su ani "GG.AA SS:MM" yap; saat yoksa "-". */
static void stamp(char *out, int cap)
{
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (wolf_now(&y, &mo, &d, &h, &mi, &s) != 0) { wolf_strcpy(out, cap, "-"); return; }
    char a[4], b[4], c[4], m[4];
    two(d, a); two(mo, b); two(h, c); two(mi, m);
    wolf_strcpy(out, cap, a); wolf_strcat(out, cap, ".");
    wolf_strcat(out, cap, b); wolf_strcat(out, cap, " ");
    wolf_strcat(out, cap, c); wolf_strcat(out, cap, ":");
    wolf_strcat(out, cap, m);
}

static int add_entry(const char *path, const char *when)
{
    if (!path[0] || path[0] == '#') return 0;      /* yorum/bos satir */
    for (int i = 1; i < g_n; i++)                  /* tekrar: basi kaydir, aradakini sil */
        if (wolf_streq(g_path[i], path)) {
            for (int k = i; k > 0; k--) {
                wolf_strcpy(g_path[k], PLEN, g_path[k - 1]);
                wolf_strcpy(g_when[k], 20, g_when[k - 1]);
                g_kind[k] = g_kind[k - 1];
            }
            wolf_strcpy(g_when[0], 20, when);
            return 1;
        }
    if (wolf_streq(g_path[0], path)) { wolf_strcpy(g_when[0], 20, when); return 1; }
    if (g_n >= MAXR) return 0;
    for (int k = g_n; k > 0; k--) {
        wolf_strcpy(g_path[k], PLEN, g_path[k - 1]);
        wolf_strcpy(g_when[k], 20, g_when[k - 1]);
        g_kind[k] = g_kind[k - 1];
    }
    wolf_strcpy(g_path[0], PLEN, path);
    wolf_strcpy(g_when[0], 20, when);
    g_kind[0] = kind_of(path);
    g_n++;
    return 1;
}

/* bas harfleri kuculterek iceriyor mu (ASCII) */
static int ci_contains(const char *hay, const char *needle)
{
    if (!needle[0]) return 1;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && lc(*a) == lc(*b)) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

/* g_view'daki satirlari g_query'e gore say: arama kucuk harfle gelir */
static void refilter(void)
{
    g_vn = 0;
    int lim = g_showall ? g_n : (g_n < 5 ? g_n : 5);
    for (int i = 0; i < g_n && g_vn < MAXR; i++) {
        if (!g_query[0]) {
            g_view[g_vn++] = i;
            if (g_vn >= lim) break;
            continue;
        }
        /* arama ad parcasi ile sinirli (query kucuk harfe cevrilmis gelir) */
        char low[PLEN];
        int k = 0;
        const char *bn = base_of(g_path[i]);
        for (; bn[k] && k < PLEN - 1; k++) low[k] = lc(bn[k]);
        low[k] = 0;
        if (ci_contains(low, g_query)) g_view[g_vn++] = i;
    }
}

static void list_save(void)
{
    static char buf[RBUF];
    int n = 0;
    for (int i = 0; i < g_n; i++) {
        char line[PLEN + 32];
        wolf_strcpy(line, sizeof(line), g_when[i]);
        wolf_strcat(line, sizeof(line), "|");
        wolf_strcat(line, sizeof(line), g_path[i]);
        wolf_strcat(line, sizeof(line), "\n");
        int k = 0;
        while (line[k] && n < RBUF - 1) buf[n++] = line[k++];
    }
    if (wolf_fs_write(s_recpath, buf, n) != 0 && !s_rec2) {
        s_rec2 = 1;
        s_recpath = REC2;
        wolf_fs_write(s_recpath, buf, n);
    }
}

/* once liste dosyasini oku; bos ise klasorleri tara (zamansiz giris) */
static void list_load(void)
{
    static char buf[RBUF];
    g_n = 0;
    int n = wolf_fs_read(s_recpath, buf, RBUF - 1);
    if (n <= 0) {
        /* REC1 bos/okunamadi: REC2'de içerik var MI, ona bak. Ikisi de
         * bos ise REC1'de kalalim — yazma sirasi dizini kendisi yaratir;
         * REC2 yalniz icerik orada bulunursa kalici hedef olur. */
        int n2 = wolf_fs_read(REC2, buf, RBUF - 1);
        if (n2 > 0) {
            s_rec2 = 1;
            s_recpath = REC2;
            n = n2;
        }
    }
    if (n > 0) {
        buf[n] = 0;
        int start = 0;
        for (int i = 0; i <= n; i++) {
            if (buf[i] != '\n' && i != n) continue;
            buf[i] = 0;
            char *line = buf + start;
            char *bar = line;
            while (*bar && *bar != '|') bar++;
            if (*bar == '|') {
                *bar = 0;
                if (wolf_fs_exists(bar + 1)) add_entry(bar + 1, line);
            } else if (wolf_fs_exists(line)) {
                add_entry(line, "");
            }
            start = i + 1;
        }
    }
    if (g_n == 0) {
        /* tarama: gercek dosyalar, zamansiz; tiklaninca listeye yerlesir */
        static const char *dirs[3] = { "C:/belgeler", "C:/projler", "C:/indirilenler" };
        char nm[PLEN];
        for (int d = 0; d < 3 && g_n < MAXR; d++) {
            for (int i = 0; g_n < MAXR; i++) {
                int isdir = 0;
                if (wolf_fs_list(dirs[d], i, nm, sizeof(nm), &isdir) != 0) break;
                if (isdir) continue;
                char full[PLEN + 24];
                wolf_strcpy(full, sizeof(full), dirs[d]);
                wolf_strcat(full, sizeof(full), "/");
                wolf_strcat(full, sizeof(full), nm);
                add_entry(full, "");
            }
        }
    }
}

/* son kullanilanlar satiri (g_list icinde; baslik ofseti cagiran verir) */
static void row_cb(wolf_id w, void *u);
static void recent_row(wolf_id box, int tw, int y, int kind, const char *name,
                       const char *when, const char *where, int view_i)
{
    int rh = S(44), rw = tw - 2;
    int ty = (rh - S(12)) / 2;
    wolf_id r = wolf_panel(box, 0, y, rw, rh);
    bgo(r, 0); rad(r, 0); wolf_set_click(r, 1); hov(r, 0x1F232B);
    wolf_on_click_ex(r, row_cb, (void *)(long)view_i);
    letter_box(r, S(14), (rh - S(24)) / 2, 24, K_LET[kind], K_COL[kind], 10);
    lab(r, S(14) + S(34), (rh - F(13) - 6) / 2, 320, name, 13, C_TXT, -1);
    int t1 = rw - S(420);
    lab(r, t1, ty, 110, K_NAME[kind], 12, C_META, -1);
    lab(r, t1 + S(120), ty, 140, when && when[0] ? when : "-", 12, C_META, -1);
    lab(r, t1 + S(270), ty, 140, where, 12, C_META, -1);
}

/* gorunen satirlari yeniden ciz (yalniz g_list; baslik g_box'ta kalir) */
static void list_redraw(void)
{
    if (!g_list) return;
    wolf_clear(g_list);
    int rows = g_vn;
    int bh = S(34) + (rows > 0 ? rows : 1) * S(44) + S(12);
    wolf_resize(g_box, g_list_w, bh);
    wolf_resize(g_list, g_list_w - 2, bh - S(34) - 2);
    if (rows == 0) {
        lab(g_list, S(14), S(12), g_list_w - S(28),
            g_query[0] ? "Sonuc yok" : "Henuz belge acilmadi - kartlardan baslayin",
            13, C_PAS, -1);
        return;
    }
    char place[32];
    for (int i = 0; i < rows; i++) {
        int r = g_view[i];
        if (i > 0) {
            wolf_id d = wolf_panel(g_list, 0, i * S(44), g_list_w - 2, 1);
            bgc(d, C_DIV); rad(d, 0); bgo(d, 100);
        }
        place_of(g_path[r], place, sizeof(place));
        recent_row(g_list, g_list_w, i * S(44), g_kind[r],
                   base_of(g_path[r]), g_when[r], place, i);
    }
}

/* ---- satir eylemi: listeye isle, kaydet, uygulamayla ac ---- */
static void row_cb(wolf_id w, void *u)
{
    (void)w;
    int vi = (int)(long)u;
    if (vi < 0 || vi >= g_vn) return;
    int i = g_view[vi];
    char keep[PLEN];
    int kind = g_kind[i];
    wolf_strcpy(keep, PLEN, g_path[i]);
    char tm[20];
    stamp(tm, sizeof(tm));
    add_entry(keep, tm);
    list_save();
    refilter();
    list_redraw();
    wolf_open_app_file(K_APP[kind], keep);
}

/* ---- arama ---- */
static wolf_id g_search;
static void to_low(const char *s, char *out, int cap)
{
    int i = 0;
    for (; s[i] && i < cap - 1; i++) out[i] = lc(s[i]);
    out[i] = 0;
}
static void search_cb(wolf_id w, void *u)
{
    (void)w; (void)u;
    char raw[64];
    wolf_text_of(g_search, raw, sizeof(raw));
    to_low(raw, g_query, sizeof(g_query));
    refilter();
    list_redraw();
}

/* ---- Tumunu gor / Daralt ---- */
static wolf_id g_see;
static void see_cb(wolf_id w, void *u)
{
    (void)w; (void)u;
    g_showall = !g_showall;
    wolf_text(g_see, g_showall ? "Daralt" : "Tumunu gor");
    refilter();
    list_redraw();
}

static void build(void);

/* cozunurluk degisti: yeniden olcek, yeniden ciz (her ekranda kalite) */
static void on_resz(wolf_id w, void *u)
{
    (void)w; (void)u;
    if (wolf_screen(&g_w, &g_h) != 0) return;
    int a = g_w * 1000 / 1280, b = g_h * 1000 / 720;
    g_s = a < b ? a : b;
    if (g_s < 300) g_s = 300;
    wolf_clear(g_root);
    build();
}

/* liste kutusu: zemin + baslik g_box'ta, satirlar g_list'te (temizlik
 * basliga dokunmasin diye ayri panel) */
static void list_area(wolf_id main, int aw, int ty)
{
    g_list_w = aw;
    g_box = wolf_panel(main, 0, ty, aw, S(34) + 5 * S(44) + S(12));
    bgc(g_box, C_CARD); rad(g_box, S(10)); bgo(g_box, 100);
    brd_c(g_box, 15, C_PLINE);

    wolf_id hd = wolf_panel(g_box, 1, 1, aw - 2, S(34));
    bgo(hd, 0); rad(hd, 0);
    brd_c(hd, 2, C_PLINE);
    int t1 = aw - 2 - S(420);
    lab(hd, S(14), (S(34) - F(12) - 6) / 2, 220, "Ad", 12, C_META, -1);
    lab(hd, t1, (S(34) - F(12) - 6) / 2, 110, "Tur", 12, C_META, -1);
    lab(hd, t1 + S(120), (S(34) - F(12) - 6) / 2, 140, "Son kullanim", 12, C_META, -1);
    lab(hd, t1 + S(270), (S(34) - F(12) - 6) / 2, 140, "Konum", 12, C_META, -1);

    g_list = wolf_panel(g_box, 1, S(34) + 1, aw - 2, 4 * S(44) + S(12));
    bgo(g_list, 0); rad(g_list, 0);
    refilter();
    list_redraw();
}

static void build(void)
{
    wolf_style(g_root, WOLF_SP_BG_COLOR, C_GROUND, 0);
    wolf_style(g_root, WOLF_SP_BG_OPA, 100, 0);

    titlebar("Ofis");

    /* sol ray: 72 px, sagda 1 px ayirici; 44x44 kutular 10 px aralikla */
    wolf_id rail = wolf_panel(g_root, 0, S(36), S(72), g_h - S(36));
    bgc(rail, C_GROUND); rad(rail, 0); bgo(rail, 100);
    brd_c(rail, 8, C_PLINE);
    struct { const char *ch; uint32_t c; wolf_event_fn cb; } apps[4] = {
        { "W", C_BLUE,   open_murekkep },
        { "X", C_GREEN,  open_abakus },
        { "P", C_ORANGE, open_sahne },
        { "D", C_PURP,   open_mercek },
    };
    for (int i = 0; i < 4; i++) {
        int y = S(36) + S(14) + i * (S(44) + S(10));
        wolf_id b = letter_box(rail, (S(72) - S(44)) / 2, y, 44,
                               apps[i].ch, apps[i].c, 13);
        wolf_set_click(b, 1);
        wolf_on_click(b, apps[i].cb);
    }

    /* ana alan */
    int mx = S(72) + S(32);
    int aw = g_w - mx - S(32);
    wolf_id main = wolf_panel(g_root, mx, S(36), aw, g_h - S(36));
    bgc(main, C_GROUND); rad(main, 0); bgo(main, 100);
    wolf_scrollable(main, 1);

    /* karsilama + arama (420x36 sagda) */
    lab(main, 0, S(24), aw - S(440), "Merhaba", 24, C_TXT, -1);
    lab(main, 0, S(24) + S(36), aw - S(440), "Bugun ne uzerinde calisiyoruz?", 12, C_META, -1);
    g_search = wolf_input(main, aw - S(420), S(24), S(420), S(36),
                          "Belge, tablo, sunum veya PDF ara");
    wolf_on_change(g_search, search_cb);

    /* dort kart: 118 yukseklik, 10 yari cap */
    int cy = S(24) + S(36) + S(22);
    int gap = S(12);
    int cw = (aw - 3 * gap) / 4;
    struct { const char *ch, *t1, *t2; uint32_t c; wolf_event_fn cb; } cards[4] = {
        { "W", "Boş belge", "Word tarzi metin",      C_BLUE,   open_murekkep },
        { "X", "Boş tablo", "Hucreli hesap sayfasi", C_GREEN,  open_abakus },
        { "P", "Boş sunum", "Slaytlarla anlatin",    C_ORANGE, open_sahne },
        { "PDF", "PDF ac",  "Dosya sec ya da surukle", C_RED,  open_mercek },
    };
    for (int i = 0; i < 4; i++) {
        wolf_id c = wolf_panel(main, i * (cw + gap), cy, cw, S(118));
        bgc(c, C_CARD); rad(c, S(10)); bgo(c, 100);
        brd_c(c, 15, C_PLINE);
        wolf_set_click(c, 1); hov(c, C_SEL);
        wolf_on_click(c, cards[i].cb);
        letter_box(c, S(16), S(16), 36, cards[i].ch, cards[i].c, 13);
        lab(c, S(16), S(118) - S(16) - S(34), cw - S(32), cards[i].t1, 13, C_TXT, -1);
        lab(c, S(16), S(118) - S(16) - S(17), cw - S(32), cards[i].t2, 12, C_META, -1);
    }

    /* son kullanilanlar */
    int ry = cy + S(118) + S(22);
    lab(main, 0, ry, 220, "Son kullanılanlar", 15, C_TXT, -1);
    g_see = lab(main, 0, ry + S(2), 110, g_showall ? "Daralt" : "Tumunu gor", 12, C_META, 2);
    wolf_move(g_see, aw - S(110), ry + S(2));
    wolf_set_click(g_see, 1); wolf_on_click(g_see, see_cb);

    int ty = ry + S(15) + S(8);
    list_area(main, aw, ty);
}

int wolf_main(const wolf_api_t *api)
{
    wolf_bind(api);
    g_root = wolf_root();
    if (wolf_screen(&g_w, &g_h) != 0) { g_w = 1280; g_h = 720; }
    int a = g_w * 1000 / 1280, b = g_h * 1000 / 720;
    g_s = a < b ? a : b;
    if (g_s < 300) g_s = 300;
    list_load();
    wolf_on_resize(on_resz, 0);
    build();
    return 0;
}
