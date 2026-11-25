/*
 * Arctian UEFI onyukleyici - UEFI'ye ozel boot info (0x7100).
 *
 * bootinfo.h (0x7000) BIOS yoluyla ORTAK sozlesmedir; buradaki alanlar
 * yalniz UEFI yolunu ilgilendirdigi icin AYRI yapida tutulur. bootinfo.h
 * degistirilmez. Kernel 0x7000 ve 0x7100'u birlikte okur.
 *
 * Yazan: boot/uefi/ (fs.c, memmap.c, rsdp.c). Okuyan: kernel (paket 3+).
 * Not: memmap alani BootServices cagrilarinin ardindan da gecerlidir
 * (AllocatePool/EfiLoaderData); ExitBootServices sonrasi kullanilacaksa
 * kernel kendi haritasini cikarmali ya da tamponu EfiRuntimeServicesData
 * olarak ayirmali (paket 3 karari).
 */
#ifndef ARCTIAN_UEFI_INFO_H
#define ARCTIAN_UEFI_INFO_H

#include <stdint.h>

#define ARCTIAN_UEFIINFO_ADDR  0x7100u
/* "UEFI" - kucuk uclu okununca 'U' en dusuk baytta. */
#define ARCTIAN_UEFIINFO_MAGIC 0x49464555u

typedef struct {
    uint32_t magic;             /* 0x00 ARCTIAN_UEFIINFO_MAGIC */
    uint32_t struct_size;       /* 0x04 sizeof(arctian_uefiinfo_t) */
    uint64_t memmap_addr;       /* 0x08 GetMemoryMap tamponu (0 = yok) */
    uint64_t memmap_bytes;      /* 0x10 tamponun dolu kismi (bayt) */
    uint32_t memmap_count;      /* 0x18 giris sayisi */
    uint32_t memmap_desc_size;  /* 0x1C tanimlayici boyu (genelde 40) */
    uint64_t memmap_key;        /* 0x20 son MapKey (ExitBootServices icin) */
    uint64_t rsdp_addr;         /* 0x28 RSDP fiziksel adresi (0 = yok) */
    uint32_t rsdp_rev2;         /* 0x30 1 = ACPI 2.0 GUID ile bulundu */
    uint32_t rsdp_sum_ok;       /* 0x34 ilk 20 baytin checksum'i 0 mu */
    uint64_t kernel_base;       /* 0x38 okunan dosyanin havuz adresi */
    uint64_t kernel_size;       /* 0x40 dosya boyu (bayt) */
    uint32_t secureboot;        /* 0x48 SecureBoot degiskeni (0/1) */
    uint32_t _pad0;             /* 0x4C */
    uint64_t fb_phys;           /* 0x50 tam 64-bit GOP FrameBufferBase */
    uint64_t fb_size;           /* 0x58 tam 64-bit GOP FrameBufferSize */
} arctian_uefiinfo_t;

_Static_assert(sizeof(arctian_uefiinfo_t) == 0x60,
               "uefi_info 0x7100-0x7160 arasina sigmali");
_Static_assert(__builtin_offsetof(arctian_uefiinfo_t, memmap_key) == 0x20,
               "memmap_key sabit yerinde kalmali");
_Static_assert(__builtin_offsetof(arctian_uefiinfo_t, fb_phys) == 0x50,
               "fb_phys 0x7150 adresinde olmali");
_Static_assert(__builtin_offsetof(arctian_uefiinfo_t, fb_size) == 0x58,
               "fb_size 0x7158 adresinde olmali");

static inline arctian_uefiinfo_t *arctian_uefiinfo(void) {
    return (arctian_uefiinfo_t *)(uintptr_t)ARCTIAN_UEFIINFO_ADDR;
}

#endif /* ARCTIAN_UEFI_INFO_H */
