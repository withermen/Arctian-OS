/**
 * Mercek — PDF goruntuleyici (tam ekran, 0.8)
 *
 * Spec bolum 3.5: 46 px arac cubugu (sayfa ileri/geri, % zoom, eylemler),
 * solda 150 px sayfa kucuk resimleri, ortada A4 sayfa, sagda 250 px
 * Aciklamalar, altta durum satiri. Oncesi/sonrasi ve zoom calisir;
 * yazilmamis eylemler bildirimle durustce "v0.8'de yok" der.
 * Gercek pdf cozumleme v0.8 kapsami disinda; ornek icerik specten.
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
#define C_BAR    0xD4D8DF
#define C_NOTE   0xFFF7E0
#define C_NOTE_T 0x92400E

#define NPAGE 24
#define NTHUMB 8

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

/* ---- durum ---- */
static int g_page = 3;                    /* spec: 3. sayfa acik */
static const int g_zooms[6] = { 50, 67, 80, 100, 125, 150 };
static int g_zi = 3;                      /* %100 */
static wolf_id g_thumb[NTHUMB];
static wolf_id g_canvas, g_pbox, g_zbox;   /* tuval, "3 / 24", "%100" */

static void soon(wolf_id w, void *u)
{
    (void)w; (void)u;
    wolf_notify("Mercek", "Bu ozellik v0.8'de yok");
}

/* sayfa basliklari — gosterim icerigi; bos sayfalar durust "-" */
static const char *page_title(int p)
{
    if (p == 1) return "1. Giris";
    if (p == 2) return "2. Yontem";
    if (p == 3) return "3. Pazar analizi";
    if (p == 4) return "4. Sonuc";
    return 0;
}

/* iki basamakli sayi -> dize */
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
    utoa2(NPAGE, n);
    p = n;
    while (*p) t[l++] = *p++;
    t[l] = 0;
    wolf_text(g_pbox, t);
}

static void refresh_thumbs(void)
{
    for (int k = 0; k < NTHUMB; k++) {
        int on = (k + 1 == g_page);
        wolf_style(g_thumb[k], WOLF_SP_BORDER_COLOR, on ? C_ACC : C_PLINE, 0);
        wolf_style(g_thumb[k], WOLF_SP_BORDER_WIDTH, on ? 2 : 1, 0);
    }
}

/* ---- sayfa govdesi (zoom/sayfa degisince yeniden cizilir) ---- */
static void paint_page(void)
{
    wolf_clear(g_canvas);
    int z = g_zooms[g_zi];
    int pw = S(560) * z / 100;
    int ph = S(792) * z / 100;
    int avail_w = g_w - S(150) - S(250);
    int sx = pw + S(48) < avail_w ? (avail_w - pw) / 2 : S(24);
    int sy = S(24);

    wolf_id pg = wolf_panel(g_canvas, sx, sy, pw, ph);
    bgc(pg, C_PAGE); bgo(pg, 100); rad(pg, 0);
    wolf_style(pg, WOLF_SP_SHADOW_WIDTH, S(28), 0);
    wolf_style(pg, WOLF_SP_SHADOW_COLOR, 0x000000, 0);
    wolf_style(pg, WOLF_SP_SHADOW_OPA, 45, 0);
    wolf_style(pg, WOLF_SP_SHADOW_OFS_Y, S(8), 0);

    /* icerik olcegi: sayfa genisligine gore (560 taban) */
    int m = pw * 72 / 560;                 /* kenar boslugu */
    int cw = pw - 2 * m;                   /* govde genisligi */
    int y = m;
    const char *ti = page_title(g_page);
    if (ti) lab(pg, m, y, cw, ti, 22, C_INK, -1);
    else lab(pg, m, y, cw, "Bu sayfada gosterim icerigi yok", 14, C_SUB, -1);
    y += S(34);

    /* paragraf yer tutuculari (spec: gri cubukler) */
    int pct[7] = { 100, 97, 100, 90, 100, 84, 55 };
    for (int i = 0; i < 7; i++) {
        wolf_id b = wolf_panel(pg, m, y, cw * pct[i] / 100, S(8));
        bgc(b, C_BAR); rad(b, S(2)); bgo(b, 100);
        y += S(17);
    }
    y += S(10);

    if (g_page == 3) {
        /* bolge tablosu (spec) */
        wolf_id tb = wolf_panel(pg, m, y, cw, S(24) + 3 * S(22) + S(4));
        bgc(tb, 0xFFFFFF); bgo(tb, 100); rad(tb, 0);
        brd_c(tb, 15, 0xC9CED8);
        int cx = S(10);
        lab(tb, cx, S(5), cw / 2 - S(20), "Bolge", 12, 0x374151, -1);
        lab(tb, cw / 2 + cx, S(5), cw / 2 - S(20), "Pay", 12, 0x374151, 2);
        wolf_id hr = wolf_panel(tb, 1, S(24), cw - 2, 1);
        bgc(hr, 0xC9CED8); bgo(hr, 100); rad(hr, 0);
        const char *rows[3][2] = {
            { "Marmara", "%42" }, { "Ic Anadolu", "%23" }, { "Diger", "%35" },
        };
        for (int i = 0; i < 3; i++) {
            int ry = S(24) + S(4) + i * S(22);
            lab(tb, cx, ry, cw / 2 - S(20), rows[i][0], 12, C_INK, -1);
            lab(tb, cw / 2 + cx, ry, cw / 2 - S(20), rows[i][1], 12, C_INK, 2);
        }
        y += S(24) + 3 * S(22) + S(4) + S(16);

        /* sari not kutusu */
        wolf_id nb = wolf_panel(pg, m, y, cw, S(56));
        bgc(nb, C_NOTE); bgo(nb, 100); rad(nb, S(6));
        wolf_style(nb, WOLF_SP_BORDER_WIDTH, 1, 0);
        wolf_style(nb, WOLF_SP_BORDER_COLOR, 0xFDE68A, 0);
        lab(nb, S(12), S(8), cw - S(24), "Tablo kaynaklari kontrol edilecek",
            12, C_NOTE_T, -1);
        lab(nb, S(12), S(8) + S(22), cw - S(24), "— Kerem, 2 Kas", 11, 0xB45309, -1);
        y += S(56) + S(14);
    }

    /* alt paragraf */
    for (int i = 0; i < 4; i++) {
        wolf_id b = wolf_panel(pg, m, y, cw * pct[i] / 100, S(8));
        bgc(b, C_BAR); rad(b, S(2)); bgo(b, 100);
        y += S(17);
    }
}

/* ---- olaylar ---- */
static void go_page(int p)
{
    if (p < 1) p = 1;
    if (p > NPAGE) p = NPAGE;
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
    if (g_zi >= 5) return;
    g_zi++;
    set_zoom_label();
    paint_page();
}
static void cb_zout(wolf_id w, void *u)
{
    (void)w; (void)u;
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

static void titlebar(const char *text)
{
    wolf_id tb = wolf_panel(g_root, 0, 0, g_w, S(36));
    bgc(tb, C_GROUND); rad(tb, 0); bgo(tb, 100); brd_c(tb, 2, C_LINE);
    lab(tb, 12, (36 - 12) / 2, 560, text, 12, C_TXT2, -1);
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
    if (cb) wolf_on_click(b, cb); else wolf_on_click(b, soon);
    lab(b, 0, (S(30) - F(13) - 6) / 2, w, t, 13,
        primary ? 0xFFFFFF : C_TXT, 1);
    return b;
}

static void toolbar(void)
{
    wolf_id tb = wolf_panel(g_root, 0, S(36), g_w, S(46));
    bgc(tb, C_GROUND); rad(tb, 0); bgo(tb, 100); brd_c(tb, 2, C_LINE);

    /* sol kume: ‹ › | sayfa | - % + */
    tbtn(12, 30, "<", 0, cb_prev);
    tbtn(46, 30, ">", 0, cb_next);
    wolf_id pb = wolf_panel(g_root, S(84), S(36) + S(8), S(70), S(30));
    bgc(pb, C_FIELD); brd_c(pb, 15, 0x333A47); rad(pb, S(6)); bgo(pb, 100);
    g_pbox = lab(pb, 0, (S(30) - F(13) - 6) / 2, 70, "3 / 24", 13, C_TXT, 1);

    tbtn(166, 28, "-", 0, cb_zout);
    wolf_id zb = wolf_panel(g_root, S(200), S(36) + S(8), S(54), S(30));
    bgc(zb, C_GROUND); rad(zb, 0); bgo(zb, 0);
    g_zbox = lab(zb, 0, (S(30) - F(13) - 6) / 2, 54, "%100", 13, C_TXT2, 1);
    tbtn(260, 28, "+", 0, cb_zin);

    /* sag kume: eylemler + birincil (mantiksal sag kenardan sayilir) */
    int R = (g_w * 1000 + 500) / g_s;
    int x = R - 12;
    x -= 84; tbtn(x, 84, "Paylaş", 1, soon);
    x -= 8 + 52;  tbtn(x, 52, "Ara", 0, soon);
    x -= 8 + 70;  tbtn(x, 70, "Dondur", 0, soon);
    x -= 8 + 76;  tbtn(x, 76, "Not ekle", 0, soon);
    x -= 8 + 70;  tbtn(x, 70, "Vurgula", 0, soon);
}

static void leftpanel(void)
{
    wolf_id lp = wolf_panel(g_root, 0, S(82), S(150), g_h - S(82) - S(28));
    bgc(lp, C_PANEL); rad(lp, 0); bgo(lp, 100); brd_c(lp, 8, C_PLINE);
    wolf_scrollable(lp, 1);

    for (int i = 0; i < NTHUMB; i++) {
        int y = S(12) + i * (S(136) + S(12));
        wolf_id t = wolf_panel(lp, (S(150) - S(100)) / 2, y, S(100), S(136));
        bgc(t, C_PAGE); bgo(t, 100); rad(t, S(2));
        int on = (i + 1 == g_page);
        wolf_style(t, WOLF_SP_BORDER_WIDTH, on ? 2 : 1, 0);
        wolf_style(t, WOLF_SP_BORDER_COLOR, on ? C_ACC : C_PLINE, 0);
        wolf_set_click(t, 1); hov(t, 0xF0F1F4);
        wolf_on_click_ex(t, thumb_cb, (void *)(long)i);
        g_thumb[i] = t;
        /* satir izlenimi: uc ince cizgi */
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

    lab(rp, 16, S(16), 218, "Aciklamalar (2)", 14, C_TXT, -1);
    const char *au[2] = { "Elif · 14:02", "Kerem · 15:40" };
    const char *tx[2] = {
        "Tablo kaynaklarini ekleyelim mi?",
        "Sayfa 3'teki notu yarina kadar kapatirim."
    };
    for (int i = 0; i < 2; i++) {
        int y = S(16) + S(30) + i * (S(92) + S(12));
        wolf_id c = wolf_panel(rp, 16, y, S(218), S(92));
        bgc(c, C_FIELD); bgo(c, 100); rad(c, S(8));
        brd_c(c, 15, C_PLINE);
        wolf_set_click(c, 1); hov(c, 0x1A1E26); wolf_on_click(c, soon);
        lab(c, S(12), S(10), 194, au[i], 12, C_TXT2, -1);
        wolf_id body = wolf_label(c, S(12), S(32), tx[i]);
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
    lab(sb, 14, ly, 480, "24 sayfa · PDF 1.7 · 2,8 MB", 12, C_META, -1);
    lab(sb, g_w - S(224), ly, 210, "Tam ekran", 12, C_META, 2);
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

    titlebar("rapor-2026.pdf — Mercek");
    toolbar();
    leftpanel();

    /* orta tuval */
    g_canvas = wolf_panel(g_root, S(150), S(82),
                          g_w - S(150) - S(250), g_h - S(82) - S(28));
    bgc(g_canvas, C_CANV); rad(g_canvas, 0); bgo(g_canvas, 100);
    wolf_scrollable(g_canvas, 2);
    paint_page();

    rightpanel();
    statusbar();
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
