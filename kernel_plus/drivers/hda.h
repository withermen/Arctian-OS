#ifndef HDA_H
#define HDA_H

#include <stdint.h>
#include <stdbool.h>

int      hda_init(void);
bool     hda_present(void);
uint32_t hda_rate(void);

int      hda_play_pcm(const void *pcm_s16_stereo, uint32_t bytes, uint32_t sample_rate);
void     hda_stop(void);
bool     hda_playing(void);
void     hda_set_volume(int percent);
void     hda_poll(void);

#endif /* HDA_H */
