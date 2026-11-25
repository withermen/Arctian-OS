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
static uint32_t s_isn_counter = 0;

/* F2: Dinamik sira disi (OOO) yeniden birlestirme kuyrugu (yalniz OOO gelince kmalloc) */
typedef struct {
    bool     used;
    uint32_t seq;
    uint16_t len;
    uint8_t *data;
} tcp_ooo_slot_t;

typedef struct {
    tcp_ooo_slot_t slots[TCP_OOO_SLOTS];
} tcp_ooo_queue_t;

static inline uint64_t tcp_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline bool tcp_rdrand64(uint64_t *out) {
    static int has_rdrand = -1;
    if (has_rdrand < 0) {
        uint32_t eax, ebx, ecx, edx;
        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
        has_rdrand = (ecx & (1u << 30)) ? 1 : 0;
    }
    if (!has_rdrand) return false;
    for (int i = 0; i < 8; i++) {
        uint8_t ok = 0;
        uint64_t val = 0;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(val), "=qm"(ok));
        if (ok) { *out = val; return true; }
    }
    return false;
}

static inline uint64_t tcp_avalanche64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

/* F2: Kriptografik karisimli / entropi tabanli ISN uretici */
static uint32_t tcp_gen_isn(const uint8_t local_ip[4], uint16_t local_port,
                            const uint8_t remote_ip[4], uint16_t remote_port) {
    uint64_t tsc = tcp_rdtsc();
    uint64_t ms  = clock_ms();
    uint32_t seq_cnt = __atomic_add_fetch(&s_isn_counter, 1u, __ATOMIC_RELAXED);
    uint64_t rdr = 0;
    (void)tcp_rdrand64(&rdr);

    uint64_t lip = ((uint64_t)local_ip[0] << 24) | ((uint64_t)local_ip[1] << 16) |
                   ((uint64_t)local_ip[2] << 8)  | (uint64_t)local_ip[3];
    uint64_t rip = ((uint64_t)remote_ip[0] << 24) | ((uint64_t)remote_ip[1] << 16) |
                   ((uint64_t)remote_ip[2] << 8)  | (uint64_t)remote_ip[3];
    uint64_t tuple1 = (lip << 32) | rip;
    uint64_t tuple2 = ((uint64_t)local_port << 16) | (uint64_t)remote_port | ((uint64_t)seq_cnt << 32);

    uint64_t h = tcp_avalanche64(tsc ^ 0x517cc1b727220a95ULL);
    h = tcp_avalanche64(h ^ rdr ^ (ms * 0x9e3779b97f4a7c15ULL));
    h = tcp_avalanche64(h ^ tuple1);
    h = tcp_avalanche64(h ^ tuple2);

    uint32_t isn = (uint32_t)(h ^ (h >> 32));
    return isn ? isn : 0x9E3779B9u;
}

static void tcp_ooo_free_all(tcp_connection_t *c) {
    if (!c || !c->ooo) return;
    tcp_ooo_queue_t *q = (tcp_ooo_queue_t *)c->ooo;
    for (int i = 0; i < TCP_OOO_SLOTS; i++) {
        if (q->slots[i].used && q->slots[i].data) {
            kfree(q->slots[i].data);
        }
        q->slots[i].data = 0;
        q->slots[i].used = false;
    }
    kfree(q);
    c->ooo = 0;
}

static void tcp_newreno_init(tcp_connection_t *c) {
    c->cwnd = 10u * TCP_MSS;  /* RFC 6928 IW10 */
    c->ssthresh = 65535u;
    c->dup_acks = 0;
    c->in_recovery = 0;
    c->recover_seq = c->send_seq;
    c->fin_received = 0;
    c->fin_seq = 0;
}

static void tcp_release_conn(tcp_connection_t *c) {
    if (!c) return;
    tcp_ooo_free_all(c);
    if (c->recv_buffer) {
        kfree(c->recv_buffer);
        c->recv_buffer = 0;
    }
    c->state = TCP_STATE_CLOSED;
    c->send_buf_len = 0;
    c->recv_buf_head = 0;
    c->recv_buf_tail = 0;
    c->child_id = -1;
    c->dup_acks = 0;
    c->in_recovery = 0;
    c->fin_received = 0;
    c->fin_seq = 0;
}

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
        tcp_connections[i].recv_buffer = 0;
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
        tcp_connections[i].ooo = 0;
        tcp_newreno_init(&tcp_connections[i]);
    }
#ifdef ARCTIAN_NET_SELFTEST
    tcp_10_selftest();
#endif
}

static int tcp_alloc_conn(uint8_t init_state) {
    uint64_t fl = sched_irq_save();
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (tcp_connections[i].state == TCP_STATE_CLOSED) {
            tcp_connection_t *c = &tcp_connections[i];
            /* Slotu hemen sahiplen ki paralel tcp_connect cagrilari cakismasin */
            c->state = init_state;
            sched_irq_restore(fl);
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
            tcp_ooo_free_all(c);
            tcp_newreno_init(c);
            /* F3: alim tamponunu yuvada gerekince ayir */
            if (!c->recv_buffer) {
                c->recv_buffer = (uint8_t *)kmalloc(TCP_RXBUF_SIZE);
                if (!c->recv_buffer) {
                    c->state = TCP_STATE_CLOSED;
                    return -1;
                }
            }
            c->wscale_shift = TCP_WSCALE_SHIFT;
            c->peer_wscale = 0;
            c->last_adv_win = TCP_RXBUF_SIZE;
            c->ack_pending = 0;
            c->ack_pkts = 0;
            return i;
        }
    }
    sched_irq_restore(fl);
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
    if (!c->recv_buffer || len == 0) return;
    uint32_t pos = c->recv_buf_tail & TCP_RXBUF_MASK;
    uint32_t first = TCP_RXBUF_SIZE - pos;
    if (first > len) first = len;
    memcpy(&c->recv_buffer[pos], src, first);
    if (len > first) memcpy(&c->recv_buffer[0], src + first, len - first);
    c->recv_buf_tail += len;
}

static inline void tcp_rx_read(tcp_connection_t *c, uint8_t *dst, uint32_t len) {
    if (!c->recv_buffer || len == 0) return;
    uint32_t pos = c->recv_buf_head & TCP_RXBUF_MASK;
    uint32_t first = TCP_RXBUF_SIZE - pos;
    if (first > len) first = len;
    memcpy(dst, &c->recv_buffer[pos], first);
    if (len > first) memcpy(dst + first, &c->recv_buffer[0], len - first);
    c->recv_buf_head += len;
}

/* F2: Dinamik OOO kuyruguna sira disi segment ekle */
static void tcp_ooo_insert(tcp_connection_t *c, uint32_t seq, const uint8_t *data, uint16_t len) {
    if (!c || !data || len == 0) return;
    int32_t diff = (int32_t)(seq - c->recv_ack);
    if (diff <= 0 || (uint32_t)diff >= TCP_RXBUF_SIZE) return;

    tcp_ooo_queue_t *q = (tcp_ooo_queue_t *)c->ooo;
    if (!q) {
        q = (tcp_ooo_queue_t *)kmalloc(sizeof(tcp_ooo_queue_t));
        if (!q) return;
        memset(q, 0, sizeof(*q));
        c->ooo = q;
    }

    int free_idx = -1;
    int far_idx = -1;
    int32_t far_diff = diff;

    for (int i = 0; i < TCP_OOO_SLOTS; i++) {
        tcp_ooo_slot_t *s = &q->slots[i];
        if (!s->used) {
            if (free_idx < 0) free_idx = i;
            continue;
        }
        /* Mevcut slot yeni segmenti tamamen kapsiyorsa yineleme ekleme */
        int32_t sd = (int32_t)(seq - s->seq);
        if (sd >= 0 && (uint32_t)sd + len <= s->len) {
            return;
        }
        int32_t s_diff = (int32_t)(s->seq - c->recv_ack);
        if (s_diff > far_diff) {
            far_diff = s_diff;
            far_idx = i;
        }
    }

    int idx = free_idx;
    if (idx < 0) {
        if (far_idx < 0) return; /* Tum yuvalar daha yakin segmentlerle dolu */
        if (q->slots[far_idx].data) kfree(q->slots[far_idx].data);
        q->slots[far_idx].data = 0;
        q->slots[far_idx].used = false;
        idx = far_idx;
    }

    uint8_t *copy = (uint8_t *)kmalloc(len);
    if (!copy) return;
    memcpy(copy, data, len);
    q->slots[idx].used = true;
    q->slots[idx].seq  = seq;
    q->slots[idx].len  = len;
    q->slots[idx].data = copy;
}

/* F2: recv_ack ilerledikce bitisik/ortusen OOO segmentlerini RX halkasina bosalt */
static uint32_t tcp_ooo_drain(tcp_connection_t *c) {
    if (!c || !c->ooo || !c->recv_buffer) return 0;
    tcp_ooo_queue_t *q = (tcp_ooo_queue_t *)c->ooo;
    uint32_t drained = 0;

    for (;;) {
        int best = -1;
        for (int i = 0; i < TCP_OOO_SLOTS; i++) {
            if (!q->slots[i].used) continue;
            int32_t diff = (int32_t)(q->slots[i].seq - c->recv_ack);
            if (diff <= 0) {
                if (best < 0 || (int32_t)(q->slots[i].seq - q->slots[best].seq) < 0)
                    best = i;
            }
        }
        if (best < 0) break;

        tcp_ooo_slot_t *s = &q->slots[best];
        int32_t diff = (int32_t)(s->seq - c->recv_ack);
        int32_t end_diff = diff + (int32_t)s->len;
        if (end_diff <= 0) {
            if (s->data) kfree(s->data);
            s->data = 0;
            s->used = false;
            continue;
        }

        uint32_t ov = (diff < 0) ? (uint32_t)(-diff) : 0u;
        uint32_t nl = (uint32_t)s->len - ov;
        uint32_t used = c->recv_buf_tail - c->recv_buf_head;
        uint32_t sp = (used < TCP_RXBUF_SIZE) ? (TCP_RXBUF_SIZE - used) : 0u;
        uint32_t take = (nl < sp) ? nl : sp;
        if (take == 0) break;

        tcp_rx_write(c, s->data + ov, take);
        c->recv_ack += take;
        drained += take;

        if (take == nl) {
            if (s->data) kfree(s->data);
            s->data = 0;
            s->used = false;
        } else {
            uint16_t rem = (uint16_t)(nl - take);
            memmove(s->data, s->data + ov + take, rem);
            s->seq = c->recv_ack;
            s->len = rem;
            break;
        }
    }
    return drained;
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

    int id = tcp_alloc_conn(TCP_STATE_SYN_SENT);
    if (id < 0) return -1;

    tcp_connection_t *conn = &tcp_connections[id];
    for (int i = 0; i < 4; i++) {
        conn->local_ip[i] = ipv4_config.src_ip[i];
        conn->remote_ip[i] = dst_ip[i];
    }
    conn->local_port  = tcp_ephemeral_port++;
    if (tcp_ephemeral_port < 1024u) tcp_ephemeral_port = 1024u;
    conn->remote_port = dst_port;
    conn->send_seq = tcp_gen_isn(conn->local_ip, conn->local_port,
                                 conn->remote_ip, conn->remote_port);
    conn->recv_ack = 0;
    conn->send_unack = conn->send_seq;
    conn->recover_seq = conn->send_seq;
    conn->state = TCP_STATE_SYN_SENT;
    conn->retransmit_count = 0;
    conn->retransmit_timer = tcp_now();
    conn->last_activity = tcp_ticks;
    conn->idle_ms = tcp_now();
    conn->is_client = true;

    if (!tcp_send_segment(id, conn->send_seq, TCP_FLAG_SYN, 0, 0)) {
        serial_printf("[tcp] SYN gonderilemedi\n");
        tcp_release_conn(conn);
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
    tcp_release_conn(conn);
    return -1;
}

int tcp_listen(uint16_t port) {
    if (port == 0) return -1;
    int id = tcp_alloc_conn(TCP_STATE_LISTEN);
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
        uint32_t eff_win = conn->peer_window < conn->cwnd
                         ? conn->peer_window : conn->cwnd;
        uint32_t allow = eff_win > inflight ? eff_win - inflight : 0;
        uint32_t space = TCP_WINDOW_SIZE - conn->send_buf_len;
        uint32_t room = allow < space ? allow : space;
        if (room == 0) {
            if ((int32_t)(tcp_now() - (uint32_t)stall) >= 0) {
                serial_printf("[tcp] gonderim takildi: pencere/kabul yok\n");
                return false;
            }
            tcp_poll();
            eff_win = conn->peer_window < conn->cwnd
                    ? conn->peer_window : conn->cwnd;
            if (conn->state == TCP_STATE_ESTABLISHED &&
                eff_win <= (conn->send_seq - conn->send_unack))
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

/* F2: RFC 6582 NewReno tikaniklik denetimi (yavas baslangic, kacinma,
 * 3 yinelenen ACK ile hizli yeniden gonderim & hizli toparlanma). */
static void tcp_newreno_on_rto(tcp_connection_t *c) {
    uint32_t half = c->cwnd / 2u;
    c->ssthresh = (half > 2u * TCP_MSS) ? half : (2u * TCP_MSS);
    c->cwnd = TCP_MSS;
    c->dup_acks = 0;
    c->in_recovery = 0;
    c->recover_seq = c->send_seq;
}

static void tcp_newreno_on_ack(tcp_connection_t *conn, int conn_id,
                               uint32_t ack, uint32_t old_peer_win,
                               uint32_t new_peer_win, uint16_t data_len,
                               uint8_t flags, int *want_retx_out) {
    int32_t acked = (int32_t)(ack - conn->send_unack);
    if (acked > 0) {
        tcp_ack_send(conn, ack);
        if (conn->in_recovery) {
            if ((int32_t)(ack - conn->recover_seq) >= 0) {
                /* Tam ACK: hizli toparlanmadan cik */
                conn->cwnd = conn->ssthresh;
                conn->in_recovery = 0;
                conn->dup_acks = 0;
            } else {
                /* Kismi ACK (RFC 6582): pencereyi indir ve ilk onaysiz segmenti hemen gonder */
                uint32_t acku = (uint32_t)acked;
                if (conn->cwnd > acku + TCP_MSS) conn->cwnd -= acku;
                else conn->cwnd = TCP_MSS;
                if (acku >= TCP_MSS) conn->cwnd += TCP_MSS;
                conn->dup_acks = 0;
                if (conn->send_buf_len > 0) {
                    if (want_retx_out) *want_retx_out = 1;
                    if (conn_id >= 0) {
                        uint16_t n = conn->send_buf_len < TCP_MSS ? conn->send_buf_len : TCP_MSS;
                        tcp_send_segment(conn_id, conn->send_unack,
                                         TCP_FLAG_PSH | TCP_FLAG_ACK,
                                         conn->send_buffer, n);
                    }
                    conn->retransmit_timer = tcp_now();
                }
            }
        } else {
            conn->dup_acks = 0;
            if (conn->cwnd < conn->ssthresh) {
                uint32_t inc = (uint32_t)acked;
                if (inc > TCP_MSS) inc = TCP_MSS;
                conn->cwnd += inc;
            } else {
                uint32_t d = conn->cwnd ? conn->cwnd : TCP_MSS;
                uint32_t inc = (TCP_MSS * TCP_MSS) / d;
                if (inc == 0) inc = 1;
                conn->cwnd += inc;
            }
            if (conn->cwnd > (1024u * 1024u)) conn->cwnd = 1024u * 1024u;
        }
    } else if (acked == 0 && data_len == 0 &&
               !(flags & (TCP_FLAG_SYN | TCP_FLAG_FIN)) &&
               conn->send_buf_len > 0 && new_peer_win == old_peer_win) {
        if (!conn->in_recovery) {
            conn->dup_acks++;
            if (conn->dup_acks == 3) {
                uint32_t half = conn->cwnd / 2u;
                conn->ssthresh = (half > 2u * TCP_MSS) ? half : (2u * TCP_MSS);
                conn->cwnd = conn->ssthresh + 3u * TCP_MSS;
                conn->in_recovery = 1;
                conn->recover_seq = conn->send_seq;
                if (want_retx_out) *want_retx_out = 1;
                if (conn_id >= 0) {
                    uint16_t n = conn->send_buf_len < TCP_MSS ? conn->send_buf_len : TCP_MSS;
                    tcp_send_segment(conn_id, conn->send_unack,
                                     TCP_FLAG_PSH | TCP_FLAG_ACK,
                                     conn->send_buffer, n);
                }
                conn->retransmit_timer = tcp_now();
            }
        } else {
            conn->cwnd += TCP_MSS;
            if (conn->cwnd > (1024u * 1024u)) conn->cwnd = 1024u * 1024u;
        }
    }
}

int tcp_receive(int conn_id, uint8_t *buf, uint16_t buf_size) {
    if (!tcp_id_ok(conn_id)) return -1;
    tcp_connection_t *conn = &tcp_connections[conn_id];
    if (conn->state == TCP_STATE_CLOSED ||
        conn->state == TCP_STATE_TIME_WAIT ||
        !conn->recv_buffer) return -1;

    /* F3: RX tamponu cok dolu degilse agi yokla (halka tasmasin). */
    if ((conn->recv_buf_tail - conn->recv_buf_head) < (TCP_RXBUF_SIZE / 2u))
        tcp_poll();

    uint32_t available = conn->recv_buf_tail - conn->recv_buf_head;
    if (available == 0) return 0;
    if (available > buf_size) available = buf_size;

    tcp_rx_read(conn, buf, available);
    if (conn->ooo) tcp_ooo_drain(conn);

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
            tcp_release_conn(&tcp_connections[conn->child_id]);
            conn->child_id = -1;
        }
        tcp_release_conn(conn);
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
        if (conn->state == TCP_STATE_CLOSED) return true;
        if (conn->state == TCP_STATE_TIME_WAIT) return true;
        if ((int64_t)(clock_ms() - deadline) >= 0) break;
        sched_sleep_ms(1);   /* F2: pause-spin yerine uyku */
    }
    tcp_release_conn(conn);
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
        tcp_release_conn(&tcp_connections[c->child_id]);
    tcp_release_conn(c);
}

int tcp_state(int conn_id) {
    if (!tcp_id_ok(conn_id)) return -1;
    return tcp_connections[conn_id].state;
}

uint32_t tcp_rx_avail(int conn_id) {
    if (!tcp_id_ok(conn_id)) return 0;
    tcp_connection_t *c = &tcp_connections[conn_id];
    if (c->state == TCP_STATE_CLOSED || !c->recv_buffer) return 0;
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
                tcp_release_conn(conn);
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
                    tcp_newreno_init(conn);
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
                    tcp_newreno_init(conn);
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
                        tcp_release_conn(conn);
                }
                return true;
            }

            if (conn->state == TCP_STATE_CLOSE_WAIT) {
                if (flags & TCP_FLAG_ACK) {
                    uint32_t old_win = conn->peer_window;
                    uint32_t new_win = ((uint32_t)htons16(hdr->window)) << conn->peer_wscale;
                    conn->peer_window = new_win;
                    tcp_newreno_on_ack(conn, i, ack, old_win, new_win, data_len, flags, 0);
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
                                if (conn->ooo) tcp_ooo_drain(conn);
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
                    uint32_t old_win = conn->peer_window;
                    uint32_t new_win = ((uint32_t)htons16(hdr->window)) << conn->peer_wscale;
                    conn->peer_window = new_win;
                    tcp_newreno_on_ack(conn, i, ack, old_win, new_win, data_len, flags, 0);
                }

                /* F2: Sira kontrolu + OOO yeniden siralama tamponu */
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
                                if (conn->ooo) {
                                    if (tcp_ooo_drain(conn) > 0) immediate_ack = true;
                                }
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
                        /* Sira disi segment: OOO tamponuna sakla + dup-ACK gonder */
                        tcp_ooo_insert(conn, seq, data, data_len);
                        immediate_ack = true;
                    }
                }

                if (flags & TCP_FLAG_FIN) {
                    conn->fin_received = 1;
                    conn->fin_seq = seq + data_len;
                }

                /* FIN yalnizca onceki tum veriler (OOO dahil) sirali alindiysa islenir. */
                if (conn->fin_received && conn->recv_ack == conn->fin_seq) {
                    conn->fin_received = 0;
                    conn->recv_ack = conn->fin_seq + 1;
                    conn->state = TCP_STATE_CLOSE_WAIT;
                    tcp_send_segment(i, conn->send_seq, TCP_FLAG_ACK, 0, 0);
                    return true;
                }

                if (immediate_ack) {
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

            int cid = tcp_alloc_conn(TCP_STATE_SYN_RECEIVED);
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
            c->send_seq = tcp_gen_isn(c->local_ip, c->local_port,
                                      c->remote_ip, c->remote_port);
            c->send_unack = c->send_seq;
            c->recover_seq = c->send_seq;
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
                tcp_release_conn(c);
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
                if (++c->retransmit_count > 5) { tcp_release_conn(c); break; }
                c->retransmit_timer = now;
                tcp_send_segment(i, c->send_unack, TCP_FLAG_SYN, 0, 0);
            }
            break;
        case TCP_STATE_SYN_RECEIVED:
            if (tcp_ms_due(c->retransmit_timer, tcp_rto(c->retransmit_count))) {
                if (++c->retransmit_count > 5) { tcp_release_conn(c); break; }
                c->retransmit_timer = now;
                tcp_send_segment(i, c->send_unack, TCP_FLAG_SYN | TCP_FLAG_ACK, 0, 0);
            }
            break;
        case TCP_STATE_ESTABLISHED:
            if (c->send_buf_len &&
                tcp_ms_due(c->retransmit_timer, tcp_rto(c->retransmit_count))) {
                if (++c->retransmit_count > 8) {
                    serial_printf("[tcp] slot %d onaysiz veri, kapatiliyor\n", i);
                    tcp_release_conn(c);
                    break;
                }
                tcp_newreno_on_rto(c);
                c->retransmit_timer = now;
                uint16_t n = c->send_buf_len < TCP_MSS ? c->send_buf_len : TCP_MSS;
                tcp_send_segment(i, c->send_unack, TCP_FLAG_PSH | TCP_FLAG_ACK,
                                 c->send_buffer, n);
            }
            break;
        case TCP_STATE_FIN_WAIT1:
        case TCP_STATE_LAST_ACK:
            if (tcp_ms_due(c->retransmit_timer, tcp_rto(c->retransmit_count))) {
                if (++c->retransmit_count > 5) { tcp_release_conn(c); break; }
                c->retransmit_timer = now;
                tcp_send_segment(i, c->send_seq - 1, TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
            }
            break;
        case TCP_STATE_FIN_WAIT2:
            /* Karsi taraf FIN gondermezse slot sonsuza dek dolu kalirdi. */
            if (tcp_ms_due(c->idle_ms, 10000)) tcp_release_conn(c);
            break;
        case TCP_STATE_TIME_WAIT:
            if (tcp_ms_due(c->idle_ms, 5000)) tcp_release_conn(c);
            break;
        default:
            break;
        }
    }
}

void tcp_10_selftest(void) {
    /* 1. ISN benzersizligi (32 ardisik uretim) */
    uint8_t lip[4] = {10, 0, 2, 15};
    uint8_t rip[4] = {1, 1, 1, 1};
    uint32_t isns[32];
    int isn_ok = 1;
    for (int i = 0; i < 32; i++) {
        isns[i] = tcp_gen_isn(lip, (uint16_t)(40000 + (i & 3)), rip, 443);
        if (isns[i] == 0) isn_ok = 0;
        for (int j = 0; j < i; j++) {
            if (isns[j] == isns[i]) isn_ok = 0;
        }
    }

    /* 2. RFC 6582 NewReno durum makinesi */
    int nr_ok = 1;
    tcp_connection_t tc;
    memset(&tc, 0, sizeof(tc));
    tc.state = TCP_STATE_ESTABLISHED;
    tc.send_unack = 10000u;
    tc.send_seq = 10000u + 4u * TCP_MSS;
    tc.send_buf_len = 4u * TCP_MSS;
    tc.peer_window = 65535u;
    tcp_newreno_init(&tc);
    if (tc.cwnd != 10u * TCP_MSS || tc.ssthresh != 65535u) nr_ok = 0;

    /* Yavas baslangic: 1 MSS ACK -> cwnd += TCP_MSS */
    int retx = 0;
    tcp_newreno_on_ack(&tc, -1, 10000u + TCP_MSS, 65535u, 65535u, 0, TCP_FLAG_ACK, &retx);
    if (tc.cwnd != 11u * TCP_MSS || retx != 0) nr_ok = 0;

    /* 3 dup-ACK -> fast retransmit + fast recovery */
    uint32_t pre_cwnd = tc.cwnd;
    uint32_t exp_ssthresh = pre_cwnd / 2u;
    for (int d = 0; d < 3; d++) {
        retx = 0;
        tcp_newreno_on_ack(&tc, -1, tc.send_unack, 65535u, 65535u, 0, TCP_FLAG_ACK, &retx);
    }
    if (!tc.in_recovery || tc.ssthresh != exp_ssthresh ||
        tc.cwnd != exp_ssthresh + 3u * TCP_MSS || retx != 1) nr_ok = 0;

    /* Kismi ACK -> toparlanmada kal + hemen yeniden gonder */
    retx = 0;
    tcp_newreno_on_ack(&tc, -1, tc.send_unack + TCP_MSS, 65535u, 65535u, 0, TCP_FLAG_ACK, &retx);
    if (!tc.in_recovery || retx != 1) nr_ok = 0;

    /* Tam ACK -> toparlanmadan cik (cwnd = ssthresh) */
    tcp_newreno_on_ack(&tc, -1, tc.recover_seq, 65535u, 65535u, 0, TCP_FLAG_ACK, &retx);
    if (tc.in_recovery || tc.cwnd != tc.ssthresh || tc.dup_acks != 0) nr_ok = 0;

    /* Tikaniklik kacinma (cwnd >= ssthresh) */
    uint32_t ca_before = tc.cwnd;
    tc.send_seq += TCP_MSS;
    tc.send_buf_len = TCP_MSS;
    tcp_newreno_on_ack(&tc, -1, tc.send_unack + TCP_MSS, 65535u, 65535u, 0, TCP_FLAG_ACK, &retx);
    if (tc.cwnd <= ca_before) nr_ok = 0;

    /* RTO zaman asimi -> cwnd = TCP_MSS */
    tcp_newreno_on_rto(&tc);
    if (tc.cwnd != TCP_MSS || tc.in_recovery || tc.dup_acks != 0) nr_ok = 0;

    /* 3. OOO yeniden siralama + RX teslimi */
    int ooo_ok = 1;
    int cid = tcp_alloc_conn(TCP_STATE_ESTABLISHED);
    if (cid < 0) {
        ooo_ok = 0;
    } else {
        tcp_connection_t *oc = &tcp_connections[cid];
        oc->recv_ack = 5000u;
        oc->recv_buf_head = 0;
        oc->recv_buf_tail = 0;
        tcp_ooo_insert(oc, 5005u, (const uint8_t *)"WORLD", 5);
        tcp_ooo_insert(oc, 5010u, (const uint8_t *)"!", 1);
        tcp_ooo_insert(oc, 5005u, (const uint8_t *)"WORLD", 5); /* duplicate */
        if (oc->recv_ack != 5000u || oc->recv_buf_tail != 0) ooo_ok = 0;

        tcp_rx_write(oc, (const uint8_t *)"HELLO", 5);
        oc->recv_ack += 5;
        uint32_t drained = tcp_ooo_drain(oc);
        if (drained != 6u || oc->recv_ack != 5011u || tcp_rx_avail(cid) != 11u) ooo_ok = 0;

        uint8_t out[16];
        memset(out, 0, sizeof(out));
        tcp_rx_read(oc, out, 11);
        if (memcmp(out, "HELLOWORLD!", 11) != 0) ooo_ok = 0;

        tcp_release_conn(oc);
        if (oc->ooo != 0 || oc->recv_buffer != 0 || oc->state != TCP_STATE_CLOSED) ooo_ok = 0;
    }

    serial_printf("[tcp10] selftest: max_conn=%d ISN=%s NewReno=%s OOO=%s -> %s\n",
                  TCP_MAX_CONNECTIONS,
                  isn_ok ? "OK" : "FAIL",
                  nr_ok ? "OK" : "FAIL",
                  ooo_ok ? "OK" : "FAIL",
                  (isn_ok && nr_ok && ooo_ok) ? "PASS" : "FAIL");
}
