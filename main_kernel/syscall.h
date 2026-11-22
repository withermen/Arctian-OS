#ifndef ARCTIAN_SYSCALL_H
#define ARCTIAN_SYSCALL_H

#include <stdint.h>
#include "isr.h"

#define SYS_EXIT    0
#define SYS_WRITE   1
#define SYS_PUTCHAR 2
#define SYS_GETPID  3
#define SYS_TICKS   4
#define SYS_OPEN    5
#define SYS_READ    6
#define SYS_CLOSE   7
/* B3: izole .wlf GUI koprusu. CALL = wolf_api_t trampoline cagrisi
 * (rax = SYS_WOLF_CALL + tablo indeksi); WAIT = olay kuyrugunda bekle
 * (rdi = kullanici alanindaki olay tamponu). */
#define SYS_WOLF_CALL 8
#define SYS_WOLF_WAIT 9

typedef int (*arctian_cap_check_fn)(uint32_t cap);
void arctian_set_cap_check(arctian_cap_check_fn fn);

typedef void (*arctian_subject_fn)(uint32_t *uid, uint32_t *gid);
void arctian_set_subject_fn(arctian_subject_fn fn);

/* SYS_GETPID / SYS_TICKS ve fd tablolarinin süreç kimligi icin.
 * baglayan yoktursa GETPID 1, TICKS 0 doner (esek davranisi). */
typedef int      (*arctian_pid_fn)(void);
typedef uint64_t (*arctian_ticks_fn)(void);
void arctian_set_pid_fn(arctian_pid_fn fn);
void arctian_set_ticks_fn(arctian_ticks_fn fn);

/* B3: kernel_plus wolf koprusu kanca lari. Baglayan yoksa -1 doner. */
typedef int64_t (*arctian_wolf_call_fn)(uint32_t idx, uint64_t a1, uint64_t a2,
                                        uint64_t a3, uint64_t a4, uint64_t a5,
                                        uint64_t a6);
typedef int64_t (*arctian_wolf_wait_fn)(uint64_t buf_va);
void arctian_set_wolf_call_fn(arctian_wolf_call_fn fn);
void arctian_set_wolf_wait_fn(arctian_wolf_wait_fn fn);

void syscall_init(void);
void syscall_dispatch(registers_t *regs);

#endif
