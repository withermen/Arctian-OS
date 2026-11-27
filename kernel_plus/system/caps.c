#include "caps.h"
#include "wolf_bridge.h"
#include "proc.h"
#include "sched.h"
#include "serial.h"
#include "syscall.h"
#include "clock.h"
#include "afs.h"

static uint32_t s_over[SCHED_TASK_HARD_CAP];
static uint8_t  s_has[SCHED_TASK_HARD_CAP];
static uint32_t s_uid_t[SCHED_TASK_HARD_CAP];
static uint32_t s_gid_t[SCHED_TASK_HARD_CAP];
static uint8_t  s_uid_over[SCHED_TASK_HARD_CAP];
static uint16_t s_active_uid = 0;
static uint16_t s_active_gid = 0;
static int      s_err = 0;

/* ---- 0.6 uyumlulugu: gorev basina yetki maskesi ---- */
static int cur_task(void) {
    int t = sched_current();
    if (t < 0 || t >= SCHED_TASK_HARD_CAP) return 0;
    return t;
}

static uint32_t effective(void) {
    int t = cur_task();
    if (s_has[t]) return s_over[t];
    uint32_t c;
    if (proc_owner_caps(&c)) return c;
    return CAP_ALL;
}

void caps_set(uint32_t mask) {
    int t = cur_task();
    s_over[t] = mask;
    s_has[t] = 1;
}

void caps_clear(void) {
    int t = cur_task();
    s_has[t] = 0;
    s_uid_over[t] = 0;
    s_uid_t[t] = 0;
    s_gid_t[t] = 0;
    if (t == 0) proc_desktop_frame_end_if_armed();
}

void caps_enter(int pid) {
    if (pid <= 0) { caps_clear(); return; }
    caps_set(proc_caps_of(pid));
    int t = cur_task();
    s_uid_t[t] = proc_uid_of(pid);
    s_gid_t[t] = proc_gid_of(pid);
    s_uid_over[t] = 1;
}

uint32_t caps_current(void) { return effective(); }

void caps_set_uid(int uid, int gid) {
    if (effective() != CAP_ALL) return;
    int t = cur_task();
    s_uid_t[t] = (uint32_t)uid;
    s_gid_t[t] = (uint32_t)gid;
    s_uid_over[t] = 1;
    afs_set_current_uid((uint16_t)uid, (uint16_t)gid);
}

void caps_set_user(uint16_t uid, uint16_t gid) {
    if (effective() != CAP_ALL) return;
    s_active_uid = uid;
    s_active_gid = gid;
    int t = cur_task();
    s_uid_over[t] = 0;
    s_uid_t[t] = (uint32_t)uid;
    s_gid_t[t] = (uint32_t)gid;
    afs_set_current_uid(uid, gid);
}

uint32_t caps_uid(void) {
    int t = cur_task();
    return s_uid_over[t] ? s_uid_t[t] : (uint32_t)s_active_uid;
}

uint32_t caps_gid(void) {
    int t = cur_task();
    return s_uid_over[t] ? s_gid_t[t] : (uint32_t)s_active_gid;
}

void caps_syscall_subject(uint32_t *uid, uint32_t *gid) {
    uint32_t u = 0, g = 0;
    if (!proc_owner_ids(&u, &g) || (u == 0 && g == 0 && s_active_uid != 0)) {
        u = caps_uid();
        g = caps_gid();
    }
    if (uid) *uid = u;
    if (gid) *gid = g;
}

int caps_check(uint32_t cap) {
    if ((effective() & cap) == cap) return 0;
    s_err = SANDBOX_EPERM;
    return SANDBOX_EPERM;
}

int caps_last_error(void) { return s_err; }

int caps_syscall_check(uint32_t cap) {
    uint32_t c;
    if (!proc_owner_caps(&c)) c = CAP_ALL;
    return ((c & cap) == cap) ? 0 : SANDBOX_EPERM;
}

/* ---------------------------------------------------------------- */
/* 0.7: ozne + politika                                               */
/*                                                                    */
/* Kutulu bir uygulama acikken ozne kaydedilir. Politika yalnizca      */
/* uygulama kodu calisirken (caps_dispatch_begin/end arasinda) gecer;  */
/* ayni is parcacigindaki masaustu kodu etkilenmez.                    */
/* ---------------------------------------------------------------- */
static caps_policy_fn s_policy;
static int   s_subj_on;
static int   s_subj_task = -1;
static int   s_subj_trusted;
static int   s_dispatch;
static char  s_subj_id[48];
static char  s_subj_title[48];
static char  s_subj_home[80];

static void cp_s(char *d, int cap, const char *s) {
    int i = 0;
    if (cap <= 0) return;
    if (!s) { d[0] = 0; return; }
    for (; s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}

void caps_set_policy(caps_policy_fn fn) { s_policy = fn; }

/* ASI arayuzu: masaustu politikayi buradan baglar (donus 0). */
static int caps_set_policy_asi(int (*fn)(const char *path, int is_write)) {
    s_policy = (caps_policy_fn)fn;
    return 0;
}

void caps_subject_begin(const char *id, const char *title,
                        const char *home, int trusted) {
    cp_s(s_subj_id, sizeof(s_subj_id), id);
    cp_s(s_subj_title, sizeof(s_subj_title), title);
    cp_s(s_subj_home, sizeof(s_subj_home), home);
    s_subj_trusted = trusted ? 1 : 0;
    s_subj_on = 1;
    s_subj_task = sched_current();
    s_dispatch = 0;
}

void caps_subject_end(void) {
    s_subj_on = 0;
    s_subj_task = -1;
    s_subj_trusted = 0;
    s_subj_id[0] = 0;
    s_subj_title[0] = 0;
    s_subj_home[0] = 0;
    s_dispatch = 0;
}

const char *caps_subject_id(void)    { return s_subj_on ? s_subj_id : ""; }
const char *caps_subject_title(void) { return s_subj_on ? s_subj_title : ""; }
const char *caps_subject_home(void)  { return s_subj_on ? s_subj_home : ""; }
int caps_subject_trusted(void)       { return s_subj_on ? s_subj_trusted : 1; }
int caps_subject_active(void)        { return s_subj_on; }

void caps_dispatch_begin(void) { s_dispatch++; }
void caps_dispatch_end(void)   { if (s_dispatch > 0) s_dispatch--; }

static int gate_open(void) {
    return s_subj_on && s_dispatch > 0 && s_policy &&
           sched_current() == s_subj_task;
}

int caps_dispatch_active(void) { return gate_open(); }

/* Politika kapaliyken eski davranis: CAP_FS denetimi. */
static int gate(const char *path, int is_write) {
    if (!gate_open()) return 0;
    if (s_policy(path, is_write)) { s_err = SANDBOX_EPERM; return SANDBOX_EPERM; }
    return 0;
}

static int gate_denied(void) { s_err = SANDBOX_EPERM; return SANDBOX_EPERM; }

/* ---------------------------------------------------------------- */
/* Tutamac -> yol baglantisi: tutamac acildigi yola baglidir.       */
/* Boylece read/write cagrilari da kapidan gecer.                   */
/* ---------------------------------------------------------------- */
#define HP_MAX 64
static struct { void *h; char path[160]; } s_hp[HP_MAX];

static void hp_put(void *h, const char *path) {
    if (!h || !path) return;
    for (int i = 0; i < HP_MAX; i++) {
        if (s_hp[i].h == h) { cp_s(s_hp[i].path, sizeof(s_hp[i].path), path); return; }
    }
    for (int i = 0; i < HP_MAX; i++) {
        if (!s_hp[i].h) {
            s_hp[i].h = h;
            cp_s(s_hp[i].path, sizeof(s_hp[i].path), path);
            return;
        }
    }
}

static const char *hp_get(void *h) {
    if (!h) return 0;
    for (int i = 0; i < HP_MAX; i++) if (s_hp[i].h == h) return s_hp[i].path;
    return 0;
}

static void join_path(char *out, int cap, const char *dir, const char *name) {
    int n = 0;
    if (dir) for (int i = 0; dir[i] && n < cap - 2; i++) out[n++] = dir[i];
    if (n > 0 && out[n - 1] != '/' && n < cap - 2) out[n++] = '/';
    if (name) for (int i = 0; name[i] && n < cap - 2; i++) out[n++] = name[i];
    out[n] = 0;
}

static asi_fs_t s_guard;
static asi_fs_t s_save;

static void guard_subject(void) {
    uint32_t u = caps_uid();
    uint32_t g = caps_gid();
    afs_set_current_uid((uint16_t)u, (uint16_t)g);
    if (s_save.set_subject) s_save.set_subject((int)u, (int)g);
}

static uint32_t uid_perm_bits(uint32_t u, uint32_t g,
                              uint32_t fuid, uint32_t fgid, uint32_t mode) {
    if (u == fuid) return (mode >> 6) & 7u;
    if (g == fgid) return (mode >> 3) & 7u;
    return mode & 7u;
}

static int is_c_users_subpath(const char *p) {
    if (!p) return 0;
    if ((p[0] != 'C' && p[0] != 'c') || p[1] != ':' || p[2] != '/') return 0;
    const char *u = "Users/";
    for (int i = 0; u[i]; i++) {
        if (p[3 + i] != u[i]) return 0;
    }
    return p[9] != 0;
}

static int uid_path_ok(const char *path, int want_dir_read) {
    uint32_t u = caps_uid();
    uint32_t g = caps_gid();
    if (u == 0 || !path || !path[0] || !s_save.stat) return 1;
    if ((path[0] != 'C' && path[0] != 'c') || path[1] != ':') return 1;

    char sub[176];
    int len = 0;
    for (; path[len] && len < (int)sizeof(sub) - 2; len++) sub[len] = path[len];
    sub[len] = 0;

    /* Walk every ancestor directory prefix ("C:/", "C:/Users/", "C:/Users/alice/", ...)
     * and verify search/execute permission (bit 0). */
    for (int i = 2; i < len; i++) {
        if (sub[i] == '/') {
            char save = sub[i + 1];
            sub[i + 1] = 0;
            asi_stat_t st;
            if (s_save.stat(sub, &st) == 0 && st.is_dir) {
                if ((uid_perm_bits(u, g, st.uid, st.gid, st.mode) & 1u) == 0u) {
                    s_err = SANDBOX_EPERM;
                    return 0;
                }
            }
            sub[i + 1] = save;
        }
    }

    if (want_dir_read) {
        if (len > 0 && sub[len - 1] != '/') {
            sub[len++] = '/';
            sub[len] = 0;
        }
        asi_stat_t st;
        if (s_save.stat(sub, &st) == 0 && st.is_dir) {
            if ((uid_perm_bits(u, g, st.uid, st.gid, st.mode) & 4u) == 0u) {
                s_err = SANDBOX_EPERM;
                return 0;
            }
        }
    }
    return 1;
}

static void *g_root(void) {
    if (gate_open()) {
        if (gate("C:/", 0)) return 0;
    } else if (caps_check(CAP_FS)) return 0;
    guard_subject();
    void *h = s_save.root();
    if (h) hp_put(h, "C:/");
    return h;
}

static void *g_create(void *d, const char *n, int is_dir) {
    char p[176];
    join_path(p, sizeof(p), hp_get(d), n);
    if (gate_open()) {
        if (gate(p, 1)) return 0;
    } else if (caps_check(CAP_FS)) return 0;
    if (!uid_path_ok(p, 0)) return 0;
    int existed = 0;
    if (s_save.stat && p[0]) {
        asi_stat_t st;
        existed = (s_save.stat(p, &st) == 0);
    }
    guard_subject();
    void *h = s_save.create(d, n, is_dir);
    if (h) {
        hp_put(h, p);
        if (!existed && caps_uid() != 0 && is_c_users_subpath(p) && s_save.chmod)
            s_save.chmod(p, is_dir ? 0700u : 0600u);
    }
    return h;
}

static void *g_find(void *d, const char *n) {
    char p[176];
    join_path(p, sizeof(p), hp_get(d), n);
    if (gate_open()) {
        if (gate(p, 0)) return 0;
    } else if (caps_check(CAP_FS)) return 0;
    if (!uid_path_ok(p, 0)) return 0;
    guard_subject();
    void *h = s_save.find(d, n);
    if (h) hp_put(h, p);
    return h;
}

static int g_write(void *f, const void *data, uint32_t size) {
    const char *p = hp_get(f);
    if (gate_open()) {
        if (gate(p, 1)) return -1;
    } else if (caps_check(CAP_FS)) return -1;
    if (!uid_path_ok(p, 0)) return -1;
    guard_subject();
    int rc = s_save.write(f, data, size);
    if (rc == 0 && size > 0) proc_add_io_bytes((uint64_t)size);
    return rc;
}

static uint32_t g_read(void *f, void *buf, uint32_t size) {
    const char *p = hp_get(f);
    if (gate_open()) {
        if (gate(p, 0)) return 0;
    } else if (caps_check(CAP_FS)) return 0;
    if (!uid_path_ok(p, 0)) return 0;
    guard_subject();
    uint32_t n = s_save.read(f, buf, size);
    if (n > 0) proc_add_io_bytes((uint64_t)n);
    return n;
}

static int g_count(void) {
    if (gate_open()) { if (gate("C:/", 0)) return 0; }
    else if (caps_check(CAP_FS)) return 0;
    guard_subject();
    return s_save.count();
}

static const char *g_name(int idx) {
    if (gate_open()) { if (gate("C:/", 0)) return 0; }
    else if (caps_check(CAP_FS)) return 0;
    guard_subject();
    return s_save.name(idx);
}

static uint32_t g_size(int idx) {
    if (gate_open()) { if (gate("C:/", 0)) return 0; }
    else if (caps_check(CAP_FS)) return 0;
    guard_subject();
    return s_save.size(idx);
}

static int g_remove(const char *n) {
    if (gate_open()) { if (gate(n, 1)) return -1; }
    else if (caps_check(CAP_FS)) return -1;
    if (!uid_path_ok(n, 0)) return -1;
    guard_subject();
    return s_save.remove(n);
}

static int g_mkdir(const char *p) {
    if (gate_open()) { if (gate(p, 1)) return -1; }
    else if (caps_check(CAP_FS)) return -1;
    if (!uid_path_ok(p, 0)) return -1;
    char ds[176];
    int l = 0;
    for (; p && p[l] && l < (int)sizeof(ds) - 2; l++) ds[l] = p[l];
    if (l > 0 && ds[l - 1] != '/') ds[l++] = '/';
    ds[l] = 0;
    int existed = 0;
    if (s_save.stat && ds[0]) {
        asi_stat_t st;
        existed = (s_save.stat(ds, &st) == 0);
    }
    guard_subject();
    int rc = s_save.mkdir(p);
    if (rc == 0 && !existed && caps_uid() != 0 && is_c_users_subpath(ds) && s_save.chmod)
        s_save.chmod(ds, 0700u);
    return rc;
}

static void *g_create_in(const char *p, const char *n, int is_dir) {
    char full[176];
    join_path(full, sizeof(full), p, n);
    if (is_dir) {
        int l = 0; while (full[l]) l++;
        if (l > 0 && full[l - 1] != '/' && l < (int)sizeof(full) - 1) {
            full[l++] = '/'; full[l] = 0;
        }
    }
    if (gate_open()) {
        if (gate(full, 1)) return 0;
    } else if (caps_check(CAP_FS)) return 0;
    if (!uid_path_ok(full, 0)) return 0;
    int existed = 0;
    if (s_save.stat && full[0]) {
        asi_stat_t st;
        existed = (s_save.stat(full, &st) == 0);
    }
    guard_subject();
    void *h = s_save.create_in(p, n, is_dir);
    if (h) {
        hp_put(h, full);
        if (!existed && caps_uid() != 0 && is_c_users_subpath(full) && s_save.chmod)
            s_save.chmod(full, is_dir ? 0700u : 0600u);
    }
    return h;
}

static void *g_find_in(const char *p, const char *n) {
    char full[176];
    join_path(full, sizeof(full), p, n);
    if (gate_open()) {
        if (gate(full, 0)) return 0;
    } else if (caps_check(CAP_FS)) return 0;
    if (!uid_path_ok(full, 0)) return 0;
    guard_subject();
    void *h = s_save.find_in(p, n);
    if (h) hp_put(h, full);
    return h;
}

static uint32_t g_read_path(const char *p, void *buf, uint32_t size) {
    if (gate_open()) { if (gate(p, 0)) return 0; }
    else if (caps_check(CAP_FS)) return 0;
    if (!uid_path_ok(p, 0)) return 0;
    guard_subject();
    uint32_t n = s_save.read_path(p, buf, size);
    if (n > 0) proc_add_io_bytes((uint64_t)n);
    return n;
}

static int g_count_dir(const char *p) {
    if (gate_open()) { if (gate(p, 0)) return 0; }
    else if (caps_check(CAP_FS)) return 0;
    if (!uid_path_ok(p, 1)) return 0;
    guard_subject();
    return s_save.count_dir(p);
}

static int g_list_dir(const char *p, int idx, char *out, int cap, int *is_dir) {
    if (gate_open()) { if (gate(p, 0)) return -1; }
    else if (caps_check(CAP_FS)) return -1;
    if (!uid_path_ok(p, 1)) return -1;
    guard_subject();
    return s_save.list_dir(p, idx, out, cap, is_dir);
}

static int g_stat(const char *p, asi_stat_t *out) {
    if (gate_open()) { if (gate(p, 0)) return -1; }
    else if (caps_check(CAP_FS)) return -1;
    if (!uid_path_ok(p, 0)) return -1;
    guard_subject();
    return s_save.stat ? s_save.stat(p, out) : -1;
}

static int g_stat_handle(void *f, asi_stat_t *out) {
    const char *p = hp_get(f);
    if (gate_open()) { if (gate(p, 0)) return -1; }
    else if (caps_check(CAP_FS)) return -1;
    if (!uid_path_ok(p, 0)) return -1;
    guard_subject();
    return s_save.stat_handle ? s_save.stat_handle(f, out) : -1;
}

static int g_chmod(const char *p, uint32_t mode) {
    if (gate_open()) { if (gate(p, 1)) return -1; }
    else if (caps_check(CAP_FS)) return -1;
    if (!uid_path_ok(p, 0)) return -1;
    guard_subject();
    return s_save.chmod ? s_save.chmod(p, mode) : -1;
}

static int g_chown(const char *p, int uid, int gid) {
    if (gate_open()) { if (gate(p, 1)) return -1; }
    else if (caps_check(CAP_FS)) return -1;
    if (!uid_path_ok(p, 0)) return -1;
    guard_subject();
    return s_save.chown ? s_save.chown(p, uid, gid) : -1;
}

static void g_set_subject(int uid, int gid) {
    if (gate_open() || effective() != CAP_ALL) return;
    int t = cur_task();
    s_uid_t[t] = (uint32_t)uid;
    s_gid_t[t] = (uint32_t)gid;
    s_uid_over[t] = 1;
    afs_set_current_uid((uint16_t)uid, (uint16_t)gid);
    if (s_save.set_subject) s_save.set_subject(uid, gid);
}

static int g_rename(const char *from, const char *to) {
    if (gate_open()) { if (gate(from, 1) || gate(to, 1)) return -1; }
    else if (caps_check(CAP_FS)) return -1;
    if (!uid_path_ok(from, 0) || !uid_path_ok(to, 0)) return -1;
    guard_subject();
    return s_save.rename ? s_save.rename(from, to) : -1;
}

static int g_fsck(int fix, int *problems, int *fixed) {
    if (gate_open()) { if (gate("C:/", 1)) return -1; }
    else if (caps_check(CAP_FS)) return -1;
    guard_subject();
    return s_save.fsck ? s_save.fsck(fix, problems, fixed) : -1;
}

void caps_fs_guard_install(asi_fs_t **slot) {
    if (!slot || !*slot) return;
    s_save = **slot;
    s_guard = **slot;
    s_guard.root = g_root;
    s_guard.create = g_create;
    s_guard.find = g_find;
    s_guard.write = g_write;
    s_guard.read = g_read;
    s_guard.count = g_count;
    s_guard.name = g_name;
    s_guard.size = g_size;
    s_guard.remove = g_remove;
    s_guard.mkdir = g_mkdir;
    s_guard.create_in = g_create_in;
    s_guard.find_in = g_find_in;
    s_guard.read_path = g_read_path;
    s_guard.count_dir = g_count_dir;
    s_guard.list_dir = g_list_dir;
    s_guard.stat = g_stat;
    s_guard.stat_handle = g_stat_handle;
    s_guard.chmod = g_chmod;
    s_guard.chown = g_chown;
    s_guard.set_subject = g_set_subject;
    s_guard.rename = g_rename;
    s_guard.fsck = g_fsck;
    *slot = &s_guard;
}

static asi_sandbox_t s_sandbox;

void caps_init(asi_t *asi) {
    for (int i = 0; i < SCHED_TASK_HARD_CAP; i++) {
        s_over[i] = CAP_ALL;
        s_has[i] = 0;
        s_uid_t[i] = 0;
        s_gid_t[i] = 0;
        s_uid_over[i] = 0;
    }
    s_active_uid = 0;
    s_active_gid = 0;
    s_err = 0;
    s_policy = 0;
    s_subj_on = 0;
    s_subj_task = -1;
    s_subj_trusted = 0;
    s_dispatch = 0;
    s_subj_id[0] = 0;
    s_subj_title[0] = 0;
    s_subj_home[0] = 0;
    for (int i = 0; i < HP_MAX; i++) { s_hp[i].h = 0; s_hp[i].path[0] = 0; }
    s_sandbox.enter = caps_enter;
    s_sandbox.leave = caps_clear;
    s_sandbox.current = caps_current;
    s_sandbox.check = caps_check;
    s_sandbox.last_error = caps_last_error;
    s_sandbox.selftest = caps_selftest;
    s_sandbox.set_uid = caps_set_uid;
    s_sandbox.uid = caps_uid;
    s_sandbox.gid = caps_gid;
    /* 0.7: masaustunun ozne/kapi arayuzu */
    s_sandbox.subject_begin = caps_subject_begin;
    s_sandbox.subject_end = caps_subject_end;
    s_sandbox.subject_id = caps_subject_id;
    s_sandbox.subject_title = caps_subject_title;
    s_sandbox.subject_home = caps_subject_home;
    s_sandbox.subject_trusted = caps_subject_trusted;
    s_sandbox.subject_active = caps_subject_active;
    s_sandbox.dispatch_begin = caps_dispatch_begin;
    s_sandbox.dispatch_end = caps_dispatch_end;
    s_sandbox.dispatch_active = caps_dispatch_active;
    s_sandbox.set_policy = caps_set_policy_asi;
    s_sandbox.set_user = caps_set_user;
    if (asi) asi->sandbox = &s_sandbox;
    arctian_set_cap_check(caps_syscall_check);
    arctian_set_subject_fn(caps_syscall_subject);
    /* SYS_GETPID/SYS_TICKS gercek surec kimligini ve saati gorsun;
     * fd tablollari da pid'e gore ayristiriliyor. */
    arctian_set_pid_fn(proc_getpid);
    arctian_set_ticks_fn(clock_ms);
    arctian_set_wolf_call_fn(wolf_bridge_call);
    arctian_set_wolf_wait_fn(wolf_bridge_wait);
}

int caps_selftest(void) {
    if (!g_asi || !g_asi->net || !g_asi->fs) return -1;
    int net_ok = 0, fs_ok = 0, allow_ok = 0;

    caps_set(CAP_FS);
    uint32_t len = 0;
    const char *b = g_asi->net->http_get("http://arctian.invalid/", &len);
    net_ok = (b == 0 && caps_last_error() == SANDBOX_EPERM);

    caps_set(CAP_NET);
    void *h = g_asi->fs->create(0, "caps_denied.tmp", 0);
    fs_ok = (h == 0 && caps_last_error() == SANDBOX_EPERM);

    caps_set(CAP_ALL);
    void *h2 = g_asi->fs->create(0, "caps_allow.tmp", 0);
    allow_ok = (h2 != 0);
    if (h2) g_asi->fs->remove("caps_allow.tmp");

    caps_clear();
    serial_printf("[caps] sandbox selftest: net_eprem=%s fs_eprem=%s izinli_fs=%s -> %s\n",
                  net_ok ? "EVET" : "HAYIR",
                  fs_ok ? "EVET" : "HAYIR",
                  allow_ok ? "EVET" : "HAYIR",
                  (net_ok && fs_ok && allow_ok) ? "OK" : "HATA");
    return (net_ok && fs_ok && allow_ok) ? 0 : -1;
}

int caps_multiuser_selftest(void) {
    if (!g_asi || !g_asi->fs || !g_asi->sandbox || !g_asi->sandbox->set_user) return -1;
    asi_fs_t *fs = g_asi->fs;
    asi_sandbox_t *sb = g_asi->sandbox;

    caps_clear();
    sb->set_user(0, 0);
    fs->remove("C:/Users/mu_a/secret.txt");
    fs->remove("C:/Users/mu_a/");
    fs->remove("C:/Users/mu_b/");
    fs->remove("C:/mu_shared.txt");

    if (fs->mkdir("C:/Users") != 0) {
        serial_puts("[multiuser11] salt okunur ortam -> atlandi\n");
        return 0;
    }
    if (fs->chown) fs->chown("C:/Users/", 0, 0);
    if (fs->chmod) fs->chmod("C:/Users/", 0755u);

    if (fs->mkdir("C:/Users/mu_a") != 0 || fs->mkdir("C:/Users/mu_b") != 0) {
        sb->set_user(0, 0);
        serial_puts("[multiuser11] home dir olusturulamadi -> atlandi\n");
        return 0;
    }
    if (fs->chown) {
        fs->chown("C:/Users/mu_a/", 1000, 1000);
        fs->chown("C:/Users/mu_b/", 1001, 1001);
    }
    if (fs->chmod) {
        fs->chmod("C:/Users/mu_a/", 0700u);
        fs->chmod("C:/Users/mu_b/", 0700u);
    }

    /* Paylasilan sistem/uygulama dosyasi: 0644 (herkes okuyabilir) */
    void *hs = fs->create(0, "C:/mu_shared.txt", 0);
    if (hs) {
        fs->write(hs, "shared11", 8);
        if (fs->chmod) fs->chmod("C:/mu_shared.txt", 0644u);
    }

    /* Kullanici A (uid=1000): ozel dosya olusturur ve okur */
    sb->set_user(1000, 1000);
    void *ha = fs->create_in("C:/Users/mu_a", "secret.txt", 0);
    int a_write_ok = (ha != 0 && fs->write(ha, "topsecretA", 10) == 0);
    if (fs->chmod) fs->chmod("C:/Users/mu_a/secret.txt", 0600u);

    char rbuf[32];
    for (int i = 0; i < (int)sizeof(rbuf); i++) rbuf[i] = 0;
    uint32_t rn = fs->read_path("C:/Users/mu_a/secret.txt", rbuf, 10);
    int a_read_ok = (rn == 10 && rbuf[0] == 't' && rbuf[9] == 'A');

    /* Kullanici B (uid=1001): A'nin dosyasini okuyamaz/yazamz/silemez/listeleyemez, ama paylasilan okuyabilir */
    sb->set_user(1001, 1001);
    char bbuf[32];
    uint32_t bn = fs->read_path("C:/Users/mu_a/secret.txt", bbuf, 10);
    int b_read_deny = (bn == 0);
    int b_find_deny = (fs->find_in("C:/Users/mu_a", "secret.txt") == 0);
    int b_write_deny = (fs->create_in("C:/Users/mu_a", "hack.txt", 0) == 0 && fs->write(ha, "x", 1) != 0);
    int b_del_deny = (fs->remove("C:/Users/mu_a/secret.txt") != 0);
    char lout[32];
    int ldir = 0;
    int b_list_deny = (fs->list_dir("C:/Users/mu_a", 0, lout, sizeof(lout), &ldir) != 0);

    char sbuf[16];
    for (int i = 0; i < (int)sizeof(sbuf); i++) sbuf[i] = 0;
    uint32_t sn = fs->read_path("C:/mu_shared.txt", sbuf, 8);
    int b_shared_ok = (sn == 8 && sbuf[0] == 's');

    sb->set_user(0, 0);
    fs->remove("C:/Users/mu_a/secret.txt");
    fs->remove("C:/Users/mu_a/");
    fs->remove("C:/Users/mu_b/");
    fs->remove("C:/mu_shared.txt");

    int ok = a_write_ok && a_read_ok && b_read_deny && b_find_deny &&
             b_write_deny && b_del_deny && b_list_deny && b_shared_ok;
    serial_printf("[multiuser11] a_rw=%s b_read_deny=%s b_find_deny=%s b_wr_deny=%s b_del_deny=%s b_list_deny=%s shared=%s -> %s\n",
                  (a_write_ok && a_read_ok) ? "OK" : "FAIL",
                  b_read_deny ? "OK" : "FAIL",
                  b_find_deny ? "OK" : "FAIL",
                  b_write_deny ? "OK" : "FAIL",
                  b_del_deny ? "OK" : "FAIL",
                  b_list_deny ? "OK" : "FAIL",
                  b_shared_ok ? "OK" : "FAIL",
                  ok ? "OK" : "HATA");
    return ok ? 0 : -1;
}