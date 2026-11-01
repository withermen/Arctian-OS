#include "ipv4.h"
#include "arp.h"
#include "udp.h"
#include "dhcp.h"
#include "net.h"
#include "net_task.h"
#include "firewall.h"
#include "kernel.h"
#include "serial.h"
#include "clock.h"

extern bool tcp_receive_packet(const uint8_t *src_ip, const uint8_t *packet, uint16_t length);

ipv4_config_t ipv4_config;
uint8_t ipv4_broadcast[4] = {255, 255, 255, 255};

static uint16_t ipv4_id = 0;

uint16_t ipv4_checksum(const uint8_t *data, uint16_t length) {
    uint32_t sum = 0;
    for (uint16_t i = 0; i < length - 1; i += 2) {
        sum += ((uint16_t)data[i] << 8) | data[i + 1];
    }
    if (length & 1) sum += (uint16_t)data[length - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

void ipv4_init(void) {
    ipv4_config.src_ip[0] = 192; ipv4_config.src_ip[1] = 168;
    ipv4_config.src_ip[2] = 1;   ipv4_config.src_ip[3] = 100;
    ipv4_config.gateway[0] = 192; ipv4_config.gateway[1] = 168;
    ipv4_config.gateway[2] = 1;   ipv4_config.gateway[3] = 1;
    ipv4_config.subnet_mask[0] = 255; ipv4_config.subnet_mask[1] = 255;
    ipv4_config.subnet_mask[2] = 255; ipv4_config.subnet_mask[3] = 0;
    ipv4_config.dns_server[0] = 8; ipv4_config.dns_server[1] = 8;
    ipv4_config.dns_server[2] = 8; ipv4_config.dns_server[3] = 8;
    ipv4_config.mtu = 1500;
    ipv4_config.configured = false;
}

bool ipv4_configure(const uint8_t ip[4], const uint8_t gateway[4],
                    const uint8_t mask[4], const uint8_t dns[4]) {
    for (int i = 0; i < 4; i++) ipv4_config.src_ip[i] = ip[i];
    for (int i = 0; i < 4; i++) ipv4_config.gateway[i] = gateway[i];
    for (int i = 0; i < 4; i++) ipv4_config.subnet_mask[i] = mask[i];
    for (int i = 0; i < 4; i++) ipv4_config.dns_server[i] = dns[i];
    ipv4_config.configured = true;
    serial_printf("[ip] adres %d.%d.%d.%d maske %d.%d.%d.%d gw %d.%d.%d.%d\n",
                  ip[0], ip[1], ip[2], ip[3],
                  mask[0], mask[1], mask[2], mask[3],
                  gateway[0], gateway[1], gateway[2], gateway[3]);
    return true;
}

void ipv4_set_default_gateway(const uint8_t gw[4]) {
    for (int i = 0; i < 4; i++) ipv4_config.gateway[i] = gw[i];
}

void ipv4_config_dhcp(void) {
    /* Once gercek DHCP dene (kopru/NAT aglarinda dogru adresleri verir).
     * Yanit gelmezse QEMU/VirtualBox kullanici-modu NAT (slirp)
     * varsayilanlarina dus. */
    if (dhcp_configure()) {
        serial_printf("[ip] DHCP ile yapilandirildi\n");
        return;
    }

    uint8_t dhcp_ip[4]    = {10, 0, 2, 15};
    uint8_t dhcp_gw[4]    = {10, 0, 2, 2};
    uint8_t dhcp_mask[4]  = {255, 255, 255, 0};
    uint8_t dhcp_dns[4]   = {10, 0, 2, 3};
    ipv4_configure(dhcp_ip, dhcp_gw, dhcp_mask, dhcp_dns);
    serial_printf("[ip] slirp varsayilani: %d.%d.%d.%d gw %d.%d.%d.%d\n",
                  dhcp_ip[0], dhcp_ip[1], dhcp_ip[2], dhcp_ip[3],
                  dhcp_gw[0], dhcp_gw[1], dhcp_gw[2], dhcp_gw[3]);
}

static bool is_same_subnet(const uint8_t a[4], const uint8_t b[4]) {
    for (int i = 0; i < 4; i++) {
        if ((a[i] & ipv4_config.subnet_mask[i]) !=
            (b[i] & ipv4_config.subnet_mask[i])) return false;
    }
    return true;
}

/* Tek bir IP cercevesi kurup suruce verir. `frag` = bayrak+ofset alani
 * (DF/MF/ofset 16 bitlik ham deger), `id` parcalar arasinda ortaktır. */
static bool ip_send_frame(const uint8_t dst_mac[6], const uint8_t *my_mac,
                          const uint8_t dst_ip[4], uint8_t protocol,
                          uint16_t id, uint16_t frag,
                          const uint8_t *data, uint16_t data_len) {
    uint32_t total = 20u + data_len;
    if (total + 14u > NET_PKT_MAX) return false;

    uint8_t packet[NET_PKT_MAX];
    uint16_t pkt_len = (uint16_t)(14u + total);

    for (int i = 0; i < 6; i++) packet[i] = dst_mac[i];
    for (int i = 0; i < 6; i++) packet[6 + i] = my_mac[i];
    packet[12] = 0x08; packet[13] = 0x00;

    uint8_t *iph = packet + 14;
    iph[0] = 0x45;
    iph[1] = 0x00;
    iph[2] = (total >> 8) & 0xFF;
    iph[3] = total & 0xFF;
    iph[4] = (id >> 8) & 0xFF;
    iph[5] = id & 0xFF;
    iph[6] = (frag >> 8) & 0xFF;
    iph[7] = frag & 0xFF;
    iph[8] = 64;
    iph[9] = protocol;
    iph[10] = 0; iph[11] = 0;
    for (int i = 0; i < 4; i++) iph[12 + i] = ipv4_config.src_ip[i];
    for (int i = 0; i < 4; i++) iph[16 + i] = dst_ip[i];

    uint16_t hdr_csum = ipv4_checksum(iph, 20);
    iph[10] = (hdr_csum >> 8) & 0xFF;
    iph[11] = hdr_csum & 0xFF;

    for (uint16_t i = 0; i < data_len; i++) iph[20 + i] = data[i];

    return net_send_packet(packet, pkt_len);
}

bool ipv4_send(const uint8_t dst_ip[4], uint8_t protocol,
               const uint8_t *payload, uint16_t payload_len) {
    if (!ipv4_config.configured) {
        serial_printf("[ip] gonderilemedi: yapilandirma yok\n");
        return false;
    }
    /* Madde 70: MTU asimi eskiden sessiz "basarisiz" demekti; buyuk UDP
     * yazmasi ya da buyuk tekil TCP cercevesi hic yola cikmiyordu. Artik
     * cerceve MTU'yu asiyorsa IP katmani parcalara boler (ayni id, MF=1,
     * ofset 8 bayt katli). TCP zaten MSS boyunda gonderdigi icin buraya
     * nadiren dusur; asil ihtiyaç UDP/DNS ve ileriye kucuk MTU'lar. */
    if (payload_len > 65535u - 20u) {
        serial_printf("[ip] payload ip basligina sigmiyor (%d bayt)\n",
                      (int)payload_len);
        return false;
    }

    uint16_t fw_dport = 0;
    if ((protocol == IPV4_PROTO_TCP || protocol == IPV4_PROTO_UDP) && payload_len >= 4)
        fw_dport = ((uint16_t)payload[2] << 8) | payload[3];
    if (!firewall_outbound(protocol, dst_ip, fw_dport)) {
        serial_printf("[ip] guvenlik duvari engelledi proto=%d port=%d\n",
                      (int)protocol, (int)fw_dport);
        return false;
    }

    const uint8_t *my_mac = net_get_mac();
    if (!my_mac) {
        serial_printf("[ip] gonderilemedi: MAC yok\n");
        return false;
    }

    const uint8_t *next_hop = dst_ip;
    if (!is_same_subnet(ipv4_config.src_ip, dst_ip)) {
        next_hop = ipv4_config.gateway;
    }

    uint8_t dst_mac[6];
    if (!arp_resolve(next_hop, dst_mac)) {
        serial_printf("[ip] ARP basarisiz, paket dusuruldu\n");
        return false;
    }

    uint16_t mtu = ipv4_config.mtu;
    if (mtu < 576) mtu = 576;                  /* IP asgisi (RFC 791) */
    if (mtu > NET_PKT_MAX - 14) mtu = NET_PKT_MAX - 14;
    uint16_t chunk = (uint16_t)((mtu - 20) & ~7u);   /* 8 bayt katli */

    uint16_t id = ipv4_id++;
    uint16_t off = 0;
    for (;;) {
        uint16_t n = (uint16_t)(payload_len - off);
        uint16_t frag;
        if (20u + n <= mtu) {
            /* Son (ya da tek) parca: tek cercevede DF, ara parca ise yok. */
            frag = (off == 0) ? IPV4_FLAG_DF : (uint16_t)(off >> 3);
        } else {
            n = chunk;
            frag = (uint16_t)(IPV4_FLAG_MF | (off >> 3));
        }
        if (!ip_send_frame(dst_mac, my_mac, dst_ip, protocol, id, frag,
                           payload + off, n)) {
            serial_printf("[ip] net_send_packet basarisiz (off=%d len=%d)\n",
                          (int)off, (int)n);
            return false;
        }
        off += n;
        if (off >= payload_len) break;
    }
    return true;
}

/* IP basligini ayristirir. Parcali datagramlarda da basariyla doner;
 * cagiran p.fragmented alanina bakarak birlestirme mi dagitim mi
 * yapacagina karar verir. */
typedef struct {
    uint8_t  src[4], dst[4];
    uint8_t  proto;
    uint16_t id;
    uint16_t frag;            /* bayrak + ofset (ham 16 bit) */
    uint16_t payload_len;
    const uint8_t *payload;
    int      fragmented;      /* MF ayarli ya da ofset != 0 */
} ip_pkt_t;

static int ip4_eq(const uint8_t a[4], const uint8_t b[4]) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static bool ip_parse(const uint8_t *packet, uint16_t length, ip_pkt_t *o) {
    if (length < 34) return false;

    const uint8_t *iph = packet + 14;
    if ((iph[0] >> 4) != 4) return false;

    uint16_t total_len = ((uint16_t)iph[2] << 8) | iph[3];
    if (total_len < 20 || 14 + total_len > length) return false;
    uint16_t hdr_len = (iph[0] & 0x0F) * 4;
    if (hdr_len < 20) return false;

    /* Baslik saglama toplami: gonderenin dogrulugunun tek kaniti.
     * Gecerli baslikta hesap her zaman 0 cikar. */
    if (ipv4_checksum(iph, hdr_len) != 0) return false;

    uint16_t frag = ((uint16_t)iph[6] << 8) | iph[7];

    bool for_us = false;
    for (int i = 0; i < 4; i++) {
        if (iph[16 + i] != ipv4_config.src_ip[i]) break;
        if (i == 3) for_us = true;
    }
    if (!for_us && ipv4_config.src_ip[0]) return false;

    if (o) {
        for (int i = 0; i < 4; i++) { o->src[i] = iph[12 + i]; o->dst[i] = iph[16 + i]; }
        o->proto = iph[9];
        o->id = (uint16_t)(((uint16_t)iph[4] << 8) | iph[5]);
        o->frag = frag;
        o->payload = iph + hdr_len;
        o->payload_len = (uint16_t)(total_len - hdr_len);
        o->fragmented = ((frag & 0x3FFFu) != 0) ? 1 : 0;
    }
    return true;
}

/* TCP/UDP saglama toplamini pseudo-header uzerinden dogrular. Gonderici
 * (tcp_calc_checksum / udp_send) ayni matematigi kullaniyor; gecerli
 * pakette buradaki hesap 0 doner. UDP'de 0 alanasi "hesaplanmadi" demek
 * ve IPv4'te yasal, o durumda geciriyoruz.
 * Baslik isaretcisi degil kaynak/hedef adresleri alinir: birlestirilmis
 * datagramin orijinal IP basligi artik ortada degil. */
static bool l4_checksum_ok(const uint8_t src[4], const uint8_t dst[4],
                           const uint8_t *l4, uint16_t l4_len, uint8_t proto) {
    if (proto == IPV4_PROTO_UDP && l4[6] == 0 && l4[7] == 0) return true;

    uint32_t sum = 0;
    for (int i = 0; i < 4; i++) sum += (uint32_t)src[i] << ((i & 1) ? 0 : 8);
    for (int i = 0; i < 4; i++) sum += (uint32_t)dst[i] << ((i & 1) ? 0 : 8);
    sum += proto;
    sum += l4_len;
    for (uint16_t i = 0; i + 1 < l4_len; i += 2)
        sum += ((uint32_t)l4[i] << 8) | l4[i + 1];
    if (l4_len & 1) sum += (uint32_t)l4[l4_len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum == 0;
}

/* Katmanlara dagitim: ateis duvari + saglama toplami + TCP/UDP.
 * Hem tek cerceveli hem birlestirilmis datagramlar buradan gecer. */
static void ip_dispatch(const uint8_t src[4], const uint8_t dst[4], uint8_t proto,
                        const uint8_t *l4, uint16_t l4_len) {
    uint16_t fw_dport = 0;
    if ((proto == IPV4_PROTO_TCP || proto == IPV4_PROTO_UDP) && l4_len >= 4)
        fw_dport = ((uint16_t)l4[2] << 8) | l4[3];
    if (!firewall_inbound(proto, src, fw_dport)) return;

    if (proto == IPV4_PROTO_TCP || proto == IPV4_PROTO_UDP) {
        if (l4_len < 8) return;
        if (!l4_checksum_ok(src, dst, l4, l4_len, proto)) {
            serial_printf("[ip] saglama toplami bozuk %s paketi dusuruldu\n",
                          proto == IPV4_PROTO_TCP ? "tcp" : "udp");
            return;
        }
    }
    if (proto == IPV4_PROTO_TCP) tcp_receive_packet(src, l4, l4_len);
    else if (proto == IPV4_PROTO_UDP) udp_dispatch(src, l4, l4_len);
}

bool ipv4_receive(const uint8_t *packet, uint16_t length,
                  uint8_t *protocol_out, uint8_t *src_ip_out,
                  const uint8_t **payload_out, uint16_t *payload_len_out) {
    ip_pkt_t p;
    if (!ip_parse(packet, length, &p)) return false;
    /* Bu API parca birlestirmez; parcali datagramlari poll yolu
     * (ipv4_poll -> frag_feed) birlestirir. */
    if (p.fragmented) return false;
    if (protocol_out) *protocol_out = p.proto;
    if (src_ip_out) for (int i = 0; i < 4; i++) src_ip_out[i] = p.src[i];
    if (payload_out) *payload_out = p.payload;
    if (payload_len_out) *payload_len_out = p.payload_len;
    return true;
}

/* ------------------------------------------------------------------ */
/* Parca birlestirme (madde 70)                                        */
/* ------------------------------------------------------------------ */
/* IP parcalari eskiden tek satirla dusuruluyordu, yani "IP fragmentasyonu
 * var" gorunuyordu ama yoktu: 1500 bayttan buyuk yanit gonderen bir
 * sunucunun (ozellikle UDP/DNS ya da kucuk MTU'lu aglar) verisi hicbir
 * katmana ulasmadan kayboluyordu. Asagidaki tablo sinirli sayida
 * datagrami birlestirir:
 *   - ayni (kaynak, hedef, protokol, kimlik) uclusu ayni datagramdir;
 *   - her parca 8 baytlik blok isaretiyle izlenir; cakisan parca
 *     (teardrop benzeri) tum datagrami dusurur (RFC 5722);
 *   - son parca (MF=0) geldiginde toplam uzunluk bilinir; tum bloklar
 *     kapandiysa datagram tamamlanir;
 *   - yarim kalan datagramlar IP_FRAG_TMO ms sonra birakilir.
 */
#define IP_FRAG_MAX   4096u
#define IP_FRAG_SLOTS 2
#define IP_FRAG_TMO   5000ull

typedef struct {
    int      used;
    uint8_t  src[4], dst[4];
    uint16_t id;
    uint8_t  proto;
    uint64_t t0;                       /* ilk parcanin zamani */
    uint16_t total;                    /* 0 = son parca henuz gelmedi */
    uint8_t  got[(IP_FRAG_MAX / 8u) + 1u];
    uint8_t  data[IP_FRAG_MAX];
} ip_frag_t;

static ip_frag_t s_frag[IP_FRAG_SLOTS];

static void frag_release(ip_frag_t *f) {
    if (!f) return;
    f->used = 0;
    f->total = 0;
}

/* Zamanasimina ugrayan yarim datagramlari birak. */
static void frag_sweep(uint64_t now) {
    for (int i = 0; i < IP_FRAG_SLOTS; i++) {
        ip_frag_t *f = &s_frag[i];
        if (!f->used) continue;
        if (now - f->t0 > IP_FRAG_TMO) {
            serial_printf("[ip] parca birlestirme zaman asimi id=0x%x\n",
                          (unsigned)f->id);
            frag_release(f);
        }
    }
}

static ip_frag_t *frag_slot(const ip_pkt_t *p, uint64_t now) {
    for (int i = 0; i < IP_FRAG_SLOTS; i++) {
        ip_frag_t *f = &s_frag[i];
        if (f->used && f->id == p->id && f->proto == p->proto &&
            ip4_eq(f->src, p->src) && ip4_eq(f->dst, p->dst))
            return f;
    }
    ip_frag_t *slot = 0;
    for (int i = 0; i < IP_FRAG_SLOTS; i++) {
        ip_frag_t *f = &s_frag[i];
        if (!f->used) { slot = f; break; }
        if (!slot || f->t0 < slot->t0) slot = f;
    }
    if (!slot) return 0;
    /* Yer yok: en eskiyi at (gelen trafik, yarim kalmis eskisinden
     * degerli; eskiden burada hicbir sey yapilmazdi da). */
    for (unsigned i = 0; i < sizeof(slot->got); i++) slot->got[i] = 0;
    slot->used = 1;
    slot->total = 0;
    slot->t0 = now;
    slot->id = p->id;
    slot->proto = p->proto;
    for (int i = 0; i < 4; i++) { slot->src[i] = p->src[i]; slot->dst[i] = p->dst[i]; }
    return slot;
}

static int frag_all_got(const ip_frag_t *f, uint16_t total) {
    uint16_t blocks = (uint16_t)((total + 7u) / 8u);
    for (uint16_t b = 0; b < blocks; b++)
        if (!(f->got[b >> 3] & (uint8_t)(1u << (b & 7)))) return 0;
    return 1;
}

/* Bir parca besle. Donus: >=0 = datagram tamamlandi, yuva indeksi
 * (cagiran isini bitirdikten sonra frag_release ile birakmali);
 * -1 = bekleniyor ya da dusuruldu. */
static int frag_feed(const ip_pkt_t *p, uint64_t now,
                     const uint8_t **out, uint16_t *out_len) {
    uint16_t off = (uint16_t)(((uint32_t)(p->frag & 0x1FFFu)) * 8u);
    int mf = (p->frag & IPV4_FLAG_MF) ? 1 : 0;
    uint16_t len = p->payload_len;
    if (!len) return -1;
    if (mf && (len & 7u)) return -1;              /* 8'in kati olmali */
    if ((uint32_t)off + len > IP_FRAG_MAX) {
        serial_printf("[ip] parca siniri asiyor (off=%d len=%d)\n", (int)off, (int)len);
        return -1;
    }

    ip_frag_t *f = frag_slot(p, now);
    if (!f) return -1;

    uint16_t b0 = (uint16_t)(off >> 3);
    uint16_t b1 = (uint16_t)((off + len + 7u) >> 3);
    for (uint16_t b = b0; b < b1; b++) {
        if (f->got[b >> 3] & (uint8_t)(1u << (b & 7))) {
            /* Cakisma: datagramin tamami birakilir. */
            serial_printf("[ip] parca cakismasi id=0x%x - datagram birakildi\n",
                          (unsigned)f->id);
            frag_release(f);
            return -1;
        }
    }
    for (uint16_t i = 0; i < len; i++) f->data[off + i] = p->payload[i];
    for (uint16_t b = b0; b < b1; b++) f->got[b >> 3] |= (uint8_t)(1u << (b & 7));

    if (!mf) f->total = (uint16_t)(off + len);
    if (!f->total) return -1;                       /* son parca bekleniyor */
    if (!frag_all_got(f, f->total)) return -1;      /* henuz acik var */

    *out = f->data;
    *out_len = f->total;
    return (int)(f - s_frag);
}

/* NIC'ten bekleyen tum kareleri bosaltir ve katmanlara dagitir.
 * Yeniden giris atomik test-isset ile karsilanir: eski surumde flag
 * okuma-yazma ile yapiliyordu, ic cagrinin erkenden temizledigi flag
 * uzerinden poll tekrar girise aciliyordu (madde 34). */
static volatile uint32_t s_poll_busy;

static int poll_enter(void) {
    uint32_t busy;
    __asm__ volatile("lock xchgl %0, %1"
                     : "=r"(busy), "+m"(s_poll_busy) : "0"(1u) : "memory");
    return busy == 0;
}

int ipv4_poll_nested(void) { return s_poll_busy != 0; }

static void ipv4_drain(int handle_ip) {
    for (int n = 0; n < 64; n++) {
        uint8_t rx_buf[NET_PKT_MAX];
        uint16_t rx_len = sizeof(rx_buf);
        if (!net_receive_packet(rx_buf, &rx_len)) break;
        if (rx_len < 14) continue;

        uint16_t ether_type = ((uint16_t)rx_buf[12] << 8) | rx_buf[13];
        if (ether_type == 0x0806) {
            arp_receive(rx_buf, rx_len);
            continue;
        }
        if (!handle_ip) continue;   /* ic-ice ARP bosaltmasi: katmanlara
                                     * dokunma, TCP/UDP kuyrugunu karistir */
        if (ether_type != 0x0800) continue;

        ip_pkt_t p;
        if (!ip_parse(rx_buf, rx_len, &p)) continue;

        if (!p.fragmented) {
            ip_dispatch(p.src, p.dst, p.proto, p.payload, p.payload_len);
            continue;
        }

        /* Parcali datagram: birlestir, tamamlaninca dagit. */
        uint64_t now = clock_ms();
        frag_sweep(now);
        const uint8_t *msg = 0;
        uint16_t mlen = 0;
        int slot = frag_feed(&p, now, &msg, &mlen);
        if (slot < 0) continue;
        ip_dispatch(p.src, p.dst, p.proto, msg, mlen);
        frag_release(&s_frag[slot]);
    }
}

void ipv4_poll(void) {
    if (!poll_enter()) return;
    ipv4_drain(1);
    s_poll_busy = 0;
    __asm__ volatile("" ::: "memory");
    net_task_yield_point();
}

/* ipv4_poll icinden tetiklenen ARP cozumlemesi icin: yalnizca ARP
 * cercevelerini isler (onekbellek guncellenir, isteklere yanit).
 * TCP/UDP cerceveleri bu kisitli modda dusurulur; onlar dis dongunun
 * sonraki turunda yine karsisina cikacaktir. Boylece poll sirasinda
 * ARP cozumlemek imkansiz olmaktan cikar (madde 34). */
void ipv4_poll_arp_only(void) {
    if (!poll_enter()) return;      /* gercekten ic-ice: bekleme yok */
    ipv4_drain(0);
    s_poll_busy = 0;
    __asm__ volatile("" ::: "memory");
}

/* ------------------------------------------------------------------ */
/* Parcalama/birlestirme kendini-sinavi (madde 70)                     */
/* ------------------------------------------------------------------ */
/* Ag cikisi gerektirmez: cerceveler elle kurulur ve dogrudan ayristirici
 * ile birlestiriciye verilir. Cerceve tamamlaninca TCP/UDP'ye dagitim
 * yapilmaz (slot bosaltilir), yani test canli kuyruklara dokunmaz.
 * Donus: 0 = basarili, 1 = hata. */
static void fragtest_frame(uint8_t *b, uint16_t id, uint16_t frag,
                           const uint8_t *data, uint16_t dlen) {
    for (int i = 0; i < 6; i++) { b[i] = 0x52; b[6 + i] = 0x52; }  /* dummy MAC */
    b[12] = 0x08; b[13] = 0x00;
    uint8_t *iph = b + 14;
    iph[0] = 0x45; iph[1] = 0x00;
    uint16_t total = (uint16_t)(20 + dlen);
    iph[2] = (total >> 8) & 0xFF; iph[3] = total & 0xFF;
    iph[4] = (id >> 8) & 0xFF;    iph[5] = id & 0xFF;
    iph[6] = (frag >> 8) & 0xFF;  iph[7] = frag & 0xFF;
    iph[8] = 64; iph[9] = IPV4_PROTO_UDP;
    iph[10] = 0; iph[11] = 0;
    uint8_t peer[4] = { 10, 0, 2, 99 };
    for (int i = 0; i < 4; i++) iph[12 + i] = peer[i];
    for (int i = 0; i < 4; i++) iph[16 + i] = ipv4_config.src_ip[i];
    uint16_t c = ipv4_checksum(iph, 20);
    iph[10] = (c >> 8) & 0xFF; iph[11] = c & 0xFF;
    for (uint16_t i = 0; i < dlen; i++) iph[20 + i] = data[i];
}

int ipv4_frag_selftest(void) {
    uint8_t pay[32];
    for (int i = 0; i < 32; i++) pay[i] = (uint8_t)(i + 1);

    uint16_t flen = 14 + 20 + 16;
    uint8_t fa[64], fb[64];
    fragtest_frame(fa, 0xBEEF, IPV4_FLAG_MF, pay, 16);        /* ilk parca */
    fragtest_frame(fb, 0xBEEF, 16 >> 3, pay + 16, 16);        /* son parca */

    uint64_t t = clock_ms();
    int ok = 0;
    for (int pass = 0; pass < 1; pass++) {
        ip_pkt_t p;
        const uint8_t *out = 0;
        uint16_t olen = 0;
        if (!ip_parse(fa, flen, &p) || !p.fragmented) break;
        if (frag_feed(&p, t, &out, &olen) >= 0) break;        /* eksikken bitmemeli */
        if (!ip_parse(fb, flen, &p)) break;
        int slot = frag_feed(&p, t, &out, &olen);
        if (slot < 0) break;
        if (olen != 32) { frag_release(&s_frag[slot]); break; }
        ok = 1;
        for (int i = 0; i < 32; i++) if (out[i] != pay[i]) { ok = 0; break; }
        frag_release(&s_frag[slot]);
    }

    /* Cakisma: ayni parca iki kez beslenirse datagram birakilmali,
     * sonraki temiz_parcalar yine birlesebilmeli (teardrop savunmasi). */
    int overlap_ok = 0;
    for (int pass = 0; pass < 1; pass++) {
        ip_pkt_t p;
        const uint8_t *out = 0;
        uint16_t olen = 0;
        if (!ip_parse(fa, flen, &p)) break;
        if (frag_feed(&p, t + 1, &out, &olen) >= 0) break;
        if (frag_feed(&p, t + 1, &out, &olen) >= 0) break;    /* cakisma: -1 */
        if (frag_feed(&p, t + 1, &out, &olen) >= 0) break;    /* slot bosti, yeniden baslar */
        if (!ip_parse(fb, flen, &p)) break;
        int slot = frag_feed(&p, t + 1, &out, &olen);
        if (slot < 0) break;
        overlap_ok = (olen == 32);
        frag_release(&s_frag[slot]);
    }

    for (int i = 0; i < IP_FRAG_SLOTS; i++) frag_release(&s_frag[i]);

    serial_printf("[ipfrag] birlestirme=%s cakisma-korunmasi=%s\n",
                  ok ? "OK" : "HATA", overlap_ok ? "OK" : "HATA");
    return (ok && overlap_ok) ? 0 : 1;
}
