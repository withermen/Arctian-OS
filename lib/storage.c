/**
 * Arctian storage facade: birlesik blok katmanina delege eder.
 * (ATA / AHCI-SATA / NVMe destegi blockdev + blk_* suruculerinde.)
 * Ham LBA 0 disklerin yani sira GPT ve MBR bolum tablolarini tarayarak
 * Arctian OS bolumunun taban LBA ofsetini dinamik olarak saptar.
 */
#pragma GCC optimize("Os")
#include "storage.h"
#include "blockdev.h"
#include "superblock.h"
#include "kernel.h"

#define AFS_SIG_MAGIC 0x53464121u /* "!AFS" */

static int s_cur = -1;
static uint64_t s_part_base = 0;
static uint64_t s_part_sectors = 0;
static int s_part_type = STORAGE_PART_RAW;
static storage_crypt_t s_crypt;
static int s_crypt_on = 0;

static uint32_t st_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t st_le64(const uint8_t *p) {
    return (uint64_t)st_le32(p) | ((uint64_t)st_le32(p + 4) << 32);
}

static int guid_eq_bytes(const uint8_t a[16], const uint8_t b[16]) {
    for (int i = 0; i < 16; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

static int guid_is_zero(const uint8_t g[16]) {
    for (int i = 0; i < 16; i++)
        if (g[i] != 0) return 0;
    return 1;
}

static int has_magic_at(int dev, uint64_t lba, uint64_t total_secs, uint32_t expected) {
    if (lba >= total_secs) return 0;
    uint8_t buf[512];
    if (blockdev_read(dev, lba, 1, buf) != 0) return 0;
    return st_le32(buf) == expected;
}

/* Bir GPT veya MBR bolumunun Arctian OS bolumu olup olmadigini sinirlar:
 * 1) start_lba + SUPERBLOCK_LBA'da "ARCT" (tam bolum ici imaj)
 * 2) start_lba'da dogrudan "ARCT" (superblock bolum basinda)
 * 3) start_lba'da dogrudan "!AFS" veya Arctian tip GUID/tipi
 * Donus: 1 = eslesti (out_base, out_secs dolu), 0 = hayir. */
static int probe_partition_candidate(int dev, uint64_t total_secs,
                                     uint64_t start_lba, uint64_t part_secs,
                                     int type_match,
                                     uint64_t *out_base, uint64_t *out_secs) {
    if (start_lba == 0 || part_secs == 0 || start_lba >= total_secs) return 0;
    if (part_secs > total_secs - start_lba) part_secs = total_secs - start_lba;

    if (part_secs > SUPERBLOCK_LBA &&
        has_magic_at(dev, start_lba + SUPERBLOCK_LBA, total_secs, SUPERBLOCK_MAGIC)) {
        *out_base = start_lba;
        *out_secs = part_secs;
        return 1;
    }
    if (has_magic_at(dev, start_lba, total_secs, SUPERBLOCK_MAGIC)) {
        /* Superblock bolumun 0. sektorundeyse ve bolum LBA >= 2048 ise
         * modul/tablo LBA'lariyla uyumlu taban = start_lba - SUPERBLOCK_LBA. */
        if (start_lba == SUPERBLOCK_LBA) {
            *out_base = 0;
            *out_secs = total_secs;
        } else if (start_lba > SUPERBLOCK_LBA) {
            *out_base = start_lba - SUPERBLOCK_LBA;
            *out_secs = part_secs + SUPERBLOCK_LBA;
        } else {
            *out_base = start_lba;
            *out_secs = part_secs;
        }
        return 1;
    }
    if (has_magic_at(dev, start_lba, total_secs, AFS_SIG_MAGIC) || type_match) {
        *out_base = start_lba;
        *out_secs = part_secs;
        return 1;
    }
    return 0;
}

static int probe_gpt(int dev, uint64_t total_secs, uint64_t *out_base, uint64_t *out_secs) {
    static const uint8_t arctian_guid[16] = ARCTIAN_GPT_TYPE_GUID_INIT;
    if (total_secs < 34ull) return 0;

    uint8_t lba1[512];
    if (blockdev_read(dev, 1, 1, lba1) != 0) return 0;
    static const char sig[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };
    for (int i = 0; i < 8; i++)
        if (lba1[i] != (uint8_t)sig[i]) return 0;

    uint32_t hdr_sz = st_le32(lba1 + 12);
    if (hdr_sz < 92u || hdr_sz > 512u) return 0;

    uint64_t part_lba = st_le64(lba1 + 72);
    uint32_t num_ent  = st_le32(lba1 + 80);
    uint32_t ent_sz   = st_le32(lba1 + 84);
    if (ent_sz < 128u || ent_sz > 512u || (512u % ent_sz) != 0u) return 0;
    if (part_lba < 2ull || part_lba >= total_secs) return 0;
    if (num_ent > 128u) num_ent = 128u;

    uint32_t per_sec = 512u / ent_sz;
    uint32_t nsecs = (num_ent + per_sec - 1u) / per_sec;
    uint8_t psec[512];

    /* Iki gecis: once superblock/AFS imzali bolum, sonra yalniz Arctian GUID. */
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t s = 0; s < nsecs; s++) {
            if (part_lba + s >= total_secs) break;
            if (blockdev_read(dev, part_lba + s, 1, psec) != 0) break;
            for (uint32_t e = 0; e < per_sec; e++) {
                uint32_t idx = s * per_sec + e;
                if (idx >= num_ent) break;
                const uint8_t *ent = psec + e * ent_sz;
                if (guid_is_zero(ent)) continue;
                uint64_t first_lba = st_le64(ent + 32);
                uint64_t last_lba  = st_le64(ent + 40);
                if (first_lba == 0 || last_lba < first_lba || last_lba >= total_secs)
                    continue;
                int is_arct_guid = guid_eq_bytes(ent, arctian_guid);
                uint64_t psecs = last_lba - first_lba + 1ull;
                if (pass == 0) {
                    if (probe_partition_candidate(dev, total_secs, first_lba, psecs,
                                                  0, out_base, out_secs))
                        return 1;
                } else if (is_arct_guid) {
                    *out_base = first_lba;
                    *out_secs = psecs;
                    return 1;
                }
            }
        }
    }
    return 0;
}

static int probe_mbr(int dev, uint64_t total_secs, uint64_t *out_base, uint64_t *out_secs) {
    uint8_t lba0[512];
    if (blockdev_read(dev, 0, 1, lba0) != 0) return 0;
    if (lba0[510] != 0x55u || lba0[511] != 0xAAu) return 0;

    /* Koruyucu MBR (0xEE) varsa GPT islemelidir; ham MBR'yi atla. */
    for (int p = 0; p < 4; p++) {
        if (lba0[446 + p * 16 + 4] == 0xEEu) return 0;
    }

    for (int pass = 0; pass < 2; pass++) {
        for (int p = 0; p < 4; p++) {
            const uint8_t *ent = lba0 + 446 + p * 16;
            uint8_t ptype = ent[4];
            if (ptype == 0) continue;
            uint32_t start = st_le32(ent + 8);
            uint32_t nsecs = st_le32(ent + 12);
            if (start == 0 || nsecs == 0) continue;
            if (probe_partition_candidate(dev, total_secs, start, nsecs,
                                          (pass == 1 && ptype == 0xACu),
                                          out_base, out_secs))
                return 1;
        }
    }
    return 0;
}

int storage_init(void) {
    int n = blockdev_init();
    if (n <= 0) {
        s_cur = -1;
        s_part_base = 0;
        s_part_sectors = 0;
        s_part_type = STORAGE_PART_RAW;
        return -1;
    }

    s_cur = -1;
    s_part_base = 0;
    s_part_sectors = 0;
    s_part_type = STORAGE_PART_RAW;

    for (int i = 0; i < n; i++) {
        blockdev_info_t info;
        if (blockdev_info(i, &info) != 0 || info.sectors == 0) continue;

        uint64_t base = 0, secs = info.sectors;
        if (probe_gpt(i, info.sectors, &base, &secs)) {
            s_cur = i;
            s_part_base = base;
            s_part_sectors = secs;
            s_part_type = STORAGE_PART_GPT;
            break;
        }
        if (probe_mbr(i, info.sectors, &base, &secs)) {
            s_cur = i;
            s_part_base = base;
            s_part_sectors = secs;
            s_part_type = STORAGE_PART_MBR;
            break;
        }
        if (has_magic_at(i, SUPERBLOCK_LBA, info.sectors, SUPERBLOCK_MAGIC)) {
            s_cur = i;
            s_part_base = 0;
            s_part_sectors = info.sectors;
            s_part_type = STORAGE_PART_RAW;
            break;
        }
    }

    if (s_cur < 0) {
        blockdev_info_t info0;
        s_cur = 0;
        s_part_base = 0;
        s_part_type = STORAGE_PART_RAW;
        s_part_sectors = (blockdev_info(0, &info0) == 0) ? info0.sectors : 0;
    }
    return 0;
}

uint64_t storage_partition_base(void)    { return s_part_base; }
uint64_t storage_partition_sectors(void) { return s_part_sectors; }
int      storage_partition_type(void)    { return s_part_type; }

int storage_raw_read(uint64_t lba, uint32_t count, void *buf) {
    if (s_cur < 0) return -1;
    return blockdev_read(s_cur, s_part_base + lba, count, buf);
}

int storage_raw_write(uint64_t lba, uint32_t count, const void *buf) {
    if (s_cur < 0) return -1;
    return blockdev_write(s_cur, s_part_base + lba, count, buf);
}

void storage_set_crypt(const storage_crypt_t *c) {
    /* Sifreleme durumu degisirken onbellekteki sektorlerin anlami degisir:
     * kirli veriyi indir + onbellegi temizle tek atomik hareket olsun ki
     * iki adim arasina eski anahtarla yazim sikismasin. */
    blockdev_cache_reset();
    if (!c) { s_crypt_on = 0; return; }
    s_crypt = *c;
    s_crypt_on = 1;
}

int storage_flush(void) {
    if (s_cur < 0) return -1;
    return blockdev_flush(s_cur);
}

int storage_crypt_active(void) { return s_crypt_on; }

int storage_read(uint64_t lba, uint32_t count, void *buf) {
    if (s_cur < 0) return -1;
    if (!s_crypt_on) return blockdev_read(s_cur, s_part_base + lba, count, buf);
    uint64_t base = s_crypt.base_lba, end = s_crypt.end_lba;
    /* Aralik matematisi tamamiyla 64 bit: 2 TB ustu disklerde 32 bitlik
     * lba+count tasari istegi yanlis siniflandirip sifreli bolgeye duz
     * veri yazabiliyordu. */
    uint64_t e = lba + count;
    if (lba >= end || e <= base) return blockdev_read(s_cur, s_part_base + lba, count, buf);
    if (lba >= base && e <= end) return s_crypt.read(lba, count, buf);
    uint32_t done = 0;
    if (lba < base) {
        uint64_t n = base - lba;
        if (n > count) n = count;
        if (blockdev_read(s_cur, s_part_base + lba, (uint32_t)n, buf) != 0) return -1;
        done = (uint32_t)n;
    }
    uint64_t l2 = lba + done;
    uint32_t n2 = count - done;
    if (l2 + n2 > end) n2 = (uint32_t)(end - l2);
    if (n2 && s_crypt.read(l2, n2, (uint8_t *)buf + (size_t)done * 512u) != 0) return -1;
    done += n2;
    if (done < count)
        if (blockdev_read(s_cur, s_part_base + lba + done, count - done,
                          (uint8_t *)buf + (size_t)done * 512u) != 0) return -1;
    return 0;
}

int storage_write(uint64_t lba, uint32_t count, const void *buf) {
    if (s_cur < 0) return -1;
    if (!s_crypt_on) return blockdev_write(s_cur, s_part_base + lba, count, buf);
    uint64_t base = s_crypt.base_lba, end = s_crypt.end_lba;
    /* storage_read'teki ile ayni 64-bit aralik gerekcesi */
    uint64_t e = lba + count;
    if (lba >= end || e <= base) return blockdev_write(s_cur, s_part_base + lba, count, buf);
    if (lba >= base && e <= end) return s_crypt.write(lba, count, buf);
    uint32_t done = 0;
    if (lba < base) {
        uint64_t n = base - lba;
        if (n > count) n = count;
        if (blockdev_write(s_cur, s_part_base + lba, (uint32_t)n, buf) != 0) return -1;
        done = (uint32_t)n;
    }
    uint64_t l2 = lba + done;
    uint32_t n2 = count - done;
    if (l2 + n2 > end) n2 = (uint32_t)(end - l2);
    if (n2 && s_crypt.write(l2, n2, (const uint8_t *)buf + (size_t)done * 512u) != 0) return -1;
    done += n2;
    if (done < count)
        if (blockdev_write(s_cur, s_part_base + lba + done, count - done,
                           (const uint8_t *)buf + (size_t)done * 512u) != 0) return -1;
    return 0;
}

uint64_t storage_sector_count(void) {
    if (s_cur < 0) return 0;
    if (s_part_sectors > 0) return s_part_sectors;
    blockdev_info_t info;
    if (blockdev_info(s_cur, &info) != 0) return 0;
    return info.sectors;
}
