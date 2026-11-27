#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

typedef enum {
    AUDIO_BACKEND_NONE = 0,
    AUDIO_BACKEND_HDA,
    AUDIO_BACKEND_VIRTIO_SND,
    AUDIO_BACKEND_AC97,
    AUDIO_BACKEND_PCSPK
} audio_backend_t;

int      audio_init(void);

int      audio_present(void);
int      audio_play_wav(const void *data, uint32_t len);
void     audio_stop(void);
int      audio_is_playing(void);
int      audio_set_volume(int percent);
int      audio_volume(void);
void     audio_service(void);
const char *audio_last_error(void);
uint32_t audio_sample_rate(void);

audio_backend_t audio_backend(void);
const char     *audio_backend_name(void);

/* Streaming PCM API (for /dev/dsp, WPE WebKit, games/players) */
int      audio_stream_open(uint32_t rate, int channels, int bits);
int      audio_stream_write(const void *data, uint32_t bytes);
void     audio_stream_close(void);
uint32_t audio_stream_free(void);
uint32_t audio_stream_buffered(void);
int      audio_stream_active(void);
int      audio_power_v11_selftest(void);

#endif /* AUDIO_H */
