/**
 * Arctian Intel High Definition Audio (HDA) Driver
 * PCI Class 0x04 Subclass 0x03 (Intel ICH6 / ICH9 / PCH HDA + compatible).
 *
 * Allocates CORB/RIRB, BDL, and PCM DMA pages via vmm_alloc_frame() (0 static DMA BSS).
 */
#include "hda.h"
#include "kernel.h"
#include "pci.h"
#include "paging.h"
#include "mmio.h"
#include "clock.h"
#include "serial.h"

/* HDA Controller Register Offsets */
#define HDA_REG_GCAP        0x00u
#define HDA_REG_VMIN        0x02u
#define HDA_REG_VMAJ        0x03u
#define HDA_REG_GCTL        0x08u
#define HDA_REG_WAKEEN      0x0Cu
#define HDA_REG_STATESTS    0x0Eu
#define HDA_REG_INTCTL      0x20u
#define HDA_REG_INTSTS      0x24u
#define HDA_REG_WALCLK      0x30u
#define HDA_REG_CORBLBASE   0x40u
#define HDA_REG_CORBUBASE   0x44u
#define HDA_REG_CORBWP      0x48u
#define HDA_REG_CORBRP      0x4Au
#define HDA_REG_CORBCTL     0x4Cu
#define HDA_REG_CORBSTS     0x4Du
#define HDA_REG_CORBSIZE    0x4Eu
#define HDA_REG_RIRBLBASE   0x50u
#define HDA_REG_RIRBUBASE   0x54u
#define HDA_REG_RIRBWP      0x58u
#define HDA_REG_RINTCNT     0x5Au
#define HDA_REG_RIRBCTL     0x5Cu
#define HDA_REG_RIRBSTS     0x5Du
#define HDA_REG_RIRBSIZE    0x5Eu
#define HDA_REG_ICOI        0x60u
#define HDA_REG_ICII        0x64u
#define HDA_REG_ICIS        0x68u

/* Stream Descriptor Offsets (relative to stream base) */
#define HDA_SD_CTL          0x00u /* 24-bit */
#define HDA_SD_STS          0x03u /* 8-bit */
#define HDA_SD_LPIB         0x04u
#define HDA_SD_CBL          0x08u
#define HDA_SD_LVI          0x0Cu
#define HDA_SD_FIFOS        0x12u
#define HDA_SD_FMT          0x14u
#define HDA_SD_BDLPL        0x18u
#define HDA_SD_BDLPU        0x1Cu

#define HDA_GCTL_CRST       (1u << 0)
#define HDA_ICIS_ICB        (1u << 0)
#define HDA_ICIS_IRV        (1u << 1)

#define HDA_SD_CTL_SRST     (1u << 0)
#define HDA_SD_CTL_RUN      (1u << 1)
#define HDA_SD_CTL_IOCE     (1u << 2)
#define HDA_SD_STS_BCIS     (1u << 2)
#define HDA_SD_STS_FIFOE    (1u << 3)
#define HDA_SD_STS_DESE     (1u << 4)

/* Codec Verbs */
#define HDA_VERB_GET_PARAM          0xF00u
#define HDA_VERB_GET_CONN_SEL       0xF01u
#define HDA_VERB_SET_CONN_SEL       0x701u
#define HDA_VERB_GET_CONN_LIST      0xF02u
#define HDA_VERB_SET_POWER          0x705u
#define HDA_VERB_SET_CHAN_STREAM    0x706u
#define HDA_VERB_GET_PIN_CTL        0xF07u
#define HDA_VERB_SET_PIN_CTL        0x707u
#define HDA_VERB_SET_EAPD           0x70Cu
#define HDA_VERB_GET_CFG_DEF        0xF1Cu
#define HDA_VERB_SET_CONV_FMT       0x2u   /* 4-bit verb */
#define HDA_VERB_SET_AMP_GAIN       0x3u   /* 4-bit verb */

/* Codec Parameters */
#define HDA_PARAM_VENDOR_ID         0x00u
#define HDA_PARAM_SUB_NODES         0x04u
#define HDA_PARAM_FG_TYPE           0x05u
#define HDA_PARAM_WCAPS             0x09u
#define HDA_PARAM_PCM_RATES         0x0Au
#define HDA_PARAM_PIN_CAPS          0x0Cu
#define HDA_PARAM_OUT_AMP_CAPS      0x12u
#define HDA_PARAM_CONN_LEN          0x0Eu

#define HDA_WTYPE_DAC               0x0u
#define HDA_WTYPE_ADC               0x1u
#define HDA_WTYPE_MIXER             0x2u
#define HDA_WTYPE_SELECTOR          0x3u
#define HDA_WTYPE_PIN               0x4u

#define HDA_RING_PAGES              16u
#define HDA_PAGE_BYTES              4096u
#define HDA_RING_BYTES              (HDA_RING_PAGES * HDA_PAGE_BYTES)
#define HDA_STREAM_ID               1u

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint32_t ioc;
} hda_bdl_entry_t;

typedef struct {
    bool     present;
    uint8_t  bus, slot, func, irq;
    uint16_t vendor, device;
    uint64_t mmio_base;
    volatile uint8_t *mmio;

    uint8_t  iss, oss, bss;
    uint32_t osd_off;

    /* CORB / RIRB (allocated from 1 frame) */
    uint64_t           cmd_frame_pa;
    volatile uint32_t *corb;
    volatile uint64_t *rirb;
    uint16_t           corb_entries;
    uint16_t           rirb_entries;
    uint16_t           corb_wp;
    uint16_t           rirb_rp;
    bool               corb_active;

    /* Codec & Output Path */
    uint8_t  cad;
    uint32_t codec_vendor;
    uint8_t  afg_nid;
    uint8_t  dac_nid;
    uint8_t  pin_nid;
    uint8_t  dac_amp_steps;
    uint8_t  pin_amp_steps;
    bool     dac_has_amp;
    bool     pin_has_amp;

    /* Output BDL + DMA Ring Pages */
    uint64_t         bdl_pa;
    hda_bdl_entry_t *bdl;
    uint64_t         ring_pa[HDA_RING_PAGES];
    uint8_t         *ring_va[HDA_RING_PAGES];

    /* Playback state */
    bool           playing;
    int            volume;
    uint32_t       rate;
    const uint8_t *src;
    uint32_t       src_len;
    uint32_t       src_pos;
    uint32_t       active_cbl;
    uint32_t       last_lpib;
    uint64_t       played_bytes;
    uint64_t       total_target_bytes;
    uint32_t       fill_page;
    uint32_t       pages_queued;
} hda_state_t;

static hda_state_t s_hda;

static inline uint8_t  hda_r8(uint32_t off)  { return *(volatile uint8_t *)(s_hda.mmio + off); }
static inline uint16_t hda_r16(uint32_t off) { return *(volatile uint16_t *)(s_hda.mmio + off); }
static inline uint32_t hda_r32(uint32_t off) { return *(volatile uint32_t *)(s_hda.mmio + off); }
static inline void     hda_w8(uint32_t off, uint8_t v)   { *(volatile uint8_t *)(s_hda.mmio + off) = v; }
static inline void     hda_w16(uint32_t off, uint16_t v) { *(volatile uint16_t *)(s_hda.mmio + off) = v; }
static inline void     hda_w32(uint32_t off, uint32_t v) { *(volatile uint32_t *)(s_hda.mmio + off) = v; }

static void hda_udelay(uint32_t us) {
    for (uint32_t i = 0; i < us * 40u; i++) __asm__ volatile("pause");
}

static bool hda_find_pci(pci_device_t *out, pci_bar_t *bar0) {
    for (int b = 0; b < 256; b++) {
        for (int s = 0; s < 32; s++) {
            if (pci_config_readw((uint8_t)b, (uint8_t)s, 0, 0) == 0xFFFFu) continue;
            uint8_t hdr = pci_config_readb((uint8_t)b, (uint8_t)s, 0, 0x0Eu);
            int nfunc = (hdr & 0x80u) ? 8 : 1;
            for (int f = 0; f < nfunc; f++) {
                uint16_t vendor = pci_config_readw((uint8_t)b, (uint8_t)s, (uint8_t)f, 0);
                if (vendor == 0xFFFFu) continue;
                uint32_t rc = pci_config_readl((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x08u);
                uint8_t cls = (uint8_t)((rc >> 24) & 0xFFu);
                uint8_t sub = (uint8_t)((rc >> 16) & 0xFFu);
                if (cls != 0x04u || sub != 0x03u) continue;

                out->bus = (uint8_t)b;
                out->slot = (uint8_t)s;
                out->func = (uint8_t)f;
                out->vendor = vendor;
                out->device = pci_config_readw((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x02u);
                out->class_code = cls;
                out->subclass = sub;
                out->prog_if = (uint8_t)((rc >> 8) & 0xFFu);
                out->rev_id = (uint8_t)(rc & 0xFFu);
                out->irq = pci_config_readb((uint8_t)b, (uint8_t)s, (uint8_t)f, PCI_CONFIG_INTERRUPT);
                *bar0 = pci_read_bar((uint8_t)b, (uint8_t)s, (uint8_t)f, 0);
                return true;
            }
        }
    }
    return false;
}

static uint16_t hda_pick_ring_size(uint8_t reg_val, uint8_t *sz_bits_out) {
    uint8_t cap = (uint8_t)((reg_val >> 4) & 0x0Fu);
    if (cap & 0x04u) { *sz_bits_out = 0x02u; return 256u; }
    if (cap & 0x02u) { *sz_bits_out = 0x01u; return 16u; }
    *sz_bits_out = 0x00u;
    return 2u;
}

static int hda_setup_corb_rirb(void) {
    s_hda.cmd_frame_pa = vmm_alloc_frame();
    if (!s_hda.cmd_frame_pa) return -1;

    uint64_t corb_pa = s_hda.cmd_frame_pa;
    uint64_t rirb_pa = s_hda.cmd_frame_pa + 2048ull;
    s_hda.corb = (volatile uint32_t *)(uintptr_t)corb_pa;
    s_hda.rirb = (volatile uint64_t *)(uintptr_t)rirb_pa;

    /* Stop CORB and RIRB DMA */
    hda_w8(HDA_REG_CORBCTL, 0u);
    hda_w8(HDA_REG_RIRBCTL, 0u);
    for (int i = 0; i < 10000; i++) {
        if (!(hda_r8(HDA_REG_CORBCTL) & 0x02u) && !(hda_r8(HDA_REG_RIRBCTL) & 0x02u))
            break;
        hda_udelay(10);
    }

    uint8_t csz_bits = 0, rsz_bits = 0;
    s_hda.corb_entries = hda_pick_ring_size(hda_r8(HDA_REG_CORBSIZE), &csz_bits);
    s_hda.rirb_entries = hda_pick_ring_size(hda_r8(HDA_REG_RIRBSIZE), &rsz_bits);

    hda_w8(HDA_REG_CORBSIZE, csz_bits);
    hda_w32(HDA_REG_CORBLBASE, (uint32_t)(corb_pa & 0xFFFFFFFFull));
    hda_w32(HDA_REG_CORBUBASE, (uint32_t)(corb_pa >> 32));

    /* Reset CORB read pointer */
    hda_w16(HDA_REG_CORBRP, 0x8000u);
    for (int i = 0; i < 5000 && !(hda_r16(HDA_REG_CORBRP) & 0x8000u); i++) hda_udelay(10);
    hda_w16(HDA_REG_CORBRP, 0x0000u);
    for (int i = 0; i < 5000 && (hda_r16(HDA_REG_CORBRP) & 0x8000u); i++) hda_udelay(10);
    hda_w16(HDA_REG_CORBWP, 0x0000u);
    s_hda.corb_wp = 0;

    hda_w8(HDA_REG_RIRBSIZE, rsz_bits);
    hda_w32(HDA_REG_RIRBLBASE, (uint32_t)(rirb_pa & 0xFFFFFFFFull));
    hda_w32(HDA_REG_RIRBUBASE, (uint32_t)(rirb_pa >> 32));
    hda_w16(HDA_REG_RIRBWP, 0x8000u);
    s_hda.rirb_rp = 0;
    hda_w16(HDA_REG_RINTCNT, 1u);
    hda_w8(HDA_REG_RIRBSTS, 0x05u);
    hda_w8(HDA_REG_CORBSTS, 0x01u);

    hda_w8(HDA_REG_CORBCTL, 0x02u);
    hda_w8(HDA_REG_RIRBCTL, 0x02u);
    s_hda.corb_active = true;
    return 0;
}

static int hda_cmd_imm(uint32_t raw, uint32_t *resp_out) {
    for (int i = 0; i < 20000; i++) {
        if (!(hda_r16(HDA_REG_ICIS) & HDA_ICIS_ICB)) break;
        hda_udelay(5);
    }
    if (hda_r16(HDA_REG_ICIS) & HDA_ICIS_ICB) return -1;

    hda_w16(HDA_REG_ICIS, HDA_ICIS_IRV);
    hda_w32(HDA_REG_ICOI, raw);
    hda_w16(HDA_REG_ICIS, HDA_ICIS_ICB);

    for (int i = 0; i < 40000; i++) {
        uint16_t st = hda_r16(HDA_REG_ICIS);
        if ((st & HDA_ICIS_IRV) && !(st & HDA_ICIS_ICB)) {
            if (resp_out) *resp_out = hda_r32(HDA_REG_ICII);
            hda_w16(HDA_REG_ICIS, HDA_ICIS_IRV);
            return 0;
        }
        hda_udelay(5);
    }
    return -2;
}

static int hda_cmd_raw(uint8_t cad, uint8_t nid, uint32_t verb_payload20, uint32_t *resp_out) {
    uint32_t raw = ((uint32_t)(cad & 0x0Fu) << 28) |
                   ((uint32_t)nid << 20) |
                   (verb_payload20 & 0xFFFFFu);

    if (s_hda.corb_active) {
        uint16_t next_wp = (uint16_t)((s_hda.corb_wp + 1u) % s_hda.corb_entries);
        s_hda.corb[next_wp] = raw;
        __asm__ volatile("mfence" ::: "memory");
        s_hda.corb_wp = next_wp;
        hda_w16(HDA_REG_CORBWP, next_wp);

        for (int i = 0; i < 40000; i++) {
            uint16_t rwp = (uint16_t)(hda_r16(HDA_REG_RIRBWP) & 0xFFu);
            while (s_hda.rirb_rp != rwp) {
                s_hda.rirb_rp = (uint16_t)((s_hda.rirb_rp + 1u) % s_hda.rirb_entries);
                __asm__ volatile("mfence" ::: "memory");
                uint64_t entry = s_hda.rirb[s_hda.rirb_rp];
                uint32_t resp  = (uint32_t)(entry & 0xFFFFFFFFull);
                uint32_t ex    = (uint32_t)(entry >> 32);
                if (ex & 0x10u) continue; /* Unsolicited response */
                if ((ex & 0x0Fu) == (uint32_t)cad) {
                    if (resp_out) *resp_out = resp;
                    hda_w8(HDA_REG_RIRBSTS, 0x05u);
                    return 0;
                }
            }
            hda_udelay(5);
        }
        /* CORB/RIRB timed out; fall back to immediate command */
        s_hda.corb_active = false;
    }

    return hda_cmd_imm(raw, resp_out);
}

static uint32_t hda_verb12(uint8_t cad, uint8_t nid, uint16_t verb12, uint8_t param) {
    uint32_t resp = 0;
    uint32_t vp = (((uint32_t)verb12 & 0xFFFu) << 8) | (uint32_t)param;
    if (hda_cmd_raw(cad, nid, vp, &resp) != 0) return 0xFFFFFFFFu;
    return resp;
}

static void hda_verb4(uint8_t cad, uint8_t nid, uint8_t verb4, uint16_t payload16) {
    uint32_t vp = (((uint32_t)verb4 & 0x0Fu) << 16) | (uint32_t)payload16;
    (void)hda_cmd_raw(cad, nid, vp, 0);
}

static uint32_t hda_get_param(uint8_t cad, uint8_t nid, uint8_t param) {
    return hda_verb12(cad, nid, HDA_VERB_GET_PARAM, param);
}

static uint8_t hda_conn_entry(uint8_t cad, uint8_t nid, uint32_t idx, bool long_form) {
    if (long_form) {
        uint32_t v = hda_verb12(cad, nid, HDA_VERB_GET_CONN_LIST, (uint8_t)(idx & ~1u));
        return (uint8_t)((v >> ((idx & 1u) * 16u)) & 0xFFFFu);
    }
    uint32_t v = hda_verb12(cad, nid, HDA_VERB_GET_CONN_LIST, (uint8_t)(idx & ~3u));
    return (uint8_t)((v >> ((idx & 3u) * 8u)) & 0xFFu);
}

static void hda_unmute_amp(uint8_t cad, uint8_t nid, bool is_out, bool is_in,
                           uint8_t idx, uint8_t gain) {
    uint16_t p = (uint16_t)(gain & 0x7Fu);
    if (is_out) p |= (1u << 15);
    if (is_in)  p |= (1u << 14);
    p |= (1u << 13) | (1u << 12); /* Left + Right */
    p |= (uint16_t)((idx & 0x0Fu) << 8);
    hda_verb4(cad, nid, HDA_VERB_SET_AMP_GAIN, p);
}

static uint16_t hda_encode_fmt(uint32_t hz) {
    /* 16-bit signed stereo PCM */
    uint16_t base = 0u; /* 48 kHz family */
    uint16_t mult = 0u;
    uint16_t div  = 0u;

    if (hz == 44100u || hz == 22050u || hz == 11025u || hz == 88200u) {
        base = 1u;
        if (hz == 88200u)      mult = 1u;
        else if (hz == 22050u) div  = 1u;
        else if (hz == 11025u) div  = 3u;
    } else {
        if (hz == 96000u)      mult = 1u;
        else if (hz == 24000u) div  = 1u;
        else if (hz == 16000u) div  = 2u;
        else if (hz == 12000u) div  = 3u;
        else if (hz == 8000u)  div  = 5u;
        else if (hz == 32000u) { mult = 1u; div = 2u; }
    }
    return (uint16_t)((base << 14) | (mult << 11) | (div << 8) | (1u << 4) | 1u);
}

static void hda_enable_pin_and_dac(uint8_t cad, uint8_t w_start, uint8_t w_count,
                                   uint8_t pin, uint8_t dac) {
    /* Power up DAC and Pin to D0 */
    hda_verb12(cad, dac, HDA_VERB_SET_POWER, 0x00u);
    hda_verb12(cad, pin, HDA_VERB_SET_POWER, 0x00u);

    uint32_t pin_wcaps = hda_get_param(cad, pin, HDA_PARAM_WCAPS);
    uint32_t pin_caps  = hda_get_param(cad, pin, HDA_PARAM_PIN_CAPS);

    uint8_t pctl = 0x40u; /* OUT_EN */
    if (pin_caps & (1u << 3)) pctl |= 0x80u; /* HP_EN if supported */
    hda_verb12(cad, pin, HDA_VERB_SET_PIN_CTL, pctl);

    if (pin_caps & (1u << 16)) /* EAPD capable */
        hda_verb12(cad, pin, HDA_VERB_SET_EAPD, 0x02u);

    /* Unmute any intermediate mixer/selector on the pin's connection list */
    uint32_t clen_raw = hda_get_param(cad, pin, HDA_PARAM_CONN_LEN);
    uint32_t clen = clen_raw & 0x7Fu;
    bool long_f = (clen_raw & 0x80u) != 0;
    for (uint32_t i = 0; i < clen; i++) {
        uint8_t src = hda_conn_entry(cad, pin, i, long_f);
        if (src >= w_start && src < (uint8_t)(w_start + w_count)) {
            uint32_t swc = hda_get_param(cad, src, HDA_PARAM_WCAPS);
            uint8_t stype = (uint8_t)((swc >> 20) & 0xFu);
            if (stype == HDA_WTYPE_MIXER || stype == HDA_WTYPE_SELECTOR) {
                hda_verb12(cad, src, HDA_VERB_SET_POWER, 0x00u);
                hda_unmute_amp(cad, src, true, false, 0, 0x3Fu);
                uint32_t mlen = hda_get_param(cad, src, HDA_PARAM_CONN_LEN) & 0x7Fu;
                for (uint32_t j = 0; j < mlen; j++)
                    hda_unmute_amp(cad, src, false, true, (uint8_t)j, 0x3Fu);
            }
            if (src == dac || stype == HDA_WTYPE_MIXER || stype == HDA_WTYPE_SELECTOR) {
                if (clen > 1) hda_verb12(cad, pin, HDA_VERB_SET_CONN_SEL, (uint8_t)i);
                break;
            }
        }
    }

    /* Check DAC & Pin Output Amp Capabilities */
    uint32_t dac_wcaps = hda_get_param(cad, dac, HDA_PARAM_WCAPS);
    if (dac_wcaps & (1u << 2)) { /* Out Amp Present */
        uint32_t acap = hda_get_param(cad, dac, HDA_PARAM_OUT_AMP_CAPS);
        s_hda.dac_has_amp = true;
        s_hda.dac_amp_steps = (uint8_t)((acap >> 8) & 0x7Fu);
    }
    if (pin_wcaps & (1u << 2)) {
        uint32_t acap = hda_get_param(cad, pin, HDA_PARAM_OUT_AMP_CAPS);
        s_hda.pin_has_amp = true;
        s_hda.pin_amp_steps = (uint8_t)((acap >> 8) & 0x7Fu);
    }

    hda_verb12(cad, dac, HDA_VERB_SET_CHAN_STREAM, (uint8_t)(HDA_STREAM_ID << 4));
    hda_verb4(cad, dac, HDA_VERB_SET_CONV_FMT, hda_encode_fmt(48000u));
}

static uint8_t hda_trace_dac(uint8_t cad, uint8_t nid, uint8_t w_start, uint8_t w_count, int depth) {
    if (depth > 4) return 0;
    uint32_t wc = hda_get_param(cad, nid, HDA_PARAM_WCAPS);
    uint8_t wt = (uint8_t)((wc >> 20) & 0xFu);
    if (wt == HDA_WTYPE_DAC) return nid;

    uint32_t clen_raw = hda_get_param(cad, nid, HDA_PARAM_CONN_LEN);
    uint32_t clen = clen_raw & 0x7Fu;
    bool long_f = (clen_raw & 0x80u) != 0;
    for (uint32_t i = 0; i < clen; i++) {
        uint8_t src = hda_conn_entry(cad, nid, i, long_f);
        if (src >= w_start && src < (uint8_t)(w_start + w_count)) {
            uint8_t found = hda_trace_dac(cad, src, w_start, w_count, depth + 1);
            if (found) return found;
        }
    }
    return 0;
}

static int hda_probe_codec(uint8_t cad) {
    uint32_t vid = hda_get_param(cad, 0u, HDA_PARAM_VENDOR_ID);
    if (vid == 0u || vid == 0xFFFFFFFFu) return -1;

    uint32_t sub = hda_get_param(cad, 0u, HDA_PARAM_SUB_NODES);
    uint8_t fg_start = (uint8_t)((sub >> 16) & 0xFFu);
    uint8_t fg_count = (uint8_t)(sub & 0xFFu);

    uint8_t afg = 0;
    for (uint8_t i = 0; i < fg_count; i++) {
        uint8_t nid = (uint8_t)(fg_start + i);
        if ((hda_get_param(cad, nid, HDA_PARAM_FG_TYPE) & 0xFFu) == 0x01u) {
            afg = nid;
            break;
        }
    }
    if (!afg) return -2;

    hda_verb12(cad, afg, HDA_VERB_SET_POWER, 0x00u);
    hda_udelay(2000);

    uint32_t wsub = hda_get_param(cad, afg, HDA_PARAM_SUB_NODES);
    uint8_t w_start = (uint8_t)((wsub >> 16) & 0xFFu);
    uint8_t w_count = (uint8_t)(wsub & 0xFFu);

    uint8_t first_dac = 0;
    uint8_t best_pin = 0;
    uint8_t best_dac = 0;
    int     best_prio = -1;

    for (uint8_t i = 0; i < w_count; i++) {
        uint8_t nid = (uint8_t)(w_start + i);
        uint32_t wc = hda_get_param(cad, nid, HDA_PARAM_WCAPS);
        uint8_t wt = (uint8_t)((wc >> 20) & 0xFu);

        if (wt == HDA_WTYPE_DAC && !first_dac) first_dac = nid;
        if (wt != HDA_WTYPE_PIN) continue;

        uint32_t pcaps = hda_get_param(cad, nid, HDA_PARAM_PIN_CAPS);
        if (!(pcaps & (1u << 4))) continue; /* Not output capable */

        uint32_t cfg = hda_verb12(cad, nid, HDA_VERB_GET_CFG_DEF, 0u);
        uint8_t port_conn = (uint8_t)((cfg >> 30) & 0x03u);
        uint8_t def_dev   = (uint8_t)((cfg >> 20) & 0x0Fu);
        if (port_conn == 1u) continue; /* No physical connection */

        int prio = 0;
        if (def_dev == 0u) prio = 3;      /* Line Out */
        else if (def_dev == 1u) prio = 2; /* Speaker */
        else if (def_dev == 2u) prio = 1; /* Headphone */
        else continue;

        uint8_t traced = hda_trace_dac(cad, nid, w_start, w_count, 0);
        if (prio > best_prio) {
            best_prio = prio;
            best_pin = nid;
            best_dac = traced;
        }
    }

    if (!best_pin || (!best_dac && !first_dac)) return -3;
    if (!best_dac) best_dac = first_dac;

    s_hda.cad = cad;
    s_hda.codec_vendor = vid;
    s_hda.afg_nid = afg;
    s_hda.dac_nid = best_dac;
    s_hda.pin_nid = best_pin;

    hda_enable_pin_and_dac(cad, w_start, w_count, best_pin, best_dac);
    return 0;
}

static void hda_reset_stream(void) {
    uint32_t sd = s_hda.osd_off;
    uint32_t ctl = hda_r32(sd + HDA_SD_CTL) & 0x00FFFFFFu;
    ctl &= ~(HDA_SD_CTL_RUN | HDA_SD_CTL_IOCE);
    hda_w32(sd + HDA_SD_CTL, ctl);
    for (int i = 0; i < 5000 && (hda_r32(sd + HDA_SD_CTL) & HDA_SD_CTL_RUN); i++) hda_udelay(5);

    hda_w32(sd + HDA_SD_CTL, HDA_SD_CTL_SRST);
    for (int i = 0; i < 5000 && !(hda_r32(sd + HDA_SD_CTL) & HDA_SD_CTL_SRST); i++) hda_udelay(5);
    hda_w32(sd + HDA_SD_CTL, 0u);
    for (int i = 0; i < 5000 && (hda_r32(sd + HDA_SD_CTL) & HDA_SD_CTL_SRST); i++) hda_udelay(5);

    hda_w8(sd + HDA_SD_STS, HDA_SD_STS_BCIS | HDA_SD_STS_FIFOE | HDA_SD_STS_DESE);
}

static void hda_fill_ring_page(uint32_t page_idx) {
    uint8_t *dst = s_hda.ring_va[page_idx];
    uint32_t copied = 0;
    if (s_hda.src && s_hda.src_pos < s_hda.src_len) {
        uint32_t rem = s_hda.src_len - s_hda.src_pos;
        copied = (rem > HDA_PAGE_BYTES) ? HDA_PAGE_BYTES : rem;
        memcpy(dst, s_hda.src + s_hda.src_pos, copied);
        s_hda.src_pos += copied;
    }
    if (copied < HDA_PAGE_BYTES) {
        memset(dst + copied, 0, HDA_PAGE_BYTES - copied);
    }
}

int hda_init(void) {
    memset(&s_hda, 0, sizeof(s_hda));
    s_hda.volume = 80;
    s_hda.rate = 48000u;

    pci_device_t dev;
    pci_bar_t bar0;
    if (!hda_find_pci(&dev, &bar0)) return -1;
    if (bar0.is_io || bar0.address == 0) return -2;

    uint64_t msz = bar0.size ? bar0.size : 0x4000u;
    if (vmm_map_mmio_range(bar0.address, msz) != 0)
        mmio_map(bar0.address, msz);

    pci_enable_bus_mastering(dev.bus, dev.slot, dev.func);
    {
        uint16_t cmd = pci_config_readw(dev.bus, dev.slot, dev.func, PCI_CONFIG_COMMAND);
        pci_config_writew(dev.bus, dev.slot, dev.func, PCI_CONFIG_COMMAND, (uint16_t)(cmd | PCI_CMD_INT_DISABLE));
    }
    /* Clear TCSEL bits [2:0] to ensure TC0 */
    uint8_t tcsel = pci_config_readb(dev.bus, dev.slot, dev.func, 0x44u);
    pci_config_writeb(dev.bus, dev.slot, dev.func, 0x44u, (uint8_t)(tcsel & ~0x07u));

    s_hda.bus = dev.bus;
    s_hda.slot = dev.slot;
    s_hda.func = dev.func;
    s_hda.irq = dev.irq;
    s_hda.vendor = dev.vendor;
    s_hda.device = dev.device;
    s_hda.mmio_base = bar0.address;
    s_hda.mmio = (volatile uint8_t *)(uintptr_t)bar0.address;

    uint16_t gcap = hda_r16(HDA_REG_GCAP);
    s_hda.iss = (uint8_t)((gcap >> 8) & 0x0Fu);
    s_hda.oss = (uint8_t)((gcap >> 12) & 0x0Fu);
    s_hda.bss = (uint8_t)((gcap >> 3) & 0x1Fu);
    if (s_hda.oss == 0) return -3;
    s_hda.osd_off = 0x80u + (uint32_t)s_hda.iss * 0x20u;

    /* Reset controller */
    hda_w32(HDA_REG_INTCTL, 0u);
    hda_w32(HDA_REG_GCTL, hda_r32(HDA_REG_GCTL) & ~HDA_GCTL_CRST);
    for (int i = 0; i < 10000 && (hda_r32(HDA_REG_GCTL) & HDA_GCTL_CRST); i++) hda_udelay(10);
    hda_udelay(1000);
    hda_w32(HDA_REG_GCTL, hda_r32(HDA_REG_GCTL) | HDA_GCTL_CRST);
    for (int i = 0; i < 10000 && !(hda_r32(HDA_REG_GCTL) & HDA_GCTL_CRST); i++) hda_udelay(10);
    if (!(hda_r32(HDA_REG_GCTL) & HDA_GCTL_CRST)) return -4;

    /* Wait for codecs to report in STATESTS */
    hda_udelay(2000);
    uint16_t statests = hda_r16(HDA_REG_STATESTS) & 0x7FFFu;
    if (!statests) {
        clock_sleep_ms(5);
        statests = hda_r16(HDA_REG_STATESTS) & 0x7FFFu;
    }
    hda_w16(HDA_REG_STATESTS, statests);
    if (!statests) return -5;

    if (hda_setup_corb_rirb() != 0) return -6;

    bool codec_ok = false;
    for (uint8_t cad = 0; cad < 15u; cad++) {
        if (statests & (uint16_t)(1u << cad)) {
            if (hda_probe_codec(cad) == 0) {
                codec_ok = true;
                break;
            }
        }
    }
    if (!codec_ok) return -7;

    /* Allocate BDL frame and PCM DMA pages via vmm_alloc_frame() */
    s_hda.bdl_pa = vmm_alloc_frame();
    if (!s_hda.bdl_pa) return -8;
    s_hda.bdl = (hda_bdl_entry_t *)(uintptr_t)s_hda.bdl_pa;

    for (uint32_t i = 0; i < HDA_RING_PAGES; i++) {
        s_hda.ring_pa[i] = vmm_alloc_frame();
        if (!s_hda.ring_pa[i]) return -8;
        s_hda.ring_va[i] = (uint8_t *)(uintptr_t)s_hda.ring_pa[i];
        s_hda.bdl[i].addr = s_hda.ring_pa[i];
        s_hda.bdl[i].len  = HDA_PAGE_BYTES;
        s_hda.bdl[i].ioc  = 1u;
    }

    s_hda.present = true;
    hda_set_volume(s_hda.volume);

    serial_printf("[hda] pci=%04X:%04X mmio=0x%llX codec=%08X afg=%u dac=%u pin=%u oss=%u\n",
                  s_hda.vendor, s_hda.device, (unsigned long long)s_hda.mmio_base,
                  s_hda.codec_vendor, s_hda.afg_nid, s_hda.dac_nid, s_hda.pin_nid, s_hda.oss);
    return 0;
}

bool     hda_present(void) { return s_hda.present; }
uint32_t hda_rate(void)    { return s_hda.rate; }
bool     hda_playing(void) { return s_hda.playing; }

void hda_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_hda.volume = percent;
    if (!s_hda.present) return;

    if (percent == 0) {
        hda_verb4(s_hda.cad, s_hda.dac_nid, HDA_VERB_SET_AMP_GAIN, 0xB080u);
        hda_verb4(s_hda.cad, s_hda.pin_nid, HDA_VERB_SET_AMP_GAIN, 0xB080u);
        return;
    }

    if (s_hda.dac_has_amp) {
        uint8_t max_g = s_hda.dac_amp_steps ? s_hda.dac_amp_steps : 0x3Fu;
        uint8_t g = (uint8_t)(((uint32_t)percent * max_g) / 100u);
        hda_unmute_amp(s_hda.cad, s_hda.dac_nid, true, false, 0, g);
    } else {
        hda_unmute_amp(s_hda.cad, s_hda.dac_nid, true, false, 0, 0x3Fu);
    }

    if (s_hda.pin_has_amp) {
        uint8_t max_g = s_hda.pin_amp_steps ? s_hda.pin_amp_steps : 0x3Fu;
        uint8_t g = (uint8_t)(((uint32_t)percent * max_g) / 100u);
        hda_unmute_amp(s_hda.cad, s_hda.pin_nid, true, false, 0, g);
    } else {
        hda_unmute_amp(s_hda.cad, s_hda.pin_nid, true, false, 0, 0x3Fu);
    }
}

void hda_stop(void) {
    if (!s_hda.present) return;
    hda_reset_stream();
    s_hda.playing = false;
    s_hda.src = 0;
    s_hda.src_len = 0;
    s_hda.src_pos = 0;
}

int hda_play_pcm(const void *pcm_s16_stereo, uint32_t bytes, uint32_t sample_rate) {
    if (!s_hda.present || !pcm_s16_stereo || bytes == 0) return -1;
    if (sample_rate == 0) sample_rate = 48000u;

    hda_stop();

    s_hda.src = (const uint8_t *)pcm_s16_stereo;
    s_hda.src_len = bytes;
    s_hda.src_pos = 0;
    s_hda.rate = sample_rate;
    s_hda.played_bytes = 0;
    s_hda.last_lpib = 0;
    s_hda.total_target_bytes = (uint64_t)bytes + HDA_PAGE_BYTES;

    uint16_t fmt = hda_encode_fmt(sample_rate);
    hda_verb4(s_hda.cad, s_hda.dac_nid, HDA_VERB_SET_CONV_FMT, fmt);

    for (uint32_t i = 0; i < HDA_RING_PAGES; i++) {
        hda_fill_ring_page(i);
        s_hda.bdl[i].addr = s_hda.ring_pa[i];
        s_hda.bdl[i].len  = HDA_PAGE_BYTES;
        s_hda.bdl[i].ioc  = 1u;
    }
    s_hda.fill_page = 0;
    s_hda.active_cbl = HDA_RING_BYTES;

    __asm__ volatile("mfence" ::: "memory");

    uint32_t sd = s_hda.osd_off;
    hda_w32(sd + HDA_SD_CBL, s_hda.active_cbl);
    hda_w16(sd + HDA_SD_LVI, (uint16_t)(HDA_RING_PAGES - 1u));
    hda_w16(sd + HDA_SD_FMT, fmt);
    hda_w32(sd + HDA_SD_BDLPL, (uint32_t)(s_hda.bdl_pa & 0xFFFFFFFFull));
    hda_w32(sd + HDA_SD_BDLPU, (uint32_t)(s_hda.bdl_pa >> 32));
    hda_w8(sd + HDA_SD_STS, HDA_SD_STS_BCIS | HDA_SD_STS_FIFOE | HDA_SD_STS_DESE);

    uint32_t ctl = ((uint32_t)HDA_STREAM_ID << 20) | HDA_SD_CTL_RUN | HDA_SD_CTL_IOCE;
    hda_w32(sd + HDA_SD_CTL, ctl);
    s_hda.playing = true;

    serial_printf("[hda] stream start rate=%u bytes=%u fmt=0x%04X\n",
                  sample_rate, bytes, fmt);
    return 0;
}

void hda_poll(void) {
    if (!s_hda.present || !s_hda.playing) return;

    uint32_t sd = s_hda.osd_off;
    uint8_t sts = hda_r8(sd + HDA_SD_STS);
    if (sts & (HDA_SD_STS_BCIS | HDA_SD_STS_FIFOE | HDA_SD_STS_DESE))
        hda_w8(sd + HDA_SD_STS, sts);

    uint32_t lpib = hda_r32(sd + HDA_SD_LPIB);
    if (lpib >= s_hda.active_cbl) lpib %= s_hda.active_cbl;

    uint32_t delta = (lpib >= s_hda.last_lpib)
                     ? (lpib - s_hda.last_lpib)
                     : (s_hda.active_cbl - s_hda.last_lpib + lpib);
    s_hda.last_lpib = lpib;
    s_hda.played_bytes += delta;

    uint32_t cur_page = lpib / HDA_PAGE_BYTES;
    while (s_hda.fill_page != cur_page) {
        hda_fill_ring_page(s_hda.fill_page);
        s_hda.fill_page = (s_hda.fill_page + 1u) % HDA_RING_PAGES;
    }

    if (s_hda.src_pos >= s_hda.src_len &&
        s_hda.played_bytes >= s_hda.total_target_bytes) {
        hda_stop();
    }
}
