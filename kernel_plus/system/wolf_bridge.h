#ifndef ARCTIAN_WOLF_BRIDGE_H
#define ARCTIAN_WOLF_BRIDGE_H

#include <stdint.h>

/* B3: izole .wlf GUI koprusu (kernel_plus tarafi).
 * Trampoline sayfalari main_kernel/wlf_user.c tarafinda kurulur;
 * burasi syscall dispatcher + olay kuyrugu + LVGL broker'idir. */

int64_t wolf_bridge_call(uint32_t idx, uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6);
int64_t wolf_bridge_wait(uint64_t buf_va);

/* kp init: izole calistirma baslarken/bitarken. */
void wolf_bridge_attach(void *body, void *api, uint64_t buf_va);
void wolf_bridge_detach(void);

/* asi_wlf_t kancalari (masaustu cagirir). */
int  wolf_bridge_event_push(void *body, uint64_t cb, uint64_t user,
                            int64_t wid, int64_t kind);
void wolf_bridge_drain(void);

#endif
