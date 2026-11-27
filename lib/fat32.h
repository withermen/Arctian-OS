#ifndef ARCTIAN_FAT32_H
#define ARCTIAN_FAT32_H

#include <stdint.h>

/* FAT12/16/32 okuma/yazma surucusu (USB depolama icin).
 * Tum yollar '/'-ayrilmis ve birim onekisizdir: "/", "/belgeler", "/MERHABA.TXT". */

typedef struct {
    char     name[256];
    int      is_dir;
    uint32_t size;
    uint32_t first_cluster;
} fat_dirent_t;

/* bdev: blockdev indeksi. 0 = basarili. */
int      fat32_mount(int bdev);
void     fat32_unmount(void);
int      fat32_mounted(void);
int      fat32_bdev(void);

/* Klasordeki gorunur girdi sayisi; hata/dosya ise 0. */
int      fat_count_dir(const char *path);

/* idx'inci girdi. out'a ad yazilir. 0 = basarili, 1 = liste sonu, <0 = hata. */
int      fat_list_dir(const char *path, int idx, char *out, int cap, int *is_dir);

/* Dosya icerigini buf'a okur; okunan bayt sayisini doner. */
uint32_t fat_read_path(const char *path, void *buf, uint32_t size);

/* Yol hakkinda bilgi. 0 = basarili. */
int      fat_stat(const char *path, uint32_t *size, int *is_dir);

/* Yazma islemleri (0 = basarili, <0 = hata). */
int      fat32_create_file(const char *path);
int      fat32_write_file(const char *path, const void *data, uint32_t size);
int      fat32_delete_file(const char *path);
int      fat32_mkdir(const char *path);

#endif /* ARCTIAN_FAT32_H */
