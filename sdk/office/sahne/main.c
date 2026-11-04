/**
 * Sahne — sunum uygulamasi (tam ekran, 0.8)
 *
 * Spec bolum 3.4: 70 px ribon, solda 200 px kucuk resim rayi, ortada koyu
 * tuval uzerinde slayt, sagda 240 px Tasarim paneli, altta durum satiri.
 * Kucuk resme tiklama slayti degistirir (ortadaki slayt + aktif cerceve).
 * Gercek pptx ayristirma v0.8 kapsami disinda; icerik specten.
 */
#include "wolf.h"

#define C_GROUND 0x14161B
#define C_LINE   0x2C313B
#define C_PLINE  0x262B35
#define C_CANV   0x0B0C0F   /* slayt tuval zemini */
#define C_PANEL  0x181B22
#define C_FIELD  0x0F1115
#define C_ACC    0x3B82F6
#define C_TXT    0xE8EAEE
#define C_TXT2   0xC9CED8
#define C_META   0x8B919C
#define C_TAB    0xA3A8B3
#define C_SLIDE  0xFFFFFF
#define C_INK    0x1A1D24
#define C_SUB    0x6B717D

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

/* ---- slaytlar (spec ornek icerigi; bos slaytlar "-" ile durust) ---- */
#define NSLIDE 8
typedef struct { const char *title, *sub, *b1, *b2, *b3; } slide_t;
static const slide_t g_slides[NSLIDE] = {
    { "Arctian 0.8", "Lansman sunumu · Kasim 2026", 0, 0, 0 },
    { "Yeni dönem lansmanı", "Kerem · Ürün ekibi",
      "Yeni masaustu deneyimi", "Ofis uygulamalari ailesi",
      "Herm cozunurlukte net gorunum" },
    { "Neler degisti?", "Ksuruk — tam liste surum notlarinda",
      "-", "-", "-" },
    { "-", "", 0, 0, 0 },
    { "-", "", 0, 0, 0 },
    { "-", "", 0, 0, 0 },
    { "-", "", 0, 0, 0 },
    { "Tesekkurler", "sorular? · kerem@arctian", 0, 0, 0 },
};
static int g_cur = 1;                 /* slayt 2 secili (spec) */
static wolf_id g_thumb[NSLIDE];
static wolf_id g_slide, g_snum;       /* ortadaki slayt kutusu + durum metni */

static void soon(wolf_id w, void *u)
{
    (void)w; (void)u;
    wolf_notify("Sahne", "Bu ozellik v0.8'de yok");
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

/* ---- sekme (34) + ribon (70) ---- */
static void tabribbon(void)
{
    wolf_id st = wolf_panel(g_root, 0, S(36), g_w, S(34));
    bgc(st, C_GROUND); rad(st, 0); bgo(st, 100); brd_c(st, 2, C_LINE);
    const char *tabs[6] = { "Dosya", "Giris", "Ekle", "Tasarim", "Gecisler", "Gorunum" };
    int on = 1, x = 12;
    for (int i = 0; i < 6; i++) {
        int tw = wolf_strlen(tabs[i]) * 7 + 4;
        wolf_id t = lab(st, x, (34 - 13) / 2 - 1, tw + 16, tabs[i], 13,
                        i == on ? C_TXT : C_TAB, 1);
        wolf_set_click(t, 1); wolf_on_click(t, soon);
        if (i == on) {
            wolf_id u = wolf_panel(st, x, S(32), S(tw + 16), 2);
            bgc(u, C_ACC); bgo(u, 100); rad(u, 0);
        }
        x += S(tw + 16);
    }

    int y = S(70), h = S(70);
    wolf_id rb = wolf_panel(g_root, 0, y, g_w, h);
    bgc(rb, C_GROUND); rad(rb, 0); bgo(rb, 100); brd_c(rb, 2, C_LINE);
    struct { int x, w; const char *t; } btns[5] = {
        { 14, 84, "Yeni slayt" }, { 106, 84, "Kucuk resim" },
        { 198, 74, "Metin kutusu" }, { 280, 66, "Gorsel" },
        { 354, 74, "Gecis" },
    };
    for (int i = 0; i < 5; i++) {
        wolf_id b = wolf_panel(rb, S(btns[i].x), S(10), S(btns[i].w), S(30));
        bgc(b, C_FIELD); rad(b, S(6)); bgo(b, 100);
        brd_c(b, 15, 0x333A47);
        wolf_set_click(b, 1); hov(b, 0x1A1E26); wolf_on_click(b, soon);
        lab(b, 0, (S(30) - F(12) - 6) / 2, btns[i].w, btns[i].t, 12, C_TXT, 1);
    }
    lab(rb, 14, h - S(16), 380, "Slayt · Icerik · Tasarim", 11, 0x7D8491, -1);
}

/* ---- sol ray: kucuk resimler (200 px) ---- */
/* ortadaki slayti g_cur icerigiyle bastan ciz */
static void paint_slide(void)
{
    wolf_clear(g_slide);
    const slide_t *s = &g_slides[g_cur];
    lab(g_slide, S(56), S(70), 648, s->title, 34, C_INK, -1);
    if (s->sub && s->sub[0])
        lab(g_slide, S(56), S(70) + S(48), 648, s->sub, 15, C_SUB, -1);
    if (s->b1 && s->b1[0]) {
        int by = S(70) + S(110);
        const char *bl[3] = { s->b1, s->b2, s->b3 };
        for (int k = 0; k < 3 && bl[k] && bl[k][0]; k++) {
            wolf_id d = wolf_panel(g_slide, S(56), by + S(5), S(6), S(6));
            bgc(d, C_ACC); bgo(d, 100); rad(d, S(3));
            lab(g_slide, S(72), by, 620, bl[k], 16, C_INK, -1);
            by += S(16) + S(16);
        }
    }
}

static void thumb_cb(wolf_id w, void *u)
{
    (void)w;
    int i = (int)(long)u;
    if (i == g_cur) return;
    g_cur = i;
    /* aktif cerceve + ortadaki slayt + durum: hepsi burada */
    for (int k = 0; k < NSLIDE; k++) {
        wolf_style(g_thumb[k], WOLF_SP_BORDER_COLOR,
                   k == g_cur ? C_ACC : C_PLINE, 0);
        wolf_style(g_thumb[k], WOLF_SP_BORDER_WIDTH, k == g_cur ? 2 : 1, 0);
    }
    paint_slide();
    char t[32];
    int l = 0;
    const char *pre = "Slayt ";
    while (*pre) t[l++] = *pre++;
    t[l++] = (char)('0' + g_cur + 1);
    const char *post = " / 8 · Turkce · %64";
    while (*post && l < 30) t[l++] = *post++;
    t[l] = 0;
    wolf_text(g_snum, t);
}

static void leftpanel(void)
{
    wolf_id lp = wolf_panel(g_root, 0, S(140), S(200), g_h - S(140) - S(28));
    bgc(lp, C_PANEL); rad(lp, 0); bgo(lp, 100); brd_c(lp, 8, C_PLINE);
    wolf_scrollable(lp, 1);

    for (int i = 0; i < NSLIDE; i++) {
        int y = S(12) + i * (S(96) + S(12));
        char n[4]; int l = 0;
        if (i + 1 >= 10) n[l++] = (char)('0' + (i + 1) / 10);
        n[l++] = (char)('0' + (i + 1) % 10);
        n[l] = 0;
        lab(lp, S(10), y + S(42), 18, n, 12, C_META, 2);

        wolf_id t = wolf_panel(lp, S(32), y, S(160), S(96));
        bgc(t, C_SLIDE); bgo(t, 100); rad(t, S(3));
        wolf_style(t, WOLF_SP_BORDER_WIDTH, i == g_cur ? 2 : 1, 0);
        wolf_style(t, WOLF_SP_BORDER_COLOR, i == g_cur ? C_ACC : C_PLINE, 0);
        wolf_set_click(t, 1); hov(t, 0xF0F1F4);
        wolf_on_click_ex(t, thumb_cb, (void *)(long)i);
        g_thumb[i] = t;
        /* kucuk baslik izlenimi: slayt basliginin ilk kelimesi */
        const char *ti = g_slides[i].title;
        lab(t, S(8), S(8), 144, ti, 9, ti[0] == '-' ? 0xC9CED8 : 0x3A4150, -1);
    }
}

/* ---- orta: tuval + slayt, sag: Tasarim paneli (240) ---- */
static void center_right(void)
{
    int lx = S(200), rx = g_w - S(240);
    int top = S(140), bot = g_h - S(28);

    wolf_id cv = wolf_panel(g_root, lx, top, rx - lx, bot - top);
    bgc(cv, C_CANV); rad(cv, 0); bgo(cv, 100);
    wolf_scrollable(cv, 1);

    /* slayt 760x428, yatay dikey ortali (tuval daralirsa kaydirilir) */
    int sw = S(760), sh = S(428);
    int sx = 0;
    if (rx - lx > sw + S(48)) sx = (rx - lx - sw) / 2;
    int sy = 0;
    if (bot - top > sh + S(40)) sy = (bot - top - sh) / 2;
    g_slide = wolf_panel(cv, sx, sy, sw, sh);
    bgc(g_slide, C_SLIDE); bgo(g_slide, 100); rad(g_slide, 0);
    wolf_style(g_slide, WOLF_SP_SHADOW_WIDTH, S(28), 0);
    wolf_style(g_slide, WOLF_SP_SHADOW_COLOR, 0x000000, 0);
    wolf_style(g_slide, WOLF_SP_SHADOW_OPA, 45, 0);
    wolf_style(g_slide, WOLF_SP_SHADOW_OFS_Y, S(8), 0);
    paint_slide();

    /* sag panel */
    wolf_id rp = wolf_panel(g_root, rx, top, S(240), bot - top);
    bgc(rp, C_PANEL); rad(rp, 0); bgo(rp, 100); brd_c(rp, 4, C_PLINE);
    int y = S(16);
    lab(rp, 16, y, 208, "Tasarım", 14, C_TXT, -1);
    y += S(30);
    lab(rp, 16, y, 208, "Tema", 11, C_META, -1);
    y += S(20);
    for (int i = 0; i < 4; i++) {
        static const uint32_t tc[4] = { 0xFFFFFF, 0x1E293B, 0xF5F5F4, 0x0F172A };
        wolf_id t = wolf_panel(rp, 16 + (i % 2) * S(52), y + (i / 2) * (S(30) + S(8)),
                               S(44), S(30));
        bgc(t, tc[i]); bgo(t, 100); rad(t, S(4));
        wolf_style(t, WOLF_SP_BORDER_WIDTH, i == 0 ? 2 : 1, 0);
        wolf_style(t, WOLF_SP_BORDER_COLOR, i == 0 ? C_ACC : C_PLINE, 0);
        wolf_set_click(t, 1); wolf_on_click(t, soon);
    }
    y += 2 * (S(30) + S(8)) + S(8);
    lab(rp, 16, y, 208, "Vurgu rengi", 11, C_META, -1);
    y += S(20);
    static const uint32_t ac[4] = { 0x3B82F6, 0x22C55E, 0xF59E0B, 0xEF4444 };
    for (int i = 0; i < 4; i++) {
        wolf_id c = wolf_panel(rp, 16 + i * (S(20) + S(12)), y, S(20), S(20));
        bgc(c, ac[i]); bgo(c, 100); rad(c, S(10));
        wolf_style(c, WOLF_SP_BORDER_WIDTH, i == 0 ? 2 : 0, 0);
        wolf_style(c, WOLF_SP_BORDER_COLOR, C_TXT, 0);
        wolf_set_click(c, 1); wolf_on_click(c, soon);
    }
    y += S(20) + S(18);
    lab(rp, 16, y, 208, "Gecis", 11, C_META, -1);
    y += S(20);
    wolf_id gb = wolf_panel(rp, 16, y, S(208), S(32));
    bgc(gb, C_FIELD); bgo(gb, 100); rad(gb, S(6));
    brd_c(gb, 15, 0x333A47);
    wolf_set_click(gb, 1); hov(gb, 0x1A1E26); wolf_on_click(gb, soon);
    lab(gb, S(12), (S(32) - F(13) - 6) / 2, 150, "Solma", 13, C_TXT, -1);
    {
        wolf_id ch = wolf_label(gb, 0, 0, "v");
        wolf_resize(ch, S(24), F(12) + 6);
        tcol(ch, C_META); fnt(ch, 12);
        wolf_move(ch, S(208) - S(30), (S(32) - F(12) - 6) / 2);
    }
}

static void statusbar(void)
{
    wolf_id sb = wolf_panel(g_root, 0, g_h - S(28), g_w, S(28));
    bgc(sb, C_GROUND); rad(sb, 0); bgo(sb, 100); brd_c(sb, 1, C_LINE);
    int ly = (S(28) - F(12) - 6) / 2;
    g_snum = lab(sb, 14, ly, 320, "Slayt 2 / 8 · Turkce · %64", 12, C_META, -1);
    lab(sb, g_w - S(224), ly, 210, "Duzen gorunumu", 12, C_META, 2);
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

    titlebar("Lansman sunumu.ppt — Sahne");
    tabribbon();
    leftpanel();
    center_right();
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
