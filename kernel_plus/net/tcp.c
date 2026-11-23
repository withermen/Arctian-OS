#include "tcp.h"
#include "sched.h"
#include "alloc.h"
#include "ipv4.h"
#include "net.h"
#include "firewall.h"
#include "kernel.h"
#include "clock.h"
#include "serial.h"
#include <string.h>

static tcp_connection_t tcp_connections[TCP_MAX_CONNECTIONS];
static uint16_t tcp_ephemeral_port = 1024;
static uint32_t tcp_ticks = 0;

/* Zamanlayici saatleri clock_ms'in 32-bit kesiti olarak tutulur;
 * karsilastirmalar wrap-safe yapilir. Eskiden retransmit_timer bir
 * yerde guncelleniyor ama hicbir yerden okunmuyordu: kayip pakette
 * baglanti sonsuza dek donuyordu. */
static inline uint32_t tcp_now(void) { return (uint32_t)clock_ms(); }
static inline bool tcp_ms_due(uint32_t start, uint32_t timeout_ms) {
    return (int32_t)(tcp_now() - start) >= (int32_t)timeout_ms;
}
static inline uint32_t tcp_rto(uint8_t count) {
    uint32_t r = TCP_RETRANSMIT_TIMEOUT;
    if (count > 3) count = 3;          /* en fazla 8 sn bekleme */
    return r << count;
}

/* Ag bayt sirasi (big-endian) yardimcilari. x86 little-endian oldugundan
 * TCP alanlari tele yazilirken/okunurken cevrilmelidir. */
static inline uint16_t htons16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint32_t htonl32(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

void tcp_init(void) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_connections[i].state = TCP_STATE_CLOSED;
        tcp_connections[i].send_buf_len = 0;
        tcp_connections[i].recv_buf_head = 0;
        tcp_connections[i].recv_buf_tail = 0;
        tcp_connections[i].send_seq = 0;
        tcp_connections[i].recv_ack = 0;
        tcp_connections[i].send_unack = 0;
        tcp_connections[i].retransmit_count = 0;
        tcp_connections[i].retransmit_timer = 0;
        tcp_connections[i].last_activity = 0;
        tcp_connections[i].child_id = -1;
        tcp_connections[i].peer_window = 0;
        tcp_connections[i].idle_ms = 0;
    }
}

static int tcp_alloc_conn(void) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (tcp_connections[i].state == TCP_STATE_CLOSED) {
            tcp_connection_t *c = &tcp_connections[i];
            c->send_buf_len = 0;
            c->recv_buf_head = 0;
            c->recv_buf_tail = 0;
            c->retransmit_count = 0;
            c->retransmit_timer = tcp_now();
            c->send_unack = 0;
            c->child_id = -1;
            /* Karsi tarafin penceresi SYN/ACK'e kadar bilinmiyor; kotu
             * huylu olmayan varsayilan olarak MSS'lik bir alanla basla,
             * ilk bildirim gercek degeriyle ezilir. */
            c->peer_window = TCP_MSS;
            c->idle_ms = tcp_now();
            /* F3: alim tamponunu yuvada yeniden kullan (fragmentasyonu onler) */
            if (!c->recv_buffer) {
                c->recv_buffer = (uint8_t *)kmalloc(TCP_RXBUF_SIZE);
                if (!c->recv_buffer) return -1;
            }
            c->wscale_shift = TCP_WSCALE_SHIFT;
            c->peer_wscale = 0;
            c->last_adv_win = TCP_RXBUF_SIZE;
            c->ack_pending = 0;
            c->ack_pkts = 0;
            return i;
        }
    }
    return -1;
}

/* App tarafindan gelen conn_id dogrulamasi: tcp_* fonksiyonlari ASI API
 * uzerinden dogrudan uygulama girdisi alir; sinir kontrolu olmadan diziye
 * indekslemek cekirdek belleginde rastgele erisim demekti. */
static inline bool tcp_id_ok(int conn_id) {
    return conn_id >= 0 && conn_id < TCP_MAX_CONNECTIONS;
}

static uint16_t tcp_calc_checksum(const uint8_t *src_ip, const uint8_t *dst_ip,
                                  const uint8_t *tcp_segment, uint16_t tcp_len) {
    uint32_t sum = 0;
    uint8_t pseudo[12];
    for (int i = 0; i < 4; i++) pseudo[i] = src_ip[i];
    for (int i = 0; i < 4; i++) pseudo[4 + i] = dst_ip[i];
    pseudo[8] = 0;
    pseudo[9] = IPV4_PROTO_TCP;
    pseudo[10] = (tcp_len >> 8) & 0xFF;
    pseudo[11] = tcp_len & 0xFF;

    for (int i = 0; i < 12; i += 2) {
        sum += ((uint16_t)pseudo[i] << 8) | pseudo[i + 1];
    }
    for (uint16_t i = 0; i < tcp_len - 1; i += 2) {
        sum += ((uint16_t)tcp_segment[i] << 8) | tcp_segment[i + 1];
    }
    if (tcp_len & 1) sum += (uint16_t)tcp_segment[tcp_len - 1] << 8;

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* F3: dairesel RX tamponu — her tcp_receive'de memmove yok (O(1)). */
static inline void tcp_rx_write(tcp_connection_t *c, const uint8_t *src, uint32_t len) {
    uint32_t pos = c->recv_buf_tail & TCP_RXBUF_MASK;
    uint32_t first = TCP_RXBUF_SIZE - pos;
    if (first > len) first = len;
    memcpy(&c->recv_buffer[pos], src, first);
    if (len > first) memcpy(&c->recv_buffer[0], src + first, len - first);
    c->recv_buf_tail += len;
}

static inline void tcp_rx_read(tcp_connection_t *c, uint8_t *dst, uint32_t len) {
    uint32_t pos = c->recv_buf_head & TCP_RXBUF_MASK;
    uint32_t first = TCP_RXBUF_SIZE - pos;
    if (first > len) first = len;
    memcpy(dst, &c->recv_buffer[pos], first);
    if (len > first) memcpy(dst + first, &c->recv_buffer[0], len - first);
    c->recv_buf_head += len;
}

/* Segment gonderir; seq cagiran tarafindan verilir. Eskiden fonksiyon
 * gonderim sonrasi send_seq'i kendi ilerletiyordu; yeniden gonderimde
 * orijinal seq gerekiyordu ve bu yuzden yeniden gonderim yazilamadi. */
static bool tcp_send_segment(int conn_id, uint32_t seq, uint8_t flags,
                             const uint8_t *data, uint16_t data_len) {
    tcp_connection_t *conn = &tcp_connections[conn_id];
    if (conn->state == TCP_STATE_CLOSED) return false;

    uint16_t hdr_len = 20;
    if (flags & TCP_FLAG_SYN) {
        /* F3: MSS=1460 secenegi (yoksa karsi taraf 536 bayt varsayar!) +
         * RFC 7323 window scale (destekleniyorsa). */
        hdr_len = conn->wscale_shift ? 28 : 24;
    }
    uint16_t tcp_len = hdr_len + data_len;
    uint8_t segment[1500];

    tcp_header_t *hdr = (tcp_header_t *)segment;
    hdr->src_port  = htons16(conn->local_port);
    hdr->dst_port  = htons16(conn->remote_port);
    hdr->seq_num   = htonl32(seq);
    hdr->ack_num   = htonl32(conn->recv_ack);
    hdr->data_offset = (uint8_t)(hdr_len << 2);
    hdr->flags     = flags;
    uint32_t used = conn->recv_buf_tail - conn->recv_buf_head;
    uint32_t avail = (used < TCP_RXBUF_SIZE) ? (TCP_RXBUF_SIZE - used) : 0;
    /* RFC 7323: SYN/SYN-ACK paketindeki pencere alani ASLA olceklenmez. */
    uint32_t win = (flags & TCP_FLAG_SYN) ? avail : (avail >> conn->wscale_shift);
    if (win > 65535u) win = 65535u;
    hdr->window    = htons16((uint16_t)win);
    if (flags & TCP_FLAG_SYN) {
        /* MSS option: kind=2, len=4, value=TCP_MSS (1460) */
        segment[20] = 2; segment[21] = 4;
        segment[22] = (uint8_t)(TCP_MSS >> 8);
        segment[23] = (uint8_t)(TCP_MSS & 0xFF);
        if (conn->wscale_shift) {
            /* Window scale option: kind=3, len=3, shift + NOP */
            segment[24] = 3; segment[25] = 3;
            segment[26] = conn->wscale_shift; segment[27] = 1;
        }
    }
    hdr->urgent_ptr = 0;
    hdr->checksum  = 0;

    if (data_len > 0 && data) memcpy(segment + hdr_len, data, data_len);

    hdr->checksum = htons16(tcp_calc_checksum(conn->local_ip, conn->remote_ip,
                                              segment, tcp_len));

    if (flags & TCP_FLAG_ACK) {
        conn->ack_pending = 0;
        conn->ack_pkts = 0;
        conn->last_adv_win = (flags & TCP_FLAG_SYN) ? win : (win << conn->wscale_shift);
    }

    return ipv4_send(conn->remote_ip, IPV4_PROTO_TCP, segment, tcp_len);
}

int tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port) {
    serial_printf("[tcp] connect -> %d.%d.%d.%d:%d\n",
                  dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3], (int)dst_port);
    if (!firewall_outbound(IPV4_PROTO_TCP, dst_ip, dst_port)) {
        serial_printf("[tcp] guvenlik duvari reddetti\n");
        return -1;
    }

    int id = tcp_alloc_conn();
    if (id < 0) return -1;

    tcp_connection_t *conn = &tcp_connections[id];
    for (int i = 0; i < 4; i++) {
        conn->local_ip[i] = ipv4_config.src_ip[i];
        conn->remote_ip[i] = dst_ip[i];
    }
    conn->local_port  = tcp_ephemeral_port++;
    conn->remote_port = dst_port;
    conn->send_seq = 0x12345678;
    conn->recv_ack = 0;
    conn->send_unack = conn->send_seq;
    conn->state = TCP_STATE_SYN_SENT;
    conn->retransmit_count = 0;
    conn->retransmit_timer = tcp_now();
    conn->last_activity = tcp_ticks;
    conn->idle_ms = tcp_now();
    conn->is_client = true;

    if (!tcp_send_segment(id, conn->send_seq, TCP_FLAG_SYN, 0, 0)) {
        serial_printf("[tcp] SYN gonderilemedi\n");
        conn->state = TCP_STATE_CLOSED;
        return -1;
    }
    conn->send_seq++;

    uint64_t deadline = clock_ms() + 4000;
    for (;;) {
        tcp_poll();
        if (conn->state == TCP_STATE_ESTABLISHED) return id;
        if (conn->state == TCP_STATE_CLOSED) return -1;
        if ((int64_t)(clock_ms() - deadline) >= 0) break;
        sched_sleep_ms(1);   /* F2: pause-spin yerine uyku */
    }
    serial_printf("[tcp] connect %d.%d.%d.%d:%d basarisiz (son durum=%d)\n",
                  dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3], dst_port,
                  conn->state);
    conn->state = TCP_STATE_CLOSED;
    return -1;
}

int tcp_listen(uint16_t port) {
    if (port == 0) return -1;
    int id = tcp_alloc_conn();
    if (id < 0) return -1;

    tcp_connection_t *conn = &tcp_connections[id];
    for (int i = 0; i < 4; i++) {
        conn->local_ip[i] = ipv4_config.src_ip[i];
        conn->remote_ip[i] = 0;
    }
    conn->local_port = port;
    conn->remote_port = 0;
    conn->send_seq = 0;
    conn->recv_ack = 0;
    conn->send_unack = 0;
    conn->state = TCP_STATE_LISTEN;
    conn->is_client = false;
    conn->child_id = -1;
    conn->send_buf_len = 0;
    conn->recv_buf_head = 0;
    conn->recv_buf_tail = 0;
    conn->retransmit_count = 0;
    conn->retransmit_timer = 0;
    conn->last_activity = tcp_ticks;
    return id;
}

int tcp_accept(int listen_id) {
    if (listen_id < 0 || listen_id >= TCP_MAX_CONNECTIONS) return -1;
    tcp_connection_t *l = &tcp_connections[listen_id];
    if (l->state != TCP_STATE_LISTEN || l->child_id < 0) return -1;

    int cid = l->child_id;
    tcp_connection_t *c = &tcp_connections[cid];
    if (c->state == TCP_STATE_ESTABLISHED) {
        l->child_id = -1;
        return cid;
    }
    if (c->state == TCP_STATE_CLOSED || c->state == TCP_STATE_TIME_WAIT) {
        l->child_id = -1;
    }
    return -1;
}

bool tcp_send(int conn_id, const uint8_t *data, uint16_t length) {
    if (!tcp_id_ok(conn_id)) return false;
    tcp_connection_t *conn = &tcp_connections[conn_id];
    if (conn->state != TCP_STATE_ESTABLISHED) return false;

    /* Madde 32: karsi tarafin penceresi hic okunmuyordu, sinirsiz
     * gonderim yapiliyordu. Gonderilen ama henüz onaylanmayan veri
     * send_buffer'da durur; pencere kapanirsa ACK beklenir. Ayni tampon
     * kayip pakette yeniden gonderime malzeme olur (madde 31). */
    uint32_t offset = 0;
    uint64_t stall = clock_ms() + 10000;
    while (offset < length) {
        if (conn->state != TCP_STATE_ESTABLISHED) return false;

        uint32_t inflight = conn->send_seq - conn->send_unack;
        uint32_t allow = conn->peer_window > inflight
                       ? conn->peer_window - inflight : 0;
        uint32_t space = TCP_WINDOW_SIZE - conn->send_buf_len;
        uint32_t room = allow < space ? allow : space;
        if (room == 0) {
            if ((int32_t)(tcp_now() - (uint32_t)stall) >= 0) {
                serial_printf("[tcp] gonderim takildi: pencere/kabul yok\n");
                return false;
            }
            tcp_poll();
            if (conn->state == TCP_STATE_ESTABLISHED &&
                conn->peer_window <= (conn->send_seq - conn->send_unack))
                sched_sleep_ms(1);
            continue;
        }
        uint32_t rest = length - offset;
        uint32_t chunk = rest < room ? rest : room;
        if (chunk > TCP_MSS) chunk = TCP_MSS;

        memcpy(conn->send_buffer + conn->send_buf_len, data + offset, chunk);
        conn->send_buf_len += (uint16_t)chunk;

        bool first_out = (conn->send_seq == conn->send_unack);
        if (!tcp_send_segment(conn_id, conn->send_seq,
                              TCP_FLAG_PSH | TCP_FLAG_ACK,
                              data + offset, (uint16_t)chunk)) {
            conn->send_buf_len -= (uint16_t)chunk;
            return false;
        }
        conn->send_seq += chunk;
        if (first_out) conn->retransmit_timer = tcp_now();
        offset += chunk;
        stall = clock_ms() + 10000;
    }
    conn->last_activity = tcp_ticks;
    return true;
}

/* Onaylanan baytlari yeniden gonderim tamponundan dusur. */
static void tcp_ack_send(tcp_connection_t *conn, uint32_t ack) {
    int32_t acked = (int32_t)(ack - conn->send_unack);
    if (acked <= 0) return;
    if (acked <= (int32_t)conn->send_buf_len) {
        memmove(conn->send_buffer, conn->send_buffer + acked,
                conn->send_buf_len - acked);
        conn->send_buf_len -= (uint16_t)acked;
    } else {
        conn->send_buf_len = 0;
    }
    conn->send_unack = ack;
    conn->retransmit_count = 0;
    if (conn->send_buf_len || conn->state == TCP_STATE_FIN_WAIT1)
        conn->retransmit_timer = tcp_now();
    if ((int32_t)(ack - conn->send_seq) > 0) conn->send_seq = ack;
}

int tcp_receive(int conn_id, uint8_t *buf, uint16_t buf_size) {
    if (!tcp_id_ok(conn_id)) return -1;
    tcp_connection_t *conn = &tcp_connections[conn_id];
    if (conn->state == TCP_STATE_CLOSED ||
        conn->state == TCP_STATE_TIME_WAIT) return -1;

    /* F3: RX tamponu cok dolu degilse agi yokla (halka tasmasin). */
    if ((conn->recv_buf_tail - conn->recv_buf_head) < (TCP_RXBUF_SIZE / 2u))
        tcp_poll();

    uint32_t available = conn->recv_buf_tail - conn->recv_buf_head;
    if (available == 0) return 0;
    if (available > buf_size) available = buf_size;

    tcp_rx_read(conn, buf, available);

    /* F3: her okumada gereksiz ACK gonderme; pencere anlamli olcude
     * acildiysa (veya darsa) karsi tarafi guncelle. */
    if (conn->state == TCP_STATE_ESTABLISHED || conn->state == TCP_STATE_CLOSE_WAIT) {
        uint32_t used = conn->recv_buf_tail - conn->recv_buf_head;
        uint32_t avail = (used < TCP_RXBUF_SIZE) ? (TCP_RXBUF_SIZE - used) : 0;
        if (avail >= conn->last_adv_win + 32768u ||
            (conn->last_adv_win < 32768u && avail >= 32768u)) {
            tcp_send_segment(conn_id, conn->send_seq, TCP_FLAG_ACK, 0, 0);
        }
    }

    return (int)available;
}

bool tcp_close(int conn_id) {
    if (!tcp_id_ok(conn_id)) return false;
    tcp_connection_t *conn = &tcp_connections[conn_id];

    if (conn->state == TCP_STATE_LISTEN) {
        if (conn->child_id >= 0) {
            tcp_connections[conn->child_id].state = TCP_STATE_CLOSED;
            conn->child_id = -1;
        }
        conn->state = TCP_STATE_CLOSED;
        return true;
    }

    if (conn->state != TCP_STATE_ESTABLISHED &&
        conn->state != TCP_STATE_CLOSE_WAIT) return false;

    /* CLOSE_WAIT'tan kapanis FIN'in onayini bekler (LAST_ACK);
     * ESTABLISHED'dan yarim kapatmada FIN_WAIT1'e gecilir. */
    conn->state = (conn->state == TCP_STATE_CLOSE_WAIT)
                ? TCP_STATE_LAST_ACK : TCP_STATE_FIN_WAIT1;
    tcp_send_segment(conn_id, conn->send_seq, TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
    conn->send_seq++;
    conn->retransmit_timer = tcp_now();
    conn->retransmit_count = 0;

    uint64_t deadline = clock_ms() + 1000;
    for (;;) {
        tcp_poll();
        if (conn->state == TCP_STATE_CLOSED ||
            conn->state == TCP_STATE_TIME_WAIT) return true;
        if ((int64_t)(clock_ms() - deadline) >= 0) break;
        sched_sleep_ms(1);   /* F2: pause-spin yerine uyku */
    }
    conn->state = TCP_STATE_CLOSED;
    return true;
}

/* Beklenmedik durumda slotu serit birak: ksock_close, tcp_close'un
 * islemeyecegi yarim hallerde slotu yoksa sonsuza dek sizdiriyordu. */
void tcp_abort(int conn_id) {
    if (!tcp_id_ok(conn_id)) return;
    tcp_connection_t *c = &tcp_connections[conn_id];
    if (c->state == TCP_STATE_ESTABLISHED || c->state == TCP_STATE_CLOSE_WAIT)
        tcp_send_segment(conn_id, c->send_seq, TCP_FLAG_RST, 0, 0);
    if (c->state == TCP_STATE_LISTEN && c->child_id >= 0)
        tcp_connections[c->child_id].state = TCP_STATE_CLOSED;
    c->state = TCP_STATE_CLOSED;
    c->send_buf_len = 0;
    c->recv_buf_head = 0;
    c->recv_buf_tail = 0;
    c->child_id = -1;
}

int tcp_state(int conn_id) {
    if (!tcp_id_ok(conn_id)) return -1;
    return tcp_connections[conn_id].state;
}

uint32_t tcp_rx_avail(int conn_id) {
    if (!tcp_id_ok(conn_id)) return 0;
    tcp_connection_t *c = &tcp_connections[conn_id];
    if (c->state == TCP_STATE_CLOSED) return 0;
    return c->recv_buf_tail - c->recv_buf_head;
}

/* F3: SYN/SYN-ACK seceneklerinden window scale olcegini oku (-1 = yok). */
static int tcp_parse_wscale(const uint8_t *packet, uint16_t hdr_len) {
    uint16_t o = 20;
    while (o < hdr_len) {
        uint8_t kind = packet[o];
        if (kind == 0) break;
        if (kind == 1) { o++; continue; }
        if (o + 2 > hdr_len) break;
        uint8_t len = packet[o + 1];
        if (len < 2 || o + len > hdr_len) break;
        if (kind == 3 && len == 3) {
            uint8_t sh = packet[o + 2];
            return (int)(sh > 14 ? 14 : sh);
        }
        o = (uint16_t)(o + len);
    }
    return -1;
}

void tcp_flush_acks(void) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_connection_t *c = &tcp_connections[i];
        if ((c->state == TCP_STATE_ESTABLISHED || c->state == TCP_STATE_CLOSE_WAIT) &&
            c->ack_pending) {
            tcp_send_segment(i, c->send_seq, TCP_FLAG_ACK, 0, 0);
        }
    }
}

bool tcp_receive_packet(const uint8_t *src_ip, const uint8_t *packet, uint16_t length) {
    if (length < 20) return false;

    tcp_header_t *hdr = (tcp_header_t *)packet;
    uint16_t src_port = htons16(hdr->src_port);
    uint16_t dst_port = htons16(hdr->dst_port);

    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_connection_t *conn = &tcp_connections[i];
        if (conn->state == TCP_STATE_CLOSED) continue;

        if (conn->remote_port == src_port && conn->local_port == dst_port &&
            conn->remote_ip[0] == src_ip[0] && conn->remote_ip[1] == src_ip[1] &&
            conn->remote_ip[2] == src_ip[2] && conn->remote_ip[3] == src_ip[3]) {

            conn->last_activity = tcp_ticks;
            conn->idle_ms = tcp_now();
            uint32_t seq = htonl32(hdr->seq_num);
            uint32_t ack = htonl32(hdr->ack_num);
            uint8_t flags = hdr->flags;
            uint8_t data_offset = hdr->data_offset >> 4;
            uint16_t hdr_len = data_offset * 4;
            if (hdr_len < 20 || hdr_len > length) return false;
            uint16_t data_len = length - hdr_len;
            const uint8_t *data = packet + hdr_len;

            if (flags & TCP_FLAG_RST) {
                conn->state = TCP_STATE_CLOSED;
                return true;
            }

            if (conn->state == TCP_STATE_SYN_SENT) {
                if (flags & TCP_FLAG_SYN) {
                    conn->recv_ack = seq + 1;
                    conn->recv_seq = seq;
                    int pws = tcp_parse_wscale(packet, hdr_len);
                    if (pws >= 0) {
                        conn->peer_wscale = (uint8_t)pws;
                    } else {
                        /* RFC 7323: karsi taraf wscale gondermediyse her iki yon 0. */
                        conn->wscale_shift = 0;
                        conn->peer_wscale = 0;
                    }
                    /* SYN-ACK penceresi olceksizdir (RFC 7323). */
                    conn->peer_window = (uint32_t)htons16(hdr->window);
                    conn->send_seq = ack;
                    conn->send_unack = ack;   /* kendi SYN'umuz onaylandi */
                    conn->state = TCP_STATE_ESTABLISHED;
                    /* El sikismayi TAMAMLA: son ACK gonderilmezse bazi
                     * yiginlar (slirp dahil) baglantiyi yarim acik sayar ve
                     * veri gonderimini yok sayar. */
                    tcp_send_segment(i, conn->send_seq, TCP_FLAG_ACK, 0, 0);
                }
                return true;
            }

            if (conn->state == TCP_STATE_SYN_RECEIVED) {
                if (flags & TCP_FLAG_ACK) {
                    conn->peer_window = ((uint32_t)htons16(hdr->window)) << conn->peer_wscale;
                    tcp_ack_send(conn, ack);   /* SYN okteti dusulur */
                    conn->state = TCP_STATE_ESTABLISHED;
                }
                return true;
            }

            if (conn->state == TCP_STATE_FIN_WAIT1) {
                if (flags & TCP_FLAG_ACK) {
                    conn->peer_window = ((uint32_t)htons16(hdr->window)) << conn->peer_wscale;
                    int fin_acked = ((int32_t)(ack - conn->send_seq) >= 0);
                    tcp_ack_send(conn, ack);
                    if (fin_acked) {
                        conn->state = TCP_STATE_FIN_WAIT2;
                        conn->idle_ms = tcp_now();
                    }
                }
                return true;
            }

            if (conn->state == TCP_STATE_FIN_WAIT2 && (flags & TCP_FLAG_FIN)) {
                conn->recv_ack = seq + 1;
                conn->state = TCP_STATE_TIME_WAIT;
                conn->idle_ms = tcp_now();
                tcp_send_segment(i, conn->send_seq, TCP_FLAG_ACK, 0, 0);
                return true;
            }

            if (conn->state == TCP_STATE_LAST_ACK) {
                if (flags & TCP_FLAG_ACK) {
                    tcp_ack_send(conn, ack);
                    if ((int32_t)(ack - conn->send_seq) >= 0)
                        conn->state = TCP_STATE_CLOSED;
                }
                return true;
            }

            if (conn->state == TCP_STATE_CLOSE_WAIT) {
                if (flags & TCP_FLAG_ACK) {
                    conn->peer_window = ((uint32_t)htons16(hdr->window)) << conn->peer_wscale;
                    tcp_ack_send(conn, ack);
                }
                if (data_len > 0) {
                    int32_t diff = (int32_t)(seq - conn->recv_ack);
                    if (diff <= 0) {
                        uint32_t ov = (uint32_t)(-diff);
                        if (ov < data_len) {
                            uint32_t nl = data_len - ov;
                            uint32_t used = conn->recv_buf_tail - conn->recv_buf_head;
                            uint32_t sp = (used < TCP_RXBUF_SIZE)
                                        ? (TCP_RXBUF_SIZE - used) : 0;
                            uint32_t take = nl < sp ? nl : sp;
                            if (take > 0) {
                                tcp_rx_write(conn, data + ov, take);
                                conn->recv_ack += take;
                            }
                        }
                    }
                    conn->ack_pending = 1;
                    if (++conn->ack_pkts >= 8)
                        tcp_send_segment(i, conn->send_seq, TCP_FLAG_ACK, 0, 0);
                }
                return true;
            }

            if (conn->state == TCP_STATE_ESTABLISHED) {
                if (flags & TCP_FLAG_ACK) {
                    conn->peer_window = ((uint32_t)htons16(hdr->window)) << conn->peer_wscale;
                    tcp_ack_send(conn, ack);
                }

                /* Sira kontrolu (RFC 793): siradisi paketleri tampona kopyalayip
                 * TLS akisini bozma; yinelenen/kismi ortusen paketleri ayikla. */
                bool immediate_ack = false;
                if (data_len > 0) {
                    int32_t diff = (int32_t)(seq - conn->recv_ack);
                    if (diff <= 0) {
                        uint32_t ov = (uint32_t)(-diff);
                        uint32_t take = 0;
                        if (ov < data_len) {
                            uint32_t nl = data_len - ov;
                            uint32_t used = conn->recv_buf_tail - conn->recv_buf_head;
                            uint32_t sp = (used < TCP_RXBUF_SIZE)
                                        ? (TCP_RXBUF_SIZE - used) : 0;
                            take = nl < sp ? nl : sp;
                            if (take > 0) {
                                tcp_rx_write(conn, data + ov, take);
                                conn->recv_ack += take;
                            }
                        }
                        if (take > 0) {
                            conn->ack_pending = 1;
                            if (++conn->ack_pkts >= 8) immediate_ack = true;
                        } else {
                            /* Tekrar paket ya da tampon dolu: hemen ACK gonder. */
                            immediate_ack = true;
                        }
                    } else {
                        /* Siradisi paket: hizli yeniden gonderim icin hemen dup-ACK. */
                        immediate_ack = true;
                    }
                }

                /* FIN yalnizca onceki tum veriler sirali alindiysa islenir. */
                if ((flags & TCP_FLAG_FIN) && (seq + data_len == conn->recv_ack)) {
                    conn->recv_ack = seq + data_len + 1;
                    conn->state = TCP_STATE_CLOSE_WAIT;
                    tcp_send_segment(i, conn->send_seq, TCP_FLAG_ACK, 0, 0);
                    return true;
                }

                if (immediate_ack || (flags & TCP_FLAG_FIN)) {
                    tcp_send_segment(i, conn->send_seq, TCP_FLAG_ACK, 0, 0);
                }
                return true;
            }

            return true;
        }
    }

    if ((hdr->flags & TCP_FLAG_SYN) && !(hdr->flags & TCP_FLAG_ACK)) {
        for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
            tcp_connection_t *l = &tcp_connections[i];
            if (l->state != TCP_STATE_LISTEN || l->local_port != dst_port) continue;

            if (l->child_id >= 0) {
                tcp_connection_t *old = &tcp_connections[l->child_id];
                if (old->state != TCP_STATE_CLOSED && old->state != TCP_STATE_TIME_WAIT)
                    continue;
                l->child_id = -1;
            }

            int cid = tcp_alloc_conn();
            if (cid < 0) continue;

            tcp_connection_t *c = &tcp_connections[cid];
            for (int k = 0; k < 4; k++) {
                c->local_ip[k] = l->local_ip[k];
                c->remote_ip[k] = src_ip[k];
            }
            c->local_port = l->local_port;
            c->remote_port = src_port;
            c->recv_seq = htonl32(hdr->seq_num);
            c->recv_ack = c->recv_seq + 1;
            c->send_seq = 0x20000000u + tcp_ticks * 7919u;
            c->send_unack = c->send_seq;
            c->send_buf_len = 0;
            c->recv_buf_head = 0;
            c->recv_buf_tail = 0;
            c->retransmit_count = 0;
            c->retransmit_timer = tcp_now();
            c->last_activity = tcp_ticks;
            c->idle_ms = tcp_now();
            c->peer_window = (uint32_t)htons16(hdr->window);
            {
                uint16_t syn_hlen = (uint16_t)((hdr->data_offset >> 4) * 4);
                int pws = tcp_parse_wscale(packet, syn_hlen <= length ? syn_hlen : length);
                if (pws >= 0) {
                    c->peer_wscale = (uint8_t)pws;
                } else {
                    c->wscale_shift = 0;
                    c->peer_wscale = 0;
                }
            }
            c->state = TCP_STATE_SYN_RECEIVED;
            c->is_client = false;
            c->child_id = -1;

            if (!tcp_send_segment(cid, c->send_seq, TCP_FLAG_SYN | TCP_FLAG_ACK, 0, 0)) {
                c->state = TCP_STATE_CLOSED;
                continue;
            }
            c->send_seq++;
            l->child_id = (int8_t)cid;
            return true;
        }
    }

    return false;
}

void tcp_poll(void) {
    tcp_ticks++;

    /* RX kuyrugunu bosalt: bir kez yeterli (her cagri tum paketleri isler). */
    ipv4_poll();

    /* Madde 31: yeniden gonderim zamanlayicisi tanımlıydı ama hiçbir yerden
     * çağrılmıyordu; kayıp pakette bağlantı donuyordu. Her zamanlayıcılı
     * durum burada servis edilir. */
    uint32_t now = tcp_now();
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_connection_t *c = &tcp_connections[i];
        switch (c->state) {
        case TCP_STATE_SYN_SENT:
            if (tcp_ms_due(c->retransmit_timer, tcp_rto(c->retransmit_count))) {
                if (++c->retransmit_count > 5) { c->state = TCP_STATE_CLOSED; break; }
                c->retransmit_timer = now;
                tcp_send_segment(i, c->send_seq, TCP_FLAG_SYN, 0, 0);
            }
            break;
        case TCP_STATE_SYN_RECEIVED:
            if (tcp_ms_due(c->retransmit_timer, tcp_rto(c->retransmit_count))) {
                if (++c->retransmit_count > 5) { c->state = TCP_STATE_CLOSED; break; }
                c->retransmit_timer = now;
                tcp_send_segment(i, c->send_unack, TCP_FLAG_SYN | TCP_FLAG_ACK, 0, 0);
            }
            break;
        case TCP_STATE_ESTABLISHED:
            if (c->send_buf_len &&
                tcp_ms_due(c->retransmit_timer, tcp_rto(c->retransmit_count))) {
                if (++c->retransmit_count > 8) {
                    serial_printf("[tcp] slot %d onaysiz veri, kapatiliyor\n", i);
                    c->state = TCP_STATE_CLOSED;
                    break;
                }
                c->retransmit_timer = now;
                uint16_t n = c->send_buf_len < TCP_MSS ? c->send_buf_len : TCP_MSS;
                tcp_send_segment(i, c->send_unack, TCP_FLAG_PSH | TCP_FLAG_ACK,
                                 c->send_buffer, n);
            }
            break;
        case TCP_STATE_FIN_WAIT1:
        case TCP_STATE_LAST_ACK:
            if (tcp_ms_due(c->retransmit_timer, tcp_rto(c->retransmit_count))) {
                if (++c->retransmit_count > 5) { c->state = TCP_STATE_CLOSED; break; }
                c->retransmit_timer = now;
                tcp_send_segment(i, c->send_seq - 1, TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
            }
            break;
        case TCP_STATE_FIN_WAIT2:
            /* Karsi taraf FIN gondermezse slot sonsuza dek dolu kalirdi. */
            if (tcp_ms_due(c->idle_ms, 10000)) c->state = TCP_STATE_CLOSED;
            break;
        case TCP_STATE_TIME_WAIT:
            if (tcp_ms_due(c->idle_ms, 5000)) c->state = TCP_STATE_CLOSED;
            break;
        default:
            break;
        }
    }
}
