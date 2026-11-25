#ifndef IPV6_H
#define IPV6_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define IPV6_ADDR_LEN       16
#define IPV6_HDR_LEN        40

/* Next Header values (RFC 8200 / RFC 4443) */
#define IPV6_NH_HOPOPT      0
#define IPV6_NH_TCP         6
#define IPV6_NH_UDP         17
#define IPV6_NH_ICMPV6      58
#define IPV6_NH_NONE        59

/* ICMPv6 message types (RFC 4443 / RFC 4861) */
#define ICMPV6_ECHO_REQ     128
#define ICMPV6_ECHO_REPLY   129
#define ICMPV6_NDP_RS       133
#define ICMPV6_NDP_RA       134
#define ICMPV6_NDP_NS       135
#define ICMPV6_NDP_NA       136

#define NDP_CACHE_SIZE      16
#define NDP_CACHE_TIMEOUT   600000u

typedef struct {
    uint8_t ll_addr[16];
    uint8_t global_addr[16];
    uint8_t router_ll[16];
    uint8_t prefix[16];
    uint8_t prefix_len;
    uint8_t dns_addr[16];
    bool    has_ll;
    bool    has_global;
    bool    has_router;
    bool    has_dns;
} ipv6_info_t;

void     ipv6_init(void);
void     ipv6_rx(const uint8_t *frame, uint16_t frame_len);
void     ipv6_poll(void);
bool     ipv6_send(const uint8_t dst[16], uint8_t next_hdr,
                   const uint8_t *payload, uint16_t payload_len);
bool     ipv6_send_rs(void);
bool     ipv6_ndp_resolve(const uint8_t ip[16], uint8_t mac_out[6]);

uint16_t ipv6_checksum(const uint8_t src[16], const uint8_t dst[16],
                       uint8_t next_hdr, const uint8_t *payload, uint16_t len);
void     ipv6_make_link_local(const uint8_t mac[6], uint8_t out[16]);
void     ipv6_format_addr(const uint8_t addr[16], char *out, size_t cap);
bool     ipv6_get_addr(ipv6_info_t *out);
int      ipv6_selftest(void);

#endif /* IPV6_H */
