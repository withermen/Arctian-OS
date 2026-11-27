#include "fat32.h"
#include "blockdev.h"
#include "kernel.h"

typedef struct {
    int      bdev;
    int      mounted;
    int      is_fat32;
    int      is_fat12;
    int      io_error;
    uint64_t part_lba;       /* Partition start LBA in 512-byte sectors */
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint32_t fat_size;
    uint32_t root_dir_sectors;
    uint32_t first_fat_sector;
    uint32_t first_root_dir_sector;
    uint32_t first_data_sector;
    uint32_t count_of_clusters;
    uint32_t root_cluster;
    uint32_t total_sectors;
    uint32_t bps512;
    uint16_t fsinfo_sec;
    uint32_t next_free_hint;
    char     label[12];
} fat32_t;

static fat32_t s_fs;
static uint8_t s_sec[4096];
static uint8_t s_fatbuf[4096];
static uint8_t s_fat_cache[4096];
static uint32_t s_fat_cache_lba;
static int s_fat_cache_valid;

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void str_copy_n(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* UTF-8 decoder & Turkish-aware case-folding compare */
static uint32_t utf8_next(const char **ps) {
    const unsigned char *s = (const unsigned char *)*ps;
    unsigned char c = s[0];
    if (c == 0) return 0;
    if (c < 0x80) { (*ps)++; return c; }
    if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
        *ps += 2;
        return cp;
    }
    if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (uint32_t)(s[2] & 0x3F);
        *ps += 3;
        return cp;
    }
    if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
                      ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
        *ps += 4;
        return cp;
    }
    (*ps)++;
    return c;
}

static uint32_t fold_cp(uint32_t cp) {
    if (cp >= 'a' && cp <= 'z') return cp - 32;
    switch (cp) {
        case 0x011F: return 0x011E; /* ğ -> Ğ */
        case 0x00FC: return 0x00DC; /* ü -> Ü */
        case 0x015F: return 0x015E; /* ş -> Ş */
        case 0x00F6: return 0x00D6; /* ö -> Ö */
        case 0x00E7: return 0x00C7; /* ç -> Ç */
        case 0x0131: return 'I';    /* ı -> I */
        case 0x0130: return 'I';    /* İ -> I */
        default: return cp;
    }
}

static int str_eq_ci(const char *a, const char *b) {
    for (;;) {
        uint32_t ca = fold_cp(utf8_next(&a));
        uint32_t cb = fold_cp(utf8_next(&b));
        if (ca != cb) return 0;
        if (ca == 0) return 1;
    }
}

static int utf16_to_utf8(const uint16_t *u16, int max_u16, char *out, int cap) {
    int p = 0;
    for (int i = 0; i < max_u16; i++) {
        uint16_t w = u16[i];
        if (w == 0 || w == 0xFFFF) break;
        uint32_t cp = w;
        if (w >= 0xD800 && w <= 0xDBFF && i + 1 < max_u16) {
            uint16_t w2 = u16[i + 1];
            if (w2 >= 0xDC00 && w2 <= 0xDFFF) {
                cp = 0x10000u + (((uint32_t)(w - 0xD800) << 10) | (uint32_t)(w2 - 0xDC00));
                i++;
            }
        }
        if (cp < 0x80) {
            if (p + 1 >= cap) break;
            out[p++] = (char)cp;
        } else if (cp < 0x800) {
            if (p + 2 >= cap) break;
            out[p++] = (char)(0xC0 | (cp >> 6));
            out[p++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            if (p + 3 >= cap) break;
            out[p++] = (char)(0xE0 | (cp >> 12));
            out[p++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[p++] = (char)(0x80 | (cp & 0x3F));
        } else {
            if (p + 4 >= cap) break;
            out[p++] = (char)(0xF0 | (cp >> 18));
            out[p++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[p++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[p++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    if (cap > 0) out[p] = 0;
    return p;
}

static int utf8_to_utf16(const char *s, uint16_t *out, int cap) {
    int n = 0;
    while (*s && n < cap - 1) {
        uint32_t cp = utf8_next(&s);
        if (cp == 0) break;
        if (cp <= 0xFFFF) {
            out[n++] = (uint16_t)cp;
        } else if (cp <= 0x10FFFF && n + 2 <= cap - 1) {
            cp -= 0x10000;
            out[n++] = (uint16_t)(0xD800 + (cp >> 10));
            out[n++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        }
    }
    out[n] = 0;
    return n;
}

static uint8_t sfn_checksum(const uint8_t sfn[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + sfn[i]);
    return sum;
}

static int rd_sector(fat32_t *fs, uint32_t lba, uint8_t *buf) {
    return blockdev_read(fs->bdev, fs->part_lba + (uint64_t)lba * fs->bps512, fs->bps512, buf);
}

static int wr_sector(fat32_t *fs, uint32_t lba, const uint8_t *buf) {
    if (s_fat_cache_valid && s_fat_cache_lba == lba)
        memcpy(s_fat_cache, buf, fs->bytes_per_sector);
    return blockdev_write(fs->bdev, fs->part_lba + (uint64_t)lba * fs->bps512, fs->bps512, buf);
}

static int rd_fat(fat32_t *fs, uint32_t lba, uint8_t *buf) {
    if (s_fat_cache_valid && s_fat_cache_lba == lba) {
        memcpy(buf, s_fat_cache, fs->bytes_per_sector);
        return 0;
    }
    if (rd_sector(fs, lba, buf) != 0) return -1;
    memcpy(s_fat_cache, buf, fs->bytes_per_sector);
    s_fat_cache_lba = lba;
    s_fat_cache_valid = 1;
    return 0;
}

static uint32_t fat_raw_get(fat32_t *fs, uint32_t cluster) {
    if (cluster < 2) return 0;
    if (fs->is_fat32) {
        uint32_t off = cluster * 4u;
        uint32_t sec = fs->first_fat_sector + off / fs->bytes_per_sector;
        uint32_t idx = off % fs->bytes_per_sector;
        if (rd_fat(fs, sec, s_fatbuf) != 0) { fs->io_error = 1; return 0x0FFFFFFFu; }
        return rd32(s_fatbuf + idx) & 0x0FFFFFFFu;
    } else if (fs->is_fat12) {
        uint32_t off = cluster + cluster / 2u;
        uint32_t sec = fs->first_fat_sector + off / fs->bytes_per_sector;
        uint32_t idx = off % fs->bytes_per_sector;
        if (rd_fat(fs, sec, s_fatbuf) != 0) { fs->io_error = 1; return 0xFFFu; }
        uint32_t lo = s_fatbuf[idx];
        uint32_t hi;
        if (idx + 1 < fs->bytes_per_sector) hi = s_fatbuf[idx + 1];
        else {
            static uint8_t s_nb[4096];
            if (rd_sector(fs, sec + 1, s_nb) != 0) { fs->io_error = 1; return 0xFFFu; }
            hi = s_nb[0];
        }
        return (cluster & 1u) ? ((lo >> 4) | (hi << 4)) : (lo | ((hi & 0x0Fu) << 8));
    } else {
        uint32_t off = cluster * 2u;
        uint32_t sec = fs->first_fat_sector + off / fs->bytes_per_sector;
        uint32_t idx = off % fs->bytes_per_sector;
        if (rd_fat(fs, sec, s_fatbuf) != 0) { fs->io_error = 1; return 0xFFFFu; }
        return rd16(s_fatbuf + idx);
    }
}

static uint32_t fat_next(fat32_t *fs, uint32_t cluster) {
    uint32_t v = fat_raw_get(fs, cluster);
    if (fs->io_error) return fs->is_fat32 ? 0x0FFFFFFFu : (fs->is_fat12 ? 0xFFFFFFu : 0xFFFFFFFFu);
    if (fs->is_fat32) {
        if (v >= 0x0FFFFFF8u || v == 0) return 0x0FFFFFFFu;
        return v;
    } else if (fs->is_fat12) {
        if (v >= 0xFF8u || v == 0) return 0xFFFFFFu;
        return v;
    } else {
        if (v >= 0xFFF8u || v == 0) return 0xFFFFFFFFu;
        return v;
    }
}

static int fat_set(fat32_t *fs, uint32_t cluster, uint32_t value) {
    if (cluster < 2 || cluster >= fs->count_of_clusters + 2u) return -1;
    for (uint8_t f = 0; f < fs->num_fats; f++) {
        uint32_t fat_base = fs->first_fat_sector + (uint32_t)f * fs->fat_size;
        if (fs->is_fat32) {
            uint32_t off = cluster * 4u;
            uint32_t sec = fat_base + off / fs->bytes_per_sector;
            uint32_t idx = off % fs->bytes_per_sector;
            if (rd_sector(fs, sec, s_fatbuf) != 0) return -1;
            uint32_t old = rd32(s_fatbuf + idx);
            wr32(s_fatbuf + idx, (old & 0xF0000000u) | (value & 0x0FFFFFFFu));
            if (wr_sector(fs, sec, s_fatbuf) != 0) return -1;
        } else if (fs->is_fat12) {
            uint32_t off = cluster + cluster / 2u;
            uint32_t sec = fat_base + off / fs->bytes_per_sector;
            uint32_t idx = off % fs->bytes_per_sector;
            if (rd_sector(fs, sec, s_fatbuf) != 0) return -1;
            if (idx + 1 < fs->bytes_per_sector) {
                if (cluster & 1u) {
                    s_fatbuf[idx] = (uint8_t)((s_fatbuf[idx] & 0x0Fu) | ((value & 0x0Fu) << 4));
                    s_fatbuf[idx + 1] = (uint8_t)((value >> 4) & 0xFFu);
                } else {
                    s_fatbuf[idx] = (uint8_t)(value & 0xFFu);
                    s_fatbuf[idx + 1] = (uint8_t)((s_fatbuf[idx + 1] & 0xF0u) | ((value >> 8) & 0x0Fu));
                }
                if (wr_sector(fs, sec, s_fatbuf) != 0) return -1;
            } else {
                static uint8_t s_nb2[4096];
                if (rd_sector(fs, sec + 1, s_nb2) != 0) return -1;
                if (cluster & 1u) {
                    s_fatbuf[idx] = (uint8_t)((s_fatbuf[idx] & 0x0Fu) | ((value & 0x0Fu) << 4));
                    s_nb2[0] = (uint8_t)((value >> 4) & 0xFFu);
                } else {
                    s_fatbuf[idx] = (uint8_t)(value & 0xFFu);
                    s_nb2[0] = (uint8_t)((s_nb2[0] & 0xF0u) | ((value >> 8) & 0x0Fu));
                }
                if (wr_sector(fs, sec, s_fatbuf) != 0) return -1;
                if (wr_sector(fs, sec + 1, s_nb2) != 0) return -1;
            }
        } else {
            uint32_t off = cluster * 2u;
            uint32_t sec = fat_base + off / fs->bytes_per_sector;
            uint32_t idx = off % fs->bytes_per_sector;
            if (rd_sector(fs, sec, s_fatbuf) != 0) return -1;
            wr16(s_fatbuf + idx, (uint16_t)(value & 0xFFFFu));
            if (wr_sector(fs, sec, s_fatbuf) != 0) return -1;
        }
    }
    return 0;
}

static int fat_is_eoc(fat32_t *fs, uint32_t c) {
    if (fs->is_fat32) return c >= 0x0FFFFFF8u;
    if (fs->is_fat12) return c >= 0xFF8u || c == 0xFFFFFFu;
    return c >= 0xFFF8u;
}

static uint32_t fat_eoc_val(fat32_t *fs) {
    if (fs->is_fat32) return 0x0FFFFFFFu;
    if (fs->is_fat12) return 0xFFFu;
    return 0xFFFFu;
}

static int cluster_valid(fat32_t *fs, uint32_t c) {
    return c >= 2 && c < fs->count_of_clusters + 2u;
}

static uint32_t cluster_lba(fat32_t *fs, uint32_t cluster) {
    return fs->first_data_sector + (cluster - 2u) * fs->sectors_per_cluster;
}

static void fsinfo_update(fat32_t *fs, int delta_free) {
    if (!fs->is_fat32 || fs->fsinfo_sec == 0 || fs->fsinfo_sec >= fs->reserved_sectors)
        return;
    uint8_t buf[4096];
    if (rd_sector(fs, fs->fsinfo_sec, buf) != 0) return;
    if (rd32(buf) != 0x41615252u || rd32(buf + 484) != 0x61417272u || rd32(buf + 508) != 0xAA550000u)
        return;
    uint32_t fc = rd32(buf + 488);
    if (fc != 0xFFFFFFFFu) {
        if (delta_free < 0 && fc >= (uint32_t)(-delta_free)) fc -= (uint32_t)(-delta_free);
        else if (delta_free > 0) fc += (uint32_t)delta_free;
        wr32(buf + 488, fc);
    }
    wr32(buf + 492, fs->next_free_hint);
    wr_sector(fs, fs->fsinfo_sec, buf);
}

static int zero_cluster(fat32_t *fs, uint32_t cluster) {
    uint8_t z[4096];
    memset(z, 0, fs->bytes_per_sector);
    uint32_t lba = cluster_lba(fs, cluster);
    for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
        if (wr_sector(fs, lba + s, z) != 0) return -1;
    }
    return 0;
}

static uint32_t fat_alloc_cluster(fat32_t *fs) {
    uint32_t max_c = fs->count_of_clusters + 2u;
    uint32_t start = fs->next_free_hint;
    if (start < 2 || start >= max_c) start = 2;

    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t begin = (pass == 0) ? start : 2u;
        uint32_t end = (pass == 0) ? max_c : start;
        for (uint32_t c = begin; c < end; c++) {
            fs->io_error = 0;
            uint32_t v = fat_raw_get(fs, c);
            if (fs->io_error) return 0;
            if (v == 0) {
                if (fat_set(fs, c, fat_eoc_val(fs)) != 0) return 0;
                zero_cluster(fs, c);
                fs->next_free_hint = (c + 1 < max_c) ? (c + 1) : 2u;
                fsinfo_update(fs, -1);
                return c;
            }
        }
    }
    return 0;
}

static void fat_free_chain(fat32_t *fs, uint32_t start_cluster) {
    uint32_t c = start_cluster;
    uint32_t steps = 0;
    int freed = 0;
    while (cluster_valid(fs, c) && steps++ <= fs->count_of_clusters) {
        fs->io_error = 0;
        uint32_t nxt = fat_next(fs, c);
        if (fs->io_error) break;
        if (fat_set(fs, c, 0) != 0) break;
        freed++;
        if (fat_is_eoc(fs, nxt)) break;
        c = nxt;
    }
    if (freed > 0) fsinfo_update(fs, freed);
}

static void make_short(const uint8_t *de, char *out, int cap) {
    int p = 0;
    for (int i = 0; i < 8; i++) {
        char c = (char)de[i];
        if (c == ' ') break;
        if (i == 0 && (uint8_t)c == 0x05) c = (char)0xE5;
        if (p < cap - 1) out[p++] = c;
    }
    int has_ext = 0;
    for (int i = 8; i < 11; i++) if (de[i] != ' ') { has_ext = 1; break; }
    if (has_ext) {
        if (p < cap - 1) out[p++] = '.';
        for (int i = 8; i < 11; i++) {
            char c = (char)de[i];
            if (c == ' ') break;
            if (p < cap - 1) out[p++] = c;
        }
    }
    out[p] = 0;
}

static int dir_iter(fat32_t *fs, uint32_t start_cluster, int root16,
                    int target, fat_dirent_t *out) {
    uint16_t lfn_u16[261];
    memset(lfn_u16, 0, sizeof(lfn_u16));
    uint8_t lfn_csum = 0;
    int lfn_have = 0;
    int idx = 0;

    uint32_t cluster = start_cluster;
    uint32_t lba;
    uint32_t nsec;
    uint32_t steps = 0;
    uint32_t slow = start_cluster;
    int slow_flip = 0;

    if (root16) {
        lba = fs->first_root_dir_sector;
        nsec = fs->root_dir_sectors;
    } else {
        if (cluster < 2) return 1;
        if (!cluster_valid(fs, cluster)) return -1;
        lba = cluster_lba(fs, cluster);
        nsec = fs->sectors_per_cluster;
    }

    for (;;) {
        for (uint32_t s = 0; s < nsec; s++) {
            if (rd_sector(fs, lba + s, s_sec) != 0) return -1;
            for (int e = 0; e + 32 <= fs->bytes_per_sector; e += 32) {
                uint8_t *de = s_sec + e;
                uint8_t first = de[0];
                if (first == 0x00) return 1;
                if (first == 0xE5) { lfn_have = 0; continue; }
                uint8_t attr = de[11];
                if ((attr & 0x3F) == 0x0F) {
                    int ord = de[0] & 0x1F;
                    if (de[0] & 0x40) {
                        memset(lfn_u16, 0, sizeof(lfn_u16));
                        lfn_csum = de[13];
                        lfn_have = 1;
                    } else if (!lfn_have || de[13] != lfn_csum) {
                        lfn_have = 0;
                        continue;
                    }
                    if (ord >= 1 && ord <= 20) {
                        uint16_t *pp = lfn_u16 + (ord - 1) * 13;
                        static const int o[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
                        for (int k = 0; k < 13; k++) {
                            uint16_t ch = (uint16_t)(de[o[k]] | (de[o[k] + 1] << 8));
                            pp[k] = (ch == 0xFFFF) ? 0 : ch;
                        }
                    }
                    continue;
                }
                if (attr & 0x08) { lfn_have = 0; continue; }

                char sname[16];
                make_short(de, sname, sizeof(sname));
                int skip = (sname[0] == '.' &&
                            (sname[1] == 0 || (sname[1] == '.' && sname[2] == 0)));
                if (!skip) {
                    if (idx == target) {
                        if (lfn_have && lfn_csum == sfn_checksum(de) && lfn_u16[0] != 0) {
                            utf16_to_utf8(lfn_u16, 260, out->name, sizeof(out->name));
                        } else {
                            str_copy_n(out->name, sname, sizeof(out->name));
                        }
                        out->is_dir = (attr & 0x10) ? 1 : 0;
                        out->first_cluster = (uint32_t)(de[26] | (de[27] << 8)) |
                                             ((uint32_t)(de[20] | (de[21] << 8)) << 16);
                        out->size = rd32(de + 28);
                        return 0;
                    }
                    idx++;
                }
                lfn_have = 0;
            }
        }
        if (root16) return 1;
        if (fs->io_error) return -1;
        uint32_t nextc = fat_next(fs, cluster);
        if (fs->io_error) return -1;
        if (fat_is_eoc(fs, nextc)) return 1;
        if (!cluster_valid(fs, nextc)) return -1;
        if (++steps > fs->count_of_clusters + 1) return -1;
        if ((slow_flip ^= 1)) {
            slow = fat_next(fs, slow);
            if (slow == nextc) return -1;
        }
        cluster = nextc;
        lba = cluster_lba(fs, cluster);
        nsec = fs->sectors_per_cluster;
    }
}

static int resolve(fat32_t *fs, const char *path, fat_dirent_t *out) {
    uint32_t cluster = fs->is_fat32 ? fs->root_cluster : 0;
    int root16 = fs->is_fat32 ? 0 : 1;

    const char *p = path;
    while (*p == '/') p++;
    if (*p == 0) {
        out->is_dir = 1;
        out->size = 0;
        out->first_cluster = cluster;
        str_copy_n(out->name, "/", sizeof(out->name));
        return 0;
    }

    char comp[260];
    while (*p) {
        int n = 0;
        while (*p && *p != '/') { if (n < 255) comp[n++] = *p; p++; }
        comp[n] = 0;
        while (*p == '/') p++;

        fat_dirent_t de;
        int found = 0;
        for (int i = 0; ; i++) {
            int r = dir_iter(fs, cluster, root16, i, &de);
            if (r != 0) break;
            if (str_eq_ci(de.name, comp)) { found = 1; break; }
        }
        if (!found) return -1;
        if (*p == 0) { *out = de; return 0; }
        if (!de.is_dir) return -1;
        cluster = de.first_cluster;
        root16 = 0;
    }
    return -1;
}

/* ---- Partition probing (VBR at LBA 0, GPT at LBA 1, or MBR at LBA 0) ---- */
static int is_valid_fat_bpb(const uint8_t *bs) {
    if (bs[510] != 0x55 || bs[511] != 0xAA) return 0;
    if (!((bs[0] == 0xEB && bs[2] == 0x90) || bs[0] == 0xE9)) return 0;
    uint16_t bps = rd16(bs + 11);
    uint8_t spc = bs[13];
    uint16_t reserved = rd16(bs + 14);
    uint8_t nfats = bs[16];
    uint16_t fat16sz = rd16(bs + 22);
    uint32_t total = rd16(bs + 19) ? rd16(bs + 19) : rd32(bs + 32);
    uint32_t fat_sz = fat16sz ? fat16sz : rd32(bs + 36);
    if (bps < 512 || bps > 4096 || (bps & (bps - 1))) return 0;
    if (spc == 0 || (spc & (spc - 1))) return 0;
    if (reserved == 0 || nfats == 0 || nfats > 4 || fat_sz == 0 || total == 0) return 0;
    /* Reject exFAT ("EXFAT   ") */
    if (memcmp(bs + 3, "EXFAT   ", 8) == 0) return 0;
    return 1;
}

static int try_find_fat_partition(int bdev, uint64_t *part_lba_out, uint8_t bs_out[512]) {
    uint8_t lba0[512];
    if (blockdev_read(bdev, 0, 1, lba0) != 0) return -1;

    if (is_valid_fat_bpb(lba0)) {
        *part_lba_out = 0;
        memcpy(bs_out, lba0, 512);
        return 0;
    }

    if (lba0[510] != 0x55 || lba0[511] != 0xAA) return -2;

    /* 1) Check GPT header at LBA 1 */
    uint8_t gpt[512];
    if (blockdev_read(bdev, 1, 1, gpt) == 0 && memcmp(gpt, "EFI PART", 8) == 0) {
        uint64_t ent_lba = rd64(gpt + 72);
        uint32_t num_ent = rd32(gpt + 80);
        uint32_t ent_sz  = rd32(gpt + 84);
        if (num_ent > 128) num_ent = 128;
        if (ent_sz >= 128 && ent_sz <= 512) {
            uint32_t per_sec = 512u / ent_sz;
            uint32_t nsec = (num_ent + per_sec - 1u) / per_sec;
            for (uint32_t s = 0; s < nsec; s++) {
                uint8_t esec[512];
                if (blockdev_read(bdev, ent_lba + s, 1, esec) != 0) break;
                for (uint32_t k = 0; k < per_sec; k++) {
                    const uint8_t *e = esec + k * ent_sz;
                    int zero = 1;
                    for (int g = 0; g < 16; g++) if (e[g] != 0) { zero = 0; break; }
                    if (zero) continue;
                    uint64_t slba = rd64(e + 32);
                    uint64_t elba = rd64(e + 40);
                    if (slba > 0 && elba >= slba) {
                        uint8_t pbs[512];
                        if (blockdev_read(bdev, slba, 1, pbs) == 0 && is_valid_fat_bpb(pbs)) {
                            *part_lba_out = slba;
                            memcpy(bs_out, pbs, 512);
                            return 0;
                        }
                    }
                }
            }
        }
    }

    /* 2) Check MBR 4 primary partitions */
    for (int i = 0; i < 4; i++) {
        const uint8_t *pe = lba0 + 0x1BE + i * 16;
        uint8_t ptype = pe[4];
        uint32_t slba = rd32(pe + 8);
        uint32_t secs = rd32(pe + 12);
        if (ptype == 0 || slba == 0 || secs == 0) continue;

        if (ptype == 0x05 || ptype == 0x0F) {
            /* Extended partition EBR chain */
            uint32_t ext_base = slba;
            uint32_t cur_ebr = ext_base;
            for (int link = 0; link < 8 && cur_ebr != 0; link++) {
                uint8_t ebr[512];
                if (blockdev_read(bdev, cur_ebr, 1, ebr) != 0) break;
                if (ebr[510] != 0x55 || ebr[511] != 0xAA) break;
                const uint8_t *e0 = ebr + 0x1BE;
                if (e0[4] != 0 && rd32(e0 + 8) != 0) {
                    uint64_t plba = (uint64_t)cur_ebr + rd32(e0 + 8);
                    uint8_t pbs[512];
                    if (blockdev_read(bdev, plba, 1, pbs) == 0 && is_valid_fat_bpb(pbs)) {
                        *part_lba_out = plba;
                        memcpy(bs_out, pbs, 512);
                        return 0;
                    }
                }
                const uint8_t *e1 = ebr + 0x1CE;
                if (e1[4] != 0 && rd32(e1 + 8) != 0)
                    cur_ebr = ext_base + rd32(e1 + 8);
                else
                    break;
            }
            continue;
        }

        if (ptype == 0x01 || ptype == 0x04 || ptype == 0x06 ||
            ptype == 0x0B || ptype == 0x0C || ptype == 0x0E ||
            ptype == 0x07 || ptype == 0xEF) {
            uint8_t pbs[512];
            if (blockdev_read(bdev, slba, 1, pbs) == 0 && is_valid_fat_bpb(pbs)) {
                *part_lba_out = slba;
                memcpy(bs_out, pbs, 512);
                return 0;
            }
        }
    }

    return -3;
}

int fat32_mount(int bdev) {
    memset(&s_fs, 0, sizeof(s_fs));
    s_fat_cache_valid = 0;

    uint64_t part_lba = 0;
    uint8_t bs[512];
    if (try_find_fat_partition(bdev, &part_lba, bs) != 0)
        return -1;

    uint16_t bps = rd16(bs + 11);
    uint8_t spc = bs[13];
    uint16_t reserved = rd16(bs + 14);
    uint8_t nfats = bs[16];
    uint16_t root_entries = rd16(bs + 17);
    uint16_t fat16sz = rd16(bs + 22);
    uint32_t total16 = rd16(bs + 19);
    uint32_t total32 = rd32(bs + 32);
    uint32_t total = total16 ? total16 : total32;
    uint32_t fat_sz = fat16sz ? fat16sz : rd32(bs + 36);
    uint32_t root_cluster = fat16sz ? 0 : rd32(bs + 44);

    uint32_t root_dir_sectors = ((uint32_t)root_entries * 32u + (bps - 1u)) / bps;
    uint32_t first_fat = reserved;
    uint32_t first_root = reserved + (uint32_t)nfats * fat_sz;
    uint32_t first_data = first_root + root_dir_sectors;
    if (total <= first_data) return -5;

    s_fs.bdev = bdev;
    s_fs.part_lba = part_lba;
    s_fs.is_fat32 = (fat16sz == 0);
    s_fs.io_error = 0;
    s_fs.bytes_per_sector = bps;
    s_fs.sectors_per_cluster = spc;
    s_fs.reserved_sectors = reserved;
    s_fs.num_fats = nfats;
    s_fs.fat_size = fat_sz;
    s_fs.root_dir_sectors = root_dir_sectors;
    s_fs.first_fat_sector = first_fat;
    s_fs.first_root_dir_sector = first_root;
    s_fs.first_data_sector = first_data;
    s_fs.count_of_clusters = (total - first_data) / spc;
    s_fs.is_fat12 = (!s_fs.is_fat32 && s_fs.count_of_clusters < 4085u) ? 1 : 0;
    s_fs.root_cluster = s_fs.is_fat32 ? (root_cluster ? root_cluster : 2u) : 0;
    s_fs.total_sectors = total;
    s_fs.bps512 = bps / 512u;
    s_fs.fsinfo_sec = s_fs.is_fat32 ? rd16(bs + 48) : 0;
    s_fs.next_free_hint = 2;

    if (s_fs.is_fat32 && s_fs.fsinfo_sec > 0 && s_fs.fsinfo_sec < reserved) {
        uint8_t fsibuf[4096];
        if (rd_sector(&s_fs, s_fs.fsinfo_sec, fsibuf) == 0 &&
            rd32(fsibuf) == 0x41615252u && rd32(fsibuf + 484) == 0x61417272u) {
            uint32_t nh = rd32(fsibuf + 492);
            if (nh >= 2 && nh < s_fs.count_of_clusters + 2u)
                s_fs.next_free_hint = nh;
        }
    }

    int lbl_off = s_fs.is_fat32 ? 71 : 43;
    for (int i = 0; i < 11; i++) s_fs.label[i] = (char)bs[lbl_off + i];
    s_fs.label[11] = 0;

    s_fs.mounted = 1;
    return 0;
}

void fat32_unmount(void) {
    memset(&s_fs, 0, sizeof(s_fs));
    s_fat_cache_valid = 0;
}

int fat32_mounted(void) { return s_fs.mounted; }
int fat32_bdev(void) { return s_fs.mounted ? s_fs.bdev : -1; }

int fat_count_dir(const char *path) {
    if (!s_fs.mounted) return 0;
    fat_dirent_t de;
    if (resolve(&s_fs, path, &de) != 0 || !de.is_dir) return 0;
    uint32_t cluster = de.first_cluster;
    int root16 = 0;
    if (!s_fs.is_fat32 && path[0] == '/' && path[1] == 0) root16 = 1;
    else if (path[0] == '/' && path[1] == 0) cluster = s_fs.root_cluster;
    int n = 0;
    fat_dirent_t tmp;
    while (dir_iter(&s_fs, cluster, root16, n, &tmp) == 0) n++;
    return n;
}

int fat_list_dir(const char *path, int idx, char *out, int cap, int *is_dir) {
    if (!s_fs.mounted) return -1;
    fat_dirent_t de;
    if (resolve(&s_fs, path, &de) != 0 || !de.is_dir) return -1;
    uint32_t cluster = de.first_cluster;
    int root16 = 0;
    if (!s_fs.is_fat32 && path[0] == '/' && path[1] == 0) root16 = 1;
    else if (path[0] == '/' && path[1] == 0) cluster = s_fs.root_cluster;

    int r = dir_iter(&s_fs, cluster, root16, idx, &de);
    if (r != 0) return r;
    str_copy_n(out, de.name, cap);
    if (is_dir) *is_dir = de.is_dir;
    return 0;
}

uint32_t fat_read_path(const char *path, void *buf, uint32_t size) {
    if (!s_fs.mounted || !buf) return 0;
    fat_dirent_t de;
    if (resolve(&s_fs, path, &de) != 0 || de.is_dir) return 0;

    uint32_t toread = de.size < size ? de.size : size;
    uint32_t done = 0;
    uint32_t cluster = de.first_cluster;
    uint8_t *out = (uint8_t *)buf;

    while (done < toread && cluster >= 2) {
        if (!cluster_valid(&s_fs, cluster)) break;
        uint32_t lba = cluster_lba(&s_fs, cluster);
        for (uint32_t s = 0; s < s_fs.sectors_per_cluster && done < toread; s++) {
            if (rd_sector(&s_fs, lba + s, s_sec) != 0) return done;
            uint32_t chunk = s_fs.bytes_per_sector;
            if (chunk > toread - done) chunk = toread - done;
            memcpy(out + done, s_sec, chunk);
            done += chunk;
        }
        cluster = fat_next(&s_fs, cluster);
        if (s_fs.io_error) break;
        if (fat_is_eoc(&s_fs, cluster)) break;
    }
    return done;
}

int fat_stat(const char *path, uint32_t *size, int *is_dir) {
    if (!s_fs.mounted) return -1;
    fat_dirent_t de;
    if (resolve(&s_fs, path, &de) != 0) return -1;
    if (size) *size = de.size;
    if (is_dir) *is_dir = de.is_dir;
    return 0;
}

/* ---- Write support helpers ---- */
typedef struct {
    uint32_t sfn_lba;
    int      sfn_off;
    uint32_t first_cluster;
    uint32_t size;
    uint8_t  attr;
    /* Track LFN slots preceding SFN (up to 20) */
    int      lfn_cnt;
    uint32_t lfn_lba[20];
    int      lfn_off[20];
} dir_loc_t;

static int dir_find_loc(fat32_t *fs, uint32_t start_cluster, int root16,
                        const char *target_name, dir_loc_t *loc) {
    uint16_t lfn_u16[261];
    memset(lfn_u16, 0, sizeof(lfn_u16));
    uint8_t lfn_csum = 0;
    int lfn_have = 0;
    int lfn_cnt = 0;
    uint32_t lfn_lba[20];
    int lfn_off[20];

    uint32_t cluster = start_cluster;
    uint32_t lba = root16 ? fs->first_root_dir_sector : cluster_lba(fs, cluster);
    uint32_t nsec = root16 ? fs->root_dir_sectors : fs->sectors_per_cluster;
    uint32_t steps = 0;

    if (!root16 && !cluster_valid(fs, cluster)) return -1;

    for (;;) {
        for (uint32_t s = 0; s < nsec; s++) {
            if (rd_sector(fs, lba + s, s_sec) != 0) return -1;
            for (int e = 0; e + 32 <= fs->bytes_per_sector; e += 32) {
                uint8_t *de = s_sec + e;
                uint8_t first = de[0];
                if (first == 0x00) return -1;
                if (first == 0xE5) { lfn_have = 0; lfn_cnt = 0; continue; }
                uint8_t attr = de[11];
                if ((attr & 0x3F) == 0x0F) {
                    int ord = de[0] & 0x1F;
                    if (de[0] & 0x40) {
                        memset(lfn_u16, 0, sizeof(lfn_u16));
                        lfn_csum = de[13];
                        lfn_have = 1;
                        lfn_cnt = 0;
                    } else if (!lfn_have || de[13] != lfn_csum) {
                        lfn_have = 0; lfn_cnt = 0; continue;
                    }
                    if (lfn_cnt < 20) {
                        lfn_lba[lfn_cnt] = lba + s;
                        lfn_off[lfn_cnt] = e;
                        lfn_cnt++;
                    }
                    if (ord >= 1 && ord <= 20) {
                        uint16_t *pp = lfn_u16 + (ord - 1) * 13;
                        static const int o[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
                        for (int k = 0; k < 13; k++) {
                            uint16_t ch = rd16(de + o[k]);
                            pp[k] = (ch == 0xFFFF) ? 0 : ch;
                        }
                    }
                    continue;
                }
                if (attr & 0x08) { lfn_have = 0; lfn_cnt = 0; continue; }

                char name[256];
                if (lfn_have && lfn_csum == sfn_checksum(de) && lfn_u16[0] != 0) {
                    utf16_to_utf8(lfn_u16, 260, name, sizeof(name));
                } else {
                    make_short(de, name, sizeof(name));
                    lfn_cnt = 0;
                }
                if (str_eq_ci(name, target_name)) {
                    loc->sfn_lba = lba + s;
                    loc->sfn_off = e;
                    loc->first_cluster = (uint32_t)rd16(de + 26) | ((uint32_t)rd16(de + 20) << 16);
                    loc->size = rd32(de + 28);
                    loc->attr = attr;
                    loc->lfn_cnt = lfn_cnt;
                    for (int i = 0; i < lfn_cnt; i++) {
                        loc->lfn_lba[i] = lfn_lba[i];
                        loc->lfn_off[i] = lfn_off[i];
                    }
                    return 0;
                }
                lfn_have = 0;
                lfn_cnt = 0;
            }
        }
        if (root16) return -1;
        uint32_t nextc = fat_next(fs, cluster);
        if (fs->io_error || fat_is_eoc(fs, nextc) || !cluster_valid(fs, nextc)) return -1;
        if (++steps > fs->count_of_clusters + 1) return -1;
        cluster = nextc;
        lba = cluster_lba(fs, cluster);
        nsec = fs->sectors_per_cluster;
    }
}

static int sfn_char_valid(char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return 1;
    switch (c) {
        case '$': case '%': case '\'': case '-': case '_': case '@':
        case '~': case '`': case '!': case '(': case ')': case '{':
        case '}': case '^': case '#': case '&': return 1;
        default: return 0;
    }
}

static int dir_sfn_exists(fat32_t *fs, uint32_t dir_cluster, int root16, const uint8_t sfn[11]) {
    uint32_t cluster = dir_cluster;
    uint32_t lba = root16 ? fs->first_root_dir_sector : cluster_lba(fs, cluster);
    uint32_t nsec = root16 ? fs->root_dir_sectors : fs->sectors_per_cluster;
    uint32_t steps = 0;
    for (;;) {
        for (uint32_t s = 0; s < nsec; s++) {
            if (rd_sector(fs, lba + s, s_sec) != 0) return 0;
            for (int e = 0; e + 32 <= fs->bytes_per_sector; e += 32) {
                uint8_t *de = s_sec + e;
                if (de[0] == 0x00) return 0;
                if (de[0] == 0xE5 || (de[11] & 0x3F) == 0x0F || (de[11] & 0x08)) continue;
                if (memcmp(de, sfn, 11) == 0) return 1;
            }
        }
        if (root16) return 0;
        uint32_t nextc = fat_next(fs, cluster);
        if (fs->io_error || fat_is_eoc(fs, nextc) || !cluster_valid(fs, nextc)) return 0;
        if (++steps > fs->count_of_clusters + 1) return 0;
        cluster = nextc;
        lba = cluster_lba(fs, cluster);
        nsec = fs->sectors_per_cluster;
    }
}

static void build_sfn(fat32_t *fs, uint32_t dir_cluster, int root16,
                      const char *name, uint8_t sfn[11], int *needs_lfn) {
    memset(sfn, ' ', 11);

    /* Check if exact uppercase 8.3 */
    const char *dot = 0;
    int len = 0;
    int exact = 1;
    for (int i = 0; name[i]; i++) {
        unsigned char c = (unsigned char)name[i];
        len++;
        if (c == '.') {
            if (dot) exact = 0;
            else dot = name + i;
        } else if (c >= 0x80 || (c >= 'a' && c <= 'z') || !sfn_char_valid((char)c)) {
            exact = 0;
        }
    }
    int base_len = dot ? (int)(dot - name) : len;
    int ext_len = dot ? (len - base_len - 1) : 0;
    if (exact && base_len >= 1 && base_len <= 8 && ext_len <= 3) {
        for (int i = 0; i < base_len; i++) sfn[i] = (uint8_t)name[i];
        if (dot) for (int i = 0; i < ext_len; i++) sfn[8 + i] = (uint8_t)dot[1 + i];
        *needs_lfn = 0;
        return;
    }

    *needs_lfn = 1;
    char base[8];
    int bi = 0;
    for (int i = 0; i < (dot ? base_len : len) && bi < 6; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c < 0x80 && sfn_char_valid((char)c)) base[bi++] = (char)c;
    }
    if (bi == 0) { base[0] = 'F'; base[1] = 'I'; base[2] = 'L'; base[3] = 'E'; bi = 4; }

    char ext[3];
    int ei = 0;
    if (dot) {
        for (int i = 1; dot[i] && ei < 3; i++) {
            unsigned char c = (unsigned char)dot[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            if (c < 0x80 && sfn_char_valid((char)c)) ext[ei++] = (char)c;
        }
    }

    for (int num = 1; num < 9999; num++) {
        memset(sfn, ' ', 11);
        int keep = bi;
        char suf[8];
        int sn = 0;
        int tmp = num;
        char rev[6]; int rn = 0;
        while (tmp > 0 && rn < 5) { rev[rn++] = (char)('0' + (tmp % 10)); tmp /= 10; }
        suf[sn++] = '~';
        while (rn > 0) suf[sn++] = rev[--rn];
        if (keep + sn > 8) keep = 8 - sn;
        if (keep < 1) keep = 1;
        for (int i = 0; i < keep; i++) sfn[i] = (uint8_t)base[i];
        for (int i = 0; i < sn && keep + i < 8; i++) sfn[keep + i] = (uint8_t)suf[i];
        for (int i = 0; i < ei; i++) sfn[8 + i] = (uint8_t)ext[i];
        if (!dir_sfn_exists(fs, dir_cluster, root16, sfn)) return;
    }
}

typedef struct {
    uint32_t lba;
    int      off;
} slot_pos_t;

static int dir_add_entry(fat32_t *fs, uint32_t dir_cluster, int root16,
                         const char *name, uint8_t attr, uint32_t first_cluster, uint32_t size) {
    uint8_t sfn[11];
    int needs_lfn = 0;
    build_sfn(fs, dir_cluster, root16, name, sfn, &needs_lfn);

    uint16_t u16[256];
    int u16_len = 0;
    int n_lfn = 0;
    if (needs_lfn) {
        u16_len = utf8_to_utf16(name, u16, 256);
        n_lfn = (u16_len + 12) / 13;
        if (n_lfn > 20) n_lfn = 20;
    }
    int need = n_lfn + 1;

    slot_pos_t slots[21];
    int have = 0;

    uint32_t cluster = dir_cluster;
    uint32_t prev_cluster = 0;
    uint32_t lba = root16 ? fs->first_root_dir_sector : cluster_lba(fs, cluster);
    uint32_t nsec = root16 ? fs->root_dir_sectors : fs->sectors_per_cluster;
    uint32_t steps = 0;
    int hit_eod = 0;

    for (;;) {
        for (uint32_t s = 0; s < nsec; s++) {
            if (rd_sector(fs, lba + s, s_sec) != 0) return -1;
            for (int e = 0; e + 32 <= fs->bytes_per_sector; e += 32) {
                uint8_t f = s_sec[e];
                if (f == 0x00 || f == 0xE5) {
                    slots[have].lba = lba + s;
                    slots[have].off = e;
                    have++;
                    if (f == 0x00) hit_eod = 1;
                    if (have == need) goto found_slots;
                } else {
                    have = 0;
                }
            }
        }
        if (root16) return -1;
        prev_cluster = cluster;
        uint32_t nextc = fat_next(fs, cluster);
        if (fs->io_error) return -1;
        if (fat_is_eoc(fs, nextc)) {
            uint32_t nc = fat_alloc_cluster(fs);
            if (!nc) return -1;
            if (fat_set(fs, prev_cluster, nc) != 0) return -1;
            cluster = nc;
            lba = cluster_lba(fs, cluster);
            nsec = fs->sectors_per_cluster;
        } else {
            if (!cluster_valid(fs, nextc) || ++steps > fs->count_of_clusters + 1) return -1;
            cluster = nextc;
            lba = cluster_lba(fs, cluster);
            nsec = fs->sectors_per_cluster;
        }
    }

found_slots:
    (void)hit_eod;
    uint8_t csum = sfn_checksum(sfn);
    static const int o[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};

    /* Write LFN entries from highest ordinal down to 1 */
    for (int i = 0; i < n_lfn; i++) {
        int ord = n_lfn - i;
        uint8_t ent[32];
        memset(ent, 0xFF, sizeof(ent));
        ent[0] = (uint8_t)(ord | (i == 0 ? 0x40 : 0));
        ent[11] = 0x0F;
        ent[12] = 0;
        ent[13] = csum;
        ent[26] = 0; ent[27] = 0;
        int base = (ord - 1) * 13;
        for (int k = 0; k < 13; k++) {
            int pos = base + k;
            uint16_t ch = 0xFFFF;
            if (pos < u16_len) ch = u16[pos];
            else if (pos == u16_len) ch = 0x0000;
            wr16(ent + o[k], ch);
        }
        if (rd_sector(fs, slots[i].lba, s_sec) != 0) return -1;
        memcpy(s_sec + slots[i].off, ent, 32);
        if (wr_sector(fs, slots[i].lba, s_sec) != 0) return -1;
    }

    /* Write SFN entry */
    uint8_t sfne[32];
    memset(sfne, 0, sizeof(sfne));
    memcpy(sfne, sfn, 11);
    sfne[11] = attr;
    wr16(sfne + 20, (uint16_t)(first_cluster >> 16));
    wr16(sfne + 26, (uint16_t)(first_cluster & 0xFFFFu));
    wr32(sfne + 28, size);

    if (rd_sector(fs, slots[n_lfn].lba, s_sec) != 0) return -1;
    memcpy(s_sec + slots[n_lfn].off, sfne, 32);
    if (wr_sector(fs, slots[n_lfn].lba, s_sec) != 0) return -1;
    return 0;
}

static int split_parent_child(fat32_t *fs, const char *path,
                              uint32_t *parent_cluster_out, int *parent_root16_out,
                              char *child_out, int child_cap) {
    if (!path) return -1;
    const char *p = path;
    while (*p == '/') p++;
    if (!*p) return -1;

    /* Find last '/' */
    int last_slash = -1;
    int len = 0;
    while (p[len]) {
        if (p[len] == '/') last_slash = len;
        len++;
    }
    /* Strip trailing slashes if any */
    while (len > 0 && p[len - 1] == '/') len--;
    if (len == 0) return -1;

    last_slash = -1;
    for (int i = 0; i < len; i++) if (p[i] == '/') last_slash = i;

    char parent_path[260];
    if (last_slash < 0) {
        parent_path[0] = '/'; parent_path[1] = 0;
        int cn = len < child_cap - 1 ? len : child_cap - 1;
        memcpy(child_out, p, (size_t)cn);
        child_out[cn] = 0;
    } else {
        int pn = last_slash < 258 ? last_slash : 258;
        parent_path[0] = '/';
        memcpy(parent_path + 1, p, (size_t)pn);
        parent_path[1 + pn] = 0;
        int rem = len - last_slash - 1;
        int cn = rem < child_cap - 1 ? rem : child_cap - 1;
        memcpy(child_out, p + last_slash + 1, (size_t)cn);
        child_out[cn] = 0;
    }
    if (!child_out[0]) return -1;

    fat_dirent_t pde;
    if (resolve(fs, parent_path, &pde) != 0 || !pde.is_dir) return -1;
    if (parent_path[0] == '/' && parent_path[1] == 0) {
        *parent_root16_out = fs->is_fat32 ? 0 : 1;
        *parent_cluster_out = fs->is_fat32 ? fs->root_cluster : 0;
    } else {
        *parent_root16_out = 0;
        *parent_cluster_out = pde.first_cluster;
    }
    return 0;
}

static int write_data_chain(fat32_t *fs, const void *data, uint32_t size, uint32_t *first_c_out) {
    *first_c_out = 0;
    if (size == 0 || !data) return 0;

    const uint8_t *src = (const uint8_t *)data;
    uint32_t done = 0;
    uint32_t first_c = 0;
    uint32_t prev_c = 0;

    while (done < size) {
        uint32_t c = fat_alloc_cluster(fs);
        if (!c) {
            if (first_c) fat_free_chain(fs, first_c);
            return -1;
        }
        if (!first_c) first_c = c;
        else if (fat_set(fs, prev_c, c) != 0) {
            fat_free_chain(fs, first_c);
            return -1;
        }
        prev_c = c;

        uint32_t lba = cluster_lba(fs, c);
        for (uint32_t s = 0; s < fs->sectors_per_cluster && done < size; s++) {
            uint32_t chunk = fs->bytes_per_sector;
            if (chunk > size - done) chunk = size - done;
            memset(s_sec, 0, fs->bytes_per_sector);
            memcpy(s_sec, src + done, chunk);
            if (wr_sector(fs, lba + s, s_sec) != 0) {
                fat_free_chain(fs, first_c);
                return -1;
            }
            done += chunk;
        }
    }

    *first_c_out = first_c;
    return 0;
}

int fat32_create_file(const char *path) {
    if (!s_fs.mounted) return -1;
    uint32_t pcluster = 0;
    int proot16 = 0;
    char name[256];
    if (split_parent_child(&s_fs, path, &pcluster, &proot16, name, sizeof(name)) != 0)
        return -1;

    dir_loc_t loc;
    if (dir_find_loc(&s_fs, pcluster, proot16, name, &loc) == 0) {
        if (loc.attr & 0x10) return -1;
        return 0; /* Already exists */
    }
    if (dir_add_entry(&s_fs, pcluster, proot16, name, 0x20, 0, 0) != 0)
        return -1;
    blockdev_flush(s_fs.bdev);
    return 0;
}

int fat32_write_file(const char *path, const void *data, uint32_t size) {
    if (!s_fs.mounted) return -1;
    uint32_t pcluster = 0;
    int proot16 = 0;
    char name[256];
    if (split_parent_child(&s_fs, path, &pcluster, &proot16, name, sizeof(name)) != 0)
        return -1;

    uint32_t new_first = 0;
    if (write_data_chain(&s_fs, data, size, &new_first) != 0)
        return -1;

    dir_loc_t loc;
    if (dir_find_loc(&s_fs, pcluster, proot16, name, &loc) == 0) {
        if (loc.attr & 0x10) {
            if (new_first) fat_free_chain(&s_fs, new_first);
            return -1;
        }
        if (loc.first_cluster >= 2)
            fat_free_chain(&s_fs, loc.first_cluster);
        if (rd_sector(&s_fs, loc.sfn_lba, s_sec) != 0) return -1;
        uint8_t *de = s_sec + loc.sfn_off;
        wr16(de + 20, (uint16_t)(new_first >> 16));
        wr16(de + 26, (uint16_t)(new_first & 0xFFFFu));
        wr32(de + 28, size);
        if (wr_sector(&s_fs, loc.sfn_lba, s_sec) != 0) return -1;
    } else {
        if (dir_add_entry(&s_fs, pcluster, proot16, name, 0x20, new_first, size) != 0) {
            if (new_first) fat_free_chain(&s_fs, new_first);
            return -1;
        }
    }
    blockdev_flush(s_fs.bdev);
    return 0;
}

int fat32_delete_file(const char *path) {
    if (!s_fs.mounted) return -1;
    uint32_t pcluster = 0;
    int proot16 = 0;
    char name[256];
    if (split_parent_child(&s_fs, path, &pcluster, &proot16, name, sizeof(name)) != 0)
        return -1;

    dir_loc_t loc;
    if (dir_find_loc(&s_fs, pcluster, proot16, name, &loc) != 0)
        return -1;

    if (loc.attr & 0x10) {
        /* Refuse deleting non-empty directory */
        fat_dirent_t tmp;
        if (dir_iter(&s_fs, loc.first_cluster, 0, 0, &tmp) == 0)
            return -2;
    }

    if (loc.first_cluster >= 2)
        fat_free_chain(&s_fs, loc.first_cluster);

    for (int i = 0; i < loc.lfn_cnt; i++) {
        if (rd_sector(&s_fs, loc.lfn_lba[i], s_sec) == 0) {
            s_sec[loc.lfn_off[i]] = 0xE5;
            wr_sector(&s_fs, loc.lfn_lba[i], s_sec);
        }
    }
    if (rd_sector(&s_fs, loc.sfn_lba, s_sec) != 0) return -1;
    s_sec[loc.sfn_off] = 0xE5;
    if (wr_sector(&s_fs, loc.sfn_lba, s_sec) != 0) return -1;

    blockdev_flush(s_fs.bdev);
    return 0;
}

int fat32_mkdir(const char *path) {
    if (!s_fs.mounted || !path) return -1;
    const char *p = path;
    while (*p == '/') p++;
    if (!*p) return 0; /* Root always exists */

    uint32_t cur_cluster = s_fs.is_fat32 ? s_fs.root_cluster : 0;
    int cur_root16 = s_fs.is_fat32 ? 0 : 1;

    while (*p) {
        char comp[256];
        int cn = 0;
        while (*p && *p != '/') {
            if (cn < 255) comp[cn++] = *p;
            p++;
        }
        comp[cn] = 0;
        while (*p == '/') p++;

        dir_loc_t loc;
        if (dir_find_loc(&s_fs, cur_cluster, cur_root16, comp, &loc) == 0) {
            if (!(loc.attr & 0x10)) return -1;
            cur_cluster = loc.first_cluster;
            cur_root16 = 0;
        } else {
            uint32_t newc = fat_alloc_cluster(&s_fs);
            if (!newc) return -1;

            /* Initialize '.' and '..' entries in new directory cluster */
            memset(s_sec, 0, s_fs.bytes_per_sector);
            memset(s_sec, ' ', 11);
            s_sec[0] = '.';
            s_sec[11] = 0x10;
            wr16(s_sec + 20, (uint16_t)(newc >> 16));
            wr16(s_sec + 26, (uint16_t)(newc & 0xFFFFu));

            uint32_t dotdot = (cur_root16 || (s_fs.is_fat32 && cur_cluster == s_fs.root_cluster))
                              ? 0u : cur_cluster;
            memset(s_sec + 32, ' ', 11);
            s_sec[32] = '.'; s_sec[33] = '.';
            s_sec[32 + 11] = 0x10;
            wr16(s_sec + 32 + 20, (uint16_t)(dotdot >> 16));
            wr16(s_sec + 32 + 26, (uint16_t)(dotdot & 0xFFFFu));

            if (wr_sector(&s_fs, cluster_lba(&s_fs, newc), s_sec) != 0) {
                fat_free_chain(&s_fs, newc);
                return -1;
            }
            if (dir_add_entry(&s_fs, cur_cluster, cur_root16, comp, 0x10, newc, 0) != 0) {
                fat_free_chain(&s_fs, newc);
                return -1;
            }
            cur_cluster = newc;
            cur_root16 = 0;
        }
    }

    blockdev_flush(s_fs.bdev);
    return 0;
}
