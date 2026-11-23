/* Arctian Ring-3 Grafik & Girdi Koprusu — ortak kullanici/cekirdek ABI.
 *
 * Kullanim (Ring-3 C / C++20 uygulamasi):
 *   int wfd = arctian_gfx_create("Pencere", 640, 480, 0);
 *   uint32_t *fb = mmap(NULL, 640*480*4, PROT_READ|PROT_WRITE, MAP_SHARED, wfd, 0);
 *   // ARGB8888 cizim ...
 *   arctian_gfx_present(wfd, x, y, w, h);
 *   // poll(&pfd, 1, timeout) + read(wfd, &ev, sizeof(ev)) ile fare/klavye/tekerlek!
 */
#ifndef ARCTIAN_POSIX_GFX_H
#define ARCTIAN_POSIX_GFX_H

#include <stdint.h>

enum {
    ARCTIAN_EV_NONE        = 0,
    ARCTIAN_EV_MOUSE_MOVE  = 1,
    ARCTIAN_EV_MOUSE_DOWN  = 2,
    ARCTIAN_EV_MOUSE_UP    = 3,
    ARCTIAN_EV_MOUSE_WHEEL = 4,
    ARCTIAN_EV_KEY_DOWN    = 5,
    ARCTIAN_EV_KEY_UP      = 6,
    ARCTIAN_EV_RESIZE      = 7,
    ARCTIAN_EV_CLOSE       = 8
};

typedef struct {
    uint32_t type;       /* ARCTIAN_EV_* */
    int32_t  x;          /* fare X veya yeni genislik */
    int32_t  y;          /* fare Y veya yeni yukseklik */
    int32_t  delta;      /* tekerlek deltasi veya buton (1=sol,2=sag,3=orta) */
    uint32_t key;        /* ASCII / LVGL tus kodu */
    uint32_t mods;       /* Shift=1, Ctrl=2, Alt=4 */
} arctian_gfx_event_t;

#endif /* ARCTIAN_POSIX_GFX_H */
