/**
 * Arctian Keyboard Driver Header
 * TR-Q / TR-F / EN-Q UTF-8 klavye destegi - event-based API for GUI
 */
#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

/** A single keyboard event for the GUI main loop */
typedef struct {
    char ch;            /* ASCII fallback or control char, 0 if non-printable */
    uint8_t scancode;
    bool pressed;       /* true=press, false=release */
    bool alt;           /* modifier states at time of event */
    bool ctrl;
    bool shift;
    uint32_t codepoint; /* Unicode codepoint or LVGL special key */
    bool altgr;         /* AltGr (Right Alt) state at time of event */
} keyboard_event_t;

/** Pop the next event from the keyboard event queue. Returns false if empty. */
bool keyboard_pop_event(keyboard_event_t *ev);

/** Is there a queued event waiting to be popped? */
bool keyboard_event_avail(void);

/** Pop next Unicode codepoint (0 if empty). */
uint32_t keyboard_pop_cp(void);

/** Pop next keypress as NUL-terminated UTF-8 string. Returns byte length (0 if empty). */
int keyboard_pop_utf8(char out[5]);

/** Pop next keypress as ASCII/control byte (-1 if empty). */
int keyboard_pop(void);

/** Poll USB HID keyboard queue and pump pending scancodes. */
void keyboard_poll(void);

/** Process queued USB HID scancodes (call from poll loops). */
void keyboard_process_usb_queue(void);

/** Push a single character into the legacy polling buffer (for old apps). */
void keyboard_push_char(char c);

/** Clear the legacy keyboard buffer. */
void keyboard_clear_buffer(void);

/** Keyboard translation tables (ASCII fallback) — defined in keyboard.c */
extern const char keyboard_map_normal[128];
extern const char keyboard_map_shift[128];

/** Select the active keyboard layout: 0=TR-Q, 1=TR-F, 2=EN-Q. */
void keyboard_set_layout(int id);

/** Return the active keyboard layout id (0..2). */
int keyboard_get_layout(void);

/** Get pointer to legacy keyboard buffer. */
const char* keyboard_get_buffer(void);

/** Global hotkeys (1=Alt+Tab, 2=Ctrl+Alt+T, 3..8=Ctrl+C/X/V/A/Z/Y). */
void keyboard_push_hotkey(int code);
int  keyboard_pop_hotkey(void);

/** Anlik Ctrl/Shift durumu (coklu secim gibi tiklama aninda gereken durumlar icin). */
int  keyboard_ctrl_down(void);
int  keyboard_shift_down(void);

/** v1.1 self-test for TR-Q, TR-F, and AltGr UTF-8 mapping. Returns 0 on pass. */
int  keyboard_v11_selftest(void);

#endif /* KEYBOARD_H */
