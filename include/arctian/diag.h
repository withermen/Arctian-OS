/*
 * Arctian - teshis (system check) kayitlari
 *
 * Amac: suruculer ve servisler "su bilesen calisiyor/calismiyor" bilgisini
 * tek yerde toplasin; Ayarlar > Sistem Denetimi ekrani bunu gostersin.
 *
 * Tasarim: sabit dizi, dinamik bellek yok, kilit yok (kayitlar acilista ve
 * nadir olaylarda yazilir). Metinler kisaltilir; tam cumle beklenmez.
 *
 * Uygulama: lib/diag.c
 * Kullanan : S2 (pil/termal), S4 (ag), S6 (CPU), S3 (ekran; syscall ile)
 *
 * NOT: desktop ayri adres alaninda calisir; bu tabloyu dogrudan okuyamaz.
 * 0.9'da kernel tarafi `diag_dump()` ile seri porta basar; kullanici arayuzu
 * icin syscall S5/S6 sirasinda eklenecek (plan §20).
 */
#ifndef ARCTIAN_DIAG_H
#define ARCTIAN_DIAG_H

#include <stdint.h>
#include <stdbool.h>

#define DIAG_MAX        32
#define DIAG_NAME_LEN   24
#define DIAG_DETAIL_LEN 72

typedef struct {
    char     name[DIAG_NAME_LEN];      /* "nvme0", "e1000e", "batarya" */
    char     detail[DIAG_DETAIL_LEN];  /* "250 GB, PCIe 3.0 x4" */
    uint32_t code;                     /* surucuye ozel hata kodu, 0 = yok */
    uint8_t  ok;                       /* 1 = calisiyor, 0 = sorunlu */
    uint8_t  seen;                     /* 1 = bu onyuklemede denendi */
} diag_entry_t;

void diag_reset(void);

/* Ayni isim varsa guncellenir (tekrar kayit sismesi olmaz). */
void diag_set(const char *name, bool ok, uint32_t code, const char *detail);

/* Kisa yol: ok=true ise kod 0. */
void diag_ok(const char *name, const char *detail);
void diag_fail(const char *name, uint32_t code, const char *detail);

int  diag_count(void);
const diag_entry_t *diag_get(int i);

/* Tum kayitlar ok mu? Hic kayit yoksa false (bilinmiyor). */
bool diag_all_ok(void);

/* Seri porta ozet bas (klog varsa oradan, yoksa dogrudan). */
void diag_dump(void);

#endif /* ARCTIAN_DIAG_H */
