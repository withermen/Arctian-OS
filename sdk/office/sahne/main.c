/**
 * Sahne — sunum uygulamasi (tam ekran, 0.8.4)
 *
 * Spec bolum 3.4 kabuk: 70 px ribon, solda 200 px kucuk resim rayi, ortada
 * koyu tuval uzerinde slayt, sagda 240 px Tasarim paneli, altta durum
 * satiri. 0.8.4'te icerik gercek: slayt duzenlenebilir modelde durur,
 * metne tiklama yerinde duzenler (Enter kaydeder), ribon Ac/Kaydet belge
 * dosyasiyla (*.sln: "# baslik", "> altbaslik", "- madde") calisir, yeni
 * slayt / sil / madde ekle gercekten isler. pptx ayristirma ikinci faz;
 * ornek belge aksini isaret edene kadar bellekte durur.
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

/* ---- kucuk string yardimcilari (freestanding: libc yok) ---- */
static void scpy(char *d, int cap, const char *s)
{
    int i = 0;
    if (!s) { if (cap > 0) d[0] = 0; return; }
    for (; i < cap - 1 && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}
static void sadd(char *d, int cap, const char *s)
{
    int n = 0;
    while (n < cap - 1 && d[n]) n++;
    scpy(d + n, cap - n, s);
}
static void sito(char *d, int cap, int v)
{
    char t[12];
    int n = 0, l = 0;
    if (v < 0) { d[l++] = '-'; v = -v; }
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n && l < cap - 1) d[l++] = t[--n];
    d[l] = 0;
}
/* ---- slayt modeli ----
 * Alan numarasi duzenlemede kullanilir: -2 baslik, -1 altbaslik, 0..2 madde */
#define NSLIDE_MAX 12
typedef struct {
    char title[96];
    char sub[96];
    char item[3][140];
    int  nitem;
} slide_t;
static slide_t g_sl[NSLIDE_MAX];
static int g_n;
static int g_cur;
static char g_file[128] = "C:/belgeler/sunum.sln";
static char g_doc[64]   = "sunum.sln";

static wolf_id g_thumb[NSLIDE_MAX], g_tlab[NSLIDE_MAX];
static wolf_id g_slide, g_snum;

/* yerinde duzenleme: aktif giris ve duzenlenen alan */
static wolf_id g_edit;
static int g_ef;

/* freestanding: yapilar atanamaz (memcpy cagrisina doner), bayt bayt kopyala */
static void sl_copy(slide_t *d, const slide_t *s)
{
    const unsigned char *a = (const unsigned char *)s;
    unsigned char *b = (unsigned char *)d;
    for (unsigned i = 0; i < sizeof(slide_t); i++) b[i] = a[i];
}
static void sl_blank(slide_t *s)
{
    s->title[0] = 0; s->sub[0] = 0; s->nitem = 0;
    s->item[0][0] = 0; s->item[1][0] = 0; s->item[2][0] = 0;
}

static void sample_deck(void)
{
    g_n = 8; g_cur = 1;
    for (int i = 0; i < NSLIDE_MAX; i++) sl_blank(&g_sl[i]);
    scpy(g_sl[0].title, 96, "Arctian 0.8");
    scpy(g_sl[0].sub, 96, "Lansman sunumu · Kasim 2026");
    scpy(g_sl[1].title, 96, "Yeni dönem lansmanı");
    scpy(g_sl[1].sub, 96, "Kerem · Ürün ekibi");
    scpy(g_sl[1].item[0], 140, "Yeni masaustu deneyimi");
    scpy(g_sl[1].item[1], 140, "Ofis uygulamalari ailesi");
    scpy(g_sl[1].item[2], 140, "Herm cozunurlukte net gorunum");
    g_sl[1].nitem = 3;
    scpy(g_sl[2].title, 96, "Neler degisti?");
    scpy(g_sl[2].sub, 96, "Ksuruk — tam liste surum notlarinda");
    scpy(g_sl[2].item[0], 140, "-");
    scpy(g_sl[2].item[1], 140, "-");
    scpy(g_sl[2].item[2], 140, "-");
    g_sl[2].nitem = 3;
    scpy(g_sl[7].title, 96, "Tesekkurler");
    scpy(g_sl[7].sub, 96, "sorular? · kerem@arctian");
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

/* ----ileri bildirimler---- */
static void paint_slide(void);
static void status_update(void);
static void commit_edit(void);
static void edit_field(int f);
static void rebuild(void);
static int  load_doc(int notify, int apply);
static void save_file(void);

/* ---- sekme (34) + ribon (70) ---- */
static void rib_open(wolf_id w, void *u)   { (void)w; (void)u; load_doc(1, 1); }
static void rib_save(wolf_id w, void *u)   { (void)w; (void)u; save_file(); }

static void rib_new(wolf_id w, void *u)
{
    (void)w; (void)u;
    commit_edit();
    if (g_n >= NSLIDE_MAX) { wolf_notify("Sahne", "En fazla 12 slayt"); return; }
    for (int i = g_n; i > g_cur + 1; i--) sl_copy(&g_sl[i], &g_sl[i - 1]);
    g_n++;
    g_cur++;
    sl_blank(&g_sl[g_cur]);   /* arkadasinin ardina bos slayt girer */
    rebuild();
}

static void rib_del(wolf_id w, void *u)
{
    (void)w; (void)u;
    commit_edit();
    if (g_n <= 1) { wolf_notify("Sahne", "Son slayt silinmez"); return; }
    for (int i = g_cur; i < g_n - 1; i++) sl_copy(&g_sl[i], &g_sl[i + 1]);
    g_n--;
    if (g_cur >= g_n) g_cur = g_n - 1;
    rebuild();
}

static void rib_item(wolf_id w, void *u)
{
    (void)w; (void)u;
    commit_edit();
    slide_t *s = &g_sl[g_cur];
    if (s->nitem >= 3) { wolf_notify("Sahne", "Slayt başına en fazla 3 madde"); return; }
    s->item[s->nitem][0] = 0;
    s->nitem++;
    paint_slide();
    edit_field(s->nitem - 1);   /* hemen yazmaya basla */
}

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
        wolf_set_click(t, 1);
        if (i == on) {
            wolf_id u = wolf_panel(st, x, S(32), S(tw + 16), 2);
            bgc(u, C_ACC); bgo(u, 100); rad(u, 0);
        }
        x += S(tw + 16);
    }

    int y = S(70), h = S(70);
    wolf_id rb = wolf_panel(g_root, 0, y, g_w, h);
    bgc(rb, C_GROUND); rad(rb, 0); bgo(rb, 100); brd_c(rb, 2, C_LINE);
    struct { int x, w; const char *t; wolf_event_fn cb; } btns[5] = {
        { 14, 60, "Ac",          rib_open },
        { 82, 74, "Kaydet",      rib_save },
        { 170, 84, "Yeni slayt", rib_new },
        { 262, 74, "Slayti sil", rib_del },
        { 344, 84, "Madde ekle", rib_item },
    };
    for (int i = 0; i < 5; i++) {
        wolf_id b = wolf_panel(rb, S(btns[i].x), S(10), S(btns[i].w), S(30));
        bgc(b, C_FIELD); rad(b, S(6)); bgo(b, 100);
        brd_c(b, 15, 0x333A47);
        wolf_set_click(b, 1); hov(b, 0x1A1E26); wolf_on_click(b, btns[i].cb);
        lab(b, 0, (S(30) - F(12) - 6) / 2, btns[i].w, btns[i].t, 12, C_TXT, 1);
    }
    lab(rb, 14, h - S(16), 420, "Metne tikla: yerinde duzenle · Enter: kaydet",
        11, 0x7D8491, -1);
}

/* ---- yerinde metin duzenleme ---- */

/* alanin slayt uzerindeki geometrisi (paint_slide ile ayni olmali) */
static int fx_of(int f) { return f <= -1 ? S(56) : S(72); }
static int fy_of(int f)
{
    if (f == -2) return S(70);
    if (f == -1) return S(70) + S(48);
    return S(70) + S(110) + f * (S(16) + S(16));
}
static int fw_of(int f) { return f <= -1 ? 648 : 620; }
static int fp_of(int f) { return f == -2 ? 34 : (f == -1 ? 15 : 16); }

static char *txt_of(int f)
{
    if (f == -2) return g_sl[g_cur].title;
    if (f == -1) return g_sl[g_cur].sub;
    return g_sl[g_cur].item[f];
}
static int txt_cap(int f) { return f <= -1 ? 96 : 140; }

/* giride Enter veya baska bir yere tiklama: metni modele dok, giriyi kaldir */
static void commit_edit(void)
{
    if (!g_edit) return;
    char tmp[160];
    if (wolf_text_of(g_edit, tmp, (int)sizeof tmp) > 0) {
        /* Enter giride yeni satir birakabilir: ilk satiri al */
        for (int i = 0; tmp[i]; i++)
            if (tmp[i] == '\n' || tmp[i] == '\r') { tmp[i] = 0; break; }
        scpy(txt_of(g_ef), txt_cap(g_ef), tmp);
    }
    wolf_destroy(g_edit);
    g_edit = 0;
    paint_slide();
    /* kucuk resmi tazele: baslik degistiyse ray da gosterir */
    if (g_cur < g_n && g_tlab[g_cur])
        wolf_text(g_tlab[g_cur], g_sl[g_cur].title[0] ? g_sl[g_cur].title : "(bos)");
}

static void edit_enter_cb(wolf_id w, void *u) { (void)w; (void)u; commit_edit(); }

static void edit_click_cb(wolf_id w, void *u)
{
    (void)w;
    edit_field((int)(long)u - 2);
}

static void edit_field(int f)
{
    if (f < -2 || f > 2) return;
    commit_edit();
    g_ef = f;
    int px = fp_of(f);
    g_edit = wolf_input(g_slide, fx_of(f) - 4, fy_of(f) - 4,
                        S(fw_of(f)) + 8, F(px) + 18, "");
    wolf_text(g_edit, txt_of(f));
    fnt(g_edit, px);
    tcol(g_edit, C_INK);
    bgc(g_edit, C_SLIDE); bgo(g_edit, 100); rad(g_edit, S(3));
    wolf_style(g_edit, WOLF_SP_BORDER_WIDTH, 2, 0);
    wolf_style(g_edit, WOLF_SP_BORDER_COLOR, C_ACC, 0);
    wolf_on_enter_ex(g_edit, edit_enter_cb, 0);
    wolf_to_front(g_edit);
}

/* metin etiketi: tiklanabilir, kullanici alan_no+2 tasir */
static wolf_id lab_ed(wolf_id p, int f, int wpx, const char *t,
                      int px, uint32_t col)
{
    wolf_id l = lab(p, fx_of(f), fy_of(f), wpx, t, px, col, -1);
    wolf_set_click(l, 1);
    wolf_on_click_ex(l, edit_click_cb, (void *)(long)(f + 2));
    return l;
}

/* ortadaki slayti g_cur icerigiyle bastan ciz */
static void paint_slide(void)
{
    wolf_clear(g_slide);
    g_edit = 0;
    slide_t *s = &g_sl[g_cur];
    lab_ed(g_slide, -2, 648, s->title, 34, s->title[0] ? C_INK : 0xC9CED8);
    lab_ed(g_slide, -1, 648, s->sub, 15, s->sub[0] ? C_SUB : 0xD8DCE2);
    for (int k = 0; k < s->nitem && k < 3; k++) {
        wolf_id d = wolf_panel(g_slide, S(56), fy_of(k) + S(5), S(6), S(6));
        bgc(d, C_ACC); bgo(d, 100); rad(d, S(3));
        lab_ed(g_slide, k, 620, s->item[k], 16, s->item[k][0] ? C_INK : 0xD8DCE2);
    }
}

/* ---- sol ray: kucuk resimler (200 px) ---- */
static void thumb_cb(wolf_id w, void *u)
{
    (void)w;
    int i = (int)(long)u;
    commit_edit();
    if (i == g_cur) return;
    g_cur = i;
    for (int k = 0; k < g_n; k++) {
        wolf_style(g_thumb[k], WOLF_SP_BORDER_COLOR,
                   k == g_cur ? C_ACC : C_PLINE, 0);
        wolf_style(g_thumb[k], WOLF_SP_BORDER_WIDTH, k == g_cur ? 2 : 1, 0);
    }
    paint_slide();
    status_update();
}

static void leftpanel(void)
{
    wolf_id lp = wolf_panel(g_root, 0, S(140), S(200), g_h - S(140) - S(28));
    bgc(lp, C_PANEL); rad(lp, 0); bgo(lp, 100); brd_c(lp, 8, C_PLINE);
    wolf_scrollable(lp, 1);

    for (int i = 0; i < g_n; i++) {
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
        const char *ti = g_sl[i].title[0] ? g_sl[i].title : "(bos)";
        g_tlab[i] = lab(t, S(8), S(8), 144, ti, 9,
                        g_sl[i].title[0] ? 0x3A4150 : 0xC9CED8, -1);
    }
}

/* ---- orta: tuval + slayt, sag: Tasarim paneli (240) ---- */
static void cv_click_cb(wolf_id w, void *u) { (void)w; (void)u; commit_edit(); }

static void center_right(void)
{
    int lx = S(200), rx = g_w - S(240);
    int top = S(140), bot = g_h - S(28);

    wolf_id cv = wolf_panel(g_root, lx, top, rx - lx, bot - top);
    bgc(cv, C_CANV); rad(cv, 0); bgo(cv, 100);
    wolf_scrollable(cv, 1);
    wolf_set_click(cv, 1);
    wolf_on_click(cv, cv_click_cb);   /* bosluga tiklama duzeni de kapatir */

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
        wolf_set_click(t, 1);
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
        wolf_set_click(c, 1);
    }
    y += S(20) + S(18);
    lab(rp, 16, y, 208, "Gecis", 11, C_META, -1);
    y += S(20);
    wolf_id gb = wolf_panel(rp, 16, y, S(208), S(32));
    bgc(gb, C_FIELD); bgo(gb, 100); rad(gb, S(6));
    brd_c(gb, 15, 0x333A47);
    wolf_set_click(gb, 1); hov(gb, 0x1A1E26);
    lab(gb, S(12), (S(32) - F(13) - 6) / 2, 150, "Solma", 13, C_TXT, -1);
    {
        wolf_id ch = wolf_label(gb, 0, 0, "v");
        wolf_resize(ch, S(24), F(12) + 6);
        tcol(ch, C_META); fnt(ch, 12);
        wolf_move(ch, S(208) - S(30), (S(32) - F(12) - 6) / 2);
    }
}

static void status_update(void)
{
    char t[64], n1[8], n2[8];
    sito(n1, sizeof n1, g_cur + 1);
    sito(n2, sizeof n2, g_n);
    scpy(t, sizeof t, "Slayt ");
    sadd(t, sizeof t, n1);
    sadd(t, sizeof t, " / ");
    sadd(t, sizeof t, n2);
    sadd(t, sizeof t, " · ");
    sadd(t, sizeof t, g_doc);
    wolf_text(g_snum, t);
}

static void statusbar(void)
{
    wolf_id sb = wolf_panel(g_root, 0, g_h - S(28), g_w, S(28));
    bgc(sb, C_GROUND); rad(sb, 0); bgo(sb, 100); brd_c(sb, 1, C_LINE);
    int ly = (S(28) - F(12) - 6) / 2;
    g_snum = lab(sb, 14, ly, 380, "Slayt", 12, C_META, -1);
    lab(sb, g_w - S(224), ly, 210, "Duzen gorunumu", 12, C_META, 2);
    status_update();
}

/* ---- belge dosyasi (*.sln duz metin) ----
 * Satir bicimi: "# baslik" yeni slayt acar, "> altbaslik" ve "- madde"
 * mevcut slayta eklenir; bos satirlar yoksul. apply=0 yalniz modeli yukler
 * (acilista kurulumdan once cagrilir; ikinci kez cizmesin). */
static int load_doc(int notify, int apply){
    static char buf[4096];
    commit_edit();
    int n = wolf_fs_read(g_file, buf, (int)sizeof buf - 1);
    if (n <= 0) {
        if (notify) {
            char m[180];
            scpy(m, sizeof m, "Dosya okunamadi: ");
            sadd(m, sizeof m, g_file);
            wolf_notify("Sahne", m);
        }
        return 0;
    }
    buf[n] = 0;
    int cnt = 0;
    char *p = buf;
    while (*p && cnt < NSLIDE_MAX) {
        char *e = p;
        while (*e && *e != '\n') e++;
        if (*e == '\n') *e++ = 0;
        char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == '#') {
            q++;
            while (*q == ' ') q++;
            g_sl[cnt].title[0] = 0; g_sl[cnt].sub[0] = 0; g_sl[cnt].nitem = 0;
            scpy(g_sl[cnt].title, 96, q);
            cnt++;
        } else if (*q == '>' && cnt > 0) {
            q++;
            while (*q == ' ') q++;
            scpy(g_sl[cnt - 1].sub, 96, q);
        } else if (*q == '-' && cnt > 0) {
            slide_t *s = &g_sl[cnt - 1];
            if (s->nitem < 3) {
                q++;
                while (*q == ' ') q++;
                scpy(s->item[s->nitem], 140, q);
                s->nitem++;
            }
        }
        p = e;
    }
    if (cnt == 0) {
        if (notify) wolf_notify("Sahne", "Belge bos veya bicim taninmadi");
        return 0;
    }
    g_n = cnt;
    g_cur = 0;
    /* baslik cubugu dosya adini gostersin */
    const char *b = g_file;
    for (const char *k = g_file; *k; k++) if (*k == '/') b = k + 1;
    scpy(g_doc, sizeof g_doc, b);
    if (apply) rebuild();
    if (notify) {
        char m[48];
        scpy(m, sizeof m, "Acildi: ");
        sadd(m, sizeof m, g_doc);
        wolf_notify("Sahne", m);
    }
    return 1;
}

/* tampona bir etiketli satir yazar ("# ", "> ", "- "); sigmiyorsa keser.
 * force=1 bos metni de yazar (# satiri slayt ayiracidir, kaybolmasin). */
static int putline(char *buf, int cap, int l, char mark, const char *t, int force)
{
    if ((!t[0] && !force) || l + 3 >= cap) return l;
    buf[l++] = mark;
    buf[l++] = ' ';
    for (int i = 0; t[i] && l < cap - 1; i++) buf[l++] = t[i];
    buf[l++] = '\n';
    return l;
}

static void save_file(void)
{
    static char buf[4096];
    commit_edit();
    int l = 0, cap = (int)sizeof buf;
    buf[0] = 0;
    for (int i = 0; i < g_n; i++) {
        slide_t *s = &g_sl[i];
        /* her slayt en fazla ~360 bayt; son satira yaklasiyarak bitir */
        if (l + 400 > cap) { wolf_notify("Sahne", "Belge buyuk, sigmadi"); return; }
        l = putline(buf, cap, l, '#', s->title, 1);
        l = putline(buf, cap, l, '>', s->sub, 0);
        for (int k = 0; k < s->nitem && k < 3; k++)
            l = putline(buf, cap, l, '-', s->item[k], 0);
    }
    buf[l] = 0;
    if (wolf_fs_write(g_file, buf, l) == 0) {
        char m[64];
        scpy(m, sizeof m, "Kaydedildi: ");
        sadd(m, sizeof m, g_doc);
        wolf_notify("Sahne", m);
    } else {
        char m[180];
        scpy(m, sizeof m, "Kaydedilemedi: ");
        sadd(m, sizeof m, g_file);
        wolf_notify("Sahne", m);
    }
}

/* ---- kurulum ---- */
static void build(void);

static void rebuild(void)
{
    commit_edit();
    g_edit = 0;
    char t[96];
    scpy(t, sizeof t, g_doc);
    sadd(t, sizeof t, " — Sahne");
    wolf_clear(g_root);
    titlebar(t);
    tabribbon();
    for (int i = 0; i < NSLIDE_MAX; i++) { g_thumb[i] = 0; g_tlab[i] = 0; }
    leftpanel();
    center_right();
    statusbar();
}

static void on_resz(wolf_id w, void *u)
{
    (void)w; (void)u;
    int w0 = g_w, h0 = g_h;
    if (wolf_screen(&g_w, &g_h) != 0) return;
    if (g_w == w0 && g_h == h0) return;   /* boyut ayni: yeniden kurmayiz */
    int a = g_w * 1000 / 1280, b = g_h * 1000 / 720;
    g_s = a < b ? a : b;
    if (g_s < 300) g_s = 300;
    commit_edit();                        /* yazilmakta olan kaybolmasin */
    rebuild();
}

static void build(void)
{
    wolf_style(g_root, WOLF_SP_BG_COLOR, C_GROUND, 0);
    wolf_style(g_root, WOLF_SP_BG_OPA, 100, 0);
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

    const char *arg = wolf_arg();
    if (arg && arg[0]) {
        scpy(g_file, sizeof g_file, arg);
        sample_deck();                    /* acilamazsa ornek bellekte kalsin */
        if (!load_doc(0, 0)) {
            char m[200];
            scpy(m, sizeof m, "Belge acilamadi, ornek yuklendi: ");
            sadd(m, sizeof m, arg);
            wolf_notify("Sahne", m);
        }
    } else {
        sample_deck();
    }

    wolf_on_resize(on_resz, 0);
    build();
    return 0;
}
