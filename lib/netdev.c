/*
 * Arctian - netdev kayit defteri
 *
 * Cok kucuk ve bilincli olarak "aptal": politika yok, kuyruk yok, kilit yok.
 * Tek cekirdekli 0.9'da surucu init sirasinda tek iş parçacigi yazar; SMP
 * geldiginde kayit defteri sadece okuma tarafinda kullanilir (kayitlar acilista
 * biter). Bu yuzden burada spinlock yok; SMP'de kayit yarisi olusursa S6
 * kendi kilidini ekler ve bu not guncellenir.
 */
#include "netdev.h"

static netdev_t *s_devs[NETDEV_MAX];
static int s_count;

bool netdev_mac_valid(const uint8_t mac[NETDEV_MAC_LEN]) {
    static const uint8_t zero[NETDEV_MAC_LEN] = {0};
    static const uint8_t bc[NETDEV_MAC_LEN]   = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    bool all_zero = true, all_bc = true;
    for (int i = 0; i < NETDEV_MAC_LEN; i++) {
        if (mac[i] != zero[i]) all_zero = false;
        if (mac[i] != bc[i])   all_bc = false;
    }
    /* Grup biti set ise unicast degil (multicast/broadcast) - NIC MAC'i olamaz. */
    if (mac[0] & 0x01) return false;
    return !all_zero && !all_bc;
}

int netdev_register(netdev_t *dev, const netdev_ops_t *ops, void *priv) {
    if (!dev || !ops) return -1;
    if (s_count >= NETDEV_MAX) return -2;
    for (int i = 0; i < s_count; i++) {
        if (s_devs[i] == dev) return -3;
    }

    dev->ops   = ops;
    dev->priv  = priv;
    dev->index = (uint8_t)s_count;
    dev->mtu   = 1500;
    dev->up    = false;
    dev->link  = false;

    const char *n = ops->name ? ops->name : "net";
    int i = 0;
    for (; i < NETDEV_NAME_LEN - 1 && n[i]; i++) dev->name[i] = n[i];
    dev->name[i] = '\0';

    s_devs[s_count++] = dev;
    return 0;
}

int netdev_count(void) {
    return s_count;
}

netdev_t *netdev_at(int i) {
    if (i < 0 || i >= s_count) return 0;
    return s_devs[i];
}

netdev_t *netdev_primary(void) {
    for (int i = 0; i < s_count; i++) {
        if (s_devs[i]->link) return s_devs[i];
    }
    for (int i = 0; i < s_count; i++) {
        if (s_devs[i]->up) return s_devs[i];
    }
    return s_count ? s_devs[0] : 0;
}

bool netdev_send(netdev_t *dev, const uint8_t *frame, uint16_t len) {
    if (!dev || !dev->ops || !dev->ops->send) return false;
    if (!dev->up) return false;
    return dev->ops->send(dev, frame, len);
}

int netdev_recv(netdev_t *dev, uint8_t *buf, uint16_t cap, uint16_t *out_len) {
    if (!dev || !dev->ops || !dev->ops->recv) return -1;
    if (!dev->up) return -1;
    return dev->ops->recv(dev, buf, cap, out_len);
}

void netdev_set_mac(netdev_t *dev, const uint8_t mac[NETDEV_MAC_LEN]) {
    if (!dev || !mac) return;
    for (int i = 0; i < NETDEV_MAC_LEN; i++) dev->mac[i] = mac[i];
}
