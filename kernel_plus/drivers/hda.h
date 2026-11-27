#ifndef HDA_H
#define HDA_H

#include <stdint.h>
#include <stdbool.h>

typedef uint32_t (*hda_stream_pull_fn)(void *dst, uint32_t max_bytes, void *ctx);

int      hda_init(void);
bool     hda_present(void);
uint32_t hda_rate(void);

int      hda_play_pcm(const void *pcm_s16_stereo, uint32_t bytes, uint32_t sample_rate);
int      hda_stream_start(uint32_t sample_rate, hda_stream_pull_fn pull, void *ctx);
void     hda_stop(void);
bool     hda_playing(void);
void     hda_set_volume(int percent);
void     hda_poll(void);

int      hda_out_pin_count(void);
bool     hda_headphone_plugged(void);
void     hda_poll_jacks(void);

#endif /* HDA_H */
