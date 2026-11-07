/**
 * Arctian Superblock + Module Table
 * Disk uzerindeki sabit LBA'da tutulur; modulleri ve kurulum durumunu tanimlar.
 */
#ifndef ARCTIAN_SUPERBLOCK_H
#define ARCTIAN_SUPERBLOCK_H

#include <stdint.h>

#define SUPERBLOCK_MAGIC      0x54435241u /* "ARCT" little-endian */
#define SUPERBLOCK_VERSION    1u
#define SUPERBLOCK_LBA        2048u
#define SUPERBLOCK_SECTORS    1u
#define MODULE_TABLE_LBA      2049u
#define STATE_LBA             2050u
#define MODULE_DATA_START_LBA 4096u
#define MAX_MODULES           32

/* 0.8.2: B yuvasi (cekirdek guncellemesi). Moduller B tablosundan once
 * bitmeli; AFS yeni duzende 65536'dan baslar, boylece B bolgesi kullanici
 * dosyalarina dokunmaz. Eski disklerde (AFS 20000) bu bolge dolu oldugu
 * icin kernel update reddedilir. */
#define SLOTB_TABLE_LBA       32768u   /* B yuvasi modul tablosu (1 sektor) */
#define SLOTB_DATA_LBA        32832u   /* B yuvasi modullerinin ilk sektoru */
#define SLOTB_MAX_END         65536u   /* B verisi bu sektorde BITMELI */
#define SLOTB_TABLE_MAGIC     0x42544253u /* "SBTB" */

/* superblock.flags */
#define SB_FLAG_INSTALLED   0x00000001u

typedef struct __attribute__((packed)) {
    uint32_t magic;                 /* SUPERBLOCK_MAGIC */
    uint16_t version;               /* SUPERBLOCK_VERSION */
    uint16_t header_size;           /* sizeof(superblock_t) */
    uint32_t flags;                 /* SB_FLAG_* */
    uint32_t module_count;
    uint32_t module_tab_lba;        /* modul tablosunun LBA'si */
    uint32_t module_tab_sectors;
    uint32_t state_lba;             /* kurulum durumu (ayniysa installed) */
    uint32_t reserved;
    uint32_t checksum;              /* header uzerinde amod_checksum */
} superblock_t;

typedef struct __attribute__((packed)) {
    char     name[32];
    uint16_t type;                  /* amod_type_t */
    uint16_t abi_version;
    uint32_t flags;
    uint32_t load_addr;
    uint32_t image_lba;
    uint32_t image_sectors;
    uint32_t image_size;            /* header HARIC */
    uint32_t entry_off;
    uint32_t checksum;
} module_entry_t;

#endif /* ARCTIAN_SUPERBLOCK_H */
