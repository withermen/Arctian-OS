#ifndef ARCTIAN_UI_BROKER_H
#define ARCTIAN_UI_BROKER_H

/*
 * UI broker - LVGL nesneleri ile .wlf uygulamalari arasinda kimlik katmani.
 *
 * Uygulama lv_obj_t* gormez; yalnizca ui_widget_id tasir. Broker, widget
 * kimlik tablosunu ve olay kuyrugunu tek bir spinlock ile korur. Gercek
 * LVGL cagrilari ui_broker_ops_t uzerinden disaridan enjekte edilir; boylece
 * bu dosya LVGL'ye dogrudan baglanmaz ve cekirdek disi testlerde de
 * kullanilabilir.
 *
 * KILIT KURALI: Tum broker durumu tek spinlock altindadir. Kanca (ops)
 * cagrilari kilit BIRAKILDIKTAN sonra yapilir; boylece LVGL callback'i
 * broker'a geri girebilir ve ic ice kilitlenme olmaz. Kanca icinden broker
 * cagirmak serbesttir, fakat kilit tutulurken ops cagrilmamalidir.
 *
 * Kimlik bicimi: (nesil << 12) | (slot + 1). Slot yeniden kullanildiginda
 * nesil artar; eski kimlikler gecersiz olur. UI_WIDGET_INVALID (0) kok
 * ebeveyn yok anlaminda da kullanilir.
 */

#include <stdint.h>

typedef uint32_t ui_widget_id;

#define UI_WIDGET_INVALID       0u
#define UI_BROKER_WIDGET_MAX    1024u
#define UI_BROKER_EVENT_MAX     64u
#define UI_BROKER_APP_MAX       32u

/* Donus kodlari (negatif = hata). */
#define UI_BROKER_OK            0
#define UI_BROKER_ERR_PARAM     (-1)
#define UI_BROKER_ERR_STATE     (-2)
#define UI_BROKER_ERR_FULL      (-3)
#define UI_BROKER_ERR_EMPTY     (-4)
#define UI_BROKER_ERR_NOCAP     (-5)

/* Widget turleri. */
typedef enum {
    UI_WIDGET_ROOT = 0,
    UI_WIDGET_PANEL,
    UI_WIDGET_LABEL,
    UI_WIDGET_BUTTON,
    UI_WIDGET_INPUT,
    UI_WIDGET_LIST,
    UI_WIDGET_DIALOG,
    UI_WIDGET_PROGRESS,
    UI_WIDGET_IMAGE,
    UI_WIDGET_TYPE_MAX
} ui_widget_type_t;

/* Olay turleri. */
typedef enum {
    UI_EVENT_NONE = 0,
    UI_EVENT_CLICK,
    UI_EVENT_VALUE_CHANGED,
    UI_EVENT_FOCUS,
    UI_EVENT_BLUR,
    UI_EVENT_KEY,
    UI_EVENT_CLOSE,
    UI_EVENT_CUSTOM
} ui_event_type_t;

/* Yetenek bitleri (wlf basligindaki FS/NET/EXEC bayraklarindan bagimsiz,
 * UI broker'in kendi yuzeyi). */
#define UI_BROKER_CAP_CREATE    0x00000001u
#define UI_BROKER_CAP_LABEL     0x00000002u
#define UI_BROKER_CAP_BUTTON    0x00000004u
#define UI_BROKER_CAP_INPUT     0x00000008u
#define UI_BROKER_CAP_DESTROY   0x00000010u
#define UI_BROKER_CAP_EVENTS    0x00000020u
#define UI_BROKER_CAP_ALL       0x0000003Fu

/*
 * Cagri kancalari. Hepsi istege baglidir; NULL olan kanca no-op sayilir.
 * ctx, enjeksiyon yapan katmanin baglamidir (ornegin LVGL portu).
 * Donus: 0 basarili, negatif hata. input_get basarida yazilan bayt sayisini
 * dondurur.
 */
typedef struct {
    void *ctx;

    int (*create)(void *ctx, ui_widget_id id, uint32_t type,
                  ui_widget_id parent_id);
    int (*label_set)(void *ctx, ui_widget_id id, const char *text);
    int (*button_set)(void *ctx, ui_widget_id id, const char *text, int pressed);
    int (*input_get)(void *ctx, ui_widget_id id, char *buf, uint32_t cap);
    int (*destroy)(void *ctx, ui_widget_id id);
} ui_broker_ops_t;

/* Kuyruktan alinan olay. a/b anlami olay turune gore degisir (ornegin
 * CLICK icin x/y, KEY icin kod/modifier). */
typedef struct {
    ui_widget_id id;
    uint32_t type;
    int32_t a;
    int32_t b;
} ui_event_t;

/* Durum / yasam dongusu. */
void          ui_broker_init(void);
int           ui_broker_set_ops(const ui_broker_ops_t *ops);

/* Widget kimlik tablosu. */
int           ui_broker_widget_create(uint32_t type, ui_widget_id parent_id,
                                      ui_widget_id *out_id);
int           ui_broker_widget_destroy(ui_widget_id id);
int           ui_broker_widget_valid(ui_widget_id id);
int           ui_broker_widget_info(ui_widget_id id, uint32_t *out_type,
                                    ui_widget_id *out_parent);
uint32_t      ui_broker_widget_count(void);

/* Temel widget kancalari. */
int           ui_broker_label_set(ui_widget_id id, const char *text);
int           ui_broker_button_set(ui_widget_id id, const char *text,
                                   int pressed);
int           ui_broker_input_get(ui_widget_id id, char *buf, uint32_t cap);

/* Olay kuyrugu (halka tampon, UI_BROKER_EVENT_MAX olay).
 * event_wait scheduler cagirmaz; kuyruk bosken timeout_ticks boyunca
 * donerek bekler (1 tick = 1 ms). 0 = hemen don. */
int           ui_broker_event_push(ui_widget_id id, uint32_t ev,
                                   int32_t a, int32_t b);
int           ui_broker_event_wait(ui_event_t *out, uint32_t timeout_ticks);
uint32_t      ui_broker_event_pending(void);
void          ui_broker_event_flush(void);

/* Yetenek denetimi (uygulama basina bit maskesi). */
int           ui_broker_set_caps(uint32_t app_id, uint32_t caps);
uint32_t      ui_broker_get_caps(uint32_t app_id);
int           ui_broker_check_caps(uint32_t app_id, uint32_t cap);
int           ui_broker_clear_caps(uint32_t app_id);

#endif /* ARCTIAN_UI_BROKER_H */
