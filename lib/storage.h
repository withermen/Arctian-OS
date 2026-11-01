#ifndef ARCTIAN_STORAGE_H
#define ARCTIAN_STORAGE_H

#include <stdint.h>

/* Secili (boot/kurulu) blok cihazi uzerinden 512-byte sektor IO. */
int storage_init(void);
/* LBA 64 bit: 2 TB ustu disklerde 32 bitlik LBA yanlis bolgeye denk
 * geliyordu; kapak katman da artik 64 bit tasiyor. */
int storage_read(uint64_t lba, uint32_t count, void *buf);
int storage_write(uint64_t lba, uint32_t count, const void *buf);

/* Secili cihazin toplam 512-byte sektor sayisi (0 = bilinmiyor). */
uint64_t storage_sector_count(void);

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
