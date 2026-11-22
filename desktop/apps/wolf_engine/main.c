/* Wolf Engine — JavaScript motoru (yer tutucu).
 *
 * QuickJS 0.9.1'de tamamen kaldirildi. Tarayicinin kendi JS motoru
 * ayri yazilacak. Bu dosya yalnizca ABI'yi korur: terminal ve testler
 * ayni sembolleri cagirmaya devam eder. Motor yokken eval -3 dondurur
 * ve cagiran taraf "Calistirma hatasi" gosterir; hicbir yerde cokme
 * olmaz.
 *
 * Disa bagimlilik yok (yalnizca libc'nin memcpy/strlen'i). */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "wolf_engine.h"
#include "asi.h"

static wolf_print_fn g_print_cb = 0;
static wolf_host_t    g_host;
static int            g_have_host = 0;

unsigned wolf_engine_version(void) { return 0; }   /* 0 = motor yok */

void wolf_engine_set_print(wolf_print_fn cb) { g_print_cb = cb; }

void wolf_engine_set_host(const wolf_host_t *host) {
    if (!host) return;
    memcpy(&g_host, host, sizeof(g_host));
    g_have_host = 1;
}

void wolf_engine_set_limits(unsigned steps, unsigned depth) {
    (void)steps;
    (void)depth;
}

int wolf_engine_eval(const char *src, unsigned len, char *out, unsigned out_cap) {
    (void)src;
    (void)len;
    if (out && out_cap) {
        const char *m = "JS motoru yok";
        size_t n = strlen(m);
        if (n > (size_t)out_cap - 1) n = (size_t)out_cap - 1;
        memcpy(out, m, n);
        out[n] = 0;
    }
    return -3;
}

void wolf_engine_reset(void) { }

void wolf_engine_dispatch_event(long long handle, const char *ev, unsigned len) {
    (void)handle;
    (void)ev;
    (void)len;
}

unsigned wolf_engine_listener_count(void) { return 0; }

void app_entry(void *win, asi_t *asi) { (void)win; (void)asi; }

/* Kullanilmayan alanlarin uyari vermemesi icin tek noktadan referans. */
void wolf_engine_stub_refs(void) { (void)g_print_cb; (void)g_have_host; }
