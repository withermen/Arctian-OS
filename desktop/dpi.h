#ifndef ARCTIAN_DPI_H
#define ARCTIAN_DPI_H

/* DPI olcekleme cekirdegi (0.9 dalga 1, paket 1).
 *
 * Saf olcek matematigi: hicbir ekran boyutu varsaymaz, sabit piksel
 * icermez. Gercek mantiksal alan cagiran tarafca fiziksel / olcek ile
 * bulunur (paket 2'de bootinfo'dan okunur). 1280x720 yalnizca tasarim
 * referansidir; buraya gomulmez (mudur + S1 karari).
 *
 * UI tek is parcacigi: global tek ornek, kilit yok. */

#include <stdbool.h>

void dpi_init(void);              /* varsayilan 100 */
int  dpi_set_percent(int pct);    /* yalniz 100/125/150; digeri en yakina
                                   * cekilir; etkin degeri dondurur */
int  dpi_get_percent(void);       /* 100 / 125 / 150 */
int  dpi_scale_x100(void);        /* etkin olcek x100 (ayni deger) */
int  dpi_px(int base);            /* taban olcu -> olcekli piksel;
                                   * yarim degerler sifirdan uzaga
                                   * tutarli yuvarlanir; tasma yok */
int  dpi_unpx(int scaled);        /* olcekli piksel -> taban olcu;
                                   * dpi_px'in tersi (round-trip) */
int  dpi_font_px(int base_pt);    /* punto -> px; kucuk degerlerde en az
                                   * 9 px korunur (okunabilirlik) */

/* Birim testleri: bilinen degerler, en yakin deger cekmesi, ters cevirme
 * tutarliligi, tasma/negatif korumasi. Cagrilmazsa derlemeye zarari yok;
 * ilerideki test hedefi cagracak. Girdigi olcek degerini geri yukariler. */
bool dpi_selftest(void);

#endif /* ARCTIAN_DPI_H */
