#include "kernel.h"
#include "audio.h"
#include "hda.h"
#include "virtio_snd.h"
#include "ac97.h"
#include "acpi.h"
#include "ec.h"
#include "alloc.h"
#include "clock.h"
#include "serial.h"
#include "sync.h"
#include "sched.h"

extern int  virtio_snd_stream_start(uint32_t sample_rate,
                                    uint32_t (*pull)(void *dst, uint32_t max_bytes, void *ctx),
                                    void *ctx);
extern void virtio_snd_scale_pcm_s16(int16_t *dst, const uint8_t *src, uint32_t bytes, int volume);
extern int  posix_dsp_selftest(void);

#define AUDIO_SLOTS             32
#define AUDIO_SLOT_BYTES        16384
#define AUDIO_RING_BYTES        (AUDIO_SLOTS * AUDIO_SLOT_BYTES)
#define AUDIO_STREAM_RING_BYTES 65536u

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

/* Non-blocking S16_LE stereo ring buffer for /dev/dsp streaming */
static spinlock_t s_stream_lock;
static uint8_t   *s_stream_ring;
static uint32_t   s_stream_head;
static uint32_t   s_stream_tail;
static uint32_t   s_stream_count;
static uint32_t   s_stream_rate = 48000u;
static int        s_stream_ch = 2;
static int        s_stream_bits = 16;
static int        s_stream_open;
static int        s_stream_started;

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

static uint32_t audio_stream_pull_s16(void *dst, uint32_t max_bytes, void *ctx) {
    (void)ctx;
    if (!dst || max_bytes < 4u || !s_stream_ring) return 0u;
    uint8_t *out = (uint8_t *)dst;
    uint64_t fl = sched_irq_save();
    spin_lock(&s_stream_lock);
    uint32_t avail = s_stream_count & ~3u;
    uint32_t cap = max_bytes & ~3u;
    uint32_t take = (avail < cap) ? avail : cap;
    if (take > 0u) {
        uint32_t first = AUDIO_STREAM_RING_BYTES - s_stream_head;
        if (first > take) first = take;
        memcpy(out, s_stream_ring + s_stream_head, first);
        if (take > first) {
            memcpy(out + first, s_stream_ring, take - first);
        }
        s_stream_head = (s_stream_head + take) % AUDIO_STREAM_RING_BYTES;
        s_stream_count -= take;
    }
    spin_unlock(&s_stream_lock);
    sched_irq_restore(fl);
    return take;
}

static void pcspk_service(void) {
    if (!s_playing) return;
    if (s_stream_open) {
        uint8_t tmp[512];
        uint32_t n = audio_stream_pull_s16(tmp, sizeof(tmp), 0);
        if (n < 32u) { pcspk_off(); return; }
        uint32_t frames = n / 4u;
        uint32_t zc = 0;
        int prev_pos = 0;
        for (uint32_t i = 0; i < frames; i++) {
            int16_t s = sample_at(tmp + i * 4u, 16);
            int pos = (s >= 0);
            if (i > 0 && pos != prev_pos) zc++;
            prev_pos = pos;
        }
        uint32_t hz = (zc * (s_stream_rate ? s_stream_rate : 48000u)) / (2u * frames);
        pcspk_tone(hz);
        return;
    }
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
        uint32_t n = s_stream_open
                     ? audio_stream_pull_s16(s_ring + (uint32_t)s_open * AUDIO_SLOT_BYTES + s_open_off,
                                             AUDIO_SLOT_BYTES - s_open_off, 0)
                     : convert_into(s_ring + (uint32_t)s_open * AUDIO_SLOT_BYTES + s_open_off,
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

    if (!s_stream_open && s_started && s_count == 0 && s_src_pos >= s_src_len &&
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
    if (!s_ready) return;
    if (!s_playing) {
        if (s_backend == AUDIO_BACKEND_HDA) hda_poll();
        return;
    }
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

    uint64_t fl = sched_irq_save();
    spin_lock(&s_stream_lock);
    s_stream_open = 0;
    s_stream_started = 0;
    s_stream_head = 0;
    s_stream_tail = 0;
    s_stream_count = 0;
    uint8_t *ring_free = s_stream_ring;
    s_stream_ring = 0;
    spin_unlock(&s_stream_lock);
    sched_irq_restore(fl);
    if (ring_free) kfree(ring_free);
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

int audio_stream_open(uint32_t rate, int channels, int bits) {
    if (!s_ready) return -1;
    if (rate == 0u) rate = 48000u;
    if (rate < 8000u) rate = 8000u;
    if (rate > 96000u) rate = 96000u;
    if (channels != 1 && channels != 2) channels = 2;
    if (bits != 8 && bits != 16) bits = 16;

    if (s_stream_open && !s_stream_started && s_stream_ring) {
        uint64_t fl = sched_irq_save();
        spin_lock(&s_stream_lock);
        s_stream_rate = rate;
        s_stream_ch = channels;
        s_stream_bits = bits;
        spin_unlock(&s_stream_lock);
        sched_irq_restore(fl);
        return 0;
    }

    audio_stop();

    uint8_t *ring = (uint8_t *)kmalloc(AUDIO_STREAM_RING_BYTES);
    if (!ring) return -1;
    memset(ring, 0, AUDIO_STREAM_RING_BYTES);

    uint64_t fl = sched_irq_save();
    spin_lock(&s_stream_lock);
    s_stream_ring = ring;
    s_stream_head = 0;
    s_stream_tail = 0;
    s_stream_count = 0;
    s_stream_rate = rate;
    s_stream_ch = channels;
    s_stream_bits = bits;
    s_stream_open = 1;
    s_stream_started = 0;
    spin_unlock(&s_stream_lock);
    sched_irq_restore(fl);
    return 0;
}

static int audio_stream_start_backend(void) {
    int rc = -1;
    if (s_backend == AUDIO_BACKEND_HDA) {
        rc = hda_stream_start(s_stream_rate, audio_stream_pull_s16, 0);
    } else if (s_backend == AUDIO_BACKEND_VIRTIO_SND) {
        rc = virtio_snd_stream_start(s_stream_rate, audio_stream_pull_s16, 0);
    } else if (s_backend == AUDIO_BACKEND_AC97) {
        ac97_set_rate(s_stream_rate);
        ac97_set_volume(s_volume);
        ac97_reset_dma();
        ac97_set_bdbar((uint32_t)(uintptr_t)s_bdl);
        s_tail = 0;
        s_count = 0;
        s_open = 0;
        s_open_off = 0;
        s_started = 0;
        rc = 0;
    } else if (s_backend == AUDIO_BACKEND_PCSPK) {
        rc = 0;
    }
    if (rc == 0) {
        s_stream_started = 1;
        s_playing = 1;
    }
    return rc;
}

int audio_stream_write(const void *data, uint32_t bytes) {
    if (!s_ready || !s_stream_open || !data || !s_stream_ring) return -1;
    if (bytes == 0u) return 0;

    const uint8_t *in = (const uint8_t *)data;
    uint32_t frame_in = (uint32_t)((s_stream_bits / 8) * s_stream_ch);
    if (frame_in == 0u) return -1;

    uint64_t fl = sched_irq_save();
    spin_lock(&s_stream_lock);
    uint32_t free_bytes = AUDIO_STREAM_RING_BYTES - s_stream_count;
    uint32_t max_frames = free_bytes / 4u;
    uint32_t in_frames = bytes / frame_in;
    uint32_t take_frames = (in_frames < max_frames) ? in_frames : max_frames;

    if (take_frames > 0u) {
        if (s_stream_bits == 16 && s_stream_ch == 2) {
            uint32_t copy_bytes = take_frames * 4u;
            uint32_t first = AUDIO_STREAM_RING_BYTES - s_stream_tail;
            if (first > copy_bytes) first = copy_bytes;
            memcpy(s_stream_ring + s_stream_tail, in, first);
            if (copy_bytes > first) {
                memcpy(s_stream_ring, in + first, copy_bytes - first);
            }
            s_stream_tail = (s_stream_tail + copy_bytes) % AUDIO_STREAM_RING_BYTES;
            s_stream_count += copy_bytes;
        } else {
            for (uint32_t i = 0; i < take_frames; i++) {
                const uint8_t *f = in + i * frame_in;
                int16_t l, r;
                if (s_stream_ch == 2) {
                    l = sample_at(f, s_stream_bits);
                    r = sample_at(f + (s_stream_bits / 8), s_stream_bits);
                } else {
                    l = r = sample_at(f, s_stream_bits);
                }
                uint32_t p = s_stream_tail;
                s_stream_ring[p] = (uint8_t)(l & 0xFF);
                p = (p + 1u) % AUDIO_STREAM_RING_BYTES;
                s_stream_ring[p] = (uint8_t)((l >> 8) & 0xFF);
                p = (p + 1u) % AUDIO_STREAM_RING_BYTES;
                s_stream_ring[p] = (uint8_t)(r & 0xFF);
                p = (p + 1u) % AUDIO_STREAM_RING_BYTES;
                s_stream_ring[p] = (uint8_t)((r >> 8) & 0xFF);
                s_stream_tail = (p + 1u) % AUDIO_STREAM_RING_BYTES;
                s_stream_count += 4u;
            }
        }
    }
    uint32_t cur_cnt = s_stream_count;
    int started = s_stream_started;
    spin_unlock(&s_stream_lock);
    sched_irq_restore(fl);

    if (!started && cur_cnt > 0u) {
        if (audio_stream_start_backend() != 0) return -1;
    }
    if (s_stream_started) {
        audio_service();
    }
    return (int)(take_frames * frame_in);
}

void audio_stream_close(void) {
    if (!s_stream_open && !s_stream_ring) return;
    audio_stop();
}

uint32_t audio_stream_free(void) {
    if (!s_stream_open) return 0u;
    uint64_t fl = sched_irq_save();
    spin_lock(&s_stream_lock);
    uint32_t free_bytes = AUDIO_STREAM_RING_BYTES - s_stream_count;
    uint32_t frame_in = (uint32_t)((s_stream_bits / 8) * s_stream_ch);
    uint32_t res = (frame_in > 0u) ? ((free_bytes / 4u) * frame_in) : 0u;
    spin_unlock(&s_stream_lock);
    sched_irq_restore(fl);
    return res;
}

uint32_t audio_stream_buffered(void) {
    uint64_t fl = sched_irq_save();
    spin_lock(&s_stream_lock);
    uint32_t res = s_stream_count;
    spin_unlock(&s_stream_lock);
    sched_irq_restore(fl);
    return res;
}

int audio_stream_active(void) {
    return s_stream_open;
}

int audio_power_v11_selftest(void) {
    /* 1. VirtIO-Sound PCM int16 software volume scaling (overflow/clip-safe) */
    static const int16_t s_in[4] = { 10000, -10000, 32767, -32768 };
    int16_t s_out[4];
    bool vsnd_vol_ok = true;

    virtio_snd_scale_pcm_s16(s_out, (const uint8_t *)s_in, sizeof(s_in), 0);
    for (int i = 0; i < 4; i++) {
        if (s_out[i] != 0) vsnd_vol_ok = false;
    }
    virtio_snd_scale_pcm_s16(s_out, (const uint8_t *)s_in, sizeof(s_in), 50);
    if (s_out[0] != 5000 || s_out[1] != -5000 || s_out[2] != 16383 || s_out[3] != -16384)
        vsnd_vol_ok = false;
    virtio_snd_scale_pcm_s16(s_out, (const uint8_t *)s_in, sizeof(s_in), 100);
    for (int i = 0; i < 4; i++) {
        if (s_out[i] != s_in[i]) vsnd_vol_ok = false;
    }
    if (virtio_snd_present()) {
        virtio_snd_set_volume(50);
        virtio_snd_set_volume(s_volume);
    }

    /* 2. /dev/dsp ring-buffered stream open -> write -> service -> close */
    bool dsp_ok = false;
    if (audio_stream_open(48000u, 2, 16) == 0) {
        uint8_t zero_pcm[1024];
        memset(zero_pcm, 0, sizeof(zero_pcm));
        int wr = audio_stream_write(zero_pcm, sizeof(zero_pcm));
        audio_service();
        dsp_ok = (wr == (int)sizeof(zero_pcm) && audio_stream_active() == 1);
        audio_stream_close();
    }
    if (posix_dsp_selftest() != 0) dsp_ok = false;

    /* 3. HDA multi-pin + jack-sense check */
    int hda_pins = 0;
    bool hda_ok = true;
    if (hda_present()) {
        hda_pins = hda_out_pin_count();
        hda_poll_jacks();
        hda_ok = (hda_pins >= 1 && hda_pins <= 4);
    }

    /* 4. Safe S3 sleep guard + backlight query */
    int s3_rc = acpi_enter_sleep();
    bool s3_ok = (s3_rc == -1);
    int bl = ec_brightness_get();
    bool bl_ok = (bl == -1 || (bl >= 0 && bl <= 100));

    bool all_ok = vsnd_vol_ok && dsp_ok && hda_ok && s3_ok && bl_ok;
    serial_printf("[audio-v11] selftest: backend=%s dsp_ring=%s vsnd_vol=%s hda_pins=%d s3_guard=%s bl=%d -> %s\n",
                  audio_backend_name(),
                  dsp_ok ? "OK" : "FAIL",
                  vsnd_vol_ok ? "OK" : "FAIL",
                  hda_pins,
                  s3_ok ? "OK" : "FAIL",
                  bl,
                  all_ok ? "PASS" : "FAIL");
    return all_ok ? 0 : -1;
}

int audio_init(void) {
    s_ready = 0;
    s_backend = AUDIO_BACKEND_NONE;
    spin_init(&s_stream_lock);

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
    audio_power_v11_selftest();
    return 0;
}
