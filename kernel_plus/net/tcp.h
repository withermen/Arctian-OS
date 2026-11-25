#ifndef TCP_H
#define TCP_H

#include <stdint.h>
#include <stdbool.h>

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10
#define TCP_FLAG_URG 0x20

#define TCP_STATE_CLOSED       0
#define TCP_STATE_LISTEN       1
#define TCP_STATE_SYN_SENT     2
#define TCP_STATE_SYN_RECEIVED 3
#define TCP_STATE_ESTABLISHED  4
#define TCP_STATE_FIN_WAIT1    5
#define TCP_STATE_FIN_WAIT2    6
#define TCP_STATE_CLOSING      7
#define TCP_STATE_TIME_WAIT    8
#define TCP_STATE_CLOSE_WAIT   9
#define TCP_STATE_LAST_ACK    10

#define TCP_MAX_CONNECTIONS   64
#define TCP_RETRANSMIT_TIMEOUT 1000
#define TCP_WINDOW_SIZE       8192
#define TCP_MSS               1460
#define TCP_RXBUF_SIZE        (128u * 1024u)  /* F3: kmalloc dairesel halka (2^n) */
#define TCP_RXBUF_MASK        (TCP_RXBUF_SIZE - 1u)
#define TCP_WSCALE_SHIFT      3   /* F3: pencere olcegi (x8) */
#define TCP_OOO_SLOTS         8   /* F2: dinamik OOO yeniden siralama yuvalari */

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t  data_offset;
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;
} __attribute__((packed)) tcp_header_t;

typedef struct {
    uint8_t  local_ip[4];
    uint8_t  remote_ip[4];
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t send_seq;
    uint32_t send_ack;
    uint32_t recv_seq;
    uint32_t recv_ack;
    uint32_t send_unack;
    uint32_t send_next;
    uint8_t  state;
    uint8_t  retransmit_count;
    uint32_t retransmit_timer;
    uint32_t last_activity;
    uint8_t  send_buffer[TCP_WINDOW_SIZE];
    uint16_t send_buf_len;
    uint8_t *recv_buffer;      /* F3: kmalloc(TCP_RXBUF_SIZE), alloc_conn'da */
    uint32_t recv_buf_head;   /* mutlak offset; bosalinca 0'a doner */
    uint32_t recv_buf_tail;
    bool     is_client;
    int8_t   child_id;        /* dinleyici: bekleyen baglanti (-1 = yok) */
    uint32_t peer_window;     /* karsi tarafin son bildirdigi pencere (bayt, olcekli) */
    uint32_t last_adv_win;    /* F3: bizim son bildirdigimiz alim penceresi (bayt) */
    uint8_t  wscale_shift;    /* F3: bizim SYN'de bildirdigimiz olcek */
    uint8_t  peer_wscale;     /* F3: karsi tarafin SYN'de bildirdigi olcek */
    uint8_t  ack_pending;     /* F3: birlestirilmis ACK bekliyor */
    uint16_t ack_pkts;        /* F3: son ACK'tan beri alinan sirali paket sayisi */
    uint32_t idle_ms;         /* son aktivite (clock_ms kesiti): bekleme
                               * durumlarinda slot toplama icin */
    /* F2: RFC 6582 NewReno tikaniklik denetimi + dinamik OOO kuyrugu */
    uint32_t cwnd;            /* tikaniklik penceresi (bayt) */
    uint32_t ssthresh;        /* yavas baslangic esigi (bayt) */
    uint8_t  dup_acks;        /* ardisik yinelenen ACK sayaci */
    uint8_t  in_recovery;     /* 1 = hizli toparlanma etkin */
    uint8_t  fin_received;    /* 1 = sira disi FIN alindi, bekleniyor */
    uint32_t recover_seq;     /* RFC 6582 'recover' sira numarasi */
    uint32_t fin_seq;         /* FIN'in beklendigi sira numarasi */
    void    *ooo;             /* dinamik OOO tamponu (yalniz gerekince kmalloc) */
} tcp_connection_t;

void tcp_init(void);
void tcp_10_selftest(void);
int  tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port);
int  tcp_listen(uint16_t port);
int  tcp_accept(int listen_id);
bool tcp_send(int conn_id, const uint8_t *data, uint16_t length);
int  tcp_receive(int conn_id, uint8_t *buf, uint16_t buf_size);
bool tcp_close(int conn_id);
void tcp_abort(int conn_id);   /* el sikismasiz/zamanasimli slotu serit */
int  tcp_state(int conn_id);
uint32_t tcp_rx_avail(int conn_id);
void tcp_poll(void);
void tcp_flush_acks(void);
bool tcp_receive_packet(const uint8_t *src_ip, const uint8_t *packet, uint16_t length);

#endif
