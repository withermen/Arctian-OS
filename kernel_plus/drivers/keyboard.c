/**
 * Arctian Keyboard Driver
 * TR-Q / TR-F / EN-Q UTF-8 klavye destegi - 64-bit AMD64
 * Bagimsiz: <string.h> kullanilmadi
 * USB HID bridge: keyboard_usb_push_scancode()
 */

#include "kernel.h"
#include "isr.h"
#include "pic.h"
#include "keyboard.h"
#include "clock.h"
#include "mouse.h"
#include "serial.h"

/* USB HID poll kancasi (usb_hid.c yoksa zayif no-op) */
__attribute__((weak)) void usb_hid_poll(void) { }

/* USB HID'den gelen scancode'lari PS/2 handler'a iletmek icin */
#define USB_SCANCODE_QUEUE_SIZE 32
static uint8_t usb_scancode_queue[USB_SCANCODE_QUEUE_SIZE];
static int usb_scancode_queue_head = 0;
static int usb_scancode_queue_tail = 0;

/* Klavye durumu */
static bool shift_pressed = false;
static bool ctrl_pressed = false;
static bool alt_pressed = false;
static bool caps_lock = false;
static bool gui_pressed = false;
static bool altgr_pressed = false;  /* Right Alt (AltGr) */
static bool extended_byte = false;  /* 0xE0 prefix handler */

/* Terminal degiskenleri (main_kernel'de tanimli) */
extern size_t terminal_row;
extern size_t terminal_column;
extern uint8_t terminal_color;
extern uint16_t* terminal_buffer;

/* GUI baslat menusu (desktop'ta tanimli) */
__attribute__((weak)) void gui_toggle_start_menu(void) { }

/* Klavye portlari */
#define KEYBOARD_DATA_PORT    0x60
#define KEYBOARD_STATUS_PORT  0x64
#define KEYBOARD_COMMAND_PORT 0x64

/* Klavye komutlari */
#define KEYBOARD_CMD_ENABLE       0xAE
#define KEYBOARD_CMD_DISABLE      0xAD
#define KEYBOARD_CMD_READ_CONFIG  0x20
#define KEYBOARD_CMD_WRITE_CONFIG 0x60

#define KEY_RELEASED 0x80

/* LVGL ozel tus kodlari (lv_group.h ile birebir uyumlu) */
#define KBD_KEY_HOME  2u
#define KBD_KEY_END   3u
#define KBD_KEY_UP    17u
#define KBD_KEY_DOWN  18u
#define KBD_KEY_RIGHT 19u
#define KBD_KEY_LEFT  20u
#define KBD_KEY_DEL   127u

static int ps2_wait_buffer(void) {
    for (int i = 0; i < 100000; i++)
        if (inb(KEYBOARD_STATUS_PORT) & 0x01) return 1;
    return 0;
}

static void ps2_flush(void) {
    for (int i = 0; i < 32; i++) {
        if (!(inb(KEYBOARD_STATUS_PORT) & 0x01)) break;
        inb(KEYBOARD_DATA_PORT);
    }
}

/* Legacy klavye bufferi */
#define KEYBOARD_BUFFER_SIZE 128
static char keyboard_buffer[KEYBOARD_BUFFER_SIZE];
static size_t keyboard_buffer_pos = 0;

/* Event queue for GUI main loop (circular buffer) */
#define KEYBOARD_EVENT_QUEUE_SIZE 64
static keyboard_event_t keyboard_event_queue[KEYBOARD_EVENT_QUEUE_SIZE];
static int keyboard_event_head = 0;
static int keyboard_event_tail = 0;

static int s_hotkey_q[8];
static int s_hotkey_h = 0;
static int s_hotkey_t = 0;
static int s_hot_guard = 0;

void keyboard_push_hotkey(int code) {
    static uint64_t last_ms;
    uint64_t now = clock_ms();
    if (now - last_ms < 400) return;
    last_ms = now;
    int next = (s_hotkey_h + 1) % 8;
    if (next == s_hotkey_t) return;
    s_hotkey_q[s_hotkey_h] = code;
    s_hotkey_h = next;
}

int keyboard_pop_hotkey(void) {
    if (s_hotkey_t == s_hotkey_h) return 0;
    int c = s_hotkey_q[s_hotkey_t];
    s_hotkey_t = (s_hotkey_t + 1) % 8;
    return c;
}

int keyboard_ctrl_down(void)  { return ctrl_pressed ? 1 : 0; }
int keyboard_shift_down(void) { return shift_pressed ? 1 : 0; }

/* Unicode codepoint -> legacy ASCII fallback (for legacy 8-bit callers) */
static char kbd_cp_to_ascii(uint32_t cp) {
    if (cp == 0) return 0;
    if (cp < 0x80u) return (char)cp;
    switch (cp) {
        case 0x011Fu: return 'g'; /* ğ */
        case 0x011Eu: return 'G'; /* Ğ */
        case 0x00FCu: return 'u'; /* ü */
        case 0x00DCu: return 'U'; /* Ü */
        case 0x015Fu: return 's'; /* ş */
        case 0x015Eu: return 'S'; /* Ş */
        case 0x0131u: return 'i'; /* ı */
        case 0x0130u: return 'I'; /* İ */
        case 0x00F6u: return 'o'; /* ö */
        case 0x00D6u: return 'O'; /* Ö */
        case 0x00E7u: return 'c'; /* ç */
        case 0x00C7u: return 'C'; /* Ç */
        case 0x20ACu: return 'E'; /* € */
        case 0x20BAu: return 'T'; /* ₺ */
        case 0x00A3u: return '#'; /* £ */
        case 0x00BDu: return '5'; /* ½ */
        case 0x00E9u: return 'e'; /* é */
        case 0x00C9u: return 'E'; /* É */
        case 0x00E6u: return 'a'; /* æ */
        case 0x00DFu: return 's'; /* ß */
        default:      return 0;
    }
}

static int kbd_cp_to_utf8(uint32_t cp, char out[5]) {
    if (cp == 0) {
        out[0] = '\0';
        return 0;
    }
    if (cp < 0x80u) {
        out[0] = (char)cp;
        out[1] = '\0';
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        out[2] = '\0';
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        out[3] = '\0';
        return 3;
    }
    if (cp <= 0x10FFFFu) {
        out[0] = (char)(0xF0u | (cp >> 18));
        out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[3] = (char)(0x80u | (cp & 0x3Fu));
        out[4] = '\0';
        return 4;
    }
    out[0] = '\0';
    return 0;
}

static void keyboard_push_event_cp(uint32_t cp, uint8_t scancode, bool pressed) {
    int next = (keyboard_event_head + 1) % KEYBOARD_EVENT_QUEUE_SIZE;
    if (next == keyboard_event_tail) return; /* queue full — drop */
    keyboard_event_t *ev = &keyboard_event_queue[keyboard_event_head];
    ev->ch = kbd_cp_to_ascii(cp);
    ev->scancode = scancode;
    ev->pressed = pressed;
    ev->alt = alt_pressed;
    ev->ctrl = ctrl_pressed;
    ev->shift = shift_pressed;
    ev->codepoint = cp;
    ev->altgr = altgr_pressed;
    keyboard_event_head = next;
}

bool keyboard_pop_event(keyboard_event_t *ev) {
    if (keyboard_event_tail == keyboard_event_head) return false;
    *ev = keyboard_event_queue[keyboard_event_tail];
    keyboard_event_tail = (keyboard_event_tail + 1) % KEYBOARD_EVENT_QUEUE_SIZE;
    return true;
}

bool keyboard_event_avail(void) {
    keyboard_process_usb_queue();
    return keyboard_event_tail != keyboard_event_head;
}

uint32_t keyboard_pop_cp(void) {
    keyboard_process_usb_queue();
    keyboard_event_t ev;
    while (keyboard_pop_event(&ev)) {
        if (!ev.pressed) continue;
        if (ev.codepoint != 0) return ev.codepoint;
        if (ev.ch != 0) return (uint32_t)(unsigned char)ev.ch;
    }
    return 0;
}

int keyboard_pop_utf8(char out[5]) {
    if (!out) return 0;
    uint32_t cp = keyboard_pop_cp();
    if (cp == 0) {
        out[0] = '\0';
        return 0;
    }
    return kbd_cp_to_utf8(cp, out);
}

int keyboard_pop(void) {
    keyboard_process_usb_queue();
    keyboard_event_t ev;
    while (keyboard_pop_event(&ev)) {
        if (!ev.pressed) continue;
        if (ev.codepoint != 0 && ev.codepoint < 0x80u)
            return (int)ev.codepoint;
        if (ev.ch != 0)
            return (int)(unsigned char)ev.ch;
    }
    return -1;
}

/* Legacy ASCII fallback tables (exported for binary/header compatibility) */
const char keyboard_map_normal[128] = {
    [0x01] = '\x1b',
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '*', [0x0D] = '-', [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = 'g', [0x1B] = 'u', [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
    [0x27] = 's', [0x28] = 'i', [0x29] = '"',
    [0x2B] = ',',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm',
    [0x33] = 'o', [0x34] = 'c', [0x35] = '.',
    [0x39] = ' ', [0x56] = '<'
};

const char keyboard_map_shift[128] = {
    [0x01] = '\x1b',
    [0x02] = '!', [0x03] = '\'', [0x04] = '^', [0x05] = '+', [0x06] = '%',
    [0x07] = '&', [0x08] = '/', [0x09] = '(', [0x0A] = ')', [0x0B] = '=',
    [0x0C] = '?', [0x0D] = '_', [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T',
    [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P',
    [0x1A] = 'G', [0x1B] = 'U', [0x1C] = '\n',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G',
    [0x23] = 'H', [0x24] = 'J', [0x25] = 'K', [0x26] = 'L',
    [0x27] = 'S', [0x28] = 'I', [0x29] = 'E',
    [0x2B] = ';',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B',
    [0x31] = 'N', [0x32] = 'M',
    [0x33] = 'O', [0x34] = 'C', [0x35] = ':',
    [0x39] = ' ', [0x56] = '>'
};

/* ================================================================== */
/* Full Unicode Keymaps: TR-Q (TS 2118), TR-F (TS 2117), EN-Q (US)    */
/* ================================================================== */

static const uint32_t kbd_trq_normal[128] = {
    [0x01] = 0x1Bu,
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '*', [0x0D] = '-', [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 0x0131u /* ı */, [0x18] = 'o', [0x19] = 'p',
    [0x1A] = 0x011Fu /* ğ */, [0x1B] = 0x00FCu /* ü */, [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
    [0x27] = 0x015Fu /* ş */, [0x28] = 'i', [0x29] = '"',
    [0x2B] = ',',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm',
    [0x33] = 0x00F6u /* ö */, [0x34] = 0x00E7u /* ç */, [0x35] = '.',
    [0x39] = ' ', [0x56] = '<'
};

static const uint32_t kbd_trq_shift[128] = {
    [0x01] = 0x1Bu,
    [0x02] = '!', [0x03] = '\'', [0x04] = '^', [0x05] = '+', [0x06] = '%',
    [0x07] = '&', [0x08] = '/', [0x09] = '(', [0x0A] = ')', [0x0B] = '=',
    [0x0C] = '?', [0x0D] = '_', [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T',
    [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P',
    [0x1A] = 0x011Eu /* Ğ */, [0x1B] = 0x00DCu /* Ü */, [0x1C] = '\n',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G',
    [0x23] = 'H', [0x24] = 'J', [0x25] = 'K', [0x26] = 'L',
    [0x27] = 0x015Eu /* Ş */, [0x28] = 0x0130u /* İ */, [0x29] = 0x00E9u /* é */,
    [0x2B] = ';',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B',
    [0x31] = 'N', [0x32] = 'M',
    [0x33] = 0x00D6u /* Ö */, [0x34] = 0x00C7u /* Ç */, [0x35] = ':',
    [0x39] = ' ', [0x56] = '>'
};

static const uint32_t kbd_trq_altgr[128] = {
    [0x02] = '>', [0x03] = 0x00A3u /* £ */, [0x04] = '#', [0x05] = '$', [0x06] = 0x00BDu /* ½ */,
    [0x08] = '{', [0x09] = '[', [0x0A] = ']', [0x0B] = '}',
    [0x0C] = '\\', [0x0D] = '|',
    [0x10] = '@',
    [0x12] = 0x20ACu /* € */,
    [0x14] = 0x20BAu /* ₺ */,
    [0x1B] = '~',
    [0x1E] = 0x00E6u /* æ */, [0x1F] = 0x00DFu /* ß */,
    [0x27] = '`', [0x29] = '<', [0x2B] = '`', [0x56] = '|'
};

static const uint32_t kbd_trf_normal[128] = {
    [0x01] = 0x1Bu,
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '/', [0x0D] = '-', [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'f', [0x11] = 'g', [0x12] = 0x011Fu /* ğ */, [0x13] = 0x0131u /* ı */, [0x14] = 'o',
    [0x15] = 'd', [0x16] = 'r', [0x17] = 'n', [0x18] = 'h', [0x19] = 'p',
    [0x1A] = 'q', [0x1B] = 'w', [0x1C] = '\n',
    [0x1E] = 'u', [0x1F] = 'i', [0x20] = 'e', [0x21] = 'a', [0x22] = 0x00FCu /* ü */,
    [0x23] = 't', [0x24] = 'k', [0x25] = 'm', [0x26] = 'l', [0x27] = 'y',
    [0x28] = 0x015Fu /* ş */, [0x29] = '+',
    [0x2B] = 'x', [0x2C] = 'j', [0x2D] = 0x00F6u /* ö */, [0x2E] = 'v', [0x2F] = 'c',
    [0x30] = 0x00E7u /* ç */, [0x31] = 'z', [0x32] = 's', [0x33] = 'b', [0x34] = '.',
    [0x35] = ',',
    [0x39] = ' ', [0x56] = '<'
};

static const uint32_t kbd_trf_shift[128] = {
    [0x01] = 0x1Bu,
    [0x02] = '!', [0x03] = '"', [0x04] = '^', [0x05] = '$', [0x06] = '%',
    [0x07] = '&', [0x08] = '\'', [0x09] = '(', [0x0A] = ')', [0x0B] = '=',
    [0x0C] = '?', [0x0D] = '_', [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'F', [0x11] = 'G', [0x12] = 0x011Eu /* Ğ */, [0x13] = 'I', [0x14] = 'O',
    [0x15] = 'D', [0x16] = 'R', [0x17] = 'N', [0x18] = 'H', [0x19] = 'P',
    [0x1A] = 'Q', [0x1B] = 'W', [0x1C] = '\n',
    [0x1E] = 'U', [0x1F] = 0x0130u /* İ */, [0x20] = 'E', [0x21] = 'A', [0x22] = 0x00DCu /* Ü */,
    [0x23] = 'T', [0x24] = 'K', [0x25] = 'M', [0x26] = 'L', [0x27] = 'Y',
    [0x28] = 0x015Eu /* Ş */, [0x29] = '*',
    [0x2B] = 'X', [0x2C] = 'J', [0x2D] = 0x00D6u /* Ö */, [0x2E] = 'V', [0x2F] = 'C',
    [0x30] = 0x00C7u /* Ç */, [0x31] = 'Z', [0x32] = 'S', [0x33] = 'B', [0x34] = ':',
    [0x35] = ';',
    [0x39] = ' ', [0x56] = '>'
};

static const uint32_t kbd_trf_altgr[128] = {
    [0x02] = '>', [0x03] = 0x00A3u /* £ */, [0x04] = '#', [0x05] = '$', [0x06] = 0x00BDu /* ½ */,
    [0x08] = '{', [0x09] = '[', [0x0A] = ']', [0x0B] = '}',
    [0x0C] = '\\', [0x0D] = '|',
    [0x10] = '@',
    [0x1B] = '~',
    [0x20] = 0x20ACu /* € */,
    [0x23] = 0x20BAu /* ₺ */,
    [0x27] = '`', [0x2B] = '`', [0x56] = '|'
};

static const uint32_t kbd_en_normal[128] = {
    [0x01] = 0x1Bu,
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=', [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '[', [0x1B] = ']', [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
    [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm',
    [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x39] = ' ', [0x56] = '<'
};

static const uint32_t kbd_en_shift[128] = {
    [0x01] = 0x1Bu,
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
    [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')',
    [0x0C] = '_', [0x0D] = '+', [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T',
    [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P',
    [0x1A] = '{', [0x1B] = '}', [0x1C] = '\n',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G',
    [0x23] = 'H', [0x24] = 'J', [0x25] = 'K', [0x26] = 'L',
    [0x27] = ':', [0x28] = '"', [0x29] = '~',
    [0x2B] = '|',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B',
    [0x31] = 'N', [0x32] = 'M',
    [0x33] = '<', [0x34] = '>', [0x35] = '?',
    [0x39] = ' ', [0x56] = '>'
};

static const uint32_t kbd_en_altgr[128] = {0};

static int s_layout = 0;

static uint32_t kbd_toggle_case(uint32_t cp, int layout) {
    if (layout == 0 || layout == 1) {
        if (cp == 'i') return 0x0130u;       /* i -> İ */
        if (cp == 0x0130u) return 'i';       /* İ -> i */
        if (cp == 0x0131u) return 'I';       /* ı -> I */
        if (cp == 'I') return 0x0131u;       /* I -> ı */
        if (cp == 0x011Fu) return 0x011Eu;   /* ğ <-> Ğ */
        if (cp == 0x011Eu) return 0x011Fu;
        if (cp == 0x00FCu) return 0x00DCu;   /* ü <-> Ü */
        if (cp == 0x00DCu) return 0x00FCu;
        if (cp == 0x015Fu) return 0x015Eu;   /* ş <-> Ş */
        if (cp == 0x015Eu) return 0x015Fu;
        if (cp == 0x00F6u) return 0x00D6u;   /* ö <-> Ö */
        if (cp == 0x00D6u) return 0x00F6u;
        if (cp == 0x00E7u) return 0x00C7u;   /* ç <-> Ç */
        if (cp == 0x00C7u) return 0x00E7u;
        if (cp == 0x00E9u) return 0x00C9u;   /* é <-> É */
        if (cp == 0x00C9u) return 0x00E9u;
    }
    if (cp >= 'a' && cp <= 'z') return cp - 32u;
    if (cp >= 'A' && cp <= 'Z') return cp + 32u;
    return cp;
}

static uint32_t kbd_translate(int layout, uint8_t scancode,
                              bool shift, bool altgr, bool caps) {
    if (scancode >= 128) return 0;

    const uint32_t *norm = kbd_trq_normal;
    const uint32_t *shft = kbd_trq_shift;
    const uint32_t *altg = kbd_trq_altgr;

    if (layout == 1) {
        norm = kbd_trf_normal;
        shft = kbd_trf_shift;
        altg = kbd_trf_altgr;
    } else if (layout == 2) {
        norm = kbd_en_normal;
        shft = kbd_en_shift;
        altg = kbd_en_altgr;
    }

    if (altgr) return altg[scancode];

    uint32_t cp = shift ? shft[scancode] : norm[scancode];
    if (cp && caps) cp = kbd_toggle_case(cp, layout);
    return cp;
}

void keyboard_set_layout(int id) {
    s_layout = (id >= 0 && id <= 2) ? id : 0;
}

int keyboard_get_layout(void) { return s_layout; }

/* USB HID'den scancode enjekte et */
void keyboard_usb_push_scancode(uint8_t scancode) {
    int next = (usb_scancode_queue_head + 1) % USB_SCANCODE_QUEUE_SIZE;
    if (next != usb_scancode_queue_tail) {
        usb_scancode_queue[usb_scancode_queue_head] = scancode;
        usb_scancode_queue_head = next;
    }
}

/* USB HID + kuyruk yoklamasi (IRQ1 disindaki dongulerden cagrilir) */
void keyboard_poll(void) {
    usb_hid_poll();
    keyboard_process_usb_queue();
}

/* USB kuyrugundan scancode isle */
void keyboard_process_usb_queue(void) {
    while (usb_scancode_queue_tail != usb_scancode_queue_head) {
        uint8_t scancode = usb_scancode_queue[usb_scancode_queue_tail];
        usb_scancode_queue_tail = (usb_scancode_queue_tail + 1) % USB_SCANCODE_QUEUE_SIZE;

        if (scancode & KEY_RELEASED) {
            scancode &= ~KEY_RELEASED;
            switch (scancode) {
                case 0x2A: case 0x36: shift_pressed = false; break;
                case 0x1D: ctrl_pressed = false; break;
                case 0x38: alt_pressed = false; break;
            }
            continue;
        }

        switch (scancode) {
            case 0x2A: case 0x36: shift_pressed = true;  continue;
            case 0x1D: ctrl_pressed = true;  continue;
            case 0x38: alt_pressed = true;   continue;
            case 0x3A: caps_lock = !caps_lock; continue;
        }

        if (scancode < 128) {
            uint32_t cp = kbd_translate(s_layout, scancode,
                                        shift_pressed, altgr_pressed, caps_lock);
            if (cp) keyboard_push_event_cp(cp, scancode, true);
        }
    }
}

/* Klavye interrupt handler (PS/2 + USB kuyrugu) */
void keyboard_interrupt_handler(void) {
    uint8_t status = inb(KEYBOARD_STATUS_PORT);

    /* IRQ1 kuyruk onceligi yuzunden fare byte'i buraya dusmus olabilir */
    if (status & 0x20) {
        mouse_irq_handler();
        return;
    }
    if (!(status & 0x01)) return;

    uint8_t scancode = inb(KEYBOARD_DATA_PORT);

    keyboard_process_usb_queue();

    if (scancode == 0xE0) {
        extended_byte = true;
        return;
    }

    if (scancode & KEY_RELEASED) {
        scancode &= ~KEY_RELEASED;

        if (extended_byte) {
            extended_byte = false;
            if (scancode == 0x5B || scancode == 0x5C) {
                gui_pressed = false;
            } else if (scancode == 0x38) {
                altgr_pressed = false;
            } else if (scancode == 0x1D) {
                ctrl_pressed = false;
            } else if (scancode == 0x4B || scancode == 0x4D ||
                       scancode == 0x48 || scancode == 0x50) {
                s_hot_guard = 0;
            }
            return;
        }

        if (scancode == 0x0F || scancode == 0x14) s_hot_guard = 0;
        switch (scancode) {
            case 0x2A:
            case 0x36:
                shift_pressed = false;
                break;
            case 0x1D:
                ctrl_pressed = false;
                break;
            case 0x38:
                alt_pressed = false;
                break;
        }
        return;
    }

    if (extended_byte) {
        extended_byte = false;
        switch (scancode) {
            case 0x5B:
            case 0x5C:
                if (!gui_pressed) {
                    gui_pressed = true;
                    keyboard_push_hotkey(30);
                }
                return;
            case 0x38:
                altgr_pressed = true;
                return;
            case 0x1D:
                ctrl_pressed = true;
                return;
            case 0x4B: case 0x4D: case 0x48: case 0x50:
                if (ctrl_pressed && alt_pressed && !s_hot_guard) {
                    s_hot_guard = 1;
                    int hk = scancode == 0x4B ? 20 : scancode == 0x4D ? 21
                           : scancode == 0x48 ? 22 : 23;
                    keyboard_push_hotkey(hk);
                    return;
                }
                if (scancode == 0x48) keyboard_push_event_cp(KBD_KEY_UP, scancode, true);
                else if (scancode == 0x50) keyboard_push_event_cp(KBD_KEY_DOWN, scancode, true);
                else if (scancode == 0x4D) keyboard_push_event_cp(KBD_KEY_RIGHT, scancode, true);
                else keyboard_push_event_cp(KBD_KEY_LEFT, scancode, true);
                return;
            case 0x47:
                keyboard_push_event_cp(KBD_KEY_HOME, scancode, true);
                return;
            case 0x4F:
                keyboard_push_event_cp(KBD_KEY_END, scancode, true);
                return;
            case 0x53:
                keyboard_push_event_cp(KBD_KEY_DEL, scancode, true);
                return;
            case 0x1C:
                keyboard_push_event_cp('\n', scancode, true);
                return;
            case 0x35:
                keyboard_push_event_cp('/', scancode, true);
                return;
            default:
                return;
        }
    }

    switch (scancode) {
        case 0x2A:
        case 0x36:
            shift_pressed = true;
            return;
        case 0x1D:
            ctrl_pressed = true;
            return;
        case 0x38:
            alt_pressed = true;
            return;
        case 0x3A:
            caps_lock = !caps_lock;
            return;
    }

    if (scancode == 0x0F && alt_pressed && !s_hot_guard) {
        s_hot_guard = 1;
        keyboard_push_hotkey(1);
        return;
    }
    if (scancode == 0x14 && ctrl_pressed && alt_pressed && !s_hot_guard) {
        s_hot_guard = 1;
        keyboard_push_hotkey(2);
        return;
    }
    if (scancode == 0x23 && ctrl_pressed && alt_pressed && !s_hot_guard) {
        s_hot_guard = 1;
        keyboard_push_hotkey(9);
        return;
    }
    if (ctrl_pressed && !alt_pressed) {
        if (scancode == 0x2E) { keyboard_push_hotkey(3); return; }  /* Ctrl+C */
        if (scancode == 0x2D) { keyboard_push_hotkey(4); return; }  /* Ctrl+X */
        if (scancode == 0x2F) { keyboard_push_hotkey(5); return; }  /* Ctrl+V */
        if (scancode == 0x1E) { keyboard_push_hotkey(6); return; }  /* Ctrl+A */
        if (scancode == 0x2C) { keyboard_push_hotkey(7); return; }  /* Ctrl+Z */
        if (scancode == 0x15) { keyboard_push_hotkey(8); return; }  /* Ctrl+Y */
    }
    if (ctrl_pressed && alt_pressed && !s_hot_guard) {
        if (scancode == 0x33) {
            s_hot_guard = 1;
            keyboard_push_hotkey(shift_pressed ? 15 : 10);
            return;
        }
        if (scancode == 0x34) {
            s_hot_guard = 1;
            keyboard_push_hotkey(shift_pressed ? 16 : 11);
            return;
        }
        if (scancode == 0x1E) { s_hot_guard = 1; keyboard_push_hotkey(12); return; }
        if (scancode == 0x21) { s_hot_guard = 1; keyboard_push_hotkey(13); return; }
    }

    if (scancode < 128) {
        uint32_t cp = kbd_translate(s_layout, scancode,
                                    shift_pressed, altgr_pressed, caps_lock);
        if (cp) keyboard_push_event_cp(cp, scancode, true);
    }
}

void keyboard_push_char(char c) {
    if (keyboard_buffer_pos < KEYBOARD_BUFFER_SIZE - 1) {
        keyboard_buffer[keyboard_buffer_pos++] = c;
        keyboard_buffer[keyboard_buffer_pos] = '\0';
    }
}

void keyboard_clear_buffer(void) {
    keyboard_buffer_pos = 0;
    keyboard_buffer[0] = '\0';
}

const char* keyboard_get_buffer(void) {
    return keyboard_buffer;
}

void keyboard_init(void) {
    kernel_debug("Klavye driver baslatiliyor (TR-Q UTF-8 layout)...");

    outb(KEYBOARD_COMMAND_PORT, KEYBOARD_CMD_DISABLE);
    ps2_flush();

    outb(KEYBOARD_COMMAND_PORT, KEYBOARD_CMD_READ_CONFIG);
    uint8_t config = 0;
    int config_ok = ps2_wait_buffer();
    if (config_ok) config = inb(KEYBOARD_DATA_PORT);

    if (config_ok) {
        config |= 0x01;
        outb(KEYBOARD_COMMAND_PORT, KEYBOARD_CMD_WRITE_CONFIG);
        outb(KEYBOARD_DATA_PORT, config);
    } else {
        kernel_debug("Klavye config bytei okunamadi; mevcut ayar korundu");
    }

    outb(KEYBOARD_COMMAND_PORT, KEYBOARD_CMD_ENABLE);
    keyboard_clear_buffer();

    isr_install_handler(1, keyboard_interrupt_handler);
    pic_set_mask(1, false);

    kernel_debug("Klavye driver basariyla baslatildi (TR-Q UTF-8, IRQ1)");
}

static bool kbd_check_utf8(int layout, uint8_t sc, bool shift, bool altgr,
                           const char *expected) {
    uint32_t cp = kbd_translate(layout, sc, shift, altgr, false);
    char u8[5];
    kbd_cp_to_utf8(cp, u8);
    int i = 0;
    while (expected[i] || u8[i]) {
        if (u8[i] != expected[i]) return false;
        i++;
    }
    return true;
}

int keyboard_v11_selftest(void) {
    typedef struct {
        uint8_t sc;
        bool shift;
        bool altgr;
        const char *utf8;
    } kbd_vec_t;

    static const kbd_vec_t trq_vecs[] = {
        { 0x1A, false, false, "\xC4\x9F" }, /* ğ */
        { 0x1A, true,  false, "\xC4\x9E" }, /* Ğ */
        { 0x1B, false, false, "\xC3\xBC" }, /* ü */
        { 0x1B, true,  false, "\xC3\x9C" }, /* Ü */
        { 0x27, false, false, "\xC5\x9F" }, /* ş */
        { 0x27, true,  false, "\xC5\x9E" }, /* Ş */
        { 0x17, false, false, "\xC4\xB1" }, /* ı */
        { 0x17, true,  false, "I" },        /* I */
        { 0x28, false, false, "i" },        /* i */
        { 0x28, true,  false, "\xC4\xB0" }, /* İ */
        { 0x33, false, false, "\xC3\xB6" }, /* ö */
        { 0x33, true,  false, "\xC3\x96" }, /* Ö */
        { 0x34, false, false, "\xC3\xA7" }, /* ç */
        { 0x34, true,  false, "\xC3\x87" }, /* Ç */
    };

    static const kbd_vec_t trf_vecs[] = {
        { 0x12, false, false, "\xC4\x9F" }, /* ğ */
        { 0x12, true,  false, "\xC4\x9E" }, /* Ğ */
        { 0x22, false, false, "\xC3\xBC" }, /* ü */
        { 0x22, true,  false, "\xC3\x9C" }, /* Ü */
        { 0x28, false, false, "\xC5\x9F" }, /* ş */
        { 0x28, true,  false, "\xC5\x9E" }, /* Ş */
        { 0x13, false, false, "\xC4\xB1" }, /* ı */
        { 0x13, true,  false, "I" },        /* I */
        { 0x1F, false, false, "i" },        /* i */
        { 0x1F, true,  false, "\xC4\xB0" }, /* İ */
        { 0x2D, false, false, "\xC3\xB6" }, /* ö */
        { 0x2D, true,  false, "\xC3\x96" }, /* Ö */
        { 0x30, false, false, "\xC3\xA7" }, /* ç */
        { 0x30, true,  false, "\xC3\x87" }, /* Ç */
    };

    static const kbd_vec_t trq_altgr[] = {
        { 0x12, false, true, "\xE2\x82\xAC" }, /* € */
        { 0x10, false, true, "@" },
        { 0x0C, false, true, "\\" },
        { 0x0D, false, true, "|" },
        { 0x08, false, true, "{" },
        { 0x09, false, true, "[" },
        { 0x0A, false, true, "]" },
        { 0x0B, false, true, "}" },
        { 0x1B, false, true, "~" },
        { 0x2B, false, true, "`" },
        { 0x04, false, true, "#" },
        { 0x05, false, true, "$" },
        { 0x06, false, true, "\xC2\xBD" },     /* ½ */
    };

    static const kbd_vec_t trf_altgr[] = {
        { 0x20, false, true, "\xE2\x82\xAC" }, /* € */
        { 0x10, false, true, "@" },
        { 0x0C, false, true, "\\" },
        { 0x0D, false, true, "|" },
        { 0x08, false, true, "{" },
        { 0x09, false, true, "[" },
        { 0x0A, false, true, "]" },
        { 0x0B, false, true, "}" },
        { 0x1B, false, true, "~" },
        { 0x2B, false, true, "`" },
        { 0x04, false, true, "#" },
        { 0x05, false, true, "$" },
        { 0x06, false, true, "\xC2\xBD" },     /* ½ */
    };

    bool trq_ok = true;
    for (unsigned i = 0; i < sizeof(trq_vecs) / sizeof(trq_vecs[0]); i++) {
        if (!kbd_check_utf8(0, trq_vecs[i].sc, trq_vecs[i].shift,
                            trq_vecs[i].altgr, trq_vecs[i].utf8)) {
            trq_ok = false;
            break;
        }
    }
    if (kbd_toggle_case('i', 0) != 0x0130u || kbd_toggle_case(0x0131u, 0) != 'I')
        trq_ok = false;

    bool trf_ok = true;
    for (unsigned i = 0; i < sizeof(trf_vecs) / sizeof(trf_vecs[0]); i++) {
        if (!kbd_check_utf8(1, trf_vecs[i].sc, trf_vecs[i].shift,
                            trf_vecs[i].altgr, trf_vecs[i].utf8)) {
            trf_ok = false;
            break;
        }
    }

    bool altgr_ok = true;
    for (unsigned i = 0; i < sizeof(trq_altgr) / sizeof(trq_altgr[0]); i++) {
        if (!kbd_check_utf8(0, trq_altgr[i].sc, trq_altgr[i].shift,
                            trq_altgr[i].altgr, trq_altgr[i].utf8)) {
            altgr_ok = false;
            break;
        }
    }
    for (unsigned i = 0; i < sizeof(trf_altgr) / sizeof(trf_altgr[0]); i++) {
        if (!kbd_check_utf8(1, trf_altgr[i].sc, trf_altgr[i].shift,
                            trf_altgr[i].altgr, trf_altgr[i].utf8)) {
            altgr_ok = false;
            break;
        }
    }

    serial_printf("[kbd11] selftest: TR-Q=%s TR-F=%s AltGr=%s -> %s\n",
                  trq_ok ? "OK" : "FAIL",
                  trf_ok ? "OK" : "FAIL",
                  altgr_ok ? "OK" : "FAIL",
                  (trq_ok && trf_ok && altgr_ok) ? "PASS" : "FAIL");
    return (trq_ok && trf_ok && altgr_ok) ? 0 : -1;
}

void keyboard_test(void) {
    terminal_writestring("Klavye Testi (TR-Q):\n");
    terminal_writestring("--------------------\n");
    terminal_writestring("Lutfen bir seyler yazin (Enter ile bitirin):\n");

    char old_buffer[KEYBOARD_BUFFER_SIZE];
    size_t i;
    for (i = 0; keyboard_buffer[i] != '\0' && i < KEYBOARD_BUFFER_SIZE - 1; i++) {
        old_buffer[i] = keyboard_buffer[i];
    }
    old_buffer[i] = '\0';

    keyboard_clear_buffer();

    while (1) {
        if (keyboard_buffer_pos > 0 && keyboard_buffer[keyboard_buffer_pos - 1] == '\n') {
            keyboard_buffer[keyboard_buffer_pos - 1] = '\0';
            break;
        }
    }

    terminal_writestring("\nYazdiklariniz: ");
    terminal_writestring(keyboard_buffer);
    terminal_writestring("\n\n");

    for (i = 0; old_buffer[i] != '\0' && i < KEYBOARD_BUFFER_SIZE - 1; i++) {
        keyboard_buffer[i] = old_buffer[i];
    }
    keyboard_buffer[i] = '\0';
    keyboard_buffer_pos = 0;
    while (keyboard_buffer[keyboard_buffer_pos]) keyboard_buffer_pos++;
}
