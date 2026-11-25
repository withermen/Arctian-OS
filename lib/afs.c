/**
 * ArctianFS (AFS) v6 - diske kalici dosya sistemi
 *
 * Ozellikler:
 *  - AFS v6 on-disk uyumlulugu (magic 0x53464121, version 6, 92B girdi)
 *  - Dinamik dosya tablosu (1024 dosyaya kadar) + 256-sektor metadata gunlugu
 *  - Bellek-ici sirali bos-alan (free-extent) tablosu: silme/uzerine yazma/
 *    kirpma sonrasinda bitisik bloklari birlestirir (coalesce) ve data_next'i
 *    kuyruktan geri ceker (tail trim)
 *  - 255 bayta kadar uzun yol destegi (AFS_PATH_MAX=256): >=64B yollar
 *    kisa @LN_<hex8>_<tail> belirteci + gizli .afs_longnames tablosuyla
 *    v6 disk yapisini bozmadan saklanir
 *  - Tum buyuk tamponlar kmalloc() uzerinden ayrilir (main_kernel BSS guvenli)
 */
#include "afs.h"
#include "storage.h"
#include "diskcrypt_hdr.h"
#include "alloc.h"
#include "serial.h"
#include <stddef.h>

#define AFS_MAGIC            0x53464121u  /* "!AFS" */
#define AFS_MAX_FILES_HARD   1024u
#define AFS_HEADER_BASE      36u          /* magic..flags + 64 bit kapasite */
#define AFS_ENTRY_SIZE       (AFS_NAME_MAX + 28u)  /* 92 byte */
#define AFS_HDR_MAX_SEC \
    ((AFS_HEADER_BASE + AFS_MAX_FILES_HARD * AFS_ENTRY_SIZE + 511u) / 512u)
#define AFS_CLUSTER_SEC      2048u
#define AFS_RESERVE_SEC      2048u        /* diskin sonunda birakilan emniyet alani */

#define AFS_JOURNAL_MAGIC    0x4A464121u  /* "!AFJ" */
#define AFS_JOURNAL_SEC      256u         /* 185-sektor header + 1-sektor kayit sigar */

#define AFS_LONGNAME_FILE    ".afs_longnames"
#define AFS_LN_MAGIC         0x4E4C4641u  /* "AFLN" */
#define AFS_LN_VERSION       1u
#define AFS_MAX_LONGNAMES    AFS_MAX_FILES_HARD
#define AFS_MAX_FREE_EXTENTS (AFS_MAX_FILES_HARD + 64u)

typedef struct __attribute__((packed)) {
    char     name[AFS_NAME_MAX];
    uint32_t lba;
    uint32_t size;
    uint32_t used;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    uint32_t pad;
} afs_file_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t max_files;
    uint32_t count;
    uint32_t data_next;
    uint32_t hdr_sectors;
    uint64_t capacity;
    uint32_t flags;
    afs_file_t files[AFS_MAX_FILES_HARD];
} afs_hdr_t;

typedef struct {
    uint32_t start;
    uint32_t count;
} afs_ext_t;

typedef struct {
    char     token[AFS_NAME_MAX];
    char     path[AFS_PATH_MAX];
} afs_ln_ent_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t crc32;
} afs_ln_hdr_t;

typedef struct __attribute__((packed)) {
    char     token[AFS_NAME_MAX];
    uint16_t path_len;
    char     path[AFS_PATH_MAX];
} afs_ln_disk_ent_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t seq;
    uint32_t hdr_sectors;
    uint32_t crc;
    uint32_t state;
    uint32_t prev_crc;
} afs_jrec_t;

_Static_assert(sizeof(afs_file_t) == 92u, "afs_file_t must remain 92 bytes");
_Static_assert(AFS_HDR_MAX_SEC + 1u <= AFS_JOURNAL_SEC, "header must fit inside journal");

/* Tum buyuk tablolar kmalloc() ile ayrilir; BSS boyutu < 2 KB kalir. */
static afs_hdr_t    *s_hdr = 0;
#define H            (*s_hdr)
static uint8_t      *s_hbuf = 0;
static uint32_t     *s_hgen = 0;
static afs_ext_t    *s_free_exts = 0;
static afs_ext_t    *s_tmp_exts = 0;
static uint32_t      s_free_count = 0;
static uint32_t      s_reclaimed_secs = 0;
static uint32_t      s_reused_secs = 0;

static afs_ln_ent_t *s_ln_tab = 0;
static uint32_t      s_ln_count = 0;

static int           mounted = 0;
static int           s_locked = 0;
static uint32_t      s_base = AFS_BASE_LEGACY_LBA;
static uint32_t      s_uid = 0;
static uint32_t      s_gid = 0;
static uint32_t      s_jseq = 0;
static uint32_t      s_crc32_tab[256];
static int           s_crc32_ready = 0;

static void zero_mem(void *p, size_t n) {
    uint8_t *b = (uint8_t *)p;
    for (size_t i = 0; i < n; i++) b[i] = 0;
}

static int afs_ensure_mem(void) {
    if (!s_hdr) {
        s_hdr = (afs_hdr_t *)kmalloc(sizeof(afs_hdr_t));
        if (!s_hdr) return -1;
        zero_mem(s_hdr, sizeof(afs_hdr_t));
    }
    if (!s_hbuf) {
        s_hbuf = (uint8_t *)kmalloc((size_t)AFS_HDR_MAX_SEC * 512u);
        if (!s_hbuf) return -1;
        zero_mem(s_hbuf, (size_t)AFS_HDR_MAX_SEC * 512u);
    }
    if (!s_hgen) {
        s_hgen = (uint32_t *)kmalloc((size_t)AFS_MAX_FILES_HARD * sizeof(uint32_t));
        if (!s_hgen) return -1;
        zero_mem(s_hgen, (size_t)AFS_MAX_FILES_HARD * sizeof(uint32_t));
    }
    if (!s_free_exts) {
        s_free_exts = (afs_ext_t *)kmalloc((size_t)AFS_MAX_FREE_EXTENTS * sizeof(afs_ext_t));
        if (!s_free_exts) return -1;
        zero_mem(s_free_exts, (size_t)AFS_MAX_FREE_EXTENTS * sizeof(afs_ext_t));
    }
    if (!s_tmp_exts) {
        s_tmp_exts = (afs_ext_t *)kmalloc((size_t)AFS_MAX_FILES_HARD * sizeof(afs_ext_t));
        if (!s_tmp_exts) return -1;
        zero_mem(s_tmp_exts, (size_t)AFS_MAX_FILES_HARD * sizeof(afs_ext_t));
    }
    if (!s_ln_tab) {
        s_ln_tab = (afs_ln_ent_t *)kmalloc((size_t)AFS_MAX_LONGNAMES * sizeof(afs_ln_ent_t));
        if (!s_ln_tab) return -1;
        zero_mem(s_ln_tab, (size_t)AFS_MAX_LONGNAMES * sizeof(afs_ln_ent_t));
    }
    return 0;
}

uint32_t afs_base(void) { return s_base; }

void afs_set_locked(int v) { s_locked = v ? 1 : 0; }
int  afs_is_locked(void) { return s_locked; }

int afs_disk_is_encrypted(void) {
    uint32_t hdr[128];
    if (storage_read(DISKCRYPT_HDR_LBA, 1, hdr) != 0) return 0;
    return hdr[0] == DISKCRYPT_MAGIC &&
           (hdr[1] == DISKCRYPT_VERSION || hdr[1] == 1u);
}

void afs_set_subject(int uid, int gid) {
    s_uid = (uint32_t)uid;
    s_gid = (uint32_t)gid;
}

void afs_get_subject(int *uid, int *gid) {
    if (uid) *uid = (int)s_uid;
    if (gid) *gid = (int)s_gid;
}

#define AFS_R 4u
#define AFS_W 2u
#define AFS_X 1u

static int perm_ok(uint32_t fuid, uint32_t fgid, uint32_t mode, uint32_t want) {
    if (s_uid == 0) return 1;
    uint32_t bits;
    if (s_uid == fuid) bits = (mode >> 6) & 7u;
    else if (s_gid == fgid) bits = (mode >> 3) & 7u;
    else bits = mode & 7u;
    return (bits & want) == want;
}

static void set_owner(afs_file_t *f, int is_dir) {
    f->uid = s_uid;
    f->gid = s_gid;
    f->mode = is_dir ? 0777u : 0666u;
    f->pad = 0;
}

static void afs_ensure_tree(void);

static int seq(const char *a, const char *b) {
    int i = 0;
    for (; a[i] && b[i]; i++) if (a[i] != b[i]) return 0;
    return a[i] == b[i];
}

static void cpy_cap(char *d, const char *s, int cap) {
    int i = 0;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = '\0';
}

static void cpy(char *d, const char *s) {
    cpy_cap(d, s, (int)AFS_NAME_MAX);
}

static int slen(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static int prefix_of(const char *s, const char *p) {
    for (int i = 0; p[i]; i++) if (s[i] != p[i]) return 0;
    return 1;
}

static uint64_t compute_capacity(void) {
    uint64_t secs = storage_sector_count();
    if (secs == 0) {
        storage_init();
        secs = storage_sector_count();
    }
    uint64_t cap;
    if (secs == 0) return s_base + 65536u;
    cap = secs > AFS_RESERVE_SEC ? secs - AFS_RESERVE_SEC : secs;
    if (cap > 0xFFFFFFFEull) cap = 0xFFFFFFFEull;
    if (cap < (uint64_t)s_base + AFS_CLUSTER_SEC) cap = (uint64_t)s_base + AFS_CLUSTER_SEC;
    return cap;
}

static void hdr_sync_size(void) {
    uint32_t bytes = AFS_HEADER_BASE + H.max_files * AFS_ENTRY_SIZE;
    H.hdr_sectors = (bytes + 511u) / 512u;
}

static void crc32_tab_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1u)));
        s_crc32_tab[i] = c;
    }
    s_crc32_ready = 1;
}

static uint32_t crc32_buf(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    if (!s_crc32_ready) crc32_tab_init();
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++)
        c = s_crc32_tab[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return ~c;
}

static uint32_t hdr_bytes(void) {
    return AFS_HEADER_BASE + H.max_files * AFS_ENTRY_SIZE;
}

static int hdr_load_at(uint32_t lba, uint32_t hs) {
    if (hs == 0 || hs > AFS_HDR_MAX_SEC) return -1;
    if (storage_read(lba, hs, s_hbuf) != 0) return -1;
    uint8_t *d = (uint8_t *)s_hdr;
    uint32_t n = hs * 512u;
    if (n > (uint32_t)sizeof(afs_hdr_t)) n = (uint32_t)sizeof(afs_hdr_t);
    uint32_t i;
    for (i = 0; i < n; i++) d[i] = s_hbuf[i];
    for (; i < (uint32_t)sizeof(afs_hdr_t); i++) d[i] = 0;
    return 0;
}

static int hdr_save_at(uint32_t lba, uint32_t hs) {
    if (hs == 0 || hs > AFS_HDR_MAX_SEC) return -1;
    uint8_t *s = (uint8_t *)s_hdr;
    uint32_t n = (uint32_t)sizeof(afs_hdr_t);
    if (n > hs * 512u) n = hs * 512u;
    uint32_t i;
    for (i = 0; i < n; i++) s_hbuf[i] = s[i];
    for (; i < hs * 512u; i++) s_hbuf[i] = 0;
    return storage_write(lba, hs, s_hbuf);
}

static uint32_t hdr_crc(void) {
    uint32_t saved = H.flags;
    H.flags = 0;
    uint32_t c = crc32_buf(s_hdr, hdr_bytes());
    H.flags = saved;
    return c;
}

static uint32_t journal_lba(void) {
    uint64_t secs = storage_sector_count();
    uint64_t base = (uint64_t)s_base + AFS_RESERVE_SEC + AFS_JOURNAL_SEC;
    if (secs < base) return 0;
    uint64_t j = secs - AFS_RESERVE_SEC - AFS_JOURNAL_SEC;
    if (j <= (uint64_t)s_base) return 0;
    if (j + 1u + AFS_HDR_MAX_SEC > secs) return 0;
    return (uint32_t)j;
}

static void journal_clear(uint32_t jl) {
    uint8_t sec[512];
    afs_jrec_t *rec = (afs_jrec_t *)sec;
    if (storage_read(jl, 1, sec) != 0) return;
    if (rec->magic != AFS_JOURNAL_MAGIC) return;
    rec->state = 0;
    storage_write(jl, 1, sec);
    storage_flush();
}

static int hdr_commit(void) {
    H.version = AFS_VERSION;
    hdr_sync_size();
    uint32_t prev_crc = H.flags;
    H.flags = 0;
    H.flags = hdr_crc();

    uint32_t jl = journal_lba();
    if (jl == 0 || H.data_next > jl || H.hdr_sectors + 1u > AFS_JOURNAL_SEC) {
        return hdr_save_at(s_base, H.hdr_sectors);
    }

    if (hdr_save_at(jl + 1u, H.hdr_sectors) != 0) return -1;
    storage_flush();

    uint8_t sec[512];
    afs_jrec_t *rec = (afs_jrec_t *)sec;
    for (int i = 0; i < 512; i++) sec[i] = 0;
    rec->magic = AFS_JOURNAL_MAGIC;
    rec->seq = ++s_jseq;
    rec->hdr_sectors = H.hdr_sectors;
    rec->crc = crc32_buf(s_hdr, hdr_bytes());
    rec->state = 1;
    rec->prev_crc = prev_crc;
    if (storage_write(jl, 1, sec) != 0) return -1;
    storage_flush();

    if (hdr_save_at(s_base, H.hdr_sectors) != 0) return -1;
    storage_flush();

    rec->state = 0;
    storage_write(jl, 1, sec);
    storage_flush();
    return 0;
}

static void journal_replay(void) {
    uint32_t jl = journal_lba();
    if (jl == 0) return;
    uint8_t sec[512];
    afs_jrec_t *rec = (afs_jrec_t *)sec;
    if (storage_read(jl, 1, sec) != 0) return;
    if (rec->magic != AFS_JOURNAL_MAGIC || rec->state != 1 ||
        rec->hdr_sectors == 0 || rec->hdr_sectors > AFS_HDR_MAX_SEC) return;

    uint32_t cur_hdr[128];
    if (storage_read(s_base, 1, cur_hdr) != 0) return;
    if (cur_hdr[0] == AFS_MAGIC && cur_hdr[8] != rec->prev_crc) {
        journal_clear(jl);
        return;
    }

    if (hdr_load_at(jl + 1u, rec->hdr_sectors) != 0) return;
    if (H.magic != AFS_MAGIC || (H.version != AFS_VERSION && H.version != 5u) ||
        H.max_files < 32u || H.max_files > AFS_MAX_FILES_HARD) { journal_clear(jl); return; }
    if (crc32_buf(s_hdr, hdr_bytes()) != rec->crc) { journal_clear(jl); return; }
    hdr_save_at(s_base, rec->hdr_sectors);
    storage_flush();
    journal_clear(jl);
}

/* ---- Bos-alan (free-extent) yonetimi + birlestirme + kuyruk kirpma ---- */

static uint32_t afs_data_start(void) {
    return s_base + H.hdr_sectors;
}

static uint32_t file_secs(const afs_file_t *f) {
    uint32_t s = (f->size + 511u) / 512u;
    return s ? s : 1u;
}

static void free_ext_trim_tail(void) {
    uint32_t ds = afs_data_start();
    while (s_free_count > 0) {
        afs_ext_t *last = &s_free_exts[s_free_count - 1u];
        if ((uint64_t)last->start + last->count >= (uint64_t)H.data_next) {
            if (last->start < H.data_next)
                H.data_next = last->start;
            s_free_count--;
        } else {
            break;
        }
    }
    if (H.data_next < ds) H.data_next = ds;
}

static void free_ext_remove_at(uint32_t idx) {
    for (uint32_t k = idx; k + 1u < s_free_count; k++)
        s_free_exts[k] = s_free_exts[k + 1u];
    if (s_free_count > 0) s_free_count--;
}

static void free_ext_add(uint32_t start, uint32_t count) {
    if (!s_free_exts || count == 0) return;
    uint32_t ds = afs_data_start();
    if (start < ds) {
        if ((uint64_t)start + count <= ds) return;
        count -= (ds - start);
        start = ds;
    }
    if (start >= H.data_next) return;
    if ((uint64_t)start + count > H.data_next)
        count = H.data_next - start;
    if (count == 0) return;

    uint32_t pos = 0;
    while (pos < s_free_count && s_free_exts[pos].start < start) pos++;

    /* Cift-serbest birakma korumasi */
    if (pos > 0) {
        uint32_t prev_end = s_free_exts[pos - 1u].start + s_free_exts[pos - 1u].count;
        if (prev_end > start) {
            uint32_t ov = prev_end - start;
            if (ov >= count) return;
            start += ov;
            count -= ov;
        }
    }
    if (pos < s_free_count && start + count > s_free_exts[pos].start) {
        if (start >= s_free_exts[pos].start) return;
        count = s_free_exts[pos].start - start;
        if (count == 0) return;
    }

    int merge_prev = (pos > 0 &&
                      s_free_exts[pos - 1u].start + s_free_exts[pos - 1u].count == start);
    int merge_next = (pos < s_free_count &&
                      start + count == s_free_exts[pos].start);

    if (merge_prev && merge_next) {
        s_free_exts[pos - 1u].count += count + s_free_exts[pos].count;
        free_ext_remove_at(pos);
    } else if (merge_prev) {
        s_free_exts[pos - 1u].count += count;
    } else if (merge_next) {
        s_free_exts[pos].start = start;
        s_free_exts[pos].count += count;
    } else if (s_free_count < AFS_MAX_FREE_EXTENTS) {
        for (uint32_t k = s_free_count; k > pos; k--)
            s_free_exts[k] = s_free_exts[k - 1u];
        s_free_exts[pos].start = start;
        s_free_exts[pos].count = count;
        s_free_count++;
    }

    s_reclaimed_secs += count;
    free_ext_trim_tail();
}

static uint32_t free_ext_alloc(uint32_t secs) {
    if (secs == 0) secs = 1;
    int best = -1;
    uint32_t best_cnt = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < s_free_count; i++) {
        uint32_t c = s_free_exts[i].count;
        if (c >= secs && c < best_cnt) {
            best = (int)i;
            best_cnt = c;
            if (c == secs) break;
        }
    }
    if (best >= 0) {
        uint32_t idx = (uint32_t)best;
        uint32_t lba = s_free_exts[idx].start;
        if (s_free_exts[idx].count == secs) {
            free_ext_remove_at(idx);
        } else {
            s_free_exts[idx].start += secs;
            s_free_exts[idx].count -= secs;
        }
        s_reused_secs += secs;
        return lba;
    }

    if ((uint64_t)H.data_next + secs > H.capacity) return 0;
    uint32_t lba = H.data_next;
    H.data_next += secs;
    return lba;
}

static int free_ext_consume_at(uint32_t start, uint32_t secs) {
    for (uint32_t i = 0; i < s_free_count; i++) {
        if (s_free_exts[i].start == start && s_free_exts[i].count >= secs) {
            if (s_free_exts[i].count == secs) free_ext_remove_at(i);
            else {
                s_free_exts[i].start += secs;
                s_free_exts[i].count -= secs;
            }
            s_reused_secs += secs;
            return 1;
        }
        if (s_free_exts[i].start > start) break;
    }
    return 0;
}

static void free_ext_rebuild(void) {
    if (!s_free_exts || !s_tmp_exts || !s_hdr) return;
    s_free_count = 0;
    uint32_t ds = afs_data_start();
    uint32_t dn = H.data_next;
    if (dn <= ds) return;

    uint32_t n = 0;
    for (uint32_t i = 0; i < H.max_files; i++) {
        if (!H.files[i].used) continue;
        uint32_t lba = H.files[i].lba;
        if (lba < ds || lba >= dn) continue;
        uint32_t secs = file_secs(&H.files[i]);
        if ((uint64_t)lba + secs > dn) secs = dn - lba;
        if (secs == 0) continue;
        s_tmp_exts[n].start = lba;
        s_tmp_exts[n].count = secs;
        n++;
    }

    /* Shell sort (n <= 1024) */
    for (uint32_t gap = n / 2u; gap > 0; gap /= 2u) {
        for (uint32_t i = gap; i < n; i++) {
            afs_ext_t tmp = s_tmp_exts[i];
            uint32_t j = i;
            while (j >= gap && s_tmp_exts[j - gap].start > tmp.start) {
                s_tmp_exts[j] = s_tmp_exts[j - gap];
                j -= gap;
            }
            s_tmp_exts[j] = tmp;
        }
    }

    uint32_t cursor = ds;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t st = s_tmp_exts[i].start;
        uint32_t end = st + s_tmp_exts[i].count;
        if (st > cursor && s_free_count < AFS_MAX_FREE_EXTENTS) {
            s_free_exts[s_free_count].start = cursor;
            s_free_exts[s_free_count].count = st - cursor;
            s_free_count++;
        }
        if (end > cursor) cursor = end;
    }
    if (cursor < dn && s_free_count < AFS_MAX_FREE_EXTENTS) {
        s_free_exts[s_free_count].start = cursor;
        s_free_exts[s_free_count].count = dn - cursor;
        s_free_count++;
    }
}

void afs_free_extent_stats(afs_reclaim_stats_t *out) {
    if (!out) return;
    if (!mounted) afs_mount();
    uint32_t fsecs = 0;
    for (uint32_t i = 0; i < s_free_count; i++)
        fsecs += s_free_exts[i].count;
    out->free_extents = s_free_count;
    out->free_sectors = fsecs;
    out->reclaimed_sectors = s_reclaimed_secs;
    out->reused_sectors = s_reused_secs;
    out->data_next = s_hdr ? H.data_next : 0;
    out->data_start = s_hdr ? afs_data_start() : 0;
    out->capacity = s_hdr ? (uint32_t)H.capacity : 0;
}

/* ---- Uzun yol tablosu (@LN_<hex8>_<tail> <-> .afs_longnames) ---- */

static int is_ln_token(const char *s) {
    return s && s[0] == '@' && s[1] == 'L' && s[2] == 'N' && s[3] == '_';
}

static int find_raw_idx(const char *name) {
    for (uint32_t i = 0; i < H.max_files; i++)
        if (H.files[i].used && seq(H.files[i].name, name)) return (int)i;
    return -1;
}

static int ln_find_by_token(const char *tok) {
    for (uint32_t i = 0; i < s_ln_count; i++)
        if (seq(s_ln_tab[i].token, tok)) return (int)i;
    return -1;
}

static int ln_find_by_path(const char *path) {
    for (uint32_t i = 0; i < s_ln_count; i++)
        if (seq(s_ln_tab[i].path, path)) return (int)i;
    return -1;
}

static const char *entry_full_name(uint32_t idx) {
    const char *nm = H.files[idx].name;
    if (is_ln_token(nm)) {
        int k = ln_find_by_token(nm);
        if (k >= 0) return s_ln_tab[k].path;
    }
    return nm;
}

static void ln_make_token(const char *full_path, int is_dir, char out[AFS_NAME_MAX]) {
    static const char hex[] = "0123456789ABCDEF";
    int plen = slen(full_path);
    int core_len = (is_dir && plen > 0 && full_path[plen - 1] == '/') ? (plen - 1) : plen;
    uint32_t base_h = crc32_buf(full_path, (uint32_t)plen);

    /* "@LN_" (4) + 8 hex (8) + "_" (1) = 13 byte onek */
    int max_tail = (int)AFS_NAME_MAX - 1 - 13 - (is_dir ? 1 : 0);
    int tlen = core_len < max_tail ? core_len : max_tail;
    int tstart = core_len - tlen;

    for (uint32_t salt = 0; ; salt++) {
        uint32_t h = base_h ^ (salt * 0x9E3779B9u);
        int o = 0;
        out[o++] = '@'; out[o++] = 'L'; out[o++] = 'N'; out[o++] = '_';
        for (int b = 28; b >= 0; b -= 4)
            out[o++] = hex[(h >> b) & 0xFu];
        out[o++] = '_';
        for (int k = 0; k < tlen; k++)
            out[o++] = full_path[tstart + k];
        if (is_dir) out[o++] = '/';
        out[o] = '\0';

        if (ln_find_by_token(out) < 0 && find_raw_idx(out) < 0)
            return;
    }
}

static void ln_remove_token(const char *tok) {
    int k = ln_find_by_token(tok);
    if (k < 0) return;
    for (uint32_t i = (uint32_t)k; i + 1u < s_ln_count; i++)
        s_ln_tab[i] = s_ln_tab[i + 1u];
    if (s_ln_count > 0) s_ln_count--;
}

static int ln_add_entry(const char *tok, const char *path) {
    if (s_ln_count >= AFS_MAX_LONGNAMES) return -1;
    cpy_cap(s_ln_tab[s_ln_count].token, tok, (int)AFS_NAME_MAX);
    cpy_cap(s_ln_tab[s_ln_count].path, path, (int)AFS_PATH_MAX);
    s_ln_count++;
    return 0;
}

static void ln_load(void) {
    s_ln_count = 0;
    int idx = find_raw_idx(AFS_LONGNAME_FILE);
    if (idx < 0) return;
    uint32_t sz = H.files[idx].size;
    if (sz < (uint32_t)sizeof(afs_ln_hdr_t)) return;
    uint32_t secs = file_secs(&H.files[idx]);
    uint8_t *buf = (uint8_t *)kmalloc((size_t)secs * 512u);
    if (!buf) return;
    if (storage_read(H.files[idx].lba, secs, buf) != 0) {
        kfree(buf);
        return;
    }
    const afs_ln_hdr_t *lh = (const afs_ln_hdr_t *)buf;
    if (lh->magic != AFS_LN_MAGIC || lh->version != AFS_LN_VERSION ||
        lh->count > AFS_MAX_LONGNAMES) {
        kfree(buf);
        return;
    }
    uint32_t need = (uint32_t)sizeof(afs_ln_hdr_t) +
                    lh->count * (uint32_t)sizeof(afs_ln_disk_ent_t);
    if (sz < need) {
        kfree(buf);
        return;
    }
    const afs_ln_disk_ent_t *ents = (const afs_ln_disk_ent_t *)(buf + sizeof(afs_ln_hdr_t));
    if (lh->count > 0 &&
        crc32_buf(ents, lh->count * (uint32_t)sizeof(afs_ln_disk_ent_t)) != lh->crc32) {
        kfree(buf);
        return;
    }
    for (uint32_t i = 0; i < lh->count; i++) {
        cpy_cap(s_ln_tab[s_ln_count].token, ents[i].token, (int)AFS_NAME_MAX);
        cpy_cap(s_ln_tab[s_ln_count].path, ents[i].path, (int)AFS_PATH_MAX);
        s_ln_count++;
    }
    kfree(buf);
}

/* .afs_longnames icerigini diske yazar; hdr_commit() cagiran tarafindan yapilir. */
static int ln_persist_no_commit(void) {
    int idx = find_raw_idx(AFS_LONGNAME_FILE);
    if (s_ln_count == 0) {
        if (idx >= 0) {
            uint32_t secs = file_secs(&H.files[idx]);
            free_ext_add(H.files[idx].lba, secs);
            H.files[idx].used = 0;
            H.files[idx].size = 0;
            H.files[idx].lba = 0;
            H.files[idx].name[0] = 0;
            s_hgen[idx]++;
            if (H.count) H.count--;
        }
        return 0;
    }

    uint32_t data_sz = (uint32_t)sizeof(afs_ln_hdr_t) +
                       s_ln_count * (uint32_t)sizeof(afs_ln_disk_ent_t);
    uint32_t secs = (data_sz + 511u) / 512u;
    if (secs == 0) secs = 1;
    uint8_t *buf = (uint8_t *)kmalloc((size_t)secs * 512u);
    if (!buf) return -1;
    zero_mem(buf, (size_t)secs * 512u);

    afs_ln_hdr_t *lh = (afs_ln_hdr_t *)buf;
    afs_ln_disk_ent_t *ents = (afs_ln_disk_ent_t *)(buf + sizeof(afs_ln_hdr_t));
    for (uint32_t i = 0; i < s_ln_count; i++) {
        cpy_cap(ents[i].token, s_ln_tab[i].token, (int)AFS_NAME_MAX);
        cpy_cap(ents[i].path, s_ln_tab[i].path, (int)AFS_PATH_MAX);
        ents[i].path_len = (uint16_t)slen(s_ln_tab[i].path);
    }
    lh->magic = AFS_LN_MAGIC;
    lh->version = AFS_LN_VERSION;
    lh->count = s_ln_count;
    lh->crc32 = crc32_buf(ents, s_ln_count * (uint32_t)sizeof(afs_ln_disk_ent_t));

    if (idx < 0) {
        /* Kullanici indekslerini kirletmemek icin en yuksek bos yuvayi sec. */
        for (int k = (int)H.max_files - 1; k >= 0; k--) {
            if (!H.files[k].used) { idx = k; break; }
        }
        if (idx < 0) { kfree(buf); return -1; }
        uint32_t lba = free_ext_alloc(secs);
        if (lba == 0) { kfree(buf); return -1; }
        if (storage_write(lba, secs, buf) != 0) {
            free_ext_add(lba, secs);
            kfree(buf);
            return -1;
        }
        H.files[idx].used = 1;
        H.files[idx].lba = lba;
        H.files[idx].size = data_sz;
        H.files[idx].uid = 0;
        H.files[idx].gid = 0;
        H.files[idx].mode = 0600u;
        H.files[idx].pad = 0;
        cpy(H.files[idx].name, AFS_LONGNAME_FILE);
        H.count++;
        s_hgen[idx]++;
    } else {
        uint32_t old_lba = H.files[idx].lba;
        uint32_t old_secs = file_secs(&H.files[idx]);
        if (secs <= old_secs) {
            if (storage_write(old_lba, secs, buf) != 0) { kfree(buf); return -1; }
            if (secs < old_secs)
                free_ext_add(old_lba + secs, old_secs - secs);
            H.files[idx].size = data_sz;
        } else {
            uint32_t new_lba = free_ext_alloc(secs);
            if (new_lba == 0) { kfree(buf); return -1; }
            if (storage_write(new_lba, secs, buf) != 0) {
                free_ext_add(new_lba, secs);
                kfree(buf);
                return -1;
            }
            free_ext_add(old_lba, old_secs);
            H.files[idx].lba = new_lba;
            H.files[idx].size = data_sz;
        }
    }
    kfree(buf);
    return 0;
}

/* ---- Format & Mount ---- */

void afs_format(void) {
    if (afs_ensure_mem() != 0) return;
    s_base = AFS_BASE_LBA;
    H.magic = AFS_MAGIC;
    H.version = AFS_VERSION;
    H.capacity = compute_capacity();
    if (H.capacity > s_base + AFS_JOURNAL_SEC)
        H.capacity -= AFS_JOURNAL_SEC;
    H.flags = 0;

    uint32_t room = (uint32_t)(H.capacity - s_base);
    uint32_t mf;
    if (room >= 32768u) {
        mf = AFS_MAX_FILES_HARD;
    } else if (room >= 4096u) {
        mf = room / 32u;
        if (mf < 128u) mf = 128u;
        if (mf > AFS_MAX_FILES_HARD) mf = AFS_MAX_FILES_HARD;
    } else {
        mf = 32u;
    }
    H.max_files = mf;
    H.count = 0;
    hdr_sync_size();
    H.data_next = s_base + H.hdr_sectors;

    for (uint32_t i = 0; i < AFS_MAX_FILES_HARD; i++) {
        H.files[i].used = 0;
        H.files[i].size = 0;
        H.files[i].lba = 0;
        H.files[i].uid = 0;
        H.files[i].gid = 0;
        H.files[i].mode = 0;
        H.files[i].pad = 0;
        H.files[i].name[0] = 0;
        s_hgen[i]++;
    }
    s_free_count = 0;
    s_reclaimed_secs = 0;
    s_reused_secs = 0;
    s_ln_count = 0;
    mounted = 1;
    hdr_commit();
    afs_ensure_tree();
}

void afs_mount(void) {
    uint32_t first[128];
    if (s_locked) return;
    if (afs_ensure_mem() != 0) return;
    if (storage_sector_count() == 0) storage_init();

    int have = 0;
    if (storage_read(AFS_BASE_LBA, 1, first) == 0 &&
        first[0] == AFS_MAGIC && first[1] == AFS_VERSION) {
        s_base = AFS_BASE_LBA; have = 1;
    } else if (storage_read(AFS_BASE_LEGACY_LBA, 1, first) == 0 &&
               first[0] == AFS_MAGIC &&
               (first[1] == AFS_VERSION || first[1] == 5u)) {
        s_base = AFS_BASE_LEGACY_LBA; have = 1;
    }
    if (!have) {
        if (afs_disk_is_encrypted()) return;
        afs_format();
        return;
    }
    journal_replay();
    if (storage_read(s_base, 1, first) != 0 ||
        first[0] != AFS_MAGIC ||
        (first[1] != AFS_VERSION && first[1] != 5u)) {
        if (afs_disk_is_encrypted()) return;
        afs_format();
        return;
    }
    H.magic = first[0];
    H.version = first[1];
    H.max_files = first[2];
    H.count = first[3];
    H.data_next = first[4];
    H.hdr_sectors = first[5];
    H.capacity = (uint64_t)first[6] | ((uint64_t)first[7] << 32);
    H.flags = first[8];

    if (H.max_files < 32u || H.max_files > AFS_MAX_FILES_HARD ||
        H.hdr_sectors == 0 || H.hdr_sectors > AFS_HDR_MAX_SEC) {
        if (afs_disk_is_encrypted()) return;
        afs_format();
        return;
    }
    if (hdr_load_at(s_base, H.hdr_sectors) != 0) {
        if (afs_disk_is_encrypted()) return;
        afs_format();
        return;
    }
    if (hdr_crc() != H.flags) {
        if (afs_disk_is_encrypted()) return;
        afs_format();
        return;
    }
    mounted = 1;
    if (H.version != AFS_VERSION) {
        H.version = AFS_VERSION;
        hdr_commit();
    }
    s_reclaimed_secs = 0;
    s_reused_secs = 0;
    ln_load();
    free_ext_rebuild();
    afs_ensure_tree();
    serial_printf("[afs] mount ver=%u max_files=%u count=%u free_extents=%u reclaimed=%u data_next=%u\n",
                  H.version, H.max_files, H.count, s_free_count, s_reclaimed_secs, H.data_next);
}

int afs_count(void) { if (!mounted) afs_mount(); return s_hdr ? (int)H.count : 0; }

uint32_t afs_data_next(void) { if (!mounted) afs_mount(); return s_hdr ? H.data_next : 0; }

static int find_idx(const char *name) {
    if (!name || !name[0] || !s_hdr) return -1;
    int nlen = slen(name);
    if (nlen < (int)AFS_NAME_MAX) {
        for (uint32_t i = 0; i < H.max_files; i++)
            if (H.files[i].used && seq(H.files[i].name, name)) return (int)i;
        return -1;
    }
    if (nlen >= (int)AFS_PATH_MAX) return -1;
    int k = ln_find_by_path(name);
    if (k < 0) return -1;
    return find_raw_idx(s_ln_tab[k].token);
}

static int parent_idx(const char *path) {
    char p[AFS_PATH_MAX];
    int n = 0;
    for (; path && path[n] && n < (int)sizeof(p) - 2; n++) p[n] = path[n];
    p[n] = 0;
    if (n > 0 && p[n - 1] == '/') p[--n] = 0;
    int cut = -1;
    for (int i = 0; i < n; i++) if (p[i] == '/') cut = i;
    if (cut < 0) return -1;
    p[cut + 1] = 0;
    return find_idx(p);
}

static void *hnd_enc(int slot) {
    return (void *)(uintptr_t)(((uint32_t)slot << 14) | ((s_hgen[slot] & 0x1FFFu) << 1) | 1u);
}

static int hnd_dec(void *h) {
    if (!mounted || !s_hdr || !s_hgen) return -1;
    uint32_t v = (uint32_t)(uintptr_t)h;
    if ((v & 1u) == 0) return -1;
    int slot = (int)(v >> 14);
    if (slot < 0 || slot >= (int)H.max_files || !H.files[slot].used) return -1;
    if (((v >> 1) & 0x1FFFu) != (s_hgen[slot] & 0x1FFFu)) return -1;
    return slot;
}

void *afs_create_handle(const char *name) {
    if (!mounted) afs_mount();
    if (!s_hdr || !name || !name[0]) return 0;
    int nlen = slen(name);
    if (nlen >= (int)AFS_PATH_MAX) return 0;
    if (seq(name, AFS_LONGNAME_FILE)) return 0;

    int existing = find_idx(name);
    if (existing >= 0) return hnd_enc(existing);

    int first_free = -1;
    int free_slots = 0;
    for (uint32_t i = 0; i < H.max_files; i++) {
        if (!H.files[i].used) {
            if (first_free < 0) first_free = (int)i;
            free_slots++;
        }
    }
    if (first_free < 0) return 0;

    int is_long = (nlen >= (int)AFS_NAME_MAX);
    int need_ln_slot = (is_long && find_raw_idx(AFS_LONGNAME_FILE) < 0);
    if (free_slots < (need_ln_slot ? 2 : 1)) return 0;

    int pi = parent_idx(name);
    if (pi >= 0 &&
        !perm_ok(H.files[pi].uid, H.files[pi].gid, H.files[pi].mode, AFS_W)) return 0;

    uint32_t i = (uint32_t)first_free;
    int isd = afs_is_dir(name);
    char disk_name[AFS_NAME_MAX];

    if (is_long) {
        ln_make_token(name, isd, disk_name);
        if (ln_add_entry(disk_name, name) != 0) return 0;
        if (ln_persist_no_commit() != 0) {
            ln_remove_token(disk_name);
            return 0;
        }
        /* ln_persist_no_commit en yuksek yuvayi secmis olsa da emin ol */
        if (H.files[i].used) {
            int alt = -1;
            for (uint32_t k = 0; k < H.max_files; k++)
                if (!H.files[k].used) { alt = (int)k; break; }
            if (alt < 0) {
                ln_remove_token(disk_name);
                ln_persist_no_commit();
                return 0;
            }
            i = (uint32_t)alt;
        }
    } else {
        cpy(disk_name, name);
    }

    uint32_t lba = free_ext_alloc(1u);
    if (lba == 0) {
        if (is_long) {
            ln_remove_token(disk_name);
            ln_persist_no_commit();
        }
        return 0;
    }

    H.files[i].used = 1;
    H.files[i].size = 0;
    H.files[i].lba = lba;
    set_owner(&H.files[i], isd);
    cpy(H.files[i].name, disk_name);
    H.count++;
    s_hgen[i]++;
    hdr_commit();
    return hnd_enc((int)i);
}

void *afs_find_handle(const char *name) {
    if (!mounted) afs_mount();
    int i = find_idx(name);
    return i < 0 ? 0 : hnd_enc(i);
}

int afs_rename(const char *from, const char *to) {
    if (!mounted) afs_mount();
    if (!s_hdr || !from || !to || !from[0] || !to[0]) return -1;
    if (slen(from) >= (int)AFS_PATH_MAX || slen(to) >= (int)AFS_PATH_MAX) return -3;

    char fbuf[AFS_PATH_MAX], tbuf[AFS_PATH_MAX];
    cpy_cap(fbuf, from, (int)sizeof(fbuf));
    cpy_cap(tbuf, to, (int)sizeof(tbuf));

    int fi = find_idx(fbuf);
    if (fi < 0) {
        int l = slen(fbuf);
        if (l > 0 && l < (int)AFS_PATH_MAX - 1 && fbuf[l - 1] != '/') {
            fbuf[l] = '/'; fbuf[l + 1] = '\0';
            fi = find_idx(fbuf);
        }
    }
    if (fi < 0) return -1;
    if (seq(H.files[fi].name, AFS_LONGNAME_FILE)) return -1;

    const char *orig_full = entry_full_name((uint32_t)fi);
    int isd = afs_is_dir(orig_full);
    if (isd) {
        int l = slen(tbuf);
        if (l > 0 && l < (int)AFS_PATH_MAX - 1 && tbuf[l - 1] != '/') {
            tbuf[l] = '/'; tbuf[l + 1] = '\0';
        }
        if (l == (int)AFS_PATH_MAX - 1 && tbuf[l - 1] != '/') return -3;
    }
    if (slen(tbuf) >= (int)AFS_PATH_MAX) return -3;
    if (find_idx(tbuf) >= 0) return -2;

    int spi = parent_idx(fbuf);
    if (spi >= 0) {
        if (!perm_ok(H.files[spi].uid, H.files[spi].gid, H.files[spi].mode, AFS_W))
            return -1;
    } else if (!perm_ok(H.files[fi].uid, H.files[fi].gid, H.files[fi].mode, AFS_W)) {
        return -1;
    }
    int tpi = parent_idx(tbuf);
    if (tpi >= 0 && tpi != spi &&
        !perm_ok(H.files[tpi].uid, H.files[tpi].gid, H.files[tpi].mode, AFS_W))
        return -1;

    int ln_dirty = 0;
    int fl = slen(fbuf);

    /* Hedef uzun adsa once .afs_longnames yuvasinin da yeterli oldugunu kontrol et */
    if (slen(tbuf) >= (int)AFS_NAME_MAX) {
        char ntok[AFS_NAME_MAX];
        ln_make_token(tbuf, isd, ntok);
        if (is_ln_token(H.files[fi].name)) {
            int k = ln_find_by_token(H.files[fi].name);
            if (k >= 0) {
                cpy_cap(s_ln_tab[k].token, ntok, (int)AFS_NAME_MAX);
                cpy_cap(s_ln_tab[k].path, tbuf, (int)AFS_PATH_MAX);
            } else if (ln_add_entry(ntok, tbuf) != 0) {
                return -1;
            }
        } else {
            if (ln_add_entry(ntok, tbuf) != 0) return -1;
        }
        cpy(H.files[fi].name, ntok);
        ln_dirty = 1;
    } else {
        if (is_ln_token(H.files[fi].name)) {
            ln_remove_token(H.files[fi].name);
            ln_dirty = 1;
        }
        cpy(H.files[fi].name, tbuf);
    }

    if (isd) {
        for (uint32_t j = 0; j < H.max_files; j++) {
            if ((int)j == fi || !H.files[j].used) continue;
            if (seq(H.files[j].name, AFS_LONGNAME_FILE)) continue;
            const char *cf = entry_full_name(j);
            if (!prefix_of(cf, fbuf)) continue;
            int cl = slen(cf);
            if (slen(tbuf) + (cl - fl) >= (int)AFS_PATH_MAX) continue;
            char nb[AFS_PATH_MAX];
            int n = 0;
            for (int k = 0; tbuf[k] && n < (int)AFS_PATH_MAX - 1; k++) nb[n++] = tbuf[k];
            for (int k = fl; cf[k] && n < (int)AFS_PATH_MAX - 1; k++) nb[n++] = cf[k];
            nb[n] = '\0';

            int child_dir = afs_is_dir(nb);
            if (slen(nb) >= (int)AFS_NAME_MAX) {
                char ctok[AFS_NAME_MAX];
                ln_make_token(nb, child_dir, ctok);
                if (is_ln_token(H.files[j].name)) {
                    int k = ln_find_by_token(H.files[j].name);
                    if (k >= 0) {
                        cpy_cap(s_ln_tab[k].token, ctok, (int)AFS_NAME_MAX);
                        cpy_cap(s_ln_tab[k].path, nb, (int)AFS_PATH_MAX);
                    } else if (ln_add_entry(ctok, nb) != 0) {
                        continue;
                    }
                } else if (ln_add_entry(ctok, nb) != 0) {
                    continue;
                }
                cpy(H.files[j].name, ctok);
                ln_dirty = 1;
            } else {
                if (is_ln_token(H.files[j].name)) {
                    ln_remove_token(H.files[j].name);
                    ln_dirty = 1;
                }
                cpy(H.files[j].name, nb);
            }
        }
    }

    if (ln_dirty) (void)ln_persist_no_commit();
    hdr_commit();
    return 0;
}

int afs_write_handle(void *h, const void *data, uint32_t size) {
    int i = hnd_dec(h);
    if (i < 0) return -1;
    if (!perm_ok(H.files[i].uid, H.files[i].gid, H.files[i].mode, AFS_W)) return -1;

    uint32_t old_lba = H.files[i].lba;
    uint32_t old_secs = file_secs(&H.files[i]);
    uint64_t secs64 = ((uint64_t)size + 511u) / 512u;
    if (secs64 == 0) secs64 = 1;
    if (secs64 > 0xFFFFFFFFull) return -1;
    uint32_t new_secs = (uint32_t)secs64;

    if (new_secs <= old_secs) {
        if ((uint64_t)old_lba + new_secs > H.capacity) return -1;
        if (storage_write(old_lba, new_secs, data) != 0) return -1;
        H.files[i].size = size;
        if (new_secs < old_secs)
            free_ext_add(old_lba + new_secs, old_secs - new_secs);
        hdr_commit();
        return 0;
    }

    /* Buyuyen yazim: once yerinde genisleme mumkun mu kontrol et */
    uint32_t extra = new_secs - old_secs;
    uint32_t tail_lba = old_lba + old_secs;
    if (tail_lba == H.data_next && (uint64_t)old_lba + new_secs <= H.capacity) {
        if (storage_write(old_lba, new_secs, data) != 0) return -1;
        H.files[i].size = size;
        H.data_next = old_lba + new_secs;
        hdr_commit();
        return 0;
    }
    if (free_ext_consume_at(tail_lba, extra)) {
        if (storage_write(old_lba, new_secs, data) != 0) {
            free_ext_add(tail_lba, extra);
            return -1;
        }
        H.files[i].size = size;
        hdr_commit();
        return 0;
    }

    /* Bos-alan tablosundan veya data_next'ten yeni yer ayir ve eski alani iade et */
    uint32_t new_lba = free_ext_alloc(new_secs);
    if (new_lba == 0) return -1;
    if (storage_write(new_lba, new_secs, data) != 0) {
        free_ext_add(new_lba, new_secs);
        return -1;
    }
    H.files[i].lba = new_lba;
    H.files[i].size = size;
    free_ext_add(old_lba, old_secs);
    hdr_commit();
    return 0;
}

uint32_t afs_read_at(void *h, void *buf, uint32_t size, uint32_t off) {
    int i = hnd_dec(h);
    if (i < 0) return 0;
    if (!perm_ok(H.files[i].uid, H.files[i].gid, H.files[i].mode, AFS_R)) return 0;
    if (off >= H.files[i].size) return 0;
    uint32_t n = H.files[i].size - off;
    if (size < n) n = size;
    if (n == 0) return 0;

    uint8_t sec[512];
    uint8_t *p = (uint8_t *)buf;
    uint32_t pos = off, done = 0;

    uint32_t inoff = pos % 512u;
    if (inoff != 0) {
        uint32_t chunk = 512u - inoff;
        if (chunk > n) chunk = n;
        if (storage_read(H.files[i].lba + pos / 512u, 1, sec) != 0) return 0;
        for (uint32_t k = 0; k < chunk; k++) p[done + k] = sec[inoff + k];
        done += chunk;
        pos += chunk;
    }

    while (n - done >= 512u) {
        uint32_t secs = (n - done) / 512u;
        if (secs > 512u) secs = 512u;
        if (storage_read(H.files[i].lba + pos / 512u, secs, p + done) != 0) break;
        uint32_t bytes = secs * 512u;
        done += bytes;
        pos += bytes;
    }

    if (done < n && (pos % 512u) == 0) {
        uint32_t rem = n - done;
        if (storage_read(H.files[i].lba + pos / 512u, 1, sec) == 0) {
            for (uint32_t k = 0; k < rem; k++) p[done + k] = sec[k];
            done += rem;
        }
    }
    return done;
}

uint32_t afs_read_handle(void *h, void *buf, uint32_t size) {
    return afs_read_at(h, buf, size, 0);
}

int afs_write_path(const char *path, const void *data, uint32_t size) {
    if (!mounted) afs_mount();
    if (!path || !path[0]) return -1;
    void *h = afs_find_handle(path);
    if (!h) h = afs_create_handle(path);
    if (!h) return -1;
    return afs_write_handle(h, data, size);
}

int afs_remove(const char *name) {
    if (!mounted) afs_mount();
    if (!s_hdr || !name || !name[0]) return -1;
    if (seq(name, AFS_LONGNAME_FILE)) return -1;
    int i = find_idx(name);
    if (i < 0) return -1;

    int pi = parent_idx(name);
    if (pi >= 0) {
        if (!perm_ok(H.files[pi].uid, H.files[pi].gid, H.files[pi].mode, AFS_W))
            return -1;
    } else if (!perm_ok(H.files[i].uid, H.files[i].gid, H.files[i].mode, AFS_W)) {
        return -1;
    }

    uint32_t old_lba = H.files[i].lba;
    uint32_t old_secs = file_secs(&H.files[i]);
    int was_long = is_ln_token(H.files[i].name);
    char old_tok[AFS_NAME_MAX];
    if (was_long) cpy(old_tok, H.files[i].name);

    H.files[i].used = 0;
    H.files[i].size = 0;
    H.files[i].lba = 0;
    H.files[i].name[0] = 0;
    s_hgen[i]++;
    if (H.count) H.count--;

    free_ext_add(old_lba, old_secs);
    if (was_long) {
        ln_remove_token(old_tok);
        (void)ln_persist_no_commit();
    }
    hdr_commit();
    return 0;
}

const char *afs_name(int idx) {
    if (!mounted) afs_mount();
    if (!s_hdr || idx < 0 || idx >= (int)H.max_files || !H.files[idx].used) return 0;
    if (seq(H.files[idx].name, AFS_LONGNAME_FILE)) return 0;
    return entry_full_name((uint32_t)idx);
}

uint32_t afs_size(int idx) {
    if (!mounted) afs_mount();
    if (!s_hdr || idx < 0 || idx >= (int)H.max_files || !H.files[idx].used) return 0;
    if (seq(H.files[idx].name, AFS_LONGNAME_FILE)) return 0;
    return H.files[idx].size;
}

int afs_stat(const char *path, asi_stat_t *out) {
    if (!mounted) afs_mount();
    int i = find_idx(path);
    if (i < 0 || !out) return -1;
    out->uid = H.files[i].uid;
    out->gid = H.files[i].gid;
    out->mode = H.files[i].mode;
    out->is_dir = afs_is_dir(H.files[i].name) ? 1u : 0u;
    out->size = H.files[i].size;
    return 0;
}

int afs_stat_handle(void *h, asi_stat_t *out) {
    int i = hnd_dec(h);
    if (i < 0 || !out) return -1;
    out->uid = H.files[i].uid;
    out->gid = H.files[i].gid;
    out->mode = H.files[i].mode;
    out->is_dir = afs_is_dir(H.files[i].name) ? 1u : 0u;
    out->size = H.files[i].size;
    return 0;
}

int afs_chmod(const char *path, uint32_t mode) {
    if (!mounted) afs_mount();
    int i = find_idx(path);
    if (i < 0) return -1;
    if (s_uid != 0 && s_uid != H.files[i].uid) return -1;
    H.files[i].mode = mode & 07777u;
    hdr_commit();
    return 0;
}

int afs_chown(const char *path, int uid, int gid) {
    if (!mounted) afs_mount();
    int i = find_idx(path);
    if (i < 0) return -1;
    if (s_uid != 0) return -1;
    H.files[i].uid = (uint32_t)uid;
    H.files[i].gid = (uint32_t)gid;
    hdr_commit();
    return 0;
}

int afs_perm_selftest(void) {
    if (!mounted) afs_mount();
    afs_set_subject(0, 0);
    afs_remove("afs_perm_test");
    void *h = afs_create_handle("afs_perm_test");
    if (!h) { afs_set_subject(0, 0); return -1; }
    if (afs_write_handle(h, "abc", 3) != 0) { afs_remove("afs_perm_test"); afs_set_subject(0, 0); return -2; }
    if (afs_chmod("afs_perm_test", 0640u) != 0) { afs_remove("afs_perm_test"); afs_set_subject(0, 0); return -3; }

    int owner_ok, other_deny;
    afs_set_subject(1000, 1000);
    other_deny = (afs_write_handle(h, "x", 1) != 0);

    afs_set_subject(0, 0);
    owner_ok = (afs_write_handle(h, "x", 1) == 0);

    int chown_deny;
    afs_set_subject(1000, 1000);
    chown_deny = (afs_chown("afs_perm_test", 1000, 1000) != 0);

    afs_set_subject(0, 0);
    afs_remove("C:/afs_pd/alt.txt");
    afs_remove("C:/afs_pd/");
    if (afs_ensure_dir("C:/afs_pd") != 0) { afs_set_subject(0, 0); return -5; }
    if (afs_chmod("C:/afs_pd/", 0755u) != 0) { afs_set_subject(0, 0); return -6; }
    afs_set_subject(1000, 1000);
    int mk_deny = (afs_create_handle("C:/afs_pd/alt.txt") == 0);
    afs_set_subject(0, 0);
    if (afs_chmod("C:/afs_pd/", 0777u) != 0) { afs_set_subject(0, 0); return -7; }
    afs_set_subject(1000, 1000);
    void *h2 = afs_create_handle("C:/afs_pd/alt.txt");
    int mk_ok = (h2 != 0);
    afs_set_subject(0, 0);
    if (h2) afs_remove("C:/afs_pd/alt.txt");
    afs_remove("C:/afs_pd/");

    afs_remove("afs_perm_test");
    return (owner_ok && other_deny && chown_deny && mk_deny && mk_ok) ? 0 : -4;
}

int afs_fsck(int fix, int *problems, int *fixed) {
    if (!mounted) afs_mount();
    if (!s_hdr) return -1;
    int prob = 0, fixd = 0;
    uint32_t data_start = s_base + H.hdr_sectors;
    uint32_t max_end = data_start;
    uint32_t used = 0;

    for (uint32_t i = 0; i < H.max_files; i++) {
        afs_file_t *f = &H.files[i];
        if (!f->used) continue;
        used++;
        int isd = afs_is_dir(f->name);
        int bad = 0;
        if (f->name[0] == 0) {
            bad = 1;
        } else if (is_ln_token(f->name) && ln_find_by_token(f->name) < 0) {
            bad = 1;
        } else if (!isd) {
            uint32_t secs = file_secs(f);
            if (f->lba < data_start || f->lba >= H.capacity ||
                secs > H.capacity - f->lba) bad = 1;
            else if (f->lba + secs > max_end) max_end = f->lba + secs;
        } else {
            uint32_t secs = file_secs(f);
            if (f->lba >= data_start && f->lba < H.capacity &&
                secs <= H.capacity - f->lba && f->lba + secs > max_end)
                max_end = f->lba + secs;
        }
        if (bad) {
            prob++;
            if (fix) {
                f->used = 0; f->name[0] = 0; f->size = 0; f->lba = 0;
                used--; fixd++;
            }
        }
    }

    if (H.hdr_sectors == 0 || H.hdr_sectors > AFS_HDR_MAX_SEC) {
        prob++; if (fix) { hdr_sync_size(); fixd++; }
    }
    if (H.count != used) { prob++; if (fix) { H.count = used; fixd++; } }
    if (H.data_next < max_end) { prob++; if (fix) { H.data_next = max_end; fixd++; } }
    if (H.data_next > H.capacity) { prob++; if (fix) { H.data_next = (uint32_t)H.capacity; fixd++; } }

    if (fix && prob) {
        hdr_commit();
        free_ext_rebuild();
    }
    if (problems) *problems = prob;
    if (fixed) *fixed = fixd;
    return prob ? 1 : 0;
}

/* ---- Free-extent, uzun yol ve yuksek kapasite oz-testleri ---- */

int afs_reclaim_selftest(void) {
    if (!mounted) afs_mount();
    if (!s_hdr) return -1;
    afs_set_subject(0, 0);

    afs_reclaim_stats_t st0;
    afs_free_extent_stats(&st0);
    uint32_t base_dn = H.data_next;

    /* 1) Sira disi silme + birlestirme (coalesce) + kuyruk kirpma (tail trim) */
    uint8_t blk4k[4096];
    for (int i = 0; i < 4096; i++) blk4k[i] = (uint8_t)(i ^ 0x5Au);

    void *ha = afs_create_handle("C:/recl_a.bin");
    if (!ha || afs_write_handle(ha, blk4k, sizeof(blk4k)) != 0) { afs_remove("C:/recl_a.bin"); return -2; }
    void *hb = afs_create_handle("C:/recl_b.bin");
    if (!hb || afs_write_handle(hb, blk4k, sizeof(blk4k)) != 0) { afs_remove("C:/recl_a.bin"); afs_remove("C:/recl_b.bin"); return -2; }
    void *hc = afs_create_handle("C:/recl_c.bin");
    if (!hc || afs_write_handle(hc, blk4k, sizeof(blk4k)) != 0) { afs_remove("C:/recl_a.bin"); afs_remove("C:/recl_b.bin"); afs_remove("C:/recl_c.bin"); return -2; }
    void *hd = afs_create_handle("C:/recl_d.bin");
    if (!hd || afs_write_handle(hd, blk4k, sizeof(blk4k)) != 0) {
        afs_remove("C:/recl_a.bin"); afs_remove("C:/recl_b.bin");
        afs_remove("C:/recl_c.bin"); afs_remove("C:/recl_d.bin");
        return -3;
    }

    /* A ve C sil -> 2 ayrik delik; B sil -> A+B+C tek delige birlesmeli */
    uint32_t ext_before = s_free_count;
    afs_remove("C:/recl_a.bin");
    afs_remove("C:/recl_c.bin");
    if (s_free_count != ext_before + 2u) {
        afs_remove("C:/recl_b.bin"); afs_remove("C:/recl_d.bin");
        return -4;
    }
    afs_remove("C:/recl_b.bin");
    if (s_free_count != ext_before + 1u) {
        afs_remove("C:/recl_d.bin");
        return -5;
    }

    /* 12 KB dosya E olustur -> birlesen A+B+C deligini kullanmali (data_next artmaz) */
    uint8_t blk12k[12288];
    for (int i = 0; i < 12288; i++) blk12k[i] = (uint8_t)(i + 7);
    uint32_t dn_before_e = H.data_next;
    void *he = afs_create_handle("C:/recl_e.bin");
    if (!he || afs_write_handle(he, blk12k, sizeof(blk12k)) != 0) {
        afs_remove("C:/recl_d.bin"); afs_remove("C:/recl_e.bin");
        return -6;
    }
    if (H.data_next != dn_before_e) {
        afs_remove("C:/recl_d.bin"); afs_remove("C:/recl_e.bin");
        return -7;
    }

    /* D ve E'yi sil -> kuyruk kirpma data_next'i baslangic degerine indirmeli */
    afs_remove("C:/recl_d.bin");
    afs_remove("C:/recl_e.bin");
    if (H.data_next != base_dn) return -8;

    /* 2) Uzerine yazma / kirpma geri kazanimi */
    void *ht = afs_create_handle("C:/recl_trunc.bin");
    if (!ht || afs_write_handle(ht, blk12k, sizeof(blk12k)) != 0) {
        afs_remove("C:/recl_trunc.bin");
        return -9;
    }
    uint32_t dn_12k = H.data_next;
    if (afs_write_handle(ht, blk4k, sizeof(blk4k)) != 0) {
        afs_remove("C:/recl_trunc.bin");
        return -10;
    }
    if (H.data_next >= dn_12k) {
        afs_remove("C:/recl_trunc.bin");
        return -11;
    }
    afs_remove("C:/recl_trunc.bin");
    if (H.data_next != base_dn) return -12;

    /* 3) 20x 2 MB olustur + yaz + sil dongusu (sifir sizinti dogrulamasi) */
    const uint32_t big_sz = 2u * 1024u * 1024u;
    uint32_t big_secs = big_sz / 512u;
    if ((uint64_t)H.data_next + big_secs + 64u <= H.capacity) {
        uint8_t *big = (uint8_t *)kmalloc(big_sz);
        if (!big) return -13;
        for (uint32_t k = 0; k < big_sz; k += 512u) {
            big[k] = (uint8_t)(k >> 9);
            big[k + 511u] = (uint8_t)~(k >> 9);
        }
        for (int iter = 0; iter < 20; iter++) {
            void *hf = afs_create_handle("C:/recl_2mb.bin");
            if (!hf || afs_write_handle(hf, big, big_sz) != 0) {
                afs_remove("C:/recl_2mb.bin");
                kfree(big);
                return -14;
            }
            if (afs_remove("C:/recl_2mb.bin") != 0) {
                kfree(big);
                return -15;
            }
            if (H.data_next != base_dn) {
                kfree(big);
                return -16;
            }
        }
        kfree(big);
    }

    afs_reclaim_stats_t st1;
    afs_free_extent_stats(&st1);
    serial_printf("[afs] free_extents=%u reclaimed=%u reused=%u data_next=%u->%u (20x2MB OK)\n",
                  st1.free_extents, st1.reclaimed_sectors - st0.reclaimed_sectors,
                  st1.reused_sectors - st0.reused_sectors, base_dn, H.data_next);
    return 0;
}

static int afs_longpath_selftest(void) {
    if (!mounted) afs_mount();
    if (!s_hdr) return -1;
    afs_set_subject(0, 0);
    uint32_t base_dn = H.data_next;

    const char *ldir =
        "C:/long_directory_segment_alpha_0123456789_beta_0123456789_gamma_0123456789";
    if (afs_ensure_dir(ldir) != 0) return -1;

    char p200[AFS_PATH_MAX];
    int o = 0;
    for (int i = 0; ldir[i]; i++) p200[o++] = ldir[i];
    p200[o++] = '/';
    const char *sub = "nested_long_folder_level_two_0123456789_abcdef/";
    for (int i = 0; sub[i]; i++) p200[o++] = sub[i];
    p200[o] = '\0';
    if (afs_ensure_dir(p200) != 0) { afs_remove(ldir); return -2; }

    /* ~200B ve 255B uzun dosya yollari olustur */
    char f200[AFS_PATH_MAX];
    cpy_cap(f200, p200, (int)sizeof(f200));
    int fo = slen(f200);
    while (fo < 196) { f200[fo] = (char)('a' + (fo % 26)); fo++; }
    f200[fo++] = '.'; f200[fo++] = 't'; f200[fo++] = 'x'; f200[fo++] = 't'; f200[fo] = '\0';

    char f255[AFS_PATH_MAX];
    cpy_cap(f255, p200, (int)sizeof(f255));
    int f2 = slen(f255);
    while (f2 < 251) { f255[f2] = (char)('A' + (f2 % 26)); f2++; }
    f255[f2++] = '.'; f255[f2++] = 'b'; f255[f2++] = 'i'; f255[f2++] = 'n'; f255[f2] = '\0';

    const char *msg200 = "longpath-200B-payload";
    const char *msg255 = "longpath-255B-payload-ok";
    if (afs_write_path(f200, msg200, (uint32_t)slen(msg200)) != 0 ||
        afs_write_path(f255, msg255, (uint32_t)slen(msg255)) != 0) {
        afs_remove(f200); afs_remove(f255); afs_remove(p200); afs_remove(ldir);
        return -3;
    }

    char rbuf[64];
    zero_mem(rbuf, sizeof(rbuf));
    uint32_t rn1 = afs_read_path(f200, rbuf, sizeof(rbuf) - 1);
    if (rn1 != (uint32_t)slen(msg200) || !seq(rbuf, msg200)) {
        afs_remove(f200); afs_remove(f255); afs_remove(p200); afs_remove(ldir);
        return -4;
    }
    zero_mem(rbuf, sizeof(rbuf));
    uint32_t rn2 = afs_read_path(f255, rbuf, sizeof(rbuf) - 1);
    if (rn2 != (uint32_t)slen(msg255) || !seq(rbuf, msg255)) {
        afs_remove(f200); afs_remove(f255); afs_remove(p200); afs_remove(ldir);
        return -5;
    }

    /* list_dir + .afs_longnames gizliligi kontrolu */
    int dc = afs_count_dir(p200);
    if (dc != 2) {
        afs_remove(f200); afs_remove(f255); afs_remove(p200); afs_remove(ldir);
        return -6;
    }
    char lname[AFS_PATH_MAX];
    int isd = 0;
    if (afs_list_dir(p200, 0, lname, sizeof(lname), &isd) != 0 || isd != 0) {
        afs_remove(f200); afs_remove(f255); afs_remove(p200); afs_remove(ldir);
        return -7;
    }
    /* Kok dizinde .afs_longnames gizli olmali */
    int rc_root = afs_count_dir("C:/");
    for (int k = 0; k < rc_root; k++) {
        char rn[AFS_PATH_MAX];
        if (afs_list_dir("C:/", k, rn, sizeof(rn), &isd) == 0 && seq(rn, AFS_LONGNAME_FILE)) {
            afs_remove(f200); afs_remove(f255); afs_remove(p200); afs_remove(ldir);
            return -8;
        }
    }

    /* Rename testi (200B -> kisa yol -> sil) */
    if (afs_rename(f200, "C:/renamed_short.txt") != 0) {
        afs_remove(f200); afs_remove(f255); afs_remove(p200); afs_remove(ldir);
        return -9;
    }
    zero_mem(rbuf, sizeof(rbuf));
    if (afs_read_path("C:/renamed_short.txt", rbuf, sizeof(rbuf) - 1) != (uint32_t)slen(msg200) ||
        !seq(rbuf, msg200)) {
        afs_remove("C:/renamed_short.txt"); afs_remove(f255); afs_remove(p200); afs_remove(ldir);
        return -10;
    }

    afs_remove("C:/renamed_short.txt");
    afs_remove(f255);
    afs_remove(p200);
    char ldir_slash[AFS_PATH_MAX];
    cpy_cap(ldir_slash, ldir, (int)sizeof(ldir_slash));
    int ls = slen(ldir_slash);
    ldir_slash[ls++] = '/'; ldir_slash[ls] = '\0';
    afs_remove(ldir_slash);

    if (H.data_next != base_dn || s_ln_count != 0) return -11;
    serial_printf("[afs] longpath selftest (200B + 255B + rename + hidden .afs_longnames): OK\n");
    return 0;
}

static int afs_capacity_selftest(void) {
    if (!mounted) afs_mount();
    if (!s_hdr) return -1;
    if (H.max_files < 1024u) {
        serial_printf("[afs] capacity selftest: max_files=%u (<1024) SKIP\n", H.max_files);
        return 0;
    }
    afs_set_subject(0, 0);
    uint32_t base_dn = H.data_next;
    uint32_t base_cnt = H.count;

    const int target = 550;   /* eski 512 sinirinin ustu */
    char path[64];
    for (int i = 0; i < target; i++) {
        /* "C:/cap_000" */
        path[0] = 'C'; path[1] = ':'; path[2] = '/';
        path[3] = 'c'; path[4] = 'a'; path[5] = 'p'; path[6] = '_';
        path[7] = (char)('0' + (i / 100) % 10);
        path[8] = (char)('0' + (i / 10) % 10);
        path[9] = (char)('0' + (i % 10));
        path[10] = '\0';
        void *h = afs_create_handle(path);
        if (!h) {
            for (int k = 0; k < i; k++) {
                path[7] = (char)('0' + (k / 100) % 10);
                path[8] = (char)('0' + (k / 10) % 10);
                path[9] = (char)('0' + (k % 10));
                afs_remove(path);
            }
            return -2;
        }
    }
    int ok_cnt = (H.count == base_cnt + (uint32_t)target);
    for (int i = 0; i < target; i++) {
        path[0] = 'C'; path[1] = ':'; path[2] = '/';
        path[3] = 'c'; path[4] = 'a'; path[5] = 'p'; path[6] = '_';
        path[7] = (char)('0' + (i / 100) % 10);
        path[8] = (char)('0' + (i / 10) % 10);
        path[9] = (char)('0' + (i % 10));
        path[10] = '\0';
        afs_remove(path);
    }
    if (!ok_cnt || H.count != base_cnt || H.data_next != base_dn) return -3;
    serial_printf("[afs] capacity selftest: max_files=%u created=%d reclaimed data_next=%u -> OK\n",
                  H.max_files, target, H.data_next);
    return 0;
}

int afs_journal_selftest(void) {
    if (!mounted) afs_mount();
    if (!s_hdr) return -1;
    uint32_t jl = journal_lba();
    if (jl == 0) return -1;

    afs_set_subject(0, 0);
    afs_remove("afs_jrn_test");
    afs_remove("afs_jrn_replay");

    uint32_t saved_count = H.count;
    uint32_t saved_dn = H.data_next;

    void *h = afs_create_handle("afs_jrn_test");
    if (!h) return -2;
    if (afs_write_handle(h, "journal-data", 12) != 0) {
        afs_remove("afs_jrn_test");
        return -3;
    }

    if (H.flags == 0 || hdr_crc() != H.flags) {
        afs_remove("afs_jrn_test");
        return -4;
    }
    uint32_t disk_crc = H.flags;

    int idx = -1;
    for (uint32_t i = 0; i < H.max_files; i++)
        if (!H.files[i].used) { idx = (int)i; break; }
    if (idx < 0) { afs_remove("afs_jrn_test"); return -5; }

    H.files[idx].used = 1;
    H.files[idx].size = 0;
    H.files[idx].lba = H.data_next;
    set_owner(&H.files[idx], 0);
    cpy(H.files[idx].name, "afs_jrn_replay");
    H.count++;
    H.version = AFS_VERSION;
    hdr_sync_size();
    H.flags = 0; H.flags = hdr_crc();

    int ok = 0;
    uint8_t sec[512];
    afs_jrec_t *rec = (afs_jrec_t *)sec;
    for (int i = 0; i < 512; i++) sec[i] = 0;
    rec->magic = AFS_JOURNAL_MAGIC;
    rec->seq = ++s_jseq;
    rec->hdr_sectors = H.hdr_sectors;
    rec->crc = crc32_buf(s_hdr, hdr_bytes());
    rec->state = 1;
    rec->prev_crc = disk_crc;
    if (hdr_save_at(jl + 1u, H.hdr_sectors) == 0) {
        storage_flush();
        if (storage_write(jl, 1, sec) == 0) {
            storage_flush();
            hdr_load_at(s_base, H.hdr_sectors);
            journal_replay();
            ok = (find_idx("afs_jrn_replay") >= 0);
        }
    }

    for (uint32_t i = 0; i < H.max_files; i++) {
        afs_file_t *f = &H.files[i];
        if (!f->used) continue;
        if (seq(f->name, "afs_jrn_replay") || seq(f->name, "afs_jrn_test")) {
            f->used = 0; f->name[0] = 0; f->size = 0; f->lba = 0;
            s_hgen[i]++;
        }
    }
    H.count = saved_count;
    H.data_next = saved_dn;
    hdr_commit();
    free_ext_rebuild();

    int clean = 0;
    { int p = 0, f = 0; clean = (afs_fsck(0, &p, &f) == 0 && p == 0); }
    if (!ok || !clean) return ok ? -7 : -6;

#ifdef ARCTIAN_AFS_SELFTEST
    {
        int rr = afs_reclaim_selftest();
        if (rr != 0) {
            serial_printf("[afs] reclaim selftest HATA kod=%d\n", rr);
            return -10;
        }
        int lr = afs_longpath_selftest();
        if (lr != 0) {
            serial_printf("[afs] longpath selftest HATA kod=%d\n", lr);
            return -11;
        }
        int cr = afs_capacity_selftest();
        if (cr != 0) {
            serial_printf("[afs] capacity selftest HATA kod=%d\n", cr);
            return -12;
        }
    }
#endif
    return 0;
}

/* ---- Klasor (yol tabanli) katman ---- */

int afs_is_dir(const char *name) {
    if (!name) return 0;
    int n = 0; while (name[n]) n++;
    return n > 0 && name[n - 1] == '/';
}

static int pref_match(const char *nm, const char *p, int pl) {
    for (int i = 0; i < pl; i++) if (nm[i] != p[i]) return 0;
    return 1;
}

static int direct_child(const char *rest) {
    if (!rest || !rest[0]) return 0;
    for (int i = 0; rest[i]; i++) {
        if (rest[i] == '/') return rest[i + 1] == 0;
    }
    return 1;
}

static int dir_prefix(const char *path, char *prefix, int cap) {
    if (!path || !path[0] || cap < 3) return 0;
    int o = 0;
    for (int i = 0; path[i] && o < cap - 1; i++) prefix[o++] = path[i];
    if (o > 0 && prefix[o - 1] != '/') prefix[o++] = '/';
    prefix[o] = 0;
    return o;
}

static int path_join(char *dst, int cap, const char *dir, const char *name, int is_dir) {
    int o = 0;
    if (dir && dir[0]) {
        for (int i = 0; dir[i]; i++) {
            if (o >= cap - 1) return -1;
            dst[o++] = dir[i];
        }
        if (o > 0 && dst[o - 1] != '/') {
            if (o >= cap - 1) return -1;
            dst[o++] = '/';
        }
    }
    if (name) {
        for (int i = 0; name[i]; i++) {
            if (o >= cap - 1) return -1;
            dst[o++] = name[i];
        }
    }
    if (is_dir && o > 0 && dst[o - 1] != '/') {
        if (o >= cap - 1) return -1;
        dst[o++] = '/';
    }
    dst[o] = 0;
    return 0;
}

int afs_ensure_dir(const char *path) {
    if (!path || !path[0]) return -1;
    if (!mounted) afs_mount();
    if (slen(path) >= (int)AFS_PATH_MAX - 1) return -1;
    char buf[AFS_PATH_MAX];
    int n = 0;
    for (int i = 0; path[i] && n < (int)sizeof(buf) - 1; i++) buf[n++] = path[i];
    if (n > 0 && buf[n - 1] != '/' && n < (int)sizeof(buf) - 1) buf[n++] = '/';
    buf[n] = 0;
    for (int k = 1; k < n; k++) {
        if (buf[k] == '/') {
            char save = buf[k + 1];
            buf[k + 1] = 0;
            afs_create_handle(buf);
            buf[k + 1] = save;
        }
    }
    return 0;
}

static void migrate_user_ownership(void) {
    static const char *skip[] = { "C:/etc/", "C:/firmware/" };
    int changed = 0;
    for (uint32_t i = 0; i < H.max_files; i++) {
        if (!H.files[i].used) continue;
        if (H.files[i].uid != 0 || H.files[i].gid != 0) continue;
        const char *nm = entry_full_name(i);
        if (nm[0] == 0 || !prefix_of(nm, AFS_USER_ROOT)) continue;
        int skip_this = 0;
        for (unsigned k = 0; k < sizeof(skip) / sizeof(skip[0]); k++)
            if (prefix_of(nm, skip[k])) { skip_this = 1; break; }
        if (skip_this) continue;
        H.files[i].uid = 1000;
        H.files[i].gid = 1000;
        changed = 1;
    }
    if (changed) hdr_commit();
}

static void afs_ensure_tree(void) {
    afs_ensure_dir(AFS_USER_ROOT);
    afs_ensure_dir(AFS_USER_APPS);
    afs_ensure_dir(AFS_USER_DOWNLOAD);
    afs_ensure_dir(AFS_USER_DOCS);
    afs_ensure_dir(AFS_USER_DESKTOP);
    afs_ensure_dir(AFS_USER_LOGS);
    migrate_user_ownership();
}

void *afs_create_in(const char *path, const char *name, int is_dir) {
    if (!mounted) afs_mount();
    char full[AFS_PATH_MAX];
    if (path_join(full, sizeof(full), path, name, is_dir) != 0) return 0;
    if (!full[0]) return 0;
    return afs_create_handle(full);
}

void *afs_find_in(const char *path, const char *name) {
    if (!mounted) afs_mount();
    char full[AFS_PATH_MAX];
    if (path_join(full, sizeof(full), path, name, 0) != 0) return 0;
    if (!full[0]) return 0;
    return afs_find_handle(full);
}

uint32_t afs_read_path(const char *path, void *buf, uint32_t size) {
    void *h = afs_find_handle(path);
    if (!h) return 0;
    return afs_read_handle(h, buf, size);
}

int afs_count_dir(const char *path) {
    if (!mounted) afs_mount();
    if (!s_hdr) return 0;
    char p[AFS_PATH_MAX];
    int pl = dir_prefix(path, p, sizeof(p));
    if (pl <= 0) return 0;
    int c = 0;
    for (uint32_t i = 0; i < H.max_files; i++) {
        if (!H.files[i].used) continue;
        if (seq(H.files[i].name, AFS_LONGNAME_FILE)) continue;
        const char *nm = entry_full_name(i);
        if (!pref_match(nm, p, pl)) continue;
        if (direct_child(nm + pl)) c++;
    }
    return c;
}

int afs_list_dir(const char *path, int idx, char *out, int cap, int *is_dir) {
    if (!mounted) afs_mount();
    if (!s_hdr) return -1;
    char p[AFS_PATH_MAX];
    int pl = dir_prefix(path, p, sizeof(p));
    if (pl <= 0) return -1;
    int c = 0;
    for (uint32_t i = 0; i < H.max_files; i++) {
        if (!H.files[i].used) continue;
        if (seq(H.files[i].name, AFS_LONGNAME_FILE)) continue;
        const char *nm = entry_full_name(i);
        if (!pref_match(nm, p, pl)) continue;
        const char *rest = nm + pl;
        if (!direct_child(rest)) continue;
        if (c == idx) {
            int rl = 0; while (rest[rl]) rl++;
            int isd = (rl > 0 && rest[rl - 1] == '/');
            int copy = isd ? rl - 1 : rl;
            if (copy > cap - 1) copy = cap - 1;
            if (out) { for (int k = 0; k < copy; k++) out[k] = rest[k]; out[copy] = 0; }
            if (is_dir) *is_dir = isd;
            return 0;
        }
        c++;
    }
    return -1;
}

/* ---- ASI sarmalayicilari ---- */
static void    *w_root(void) { return 0; }
static void    *w_create(void *d, const char *n, int isdir) { (void)d; (void)isdir; return afs_create_handle(n); }
static void    *w_find(void *d, const char *n) { (void)d; return afs_find_handle(n); }
static int      w_write(void *f, const void *data, uint32_t sz) { return afs_write_handle(f, data, sz); }
static uint32_t w_read(void *f, void *buf, uint32_t sz) { return afs_read_handle(f, buf, sz); }
static int      w_count(void) { return afs_count(); }
static const char *w_name(int idx) { return afs_name(idx); }
static uint32_t w_size(int idx) { return afs_size(idx); }
static int      w_remove(const char *n) { return afs_remove(n); }
static int      w_mkdir(const char *path) { return afs_ensure_dir(path); }
static void    *w_create_in(const char *path, const char *n, int isdir) { return afs_create_in(path, n, isdir); }
static void    *w_find_in(const char *path, const char *n) { return afs_find_in(path, n); }
static uint32_t w_read_path(const char *path, void *buf, uint32_t sz) { return afs_read_path(path, buf, sz); }
static int      w_count_dir(const char *path) { return afs_count_dir(path); }
static int      w_list_dir(const char *path, int idx, char *out, int cap, int *isdir) { return afs_list_dir(path, idx, out, cap, isdir); }
static int      w_stat(const char *path, asi_stat_t *out) { return afs_stat(path, out); }
static int      w_stat_handle(void *f, asi_stat_t *out) { return afs_stat_handle(f, out); }
static int      w_chmod(const char *path, uint32_t mode) { return afs_chmod(path, mode); }
static int      w_chown(const char *path, int uid, int gid) { return afs_chown(path, uid, gid); }
static void     w_set_subject(int uid, int gid) { afs_set_subject(uid, gid); }
static int      w_fsck(int fix, int *problems, int *fixed) { return afs_fsck(fix, problems, fixed); }
static int      w_rename(const char *from, const char *to) { return afs_rename(from, to); }

asi_fs_t *afs_asi(void) {
    static asi_fs_t fs;
    fs.root = w_root;
    fs.create = w_create;
    fs.find = w_find;
    fs.write = w_write;
    fs.read = w_read;
    fs.count = w_count;
    fs.name = w_name;
    fs.size = w_size;
    fs.remove = w_remove;
    fs.mkdir = w_mkdir;
    fs.create_in = w_create_in;
    fs.find_in = w_find_in;
    fs.read_path = w_read_path;
    fs.count_dir = w_count_dir;
    fs.list_dir = w_list_dir;
    fs.stat = w_stat;
    fs.stat_handle = w_stat_handle;
    fs.chmod = w_chmod;
    fs.chown = w_chown;
    fs.set_subject = w_set_subject;
    fs.fsck = w_fsck;
    fs.rename = w_rename;
    return &fs;
}
