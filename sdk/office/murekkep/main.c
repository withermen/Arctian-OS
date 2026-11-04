/**
 * Muirekep — belge editoru (tam ekran, 0.8)
 *
 * Spec bolum 3.2 + 2.1: 36 px baslik, 34 px sekme seridi, 92 px ribbon
 * (Pano / Yazi tipi / Paragraf ve stiller / Gorunüm), gri zemin uzerinde
 * 620x820 beyaz sayfa, 28 px durum cubugu. Gercek docx ayristirma v0.8
 * kapsami disinda; ornek icerik specten gelir. Olcek 1280x720 referans.
 */
#include "wolf.h"

#define C_GROUND 0x14161B
#define C_LINE   0x2C313B
#define C_PLINE  0x262B35
#define C_BODY   0x0B0C0F   /* sayfa cevresi */
#define C_ACC    0x3B82F6
#define C_ACC_D  0x2E6FD6
#define C_TXT    0xE8EAEE
#define C_TXT2   0xC9CED8
#define C_META   0x8B919C
#define C_TAB    0xA3A8B3
#define C_FIELD  0x0F1115
#define C_BORD2  0x333A47
#define C_GL     0x7D8491   /* grup etiketi */
#define C_PAGE   0xFBFBFC   /* sayfa zemini */
#define C_INK    0x1A1D24   /* sayfa metni */
#define C_SUB    0x6B717D
#define C_PARA   0x2A2F38
#define C_BAR    0xD4D8DF   /* yer tutucu cubuk */

#define SVG_MIN   "<svg viewBox=\"0 0 10 10\"><path d=\"M1 5h8\" stroke=\"#8b919c\" stroke-width=\"1\" fill=\"none\"/></svg>"
#define SVG_MAX   "<svg viewBox=\"0 0 10 10\"><rect x=\"1.5\" y=\"1.5\" width=\"7\" height=\"7\" stroke=\"#8b919c\" stroke-width=\"1\" fill=\"none\"/></svg>"
#define SVG_CLOSE "<svg viewBox=\"0 0 10 10\"><path d=\"M1.5 1.5l7 7M8.5 1.5l-7 7\" stroke=\"#8b919c\" stroke-width=\"1.1\" fill=\"none\"/></svg>"

/* hizalama ikonlari (16 px) — spec eksigi "≡" yerine cizgi ikon */
#define I_AL_L "<svg viewBox=\"0 0 24 24\"><path d=\"M3 6h18M3 10.5h12M3 15h18M3 19.5h12\" stroke=\"#a3a8b3\" stroke-width=\"1.8\" fill=\"none\" stroke-linecap=\"round\"/></svg>"
#define I_AL_C "<svg viewBox=\"0 0 24 24\"><path d=\"M3 6h18M6 10.5h12M3 15h18M6 19.5h12\" stroke=\"#a3a8b3\" stroke-width=\"1.8\" fill=\"none\" stroke-linecap=\"round\"/></svg>"
#define I_AL_R "<svg viewBox=\"0 0 24 24\"><path d=\"M3 6h18M9 10.5h12M3 15h18M9 19.5h12\" stroke=\"#a3a8b3\" stroke-width=\"1.8\" fill=\"none\" stroke-linecap=\"round\"/></svg>"
#define I_AL_J "<svg viewBox=\"0 0 24 24\"><path d=\"M3 6h18M3 10.5h18M3 15h18M3 19.5h18\" stroke=\"#a3a8b3\" stroke-width=\"1.8\" fill=\"none\" stroke-linecap=\"round\"/></svg>"

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

/* sar sarma paragraf (etiket yuksekligi satir sayisindan hesaplanir) */
static wolf_id para(wolf_id p, int x, int y, int wpx, const char *t,
                    int px, uint32_t col, int lines)
{
    wolf_id l = wolf_label(p, x, y, t);
    wolf_resize(l, S(wpx), (F(px) + 4) * lines + 6);
    wolf_style(l, WOLF_SP_TEXT_LONG, 0, 0);      /* sar */
    fnt(l, px); tcol(l, col);
    wolf_style(l, WOLF_SP_TEXT_LINE, F(px) * 7 / 17, 0);
    return l;
}

/* 28 px ribon dugmesi; primary = mavi dolgu */
static wolf_id btn28(wolf_id p, int x, int y, int w, const char *t, int primary)
{
    wolf_id b = wolf_panel(p, S(x), S(y), S(w), S(28));
    rad(b, S(6)); bgo(b, 100);
    if (primary) { bgc(b, C_ACC); hov(b, C_ACC_D); }
    else { bgc(b, C_FIELD); brd_c(b, 15, C_BORD2); hov(b, 0x1A1E26); }
    wolf_set_click(b, 1);
    wolf_id l = wolf_label(b, 0, (S(28) - F(13) - 6) / 2, t);
    wolf_resize(l, S(w), F(13) + 6);
    wolf_style(l, WOLF_SP_TEXT_ALIGN, 1, 0);
    wolf_style(l, WOLF_SP_TEXT_LONG, 1, 0);
    fnt(l, 13); tcol(l, primary ? 0xFFFFFF : C_TXT);
    return b;
}

static void soon(wolf_id w, void *u)
{
    (void)w; (void)u;
    wolf_notify("Mürekkep", "Duzenleme v0.8'de yok — gorunum surumu");
}

/* ---- baslik cubugu ---- */
static void cb_min(wolf_id w, void *u)   { (void)w; (void)u; wolf_win_min(); }
static void cb_max(wolf_id w, void *u)   { (void)w; (void)u; wolf_notify("Mürekkep", "Zaten tam ekrandasin"); }
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
    bgc(tb, C_GROUND); rad(tb, 0); bgo(tb, 100); brd_c(tb, 2, C_LINE);
    lab(tb, 12, (36 - 12) / 2, 560, text, 12, C_TXT2, -1);
    winbtn(g_w - S(46) * 3,  SVG_MIN,   cb_min,   0x262B35);
    winbtn(g_w - S(46) * 2,  SVG_MAX,   cb_max,   0x262B35);
    winbtn(g_w - S(46),      SVG_CLOSE, cb_close, 0xC42B1C);
}

/* ---- sekme seridi (34 px); secili: acik metin + 2 px mavi alt cizgi ---- */
static void tabstrip(void)
{
    wolf_id st = wolf_panel(g_root, 0, S(36), g_w, S(34));
    bgc(st, C_GROUND); rad(st, 0); bgo(st, 100); brd_c(st, 2, C_LINE);
    const char *tabs[6] = { "Dosya", "Giris", "Ekle", "Duzen", "Basvurular", "Gorunum" };
    int on = 1;   /* Giriş secili (spec) */
    int x = 12;
    for (int i = 0; i < 6; i++) {
        int tw = wolf_strlen(tabs[i]) * 7 + 4;
        wolf_id t = lab(st, x, (34 - 13) / 2 - 1, tw + 16, tabs[i], 13,
                        i == on ? C_TXT : C_TAB, 1);
        wolf_set_click(t, 1); wolf_on_click(t, soon);
        if (i == on) {
            wolf_id u = wolf_panel(st, x - S(8) + S(8), S(32), S(tw + 16), 2);
            bgc(u, C_ACC); bgo(u, 100); rad(u, 0);
        }
        x += S(tw + 16);
    }
}

/* ---- ribbon (92 px) ---- */
static void ribbon(void)
{
    int y = S(70), h = S(92);
    wolf_id rb = wolf_panel(g_root, 0, y, g_w, h);
    bgc(rb, C_GROUND); rad(rb, 0); bgo(rb, 100); brd_c(rb, 2, C_LINE);

    /* grup cercevesi: sagda 1px ayirici; altta 11 px grup etiketi */
    struct { int x, w; const char *name; } grps[4] = {
        { 12, 70, "Pano" }, { 82, 200, "Yazi tipi" },
        { 282, 220, "Paragraf ve stiller" }, { 0, 130, "Gorunum" },
    };
    grps[3].x = 1280 - 12 - 130;
    for (int i = 0; i < 4; i++) {
        wolf_id g = wolf_panel(rb, S(grps[i].x), 1, S(grps[i].w), h - 2);
        bgo(g, 0); rad(g, 0);
        if (i < 3) brd_c(g, 8, C_PLINE);
        wolf_id gl = wolf_label(g, 0, h - S(8) - F(11) - 4, grps[i].name);
        wolf_resize(gl, S(grps[i].w), F(11) + 4);
        wolf_style(gl, WOLF_SP_TEXT_ALIGN, 1, 0);
        wolf_style(gl, WOLF_SP_TEXT_LONG, 1, 0);
        fnt(gl, 11); tcol(gl, C_GL);
    }

    /* Pano: tek buyuk dugme */
    wolf_id paste = wolf_panel(rb, S(12) + S(10), S(8), S(50), S(54));
    bgc(paste, C_FIELD); rad(paste, S(6)); bgo(paste, 100);
    brd_c(paste, 15, C_BORD2); wolf_set_click(paste, 1); hov(paste, 0x1A1E26);
    wolf_on_click(paste, soon);
    lab(paste, 0, S(6), 50, "Yapistir", 11, C_TXT2, 1);

    /* Yazı tipi: Calibri + 11; B I U S A */
    btn28(rb, 82 + 12, 8, 150, "Calibri", 0);
    btn28(rb, 82 + 12 + 156, 8, 56, "11", 0);
    const char *fmt[5] = { "B", "I", "U", "S", "A" };
    for (int i = 0; i < 5; i++) {
        wolf_id c = wolf_panel(rb, S(82 + 12 + i * 32), S(44), S(26), S(26));
        bgo(c, 0); rad(c, S(4)); wolf_set_click(c, 1); hov(c, 0x1F232B);
        wolf_on_click(c, soon);
        lab(c, 0, (S(26) - F(14) - 6) / 2, 26, fmt[i], 14,
            i == 3 ? C_TAB : C_TXT, 1);
    }

    /* Paragraf: 4 hizalama ikonu + 3 stil dugmesi */
    const char *ails[4] = { I_AL_L, I_AL_C, I_AL_R, I_AL_J };
    for (int i = 0; i < 4; i++) {
        wolf_id c = wolf_panel(rb, S(282 + 12 + i * 32), S(6), S(26), S(26));
        bgo(c, 0); rad(c, S(4)); wolf_set_click(c, 1); hov(c, 0x1F232B);
        wolf_on_click(c, soon);
        wolf_icon(c, (S(26) - S(16)) / 2, (S(26) - S(16)) / 2, S(16), ails[i]);
    }
    btn28(rb, 282 + 12, 38, 70, "Başlık 1", 0);
    btn28(rb, 282 + 12 + 74, 38, 70, "Başlık 2", 0);
    btn28(rb, 282 + 12 + 148, 38, 70, "Normal", 0);

    /* Gorunum (sagda): dugme degil, salt bilgi etiketi */
    lab(rb, grps[3].x + 8, 8, 130, "Duzenleme modu", 12, C_META, -1);
}

/* sayfa ici yer tutucu cubuk */
static void bar(wolf_id page, int x, int y, int wpx)
{
    wolf_id b = wolf_panel(page, x, y, S(wpx), S(9));
    bgc(b, C_BAR); rad(b, S(2)); bgo(b, 100);
}

/* alti cubukluk paragraf bloğu */
static int bars6(wolf_id page, int x, int y)
{
    int w = 476;
    int pct[6] = { 100, 96, 100, 88, 100, 62 };
    for (int i = 0; i < 6; i++) bar(page, x, y + i * S(19), w * pct[i] / 100);
    return y + 6 * S(19);
}

static void build(void);

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

static void build(void)
{
    wolf_style(g_root, WOLF_SP_BG_COLOR, C_GROUND, 0);
    wolf_style(g_root, WOLF_SP_BG_OPA, 100, 0);

    titlebar("Proje raporu.doc — Mürekkep");
    tabstrip();
    ribbon();

    /* govde: gri zemin, kaydirilabilir */
    int by = S(162), bh = g_h - by - S(28);
    wolf_id body = wolf_panel(g_root, 0, by, g_w, bh);
    bgc(body, C_BODY); rad(body, 0); bgo(body, 100);
    wolf_scrollable(body, 1);

    /* sayfa 620x820, yatay ortali, golgeli */
    int pw = S(620), ph = S(820);
    int px = (g_w - pw) / 2;
    wolf_id page = wolf_panel(body, px, S(28), pw, ph);
    bgc(page, C_PAGE); rad(page, 0); bgo(page, 100);
    wolf_style(page, WOLF_SP_SHADOW_WIDTH, S(30), 0);
    wolf_style(page, WOLF_SP_SHADOW_COLOR, 0x000000, 0);
    wolf_style(page, WOLF_SP_SHADOW_OPA, 40, 0);
    wolf_style(page, WOLF_SP_SHADOW_OFS_Y, S(10), 0);

    /* sayfa icerigi — ic bosluk 72, govde genisligi 476 */
    int cx = S(72), cw = 476;
    int y = S(72);
    lab(page, cx, y, cw, "Proje raporu", 26, C_INK, -1);
    y += S(40);
    lab(page, cx, y, cw, "Hazırlayan: Kerem · 4 Kasım 2026", 12, C_SUB, -1);
    y += S(17) + S(24);
    lab(page, cx, y, cw, "1. Özet", 17, C_INK, -1);
    y += S(24) + S(10);
    para(page, cx, y, cw,
         "Bu bolge projenin kapsamini ve bu cosekteki ana sonuclari ozetler. "
         "Metin, yer tutucu satirlarla gosterilmistir.",
         12, C_PARA, 3);
    y += S(64) + S(10);
    y = bars6(page, cx, y) + S(18);
    lab(page, cx, y, cw, "2. Bulgular", 17, C_INK, -1);
    y += S(24) + S(10);
    bars6(page, cx, y);

    /* durum cubugu */
    wolf_id sb = wolf_panel(g_root, 0, g_h - S(28), g_w, S(28));
    bgc(sb, C_GROUND); rad(sb, 0); bgo(sb, 100); brd_c(sb, 1, C_LINE);
    int ly = (S(28) - F(12) - 6) / 2;
    lab(sb, 14, ly, 110, "Sayfa 1 / 4", 12, C_META, -1);
    lab(sb, 14 + 118, ly, 110, "412 sozcuk", 12, C_META, -1);
    lab(sb, 14 + 236, ly, 90, "Turkce", 12, C_META, -1);
    lab(sb, g_w - S(94), ly, 80, "100%", 12, C_META, 2);
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
