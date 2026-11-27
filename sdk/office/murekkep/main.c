/**
 * Mürekkep — belge editörü (tam ekran)
 *
 * 36 px başlık, 34 px sekme şeridi, 92 px ribon (Dosya / Yazı tipi /
 * Paragraf ve stiller / Görünüm), gri zemin üzerinde 620x820 beyaz sayfa,
 * 28 px durum çubuğu. Ortak office_picker.h ile C:/belgeler ve diğer
 * klasörlerden .txt/.md açar ve farklı kaydeder.
 */
#include "wolf.h"
#include "../office_picker.h"

#define C_GROUND 0x14161B
#define C_LINE   0x2C313B
#define C_PLINE  0x262B35
#define C_BODY   0x0B0C0F   /* sayfa çevresi */
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

#define SVG_MIN   "<svg viewBox=\"0 0 10 10\"><path d=\"M1 5h8\" stroke=\"#8b919c\" stroke-width=\"1\" fill=\"none\"/></svg>"
#define SVG_MAX   "<svg viewBox=\"0 0 10 10\"><rect x=\"1.5\" y=\"1.5\" width=\"7\" height=\"7\" stroke=\"#8b919c\" stroke-width=\"1\" fill=\"none\"/></svg>"
#define SVG_CLOSE "<svg viewBox=\"0 0 10 10\"><path d=\"M1.5 1.5l7 7M8.5 1.5l-7 7\" stroke=\"#8b919c\" stroke-width=\"1.1\" fill=\"none\"/></svg>"
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

/* ---- küçük string yardımcıları (freestanding: libc yok) ---- */
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

/* ---- belge ---- */
#define DOC_CAP 8192
static char g_doc[DOC_CAP] = "";
static char g_file[160] = "C:/belgeler/belge.txt";
static char g_docname[64] = "belge.txt";
static int  g_untitled = 1;
static int  g_align = 0;
static wolf_id g_page;      /* çok satırlı alan */
static wolf_id g_cnt;       /* durum: kelime/satır */
static wolf_id g_title_lbl; /* başlık çubuğu metni */

static const char *MK_PRE[4]  = { "**", "*", "__", "~~" };
static const char *MK_POST[4] = { "**", "*", "__", "~~" };
static const char *MK_LBL[4]  = { "B", "I", "U", "S" };

static void sync_doc(void)
{
    if (g_page) wolf_text_of(g_page, g_doc, DOC_CAP);
}

static void status_update(void)
{
    int w = 0, ln = 0, in = 0, i;
    for (i = 0; g_doc[i]; i++) {
        char c = g_doc[i];
        if (c == '\n') ln++;
        int sp = (c == ' ' || c == '\n' || c == '\t' || c == '\r');
        if (!sp && !in) { w++; in = 1; }
        if (sp) in = 0;
    }
    if (i == 0) ln = 0;
    else if (g_doc[i - 1] != '\n') ln++;
    char t[64], nw[12];
    scpy(t, sizeof t, "kelime ");
    sito(nw, sizeof nw, w);
    sadd(t, sizeof t, nw);
    sadd(t, sizeof t, " · satır ");
    sito(nw, sizeof nw, ln);
    sadd(t, sizeof t, nw);
    wolf_text(g_cnt, t);
}

static void title_refresh(void)
{
    if (!g_title_lbl) return;
    char t[96];
    scpy(t, sizeof t, g_docname);
    sadd(t, sizeof t, " — Mürekkep");
    wolf_text(g_title_lbl, t);
}

/* ---- başlık çubuğu ---- */
static void cb_min(wolf_id w, void *u)   { (void)w; (void)u; wolf_win_min(); }
static void cb_max(wolf_id w, void *u)   { (void)w; (void)u; wolf_notify("Mürekkep", "Zaten tam ekrandasın"); }
static void cb_close(wolf_id w, void *u) { (void)w; (void)u; wolf_win_close(); }

static void winbtn(int x, const char *svg, wolf_event_fn cb, uint32_t hover)
{
    wolf_id b = wolf_panel(g_root, x, 0, S(46), S(36));
    bgo(b, 0); rad(b, 0); wolf_set_click(b, 1); hov(b, hover);
    wolf_icon(b, (S(46) - S(10)) / 2, (S(36) - S(10)) / 2, S(10), svg);
    wolf_on_click(b, cb);
}

static void titlebar(void)
{
    char t[96];
    wolf_id tb = wolf_panel(g_root, 0, 0, g_w, S(36));
    bgc(tb, C_GROUND); rad(tb, 0); bgo(tb, 100); brd_c(tb, 2, C_LINE);
    scpy(t, sizeof t, g_docname);
    sadd(t, sizeof t, " — Mürekkep");
    g_title_lbl = lab(tb, 12, (36 - 12) / 2, 560, t, 12, C_TXT2, -1);
    winbtn(g_w - S(46) * 3,  SVG_MIN,   cb_min,   0x262B35);
    winbtn(g_w - S(46) * 2,  SVG_MAX,   cb_max,   0x262B35);
    winbtn(g_w - S(46),      SVG_CLOSE, cb_close, 0xC42B1C);
}

/* ---- ileri bildirimler ---- */
static void rebuild(void);
static void load_doc_file(int notify);
static void save_doc_file(void);
static void cb_open(wolf_id w, void *u);
static void cb_save_as(wolf_id w, void *u);

/* ---- sekme şeridi (34 px) ---- */
static void cb_tab(wolf_id w, void *u)
{
    (void)w;
    int idx = (int)(long)u;
    if (idx == 0) { cb_open(0, 0); return; }
    if (idx == 2) { wolf_ta_wrap(g_page, "\n---\n", ""); sync_doc(); status_update(); return; }
    if (idx == 3) { wolf_ta_wrap(g_page, "- ", ""); sync_doc(); status_update(); return; }
    if (idx == 4) { wolf_ta_wrap(g_page, "# ", ""); sync_doc(); status_update(); return; }
    if (idx == 5) { wolf_notify("Mürekkep", "Tam sayfa düzenleme görünümü etkin"); return; }
}

static void tabstrip(void)
{
    wolf_id st = wolf_panel(g_root, 0, S(36), g_w, S(34));
    bgc(st, C_GROUND); rad(st, 0); bgo(st, 100); brd_c(st, 2, C_LINE);
    const char *tabs[6] = { "Dosya", "Giriş", "Ekle", "Düzen", "Başvurular", "Görünüm" };
    int on = 1;
    int x = 12;
    for (int i = 0; i < 6; i++) {
        int tw = wolf_strlen(tabs[i]) * 7 + 4;
        wolf_id t = lab(st, x, (34 - 13) / 2 - 1, tw + 16, tabs[i], 13,
                        i == on ? C_TXT : C_TAB, 1);
        wolf_set_click(t, 1); wolf_on_click_ex(t, cb_tab, (void *)(long)i);
        if (i == on) {
            wolf_id u = wolf_panel(st, x, S(32), S(tw + 16), 2);
            bgc(u, C_ACC); bgo(u, 100); rad(u, 0);
        }
        x += S(tw + 16);
    }
}

/* ---- ribon (92 px) ---- */
static wolf_id btn28(wolf_id p, int x, int y, int w, const char *t,
                     wolf_event_fn cb, int primary)
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
    if (cb) wolf_on_click(b, cb);
    return b;
}

static void set_docname(void)
{
    const char *b = g_file;
    for (const char *k = g_file; *k; k++) if (*k == '/') b = k + 1;
    scpy(g_docname, sizeof g_docname, b);
}

static void on_pick_open(const char *path, void *u)
{
    (void)u;
    if (!path || !path[0]) return;
    scpy(g_file, sizeof g_file, path);
    g_untitled = 0;
    load_doc_file(1);
    title_refresh();
}

static void on_pick_save(const char *path, void *u)
{
    (void)u;
    if (!path || !path[0]) return;
    scpy(g_file, sizeof g_file, path);
    g_untitled = 0;
    set_docname();
    title_refresh();
    save_doc_file();
}

static void cb_new(wolf_id w, void *u)
{
    (void)w; (void)u;
    g_doc[0] = 0;
    scpy(g_file, sizeof g_file, "C:/belgeler/belge.txt");
    g_untitled = 1;
    set_docname();
    title_refresh();
    if (g_page) wolf_text(g_page, "");
    status_update();
}

static void cb_open(wolf_id w, void *u)
{
    (void)w; (void)u;
    office_picker_open(OP_MODE_OPEN, "Belge Aç", "C:/belgeler", g_docname,
                       ".txt,.md", ".txt", on_pick_open, 0);
}

static void cb_save(wolf_id w, void *u)
{
    (void)w; (void)u;
    if (g_untitled) {
        cb_save_as(0, 0);
        return;
    }
    save_doc_file();
}

static void cb_save_as(wolf_id w, void *u)
{
    (void)w; (void)u;
    office_picker_open(OP_MODE_SAVE, "Belgeyi Farklı Kaydet", "C:/belgeler", g_docname,
                       ".txt,.md", ".txt", on_pick_save, 0);
}

static void cb_mark(wolf_id w, void *u)
{
    (void)w;
    int k = (int)(long)u;
    if (k < 0 || k > 3) return;
    wolf_ta_wrap(g_page, MK_PRE[k], MK_POST[k]);
    sync_doc();
    status_update();
}

static void cb_prefix(wolf_id w, void *u)
{
    (void)w;
    wolf_ta_wrap(g_page, (const char *)u, "");
    sync_doc();
    status_update();
}

static void cb_align(wolf_id w, void *u)
{
    (void)w;
    int idx = (int)(long)u;
    g_align = (idx >= 0 && idx <= 2) ? idx : 0;
    if (g_page) wolf_style(g_page, WOLF_SP_TEXT_ALIGN, g_align, 0);
}

static void ribbon(void)
{
    int y = S(70), h = S(92);
    wolf_id rb = wolf_panel(g_root, 0, y, g_w, h);
    bgc(rb, C_GROUND); rad(rb, 0); bgo(rb, 100); brd_c(rb, 2, C_LINE);

    struct { int x, w; const char *name; } grps[4] = {
        { 12, 186, "Dosya" }, { 198, 206, "Yazı tipi" },
        { 404, 226, "Paragraf ve stiller" }, { 0, 130, "Görünüm" },
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

    /* Dosya: Yeni / Aç / Kaydet / Farklı Kaydet */
    btn28(rb, 20,  6, 68, "Yeni",          cb_new,     0);
    btn28(rb, 94,  6, 94, "Aç",            cb_open,    0);
    btn28(rb, 20, 38, 68, "Kaydet",        cb_save,    1);
    btn28(rb, 94, 38, 94, "Farklı Kaydet", cb_save_as, 0);

    /* Yazı tipi: Markdown bilgisi + B I U S */
    wolf_id f1 = wolf_panel(rb, S(210), S(8), S(124), S(28));
    bgc(f1, C_FIELD); rad(f1, S(6)); bgo(f1, 100); brd_c(f1, 15, C_BORD2);
    lab(f1, 0, (S(28) - F(13) - 6) / 2, 124, "Markdown", 13, C_TXT2, 1);
    wolf_id f2 = wolf_panel(rb, S(340), S(8), S(54), S(28));
    bgc(f2, C_FIELD); rad(f2, S(6)); bgo(f2, 100); brd_c(f2, 15, C_BORD2);
    lab(f2, 0, (S(28) - F(13) - 6) / 2, 54, "14 px", 13, C_TXT2, 1);
    for (int i = 0; i < 4; i++) {
        wolf_id c = wolf_panel(rb, S(210 + i * 34), S(44), S(26), S(26));
        bgo(c, 0); rad(c, S(4)); wolf_set_click(c, 1); hov(c, 0x1F232B);
        wolf_on_click_ex(c, cb_mark, (void *)(long)i);
        lab(c, 0, (S(26) - F(14) - 6) / 2, 26, MK_LBL[i], 14, C_TXT, 1);
    }

    /* Paragraf: 4 hizalama ikonu + 3 satır başı imi */
    const char *ails[4] = { I_AL_L, I_AL_C, I_AL_R, I_AL_J };
    for (int i = 0; i < 4; i++) {
        wolf_id c = wolf_panel(rb, S(416 + i * 32), S(6), S(26), S(26));
        bgo(c, 0); rad(c, S(4)); wolf_set_click(c, 1); hov(c, 0x1F232B);
        wolf_on_click_ex(c, cb_align, (void *)(long)i);
        wolf_icon(c, (S(26) - S(16)) / 2, (S(26) - S(16)) / 2, S(16), ails[i]);
    }
    static const char *PX[3] = { "# ", "## ", "- " };
    const char *PXN[3] = { "Başlık 1", "Başlık 2", "Madde" };
    for (int i = 0; i < 3; i++) {
        wolf_id b = btn28(rb, 416 + i * 70, 38, 66, PXN[i], 0, 0);
        wolf_on_click_ex(b, cb_prefix, (void *)PX[i]);
    }

    /* Görünüm */
    lab(rb, S(grps[3].x + 8), S(8), 120, "Düz metin modu", 12, C_META, -1);
    lab(rb, S(grps[3].x + 8), S(24), 120, "txt / md", 11, 0x6B717D, -1);
}

/* ---- dosya ---- */
static void load_doc_file(int notify)
{
    static char buf[DOC_CAP];
    int n = wolf_fs_read(g_file, buf, DOC_CAP - 1);
    if (n < 0) {
        if (notify) {
            char m[200];
            scpy(m, sizeof m, "Dosya okunamadı: ");
            sadd(m, sizeof m, g_file);
            wolf_notify("Mürekkep", m);
        }
        return;
    }
    buf[n] = 0;
    scpy(g_doc, sizeof g_doc, buf);
    set_docname();
    title_refresh();
    if (g_page) wolf_text(g_page, g_doc);
    status_update();
    if (notify) {
        char m[96];
        scpy(m, sizeof m, "Açıldı: ");
        sadd(m, sizeof m, g_docname);
        wolf_notify("Mürekkep", m);
    }
}

static void save_doc_file(void)
{
    sync_doc();
    int n = 0;
    while (n < DOC_CAP - 1 && g_doc[n]) n++;
    if (wolf_fs_write(g_file, g_doc, n) == 0) {
        char m[96];
        scpy(m, sizeof m, "Kaydedildi: ");
        sadd(m, sizeof m, g_docname);
        wolf_notify("Mürekkep", m);
    } else {
        char m[200];
        scpy(m, sizeof m, "Kaydedilemedi: ");
        sadd(m, sizeof m, g_file);
        wolf_notify("Mürekkep", m);
    }
    status_update();
}

static void on_chg(wolf_id w, void *u)
{
    (void)w; (void)u;
    sync_doc();
    status_update();
}

/* ---- kurulum ---- */
static void build(void);

static void rebuild(void)
{
    wolf_clear(g_root);
    build();
}

static void on_resz(wolf_id w, void *u)
{
    (void)w; (void)u;
    int w0 = g_w, h0 = g_h;
    if (wolf_screen(&g_w, &g_h) != 0) return;
    if (g_w == w0 && g_h == h0) return;
    int a = g_w * 1000 / 1280, b = g_h * 1000 / 720;
    g_s = a < b ? a : b;
    if (g_s < 300) g_s = 300;
    sync_doc();
    rebuild();
}

static void build(void)
{
    office_picker_reset();
    wolf_style(g_root, WOLF_SP_BG_COLOR, C_GROUND, 0);
    wolf_style(g_root, WOLF_SP_BG_OPA, 100, 0);

    titlebar();
    tabstrip();
    ribbon();

    /* gövde: gri zemin, kaydırılabilir */
    int by = S(162), bh = g_h - by - S(28);
    wolf_id body = wolf_panel(g_root, 0, by, g_w, bh);
    bgc(body, C_BODY); rad(body, 0); bgo(body, 100);
    wolf_scrollable(body, 1);

    /* sayfa 620x820, yatay ortalı, gölgeli */
    int pw = S(620), ph = S(820);
    int px = (g_w - pw) / 2;
    wolf_id page = wolf_panel(body, px, S(28), pw, ph);
    bgc(page, C_PAGE); rad(page, 0); bgo(page, 100);
    wolf_style(page, WOLF_SP_SHADOW_WIDTH, S(30), 0);
    wolf_style(page, WOLF_SP_SHADOW_COLOR, 0x000000, 0);
    wolf_style(page, WOLF_SP_SHADOW_OPA, 40, 0);
    wolf_style(page, WOLF_SP_SHADOW_OFS_Y, S(10), 0);

    /* sayfa içi: çok satırlı alan */
    g_page = wolf_input_multi(page, S(72), S(64), S(476), S(700),
                              "Belge boş — yazmaya başlayın");
    wolf_text(g_page, g_doc);
    fnt(g_page, 14);
    tcol(g_page, C_INK);
    wolf_style(g_page, WOLF_SP_TEXT_ALIGN, g_align, 0);
    wolf_on_change_ex(g_page, on_chg, 0);

    /* durum çubuğu */
    wolf_id sb = wolf_panel(g_root, 0, g_h - S(28), g_w, S(28));
    bgc(sb, C_GROUND); rad(sb, 0); bgo(sb, 100); brd_c(sb, 1, C_LINE);
    int ly = (S(28) - F(12) - 6) / 2;
    g_cnt = lab(sb, 14, ly, 260, "kelime 0 · satır 0", 12, C_META, -1);
    lab(sb, g_w - S(174), ly, 160, "Düz metin · UTF-8", 12, C_META, 2);
    status_update();
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
        g_untitled = 0;
    }
    set_docname();
    {
        static char buf[DOC_CAP];
        int n = wolf_fs_read(g_file, buf, DOC_CAP - 1);
        if (n > 0) {
            buf[n] = 0;
            scpy(g_doc, sizeof g_doc, buf);
            g_untitled = 0;
        } else if (arg && arg[0]) {
            char m[200];
            scpy(m, sizeof m, "Belge açılamadı, boş sayfa: ");
            sadd(m, sizeof m, arg);
            wolf_notify("Mürekkep", m);
        }
    }

    wolf_on_resize(on_resz, 0);
    build();
    return 0;
}
