/**
 * Arctian - Merkezi bellek haritasi
 * Tum moduller bu sabitleri kullanir; cakisma olmamasi icin tek kaynak.
 */
#ifndef ARCTIAN_MEMORY_MAP_H
#define ARCTIAN_MEMORY_MAP_H

/* --- Yukleme / gecici --- */
#define STAGE1_LOAD_ADDR        0x00007C00u
#define STAGE2_LOAD_ADDR        0x00010000u
#define VESA_BOOT_INFO_ADDR     0x00007000u
#define PML4_ADDR               0x00008000u
#define PDPT_ADDR               0x00009000u
#define PD_LOW_ADDR             0x0000A000u
#define PD_FB_ADDR              0x0000D000u
#define BOOT_STACK_TOP          0x0008FF00u

/* --- Modul bolgeleri (sabit adresli duz .bin) --- */
#define MAIN_KERNEL_BASE        0x00100000u
#define MAIN_KERNEL_MAX         (1u * 1024u * 1024u)

#define INSTALL_BASE            0x00200000u
#define INSTALL_MAX             (1u * 1024u * 1024u)

/* 0.9: kernel_plus bss 0xCC4000'e, desktop image+bss ~0x2AC0000'e ulasti;
 * eski 7/8 MB sinirlari asildigi icin bolgeler genisletildi/tasindi. */
#define KERNEL_PLUS_BASE        0x00300000u
#define KERNEL_PLUS_MAX         (13u * 1024u * 1024u)

#define DESKTOP_BASE            0x01000000u
#define DESKTOP_MAX             (48u * 1024u * 1024u)

/* Dinamik (DuckStore) uygulama bolgesi. Desktop modulu (image+bss) 0x02AC0000
 * civarina kadar uzandigi icin bu bolge 0x04000000'den baslar. */
#define APP_BASE                0x04000000u
#define APP_MAX                 (40u * 1024u * 1024u)

/* 0.6: .wlf native uygulamalari icin AYRI bolge. DPK'lar APP_BASE'den
 * baslar ve 2 MB'luk dilimler kullanir (0x04000000, 0x04200000, ...);
 * onceki surumde .wlf de 0x01C00000 kullaniyordu ve 3. DPK ile CAKISIYORDU
 * (ikisi de ayni adrese yuklenip birbirini bozuyordu). */
#define WLF_BASE                0x06800000u
#define WLF_MAX                 (8u * 1024u * 1024u)

#define DISK_BUF_BASE           0x07000000u
#define DISK_BUF_SIZE           (16u * 1024u * 1024u)

/* Blok onbellegi + ATA DMA tamponu icin ayrilmis bolge. Modul BSS'ine
 * konursa main_kernel'in 512KB'lik yiginini modul bolgesine tasirir; bu
 * yuzden sabit, kimlik-eslemeli bir alanda tutulur. */
#define BLK_SCRATCH_BASE        0x08000000u
#define BLK_SCRATCH_SIZE        (1u * 1024u * 1024u)

#define BACKBUFFER_BASE         0x09000000u
#define BACKBUFFER_MAX          (16u * 1024u * 1024u)

/* --- Sanal bellek yonetimi (VMM / paging) --- */
#define VMM_HIGHER_BASE         0xFFFF800000000000ull
#define VMM_HIGHER_PML4_INDEX   256u
#define VMM_USER_BASE           0x0000008000000000ull
#define VMM_USER_PML4_INDEX     1u

/* VMM durum sayfasi (4 KB, main_kernel + kernel_plus ortak sabit adres). */
#define VMM_STATE_ADDR          0x0A000000u

/* Kernel heap (kmalloc): 161 MB .. 224 MB (63 MB).
 * 256 MB sistemde bile heap ile Ring-3 fiziksel cerceve havuzu cakismaz. */
#define HEAP_BASE               0x0A100000u
#define HEAP_MAX                (63u * 1024u * 1024u)

/* Kimlik (identity) esleme baslangic boyutu: stage2 0-1 GB kurar;
 * vmm_init() >1 GB takili RAM'i 2 MB huge page'lerle dinamik genisletir. */
#define IDENTITY_MAP_SIZE       (1u * 1024u * 1024u * 1024u)
#define IDENTITY_MAP_PD_ENTRIES 512u

/* Ring-3 fiziksel cerceve havuzu tabani (224 MB).
 * Gercek bitis adresi (pool_end), dinamik refcount dizisi ve >4 GB yuksek
 * bellek havuzu vmm_init() tarafindan takili RAM'e gore olceklenir. */
#define VMM_POOL_BASE           0x0E000000u
#define VMM_POOL_SIZE           (96u * 1024u * 1024u)  /* yedek varsayilan */
#define VMM_REFCOUNT_ADDR       0x0E000000u            /* dinamik: st->refcount_addr */
#define VMM_FRAMES_BASE         0x0E010000u            /* dinamik: st->frames_base */

#endif /* ARCTIAN_MEMORY_MAP_H */
