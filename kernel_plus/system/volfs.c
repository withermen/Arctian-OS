#include "kernel.h"
#include "asi.h"
#include "blockdev.h"
#include "fat32.h"
#include "exfat.h"
#include "volfs.h"
#include "usb_msd.h"
#include "usb_xhci.h"
#include "clock.h"
#include "serial.h"

static asi_fs_t *s_base = 0;
static asi_fs_t  s_vol;
static uint64_t  s_last_probe_ms = 0;

#define D_MAX_HANDLES 16
typedef struct {
    int  used;
    int  is_dir;
    char path[256];
} d_handle_t;

static d_handle_t s_d_handles[D_MAX_HANDLES];

static int is_d(const char *p) {
    return p && (p[0] == 'D' || p[0] == 'd') && p[1] == ':';
}

static const char *dpath(const char *p) {
    p += 2;
    if (*p == 0) return "/";
    return p;
}

static int is_d_handle(void *h) {
    d_handle_t *dh = (d_handle_t *)h;
    return dh >= &s_d_handles[0] && dh < &s_d_handles[D_MAX_HANDLES] && dh->used;
}

static d_handle_t *alloc_d_handle(const char *rel_path, int is_dir) {
    for (int i = 0; i < D_MAX_HANDLES; i++) {
        if (s_d_handles[i].used && strcmp(s_d_handles[i].path, rel_path) == 0) {
            s_d_handles[i].is_dir = is_dir;
            return &s_d_handles[i];
        }
    }
    for (int i = 0; i < D_MAX_HANDLES; i++) {
        if (!s_d_handles[i].used) {
            s_d_handles[i].used = 1;
            s_d_handles[i].is_dir = is_dir;
            int k = 0;
            for (; rel_path[k] && k < 255; k++) s_d_handles[i].path[k] = rel_path[k];
            s_d_handles[i].path[k] = 0;
            return &s_d_handles[i];
        }
    }
    /* Evict slot 0 if full */
    s_d_handles[0].used = 1;
    s_d_handles[0].is_dir = is_dir;
    int k = 0;
    for (; rel_path[k] && k < 255; k++) s_d_handles[0].path[k] = rel_path[k];
    s_d_handles[0].path[k] = 0;
    return &s_d_handles[0];
}

static void build_d_rel(char *out, int cap, const char *dir_full, const char *name) {
    const char *dp = is_d(dir_full) ? dpath(dir_full) : dir_full;
    int o = 0;
    if (!dp || !dp[0] || dp[0] != '/') {
        if (o < cap - 1) out[o++] = '/';
    }
    if (dp) {
        for (int i = 0; dp[i] && o < cap - 2; i++) out[o++] = dp[i];
    }
    if (o > 0 && out[o - 1] != '/' && o < cap - 2) out[o++] = '/';
    if (name) {
        const char *np = name;
        while (*np == '/') np++;
        for (int i = 0; np[i] && o < cap - 1; i++) out[o++] = np[i];
    }
    out[o] = 0;
}

static void vol_ensure(void) {
    /* If already mounted, verify USB device is still attached */
    if (fat32_mounted()) {
        int b = fat32_bdev();
        if (b >= 0 && usb_msd_is_attached(b)) return;
        fat32_unmount();
        memset(s_d_handles, 0, sizeof(s_d_handles));
    }
    if (exfat_mounted()) {
        int b = exfat_bdev();
        if (b >= 0 && usb_msd_is_attached(b)) return;
        exfat_unmount();
        memset(s_d_handles, 0, sizeof(s_d_handles));
    }

    uint64_t now = clock_ms();
    if (s_last_probe_ms != 0 && (now - s_last_probe_ms) < 250u)
        return;
    s_last_probe_ms = now ? now : 1;

    xhci_poll_hotplug();
    usb_msd_scan();

    int n = blockdev_count();
    for (int i = 0; i < n; i++) {
        blockdev_info_t in;
        if (blockdev_info(i, &in) != 0) continue;
        if (in.kind == BDEV_KIND_USB && usb_msd_is_attached(i)) {
            if (fat32_mount(i) == 0) return;
            if (exfat_mount(i) == 0) return;
        }
    }
}

void volfs_refresh(void) {
    s_last_probe_ms = 0;
    vol_ensure();
}

static uint32_t v_read_path(const char *path, void *buf, uint32_t size) {
    if (is_d(path)) {
        vol_ensure();
        if (fat32_mounted()) return fat_read_path(dpath(path), buf, size);
        if (exfat_mounted()) return exfat_read_path(dpath(path), buf, size);
        return 0;
    }
    return s_base->read_path ? s_base->read_path(path, buf, size) : 0;
}

static int v_count_dir(const char *path) {
    if (is_d(path)) {
        vol_ensure();
        if (fat32_mounted()) return fat_count_dir(dpath(path));
        if (exfat_mounted()) return exfat_count_dir(dpath(path));
        return 0;
    }
    return s_base->count_dir ? s_base->count_dir(path) : 0;
}

static int v_list_dir(const char *path, int idx, char *out, int cap, int *is_dir) {
    if (is_d(path)) {
        vol_ensure();
        if (fat32_mounted()) return fat_list_dir(dpath(path), idx, out, cap, is_dir);
        if (exfat_mounted()) return exfat_list_dir(dpath(path), idx, out, cap, is_dir);
        return -1;
    }
    return s_base->list_dir ? s_base->list_dir(path, idx, out, cap, is_dir) : -1;
}

static int v_stat(const char *path, asi_stat_t *out) {
    if (is_d(path)) {
        vol_ensure();
        uint32_t size = 0;
        int isdir = 0;
        int r = -1;
        if (fat32_mounted()) r = fat_stat(dpath(path), &size, &isdir);
        else if (exfat_mounted()) r = exfat_stat(dpath(path), &size, &isdir);
        if (r != 0) return -1;
        if (out) {
            out->uid = 0;
            out->gid = 0;
            out->mode = isdir ? 0755 : 0644;
            out->is_dir = (uint32_t)isdir;
            out->size = size;
        }
        return 0;
    }
    return s_base->stat ? s_base->stat(path, out) : -1;
}

static int v_mkdir(const char *path) {
    if (is_d(path)) {
        vol_ensure();
        if (!fat32_mounted()) return -1;
        return fat32_mkdir(dpath(path));
    }
    return s_base->mkdir ? s_base->mkdir(path) : -1;
}

static void *v_create_in(const char *path, const char *name, int is_dir) {
    if (is_d(path)) {
        vol_ensure();
        if (!fat32_mounted() || !name || !name[0]) return 0;
        char rel[256];
        build_d_rel(rel, sizeof(rel), path, name);
        if (is_dir) {
            if (fat32_mkdir(rel) != 0) return 0;
            return alloc_d_handle(rel, 1);
        }
        if (fat32_create_file(rel) != 0) return 0;
        return alloc_d_handle(rel, 0);
    }
    return s_base->create_in ? s_base->create_in(path, name, is_dir) : 0;
}

static void *v_find_in(const char *path, const char *name) {
    if (is_d(path)) {
        vol_ensure();
        if (!name || !name[0]) return 0;
        char rel[256];
        build_d_rel(rel, sizeof(rel), path, name);
        uint32_t sz = 0;
        int isdir = 0;
        int r = -1;
        if (fat32_mounted()) r = fat_stat(rel, &sz, &isdir);
        else if (exfat_mounted()) r = exfat_stat(rel, &sz, &isdir);
        if (r != 0) return 0;
        return alloc_d_handle(rel, isdir);
    }
    return s_base->find_in ? s_base->find_in(path, name) : 0;
}

static void *v_find(void *dir, const char *name) {
    if (!dir && is_d(name)) return v_find_in("D:/", dpath(name));
    if (is_d_handle(dir)) {
        d_handle_t *dh = (d_handle_t *)dir;
        char full[256];
        full[0] = 'D'; full[1] = ':';
        int o = 2;
        for (int i = 0; dh->path[i] && o < 255; i++) full[o++] = dh->path[i];
        full[o] = 0;
        return v_find_in(full, name);
    }
    return s_base->find ? s_base->find(dir, name) : 0;
}

static void *v_create(void *dir, const char *name, int is_dir) {
    if (!dir && is_d(name)) return v_create_in("D:/", dpath(name), is_dir);
    if (is_d_handle(dir)) {
        d_handle_t *dh = (d_handle_t *)dir;
        char full[256];
        full[0] = 'D'; full[1] = ':';
        int o = 2;
        for (int i = 0; dh->path[i] && o < 255; i++) full[o++] = dh->path[i];
        full[o] = 0;
        return v_create_in(full, name, is_dir);
    }
    return s_base->create ? s_base->create(dir, name, is_dir) : 0;
}

static int v_write(void *file, const void *data, uint32_t size) {
    if (is_d_handle(file)) {
        d_handle_t *dh = (d_handle_t *)file;
        if (!fat32_mounted() || dh->is_dir) return -1;
        return fat32_write_file(dh->path, data, size);
    }
    return s_base->write ? s_base->write(file, data, size) : -1;
}

static uint32_t v_read(void *file, void *buf, uint32_t size) {
    if (is_d_handle(file)) {
        d_handle_t *dh = (d_handle_t *)file;
        if (dh->is_dir) return 0;
        if (fat32_mounted()) return fat_read_path(dh->path, buf, size);
        if (exfat_mounted()) return exfat_read_path(dh->path, buf, size);
        return 0;
    }
    return s_base->read ? s_base->read(file, buf, size) : 0;
}

static int v_stat_handle(void *file, asi_stat_t *out) {
    if (is_d_handle(file)) {
        d_handle_t *dh = (d_handle_t *)file;
        uint32_t sz = 0;
        int isdir = 0;
        int r = -1;
        if (fat32_mounted()) r = fat_stat(dh->path, &sz, &isdir);
        else if (exfat_mounted()) r = exfat_stat(dh->path, &sz, &isdir);
        if (r != 0) return -1;
        if (out) {
            out->uid = 0;
            out->gid = 0;
            out->mode = isdir ? 0755 : 0644;
            out->is_dir = (uint32_t)isdir;
            out->size = sz;
        }
        return 0;
    }
    return s_base->stat_handle ? s_base->stat_handle(file, out) : -1;
}

static int v_remove(const char *name) {
    if (is_d(name)) {
        vol_ensure();
        if (!fat32_mounted()) return -1;
        return fat32_delete_file(dpath(name));
    }
    return s_base->remove ? s_base->remove(name) : -1;
}

static void v_set_subject(int uid, int gid) {
    if (s_base->set_subject) s_base->set_subject(uid, gid);
}

static int v_chmod(const char *path, uint32_t mode) {
    if (is_d(path)) return 0;
    return s_base->chmod ? s_base->chmod(path, mode) : -1;
}

static int v_chown(const char *path, int uid, int gid) {
    if (is_d(path)) return 0;
    return s_base->chown ? s_base->chown(path, uid, gid) : -1;
}

static int v_rename(const char *from, const char *to) {
    if (is_d(from) || is_d(to)) return -1;
    return s_base->rename ? s_base->rename(from, to) : -1;
}

#ifdef ARCTIAN_USB_SELFTEST
static void volfs_usb_selftest(asi_fs_t *vol) {
    asi_stat_t st;
    memset(&st, 0, sizeof(st));
    int sr = vol->stat("D:/", &st);
    serial_printf("[volfs-test] stat(D:/) rc=%d is_dir=%u fat32=%d exfat=%d\n",
                  sr, st.is_dir, fat32_mounted(), exfat_mounted());
    if (sr != 0) return;

    int cnt = vol->count_dir("D:/");
    serial_printf("[volfs-test] count_dir(D:/)=%d\n", cnt);
    for (int i = 0; i < cnt; i++) {
        char ename[256];
        int edir = 0;
        if (vol->list_dir("D:/", i, ename, sizeof(ename), &edir) == 0) {
            serial_printf("[volfs-test] entry[%d]='%s' dir=%d\n", i, ename, edir);
        }
    }

    char rbuf[128];
    memset(rbuf, 0, sizeof(rbuf));
    uint32_t rn = vol->read_path("D:/T\xC3\xBCrk\xC3\xA7" "e_Test.txt", rbuf, sizeof(rbuf) - 1);
    serial_printf("[volfs-test] read(Turkce_Test.txt) bytes=%u content='%s'\n",
                  (unsigned)rn, rbuf);

    void *wf = vol->create(0, "D:/ARCTIAN_W.TXT", 0);
    const char *wmsg = "Arctian 1.1 Write OK!\n";
    int wr = wf ? vol->write(wf, wmsg, 22) : -99;
    char wback[64];
    memset(wback, 0, sizeof(wback));
    uint32_t wn = vol->read_path("D:/ARCTIAN_W.TXT", wback, sizeof(wback) - 1);
    serial_printf("[volfs-test] create=0x%X write=%d readback=%u content='%s'\n",
                  (unsigned)(uintptr_t)wf, wr, (unsigned)wn, wback);
}
#endif

asi_fs_t *volfs_install(asi_fs_t *base) {
    s_base = base;
    s_vol = *base;
    memset(s_d_handles, 0, sizeof(s_d_handles));

    s_vol.read_path = v_read_path;
    s_vol.count_dir = v_count_dir;
    s_vol.list_dir = v_list_dir;
    s_vol.stat = v_stat;
    s_vol.stat_handle = v_stat_handle;
    s_vol.mkdir = v_mkdir;
    s_vol.create = v_create;
    s_vol.find = v_find;
    s_vol.create_in = v_create_in;
    s_vol.find_in = v_find_in;
    s_vol.write = v_write;
    s_vol.read = v_read;
    s_vol.remove = v_remove;
    s_vol.chmod = v_chmod;
    s_vol.chown = v_chown;
    s_vol.rename = v_rename;
    s_vol.set_subject = v_set_subject;

#ifdef ARCTIAN_USB_SELFTEST
    volfs_usb_selftest(&s_vol);
#endif

    return &s_vol;
}
