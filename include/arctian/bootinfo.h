/*
 * Arctian - onyukleme bilgisi (BIOS/VBE ve UEFI/GOP ortak sozlesmesi)
 *
 * 0.9 oncesi: boot/stage2.asm ekran bilgisini 0x7000'e yaziyordu, alanlar
 * sadece ilk 0x11 baytti. UEFI yolu ayni adresi kullanacak; bu yuzden yeni
 * alanlar SONA eklendi. Eski okuyucular (ilk 0x14 bayt) bozulmaz.
 *
 * Yazan: boot/stage2.asm (BIOS), boot/uefi/ (GOP)
 * Okuyan: kernel_plus/drivers/vesa.c
 *
 * KURAL: BIOS yolu 0x7014..0x7050 arasini SIFIRLAMALI; aksi hâlde onceki
 * calistirmadan kalan cop deger struct_size/source alanlarina dusebilir.
 */
#ifndef ARCTIAN_BOOTINFO_H
#define ARCTIAN_BOOTINFO_H

#include <stdint.h>
#include <stdbool.h>

#define ARCTIAN_BOOTINFO_ADDR   0x7000u
#define ARCTIAN_BOOTINFO_MAGIC  0x41534556u  /* "VESA" - geriye donuk uyum */
#define ARCTIAN_BOOTINFO_SIZE   0x50u        /* ayrilan toplam alan */

/* Kaynak: ekran bilgisini kim kurdu? */
enum {
    ARCTIAN_BOOT_SRC_UNKNOWN = 0,
    ARCTIAN_BOOT_SRC_BIOS    = 1,   /* stage2 + VBE/VBIOS */
    ARCTIAN_BOOT_SRC_UEFI    = 2,   /* UEFI GOP */
};

/* Piksel duzeni. bpp=32 iken X/bos kanalin yerini belirtir. */
enum {
    ARCTIAN_PIXFMT_UNKNOWN = 0,
    ARCTIAN_PIXFMT_BGRX8   = 1,     /* B,G,R,X - VBE 32bpp varsayilani */
    ARCTIAN_PIXFMT_RGBX8   = 2,     /* R,G,B,X - bazi GOP'lar */
    ARCTIAN_PIXFMT_BGRA8   = 3,
    ARCTIAN_PIXFMT_RGBA8   = 4,
};

#define ARCTIAN_BOOT_FLAG_LFB_VALID  (1u << 0)
#define ARCTIAN_BOOT_FLAG_UEFI       (1u << 1)
#define ARCTIAN_BOOT_FLAG_SECBOOT_OFF (1u << 2)

typedef struct {
    /* --- 0.8 uyumlu on ek (offset 0x00 - 0x13) --- */
    uint32_t magic;             /* 0x00 ARCTIAN_BOOTINFO_MAGIC */
    uint32_t fb_phys;           /* 0x04 LFB fiziksel adres */
    uint16_t pitch;             /* 0x08 satir bayt cinsinden */
    uint16_t _pad0;             /* 0x0A */
    uint16_t width;             /* 0x0C */
    uint16_t height;            /* 0x0E */
    uint8_t  bpp;               /* 0x10 */
    uint8_t  _pad1[3];          /* 0x11 */
    /* --- 0.9 ek alanlari (offset 0x14+) --- */
    uint32_t struct_size;       /* 0x14 bu yapinin bayt boyu */
    uint16_t format;            /* 0x18 ARCTIAN_PIXFMT_* */
    uint16_t source;            /* 0x1A ARCTIAN_BOOT_SRC_* */
    uint32_t flags;             /* 0x1C ARCTIAN_BOOT_FLAG_* */
    uint32_t fb_size;           /* 0x20 LFB bayt; 0 = bilinmiyor */
    uint32_t red_mask;          /* 0x24 */
    uint32_t green_mask;        /* 0x28 */
    uint32_t blue_mask;         /* 0x2C */
    uint32_t reserved_mask;     /* 0x30 */
    uint32_t _reserved[7];      /* 0x34 - 0x4F */
} arctian_bootinfo_t;

/* Dogrulama: derleme zamani yerlesim kontrolu. */
_Static_assert(sizeof(arctian_bootinfo_t) == ARCTIAN_BOOTINFO_SIZE,
               "arctian_bootinfo_t boyu ARCTIAN_BOOTINFO_SIZE ile ayni olmali");
_Static_assert(__builtin_offsetof(arctian_bootinfo_t, struct_size) == 0x14,
               "eski alanlar 0x14'ten once bitmeli");

static inline arctian_bootinfo_t *arctian_bootinfo(void) {
    return (arctian_bootinfo_t *)(uintptr_t)ARCTIAN_BOOTINFO_ADDR;
}

static inline bool arctian_bootinfo_valid(const arctian_bootinfo_t *bi) {
    if (!bi || bi->magic != ARCTIAN_BOOTINFO_MAGIC) return false;
    if (!bi->fb_phys || !bi->width || !bi->height) return false;
    if (bi->bpp != 24 && bi->bpp != 32) return false;
    return true;
}

static inline bool arctian_bootinfo_is_uefi(const arctian_bootinfo_t *bi) {
    if (!bi || bi->magic != ARCTIAN_BOOTINFO_MAGIC) return false;
    return bi->source == ARCTIAN_BOOT_SRC_UEFI ||
           (bi->flags & ARCTIAN_BOOT_FLAG_UEFI) != 0;
}

#endif /* ARCTIAN_BOOTINFO_H */
