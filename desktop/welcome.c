#include "dsk.h"
#include "version.h"        /* sürüm: version.json -> ARCTIAN_VERSION */
#include "oobe.h"

void build_welcome_window(void) {
    if (!kv_get_int("oobe_done", 0)) {
        oobe_show();
        return;
    }

    lv_obj_t *w = create_window("Arctian'a Hoş Geldiniz", 560, 300);
    lv_obj_t *body = label(w,
        "Arctian'a hoş geldiniz.\n\n"
        "Sizler için özelleştirilmiş sistemi ile,\n"
        "modern ve sade görünümlü Arctian " ARCTIAN_VERSION " artık hazır.\n\n"
        "Bu pencereyi sağ üstteki X ile kapatabilirsiniz.",
        ADL_TEXT_DIM);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 20, 56);
}
