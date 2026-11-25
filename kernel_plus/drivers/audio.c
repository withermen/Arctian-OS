#include "kernel.h"
#include "audio.h"
#include "hda.h"
#include "virtio_snd.h"
#include "ac97.h"
#include "alloc.h"
#include "clock.h"
#include "serial.h"

#define AUDIO_SLOTS      32
#define AUDIO_SLOT_BYTES 16384
#define AUDIO_RING_BYTES (AUDIO_SLOTS * AUDIO_SLOT_BYTES)

typedef struct {
    uint32_t addr;
    uint32_t ctl;
} __attribute__((packed)) audio_bd_t;

static audio_backend_t s_backend = AUDIO_BACKEND_NONE;
static audio_bd_t *s_bdl;
static uint8_t    *s_ring;
static uint8_t    *s_conv_buf;

static int       s_ready;
static int       s_playing;
static int       s_started;
static const uint8_t *s_src;
static uint32_t  s_src_len;
static uint32_t  s_src_pos;
static int       s_bits;
static int       s_channels;
static uint32_t  s_rate;
static int       s_volume = 80;
static const char *s_err = "";

static int       s_tail;
static int       s_count;
static int       s_open;
static uint32_t  s_open_off;

/* PC Speaker fallback state */
static uint64_t  s_pcspk_start_ms;
static uint32_t  s_pcspk_dur_ms;

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int16_t sample_at(const uint8_t *p, int bits) {
    if (bits == 8) return (int16_t)(((int)p[0] - 128) << 8);
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* ------------------------------------------------------------------ */
/* PC Speaker (PIT ch2) fallback                                      */
/* ------------------------------------------------------------------ */

static void pcspk_off(void) {
    uint8_t v = inb(0x61);
    outb(0x61, (uint8_t)(v & ~0x03u));
}

static void pcspk_tone(uint32_t hz) {
    if (hz < 40u || hz > 12000u || s_volume <= 0) {
        pcspk_off();
        return;
    }
    uint32_t div = 1193182u / hz;
    if (div < 1u) div = 1u;
    if (div > 65535u) div = 65535u;
    outb(0x43, 0xB6u);
    outb(0x42, (uint8_t)(div & 0xFFu));
    outb(0x42, (uint8_t)((div >> 8) & 0xFFu));
    uint8_t v = inb(0x61);
    if ((v & 0x03u) != 0x03u) outb(0x61, (uint8_t)(v | 0x03u));
}

static void pcspk_service(void) {
    if (!s_playing) return;
    uint64_t elapsed = clock_ms() - s_pcspk_start_ms;
    if (elapsed >= s_pcspk_dur_ms) {
        audio_stop();
        return;
    }
    uint32_t frame_bytes = (uint32_t)((s_bits / 8) * s_channels);
    if (!frame_bytes || !s_rate) return;
    uint32_t frame_idx = (uint32_t)((elapsed * s_rate) / 1000u);
    uint32_t byte_off = frame_idx * frame_bytes;
    uint32_t win_frames = s_rate / 50u; /* ~20ms window */
    if (win_frames < 16u) win_frames = 16u;
    if (byte_off + win_frames * frame_bytes > s_src_len) {
        pcspk_off();
        return;
    }
    uint32_t zc = 0;
    int prev_pos = 0;
    for (uint32_t i = 0; i < win_frames; i++) {
        int16_t s = sample_at(s_src + byte_off + i * frame_bytes, s_bits);
        int pos = (s >= 0);
        if (i > 0 && pos != prev_pos) zc++;
        prev_pos = pos;
    }
    uint32_t hz = (zc * s_rate) / (2u * win_frames);
    pcspk_tone(hz);
}

/* ------------------------------------------------------------------ */
/* AC97 streaming helpers                                             */
/* ------------------------------------------------------------------ */

static void program_bd(int idx, uint32_t bytes) {
    s_bdl[idx].addr = (uint32_t)(uintptr_t)(s_ring + (uint32_t)idx * AUDIO_SLOT_BYTES);
    s_bdl[idx].ctl  = bytes / 2u;
}

static uint32_t convert_into(uint8_t *dst, uint32_t max_bytes) {
    uint32_t written = 0;
    int frame_bytes = (s_bits / 8) * s_channels;
    if (frame_bytes <= 0) return 0;

    while (written + 4u <= max_bytes) {
        if (s_src_pos + (uint32_t)frame_bytes > s_src_len) break;
        const uint8_t *f = s_src + s_src_pos;
        int16_t l, r;
        if (s_channels == 2) {
            l = sample_at(f, s_bits);
            r = sample_at(f + s_bits / 8, s_bits);
        } else {
            l = r = sample_at(f, s_bits);
        }
        dst[written + 0] = (uint8_t)(l & 0xFF);
        dst[written + 1] = (uint8_t)((l >> 8) & 0xFF);
        dst[written + 2] = (uint8_t)(r & 0xFF);
        dst[written + 3] = (uint8_t)((r >> 8) & 0xFF);
        written += 4u;
        s_src_pos += (uint32_t)frame_bytes;
    }
    return written;
}

static void ac97_service(void) {
    if (s_started) {
        if (ac97_status() & AC97_SR_DCH) {
            uint8_t civ = ac97_civ();
            s_tail = (int)((civ + 1u) & 31u);
            s_count = 0;
            s_open = s_tail;
            s_open_off = 0;
        } else {
            uint8_t civ = ac97_civ();
            while (s_count > 0 && (uint8_t)s_tail != civ) {
                s_tail = (s_tail + 1) & 31;
                s_count--;
            }
        }
    }

    int committed_new = 0;
    while (s_count < AUDIO_SLOTS) {
        if (s_open_off >= AUDIO_SLOT_BYTES) {
            program_bd(s_open, AUDIO_SLOT_BYTES);
            s_open = (s_open + 1) & 31;
            s_count++;
            s_open_off = 0;
            committed_new = 1;
            continue;
        }
        uint32_t n = convert_into(s_ring + (uint32_t)s_open * AUDIO_SLOT_BYTES + s_open_off,
                                  AUDIO_SLOT_BYTES - s_open_off);
        if (n == 0) {
            if (s_open_off > 0) {
                program_bd(s_open, s_open_off);
                s_open = (s_open + 1) & 31;
                s_count++;
                s_open_off = 0;
                committed_new = 1;
            }
            break;
        }
        s_open_off += n;
    }

    if (!s_started && s_count > 0) {
        ac97_set_lvi((uint8_t)((s_open - 1) & 31));
        ac97_start_dma();
        s_started = 1;
    } else if (s_started && committed_new) {
        ac97_set_lvi((uint8_t)((s_open - 1) & 31));
    }

    if (s_started && s_count == 0 && s_src_pos >= s_src_len &&
        (ac97_status() & AC97_SR_DCH)) {
        s_playing = 0;
        s_started = 0;
        ac97_stop_dma();
    }
}

/* ------------------------------------------------------------------ */
/* Public Unified Audio API                                           */
/* ------------------------------------------------------------------ */

audio_backend_t audio_backend(void) { return s_backend; }

const char *audio_backend_name(void) {
    switch (s_backend) {
        case AUDIO_BACKEND_HDA:        return "hda";
        case AUDIO_BACKEND_VIRTIO_SND: return "virtio-snd";
        case AUDIO_BACKEND_AC97:       return "ac97";
        case AUDIO_BACKEND_PCSPK:      return "pcspk";
        default:                       return "none";
    }
}

int audio_present(void) { return s_ready; }

int audio_is_playing(void) {
    if (!s_ready) return 0;
    if (s_backend == AUDIO_BACKEND_HDA) {
        hda_poll();
        s_playing = hda_playing() ? 1 : 0;
    } else if (s_backend == AUDIO_BACKEND_VIRTIO_SND) {
        virtio_snd_poll();
        s_playing = virtio_snd_playing() ? 1 : 0;
    }
    return s_playing;
}

int audio_volume(void) { return s_volume; }
const char *audio_last_error(void) { return s_err ? s_err : ""; }

uint32_t audio_sample_rate(void) {
    switch (s_backend) {
        case AUDIO_BACKEND_HDA:        return hda_rate();
        case AUDIO_BACKEND_VIRTIO_SND: return virtio_snd_rate();
        case AUDIO_BACKEND_AC97:       return ac97_rate();
        case AUDIO_BACKEND_PCSPK:      return s_rate ? s_rate : 48000u;
        default:                       return 0u;
    }
}

void audio_service(void) {
    if (!s_ready || !s_playing) return;
    switch (s_backend) {
        case AUDIO_BACKEND_HDA:
            hda_poll();
            if (!hda_playing()) audio_stop();
            break;
        case AUDIO_BACKEND_VIRTIO_SND:
            virtio_snd_poll();
            if (!virtio_snd_playing()) audio_stop();
            break;
        case AUDIO_BACKEND_AC97:
            ac97_service();
            break;
        case AUDIO_BACKEND_PCSPK:
            pcspk_service();
            break;
        default:
            break;
    }
}

void audio_stop(void) {
    s_playing = 0;
    s_started = 0;
    s_count = 0;
    s_tail = 0;
    s_open = 0;
    s_open_off = 0;
    s_src = 0;
    s_src_len = 0;
    s_src_pos = 0;

    if (s_ready) {
        switch (s_backend) {
            case AUDIO_BACKEND_HDA:        hda_stop(); break;
            case AUDIO_BACKEND_VIRTIO_SND: virtio_snd_stop(); break;
            case AUDIO_BACKEND_AC97:       ac97_stop_dma(); break;
            case AUDIO_BACKEND_PCSPK:      pcspk_off(); break;
            default:                       break;
        }
    }
    if (s_conv_buf) {
        kfree(s_conv_buf);
        s_conv_buf = 0;
    }
}

int audio_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_volume = percent;
    if (s_ready) {
        switch (s_backend) {
            case AUDIO_BACKEND_HDA:        hda_set_volume(percent); break;
            case AUDIO_BACKEND_VIRTIO_SND: virtio_snd_set_volume(percent); break;
            case AUDIO_BACKEND_AC97:       ac97_set_volume(percent); break;
            case AUDIO_BACKEND_PCSPK:
                if (percent == 0) pcspk_off();
                break;
            default: break;
        }
    }
    return percent;
}

int audio_play_wav(const void *data, uint32_t len) {
    const uint8_t *d = (const uint8_t *)data;
    s_err = "";

    if (!s_ready) { s_err = "ses surucusu yok"; return -1; }
    if (!d || len < 44u) { s_err = "gecersiz WAV dosyasi"; return -1; }
    if (!(d[0] == 'R' && d[1] == 'I' && d[2] == 'F' && d[3] == 'F' &&
          d[8] == 'W' && d[9] == 'A' && d[10] == 'V' && d[11] == 'E')) {
        s_err = "WAV bicimi taninmadi";
        return -1;
    }

    uint16_t fmt = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *pcm = 0;
    uint32_t pcmlen = 0;

    uint32_t off = 12u;
    while (off + 8u <= len) {
        const uint8_t *id = d + off;
        uint32_t sz = rd32(d + off + 4u);
        uint32_t body = off + 8u;
        if (id[0] == 'f' && id[1] == 'm' && id[2] == 't' && id[3] == ' ') {
            if (body + 16u <= len) {
                fmt = rd16(d + body);
                ch = rd16(d + body + 2u);
                rate = rd32(d + body + 4u);
                bits = rd16(d + body + 14u);
            }
        } else if (id[0] == 'd' && id[1] == 'a' && id[2] == 't' && id[3] == 'a') {
            if (body < len) {
                pcm = d + body;
                pcmlen = sz;
                if (pcmlen > len - body) pcmlen = len - body;
            }
            break;
        }
        uint32_t adv = sz + (sz & 1u);
        if (adv < 8u) break;
        off = body + adv;
    }

    if (!pcm || fmt == 0) { s_err = "WAV verisi eksik"; return -1; }
    if (fmt != 1u && fmt != 0xFFFEu) { s_err = "desteklenmeyen WAV bicimi"; return -1; }
    if (ch != 1u && ch != 2u) { s_err = "yalniz mono/stereo"; return -1; }
    if (bits != 8u && bits != 16u) { s_err = "yalniz 8/16-bit PCM"; return -1; }
    if (rate == 0u) rate = 44100u;
    if (pcmlen == 0u) { s_err = "bos ses verisi"; return -1; }

    audio_stop();

    s_src = pcm;
    s_src_len = pcmlen;
    s_src_pos = 0;
    s_bits = bits;
    s_channels = ch;
    s_rate = rate;

    if (s_backend == AUDIO_BACKEND_HDA || s_backend == AUDIO_BACKEND_VIRTIO_SND) {
        const void *play_ptr = pcm;
        uint32_t    play_bytes = pcmlen;

        if (bits != 16u || ch != 2u) {
            uint32_t frame_bytes = (uint32_t)((bits / 8u) * ch);
            uint32_t frames = pcmlen / frame_bytes;
            uint32_t out_bytes = frames * 4u;
            if (out_bytes == 0u) { s_err = "bos ses verisi"; return -1; }
            s_conv_buf = (uint8_t *)kmalloc(out_bytes);
            if (!s_conv_buf) { s_err = "ses tamponu bellek yetersiz"; return -1; }
            uint32_t n = convert_into(s_conv_buf, out_bytes);
            play_ptr = s_conv_buf;
            play_bytes = n;
        }

        int rc = (s_backend == AUDIO_BACKEND_HDA)
                 ? hda_play_pcm(play_ptr, play_bytes, rate)
                 : virtio_snd_play_pcm(play_ptr, play_bytes, rate);
        if (rc != 0) {
            audio_stop();
            s_err = "ses akisi baslatilamadi";
            return -1;
        }
        s_playing = 1;
        return 0;
    }

    if (s_backend == AUDIO_BACKEND_AC97) {
        ac97_set_rate(rate);
        ac97_set_volume(s_volume);
        ac97_reset_dma();
        ac97_set_bdbar((uint32_t)(uintptr_t)s_bdl);

        s_tail = 0;
        s_count = 0;
        s_open = 0;
        s_open_off = 0;
        s_started = 0;
        s_playing = 1;

        audio_service();
        return 0;
    }

    if (s_backend == AUDIO_BACKEND_PCSPK) {
        uint32_t bps = rate * ch * (bits / 8u);
        s_pcspk_dur_ms = bps ? (uint32_t)(((uint64_t)pcmlen * 1000ull) / bps) : 0u;
        s_pcspk_start_ms = clock_ms();
        s_playing = 1;
        pcspk_service();
        return 0;
    }

    return -1;
}

int audio_init(void) {
    s_ready = 0;
    s_backend = AUDIO_BACKEND_NONE;

    if (hda_init() == 0) {
        s_backend = AUDIO_BACKEND_HDA;
        s_ready = 1;
    } else if (virtio_snd_init() == 0) {
        s_backend = AUDIO_BACKEND_VIRTIO_SND;
        s_ready = 1;
    } else if (ac97_init() == 0) {
        s_bdl = (audio_bd_t *)kmalloc((uint32_t)(AUDIO_SLOTS * sizeof(audio_bd_t)));
        s_ring = (uint8_t *)kmalloc(AUDIO_RING_BYTES);
        if (s_bdl && s_ring) {
            for (int i = 0; i < AUDIO_SLOTS; i++) {
                s_bdl[i].addr = 0;
                s_bdl[i].ctl = 0;
            }
            s_backend = AUDIO_BACKEND_AC97;
            s_ready = 1;
        }
    }

    if (!s_ready) {
        pcspk_off();
        s_backend = AUDIO_BACKEND_PCSPK;
        s_ready = 1;
    }

    audio_set_volume(s_volume);
    serial_printf("[audio] backend=%s\n", audio_backend_name());
    kernel_debug("AUDIO: hazir backend=%s", audio_backend_name());
    return 0;
}
