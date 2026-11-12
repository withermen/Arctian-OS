/* DPI olcekleme cekirdegi — saf matematik, ekran boyutu varsayimi yok.
 * Ayrinti icin dpi.h. UI tek is parcacigi oldugundan kilit/girisim
 * korumasi bilincli olarak yok. */

#include "dpi.h"
#include <limits.h>

/* Desteklenen olcekler. Yeni olcek eklemek icin buraya eklemek yeter;
 * dpi_set_percent en yakin kuralini kendilginden kapsar. */
static const int s_ok[3] = { 100, 125, 150 };

static int s_pct = 100;   /* global tek ornek (dpi_init sifirlar) */

void dpi_init(void) { s_pct = 100; }

int dpi_get_percent(void) { return s_pct; }

int dpi_scale_x100(void) { return s_pct; }

/* istenen degeri desteklenen en yakin olcege ceker; esit uzaklikta
 * kucuk olan secilir (tutarli, yukari yuvarlama yok). */
static int s_nearest(int pct) {
    int best = s_ok[0];
    for (int i = 1; i < 3; i++) {
        int bd = pct >= best ? pct - best : best - pct;
        int nd = pct >= s_ok[i] ? pct - s_ok[i] : s_ok[i] - pct;
        if (nd < bd) best = s_ok[i];
    }
    return best;
}

int dpi_set_percent(int pct) {
    s_pct = s_nearest(pct);
    return s_pct;
}

/* v * mul / div, yari-degerler sifirdan uzaga yuvarlanir (pozitifte
 * yarim yukselir, negatifte simetrik; her iki yon tutarli). Tasma
 * korumasi: sonuc INT_MIN/INT_MAX'a kenetlenir, sarmaz. 64-bit aralik
 * masaustu derlemesinde de kullanilabilir (uint64_t her yerde var). */
static int s_scale_round(int v, int mul, int div) {
    if (v == 0) return 0;
    int neg = v < 0;
    long long a = neg ? -(long long)v : (long long)v;   /* INT_MIN guvende */
    long long q = (a * mul + div / 2) / div;
    if (q > INT_MAX) return neg ? INT_MIN : INT_MAX;
    return neg ? (int)-q : (int)q;
}

int dpi_px(int base)   { return s_scale_round(base, s_pct, 100); }

int dpi_unpx(int scaled) { return s_scale_round(scaled, 100, s_pct); }

int dpi_font_px(int base_pt) {
    int px = dpi_px(base_pt);
    /* Kucuk puntolar 125/150'de bile okunabilir kalsin: 9 px tabani.
     * 0/negatif girdi anlamsizdir; o da tabana cekilir. */
    if (px < 9) px = 9;
    return px;
}

/* --- birim testleri ------------------------------------------------------
 * Tumu elle dogrulanmis bilinen degerler. Yarim deerler sifirdan uzaga
 * yuvarlandigi icin round-trip (unpx(px(b)) == b) 125 ve 150'de her b'de
 * kanitlanabilir: 125'te b mod 4 siniflari, 150'de cift/tek sinifi. */

bool dpi_selftest(void) {
    const int saved = s_pct;
    bool ok = true;
#define DPI_CHECK(c) do { if (!(c)) ok = false; } while (0)

    dpi_init();
    DPI_CHECK(dpi_get_percent() == 100 && dpi_scale_x100() == 100);

    /* 100: kimlik donusumu */
    DPI_CHECK(dpi_px(0) == 0);
    DPI_CHECK(dpi_px(7) == 7);
    DPI_CHECK(dpi_px(-7) == -7);
    DPI_CHECK(dpi_unpx(7) == 7);
    DPI_CHECK(dpi_font_px(12) == 12);
    DPI_CHECK(dpi_font_px(0) == 9);        /* 9 px tabani */

    /* en yakin desteklenen deger cekmesi */
    DPI_CHECK(dpi_set_percent(112) == 100);
    DPI_CHECK(dpi_set_percent(113) == 125);
    DPI_CHECK(dpi_set_percent(137) == 125);
    DPI_CHECK(dpi_set_percent(138) == 150);
    DPI_CHECK(dpi_set_percent(-500) == 100);
    DPI_CHECK(dpi_set_percent(99999) == 150);
    DPI_CHECK(dpi_get_percent() == dpi_scale_x100());

    /* 125: bilinen degerler (yarim -> sifirdan uzaga) */
    dpi_set_percent(125);
    DPI_CHECK(dpi_px(0) == 0);
    DPI_CHECK(dpi_px(1) == 1);             /* 1.25 -> 1 */
    DPI_CHECK(dpi_px(2) == 3);             /* 2.50 -> 3 */
    DPI_CHECK(dpi_px(3) == 4);             /* 3.75 -> 4 */
    DPI_CHECK(dpi_px(4) == 5);
    DPI_CHECK(dpi_px(8) == 10);
    DPI_CHECK(dpi_px(16) == 20);
    DPI_CHECK(dpi_px(-8) == -10);
    DPI_CHECK(dpi_unpx(10) == 8);
    DPI_CHECK(dpi_unpx(20) == 16);
    DPI_CHECK(dpi_unpx(3) == 2);
    DPI_CHECK(dpi_unpx(1) == 1);           /* 0.80 -> 1 */
    DPI_CHECK(dpi_font_px(8) == 10);
    DPI_CHECK(dpi_font_px(10) == 13);      /* 12.5 -> 13 */
    DPI_CHECK(dpi_font_px(6) == 9);        /* 7.5 -> 8; 9 px tabani korur */

    /* 150: bilinen degerler */
    dpi_set_percent(150);
    DPI_CHECK(dpi_px(0) == 0);
    DPI_CHECK(dpi_px(1) == 2);             /* 1.5 -> 2 */
    DPI_CHECK(dpi_px(2) == 3);
    DPI_CHECK(dpi_px(3) == 5);             /* 4.5 -> 5 */
    DPI_CHECK(dpi_px(4) == 6);
    DPI_CHECK(dpi_px(10) == 15);
    DPI_CHECK(dpi_px(16) == 24);
    DPI_CHECK(dpi_px(-2) == -3);
    DPI_CHECK(dpi_unpx(1) == 1);           /* 0.67 -> 1 */
    DPI_CHECK(dpi_unpx(2) == 1);           /* 1.33 -> 1 */
    DPI_CHECK(dpi_unpx(3) == 2);
    DPI_CHECK(dpi_unpx(5) == 3);
    DPI_CHECK(dpi_unpx(15) == 10);
    DPI_CHECK(dpi_unpx(24) == 16);         /* 16.0 -> 16 */
    DPI_CHECK(dpi_font_px(5) == 9);        /* 7.5 -> 8; taban 9 */
    DPI_CHECK(dpi_font_px(8) == 12);
    DPI_CHECK(dpi_font_px(10) == 15);
    DPI_CHECK(dpi_font_px(-4) == 9);       /* negatif girdi de tabana ceker */

    /* tasma korumasi: kenetleme, sarma yok */
    DPI_CHECK(dpi_px(INT_MAX) == INT_MAX);
    DPI_CHECK(dpi_px(INT_MIN) == INT_MIN);
    DPI_CHECK(dpi_px(20000000) == 30000000);        /* esik alti saglam */
    DPI_CHECK(dpi_unpx(INT_MAX) > 0);               /* ~1.43e9; isaret degmez */

    /* ters cevirme tutarliligi: her olcekte 0..200 round-trip */
    for (int i = 0; i < 3; i++) {
        dpi_set_percent(s_ok[i]);
        for (int b = 0; b <= 200; b++) {
            DPI_CHECK(dpi_px(b) >= b);             /* olcek buyutur/esit */
            DPI_CHECK(dpi_unpx(dpi_px(b)) == b);
        }
        if (s_ok[i] == 100) DPI_CHECK(dpi_px(137) == 137);   /* kimlik */
    }

#undef DPI_CHECK
    s_pct = saved;                          /* cagiranin degerini geri koy */
    return ok;
}
