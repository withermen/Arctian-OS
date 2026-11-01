#include "kernel.h"
#include "net.h"
#include "pci.h"

net_if_t net_if;

static net_driver_t net_drv;

extern void e1000_init(net_controller_t *nc);
extern bool e1000_send_packet(void *ctx, const uint8_t *data, uint16_t length);
extern bool e1000_receive_packet(void *ctx, uint8_t *buf, uint16_t *length);
extern bool e1000_has_packet(void *ctx);

extern void rtl8139_init(net_controller_t *nc);
extern bool rtl8139_send_packet(void *ctx, const uint8_t *data, uint16_t length);
extern bool rtl8139_receive_packet(void *ctx, uint8_t *buf, uint16_t *length);
extern bool rtl8139_has_packet(void *ctx);

extern void pcnet_init(net_controller_t *nc);
extern bool pcnet_send_packet(void *ctx, const uint8_t *data, uint16_t length);
extern bool pcnet_receive_packet(void *ctx, uint8_t *buf, uint16_t *length);
extern bool pcnet_has_packet(void *ctx);
extern bool pcnet_link_up(void);

extern void rtl8169_init(net_controller_t *nc);
extern bool rtl8169_send_packet(void *ctx, const uint8_t *data, uint16_t length);
extern bool rtl8169_receive_packet(void *ctx, uint8_t *buf, uint16_t *length);
extern bool rtl8169_has_packet(void *ctx);

extern void wifi_init(net_controller_t *nc);
extern bool wifi_send_packet(void *ctx, const uint8_t *data, uint16_t length);
extern bool wifi_has_packet(void *ctx);
extern bool wifi_receive_packet(void *ctx, uint8_t *buf, uint16_t *length);

void net_init(void) {
    net_if.valid = false;
    net_if.rx_head = 0;
    net_if.rx_tail = 0;
    net_if.queue_count = 0;
    for (int i = 0; i < NET_RING_SIZE; i++) {
        net_if.pkts[i].length = 0;
        net_if.pkts[i].in_use = false;
    }

    net_drv.type = -1;
    net_drv.driver_ctx = 0;
    net_drv.send_packet = 0;
    net_drv.has_packet = 0;
    net_drv.receive_packet = 0;

    kernel_debug("NET: Ag sistemi baslatiliyor...");

    for (int i = 0; i < net_controller_count; i++) {
        net_controller_t *nc = &net_controllers[i];

        if (nc->initialized) continue;

        if (nc->net_type == NET_TYPE_E1000) {
            kernel_debug("NET: E1000 sürücüsü yükleniyor #%d...", i);
            e1000_init(nc);
            if (nc->initialized) {
                net_drv.type = NET_TYPE_E1000;
                net_drv.driver_ctx = nc;
                net_drv.send_packet = e1000_send_packet;
                net_drv.has_packet = e1000_has_packet;
                net_drv.receive_packet = e1000_receive_packet;
                kernel_debug("NET: E1000 hazir!");
                break;
            }
        } else if (nc->net_type == NET_TYPE_RTL8139) {
            kernel_debug("NET: RTL8139 sürücüsü yükleniyor #%d...", i);
            rtl8139_init(nc);
            if (nc->initialized) {
                net_drv.type = NET_TYPE_RTL8139;
                net_drv.driver_ctx = nc;
                net_drv.send_packet = rtl8139_send_packet;
                net_drv.has_packet = rtl8139_has_packet;
                net_drv.receive_packet = rtl8139_receive_packet;
                kernel_debug("NET: RTL8139 hazir!");
                break;
            }
        } else if (nc->net_type == NET_TYPE_RTL8169) {
            kernel_debug("NET: RTL8169 sürücüsü yükleniyor #%d...", i);
            rtl8169_init(nc);
            if (nc->initialized) {
                net_drv.type = NET_TYPE_RTL8169;
                net_drv.driver_ctx = nc;
                net_drv.send_packet = rtl8169_send_packet;
                net_drv.has_packet = rtl8169_has_packet;
                net_drv.receive_packet = rtl8169_receive_packet;
                kernel_debug("NET: RTL8169 hazir!");
                break;
            }
        } else if (nc->net_type == NET_TYPE_PCNET) {
            kernel_debug("NET: PCnet sürücüsü yükleniyor #%d...", i);
            pcnet_init(nc);
            if (nc->initialized) {
                net_drv.type = NET_TYPE_PCNET;
                net_drv.driver_ctx = nc;
                net_drv.send_packet = pcnet_send_packet;
                net_drv.has_packet = pcnet_has_packet;
                net_drv.receive_packet = pcnet_receive_packet;
                kernel_debug("NET: PCnet hazir!");
                break;
            }
        } else if (nc->net_type == NET_TYPE_WIFI) {
            kernel_debug("NET: WiFi sürücüsü yükleniyor #%d...", i);
            wifi_init(nc);
            if (nc->initialized) {
                net_drv.type = NET_TYPE_WIFI;
                net_drv.driver_ctx = nc;
                net_drv.send_packet = wifi_send_packet;
                net_drv.has_packet = wifi_has_packet;
                net_drv.receive_packet = wifi_receive_packet;
                kernel_debug("NET: WiFi karti tespit edildi");
            }
        }
    }

    if (net_drv.type == -1) {
        kernel_debug("NET: Ag karti sürücüsü yüklenemedi!");
    }
}

bool net_send_packet(const uint8_t *data, uint16_t length) {
    if (!net_drv.send_packet || !net_drv.driver_ctx) return false;
    if (length > NET_PKT_MAX) return false;
    return net_drv.send_packet(net_drv.driver_ctx, data, length);
}

bool net_receive_packet(uint8_t *buf, uint16_t *length) {
    if (!net_drv.receive_packet || !net_drv.driver_ctx) return false;

    if (net_if.queue_count > 0) {
        uint16_t idx = net_if.rx_tail;
        if (buf && length && *length >= net_if.pkts[idx].length) {
            memcpy(buf, net_if.pkts[idx].data, net_if.pkts[idx].length);
            *length = net_if.pkts[idx].length;
        } else if (length) {
            *length = net_if.pkts[idx].length;
            return false;
        }
        net_if.pkts[idx].in_use = false;
        net_if.rx_tail = (net_if.rx_tail + 1) % NET_RING_SIZE;
        net_if.queue_count--;
        return true;
    }

    uint8_t temp[NET_PKT_MAX];
    uint16_t pkt_len = NET_PKT_MAX;
    bool ok = net_drv.receive_packet(net_drv.driver_ctx, temp, &pkt_len);
    if (ok && buf && length && *length >= pkt_len) {
        memcpy(buf, temp, pkt_len);
        *length = pkt_len;
    } else if (ok && length) {
        *length = pkt_len;
        return false;
    }
    return ok;
}

bool net_has_packet(void) {
    if (net_if.queue_count > 0) return true;
    if (net_drv.has_packet && net_drv.driver_ctx) {
        return net_drv.has_packet(net_drv.driver_ctx);
    }
    return false;
}

const uint8_t *net_get_mac(void) {
    if (net_if.valid) return net_if.mac;
    return 0;
}

/* RDRAND varsa ondan, yoksa zaman damgasi karisimindan 32-bit rastgele.
 * DHCP xid'i ve DNS xid/kaynak port'u MAC'ten turetiliyordu ya da sirali
 * idi; yerel agda taklit eden biri dogru degeri tahmin edebiliyordu. */
uint32_t net_rand32(void) {
    static int has_rdrand = -1;
    if (has_rdrand < 0) {
        uint32_t a, b, c, d;
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
        has_rdrand = (c & (1u << 30)) ? 1 : 0;
    }
    if (has_rdrand) {
        for (int t = 0; t < 32; t++) {
            uint32_t v; uint8_t ok;
            __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
            if (ok) return v;
        }
    }
    uint64_t tsc;
    __asm__ volatile("rdtsc" : "=A"(tsc));
    static uint64_t mix = 0x9E3779B97F4A7C15ull;
    mix ^= tsc + 0xA5A5A5A5A5A5A5A5ull;
    mix *= 0x100000001B3ull;
    return (uint32_t)(mix >> 32) ^ (uint32_t)mix;
}

extern bool e1000_link_up(void);

int net_driver_type(void) { return net_drv.type; }

bool net_link_up(void) {
    if (net_drv.type == NET_TYPE_E1000) return e1000_link_up();
    if (net_drv.type == NET_TYPE_RTL8139) return true;  /* RTL: link varsayimi */
    if (net_drv.type == NET_TYPE_PCNET) return pcnet_link_up();
    return false;
}
