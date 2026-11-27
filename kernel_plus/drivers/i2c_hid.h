/**
 * Arctian Intel LPSS / DesignWare I2C-HID Touchpad Driver (Header-Only)
 *
 * Included by mouse.c so Makefile does not require modification.
 * Scans PCI for Intel LPSS / DesignWare I2C controllers (Skylake through
 * RaptorLake/MeteorLake), probes MMIO safely with bounded timeouts (no-op
 * when absent in QEMU), and provides an I2C-HID descriptor + report parser.
 */
#ifndef ARCTIAN_I2C_HID_H
#define ARCTIAN_I2C_HID_H

#include <stdint.h>
#include <stdbool.h>
#include "pci.h"
#include "mmio.h"
#include "serial.h"

/* DesignWare APB I2C register offsets */
#define DW_IC_CON              0x00
#define DW_IC_TAR              0x04
#define DW_IC_DATA_CMD         0x10
#define DW_IC_SS_SCL_HCNT      0x14
#define DW_IC_SS_SCL_LCNT      0x18
#define DW_IC_FS_SCL_HCNT      0x1C
#define DW_IC_FS_SCL_LCNT      0x20
#define DW_IC_INTR_MASK        0x30
#define DW_IC_CLR_INTR         0x40
#define DW_IC_ENABLE           0x6C
#define DW_IC_STATUS           0x70
#define DW_IC_TXFLR            0x74
#define DW_IC_RXFLR            0x78
#define DW_IC_ENABLE_STATUS    0x9C
#define DW_IC_COMP_TYPE        0xFC

#define DW_IC_COMP_TYPE_VAL    0x44570140u
#define DW_IC_STATUS_TFNF      (1u << 1)
#define DW_IC_STATUS_TFE       (1u << 2)
#define DW_IC_STATUS_RFNE      (1u << 3)
#define DW_IC_STATUS_ACTIVITY  (1u << 0)

#define DW_IC_DATA_CMD_READ    (1u << 8)
#define DW_IC_DATA_CMD_STOP    (1u << 9)

#define LPSS_PRIV_RESETS       0x204
#define I2C_HID_MAX_CTRL       4
#define I2C_HID_TIMEOUT        10000

typedef struct __attribute__((packed)) {
    uint16_t wHIDDescLength;
    uint16_t bcdVersion;
    uint16_t wReportDescLength;
    uint16_t wReportDescRegister;
    uint16_t wInputRegister;
    uint16_t wMaxInputLength;
    uint16_t wOutputRegister;
    uint16_t wMaxOutputLength;
    uint16_t wCommandRegister;
    uint16_t wDataRegister;
    uint16_t wVendorID;
    uint16_t wProductID;
    uint16_t wVersionID;
    uint32_t reserved;
} i2c_hid_desc_t;

typedef struct {
    bool           present;
    bool           ready;
    uint64_t       mmio_base;
    uint64_t       mmio_size;
    uint16_t       pci_vendor;
    uint16_t       pci_device;
    uint8_t        slave_addr;
    i2c_hid_desc_t desc;
    bool           abs_valid;
    int            prev_x;
    int            prev_y;
} i2c_hid_state_t;

static i2c_hid_state_t s_i2c_hid;

static inline uint32_t i2c_mmio_r32(uint64_t base, uint32_t off) {
    return *(volatile uint32_t *)(uintptr_t)(base + off);
}

static inline void i2c_mmio_w32(uint64_t base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(uintptr_t)(base + off) = val;
}

static inline bool i2c_is_intel_lpss_id(uint16_t vendor, uint16_t device) {
    if (vendor != PCI_VENDOR_INTEL) return false;
    switch (device) {
        /* Skylake / Sunrise Point-LP & H */
        case 0x9D60: case 0x9D61: case 0x9D62: case 0x9D63:
        case 0x9D64: case 0x9D65:
        case 0xA160: case 0xA161: case 0xA162: case 0xA163:
        /* Kaby Lake / Cannon Lake */
        case 0x9DC5: case 0x9DC6:
        case 0x9DE8: case 0x9DE9: case 0x9DEA: case 0x9DEB:
        case 0xA368: case 0xA369: case 0xA36A: case 0xA36B:
        /* Comet Lake */
        case 0x02C5: case 0x02C6:
        case 0x02E8: case 0x02E9: case 0x02EA: case 0x02EB:
        case 0x06E8: case 0x06E9: case 0x06EA: case 0x06EB:
        /* Ice Lake */
        case 0x34C5: case 0x34C6:
        case 0x34E8: case 0x34E9: case 0x34EA: case 0x34EB:
        /* Tiger Lake-LP & H */
        case 0xA0C5: case 0xA0C6:
        case 0xA0E8: case 0xA0E9: case 0xA0EA: case 0xA0EB:
        case 0x43AD: case 0x43AE: case 0x43D8:
        case 0x43E8: case 0x43E9: case 0x43EA: case 0x43EB:
        /* Jasper Lake / Elkhart Lake */
        case 0x4DC5: case 0x4DC6:
        case 0x4DE8: case 0x4DE9: case 0x4DEA: case 0x4DEB:
        /* Alder Lake */
        case 0x51C5: case 0x51C6:
        case 0x51E8: case 0x51E9: case 0x51EA: case 0x51EB:
        case 0x54C5: case 0x54C6:
        case 0x54E8: case 0x54E9: case 0x54EA: case 0x54EB:
        /* Raptor Lake / Meteor Lake */
        case 0x7A4C: case 0x7A4D: case 0x7A4E: case 0x7A4F:
        case 0x7A7C: case 0x7A7D:
        case 0x7E50: case 0x7E51:
        case 0x7E78: case 0x7E79: case 0x7E7A: case 0x7E7B:
            return true;
        default:
            return false;
    }
}

static inline bool dw_i2c_wait_disabled(uint64_t base) {
    for (int i = 0; i < I2C_HID_TIMEOUT; i++) {
        if ((i2c_mmio_r32(base, DW_IC_ENABLE_STATUS) & 1u) == 0)
            return true;
    }
    return false;
}

static inline bool dw_i2c_xfer_read(uint64_t base, uint8_t addr,
                                    const uint8_t *wbuf, int wlen,
                                    uint8_t *rbuf, int rlen) {
    if (!base) return false;

    i2c_mmio_w32(base, DW_IC_ENABLE, 0);
    if (!dw_i2c_wait_disabled(base)) return false;

    i2c_mmio_w32(base, DW_IC_TAR, (uint32_t)(addr & 0x7Fu));
    (void)i2c_mmio_r32(base, DW_IC_CLR_INTR);
    i2c_mmio_w32(base, DW_IC_ENABLE, 1);

    for (int i = 0; i < wlen; i++) {
        int t = I2C_HID_TIMEOUT;
        while (t-- > 0 && !(i2c_mmio_r32(base, DW_IC_STATUS) & DW_IC_STATUS_TFNF)) {}
        if (t <= 0) return false;
        uint32_t cmd = (uint32_t)wbuf[i];
        if (rlen == 0 && i == wlen - 1) cmd |= DW_IC_DATA_CMD_STOP;
        i2c_mmio_w32(base, DW_IC_DATA_CMD, cmd);
    }

    int issued = 0;
    int recvd = 0;
    int guard = I2C_HID_TIMEOUT;
    while (recvd < rlen && guard-- > 0) {
        if (issued < rlen && (i2c_mmio_r32(base, DW_IC_STATUS) & DW_IC_STATUS_TFNF)) {
            uint32_t cmd = DW_IC_DATA_CMD_READ;
            if (issued == rlen - 1) cmd |= DW_IC_DATA_CMD_STOP;
            i2c_mmio_w32(base, DW_IC_DATA_CMD, cmd);
            issued++;
        }
        if (i2c_mmio_r32(base, DW_IC_STATUS) & DW_IC_STATUS_RFNE) {
            rbuf[recvd++] = (uint8_t)(i2c_mmio_r32(base, DW_IC_DATA_CMD) & 0xFFu);
        }
    }

    return recvd == rlen;
}

static inline bool i2c_hid_probe_controller(uint64_t base, uint16_t vendor, uint16_t dev_id) {
    /* Verify DesignWare IP block signature safely */
    uint32_t comp = i2c_mmio_r32(base, DW_IC_COMP_TYPE);
    if (comp != DW_IC_COMP_TYPE_VAL) {
        /* On some LPSS variants, private reset register must be released first */
        i2c_mmio_w32(base, LPSS_PRIV_RESETS, 0x07u);
        comp = i2c_mmio_r32(base, DW_IC_COMP_TYPE);
        if (comp != DW_IC_COMP_TYPE_VAL) return false;
    }

    /* Configure Master, 400 kHz Fast-mode, 7-bit addressing, restart enabled */
    i2c_mmio_w32(base, DW_IC_ENABLE, 0);
    if (!dw_i2c_wait_disabled(base)) return false;
    i2c_mmio_w32(base, DW_IC_INTR_MASK, 0);
    i2c_mmio_w32(base, DW_IC_CON, 0x65u); /* Master | Fast | Restart | SlaveDisable */
    i2c_mmio_w32(base, DW_IC_FS_SCL_HCNT, 120u);
    i2c_mmio_w32(base, DW_IC_FS_SCL_LCNT, 160u);

    /* Probe common I2C-HID touchpad slave addresses and HID descriptor registers */
    static const uint8_t addrs[] = { 0x15, 0x2C, 0x20, 0x38, 0x4C };
    static const uint16_t regs[] = { 0x0001, 0x0020 };

    for (unsigned ai = 0; ai < sizeof(addrs); ai++) {
        for (unsigned ri = 0; ri < 2u; ri++) {
            uint8_t reg_le[2] = { (uint8_t)(regs[ri] & 0xFFu), (uint8_t)(regs[ri] >> 8) };
            i2c_hid_desc_t desc;
            if (!dw_i2c_xfer_read(base, addrs[ai], reg_le, 2,
                                  (uint8_t *)&desc, (int)sizeof(desc)))
                continue;
            if (desc.wHIDDescLength == 30 && desc.bcdVersion == 0x0100 &&
                desc.wMaxInputLength >= 4 && desc.wMaxInputLength <= 64) {
                s_i2c_hid.present = true;
                s_i2c_hid.ready = true;
                s_i2c_hid.mmio_base = base;
                s_i2c_hid.pci_vendor = vendor;
                s_i2c_hid.pci_device = dev_id;
                s_i2c_hid.slave_addr = addrs[ai];
                s_i2c_hid.desc = desc;
                s_i2c_hid.abs_valid = false;
                serial_printf("[i2c-hid] touchpad found pci=%04x:%04x addr=0x%02x vid=0x%04x pid=0x%04x\n",
                              vendor, dev_id, addrs[ai], desc.wVendorID, desc.wProductID);
                return true;
            }
        }
    }
    return false;
}

static inline bool i2c_hid_init(void) {
    s_i2c_hid.present = false;
    s_i2c_hid.ready = false;

    int found_ctrl = 0;
    for (int b = 0; b < 256 && found_ctrl < I2C_HID_MAX_CTRL; b++) {
        for (int s = 0; s < 32 && found_ctrl < I2C_HID_MAX_CTRL; s++) {
            uint16_t v0 = pci_config_readw((uint8_t)b, (uint8_t)s, 0, 0);
            if (v0 == 0xFFFFu) continue;
            uint8_t hdr = pci_config_readb((uint8_t)b, (uint8_t)s, 0, 0x0E);
            int maxf = (hdr & 0x80u) ? 8 : 1;
            for (int f = 0; f < maxf; f++) {
                uint16_t vendor = pci_config_readw((uint8_t)b, (uint8_t)s, (uint8_t)f, 0);
                if (vendor == 0xFFFFu) continue;
                uint16_t dev_id = pci_config_readw((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x02);
                if (!i2c_is_intel_lpss_id(vendor, dev_id)) continue;

                found_ctrl++;
                pci_bar_t bar0 = pci_read_bar((uint8_t)b, (uint8_t)s, (uint8_t)f, 0);
                if (bar0.is_io || bar0.address == 0 || bar0.address == 0xFFFFFFFFULL)
                    continue;
                uint64_t sz = bar0.size ? bar0.size : 0x1000ULL;
                if (mmio_map(bar0.address, sz) != 0) continue;
                pci_enable_bus_mastering((uint8_t)b, (uint8_t)s, (uint8_t)f);
                s_i2c_hid.mmio_size = sz;

                if (i2c_hid_probe_controller(bar0.address, vendor, dev_id))
                    return true;
            }
        }
    }

    return false;
}

static inline bool i2c_hid_present(void) {
    return s_i2c_hid.ready;
}

/**
 * Parse an I2C-HID input report packet:
 *   pkt[0..1] = little-endian packet length (0 = no data / idle)
 *   pkt[2]    = Report ID
 *   pkt[3..]  = payload (relative mouse or PTP absolute touchpad)
 */
static inline bool i2c_hid_parse_report(const uint8_t *pkt, int len,
                                        int *dx, int *dy, uint8_t *buttons, int *scroll) {
    if (!pkt || len < 6) return false;
    uint16_t pkt_len = (uint16_t)pkt[0] | ((uint16_t)pkt[1] << 8);
    if (pkt_len == 0 || pkt_len > (uint16_t)len || pkt_len < 6) return false;

    *dx = 0;
    *dy = 0;
    *scroll = 0;
    *buttons = 0;

    if (pkt_len <= 8) {
        /* Relative HID mouse report stub */
        *buttons = pkt[3] & 0x07u;
        *dx = (int)(int8_t)pkt[4];
        *dy = -(int)(int8_t)pkt[5];
        if (pkt_len >= 7) *scroll = (int)(int8_t)pkt[6];
        return true;
    }

    /* PTP / absolute touchpad report stub:
     * pkt[3] bits: bit0=button/click, bit1=tip switch, pkt[4..5]=X, pkt[6..7]=Y */
    uint8_t flags = pkt[3];
    bool tip = (flags & 0x03u) != 0;
    *buttons = (flags & 0x01u) ? 1 : 0;
    int ax = (int)((uint16_t)pkt[4] | ((uint16_t)pkt[5] << 8));
    int ay = (int)((uint16_t)pkt[6] | ((uint16_t)pkt[7] << 8));

    if (!tip) {
        s_i2c_hid.abs_valid = false;
        return true;
    }
    if (!s_i2c_hid.abs_valid) {
        s_i2c_hid.abs_valid = true;
        s_i2c_hid.prev_x = ax;
        s_i2c_hid.prev_y = ay;
        return true;
    }
    *dx = (ax - s_i2c_hid.prev_x) / 4;
    *dy = -(ay - s_i2c_hid.prev_y) / 4;
    s_i2c_hid.prev_x = ax;
    s_i2c_hid.prev_y = ay;
    return true;
}

static inline bool i2c_hid_poll_event(int *dx, int *dy, uint8_t *buttons, int *scroll) {
    if (!s_i2c_hid.ready) return false;
    uint8_t buf[64];
    int n = (int)s_i2c_hid.desc.wMaxInputLength;
    if (n > (int)sizeof(buf)) n = (int)sizeof(buf);
    if (!dw_i2c_xfer_read(s_i2c_hid.mmio_base, s_i2c_hid.slave_addr, NULL, 0, buf, n))
        return false;
    return i2c_hid_parse_report(buf, n, dx, dy, buttons, scroll);
}

#endif /* ARCTIAN_I2C_HID_H */
