#ifndef ARCTIAN_EXFAT_H
#define ARCTIAN_EXFAT_H

#include <stdint.h>
#include "blockdev.h"
#include "kernel.h"

/* Header-only exFAT salt-okunur dosya sistemi surucusu (volfs.c tarafindan
 * dahil edilir; Makefile degisikligi gerektirmez). */

typedef struct {
    int      bdev;
    int      mounted;
    uint64_t part_lba;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t bps512;
    uint32_t fat_offset;
    uint32_t fat_length;
    uint32_t cluster_heap_offset;
    uint32_t cluster_count;
    uint32_t root_cluster;
} exfat_fs_t;

typedef struct {
    char     name[256];
    int      is_dir;
    uint64_t size;
    uint32_t first_cluster;
    int      no_fat_chain;
} exfat_dirent_t;

static exfat_fs_t s_exfat;
static uint8_t    s_ex_sec[4096];

static inline uint16_t ex_r16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t ex_r32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t ex_r64(const uint8_t *p) {
    return (uint64_t)ex_r32(p) | ((uint64_t)ex_r32(p + 4) << 32);
}

static uint32_t ex_utf8_next(const char **ps) {
    const unsigned char *s = (const unsigned char *)*ps;
    unsigned char c = s[0];
    if (!c) return 0;
    if (c < 0x80) { (*ps)++; return c; }
    if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
        *ps += 2; return cp;
    }
    if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (uint32_t)(s[2] & 0x3F);
        *ps += 3; return cp;
    }
    (*ps)++;
    return c;
}

static uint32_t ex_fold(uint32_t cp) {
    if (cp >= 'a' && cp <= 'z') return cp - 32;
    switch (cp) {
        case 0x011F: return 0x011E;
        case 0x00FC: return 0x00DC;
        case 0x015F: return 0x015E;
        case 0x00F6: return 0x00D6;
        case 0x00E7: return 0x00C7;
        case 0x0131: case 0x0130: return 'I';
        default: return cp;
    }
}

static int ex_eq_ci(const char *a, const char *b) {
    for (;;) {
        uint32_t ca = ex_fold(ex_utf8_next(&a));
        uint32_t cb = ex_fold(ex_utf8_next(&b));
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
}

static int ex_utf16_to_utf8(const uint16_t *u16, int len, char *out, int cap) {
    int p = 0;
    for (int i = 0; i < len; i++) {
        uint16_t w = u16[i];
        if (!w) break;
        if (w < 0x80) {
            if (p + 1 >= cap) break;
            out[p++] = (char)w;
        } else if (w < 0x800) {
            if (p + 2 >= cap) break;
            out[p++] = (char)(0xC0 | (w >> 6));
            out[p++] = (char)(0x80 | (w & 0x3F));
        } else {
            if (p + 3 >= cap) break;
            out[p++] = (char)(0xE0 | (w >> 12));
            out[p++] = (char)(0x80 | ((w >> 6) & 0x3F));
            out[p++] = (char)(0x80 | (w & 0x3F));
        }
    }
    if (cap > 0) out[p] = 0;
    return p;
}

static int ex_rd_sec(exfat_fs_t *fs, uint32_t lba, uint8_t *buf) {
    return blockdev_read(fs->bdev, fs->part_lba + (uint64_t)lba * fs->bps512, fs->bps512, buf);
}

static uint32_t ex_cluster_lba(exfat_fs_t *fs, uint32_t c) {
    return fs->cluster_heap_offset + (c - 2u) * fs->sectors_per_cluster;
}

static uint32_t ex_next_cluster(exfat_fs_t *fs, uint32_t c, int no_fat_chain) {
    if (no_fat_chain) return c + 1u;
    uint32_t off = c * 4u;
    uint32_t sec = fs->fat_offset + off / fs->bytes_per_sector;
    uint32_t idx = off % fs->bytes_per_sector;
    if (ex_rd_sec(fs, sec, s_ex_sec) != 0) return 0xFFFFFFFFu;
    uint32_t v = ex_r32(s_ex_sec + idx);
    if (v >= 0xFFFFFFF8u || v < 2u) return 0xFFFFFFFFu;
    return v;
}

static int ex_is_vbr(const uint8_t *bs) {
    if (bs[510] != 0x55 || bs[511] != 0xAA) return 0;
    if (memcmp(bs + 3, "EXFAT   ", 8) != 0) return 0;
    uint8_t bps_s = bs[108];
    uint8_t spc_s = bs[109];
    if (bps_s < 9 || bps_s > 12 || spc_s > 25) return 0;
    return 1;
}

static int exfat_mount(int bdev) {
    memset(&s_exfat, 0, sizeof(s_exfat));

    uint8_t lba0[512];
    if (blockdev_read(bdev, 0, 1, lba0) != 0) return -1;

    uint64_t part_lba = 0;
    uint8_t bs[512];
    int found = 0;

    if (ex_is_vbr(lba0)) {
        memcpy(bs, lba0, 512);
        found = 1;
    } else if (lba0[510] == 0x55 && lba0[511] == 0xAA) {
        /* GPT check */
        uint8_t gpt[512];
        if (blockdev_read(bdev, 1, 1, gpt) == 0 && memcmp(gpt, "EFI PART", 8) == 0) {
            uint64_t ent_lba = ex_r64(gpt + 72);
            uint32_t num_ent = ex_r32(gpt + 80);
            uint32_t ent_sz  = ex_r32(gpt + 84);
            if (num_ent > 128) num_ent = 128;
            if (ent_sz >= 128 && ent_sz <= 512) {
                uint32_t per_sec = 512u / ent_sz;
                uint32_t nsec = (num_ent + per_sec - 1u) / per_sec;
                for (uint32_t s = 0; s < nsec && !found; s++) {
                    uint8_t esec[512];
                    if (blockdev_read(bdev, ent_lba + s, 1, esec) != 0) break;
                    for (uint32_t k = 0; k < per_sec; k++) {
                        const uint8_t *e = esec + k * ent_sz;
                        uint64_t slba = ex_r64(e + 32);
                        if (slba > 0 && blockdev_read(bdev, slba, 1, bs) == 0 && ex_is_vbr(bs)) {
                            part_lba = slba;
                            found = 1;
                            break;
                        }
                    }
                }
            }
        }
        /* MBR check */
        for (int i = 0; i < 4 && !found; i++) {
            const uint8_t *pe = lba0 + 0x1BE + i * 16;
            uint8_t ptype = pe[4];
            uint32_t slba = ex_r32(pe + 8);
            if ((ptype == 0x07 || ptype == 0x0B || ptype == 0x0C || ptype == 0x0E) && slba > 0) {
                if (blockdev_read(bdev, slba, 1, bs) == 0 && ex_is_vbr(bs)) {
                    part_lba = slba;
                    found = 1;
                }
            }
        }
    }

    if (!found) return -1;

    s_exfat.bdev = bdev;
    s_exfat.part_lba = part_lba;
    s_exfat.bytes_per_sector = 1u << bs[108];
    s_exfat.sectors_per_cluster = 1u << bs[109];
    s_exfat.bps512 = s_exfat.bytes_per_sector / 512u;
    s_exfat.fat_offset = ex_r32(bs + 80);
    s_exfat.fat_length = ex_r32(bs + 84);
    s_exfat.cluster_heap_offset = ex_r32(bs + 88);
    s_exfat.cluster_count = ex_r32(bs + 92);
    s_exfat.root_cluster = ex_r32(bs + 96);
    if (s_exfat.root_cluster < 2 || s_exfat.cluster_count == 0) return -2;

    s_exfat.mounted = 1;
    return 0;
}

static inline void exfat_unmount(void) { memset(&s_exfat, 0, sizeof(s_exfat)); }
static inline int  exfat_mounted(void) { return s_exfat.mounted; }
static inline int  exfat_bdev(void)    { return s_exfat.mounted ? s_exfat.bdev : -1; }

static int exfat_dir_iter(exfat_fs_t *fs, uint32_t start_c, int no_chain,
                          int target, exfat_dirent_t *out) {
    uint32_t c = start_c;
    uint32_t steps = 0;
    int idx = 0;

    int state = 0; /* 0=idle, 1=expect stream, 2=expect filename */
    int sec_rem = 0;
    int name_len = 0;
    int name_got = 0;
    uint16_t u16[256];
    exfat_dirent_t cur;
    memset(&cur, 0, sizeof(cur));

    while (c >= 2 && c < fs->cluster_count + 2u && steps++ <= fs->cluster_count) {
        uint32_t lba = ex_cluster_lba(fs, c);
        for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
            if (ex_rd_sec(fs, lba + s, s_ex_sec) != 0) return -1;
            for (int e = 0; e + 32 <= (int)fs->bytes_per_sector; e += 32) {
                const uint8_t *de = s_ex_sec + e;
                uint8_t etype = de[0];
                if (etype == 0x00) return 1;
                if (!(etype & 0x80)) { state = 0; continue; }

                if (etype == 0x85) {
                    memset(&cur, 0, sizeof(cur));
                    sec_rem = de[1];
                    cur.is_dir = (ex_r16(de + 4) & 0x10u) ? 1 : 0;
                    name_len = 0;
                    name_got = 0;
                    state = (sec_rem >= 2) ? 1 : 0;
                } else if (state == 1 && etype == 0xC0) {
                    cur.no_fat_chain = (de[1] & 0x02u) ? 1 : 0;
                    name_len = de[3];
                    cur.first_cluster = ex_r32(de + 20);
                    cur.size = ex_r64(de + 24);
                    sec_rem--;
                    state = 2;
                } else if (state == 2 && etype == 0xC1) {
                    for (int k = 0; k < 15 && name_got < name_len && name_got < 255; k++) {
                        u16[name_got++] = ex_r16(de + 2 + k * 2);
                    }
                    sec_rem--;
                    if (sec_rem <= 0 || name_got >= name_len) {
                        ex_utf16_to_utf8(u16, name_got, cur.name, sizeof(cur.name));
                        if (idx == target) {
                            *out = cur;
                            return 0;
                        }
                        idx++;
                        state = 0;
                    }
                } else if (state > 0) {
                    if (--sec_rem <= 0) state = 0;
                }
            }
        }
        uint32_t nxt = ex_next_cluster(fs, c, no_chain);
        if (nxt >= 0xFFFFFFF8u || nxt < 2u) break;
        c = nxt;
    }
    return 1;
}

static int exfat_resolve(exfat_fs_t *fs, const char *path, exfat_dirent_t *out) {
    const char *p = path;
    while (*p == '/') p++;
    if (!*p) {
        memset(out, 0, sizeof(*out));
        out->is_dir = 1;
        out->first_cluster = fs->root_cluster;
        out->name[0] = '/'; out->name[1] = 0;
        return 0;
    }

    uint32_t c = fs->root_cluster;
    int nc = 0;
    char comp[256];
    while (*p) {
        int n = 0;
        while (*p && *p != '/') { if (n < 255) comp[n++] = *p; p++; }
        comp[n] = 0;
        while (*p == '/') p++;

        exfat_dirent_t de;
        int found = 0;
        for (int i = 0; ; i++) {
            if (exfat_dir_iter(fs, c, nc, i, &de) != 0) break;
            if (ex_eq_ci(de.name, comp)) { found = 1; break; }
        }
        if (!found) return -1;
        if (!*p) { *out = de; return 0; }
        if (!de.is_dir) return -1;
        c = de.first_cluster;
        nc = de.no_fat_chain;
    }
    return -1;
}

static inline int exfat_count_dir(const char *path) {
    if (!s_exfat.mounted) return 0;
    exfat_dirent_t de;
    if (exfat_resolve(&s_exfat, path, &de) != 0 || !de.is_dir) return 0;
    int n = 0;
    exfat_dirent_t tmp;
    while (exfat_dir_iter(&s_exfat, de.first_cluster, de.no_fat_chain, n, &tmp) == 0) n++;
    return n;
}

static inline int exfat_list_dir(const char *path, int idx, char *out, int cap, int *is_dir) {
    if (!s_exfat.mounted) return -1;
    exfat_dirent_t de;
    if (exfat_resolve(&s_exfat, path, &de) != 0 || !de.is_dir) return -1;
    exfat_dirent_t ch;
    int r = exfat_dir_iter(&s_exfat, de.first_cluster, de.no_fat_chain, idx, &ch);
    if (r != 0) return r;
    int i = 0;
    for (; ch.name[i] && i < cap - 1; i++) out[i] = ch.name[i];
    if (cap > 0) out[i] = 0;
    if (is_dir) *is_dir = ch.is_dir;
    return 0;
}

static inline uint32_t exfat_read_path(const char *path, void *buf, uint32_t size) {
    if (!s_exfat.mounted || !buf) return 0;
    exfat_dirent_t de;
    if (exfat_resolve(&s_exfat, path, &de) != 0 || de.is_dir) return 0;
    uint32_t toread = (de.size < (uint64_t)size) ? (uint32_t)de.size : size;
    uint32_t done = 0;
    uint32_t c = de.first_cluster;
    uint8_t *out = (uint8_t *)buf;
    uint32_t steps = 0;

    while (done < toread && c >= 2 && c < s_exfat.cluster_count + 2u && steps++ <= s_exfat.cluster_count) {
        uint32_t lba = ex_cluster_lba(&s_exfat, c);
        for (uint32_t s = 0; s < s_exfat.sectors_per_cluster && done < toread; s++) {
            if (ex_rd_sec(&s_exfat, lba + s, s_ex_sec) != 0) return done;
            uint32_t chunk = s_exfat.bytes_per_sector;
            if (chunk > toread - done) chunk = toread - done;
            memcpy(out + done, s_ex_sec, chunk);
            done += chunk;
        }
        c = ex_next_cluster(&s_exfat, c, de.no_fat_chain);
        if (c >= 0xFFFFFFF8u || c < 2u) break;
    }
    return done;
}

static inline int exfat_stat(const char *path, uint32_t *size, int *is_dir) {
    if (!s_exfat.mounted) return -1;
    exfat_dirent_t de;
    if (exfat_resolve(&s_exfat, path, &de) != 0) return -1;
    if (size) *size = (uint32_t)(de.size > 0xFFFFFFFFULL ? 0xFFFFFFFFu : de.size);
    if (is_dir) *is_dir = de.is_dir;
    return 0;
}

#endif /* ARCTIAN_EXFAT_H */
