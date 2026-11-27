#ifndef ARCTIAN_STORAGE_H
#define ARCTIAN_STORAGE_H

#include <stdint.h>

/* Secili (boot/kurulu) blok cihazi uzerinden 512-byte sektor IO. */
int storage_init(void);
/* LBA 64 bit: 2 TB ustu disklerde 32 bitlik LBA yanlis bolgeye denk
 * geliyordu; kapak katman da artik 64 bit tasiyor. */
int storage_read(uint64_t lba, uint32_t count, void *buf);
int storage_write(uint64_t lba, uint32_t count, const void *buf);

/* Secili cihazin/bolumun toplam 512-byte sektor sayisi (0 = bilinmiyor). */
uint64_t storage_sector_count(void);

/* Bolumleme kesfi: ham LBA 0 (0), MBR (1), GPT (2). */
#define STORAGE_PART_RAW 0
#define STORAGE_PART_MBR 1
#define STORAGE_PART_GPT 2

/* Arctian OS GPT Partition Type GUID: {41524354-4941-4E4F-5350-415254495431} */
#define ARCTIAN_GPT_TYPE_GUID_INIT \
    { 0x54, 0x43, 0x52, 0x41, 0x41, 0x49, 0x4F, 0x4E, \
      0x53, 0x50, 0x41, 0x52, 0x54, 0x49, 0x54, 0x31 }

uint64_t storage_partition_base(void);
uint64_t storage_partition_sectors(void);
int      storage_partition_type(void);

/* Biriken (write-back) yazimlari diske indir. */
int storage_flush(void);

typedef struct {
    int (*read)(uint64_t lba, uint32_t count, void *buf);
    int (*write)(uint64_t lba, uint32_t count, const void *buf);
    uint64_t base_lba;
    uint64_t end_lba;
} storage_crypt_t;

void storage_set_crypt(const storage_crypt_t *c);
int  storage_crypt_active(void);
int  storage_raw_read(uint64_t lba, uint32_t count, void *buf);
int  storage_raw_write(uint64_t lba, uint32_t count, const void *buf);

#endif /* ARCTIAN_STORAGE_H */
