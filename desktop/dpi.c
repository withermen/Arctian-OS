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
