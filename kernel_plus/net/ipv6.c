/**
 * Arctian IPv6 Dual-Stack Foundation (RFC 8200 / RFC 4861 / RFC 4443)
 *
 * Provides SLAAC EUI-64 link-local address derivation, IPv6 packet parser/builder,
 * IPv6 pseudo-header checksum, ICMPv6 Echo Request -> Echo Reply, NDP Neighbor
 * Solicitation / Neighbor Advertisement / Router Solicitation / Router Advertisement
 * (with automatic SLAAC global prefix configuration), and an NDP neighbor cache.
 */
#include "ipv6.h"
#include "ipv4.h"
#include "net.h"
#include "kernel.h"
#include "serial.h"
#include "clock.h"

typedef struct {
    uint8_t  ip[16];
    uint8_t  mac[6];
    uint32_t timestamp;
    bool     valid;
} ndp_entry_t;

typedef struct {
    ipv6_info_t info;
    ndp_entry_t cache[NDP_CACHE_SIZE];
    uint32_t    rx_packets;
    uint32_t    tx_packets;
    uint32_t    icmp6_echo_replies;
    uint32_t    ndp_na_sent;
} ipv6_state_t;

static ipv6_state_t s_v6;
static bool s_in_selftest;

static const uint8_t s_all_nodes_mc[16] = {
    0xFF, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1
};
static const uint8_t s_all_routers_mc[16] = {
    0xFF, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2
};

static bool ip6_eq(const uint8_t a[16], const uint8_t b[16]) {
    for (int i = 0; i < 16; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

static bool ip6_is_unspec(const uint8_t a[16]) {
    for (int i = 0; i < 16; i++) {
        if (a[i] != 0) return false;
    }
    return true;
}

static bool ip6_is_multicast(const uint8_t a[16]) {
    return a[0] == 0xFF;
}

static bool ip6_is_link_local(const uint8_t a[16]) {
    return a[0] == 0xFE && (a[1] & 0xC0u) == 0x80u;
}

static void ip6_copy(uint8_t dst[16], const uint8_t src[16]) {
    for (int i = 0; i < 16; i++) dst[i] = src[i];
}

static void ip6_solicited_node_mc(const uint8_t addr[16], uint8_t out[16]) {
    for (int i = 0; i < 16; i++) out[i] = 0;
    out[0]  = 0xFF; out[1]  = 0x02;
    out[11] = 0x01; out[12] = 0xFF;
    out[13] = addr[13]; out[14] = addr[14]; out[15] = addr[15];
}

static void ip6_multicast_mac(const uint8_t dst[16], uint8_t mac_out[6]) {
    mac_out[0] = 0x33;
    mac_out[1] = 0x33;
    mac_out[2] = dst[12];
    mac_out[3] = dst[13];
    mac_out[4] = dst[14];
    mac_out[5] = dst[15];
}

void ipv6_make_link_local(const uint8_t mac[6], uint8_t out[16]) {
    for (int i = 0; i < 16; i++) out[i] = 0;
    out[0]  = 0xFE;
    out[1]  = 0x80;
    out[8]  = (uint8_t)(mac[0] ^ 0x02u); /* EUI-64: flip U/L bit */
    out[9]  = mac[1];
    out[10] = mac[2];
    out[11] = 0xFF;
    out[12] = 0xFE;
    out[13] = mac[3];
    out[14] = mac[4];
    out[15] = mac[5];
}

void ipv6_format_addr(const uint8_t addr[16], char *out, size_t cap) {
    if (!out || cap == 0) return;

    uint16_t words[8];
    for (int i = 0; i < 8; i++) {
        words[i] = (uint16_t)(((uint16_t)addr[i * 2] << 8) | addr[i * 2 + 1]);
    }

    int best_start = -1, best_len = 0;
    int cur_start = -1, cur_len = 0;
    for (int i = 0; i < 8; i++) {
        if (words[i] == 0) {
            if (cur_start < 0) { cur_start = i; cur_len = 1; }
            else cur_len++;
            if (cur_len > best_len) { best_start = cur_start; best_len = cur_len; }
        } else {
            cur_start = -1;
            cur_len = 0;
        }
    }
    if (best_len < 2) { best_start = -1; best_len = 0; }

    size_t pos = 0;
    const char *hex = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        if (i == best_start) {
            if (pos + 2 < cap) { out[pos++] = ':'; out[pos++] = ':'; }
            i += best_len - 1;
            continue;
        }
        if (i > 0 && i != best_start + best_len) {
            if (pos + 1 < cap) out[pos++] = ':';
        }
        uint16_t w = words[i];
        int started = 0;
        for (int shift = 12; shift >= 0; shift -= 4) {
            uint8_t nib = (uint8_t)((w >> shift) & 0x0Fu);
            if (nib != 0 || started || shift == 0) {
                started = 1;
                if (pos + 1 < cap) out[pos++] = hex[nib];
            }
        }
    }
    out[pos] = '\0';
}

uint16_t ipv6_checksum(const uint8_t src[16], const uint8_t dst[16],
                       uint8_t next_hdr, const uint8_t *payload, uint16_t len) {
    uint32_t sum = 0;
    for (int i = 0; i < 16; i += 2) {
        sum += ((uint32_t)src[i] << 8) | src[i + 1];
        sum += ((uint32_t)dst[i] << 8) | dst[i + 1];
    }
    sum += (uint32_t)len;
    sum += (uint32_t)next_hdr;

    if (payload && len > 0) {
        for (uint16_t i = 0; i + 1 < len; i += 2) {
            sum += ((uint32_t)payload[i] << 8) | payload[i + 1];
        }
        if (len & 1u) {
            sum += (uint32_t)payload[len - 1] << 8;
        }
    }
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)(~sum);
}

static void ndp_cache_update(const uint8_t ip[16], const uint8_t mac[6]) {
    if (ip6_is_unspec(ip) || ip6_is_multicast(ip)) return;
    uint32_t now = (uint32_t)clock_ms();
    int oldest = 0;
    uint32_t oldest_ts = 0xFFFFFFFFu;

    for (int i = 0; i < NDP_CACHE_SIZE; i++) {
        if (s_v6.cache[i].valid && ip6_eq(s_v6.cache[i].ip, ip)) {
            for (int j = 0; j < 6; j++) s_v6.cache[i].mac[j] = mac[j];
            s_v6.cache[i].timestamp = now;
            return;
        }
        if (!s_v6.cache[i].valid || s_v6.cache[i].timestamp < oldest_ts) {
            oldest = i;
            oldest_ts = s_v6.cache[i].timestamp;
        }
    }

    ip6_copy(s_v6.cache[oldest].ip, ip);
    for (int j = 0; j < 6; j++) s_v6.cache[oldest].mac[j] = mac[j];
    s_v6.cache[oldest].timestamp = now;
    s_v6.cache[oldest].valid = true;
}

static bool ndp_cache_lookup(const uint8_t ip[16], uint8_t mac_out[6]) {
    uint32_t now = (uint32_t)clock_ms();
    for (int i = 0; i < NDP_CACHE_SIZE; i++) {
        if (s_v6.cache[i].valid && ip6_eq(s_v6.cache[i].ip, ip)) {
            if ((int32_t)(now - s_v6.cache[i].timestamp) < (int32_t)NDP_CACHE_TIMEOUT) {
                for (int j = 0; j < 6; j++) mac_out[j] = s_v6.cache[i].mac[j];
                return true;
            }
        }
    }
    return false;
}

static bool ipv6_send_raw(const uint8_t dst_mac[6], const uint8_t src[16],
                          const uint8_t dst[16], uint8_t next_hdr, uint8_t hop_limit,
                          const uint8_t *payload, uint16_t payload_len) {
    const uint8_t *my_mac = net_get_mac();
    if (!my_mac) return false;
    if ((uint32_t)(14u + IPV6_HDR_LEN + payload_len) > NET_PKT_MAX) return false;

    uint8_t frame[NET_PKT_MAX];
    for (int i = 0; i < 6; i++) frame[i] = dst_mac[i];
    for (int i = 0; i < 6; i++) frame[6 + i] = my_mac[i];
    frame[12] = 0x86;
    frame[13] = 0xDD;

    uint8_t *ip6 = frame + 14;
    ip6[0] = 0x60; ip6[1] = 0x00; ip6[2] = 0x00; ip6[3] = 0x00;
    ip6[4] = (uint8_t)(payload_len >> 8);
    ip6[5] = (uint8_t)(payload_len & 0xFFu);
    ip6[6] = next_hdr;
    ip6[7] = hop_limit;
    ip6_copy(ip6 + 8, src);
    ip6_copy(ip6 + 24, dst);

    if (payload_len > 0 && payload) {
        memcpy(ip6 + IPV6_HDR_LEN, payload, payload_len);
    }

    if (!net_send_packet(frame, (uint16_t)(14u + IPV6_HDR_LEN + payload_len))) {
        return false;
    }
    s_v6.tx_packets++;
    return true;
}

static bool ipv6_send_icmp6(const uint8_t dst_mac[6], const uint8_t src[16],
                            const uint8_t dst[16], uint8_t hop_limit,
                            uint8_t *icmp, uint16_t icmp_len) {
    icmp[2] = 0;
    icmp[3] = 0;
    uint16_t csum = ipv6_checksum(src, dst, IPV6_NH_ICMPV6, icmp, icmp_len);
    icmp[2] = (uint8_t)(csum >> 8);
    icmp[3] = (uint8_t)(csum & 0xFFu);
    return ipv6_send_raw(dst_mac, src, dst, IPV6_NH_ICMPV6, hop_limit, icmp, icmp_len);
}

bool ipv6_ndp_resolve(const uint8_t ip[16], uint8_t mac_out[6]) {
    if (!ip || !mac_out) return false;
    if (ip6_is_multicast(ip)) {
        ip6_multicast_mac(ip, mac_out);
        return true;
    }
    if (ndp_cache_lookup(ip, mac_out)) return true;

    const uint8_t *my_mac = net_get_mac();
    if (!my_mac || !s_v6.info.has_ll) return false;

    uint8_t sol_mc[16];
    uint8_t mc_mac[6];
    ip6_solicited_node_mc(ip, sol_mc);
    ip6_multicast_mac(sol_mc, mc_mac);

    /* NDP Neighbor Solicitation: 4B hdr + 4B reserved + 16B target + 8B SLLA opt */
    uint8_t ns[32];
    memset(ns, 0, sizeof(ns));
    ns[0] = ICMPV6_NDP_NS;
    ns[1] = 0;
    ip6_copy(ns + 8, ip);
    ns[24] = 1; /* Option: Source Link-Layer Address */
    ns[25] = 1; /* Length: 1 (8 bytes) */
    for (int i = 0; i < 6; i++) ns[26 + i] = my_mac[i];

    const uint8_t *src = (!ip6_is_link_local(ip) && s_v6.info.has_global)
                             ? s_v6.info.global_addr
                             : s_v6.info.ll_addr;

    ipv6_send_icmp6(mc_mac, src, sol_mc, 255, ns, sizeof(ns));

    if (ipv4_poll_nested()) return false;

    for (int attempt = 0; attempt < 30; attempt++) {
        clock_sleep_ms(1);
        ipv4_poll();
        if (ndp_cache_lookup(ip, mac_out)) return true;
    }
    return false;
}

static bool ipv6_same_prefix64(const uint8_t a[16], const uint8_t b[16]) {
    for (int i = 0; i < 8; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

bool ipv6_send(const uint8_t dst[16], uint8_t next_hdr,
               const uint8_t *payload, uint16_t payload_len) {
    if (!dst || !s_v6.info.has_ll) return false;

    const uint8_t *src = s_v6.info.ll_addr;
    if (!ip6_is_link_local(dst) && !ip6_is_multicast(dst) && s_v6.info.has_global) {
        src = s_v6.info.global_addr;
    }

    const uint8_t *next_hop = dst;
    if (!ip6_is_multicast(dst) && !ip6_is_link_local(dst)) {
        if (!(s_v6.info.has_global && ipv6_same_prefix64(s_v6.info.global_addr, dst))) {
            if (s_v6.info.has_router) next_hop = s_v6.info.router_ll;
        }
    }

    uint8_t dst_mac[6];
    if (!ipv6_ndp_resolve(next_hop, dst_mac)) return false;

    uint8_t hop = 64;
    if (next_hdr == IPV6_NH_ICMPV6 && payload && payload_len >= 1 &&
        payload[0] >= ICMPV6_NDP_RS && payload[0] <= ICMPV6_NDP_NA) {
        hop = 255;
    }
    return ipv6_send_raw(dst_mac, src, dst, next_hdr, hop, payload, payload_len);
}

bool ipv6_send_rs(void) {
    const uint8_t *my_mac = net_get_mac();
    if (!my_mac || !s_v6.info.has_ll) return false;

    /* NDP Router Solicitation: 4B hdr + 4B reserved + 8B SLLA opt */
    uint8_t rs[16];
    memset(rs, 0, sizeof(rs));
    rs[0] = ICMPV6_NDP_RS;
    rs[1] = 0;
    rs[8] = 1; /* Source Link-Layer Address */
    rs[9] = 1;
    for (int i = 0; i < 6; i++) rs[10 + i] = my_mac[i];

    uint8_t mc_mac[6];
    ip6_multicast_mac(s_all_routers_mc, mc_mac);
    if (!ipv6_send_icmp6(mc_mac, s_v6.info.ll_addr, s_all_routers_mc, 255, rs, sizeof(rs))) {
        return false;
    }

    /* Briefly poll for immediate RA (e.g. QEMU slirp -netdev user,ipv6=on) */
    for (int i = 0; i < 30 && !s_v6.info.has_global; i++) {
        clock_sleep_ms(5);
        ipv4_poll();
    }
    return true;
}

static bool ipv6_is_our_unicast(const uint8_t addr[16]) {
    if (s_v6.info.has_ll && ip6_eq(addr, s_v6.info.ll_addr)) return true;
    if (s_v6.info.has_global && ip6_eq(addr, s_v6.info.global_addr)) return true;
    return false;
}

static bool ipv6_accept_dst(const uint8_t dst[16]) {
    if (ipv6_is_our_unicast(dst)) return true;
    if (ip6_eq(dst, s_all_nodes_mc)) return true;
    uint8_t sol[16];
    if (s_v6.info.has_ll) {
        ip6_solicited_node_mc(s_v6.info.ll_addr, sol);
        if (ip6_eq(dst, sol)) return true;
    }
    if (s_v6.info.has_global) {
        ip6_solicited_node_mc(s_v6.info.global_addr, sol);
        if (ip6_eq(dst, sol)) return true;
    }
    return false;
}

static void ipv6_handle_echo_req(const uint8_t src_mac[6], const uint8_t src[16],
                                 const uint8_t dst[16], const uint8_t *icmp, uint16_t len) {
    if (len < 8 || len > NET_PKT_MAX - 14u - IPV6_HDR_LEN) return;
    if (ip6_is_unspec(src)) return;

    uint8_t reply[NET_PKT_MAX - 14u - IPV6_HDR_LEN];
    memcpy(reply, icmp, len);
    reply[0] = ICMPV6_ECHO_REPLY;
    reply[1] = 0;

    const uint8_t *reply_src = ipv6_is_our_unicast(dst) ? dst : s_v6.info.ll_addr;
    ndp_cache_update(src, src_mac);
    if (ipv6_send_icmp6(src_mac, reply_src, src, 64, reply, len)) {
        s_v6.icmp6_echo_replies++;
    }
}

static void ipv6_handle_ns(const uint8_t src_mac[6], const uint8_t src[16],
                           const uint8_t *icmp, uint16_t len) {
    if (len < 24) return;
    const uint8_t *target = icmp + 8;
    if (!ipv6_is_our_unicast(target)) return;

    const uint8_t *my_mac = net_get_mac();
    if (!my_mac) return;

    uint8_t peer_mac[6];
    for (int i = 0; i < 6; i++) peer_mac[i] = src_mac[i];

    /* Parse options starting at offset 24 for Source Link-Layer Address (type 1) */
    uint16_t off = 24;
    while (off + 2 <= len) {
        uint8_t otype = icmp[off];
        uint8_t olen8 = icmp[off + 1];
        if (olen8 == 0) break;
        uint16_t olen = (uint16_t)olen8 * 8u;
        if (off + olen > len) break;
        if (otype == 1 && olen >= 8 && !ip6_is_unspec(src)) {
            for (int i = 0; i < 6; i++) peer_mac[i] = icmp[off + 2 + i];
            ndp_cache_update(src, peer_mac);
        }
        off = (uint16_t)(off + olen);
    }

    uint8_t na[32];
    memset(na, 0, sizeof(na));
    na[0] = ICMPV6_NDP_NA;
    na[1] = 0;

    uint8_t dst_ip[16];
    uint8_t dst_mac[6];
    if (ip6_is_unspec(src)) {
        na[4] = 0x20; /* Override=1, Solicited=0 */
        ip6_copy(dst_ip, s_all_nodes_mc);
        ip6_multicast_mac(dst_ip, dst_mac);
    } else {
        na[4] = 0x60; /* Solicited=1, Override=1 */
        ip6_copy(dst_ip, src);
        for (int i = 0; i < 6; i++) dst_mac[i] = peer_mac[i];
    }

    ip6_copy(na + 8, target);
    na[24] = 2; /* Target Link-Layer Address */
    na[25] = 1;
    for (int i = 0; i < 6; i++) na[26 + i] = my_mac[i];

    if (ipv6_send_icmp6(dst_mac, target, dst_ip, 255, na, sizeof(na))) {
        s_v6.ndp_na_sent++;
    }
}

static void ipv6_handle_na(const uint8_t *icmp, uint16_t len) {
    if (len < 24) return;
    const uint8_t *target = icmp + 8;

    uint16_t off = 24;
    while (off + 2 <= len) {
        uint8_t otype = icmp[off];
        uint8_t olen8 = icmp[off + 1];
        if (olen8 == 0) break;
        uint16_t olen = (uint16_t)olen8 * 8u;
        if (off + olen > len) break;
        if (otype == 2 && olen >= 8) {
            ndp_cache_update(target, icmp + off + 2);
            return;
        }
        off = (uint16_t)(off + olen);
    }
}

static void ipv6_handle_ra(const uint8_t src_mac[6], const uint8_t src[16],
                           const uint8_t *icmp, uint16_t len) {
    if (len < 16) return;

    ip6_copy(s_v6.info.router_ll, src);
    s_v6.info.has_router = true;
    ndp_cache_update(src, src_mac);

    uint16_t off = 16;
    while (off + 2 <= len) {
        uint8_t otype = icmp[off];
        uint8_t olen8 = icmp[off + 1];
        if (olen8 == 0) break;
        uint16_t olen = (uint16_t)olen8 * 8u;
        if (off + olen > len) break;

        if (otype == 1 && olen >= 8) {
            ndp_cache_update(src, icmp + off + 2);
        } else if (otype == 3 && olen >= 32) {
            /* Prefix Information Option (RFC 4861 §4.6.2) */
            uint8_t plen = icmp[off + 2];
            uint8_t flags = icmp[off + 3];
            const uint8_t *pfx = icmp + off + 16;
            if ((flags & 0x40u) && plen == 64 && s_v6.info.has_ll) {
                bool is_new = !s_v6.info.has_global || !ip6_eq(s_v6.info.prefix, pfx);
                ip6_copy(s_v6.info.prefix, pfx);
                s_v6.info.prefix_len = plen;
                for (int i = 0; i < 8; i++) s_v6.info.global_addr[i] = pfx[i];
                for (int i = 8; i < 16; i++) s_v6.info.global_addr[i] = s_v6.info.ll_addr[i];
                s_v6.info.has_global = true;

                if (is_new && !s_in_selftest) {
                    char gstr[48], rstr[48];
                    ipv6_format_addr(s_v6.info.global_addr, gstr, sizeof(gstr));
                    ipv6_format_addr(s_v6.info.router_ll, rstr, sizeof(rstr));
                    serial_printf("[ipv6] slaac=%s/64 gw=%s\n", gstr, rstr);
                }
            }
        } else if (otype == 25 && olen >= 24) {
            /* RDNSS Option (RFC 8106) */
            ip6_copy(s_v6.info.dns_addr, icmp + off + 8);
            s_v6.info.has_dns = true;
        }
        off = (uint16_t)(off + olen);
    }
}

void ipv6_rx(const uint8_t *frame, uint16_t frame_len) {
    if (!frame || frame_len < 14u + IPV6_HDR_LEN) return;
    if (frame[12] != 0x86 || frame[13] != 0xDD) return;

    const uint8_t *src_mac = frame + 6;
    const uint8_t *ip6 = frame + 14;
    if ((ip6[0] >> 4) != 6) return;

    uint16_t payload_len = (uint16_t)(((uint16_t)ip6[4] << 8) | ip6[5]);
    if (14u + IPV6_HDR_LEN + (uint32_t)payload_len > frame_len) return;

    uint8_t next_hdr = ip6[6];
    const uint8_t *src = ip6 + 8;
    const uint8_t *dst = ip6 + 24;
    const uint8_t *payload = ip6 + IPV6_HDR_LEN;

    if (!ipv6_accept_dst(dst)) return;
    s_v6.rx_packets++;

    if (next_hdr == IPV6_NH_ICMPV6 && payload_len >= 4) {
        if (ipv6_checksum(src, dst, IPV6_NH_ICMPV6, payload, payload_len) != 0) {
            return;
        }
        uint8_t type = payload[0];
        switch (type) {
            case ICMPV6_ECHO_REQ:
                ipv6_handle_echo_req(src_mac, src, dst, payload, payload_len);
                break;
            case ICMPV6_NDP_NS:
                ipv6_handle_ns(src_mac, src, payload, payload_len);
                break;
            case ICMPV6_NDP_NA:
                ipv6_handle_na(payload, payload_len);
                break;
            case ICMPV6_NDP_RA:
                ipv6_handle_ra(src_mac, src, payload, payload_len);
                break;
            default:
                break;
        }
    }
}

void ipv6_poll(void) {
    ipv4_poll();
}

void ipv6_init(void) {
    memset(&s_v6, 0, sizeof(s_v6));
    const uint8_t *mac = net_get_mac();
    if (!mac) {
        serial_puts("[ipv6] no NIC, dual-stack inactive\n");
        return;
    }

    ipv6_make_link_local(mac, s_v6.info.ll_addr);
    s_v6.info.has_ll = true;

    char ll_str[48];
    ipv6_format_addr(s_v6.info.ll_addr, ll_str, sizeof(ll_str));
    serial_printf("[ipv6] ll=%s\n", ll_str);
}

bool ipv6_get_addr(ipv6_info_t *out) {
    if (!out) return false;
    *out = s_v6.info;
    return s_v6.info.has_ll;
}

int ipv6_selftest(void) {
    /* 1) EUI-64 link-local derivation check */
    uint8_t test_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    uint8_t ll[16];
    ipv6_make_link_local(test_mac, ll);
    const uint8_t exp_ll[16] = {
        0xFE, 0x80, 0, 0, 0, 0, 0, 0,
        0x50, 0x54, 0x00, 0xFF, 0xFE, 0x12, 0x34, 0x56
    };
    if (!ip6_eq(ll, exp_ll)) {
        serial_puts("[ipv6] selftest FAIL: EUI-64\n");
        return -1;
    }

    char formatted[48];
    ipv6_format_addr(ll, formatted, sizeof(formatted));
    if (strcmp(formatted, "fe80::5054:ff:fe12:3456") != 0) {
        serial_printf("[ipv6] selftest FAIL: fmt=%s\n", formatted);
        return -2;
    }

    /* 2) Pseudo-header checksum round-trip check */
    uint8_t icmp[12] = { ICMPV6_ECHO_REQ, 0, 0, 0, 0x12, 0x34, 0x00, 0x01, 'A', 'R', 'C', 'T' };
    uint16_t cs = ipv6_checksum(exp_ll, s_all_nodes_mc, IPV6_NH_ICMPV6, icmp, sizeof(icmp));
    icmp[2] = (uint8_t)(cs >> 8);
    icmp[3] = (uint8_t)(cs & 0xFFu);
    if (ipv6_checksum(exp_ll, s_all_nodes_mc, IPV6_NH_ICMPV6, icmp, sizeof(icmp)) != 0) {
        serial_puts("[ipv6] selftest FAIL: checksum\n");
        return -3;
    }

    /* 3) Synthetic NDP NA + RA parsing (preserving live state) */
    ipv6_state_t saved = s_v6;
    s_in_selftest = true;

    if (!s_v6.info.has_ll) {
        ip6_copy(s_v6.info.ll_addr, exp_ll);
        s_v6.info.has_ll = true;
    }

    /* Feed synthetic Router Advertisement for 2001:db8:1::/64 */
    uint8_t ra_frame[14 + 40 + 56];
    memset(ra_frame, 0, sizeof(ra_frame));
    for (int i = 0; i < 6; i++) {
        ra_frame[i] = 0x33;
        ra_frame[6 + i] = (uint8_t)(0xA0 + i);
    }
    ra_frame[12] = 0x86; ra_frame[13] = 0xDD;
    uint8_t *ip6 = ra_frame + 14;
    ip6[0] = 0x60;
    ip6[4] = 0; ip6[5] = 56;
    ip6[6] = IPV6_NH_ICMPV6;
    ip6[7] = 255;
    uint8_t gw_ll[16] = { 0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2 };
    ip6_copy(ip6 + 8, gw_ll);
    ip6_copy(ip6 + 24, s_all_nodes_mc);

    uint8_t *ra = ip6 + 40;
    ra[0] = ICMPV6_NDP_RA;
    ra[1] = 0;
    ra[4] = 64; /* cur hop limit */
    ra[6] = 0x07; ra[7] = 0x08; /* router lifetime 1800 */
    /* Option: SLLA (type=1, len=1) */
    ra[16] = 1; ra[17] = 1;
    for (int i = 0; i < 6; i++) ra[18 + i] = (uint8_t)(0xB0 + i);
    /* Option: Prefix Info (type=3, len=4 -> 32 bytes at offset 24) */
    ra[24] = 3; ra[25] = 4; ra[26] = 64; ra[27] = 0xC0; /* L+A */
    ra[40] = 0x20; ra[41] = 0x01; ra[42] = 0x0D; ra[43] = 0xB8;
    ra[44] = 0x00; ra[45] = 0x00; ra[46] = 0x00; ra[47] = 0x01;

    uint16_t racs = ipv6_checksum(gw_ll, s_all_nodes_mc, IPV6_NH_ICMPV6, ra, 56);
    ra[2] = (uint8_t)(racs >> 8);
    ra[3] = (uint8_t)(racs & 0xFFu);

    ipv6_rx(ra_frame, sizeof(ra_frame));

    uint8_t cached_mac[6] = {0};
    bool slaac_ok = s_v6.info.has_global &&
                    s_v6.info.global_addr[0] == 0x20 &&
                    s_v6.info.global_addr[1] == 0x01 &&
                    s_v6.info.global_addr[2] == 0x0D &&
                    s_v6.info.global_addr[3] == 0xB8 &&
                    s_v6.info.global_addr[15] == s_v6.info.ll_addr[15];
    bool ndp_ok = ndp_cache_lookup(gw_ll, cached_mac) && cached_mac[0] == 0xB0;

    s_in_selftest = false;
    s_v6 = saved;

    if (!slaac_ok || !ndp_ok) {
        serial_printf("[ipv6] selftest FAIL: slaac=%d ndp=%d\n", slaac_ok ? 1 : 0, ndp_ok ? 1 : 0);
        return -4;
    }

    serial_printf("[ipv6] selftest=OK (eui64=%s slaac=OK ndp=OK)\n", formatted);
    return 0;
}
