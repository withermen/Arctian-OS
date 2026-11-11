#ifndef ARCTIAN_BOOTANIM_H
#define ARCTIAN_BOOTANIM_H

#include <stdint.h>

/*
 * Acilis animasyonu (kernel_plus).
 *
 * Sahne, Arctian acilis animasyonu referansindan birebir alinmistir:
 * 288 cokgen (facet) arka plan, 200x200 logo, "ARCTIAN OS" basligi ve
 * 360x6 ilerleme cubugu. Cubuk, gercek acilis adimlarindan beslenir;
 * sahte zamanlayici yoktur.
 *
 * Veri tablolari bootanim_data.c (uretilmis dosya) icindedir.
 */

typedef struct {
    int16_t  x[3];       /* ucgen koseleri, 1/10 px, ekran koordinati */
    int16_t  y[3];
    uint16_t hi[3];      /* animasyon basi rengi, 0..65535 kanal basina */
    uint16_t lo[3];      /* animasyon sonu rengi */
    uint16_t delay;      /* baslangic gecikmesi (ms) */
    uint16_t tw_dur;     /* parildama suresi (ms), 0 = yok */
    uint16_t tw_delay;   /* parildama gecikmesi (ms) */
} banim_facet_t;

#define BANIM_MAX_FACETS 288u

extern const banim_facet_t banim_facets[];
extern const uint32_t banim_facet_count;
extern const uint16_t banim_ease_out[257];
extern const uint16_t banim_ease_in_out[257];
extern const uint16_t banim_ease_logo[257];

/* VESA + arkatampon hazir olduktan sonra cagrilir. */
void bootanim_start(void);

/* Gercek acilis ilerlemesi (binde 0..1000). */
void bootanim_set(uint32_t permille);
void bootanim_advance(uint32_t permille);

/*
 * Ilerleme 1000'e ulasana ve en az min_ms gecene kadar bekler,
 * sahneyi karartir ve animasyon isini durdurur.
 */
void bootanim_finish(uint32_t min_ms);

#endif /* ARCTIAN_BOOTANIM_H */
