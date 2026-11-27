#ifndef ARCTIAN_PROC_H
#define ARCTIAN_PROC_H

#include <stdint.h>
#include "asi.h"

#define PROC_MAX 16

typedef enum {
    PROC_UNUSED = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_BLOCKED,
    PROC_ZOMBIE
} proc_state_t;

void proc_init(void);
int  proc_spawn(const char *name, void (*entry)(void *), void *arg);
int  proc_fork(const char *name, void (*entry)(void *), void *arg);
int  proc_exec(void (*entry)(void *), void *arg);
void proc_exit(int code) __attribute__((noreturn));
int  proc_wait(int pid, int *code);
int  proc_kill(int pid);
int  proc_getpid(void);
int  proc_getppid(void);
int  proc_count(void);
int  proc_info(int i, int *pid, char *name, int cap, int *state, int *ppid, int *code);
int  proc_info_ex(int i, asi_proc_info_t *out);
uint32_t proc_sys_ticks(void);
int  proc_app_open(const char *title);
int  proc_app_open_caps(const char *title, uint32_t caps);
void proc_app_close(int pid, int code);
int  proc_reap(int *code_out);
uint32_t proc_caps_of(int pid);
int  proc_owner_caps(uint32_t *out);
uint32_t proc_uid_of(int pid);
uint32_t proc_gid_of(int pid);
int  proc_owner_ids(uint32_t *uid, uint32_t *gid);
int  proc_selftest(void);
int  proc_fault_selftest(void);
int  proc_v11_selftest(void);

/* v1.1 gercek per-process I/O, Ag ve bosta CPU ayristirma kancalari */
void proc_add_io_bytes(uint64_t bytes);
void proc_add_net_bytes(uint64_t bytes);
void proc_desktop_frame_begin(void);
void proc_desktop_mark_reap(void);
void proc_desktop_frame_end_if_armed(void);
void proc_desktop_blit_tsc(uint64_t tsc_delta);

/* E1: OOM Basinc & Dondurma/Cozme/Sonlandirma API'si */
int  proc_oom_state(void);
void proc_oom_freeze_apps(void);
void proc_oom_thaw_apps(void);
int  proc_oom_list_apps(asi_oom_app_t *out, int max_out);
int  proc_oom_kill_app(int pid);

#endif
