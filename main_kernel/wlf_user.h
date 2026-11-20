/**
 * Arctian - .wlf native ikili yukleyici (kullanici adres alani)
 * ============================================================
 *
 * Duz (flat) WLF_KIND_NATIVE imajini ayri bir PML4'te, kullanici sanal
 * adres araligina yukler:
 *
 *   [ kod  : image_size, X (yazilamaz) ]
 *   [ veri : bss_size,   NX + W       ]
 *   [ yigin: 16 KB,      NX + W       ]
 *
 * Basliktan yalniz image_size, bss_size ve entry_off okunur (wlF.h
 * wlf_header_t). Sozlesme geregi kod load_addr'a gore baglanmistir;
 * bu yukleyici imaji VMM_USER_BASE'e yerlestirdigi icin imajin konumdan
 * bagimsiz (PIC) olmasi ya da VMM_USER_BASE icin baglanmis olmasi
 * gerekir. Basliktaki load_addr bilgi amaciyla out->load_addr'a konur;
 * su an yeniden yerlesim (relocation) YAPILMAZ.
 *
 * W^X: kod sayfalari yazilamaz, veri ve yigin NX isaretlenir. NX biti
 * yalnizca IA32_EFER.NXE=1 iken yazilir (vmm_space_nx_ok); aksi halde
 * veri/yigin yazilabilir ama yurutulebilir kalir ve out->nx = 0 olur.
 */
#ifndef ARCTIAN_WLF_USER_H
#define ARCTIAN_WLF_USER_H

#include <stdint.h>
#include "wlf.h"

#define WLF_USER_MAGIC       0x57554631u   /* "WUF1" */
#define WLF_USER_FREED       0x46524545u   /* "FREE" - cift serbest korumasi */
#define WLF_USER_STACK_SIZE  (16u * 1024u) /* 4 sayfa yigin */

/* wlf_user_t.status / wlf_user_start hata kodlari (0 = basarili). */
#define WLF_USER_OK            0
#define WLF_USER_ERR_ARG      (-1)   /* NULL / out dolu / uzunluk 0 */
#define WLF_USER_ERR_HEADER   (-2)   /* sihir, surum, tur veya alan bozuk */
#define WLF_USER_ERR_RANGE    (-3)   /* goruntu boyu dosya disinda */
#define WLF_USER_ERR_ENTRY    (-4)   /* giris noktasi kod araliginda degil */
#define WLF_USER_ERR_TOO_BIG  (-5)   /* kullanici adres uzayina sigmiyor */
#define WLF_USER_ERR_NO_SPACE (-6)   /* PML4 ayrilamadi */
#define WLF_USER_ERR_NO_MEM   (-7)   /* cerceve / kmalloc basarisiz */
#define WLF_USER_ERR_MAP      (-8)   /* sayfa eslemesi basarisiz */
#define WLF_USER_ERR_STATE    (-9)   /* zaten serbest birakilmis / gecersiz */

typedef struct {
    uint32_t  magic;         /* WLF_USER_MAGIC; serbest birakilinca FREED */
    int32_t   status;        /* WLF_USER_* ; 0 = yuklendi */
    uint64_t  space;         /* yeni PML4 (fiziksel adres) */
    uint64_t  base_va;       /* kod baslangici (VMM_USER_BASE) */
    uint64_t  entry_va;      /* giris noktasi (base_va + entry_off) */
    uint64_t  stack_top;     /* yiginin ustu (ilk rsp) */
    uint64_t *frames;        /* kmalloc: fiziksel cerceve listesi */
    uint32_t  frame_count;   /* listedeki cerceve sayisi */
    uint32_t  code_pages;
    uint32_t  data_pages;
    uint32_t  stack_pages;
    uint32_t  image_size;
    uint32_t  bss_size;
    uint32_t  entry_off;
    uint32_t  load_addr;     /* basliktaki baglama adresi (bilgi) */
    uint32_t  nx;            /* 1: veri/yigin NX olarak isaretlendi */
} wlf_user_t;

/** Imaji yukler ve giris adresini dondurur. Hatali baslikta 0 doner ve
 *  out->status negatif hata kodunu tasir. Basarida out->status = OK.
 *  out sifirlanmis olmak zorunda degildir (icerde sifirlanir) ama daha
 *  once yuklenmis bir alan iceriyorsa once wlf_user_free cagrilmalidir. */
uint64_t wlf_user_start(const void *image, uint64_t size, wlf_user_t *out);

/** Sayfalari ve alani geri verir. Ayni yapi icin ikinci cagri -9 doner
 *  (cift serbest birakma yok). 0 = basarili. */
int wlf_user_free(wlf_user_t *out);

#endif /* ARCTIAN_WLF_USER_H */
