#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* VirtIO-Net PCI device IDs (Vendor 0x1AF4) */
#define VIRTIO_NET_PCI_DEV_TRANSITIONAL 0x1000u
#define VIRTIO_NET_PCI_DEV_MODERN       0x1041u

int  virtio_net_init(void);
bool virtio_net_present(void);
bool virtio_net_send(const void *frame, size_t len);
int  virtio_net_recv(void *buf, size_t cap);
bool virtio_net_has_packet(void);
bool virtio_net_link_up(void);
void virtio_net_mac(uint8_t out[6]);

/* net.c driver table adapters */
bool virtio_net_drv_send(void *ctx, const uint8_t *data, uint16_t length);
bool virtio_net_drv_has(void *ctx);
bool virtio_net_drv_recv(void *ctx, uint8_t *buf, uint16_t *length);

#endif /* VIRTIO_NET_H */
