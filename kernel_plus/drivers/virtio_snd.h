#ifndef VIRTIO_SND_H
#define VIRTIO_SND_H

#include <stdint.h>
#include <stdbool.h>

int      virtio_snd_init(void);
bool     virtio_snd_present(void);
uint32_t virtio_snd_rate(void);

int      virtio_snd_play_pcm(const void *pcm_s16_stereo, uint32_t bytes, uint32_t sample_rate);
void     virtio_snd_stop(void);
bool     virtio_snd_playing(void);
void     virtio_snd_set_volume(int percent);
void     virtio_snd_poll(void);

#endif /* VIRTIO_SND_H */
