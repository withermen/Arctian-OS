/**
 * ArctianFS (AFS) v7 + geriye donuk v6/v5 uyumlulugu
 *
 * Ozellikler:
 *  - AFS v7: 65.536 dosyaya kadar dinamik parcali inode tablosu (256'lik
 *    bellege-talep-edildikce kmalloc bloklari; statik BSS dizisi YOK) +
 *    dolayli inode-blok dizini (diskte de yalnizca gereken bloklar ayrilir)
 *  - Coklu-extent dosya depolama: inode basina 5 satiriici (inline) extent +
 *    parcalanmis disklerde sinirsiz extent zinciri (1 sektorluk dolayli blok).
 *    Tek bitisik extent yettigi surece sifir ek yukle hizli yol korunur.
 *  - Tam v6 on-disk geriye donuk uyumlulugu (magic 0x53464121, ver 6, 92B girdi,
 *    mevcut disk_final_6g.img sorunsuz mount/okuma/yazma/fsck)
 *  - 256-sektor metadata gunlugu (hem v6 hem v7 icin atomik kayit)
 *  - Dinamik buyuyen sirali bos-alan (free-extent) tablosu + birlestirme + tail trim
 *  - 255 bayta kadar uzun yol destegi (.afs_longnames)
 *  - Gercek uid/gid/mode izin denetimi (uid=0 root tam erisim; uid!=0 izolasyon)
 */
#pragma GCC optimize("Os")
#include "afs.h"
#include "storage.h"
#include "diskcrypt_hdr.h"
#include "alloc.h"
#include "serial.h"
#include <stddef.h>

#define AFS_MAGIC            0x53464121u  /* "!AFS" */
#define AFS_V6_MAX_FILES     1024u
#define AFS_HEADER_BASE      36u          /* magic..flags + 64 bit kapasite */
#define AFS_ENTRY_SIZE       (AFS_NAME_MAX + 28u)  /* 92 byte */
#define AFS_HDR_MAX_SEC \
    ((AFS_HEADER_BASE + AFS_V6_MAX_FILES * AFS_ENTRY_SIZE + 511u) / 512u)
#define AFS_CLUSTER_SEC      2048u
#define AFS_RESERVE_SEC      2048u        /* diskin sonunda birakilan emniyet alani */

#define AFS_JOURNAL_MAGIC    0x4A464121u  /* "!AFJ" */
#define AFS_JOURNAL_SEC      256u

#define AFS_LONGNAME_FILE    ".afs_longnames"
#define AFS_LN_MAGIC         0x4E4C4641u  /* "AFLN" */
#define AFS_LN_VERSION       1u

/* ---- AFS v7 sabitleri ---- */
#define AFS_V7_CHUNK_ENTRIES 256u
#define AFS_V7_MAX_CHUNKS    256u         /* 256 * 256 = 65536 */
#define AFS_V7_CHUNK_SECS    64u          /* 256 * 128B / 512B */
#define AFS_V7_DIR_SECS      2u           /* 256 * 4B = 1024B */
#define AFS_V7_HDR_SECS      (1u + AFS_V7_DIR_SECS) /* superblock + inode_dir */
#define AFS_V7_INLINE_EXTS   5u
#define AFS_V7_IND_EXTS      62u
#define AFS_V7_EXT_MAGIC     0x37545845u  /* "EXT7" */
#define AFS_V7_JRN_MAX_ENTS  120u

#define AFS_FREE_EXT_INIT    1024u
#define AFS_FREE_EXT_MAX     131072u
#define AFS_LN_INIT_CAP      256u

/* ---- v6 disk yapilari ---- */
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
    afs_file_t files[AFS_V6_MAX_FILES];
} afs_hdr_t;

/* ---- v7 disk yapilari ---- */
typedef struct __attribute__((packed)) {
    uint32_t lba;
    uint32_t sectors;
} afs_v7_ext_t;

typedef struct __attribute__((packed)) {
    char         name[AFS_NAME_MAX];
    uint32_t     size;
    uint8_t      used;
    uint8_t      flags;         /* bit0 = is_dir */
    uint16_t     ext_count;     /* toplam extent sayisi (inline + dolayli) */
    uint32_t     uid;
    uint32_t     gid;
    uint32_t     mode;
    afs_v7_ext_t extents[AFS_V7_INLINE_EXTS];
    uint32_t     ext_ind_lba;   /* >5 extent ise ilk dolayli blok LBA'si */
} afs_v7_inode_t;

typedef struct __attribute__((packed)) {
    uint32_t     magic;         /* AFS_V7_EXT_MAGIC */
    uint32_t     count;         /* bu bloktaki extent sayisi (0..62) */
    afs_v7_ext_t extents[AFS_V7_IND_EXTS];
    uint32_t     next_lba;      /* zincirdeki sonraki blok (0 = son) */
    uint32_t     crc32;         /* 0..507 bayt CRC32 */
} afs_v7_ext_blk_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;             /* AFS_MAGIC */
    uint32_t version;           /* 7 */
    uint32_t max_files;         /* 65536 */
    uint32_t count;
    uint32_t data_next;
    uint32_t hdr_sectors;       /* 3 */
    uint64_t capacity;
    uint32_t flags;             /* superblock CRC32 (flags=0 iken) */
    uint32_t inode_dir_lba;     /* s_base + 1 */
    uint32_t inode_dir_secs;    /* 2 */
    uint32_t inode_blk_secs;    /* 64 */
    uint32_t chunk_cap;         /* 256 */
    uint32_t num_chunks;        /* 1..256 */
    uint32_t journal_seq;
    uint32_t inode_dir_crc;
    uint8_t  reserved[512u - 64u];
} afs_v7_sb_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;           /* 7 */
    uint32_t seq;
    uint32_t count;             /* staged sektor sayisi (<= 120) */
    uint32_t crc;               /* staged payload CRC32 */
    uint32_t state;             /* 1 = replay bekliyor, 0 = temiz */
    uint32_t sb_crc;
    uint32_t pad;
    uint32_t lbas[AFS_V7_JRN_MAX_ENTS];
} afs_v7_jrec_t;

typedef struct {
    afs_v7_inode_t inodes[AFS_V7_CHUNK_ENTRIES];
    uint32_t       gen[AFS_V7_CHUNK_ENTRIES];
    uint64_t       dirty_mask;  /* 64 sektor icin kirli bit maskesi */
} afs_v7_chunk_t;

/* ---- Ortak yapilar ---- */
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
_Static_assert(AFS_HDR_MAX_SEC + 1u <= AFS_JOURNAL_SEC, "v6 header must fit inside journal");
_Static_assert(sizeof(afs_v7_inode_t) == 128u, "afs_v7_inode_t must be 128 bytes");
_Static_assert(sizeof(afs_v7_ext_blk_t) == 512u, "afs_v7_ext_blk_t must be 512 bytes");
_Static_assert(sizeof(afs_v7_sb_t) == 512u, "afs_v7_sb_t must be 512 bytes");
_Static_assert(sizeof(afs_v7_jrec_t) == 512u, "afs_v7_jrec_t must be 512 bytes");

/* Tum buyuk tablolar kmalloc() ile ayrilir; statik BSS boyutu ~4 KB kalir. */
static int             s_ver = 0;         /* 0 = bagli degil, 6 = v6, 7 = v7 */

/* v6 durumu */
static afs_hdr_t      *s_hdr = 0;
#define H              (*s_hdr)
static uint8_t        *s_hbuf = 0;
static uint32_t       *s_hgen = 0;

/* v7 durumu */
static afs_v7_sb_t     s_v7_sb;
static uint32_t       *s_v7_chunk_lba = 0; /* 256 * 4 = 1 KB (kmalloc) */
static afs_v7_chunk_t **s_v7_chunks = 0;   /* 256 * 8 = 2 KB (kmalloc) */
static uint16_t       *s_v7_chunk_used = 0;/* 256 * 2 = 512 B (kmalloc) */
static int             s_v7_dir_dirty = 0;

/* Ortak bos-alan ve uzun-yol tablolari (dinamik buyuyen) */
static afs_ext_t      *s_free_exts = 0;
static uint32_t        s_free_cap = 0;
static uint32_t        s_free_count = 0;
static uint32_t        s_reclaimed_secs = 0;
static uint32_t        s_reused_secs = 0;

static afs_ln_ent_t   *s_ln_tab = 0;
static uint32_t        s_ln_cap = 0;
static uint32_t        s_ln_count = 0;

static int             mounted = 0;
static int             s_locked = 0;
static uint32_t        s_base = AFS_BASE_LBA;
static uint32_t        s_uid = 0;
static uint32_t        s_gid = 0;
static uint32_t        s_jseq = 0;
static uint32_t        s_crc32_tab[256];
static int             s_crc32_ready = 0;

static void zero_mem(void *p, size_t n) {
    uint8_t *b = (uint8_t *)p;
    for (size_t i = 0; i < n; i++) b[i] = 0;
}

static void copy_mem(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
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

static int afs_ensure_common_mem(void) {
    if (!s_free_exts) {
        s_free_exts = (afs_ext_t *)kmalloc((size_t)AFS_FREE_EXT_INIT * sizeof(afs_ext_t));
        if (!s_free_exts) return -1;
        zero_mem(s_free_exts, (size_t)AFS_FREE_EXT_INIT * sizeof(afs_ext_t));
        s_free_cap = AFS_FREE_EXT_INIT;
    }
    if (!s_ln_tab) {
        s_ln_tab = (afs_ln_ent_t *)kmalloc((size_t)AFS_LN_INIT_CAP * sizeof(afs_ln_ent_t));
        if (!s_ln_tab) return -1;
        zero_mem(s_ln_tab, (size_t)AFS_LN_INIT_CAP * sizeof(afs_ln_ent_t));
        s_ln_cap = AFS_LN_INIT_CAP;
    }
    return 0;
}

static int afs_ensure_v6_mem(void) {
    if (afs_ensure_common_mem() != 0) return -1;
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
        s_hgen = (uint32_t *)kmalloc((size_t)AFS_V6_MAX_FILES * sizeof(uint32_t));
        if (!s_hgen) return -1;
        zero_mem(s_hgen, (size_t)AFS_V6_MAX_FILES * sizeof(uint32_t));
    }
    return 0;
}

static int afs_ensure_v7_mem(void) {
    if (afs_ensure_common_mem() != 0) return -1;
    if (!s_v7_chunk_lba) {
        s_v7_chunk_lba = (uint32_t *)kmalloc((size_t)AFS_V7_MAX_CHUNKS * sizeof(uint32_t));
        if (!s_v7_chunk_lba) return -1;
        zero_mem(s_v7_chunk_lba, (size_t)AFS_V7_MAX_CHUNKS * sizeof(uint32_t));
    }
    if (!s_v7_chunks) {
        s_v7_chunks = (afs_v7_chunk_t **)kmalloc((size_t)AFS_V7_MAX_CHUNKS * sizeof(afs_v7_chunk_t *));
        if (!s_v7_chunks) return -1;
        zero_mem(s_v7_chunks, (size_t)AFS_V7_MAX_CHUNKS * sizeof(afs_v7_chunk_t *));
    }
    if (!s_v7_chunk_used) {
        s_v7_chunk_used = (uint16_t *)kmalloc((size_t)AFS_V7_MAX_CHUNKS * sizeof(uint16_t));
        if (!s_v7_chunk_used) return -1;
        zero_mem(s_v7_chunk_used, (size_t)AFS_V7_MAX_CHUNKS * sizeof(uint16_t));
    }
    return 0;
}

static void free_v7_chunks(void) {
    if (!s_v7_chunks) return;
    for (uint32_t c = 0; c < AFS_V7_MAX_CHUNKS; c++) {
        if (s_v7_chunks[c]) {
            kfree(s_v7_chunks[c]);
            s_v7_chunks[c] = 0;
        }
        if (s_v7_chunk_used) s_v7_chunk_used[c] = 0;
        if (s_v7_chunk_lba) s_v7_chunk_lba[c] = 0;
    }
}

static int free_ext_grow(void) {
    if (s_free_cap >= AFS_FREE_EXT_MAX) return -1;
    uint32_t ncap = s_free_cap * 2u;
    if (ncap > AFS_FREE_EXT_MAX) ncap = AFS_FREE_EXT_MAX;
    afs_ext_t *nb = (afs_ext_t *)kmalloc((size_t)ncap * sizeof(afs_ext_t));
    if (!nb) return -1;
    zero_mem(nb, (size_t)ncap * sizeof(afs_ext_t));
    if (s_free_exts && s_free_count > 0)
        copy_mem(nb, s_free_exts, (size_t)s_free_count * sizeof(afs_ext_t));
    if (s_free_exts) kfree(s_free_exts);
    s_free_exts = nb;
    s_free_cap = ncap;
    return 0;
}

static int ln_grow(void) {
    uint32_t max_ln = (s_ver == 7) ? AFS_V7_MAX_FILES : AFS_V6_MAX_FILES;
    if (s_ln_cap >= max_ln) return -1;
    uint32_t ncap = s_ln_cap * 2u;
    if (ncap > max_ln) ncap = max_ln;
    afs_ln_ent_t *nb = (afs_ln_ent_t *)kmalloc((size_t)ncap * sizeof(afs_ln_ent_t));
    if (!nb) return -1;
    zero_mem(nb, (size_t)ncap * sizeof(afs_ln_ent_t));
    if (s_ln_tab && s_ln_count > 0)
        copy_mem(nb, s_ln_tab, (size_t)s_ln_count * sizeof(afs_ln_ent_t));
    if (s_ln_tab) kfree(s_ln_tab);
    s_ln_tab = nb;
    s_ln_cap = ncap;
    return 0;
}

/* ---- Temel yardimcilar ve durum erisicileri ---- */

static int afs_probe_base(uint32_t *out_base, uint32_t *out_ver) {
    uint32_t first[128];
    if (storage_sector_count() == 0) storage_init();

    if (storage_read(AFS_BASE_LBA, 1, first) == 0 &&
        first[0] == AFS_MAGIC &&
        (first[1] == AFS_VERSION || first[1] == AFS_VERSION_V6)) {
        if (out_base) *out_base = AFS_BASE_LBA;
        if (out_ver) *out_ver = first[1];
        return 1;
    }
    if (storage_read(AFS_BASE_LEGACY_LBA, 1, first) == 0 &&
        first[0] == AFS_MAGIC &&
        (first[1] == AFS_VERSION || first[1] == AFS_VERSION_V6 || first[1] == 5u)) {
        if (out_base) *out_base = AFS_BASE_LEGACY_LBA;
        if (out_ver) *out_ver = first[1];
        return 1;
    }
    if (storage_partition_base() > 0 &&
        storage_read(0, 1, first) == 0 &&
        first[0] == AFS_MAGIC &&
        (first[1] == AFS_VERSION || first[1] == AFS_VERSION_V6)) {
        if (out_base) *out_base = 0;
        if (out_ver) *out_ver = first[1];
        return 1;
    }
    return 0;
}

uint32_t afs_base(void) { return s_base; }

uint32_t afs_get_base_lba(void) {
    if (mounted) return s_base;
    uint32_t b = AFS_BASE_LBA;
    if (afs_probe_base(&b, 0)) return b;
    if (storage_partition_base() > 0 &&
        storage_sector_count() > 0 &&
        storage_sector_count() < AFS_BASE_LBA + 4096u) {
        return 0;
    }
    return AFS_BASE_LBA;
}

uint32_t afs_version(void) { return (uint32_t)s_ver; }

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

void afs_set_current_uid(uint16_t uid, uint16_t gid) {
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

/* ---- Birlesik slot erisimi (v6 ve v7 tek kod yolundan yurur) ---- */

static inline uint32_t slot_capacity(void) {
    if (s_ver == 7) return s_v7_sb.num_chunks * AFS_V7_CHUNK_ENTRIES;
    return s_hdr ? H.max_files : 0;
}

static inline uint32_t meta_count(void) {
    if (s_ver == 7) return s_v7_sb.count;
    return s_hdr ? H.count : 0;
}

static inline void meta_inc_count(void) {
    if (s_ver == 7) s_v7_sb.count++;
    else if (s_hdr) H.count++;
}

static inline void meta_dec_count(void) {
    if (s_ver == 7) { if (s_v7_sb.count) s_v7_sb.count--; }
    else if (s_hdr && H.count) H.count--;
}

static inline uint32_t meta_data_next(void) {
    if (s_ver == 7) return s_v7_sb.data_next;
    return s_hdr ? H.data_next : 0;
}

static inline void meta_set_data_next(uint32_t dn) {
    if (s_ver == 7) s_v7_sb.data_next = dn;
    else if (s_hdr) H.data_next = dn;
}

static inline uint64_t meta_capacity(void) {
    if (s_ver == 7) return s_v7_sb.capacity;
    return s_hdr ? H.capacity : 0;
}

static inline uint32_t afs_data_start(void) {
    if (s_ver == 7) return s_base + s_v7_sb.hdr_sectors;
    return s_hdr ? (s_base + H.hdr_sectors) : 0;
}

static inline afs_v7_inode_t *v7_inode(uint32_t idx) {
    uint32_t c = idx >> 8;
    if (c >= s_v7_sb.num_chunks || !s_v7_chunks[c]) return 0;
    return &s_v7_chunks[c]->inodes[idx & 255u];
}

static inline void v7_mark_dirty(uint32_t idx) {
    uint32_t c = idx >> 8;
    if (c < s_v7_sb.num_chunks && s_v7_chunks[c]) {
        uint32_t sec = (idx & 255u) >> 2;
        s_v7_chunks[c]->dirty_mask |= (1ULL << sec);
    }
}

static inline int slot_used(uint32_t idx) {
    if (s_ver == 7) {
        afs_v7_inode_t *in = v7_inode(idx);
        return in ? (in->used != 0) : 0;
    }
    return (s_hdr && idx < H.max_files) ? (H.files[idx].used != 0) : 0;
}

static inline const char *slot_name(uint32_t idx) {
    if (s_ver == 7) {
        afs_v7_inode_t *in = v7_inode(idx);
        return in ? in->name : "";
    }
    return (s_hdr && idx < H.max_files) ? H.files[idx].name : "";
}

static inline uint32_t slot_size(uint32_t idx) {
    if (s_ver == 7) {
        afs_v7_inode_t *in = v7_inode(idx);
        return in ? in->size : 0;
    }
    return (s_hdr && idx < H.max_files) ? H.files[idx].size : 0;
}

static inline void slot_get_perm(uint32_t idx, uint32_t *uid, uint32_t *gid, uint32_t *mode) {
    if (s_ver == 7) {
        afs_v7_inode_t *in = v7_inode(idx);
        if (in) { *uid = in->uid; *gid = in->gid; *mode = in->mode; return; }
    } else if (s_hdr && idx < H.max_files) {
        *uid = H.files[idx].uid; *gid = H.files[idx].gid; *mode = H.files[idx].mode; return;
    }
    *uid = 0; *gid = 0; *mode = 0;
}

static inline void slot_set_perm_owner(uint32_t idx, uint32_t uid, uint32_t gid, uint32_t mode) {
    if (s_ver == 7) {
        afs_v7_inode_t *in = v7_inode(idx);
        if (in) { in->uid = uid; in->gid = gid; in->mode = mode; v7_mark_dirty(idx); }
    } else if (s_hdr && idx < H.max_files) {
        H.files[idx].uid = uid; H.files[idx].gid = gid; H.files[idx].mode = mode;
    }
}

static inline void slot_bump_gen(uint32_t idx) {
    if (s_ver == 7) {
        uint32_t c = idx >> 8;
        if (c < s_v7_sb.num_chunks && s_v7_chunks[c])
            s_v7_chunks[c]->gen[idx & 255u]++;
    } else if (s_hgen && idx < AFS_V6_MAX_FILES) {
        s_hgen[idx]++;
    }
}

static inline uint32_t slot_get_gen(uint32_t idx) {
    if (s_ver == 7) {
        uint32_t c = idx >> 8;
        if (c < s_v7_sb.num_chunks && s_v7_chunks[c])
            return s_v7_chunks[c]->gen[idx & 255u];
        return 0;
    }
    return (s_hgen && idx < AFS_V6_MAX_FILES) ? s_hgen[idx] : 0;
}

static uint64_t compute_capacity(void) {
    uint64_t secs = storage_sector_count();
    if (secs == 0) {
        storage_init();
        secs = storage_sector_count();
    }
    uint64_t cap;
    if (secs == 0) return (uint64_t)s_base + 65536u;
    cap = secs > AFS_RESERVE_SEC ? secs - AFS_RESERVE_SEC : secs;
    if (cap > 0xFFFFFFFEull) cap = 0xFFFFFFFEull;
    if (cap < (uint64_t)s_base + AFS_CLUSTER_SEC) cap = (uint64_t)s_base + AFS_CLUSTER_SEC;
    return cap;
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
    if (storage_read(jl, 1, sec) != 0) return;
    uint32_t magic = *(uint32_t *)sec;
    if (magic != AFS_JOURNAL_MAGIC) return;
    if (*(uint32_t *)(sec + 4) == 7u) {
        afs_v7_jrec_t *r7 = (afs_v7_jrec_t *)sec;
        r7->state = 0;
    } else {
        afs_jrec_t *rec = (afs_jrec_t *)sec;
        rec->state = 0;
    }
    storage_write(jl, 1, sec);
    storage_flush();
}

/* ---- v6 header I/O & commit ---- */

static void v6_hdr_sync_size(void) {
    uint32_t bytes = AFS_HEADER_BASE + H.max_files * AFS_ENTRY_SIZE;
    H.hdr_sectors = (bytes + 511u) / 512u;
}

static uint32_t v6_hdr_bytes(void) {
    return AFS_HEADER_BASE + H.max_files * AFS_ENTRY_SIZE;
}

static int v6_hdr_load_at(uint32_t lba, uint32_t hs) {
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

static int v6_hdr_save_at(uint32_t lba, uint32_t hs) {
    if (hs == 0 || hs > AFS_HDR_MAX_SEC) return -1;
    uint8_t *s = (uint8_t *)s_hdr;
    uint32_t n = (uint32_t)sizeof(afs_hdr_t);
    if (n > hs * 512u) n = hs * 512u;
    uint32_t i;
    for (i = 0; i < n; i++) s_hbuf[i] = s[i];
    for (; i < hs * 512u; i++) s_hbuf[i] = 0;
    return storage_write(lba, hs, s_hbuf);
}

static uint32_t v6_hdr_crc(void) {
    uint32_t saved = H.flags;
    H.flags = 0;
    uint32_t c = crc32_buf(s_hdr, v6_hdr_bytes());
    H.flags = saved;
    return c;
}

static int v6_commit(void) {
    H.version = AFS_VERSION_V6;
    v6_hdr_sync_size();
    uint32_t prev_crc = H.flags;
    H.flags = 0;
    H.flags = v6_hdr_crc();

    uint32_t jl = journal_lba();
    if (jl == 0 || H.data_next > jl || H.hdr_sectors + 1u > AFS_JOURNAL_SEC) {
        return v6_hdr_save_at(s_base, H.hdr_sectors);
    }

    if (v6_hdr_save_at(jl + 1u, H.hdr_sectors) != 0) return -1;
    storage_flush();

    uint8_t sec[512];
    afs_jrec_t *rec = (afs_jrec_t *)sec;
    zero_mem(sec, 512);
    rec->magic = AFS_JOURNAL_MAGIC;
    rec->seq = ++s_jseq;
    rec->hdr_sectors = H.hdr_sectors;
    rec->crc = crc32_buf(s_hdr, v6_hdr_bytes());
    rec->state = 1;
    rec->prev_crc = prev_crc;
    if (storage_write(jl, 1, sec) != 0) return -1;
    storage_flush();

    if (v6_hdr_save_at(s_base, H.hdr_sectors) != 0) return -1;
    storage_flush();

    rec->state = 0;
    storage_write(jl, 1, sec);
    storage_flush();
    return 0;
}

/* ---- v7 metadata CRC & journaled commit ---- */

static uint32_t v7_sb_crc(const afs_v7_sb_t *sb) {
    afs_v7_sb_t tmp = *sb;
    tmp.flags = 0;
    return crc32_buf(&tmp, sizeof(tmp));
}

static int v7_commit(void) {
    s_v7_sb.magic = AFS_MAGIC;
    s_v7_sb.version = AFS_VERSION;
    s_v7_sb.max_files = AFS_V7_MAX_FILES;
    s_v7_sb.hdr_sectors = AFS_V7_HDR_SECS;
    s_v7_sb.inode_dir_lba = s_base + 1u;
    s_v7_sb.inode_dir_secs = AFS_V7_DIR_SECS;
    s_v7_sb.inode_blk_secs = AFS_V7_CHUNK_SECS;
    s_v7_sb.chunk_cap = AFS_V7_CHUNK_ENTRIES;
    s_v7_sb.journal_seq = ++s_jseq;
    s_v7_sb.inode_dir_crc = crc32_buf(s_v7_chunk_lba,
                                      (size_t)AFS_V7_MAX_CHUNKS * sizeof(uint32_t));
    s_v7_sb.flags = 0;
    s_v7_sb.flags = v7_sb_crc(&s_v7_sb);

    /* Kac metadata sektoru kirli? */
    uint32_t dirty_chunk_secs = 0;
    for (uint32_t c = 0; c < s_v7_sb.num_chunks; c++) {
        if (!s_v7_chunks[c] || s_v7_chunks[c]->dirty_mask == 0) continue;
        uint64_t m = s_v7_chunks[c]->dirty_mask;
        for (uint32_t s = 0; s < AFS_V7_CHUNK_SECS; s++)
            if (m & (1ULL << s)) dirty_chunk_secs++;
    }

    uint32_t total_dirty = dirty_chunk_secs + (s_v7_dir_dirty ? AFS_V7_DIR_SECS : 0u) + 1u;
    uint32_t jl = journal_lba();

    /* Eger kirli sektor sayisi gunluk limitini (120) asiyorsa (ornegin format/toplu fsck)
     * inode parcalarini once diske yazip ardindan dir+superblock'u gunlukle. */
    int direct_chunks = (jl == 0 || s_v7_sb.data_next > jl || total_dirty > AFS_V7_JRN_MAX_ENTS);
    if (direct_chunks) {
        for (uint32_t c = 0; c < s_v7_sb.num_chunks; c++) {
            if (!s_v7_chunks[c] || s_v7_chunks[c]->dirty_mask == 0) continue;
            uint64_t m = s_v7_chunks[c]->dirty_mask;
            const uint8_t *base_ptr = (const uint8_t *)s_v7_chunks[c]->inodes;
            if (m == ~0ULL) {
                if (storage_write(s_v7_chunk_lba[c], AFS_V7_CHUNK_SECS, base_ptr) != 0)
                    return -1;
            } else {
                for (uint32_t s = 0; s < AFS_V7_CHUNK_SECS; s++) {
                    if (m & (1ULL << s)) {
                        if (storage_write(s_v7_chunk_lba[c] + s, 1, base_ptr + (size_t)s * 512u) != 0)
                            return -1;
                    }
                }
            }
            s_v7_chunks[c]->dirty_mask = 0;
        }
        storage_flush();
        dirty_chunk_secs = 0;
        total_dirty = (s_v7_dir_dirty ? AFS_V7_DIR_SECS : 0u) + 1u;
    }

    if (jl == 0 || s_v7_sb.data_next > jl || total_dirty > AFS_V7_JRN_MAX_ENTS) {
        if (s_v7_dir_dirty) {
            if (storage_write(s_v7_sb.inode_dir_lba, AFS_V7_DIR_SECS, s_v7_chunk_lba) != 0)
                return -1;
            s_v7_dir_dirty = 0;
        }
        if (storage_write(s_base, 1, &s_v7_sb) != 0) return -1;
        storage_flush();
        return 0;
    }

    /* Gunlukle atomik commit: staged payload [jl+1 .. jl+total_dirty] */
    uint8_t jsec[512];
    zero_mem(jsec, 512);
    afs_v7_jrec_t *rec = (afs_v7_jrec_t *)jsec;
    uint32_t idx = 0;
    uint32_t crc = 0xFFFFFFFFu;
    if (!s_crc32_ready) crc32_tab_init();

    /* 1) Kirli chunk sektorleri */
    for (uint32_t c = 0; c < s_v7_sb.num_chunks; c++) {
        if (!s_v7_chunks[c] || s_v7_chunks[c]->dirty_mask == 0) continue;
        uint64_t m = s_v7_chunks[c]->dirty_mask;
        const uint8_t *base_ptr = (const uint8_t *)s_v7_chunks[c]->inodes;
        for (uint32_t s = 0; s < AFS_V7_CHUNK_SECS; s++) {
            if (!(m & (1ULL << s))) continue;
            const uint8_t *p = base_ptr + (size_t)s * 512u;
            if (storage_write(jl + 1u + idx, 1, p) != 0) return -1;
            for (uint32_t b = 0; b < 512u; b++)
                crc = s_crc32_tab[(crc ^ p[b]) & 0xFFu] ^ (crc >> 8);
            rec->lbas[idx++] = s_v7_chunk_lba[c] + s;
        }
    }
    /* 2) inode_dir */
    if (s_v7_dir_dirty) {
        const uint8_t *dp = (const uint8_t *)s_v7_chunk_lba;
        for (uint32_t s = 0; s < AFS_V7_DIR_SECS; s++) {
            const uint8_t *p = dp + (size_t)s * 512u;
            if (storage_write(jl + 1u + idx, 1, p) != 0) return -1;
            for (uint32_t b = 0; b < 512u; b++)
                crc = s_crc32_tab[(crc ^ p[b]) & 0xFFu] ^ (crc >> 8);
            rec->lbas[idx++] = s_v7_sb.inode_dir_lba + s;
        }
    }
    /* 3) Superblock */
    {
        const uint8_t *p = (const uint8_t *)&s_v7_sb;
        if (storage_write(jl + 1u + idx, 1, p) != 0) return -1;
        for (uint32_t b = 0; b < 512u; b++)
            crc = s_crc32_tab[(crc ^ p[b]) & 0xFFu] ^ (crc >> 8);
        rec->lbas[idx++] = s_base;
    }
    storage_flush();

    rec->magic = AFS_JOURNAL_MAGIC;
    rec->version = AFS_VERSION;
    rec->seq = s_jseq;
    rec->count = idx;
    rec->crc = ~crc;
    rec->state = 1;
    rec->sb_crc = s_v7_sb.flags;
    if (storage_write(jl, 1, jsec) != 0) return -1;
    storage_flush();

    /* Gercek yerlerine yaz */
    for (uint32_t k = 0; k < idx; k++) {
        uint8_t buf[512];
        if (storage_read(jl + 1u + k, 1, buf) != 0) return -1;
        if (storage_write(rec->lbas[k], 1, buf) != 0) return -1;
    }
    storage_flush();

    rec->state = 0;
    storage_write(jl, 1, jsec);
    storage_flush();

    for (uint32_t c = 0; c < s_v7_sb.num_chunks; c++)
        if (s_v7_chunks[c]) s_v7_chunks[c]->dirty_mask = 0;
    s_v7_dir_dirty = 0;
    return 0;
}

static int afs_commit(void) {
    if (s_ver == 7) return v7_commit();
    return v6_commit();
}

static void journal_replay(void) {
    uint32_t jl = journal_lba();
    if (jl == 0) return;
    uint8_t sec[512];
    if (storage_read(jl, 1, sec) != 0) return;
    uint32_t magic = *(uint32_t *)sec;
    if (magic != AFS_JOURNAL_MAGIC) return;

    if (*(uint32_t *)(sec + 4) == 7u) {
        afs_v7_jrec_t r7;
        copy_mem(&r7, sec, sizeof(r7));
        if (r7.state != 1u || r7.count == 0u || r7.count > AFS_V7_JRN_MAX_ENTS) {
            if (r7.state == 1u) journal_clear(jl);
            return;
        }
        uint8_t *staged = (uint8_t *)kmalloc((size_t)r7.count * 512u);
        if (!staged) return;
        if (storage_read(jl + 1u, r7.count, staged) != 0) { kfree(staged); return; }
        if (crc32_buf(staged, r7.count * 512u) != r7.crc) {
            kfree(staged);
            journal_clear(jl);
            return;
        }
        for (uint32_t k = 0; k < r7.count; k++) {
            if (r7.lbas[k] < s_base || r7.lbas[k] >= jl) {
                kfree(staged);
                journal_clear(jl);
                return;
            }
        }
        for (uint32_t k = 0; k < r7.count; k++)
            storage_write(r7.lbas[k], 1, staged + (size_t)k * 512u);
        storage_flush();
        kfree(staged);
        journal_clear(jl);
        return;
    }

    /* v6 gunluk oynatma */
    if (!s_hdr && afs_ensure_v6_mem() != 0) return;
    afs_jrec_t *rec = (afs_jrec_t *)sec;
    if (rec->state != 1 || rec->hdr_sectors == 0 || rec->hdr_sectors > AFS_HDR_MAX_SEC) return;

    uint32_t cur_hdr[128];
    if (storage_read(s_base, 1, cur_hdr) != 0) return;
    if (cur_hdr[0] == AFS_MAGIC && cur_hdr[8] != rec->prev_crc) {
        journal_clear(jl);
        return;
    }

    if (v6_hdr_load_at(jl + 1u, rec->hdr_sectors) != 0) return;
    if (H.magic != AFS_MAGIC || (H.version != AFS_VERSION_V6 && H.version != 5u) ||
        H.max_files < 32u || H.max_files > AFS_V6_MAX_FILES) { journal_clear(jl); return; }
    if (crc32_buf(s_hdr, v6_hdr_bytes()) != rec->crc) { journal_clear(jl); return; }
    v6_hdr_save_at(s_base, rec->hdr_sectors);
    storage_flush();
    journal_clear(jl);
}

/* ---- Bos-alan (free-extent) yonetimi + birlestirme + kuyruk kirpma ---- */

static uint32_t v6_file_secs(const afs_file_t *f) {
    uint32_t s = (f->size + 511u) / 512u;
    return s ? s : 1u;
}

static void free_ext_trim_tail(void) {
    uint32_t ds = afs_data_start();
    uint32_t dn = meta_data_next();
    while (s_free_count > 0) {
        afs_ext_t *last = &s_free_exts[s_free_count - 1u];
        if ((uint64_t)last->start + last->count >= (uint64_t)dn) {
            if (last->start < dn)
                dn = last->start;
            s_free_count--;
        } else {
            break;
        }
    }
    if (dn < ds) dn = ds;
    meta_set_data_next(dn);
}

static void free_ext_remove_at(uint32_t idx) {
    for (uint32_t k = idx; k + 1u < s_free_count; k++)
        s_free_exts[k] = s_free_exts[k + 1u];
    if (s_free_count > 0) s_free_count--;
}

static void free_ext_add(uint32_t start, uint32_t count) {
    if (!s_free_exts || count == 0) return;
    uint32_t ds = afs_data_start();
    uint32_t dn = meta_data_next();
    if (start < ds) {
        if ((uint64_t)start + count <= ds) return;
        count -= (ds - start);
        start = ds;
    }
    if (start >= dn) return;
    if ((uint64_t)start + count > dn)
        count = dn - start;
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
    } else {
        if (s_free_count >= s_free_cap && free_ext_grow() != 0) {
            s_reclaimed_secs += count;
            free_ext_trim_tail();
            return;
        }
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

    uint32_t dn = meta_data_next();
    uint64_t cap = meta_capacity();
    if ((uint64_t)dn + secs > cap) return 0;
    meta_set_data_next(dn + secs);
    return dn;
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

/* ---- v7 coklu-extent okuma / yazma / geri kazanim ---- */

static int v7_read_ind_blk(uint32_t lba, afs_v7_ext_blk_t *out) {
    if (lba == 0) return -1;
    if (storage_read(lba, 1, out) != 0) return -1;
    if (out->magic != AFS_V7_EXT_MAGIC || out->count > AFS_V7_IND_EXTS) return -1;
    if (crc32_buf(out, sizeof(*out) - 4u) != out->crc32) return -1;
    return 0;
}

static int v7_write_ind_blk(uint32_t lba, afs_v7_ext_blk_t *blk) {
    blk->magic = AFS_V7_EXT_MAGIC;
    blk->crc32 = crc32_buf(blk, sizeof(*blk) - 4u);
    return storage_write(lba, 1, blk);
}

/* Bir v7 inode'unun tum veri extent'lerini (ve varsa dolayli blok LBA'larini) toplar. */
static int v7_collect_extents(const afs_v7_inode_t *in,
                              afs_ext_t **out_exts, uint32_t *out_cnt,
                              uint32_t **out_ind, uint32_t *out_ind_cnt) {
    *out_exts = 0; *out_cnt = 0;
    if (out_ind) *out_ind = 0;
    if (out_ind_cnt) *out_ind_cnt = 0;

    uint32_t total = in->ext_count;
    if (total == 0) return 0;

    afs_ext_t *exts = (afs_ext_t *)kmalloc((size_t)total * sizeof(afs_ext_t));
    if (!exts) return -1;

    uint32_t n_in = total < AFS_V7_INLINE_EXTS ? total : AFS_V7_INLINE_EXTS;
    for (uint32_t i = 0; i < n_in; i++) {
        exts[i].start = in->extents[i].lba;
        exts[i].count = in->extents[i].sectors;
    }

    uint32_t ind_cap = (total > AFS_V7_INLINE_EXTS)
                     ? ((total - AFS_V7_INLINE_EXTS + AFS_V7_IND_EXTS - 1u) / AFS_V7_IND_EXTS + 1u)
                     : 0u;
    uint32_t *inds = 0;
    uint32_t ind_cnt = 0;
    if (ind_cap > 0 && out_ind) {
        inds = (uint32_t *)kmalloc((size_t)ind_cap * sizeof(uint32_t));
        if (!inds) { kfree(exts); return -1; }
    }

    uint32_t pos = n_in;
    uint32_t cur_lba = in->ext_ind_lba;
    while (pos < total && cur_lba != 0) {
        afs_v7_ext_blk_t blk;
        if (v7_read_ind_blk(cur_lba, &blk) != 0) {
            if (inds) kfree(inds);
            kfree(exts);
            return -1;
        }
        if (inds && ind_cnt < ind_cap) inds[ind_cnt++] = cur_lba;
        for (uint32_t k = 0; k < blk.count && pos < total; k++) {
            exts[pos].start = blk.extents[k].lba;
            exts[pos].count = blk.extents[k].sectors;
            pos++;
        }
        cur_lba = blk.next_lba;
    }
    if (pos != total) {
        if (inds) kfree(inds);
        kfree(exts);
        return -1;
    }

    *out_exts = exts;
    *out_cnt = pos;
    if (out_ind) *out_ind = inds;
    if (out_ind_cnt) *out_ind_cnt = ind_cnt;
    return 0;
}

static uint32_t v7_total_secs(const afs_v7_inode_t *in) {
    if (in->ext_count == 0) return 0;
    if (in->ext_count == 1 && in->ext_ind_lba == 0) return in->extents[0].sectors;
    afs_ext_t *exts = 0;
    uint32_t cnt = 0;
    if (v7_collect_extents(in, &exts, &cnt, 0, 0) != 0) return 0;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < cnt; i++) sum += exts[i].count;
    kfree(exts);
    return sum;
}

static void v7_free_inode_extents(afs_v7_inode_t *in) {
    if (!in || in->ext_count == 0) {
        if (in) { in->ext_ind_lba = 0; zero_mem(in->extents, sizeof(in->extents)); }
        return;
    }
    if (in->ext_count <= AFS_V7_INLINE_EXTS && in->ext_ind_lba == 0) {
        for (uint32_t i = 0; i < in->ext_count; i++)
            free_ext_add(in->extents[i].lba, in->extents[i].sectors);
    } else {
        afs_ext_t *exts = 0;
        uint32_t cnt = 0;
        uint32_t *inds = 0;
        uint32_t ind_cnt = 0;
        if (v7_collect_extents(in, &exts, &cnt, &inds, &ind_cnt) == 0) {
            for (uint32_t i = 0; i < cnt; i++)
                free_ext_add(exts[i].start, exts[i].count);
            for (uint32_t i = 0; i < ind_cnt; i++)
                free_ext_add(inds[i], 1u);
            kfree(exts);
            if (inds) kfree(inds);
        }
    }
    in->ext_count = 0;
    in->ext_ind_lba = 0;
    zero_mem(in->extents, sizeof(in->extents));
}

/* Coklu-extent tahsis: once tek bitisik parca (hizli yol), yetmezse bos
 * deliklerden (en buyukten kucuge) + gerekirse data_next'ten toplar. */
static int v7_alloc_extents(uint32_t need_secs, afs_ext_t **out_exts, uint32_t *out_cnt) {
    *out_exts = 0; *out_cnt = 0;
    if (need_secs == 0) return 0;

    /* 1) Hizli yol A: tek bos delik yeterli mi? (en iyi uyum) */
    int best = -1;
    uint32_t best_cnt = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < s_free_count; i++) {
        uint32_t c = s_free_exts[i].count;
        if (c >= need_secs && c < best_cnt) {
            best = (int)i; best_cnt = c;
            if (c == need_secs) break;
        }
    }
    if (best >= 0) {
        afs_ext_t *res = (afs_ext_t *)kmalloc(sizeof(afs_ext_t));
        if (!res) return -1;
        uint32_t idx = (uint32_t)best;
        res[0].start = s_free_exts[idx].start;
        res[0].count = need_secs;
        if (s_free_exts[idx].count == need_secs) free_ext_remove_at(idx);
        else { s_free_exts[idx].start += need_secs; s_free_exts[idx].count -= need_secs; }
        s_reused_secs += need_secs;
        *out_exts = res; *out_cnt = 1;
        return 0;
    }

    /* 2) Hizli yol B: data_next sonunda tek bitisik alan yeterli mi? */
    uint32_t dn = meta_data_next();
    uint64_t cap = meta_capacity();
    if ((uint64_t)dn + need_secs <= cap) {
        afs_ext_t *res = (afs_ext_t *)kmalloc(sizeof(afs_ext_t));
        if (!res) return -1;
        res[0].start = dn;
        res[0].count = need_secs;
        meta_set_data_next(dn + need_secs);
        *out_exts = res; *out_cnt = 1;
        return 0;
    }

    /* 3) Coklu-extent yolu: bos parcalari topla (en buyuk parcalar once). */
    uint64_t avail = (cap > dn) ? (cap - dn) : 0;
    for (uint32_t i = 0; i < s_free_count; i++) avail += s_free_exts[i].count;
    if (avail < need_secs) return -1;

    uint32_t rcap = 32u;
    afs_ext_t *res = (afs_ext_t *)kmalloc((size_t)rcap * sizeof(afs_ext_t));
    if (!res) return -1;
    uint32_t rcnt = 0;
    uint32_t rem = need_secs;

    while (rem > 0 && s_free_count > 0) {
        uint32_t max_i = 0;
        for (uint32_t i = 1; i < s_free_count; i++)
            if (s_free_exts[i].count > s_free_exts[max_i].count) max_i = i;
        uint32_t take = s_free_exts[max_i].count;
        if (take > rem) take = rem;
        uint32_t lba = s_free_exts[max_i].start;
        if (s_free_exts[max_i].count == take) free_ext_remove_at(max_i);
        else { s_free_exts[max_i].start += take; s_free_exts[max_i].count -= take; }
        s_reused_secs += take;

        if (rcnt >= rcap) {
            uint32_t ncap = rcap * 2u;
            afs_ext_t *nb = (afs_ext_t *)kmalloc((size_t)ncap * sizeof(afs_ext_t));
            if (!nb) {
                for (uint32_t k = 0; k < rcnt; k++) free_ext_add(res[k].start, res[k].count);
                free_ext_add(lba, take);
                kfree(res);
                return -1;
            }
            copy_mem(nb, res, (size_t)rcnt * sizeof(afs_ext_t));
            kfree(res);
            res = nb; rcap = ncap;
        }
        res[rcnt].start = lba;
        res[rcnt].count = take;
        rcnt++;
        rem -= take;
    }

    if (rem > 0) {
        dn = meta_data_next();
        if ((uint64_t)dn + rem > cap) {
            for (uint32_t k = 0; k < rcnt; k++) free_ext_add(res[k].start, res[k].count);
            kfree(res);
            return -1;
        }
        if (rcnt >= rcap) {
            afs_ext_t *nb = (afs_ext_t *)kmalloc((size_t)(rcap + 1u) * sizeof(afs_ext_t));
            if (!nb) {
                for (uint32_t k = 0; k < rcnt; k++) free_ext_add(res[k].start, res[k].count);
                kfree(res);
                return -1;
            }
            copy_mem(nb, res, (size_t)rcnt * sizeof(afs_ext_t));
            kfree(res);
            res = nb; rcap++;
        }
        res[rcnt].start = dn;
        res[rcnt].count = rem;
        rcnt++;
        meta_set_data_next(dn + rem);
    }

    if (rcnt > 0xFFFFu) {
        for (uint32_t k = 0; k < rcnt; k++) free_ext_add(res[k].start, res[k].count);
        kfree(res);
        return -1;
    }

    *out_exts = res;
    *out_cnt = rcnt;
    return 0;
}

static int v7_store_extents(afs_v7_inode_t *in, const afs_ext_t *exts, uint32_t cnt) {
    zero_mem(in->extents, sizeof(in->extents));
    in->ext_ind_lba = 0;
    in->ext_count = (uint16_t)cnt;
    if (cnt == 0) return 0;

    uint32_t n_in = cnt < AFS_V7_INLINE_EXTS ? cnt : AFS_V7_INLINE_EXTS;
    for (uint32_t i = 0; i < n_in; i++) {
        in->extents[i].lba = exts[i].start;
        in->extents[i].sectors = exts[i].count;
    }
    if (cnt <= AFS_V7_INLINE_EXTS) return 0;

    uint32_t rem = cnt - AFS_V7_INLINE_EXTS;
    uint32_t n_ind = (rem + AFS_V7_IND_EXTS - 1u) / AFS_V7_IND_EXTS;
    uint32_t *ind_lbas = (uint32_t *)kmalloc((size_t)n_ind * sizeof(uint32_t));
    if (!ind_lbas) return -1;

    for (uint32_t b = 0; b < n_ind; b++) {
        ind_lbas[b] = free_ext_alloc(1u);
        if (ind_lbas[b] == 0) {
            for (uint32_t k = 0; k < b; k++) free_ext_add(ind_lbas[k], 1u);
            kfree(ind_lbas);
            return -1;
        }
    }

    uint32_t pos = AFS_V7_INLINE_EXTS;
    for (uint32_t b = 0; b < n_ind; b++) {
        afs_v7_ext_blk_t blk;
        zero_mem(&blk, sizeof(blk));
        uint32_t take = cnt - pos;
        if (take > AFS_V7_IND_EXTS) take = AFS_V7_IND_EXTS;
        blk.count = take;
        for (uint32_t k = 0; k < take; k++) {
            blk.extents[k].lba = exts[pos + k].start;
            blk.extents[k].sectors = exts[pos + k].count;
        }
        blk.next_lba = (b + 1u < n_ind) ? ind_lbas[b + 1u] : 0u;
        if (v7_write_ind_blk(ind_lbas[b], &blk) != 0) {
            for (uint32_t k = 0; k < n_ind; k++) free_ext_add(ind_lbas[k], 1u);
            kfree(ind_lbas);
            return -1;
        }
        pos += take;
    }

    in->ext_ind_lba = ind_lbas[0];
    kfree(ind_lbas);
    return 0;
}

static int v7_write_data(const afs_ext_t *exts, uint32_t ext_cnt, const void *data, uint32_t size) {
    const uint8_t *src = (const uint8_t *)data;
    uint32_t byte_off = 0;
    for (uint32_t e = 0; e < ext_cnt; e++) {
        uint32_t lba = exts[e].start;
        uint32_t secs = exts[e].count;
        uint32_t rem_bytes = (byte_off < size) ? (size - byte_off) : 0u;
        uint32_t ext_bytes = secs * 512u;
        if (rem_bytes >= ext_bytes) {
            if (storage_write(lba, secs, src + byte_off) != 0) return -1;
            byte_off += ext_bytes;
        } else {
            uint32_t full_secs = rem_bytes / 512u;
            if (full_secs > 0) {
                if (storage_write(lba, full_secs, src + byte_off) != 0) return -1;
                byte_off += full_secs * 512u;
            }
            if (full_secs < secs) {
                uint8_t sec[512];
                zero_mem(sec, 512);
                uint32_t tail = (byte_off < size) ? (size - byte_off) : 0u;
                if (tail > 512u) tail = 512u;
                for (uint32_t k = 0; k < tail; k++) sec[k] = src[byte_off + k];
                if (storage_write(lba + full_secs, 1, sec) != 0) return -1;
                byte_off += tail;
                if (full_secs + 1u < secs) {
                    zero_mem(sec, 512);
                    for (uint32_t s = full_secs + 1u; s < secs; s++) {
                        if (storage_write(lba + s, 1, sec) != 0) return -1;
                    }
                }
            }
        }
    }
    return 0;
}

/* ---- Bos-alan tablosunu yeniden olusturma (O(N log N) heapsort) ---- */

static void heapsort_exts(afs_ext_t *arr, uint32_t n) {
    if (n < 2u) return;
    for (uint32_t start = n / 2u; start > 0; start--) {
        uint32_t root = start - 1u;
        while (root * 2u + 1u < n) {
            uint32_t child = root * 2u + 1u;
            if (child + 1u < n && arr[child].start < arr[child + 1u].start)
                child++;
            if (arr[root].start < arr[child].start) {
                afs_ext_t t = arr[root]; arr[root] = arr[child]; arr[child] = t;
                root = child;
            } else break;
        }
    }
    for (uint32_t end = n - 1u; end > 0; end--) {
        afs_ext_t t = arr[0]; arr[0] = arr[end]; arr[end] = t;
        uint32_t root = 0;
        while (root * 2u + 1u < end) {
            uint32_t child = root * 2u + 1u;
            if (child + 1u < end && arr[child].start < arr[child + 1u].start)
                child++;
            if (arr[root].start < arr[child].start) {
                afs_ext_t tmp = arr[root]; arr[root] = arr[child]; arr[child] = tmp;
                root = child;
            } else break;
        }
    }
}

static void free_ext_rebuild(void) {
    if (!s_free_exts) return;
    s_free_count = 0;
    uint32_t ds = afs_data_start();
    uint32_t dn = meta_data_next();
    if (dn <= ds) return;

    uint32_t cap = 256u;
    afs_ext_t *used = (afs_ext_t *)kmalloc((size_t)cap * sizeof(afs_ext_t));
    if (!used) return;
    uint32_t n = 0;

    #define PUSH_USED(ST, CNT) do { \
        uint32_t _st = (ST), _cnt = (CNT); \
        if (_st >= ds && _st < dn && _cnt > 0) { \
            if ((uint64_t)_st + _cnt > dn) _cnt = dn - _st; \
            if (n >= cap) { \
                uint32_t ncap = cap * 2u; \
                afs_ext_t *nb = (afs_ext_t *)kmalloc((size_t)ncap * sizeof(afs_ext_t)); \
                if (nb) { copy_mem(nb, used, (size_t)n * sizeof(afs_ext_t)); kfree(used); used = nb; cap = ncap; } \
            } \
            if (n < cap) { used[n].start = _st; used[n].count = _cnt; n++; } \
        } \
    } while (0)

    if (s_ver == 6 && s_hdr) {
        for (uint32_t i = 0; i < H.max_files; i++) {
            if (!H.files[i].used) continue;
            PUSH_USED(H.files[i].lba, v6_file_secs(&H.files[i]));
        }
    } else if (s_ver == 7) {
        for (uint32_t c = 0; c < s_v7_sb.num_chunks; c++) {
            PUSH_USED(s_v7_chunk_lba[c], AFS_V7_CHUNK_SECS);
            if (!s_v7_chunks[c]) continue;
            for (uint32_t i = 0; i < AFS_V7_CHUNK_ENTRIES; i++) {
                afs_v7_inode_t *in = &s_v7_chunks[c]->inodes[i];
                if (!in->used || in->ext_count == 0) continue;
                if (in->ext_count <= AFS_V7_INLINE_EXTS && in->ext_ind_lba == 0) {
                    for (uint32_t e = 0; e < in->ext_count; e++)
                        PUSH_USED(in->extents[e].lba, in->extents[e].sectors);
                } else {
                    afs_ext_t *exts = 0; uint32_t ecnt = 0;
                    uint32_t *inds = 0; uint32_t icnt = 0;
                    if (v7_collect_extents(in, &exts, &ecnt, &inds, &icnt) == 0) {
                        for (uint32_t e = 0; e < ecnt; e++)
                            PUSH_USED(exts[e].start, exts[e].count);
                        for (uint32_t e = 0; e < icnt; e++)
                            PUSH_USED(inds[e], 1u);
                        kfree(exts);
                        if (inds) kfree(inds);
                    }
                }
            }
        }
    }
    #undef PUSH_USED

    heapsort_exts(used, n);

    uint32_t cursor = ds;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t st = used[i].start;
        uint32_t end = st + used[i].count;
        if (st > cursor) {
            if (s_free_count < s_free_cap || free_ext_grow() == 0) {
                s_free_exts[s_free_count].start = cursor;
                s_free_exts[s_free_count].count = st - cursor;
                s_free_count++;
            }
        }
        if (end > cursor) cursor = end;
    }
    if (cursor < dn) {
        if (s_free_count < s_free_cap || free_ext_grow() == 0) {
            s_free_exts[s_free_count].start = cursor;
            s_free_exts[s_free_count].count = dn - cursor;
            s_free_count++;
        }
    }
    kfree(used);
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
    out->data_next = mounted ? meta_data_next() : 0;
    out->data_start = mounted ? afs_data_start() : 0;
    out->capacity = mounted ? (uint32_t)meta_capacity() : 0;
}

/* ---- Uzun yol tablosu (@LN_<hex8>_<tail> <-> .afs_longnames) ---- */

static int is_ln_token(const char *s) {
    return s && s[0] == '@' && s[1] == 'L' && s[2] == 'N' && s[3] == '_';
}

static int find_raw_idx(const char *name) {
    uint32_t cap = slot_capacity();
    for (uint32_t i = 0; i < cap; i++) {
        if (slot_used(i)) {
            const char *nm = slot_name(i);
            if (nm[0] == name[0] && seq(nm, name)) return (int)i;
        }
    }
    return -1;
}

static int ln_find_by_token(const char *tok) {
    for (uint32_t i = 0; i < s_ln_count; i++)
        if (s_ln_tab[i].token[0] == tok[0] && seq(s_ln_tab[i].token, tok)) return (int)i;
    return -1;
}

static int ln_find_by_path(const char *path) {
    for (uint32_t i = 0; i < s_ln_count; i++)
        if (s_ln_tab[i].path[0] == path[0] && seq(s_ln_tab[i].path, path)) return (int)i;
    return -1;
}

static const char *entry_full_name(uint32_t idx) {
    const char *nm = slot_name(idx);
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
    if (s_ln_count >= s_ln_cap && ln_grow() != 0) return -1;
    cpy_cap(s_ln_tab[s_ln_count].token, tok, (int)AFS_NAME_MAX);
    cpy_cap(s_ln_tab[s_ln_count].path, path, (int)AFS_PATH_MAX);
    s_ln_count++;
    return 0;
}

static int raw_read_all(uint32_t idx, void *buf, uint32_t size) {
    if (s_ver == 6) {
        uint32_t secs = (size + 511u) / 512u;
        if (secs == 0) secs = 1;
        return storage_read(H.files[idx].lba, secs, buf);
    }
    afs_v7_inode_t *in = v7_inode(idx);
    if (!in || in->ext_count == 0) return -1;
    afs_ext_t *exts = 0; uint32_t cnt = 0;
    if (v7_collect_extents(in, &exts, &cnt, 0, 0) != 0) return -1;
    uint8_t *p = (uint8_t *)buf;
    uint32_t rem = size;
    uint32_t off = 0;
    for (uint32_t e = 0; e < cnt && rem > 0; e++) {
        uint32_t secs = exts[e].count;
        uint32_t need_secs = (rem + 511u) / 512u;
        if (secs > need_secs) secs = need_secs;
        if (storage_read(exts[e].start, secs, p + off) != 0) { kfree(exts); return -1; }
        uint32_t got = secs * 512u;
        if (got > rem) got = rem;
        off += got;
        rem -= got;
    }
    kfree(exts);
    return 0;
}

static void ln_load(void) {
    s_ln_count = 0;
    int idx = find_raw_idx(AFS_LONGNAME_FILE);
    if (idx < 0) return;
    uint32_t sz = slot_size((uint32_t)idx);
    if (sz < (uint32_t)sizeof(afs_ln_hdr_t)) return;
    uint32_t secs = (sz + 511u) / 512u;
    if (secs == 0) secs = 1;
    uint8_t *buf = (uint8_t *)kmalloc((size_t)secs * 512u);
    if (!buf) return;
    if (raw_read_all((uint32_t)idx, buf, secs * 512u) != 0) {
        kfree(buf);
        return;
    }
    const afs_ln_hdr_t *lh = (const afs_ln_hdr_t *)buf;
    uint32_t max_ln = (s_ver == 7) ? AFS_V7_MAX_FILES : AFS_V6_MAX_FILES;
    if (lh->magic != AFS_LN_MAGIC || lh->version != AFS_LN_VERSION ||
        lh->count > max_ln) {
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
        if (s_ln_count >= s_ln_cap && ln_grow() != 0) break;
        cpy_cap(s_ln_tab[s_ln_count].token, ents[i].token, (int)AFS_NAME_MAX);
        cpy_cap(s_ln_tab[s_ln_count].path, ents[i].path, (int)AFS_PATH_MAX);
        s_ln_count++;
    }
    kfree(buf);
}

static int alloc_slot_internal(int prefer_high, uint32_t *out_idx) {
    if (s_ver == 6) {
        if (prefer_high) {
            for (int k = (int)H.max_files - 1; k >= 0; k--) {
                if (!H.files[k].used) { *out_idx = (uint32_t)k; return 0; }
            }
        } else {
            for (uint32_t i = 0; i < H.max_files; i++) {
                if (!H.files[i].used) { *out_idx = i; return 0; }
            }
        }
        return -1;
    }

    if (!prefer_high) {
        for (uint32_t c = 0; c < s_v7_sb.num_chunks; c++) {
            if (s_v7_chunk_used[c] >= AFS_V7_CHUNK_ENTRIES || !s_v7_chunks[c]) continue;
            for (uint32_t i = 0; i < AFS_V7_CHUNK_ENTRIES; i++) {
                if (!s_v7_chunks[c]->inodes[i].used) {
                    *out_idx = c * AFS_V7_CHUNK_ENTRIES + i;
                    return 0;
                }
            }
        }
    } else {
        for (int c = (int)s_v7_sb.num_chunks - 1; c >= 0; c--) {
            if (s_v7_chunk_used[c] >= AFS_V7_CHUNK_ENTRIES || !s_v7_chunks[c]) continue;
            for (int i = (int)AFS_V7_CHUNK_ENTRIES - 1; i >= 0; i--) {
                if (!s_v7_chunks[c]->inodes[i].used) {
                    *out_idx = (uint32_t)c * AFS_V7_CHUNK_ENTRIES + (uint32_t)i;
                    return 0;
                }
            }
        }
    }

    /* Tum mevcut parcalar dolu: talep uzerine yeni 256-girdili parca ayir */
    if (s_v7_sb.num_chunks >= AFS_V7_MAX_CHUNKS) return -1;
    uint32_t chunk_lba = free_ext_alloc(AFS_V7_CHUNK_SECS);
    if (chunk_lba == 0) return -1;
    afs_v7_chunk_t *nc = (afs_v7_chunk_t *)kmalloc(sizeof(afs_v7_chunk_t));
    if (!nc) {
        free_ext_add(chunk_lba, AFS_V7_CHUNK_SECS);
        return -1;
    }
    zero_mem(nc, sizeof(afs_v7_chunk_t));
    for (uint32_t i = 0; i < AFS_V7_CHUNK_ENTRIES; i++) nc->gen[i] = 1u;
    nc->dirty_mask = ~0ULL;

    uint32_t c = s_v7_sb.num_chunks++;
    s_v7_chunk_lba[c] = chunk_lba;
    s_v7_chunks[c] = nc;
    s_v7_chunk_used[c] = 0;
    s_v7_dir_dirty = 1;

    *out_idx = c * AFS_V7_CHUNK_ENTRIES + (prefer_high ? (AFS_V7_CHUNK_ENTRIES - 1u) : 0u);
    return 0;
}

static int ln_persist_no_commit(void) {
    int idx = find_raw_idx(AFS_LONGNAME_FILE);
    if (s_ln_count == 0) {
        if (idx >= 0) {
            if (s_ver == 6) {
                uint32_t secs = v6_file_secs(&H.files[idx]);
                free_ext_add(H.files[idx].lba, secs);
                H.files[idx].used = 0; H.files[idx].size = 0;
                H.files[idx].lba = 0; H.files[idx].name[0] = 0;
            } else {
                afs_v7_inode_t *in = v7_inode((uint32_t)idx);
                if (in) {
                    v7_free_inode_extents(in);
                    zero_mem(in, sizeof(*in));
                    s_v7_chunk_used[(uint32_t)idx >> 8]--;
                    v7_mark_dirty((uint32_t)idx);
                }
            }
            slot_bump_gen((uint32_t)idx);
            meta_dec_count();
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

    if (s_ver == 6) {
        if (idx < 0) {
            uint32_t nidx = 0;
            if (alloc_slot_internal(1, &nidx) != 0) { kfree(buf); return -1; }
            uint32_t lba = free_ext_alloc(secs);
            if (lba == 0) { kfree(buf); return -1; }
            if (storage_write(lba, secs, buf) != 0) {
                free_ext_add(lba, secs); kfree(buf); return -1;
            }
            H.files[nidx].used = 1; H.files[nidx].lba = lba; H.files[nidx].size = data_sz;
            H.files[nidx].uid = 0; H.files[nidx].gid = 0; H.files[nidx].mode = 0600u; H.files[nidx].pad = 0;
            cpy(H.files[nidx].name, AFS_LONGNAME_FILE);
            meta_inc_count();
            slot_bump_gen(nidx);
        } else {
            uint32_t old_lba = H.files[idx].lba;
            uint32_t old_secs = v6_file_secs(&H.files[idx]);
            if (secs <= old_secs) {
                if (storage_write(old_lba, secs, buf) != 0) { kfree(buf); return -1; }
                if (secs < old_secs) free_ext_add(old_lba + secs, old_secs - secs);
                H.files[idx].size = data_sz;
            } else {
                uint32_t new_lba = free_ext_alloc(secs);
                if (new_lba == 0) { kfree(buf); return -1; }
                if (storage_write(new_lba, secs, buf) != 0) {
                    free_ext_add(new_lba, secs); kfree(buf); return -1;
                }
                free_ext_add(old_lba, old_secs);
                H.files[idx].lba = new_lba;
                H.files[idx].size = data_sz;
            }
        }
    } else {
        afs_ext_t *exts = 0; uint32_t ecnt = 0;
        if (v7_alloc_extents(secs, &exts, &ecnt) != 0) { kfree(buf); return -1; }
        if (v7_write_data(exts, ecnt, buf, secs * 512u) != 0) {
            for (uint32_t k = 0; k < ecnt; k++) free_ext_add(exts[k].start, exts[k].count);
            kfree(exts); kfree(buf); return -1;
        }
        uint32_t target_idx = 0;
        int is_new = (idx < 0);
        if (is_new) {
            if (alloc_slot_internal(1, &target_idx) != 0) {
                for (uint32_t k = 0; k < ecnt; k++) free_ext_add(exts[k].start, exts[k].count);
                kfree(exts); kfree(buf); return -1;
            }
        } else {
            target_idx = (uint32_t)idx;
        }
        afs_v7_inode_t *in = v7_inode(target_idx);
        afs_v7_inode_t saved;
        if (!is_new) {
            saved = *in;
            v7_free_inode_extents(in);
        } else {
            zero_mem(in, sizeof(*in));
            in->used = 1;
            in->uid = 0; in->gid = 0; in->mode = 0600u;
            cpy(in->name, AFS_LONGNAME_FILE);
        }
        if (v7_store_extents(in, exts, ecnt) != 0) {
            for (uint32_t k = 0; k < ecnt; k++) free_ext_add(exts[k].start, exts[k].count);
            if (!is_new) *in = saved;
            else zero_mem(in, sizeof(*in));
            kfree(exts); kfree(buf); return -1;
        }
        in->size = data_sz;
        if (is_new) {
            s_v7_chunk_used[target_idx >> 8]++;
            meta_inc_count();
            slot_bump_gen(target_idx);
        }
        v7_mark_dirty(target_idx);
        kfree(exts);
    }

    kfree(buf);
    return 0;
}

/* ---- Format & Mount ---- */

void afs_format(void) {
    if (afs_ensure_v7_mem() != 0) return;
    free_v7_chunks();

    if (storage_partition_base() > 0 &&
        storage_sector_count() > 0 &&
        storage_sector_count() < AFS_BASE_LBA + 4096u) {
        s_base = 0;
    } else {
        s_base = AFS_BASE_LBA;
    }

    s_ver = 7;
    zero_mem(&s_v7_sb, sizeof(s_v7_sb));
    s_v7_sb.magic = AFS_MAGIC;
    s_v7_sb.version = AFS_VERSION;
    s_v7_sb.max_files = AFS_V7_MAX_FILES;
    s_v7_sb.count = 0;
    s_v7_sb.hdr_sectors = AFS_V7_HDR_SECS;
    s_v7_sb.capacity = compute_capacity();
    if (s_v7_sb.capacity > (uint64_t)s_base + AFS_JOURNAL_SEC)
        s_v7_sb.capacity -= AFS_JOURNAL_SEC;
    s_v7_sb.inode_dir_lba = s_base + 1u;
    s_v7_sb.inode_dir_secs = AFS_V7_DIR_SECS;
    s_v7_sb.inode_blk_secs = AFS_V7_CHUNK_SECS;
    s_v7_sb.chunk_cap = AFS_V7_CHUNK_ENTRIES;
    s_v7_sb.num_chunks = 1u;

    s_v7_chunk_lba[0] = s_base + AFS_V7_HDR_SECS;
    s_v7_sb.data_next = s_v7_chunk_lba[0] + AFS_V7_CHUNK_SECS;

    afs_v7_chunk_t *c0 = (afs_v7_chunk_t *)kmalloc(sizeof(afs_v7_chunk_t));
    if (!c0) return;
    zero_mem(c0, sizeof(afs_v7_chunk_t));
    for (uint32_t i = 0; i < AFS_V7_CHUNK_ENTRIES; i++) c0->gen[i] = 1u;
    c0->dirty_mask = ~0ULL;
    s_v7_chunks[0] = c0;
    s_v7_chunk_used[0] = 0;
    s_v7_dir_dirty = 1;

    s_free_count = 0;
    s_reclaimed_secs = 0;
    s_reused_secs = 0;
    s_ln_count = 0;
    mounted = 1;
    afs_commit();
    afs_ensure_tree();
}

void afs_mount(void) {
    uint32_t first[128];
    if (s_locked) return;
    if (storage_sector_count() == 0) storage_init();

    uint32_t det_base = AFS_BASE_LBA;
    uint32_t det_ver = 0;
    if (!afs_probe_base(&det_base, &det_ver)) {
        if (afs_disk_is_encrypted()) return;
        afs_format();
        return;
    }
    s_base = det_base;
    journal_replay();

    if (storage_read(s_base, 1, first) != 0 || first[0] != AFS_MAGIC ||
        (first[1] != AFS_VERSION && first[1] != AFS_VERSION_V6 && first[1] != 5u)) {
        if (afs_disk_is_encrypted()) return;
        afs_format();
        return;
    }

    if (first[1] == AFS_VERSION) {
        /* ---- AFS v7 mount ---- */
        if (afs_ensure_v7_mem() != 0) return;
        free_v7_chunks();
        copy_mem(&s_v7_sb, first, sizeof(s_v7_sb));

        if (s_v7_sb.flags != v7_sb_crc(&s_v7_sb) ||
            s_v7_sb.max_files < 256u || s_v7_sb.max_files > AFS_V7_MAX_FILES ||
            s_v7_sb.chunk_cap != AFS_V7_CHUNK_ENTRIES ||
            s_v7_sb.inode_blk_secs != AFS_V7_CHUNK_SECS ||
            s_v7_sb.inode_dir_secs != AFS_V7_DIR_SECS ||
            s_v7_sb.hdr_sectors != AFS_V7_HDR_SECS ||
            s_v7_sb.num_chunks == 0 || s_v7_sb.num_chunks > AFS_V7_MAX_CHUNKS) {
            if (afs_disk_is_encrypted()) return;
            afs_format();
            return;
        }
        if (storage_read(s_v7_sb.inode_dir_lba, AFS_V7_DIR_SECS, s_v7_chunk_lba) != 0) {
            if (afs_disk_is_encrypted()) return;
            afs_format();
            return;
        }
        if (crc32_buf(s_v7_chunk_lba, (size_t)AFS_V7_MAX_CHUNKS * sizeof(uint32_t)) != s_v7_sb.inode_dir_crc) {
            if (afs_disk_is_encrypted()) return;
            afs_format();
            return;
        }
        for (uint32_t c = 0; c < s_v7_sb.num_chunks; c++) {
            if (s_v7_chunk_lba[c] < s_base + AFS_V7_HDR_SECS ||
                (uint64_t)s_v7_chunk_lba[c] + AFS_V7_CHUNK_SECS > s_v7_sb.capacity) {
                free_v7_chunks();
                if (afs_disk_is_encrypted()) return;
                afs_format();
                return;
            }
            afs_v7_chunk_t *ck = (afs_v7_chunk_t *)kmalloc(sizeof(afs_v7_chunk_t));
            if (!ck) { free_v7_chunks(); return; }
            zero_mem(ck, sizeof(afs_v7_chunk_t));
            if (storage_read(s_v7_chunk_lba[c], AFS_V7_CHUNK_SECS, ck->inodes) != 0) {
                kfree(ck);
                free_v7_chunks();
                return;
            }
            uint16_t u = 0;
            for (uint32_t i = 0; i < AFS_V7_CHUNK_ENTRIES; i++) {
                ck->gen[i] = 1u;
                if (ck->inodes[i].used) u++;
            }
            s_v7_chunks[c] = ck;
            s_v7_chunk_used[c] = u;
        }
        s_ver = 7;
        s_v7_dir_dirty = 0;
        mounted = 1;
        s_reclaimed_secs = 0;
        s_reused_secs = 0;
        ln_load();
        free_ext_rebuild();
        afs_ensure_tree();
        serial_printf("[afs] mount ver=7 max_files=%u chunks=%u count=%u free_extents=%u data_next=%u\n",
                      s_v7_sb.max_files, s_v7_sb.num_chunks, s_v7_sb.count, s_free_count, s_v7_sb.data_next);
        return;
    }

    /* ---- AFS v6 / v5 geriye donuk mount ---- */
    if (afs_ensure_v6_mem() != 0) return;
    H.magic = first[0];
    H.version = first[1];
    H.max_files = first[2];
    H.count = first[3];
    H.data_next = first[4];
    H.hdr_sectors = first[5];
    H.capacity = (uint64_t)first[6] | ((uint64_t)first[7] << 32);
    H.flags = first[8];

    if (H.max_files < 32u || H.max_files > AFS_V6_MAX_FILES ||
        H.hdr_sectors == 0 || H.hdr_sectors > AFS_HDR_MAX_SEC) {
        if (afs_disk_is_encrypted()) return;
        afs_format();
        return;
    }
    if (v6_hdr_load_at(s_base, H.hdr_sectors) != 0) {
        if (afs_disk_is_encrypted()) return;
        afs_format();
        return;
    }
    if (v6_hdr_crc() != H.flags) {
        if (afs_disk_is_encrypted()) return;
        afs_format();
        return;
    }
    s_ver = 6;
    mounted = 1;
    if (H.version != AFS_VERSION_V6) {
        H.version = AFS_VERSION_V6;
        v6_commit();
    }
    s_reclaimed_secs = 0;
    s_reused_secs = 0;
    ln_load();
    free_ext_rebuild();
    afs_ensure_tree();
    serial_printf("[afs] mount ver=%u (compat) max_files=%u count=%u free_extents=%u reclaimed=%u data_next=%u\n",
                  H.version, H.max_files, H.count, s_free_count, s_reclaimed_secs, H.data_next);
}

int afs_count(void) { if (!mounted) afs_mount(); return mounted ? (int)meta_count() : 0; }

uint32_t afs_data_next(void) { if (!mounted) afs_mount(); return mounted ? meta_data_next() : 0; }

static int find_idx(const char *name) {
    if (!name || !name[0] || !mounted) return -1;
    int nlen = slen(name);
    if (nlen < (int)AFS_NAME_MAX) {
        return find_raw_idx(name);
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
    uint32_t g = slot_get_gen((uint32_t)slot);
    return (void *)(uintptr_t)(((uint64_t)(uint32_t)slot << 14) | ((uint64_t)(g & 0x1FFFu) << 1) | 1ull);
}

static int hnd_dec(void *h) {
    if (!mounted) return -1;
    uintptr_t v = (uintptr_t)h;
    if ((v & 1u) == 0) return -1;
    int slot = (int)(v >> 14);
    if (slot < 0 || slot >= (int)slot_capacity() || !slot_used((uint32_t)slot)) return -1;
    if (((v >> 1) & 0x1FFFu) != (slot_get_gen((uint32_t)slot) & 0x1FFFu)) return -1;
    return slot;
}

uint32_t afs_file_extent_count(void *h) {
    int i = hnd_dec(h);
    if (i < 0) return 0;
    if (s_ver == 6) return 1u;
    afs_v7_inode_t *in = v7_inode((uint32_t)i);
    return in ? (uint32_t)in->ext_count : 0;
}

void *afs_create_handle(const char *name) {
    if (!mounted) afs_mount();
    if (!mounted || !name || !name[0]) return 0;
    int nlen = slen(name);
    if (nlen >= (int)AFS_PATH_MAX) return 0;
    if (seq(name, AFS_LONGNAME_FILE)) return 0;

    int existing = find_idx(name);
    if (existing >= 0) return hnd_enc(existing);

    int pi = parent_idx(name);
    if (pi >= 0) {
        uint32_t puid, pgid, pmode;
        slot_get_perm((uint32_t)pi, &puid, &pgid, &pmode);
        if (!perm_ok(puid, pgid, pmode, AFS_W)) return 0;
    }

    int is_long = (nlen >= (int)AFS_NAME_MAX);
    int isd = afs_is_dir(name);
    char disk_name[AFS_NAME_MAX];

    if (is_long) {
        ln_make_token(name, isd, disk_name);
        if (ln_add_entry(disk_name, name) != 0) return 0;
        if (ln_persist_no_commit() != 0) {
            ln_remove_token(disk_name);
            return 0;
        }
    } else {
        cpy(disk_name, name);
    }

    uint32_t i = 0;
    if (alloc_slot_internal(0, &i) != 0) {
        if (is_long) {
            ln_remove_token(disk_name);
            ln_persist_no_commit();
        }
        return 0;
    }

    uint32_t mode = isd ? 0777u : 0666u;
    if (s_ver == 6) {
        uint32_t lba = free_ext_alloc(1u);
        if (lba == 0) {
            if (is_long) { ln_remove_token(disk_name); ln_persist_no_commit(); }
            return 0;
        }
        H.files[i].used = 1;
        H.files[i].size = 0;
        H.files[i].lba = lba;
        H.files[i].uid = s_uid;
        H.files[i].gid = s_gid;
        H.files[i].mode = mode;
        H.files[i].pad = 0;
        cpy(H.files[i].name, disk_name);
    } else {
        afs_v7_inode_t *in = v7_inode(i);
        zero_mem(in, sizeof(*in));
        in->used = 1;
        in->flags = isd ? 1u : 0u;
        in->ext_count = 0;
        in->size = 0;
        in->uid = s_uid;
        in->gid = s_gid;
        in->mode = mode;
        cpy(in->name, disk_name);
        s_v7_chunk_used[i >> 8]++;
        v7_mark_dirty(i);
    }

    meta_inc_count();
    slot_bump_gen(i);
    afs_commit();
    return hnd_enc((int)i);
}

void *afs_find_handle(const char *name) {
    if (!mounted) afs_mount();
    int i = find_idx(name);
    return i < 0 ? 0 : hnd_enc(i);
}

int afs_rename(const char *from, const char *to) {
    if (!mounted) afs_mount();
    if (!mounted || !from || !to || !from[0] || !to[0]) return -1;
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
    if (seq(slot_name((uint32_t)fi), AFS_LONGNAME_FILE)) return -1;

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
        uint32_t u, g, m; slot_get_perm((uint32_t)spi, &u, &g, &m);
        if (!perm_ok(u, g, m, AFS_W)) return -1;
    } else {
        uint32_t u, g, m; slot_get_perm((uint32_t)fi, &u, &g, &m);
        if (!perm_ok(u, g, m, AFS_W)) return -1;
    }
    int tpi = parent_idx(tbuf);
    if (tpi >= 0 && tpi != spi) {
        uint32_t u, g, m; slot_get_perm((uint32_t)tpi, &u, &g, &m);
        if (!perm_ok(u, g, m, AFS_W)) return -1;
    }

    int ln_dirty = 0;
    int fl = slen(fbuf);
    char cur_raw[AFS_NAME_MAX];
    cpy(cur_raw, slot_name((uint32_t)fi));

    char *fi_name_dst = (s_ver == 7) ? v7_inode((uint32_t)fi)->name : H.files[fi].name;
    if (slen(tbuf) >= (int)AFS_NAME_MAX) {
        char ntok[AFS_NAME_MAX];
        ln_make_token(tbuf, isd, ntok);
        if (is_ln_token(cur_raw)) {
            int k = ln_find_by_token(cur_raw);
            if (k >= 0) {
                cpy_cap(s_ln_tab[k].token, ntok, (int)AFS_NAME_MAX);
                cpy_cap(s_ln_tab[k].path, tbuf, (int)AFS_PATH_MAX);
            } else if (ln_add_entry(ntok, tbuf) != 0) return -1;
        } else if (ln_add_entry(ntok, tbuf) != 0) return -1;
        cpy(fi_name_dst, ntok);
        ln_dirty = 1;
    } else {
        if (is_ln_token(cur_raw)) { ln_remove_token(cur_raw); ln_dirty = 1; }
        cpy(fi_name_dst, tbuf);
    }
    if (s_ver == 7) v7_mark_dirty((uint32_t)fi);

    if (isd) {
        uint32_t cap = slot_capacity();
        for (uint32_t j = 0; j < cap; j++) {
            if ((int)j == fi || !slot_used(j)) continue;
            const char *jraw = slot_name(j);
            if (seq(jraw, AFS_LONGNAME_FILE)) continue;
            const char *cf = entry_full_name(j);
            if (!prefix_of(cf, fbuf)) continue;
            int cl = slen(cf);
            if (slen(tbuf) + (cl - fl) >= (int)AFS_PATH_MAX) continue;
            char nb[AFS_PATH_MAX];
            int n = 0;
            for (int k = 0; tbuf[k] && n < (int)AFS_PATH_MAX - 1; k++) nb[n++] = tbuf[k];
            for (int k = fl; cf[k] && n < (int)AFS_PATH_MAX - 1; k++) nb[n++] = cf[k];
            nb[n] = '\0';

            char *j_dst = (s_ver == 7) ? v7_inode(j)->name : H.files[j].name;
            char jcur[AFS_NAME_MAX]; cpy(jcur, jraw);
            int child_dir = afs_is_dir(nb);
            if (slen(nb) >= (int)AFS_NAME_MAX) {
                char ctok[AFS_NAME_MAX];
                ln_make_token(nb, child_dir, ctok);
                if (is_ln_token(jcur)) {
                    int k = ln_find_by_token(jcur);
                    if (k >= 0) {
                        cpy_cap(s_ln_tab[k].token, ctok, (int)AFS_NAME_MAX);
                        cpy_cap(s_ln_tab[k].path, nb, (int)AFS_PATH_MAX);
                    } else if (ln_add_entry(ctok, nb) != 0) continue;
                } else if (ln_add_entry(ctok, nb) != 0) continue;
                cpy(j_dst, ctok);
                ln_dirty = 1;
            } else {
                if (is_ln_token(jcur)) { ln_remove_token(jcur); ln_dirty = 1; }
                cpy(j_dst, nb);
            }
            if (s_ver == 7) v7_mark_dirty(j);
        }
    }

    if (ln_dirty) (void)ln_persist_no_commit();
    afs_commit();
    return 0;
}

static int v6_write_handle_inner(int i, const void *data, uint32_t size) {
    uint32_t old_lba = H.files[i].lba;
    uint32_t old_secs = v6_file_secs(&H.files[i]);
    uint64_t secs64 = ((uint64_t)size + 511u) / 512u;
    if (secs64 == 0) secs64 = 1;
    if (secs64 > 0xFFFFFFFFull) return -1;
    uint32_t new_secs = (uint32_t)secs64;

    afs_ext_t ext;
    if (new_secs <= old_secs) {
        if ((uint64_t)old_lba + new_secs > H.capacity) return -1;
        ext.start = old_lba; ext.count = new_secs;
        if (v7_write_data(&ext, 1, data, size) != 0) return -1;
        H.files[i].size = size;
        if (new_secs < old_secs)
            free_ext_add(old_lba + new_secs, old_secs - new_secs);
        v6_commit();
        return 0;
    }

    uint32_t extra = new_secs - old_secs;
    uint32_t tail_lba = old_lba + old_secs;
    if (tail_lba == H.data_next && (uint64_t)old_lba + new_secs <= H.capacity) {
        ext.start = old_lba; ext.count = new_secs;
        if (v7_write_data(&ext, 1, data, size) != 0) return -1;
        H.files[i].size = size;
        H.data_next = old_lba + new_secs;
        v6_commit();
        return 0;
    }
    if (free_ext_consume_at(tail_lba, extra)) {
        ext.start = old_lba; ext.count = new_secs;
        if (v7_write_data(&ext, 1, data, size) != 0) {
            free_ext_add(tail_lba, extra);
            return -1;
        }
        H.files[i].size = size;
        v6_commit();
        return 0;
    }

    uint32_t new_lba = free_ext_alloc(new_secs);
    if (new_lba == 0) return -1;
    ext.start = new_lba; ext.count = new_secs;
    if (v7_write_data(&ext, 1, data, size) != 0) {
        free_ext_add(new_lba, new_secs);
        return -1;
    }
    H.files[i].lba = new_lba;
    H.files[i].size = size;
    free_ext_add(old_lba, old_secs);
    v6_commit();
    return 0;
}

static int v7_write_handle_inner(int i, const void *data, uint32_t size) {
    afs_v7_inode_t *in = v7_inode((uint32_t)i);
    if (!in) return -1;

    uint64_t secs64 = ((uint64_t)size + 511u) / 512u;
    if (secs64 > 0xFFFFFFFFull) return -1;
    uint32_t new_secs = (uint32_t)secs64;

    if (new_secs == 0) {
        v7_free_inode_extents(in);
        in->size = 0;
        v7_mark_dirty((uint32_t)i);
        v7_commit();
        return 0;
    }

    uint32_t old_secs = v7_total_secs(in);

    /* Hizli yol: tek extent yerinde kucultme veya kuyruk genisletme */
    if (in->ext_count == 1 && in->ext_ind_lba == 0) {
        uint32_t old_lba = in->extents[0].lba;
        if (new_secs <= old_secs) {
            afs_ext_t ext = { old_lba, new_secs };
            if (v7_write_data(&ext, 1, data, size) != 0) return -1;
            in->extents[0].sectors = new_secs;
            in->size = size;
            if (new_secs < old_secs)
                free_ext_add(old_lba + new_secs, old_secs - new_secs);
            v7_mark_dirty((uint32_t)i);
            v7_commit();
            return 0;
        }
        uint32_t extra = new_secs - old_secs;
        uint32_t tail_lba = old_lba + old_secs;
        if (tail_lba == s_v7_sb.data_next && (uint64_t)old_lba + new_secs <= s_v7_sb.capacity) {
            afs_ext_t ext = { old_lba, new_secs };
            if (v7_write_data(&ext, 1, data, size) != 0) return -1;
            in->extents[0].sectors = new_secs;
            in->size = size;
            s_v7_sb.data_next = old_lba + new_secs;
            v7_mark_dirty((uint32_t)i);
            v7_commit();
            return 0;
        }
        if (free_ext_consume_at(tail_lba, extra)) {
            afs_ext_t ext = { old_lba, new_secs };
            if (v7_write_data(&ext, 1, data, size) != 0) {
                free_ext_add(tail_lba, extra);
                return -1;
            }
            in->extents[0].sectors = new_secs;
            in->size = size;
            v7_mark_dirty((uint32_t)i);
            v7_commit();
            return 0;
        }
    }

    /* Coklu extent: mevcut alan yeterliyse yerinde uzerine yaz + fazlaligi kirp */
    if (old_secs > 0 && new_secs <= old_secs) {
        afs_ext_t *exts = 0; uint32_t ecnt = 0;
        uint32_t *inds = 0; uint32_t icnt = 0;
        if (v7_collect_extents(in, &exts, &ecnt, &inds, &icnt) != 0) return -1;
        uint32_t keep = 0, rem = new_secs;
        while (keep < ecnt && rem > 0) {
            if (exts[keep].count <= rem) {
                rem -= exts[keep].count;
                keep++;
            } else {
                free_ext_add(exts[keep].start + rem, exts[keep].count - rem);
                exts[keep].count = rem;
                rem = 0;
                keep++;
            }
        }
        for (uint32_t k = keep; k < ecnt; k++)
            free_ext_add(exts[k].start, exts[k].count);
        for (uint32_t k = 0; k < icnt; k++)
            free_ext_add(inds[k], 1u);
        if (inds) kfree(inds);

        if (v7_write_data(exts, keep, data, size) != 0) { kfree(exts); return -1; }
        if (v7_store_extents(in, exts, keep) != 0) { kfree(exts); return -1; }
        in->size = size;
        kfree(exts);
        v7_mark_dirty((uint32_t)i);
        v7_commit();
        return 0;
    }

    /* Buyuyen coklu-extent yazim: once CoW (yeni tahsis), yer sikisiysa ekleme */
    afs_ext_t *new_exts = 0;
    uint32_t new_cnt = 0;
    if (v7_alloc_extents(new_secs, &new_exts, &new_cnt) == 0) {
        if (v7_write_data(new_exts, new_cnt, data, size) != 0) {
            for (uint32_t k = 0; k < new_cnt; k++) free_ext_add(new_exts[k].start, new_exts[k].count);
            kfree(new_exts);
            return -1;
        }
        afs_v7_inode_t saved = *in;
        if (v7_store_extents(in, new_exts, new_cnt) != 0) {
            *in = saved;
            for (uint32_t k = 0; k < new_cnt; k++) free_ext_add(new_exts[k].start, new_exts[k].count);
            kfree(new_exts);
            return -1;
        }
        in->size = size;
        kfree(new_exts);
        v7_free_inode_extents(&saved);
        v7_mark_dirty((uint32_t)i);
        v7_commit();
        return 0;
    }

    /* Yer sikisik: eski extentleri koruyup sadece (new_secs - old_secs) kadar ekle */
    if (old_secs > 0) {
        uint32_t extra = new_secs - old_secs;
        afs_ext_t *old_exts = 0; uint32_t old_cnt = 0;
        uint32_t *old_inds = 0; uint32_t old_icnt = 0;
        if (v7_collect_extents(in, &old_exts, &old_cnt, &old_inds, &old_icnt) != 0) return -1;
        afs_ext_t *ext_add = 0; uint32_t add_cnt = 0;
        if (v7_alloc_extents(extra, &ext_add, &add_cnt) != 0) {
            kfree(old_exts); if (old_inds) kfree(old_inds);
            return -1;
        }
        afs_ext_t *comb = (afs_ext_t *)kmalloc((size_t)(old_cnt + add_cnt) * sizeof(afs_ext_t));
        if (!comb) {
            for (uint32_t k = 0; k < add_cnt; k++) free_ext_add(ext_add[k].start, ext_add[k].count);
            kfree(ext_add); kfree(old_exts); if (old_inds) kfree(old_inds);
            return -1;
        }
        uint32_t ccnt = old_cnt;
        copy_mem(comb, old_exts, (size_t)old_cnt * sizeof(afs_ext_t));
        for (uint32_t k = 0; k < add_cnt; k++) {
            if (ccnt > 0 && comb[ccnt - 1u].start + comb[ccnt - 1u].count == ext_add[k].start)
                comb[ccnt - 1u].count += ext_add[k].count;
            else
                comb[ccnt++] = ext_add[k];
        }
        if (v7_write_data(comb, ccnt, data, size) != 0) {
            for (uint32_t k = 0; k < add_cnt; k++) free_ext_add(ext_add[k].start, ext_add[k].count);
            kfree(comb); kfree(ext_add); kfree(old_exts); if (old_inds) kfree(old_inds);
            return -1;
        }
        for (uint32_t k = 0; k < old_icnt; k++) free_ext_add(old_inds[k], 1u);
        if (v7_store_extents(in, comb, ccnt) != 0) {
            kfree(comb); kfree(ext_add); kfree(old_exts); if (old_inds) kfree(old_inds);
            return -1;
        }
        in->size = size;
        kfree(comb); kfree(ext_add); kfree(old_exts); if (old_inds) kfree(old_inds);
        v7_mark_dirty((uint32_t)i);
        v7_commit();
        return 0;
    }

    return -1;
}

int afs_write_handle(void *h, const void *data, uint32_t size) {
    int i = hnd_dec(h);
    if (i < 0) return -1;
    uint32_t uid, gid, mode;
    slot_get_perm((uint32_t)i, &uid, &gid, &mode);
    if (!perm_ok(uid, gid, mode, AFS_W)) return -1;
    if (s_ver == 7) return v7_write_handle_inner(i, data, size);
    return v6_write_handle_inner(i, data, size);
}

static uint32_t read_extent_slice(uint32_t ext_lba, uint32_t ext_secs,
                                  uint32_t in_ext_off, uint8_t *dst, uint32_t want) {
    uint32_t ext_bytes = ext_secs * 512u;
    if (in_ext_off >= ext_bytes || want == 0) return 0;
    uint32_t n = ext_bytes - in_ext_off;
    if (want < n) n = want;

    uint8_t sec[512];
    uint32_t pos = in_ext_off, done = 0;
    uint32_t inoff = pos % 512u;
    if (inoff != 0) {
        uint32_t chunk = 512u - inoff;
        if (chunk > n) chunk = n;
        if (storage_read(ext_lba + pos / 512u, 1, sec) != 0) return 0;
        for (uint32_t k = 0; k < chunk; k++) dst[done + k] = sec[inoff + k];
        done += chunk;
        pos += chunk;
    }
    while (n - done >= 512u) {
        uint32_t secs = (n - done) / 512u;
        if (secs > 512u) secs = 512u;
        if (storage_read(ext_lba + pos / 512u, secs, dst + done) != 0) break;
        uint32_t bytes = secs * 512u;
        done += bytes;
        pos += bytes;
    }
    if (done < n && (pos % 512u) == 0) {
        uint32_t rem = n - done;
        if (storage_read(ext_lba + pos / 512u, 1, sec) == 0) {
            for (uint32_t k = 0; k < rem; k++) dst[done + k] = sec[k];
            done += rem;
        }
    }
    return done;
}

uint32_t afs_read_at(void *h, void *buf, uint32_t size, uint32_t off) {
    int i = hnd_dec(h);
    if (i < 0) return 0;
    uint32_t uid, gid, mode;
    slot_get_perm((uint32_t)i, &uid, &gid, &mode);
    if (!perm_ok(uid, gid, mode, AFS_R)) return 0;

    uint32_t fsz = slot_size((uint32_t)i);
    if (off >= fsz) return 0;
    uint32_t n = fsz - off;
    if (size < n) n = size;
    if (n == 0) return 0;

    if (s_ver == 6) {
        return read_extent_slice(H.files[i].lba, v6_file_secs(&H.files[i]),
                                 off, (uint8_t *)buf, n);
    }

    afs_v7_inode_t *in = v7_inode((uint32_t)i);
    if (!in || in->ext_count == 0) return 0;
    if (in->ext_count == 1 && in->ext_ind_lba == 0) {
        return read_extent_slice(in->extents[0].lba, in->extents[0].sectors,
                                 off, (uint8_t *)buf, n);
    }

    afs_ext_t *exts = 0; uint32_t ecnt = 0;
    if (v7_collect_extents(in, &exts, &ecnt, 0, 0) != 0) return 0;

    uint8_t *p = (uint8_t *)buf;
    uint32_t done = 0;
    uint64_t cur_byte = 0;
    for (uint32_t e = 0; e < ecnt && done < n; e++) {
        uint64_t elen = (uint64_t)exts[e].count * 512ull;
        uint64_t cur_pos = (uint64_t)off + done;
        if (cur_pos < cur_byte + elen) {
            uint32_t in_off = (uint32_t)(cur_pos - cur_byte);
            uint32_t got = read_extent_slice(exts[e].start, exts[e].count,
                                             in_off, p + done, n - done);
            done += got;
            if (got == 0) break;
        }
        cur_byte += elen;
    }
    kfree(exts);
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
    if (!mounted || !name || !name[0]) return -1;
    if (seq(name, AFS_LONGNAME_FILE)) return -1;
    int i = find_idx(name);
    if (i < 0) return -1;

    uint32_t fuid, fgid, fmode;
    slot_get_perm((uint32_t)i, &fuid, &fgid, &fmode);
    int pi = parent_idx(name);
    if (pi >= 0) {
        uint32_t puid, pgid, pmode;
        slot_get_perm((uint32_t)pi, &puid, &pgid, &pmode);
        if (!perm_ok(puid, pgid, pmode, AFS_W)) return -1;
    }
    if (!perm_ok(fuid, fgid, fmode, AFS_W)) return -1;

    char old_tok[AFS_NAME_MAX];
    const char *raw_nm = slot_name((uint32_t)i);
    int was_long = is_ln_token(raw_nm);
    if (was_long) cpy(old_tok, raw_nm);

    if (s_ver == 6) {
        uint32_t old_lba = H.files[i].lba;
        uint32_t old_secs = v6_file_secs(&H.files[i]);
        H.files[i].used = 0; H.files[i].size = 0;
        H.files[i].lba = 0; H.files[i].name[0] = 0;
        slot_bump_gen((uint32_t)i);
        meta_dec_count();
        free_ext_add(old_lba, old_secs);
    } else {
        afs_v7_inode_t *in = v7_inode((uint32_t)i);
        v7_free_inode_extents(in);
        zero_mem(in, sizeof(*in));
        s_v7_chunk_used[(uint32_t)i >> 8]--;
        v7_mark_dirty((uint32_t)i);
        slot_bump_gen((uint32_t)i);
        meta_dec_count();
    }

    if (was_long) {
        ln_remove_token(old_tok);
        (void)ln_persist_no_commit();
    }
    afs_commit();
    return 0;
}

const char *afs_name(int idx) {
    if (!mounted) afs_mount();
    if (!mounted || idx < 0 || idx >= (int)slot_capacity() || !slot_used((uint32_t)idx)) return 0;
    if (seq(slot_name((uint32_t)idx), AFS_LONGNAME_FILE)) return 0;
    return entry_full_name((uint32_t)idx);
}

uint32_t afs_size(int idx) {
    if (!mounted) afs_mount();
    if (!mounted || idx < 0 || idx >= (int)slot_capacity() || !slot_used((uint32_t)idx)) return 0;
    if (seq(slot_name((uint32_t)idx), AFS_LONGNAME_FILE)) return 0;
    return slot_size((uint32_t)idx);
}

int afs_stat(const char *path, asi_stat_t *out) {
    if (!mounted) afs_mount();
    int i = find_idx(path);
    if (i < 0 || !out) return -1;
    slot_get_perm((uint32_t)i, &out->uid, &out->gid, &out->mode);
    if (s_ver == 7) {
        afs_v7_inode_t *in = v7_inode((uint32_t)i);
        out->is_dir = (in && (in->flags & 1u)) ? 1u : 0u;
    } else {
        out->is_dir = afs_is_dir(H.files[i].name) ? 1u : 0u;
    }
    out->size = slot_size((uint32_t)i);
    return 0;
}

int afs_stat_handle(void *h, asi_stat_t *out) {
    int i = hnd_dec(h);
    if (i < 0 || !out) return -1;
    slot_get_perm((uint32_t)i, &out->uid, &out->gid, &out->mode);
    if (s_ver == 7) {
        afs_v7_inode_t *in = v7_inode((uint32_t)i);
        out->is_dir = (in && (in->flags & 1u)) ? 1u : 0u;
    } else {
        out->is_dir = afs_is_dir(H.files[i].name) ? 1u : 0u;
    }
    out->size = slot_size((uint32_t)i);
    return 0;
}

int afs_chmod(const char *path, uint32_t mode) {
    if (!mounted) afs_mount();
    int i = find_idx(path);
    if (i < 0) return -1;
    uint32_t uid, gid, oldm;
    slot_get_perm((uint32_t)i, &uid, &gid, &oldm);
    if (s_uid != 0 && s_uid != uid) return -1;
    slot_set_perm_owner((uint32_t)i, uid, gid, mode & 07777u);
    afs_commit();
    return 0;
}

int afs_chown(const char *path, int uid, int gid) {
    if (!mounted) afs_mount();
    int i = find_idx(path);
    if (i < 0) return -1;
    if (s_uid != 0) return -1;
    uint32_t ouid, ogid, omode;
    slot_get_perm((uint32_t)i, &ouid, &ogid, &omode);
    slot_set_perm_owner((uint32_t)i, (uint32_t)uid, (uint32_t)gid, omode);
    afs_commit();
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
    if (!mounted) return -1;
    int prob = 0, fixd = 0;
    uint32_t data_start = afs_data_start();
    uint32_t max_end = data_start;
    uint64_t cap = meta_capacity();
    uint32_t used = 0;

    if (s_ver == 6) {
        for (uint32_t i = 0; i < H.max_files; i++) {
            afs_file_t *f = &H.files[i];
            if (!f->used) continue;
            used++;
            int isd = afs_is_dir(f->name);
            int bad = 0;
            if (f->name[0] == 0) bad = 1;
            else if (is_ln_token(f->name) && ln_find_by_token(f->name) < 0) bad = 1;
            else if (!isd) {
                uint32_t secs = v6_file_secs(f);
                if (f->lba < data_start || f->lba >= cap || secs > cap - f->lba) bad = 1;
                else if (f->lba + secs > max_end) max_end = f->lba + secs;
            } else {
                uint32_t secs = v6_file_secs(f);
                if (f->lba >= data_start && f->lba < cap && secs <= cap - f->lba && f->lba + secs > max_end)
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
            prob++; if (fix) { v6_hdr_sync_size(); fixd++; }
        }
        if (H.count != used) { prob++; if (fix) { H.count = used; fixd++; } }
        if (H.data_next < max_end) { prob++; if (fix) { H.data_next = max_end; fixd++; } }
        if (H.data_next > H.capacity) { prob++; if (fix) { H.data_next = (uint32_t)H.capacity; fixd++; } }
    } else {
        for (uint32_t c = 0; c < s_v7_sb.num_chunks; c++) {
            if (s_v7_chunk_lba[c] < data_start ||
                (uint64_t)s_v7_chunk_lba[c] + AFS_V7_CHUNK_SECS > cap) {
                prob++;
            } else if (s_v7_chunk_lba[c] + AFS_V7_CHUNK_SECS > max_end) {
                max_end = s_v7_chunk_lba[c] + AFS_V7_CHUNK_SECS;
            }
            if (!s_v7_chunks[c]) { prob++; continue; }
            for (uint32_t i = 0; i < AFS_V7_CHUNK_ENTRIES; i++) {
                afs_v7_inode_t *in = &s_v7_chunks[c]->inodes[i];
                if (!in->used) continue;
                used++;
                int bad = 0;
                if (in->name[0] == 0) bad = 1;
                else if (is_ln_token(in->name) && ln_find_by_token(in->name) < 0) bad = 1;
                else if (in->ext_count > 0) {
                    afs_ext_t *exts = 0; uint32_t ecnt = 0;
                    uint32_t *inds = 0; uint32_t icnt = 0;
                    if (v7_collect_extents(in, &exts, &ecnt, &inds, &icnt) != 0) {
                        bad = 1;
                    } else {
                        uint64_t total_s = 0;
                        for (uint32_t e = 0; e < ecnt; e++) {
                            if (exts[e].start < data_start || exts[e].start >= cap ||
                                exts[e].count == 0 || exts[e].count > cap - exts[e].start) {
                                bad = 1; break;
                            }
                            if (exts[e].start + exts[e].count > max_end)
                                max_end = exts[e].start + exts[e].count;
                            total_s += exts[e].count;
                        }
                        for (uint32_t e = 0; !bad && e < icnt; e++) {
                            if (inds[e] < data_start || inds[e] >= cap) { bad = 1; break; }
                            if (inds[e] + 1u > max_end) max_end = inds[e] + 1u;
                        }
                        uint32_t need_s = (in->size + 511u) / 512u;
                        if (!bad && total_s < need_s) bad = 1;
                        kfree(exts);
                        if (inds) kfree(inds);
                    }
                } else if (in->size > 0) {
                    bad = 1;
                }
                if (bad) {
                    prob++;
                    if (fix) {
                        zero_mem(in, sizeof(*in));
                        s_v7_chunk_used[c]--;
                        v7_mark_dirty(c * AFS_V7_CHUNK_ENTRIES + i);
                        used--; fixd++;
                    }
                }
            }
        }
        if (s_v7_sb.count != used) { prob++; if (fix) { s_v7_sb.count = used; fixd++; } }
        if (s_v7_sb.data_next < max_end) { prob++; if (fix) { s_v7_sb.data_next = max_end; fixd++; } }
        if (s_v7_sb.data_next > s_v7_sb.capacity) { prob++; if (fix) { s_v7_sb.data_next = (uint32_t)s_v7_sb.capacity; fixd++; } }
    }

    if (fix && prob) {
        afs_commit();
        free_ext_rebuild();
    }
    if (problems) *problems = prob;
    if (fixed) *fixed = fixd;
    return prob ? 1 : 0;
}

/* ---- Free-extent, uzun yol ve yuksek kapasite oz-testleri ---- */

int afs_reclaim_selftest(void) {
    if (!mounted) afs_mount();
    if (!mounted) return -1;
    afs_set_subject(0, 0);

    afs_reclaim_stats_t st0;
    afs_free_extent_stats(&st0);
    uint32_t base_dn = meta_data_next();

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

    uint8_t blk12k[12288];
    for (int i = 0; i < 12288; i++) blk12k[i] = (uint8_t)(i + 7);
    uint32_t dn_before_e = meta_data_next();
    void *he = afs_create_handle("C:/recl_e.bin");
    if (!he || afs_write_handle(he, blk12k, sizeof(blk12k)) != 0) {
        afs_remove("C:/recl_d.bin"); afs_remove("C:/recl_e.bin");
        return -6;
    }
    if (meta_data_next() != dn_before_e) {
        afs_remove("C:/recl_d.bin"); afs_remove("C:/recl_e.bin");
        return -7;
    }

    afs_remove("C:/recl_d.bin");
    afs_remove("C:/recl_e.bin");
    if (meta_data_next() != base_dn) return -8;

    void *ht = afs_create_handle("C:/recl_trunc.bin");
    if (!ht || afs_write_handle(ht, blk12k, sizeof(blk12k)) != 0) {
        afs_remove("C:/recl_trunc.bin");
        return -9;
    }
    uint32_t dn_12k = meta_data_next();
    if (afs_write_handle(ht, blk4k, sizeof(blk4k)) != 0) {
        afs_remove("C:/recl_trunc.bin");
        return -10;
    }
    if (meta_data_next() >= dn_12k) {
        afs_remove("C:/recl_trunc.bin");
        return -11;
    }
    afs_remove("C:/recl_trunc.bin");
    if (meta_data_next() != base_dn) return -12;

    const uint32_t big_sz = 2u * 1024u * 1024u;
    uint32_t big_secs = big_sz / 512u;
    if ((uint64_t)meta_data_next() + big_secs + 64u <= meta_capacity()) {
        uint8_t *big = (uint8_t *)kmalloc(big_sz);
        if (!big) return -13;
        for (uint32_t k = 0; k < big_sz; k += 512u) {
            big[k] = (uint8_t)(k >> 9);
            big[k + 511u] = (uint8_t)~(k >> 9);
        }
        for (int iter = 0; iter < 20; iter++) {
            void *hf = afs_create_handle("C:/recl_2mb.bin");
            if (!hf || afs_write_handle(hf, big, big_sz) != 0) {
                afs_remove("C:/recl_2mb.bin"); kfree(big); return -14;
            }
            if (afs_remove("C:/recl_2mb.bin") != 0) { kfree(big); return -15; }
            if (meta_data_next() != base_dn) { kfree(big); return -16; }
        }
        kfree(big);
    }

    afs_reclaim_stats_t st1;
    afs_free_extent_stats(&st1);
    serial_printf("[afs] free_extents=%u reclaimed=%u reused=%u data_next=%u->%u (20x2MB OK)\n",
                  st1.free_extents, st1.reclaimed_sectors - st0.reclaimed_sectors,
                  st1.reused_sectors - st0.reused_sectors, base_dn, meta_data_next());
    return 0;
}

static int afs_longpath_selftest(void) {
    if (!mounted) afs_mount();
    if (!mounted) return -1;
    afs_set_subject(0, 0);
    uint32_t base_dn = meta_data_next();

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
    int rc_root = afs_count_dir("C:/");
    for (int k = 0; k < rc_root; k++) {
        char rn[AFS_PATH_MAX];
        if (afs_list_dir("C:/", k, rn, sizeof(rn), &isd) == 0 && seq(rn, AFS_LONGNAME_FILE)) {
            afs_remove(f200); afs_remove(f255); afs_remove(p200); afs_remove(ldir);
            return -8;
        }
    }

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

    if (meta_data_next() != base_dn || s_ln_count != 0) return -11;
    serial_printf("[afs] longpath selftest (200B + 255B + rename + hidden .afs_longnames): OK\n");
    return 0;
}

static int afs_capacity_selftest(void) {
    if (!mounted) afs_mount();
    if (!mounted) return -1;
    uint32_t mf = (s_ver == 7) ? s_v7_sb.max_files : H.max_files;
    if (mf < 1024u) {
        serial_printf("[afs] capacity selftest: max_files=%u (<1024) SKIP\n", mf);
        return 0;
    }
    afs_set_subject(0, 0);
    uint32_t base_dn = meta_data_next();
    uint32_t base_cnt = meta_count();

    const int target = 550;
    char path[64];
    for (int i = 0; i < target; i++) {
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
    int ok_cnt = (meta_count() == base_cnt + (uint32_t)target);
    for (int i = 0; i < target; i++) {
        path[0] = 'C'; path[1] = ':'; path[2] = '/';
        path[3] = 'c'; path[4] = 'a'; path[5] = 'p'; path[6] = '_';
        path[7] = (char)('0' + (i / 100) % 10);
        path[8] = (char)('0' + (i / 10) % 10);
        path[9] = (char)('0' + (i % 10));
        path[10] = '\0';
        afs_remove(path);
    }
    if (!ok_cnt || meta_count() != base_cnt || meta_data_next() != base_dn) return -3;
    serial_printf("[afs] capacity selftest: max_files=%u created=%d reclaimed data_next=%u -> OK\n",
                  mf, target, meta_data_next());
    return 0;
}

int afs_journal_selftest(void) {
    if (!mounted) afs_mount();
    if (!mounted) return -1;
    uint32_t jl = journal_lba();
    if (jl == 0) return -1;

    afs_set_subject(0, 0);
    afs_remove("afs_jrn_test");
    afs_remove("afs_jrn_replay");

    uint32_t saved_count = meta_count();
    uint32_t saved_dn = meta_data_next();

    void *h = afs_create_handle("afs_jrn_test");
    if (!h) return -2;
    if (afs_write_handle(h, "journal-data", 12) != 0) {
        afs_remove("afs_jrn_test");
        return -3;
    }

    int ok = 0;
    if (s_ver == 6) {
        if (H.flags == 0 || v6_hdr_crc() != H.flags) {
            afs_remove("afs_jrn_test"); return -4;
        }
        uint32_t disk_crc = H.flags;
        uint32_t idx = 0;
        if (alloc_slot_internal(0, &idx) != 0) { afs_remove("afs_jrn_test"); return -5; }
        H.files[idx].used = 1; H.files[idx].size = 0; H.files[idx].lba = H.data_next;
        H.files[idx].uid = s_uid; H.files[idx].gid = s_gid; H.files[idx].mode = 0666u; H.files[idx].pad = 0;
        cpy(H.files[idx].name, "afs_jrn_replay");
        H.count++; H.version = AFS_VERSION_V6; v6_hdr_sync_size();
        H.flags = 0; H.flags = v6_hdr_crc();

        uint8_t sec[512]; zero_mem(sec, 512);
        afs_jrec_t *rec = (afs_jrec_t *)sec;
        rec->magic = AFS_JOURNAL_MAGIC; rec->seq = ++s_jseq;
        rec->hdr_sectors = H.hdr_sectors;
        rec->crc = crc32_buf(s_hdr, v6_hdr_bytes());
        rec->state = 1; rec->prev_crc = disk_crc;
        if (v6_hdr_save_at(jl + 1u, H.hdr_sectors) == 0) {
            storage_flush();
            if (storage_write(jl, 1, sec) == 0) {
                storage_flush();
                v6_hdr_load_at(s_base, H.hdr_sectors);
                journal_replay();
                ok = (find_idx("afs_jrn_replay") >= 0);
            }
        }
        for (uint32_t i = 0; i < H.max_files; i++) {
            if (!H.files[i].used) continue;
            if (seq(H.files[i].name, "afs_jrn_replay") || seq(H.files[i].name, "afs_jrn_test")) {
                H.files[i].used = 0; H.files[i].name[0] = 0; H.files[i].size = 0; H.files[i].lba = 0;
                slot_bump_gen(i);
            }
        }
        H.count = saved_count; H.data_next = saved_dn;
        v6_commit();
    } else {
        uint32_t idx = 0;
        if (alloc_slot_internal(0, &idx) != 0) { afs_remove("afs_jrn_test"); return -5; }
        uint32_t c = idx >> 8;
        uint32_t in_c = idx & 255u;
        uint32_t sec_in_c = in_c >> 2;
        afs_v7_inode_t *in = v7_inode(idx);
        zero_mem(in, sizeof(*in));
        in->used = 1; in->uid = s_uid; in->gid = s_gid; in->mode = 0666u;
        cpy(in->name, "afs_jrn_replay");
        s_v7_sb.count++;
        s_v7_sb.journal_seq = ++s_jseq;
        s_v7_sb.flags = 0;
        s_v7_sb.flags = v7_sb_crc(&s_v7_sb);

        const uint8_t *p0 = (const uint8_t *)s_v7_chunks[c]->inodes + (size_t)sec_in_c * 512u;
        const uint8_t *p1 = (const uint8_t *)&s_v7_sb;
        uint8_t staged[1024];
        copy_mem(staged, p0, 512);
        copy_mem(staged + 512, p1, 512);

        uint8_t jsec[512]; zero_mem(jsec, 512);
        afs_v7_jrec_t *r7 = (afs_v7_jrec_t *)jsec;
        r7->magic = AFS_JOURNAL_MAGIC; r7->version = AFS_VERSION;
        r7->seq = s_jseq; r7->count = 2;
        r7->crc = crc32_buf(staged, 1024);
        r7->state = 1; r7->sb_crc = s_v7_sb.flags;
        r7->lbas[0] = s_v7_chunk_lba[c] + sec_in_c;
        r7->lbas[1] = s_base;

        if (storage_write(jl + 1u, 2, staged) == 0) {
            storage_flush();
            if (storage_write(jl, 1, jsec) == 0) {
                storage_flush();
                /* Bellegi diskten geri yukle ve replay'i tetikle */
                storage_read(s_base, 1, &s_v7_sb);
                storage_read(s_v7_chunk_lba[c] + sec_in_c, 1,
                             (uint8_t *)s_v7_chunks[c]->inodes + (size_t)sec_in_c * 512u);
                journal_replay();
                storage_read(s_base, 1, &s_v7_sb);
                storage_read(s_v7_chunk_lba[c] + sec_in_c, 1,
                             (uint8_t *)s_v7_chunks[c]->inodes + (size_t)sec_in_c * 512u);
                if (in->used) s_v7_chunk_used[c]++;
                ok = (find_idx("afs_jrn_replay") >= 0);
            }
        }
        afs_remove("afs_jrn_replay");
        afs_remove("afs_jrn_test");
        s_v7_sb.count = saved_count;
        s_v7_sb.data_next = saved_dn;
        v7_commit();
    }
    free_ext_rebuild();

    int clean = 0;
    { int p = 0, f = 0; clean = (afs_fsck(0, &p, &f) == 0 && p == 0); }
    if (!ok || !clean) return ok ? -7 : -6;

#ifdef ARCTIAN_AFS_SELFTEST
    {
        int rr = afs_reclaim_selftest();
        if (rr != 0) { serial_printf("[afs] reclaim selftest HATA kod=%d\n", rr); return -10; }
        int lr = afs_longpath_selftest();
        if (lr != 0) { serial_printf("[afs] longpath selftest HATA kod=%d\n", lr); return -11; }
        int cr = afs_capacity_selftest();
        if (cr != 0) { serial_printf("[afs] capacity selftest HATA kod=%d\n", cr); return -12; }
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
    uint32_t cap = slot_capacity();
    for (uint32_t i = 0; i < cap; i++) {
        if (!slot_used(i)) continue;
        uint32_t uid, gid, mode;
        slot_get_perm(i, &uid, &gid, &mode);
        if (uid != 0 || gid != 0) continue;
        const char *nm = entry_full_name(i);
        if (nm[0] == 0 || !prefix_of(nm, AFS_USER_ROOT)) continue;
        int skip_this = 0;
        for (unsigned k = 0; k < sizeof(skip) / sizeof(skip[0]); k++)
            if (prefix_of(nm, skip[k])) { skip_this = 1; break; }
        if (skip_this) continue;
        slot_set_perm_owner(i, 1000u, 1000u, mode);
        changed = 1;
    }
    if (changed) afs_commit();
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
    if (!mounted) return 0;
    char p[AFS_PATH_MAX];
    int pl = dir_prefix(path, p, sizeof(p));
    if (pl <= 0) return 0;
    int c = 0;
    uint32_t cap = slot_capacity();
    for (uint32_t i = 0; i < cap; i++) {
        if (!slot_used(i)) continue;
        if (seq(slot_name(i), AFS_LONGNAME_FILE)) continue;
        const char *nm = entry_full_name(i);
        if (!pref_match(nm, p, pl)) continue;
        if (direct_child(nm + pl)) c++;
    }
    return c;
}

int afs_list_dir(const char *path, int idx, char *out, int cap, int *is_dir) {
    if (!mounted) afs_mount();
    if (!mounted) return -1;
    char p[AFS_PATH_MAX];
    int pl = dir_prefix(path, p, sizeof(p));
    if (pl <= 0) return -1;
    int c = 0;
    uint32_t scap = slot_capacity();
    for (uint32_t i = 0; i < scap; i++) {
        if (!slot_used(i)) continue;
        if (seq(slot_name(i), AFS_LONGNAME_FILE)) continue;
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
