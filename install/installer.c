/**
 * Arctian kalici kurulum modulu (installer).
 *
 * Debug ISO (ARCTIAN_DEBUG_BOOT):
 *  - Kurulumu atlar ve dogrudan canli masaustune gecer (sifir tus basimi).
 *
 * Release ISO (#ifndef ARCTIAN_DEBUG_BOOT):
 *  - Grafiksel disk secim ekrani + canli 0%-100% kurulum ilerleme arayuzu.
 *  - Kurulum tamamlandiginda yazilan superblock/tablo/onyukleyiciyi dogrular.
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
#include "printf.h"

#ifndef ARCTIAN_DEBUG_BOOT
#include "inst_ui.h"

#define CHUNK_SECS          128u
#define MIN_INSTALL_SECS    10485760ULL /* 5 GiB = 5 * 1024^3 / 512: sistem + WebKit + AFS */

/* AFS v6 sabitleri (lib/afs.c ile birebir uyumlu, dogrudan hedef diske yazar) */
#define INST_AFS_MAGIC       0x53464121u /* "!AFS" */
#define INST_AFS_VERSION     6u
#define INST_AFS_BASE_LBA    65536u
#define INST_AFS_MAX_FILES   1024u
#define INST_AFS_NAME_MAX    64u
#define INST_AFS_RESERVE_SEC 2048u
#define INST_AFS_JOURNAL_SEC 256u
#define INST_AFS_HDR_BASE    36u
#define INST_AFS_ENTRY_SIZE  (INST_AFS_NAME_MAX + 28u) /* 92 byte */
#define INST_AFS_HDR_SECS    ((INST_AFS_HDR_BASE + INST_AFS_MAX_FILES * INST_AFS_ENTRY_SIZE + 511u) / 512u)

typedef struct __attribute__((packed)) {
    char     name[INST_AFS_NAME_MAX];
    uint32_t lba;
    uint32_t size;
    uint32_t used;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    uint32_t pad;
} inst_afs_file_t;

typedef struct __attribute__((packed)) {
    uint32_t        magic;
    uint32_t        version;
    uint32_t        max_files;
    uint32_t        count;
    uint32_t        data_next;
    uint32_t        hdr_sectors;
    uint64_t        capacity;
    uint32_t        flags;
    inst_afs_file_t files[INST_AFS_MAX_FILES];
} inst_afs_hdr_t;

_Static_assert(sizeof(inst_afs_file_t) == 92u, "inst_afs_file_t must be 92 bytes");
_Static_assert(INST_AFS_HDR_SECS == 185u, "INST_AFS_HDR_SECS must be 185");

static uint8_t chunk[CHUNK_SECS * 512u];
static uint8_t verify_buf[4096];
static uint8_t s_afs_hdr_buf[INST_AFS_HDR_SECS * 512u];

typedef struct {
    uint64_t total_secs;
    uint64_t total_bytes;
    uint64_t written_secs;
    uint64_t written_bytes;
    uint32_t last_pct;
    uint64_t last_tenths;
} inst_prog_t;

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
                                inst_prog_t *pr, const char *step, const char *detail) {
    uint32_t lba2048 = 0, size = 0;
    if (iso9660_find(name, &lba2048, &size) != 0) return -1;
    if (out_size) *out_size = size;

    uint64_t total_secs = ((uint64_t)size + 511ULL) / 512ULL;
    uint64_t lba512 = (uint64_t)lba2048 * 4ULL;
    if (lba512 > 0xFFFFFFFFULL) return -1;

    if (hdr) {
        if (media_read((uint32_t)lba512, 1, hdr) != 0) return -1;
    }

    if (pr) prog_refresh(pr, step, detail, true);

    uint64_t done = 0;
    while (done < total_secs) {
        uint64_t rem = total_secs - done;
        uint32_t n = (rem > CHUNK_SECS) ? CHUNK_SECS : (uint32_t)rem;
        if (lba512 + done > 0xFFFFFFFFULL) return -1;
        if (media_read((uint32_t)(lba512 + done), n, chunk) != 0) return -1;
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

static uint32_t inst_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t inst_le64(const uint8_t *p) {
    return (uint64_t)inst_le32(p) | ((uint64_t)inst_le32(p + 4) << 32);
}

static bool guid_is_zero(const uint8_t g[16]) {
    for (int i = 0; i < 16; i++) {
        if (g[i] != 0) return false;
    }
    return true;
}

static bool guid_is_esp(const uint8_t g[16]) {
    /* EFI System Partition GUID: {C12A7328-F81F-11D2-BA4B-00A0C93EC93B} */
    static const uint8_t esp[16] = {
        0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
        0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
    };
    for (int i = 0; i < 16; i++) {
        if (g[i] != esp[i]) return false;
    }
    return true;
}

static void probe_disk_tables(int dev_idx, uint64_t total_sectors,
                              bool *out_gpt, bool *out_efi, bool *out_mbr,
                              uint32_t *out_gpt_parts) {
    *out_gpt = false;
    *out_efi = false;
    *out_mbr = false;
    *out_gpt_parts = 0;

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

    if (total_sectors >= 34ULL) {
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
                uint64_t part_lba = inst_le64(lba1 + 72);
                uint32_t num_ent  = inst_le32(lba1 + 80);
                uint32_t ent_sz   = inst_le32(lba1 + 84);
                if (ent_sz >= 128u && ent_sz <= 512u && (512u % ent_sz) == 0u &&
                    part_lba >= 2ULL && part_lba < total_sectors) {
                    if (num_ent > 128u) num_ent = 128u;
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
                            if (!guid_is_zero(ent)) {
                                (*out_gpt_parts)++;
                                if (guid_is_esp(ent)) *out_efi = true;
                            }
                        }
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

        uint8_t sb_sec[512];
        int has_arct = 0;
        if (blockdev_read(i, SUPERBLOCK_LBA, 1, sb_sec) == 0) {
            const superblock_t *sb = (const superblock_t *)sb_sec;
            if (sb->magic == SUPERBLOCK_MAGIC) has_arct = 1;
        }

        bool has_gpt = false, has_efi = false, has_mbr = false;
        uint32_t gpt_parts = 0;
        probe_disk_tables(i, info.sectors, &has_gpt, &has_efi, &has_mbr, &gpt_parts);

        out[cnt].bdev_idx     = i;
        out[cnt].info         = info;
        out[cnt].size_mb      = info.sectors / 2048ULL;
        out[cnt].has_arctian  = (uint8_t)has_arct;
        out[cnt].too_small    = (info.sectors < min_req_secs) ? 1u : 0u;
        out[cnt].has_gpt      = has_gpt;
        out[cnt].has_efi_part = has_efi;
        out[cnt].has_mbr      = has_mbr;
        out[cnt].gpt_parts    = gpt_parts;

        serial_printf("[inst] disk#%d %s %lluMB gpt=%d efi=%d mbr=%d parts=%u arctian=%d\n",
                      cnt, info.name, (unsigned long long)out[cnt].size_mb,
                      has_gpt ? 1 : 0, has_efi ? 1 : 0, has_mbr ? 1 : 0,
                      (unsigned)gpt_parts, has_arct);
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
        if (!disks[i].too_small && !disks[i].has_arctian && !disks[i].has_efi_part) return i;
    }
    for (int i = 0; i < count; i++) {
        if (!disks[i].too_small && !disks[i].has_arctian) return i;
    }
    for (int i = 0; i < count; i++) {
        if (!disks[i].too_small) return i;
    }
    return 0;
}

static uint32_t inst_afs_crc32(const void *data, uint32_t len) {
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

static int inst_afs_find_slot(const inst_afs_hdr_t *hdr, const char *name) {
    for (uint32_t i = 0; i < hdr->max_files; i++) {
        if (hdr->files[i].used && inst_str_eq(hdr->files[i].name, name))
            return (int)i;
    }
    return -1;
}

static bool inst_afs_add_entry(inst_afs_hdr_t *hdr, const char *name, uint32_t lba,
                               uint32_t size, uint32_t uid, uint32_t gid, uint32_t mode) {
    int existing = inst_afs_find_slot(hdr, name);
    if (existing >= 0) return true;
    for (uint32_t i = 0; i < hdr->max_files; i++) {
        if (!hdr->files[i].used) {
            inst_afs_file_t *f = &hdr->files[i];
            uint32_t k = 0;
            for (; name[k] && k < INST_AFS_NAME_MAX - 1u; k++)
                f->name[k] = name[k];
            f->name[k] = '\0';
            f->lba  = lba;
            f->size = size;
            f->used = 1u;
            f->uid  = uid;
            f->gid  = gid;
            f->mode = mode;
            f->pad  = 0u;
            hdr->count++;
            return true;
        }
    }
    return false;
}

static bool inst_afs_ensure_dir(int target, inst_afs_hdr_t *hdr, uint32_t *data_next,
                                const char *path, uint32_t uid, uint32_t gid) {
    if (inst_afs_find_slot(hdr, path) >= 0) return true;
    if ((uint64_t)(*data_next) + 1ULL > hdr->capacity) return false;
    for (unsigned i = 0; i < 512u; i++) verify_buf[i] = 0;
    if (blockdev_write(target, *data_next, 1, verify_buf) != 0) return false;
    if (!inst_afs_add_entry(hdr, path, *data_next, 0u, uid, gid, 0777u)) return false;
    (*data_next)++;
    return true;
}

static bool inst_afs_copy_from_iso(int target, inst_afs_hdr_t *hdr, uint32_t *data_next,
                                    const char *iso_name, const char *afs_path,
                                    uint32_t uid, uint32_t gid, uint32_t mode,
                                    inst_prog_t *pr, const char *step, const char *detail,
                                    bool *out_copied) {
    if (out_copied) *out_copied = false;
    uint32_t lba2048 = 0, size = 0;
    if (iso9660_find(iso_name, &lba2048, &size) != 0 || size == 0) {
        return true; /* Opsiyonel ISO dosyasi yok; sessizce atla */
    }

    uint64_t total_secs = ((uint64_t)size + 511ULL) / 512ULL;
    uint64_t lba512 = (uint64_t)lba2048 * 4ULL;
    if (lba512 > 0xFFFFFFFFULL || total_secs == 0 || total_secs > 0xFFFFFFFFULL) return false;
    if ((uint64_t)(*data_next) + total_secs > hdr->capacity) return false;

    uint32_t dst_lba = *data_next;
    if (pr) prog_refresh(pr, step, detail, true);

    uint64_t done = 0;
    while (done < total_secs) {
        uint64_t rem = total_secs - done;
        uint32_t n = (rem > CHUNK_SECS) ? CHUNK_SECS : (uint32_t)rem;
        if (lba512 + done > 0xFFFFFFFFULL) return false;
        if (media_read((uint32_t)(lba512 + done), n, chunk) != 0) return false;
        if (blockdev_write(target, (uint64_t)dst_lba + done, n, chunk) != 0) return false;
        done += n;
        if (pr) {
            pr->written_secs  += n;
            pr->written_bytes += (uint64_t)n * 512ULL;
            prog_refresh(pr, step, detail, false);
        }
    }
    if (blockdev_flush(target) != 0) return false;

    if (!inst_afs_add_entry(hdr, afs_path, dst_lba, size, uid, gid, mode)) return false;
    *data_next += (uint32_t)total_secs;
    if (out_copied) *out_copied = true;
    return true;
}

static bool inst_populate_afs(const inst_disk_t *disk, inst_prog_t *pr,
                              bool *out_webkit_ok, const char **err_out) {
    int target = disk->bdev_idx;
    uint64_t disk_secs = disk->info.sectors;
    *out_webkit_ok = false;

    uint64_t cap = (disk_secs > INST_AFS_RESERVE_SEC) ? (disk_secs - INST_AFS_RESERVE_SEC) : disk_secs;
    if (cap > 0xFFFFFFFEULL) cap = 0xFFFFFFFEULL;
    if (cap > (uint64_t)INST_AFS_BASE_LBA + INST_AFS_JOURNAL_SEC)
        cap -= INST_AFS_JOURNAL_SEC;
    if (cap <= (uint64_t)INST_AFS_BASE_LBA + INST_AFS_HDR_SECS + 16ULL) {
        *err_out = "AFS dosya sistemi icin disk alani yetersiz.";
        return false;
    }

    /* Eski gunluk (journal) varsa temizle ki acilista eski baslik donmesin */
    if (disk_secs >= (uint64_t)INST_AFS_BASE_LBA + INST_AFS_RESERVE_SEC + INST_AFS_JOURNAL_SEC) {
        uint64_t jl = disk_secs - INST_AFS_RESERVE_SEC - INST_AFS_JOURNAL_SEC;
        for (unsigned i = 0; i < 512u; i++) verify_buf[i] = 0;
        if (blockdev_write(target, jl, 1, verify_buf) != 0) {
            *err_out = "AFS gunluk sektoru temizlenemedi.";
            return false;
        }
    }

    for (unsigned i = 0; i < sizeof(s_afs_hdr_buf); i++) s_afs_hdr_buf[i] = 0;
    inst_afs_hdr_t *hdr = (inst_afs_hdr_t *)s_afs_hdr_buf;
    hdr->magic       = INST_AFS_MAGIC;
    hdr->version     = INST_AFS_VERSION;
    hdr->max_files   = INST_AFS_MAX_FILES;
    hdr->count       = 0;
    hdr->hdr_sectors = INST_AFS_HDR_SECS;
    hdr->capacity    = cap;
    hdr->flags       = 0;

    uint32_t data_next = INST_AFS_BASE_LBA + INST_AFS_HDR_SECS;

    /* Standart kullanici agaci (afs_ensure_tree ile uyumlu) */
    static const char *base_dirs[6] = {
        "C:/", "C:/apps/", "C:/indirilenler/", "C:/belgeler/", "C:/masaustu/", "C:/logs/"
    };
    for (int i = 0; i < 6; i++) {
        if (!inst_afs_ensure_dir(target, hdr, &data_next, base_dirs[i], 1000u, 1000u)) {
            *err_out = "AFS kok dizinleri olusturulamadi.";
            return false;
        }
    }

    /* Opsiyonel ISO paketleri: WPE WebKit + DejaVuSans + CA sertifikalari */
    uint32_t opt_sz = 0;
    if (iso_size("WEBKIT.ELF", &opt_sz) == 0 && opt_sz > 0) {
        bool wk_ok = false;
        if (!inst_afs_copy_from_iso(target, hdr, &data_next, "WEBKIT.ELF", "C:/webkit.elf",
                                    1000u, 1000u, 0755u, pr,
                                    "Ek bilesenler kuruluyor...",
                                    "WPE WebKit 2.44 tarayici kopyalaniyor (C:/webkit.elf)...",
                                    &wk_ok)) {
            *err_out = "WEBKIT.ELF kopyalanamadi (medya/disk hatasi).";
            return false;
        }
        *out_webkit_ok = wk_ok;
    }

    if (iso_size("DEJAVU.TTF", &opt_sz) == 0 && opt_sz > 0) {
        bool f1 = false, f2 = false;
        if (!inst_afs_ensure_dir(target, hdr, &data_next, "C:/design/", 1000u, 1000u) ||
            !inst_afs_ensure_dir(target, hdr, &data_next, "C:/design/font/", 1000u, 1000u) ||
            !inst_afs_ensure_dir(target, hdr, &data_next, "C:/usr/", 1000u, 1000u) ||
            !inst_afs_ensure_dir(target, hdr, &data_next, "C:/usr/share/", 1000u, 1000u) ||
            !inst_afs_ensure_dir(target, hdr, &data_next, "C:/usr/share/fonts/", 1000u, 1000u)) {
            *err_out = "AFS yazi tipi dizinleri olusturulamadi.";
            return false;
        }
        if (!inst_afs_copy_from_iso(target, hdr, &data_next, "DEJAVU.TTF",
                                    "C:/design/font/DejaVuSans.ttf",
                                    1000u, 1000u, 0644u, pr,
                                    "Ek bilesenler kuruluyor...",
                                    "Sistem yazi tipi kopyalaniyor (DejaVuSans.ttf)...",
                                    &f1) ||
            !inst_afs_copy_from_iso(target, hdr, &data_next, "DEJAVU.TTF",
                                    "C:/usr/share/fonts/DejaVuSans.ttf",
                                    1000u, 1000u, 0644u, pr,
                                    "Ek bilesenler kuruluyor...",
                                    "Sistem yazi tipi kopyalaniyor (/usr/share/fonts)...",
                                    &f2)) {
            *err_out = "DEJAVU.TTF kopyalanamadi.";
            return false;
        }
    }

    if (iso_size("CACERT.PEM", &opt_sz) == 0 && opt_sz > 0) {
        bool ca_ok = false;
        if (!inst_afs_ensure_dir(target, hdr, &data_next, "C:/etc/", 0u, 0u) ||
            !inst_afs_ensure_dir(target, hdr, &data_next, "C:/etc/ssl/", 0u, 0u) ||
            !inst_afs_ensure_dir(target, hdr, &data_next, "C:/etc/ssl/certs/", 0u, 0u)) {
            *err_out = "AFS sertifika dizinleri olusturulamadi.";
            return false;
        }
        if (!inst_afs_copy_from_iso(target, hdr, &data_next, "CACERT.PEM",
                                    "C:/etc/ssl/certs/ca-certificates.crt",
                                    0u, 0u, 0644u, pr,
                                    "Ek bilesenler kuruluyor...",
                                    "Kok TLS sertifikalari kopyalaniyor (ca-certificates.crt)...",
                                    &ca_ok)) {
            *err_out = "CACERT.PEM kopyalanamadi.";
            return false;
        }
    }

    hdr->data_next = data_next;
    hdr->flags = 0;
    hdr->flags = inst_afs_crc32(s_afs_hdr_buf,
                                INST_AFS_HDR_BASE + INST_AFS_MAX_FILES * INST_AFS_ENTRY_SIZE);

    if (blockdev_write(target, INST_AFS_BASE_LBA, INST_AFS_HDR_SECS, s_afs_hdr_buf) != 0 ||
        blockdev_flush(target) != 0) {
        *err_out = "AFS baslik tablosu diske yazilamadi.";
        return false;
    }

    serial_printf("[inst] AFS v6 hazir: base=%u files=%u data_next=%u webkit=%d\n",
                  (unsigned)INST_AFS_BASE_LBA, (unsigned)hdr->count,
                  (unsigned)hdr->data_next, *out_webkit_ok ? 1 : 0);
    return true;
}

static bool perform_install(const inst_disk_t *disk, uint64_t total_secs_est,
                            uint64_t total_bytes_est, const char **err_out) {
    static const char *mods[3]   = { "INSTALL", "KPLUS", "DESKTOP" };
    static const char *labels[3] = {
        "Kurulum modulu yaziliyor (INSTALL)...",
        "Sistem cekirdegi yaziliyor (KPLUS)...",
        "Masaustu ortami yaziliyor (DESKTOP)..."
    };

    int target = disk->bdev_idx;
    inst_prog_t pr;
    pr.total_secs    = total_secs_est;
    pr.total_bytes   = total_bytes_est;
    pr.written_secs  = 0;
    pr.written_bytes = 0;
    pr.last_pct      = 0xFFFFFFFFu;
    pr.last_tenths   = ~0ULL;

    inst_render_progress_shell(disk);

    /* Adim 1: Disk hazirlaniyor (eski superblock varsa temizle) */
    inst_update_progress(1u, 0ULL, total_bytes_est,
                         "Disk hazirlaniyor...",
                         "Onceki baslik kayitlari temizleniyor", false);
    for (unsigned i = 0; i < 512u; i++) verify_buf[i] = 0;
    if (blockdev_write(target, SUPERBLOCK_LBA, 1, verify_buf) != 0) {
        *err_out = "Hedef diske yazilamadi (LBA 2048).";
        return false;
    }

    /* Adim 2: Onyukleyici (LBA 0) */
    uint32_t hd_size = 0;
    uint8_t hd_hdr[512];
    if (copy_iso_to_disk("HDIMG", target, 0ULL, &hd_size, hd_hdr, &pr,
                         "Sistem imaji yaziliyor...",
                         "Onyukleme imaji yaziliyor (HDIMG)...") < 0) {
        *err_out = "Onyukleme imaji (HDIMG) okunamadi veya yazilamadi.";
        return false;
    }

    /* Moduller + modul tablosu */
    module_entry_t table[MAX_MODULES];
    int mc = 0;
    uint64_t next_lba = MODULE_DATA_START_LBA;

    for (int m = 0; m < 3; m++) {
        uint32_t fsize = 0;
        uint8_t mhdr[512];
        int64_t secs = copy_iso_to_disk(mods[m], target, next_lba, &fsize, mhdr, &pr,
                                        "Sistem imaji yaziliyor...", labels[m]);
        if (secs < 0 || next_lba > 0xFFFFFFFFULL || (uint64_t)secs > 0xFFFFFFFFULL) {
            *err_out = "Sistem modulu kopyalanamadi (medya/disk hatasi).";
            return false;
        }
        const amod_header_t *h = (const amod_header_t *)mhdr;
        if (h->magic != AMOD_MAGIC) {
            *err_out = "Gecersiz modul basligi (AMOD).";
            return false;
        }

        module_entry_t *e = &table[mc++];
        for (int i = 0; i < 32; i++) e->name[i] = h->name[i];
        e->name[31]      = '\0';
        e->type          = h->type;
        e->abi_version   = h->abi_version;
        e->flags         = h->flags;
        e->load_addr     = h->load_addr;
        e->image_lba     = (uint32_t)next_lba;
        e->image_sectors = (uint32_t)secs;
        e->image_size    = h->image_size;
        e->entry_off     = h->entry_off;
        e->checksum      = h->checksum;

        next_lba += (uint64_t)secs;
    }

    /* Adim 2b: AFS dosya sistemini formatla + WebKit/font/sertifika yukle */
    bool webkit_copied = false;
    if (!inst_populate_afs(disk, &pr, &webkit_copied, err_out)) {
        return false;
    }

    /* Adim 3: Modul tablosu + Superblock + A/B yuva durumu */
    inst_update_progress(95u, total_bytes_est, total_bytes_est,
                         "Sistem imaji yaziliyor...",
                         "Modul tablosu ve superblock yaziliyor...", false);

    uint32_t tab_secs = (uint32_t)((mc * sizeof(module_entry_t) + 511u) / 512u);
    static uint8_t tabbuf[4096];
    for (unsigned i = 0; i < sizeof(tabbuf); i++) tabbuf[i] = 0;
    for (int i = 0; i < mc; i++) {
        const uint8_t *src = (const uint8_t *)&table[i];
        for (unsigned j = 0; j < sizeof(module_entry_t); j++)
            tabbuf[i * sizeof(module_entry_t) + j] = src[j];
    }
    if (blockdev_write(target, MODULE_TABLE_LBA, tab_secs, tabbuf) != 0) {
        *err_out = "Modul tablosu yazilamadi.";
        return false;
    }

    for (unsigned i = 0; i < 512u; i++) verify_buf[i] = 0;
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
    if (blockdev_write(target, SUPERBLOCK_LBA, 1, verify_buf) != 0) {
        *err_out = "Superblock yazilamadi.";
        return false;
    }

    /* Temiz A/B yuva durumu: A etkin ve saglam. */
    {
        uint8_t st[512];
        for (unsigned i = 0; i < sizeof(st); i++) st[i] = 0;
        uint32_t magic = 0x544C4F53u; /* "SOLT" */
        st[0] = (uint8_t)magic; st[1] = (uint8_t)(magic >> 8);
        st[2] = (uint8_t)(magic >> 16); st[3] = (uint8_t)(magic >> 24);
        st[4] = 1; st[6] = 16;
        st[8] = 0; st[9] = 0xFF; st[10] = 1; st[11] = 0;
        uint32_t c = 0xFFFFFFFFu;
        for (int i = 0; i < 12; i++) {
            c ^= st[i];
            for (int k = 0; k < 8; k++)
                c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1u)));
        }
        c ^= 0xFFFFFFFFu;
        st[12] = (uint8_t)c; st[13] = (uint8_t)(c >> 8);
        st[14] = (uint8_t)(c >> 16); st[15] = (uint8_t)(c >> 24);
        if (blockdev_write(target, STATE_LBA, 1, st) != 0) {
            *err_out = "Yuva durumu yazilamadi.";
            return false;
        }
    }
    if (blockdev_flush(target) != 0) {
        *err_out = "Disk onbellegi diske yazilamadi.";
        return false;
    }

    /* Adim 4: Kurulum dogrulaniyor */
    inst_update_progress(98u, total_bytes_est, total_bytes_est,
                         "Kurulum dogrulaniyor...",
                         "Onyukleyici, superblock ve modul tablosu kontrol ediliyor", false);

    if (blockdev_read(target, 0, 1, verify_buf) != 0) {
        *err_out = "Dogrulama: LBA 0 okunamadi.";
        return false;
    }
    for (int i = 0; i < 512; i++) {
        if (verify_buf[i] != hd_hdr[i]) {
            *err_out = "Dogrulama: onyukleme sektoru uyusmuyor.";
            return false;
        }
    }

    if (blockdev_read(target, SUPERBLOCK_LBA, 1, verify_buf) != 0) {
        *err_out = "Dogrulama: superblock okunamadi.";
        return false;
    }
    const superblock_t *sb_chk = (const superblock_t *)verify_buf;
    if (sb_chk->magic != SUPERBLOCK_MAGIC ||
        !(sb_chk->flags & SB_FLAG_INSTALLED) ||
        sb_chk->checksum != amod_checksum((const uint8_t *)sb_chk,
                                          (uint32_t)(sizeof(superblock_t) - 4))) {
        *err_out = "Dogrulama: superblock gecersiz.";
        return false;
    }

    if (blockdev_read(target, MODULE_TABLE_LBA, tab_secs, verify_buf) != 0) {
        *err_out = "Dogrulama: modul tablosu okunamadi.";
        return false;
    }
    for (unsigned i = 0; i < (unsigned)mc * sizeof(module_entry_t); i++) {
        if (verify_buf[i] != tabbuf[i]) {
            *err_out = "Dogrulama: modul tablosu uyusmuyor.";
            return false;
        }
    }

    if (blockdev_read(target, INST_AFS_BASE_LBA, 1, verify_buf) != 0) {
        *err_out = "Dogrulama: AFS basligi okunamadi.";
        return false;
    }
    for (unsigned i = 0; i < 512u; i++) {
        if (verify_buf[i] != s_afs_hdr_buf[i]) {
            *err_out = "Dogrulama: AFS basligi uyusmuyor.";
            return false;
        }
    }
    if (webkit_copied) {
        const inst_afs_hdr_t *afs_hdr = (const inst_afs_hdr_t *)s_afs_hdr_buf;
        int wk_idx = inst_afs_find_slot(afs_hdr, "C:/webkit.elf");
        if (wk_idx < 0) {
            *err_out = "Dogrulama: C:/webkit.elf AFS kaydi eksik.";
            return false;
        }
        serial_printf("[inst] AFS verify OK: C:/webkit.elf lba=%u size=%u\n",
                      (unsigned)afs_hdr->files[wk_idx].lba,
                      (unsigned)afs_hdr->files[wk_idx].size);
    }

    inst_update_progress(100u, total_bytes_est, total_bytes_est,
                         "Kurulum tamamlandi!",
                         "Tum sistem dosyalari ve onyukleyici dogrulandi.", true);
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

    /* Toplam yazilacak boyut (0%-100% ilerleme hesabi ve disk boyut denetimi) */
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
    }
    total_secs += opt_secs;
    if (total_secs == 0) total_secs = 1;
    uint64_t total_bytes = total_secs * 512ULL;
    uint64_t min_req_secs = (uint64_t)INST_AFS_BASE_LBA + 4096ULL + mod_secs + opt_secs;
    if (min_req_secs < MIN_INSTALL_SECS) min_req_secs = MIN_INSTALL_SECS;

    serial_puts("[inst] fb_init\n");
    inst_fb_init();
    serial_printf("[inst] fb active=%d w=%u h=%u bpp=%u phys=%llX\n",
                  (int)g_fb.active, (unsigned)g_fb.width, (unsigned)g_fb.height,
                  (unsigned)g_fb.bpp, (unsigned long long)g_fb.phys);

    static inst_disk_t disks[INST_MAX_DISKS];
    int disk_count = 0;
    serial_puts("[inst] scan_disks\n");
    scan_disks(disks, &disk_count, min_req_secs);
    serial_printf("[inst] disks=%d\n", disk_count);

    int sel = pick_default_disk(disks, disk_count);
    int scroll = 0;
    bool confirm = false;
    const char *err_msg = 0;

    inst_kbd_flush();
    serial_puts("[inst] render_picker\n");
    inst_render_picker(disks, disk_count, sel, scroll, confirm, err_msg, min_req_secs, true);
    serial_puts("[inst] picker ready\n");

    for (;;) {
        inst_key_t k = inst_wait_key();

        if (k == INST_KEY_RESCAN) {
            scan_disks(disks, &disk_count, min_req_secs);
            sel = pick_default_disk(disks, disk_count);
            scroll = 0;
            confirm = false;
            err_msg = 0;
            inst_render_picker(disks, disk_count, sel, scroll, confirm, err_msg, min_req_secs, true);
            continue;
        }

        if (k == INST_KEY_ESC) {
            if (confirm || err_msg) {
                confirm = false;
                err_msg = 0;
                inst_render_picker(disks, disk_count, sel, scroll, confirm, err_msg, min_req_secs, false);
            }
            continue;
        }

        if (disk_count <= 0) {
            if (k == INST_KEY_ENTER) {
                err_msg = "Kurulum icin disk bulunamadi! [R] ile yeniden tarayin.";
                inst_render_picker(disks, disk_count, sel, scroll, false, err_msg, min_req_secs, false);
            }
            continue;
        }

        if (k == INST_KEY_UP) {
            if (sel > 0) {
                sel--;
                if (sel < scroll) scroll = sel;
            }
            confirm = false;
            err_msg = 0;
            inst_render_picker(disks, disk_count, sel, scroll, confirm, err_msg, min_req_secs, false);
            continue;
        }

        if (k == INST_KEY_DOWN) {
            if (sel + 1 < disk_count) {
                sel++;
                if (sel >= scroll + INST_MAX_VISIBLE) scroll = sel - INST_MAX_VISIBLE + 1;
            }
            confirm = false;
            err_msg = 0;
            inst_render_picker(disks, disk_count, sel, scroll, confirm, err_msg, min_req_secs, false);
            continue;
        }

        if (k >= INST_KEY_1 && k <= INST_KEY_9) {
            int idx = (int)(k - INST_KEY_1);
            if (idx < disk_count) {
                sel = idx;
                if (sel < scroll) scroll = sel;
                if (sel >= scroll + INST_MAX_VISIBLE) scroll = sel - INST_MAX_VISIBLE + 1;
                confirm = false;
                err_msg = 0;
                inst_render_picker(disks, disk_count, sel, scroll, confirm, err_msg, min_req_secs, false);
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
                ep = inst_str_append(ep, eend, "Secili disk boyutu yetersiz! En az ");
                ep = inst_str_append(ep, eend, req_sz);
                inst_str_append(ep, eend, " alan gerekli.");
                err_msg = small_err;
                confirm = false;
                inst_render_picker(disks, disk_count, sel, scroll, confirm, err_msg, min_req_secs, false);
                continue;
            }
            if (!confirm) {
                confirm = true;
                inst_render_picker(disks, disk_count, sel, scroll, confirm, err_msg, min_req_secs, false);
                continue;
            }

            /* Onaylandi: kurulumu baslat */
            serial_printf("[inst] hedef disk=%s (%llu MB)\n",
                          disks[sel].info.name, (unsigned long long)disks[sel].size_mb);
            const char *inst_err = 0;
            if (perform_install(&disks[sel], total_secs, total_bytes, &inst_err)) {
                serial_puts("[inst] kurulum tamamlandi\n");

                /* Basari ekrani + 15 sn yeniden baslatma sayaci */
                int secs_left = 15;
                bool auto_reboot = true;
                uint8_t prev_rtc = inst_cmos_sec();
                inst_kbd_flush();

                for (;;) {
                    char ftr[96];
                    if (auto_reboot) {
                        char *fp = ftr;
                        char *fend = ftr + sizeof(ftr);
                        fp = inst_str_append(fp, fend, "[ENTER] Simdi Yeniden Baslat   |   Otomatik: ");
                        fp = inst_u64_append(fp, fend, (uint64_t)secs_left);
                        inst_str_append(fp, fend, " sn ([ESC] Durdur)");
                    } else {
                        inst_str_append(ftr, ftr + sizeof(ftr),
                                        "ISO medyasini cikarin ve [ENTER] ile yeniden baslatin");
                    }
                    inst_render_status_card(false,
                                            "Kurulum basariyla tamamlandi!",
                                            "Arctian OS hedef diske kuruldu ve dogrulandi.",
                                            ftr);

                    /* 1 saniye boyunca tus bekle veya RTC degisimini izle */
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
                                        "Kurulum Hatasi!",
                                        inst_err ? inst_err : "Bilinmeyen yazma/okuma hatasi.",
                                        "[ENTER] Disk secim ekranina don");
                inst_kbd_flush();
                while (inst_wait_key() != INST_KEY_ENTER) { }

                scan_disks(disks, &disk_count, min_req_secs);
                sel = pick_default_disk(disks, disk_count);
                scroll = 0;
                confirm = false;
                err_msg = 0;
                inst_render_picker(disks, disk_count, sel, scroll, confirm, err_msg, min_req_secs, true);
            }
        }
    }
#endif /* !ARCTIAN_DEBUG_BOOT */
}
