/*
 * Arctian UEFI onyukleyici - PE/COFF yerlesimi (GNU ld -m i386pep).
 *
 * Makefile su komutu calistirir:
 *   ld -m i386pep -nostdlib -T boot/uefi/linker.ld -o build/uefi/BOOTX64.EFI ...
 * Girdi sembolu burada tanimlanir (Makefile -e gecirmiyor). Linux gcc
 * ELF nesne uretir; nasm -f win64 COFF uretir - i386pep her ikisini alir.
 * -mabi=ms ile uretilen .pdata/.xdata/.eh_frame BOŞ veridir (EFI'da unwind
 * yok) /DISCARD/ ile atilir; .drectve PE girdisi degildir, atilmali.
 */
ENTRY(efi_entry)
OUTPUT_FORMAT(pei-x86-64)

/* GNU ld i386pep notlari:
 * - ImageBase yalnız ld komut satirindan (--image-base=0x400000, Makefile
 *   efi kurali) ayarlanir; linker script'teki __image_base__ sembol de ayni
 *   degere sabitlendi ki VMA/baslik tutarli kalsin. VMA tabanin ALTINDA
 *   kalirsa "section below image base" + kaymis RVAlar, USTUNDE kalirsa
 *   R_X86_64_32 tasma (-fno-pic) olur; ikisi de OVMF'de Unsupported/#GP.
 * - Subsystem 10 (EFI application) Makefile'dan gelir; 3 (Windows CUI)
 *   iken EDK2 LoadImage image'i reddeder. */
__image_base__ = 0x400000;

SECTIONS
{
    /* VMA tabanin hemen ustunde baslar (RVAlar ~0x200'den). */
    . = 0x400000 + SIZEOF_HEADERS;

    .text : { *(.text .text.*) }

    .data : {
        *(.data .data.*)
        *(.rodata .rodata.*)   /* Linux gcc: const veriler .rodata'da */
        *(.rdata .rdata.*)     /* COFF nesneler (nasm) icin */
        *(.bss .bss.*)
        *(COMMON)
    }

    /* GNU ld PE yurutucusu BASE_RELOCATION_TABLE'i bu bolumden kurar;
     * OVMF image'i tercih edilen tabana koyamazsa reloc sarttir. */
    .reloc : { *(.reloc) }

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame.*)
        *(.eh_frame_hdr .eh_frame_hdr.*)
        *(.pdata .pdata.*)
        *(.xdata .xdata.*)
        *(.drectve)
    }
}
