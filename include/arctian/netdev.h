/*
 * Arctian - ortak ag aygiti arayuzu
 *
 * Amac: her surucu kendi ozel global'ini degil, ayni `netdev_t` sozlesmesini
 * doldursun. Boylece net.c tek bir "birincil aygit" uzerinden calisirken
 * birden fazla NIC kayitli olabilir (oncelik: kablo bagli olan).
 *
 * Uygulama: lib/netdev.c (kayit defteri, en fazla NETDEV_MAX aygit)
 * Kullanan: kernel_plus/drivers/net.c + e1000/rtl8168/rtl8125/virtio-net
 *
 * KURAL: surucu `netdev_ops_t` tablosunu statik tutar; `netdev_t` ornegi
 * surucunun kendi global'idir (kayit kopyalamaz, isaretci saklar).
 */
#ifndef ARCTIAN_NETDEV_H
#define ARCTIAN_NETDEV_H

#include <stdint.h>
#include <stdbool.h>

#define NETDEV_MAC_LEN   6
#define NETDEV_NAME_LEN  16
#define NETDEV_MAX       4

typedef struct netdev netdev_t;

typedef struct {
    const char *name;   /* "e1000e", "rtl8168", "rtl8125", "virtio-net" */

    /* Donanim hazir mi? Kayit aninda cagrilir. */
    bool (*init)(netdev_t *dev);

    /* Tek cerceve gonder. false = kuyruk dolu / hata. */
    bool (*send)(netdev_t *dev, const uint8_t *frame, uint16_t len);

    /* Bekleyen cerceve var mi? (poll modu; IRQ yoksa zorunlu) */
    bool (*has_packet)(netdev_t *dev);

    /* Cerceveyi al. Donen: alinan bayt, <0 hata, 0 paket yok. */
    int  (*recv)(netdev_t *dev, uint8_t *buf, uint16_t cap, uint16_t *out_len);

    /* Kablo/link durumu. NULL ise "bilinmiyor" sayilir. */
    bool (*link_up)(netdev_t *dev);

    /* Kapatma (kapatma sirasinda cagrilir; NULL olabilir). */
    void (*close)(netdev_t *dev);
} netdev_ops_t;

struct netdev {
    const netdev_ops_t *ops;
    void       *priv;               /* surucuya ozel baglam */
    uint8_t     mac[NETDEV_MAC_LEN];
    uint16_t    mtu;
    uint8_t     index;              /* kayit sirasi */
    bool        up;                 /* init basarili */
    bool        link;               /* son olculen link durumu */
    char        name[NETDEV_NAME_LEN];
};

/* Basarili: 0, hata: <0. Ayni aygit iki kez kaydedilemez. */
int netdev_register(netdev_t *dev, const netdev_ops_t *ops, void *priv);

int       netdev_count(void);
netdev_t *netdev_at(int i);

/* Birincil aygit: link'i acik olan ilk aygit, yoksa ilk kayitli. */
netdev_t *netdev_primary(void);

bool netdev_send(netdev_t *dev, const uint8_t *frame, uint16_t len);
int  netdev_recv(netdev_t *dev, uint8_t *buf, uint16_t cap, uint16_t *out_len);
void netdev_set_mac(netdev_t *dev, const uint8_t mac[NETDEV_MAC_LEN]);
bool netdev_mac_valid(const uint8_t mac[NETDEV_MAC_LEN]);

#endif /* ARCTIAN_NETDEV_H */
