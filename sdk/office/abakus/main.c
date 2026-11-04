/**
 * Abakus — hucreli tablo (tam ekran, 0.8)
 *
 * Spec bolum 3.3: 36 baslik, 34 sekme, 70 ribon, 30 formul cubugu
 * (ad kutusu + fx), izgara A..G, sayfa sekmeleri, durum satiri.
 * Hucreye tiklama secimi tasir, ad kutusu ve formul cubugu guncellenir.
 * Gercek xlsx ayristirma v0.8 kapsami disinda; ornek icerik specten.
 *
 * Slot butcesi dar (192): hucreler panel+etiket cifti (70 hucre = 140),
 * kabukta ikon/panel israfina girilmez — kenar dugmeleri metin etiket.
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

#define COLW 172           /* A..G sabit genislik (1204 = 44 + 7*172) */
#define ROWH 26
#define NROW 10
#define NCOL 7

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

/* ---- ornek icerik (spec) ---- */
static const char *g_val[NROW][NCOL] = {
    { "Kalem", "Adet", "Birim fiyat", "Tutar", "Not", "", "" },
    { "Sunucu kirasi", "12", "8.500", "102.000", "", "", "" },
    { "Lisanslar", "25", "4.200", "105.000", "", "", "" },
    { "Danismanlik", "40", "1.500", "60.000", "", "", "" },
    { "Egitim", "8", "3.750", "30.000", "", "", "" },
    { "Donanim", "5", "22.000", "110.000", "", "", "" },
    { "Pazarlama", "1", "98.000", "98.000", "-", "", "" },
    { "", "", "", "", "", "", "" },
    { "", "", "", "", "", "", "" },
    { "Toplam", "", "", "466.050", "", "", "" },
};

/* secim durumu + sabit kabuk widget'lari */
static wolf_id g_cell[NROW][NCOL];
static wolf_id g_frame;        /* secim cercevesi (grid uzerinde gezer) */
static wolf_id g_name, g_fbox; /* ad kutusu + formul alani etiketleri  */
static int g_sel;              /* secili hucre: r*NCOL+c */

static void soon(wolf_id w, void *u)
{
    (void)w; (void)u;
    wolf_notify("Abaküs", "Bu ozellik v0.8'de yok");
}

/* ---- baslik (36): kompakt, kenar dugmeleri metin ---- */
static void cb_min(wolf_id w, void *u)   { (void)w; (void)u; wolf_win_min(); }
static void cb_close(wolf_id w, void *u) { (void)w; (void)u; wolf_win_close(); }

static void titlebar(const char *text)
{
    wolf_id tb = wolf_panel(g_root, 0, 0, g_w, S(36));
    bgc(tb, C_GROUND); rad(tb, 0); bgo(tb, 100); brd_c(tb, 2, C_LINE);
    lab(tb, 12, (36 - 12) / 2, 520, text, 12, C_TXT2, -1);
    /* — ve x etiketleri; hover yok ama slot kazandiriyor */
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
}

/* ---- ribon (70): Pano + Duzen grubu, gerisi etiket ---- */
static void ribbon(void)
{
    int y = S(70), h = S(70);
    wolf_id rb = wolf_panel(g_root, 0, y, g_w, h);
    bgc(rb, C_GROUND); rad(rb, 0); bgo(rb, 100); brd_c(rb, 2, C_LINE);

    struct { int x, w; const char *t; } btns[2] = {
        { 14, 74, "Yapistir" }, { 100, 62, "Topla" },
    };
    for (int i = 0; i < 2; i++) {
        wolf_id b = wolf_panel(rb, S(btns[i].x), S(10), S(btns[i].w), S(30));
        bgc(b, C_FIELD); rad(b, S(6)); bgo(b, 100);
        brd_c(b, 15, 0x333A47);
        wolf_set_click(b, 1); hov(b, 0x1A1E26); wolf_on_click(b, soon);
        lab(b, 0, (S(30) - F(12) - 6) / 2, btns[i].w, btns[i].t, 12, C_TXT, 1);
    }
    wolf_id gl = lab(rb, 14, h - S(16), 380,
                     "Pano · Duzen · Yazı tipi · Sayı · Hizala · Hucre", 11,
                     0x7D8491, -1);
    (void)gl;
}

/* ---- formul cubugu (30) ---- */
static void fbar(void)
{
    int y = S(140), h = S(30);
    wolf_id fb = wolf_panel(g_root, 0, y, g_w, h);
    bgc(fb, C_GROUND); rad(fb, 0); bgo(fb, 100); brd_c(fb, 2, C_LINE);

    wolf_id nb = wolf_panel(fb, S(8), S(4), S(56), S(22));
    bgc(nb, C_FIELD); rad(nb, S(4)); bgo(nb, 100); brd_c(nb, 15, 0x333A47);
    g_name = lab(nb, 0, (S(22) - F(12) - 6) / 2, 56, "D2", 12, C_TXT, 1);

    wolf_id fx = lab(fb, S(72), (h - F(12) - 6) / 2, 26, "fx", 12, C_META, 1);
    wolf_set_click(fx, 1); wolf_on_click(fx, soon);

    wolf_id ib = wolf_panel(fb, S(102), S(4), g_w - S(110), S(22));
    bgc(ib, C_FIELD); rad(ib, S(4)); bgo(ib, 100); brd_c(ib, 15, 0x333A47);
    g_fbox = lab(ib, S(8), (S(22) - F(12) - 6) / 2, 900, "=B2*C2", 12, C_TXT, -1);
}

/* ---- hucre tiklama: secim cercevesi + ad kutusu + formul ---- */
static void cell_cb(wolf_id w, void *u)
{
    (void)w;
    int k = (int)(long)u;
    int r = k / NCOL, c = k % NCOL;
    g_sel = k;

    char nm[8];
    nm[0] = (char)('A' + c);
    int d = 0;
    int rr = r + 1;
    if (rr >= 10) nm[1 + d++] = (char)('0' + rr / 10);
    nm[1 + d] = (char)('0' + rr % 10);
    nm[2 + d] = 0;
    wolf_text(g_name, nm);

    const char *v = g_val[r][c];
    if (r == 1 && c == 3) v = "=B2*C2";      /* ornek formul */
    else if (!v || !v[0]) v = "";
    wolf_text(g_fbox, v);

    int gx = (g_w - S(1204)) / 2;
    int x = gx + S(44) + c * S(COLW);
    int y = S(170) + S(24) + r * S(ROWH);
    wolf_move(g_frame, x, y);
    (void)0;
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

    /* hucreler: panel (tiklanabilir) + icerik etiketi */
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

            const char *v = g_val[r][c];
            if (v && v[0]) {
                int al = (c >= 1 && c <= 3) ? 2 : 0;   /* sayilar saga */
                int px = (r == 0 || r == NROW - 1) ? 12 : 12;
                uint32_t col = (r == 0) ? C_META : C_INK;
                if (r == NROW - 1) col = C_TXT;
                lab(p, c ? S(8) : S(8), (S(ROWH) - F(px) - 6) / 2,
                    COLW - 16, v, px, col, al);
            }
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
    /* baslangic: D2 */
    g_sel = 1 * NCOL + 3;
    int x = gx + S(44) + 3 * S(COLW);
    int y = gy + S(24) + 1 * S(ROWH);
    wolf_move(g_frame, x, y);
}

/* sayfa sekmeleri (32) + durum (28) */
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
    lab(sb, 14, ly, 460, "Ortalama: 35.700    Toplam: 466.050    Sayim: 9",
        12, C_META, -1);
    lab(sb, g_w - S(124), ly, 110, "Hazır", 12, C_META, 2);
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

    titlebar("Butce 2026.xls — Abaküs");
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
    wolf_on_resize(on_resz, 0);
    build();
    return 0;
}
