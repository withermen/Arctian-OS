/**
 * Şafak — ofis baslangic ekrani (tam ekran, 0.8)
 *
 * Spec bolum 3.1: 36 px baslik, 72 px sol ray, karsilama + arama, dort kart,
 * son kullanilanlar tablosu. Olcek 1280x720 referansli; cozunurluk degisince
 * her sey bastan cizilir (wolf_on_resize).
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

/* renk kutusu + ortali harf (W/X/P/S/D ikonlari); icerik ortalama elle */
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
static void soon(wolf_id w, void *u)          { (void)w; (void)u; wolf_notify("Ofis", "Bu ozellik v0.8'de yok"); }

/* son kullanilanlar satiri: ad + uc meta kolonu, tiklama uygulamayi acar */
static void row_doc1(wolf_id w, void *u) { (void)w; (void)u; wolf_open_app("murekkep"); }
static void row_xls(wolf_id w, void *u)  { (void)w; (void)u; wolf_open_app("abakus"); }
static void row_ppt(wolf_id w, void *u)  { (void)w; (void)u; wolf_open_app("sahne"); }
static void row_pdf(wolf_id w, void *u)  { (void)w; (void)u; wolf_open_app("mercek"); }

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

static void recent_row(wolf_id box, int tw, int i, const char *letter,
                       uint32_t col, const char *name, const char *kind,
                       const char *when, const char *where, wolf_event_fn cb)
{
    int rh = S(44), y = S(34) + i * rh;
    int rw = tw - 2;
    int ty = (rh - S(12)) / 2;
    if (i > 0) {
        wolf_id d = wolf_panel(box, 0, y, rw, 1);
        bgc(d, C_DIV); rad(d, 0); bgo(d, 100);
    }
    wolf_id r = wolf_panel(box, 0, y, rw, rh);
    bgo(r, 0); rad(r, 0); wolf_set_click(r, 1); hov(r, 0x1F232B);
    wolf_on_click(r, cb);
    letter_box(r, S(14), (rh - S(24)) / 2, 24, letter, col, 10);
    lab(r, S(14) + S(34), (rh - F(13) - 6) / 2, 320, name, 13, C_TXT, -1);
    int t1 = rw - S(420);
    lab(r, t1, ty, 110, kind, 12, C_META, -1);
    lab(r, t1 + S(120), ty, 140, when, 12, C_META, -1);
    lab(r, t1 + S(270), ty, 140, where, 12, C_META, -1);
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
        { "D", C_PURP,   soon },
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
    lab(main, 0, S(24), aw - S(440), "Merhaba, Kerem", 24, C_TXT, -1);
    lab(main, 0, S(24) + S(36), aw - S(440), "Bugun ne uzerinde calisiyoruz?", 12, C_META, -1);
    wolf_id srch = wolf_panel(main, aw - S(420), S(24), S(420), S(36));
    bgc(srch, C_FIELD); rad(srch, S(8)); bgo(srch, 100);
    brd_c(srch, 15, C_LINE);
    wolf_set_click(srch, 1); wolf_on_click(srch, soon);
    lab(srch, S(12), (S(36) - F(13) - 6) / 2, 396, "Belge, tablo, sunum veya PDF ara", 13, C_PAS, -1);

    /* dort kart: 118 yukseklik, 10 yari cap */
    int cy = S(24) + S(36) + S(22);
    int gap = S(12);
    int cw = (aw - 3 * gap) / 4;
    struct { const char *ch, *t1, *t2; uint32_t c; wolf_event_fn cb; } cards[4] = {
        { "W", "Boş belge", "Word tarzi metin",       C_BLUE,   open_murekkep },
        { "X", "Boş tablo", "Hucreli hesap sayfasi",  C_GREEN,  open_abakus },
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
    wolf_id see = lab(main, 0, ry + S(2), 110, "Tumunu gor", 12, C_META, 2);
    wolf_move(see, aw - S(110), ry + S(2));
    wolf_set_click(see, 1); wolf_on_click(see, soon);

    int ty = ry + S(15) + S(8);
    wolf_id box = wolf_panel(main, 0, ty, aw, S(34) + 5 * S(44) + S(12));
    bgc(box, C_CARD); rad(box, S(10)); bgo(box, 100);
    brd_c(box, 15, C_PLINE);

    wolf_id hd = wolf_panel(box, 1, 1, aw - 2, S(34));
    bgo(hd, 0); rad(hd, 0);
    brd_c(hd, 2, C_PLINE);
    int t1 = aw - 2 - S(420);
    lab(hd, S(14), (S(34) - F(12) - 6) / 2, 220, "Ad", 12, C_META, -1);
    lab(hd, t1, (S(34) - F(12) - 6) / 2, 110, "Tur", 12, C_META, -1);
    lab(hd, t1 + S(120), (S(34) - F(12) - 6) / 2, 140, "Degistirildi", 12, C_META, -1);
    lab(hd, t1 + S(270), (S(34) - F(12) - 6) / 2, 140, "Konum", 12, C_META, -1);

    recent_row(box, aw, 0, "B", C_BLUE,   "Proje raporu.doc",     "Belge", "Bugun 14:20", "Belgeler",     row_doc1);
    recent_row(box, aw, 1, "T", C_GREEN,  "Butce 2026.xls",       "Tablo", "Dun 17:05",   "Belgeler",     row_xls);
    recent_row(box, aw, 2, "S", C_ORANGE, "Lansman sunumu.ppt",   "Sunum", "3 Kas",       "Masaustu",     row_ppt);
    recent_row(box, aw, 3, "P", C_RED,    "rapor-2026.pdf",       "PDF",   "31 Eki",      "Indirilenler", row_pdf);
    recent_row(box, aw, 4, "B", C_BLUE,   "Toplanti notlari.doc", "Belge", "28 Eki",      "Belgeler",     row_doc1);
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
    build();
    return 0;
}
