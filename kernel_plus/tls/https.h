#ifndef ARCTIAN_HTTPS_H
#define ARCTIAN_HTTPS_H

#include <stdint.h>
#include <stdbool.h>

/* mbedTLS tabanli HTTPS (TLS 1.3) istemcisi. */

/* Platform/TLS altyapisini baslatir (birden fazla cagrilabilir). */
void https_init(void);

/* https://host:port/path icin GET yapar. Yanit govdesi dahili tamponda
 * doner ve bir sonraki cagriya kadar gecerlidir. Hata: NULL. */
const char *https_get(const char *host, uint16_t port, const char *path,
                      uint32_t *len_out);

/* "https://host[:port]/path" seklindeki URL'yi ayristirip https_get yapar. */
const char *https_get_url(const char *url, uint32_t *len_out);

/* http/https ortak: yonlendirmeleri takip eder, chunked cozer, YALNIZCA
 * govdeyi dondurur (bir sonraki cagriya kadar gecerli). Hata: NULL. */
const char *https_fetch_body(const char *url, uint32_t *len_out);

/* Son https_fetch_body cagrisinin ulastigi nihai (yonlendirme sonrasi) URL. */
const char *https_fetch_last_url(void);

/* 0.8.2: Akisli indirme. Govde parcalari geldikce sink'e verilir (0 = devam,
 * <0 = iptal). Content-Length govdeyi bilmiyorsa baglanti kapanisina kadar
 * akar; chunked govde reddedilir. Yonlendirme takip eder. Donus: govde bayt
 * sayisi ya da <0 hata. */
typedef int (*https_sink_fn)(void *ctx, const uint8_t *data, uint32_t len);
int https_stream(const char *url, https_sink_fn sink, void *ctx);

/* Son TLS hatasini okunabilir metin olarak yazar (hata ayiklama). */
void https_last_error(char *buf, int cap);

/* JS document.cookie tarafindan yazilan cerezi depoya ekler. */
void https_cookie_set(const char *host, const char *name, const char *value);

/* cookie_set'in secure bayrakli hali; secure=1 cerez yalnizca TLS
 * baglantilarina gonderilir (madde 27). */
void https_cookie_set2(const char *host, const char *name, const char *value,
                       int secure);

/* Cerez deposunun http tarafi (madde 27): duz http isteklerine
 * "Cookie: ..." basligi kurar (tls=0; Secure cerezler elenir) ve http
 * yanitindaki Set-Cookie satirlarini depoya isler. */
int  https_ck_build(const char *host, int tls, char *out, int cap);
void https_ck_absorb(const char *hdr, uint32_t hlen, const char *host, int tls);

#endif /* ARCTIAN_HTTPS_H */
