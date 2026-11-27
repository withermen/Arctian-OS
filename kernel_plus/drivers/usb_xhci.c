#include "kernel.h"
#include "usb_xhci.h"
#include "usb.h"
#include "usb_hid.h"
#include "pci.h"
#include "serial.h"
#include "alloc.h"
#include "spinlock.h"

/* Capability registers (cap_base) */
#define XHCI_CAPLENGTH       0x00
#define XHCI_HCSPARAMS1      0x04
#define XHCI_HCSPARAMS2      0x08
#define XHCI_HCSPARAMS3      0x0C
#define XHCI_HCCPARAMS1      0x10
#define XHCI_DBOFF           0x14
#define XHCI_RTSOFF          0x18
#define XHCI_HCCPARAMS2      0x1C

/* Operational registers (op_base = cap_base + cap_length) */
#define XHCI_USBCMD          0x00
#define XHCI_USBSTS          0x04
#define XHCI_PAGESIZE        0x08
#define XHCI_DNCTRL          0x14
#define XHCI_CRCR            0x18
#define XHCI_DCBAAP          0x30
#define XHCI_CONFIG          0x38

#define XHCI_USBSTS_HCH      (1u << 0)
#define XHCI_USBSTS_HSE      (1u << 2)
#define XHCI_USBSTS_EINT     (1u << 3)
#define XHCI_USBSTS_PCD      (1u << 4)
#define XHCI_USBSTS_CNR      (1u << 11)

/* Port registers (port_base = op_base + 0x400 + port * 0x10) */
#define XHCI_PORTSC          0x00
#define XHCI_PORTPMSC        0x04
#define XHCI_PORTLI          0x08

#define XHCI_PORTSC_CCS      (1u << 0)
#define XHCI_PORTSC_PED      (1u << 1)
#define XHCI_PORTSC_OCA      (1u << 3)
#define XHCI_PORTSC_RESET    (1u << 4)
#define XHCI_PORTSC_PLS_MASK (0x0Fu << 5)
#define XHCI_PORTSC_PP       (1u << 9)
#define XHCI_PORTSC_SPD_SHIFT 10
#define XHCI_PORTSC_SPD_MASK (0x0Fu << 10)
#define XHCI_PORTSC_CSC      (1u << 17)
#define XHCI_PORTSC_PEC      (1u << 18)
#define XHCI_PORTSC_WRC      (1u << 19)
#define XHCI_PORTSC_OCC      (1u << 20)
#define XHCI_PORTSC_PRC      (1u << 21)
#define XHCI_PORTSC_PLC      (1u << 22)
#define XHCI_PORTSC_CEC      (1u << 23)
#define XHCI_PORTSC_WPR      (1u << 31)
#define XHCI_PORTSC_RW1C_MASK (XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | XHCI_PORTSC_WRC | \
                               XHCI_PORTSC_OCC | XHCI_PORTSC_PRC | XHCI_PORTSC_PLC | \
                               XHCI_PORTSC_CEC)

/* Runtime Interrupter 0 registers (rt_base + offset) */
#define XHCI_IR0_IMAN        0x20
#define XHCI_IR0_IMOD        0x24
#define XHCI_IR0_ERSTSZ      0x28
#define XHCI_IR0_ERSTBA      0x30
#define XHCI_IR0_ERDP        0x38

/* TRB types */
#define XHCI_TRB_TYPE_NORMAL         1u
#define XHCI_TRB_TYPE_SETUP          2u
#define XHCI_TRB_TYPE_DATA           3u
#define XHCI_TRB_TYPE_STATUS         4u
#define XHCI_TRB_TYPE_LINK           6u
#define XHCI_TRB_TYPE_ENABLE_SLOT    9u
#define XHCI_TRB_TYPE_DISABLE_SLOT   10u
#define XHCI_TRB_TYPE_ADDRESS_DEV    11u
#define XHCI_TRB_TYPE_CONFIGURE_EP   12u
#define XHCI_TRB_TYPE_EVAL_CONTEXT   13u
#define XHCI_TRB_TYPE_RESET_EP       14u
#define XHCI_TRB_TYPE_STOP_EP        15u
#define XHCI_TRB_TYPE_SET_TR_DEQUEUE 16u
#define XHCI_TRB_TYPE_TRANSFER_EVT   32u
#define XHCI_TRB_TYPE_CMD_COMP_EVT   33u
#define XHCI_TRB_TYPE_PORT_CHG_EVT   34u

#define XHCI_TRB_CYCLE     (1u << 0)
#define XHCI_TRB_TC        (1u << 1)
#define XHCI_TRB_ISP       (1u << 2)
#define XHCI_TRB_CH        (1u << 4)
#define XHCI_TRB_IOC       (1u << 5)
#define XHCI_TRB_IDT       (1u << 6)

#define XHCI_CC_SUCCESS      1u
#define XHCI_CC_STALL        6u
#define XHCI_CC_SHORT_PACKET 13u

#define XHCI_MAX_SLOTS       32
#define XHCI_MAX_PORTS       32
#define XHCI_MAX_DCI         32
#define XHCI_TRB_RING_SIZE   256
#define XHCI_DMA_SIZE        4096

#pragma pack(push, 1)
typedef struct {
    uint64_t ptr;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

typedef struct {
    uint64_t ring_seg_base;
    uint32_t ring_seg_size;
    uint32_t rsvd;
} xhci_erst_entry_t;
#pragma pack(pop)

typedef struct {
    xhci_trb_t *ring;
    uint64_t    ring_phys;
    int         cycle;
    int         enqueue_idx;
} xhci_ring_t;

typedef struct {
    uint64_t cap_base;
    uint64_t op_base;
    uint64_t port_base;
    uint64_t db_base;
    uint64_t rt_base;

    int      n_slots;
    int      n_ports;
    int      csz;           /* 0 = 32B context, 1 = 64B context */
    int      ppc;           /* Port Power Control */
    uint8_t  port_proto[XHCI_MAX_PORTS]; /* 2 = USB2, 3 = USB3 */

    xhci_ring_t cmd_ring;
    xhci_trb_t *evt_ring;
    uint64_t    evt_ring_phys;
    int         evt_cycle;
    int         evt_idx;

    /* Event demux state */
    volatile int      cmd_done;
    volatile uint8_t  cmd_cc;
    volatile uint8_t  cmd_slot;
    volatile uint64_t cmd_ptr;

    volatile int      xfer_done[XHCI_MAX_SLOTS][XHCI_MAX_DCI];
    volatile uint8_t  xfer_cc[XHCI_MAX_SLOTS][XHCI_MAX_DCI];
    volatile uint32_t xfer_res[XHCI_MAX_SLOTS][XHCI_MAX_DCI];

    volatile int      port_changed;
} xhci_ctrl_t;

static xhci_ctrl_t xhci;
static bool xhci_ok = false;
static spinlock_t xhci_lock = SPINLOCK_INIT;

/* Aligned DMA structures in BSS (identity mapped < 16MB) */
static uint64_t          dcbaap_storage[XHCI_MAX_SLOTS + 1] __attribute__((aligned(64)));
static uint8_t           dev_ctx_pool[XHCI_MAX_SLOTS][64 * 32] __attribute__((aligned(64)));
static uint8_t           input_ctx_pool[64 * 33] __attribute__((aligned(64)));
static xhci_trb_t        cmd_ring_storage[XHCI_TRB_RING_SIZE] __attribute__((aligned(4096)));
static xhci_trb_t        evt_ring_storage[XHCI_TRB_RING_SIZE] __attribute__((aligned(4096)));
static xhci_erst_entry_t erst_storage[1] __attribute__((aligned(64)));
static uint8_t           s_xhci_dma[XHCI_DMA_SIZE] __attribute__((aligned(4096)));
static uint64_t          s_scratchpad_arr[64] __attribute__((aligned(64)));

/* Per-slot transfer rings (allocated on demand via kmalloc, 4KB aligned) */
static xhci_ring_t       xhci_ep_rings[XHCI_MAX_SLOTS][XHCI_MAX_DCI];
static bool              xhci_ep_rings_init[XHCI_MAX_SLOTS][XHCI_MAX_DCI];
static uint8_t           xhci_intr_armed[XHCI_MAX_SLOTS][XHCI_MAX_DCI];
static uint8_t           xhci_intr_buf[XHCI_MAX_SLOTS][XHCI_MAX_DCI][64] __attribute__((aligned(64)));

static int               xhci_slot_port[XHCI_MAX_SLOTS];
static int               xhci_port_slot[XHCI_MAX_PORTS];
static usb_device_t     *xhci_slot_dev[XHCI_MAX_SLOTS];

/* ---- PIT microsecond delay ---- */
static uint16_t xhci_pit_cnt(void) {
    outb(0x43, 0x00);
    uint8_t lo = inb(0x40);
    uint8_t hi = inb(0x40);
    return (uint16_t)(((uint16_t)hi << 8) | lo);
}

static void xhci_delay_us(uint32_t us) {
    uint32_t total = (uint32_t)((uint64_t)us * 1193182ULL / 1000000ULL);
    if (total == 0) total = 1;
    uint16_t prev = xhci_pit_cnt();
    uint32_t elapsed = 0;
    uint32_t guard = 0;
    while (elapsed < total && guard++ < 5000000u) {
        uint16_t cur = xhci_pit_cnt();
        if (cur <= prev) elapsed += (uint32_t)(prev - cur);
        else elapsed += (uint32_t)prev + 65536u - cur;
        prev = cur;
        __asm__ volatile("pause");
    }
}

/* ---- Virtual to physical via CR3 page walk ---- */
static uint64_t xhci_v2p(const void *ptr) {
    uint64_t va = (uint64_t)(uintptr_t)ptr;
    if (va < 0x40000000ULL) return va; /* < 1GB identity mapped */
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t *pml4 = (uint64_t *)(uintptr_t)(cr3 & 0x000FFFFFFFFFF000ULL);
    uint64_t e = pml4[(va >> 39) & 0x1FFu];
    if (!(e & 1u)) return va;
    if (e & 0x80u) return (e & 0x000FFFFFC0000000ULL) | (va & 0x3FFFFFFFULL);
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(e & 0x000FFFFFFFFFF000ULL);
    e = pdpt[(va >> 30) & 0x1FFu];
    if (!(e & 1u)) return va;
    if (e & 0x80u) return (e & 0x000FFFFFC0000000ULL) | (va & 0x3FFFFFFFULL);
    uint64_t *pd = (uint64_t *)(uintptr_t)(e & 0x000FFFFFFFFFF000ULL);
    e = pd[(va >> 21) & 0x1FFu];
    if (!(e & 1u)) return va;
    if (e & 0x80u) return (e & 0x000FFFFFFFE00000ULL) | (va & 0x1FFFFFULL);
    uint64_t *pt = (uint64_t *)(uintptr_t)(e & 0x000FFFFFFFFFF000ULL);
    e = pt[(va >> 12) & 0x1FFu];
    if (!(e & 1u)) return va;
    return (e & 0x000FFFFFFFFFF000ULL) | (va & 0xFFFULL);
}

/* ---- MMIO helpers ---- */
static inline uint32_t xhci_r32(uint64_t base, uint32_t off) {
    return *(volatile uint32_t *)(uintptr_t)(base + off);
}
static inline void xhci_w32(uint64_t base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(uintptr_t)(base + off) = val;
}
static inline void xhci_w64(uint64_t base, uint32_t off, uint64_t val) {
    *(volatile uint32_t *)(uintptr_t)(base + off) = (uint32_t)(val & 0xFFFFFFFFu);
    *(volatile uint32_t *)(uintptr_t)(base + off + 4u) = (uint32_t)(val >> 32);
}

static inline uint32_t xhci_op_r32(uint32_t off) { return xhci_r32(xhci.op_base, off); }
static inline void xhci_op_w32(uint32_t off, uint32_t v) { xhci_w32(xhci.op_base, off, v); }
static inline void xhci_op_w64(uint32_t off, uint64_t v) { xhci_w64(xhci.op_base, off, v); }

static inline uint32_t xhci_port_r32(int port, uint32_t off) {
    return xhci_r32(xhci.port_base, (uint32_t)port * 0x10u + off);
}
static inline void xhci_port_w32(int port, uint32_t off, uint32_t v) {
    xhci_w32(xhci.port_base, (uint32_t)port * 0x10u + off, v);
}

static inline void xhci_doorbell(int slot, int target) {
    __asm__ volatile("" ::: "memory");
    xhci_w32(xhci.db_base, (uint32_t)slot * 4u, (uint32_t)(target & 0xFF));
}

static int xhci_wait_op_mask(uint32_t reg, uint32_t mask, uint32_t want, int timeout_ms) {
    int steps = timeout_ms * 10;
    while (steps-- > 0) {
        if ((xhci_op_r32(reg) & mask) == want) return 0;
        xhci_delay_us(100);
    }
    return -1;
}

/* ---- Context byte-offset helpers (supports CSZ=0 32B and CSZ=1 64B) ---- */
static inline uint32_t ctx_stride(void) { return xhci.csz ? 64u : 32u; }

static inline uint32_t *ictx_ctrl(void) {
    return (uint32_t *)input_ctx_pool;
}
static inline uint32_t *ictx_slot(void) {
    return (uint32_t *)(input_ctx_pool + ctx_stride());
}
static inline uint32_t *ictx_ep(int dci) {
    return (uint32_t *)(input_ctx_pool + (uint32_t)(dci + 1) * ctx_stride());
}
static inline uint32_t *dctx_slot(int slot_id) {
    return (uint32_t *)dev_ctx_pool[slot_id];
}
static inline uint32_t *dctx_ep(int slot_id, int dci) {
    return (uint32_t *)(dev_ctx_pool[slot_id] + (uint32_t)dci * ctx_stride());
}

/* ---- Event ring draining & demux ---- */
static void xhci_poll_events_locked(void) {
    int limit = XHCI_TRB_RING_SIZE * 2;
    while (limit-- > 0) {
        volatile xhci_trb_t *evt = &xhci.evt_ring[xhci.evt_idx];
        uint32_t ctrl = evt->control;
        if ((ctrl & XHCI_TRB_CYCLE) != (uint32_t)(xhci.evt_cycle ? 1u : 0u))
            break;

        __asm__ volatile("" ::: "memory");
        uint32_t status = evt->status;
        uint64_t ptr = evt->ptr;
        uint32_t trb_type = (ctrl >> 10) & 0x3Fu;
        uint8_t  cc = (uint8_t)((status >> 24) & 0xFFu);
        uint32_t res = status & 0xFFFFFFu;
        uint8_t  slot = (uint8_t)((ctrl >> 24) & 0xFFu);
        uint8_t  ep_dci = (uint8_t)((ctrl >> 16) & 0x1Fu);

        if (trb_type == XHCI_TRB_TYPE_CMD_COMP_EVT) {
            xhci.cmd_cc = cc;
            xhci.cmd_slot = slot;
            xhci.cmd_ptr = ptr;
            xhci.cmd_done = 1;
        } else if (trb_type == XHCI_TRB_TYPE_TRANSFER_EVT) {
            if (slot < XHCI_MAX_SLOTS && ep_dci < XHCI_MAX_DCI) {
                xhci.xfer_cc[slot][ep_dci] = cc;
                xhci.xfer_res[slot][ep_dci] = res;
                xhci.xfer_done[slot][ep_dci] = 1;
            }
        } else if (trb_type == XHCI_TRB_TYPE_PORT_CHG_EVT) {
            xhci.port_changed = 1;
        }

        xhci.evt_idx++;
        if (xhci.evt_idx >= XHCI_TRB_RING_SIZE) {
            xhci.evt_idx = 0;
            xhci.evt_cycle = !xhci.evt_cycle;
        }
        uint64_t erdp = xhci.evt_ring_phys + (uint64_t)xhci.evt_idx * sizeof(xhci_trb_t);
        xhci_w64(xhci.rt_base, XHCI_IR0_ERDP, erdp | (1ULL << 3));
    }
}

/* ---- Ring management ---- */
static void xhci_ring_init(xhci_ring_t *r, xhci_trb_t *buf, uint64_t phys) {
    memset(buf, 0, sizeof(xhci_trb_t) * XHCI_TRB_RING_SIZE);
    r->ring = buf;
    r->ring_phys = phys;
    r->cycle = 1;
    r->enqueue_idx = 0;
    /* Link TRB at last slot pointing back to ring start with TC=1 */
    buf[XHCI_TRB_RING_SIZE - 1].ptr = phys;
    buf[XHCI_TRB_RING_SIZE - 1].status = 0;
    buf[XHCI_TRB_RING_SIZE - 1].control = (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_TC | XHCI_TRB_CYCLE;
}

static uint64_t xhci_ring_push(xhci_ring_t *r, uint64_t ptr, uint32_t status, uint32_t control) {
    int idx = r->enqueue_idx;
    xhci_trb_t *slot = &r->ring[idx];
    uint64_t trb_phys = r->ring_phys + (uint64_t)idx * sizeof(xhci_trb_t);

    slot->ptr = ptr;
    slot->status = status;
    __asm__ volatile("" ::: "memory");
    slot->control = (control & ~XHCI_TRB_CYCLE) | (r->cycle ? XHCI_TRB_CYCLE : 0u);

    r->enqueue_idx++;
    if (r->enqueue_idx >= XHCI_TRB_RING_SIZE - 1) {
        /* Update Link TRB cycle bit to match current cycle, then wrap and toggle */
        xhci_trb_t *lnk = &r->ring[XHCI_TRB_RING_SIZE - 1];
        lnk->ptr = r->ring_phys;
        lnk->status = 0;
        __asm__ volatile("" ::: "memory");
        lnk->control = (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_TC | (r->cycle ? XHCI_TRB_CYCLE : 0u);
        r->enqueue_idx = 0;
        r->cycle = !r->cycle;
    }
    return trb_phys;
}

static int xhci_ensure_ep_ring(int slot_id, int dci) {
    if (slot_id <= 0 || slot_id >= XHCI_MAX_SLOTS) return -1;
    if (dci <= 0 || dci >= XHCI_MAX_DCI) return -1;
    if (xhci_ep_rings_init[slot_id][dci]) return 0;

    xhci_trb_t *buf = xhci_ep_rings[slot_id][dci].ring;
    uint64_t phys = xhci_ep_rings[slot_id][dci].ring_phys;
    if (!buf) {
        void *raw = kmalloc(8192);
        if (!raw) return -1;
        uintptr_t aligned = ((uintptr_t)raw + 4095u) & ~(uintptr_t)4095u;
        buf = (xhci_trb_t *)aligned;
        phys = xhci_v2p(buf);
    }

    xhci_ring_init(&xhci_ep_rings[slot_id][dci], buf, phys);
    xhci_ep_rings_init[slot_id][dci] = true;
    xhci_intr_armed[slot_id][dci] = 0;
    return 0;
}

static int xhci_send_cmd_locked(uint64_t ptr, uint32_t status, uint32_t control, uint8_t *slot_out) {
    xhci.cmd_done = 0;
    xhci.cmd_cc = 0;
    xhci.cmd_slot = 0;

    uint64_t cphys = xhci_ring_push(&xhci.cmd_ring, ptr, status, control);
    (void)cphys;
    xhci_doorbell(0, 0);

    for (int t = 0; t < 50000; t++) {
        xhci_poll_events_locked();
        if (xhci.cmd_done) {
            if (slot_out) *slot_out = xhci.cmd_slot;
            if (xhci.cmd_cc == XHCI_CC_SUCCESS)
                return 0;
            serial_printf("[xhci] cmd err cc=%u type=%u slot=%u\n",
                          (unsigned)xhci.cmd_cc, (unsigned)((control >> 10) & 0x3Fu),
                          (unsigned)xhci.cmd_slot);
            return -1;
        }
        if (t < 200) __asm__ volatile("pause");
        else xhci_delay_us(20);
    }
    serial_printf("[xhci] cmd timeout type=%u\n", (unsigned)((control >> 10) & 0x3Fu));
    return -1;
}

static int xhci_reset_endpoint_locked(int slot_id, int dci) {
    uint32_t ctrl = ((uint32_t)slot_id << 24) | ((uint32_t)dci << 16) | (XHCI_TRB_TYPE_RESET_EP << 10);
    if (xhci_send_cmd_locked(0, 0, ctrl, 0) != 0) return -1;

    xhci_ring_t *r = &xhci_ep_rings[slot_id][dci];
    uint64_t dq = r->ring_phys + (uint64_t)r->enqueue_idx * sizeof(xhci_trb_t);
    if (r->cycle) dq |= 1ULL;
    uint32_t dq_ctrl = ((uint32_t)slot_id << 24) | ((uint32_t)dci << 16) | (XHCI_TRB_TYPE_SET_TR_DEQUEUE << 10);
    return xhci_send_cmd_locked(dq, 0, dq_ctrl, 0);
}

static int xhci_enable_slot_locked(void) {
    uint8_t slot = 0;
    if (xhci_send_cmd_locked(0, 0, (XHCI_TRB_TYPE_ENABLE_SLOT << 10), &slot) != 0)
        return -1;
    if (slot == 0 || slot >= XHCI_MAX_SLOTS) return -1;
    return (int)slot;
}

static void xhci_disable_slot_locked(int slot_id) {
    if (slot_id <= 0 || slot_id >= XHCI_MAX_SLOTS) return;
    xhci_send_cmd_locked(0, 0, ((uint32_t)slot_id << 24) | (XHCI_TRB_TYPE_DISABLE_SLOT << 10), 0);
    xhci_slot_port[slot_id] = -1;
    xhci_slot_dev[slot_id] = 0;
    for (int dci = 0; dci < XHCI_MAX_DCI; dci++) {
        xhci_ep_rings_init[slot_id][dci] = false;
        xhci_intr_armed[slot_id][dci] = 0;
        xhci.xfer_done[slot_id][dci] = 0;
    }
}

static int xhci_address_device_locked(int slot_id, int port, int xhci_speed, uint16_t ep0_mps) {
    if (xhci_ensure_ep_ring(slot_id, 1) != 0) return -1;

    memset(dev_ctx_pool[slot_id], 0, sizeof(dev_ctx_pool[slot_id]));
    dcbaap_storage[slot_id] = xhci_v2p(dev_ctx_pool[slot_id]);

    memset(input_ctx_pool, 0, sizeof(input_ctx_pool));
    uint32_t *ic = ictx_ctrl();
    ic[0] = 0;
    ic[1] = (1u << 0) | (1u << 1); /* Add Slot + EP0 (DCI 1) */

    uint32_t *sc = ictx_slot();
    sc[0] = ((uint32_t)(xhci_speed & 0x0F) << 20) | (1u << 27); /* Speed + Context Entries=1 */
    sc[1] = ((uint32_t)(port + 1) << 16);                       /* Root Hub Port Number (1-based) */

    uint32_t *ep0 = ictx_ep(1);
    uint64_t ring_phys = xhci_ep_rings[slot_id][1].ring_phys;
    ep0[0] = 0;
    ep0[1] = (3u << 1) | (4u << 3) | ((uint32_t)ep0_mps << 16); /* CErr=3, EPType=Control(4), MPS */
    ep0[2] = (uint32_t)(ring_phys & 0xFFFFFFFFu) | 1u;          /* DCS=1 */
    ep0[3] = (uint32_t)(ring_phys >> 32);
    ep0[4] = 8u;                                                /* Avg TRB len = 8 */

    uint32_t cmd = ((uint32_t)slot_id << 24) | (XHCI_TRB_TYPE_ADDRESS_DEV << 10);
    return xhci_send_cmd_locked(xhci_v2p(input_ctx_pool), 0, cmd, 0);
}

static int xhci_evaluate_ep0_locked(int slot_id, uint16_t ep0_mps) {
    memset(input_ctx_pool, 0, sizeof(input_ctx_pool));
    uint32_t *ic = ictx_ctrl();
    ic[0] = 0;
    ic[1] = (1u << 1); /* Evaluate EP0 */

    uint32_t *cur_ep0 = dctx_ep(slot_id, 1);
    uint32_t *ep0 = ictx_ep(1);
    for (int i = 0; i < 8; i++) ep0[i] = cur_ep0[i];
    ep0[1] = (ep0[1] & 0x0000FFFFu) | ((uint32_t)ep0_mps << 16);

    uint32_t cmd = ((uint32_t)slot_id << 24) | (XHCI_TRB_TYPE_EVAL_CONTEXT << 10);
    return xhci_send_cmd_locked(xhci_v2p(input_ctx_pool), 0, cmd, 0);
}

static uint32_t xhci_calc_interval(int xhci_speed, uint8_t ep_type_usb, uint8_t bInterval) {
    if (ep_type_usb == USB_EP_TYPE_BULK || ep_type_usb == USB_EP_TYPE_CONTROL)
        return 0;
    if (xhci_speed >= USB_SPEED_HIGH) {
        if (bInterval == 0) return 0;
        uint32_t v = (uint32_t)(bInterval - 1);
        return (v > 15u) ? 15u : v;
    }
    /* FS / LS: bInterval is ms (1..255). Convert to 2^k * 125us */
    uint32_t frames = bInterval ? (uint32_t)bInterval : 10u;
    uint32_t uf = frames * 8u;
    uint32_t k = 0;
    while ((1u << (k + 1u)) <= uf && k < 10u) k++;
    if (k < 3u) k = 3u;
    return k;
}

static int xhci_configure_endpoints_locked(int slot_id, usb_device_t *dev, int xhci_speed) {
    if (dev->endpoint_count <= 0) return 0;

    memset(input_ctx_pool, 0, sizeof(input_ctx_pool));
    uint32_t *ic = ictx_ctrl();
    ic[0] = 0;
    ic[1] = (1u << 0); /* Slot Context */

    uint32_t *cur_sc = dctx_slot(slot_id);
    uint32_t *sc = ictx_slot();
    for (int i = 0; i < 4; i++) sc[i] = cur_sc[i];

    int max_dci = 1;
    for (int i = 0; i < dev->endpoint_count; i++) {
        usb_endpoint_descriptor_t *ep = &dev->endpoints[i];
        uint8_t ep_num = ep->bEndpointAddress & 0x0Fu;
        if (ep_num == 0) continue;
        int dir_in = (ep->bEndpointAddress & 0x80u) ? 1 : 0;
        int dci = (int)ep_num * 2 + dir_in;
        if (dci >= XHCI_MAX_DCI) continue;

        uint8_t ep_usb = ep->bmAttributes & 0x03u;
        uint32_t x_ep_type = 0;
        if (ep_usb == USB_EP_TYPE_BULK)
            x_ep_type = dir_in ? 6u : 2u;
        else if (ep_usb == USB_EP_TYPE_INTERRUPT)
            x_ep_type = dir_in ? 7u : 3u;
        else if (ep_usb == USB_EP_TYPE_ISOCHRONOUS)
            x_ep_type = dir_in ? 5u : 1u;
        else
            continue;

        if (xhci_ensure_ep_ring(slot_id, dci) != 0) return -1;
        uint64_t ring_phys = xhci_ep_rings[slot_id][dci].ring_phys;
        uint16_t mps = ep->wMaxPacketSize & 0x07FFu;
        if (mps == 0) mps = (ep_usb == USB_EP_TYPE_BULK) ? 64u : 8u;
        uint32_t interval = xhci_calc_interval(xhci_speed, ep_usb, ep->bInterval);
        uint32_t avg_trb = (ep_usb == USB_EP_TYPE_BULK) ? 512u : (uint32_t)mps;
        uint32_t esit = (ep_usb == USB_EP_TYPE_INTERRUPT) ? (uint32_t)mps : 0u;

        ic[1] |= (1u << dci);
        uint32_t *epc = ictx_ep(dci);
        epc[0] = (interval << 16);
        epc[1] = (3u << 1) | (x_ep_type << 3) | ((uint32_t)mps << 16);
        epc[2] = (uint32_t)(ring_phys & 0xFFFFFFFFu) | 1u;
        epc[3] = (uint32_t)(ring_phys >> 32);
        epc[4] = avg_trb | (esit << 16);

        if (dci > max_dci) max_dci = dci;
    }

    if (max_dci == 1) return 0;
    sc[0] = (sc[0] & ~(0x1Fu << 27)) | ((uint32_t)max_dci << 27);

    uint32_t cmd = ((uint32_t)slot_id << 24) | (XHCI_TRB_TYPE_CONFIGURE_EP << 10);
    return xhci_send_cmd_locked(xhci_v2p(input_ctx_pool), 0, cmd, 0);
}

/* ---- Control transfer (locked) ---- */
static int xhci_control_transfer_locked(usb_device_t *dev, usb_setup_packet_t *setup_buf,
                                        uint8_t *data, int dir) {
    if (!xhci_ok || !dev || !setup_buf) return -1;
    int slot_id = dev->xhci_slot_id;
    if (slot_id <= 0 || slot_id >= XHCI_MAX_SLOTS) return -1;
    if (xhci_ensure_ep_ring(slot_id, 1) != 0) return -1;

    uint16_t data_len = setup_buf->wLength;
    if (data_len > XHCI_DMA_SIZE) data_len = XHCI_DMA_SIZE;

    uint64_t setup_imm = 0;
    memcpy(&setup_imm, setup_buf, 8);

    uint32_t trt = 0;
    if (data_len > 0 && data) {
        trt = (dir == USB_DIR_IN) ? 3u : 2u;
        if (dir == USB_DIR_OUT)
            memcpy(s_xhci_dma, data, data_len);
        else
            memset(s_xhci_dma, 0, data_len);
    }

    xhci_ring_t *r = &xhci_ep_rings[slot_id][1];
    xhci.xfer_done[slot_id][1] = 0;

    /* 1) Setup Stage TRB (IDT = 1, Immediate Data in ptr) */
    xhci_ring_push(r, setup_imm, 8u,
                   (XHCI_TRB_TYPE_SETUP << 10) | XHCI_TRB_IDT | (trt << 16));

    /* 2) Data Stage TRB (optional) */
    if (data_len > 0 && data) {
        uint32_t ddir = (dir == USB_DIR_IN) ? (1u << 16) : 0u;
        uint32_t disp = (dir == USB_DIR_IN) ? XHCI_TRB_ISP : 0u;
        xhci_ring_push(r, xhci_v2p(s_xhci_dma), (uint32_t)data_len,
                       (XHCI_TRB_TYPE_DATA << 10) | ddir | disp);
    }

    /* 3) Status Stage TRB */
    uint32_t sdir = (data_len > 0 && data && dir == USB_DIR_IN) ? 0u : (1u << 16);
    xhci_ring_push(r, 0, 0, (XHCI_TRB_TYPE_STATUS << 10) | XHCI_TRB_IOC | sdir);

    xhci_doorbell(slot_id, 1);

    for (int t = 0; t < 50000; t++) {
        xhci_poll_events_locked();
        if (xhci.xfer_done[slot_id][1]) {
            uint8_t cc = xhci.xfer_cc[slot_id][1];
            if (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET) {
                if (data_len > 0 && data && dir == USB_DIR_IN) {
                    uint32_t got = data_len;
                    if (xhci.xfer_res[slot_id][1] <= data_len)
                        got = data_len - xhci.xfer_res[slot_id][1];
                    memcpy(data, s_xhci_dma, got);
                }
                return 0;
            }
            if (cc == XHCI_CC_STALL) xhci_reset_endpoint_locked(slot_id, 1);
            serial_printf("[xhci] ctrl fail slot=%d cc=%u\n", slot_id, (unsigned)cc);
            return -1;
        }
        if (t < 200) __asm__ volatile("pause");
        else xhci_delay_us(20);
    }
    serial_printf("[xhci] ctrl timeout slot=%d\n", slot_id);
    return -1;
}

int xhci_control_transfer(usb_device_t *dev, usb_setup_packet_t *setup,
                          uint8_t *data, int dir) {
    uint64_t f = irq_save();
    spin_lock(&xhci_lock);
    int r = xhci_control_transfer_locked(dev, setup, data, dir);
    spin_unlock(&xhci_lock);
    irq_restore(f);
    return r;
}

/* ---- Bulk transfer ---- */
static int xhci_bulk_transfer_locked(usb_device_t *dev, int ep_addr, void *data, int len, int dir) {
    if (!xhci_ok || !dev || len < 0) return -1;
    int slot_id = dev->xhci_slot_id;
    if (slot_id <= 0 || slot_id >= XHCI_MAX_SLOTS) return -1;

    int ep_num = ep_addr & 0x0F;
    if (ep_num == 0) return -1;
    int dir_in = ((ep_addr & 0x80) || dir == USB_DIR_IN) ? 1 : 0;
    int dci = ep_num * 2 + dir_in;
    if (dci >= XHCI_MAX_DCI) return -1;
    if (!xhci_ep_rings_init[slot_id][dci]) {
        if (xhci_ensure_ep_ring(slot_id, dci) != 0) return -1;
    }

    uint8_t *p = (uint8_t *)data;
    int rem = len;
    do {
        int chunk = rem > XHCI_DMA_SIZE ? XHCI_DMA_SIZE : rem;
        if (chunk > 0 && !dir_in && p)
            memcpy(s_xhci_dma, p, (size_t)chunk);

        xhci_ring_t *r = &xhci_ep_rings[slot_id][dci];
        xhci.xfer_done[slot_id][dci] = 0;

        uint32_t ctrl = (XHCI_TRB_TYPE_NORMAL << 10) | XHCI_TRB_IOC | (dir_in ? XHCI_TRB_ISP : 0u);
        xhci_ring_push(r, chunk > 0 ? xhci_v2p(s_xhci_dma) : 0, (uint32_t)chunk, ctrl);
        xhci_doorbell(slot_id, dci);

        int ok = 0;
        for (int t = 0; t < 150000; t++) {
            xhci_poll_events_locked();
            if (xhci.xfer_done[slot_id][dci]) {
                uint8_t cc = xhci.xfer_cc[slot_id][dci];
                if (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET) {
                    if (chunk > 0 && dir_in && p) {
                        int actual = chunk - (int)xhci.xfer_res[slot_id][dci];
                        if (actual < 0) actual = 0;
                        if (actual > chunk) actual = chunk;
                        memcpy(p, s_xhci_dma, (size_t)actual);
                    }
                    ok = 1;
                    break;
                }
                if (cc == XHCI_CC_STALL) {
                    xhci_reset_endpoint_locked(slot_id, dci);
                    return -2;
                }
                serial_printf("[xhci] bulk err slot=%d dci=%d cc=%u\n", slot_id, dci, (unsigned)cc);
                return -1;
            }
            if (t < 300) __asm__ volatile("pause");
            else xhci_delay_us(20);
        }
        if (!ok) {
            serial_printf("[xhci] bulk timeout slot=%d dci=%d\n", slot_id, dci);
            return -1;
        }

        if (p) p += chunk;
        rem -= chunk;
    } while (rem > 0);

    return 0;
}

int xhci_bulk_transfer(usb_device_t *dev, int ep_addr, void *data, int len, int dir) {
    uint64_t f = irq_save();
    spin_lock(&xhci_lock);
    int r = xhci_bulk_transfer_locked(dev, ep_addr, data, len, dir);
    spin_unlock(&xhci_lock);
    irq_restore(f);
    return r;
}

/* ---- Non-blocking Interrupt IN polling (pre-posted Normal TRB) ---- */
static int xhci_find_intr_in_dci(usb_device_t *dev, int *mps_out) {
    int ep_num = 1;
    int mps = 8;
    for (int i = 0; i < dev->endpoint_count; i++) {
        if ((dev->endpoints[i].bEndpointAddress & 0x80u) &&
            (dev->endpoints[i].bmAttributes & 0x03u) == USB_EP_TYPE_INTERRUPT) {
            ep_num = dev->endpoints[i].bEndpointAddress & 0x0Fu;
            mps = dev->endpoints[i].wMaxPacketSize & 0x07FFu;
            if (mps == 0 || mps > 64) mps = 8;
            break;
        }
    }
    if (mps_out) *mps_out = mps;
    return ep_num * 2 + 1;
}

static void xhci_arm_intr_locked(int slot_id, int dci, int len) {
    if (xhci_ensure_ep_ring(slot_id, dci) != 0) return;
    xhci.xfer_done[slot_id][dci] = 0;
    xhci_ring_t *r = &xhci_ep_rings[slot_id][dci];
    uint32_t ctrl = (XHCI_TRB_TYPE_NORMAL << 10) | XHCI_TRB_IOC | XHCI_TRB_ISP;
    xhci_ring_push(r, xhci_v2p(xhci_intr_buf[slot_id][dci]), (uint32_t)len, ctrl);
    xhci_intr_armed[slot_id][dci] = 1;
    xhci_doorbell(slot_id, dci);
}

static int xhci_poll_interrupt_nonblock(usb_device_t *dev, uint8_t *buf, int len) {
    if (!xhci_ok || !dev || dev->state != USB_DEVSTATE_CONFIGURED) return -1;
    int slot_id = dev->xhci_slot_id;
    if (slot_id <= 0 || slot_id >= XHCI_MAX_SLOTS) return -1;

    int mps = len;
    int dci = xhci_find_intr_in_dci(dev, &mps);
    if (dci >= XHCI_MAX_DCI) return -1;
    if (len > mps && mps > 0) { /* keep requested len <= 64 */ }
    int xfer_len = (len > 0 && len <= 64) ? len : 8;

    uint64_t f = irq_save();
    spin_lock(&xhci_lock);

    if (!xhci_intr_armed[slot_id][dci]) {
        xhci_arm_intr_locked(slot_id, dci, xfer_len);
        spin_unlock(&xhci_lock);
        irq_restore(f);
        return -1;
    }

    xhci_poll_events_locked();
    if (!xhci.xfer_done[slot_id][dci]) {
        spin_unlock(&xhci_lock);
        irq_restore(f);
        return -1;
    }

    uint8_t cc = xhci.xfer_cc[slot_id][dci];
    xhci.xfer_done[slot_id][dci] = 0;
    xhci_intr_armed[slot_id][dci] = 0;

    int rc = -1;
    if (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET) {
        memcpy(buf, xhci_intr_buf[slot_id][dci], (size_t)xfer_len);
        rc = 0;
    } else if (cc == XHCI_CC_STALL) {
        xhci_reset_endpoint_locked(slot_id, dci);
    }

    xhci_arm_intr_locked(slot_id, dci, xfer_len);
    spin_unlock(&xhci_lock);
    irq_restore(f);
    return rc;
}

int xhci_poll_keyboard(usb_device_t *dev, uint8_t *buf) {
    if (!dev || !dev->is_keyboard) return -1;
    memset(buf, 0, 8);
    return xhci_poll_interrupt_nonblock(dev, buf, 8);
}

int xhci_poll_mouse(usb_device_t *dev, uint8_t *buf) {
    if (!dev || !dev->is_mouse) return -1;
    memset(buf, 0, 4);
    return xhci_poll_interrupt_nonblock(dev, buf, 4);
}

/* ---- Port reset & enumeration ---- */
static void xhci_clear_port_changes(int port, uint32_t bits) {
    uint32_t sc = xhci_port_r32(port, XHCI_PORTSC);
    sc &= ~(XHCI_PORTSC_RW1C_MASK | XHCI_PORTSC_RESET | XHCI_PORTSC_WPR);
    xhci_port_w32(port, XHCI_PORTSC, sc | XHCI_PORTSC_PP | (bits & XHCI_PORTSC_RW1C_MASK));
}

static int xhci_reset_port(int port) {
    uint32_t sc = xhci_port_r32(port, XHCI_PORTSC);
    if (!(sc & XHCI_PORTSC_CCS)) return -1;

    xhci_clear_port_changes(port, XHCI_PORTSC_RW1C_MASK);

    /* If USB3 port is already enabled & in U0, no reset needed */
    sc = xhci_port_r32(port, XHCI_PORTSC);
    if (xhci.port_proto[port] == 3 && (sc & XHCI_PORTSC_PED)) {
        return 0;
    }

    /* Issue Port Reset (preserve PP, do not touch RW1C bits) */
    uint32_t wr = (sc & ~(XHCI_PORTSC_RW1C_MASK | XHCI_PORTSC_WPR)) | XHCI_PORTSC_PP | XHCI_PORTSC_RESET;
    xhci_port_w32(port, XHCI_PORTSC, wr);

    for (int t = 0; t < 200; t++) {
        xhci_delay_us(1000);
        sc = xhci_port_r32(port, XHCI_PORTSC);
        if (!(sc & XHCI_PORTSC_RESET) && (sc & (XHCI_PORTSC_PRC | XHCI_PORTSC_PED)))
            break;
    }
    xhci_delay_us(10000);
    xhci_clear_port_changes(port, XHCI_PORTSC_PRC | XHCI_PORTSC_CSC | XHCI_PORTSC_PEC);

    sc = xhci_port_r32(port, XHCI_PORTSC);
    if (!(sc & XHCI_PORTSC_CCS) || !(sc & XHCI_PORTSC_PED))
        return -1;
    return 0;
}

static uint16_t xhci_default_ep0_mps(int xhci_speed) {
    switch (xhci_speed) {
        case 1: return 8;    /* Full Speed */
        case 2: return 8;    /* Low Speed */
        case 3: return 64;   /* High Speed */
        case 4:
        case 5: return 512;  /* SuperSpeed */
        default: return 8;
    }
}

static uint8_t xhci_to_usb_speed(int xhci_speed) {
    switch (xhci_speed) {
        case 1: return USB_SPEED_FULL;
        case 2: return USB_SPEED_LOW;
        case 3: return USB_SPEED_HIGH;
        default: return USB_SPEED_SUPER;
    }
}

static int xhci_enum_port_locked(int port, usb_device_t **new_dev_out) {
    if (new_dev_out) *new_dev_out = 0;
    uint32_t portsc = xhci_port_r32(port, XHCI_PORTSC);
    if (!(portsc & XHCI_PORTSC_CCS)) return -1;

    if (xhci_reset_port(port) != 0) {
        serial_printf("[xhci] port %d reset failed (portsc=0x%X)\n",
                      port, (unsigned)xhci_port_r32(port, XHCI_PORTSC));
        return -1;
    }

    portsc = xhci_port_r32(port, XHCI_PORTSC);
    int xhci_spd = (int)((portsc & XHCI_PORTSC_SPD_MASK) >> XHCI_PORTSC_SPD_SHIFT);
    uint16_t ep0_mps = xhci_default_ep0_mps(xhci_spd);

    int slot_id = xhci_enable_slot_locked();
    if (slot_id <= 0) return -1;

    if (xhci_address_device_locked(slot_id, port, xhci_spd, ep0_mps) != 0) {
        xhci_disable_slot_locked(slot_id);
        return -1;
    }
    xhci_delay_us(2000);

    usb_device_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.xhci_slot_id = slot_id;
    tmp.controller_port = port;
    tmp.address = (uint8_t)slot_id;
    tmp.speed = xhci_to_usb_speed(xhci_spd);
    tmp.max_packet_size = ep0_mps;
    tmp.state = USB_DEVSTATE_ADDRESSED;

    usb_setup_packet_t setup;
    uint8_t dbuf[18];
    setup.bmRequestType = (USB_DIR_IN << 7) | (USB_REQTYPE_STANDARD << 5) | USB_RECIP_DEVICE;
    setup.bRequest = USB_REQ_GET_DESCRIPTOR;
    setup.wValue = (USB_DESC_DEVICE << 8);
    setup.wIndex = 0;
    setup.wLength = 8;

    if (xhci_control_transfer_locked(&tmp, &setup, dbuf, USB_DIR_IN) != 0) {
        xhci_disable_slot_locked(slot_id);
        return -1;
    }

    uint16_t real_mps0 = (tmp.speed == USB_SPEED_SUPER && dbuf[7] < 16)
                         ? (uint16_t)(1u << dbuf[7]) : (uint16_t)dbuf[7];
    if (real_mps0 == 0) real_mps0 = ep0_mps;
    tmp.max_packet_size = real_mps0;
    if (real_mps0 != ep0_mps)
        xhci_evaluate_ep0_locked(slot_id, real_mps0);

    setup.wLength = 18;
    if (xhci_control_transfer_locked(&tmp, &setup, dbuf, USB_DIR_IN) != 0) {
        xhci_disable_slot_locked(slot_id);
        return -1;
    }
    usb_device_descriptor_t *dd = (usb_device_descriptor_t *)dbuf;
    tmp.vendor_id = dd->idVendor;
    tmp.product_id = dd->idProduct;
    tmp.class_code = dd->bDeviceClass;
    tmp.subclass = dd->bDeviceSubClass;
    tmp.protocol = dd->bDeviceProtocol;

    uint8_t cfg9[9];
    setup.wValue = (USB_DESC_CONFIG << 8);
    setup.wLength = 9;
    if (xhci_control_transfer_locked(&tmp, &setup, cfg9, USB_DIR_IN) != 0) {
        xhci_disable_slot_locked(slot_id);
        return -1;
    }
    usb_config_descriptor_t *cfg = (usb_config_descriptor_t *)cfg9;
    uint16_t total_len = cfg->wTotalLength;
    if (total_len < 9) total_len = 9;
    if (total_len > 512) total_len = 512;

    uint8_t full_cfg[512];
    setup.wLength = total_len;
    if (xhci_control_transfer_locked(&tmp, &setup, full_cfg, USB_DIR_IN) != 0) {
        xhci_disable_slot_locked(slot_id);
        return -1;
    }

    /* Parse interfaces (bAlternateSetting == 0): select primary HID or MSD interface */
    int chosen_iface = -1;
    int cur_iface = -1;
    int cur_alt = 0;
    int have_priority = 0;
    uint8_t *p = full_cfg + 9;
    uint8_t *end = full_cfg + total_len;
    while (p + 2 <= end) {
        uint8_t dlen = p[0];
        if (dlen < 2 || p + dlen > end) break;
        if (p[1] == USB_DESC_INTERFACE && dlen >= 9) {
            usb_interface_descriptor_t *ifc = (usb_interface_descriptor_t *)p;
            cur_iface = ifc->bInterfaceNumber;
            cur_alt = ifc->bAlternateSetting;
            if (cur_alt == 0) {
                int prio = 0;
                if (ifc->bInterfaceClass == 0x08 && ifc->bInterfaceSubClass == 0x06 && ifc->bInterfaceProtocol == 0x50)
                    prio = 3;
                else if (ifc->bInterfaceClass == USB_CLASS_HID &&
                         (ifc->bInterfaceProtocol == USB_PROTOCOL_KEYBOARD || ifc->bInterfaceProtocol == USB_PROTOCOL_MOUSE))
                    prio = 2;
                else if (chosen_iface < 0)
                    prio = 1;
                if (prio > have_priority) {
                    have_priority = prio;
                    chosen_iface = cur_iface;
                    tmp.interface_num = ifc->bInterfaceNumber;
                    tmp.class_code = ifc->bInterfaceClass;
                    tmp.subclass = ifc->bInterfaceSubClass;
                    tmp.protocol = ifc->bInterfaceProtocol;
                    tmp.endpoint_count = 0;
                }
            }
        } else if (p[1] == USB_DESC_ENDPOINT && dlen >= 7 && cur_alt == 0 && cur_iface == chosen_iface) {
            if (tmp.endpoint_count < USB_MAX_ENDPOINTS) {
                usb_endpoint_descriptor_t *ep = (usb_endpoint_descriptor_t *)p;
                memcpy(&tmp.endpoints[tmp.endpoint_count], ep, sizeof(usb_endpoint_descriptor_t));
                tmp.endpoints[tmp.endpoint_count].wMaxPacketSize &= 0x07FFu;
                tmp.endpoint_count++;
            }
        }
        p += dlen;
    }

    setup.bmRequestType = USB_DIR_OUT | (USB_REQTYPE_STANDARD << 5) | USB_RECIP_DEVICE;
    setup.bRequest = USB_REQ_SET_CONFIGURATION;
    setup.wValue = cfg->bConfigurationValue;
    setup.wIndex = 0;
    setup.wLength = 0;
    if (xhci_control_transfer_locked(&tmp, &setup, 0, USB_DIR_OUT) != 0) {
        xhci_disable_slot_locked(slot_id);
        return -1;
    }

    if (xhci_configure_endpoints_locked(slot_id, &tmp, xhci_spd) != 0) {
        serial_printf("[xhci] configure_ep fail slot=%d\n", slot_id);
        xhci_disable_slot_locked(slot_id);
        return -1;
    }

    tmp.state = USB_DEVSTATE_CONFIGURED;
    if (tmp.class_code == USB_CLASS_HID) {
        tmp.is_keyboard = (tmp.protocol == USB_PROTOCOL_KEYBOARD);
        tmp.is_mouse = (tmp.protocol == USB_PROTOCOL_MOUSE);
    }

    /* Register into usb_devices[] (reuse detached slot if any) */
    int dev_idx = -1;
    for (int i = 0; i < usb_device_count; i++) {
        if (usb_devices[i].state == USB_DEVSTATE_DETACHED) {
            dev_idx = i;
            break;
        }
    }
    if (dev_idx < 0 && usb_device_count < USB_MAX_DEVICES)
        dev_idx = usb_device_count++;

    if (dev_idx < 0) {
        xhci_disable_slot_locked(slot_id);
        return -1;
    }

    usb_devices[dev_idx] = tmp;
    xhci_slot_port[slot_id] = port;
    xhci_port_slot[port] = slot_id;
    xhci_slot_dev[slot_id] = &usb_devices[dev_idx];
    if (new_dev_out) *new_dev_out = &usb_devices[dev_idx];

    serial_printf("[xhci] port %d slot=%d spd=%d vid=%04X pid=%04X cls=%02X/%02X/%02X eps=%d\n",
                  port, slot_id, xhci_spd, tmp.vendor_id, tmp.product_id,
                  tmp.class_code, tmp.subclass, tmp.protocol, tmp.endpoint_count);
    return 0;
}

/* ---- BIOS USBLEGSUP handoff & Supported Protocol scan ---- */
static void xhci_parse_extended_caps(void) {
    uint32_t hcc1 = xhci_r32(xhci.cap_base, XHCI_HCCPARAMS1);
    xhci.csz = (hcc1 >> 2) & 1;
    uint32_t xecp = (hcc1 >> 16) & 0xFFFFu;
    if (xecp == 0) return;

    uint32_t off = xecp << 2;
    for (int guard = 0; guard < 64 && off >= 0x20; guard++) {
        uint32_t cap = xhci_r32(xhci.cap_base, off);
        uint8_t id = (uint8_t)(cap & 0xFFu);
        uint8_t next = (uint8_t)((cap >> 8) & 0xFFu);

        if (id == 1) {
            /* USB Legacy Support Capability */
            if (cap & (1u << 16)) {
                serial_printf("[xhci] BIOS USBLEGSUP ownership handoff...\n");
                xhci_w32(xhci.cap_base, off, cap | (1u << 24));
                for (int t = 0; t < 200; t++) {
                    uint32_t v = xhci_r32(xhci.cap_base, off);
                    if (!(v & (1u << 16)) && (v & (1u << 24))) break;
                    xhci_delay_us(1000);
                }
            }
            /* Disable legacy SMIs in USBLEGCTLSTS */
            xhci_w32(xhci.cap_base, off + 4u, 0xE0000000u);
        } else if (id == 2) {
            /* Supported Protocol Capability */
            uint8_t rev_maj = (uint8_t)((cap >> 24) & 0xFFu);
            uint32_t dw2 = xhci_r32(xhci.cap_base, off + 8u);
            uint8_t po = (uint8_t)(dw2 & 0xFFu);
            uint8_t pc = (uint8_t)((dw2 >> 8) & 0xFFu);
            for (int k = 0; k < (int)pc; k++) {
                int p = (int)po - 1 + k;
                if (p >= 0 && p < XHCI_MAX_PORTS)
                    xhci.port_proto[p] = rev_maj;
            }
        }

        if (next == 0) break;
        off += ((uint32_t)next << 2);
    }
}

int xhci_init(usb_controller_t *ctrl) {
    memset(&xhci, 0, sizeof(xhci));
    memset(xhci_ep_rings, 0, sizeof(xhci_ep_rings));
    memset(xhci_ep_rings_init, 0, sizeof(xhci_ep_rings_init));
    memset(xhci_intr_armed, 0, sizeof(xhci_intr_armed));
    for (int i = 0; i < XHCI_MAX_SLOTS; i++) {
        xhci_slot_port[i] = -1;
        xhci_slot_dev[i] = 0;
    }
    for (int i = 0; i < XHCI_MAX_PORTS; i++)
        xhci_port_slot[i] = -1;

    int bar_idx = -1;
    for (int i = 0; i < 6; i++) {
        if (!ctrl->bar[i].is_io && ctrl->bar[i].address != 0) {
            bar_idx = i;
            break;
        }
    }
    if (bar_idx < 0) return -1;

    xhci.cap_base = ctrl->bar[bar_idx].address;
    uint32_t cap0 = xhci_r32(xhci.cap_base, XHCI_CAPLENGTH);
    uint8_t cap_len = (uint8_t)(cap0 & 0xFFu);
    if (cap_len < 0x20) cap_len = 0x20;

    uint32_t dboff = xhci_r32(xhci.cap_base, XHCI_DBOFF) & ~0x3u;
    uint32_t rtsoff = xhci_r32(xhci.cap_base, XHCI_RTSOFF) & ~0x1Fu;

    xhci.op_base = xhci.cap_base + cap_len;
    xhci.port_base = xhci.op_base + 0x400u;
    xhci.db_base = xhci.cap_base + dboff;
    xhci.rt_base = xhci.cap_base + rtsoff;

    uint32_t hcsp1 = xhci_r32(xhci.cap_base, XHCI_HCSPARAMS1);
    uint32_t hcsp2 = xhci_r32(xhci.cap_base, XHCI_HCSPARAMS2);
    uint32_t hcc1  = xhci_r32(xhci.cap_base, XHCI_HCCPARAMS1);
    xhci.n_slots = (int)(hcsp1 & 0xFFu);
    xhci.n_ports = (int)((hcsp1 >> 24) & 0xFFu);
    xhci.ppc = (int)((hcc1 >> 3) & 1u);
    if (xhci.n_slots >= XHCI_MAX_SLOTS) xhci.n_slots = XHCI_MAX_SLOTS - 1;
    if (xhci.n_ports > XHCI_MAX_PORTS) xhci.n_ports = XHCI_MAX_PORTS;

    xhci_parse_extended_caps();

    serial_printf("[xhci] cap=0x%X op=0x%X rt=0x%X db=0x%X slots=%d ports=%d csz=%d\n",
                  (unsigned)xhci.cap_base, (unsigned)xhci.op_base,
                  (unsigned)xhci.rt_base, (unsigned)xhci.db_base,
                  xhci.n_slots, xhci.n_ports, xhci.csz);

    /* Wait for Controller Not Ready (CNR) = 0 */
    xhci_wait_op_mask(XHCI_USBSTS, XHCI_USBSTS_CNR, 0, 500);

    /* Halt controller cleanly before reset */
    uint32_t cmd = xhci_op_r32(XHCI_USBCMD);
    if (cmd & XHCI_CMD_RUN_STOP) {
        xhci_op_w32(XHCI_USBCMD, cmd & ~XHCI_CMD_RUN_STOP);
        xhci_wait_op_mask(XHCI_USBSTS, XHCI_USBSTS_HCH, XHCI_USBSTS_HCH, 200);
    }

    /* Reset controller */
    xhci_op_w32(XHCI_USBCMD, XHCI_CMD_HC_RESET);
    if (xhci_wait_op_mask(XHCI_USBCMD, XHCI_CMD_HC_RESET, 0, 500) != 0) {
        serial_printf("[xhci] HC reset timeout\n");
        return -1;
    }
    if (xhci_wait_op_mask(XHCI_USBSTS, XHCI_USBSTS_CNR, 0, 500) != 0) {
        serial_printf("[xhci] CNR timeout\n");
        return -1;
    }

    /* Program CONFIG (MaxSlotsEn) */
    xhci_op_w32(XHCI_CONFIG, (uint32_t)xhci.n_slots);

    /* Setup DCBAAP + Scratchpad Buffers if required */
    memset(dcbaap_storage, 0, sizeof(dcbaap_storage));
    uint32_t max_sp = ((hcsp2 >> 21) & 0x1Fu) | (((hcsp2 >> 27) & 0x1Fu) << 5);
    if (max_sp > 0) {
        if (max_sp > 64) max_sp = 64;
        for (uint32_t i = 0; i < max_sp; i++) {
            void *raw = kmalloc(8192);
            if (!raw) return -1;
            uintptr_t p = ((uintptr_t)raw + 4095u) & ~(uintptr_t)4095u;
            memset((void *)p, 0, 4096);
            s_scratchpad_arr[i] = xhci_v2p((void *)p);
        }
        dcbaap_storage[0] = xhci_v2p(s_scratchpad_arr);
    }
    xhci_op_w64(XHCI_DCBAAP, xhci_v2p(dcbaap_storage));

    /* Setup Command Ring & CRCR */
    xhci_ring_init(&xhci.cmd_ring, cmd_ring_storage, xhci_v2p(cmd_ring_storage));
    xhci_op_w64(XHCI_CRCR, xhci.cmd_ring.ring_phys | 1ULL);

    /* Setup Event Ring + ERST + Interrupter 0 */
    memset(evt_ring_storage, 0, sizeof(evt_ring_storage));
    xhci.evt_ring = evt_ring_storage;
    xhci.evt_ring_phys = xhci_v2p(evt_ring_storage);
    xhci.evt_cycle = 1;
    xhci.evt_idx = 0;

    memset(erst_storage, 0, sizeof(erst_storage));
    erst_storage[0].ring_seg_base = xhci.evt_ring_phys;
    erst_storage[0].ring_seg_size = XHCI_TRB_RING_SIZE;
    erst_storage[0].rsvd = 0;

    xhci_w32(xhci.rt_base, XHCI_IR0_ERSTSZ, 1u);
    xhci_w64(xhci.rt_base, XHCI_IR0_ERDP, xhci.evt_ring_phys);
    xhci_w64(xhci.rt_base, XHCI_IR0_ERSTBA, xhci_v2p(erst_storage));
    xhci_w32(xhci.rt_base, XHCI_IR0_IMOD, 0u);
    xhci_w32(xhci.rt_base, XHCI_IR0_IMAN, 0u);

    /* Start Controller (polling mode: INTE=0) */
    xhci_op_w32(XHCI_USBCMD, XHCI_CMD_RUN_STOP);
    if (xhci_wait_op_mask(XHCI_USBSTS, XHCI_USBSTS_HCH, 0, 200) != 0) {
        serial_printf("[xhci] controller failed to start\n");
        return -1;
    }

    /* Power on ports if PPC=1 */
    if (xhci.ppc) {
        for (int p = 0; p < xhci.n_ports; p++) {
            uint32_t sc = xhci_port_r32(p, XHCI_PORTSC);
            if (!(sc & XHCI_PORTSC_PP))
                xhci_port_w32(p, XHCI_PORTSC, (sc & ~(XHCI_PORTSC_RW1C_MASK | XHCI_PORTSC_RESET | XHCI_PORTSC_WPR)) | XHCI_PORTSC_PP);
        }
        xhci_delay_us(20000);
    }

    xhci_ok = true;
    serial_printf("[xhci] started OK, scanning %d ports...\n", xhci.n_ports);

    for (int p = 0; p < xhci.n_ports; p++) {
        xhci_enum_port_locked(p, 0);
    }

    return 0;
}

int xhci_poll_hotplug(void) {
    if (!xhci_ok) return 0;

    int changed = 0;
    usb_device_t *new_hid[XHCI_MAX_PORTS];
    int new_hid_cnt = 0;

    uint64_t f = irq_save();
    spin_lock(&xhci_lock);

    xhci_poll_events_locked();
    xhci.port_changed = 0;

    for (int p = 0; p < xhci.n_ports; p++) {
        uint32_t sc = xhci_port_r32(p, XHCI_PORTSC);
        int cur_slot = xhci_port_slot[p];

        if (sc & XHCI_PORTSC_CSC) {
            xhci_clear_port_changes(p, XHCI_PORTSC_CSC);
            if ((sc & XHCI_PORTSC_CCS) && cur_slot < 0) {
                usb_device_t *nd = 0;
                if (xhci_enum_port_locked(p, &nd) == 0) {
                    changed = 1;
                    if (nd && nd->class_code == USB_CLASS_HID && new_hid_cnt < XHCI_MAX_PORTS)
                        new_hid[new_hid_cnt++] = nd;
                }
            } else if (!(sc & XHCI_PORTSC_CCS) && cur_slot > 0) {
                usb_device_t *od = xhci_slot_dev[cur_slot];
                if (od) od->state = USB_DEVSTATE_DETACHED;
                xhci_disable_slot_locked(cur_slot);
                xhci_port_slot[p] = -1;
                changed = 1;
            }
        } else if ((sc & XHCI_PORTSC_CCS) && cur_slot < 0) {
            usb_device_t *nd = 0;
            if (xhci_enum_port_locked(p, &nd) == 0) {
                changed = 1;
                if (nd && nd->class_code == USB_CLASS_HID && new_hid_cnt < XHCI_MAX_PORTS)
                    new_hid[new_hid_cnt++] = nd;
            }
        } else if (!(sc & XHCI_PORTSC_CCS) && cur_slot > 0) {
            usb_device_t *od = xhci_slot_dev[cur_slot];
            if (od) od->state = USB_DEVSTATE_DETACHED;
            xhci_disable_slot_locked(cur_slot);
            xhci_port_slot[p] = -1;
            changed = 1;
        }
    }

    spin_unlock(&xhci_lock);
    irq_restore(f);

    for (int i = 0; i < new_hid_cnt; i++) {
        usb_hid_init_device(new_hid[i]);
    }
    return changed;
}

bool xhci_is_ready(void) { return xhci_ok; }

bool xhci_port_connected(int port) {
    if (!xhci_ok || port < 0 || port >= xhci.n_ports) return false;
    return (xhci_port_r32(port, XHCI_PORTSC) & XHCI_PORTSC_CCS) != 0;
}
