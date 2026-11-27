#include "kernel.h"
#include "usb.h"
#include "pci.h"
#include "usb_hid.h"
#include "serial.h"

usb_device_t usb_devices[USB_MAX_DEVICES];
int usb_device_count = 0;

static uint8_t usb_prev_mod[USB_MAX_DEVICES];
static uint8_t usb_prev_keys[USB_MAX_DEVICES][6];
static uint8_t usb_mouse_buf[USB_MAX_DEVICES][4];

/* Complete USB HID Usage Page 0x07 -> PS/2 Set-1 scancode mapping table.
 * Extended (0xE0-prefixed) usages are flagged separately via usb_hid_is_extended(). */
static const uint8_t usb_hid_to_scancode[256] = {
    /* 0x00..0x03: reserved / rollover / POST fail / undefined */
    0x00, 0x00, 0x00, 0x00,
    /* 0x04..0x1D: A..Z */
    0x1E, /* 0x04 A */  0x30, /* 0x05 B */  0x2E, /* 0x06 C */  0x20, /* 0x07 D */
    0x12, /* 0x08 E */  0x21, /* 0x09 F */  0x22, /* 0x0A G */  0x23, /* 0x0B H */
    0x17, /* 0x0C I */  0x24, /* 0x0D J */  0x25, /* 0x0E K */  0x26, /* 0x0F L */
    0x32, /* 0x10 M */  0x31, /* 0x11 N */  0x18, /* 0x12 O */  0x19, /* 0x13 P */
    0x10, /* 0x14 Q */  0x13, /* 0x15 R */  0x1F, /* 0x16 S */  0x14, /* 0x17 T */
    0x16, /* 0x18 U */  0x2F, /* 0x19 V */  0x11, /* 0x1A W */  0x2D, /* 0x1B X */
    0x15, /* 0x1C Y */  0x2C, /* 0x1D Z */
    /* 0x1E..0x27: 1..9, 0 */
    0x02, /* 0x1E 1 */  0x03, /* 0x1F 2 */  0x04, /* 0x20 3 */  0x05, /* 0x21 4 */
    0x06, /* 0x22 5 */  0x07, /* 0x23 6 */  0x08, /* 0x24 7 */  0x09, /* 0x25 8 */
    0x0A, /* 0x26 9 */  0x0B, /* 0x27 0 */
    /* 0x28..0x2C: Enter, Esc, Backspace, Tab, Space */
    0x1C, /* 0x28 Enter */
    0x01, /* 0x29 Esc */
    0x0E, /* 0x2A Backspace */
    0x0F, /* 0x2B Tab */
    0x39, /* 0x2C Space */
    /* 0x2D..0x38: punctuation (also TR-Q extra keys: [, ], ;, ', `, ,, ., /, \) */
    0x0C, /* 0x2D - / _ */
    0x0D, /* 0x2E = / + */
    0x1A, /* 0x2F [ / { (TR g) */
    0x1B, /* 0x30 ] / } (TR u) */
    0x2B, /* 0x31 \ / | */
    0x2B, /* 0x32 Non-US # / ~ */
    0x27, /* 0x33 ; / : (TR s) */
    0x28, /* 0x34 ' / " (TR i) */
    0x29, /* 0x35 ` / ~ */
    0x33, /* 0x36 , / < (TR o) */
    0x34, /* 0x37 . / > (TR c) */
    0x35, /* 0x38 / / ? */
    /* 0x39: Caps Lock */
    0x3A,
    /* 0x3A..0x45: F1..F12 */
    0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,
    /* 0x46..0x48: PrintScreen, ScrollLock, Pause */
    0x37, 0x46, 0x45,
    /* 0x49..0x52: Insert, Home, PgUp, Delete, End, PgDn, Right, Left, Down, Up (0xE0) */
    0x52, /* 0x49 Insert */
    0x47, /* 0x4A Home */
    0x49, /* 0x4B PageUp */
    0x53, /* 0x4C Delete */
    0x4F, /* 0x4D End */
    0x51, /* 0x4E PageDown */
    0x4D, /* 0x4F Right */
    0x4B, /* 0x50 Left */
    0x50, /* 0x51 Down */
    0x48, /* 0x52 Up */
    /* 0x53..0x63: Keypad */
    0x45, /* 0x53 NumLock */
    0x35, /* 0x54 KP / (0xE0) */
    0x37, /* 0x55 KP * */
    0x4A, /* 0x56 KP - */
    0x4E, /* 0x57 KP + */
    0x1C, /* 0x58 KP Enter (0xE0) */
    0x4F, /* 0x59 KP 1 */
    0x50, /* 0x5A KP 2 */
    0x51, /* 0x5B KP 3 */
    0x4B, /* 0x5C KP 4 */
    0x4C, /* 0x5D KP 5 */
    0x4D, /* 0x5E KP 6 */
    0x47, /* 0x5F KP 7 */
    0x48, /* 0x60 KP 8 */
    0x49, /* 0x61 KP 9 */
    0x52, /* 0x62 KP 0 */
    0x53, /* 0x63 KP . */
    /* 0x64..0x65: OEM102 (< > |) & Application */
    0x56, /* 0x64 Non-US \ / | (OEM102 < > |) */
    0x5D  /* 0x65 Application (0xE0) */
};

static bool usb_hid_is_extended(uint8_t key) {
    if (key >= 0x49 && key <= 0x52) return true;
    if (key == 0x54 || key == 0x58 || key == 0x65) return true;
    return false;
}

void keyboard_usb_push_scancode(uint8_t scancode);
void mouse_usb_move(int dx, int dy, uint8_t buttons, int scroll);

static void usb_push_ps2(uint8_t sc, bool ext, bool release) {
    if (sc == 0) return;
    if (ext) keyboard_usb_push_scancode(0xE0);
    keyboard_usb_push_scancode(release ? (uint8_t)(sc | 0x80u) : sc);
}

static bool key_in_array(uint8_t key, const uint8_t arr[6]) {
    for (int i = 0; i < 6; i++) {
        if (arr[i] == key) return true;
    }
    return false;
}

static void usb_process_keyboard(usb_device_t *dev, uint8_t *buf) {
    int idx = (int)(dev - usb_devices);
    if (idx < 0 || idx >= USB_MAX_DEVICES) return;

    /* Phantom / rollover state (0x01 in byte 2): ignore */
    if (buf[2] == 0x01) return;

    uint8_t mod = buf[0];
    uint8_t prev_m = usb_prev_mod[idx];

    static const struct {
        uint8_t bit;
        uint8_t sc;
        bool    ext;
    } mod_map[8] = {
        { 0x01, 0x1D, false }, /* Left Ctrl */
        { 0x02, 0x2A, false }, /* Left Shift */
        { 0x04, 0x38, false }, /* Left Alt */
        { 0x08, 0x5B, true  }, /* Left GUI */
        { 0x10, 0x1D, true  }, /* Right Ctrl */
        { 0x20, 0x36, false }, /* Right Shift */
        { 0x40, 0x38, true  }, /* Right Alt (AltGr) */
        { 0x80, 0x5C, true  }, /* Right GUI */
    };

    /* 1) Modifier changes (Make / Break) */
    uint8_t diff_m = mod ^ prev_m;
    if (diff_m) {
        for (int m = 0; m < 8; m++) {
            if (diff_m & mod_map[m].bit) {
                bool pressed = (mod & mod_map[m].bit) != 0;
                usb_push_ps2(mod_map[m].sc, mod_map[m].ext, !pressed);
            }
        }
        usb_prev_mod[idx] = mod;
    }

    /* 2) Key releases (present in prev_keys, absent in current buf[2..7]) */
    for (int i = 0; i < 6; i++) {
        uint8_t pk = usb_prev_keys[idx][i];
        if (pk < 4) continue;
        if (!key_in_array(pk, buf + 2)) {
            uint8_t sc = usb_hid_to_scancode[pk];
            usb_push_ps2(sc, usb_hid_is_extended(pk), true);
        }
    }

    /* 3) Key presses (present in current buf[2..7], absent in prev_keys) */
    for (int i = 2; i < 8; i++) {
        uint8_t nk = buf[i];
        if (nk < 4) continue;
        if (!key_in_array(nk, usb_prev_keys[idx])) {
            uint8_t sc = usb_hid_to_scancode[nk];
            usb_push_ps2(sc, usb_hid_is_extended(nk), false);
        }
    }

    for (int i = 0; i < 6; i++)
        usb_prev_keys[idx][i] = buf[2 + i];
}

static void usb_process_mouse(usb_device_t *dev, uint8_t *buf) {
    int idx = (int)(dev - usb_devices);
    if (idx < 0 || idx >= USB_MAX_DEVICES) return;

    int dx = (int)(int8_t)buf[1];
    int dy = -(int)(int8_t)buf[2]; /* USB HID +Y is down; mouse_usb_move subtracts dy */
    int scroll = (int)(int8_t)buf[3];
    uint8_t buttons = buf[0] & 0x07;

    if (dx != 0 || dy != 0 || scroll != 0 || buttons != (usb_mouse_buf[idx][0] & 0x07)) {
        usb_mouse_buf[idx][0] = buf[0];
        usb_mouse_buf[idx][1] = buf[1];
        usb_mouse_buf[idx][2] = buf[2];
        usb_mouse_buf[idx][3] = buf[3];
        mouse_usb_move(dx, dy, buttons, scroll);
    }
}

uint8_t usb_hid_scancode_lookup(uint8_t hid_usage) {
    return usb_hid_to_scancode[hid_usage];
}

static int usb_init_controller(usb_controller_t *ctrl) {
    if (ctrl->initialized) return 0;

    pci_enable_bus_mastering(ctrl->dev.bus, ctrl->dev.slot, ctrl->dev.func);

    int ret = -1;
    switch (ctrl->usb_type) {
        case 0:
            kernel_debug("USB: UHCI baslatiliyor...");
            ret = uhci_init(ctrl);
            break;
        case 1:
            kernel_debug("USB: OHCI baslatiliyor...");
            ret = -1;
            break;
        case 2:
            kernel_debug("USB: EHCI baslatiliyor...");
            ret = -1;
            break;
        case 3:
            kernel_debug("USB: xHCI baslatiliyor...");
            ret = xhci_init(ctrl);
            break;
        default:
            kernel_debug("USB: Bilinmeyen denetleyici tipi (0x%02X)", ctrl->usb_type);
            return -1;
    }

    if (ret == 0)
        ctrl->initialized = true;

    return ret;
}

void usb_init(void) {
    kernel_debug("USB: Sürücü başlatılıyor...");
    memset(usb_prev_mod, 0, sizeof(usb_prev_mod));
    memset(usb_prev_keys, 0, sizeof(usb_prev_keys));
    memset(usb_mouse_buf, 0, sizeof(usb_mouse_buf));

    pci_init();

    if (usb_controller_count == 0) {
        kernel_debug("USB: Denetleyici bulunamadı, PS/2 fallback");
        return;
    }

    kernel_debug("USB: %d denetleyici bulundu", usb_controller_count);

    for (int i = 0; i < usb_controller_count; i++) {
        usb_controller_t *ctrl = &usb_controllers[i];
        const char *names[] = {"UHCI", "OHCI", "EHCI", "xHCI"};
        const char *name = (ctrl->usb_type < 4) ? names[ctrl->usb_type] : "?";

        kernel_debug("USB: #%d %s denetleyici başlatılıyor...", i, name);

        if (usb_init_controller(ctrl) == 0) {
            kernel_debug("USB: #%d %s başarıyla başlatıldı", i, name);
        } else {
            kernel_debug("USB: #%d %s başlatılamadı", i, name);
        }
    }

    if (usb_device_count > 0) {
        kernel_debug("USB: %d cihaz bulundu", usb_device_count);
        for (int i = 0; i < usb_device_count; i++) {
            usb_device_t *dev = &usb_devices[i];
            if (dev->is_keyboard)
                kernel_debug("USB: Cihaz #%d = HID Klavye", i);
            else if (dev->is_mouse)
                kernel_debug("USB: Cihaz #%d = HID Fare", i);
        }
    }
    /* Always call usb_hid_init() so scancode selftest + hotplug/polling task start */
    usb_hid_init();
}

void usb_poll_all(void) {
    static int s_hp_div = 0;
    if (++s_hp_div >= 10) {
        s_hp_div = 0;
        xhci_poll_hotplug();
    }

    for (int i = 0; i < usb_device_count; i++) {
        usb_device_t *dev = &usb_devices[i];
        if (dev->state != USB_DEVSTATE_CONFIGURED) continue;

        uint8_t buf[8];
        int ret = -1;

        if (dev->is_keyboard) {
            if (dev->xhci_slot_id > 0) {
                ret = xhci_poll_keyboard(dev, buf);
            } else {
                for (int c = 0; c < usb_controller_count; c++) {
                    usb_controller_t *ctrl = &usb_controllers[c];
                    if (!ctrl->initialized) continue;
                    if (ctrl->usb_type == 0) ret = uhci_poll_keyboard(dev, buf);
                    else if (ctrl->usb_type == 2) ret = ehci_poll_keyboard(dev, buf);
                    if (ret == 0) break;
                }
            }
            if (ret == 0)
                usb_process_keyboard(dev, buf);
        } else if (dev->is_mouse) {
            if (dev->xhci_slot_id > 0) {
                ret = xhci_poll_mouse(dev, buf);
            } else {
                for (int c = 0; c < usb_controller_count; c++) {
                    usb_controller_t *ctrl = &usb_controllers[c];
                    if (!ctrl->initialized) continue;
                    if (ctrl->usb_type == 0) ret = uhci_poll_mouse(dev, buf);
                    else if (ctrl->usb_type == 2) ret = ehci_poll_mouse(dev, buf);
                    if (ret == 0) break;
                }
            }
            if (ret == 0)
                usb_process_mouse(dev, buf);
        }
    }
}

int usb_control_transfer(usb_device_t *dev, usb_setup_packet_t *setup,
                         uint8_t *data, int dir) {
    if (!dev) return -1;

    if (dev->xhci_slot_id > 0)
        return xhci_control_transfer(dev, setup, data, dir);

    for (int c = 0; c < usb_controller_count; c++) {
        usb_controller_t *ctrl = &usb_controllers[c];
        if (!ctrl->initialized) continue;
        switch (ctrl->usb_type) {
            case 0:
                return uhci_control_transfer(dev, setup, data, dir);
            case 2:
                return ehci_control_transfer(dev, setup, data, dir);
        }
    }
    return -1;
}

int usb_bulk_transfer(usb_device_t *dev, int ep_addr, void *data, int len, int dir) {
    if (!dev) return -1;

    if (dev->xhci_slot_id > 0)
        return xhci_bulk_transfer(dev, ep_addr, data, len, dir);

    for (int c = 0; c < usb_controller_count; c++) {
        usb_controller_t *ctrl = &usb_controllers[c];
        if (!ctrl->initialized) continue;
        switch (ctrl->usb_type) {
            case 0:
                return uhci_bulk_transfer(dev, ep_addr, data, len, dir);
        }
    }
    return -1;
}
