/**
 * Arctian kalici kurulum modulu (installer).
 *
 * Debug ISO (ARCTIAN_DEBUG_BOOT):
 *  - Kurulumu atlar ve dogrudan canli masaustune gecer (sifir tus basimi).
 *
 * Release ISO (#ifndef ARCTIAN_DEBUG_BOOT):
 *  - UEFI (GPT + 64 MB FAT32 ESP + AFS v7) ve BIOS (MBR + AFS v7) kurulumu.
 *  - Yanina Kur (Dual-Boot): mevcut GPT diskte >= 5.0 GB bos alana yeni
 *    Arctian bolumu acar, mevcut ESP'ye \EFI\ARCTIAN\ dosyalarini ekler.
 *  - Tam Turkce UTF-8 arayuz + PS/2 fare ve klavye destegi.
 */
#include <stdint.h>
#include <stdbool.h>
#include "kernel.h"
#include "serial.h"
#include "media.h"
#include "iso9660.h"
#include "blockdev.h"
#include "superblock.h"
#include "amod.h"
#include "storage.h"
#include "fat32.h"
#include "memory_map.h"
#include "bootinfo.h"
#include "printf.h"

#ifndef ARCTIAN_DEBUG_BOOT
#include "inst_ui.h"

#define CHUNK_SECS              128u
#define MIN_INSTALL_SECS        10485760ULL /* 5 GiB = 5 * 1024^3 / 512 */
#define INST_ESP_SECTORS        131072ULL   /* 64 MiB FAT32 ESP */
#define INST_ESP_START_LBA      2048ULL
#define INST_GPT_MAX_ENTS       128u
#define INST_GPT_ENT_SZ         128u
#define INST_GPT_ENT_SECS       32u         /* 128 * 128 / 512 */

/* AFS v7 sabitleri (lib/afs.c ile birebir uyumlu) */
#define INST_AFS_MAGIC          0x53464121u /* "!AFS" */
#define INST_AFS_VERSION        7u
#define INST_AFS_V7_MAX_FILES   65536u
#define INST_AFS_BASE_LBA       65536u
#define INST_AFS_NAME_MAX       64u
#define INST_AFS_RESERVE_SEC    2048u
#define INST_AFS_JOURNAL_SEC    256u
#define INST_AFS_V7_DIR_SECS    2u
#define INST_AFS_V7_HDR_SECS    3u          /* 1 SB + 2 inode_dir */
#define INST_AFS_V7_CHUNK_ENTS  256u
#define INST_AFS_V7_CHUNK_SECS  64u         /* 256 * 128 / 512 */
#define INST_AFS_V7_INLINE_EXTS 5u

typedef struct __attribute__((packed)) {
    uint32_t lba;
    uint32_t sectors;
} inst_afs_v7_ext_t;

typedef struct __attribute__((packed)) {
    char              name[INST_AFS_NAME_MAX];
    uint32_t          size;
    uint8_t           used;
    uint8_t           flags;       /* bit0 = is_dir */
    uint16_t          ext_count;
    uint32_t          uid;
    uint32_t          gid;
    uint32_t          mode;
    inst_afs_v7_ext_t extents[INST_AFS_V7_INLINE_EXTS];
    uint32_t          ext_ind_lba;
} inst_afs_v7_inode_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t max_files;
    uint32_t count;
    uint32_t data_next;
    uint32_t hdr_sectors;
    uint64_t capacity;
    uint32_t flags;
    uint32_t inode_dir_lba;
    uint32_t inode_dir_secs;
    uint32_t inode_blk_secs;
    uint32_t chunk_cap;
    uint32_t num_chunks;
    uint32_t journal_seq;
    uint32_t inode_dir_crc;
    uint8_t  reserved[512u - 64u];
} inst_afs_v7_sb_t;

_Static_assert(sizeof(inst_afs_v7_inode_t) == 128u, "inst_afs_v7_inode_t must be 128 bytes");
_Static_assert(sizeof(inst_afs_v7_sb_t) == 512u, "inst_afs_v7_sb_t must be 512 bytes");

static uint8_t chunk[CHUNK_SECS * 512u];
static uint8_t verify_buf[4096];
static uint8_t s_v7_chunk_buf[INST_AFS_V7_CHUNK_SECS * 512u]; /* 32 KB: 256 v7 inode */
static uint8_t s_gpt_ents_buf[INST_GPT_ENT_SECS * 512u];     /* 16 KB: 128 GPT entries */

typedef struct {
    uint64_t total_secs;
    uint64_t total_bytes;
    uint64_t written_secs;
    uint64_t written_bytes;
    uint32_t last_pct;
    uint64_t last_tenths;
} inst_prog_t;

static void inst_zero(void *p, size_t n) {
    uint8_t *b = (uint8_t *)p;
    for (size_t i = 0; i < n; i++) b[i] = 0;
}

static void inst_copy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static uint16_t inst_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t inst_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t inst_le64(const uint8_t *p) {
    return (uint64_t)inst_le32(p) | ((uint64_t)inst_le32(p + 4) << 32);
}

static void inst_wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void inst_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void inst_wr64(uint8_t *p, uint64_t v) {
    inst_wr32(p, (uint32_t)(v & 0xFFFFFFFFULL));
    inst_wr32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t inst_crc32(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1u)));
    }
    return ~c;
}

static bool inst_str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a++ != *b++) return false;
    }
    return *a == *b;
}

static int iso_size(const char *name, uint32_t *size) {
    uint32_t lba = 0, sz = 0;
    if (iso9660_find(name, &lba, &sz) != 0) return -1;
    *size = sz;
    return 0;
}

static void prog_refresh(inst_prog_t *pr, const char *step, const char *detail, bool force) {
    uint32_t pct = 2u;
    if (pr->total_secs > 0) {
        pct = 2u + (uint32_t)((pr->written_secs * 92ULL) / pr->total_secs);
        if (pct > 94u) pct = 94u;
    }
    uint64_t tenths = (pr->written_bytes * 10ULL) / (1024ULL * 1024ULL);
    if (force || pct != pr->last_pct || tenths != pr->last_tenths) {
        pr->last_pct = pct;
        pr->last_tenths = tenths;
        inst_update_progress(pct, pr->written_bytes, pr->total_bytes, step, detail, false);
    }
}

static int64_t copy_iso_to_disk(const char *name, int target, uint64_t dst_lba,
                                uint32_t *out_size, uint8_t hdr[512],
                                bool patch_mbr, uint64_t mbr_sectors,
                                inst_prog_t *pr, const char *step, const char *detail) {
    uint32_t lba2048 = 0, size = 0;
    if (iso9660_find(name, &lba2048, &size) != 0) return -1;
    if (out_size) *out_size = size;

    uint64_t total_secs = ((uint64_t)size + 511ULL) / 512ULL;
    uint64_t lba512 = (uint64_t)lba2048 * 4ULL;
    if (lba512 > 0xFFFFFFFFULL) return -1;

    if (pr) prog_refresh(pr, step, detail, true);

    uint64_t done = 0;
    while (done < total_secs) {
        uint64_t rem = total_secs - done;
        uint32_t n = (rem > CHUNK_SECS) ? CHUNK_SECS : (uint32_t)rem;
        if (lba512 + done > 0xFFFFFFFFULL) return -1;
        if (media_read((uint32_t)(lba512 + done), n, chunk) != 0) return -1;

        if (done == 0) {
            if (patch_mbr) {
                uint8_t *pe = chunk + 446;
                inst_zero(pe, 64);
                pe[0] = 0x80u; /* Bootable */
                pe[1] = 0x00u; pe[2] = 0x01u; pe[3] = 0x00u;
                pe[4] = 0xACu; /* Arctian OS MBR type */
                pe[5] = 0xFEu; pe[6] = 0xFFu; pe[7] = 0xFFu;
                inst_wr32(pe + 8, 0u);
                uint32_t plen = (mbr_sectors > 0xFFFFFFFFULL) ? 0xFFFFFFFFu : (uint32_t)mbr_sectors;
                inst_wr32(pe + 12, plen);
                chunk[510] = 0x55u;
                chunk[511] = 0xAAu;
            }
            if (hdr) inst_copy(hdr, chunk, 512);
        }

        if (blockdev_write(target, dst_lba + done, n, chunk) != 0) return -1;
        done += n;
        if (pr) {
            pr->written_secs  += n;
            pr->written_bytes += (uint64_t)n * 512ULL;
            prog_refresh(pr, step, detail, false);
        }
    }
    if (blockdev_flush(target) != 0) return -1;
    return (int64_t)total_secs;
}

static bool guid_is_zero(const uint8_t g[16]) {
    for (int i = 0; i < 16; i++) {
        if (g[i] != 0) return false;
    }
    return true;
}

static bool guid_eq(const uint8_t a[16], const uint8_t b[16]) {
    for (int i = 0; i < 16; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

static bool guid_is_esp(const uint8_t g[16]) {
    /* EFI System Partition GUID: {C12A7328-F81F-11D2-BA4B-00A0C93EC93B} */
    static const uint8_t esp[16] = {
        0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
        0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
    };
    return guid_eq(g, esp);
}

static bool guid_is_arctian(const uint8_t g[16]) {
    static const uint8_t arct[16] = ARCTIAN_GPT_TYPE_GUID_INIT;
    return guid_eq(g, arct);
}

typedef struct {
    uint64_t start;
    uint64_t end;
} gpt_span_t;

static void probe_disk_tables(int dev_idx, uint64_t total_sectors, uint64_t min_req_secs,
                              bool *out_gpt, bool *out_efi, bool *out_mbr,
                              bool *out_arct, uint32_t *out_gpt_parts,
                              bool *out_can_dual, uint64_t *out_free_start,
                              uint64_t *out_free_secs) {
    *out_gpt = false;
    *out_efi = false;
    *out_mbr = false;
    *out_arct = false;
    *out_gpt_parts = 0;
    *out_can_dual = false;
    *out_free_start = 0;
    *out_free_secs = 0;

    uint8_t lba0[512];
    bool valid_mbr = false;
    bool has_pmbr = false;
    uint32_t mbr_parts = 0;
    if (blockdev_read(dev_idx, 0, 1, lba0) == 0) {
        if (lba0[510] == 0x55u && lba0[511] == 0xAAu) {
            valid_mbr = true;
            for (int p = 0; p < 4; p++) {
                uint8_t ptype = lba0[446 + p * 16 + 4];
                if (ptype == 0xEEu) has_pmbr = true;
                if (ptype == 0xEFu) *out_efi = true;
                if (ptype != 0u) mbr_parts++;
            }
        }
    }

    if (total_sectors > SUPERBLOCK_LBA) {
        uint8_t sb_sec[512];
        if (blockdev_read(dev_idx, SUPERBLOCK_LBA, 1, sb_sec) == 0) {
            const superblock_t *sb = (const superblock_t *)sb_sec;
            if (sb->magic == SUPERBLOCK_MAGIC) *out_arct = true;
        }
    }

    if (total_sectors >= 68ULL) {
        uint8_t lba1[512];
        if (blockdev_read(dev_idx, 1, 1, lba1) == 0) {
            static const char sig[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };
            bool sig_ok = true;
            for (int k = 0; k < 8; k++) {
                if (lba1[k] != (uint8_t)sig[k]) { sig_ok = false; break; }
            }
            uint32_t hdr_sz = inst_le32(lba1 + 12);
            if (sig_ok && hdr_sz >= 92u && hdr_sz <= 512u) {
                *out_gpt = true;
                uint64_t first_usable = inst_le64(lba1 + 40);
                uint64_t last_usable  = inst_le64(lba1 + 48);
                uint64_t part_lba     = inst_le64(lba1 + 72);
                uint32_t num_ent      = inst_le32(lba1 + 80);
                uint32_t ent_sz       = inst_le32(lba1 + 84);

                if (first_usable < 34ULL) first_usable = 34ULL;
                if (last_usable >= total_sectors) last_usable = total_sectors - 34ULL;

                gpt_span_t spans[INST_GPT_MAX_ENTS];
                uint32_t nspans = 0;
                bool has_free_slot = false;

                if (ent_sz >= 128u && ent_sz <= 512u && (512u % ent_sz) == 0u &&
                    part_lba >= 2ULL && part_lba < total_sectors) {
                    if (num_ent > INST_GPT_MAX_ENTS) num_ent = INST_GPT_MAX_ENTS;
                    uint32_t per_sec = 512u / ent_sz;
                    uint32_t nsecs = (num_ent + per_sec - 1u) / per_sec;
                    uint8_t psec[512];
                    for (uint32_t s = 0; s < nsecs; s++) {
                        if (part_lba + s >= total_sectors) break;
                        if (blockdev_read(dev_idx, part_lba + s, 1, psec) != 0) break;
                        for (uint32_t e = 0; e < per_sec; e++) {
                            uint32_t idx = s * per_sec + e;
                            if (idx >= num_ent) break;
                            const uint8_t *ent = psec + e * ent_sz;
                            if (guid_is_zero(ent)) {
                                has_free_slot = true;
                                continue;
                            }
                            (*out_gpt_parts)++;
                            if (guid_is_esp(ent)) *out_efi = true;
                            if (guid_is_arctian(ent)) *out_arct = true;

                            uint64_t fl = inst_le64(ent + 32);
                            uint64_t ll = inst_le64(ent + 40);
                            if (fl > 0 && ll >= fl && ll < total_sectors) {
                                if (!*out_arct && (ll - fl + 1ULL) > SUPERBLOCK_LBA) {
                                    uint8_t p_sb[512];
                                    if (blockdev_read(dev_idx, fl + SUPERBLOCK_LBA, 1, p_sb) == 0) {
                                        const superblock_t *sb = (const superblock_t *)p_sb;
                                        if (sb->magic == SUPERBLOCK_MAGIC) *out_arct = true;
                                    }
                                }
                                if (nspans < INST_GPT_MAX_ENTS) {
                                    spans[nspans].start = fl;
                                    spans[nspans].end   = ll;
                                    nspans++;
                                }
                            }
                        }
                    }

                    /* En buyuk hizali (1 MiB) bos alani bul */
                    for (uint32_t i = 1; i < nspans; i++) {
                        gpt_span_t key = spans[i];
                        uint32_t j = i;
                        while (j > 0 && spans[j - 1].start > key.start) {
                            spans[j] = spans[j - 1];
                            j--;
                        }
                        spans[j] = key;
                    }

                    uint64_t cur = ((first_usable + 2047ULL) / 2048ULL) * 2048ULL;
                    if (cur < first_usable) cur = first_usable;
                    uint64_t best_start = 0, best_secs = 0;

                    for (uint32_t i = 0; i < nspans; i++) {
                        if (spans[i].start > cur) {
                            uint64_t gsecs = spans[i].start - cur;
                            if (gsecs > best_secs) {
                                best_start = cur;
                                best_secs  = gsecs;
                            }
                        }
                        if (spans[i].end + 1ULL > cur) {
                            uint64_t nxt = spans[i].end + 1ULL;
                            cur = ((nxt + 2047ULL) / 2048ULL) * 2048ULL;
                        }
                    }
                    if (last_usable >= cur) {
                        uint64_t gsecs = last_usable - cur + 1ULL;
                        if (gsecs > best_secs) {
                            best_start = cur;
                            best_secs  = gsecs;
                        }
                    }

                    *out_free_start = best_start;
                    *out_free_secs  = best_secs;
                    if (*out_efi && has_free_slot && best_secs >= min_req_secs) {
                        *out_can_dual = true;
                    }
                }
            }
        }
    }

    if (!*out_gpt && valid_mbr && mbr_parts > 0u && !has_pmbr) {
        *out_mbr = true;
    }
}

static void scan_disks(inst_disk_t *out, int *out_count, uint64_t min_req_secs) {
    int raw = blockdev_init();
    int cnt = 0;
    for (int i = 0; i < raw && cnt < INST_MAX_DISKS; i++) {
        blockdev_info_t info;
        if (blockdev_info(i, &info) != 0) continue;
        if (info.removable != 0) continue;
        if (info.sectors == 0) continue;

        bool has_gpt = false, has_efi = false, has_mbr = false;
        bool has_arct = false, can_dual = false;
        uint32_t gpt_parts = 0;
        uint64_t free_start = 0, free_secs = 0;
        probe_disk_tables(i, info.sectors, min_req_secs,
                          &has_gpt, &has_efi, &has_mbr, &has_arct,
                          &gpt_parts, &can_dual, &free_start, &free_secs);

        out[cnt].bdev_idx       = i;
        out[cnt].info           = info;
        out[cnt].size_mb        = info.sectors / 2048ULL;
        out[cnt].has_arctian    = has_arct ? 1u : 0u;
        out[cnt].too_small      = (info.sectors < min_req_secs) ? 1u : 0u;
        out[cnt].has_gpt        = has_gpt;
        out[cnt].has_efi_part   = has_efi;
        out[cnt].has_mbr        = has_mbr;
        out[cnt].gpt_parts      = gpt_parts;
        out[cnt].can_dualboot   = can_dual;
        out[cnt].free_start_lba = free_start;
        out[cnt].free_sectors   = free_secs;

        serial_printf("[inst] disk#%d %s %lluMB gpt=%d efi=%d mbr=%d parts=%u arct=%d dual=%d free=%llu\n",
                      cnt, info.name, (unsigned long long)out[cnt].size_mb,
                      has_gpt ? 1 : 0, has_efi ? 1 : 0, has_mbr ? 1 : 0,
                      (unsigned)gpt_parts, has_arct ? 1 : 0,
                      can_dual ? 1 : 0, (unsigned long long)free_secs);
        cnt++;
    }
    *out_count = cnt;
}

static int pick_default_disk(const inst_disk_t *disks, int count) {
    for (int i = 0; i < count; i++) {
        if (!disks[i].too_small && !disks[i].has_arctian &&
            !disks[i].has_gpt && !disks[i].has_efi_part && !disks[i].has_mbr) return i;
    }
    for (int i = 0; i < count; i++) {
        if (!disks[i].too_small && disks[i].can_dualboot) return i;
    }
    for (int i = 0; i < count; i++) {
        if (!disks[i].too_small && !disks[i].has_arctian && !disks[i].has_efi_part) return i;
    }
    for (int i = 0; i < count; i++) {
        if (!disks[i].too_small) return i;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * AFS v7 Formatlama (dogrudan hedef disk/bolum LBA'sina)
 * ------------------------------------------------------------------------- */
static int inst_v7_find_slot(const inst_afs_v7_inode_t *inodes, const char *name) {
    for (uint32_t i = 0; i < INST_AFS_V7_CHUNK_ENTS; i++) {
        if (inodes[i].used && inst_str_eq(inodes[i].name, name))
            return (int)i;
    }
    return -1;
}

static bool inst_v7_add_entry(inst_afs_v7_inode_t *inodes, uint32_t *count,
                              const char *name, uint32_t rel_lba, uint32_t secs,
                              uint32_t size, uint32_t uid, uint32_t gid,
                              uint32_t mode, bool is_dir) {
    if (inst_v7_find_slot(inodes, name) >= 0) return true;
    for (uint32_t i = 0; i < INST_AFS_V7_CHUNK_ENTS; i++) {
        if (!inodes[i].used) {
            inst_afs_v7_inode_t *in = &inodes[i];
            inst_zero(in, sizeof(*in));
            uint32_t k = 0;
            for (; name[k] && k < INST_AFS_NAME_MAX - 1u; k++)
                in->name[k] = name[k];
            in->name[k] = '\0';
            in->used      = 1u;
            in->flags     = is_dir ? 1u : 0u;
            in->size      = is_dir ? 0u : size;
            in->uid       = uid;
            in->gid       = gid;
            in->mode      = mode;
            if (!is_dir && secs > 0) {
                in->ext_count          = 1u;
                in->extents[0].lba     = rel_lba;
                in->extents[0].sectors = secs;
            }
            (*count)++;
            return true;
        }
    }
    return false;
}

static bool inst_v7_ensure_dir(inst_afs_v7_inode_t *inodes, uint32_t *count,
                               const char *path, uint32_t uid, uint32_t gid) {
    return inst_v7_add_entry(inodes, count, path, 0u, 0u, 0u, uid, gid, 0777u, true);
}

static bool inst_v7_copy_from_iso(int target, uint64_t part_base, uint64_t cap,
                                    inst_afs_v7_inode_t *inodes, uint32_t *count,
                                    uint32_t *data_next,
                                    const char *iso_name, const char *afs_path,
                                    uint32_t uid, uint32_t gid, uint32_t mode,
                                    inst_prog_t *pr, const char *step, const char *detail,
                                    bool *out_copied) {
    if (out_copied) *out_copied = false;
    uint32_t lba2048 = 0, size = 0;
    if (iso9660_find(iso_name, &lba2048, &size) != 0 || size == 0) {
        return true;
    }

    uint64_t total_secs = ((uint64_t)size + 511ULL) / 512ULL;
    uint64_t lba512 = (uint64_t)lba2048 * 4ULL;
    if (lba512 > 0xFFFFFFFFULL || total_secs == 0 || total_secs > 0xFFFFFFFFULL) return false;
    if ((uint64_t)(*data_next) + total_secs > cap) return false;

    uint32_t dst_rel_lba = *data_next;
    if (pr) prog_refresh(pr, step, detail, true);

    uint64_t done = 0;
    while (done < total_secs) {
        uint64_t rem = total_secs - done;
        uint32_t n = (rem > CHUNK_SECS) ? CHUNK_SECS : (uint32_t)rem;
        if (lba512 + done > 0xFFFFFFFFULL) return false;
        if (media_read((uint32_t)(lba512 + done), n, chunk) != 0) return false;
        if (blockdev_write(target, part_base + (uint64_t)dst_rel_lba + done, n, chunk) != 0)
            return false;
        done += n;
        if (pr) {
            pr->written_secs  += n;
            pr->written_bytes += (uint64_t)n * 512ULL;
            prog_refresh(pr, step, detail, false);
        }
    }
    if (blockdev_flush(target) != 0) return false;

    if (!inst_v7_add_entry(inodes, count, afs_path, dst_rel_lba, (uint32_t)total_secs,
                           size, uid, gid, mode, false))
        return false;
    *data_next += (uint32_t)total_secs;
    if (out_copied) *out_copied = true;
    return true;
}

static bool inst_populate_afs_v7(int target, uint64_t part_base, uint64_t part_secs,
                                 inst_prog_t *pr, bool *out_webkit_ok,
                                 inst_afs_v7_sb_t *out_sb, const char **err_out) {
    *out_webkit_ok = false;

    uint64_t cap = (part_secs > INST_AFS_RESERVE_SEC) ? (part_secs - INST_AFS_RESERVE_SEC) : part_secs;
    if (cap > 0xFFFFFFFEULL) cap = 0xFFFFFFFEULL;
    if (cap > (uint64_t)INST_AFS_BASE_LBA + INST_AFS_JOURNAL_SEC)
        cap -= INST_AFS_JOURNAL_SEC;
    if (cap <= (uint64_t)INST_AFS_BASE_LBA + INST_AFS_V7_HDR_SECS + INST_AFS_V7_CHUNK_SECS + 16ULL) {
        *err_out = "AFS v7 dosya sistemi için disk alanı yetersiz.";
        return false;
    }

    /* Eski gunluk (journal) sektorunu temizle */
    if (part_secs >= (uint64_t)INST_AFS_BASE_LBA + INST_AFS_RESERVE_SEC + INST_AFS_JOURNAL_SEC) {
        uint64_t jl = part_secs - INST_AFS_RESERVE_SEC - INST_AFS_JOURNAL_SEC;
        inst_zero(verify_buf, 512);
        if (blockdev_write(target, part_base + jl, 1, verify_buf) != 0) {
            *err_out = "AFS v7 günlük sektörü temizlenemedi.";
            return false;
        }
    }

    inst_zero(s_v7_chunk_buf, sizeof(s_v7_chunk_buf));
    inst_afs_v7_inode_t *inodes = (inst_afs_v7_inode_t *)s_v7_chunk_buf;
    uint32_t count = 0;
    uint32_t chunk0_rel = INST_AFS_BASE_LBA + INST_AFS_V7_HDR_SECS;
    uint32_t data_next  = chunk0_rel + INST_AFS_V7_CHUNK_SECS;

    static const char *base_dirs[6] = {
        "C:/", "C:/apps/", "C:/indirilenler/", "C:/belgeler/", "C:/masaustu/", "C:/logs/"
    };
    for (int i = 0; i < 6; i++) {
        if (!inst_v7_ensure_dir(inodes, &count, base_dirs[i], 1000u, 1000u)) {
            *err_out = "AFS v7 kök dizinleri oluşturulamadı.";
            return false;
        }
    }

    uint32_t opt_sz = 0;
    if (iso_size("WEBKIT.ELF", &opt_sz) == 0 && opt_sz > 0) {
        bool wk_ok = false;
        if (!inst_v7_copy_from_iso(target, part_base, cap, inodes, &count, &data_next,
                                   "WEBKIT.ELF", "C:/webkit.elf",
                                   1000u, 1000u, 0755u, pr,
                                   "Ek bileşenler kuruluyor...",
                                   "WPE WebKit 2.44 tarayıcı kopyalanıyor (C:/webkit.elf)...",
                                   &wk_ok)) {
            *err_out = "WEBKIT.ELF kopyalanamadı (medya/disk hatası).";
            return false;
        }
        *out_webkit_ok = wk_ok;
    }

    if (iso_size("DEJAVU.TTF", &opt_sz) == 0 && opt_sz > 0) {
        bool f1 = false, f2 = false;
        if (!inst_v7_ensure_dir(inodes, &count, "C:/design/", 1000u, 1000u) ||
            !inst_v7_ensure_dir(inodes, &count, "C:/design/font/", 1000u, 1000u) ||
            !inst_v7_ensure_dir(inodes, &count, "C:/usr/", 1000u, 1000u) ||
            !inst_v7_ensure_dir(inodes, &count, "C:/usr/share/", 1000u, 1000u) ||
            !inst_v7_ensure_dir(inodes, &count, "C:/usr/share/fonts/", 1000u, 1000u)) {
            *err_out = "AFS yazı tipi dizinleri oluşturulamadı.";
            return false;
        }
        if (!inst_v7_copy_from_iso(target, part_base, cap, inodes, &count, &data_next,
                                   "DEJAVU.TTF", "C:/design/font/DejaVuSans.ttf",
                                   1000u, 1000u, 0644u, pr,
                                   "Ek bileşenler kuruluyor...",
                                   "Sistem yazı tipi kopyalanıyor (DejaVuSans.ttf)...",
                                   &f1) ||
            !inst_v7_copy_from_iso(target, part_base, cap, inodes, &count, &data_next,
                                   "DEJAVU.TTF", "C:/usr/share/fonts/DejaVuSans.ttf",
                                   1000u, 1000u, 0644u, pr,
                                   "Ek bileşenler kuruluyor...",
                                   "Sistem yazı tipi kopyalanıyor (/usr/share/fonts)...",
                                   &f2)) {
            *err_out = "DEJAVU.TTF kopyalanamadı.";
            return false;
        }
    }

    if (iso_size("CACERT.PEM", &opt_sz) == 0 && opt_sz > 0) {
        bool ca_ok = false;
        if (!inst_v7_ensure_dir(inodes, &count, "C:/etc/", 0u, 0u) ||
            !inst_v7_ensure_dir(inodes, &count, "C:/etc/ssl/", 0u, 0u) ||
            !inst_v7_ensure_dir(inodes, &count, "C:/etc/ssl/certs/", 0u, 0u)) {
            *err_out = "AFS sertifika dizinleri oluşturulamadı.";
            return false;
        }
        if (!inst_v7_copy_from_iso(target, part_base, cap, inodes, &count, &data_next,
                                   "CACERT.PEM", "C:/etc/ssl/certs/ca-certificates.crt",
                                   0u, 0u, 0644u, pr,
                                   "Ek bileşenler kuruluyor...",
                                   "Kök TLS sertifikaları kopyalanıyor (ca-certificates.crt)...",
                                   &ca_ok)) {
            *err_out = "CACERT.PEM kopyalanamadı.";
            return false;
        }
    }

    {
        static const struct {
            const char *iso;
            const char *afs;
            const char *detail;
        } office_wlfs[5] = {
            { "SAFAK.WLF",    "C:/apps/safak.wlf",    "Ofis paketi kuruluyor (Şafak)..." },
            { "MUREKKEP.WLF", "C:/apps/murekkep.wlf", "Ofis paketi kuruluyor (Mürekkep)..." },
            { "ABAKUS.WLF",   "C:/apps/abakus.wlf",   "Ofis paketi kuruluyor (Abaküs)..." },
            { "SAHNE.WLF",    "C:/apps/sahne.wlf",    "Ofis paketi kuruluyor (Sahne)..." },
            { "MERCEK.WLF",   "C:/apps/mercek.wlf",   "Ofis paketi kuruluyor (Mercek)..." },
        };
        for (int i = 0; i < 5; i++) {
            if (iso_size(office_wlfs[i].iso, &opt_sz) == 0 && opt_sz > 0) {
                bool w_ok = false;
                if (!inst_v7_copy_from_iso(target, part_base, cap, inodes, &count, &data_next,
                                           office_wlfs[i].iso, office_wlfs[i].afs,
                                           1000u, 1000u, 0644u, pr,
                                           "Ofis uygulamaları kuruluyor...",
                                           office_wlfs[i].detail,
                                           &w_ok)) {
                    *err_out = "Ofis .wlf paketi kopyalanamadı.";
                    return false;
                }
                if (w_ok) {
                    serial_printf("[inst] AFS v7 preinstall: %s (%u B)\n",
                                  office_wlfs[i].afs, (unsigned)opt_sz);
                }
            }
        }
    }

    /* 1) Inode chunk 0 yaz (64 sektor) */
    if (blockdev_write(target, part_base + chunk0_rel, INST_AFS_V7_CHUNK_SECS, s_v7_chunk_buf) != 0) {
        *err_out = "AFS v7 inode tablosu diske yazılamadı.";
        return false;
    }

    /* 2) Inode directory (2 sektor = 1024 B) */
    static uint32_t s_inode_dir[256];
    inst_zero(s_inode_dir, sizeof(s_inode_dir));
    s_inode_dir[0] = chunk0_rel;
    uint32_t dir_crc = inst_crc32(s_inode_dir, sizeof(s_inode_dir));
    if (blockdev_write(target, part_base + INST_AFS_BASE_LBA + 1u,
                       INST_AFS_V7_DIR_SECS, s_inode_dir) != 0) {
        *err_out = "AFS v7 inode dizini diske yazılamadı.";
        return false;
    }

    /* 3) AFS v7 Superblock (1 sektor) */
    inst_zero(out_sb, sizeof(*out_sb));
    out_sb->magic          = INST_AFS_MAGIC;
    out_sb->version        = INST_AFS_VERSION;
    out_sb->max_files      = INST_AFS_V7_MAX_FILES;
    out_sb->count          = count;
    out_sb->data_next      = data_next;
    out_sb->hdr_sectors    = INST_AFS_V7_HDR_SECS;
    out_sb->capacity       = cap;
    out_sb->inode_dir_lba  = INST_AFS_BASE_LBA + 1u;
    out_sb->inode_dir_secs = INST_AFS_V7_DIR_SECS;
    out_sb->inode_blk_secs = INST_AFS_V7_CHUNK_SECS;
    out_sb->chunk_cap      = INST_AFS_V7_CHUNK_ENTS;
    out_sb->num_chunks     = 1u;
    out_sb->journal_seq    = 1u;
    out_sb->inode_dir_crc  = dir_crc;
    out_sb->flags          = 0u;
    out_sb->flags          = inst_crc32(out_sb, sizeof(*out_sb));

    if (blockdev_write(target, part_base + INST_AFS_BASE_LBA, 1, out_sb) != 0 ||
        blockdev_flush(target) != 0) {
        *err_out = "AFS v7 süperbloğu diske yazılamadı.";
        return false;
    }

    serial_printf("[inst] AFS v7 hazir: part_base=%llu base=%u files=%u data_next=%u webkit=%d\n",
                  (unsigned long long)part_base, (unsigned)INST_AFS_BASE_LBA,
                  (unsigned)out_sb->count, (unsigned)out_sb->data_next,
                  *out_webkit_ok ? 1 : 0);
    return true;
}

/* -------------------------------------------------------------------------
 * GPT & FAT32 ESP Olusturma / Guncelleme
 * ------------------------------------------------------------------------- */
static void make_unique_guid(uint8_t out[16], uint64_t seed_a, uint64_t seed_b, uint32_t tag) {
    uint64_t h1 = seed_a ^ 0x9E3779B97F4A7C15ULL ^ ((uint64_t)tag << 32);
    uint64_t h2 = seed_b ^ 0xBF58476D1CE4E5B9ULL ^ (uint64_t)tag;
    h1 ^= h1 >> 30; h1 *= 0xBF58476D1CE4E5B9ULL; h1 ^= h1 >> 27;
    h2 ^= h2 >> 31; h2 *= 0x94D049BB133111EBULL; h2 ^= h2 >> 31;
    inst_wr64(out, h1);
    inst_wr64(out + 8, h2);
    out[6] = (uint8_t)((out[6] & 0x0Fu) | 0x40u);
    out[8] = (uint8_t)((out[8] & 0x3Fu) | 0x80u);
}

static void write_gpt_entry_at(uint8_t *ent, const uint8_t type_guid[16],
                               const uint8_t part_guid[16], uint64_t first_lba,
                               uint64_t last_lba, const char *ascii_name) {
    inst_zero(ent, INST_GPT_ENT_SZ);
    inst_copy(ent + 0, type_guid, 16);
    inst_copy(ent + 16, part_guid, 16);
    inst_wr64(ent + 32, first_lba);
    inst_wr64(ent + 40, last_lba);
    inst_wr64(ent + 48, 0ULL);
    for (int i = 0; ascii_name[i] && i < 35; i++) {
        ent[56 + i * 2] = (uint8_t)ascii_name[i];
        ent[56 + i * 2 + 1] = 0u;
    }
}

static bool write_protective_mbr(int target, uint64_t total_sectors) {
    uint8_t mbr[512];
    inst_zero(mbr, sizeof(mbr));
    uint8_t *pe = mbr + 446;
    pe[0] = 0x00u;
    pe[1] = 0x00u; pe[2] = 0x02u; pe[3] = 0x00u;
    pe[4] = 0xEEu;
    pe[5] = 0xFFu; pe[6] = 0xFFu; pe[7] = 0xFFu;
    inst_wr32(pe + 8, 1u);
    uint64_t plen = (total_sectors > 1ULL) ? (total_sectors - 1ULL) : 1ULL;
    inst_wr32(pe + 12, (plen > 0xFFFFFFFFULL) ? 0xFFFFFFFFu : (uint32_t)plen);
    mbr[510] = 0x55u;
    mbr[511] = 0xAAu;
    return blockdev_write(target, 0, 1, mbr) == 0;
}

static void build_gpt_header_buf(uint8_t hdr[512], uint32_t hdr_sz, uint64_t my_lba,
                                 uint64_t alt_lba, uint64_t first_usable,
                                 uint64_t last_usable, const uint8_t disk_guid[16],
                                 uint64_t ent_lba, uint32_t num_ent, uint32_t ent_sz,
                                 uint32_t ents_crc) {
    inst_zero(hdr, 512);
    static const char sig[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };
    inst_copy(hdr + 0, sig, 8);
    inst_wr32(hdr + 8, 0x00010000u); /* Revision 1.0 */
    inst_wr32(hdr + 12, hdr_sz);
    inst_wr32(hdr + 16, 0u);
    inst_wr32(hdr + 20, 0u);
    inst_wr64(hdr + 24, my_lba);
    inst_wr64(hdr + 32, alt_lba);
    inst_wr64(hdr + 40, first_usable);
    inst_wr64(hdr + 48, last_usable);
    inst_copy(hdr + 56, disk_guid, 16);
    inst_wr64(hdr + 72, ent_lba);
    inst_wr32(hdr + 80, num_ent);
    inst_wr32(hdr + 84, ent_sz);
    inst_wr32(hdr + 88, ents_crc);
    uint32_t hcrc = inst_crc32(hdr, hdr_sz);
    inst_wr32(hdr + 16, hcrc);
}

static bool inst_create_clean_gpt(int target, uint64_t total_sectors,
                                  uint64_t *out_esp_lba, uint64_t *out_esp_secs,
                                  uint64_t *out_arct_lba, uint64_t *out_arct_secs,
                                  const char **err_out) {
    if (total_sectors < INST_ESP_START_LBA + INST_ESP_SECTORS + MIN_INSTALL_SECS + 64ULL) {
        *err_out = "GPT + ESP + sistem için disk boyutu yetersiz.";
        return false;
    }

    uint64_t esp_lba   = INST_ESP_START_LBA;
    uint64_t esp_secs  = INST_ESP_SECTORS;
    uint64_t esp_last  = esp_lba + esp_secs - 1ULL;
    uint64_t arct_lba  = esp_last + 1ULL;
    uint64_t last_use  = total_sectors - 34ULL;
    uint64_t arct_secs = last_use - arct_lba + 1ULL;

    if (!write_protective_mbr(target, total_sectors)) {
        *err_out = "Koruyucu MBR (LBA 0) yazılamadı.";
        return false;
    }

    static const uint8_t esp_guid[16] = {
        0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
        0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
    };
    static const uint8_t arct_guid[16] = ARCTIAN_GPT_TYPE_GUID_INIT;

    uint8_t disk_guid[16], p1_guid[16], p2_guid[16];
    make_unique_guid(disk_guid, total_sectors, 0x4449534BULL, 1u);
    make_unique_guid(p1_guid, esp_lba, esp_secs, 2u);
    make_unique_guid(p2_guid, arct_lba, arct_secs, 3u);

    inst_zero(s_gpt_ents_buf, sizeof(s_gpt_ents_buf));
    write_gpt_entry_at(s_gpt_ents_buf + 0 * INST_GPT_ENT_SZ, esp_guid, p1_guid,
                       esp_lba, esp_last, "EFI System");
    write_gpt_entry_at(s_gpt_ents_buf + 1 * INST_GPT_ENT_SZ, arct_guid, p2_guid,
                       arct_lba, last_use, "Arctian OS");

    uint32_t ents_crc = inst_crc32(s_gpt_ents_buf, INST_GPT_MAX_ENTS * INST_GPT_ENT_SZ);
    uint64_t backup_hdr_lba  = total_sectors - 1ULL;
    uint64_t backup_ents_lba = total_sectors - 1ULL - INST_GPT_ENT_SECS;

    uint8_t hdr[512];
    build_gpt_header_buf(hdr, 92u, 1ULL, backup_hdr_lba, 34ULL, last_use,
                         disk_guid, 2ULL, INST_GPT_MAX_ENTS, INST_GPT_ENT_SZ, ents_crc);
    if (blockdev_write(target, 1ULL, 1, hdr) != 0 ||
        blockdev_write(target, 2ULL, INST_GPT_ENT_SECS, s_gpt_ents_buf) != 0) {
        *err_out = "Birincil GPT başlığı diske yazılamadı.";
        return false;
    }

    build_gpt_header_buf(hdr, 92u, backup_hdr_lba, 1ULL, 34ULL, last_use,
                         disk_guid, backup_ents_lba, INST_GPT_MAX_ENTS, INST_GPT_ENT_SZ, ents_crc);
    if (blockdev_write(target, backup_ents_lba, INST_GPT_ENT_SECS, s_gpt_ents_buf) != 0 ||
        blockdev_write(target, backup_hdr_lba, 1, hdr) != 0 ||
        blockdev_flush(target) != 0) {
        *err_out = "Yedek GPT başlığı diske yazılamadı.";
        return false;
    }

    *out_esp_lba   = esp_lba;
    *out_esp_secs  = esp_secs;
    *out_arct_lba  = arct_lba;
    *out_arct_secs = arct_secs;
    return true;
}

static bool inst_append_dualboot_gpt(int target, uint64_t total_sectors,
                                     uint64_t part_lba, uint64_t part_secs,
                                     const char **err_out) {
    uint8_t hdr[512];
    if (blockdev_read(target, 1ULL, 1, hdr) != 0) {
        *err_out = "Mevcut GPT başlığı okunamadı.";
        return false;
    }
    uint32_t hdr_sz  = inst_le32(hdr + 12);
    uint64_t p_ent   = inst_le64(hdr + 72);
    uint32_t num_ent = inst_le32(hdr + 80);
    uint32_t ent_sz  = inst_le32(hdr + 84);
    if (hdr_sz < 92u || hdr_sz > 512u || ent_sz != INST_GPT_ENT_SZ ||
        num_ent == 0 || num_ent > INST_GPT_MAX_ENTS || p_ent < 2ULL) {
        *err_out = "Mevcut GPT tablosu desteklenmeyen biçimde.";
        return false;
    }

    uint32_t nsecs = (num_ent * ent_sz + 511u) / 512u;
    if (blockdev_read(target, p_ent, nsecs, s_gpt_ents_buf) != 0) {
        *err_out = "Mevcut GPT bölüm girişleri okunamadı.";
        return false;
    }

    int free_slot = -1;
    for (uint32_t i = 0; i < num_ent; i++) {
        if (guid_is_zero(s_gpt_ents_buf + i * ent_sz)) {
            free_slot = (int)i;
            break;
        }
    }
    if (free_slot < 0) {
        *err_out = "GPT tablosunda boş bölüm yuvası kalmamış.";
        return false;
    }

    static const uint8_t arct_guid[16] = ARCTIAN_GPT_TYPE_GUID_INIT;
    uint8_t pguid[16];
    make_unique_guid(pguid, part_lba, part_secs, (uint32_t)free_slot + 10u);
    write_gpt_entry_at(s_gpt_ents_buf + (uint32_t)free_slot * ent_sz,
                       arct_guid, pguid, part_lba, part_lba + part_secs - 1ULL,
                       "Arctian OS");

    uint32_t ents_crc = inst_crc32(s_gpt_ents_buf, num_ent * ent_sz);
    uint64_t backup_hdr_lba = inst_le64(hdr + 32);
    if (backup_hdr_lba <= inst_le64(hdr + 48) || backup_hdr_lba >= total_sectors) {
        backup_hdr_lba = total_sectors - 1ULL;
    }
    uint64_t backup_ents_lba = (backup_hdr_lba > nsecs) ? (backup_hdr_lba - nsecs) : (total_sectors - 1ULL - nsecs);

    inst_wr64(hdr + 24, 1ULL);
    inst_wr64(hdr + 32, backup_hdr_lba);
    inst_wr32(hdr + 88, ents_crc);
    inst_wr32(hdr + 16, 0u);
    inst_wr32(hdr + 16, inst_crc32(hdr, hdr_sz));

    if (blockdev_write(target, p_ent, nsecs, s_gpt_ents_buf) != 0 ||
        blockdev_write(target, 1ULL, 1, hdr) != 0) {
        *err_out = "Güncel birincil GPT yazılamadı.";
        return false;
    }

    uint8_t bhdr[512];
    inst_copy(bhdr, hdr, 512);
    inst_wr64(bhdr + 24, backup_hdr_lba);
    inst_wr64(bhdr + 32, 1ULL);
    inst_wr64(bhdr + 72, backup_ents_lba);
    inst_wr32(bhdr + 16, 0u);
    inst_wr32(bhdr + 16, inst_crc32(bhdr, hdr_sz));

    if (blockdev_write(target, backup_ents_lba, nsecs, s_gpt_ents_buf) != 0 ||
        blockdev_write(target, backup_hdr_lba, 1, bhdr) != 0 ||
        blockdev_flush(target) != 0) {
        *err_out = "Güncel yedek GPT yazılamadı.";
        return false;
    }

    serial_printf("[inst] Dual-Boot GPT bolumu eklendi: slot=%d lba=%llu..%llu\n",
                  free_slot, (unsigned long long)part_lba,
                  (unsigned long long)(part_lba + part_secs - 1ULL));
    return true;
}

static bool inst_format_fat32_esp(int target, uint64_t esp_lba, uint64_t esp_secs,
                                  const char **err_out) {
    /* 64 MB FAT32 (sectors_per_cluster=1 => >65525 clusters -> OVMF FatDxe uyumlu) */
    uint32_t total = (uint32_t)esp_secs;
    uint16_t reserved = 32u;
    uint8_t  nfats = 2u;
    uint32_t fat_sz = (total - reserved + 129u) / 130u + 1u; /* ~1009 sectors for 131072 */
    uint32_t first_data = reserved + (uint32_t)nfats * fat_sz;
    uint32_t clusters = total - first_data;
    if (clusters < 65525u) {
        *err_out = "FAT32 ESP küme sayısı yetersiz (<65525).";
        return false;
    }

    /* Meta veri + FAT1 + FAT2 + kok dizin kumesini sifirla */
    inst_zero(chunk, sizeof(chunk));
    uint32_t zero_secs = first_data + 1u;
    uint32_t done = 0;
    while (done < zero_secs) {
        uint32_t n = zero_secs - done;
        if (n > CHUNK_SECS) n = CHUNK_SECS;
        if (blockdev_write(target, esp_lba + done, n, chunk) != 0) {
            *err_out = "FAT32 ESP alanı temizlenemedi.";
            return false;
        }
        done += n;
    }

    /* Boot Sector (BPB) */
    uint8_t bs[512];
    inst_zero(bs, sizeof(bs));
    bs[0] = 0xEBu; bs[1] = 0x58u; bs[2] = 0x90u;
    inst_copy(bs + 3, "MSWIN4.1", 8);
    inst_wr16(bs + 11, 512u);
    bs[13] = 1u;                      /* sectors_per_cluster = 1 */
    inst_wr16(bs + 14, reserved);
    bs[16] = nfats;
    inst_wr16(bs + 17, 0u);
    inst_wr16(bs + 19, 0u);
    bs[21] = 0xF8u;
    inst_wr16(bs + 22, 0u);
    inst_wr16(bs + 24, 63u);
    inst_wr16(bs + 26, 255u);
    inst_wr32(bs + 28, (uint32_t)esp_lba);
    inst_wr32(bs + 32, total);
    inst_wr32(bs + 36, fat_sz);
    inst_wr16(bs + 40, 0u);
    inst_wr16(bs + 42, 0u);
    inst_wr32(bs + 44, 2u);           /* root_cluster = 2 */
    inst_wr16(bs + 48, 1u);           /* fsinfo = 1 */
    inst_wr16(bs + 50, 6u);           /* backup boot = 6 */
    bs[64] = 0x80u;
    bs[66] = 0x29u;
    inst_wr32(bs + 67, 0x41524354u);
    inst_copy(bs + 71, "ARCTIAN_EFI", 11);
    inst_copy(bs + 82, "FAT32   ", 8);
    bs[510] = 0x55u; bs[511] = 0xAAu;

    /* FSInfo */
    uint8_t fsi[512];
    inst_zero(fsi, sizeof(fsi));
    inst_wr32(fsi + 0,   0x41615252u);
    inst_wr32(fsi + 484, 0x61417272u);
    inst_wr32(fsi + 488, clusters - 1u);
    inst_wr32(fsi + 492, 3u);
    inst_wr32(fsi + 508, 0xAA550000u);

    /* FAT[0..2] */
    uint8_t fat0[512];
    inst_zero(fat0, sizeof(fat0));
    inst_wr32(fat0 + 0, 0x0FFFFFF8u);
    inst_wr32(fat0 + 4, 0x0FFFFFFFu);
    inst_wr32(fat0 + 8, 0x0FFFFFFFu);

    if (blockdev_write(target, esp_lba + 0u, 1, bs) != 0 ||
        blockdev_write(target, esp_lba + 1u, 1, fsi) != 0 ||
        blockdev_write(target, esp_lba + 6u, 1, bs) != 0 ||
        blockdev_write(target, esp_lba + 7u, 1, fsi) != 0 ||
        blockdev_write(target, esp_lba + reserved, 1, fat0) != 0 ||
        blockdev_write(target, esp_lba + reserved + fat_sz, 1, fat0) != 0 ||
        blockdev_flush(target) != 0) {
        *err_out = "FAT32 ESP başlık sektörleri yazılamadı.";
        return false;
    }
    return true;
}

static bool inst_load_iso_to_buf(const char *iso_name, uint8_t *buf, uint32_t max_bytes,
                                 uint32_t *out_size) {
    uint32_t lba2048 = 0, size = 0;
    if (iso9660_find(iso_name, &lba2048, &size) != 0 || size == 0 || size > max_bytes)
        return false;

    uint64_t lba512 = (uint64_t)lba2048 * 4ULL;
    uint32_t total_secs = (size + 511u) / 512u;
    uint32_t done = 0;
    while (done < total_secs) {
        uint32_t n = total_secs - done;
        if (n > CHUNK_SECS) n = CHUNK_SECS;
        if (media_read((uint32_t)(lba512 + done), n, chunk) != 0) return false;
        uint32_t bytes = n * 512u;
        uint32_t rem = size - done * 512u;
        if (bytes > rem) bytes = rem;
        inst_copy(buf + done * 512u, chunk, bytes);
        done += n;
    }
    *out_size = size;
    return true;
}

static bool inst_populate_esp(int target, bool is_dualboot, const char **err_out) {
    if (fat32_mount(target) != 0) {
        *err_out = "FAT32 EFI Sistem Bölümü (ESP) bağlanamadı.";
        return false;
    }

    if (fat32_mkdir("/EFI") != 0 || fat32_mkdir("/EFI/ARCTIAN") != 0) {
        fat32_unmount();
        *err_out = "ESP üzerinde /EFI/ARCTIAN klasörü oluşturulamadı.";
        return false;
    }

    uint8_t *stage = (uint8_t *)(uintptr_t)DISK_BUF_BASE;
    uint32_t max_stage = DISK_BUF_SIZE / 2u;

    uint32_t efi_sz = 0;
    if (!inst_load_iso_to_buf("BOOTX64.EFI", stage, max_stage, &efi_sz)) {
        fat32_unmount();
        *err_out = "ISO kökünden BOOTX64.EFI okunamadı.";
        return false;
    }
    if (fat32_write_file("/EFI/ARCTIAN/BOOTX64.EFI", stage, efi_sz) != 0) {
        fat32_unmount();
        *err_out = "/EFI/ARCTIAN/BOOTX64.EFI yazılamadı.";
        return false;
    }

    uint32_t ex_sz = 0;
    int ex_dir = 0;
    bool write_boot_fallback = !is_dualboot ||
                               (fat_stat("/EFI/BOOT/BOOTX64.EFI", &ex_sz, &ex_dir) != 0);
    if (write_boot_fallback) {
        if (fat32_mkdir("/EFI/BOOT") != 0 ||
            fat32_write_file("/EFI/BOOT/BOOTX64.EFI", stage, efi_sz) != 0) {
            fat32_unmount();
            *err_out = "/EFI/BOOT/BOOTX64.EFI yazılamadı.";
            return false;
        }
    }

    uint32_t kelf_sz = 0;
    if (!inst_load_iso_to_buf("KERNEL.ELF", stage, max_stage, &kelf_sz)) {
        fat32_unmount();
        *err_out = "ISO kökünden KERNEL.ELF okunamadı.";
        return false;
    }
    if (fat32_write_file("/EFI/ARCTIAN/KERNEL.ELF", stage, kelf_sz) != 0) {
        fat32_unmount();
        *err_out = "/EFI/ARCTIAN/KERNEL.ELF yazılamadı.";
        return false;
    }

    fat32_unmount();
    if (blockdev_flush(target) != 0) {
        *err_out = "ESP önbelleği diske indirilemedi.";
        return false;
    }

    /* Dogrulama: tekrar mount edip boyutlari kontrol et */
    if (fat32_mount(target) != 0) {
        *err_out = "Doğrulama: ESP yeniden bağlanamadı.";
        return false;
    }
    uint32_t vsz = 0;
    int vdir = 0;
    if (fat_stat("/EFI/ARCTIAN/KERNEL.ELF", &vsz, &vdir) != 0 || vsz != kelf_sz || vdir) {
        fat32_unmount();
        *err_out = "Doğrulama: /EFI/ARCTIAN/KERNEL.ELF doğrulanamadı.";
        return false;
    }
    if (fat_stat("/EFI/ARCTIAN/BOOTX64.EFI", &vsz, &vdir) != 0 || vsz != efi_sz || vdir) {
        fat32_unmount();
        *err_out = "Doğrulama: /EFI/ARCTIAN/BOOTX64.EFI doğrulanamadı.";
        return false;
    }
    fat32_unmount();

    serial_printf("[inst] ESP hazir: dual=%d bootx64=%u kelf=%u fallback=%d\n",
                  is_dualboot ? 1 : 0, (unsigned)efi_sz, (unsigned)kelf_sz,
                  write_boot_fallback ? 1 : 0);
    return true;
}

/* -------------------------------------------------------------------------
 * Tam Kurulum Akisi (UEFI/GPT, BIOS/MBR, Dual-Boot)
 * ------------------------------------------------------------------------- */
static bool perform_install(const inst_disk_t *disk, inst_scheme_t scheme,
                            inst_method_t method, uint64_t total_secs_est,
                            uint64_t total_bytes_est, const char **err_out) {
    static const char *mods[3]   = { "INSTALL", "KPLUS", "DESKTOP" };
    static const char *labels[3] = {
        "Kurulum modülü yazılıyor (INSTALL)...",
        "Sistem çekirdeği yazılıyor (KPLUS)...",
        "Masaüstü ortamı yazılıyor (DESKTOP)..."
    };

    int target = disk->bdev_idx;
    bool is_dualboot = (method == INST_METHOD_DUALBOOT && disk->can_dualboot);
    bool is_gpt = is_dualboot || (scheme == INST_SCHEME_UEFI_GPT);

    inst_prog_t pr;
    pr.total_secs    = total_secs_est;
    pr.total_bytes   = total_bytes_est;
    pr.written_secs  = 0;
    pr.written_bytes = 0;
    pr.last_pct      = 0xFFFFFFFFu;
    pr.last_tenths   = ~0ULL;

    inst_render_progress_shell(disk, scheme, method);

    uint64_t part_base = 0;
    uint64_t part_secs = disk->info.sectors;

    if (is_dualboot) {
        inst_update_progress(1u, 0ULL, total_bytes_est,
                             "Dual-Boot GPT hazırlanıyor...",
                             "Mevcut bölümler korunarak yeni Arctian bölümü açılıyor", false);
        part_base = disk->free_start_lba;
        part_secs = disk->free_sectors;
        if (!inst_append_dualboot_gpt(target, disk->info.sectors, part_base, part_secs, err_out))
            return false;

        inst_update_progress(2u, 0ULL, total_bytes_est,
                             "Mevcut ESP güncelleniyor...",
                             "\\EFI\\ARCTIAN\\BOOTX64.EFI ve KERNEL.ELF kopyalanıyor", false);
        if (!inst_populate_esp(target, true, err_out))
            return false;
    } else if (scheme == INST_SCHEME_UEFI_GPT) {
        inst_update_progress(1u, 0ULL, total_bytes_est,
                             "GPT ve 64 MB FAT32 ESP oluşturuluyor...",
                             "UEFI bölüm tablosu diske yazılıyor", false);
        uint64_t esp_lba = 0, esp_secs = 0;
        if (!inst_create_clean_gpt(target, disk->info.sectors,
                                   &esp_lba, &esp_secs, &part_base, &part_secs, err_out))
            return false;

        if (!inst_format_fat32_esp(target, esp_lba, esp_secs, err_out))
            return false;

        inst_update_progress(2u, 0ULL, total_bytes_est,
                             "EFI önyükleme dosyaları kopyalanıyor...",
                             "\\EFI\\BOOT\\BOOTX64.EFI ve \\EFI\\ARCTIAN\\KERNEL.ELF", false);
        if (!inst_populate_esp(target, false, err_out))
            return false;
    } else {
        inst_update_progress(1u, 0ULL, total_bytes_est,
                             "BIOS (MBR) disk hazırlanıyor...",
                             "Önceki başlık kayıtları temizleniyor", false);
        inst_zero(verify_buf, 512);
        if (blockdev_write(target, 1ULL, 1, verify_buf) != 0) {
            *err_out = "LBA 1 temizlenemedi.";
            return false;
        }
    }

    /* Eski superblock temizle */
    inst_zero(verify_buf, 512);
    if (blockdev_write(target, part_base + SUPERBLOCK_LBA, 1, verify_buf) != 0) {
        *err_out = "Hedef bölüme yazılamadı (SUPERBLOCK_LBA).";
        return false;
    }

    /* HDIMG (part_base + 0; BIOS/MBR'de LBA 0 MBR bolum tablosu yamali) */
    uint32_t hd_size = 0;
    uint8_t hd_hdr[512];
    if (copy_iso_to_disk("HDIMG", target, part_base, &hd_size, hd_hdr,
                         !is_gpt, disk->info.sectors, &pr,
                         "Sistem imajı yazılıyor...",
                         "Önyükleme imajı yazılıyor (HDIMG)...") < 0) {
        *err_out = "Önyükleme imajı (HDIMG) okunamadı veya yazılamadı.";
        return false;
    }

    /* Moduller (part_base + 4096..) */
    module_entry_t table[MAX_MODULES];
    int mc = 0;
    uint64_t next_rel_lba = MODULE_DATA_START_LBA;

    for (int m = 0; m < 3; m++) {
        uint32_t fsize = 0;
        uint8_t mhdr[512];
        int64_t secs = copy_iso_to_disk(mods[m], target, part_base + next_rel_lba,
                                        &fsize, mhdr, false, 0, &pr,
                                        "Sistem imajı yazılıyor...", labels[m]);
        if (secs < 0 || next_rel_lba > 0xFFFFFFFFULL || (uint64_t)secs > 0xFFFFFFFFULL) {
            *err_out = "Sistem modülü kopyalanamadı (medya/disk hatası).";
            return false;
        }
        const amod_header_t *h = (const amod_header_t *)mhdr;
        if (h->magic != AMOD_MAGIC) {
            *err_out = "Geçersiz modül başlığı (AMOD).";
            return false;
        }

        module_entry_t *e = &table[mc++];
        for (int i = 0; i < 32; i++) e->name[i] = h->name[i];
        e->name[31]      = '\0';
        e->type          = h->type;
        e->abi_version   = h->abi_version;
        e->flags         = h->flags;
        e->load_addr     = h->load_addr;
        e->image_lba     = (uint32_t)next_rel_lba;
        e->image_sectors = (uint32_t)secs;
        e->image_size    = h->image_size;
        e->entry_off     = h->entry_off;
        e->checksum      = h->checksum;

        next_rel_lba += (uint64_t)secs;
    }

    /* AFS v7 dosya sistemi + ek bilesenler */
    bool webkit_copied = false;
    inst_afs_v7_sb_t afs_sb;
    if (!inst_populate_afs_v7(target, part_base, part_secs, &pr,
                              &webkit_copied, &afs_sb, err_out)) {
        return false;
    }

    /* Modul tablosu + Superblock + A/B yuva durumu */
    inst_update_progress(95u, total_bytes_est, total_bytes_est,
                         "Sistem imajı yazılıyor...",
                         "Modül tablosu ve superblock yazılıyor...", false);

    uint32_t tab_secs = (uint32_t)((mc * sizeof(module_entry_t) + 511u) / 512u);
    static uint8_t tabbuf[4096];
    inst_zero(tabbuf, sizeof(tabbuf));
    for (int i = 0; i < mc; i++) {
        inst_copy(tabbuf + i * sizeof(module_entry_t), &table[i], sizeof(module_entry_t));
    }
    if (blockdev_write(target, part_base + MODULE_TABLE_LBA, tab_secs, tabbuf) != 0) {
        *err_out = "Modül tablosu yazılamadı.";
        return false;
    }

    inst_zero(verify_buf, 512);
    superblock_t *sb = (superblock_t *)verify_buf;
    sb->magic              = SUPERBLOCK_MAGIC;
    sb->version            = SUPERBLOCK_VERSION;
    sb->header_size        = (uint16_t)sizeof(superblock_t);
    sb->flags              = SB_FLAG_INSTALLED;
    sb->module_count       = (uint32_t)mc;
    sb->module_tab_lba     = MODULE_TABLE_LBA;
    sb->module_tab_sectors = tab_secs;
    sb->state_lba          = STATE_LBA;
    sb->reserved           = 0;
    sb->checksum           = amod_checksum((const uint8_t *)sb, (uint32_t)(sizeof(superblock_t) - 4));
    if (blockdev_write(target, part_base + SUPERBLOCK_LBA, 1, verify_buf) != 0) {
        *err_out = "Superblock yazılamadı.";
        return false;
    }

    {
        uint8_t st[512];
        inst_zero(st, sizeof(st));
        inst_wr32(st + 0, 0x544C4F53u); /* "SOLT" */
        st[4] = 1; st[6] = 16;
        st[8] = 0; st[9] = 0xFF; st[10] = 1; st[11] = 0;
        inst_wr32(st + 12, inst_crc32(st, 12));
        if (blockdev_write(target, part_base + STATE_LBA, 1, st) != 0) {
            *err_out = "Yuva durumu yazılamadı.";
            return false;
        }
    }
    if (blockdev_flush(target) != 0) {
        *err_out = "Disk önbelleği diske yazılamadı.";
        return false;
    }

    /* Dogrulama */
    inst_update_progress(98u, total_bytes_est, total_bytes_est,
                         "Kurulum doğrulanıyor...",
                         "Önyükleyici, superblock, modül tablosu ve AFS v7 kontrol ediliyor", false);

    if (blockdev_read(target, part_base, 1, verify_buf) != 0) {
        *err_out = "Doğrulama: önyükleme sektörü okunamadı.";
        return false;
    }
    for (int i = 0; i < 512; i++) {
        if (verify_buf[i] != hd_hdr[i]) {
            *err_out = "Doğrulama: önyükleme sektörü uyuşmuyor.";
            return false;
        }
    }

    if (blockdev_read(target, part_base + SUPERBLOCK_LBA, 1, verify_buf) != 0) {
        *err_out = "Doğrulama: superblock okunamadı.";
        return false;
    }
    const superblock_t *sb_chk = (const superblock_t *)verify_buf;
    if (sb_chk->magic != SUPERBLOCK_MAGIC ||
        !(sb_chk->flags & SB_FLAG_INSTALLED) ||
        sb_chk->checksum != amod_checksum((const uint8_t *)sb_chk,
                                          (uint32_t)(sizeof(superblock_t) - 4))) {
        *err_out = "Doğrulama: superblock geçersiz.";
        return false;
    }

    if (blockdev_read(target, part_base + MODULE_TABLE_LBA, tab_secs, verify_buf) != 0) {
        *err_out = "Doğrulama: modül tablosu okunamadı.";
        return false;
    }
    for (unsigned i = 0; i < (unsigned)mc * sizeof(module_entry_t); i++) {
        if (verify_buf[i] != tabbuf[i]) {
            *err_out = "Doğrulama: modül tablosu uyuşmuyor.";
            return false;
        }
    }

    if (blockdev_read(target, part_base + INST_AFS_BASE_LBA, 1, verify_buf) != 0) {
        *err_out = "Doğrulama: AFS v7 başlığı okunamadı.";
        return false;
    }
    const uint8_t *sb_bytes = (const uint8_t *)&afs_sb;
    for (unsigned i = 0; i < 512u; i++) {
        if (verify_buf[i] != sb_bytes[i]) {
            *err_out = "Doğrulama: AFS v7 başlığı uyuşmuyor.";
            return false;
        }
    }
    if (webkit_copied) {
        const inst_afs_v7_inode_t *inodes = (const inst_afs_v7_inode_t *)s_v7_chunk_buf;
        int wk_idx = inst_v7_find_slot(inodes, "C:/webkit.elf");
        if (wk_idx < 0) {
            *err_out = "Doğrulama: C:/webkit.elf AFS kaydı eksik.";
            return false;
        }
        serial_printf("[inst] AFS v7 verify OK: C:/webkit.elf rel_lba=%u size=%u\n",
                      (unsigned)inodes[wk_idx].extents[0].lba,
                      (unsigned)inodes[wk_idx].size);
    }

    inst_update_progress(100u, total_bytes_est, total_bytes_est,
                         "Kurulum tamamlandı!",
                         "Tüm sistem dosyaları, ESP ve AFS v7 doğrulandı.", true);
    return true;
}
#endif /* !ARCTIAN_DEBUG_BOOT */

void install_main(void *arg) __attribute__((section(".text.entry")));
void install_main(void *arg) {
    (void)arg;
    terminal_initialize();
    show_arctian_header();

#ifdef ARCTIAN_DEBUG_BOOT
    serial_puts("[inst] debug build: kurulum atlandi\n");
    return;
#else
    serial_puts("[inst] grafiksel kurulum baslatiliyor\n");

    if (media_init() != 0) {
        terminal_writestring("Boot medyasi baslatilamadi.\n");
        serial_puts("[inst] media_init fail\n");
        return;
    }
    serial_puts("[inst] media ok\n");

    if (!media_is_cd()) {
        terminal_writestring("Canli ISO/CD ortami bulunamadi; kurulum atlandi.\n");
        serial_puts("[inst] CD degil, kurulum atlandi\n");
        return;
    }

    static const char *all_files[4] = { "HDIMG", "INSTALL", "KPLUS", "DESKTOP" };
    uint64_t total_secs = 0;
    uint64_t mod_secs = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t sz = 0;
        if (iso_size(all_files[i], &sz) == 0) {
            uint64_t s = ((uint64_t)sz + 511ULL) / 512ULL;
            total_secs += s;
            if (i > 0) mod_secs += s;
        }
    }
    uint64_t opt_secs = 0;
    {
        uint32_t sz = 0;
        if (iso_size("WEBKIT.ELF", &sz) == 0 && sz > 0)
            opt_secs += ((uint64_t)sz + 511ULL) / 512ULL;
        if (iso_size("DEJAVU.TTF", &sz) == 0 && sz > 0)
            opt_secs += 2ULL * (((uint64_t)sz + 511ULL) / 512ULL);
        if (iso_size("CACERT.PEM", &sz) == 0 && sz > 0)
            opt_secs += ((uint64_t)sz + 511ULL) / 512ULL;
        static const char *wlf_iso[5] = {
            "SAFAK.WLF", "MUREKKEP.WLF", "ABAKUS.WLF", "SAHNE.WLF", "MERCEK.WLF"
        };
        for (int i = 0; i < 5; i++) {
            if (iso_size(wlf_iso[i], &sz) == 0 && sz > 0)
                opt_secs += ((uint64_t)sz + 511ULL) / 512ULL;
        }
    }
    total_secs += opt_secs;
    if (total_secs == 0) total_secs = 1;
    uint64_t total_bytes = total_secs * 512ULL;
    uint64_t min_req_secs = (uint64_t)INST_AFS_BASE_LBA + 4096ULL + mod_secs + opt_secs;
    if (min_req_secs < MIN_INSTALL_SECS) min_req_secs = MIN_INSTALL_SECS;

    serial_puts("[inst] fb_init\n");
    inst_fb_init();
    serial_printf("[inst] fb active=%d w=%u h=%u bpp=%u phys=%llX mouse=%d\n",
                  (int)g_fb.active, (unsigned)g_fb.width, (unsigned)g_fb.height,
                  (unsigned)g_fb.bpp, (unsigned long long)g_fb.phys,
                  s_mouse_ready ? 1 : 0);

    static inst_disk_t disks[INST_MAX_DISKS];
    int disk_count = 0;
    serial_puts("[inst] scan_disks\n");
    scan_disks(disks, &disk_count, min_req_secs);
    serial_printf("[inst] disks=%d\n", disk_count);

    int sel = pick_default_disk(disks, disk_count);
    int scroll = 0;
    inst_scheme_t scheme = arctian_bootinfo_is_uefi(arctian_bootinfo())
                           ? INST_SCHEME_UEFI_GPT : INST_SCHEME_BIOS_MBR;
    inst_method_t method = INST_METHOD_CLEAN;
    bool confirm = false;
    const char *err_msg = 0;

    inst_kbd_flush();
    serial_puts("[inst] render_picker\n");
    inst_render_picker(disks, disk_count, sel, scroll, scheme, method,
                       confirm, err_msg, min_req_secs, true);
    serial_puts("[inst] picker ready\n");

    for (;;) {
        inst_key_t k = inst_wait_key();

        if (k == INST_KEY_RESCAN) {
            scan_disks(disks, &disk_count, min_req_secs);
            sel = pick_default_disk(disks, disk_count);
            scroll = 0;
            method = INST_METHOD_CLEAN;
            confirm = false;
            err_msg = 0;
            inst_render_picker(disks, disk_count, sel, scroll, scheme, method,
                               confirm, err_msg, min_req_secs, true);
            continue;
        }

        if (k == INST_KEY_MODE) {
            scheme = (scheme == INST_SCHEME_UEFI_GPT)
                     ? INST_SCHEME_BIOS_MBR : INST_SCHEME_UEFI_GPT;
            confirm = false;
            err_msg = 0;
            inst_render_picker(disks, disk_count, sel, scroll, scheme, method,
                               confirm, err_msg, min_req_secs, false);
            continue;
        }

        if (k == INST_KEY_ESC) {
            if (confirm || err_msg) {
                confirm = false;
                err_msg = 0;
                inst_render_picker(disks, disk_count, sel, scroll, scheme, method,
                                   confirm, err_msg, min_req_secs, false);
            }
            continue;
        }

        if (disk_count <= 0) {
            if (k == INST_KEY_ENTER) {
                err_msg = "Kurulum için disk bulunamadı! [R] ile yeniden tarayın.";
                inst_render_picker(disks, disk_count, sel, scroll, scheme, method,
                                   false, err_msg, min_req_secs, false);
            }
            continue;
        }

        if (k == INST_KEY_METHOD_CLEAN) {
            method = INST_METHOD_CLEAN;
            confirm = false;
            err_msg = 0;
            inst_render_picker(disks, disk_count, sel, scroll, scheme, method,
                               confirm, err_msg, min_req_secs, false);
            continue;
        }

        if (k == INST_KEY_METHOD_DUAL || k == INST_KEY_DUALBOOT ||
            k == INST_KEY_LEFT || k == INST_KEY_RIGHT) {
            if (disks[sel].can_dualboot) {
                if (k == INST_KEY_METHOD_DUAL) method = INST_METHOD_DUALBOOT;
                else if (k == INST_KEY_LEFT)   method = INST_METHOD_CLEAN;
                else if (k == INST_KEY_RIGHT)  method = INST_METHOD_DUALBOOT;
                else method = (method == INST_METHOD_CLEAN) ? INST_METHOD_DUALBOOT : INST_METHOD_CLEAN;
                confirm = false;
                err_msg = 0;
                inst_render_picker(disks, disk_count, sel, scroll, scheme, method,
                                   confirm, err_msg, min_req_secs, false);
            }
            continue;
        }

        if (k == INST_KEY_UP) {
            if (sel > 0) {
                sel--;
                if (sel < scroll) scroll = sel;
                method = INST_METHOD_CLEAN;
            }
            confirm = false;
            err_msg = 0;
            inst_render_picker(disks, disk_count, sel, scroll, scheme, method,
                               confirm, err_msg, min_req_secs, false);
            continue;
        }

        if (k == INST_KEY_DOWN) {
            if (sel + 1 < disk_count) {
                sel++;
                if (sel >= scroll + INST_MAX_VISIBLE) scroll = sel - INST_MAX_VISIBLE + 1;
                method = INST_METHOD_CLEAN;
            }
            confirm = false;
            err_msg = 0;
            inst_render_picker(disks, disk_count, sel, scroll, scheme, method,
                               confirm, err_msg, min_req_secs, false);
            continue;
        }

        if (k >= INST_KEY_1 && k <= INST_KEY_9) {
            int idx = (int)(k - INST_KEY_1);
            int clicked = s_clicked_disk_abs;
            s_clicked_disk_abs = -1;
            if (clicked >= 0 && clicked < disk_count) {
                if (sel != clicked) method = INST_METHOD_CLEAN;
                sel = clicked;
                if (sel < scroll) scroll = sel;
                if (sel >= scroll + INST_MAX_VISIBLE) scroll = sel - INST_MAX_VISIBLE + 1;
                confirm = false;
                err_msg = 0;
                inst_render_picker(disks, disk_count, sel, scroll, scheme, method,
                                   confirm, err_msg, min_req_secs, false);
            } else if (disks[sel].can_dualboot && (idx == 0 || idx == 1) &&
                       (confirm || idx >= disk_count)) {
                method = (idx == 1) ? INST_METHOD_DUALBOOT : INST_METHOD_CLEAN;
                err_msg = 0;
                inst_render_picker(disks, disk_count, sel, scroll, scheme, method,
                                   confirm, err_msg, min_req_secs, false);
            } else if (idx < disk_count) {
                if (sel != idx) method = INST_METHOD_CLEAN;
                sel = idx;
                if (sel < scroll) scroll = sel;
                if (sel >= scroll + INST_MAX_VISIBLE) scroll = sel - INST_MAX_VISIBLE + 1;
                confirm = false;
                err_msg = 0;
                inst_render_picker(disks, disk_count, sel, scroll, scheme, method,
                                   confirm, err_msg, min_req_secs, false);
            }
            continue;
        }

        if (k == INST_KEY_ENTER) {
            err_msg = 0;
            if (disks[sel].too_small) {
                static char small_err[96];
                char req_sz[24];
                inst_fmt_bytes(min_req_secs * 512ULL, req_sz, sizeof(req_sz));
                char *ep = small_err;
                char *eend = small_err + sizeof(small_err);
                ep = inst_str_append(ep, eend, "Seçili disk boyutu yetersiz! En az ");
                ep = inst_str_append(ep, eend, req_sz);
                inst_str_append(ep, eend, " alan gerekli.");
                err_msg = small_err;
                confirm = false;
                inst_render_picker(disks, disk_count, sel, scroll, scheme, method,
                                   confirm, err_msg, min_req_secs, false);
                continue;
            }
            if (!confirm) {
                confirm = true;
                inst_render_picker(disks, disk_count, sel, scroll, scheme, method,
                                   confirm, err_msg, min_req_secs, false);
                continue;
            }

            serial_printf("[inst] hedef disk=%s (%llu MB) scheme=%d method=%d\n",
                          disks[sel].info.name, (unsigned long long)disks[sel].size_mb,
                          (int)scheme, (int)method);
            const char *inst_err = 0;
            if (perform_install(&disks[sel], scheme, method, total_secs, total_bytes, &inst_err)) {
                serial_puts("[inst] kurulum tamamlandi\n");

                int secs_left = 15;
                bool auto_reboot = true;
                uint8_t prev_rtc = inst_cmos_sec();
                inst_kbd_flush();

                for (;;) {
                    char ftr[96];
                    if (auto_reboot) {
                        char *fp = ftr;
                        char *fend = ftr + sizeof(ftr);
                        fp = inst_str_append(fp, fend, "Otomatik yeniden başlatma: ");
                        fp = inst_u64_append(fp, fend, (uint64_t)secs_left);
                        inst_str_append(fp, fend, " sn");
                    } else {
                        inst_str_append(ftr, ftr + sizeof(ftr),
                                        "ISO medyasını çıkarın ve yeniden başlatın");
                    }
                    inst_render_status_card(false,
                                            "Kurulum başarıyla tamamlandı!",
                                            "Arctian OS hedef diske kuruldu ve doğrulandı.",
                                            ftr, auto_reboot);

                    for (;;) {
                        inst_key_t rk = inst_poll_key();
                        if (rk == INST_KEY_ENTER) {
                            serial_puts("[inst] yeniden baslatiliyor\n");
                            inst_reboot();
                        }
                        if (rk == INST_KEY_ESC && auto_reboot) {
                            auto_reboot = false;
                            break;
                        }
                        if (auto_reboot) {
                            uint8_t cur_rtc = inst_cmos_sec();
                            if (cur_rtc != prev_rtc) {
                                prev_rtc = cur_rtc;
                                secs_left--;
                                if (secs_left <= 0) {
                                    serial_puts("[inst] otomatik yeniden baslatiliyor\n");
                                    inst_reboot();
                                }
                                break;
                            }
                        }
                        for (int i = 0; i < 30000; i++) asm volatile("pause");
                    }
                }
            } else {
                serial_printf("[inst] HATA: %s\n", inst_err ? inst_err : "bilinmeyen");
                inst_render_status_card(true,
                                        "Kurulum Hatası!",
                                        inst_err ? inst_err : "Bilinmeyen yazma/okuma hatası.",
                                        "Disk seçim ekranına dönmek için [ENTER]", false);
                inst_kbd_flush();
                while (inst_wait_key() != INST_KEY_ENTER) { }

                scan_disks(disks, &disk_count, min_req_secs);
                sel = pick_default_disk(disks, disk_count);
                scroll = 0;
                method = INST_METHOD_CLEAN;
                confirm = false;
                err_msg = 0;
                inst_render_picker(disks, disk_count, sel, scroll, scheme, method,
                                   confirm, err_msg, min_req_secs, true);
            }
        }
    }
#endif /* !ARCTIAN_DEBUG_BOOT */
}
