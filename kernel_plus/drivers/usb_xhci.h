#ifndef USB_XHCI_H
#define USB_XHCI_H

#include <stdint.h>
#include <stdbool.h>
#include "usb.h"
#include "pci.h"

int  xhci_init(usb_controller_t *ctrl);
int  xhci_control_transfer(usb_device_t *dev, usb_setup_packet_t *setup,
                           uint8_t *data, int dir);
int  xhci_bulk_transfer(usb_device_t *dev, int ep_addr, void *data, int len, int dir);
int  xhci_poll_keyboard(usb_device_t *dev, uint8_t *buf);
int  xhci_poll_mouse(usb_device_t *dev, uint8_t *buf);
int  xhci_poll_hotplug(void);
bool xhci_is_ready(void);
bool xhci_port_connected(int port);

#endif /* USB_XHCI_H */
