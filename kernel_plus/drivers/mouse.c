/**
 * Arctian PS/2 Mouse Driver
 * Professional implementation with IntelliMouse scroll wheel support
 * IRQ12 (INT 44) - VESA framebuffer cursor
 *
 * Protocol:
 *   Standard PS/2 mouse: 3-byte packets
 *   IntelliMouse:        4-byte packets (byte 3 = scroll wheel delta)
 *
 * USB HID bridge: mouse_usb_move() bypasses PS/2 hardware path
 *
 * References:
 *   https://wiki.osdev.org/Mouse_Input
 *   https://web.archive.org/web/20090227114421/http://pferrie.byethost22.com/pic/pic_mouse.htm
 */

#include "kernel.h"
#include "isr.h"
#include "pic.h"
#include "mouse.h"
#include "serial.h"
#include "clock.h"
#include "i2c_hid.h"

/* USB HID fare state (PS/2 donanimina bagli olmadan calisir) */
static int usb_mouse_dx = 0;
static int usb_mouse_dy = 0;
static int usb_mouse_scroll = 0;
static uint8_t usb_mouse_buttons = 0;
/* USB HID fare verisi en az bir kez geldi mi? Gelmediyse PS/2 durumuna
 * dokunulmaz: aksi halde her IRQ12'de butonlar 0'a cekilip tiklamalar
 * siliniyordu (sol tus basimi aninda kayboluyordu). */
static bool usb_mouse_ever = false;

/* ================================================================== */
/* PS/2 Controller Ports & Commands                                   */
/* ================================================================== */

#define PS2_DATA            0x60
#define PS2_STATUS          0x64
#define PS2_CMD             0x64

/* Controller commands */
#define PS2_CMD_READ_CONFIG     0x20
#define PS2_CMD_WRITE_CONFIG    0x60
#define PS2_CMD_DISABLE_PORT2   0xA7
#define PS2_CMD_ENABLE_PORT2    0xA8
#define PS2_CMD_TEST_PORT2      0xA9

/* Mouse device commands */
#define MOUSE_CMD_RESET         0xFF
#define MOUSE_CMD_RESEND        0xFE
#define MOUSE_CMD_SET_DEFAULTS  0xF6
#define MOUSE_CMD_DISABLE       0xF5
#define MOUSE_CMD_ENABLE        0xF4
#define MOUSE_CMD_SET_SAMPLE    0xF3
#define MOUSE_CMD_SET_RATE      0xF3
#define MOUSE_CMD_GET_ID        0xF2
#define MOUSE_CMD_SET_REMOTE    0xF0
#define MOUSE_CMD_SET_WRAP      0xEE
#define MOUSE_CMD_SET_STREAM    0xEA
#define MOUSE_CMD_STATUS_REQ    0xE9
#define MOUSE_CMD_SET_RES       0xE8

/* Mouse response codes */
#define MOUSE_ACK           0xFA
#define MOUSE_NACK          0xFE
#define MOUSE_ERROR         0xFC
#define MOUSE_SELFTEST_OK   0xAA

/* Default device IDs */
#define MOUSE_ID_STANDARD   0x00
#define MOUSE_ID_INTELLI    0x03
#define MOUSE_ID_INTELLI_5B 0x04

/* Gelismis (dokunmatik yuzey) cihaz kimlikleri */
#define MOUSE_ID_SYNAPTICS  0x47
#define MOUSE_ID_ALPS       0x73
#define MOUSE_ID_ELANTECH   0x04
#define MOUSE_ID_ELANTECH2  0x0E

/* ================================================================== */
/* Packet Types                                                        */
/* ================================================================== */

#define PACKET_3BYTE    3
#define PACKET_4BYTE    4
#define PACKET_6BYTE    6   /* x/y ust bitleri + Z basinc + tekerlek */

/* Basinc esigi (0..255) ve tap icin ust sure siniri (ms) */
#define MOUSE_TAP_PRESSURE  40
#define MOUSE_TAP_MAX_MS    220

/* Taninan cihaz ailesi */
typedef enum {
    MOUSE_DEV_UNKNOWN = 0,
    MOUSE_DEV_STANDARD,
    MOUSE_DEV_INTELLI,
    MOUSE_DEV_SYNAPTICS,
    MOUSE_DEV_ALPS,
    MOUSE_DEV_ELANTECH
} mouse_device_t;

/* ================================================================== */
/* Timeouts & Delays                                                  */
/* ================================================================== */

#define TIMEOUT_COUNT       100000
#define SHORT_DELAY         50      /* ~50 iterations for short waits */

/* ================================================================== */
/* Module State                                                        */
/* ================================================================== */

static mouse_state_t mouse_state;
static int mouse_packet_size = PACKET_3BYTE;
static int mouse_packet_cycle = 0;
static uint8_t mouse_packet[6];     /* en fazla 6-bayt gelismis paket */
static mouse_device_t mouse_device = MOUSE_DEV_UNKNOWN;

/*
 * Screen dimensions (default VESA 1024x768, updated by vesa_init)
 * Defined globally so VESA/framebuffer code can set them.
 */
int mouse_screen_width  = 1024;
int mouse_screen_height = 768;

/* ================================================================== */
/* Low-Level PS/2 Controller Helpers                                   */
/* ================================================================== */

/**
 * Wait for the PS/2 controller input buffer to be empty.
 * Returns 0 on success, -1 on timeout.
 */
static int ps2_wait_write(void) {
    int timeout = TIMEOUT_COUNT;
    while (timeout--) {
        if ((inb(PS2_STATUS) & 0x02) == 0)
            return 0;
    }
    return -1;
}

/**
 * Wait for the PS/2 controller output buffer to be full (data ready).
 * Returns 0 on success, -1 on timeout.
 */
static int ps2_wait_read(void) {
    int timeout = TIMEOUT_COUNT;
    while (timeout--) {
        if (inb(PS2_STATUS) & 0x01)
            return 0;
    }
    return -1;
}

/**
 * Write a byte to the PS/2 data port (after verifying buffer is empty).
 * Returns 0 on success, -1 on timeout.
 */
static int ps2_write_data(uint8_t data) {
    if (ps2_wait_write() != 0)
        return -1;
    outb(PS2_DATA, data);
    return 0;
}

/**
 * Write a command byte to the PS/2 command port (after verifying buffer is empty).
 * Returns 0 on success, -1 on timeout.
 */
static int ps2_write_cmd(uint8_t cmd) {
    if (ps2_wait_write() != 0)
        return -1;
    outb(PS2_CMD, cmd);
    return 0;
}

/**
 * Read a data byte from the PS/2 data port (after verifying data is ready).
 * Returns 0 on success with value in *data, -1 on timeout.
 */
static int ps2_read_data(uint8_t *data) {
    if (ps2_wait_read() != 0)
        return -1;
    *data = inb(PS2_DATA);
    return 0;
}

/**
 * Flush any stale data from the PS/2 output buffer.
 * Reads all available bytes without waiting.
 */
static void ps2_flush_buffer(void) {
    int timeout = SHORT_DELAY;
    while (timeout--) {
        if (inb(PS2_STATUS) & 0x01) {
            inb(PS2_DATA);  /* discard byte */
        } else {
            break;
        }
    }
}

/* ================================================================== */
/* Mouse-Specific Low-Level Helpers                                    */
/* ================================================================== */

/**
 * Send a command to the mouse device via the PS/2 controller's port 2.
 * The sequence is: tell controller (0xD4) -> send byte -> wait for ACK.
 * Returns 0 on ACK, -1 on timeout, -2 on unexpected response.
 */
static int mouse_send_cmd(uint8_t cmd) {
    /* Tell the controller we want to talk to the mouse (port 2) */
    if (ps2_write_cmd(0xD4) != 0)
        return -1;
    /* Write the actual command byte */
    if (ps2_write_data(cmd) != 0)
        return -1;
    /* Wait for ACK (0xFA) */
    uint8_t resp;
    int timeout = TIMEOUT_COUNT;
    while (timeout--) {
        if (ps2_read_data(&resp) == 0) {
            if (resp == MOUSE_ACK)
                return 0;
            if (resp == MOUSE_NACK)
                return -2;   /* NACK: retry */
            return -3;        /* unexpected response */
        }
    }
    return -1;  /* timeout */
}

/**
 * Wait for a specific response byte from the mouse.
 * Returns 0 if expected byte received, -1 otherwise.
 */
static int mouse_expect_response(uint8_t expected) {
    uint8_t data;
    int timeout = TIMEOUT_COUNT;
    while (timeout--) {
        if (ps2_read_data(&data) == 0) {
            if (data == expected)
                return 0;
        }
    }
    return -1;
}

/* ================================================================== */
/* Detection: IntelliMouse (scroll wheel) Protocol                    */
/* ================================================================== */

/**
 * Detect and enable IntelliMouse scroll wheel support.
 *
 * The magic sequence to switch a scroll-wheel mouse to 4-byte mode:
 *   Set sample rate to 200 -> Set sample rate to 100 -> Set sample rate to 80
 *   Then read ID; if ID == 0x03, the mouse supports scroll wheel.
 *
 * After detection, we set a high sample rate (100Hz) and 4 counts/mm resolution.
 */
static bool mouse_detect_intellimouse(void) {
    /*
     * The IntelliMouse detection magic:
     *   200 Hz -> 100 Hz -> 80 Hz
     * After this sequence, Get_ID returns 0x03 instead of 0x00.
     */
    mouse_send_cmd(MOUSE_CMD_SET_SAMPLE);
    mouse_send_cmd(200);
    mouse_send_cmd(MOUSE_CMD_SET_SAMPLE);
    mouse_send_cmd(100);
    mouse_send_cmd(MOUSE_CMD_SET_SAMPLE);
    mouse_send_cmd(80);

    /* Read device ID */
    if (mouse_send_cmd(MOUSE_CMD_GET_ID) != 0)
        return false;

    uint8_t id;
    if (ps2_read_data(&id) != 0)
        return false;

    /* 0x03 = standard IntelliMouse, 0x04 = 5-button IntelliMouse */
    return (id == MOUSE_ID_INTELLI || id == MOUSE_ID_INTELLI_5B);
}

/* ================================================================== */
/* Device Identification: Synaptics / ALPS / Elantech                 */
/* ================================================================== */

/**
 * Get_ID komutunu gonderip cihazin kimlik baytini okur.
 * Basarisizlikta 0xFF doner (gecerli bir kimlik degil).
 */
static uint8_t mouse_read_device_id(void) {
    uint8_t id;
    if (mouse_send_cmd(MOUSE_CMD_GET_ID) != 0)
        return 0xFF;
    if (ps2_read_data(&id) != 0)
        return 0xFF;
    return id;
}

/**
 * Ornekleme hizini ayarlar; ACK disinda bir sey beklenmez.
 * Basarili olursa 0 doner.
 */
static int mouse_set_rate(uint8_t rate) {
    if (mouse_send_cmd(MOUSE_CMD_SET_RATE) != 0)
        return -1;
    return mouse_send_cmd(rate);
}

/**
 * Synaptics "magic knock": arka arkaya 4 kez 20 Hz ornekleme hizi,
 * ardindan Get_ID. Dokunmatik yuzeyler 0x47 ile kimlik verir.
 */
static bool mouse_prob_synaptics(void) {
    for (int i = 0; i < 4; i++) {
        if (mouse_set_rate(20) != 0)
            return false;
    }
    return mouse_read_device_id() == MOUSE_ID_SYNAPTICS;
}

/**
 * ALPS: 200 Hz (0xC8) ornekleme hizindan sonra Get_ID -> 0x73.
 */
static bool mouse_prob_alps(void) {
    if (mouse_set_rate(0xC8) != 0)
        return false;
    return mouse_read_device_id() == MOUSE_ID_ALPS;
}

/**
 * Elantech: 10 Hz (0x0A) ornekleme hizindan sonra Get_ID -> 0x04/0x0E.
 */
static bool mouse_prob_elantech(void) {
    if (mouse_set_rate(0x0A) != 0)
        return false;
    uint8_t id = mouse_read_device_id();
    return id == MOUSE_ID_ELANTECH || id == MOUSE_ID_ELANTECH2;
}

/**
 * Synaptics ozel komut argumani gonderir:
 * 4 kez Set Resolution (0xE8) ile 8 bit MSB'den LSB'ye 2'ser bit kodlanir.
 */
static bool synaptics_send_arg(uint8_t arg) {
    for (int shift = 6; shift >= 0; shift -= 2) {
        if (mouse_send_cmd(MOUSE_CMD_SET_RES) != 0)
            return false;
        if (mouse_send_cmd((uint8_t)((arg >> shift) & 0x03u)) != 0)
            return false;
    }
    return true;
}

/**
 * Synaptics'i mutlak (absolute) 6-bayt moduna alir.
 * Gercek Synaptics PS/2 protokolu:
 *   1) Query capabilities (0x02) -> Set Resolution (0xE8) x4 + Status Request (0xE9)
 *   2) Set Mode (0x81 = Absolute + W-mode, destek yoksa 0x80) -> Set Resolution (0xE8) x4
 *   3) Set Sample Rate 20 (0xF3 0x14) ile modu uygula.
 */
static bool mouse_synaptics_absolute(void) {
    uint8_t caps[3] = {0, 0, 0};
    uint8_t mode = 0x80u;

    if (synaptics_send_arg(0x02u) && mouse_send_cmd(MOUSE_CMD_STATUS_REQ) == 0) {
        bool ok = true;
        for (int i = 0; i < 3; i++) {
            if (ps2_read_data(&caps[i]) != 0) {
                ok = false;
                break;
            }
        }
        /* capExtended (byte0 bit7) veya capMultiFinger (byte2 bit1) varsa W-mode ac */
        if (ok && ((caps[0] & 0x80u) || (caps[2] & 0x02u)))
            mode = 0x81u;
    }

    if (!synaptics_send_arg(mode))
        return false;
    if (mouse_set_rate(20) != 0)
        return false;

    ps2_flush_buffer();
    return true;
}

/**
 * Cihaz ailesini tespit eder ve seri porta raporlar.
 *
 * Sirayla Synaptics knock, ALPS ve Elantech yoklamalari denenir; hicbiri
 * tutmazsa cihaz standart/IntelliMouse kabul edilir. Yoklamalar ornekleme
 * hizini degistirdigi icin sonunda SET_DEFAULTS ile temiz duruma donulur
 * (Synaptics mutlak modu basariyla kurulduysa bu adim atlanir).
 */
static void mouse_identify_device(void) {
    ps2_flush_buffer();

    if (mouse_prob_synaptics()) {
        mouse_device = MOUSE_DEV_SYNAPTICS;
        if (mouse_synaptics_absolute()) {
            mouse_packet_size = PACKET_6BYTE;
            serial_printf("[ps2] synaptics dokunmatik yuzey (id=0x%x), "
                          "6-bayt mutlak mod\n", MOUSE_ID_SYNAPTICS);
            return;     /* SET_DEFAULTS mutlak modu bozar: dokunma */
        }
        serial_printf("[ps2] synaptics dokunmatik yuzey (id=0x%x), "
                      "mutlak mod yok: 3/4-bayt\n", MOUSE_ID_SYNAPTICS);
        mouse_send_cmd(MOUSE_CMD_SET_DEFAULTS);
        ps2_flush_buffer();
        return;
    }

    ps2_flush_buffer();
    if (mouse_prob_alps()) {
        mouse_device = MOUSE_DEV_ALPS;
        serial_printf("[ps2] alps dokunmatik yuzey (id=0x%x), 3/4-bayt\n",
                      MOUSE_ID_ALPS);
        mouse_send_cmd(MOUSE_CMD_SET_DEFAULTS);
        ps2_flush_buffer();
        return;
    }

    ps2_flush_buffer();
    if (mouse_prob_elantech()) {
        mouse_device = MOUSE_DEV_ELANTECH;
        serial_printf("[ps2] elantech dokunmatik yuzey (id=0x%x/0x%x), "
                      "3/4-bayt\n", MOUSE_ID_ELANTECH, MOUSE_ID_ELANTECH2);
        mouse_send_cmd(MOUSE_CMD_SET_DEFAULTS);
        ps2_flush_buffer();
        return;
    }

    mouse_device = MOUSE_DEV_STANDARD;
    serial_printf("[ps2] gelismis kimlik yok: standart PS/2 fare, "
                  "3/4-bayt yol\n");
    mouse_send_cmd(MOUSE_CMD_SET_DEFAULTS);
    ps2_flush_buffer();
}

/* ================================================================== */
/* USB HID Mouse Bridge API                                           */
/* ================================================================== */

void mouse_usb_move(int dx, int dy, uint8_t buttons, int scroll) {
    usb_mouse_ever = true;
    usb_mouse_dx += dx;
    usb_mouse_dy += dy;
    usb_mouse_scroll += scroll;
    usb_mouse_buttons = buttons;
}

/* Sol tus basma kenari sayaci: yavas kare hizinda (TCG) kisa tiklamalar
 * yoklama arasina dusup kayboluyordu; masaustu bu sayacla telafi eder. */
static volatile uint32_t s_left_press_seq = 0;
static bool s_left_prev = false;

static void mouse_track_left(bool left) {
    if (left && !s_left_prev) s_left_press_seq++;
    s_left_prev = left;
}

uint32_t mouse_left_press_seq(void) { return s_left_press_seq; }

/* Kaydirma: son okumadan bu yana biriken miktar (mouse_scroll_delta()).
 * USB ve PS/2 tekerlek yollarinin ikisi de buraya besler. */
static volatile int s_scroll_delta = 0;

/* Dokunma (tap): basinc esigini asan kisa basislar sayilir. */
static volatile uint32_t s_tap_count = 0;
static bool s_pressure_prev = false;
static uint64_t s_pressure_down_ms = 0;

static void mouse_track_pressure(int z) {
    bool pressed = z >= MOUSE_TAP_PRESSURE;
    if (pressed && !s_pressure_prev) {
        s_pressure_down_ms = clock_ms();
    } else if (!pressed && s_pressure_prev) {
        if (clock_ms() - s_pressure_down_ms <= MOUSE_TAP_MAX_MS)
            s_tap_count++;
    }
    s_pressure_prev = pressed;
}

static void mouse_process_usb(void) {
    if (!usb_mouse_ever) return;   /* USB fare yok: PS/2 durumunu bozma */
    if (usb_mouse_dx == 0 && usb_mouse_dy == 0 && usb_mouse_scroll == 0 &&
        usb_mouse_buttons == mouse_state.buttons[0] + (mouse_state.buttons[1] << 1) + (mouse_state.buttons[2] << 2))
        return;

    mouse_state.dx = usb_mouse_dx;
    mouse_state.dy = usb_mouse_dy;

    mouse_state.x += usb_mouse_dx;
    mouse_state.y -= usb_mouse_dy;

    if (mouse_state.x < 0) mouse_state.x = 0;
    if (mouse_state.x >= mouse_screen_width) mouse_state.x = mouse_screen_width - 1;
    if (mouse_state.y < 0) mouse_state.y = 0;
    if (mouse_state.y >= mouse_screen_height) mouse_state.y = mouse_screen_height - 1;

    mouse_state.buttons[MOUSE_BUTTON_LEFT]   = (usb_mouse_buttons & 0x01) != 0;
    mouse_state.buttons[MOUSE_BUTTON_RIGHT]  = (usb_mouse_buttons & 0x02) != 0;
    mouse_state.buttons[MOUSE_BUTTON_MIDDLE] = (usb_mouse_buttons & 0x04) != 0;
    mouse_track_left(mouse_state.buttons[MOUSE_BUTTON_LEFT]);

    s_scroll_delta += usb_mouse_scroll;
    mouse_state.scroll += usb_mouse_scroll;
    usb_mouse_dx = 0;
    usb_mouse_dy = 0;
    usb_mouse_scroll = 0;
}

/* ================================================================== */
/* IRQ12 Interrupt Handler                                             */
/* ================================================================== */

/**
 * Gercek Synaptics 6-bayt mutlak (absolute + W-mode) paketi cozer:
 *   p[0] : bit7=1, bit6=0, bit5..4=W[3:2], bit3=0, bit2=W[1], bit1=R, bit0=L
 *   p[1] : bit7..4=Y[11:8], bit3..0=X[11:8]
 *   p[2] : Z basinc (0..255)
 *   p[3] : bit7..6=11, bit5=Y[12], bit4=X[12], bit2=W[0]
 *   p[4] : X[7:0]
 *   p[5] : Y[7:0]
 *
 * 13-bit mutlak X/Y koordinatlarini yumusak goreceli delta'ya cevirir;
 * W=0/1 (2+ parmak) algilandiginda dikey iki-parmak kaydirma uretir.
 */
static void mouse_decode_packet6(void) {
    static bool syn_touching = false;
    static bool syn_two_finger = false;
    static int  syn_prev_x = 0;
    static int  syn_prev_y = 0;
    static int  syn_prev_scroll_y = 0;
    static int  syn_acc_dx = 0;
    static int  syn_acc_dy = 0;
    static int  syn_acc_scroll = 0;

    const uint8_t *p = mouse_packet;
    int x = (int)p[4] | ((int)(p[1] & 0x0Fu) << 8) | ((int)(p[3] & 0x10u) << 8);
    int y = (int)p[5] | ((int)(p[1] & 0xF0u) << 4) | ((int)(p[3] & 0x20u) << 7);
    int z = (int)p[2];
    int w = ((p[0] & 0x30u) >> 2) | ((p[0] & 0x04u) >> 1) | ((p[3] & 0x04u) >> 2);

    mouse_state.buttons[MOUSE_BUTTON_LEFT]   = (p[0] & 0x01u) != 0;
    mouse_state.buttons[MOUSE_BUTTON_RIGHT]  = (p[0] & 0x02u) != 0;
    mouse_state.buttons[MOUSE_BUTTON_MIDDLE] = false;
    mouse_track_left(mouse_state.buttons[MOUSE_BUTTON_LEFT]);
    mouse_track_pressure(z);

    if (z < 10) {
        syn_touching = false;
        syn_two_finger = false;
        syn_acc_dx = 0;
        syn_acc_dy = 0;
        syn_acc_scroll = 0;
        mouse_state.dx = 0;
        mouse_state.dy = 0;
        return;
    }

    bool two_f = (z >= 15) && (w == 0 || w == 1);
    if (!syn_touching || two_f != syn_two_finger) {
        syn_touching = true;
        syn_two_finger = two_f;
        syn_prev_x = x;
        syn_prev_y = y;
        syn_prev_scroll_y = y;
        syn_acc_dx = 0;
        syn_acc_dy = 0;
        syn_acc_scroll = 0;
        mouse_state.dx = 0;
        mouse_state.dy = 0;
        return;
    }

    if (two_f) {
        int dy_raw = y - syn_prev_scroll_y;
        syn_prev_scroll_y = y;
        if (dy_raw > -1500 && dy_raw < 1500) {
            syn_acc_scroll += dy_raw;
            int steps = syn_acc_scroll / 120;
            syn_acc_scroll -= steps * 120;
            if (steps != 0) {
                s_scroll_delta += steps;
                mouse_state.scroll += steps;
            }
        }
        mouse_state.dx = 0;
        mouse_state.dy = 0;
        return;
    }

    int rx = x - syn_prev_x;
    int ry = y - syn_prev_y;
    syn_prev_x = x;
    syn_prev_y = y;

    if (rx < -1500 || rx > 1500) rx = 0;
    if (ry < -1500 || ry > 1500) ry = 0;

    syn_acc_dx += rx;
    syn_acc_dy += ry;
    int dx = syn_acc_dx / 4;
    int dy = syn_acc_dy / 4;
    syn_acc_dx -= dx * 4;
    syn_acc_dy -= dy * 4;

    mouse_state.dx = dx;
    mouse_state.dy = dy;

    mouse_state.x += dx;
    mouse_state.y -= dy;

    if (mouse_state.x < 0) mouse_state.x = 0;
    if (mouse_state.x >= mouse_screen_width)
        mouse_state.x = mouse_screen_width - 1;
    if (mouse_state.y < 0) mouse_state.y = 0;
    if (mouse_state.y >= mouse_screen_height)
        mouse_state.y = mouse_screen_height - 1;
}

/**
 * Decode and process a completed 3-byte or 4-byte mouse packet.
 * Called at the end of packet assembly.
 */
static void mouse_decode_packet(void) {
    if (mouse_packet_size == PACKET_6BYTE) {
        mouse_decode_packet6();
        return;
    }

    uint8_t status_byte = mouse_packet[0];
    int dx, dy;

    /* Movement deltas (sign-extend from 8-bit) */
    dx = (int)(int8_t)mouse_packet[1];
    dy = (int)(int8_t)mouse_packet[2];

    /* Overflow detection: if overflow bits set, use max/min values */
    if (status_byte & 0x40) dx = (dx > 0) ? 127 : -128;
    if (status_byte & 0x80) dy = (dy > 0) ? 127 : -128;

    /* Store deltas for polling consumers */
    mouse_state.dx = dx;
    mouse_state.dy = dy;

    /* Update absolute position (Y inverted: screen Y grows downward) */
    mouse_state.x += dx;
    mouse_state.y -= dy;

    /* Clamp to screen bounds */
    if (mouse_state.x < 0) mouse_state.x = 0;
    if (mouse_state.x >= mouse_screen_width)
        mouse_state.x = mouse_screen_width - 1;
    if (mouse_state.y < 0) mouse_state.y = 0;
    if (mouse_state.y >= mouse_screen_height)
        mouse_state.y = mouse_screen_height - 1;

    /* Button states */
    mouse_state.buttons[MOUSE_BUTTON_LEFT]   = (status_byte & 0x01) != 0;
    mouse_state.buttons[MOUSE_BUTTON_RIGHT]  = (status_byte & 0x02) != 0;
    mouse_state.buttons[MOUSE_BUTTON_MIDDLE] = (status_byte & 0x04) != 0;
    mouse_track_left(mouse_state.buttons[MOUSE_BUTTON_LEFT]);

    if (mouse_packet_size == PACKET_4BYTE) {
        int wheel = (int)(int8_t)mouse_packet[3];
        s_scroll_delta += wheel;
        mouse_state.scroll += wheel;
    }
}

/**
 * IRQ12 handler - called on each mouse interrupt.
 * Assembles bytes into 3- or 4-byte packets, then decodes.
 * Also processes USB HID mouse data queued via mouse_usb_move().
 */
void mouse_irq_handler(void) {
    // USB HID fare verisini işle (PS/2 olsa da olmasa da çalışır)
    mouse_process_usb();

    uint8_t status = inb(PS2_STATUS);

    if (!(status & 0x01))
        return;

    uint8_t data = inb(PS2_DATA);
    mouse_feed_byte(data);
}

/* PS/2 baytini paket durum makinesine verir. Hem IRQ12 hem de yoklama
 * (mouse_poll) yolu ayni makineyi kullanir. */
void mouse_feed_byte(uint8_t data) {
    bool syn6 = (mouse_device == MOUSE_DEV_SYNAPTICS && mouse_packet_size == PACKET_6BYTE);

    switch (mouse_packet_cycle) {
        case 0:
            if (syn6) {
                /* Synaptics absolute byte 0: bit7=1, bit6=0, bit3=0 */
                if ((data & 0xC8u) != 0x80u) return;
            } else {
                /* Standart PS/2 byte 0: bit3=1 */
                if (!(data & 0x08u)) return;
            }
            mouse_packet[0] = data;
            mouse_packet_cycle = 1;
            break;

        case 1:
            mouse_packet[1] = data;
            mouse_packet_cycle = 2;
            break;

        case 2:
            mouse_packet[2] = data;

            if (mouse_packet_size > PACKET_3BYTE) {
                mouse_packet_cycle = 3;  /* 4. ya da 6. bayta kadar devam */
            } else {
                mouse_packet_cycle = 0;
                mouse_decode_packet();
            }
            break;

        case 3:     /* 4. bayt (tekerlek) veya Synaptics 6-baytlik paketin 4. bayti */
            if (syn6 && (data & 0xC0u) != 0xC0u) {
                mouse_packet_cycle = 0;
                return;
            }
            mouse_packet[3] = data;
            mouse_packet_cycle++;
            if (mouse_packet_cycle >= mouse_packet_size) {
                mouse_packet_cycle = 0;
                mouse_decode_packet();
            }
            break;

        case 4:
        case 5:
            mouse_packet[mouse_packet_cycle] = data;
            mouse_packet_cycle++;
            if (mouse_packet_cycle >= mouse_packet_size) {
                mouse_packet_cycle = 0;
                mouse_decode_packet();
            }
            break;

        default:
            mouse_packet_cycle = 0;
            break;
    }
}

void mouse_poll(void) {
    if (i2c_hid_present()) {
        int dx = 0, dy = 0, scroll = 0;
        uint8_t btns = 0;
        if (i2c_hid_poll_event(&dx, &dy, &btns, &scroll)) {
            mouse_state.dx = dx;
            mouse_state.dy = dy;
            mouse_state.x += dx;
            mouse_state.y -= dy;
            if (mouse_state.x < 0) mouse_state.x = 0;
            if (mouse_state.x >= mouse_screen_width) mouse_state.x = mouse_screen_width - 1;
            if (mouse_state.y < 0) mouse_state.y = 0;
            if (mouse_state.y >= mouse_screen_height) mouse_state.y = mouse_screen_height - 1;
            mouse_state.buttons[MOUSE_BUTTON_LEFT] = (btns & 0x01u) != 0;
            mouse_state.buttons[MOUSE_BUTTON_RIGHT] = (btns & 0x02u) != 0;
            mouse_state.buttons[MOUSE_BUTTON_MIDDLE] = (btns & 0x04u) != 0;
            mouse_track_left(mouse_state.buttons[MOUSE_BUTTON_LEFT]);
            if (scroll != 0) {
                s_scroll_delta += scroll;
                mouse_state.scroll += scroll;
            }
        }
    }

    if (!mouse_state.present) return;
    int guard = 64;
    while (guard-- > 0) {
        uint8_t st = inb(PS2_STATUS);
        if (!(st & 0x01)) break;
        if (!(st & 0x20)) break;      /* AUX degil: klavye bayti, dokunma */
        uint8_t d = inb(PS2_DATA);
        mouse_feed_byte(d);
    }
    mouse_process_usb();
}

/* ================================================================== */
/* Initialization                                                      */
/* ================================================================== */

/**
 * Initialize the PS/2 mouse.
 *
 * Sequence:
 *   1. Flush stale buffer data
 *   2. Enable the auxiliary device (PS/2 port 2)
 *   3. Read + modify + write controller config (enable IRQ12, clock)
 *   4. Reset mouse, verify self-test result
 *   5. Set default settings
 *   6. Detect IntelliMouse (scroll wheel)
 *   7. Set resolution and sample rate
 *   8. Enable data reporting
 *   9. Unmask PIC IRQ12, register handler
 */
void mouse_init(void) {
    kernel_debug("PS/2 mouse baslatiliyor...");

    /* ----- Reset state ----- */
    mouse_state.x = mouse_screen_width / 2;
    mouse_state.y = mouse_screen_height / 2;
    mouse_state.dx = 0;
    mouse_state.dy = 0;
    mouse_state.scroll = 0;
    mouse_state.buttons[0] = false;
    mouse_state.buttons[1] = false;
    mouse_state.buttons[2] = false;
    mouse_state.present = false;
    mouse_state.scroll_wheel = false;
    mouse_packet_cycle = 0;
    mouse_packet_size = PACKET_3BYTE;
    mouse_device = MOUSE_DEV_UNKNOWN;
    s_scroll_delta = 0;
    s_pressure_prev = false;
    s_tap_count = 0;

    /*
     * Step 1: Flush any stale data from the PS/2 buffer.
     * This prevents garbage bytes from corrupting our protocol.
     */
    ps2_flush_buffer();

    /* Step 2: Enable the auxiliary device (PS/2 port 2 / mouse) */
    ps2_write_cmd(PS2_CMD_ENABLE_PORT2);
    ps2_flush_buffer();

    /* Step 3: Read current controller configuration */
    ps2_write_cmd(PS2_CMD_READ_CONFIG);
    uint8_t config;
    if (ps2_read_data(&config) != 0) {
        kernel_warning("PS/2 mouse: config okunamadi!");
        if (i2c_hid_init()) {
            mouse_state.present = true;
            mouse_state.scroll_wheel = true;
        }
        return;
    }

    /*
     * Enable IRQ12 for port 2 (bit 1), enable clock (clear bit 5).
     *
     * PS/2 config register bits:
     *   bit 0: Port 1 IRQ enable
     *   bit 1: Port 2 IRQ enable (mouse)
     *   bit 5: Port 2 clock disable (0 = enable clock)
     */
    config |= 0x02;     /* Enable IRQ12 */
    config &= ~0x20;    /* Enable clock for port 2 */

    ps2_write_cmd(PS2_CMD_WRITE_CONFIG);
    ps2_write_data(config);

    /* Step 4: Reset the mouse */
    int ret;
    ret = mouse_send_cmd(MOUSE_CMD_RESET);

    /*
     * IMPORTANT: After reset, the mouse sends:
     *   0xFA (ACK)  - handled by mouse_send_cmd
     *   0xAA (self-test passed)
     *   0x00 (device ID = standard PS/2 mouse)
     *
     * We must read both remaining bytes to keep the protocol in sync.
     */
    if (ret != 0) {
        kernel_warning("PS/2 mouse: reset basarisiz!");
        if (i2c_hid_init()) {
            mouse_state.present = true;
            mouse_state.scroll_wheel = true;
        }
        return;
    }

    uint8_t self_test_result;
    uint8_t device_id;

    if (ps2_read_data(&self_test_result) != 0 ||
        ps2_read_data(&device_id) != 0) {
        kernel_warning("PS/2 mouse: reset yaniti okunamadi!");
        return;
    }

    if (self_test_result != MOUSE_SELFTEST_OK) {
        kernel_warning("PS/2 mouse: self-test basarisiz!");
        return;
    }

    kernel_debug("PS/2 mouse: self-test OK, device ID = 0x00");

    /* Step 5: Set default settings (disable, 1:1 scaling, 4 counts/mm) */
    mouse_send_cmd(MOUSE_CMD_SET_DEFAULTS);

    /*
     * Step 5b: Gelismis cihaz kimligi (Synaptics/ALPS/Elantech).
     * Basarili bir Synaptics mutlak modu PACKET_6BYTE'a gecirir ve
     * ardindan gelen IntelliMouse yoklamasi atlanir.
     */
    mouse_identify_device();

    /* Step 6: Detect IntelliMouse scroll wheel (6-bayt modda gerekmez) */
    if (mouse_packet_size == PACKET_6BYTE) {
        mouse_state.scroll_wheel = true;
        kernel_debug("PS/2 mouse: 6-bayt gelismis paket modu");
    } else if (mouse_detect_intellimouse()) {
        mouse_packet_size = PACKET_4BYTE;
        mouse_state.scroll_wheel = true;
        mouse_device = MOUSE_DEV_INTELLI;
        kernel_debug("PS/2 mouse: IntelliMouse scroll wheel detected");
    } else {
        mouse_packet_size = PACKET_3BYTE;
        kernel_debug("PS/2 mouse: standard 3-button mode");
    }

    /*
     * Step 7: Set resolution and sample rate.
     * Resolution code: 0=1, 1=2, 2=4, 3=8 counts/mm
     * We use 4 counts/mm (code 2) for reasonable sensitivity.
     *
     * IMPORTANT: The sample rate must be set AFTER detection,
     * because the detection sequence changes the sample rate.
     */
    if (mouse_packet_size != PACKET_6BYTE) {
        mouse_send_cmd(MOUSE_CMD_SET_SAMPLE);
        mouse_send_cmd(100);        /* 100 Hz sample rate */

        mouse_send_cmd(MOUSE_CMD_SET_RES);
        mouse_send_cmd(2);          /* 4 counts/mm */
    }

    /* Step 8: Enable data reporting (stream mode) */
    if (mouse_send_cmd(MOUSE_CMD_ENABLE) != 0) {
        kernel_warning("PS/2 mouse: enable basarisiz!");
        return;
    }

    /* Final flush: clear any extra bytes sent during init */
    ps2_flush_buffer();

    /* Step 9: Unmask IRQ12 on PIC and register handler */
    pic_set_mask(2, false);
    pic_set_mask(IRQ_MOUSE, false);
    isr_install_handler(IRQ_MOUSE, mouse_irq_handler);

    mouse_state.present = true;
    kernel_debug("PS/2 mouse basariyla baslatildi (IRQ12)");

    if (i2c_hid_init()) {
        mouse_state.scroll_wheel = true;
    }
}

bool mouse_i2c_hid_present(void) {
    return i2c_hid_present();
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

mouse_state_t* mouse_get_state(void) {
    return &mouse_state;
}

void mouse_set_screen_size(int width, int height) {
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    mouse_screen_width = width;
    mouse_screen_height = height;
    if (mouse_state.x >= mouse_screen_width)  mouse_state.x = mouse_screen_width - 1;
    if (mouse_state.y >= mouse_screen_height) mouse_state.y = mouse_screen_height - 1;
    if (mouse_state.x < 0) mouse_state.x = 0;
    if (mouse_state.y < 0) mouse_state.y = 0;
}

int mouse_scroll_delta(void) {
    return __atomic_exchange_n(&s_scroll_delta, 0, __ATOMIC_RELAXED);
}

int mouse_tap_count(void) {
    return (int)__atomic_load_n(&s_tap_count, __ATOMIC_RELAXED);
}

