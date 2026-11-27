#ifndef USB_MSD_H
#define USB_MSD_H

#include <stdbool.h>

void usb_msd_scan(void);
bool usb_msd_is_attached(int bdev_idx);
int  usb_msd_count(void);

#endif /* USB_MSD_H */
