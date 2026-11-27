/* Arctian OS Ring-3 Grafik & Girdi Koprusu (Kullanici Alani C/C++20 Basligi) */
#ifndef ARCTIAN_USER_GFX_H
#define ARCTIAN_USER_GFX_H

#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/mman.h>
#include "posix_gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARCTIAN_SYS_GFX_CREATE  500
#define ARCTIAN_SYS_GFX_PRESENT 501
#define ARCTIAN_SYS_GFX_POLL    502
#define ARCTIAN_SYS_GFX_RESIZE  503
#define ARCTIAN_SYS_GFX_DESTROY 504
#define ARCTIAN_SYS_SHM_CREATE  505
#define ARCTIAN_SYS_CLIP_GET    506
#define ARCTIAN_SYS_CLIP_SET    507

static inline long arctian_syscall6(long n, long a1, long a2, long a3,
                                    long a4, long a5, long a6) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline int arctian_gfx_create(const char *title, uint32_t w, uint32_t h, uint32_t flags) {
    return (int)arctian_syscall6(ARCTIAN_SYS_GFX_CREATE, (long)(uintptr_t)title,
                                 (long)w, (long)h, (long)flags, 0, 0);
}

static inline uint32_t *arctian_gfx_mmap(int win_fd, uint32_t w, uint32_t h) {
    size_t sz = (size_t)w * (size_t)h * 4u;
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, win_fd, 0);
    return (p == MAP_FAILED) ? NULL : (uint32_t *)p;
}

static inline int arctian_gfx_present(int win_fd, int32_t x, int32_t y, int32_t w, int32_t h) {
    return (int)arctian_syscall6(ARCTIAN_SYS_GFX_PRESENT, (long)win_fd,
                                 (long)x, (long)y, (long)w, (long)h, 0);
}

static inline int arctian_gfx_poll(int win_fd, arctian_gfx_event_t *ev, int timeout_ms) {
    return (int)arctian_syscall6(ARCTIAN_SYS_GFX_POLL, (long)win_fd,
                                 (long)(uintptr_t)ev, (long)timeout_ms, 0, 0, 0);
}

static inline int arctian_gfx_resize(int win_fd, uint32_t w, uint32_t h) {
    return (int)arctian_syscall6(ARCTIAN_SYS_GFX_RESIZE, (long)win_fd,
                                 (long)w, (long)h, 0, 0, 0);
}

static inline int arctian_gfx_destroy(int win_fd) {
    return (int)arctian_syscall6(ARCTIAN_SYS_GFX_DESTROY, (long)win_fd, 0, 0, 0, 0, 0);
}

static inline int arctian_shm_create(size_t size) {
    return (int)arctian_syscall6(ARCTIAN_SYS_SHM_CREATE, (long)size, 0, 0, 0, 0, 0);
}

static inline int arctian_clip_get(char *buf, size_t cap) {
    return (int)arctian_syscall6(ARCTIAN_SYS_CLIP_GET, (long)(uintptr_t)buf,
                                 (long)cap, 0, 0, 0, 0);
}

static inline int arctian_clip_set(const char *str, size_t len) {
    return (int)arctian_syscall6(ARCTIAN_SYS_CLIP_SET, (long)(uintptr_t)str,
                                 (long)len, 0, 0, 0, 0);
}

#ifdef __cplusplus
}
#endif

#endif /* ARCTIAN_USER_GFX_H */
