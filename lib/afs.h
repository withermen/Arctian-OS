#ifndef ARCTIAN_AFS_H
#define ARCTIAN_AFS_H

#include <stdint.h>
#include "asi.h"

/* Yeni duzen (0.8.2+): taban 65536. Modul bolgesi 27836'da biterken eski
 * taban 20000 AFS'yi modullerin uzerine yaziyordu; yeni kurulumlar 65536'dan
 * formatlanir, eski diskler legacy tabandan okunmaya devam eder. */
#define AFS_BASE_LBA        65536u
#define AFS_BASE_LEGACY_LBA 20000u
#define AFS_VERSION_V6      6u
#define AFS_VERSION         7u      /* v7: 65536 dosya + coklu extent + gercek uid/gid/mode */
#define AFS_V7_MAX_FILES    65536u
#define AFS_NAME_MAX        64u     /* disk uzerindeki kisa girdi adi (+ sonlandirici) */
#define AFS_PATH_MAX        256u    /* uzun yol destegi (en fazla 255 bayt + sonlandirici) */

/* Mount sirasinda saptanan etkin taban LBA (legacy 20000 veya yeni 65536). */
uint32_t    afs_base(void);
uint32_t    afs_get_base_lba(void);
uint32_t    afs_version(void);

/* Kullanici diski (C:/) kok yolu. Sistem dosyalari (ayar, .dpk) ham kokte
 * tutulur; bunlar masaustunde gizlidir. C: diski kullaniciya aittir. */
#define AFS_USER_ROOT     "C:/"
#define AFS_USER_APPS     "C:/apps"
#define AFS_USER_DOWNLOAD "C:/indirilenler"
#define AFS_USER_DOCS     "C:/belgeler"
#define AFS_USER_DESKTOP  "C:/masaustu"
#define AFS_USER_LOGS     "C:/logs"

void        afs_format(void);
void        afs_mount(void);
int         afs_count(void);

void       *afs_create_handle(const char *name);
void       *afs_find_handle(const char *name);
int         afs_write_handle(void *h, const void *data, uint32_t size);
uint32_t    afs_read_handle(void *h, void *buf, uint32_t size);
uint32_t    afs_read_at(void *h, void *buf, uint32_t size, uint32_t off);
int         afs_remove(const char *name);
const char *afs_name(int idx);
uint32_t    afs_size(int idx);
uint32_t    afs_file_extent_count(void *h);

/* ---- Klasor (yol tabanli) API ----
 * path ornegi: "C:/apps". Adlar '/' ile ayrilir; klasor adi '/' ile biter. */
int         afs_is_dir(const char *name);
int         afs_ensure_dir(const char *path);                  /* 0 = hazir */
void       *afs_create_in(const char *path, const char *name, int is_dir);
void       *afs_find_in(const char *path, const char *name);
uint32_t    afs_read_path(const char *path, void *buf, uint32_t size);
int         afs_write_path(const char *path, const void *data, uint32_t size);
int         afs_count_dir(const char *path);
int         afs_list_dir(const char *path, int idx, char *out, int cap, int *is_dir);

void        afs_set_subject(int uid, int gid);
void        afs_set_current_uid(uint16_t uid, uint16_t gid);
void        afs_get_subject(int *uid, int *gid);
int         afs_chmod(const char *path, uint32_t mode);
int         afs_chown(const char *path, int uid, int gid);
int         afs_stat(const char *path, asi_stat_t *out);
int         afs_stat_handle(void *h, asi_stat_t *out);
int         afs_rename(const char *from, const char *to);   /* 0/-1/-2/-3 */
int         afs_perm_selftest(void);

/* Butunluk: metadata gunlugu (journal) + denetim (fsck) + geri kazanim. */
typedef struct {
    uint32_t free_extents;
    uint32_t free_sectors;
    uint32_t reclaimed_sectors;
    uint32_t reused_sectors;
    uint32_t data_next;
    uint32_t data_start;
    uint32_t capacity;
} afs_reclaim_stats_t;

int         afs_fsck(int fix, int *problems, int *fixed);
int         afs_journal_selftest(void);
void        afs_free_extent_stats(afs_reclaim_stats_t *out);
int         afs_reclaim_selftest(void);

void        afs_set_locked(int v);
int         afs_is_locked(void);
int         afs_disk_is_encrypted(void);
uint32_t    afs_data_next(void);

asi_fs_t   *afs_asi(void);

#endif /* ARCTIAN_AFS_H */
