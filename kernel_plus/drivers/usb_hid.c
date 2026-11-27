#include "kernel.h"
#include "usb_hid.h"
#include "usb.h"
#include "keyboard.h"
#include "sched.h"
#include "spinlock.h"
#include "serial.h"

extern uint8_t usb_hid_scancode_lookup(uint8_t hid_usage);

static bool hid_initialized = false;
static bool hid_task_started = false;

static void usb_hid_selftest(void) {
    int ok = (usb_hid_scancode_lookup(0x04) == 0x1E && /* A */
              usb_hid_scancode_lookup(0x1D) == 0x2C && /* Z */
              usb_hid_scancode_lookup(0x1E) == 0x02 && /* 1 */
              usb_hid_scancode_lookup(0x27) == 0x0B && /* 0 */
              usb_hid_scancode_lookup(0x28) == 0x1C && /* Enter */
              usb_hid_scancode_lookup(0x29) == 0x01 && /* Esc */
              usb_hid_scancode_lookup(0x2A) == 0x0E && /* Backspace */
              usb_hid_scancode_lookup(0x2F) == 0x1A && /* [ (TR g) */
              usb_hid_scancode_lookup(0x33) == 0x27 && /* ; (TR s) */
              usb_hid_scancode_lookup(0x3A) == 0x3B && /* F1 */
              usb_hid_scancode_lookup(0x45) == 0x58 && /* F12 */
              usb_hid_scancode_lookup(0x4F) == 0x4D && /* Right */
              usb_hid_scancode_lookup(0x64) == 0x56);  /* OEM102 < > | */
    serial_printf("[usb-hid] scancode selftest: %s\n", ok ? "OK" : "FAIL");
}

static void usb_hid_poll_task(void *arg) {
    (void)arg;
    for (;;) {
        usb_hid_poll();
        uint64_t f = irq_save();
        keyboard_process_usb_queue();
        irq_restore(f);
        sched_sleep_ms(10);
    }
}

int usb_hid_init_device(usb_device_t *dev) {
    if (!dev || dev->class_code != USB_CLASS_HID) return -1;
    if (dev->state != USB_DEVSTATE_CONFIGURED) return -1;

    if (dev->protocol == USB_PROTOCOL_KEYBOARD) {
        dev->is_keyboard = true;
        usb_hid_set_boot_protocol(dev);

        usb_setup_packet_t setup;
        setup.bmRequestType = USB_DIR_OUT | (USB_REQTYPE_CLASS << 5) | USB_RECIP_INTERFACE;
        setup.bRequest = HID_REQ_SET_IDLE;
        setup.wValue = 0;
        setup.wIndex = dev->interface_num;
        setup.wLength = 0;
        usb_control_transfer(dev, &setup, 0, USB_DIR_OUT);
        hid_initialized = true;
        serial_printf("[usb-hid] keyboard ready slot=%d\n", dev->xhci_slot_id);
        return 0;
    } else if (dev->protocol == USB_PROTOCOL_MOUSE) {
        dev->is_mouse = true;
        usb_hid_set_boot_protocol(dev);

        usb_setup_packet_t setup;
        setup.bmRequestType = USB_DIR_OUT | (USB_REQTYPE_CLASS << 5) | USB_RECIP_INTERFACE;
        setup.bRequest = HID_REQ_SET_IDLE;
        setup.wValue = 0;
        setup.wIndex = dev->interface_num;
        setup.wLength = 0;
        usb_control_transfer(dev, &setup, 0, USB_DIR_OUT);
        hid_initialized = true;
        serial_printf("[usb-hid] mouse ready slot=%d\n", dev->xhci_slot_id);
        return 0;
    }
    return -1;
}

int usb_hid_init(void) {
    int found = 0;

    usb_hid_selftest();
    kernel_debug("USB HID: Cihazlar taranıyor...");

    for (int i = 0; i < usb_device_count; i++) {
        if (usb_hid_init_device(&usb_devices[i]) == 0)
            found++;
    }

    hid_initialized = (found > 0);
    kernel_debug("USB HID: %d cihaz başlatıldı", found);

    if (!hid_task_started) {
        sched_tid_t tid = sched_spawn("usbhid", usb_hid_poll_task, 0);
        if (tid > 0) {
            sched_mark_freeze_exempt(tid, 1);
            sched_enqueue_cpu(0, tid);
            hid_task_started = true;
        }
    }

    return (found > 0) ? 0 : -1;
}

void usb_hid_poll(void) {
    usb_poll_all();
}

int usb_hid_keyboard_count(void) {
    int count = 0;
    for (int i = 0; i < usb_device_count; i++) {
        if (usb_devices[i].class_code == USB_CLASS_HID &&
            usb_devices[i].protocol == USB_PROTOCOL_KEYBOARD &&
            usb_devices[i].state == USB_DEVSTATE_CONFIGURED)
            count++;
    }
    return count;
}

int usb_hid_mouse_count(void) {
    int count = 0;
    for (int i = 0; i < usb_device_count; i++) {
        if (usb_devices[i].class_code == USB_CLASS_HID &&
            usb_devices[i].protocol == USB_PROTOCOL_MOUSE &&
            usb_devices[i].state == USB_DEVSTATE_CONFIGURED)
            count++;
    }
    return count;
}

bool usb_hid_has_keyboard(void) {
    return usb_hid_keyboard_count() > 0;
}

bool usb_hid_has_mouse(void) {
    return usb_hid_mouse_count() > 0;
}

void usb_hid_set_boot_protocol(usb_device_t *dev) {
    if (!dev || dev->class_code != USB_CLASS_HID) return;

    usb_setup_packet_t setup;
    setup.bmRequestType = USB_DIR_OUT | (USB_REQTYPE_CLASS << 5) | USB_RECIP_INTERFACE;
    setup.bRequest = HID_REQ_SET_PROTOCOL;
    setup.wValue = HID_PROTOCOL_BOOT;
    setup.wIndex = dev->interface_num;
    setup.wLength = 0;
    usb_control_transfer(dev, &setup, 0, USB_DIR_OUT);
}
