/* Intel Wi-Fi 6 AX201 (8086:51F0, So / HR) surucusu.
 *
 * Bu kart CNVi tarafinda "So" olarak gorunuyor ama tasima yolu AX210
 * ailesi gibi isliyor: Linux trans cfg'de device_family = AX210,
 * gen2 = true, integrated = true, umac_prph_offset = 0x300000. Yani
 * boot gen3 context info ile yapilir:
 *
 *   - PRPH scratch (surucu tarafi) + PRPH info sayfasi
 *   - CSR 0x118 context info, CSR 0x120/0x128 IML adresi ve boyu
 *   - CSR 0x0 boot ctrl'e AUTO_FUNC_BOOT_ENA
 *   - UMAC cevre penceresinden CPU_INIT_RUN (0xA05C44 + 0x300000)
 *   - IML tamamlanmasini bekle, sonra RX halkasini besle
 *
 * Onceden burada family 22000'in gen2 yolu vardi (CSR 0x40 context,
 * RFH'yi surucu kuruyor). O akis bu karta uymuyor, kaldirildi.
 *
 * Sahte basari yok: ALIVE yalnizca RX halkasindan gelen 0xCAFE
 * bildirimi ile kabul edilir. HBUS okumasi ile "tamam" denmez.
 *
 * Firmware: iwlwifi-so-a0-hr-b0-86 (gomulu). C:/firmware/ altindaki
 * ayni adli dosya varsa o kullanilir.
 */

#include "kernel.h"
#include "pci.h"
#include "wifi.h"
#include "asi.h"
#include "afs.h"
#include "alloc.h"
#include "clock.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

extern const uint8_t iwl_fw_blob[];
extern const uint8_t iwl_fw_blob_end[];

#define CSR_HW_IF_CONFIG_REG             0x000
#define CSR_INT_COALESCING               0x004
#define CSR_INT                          0x008
#define CSR_INT_MASK                     0x00c
#define CSR_RESET                        0x020
#define CSR_GP_CNTRL                     0x024
#define CSR_HW_REV                       0x028
#define CSR_UCODE_DRV_GP1_CLR            0x05c
#define CSR_MBOX_SET_REG                 0x088
#define CSR_HW_RF_ID                     0x09c
#define CSR_GIO_REG                      0x03C
#define CSR_DBG_LINK_PWR_MGMT_REG        0x250
#define CSR_MSIX_FH_INT_MASK             0x2804
#define CSR_MSIX_HW_INT_CAUSES           0x2808
#define CSR_MSIX_HW_INT_MASK             0x280C
#define CSR_GIO_CHICKEN_BITS             0x100
#define CSR_DBG_HPET_MEM_REG             0x240
#define CSR_MAC_ADDR0                    0x380
#define CSR_MAC_ADDR1                    0x384
#define CSR_LTR_LAST_MSG                 0x0DC

/* gen3 boot kayitlari */
#define CSR_CTXT_INFO_BOOT_CTRL          0x000
#define CSR_CTXT_INFO_ADDR               0x118
#define CSR_IML_DATA_ADDR                0x120
#define CSR_IML_SIZE_ADDR                0x128
#define CSR_AUTO_FUNC_BOOT_ENA           0x00000002

#define CSR_HW_IF_CONFIG_REG_HAP_WAKE    0x00080000
#define CSR_HW_IF_CONFIG_REG_NIC_READY   0x00400000
/* PREPARE (eski adi WAKE_ME) = 0x08000000. */
#define CSR_HW_IF_CONFIG_REG_PREPARE     0x08000000
/* Alinti 2048 us: yoklama ile calistigimiz icin sadece varsayilan. */
#define IWL_HOST_INT_TIMEOUT_DEF         0x40
#define CSR_DBG_LINK_PWR_MGMT_DISABLED   0x80000000
#define CSR_GIO_REG_L0S_DISABLED         0x00000002
#define CSR_INT_BIT_ALIVE                0x00000001
#define CSR_INT_BIT_FH_RX                0x80000000
#define CSR_MSIX_HW_INT_ALIVE            0x00000001
#define CSR_MSIX_HW_INT_IML              0x00000002
#define CSR_GP_CNTRL_MAC_CLOCK_READY     0x00000001
#define CSR_GP_CNTRL_INIT_DONE           0x00000004
#define CSR_GP_CNTRL_MAC_ACCESS_REQ      0x00000008
#define CSR_GP_CNTRL_GOING_TO_SLEEP      0x00000010
#define CSR_GP_CNTRL_HW_RF_KILL          0x08000000
#define CSR_MBOX_OS_ALIVE                (1u << 5)
#define CSR_GIO_CHICKEN_L1A_NO_L0S_RX    0x00800000
#define CSR_DBG_HPET_MEM_REG_VAL         0xFFFF0000
#define CSR_UCODE_SW_BIT_RFKILL          (1u << 0)
#define CSR_UCODE_DRV_GP1_BIT_CMD_BLOCKED (1u << 1)

#define HBUS_TARG_PRPH_WADDR             0x444
#define HBUS_TARG_PRPH_RADDR             0x448
#define HBUS_TARG_PRPH_WDAT              0x44c
#define HBUS_TARG_PRPH_RDAT              0x450
#define HBUS_TARG_WRPTR                  0x460

/* AX210 ailesi cevre birimi penceresi 0x300000 kayar; prph adres
 * maskesi de 24 bite cikar. */
#define IWL_PRPH_MASK                    0x00FFFFFFu
#define IWL_UMAC_PRPH_OFFSET             0x300000u

/* Gen3'te RX yazma indeksi hala bu tetik kaydindan vurulur
 * (Linux: aile < BZ ise RFH_Q_FRBDCB_WIDX_TRG). */
#define RFH_Q0_FRBDCB_WIDX_TRG           0x1C80

#define CSR_MAC_SHADOW_REG_CTRL          0x0A8
#define CSR_MAC_SHADOW_REG_CTRL_VAL      0x800FFFFFu

#define UREG_CPU_INIT_RUN                0xA05C44

#define IWL_TLV_MAGIC                    0x0a4c5749u
#define IWL_TLV_SEC_RT                   19u
#define IWL_TLV_SEC_RT_SECURE            24u
#define IWL_TLV_PHY_SKU                  23u
#define IWL_TLV_IML                      52u
#define CPU1_CPU2_SEPARATOR              0xFFFFCCCCu
#define PAGING_SEPARATOR                 0xAAAABBBBu
#define IWL_MAX_DRAM_ENTRY               64
#define IWL_MAX_SECTIONS                 80

#define IWL_CMD_QID                      0
/* Linux: max(IWL_CMD_QUEUE_SIZE, cfg->min_txq_size) = 128. */
#define IWL_CMD_QSIZE                    128
#define IWL_TFD_SIZE                     256
/* AX210: num_rbds = IWL_NUM_RBDS_AX210_HE (4096). */
#define IWL_RX_QSIZE                     4096
#define IWL_RX_BUFSZ                     4096
#define IWL_RX_FREE_DESC                 16
#define IWL_RX_USED_DESC                 32
/* Gen2/gen3 host komut basligi 8 bayt: cmd, grup, sira, uzunluk,
 * yedek, surum. Linux her komutu bu baslikla gonderiyor. */
#define IWL_CMD_HDR_WIDE                 8
#define IWL_HOST_CMD_MAX                 480

#define CMD_ALIVE                        0x01
#define CMD_INIT_COMPLETE                0x04
#define CMD_INIT_EXTENDED_CFG            0x03
#define CMD_NVM_ACCESS_COMPLETE          0x00
#define GRP_LEGACY                       0x00
#define GRP_SYSTEM                       0x02
#define GRP_REG_NVM                      0x0c
#define IWL_INIT_NVM_BIT                 (1u << 1)
#define IWL_ALIVE_OK                     0xCAFE

#define FH_FRAME_SIZE_MSK                0x00003FFFu
#define FH_FRAME_ALIGN                   64u

/* PRPH scratch bayraklari (Linux iwl_prph_scratch_flags). */
#define IWL_SCRATCH_RB_SIZE_4K           0x00010000u
#define IWL_SCRATCH_MTR_MODE             0x00020000u
#define IWL_SCRATCH_MTR_FORMAT_256B      0x000C0000u

struct iwl_sec {
    const uint8_t *data;
    uint32_t len;
    uint32_t offset;
};

/* DRAM haritasi: her giris 32 KB'lik bir parca gosterir. */
struct iwl_ctxt_dram {
    uint64_t umac_img[IWL_MAX_DRAM_ENTRY];
    uint64_t lmac_img[IWL_MAX_DRAM_ENTRY];
    uint64_t virtual_img[IWL_MAX_DRAM_ENTRY];
} __attribute__((packed));

struct iwl_prph_scratch_version {
    uint16_t mac_id;
    uint16_t version;
    uint16_t size;
    uint16_t reserved;
} __attribute__((packed));

struct iwl_prph_scratch_control {
    uint32_t control_flags;
    uint32_t reserved;
} __attribute__((packed));

struct iwl_prph_scratch_pnvm_cfg {
    uint64_t pnvm_base_addr;
    uint32_t pnvm_size;
    uint32_t reserved;
} __attribute__((packed));

struct iwl_prph_scratch_hwm_cfg {
    uint64_t hwm_base_addr;
    uint32_t hwm_size;
    uint32_t debug_token_config;
} __attribute__((packed));

struct iwl_prph_scratch_rbd_cfg {
    uint64_t free_rbd_addr;
    uint32_t reserved;
} __attribute__((packed));

struct iwl_prph_scratch_uefi_cfg {
    uint64_t base_addr;
    uint32_t size;
    uint32_t reserved;
} __attribute__((packed));

struct iwl_prph_scratch_step_cfg {
    uint32_t mbx_addr_0;
    uint32_t mbx_addr_1;
} __attribute__((packed));

struct iwl_prph_scratch_ctrl_cfg {
    struct iwl_prph_scratch_version  version;
    struct iwl_prph_scratch_control  control;
    struct iwl_prph_scratch_pnvm_cfg pnvm_cfg;
    struct iwl_prph_scratch_hwm_cfg  hwm_cfg;
    struct iwl_prph_scratch_rbd_cfg  rbd_cfg;
    struct iwl_prph_scratch_uefi_cfg reduce_power_cfg;
    struct iwl_prph_scratch_step_cfg step_cfg;
} __attribute__((packed));

struct iwl_prph_scratch {
    struct iwl_prph_scratch_ctrl_cfg ctrl_cfg;
    uint32_t fseq_override;
    uint32_t step_analog_params;
    uint32_t reserved[8];
    struct iwl_ctxt_dram dram;
} __attribute__((packed));

typedef char iwl_scratch_size_ok[(sizeof(struct iwl_prph_scratch) == 1660) ? 1 : -1];

/* Firmware'in boot sirasinda yazdigi, surucunun okudugu sayfa. */
struct iwl_prph_info {
    uint32_t boot_stage_mirror;
    uint32_t ipc_status_mirror;
    uint32_t sleep_notif;
    uint32_t reserved;
} __attribute__((packed));

struct iwl_context_info_gen3 {
    uint16_t version;
    uint16_t size;
    uint32_t config;
    uint64_t prph_info_base_addr;
    uint64_t cr_head_idx_arr_base_addr;
    uint64_t tr_tail_idx_arr_base_addr;
    uint64_t cr_tail_idx_arr_base_addr;
    uint64_t tr_head_idx_arr_base_addr;
    uint16_t cr_idx_arr_size;
    uint16_t tr_idx_arr_size;
    uint64_t mtr_base_addr;
    uint64_t mcr_base_addr;
    uint16_t mtr_size;
    uint16_t mcr_size;
    uint16_t mtr_doorbell_vec;
    uint16_t mcr_doorbell_vec;
    uint16_t mtr_msi_vec;
    uint16_t mcr_msi_vec;
    uint8_t  mtr_opt_header_size;
    uint8_t  mtr_opt_footer_size;
    uint8_t  mcr_opt_header_size;
    uint8_t  mcr_opt_footer_size;
    uint16_t msg_rings_ctrl_flags;
    uint16_t prph_info_msi_vec;
    uint64_t prph_scratch_base_addr;
    uint32_t prph_scratch_size;
    uint32_t reserved;
} __attribute__((packed));

typedef char iwl_ctxt_gen3_size_ok[(sizeof(struct iwl_context_info_gen3) == 104) ? 1 : -1];

/* Giden tampon tanimlayicisi (16 bayt): rbid + hedef adres. */
struct iwl_rx_free_desc {
    uint16_t rbid;
    uint16_t reserved[3];
    uint64_t addr;
} __attribute__((packed));

/* Gelen tampon tanimlayicisi (32 bayt): firmware rbid'i buraya yazar. */
struct iwl_rx_used_desc {
    uint32_t reserved1;
    uint16_t rbid;
    uint8_t  flags;
    uint8_t  reserved2[25];
} __attribute__((packed));

struct rx_view {
    uint8_t  cmd;
    uint8_t  group;
    uint16_t seq;
    uint16_t len;
    uint8_t  data[256];
};

static volatile uint32_t *g_mmio;
static net_controller_t  *g_nc;
static bool               g_alive;
static bool               g_init_done;
static int                g_state = ASI_WIFI_OFF;
static char               g_ssid[33];
static uint8_t            g_my_mac[6];

static const uint8_t     *g_fw;
static uint32_t           g_fw_len;
static uint8_t           *g_fw_owned;
static uint32_t           g_phy_config;

static const uint8_t     *g_iml;
static uint32_t           g_iml_len;
static uint8_t           *g_iml_buf;
static uint64_t           g_iml_phys;

static struct iwl_sec     g_sec[IWL_MAX_SECTIONS];
static int                g_nsec;
static int                g_lmac_n, g_umac_n, g_page_n;

static uint8_t           *g_tfd;
static uint64_t           g_tfd_phys;
static uint8_t           *g_cmd_buf[IWL_CMD_QSIZE];
static uint32_t           g_cmd_wr;

static uint8_t           *g_rx_buf[IWL_RX_QSIZE];
static uint8_t           *g_rx_pool;
static struct iwl_rx_free_desc *g_rx_free;
static struct iwl_rx_used_desc *g_rx_used;
static volatile uint16_t *g_rx_st;
static uint64_t           g_rx_free_phys;
static uint64_t           g_rx_used_phys;
static uint64_t           g_rx_st_phys;
static uint32_t           g_rx_wr;
static uint32_t           g_rx_rd;
static uint32_t           g_rx_boff;
static int                g_rx_open;
static bool               g_rx_restocked;

static struct iwl_context_info_gen3 *g_ctxt;
static uint64_t                     g_ctxt_phys;
static struct iwl_prph_scratch     *g_scratch;
static uint64_t                     g_scratch_phys;
static struct iwl_prph_info        *g_prph_info;
static uint64_t                     g_prph_info_phys;

static inline uint32_t rd32(uint32_t o) { return g_mmio[o / 4]; }
static inline void wr32(uint32_t o, uint32_t v) { g_mmio[o / 4] = v; }
static inline void setb(uint32_t o, uint32_t b) { wr32(o, rd32(o) | b); }

static void dly_ms(uint32_t ms) { clock_sleep_ms(ms); }

static void fence(void) { asm volatile("mfence" ::: "memory"); }

static int poll_bit(uint32_t off, uint32_t mask, uint32_t want, uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
        if ((rd32(off) & mask) == want) return 1;
        dly_ms(1);
    }
    return 0;
}

static int nic_access(void) {
    setb(CSR_GP_CNTRL, CSR_GP_CNTRL_MAC_ACCESS_REQ);
    for (int i = 0; i < 15; i++) {
        uint32_t v = rd32(CSR_GP_CNTRL);
        if ((v & CSR_GP_CNTRL_MAC_CLOCK_READY) &&
            !(v & CSR_GP_CNTRL_GOING_TO_SLEEP))
            return 1;
        dly_ms(1);
    }
    return 0;
}

static void prph_wr(uint32_t addr, uint32_t val) {
    wr32(HBUS_TARG_PRPH_WADDR, (addr & IWL_PRPH_MASK) | (3u << 24));
    wr32(HBUS_TARG_PRPH_WDAT, val);
}

static void umac_prph_wr(uint32_t addr, uint32_t val) {
    prph_wr(addr + IWL_UMAC_PRPH_OFFSET, val);
}

static void *dma_zalloc(uint32_t len, uint64_t *phys) {
    uint8_t *raw = (uint8_t *)kmalloc(len + 4096u);
    uintptr_t aligned;
    if (!raw) return 0;
    aligned = ((uintptr_t)raw + 4095u) & ~(uintptr_t)4095u;
    memset((void *)aligned, 0, len);
    *phys = (uint64_t)aligned;
    return (void *)aligned;
}

static int log2_pow2(uint32_t n) {
    int e = 0;
    while ((1u << e) < n) e++;
    return e;
}

static void read_mac(void) {
    uint32_t a = rd32(CSR_MAC_ADDR0);
    uint32_t b = rd32(CSR_MAC_ADDR1);
    g_my_mac[0] = (uint8_t)(a);
    g_my_mac[1] = (uint8_t)(a >> 8);
    g_my_mac[2] = (uint8_t)(a >> 16);
    g_my_mac[3] = (uint8_t)(a >> 24);
    g_my_mac[4] = (uint8_t)(b);
    g_my_mac[5] = (uint8_t)(b >> 8);
    kernel_debug("IWL: MAC %02X:%02X:%02X:%02X:%02X:%02X (CSR %08X %08X)",
                 g_my_mac[0], g_my_mac[1], g_my_mac[2],
                 g_my_mac[3], g_my_mac[4], g_my_mac[5], a, b);
}

static int fw_pick(void) {
    static const char *paths[] = {
        "C:/firmware/iwlwifi-so-a0-hr-b0-86.ucode",
        "C:/firmware/iwlwifi-so-a0-hr-b0.ucode",
    };
    uint32_t cap = 1600u * 1024u;
    g_fw_owned = (uint8_t *)kmalloc(cap);
    if (g_fw_owned) {
        for (int i = 0; i < 2; i++) {
            uint32_t n = afs_read_path(paths[i], g_fw_owned, cap);
            if (n > 128 && n < cap) {
                g_fw = g_fw_owned;
                g_fw_len = n;
                kernel_debug("IWL: firmware dosyadan: %s (%u)", paths[i], n);
                return 0;
            }
        }
    }
    g_fw = iwl_fw_blob;
    g_fw_len = (uint32_t)(iwl_fw_blob_end - iwl_fw_blob);
    kernel_debug("IWL: gomulu firmware (%u byte)", g_fw_len);
    return g_fw_len > 128 ? 0 : -1;
}

static int fw_parse(void) {
    uint32_t pos, magic, zero;
    if (g_fw_len < 88) return -1;
    zero = *(const uint32_t *)g_fw;
    magic = *(const uint32_t *)(g_fw + 4);
    if (zero != 0 || magic != IWL_TLV_MAGIC) {
        kernel_debug("IWL: firmware basligi bozuk magic=%08X", magic);
        return -1;
    }
    kernel_debug("IWL: fw api=%u", *(const uint32_t *)(g_fw + 72));
    pos = 88;
    g_nsec = 0;
    g_phy_config = 0;
    g_iml = 0;
    g_iml_len = 0;
    while (pos + 8 <= g_fw_len && g_nsec < IWL_MAX_SECTIONS) {
        uint32_t typ = *(const uint32_t *)(g_fw + pos);
        uint32_t len = *(const uint32_t *)(g_fw + pos + 4);
        pos += 8;
        if (pos + len > g_fw_len) break;
        if ((typ == IWL_TLV_SEC_RT || typ == IWL_TLV_SEC_RT_SECURE) && len >= 4) {
            g_sec[g_nsec].offset = *(const uint32_t *)(g_fw + pos);
            g_sec[g_nsec].data = g_fw + pos + 4;
            g_sec[g_nsec].len = len - 4;
            g_nsec++;
        }
        if (typ == IWL_TLV_PHY_SKU && len == 4)
            g_phy_config = *(const uint32_t *)(g_fw + pos);
        /* Gen3 boot, firmware'in IML imajini ister. TLV 52 yoksa
         * context info'yi bosuna yazmayalim. */
        if (typ == IWL_TLV_IML && len > 0) {
            g_iml = g_fw + pos;
            g_iml_len = len;
        }
        pos += len;
        if (pos & 3) pos += 4 - (pos & 3);
    }
    kernel_debug("IWL: %d bolum, phy_cfg=%08X, IML=%u",
                 g_nsec, g_phy_config, g_iml_len);
    if (g_nsec <= 2) return -1;
    return g_iml_len ? 0 : -1;
}

static int copy_range(int begin, int count, uint64_t *map, const char *tag) {
    if (count < 0 || count > IWL_MAX_DRAM_ENTRY) {
        kernel_debug("IWL: %s bolum sayisi sigmadi (%d)", tag, count);
        return -1;
    }
    /* Linux context info'yu sifir ayirir; kullanilmayan DRAM yuvasi 0
     * kalmali. Aksi halde yigin copu firmware'e adres olarak gider. */
    memset(map, 0, IWL_MAX_DRAM_ENTRY * sizeof(uint64_t));
    for (int i = 0; i < count; i++) {
        struct iwl_sec *s = &g_sec[begin + i];
        uint64_t phys = 0;
        void *dst;
        if (s->len == 0) {
            map[i] = 0;
            continue;
        }
        dst = dma_zalloc(s->len, &phys);
        if (!dst) {
            kernel_debug("IWL: DMA yok (%s %d, %u)", tag, i, s->len);
            return -1;
        }
        memcpy(dst, s->data, s->len);
        map[i] = phys;
    }
    return 0;
}

static int map_sections(struct iwl_ctxt_dram *dram) {
    int i, lmac = 0, umac = 0, page = 0;
    uint64_t tmp[IWL_MAX_DRAM_ENTRY];
    for (i = 0; i < g_nsec; i++) {
        if (g_sec[i].offset == CPU1_CPU2_SEPARATOR ||
            g_sec[i].offset == PAGING_SEPARATOR)
            break;
        lmac++;
    }
    if (i < g_nsec && g_sec[i].offset == CPU1_CPU2_SEPARATOR) {
        i++;
        while (i < g_nsec &&
               g_sec[i].offset != PAGING_SEPARATOR &&
               g_sec[i].offset != CPU1_CPU2_SEPARATOR) {
            umac++;
            i++;
        }
    }
    if (i < g_nsec && g_sec[i].offset == PAGING_SEPARATOR) {
        i++;
        page = g_nsec - i;
    }
    g_lmac_n = lmac;
    g_umac_n = umac;
    g_page_n = page;
    kernel_debug("IWL: bolum lmac=%d umac=%d sayfa=%d", lmac, umac, page);
    if (lmac == 0 || umac == 0) return -1;
    if (copy_range(0, lmac, tmp, "lmac") != 0) return -1;
    memcpy(dram->lmac_img, tmp, sizeof(tmp));
    if (copy_range(lmac + 1, umac, tmp, "umac") != 0) return -1;
    memcpy(dram->umac_img, tmp, sizeof(tmp));
    if (page > 0) {
        if (copy_range(lmac + umac + 2, page, tmp, "sayfa") != 0) return -1;
        memcpy(dram->virtual_img, tmp, sizeof(tmp));
    }
    return 0;
}

static void tfd_fill(uint8_t *slot, uint64_t addr, uint16_t len) {
    memset(slot, 0, IWL_TFD_SIZE);
    slot[0] = 1;
    slot[2] = (uint8_t)len;
    slot[3] = (uint8_t)(len >> 8);
    for (int i = 0; i < 8; i++)
        slot[4 + i] = (uint8_t)(addr >> (8 * i));
}

static void cmd_doorbell(void) {
    wr32(HBUS_TARG_WRPTR,
         (g_cmd_wr & (IWL_CMD_QSIZE - 1)) | (IWL_CMD_QID << 16));
}

static void rx_doorbell(void) {
    fence();
    wr32(RFH_Q0_FRBDCB_WIDX_TRG, g_rx_wr & ~7u);
}

static int rings_init(void) {
    g_tfd = dma_zalloc(IWL_CMD_QSIZE * IWL_TFD_SIZE, &g_tfd_phys);
    g_rx_free = dma_zalloc(IWL_RX_QSIZE * IWL_RX_FREE_DESC, &g_rx_free_phys);
    g_rx_used = dma_zalloc(IWL_RX_QSIZE * IWL_RX_USED_DESC, &g_rx_used_phys);
    /* rb_stts gen3'te 2 baytlik closed_rb_num; sayfayi tek basina ver. */
    g_rx_st = dma_zalloc(4096, &g_rx_st_phys);
    g_prph_info = dma_zalloc(4096, &g_prph_info_phys);
    g_scratch = dma_zalloc(sizeof(*g_scratch), &g_scratch_phys);
    g_ctxt = dma_zalloc(sizeof(*g_ctxt), &g_ctxt_phys);
    if (!g_tfd || !g_rx_free || !g_rx_used || !g_rx_st ||
        !g_prph_info || !g_scratch || !g_ctxt)
        return -1;

    for (int i = 0; i < IWL_CMD_QSIZE; i++) {
        uint64_t p = 0;
        g_cmd_buf[i] = dma_zalloc(512, &p);
        if (!g_cmd_buf[i]) return -1;
    }
    /* Tamponlar tek parca ayrilir (16 MB, 4K hizali). Halkaya
     * ALIVE'tan sonra yazilir; gen3'te RFH'yi firmware kurar. */
    {
        uint64_t pool = 0;
        g_rx_pool = dma_zalloc(IWL_RX_QSIZE * IWL_RX_BUFSZ, &pool);
        if (!g_rx_pool) return -1;
        for (int i = 0; i < IWL_RX_QSIZE; i++)
            g_rx_buf[i] = g_rx_pool + (uint32_t)i * IWL_RX_BUFSZ;
    }
    g_rx_wr = 0;
    g_rx_rd = 0;
    g_rx_open = 0;
    g_rx_restocked = false;
    g_cmd_wr = 0;
    return 0;
}

/* Linux iwl_pcie_rxmq_restock: halkayi 4095 tamponla doldurur (yazma
 * == okuma "bos" demek), sonra yazma indeksini 8'in katina yuvarlayip
 * tetigi vurur. */
static void rx_post_all(void) {
    uint32_t n = IWL_RX_QSIZE - 1;
    for (uint32_t i = 0; i < n; i++) {
        g_rx_free[i].rbid = (uint16_t)(i + 1);
        g_rx_free[i].addr = (uint64_t)(uintptr_t)g_rx_buf[i];
    }
    g_rx_free[n].rbid = 0;
    g_rx_free[n].addr = 0;
    g_rx_wr = n;
    g_rx_rd = 0;
    g_rx_open = 0;
    rx_doorbell();
}

/* Linux her tampona bir kez VID veriyor (havuz sirasi + 1). Yeniden
 * beslemede ayni VID geri yazilir; hangi tamponun doldugunu used
 * halkasindaki rbid soyluyor. */
static void rx_repost(uint16_t rbid) {
    uint32_t w;
    if (rbid == 0 || rbid > IWL_RX_QSIZE) return;
    w = g_rx_wr & (IWL_RX_QSIZE - 1);
    g_rx_free[w].rbid = rbid;
    g_rx_free[w].addr = (uint64_t)(uintptr_t)g_rx_buf[rbid - 1];
    g_rx_wr = (g_rx_wr + 1) & (IWL_RX_QSIZE - 1);
    if ((g_rx_wr & 7u) == 0) rx_doorbell();
}

static int rx_one(struct rx_view *out) {
    if (!g_rx_st) return 0;
    for (;;) {
        uint8_t *buf;
        uint16_t rbid, closed;
        struct iwl_rx_used_desc *ud;

        if (!g_rx_open) {
            fence();
            closed = *g_rx_st & (IWL_RX_QSIZE - 1);
            if (closed == (g_rx_rd & (IWL_RX_QSIZE - 1))) return 0;
            g_rx_open = 1;
            g_rx_boff = 0;
        }
        ud = &g_rx_used[g_rx_rd & (IWL_RX_QSIZE - 1)];
        rbid = ud->rbid;
        if (rbid == 0 || rbid > IWL_RX_QSIZE) {
            kernel_debug("IWL: RX bozuk rbid=%u idx=%u", rbid,
                         g_rx_rd & (IWL_RX_QSIZE - 1));
            g_rx_open = 0;
            g_rx_rd++;
            continue;
        }
        buf = g_rx_buf[rbid - 1];
        /* Bir tamponda birden cok bildirim olabilir; parcali tamponu
         * (flags bit 0) islemeden geri veriyoruz. */
        if (buf && !(ud->flags & 1u)) {
            while (g_rx_boff + 8 < IWL_RX_BUFSZ) {
                uint32_t lnf = *(uint32_t *)(buf + g_rx_boff);
                uint32_t plen, total;
                if (lnf == 0 || lnf == 0xFFFFFFFFu) break;
                plen = lnf & FH_FRAME_SIZE_MSK;
                if (plen < 4 || g_rx_boff + 4 + plen > IWL_RX_BUFSZ) break;
                out->cmd = buf[g_rx_boff + 4];
                out->group = buf[g_rx_boff + 5];
                out->seq = *(uint16_t *)(buf + g_rx_boff + 6);
                out->len = (uint16_t)(plen - 4);
                if (out->len > sizeof(out->data)) out->len = sizeof(out->data);
                if (out->len) memcpy(out->data, buf + g_rx_boff + 8, out->len);
                kernel_debug("IWL: RX grp=%u cmd=%02X seq=%04X len=%u",
                             out->group, out->cmd, out->seq, plen);
                total = (4 + plen + FH_FRAME_ALIGN - 1) & ~(FH_FRAME_ALIGN - 1);
                g_rx_boff += total;
                /* Ayni tamponda devam eden bildirim olabilir; tamponu
                 * tuketene kadar halkaya geri vermiyoruz. */
                return 1;
            }
        }
        rx_repost(rbid);
        g_rx_rd++;
        g_rx_open = 0;
    }
}

static int nic_ready(void) {
    setb(CSR_HW_IF_CONFIG_REG, CSR_HW_IF_CONFIG_REG_NIC_READY);
    for (int i = 0; i < 100; i++) {
        if (rd32(CSR_HW_IF_CONFIG_REG) & CSR_HW_IF_CONFIG_REG_NIC_READY)
            return 1;
        dly_ms(1);
    }
    return 0;
}

static int prepare_card(void) {
    if (nic_ready()) {
        setb(CSR_MBOX_SET_REG, CSR_MBOX_OS_ALIVE);
        return 0;
    }
    /* Linux iwl_pcie_prepare_card_hw: once link guc yonetimini kapat,
     * sonra PREPARE isteyip NIC_READY'yi yeniden bekle. */
    setb(CSR_DBG_LINK_PWR_MGMT_REG, CSR_DBG_LINK_PWR_MGMT_DISABLED);
    dly_ms(2);
    for (int i = 0; i < 10; i++) {
        setb(CSR_HW_IF_CONFIG_REG, CSR_HW_IF_CONFIG_REG_PREPARE);
        if (nic_ready()) {
            setb(CSR_MBOX_SET_REG, CSR_MBOX_OS_ALIVE);
            return 0;
        }
        dly_ms(25);
    }
    kernel_debug("IWL: kart hazir degil (NIC_READY yok)");
    return -1;
}

static int apm_init(void) {
    setb(CSR_GIO_CHICKEN_BITS, CSR_GIO_CHICKEN_L1A_NO_L0S_RX);
    setb(CSR_DBG_HPET_MEM_REG, CSR_DBG_HPET_MEM_REG_VAL);
    setb(CSR_HW_IF_CONFIG_REG, CSR_HW_IF_CONFIG_REG_HAP_WAKE);
    setb(CSR_GIO_REG, CSR_GIO_REG_L0S_DISABLED);
    setb(CSR_GP_CNTRL, CSR_GP_CNTRL_INIT_DONE);
    /* So long-latency kristali ~12 ms. 50 ms bekle. */
    if (!poll_bit(CSR_GP_CNTRL, CSR_GP_CNTRL_MAC_CLOCK_READY,
                  CSR_GP_CNTRL_MAC_CLOCK_READY, 50)) {
        kernel_debug("IWL: MAC clock yok, GP=%08X", rd32(CSR_GP_CNTRL));
        return -1;
    }
    return 0;
}

/* Linux'ta gen3 icin iwl_mvm_nic_config hemen donuyor; AX210 ailesinde
 * CSR_HW_IF_CONFIG_REG'e radio alanlari yazilmiyor. Asagidaki islev
 * yalnizca dogrulama amacli degerleri okuyup logluyor. */
static void radio_note(uint32_t hw_rev) {
    kernel_debug("IWL: radio yoklanmadi (AX210 ailesi), phy_cfg=%08X hw=%08X",
                 g_phy_config, hw_rev);
}

static int build_ctxt(uint32_t hw_rev) {
    uint32_t flags;
    if (map_sections(&g_scratch->dram) != 0) return -1;

    g_scratch->ctrl_cfg.version.mac_id = (uint16_t)hw_rev;
    g_scratch->ctrl_cfg.version.version = 0;
    g_scratch->ctrl_cfg.version.size =
        (uint16_t)(sizeof(*g_scratch) / 4);

    flags = IWL_SCRATCH_RB_SIZE_4K | IWL_SCRATCH_MTR_MODE |
            IWL_SCRATCH_MTR_FORMAT_256B;
    g_scratch->ctrl_cfg.control.control_flags = flags;
    g_scratch->ctrl_cfg.rbd_cfg.free_rbd_addr = g_rx_free_phys;

    /* Linux gen3 alanlarin bir kismini sifir birakiyor (version,
     * size, config, idx arr size); biz de oyle yapiyoruz. */
    g_ctxt->prph_info_base_addr = g_prph_info_phys;
    g_ctxt->prph_scratch_base_addr = g_scratch_phys;
    g_ctxt->prph_scratch_size = sizeof(*g_scratch);
    g_ctxt->cr_head_idx_arr_base_addr = g_rx_st_phys;
    g_ctxt->tr_tail_idx_arr_base_addr = g_prph_info_phys + 2048u;
    g_ctxt->cr_tail_idx_arr_base_addr = g_prph_info_phys + 3072u;
    g_ctxt->mtr_base_addr = g_tfd_phys;
    g_ctxt->mcr_base_addr = g_rx_used_phys;
    g_ctxt->mtr_size = (uint16_t)(log2_pow2(IWL_CMD_QSIZE) - 3);
    g_ctxt->mcr_size = (uint16_t)log2_pow2(IWL_RX_QSIZE);

    kernel_debug("IWL: ctxt gen3 mac_id=%04X scratch=%08X/%u flags=%08X",
                 g_scratch->ctrl_cfg.version.mac_id,
                 (uint32_t)g_scratch_phys, g_ctxt->prph_scratch_size, flags);
    kernel_debug("IWL: ctxt rings mtr=%u mcr=%u rb_stts=%08X",
                 g_ctxt->mtr_size, g_ctxt->mcr_size,
                 (uint32_t)g_rx_st_phys);
    return 0;
}

/* Linux iwl_pcie_spin_for_iml: firmware IML'imizi yukleyene kadar
 * CPU ve cihazi mesgul tutar. Kayit: MSIX_HW_INT_CAUSES bit 1. */
static void spin_for_iml(void) {
    uint32_t last = rd32(CSR_LTR_LAST_MSG);
    kernel_debug("IWL: IML beklendi, LTR=%08X", last);
    for (uint32_t i = 0; i < 100; i++) {
        if (rd32(CSR_MSIX_HW_INT_CAUSES) & CSR_MSIX_HW_INT_IML) {
            wr32(CSR_MSIX_HW_INT_CAUSES, CSR_MSIX_HW_INT_IML);
            kernel_debug("IWL: IML tamam (%u ms)", i);
            return;
        }
        last = rd32(CSR_LTR_LAST_MSG);
        dly_ms(1);
    }
    kernel_debug("IWL: IML gelmedi, LTR=%08X HW=%08X",
                 last, rd32(CSR_MSIX_HW_INT_CAUSES));
}

/* Firmware ALIVE kesmesinden once RFH'yi kendi kuruyor. Linux halkayi
 * o kesmede doldurur; ALIVE bildirimi ancak ondan sonra RX halkasina
 * yazilir. Biz kesme almiyor, kayitlari yokluyoruz. */
static void rx_restock_if_needed(const char *why) {
    if (g_rx_restocked) return;
    rx_post_all();
    g_rx_restocked = true;
    kernel_debug("IWL: RX halkasi dolduruldu (%s, wr=%u)", why, g_rx_wr);
}

static int wait_alive(uint32_t ms) {
    for (uint32_t w = 0; w < ms; w += 10) {
        struct rx_view rx;
        uint32_t inta = rd32(CSR_INT);
        uint32_t hw = rd32(CSR_MSIX_HW_INT_CAUSES);
        /* Eski CSR_INT ve MSI-X kaynak kaydi ayni biti gosteriyor,
         * ikisine de bakiyoruz. */
        if ((inta & CSR_INT_BIT_ALIVE) || (hw & CSR_MSIX_HW_INT_ALIVE)) {
            wr32(CSR_INT, inta);
            wr32(CSR_MSIX_HW_INT_CAUSES, hw);
            rx_restock_if_needed("ALIVE kesmesi");
        }
        while (rx_one(&rx)) {
            if (rx.group == GRP_LEGACY && rx.cmd == CMD_ALIVE && rx.len >= 2) {
                uint16_t st = *(uint16_t *)rx.data;
                kernel_debug("IWL: ALIVE status=%04X flags=%04X",
                             st, rx.len >= 4 ? *(uint16_t *)(rx.data + 2) : 0);
                if (st == IWL_ALIVE_OK) {
                    g_alive = true;
                    return 0;
                }
                kernel_debug("IWL: ALIVE hatali");
                return -1;
            }
        }
        if ((w % 1000) == 0) {
            kernel_debug("IWL: ALIVE bekleniyor %u ms GP=%08X INT=%08X HW=%08X rb=%u",
                         w, rd32(CSR_GP_CNTRL), inta, hw,
                         g_rx_st ? (*g_rx_st & (IWL_RX_QSIZE - 1)) : 0);
        }
        dly_ms(10);
    }
    kernel_debug("IWL: ALIVE gelmedi (sahte yedek yok)");
    return -1;
}

static int host_cmd(uint8_t group, uint8_t cmd, const void *payload,
                    uint16_t plen, uint32_t timeout_ms) {
    uint32_t idx = g_cmd_wr & (IWL_CMD_QSIZE - 1);
    uint8_t *buf = g_cmd_buf[idx];
    uint8_t *slot = g_tfd + idx * IWL_TFD_SIZE;
    uint16_t seq = (uint16_t)idx;
    uint16_t total;
    if (!buf || plen > IWL_HOST_CMD_MAX) return -1;
    memset(buf, 0, 512);
    /* Gen2/gen3 her komutu 8 baytlik genis baslikla gonderiyor:
     * +0 cmd, +1 grup, +2 sira, +4 uzunluk (yalnizca yuk), +7 surum.
     * Governin yuku +8'den baslar. */
    buf[0] = cmd;
    buf[1] = group;
    buf[2] = (uint8_t)seq;
    buf[3] = (uint8_t)(seq >> 8);
    buf[4] = (uint8_t)plen;
    buf[5] = (uint8_t)(plen >> 8);
    buf[6] = 0;
    buf[7] = 0;
    if (payload && plen) memcpy(buf + IWL_CMD_HDR_WIDE, payload, plen);
    total = (uint16_t)(IWL_CMD_HDR_WIDE + plen);
    tfd_fill(slot, (uint64_t)(uintptr_t)buf, total);
    fence();
    g_cmd_wr = (g_cmd_wr + 1) & (IWL_CMD_QSIZE - 1);
    cmd_doorbell();
    kernel_debug("IWL: komut grp=%u cmd=%02X seq=%u", group, cmd, seq);
    for (uint32_t w = 0; w < timeout_ms; w += 10) {
        struct rx_view rx;
        while (rx_one(&rx)) {
            if (rx.cmd == cmd && rx.group == group && (rx.seq & 0xFF) == seq)
                return 0;
            if (rx.cmd == CMD_INIT_COMPLETE && rx.group == GRP_LEGACY) {
                g_init_done = true;
                kernel_debug("IWL: INIT_COMPLETE");
            }
        }
        dly_ms(10);
    }
    kernel_debug("IWL: komut zaman asimi grp=%u cmd=%02X", group, cmd);
    return -1;
}

static int mvm_init(void) {
    uint32_t flags = IWL_INIT_NVM_BIT;
    uint32_t nvm_done = 0;
    /* Yanki gelmezse de INIT_COMPLETE bildirimi asil kanittir. */
    if (host_cmd(GRP_SYSTEM, CMD_INIT_EXTENDED_CFG, &flags, 4, 1500) != 0)
        kernel_debug("IWL: INIT_EXTENDED_CFG yanki yok, devam");
    /* Linux 4 baytlik sifir yuk gonderiyor (reserved alani). */
    if (host_cmd(GRP_REG_NVM, CMD_NVM_ACCESS_COMPLETE, &nvm_done, 4, 1500) != 0)
        kernel_debug("IWL: NVM_ACCESS_COMPLETE yanki yok, devam");
    for (uint32_t w = 0; w < 2000 && !g_init_done; w += 10) {
        struct rx_view rx;
        while (rx_one(&rx)) {
            if (rx.cmd == CMD_INIT_COMPLETE && rx.group == GRP_LEGACY) {
                g_init_done = true;
                kernel_debug("IWL: INIT_COMPLETE");
            }
        }
        dly_ms(10);
    }
    if (!g_init_done) kernel_debug("IWL: INIT_COMPLETE gelmedi");
    return g_init_done ? 0 : -1;
}

static int start_fw(void) {
    uint32_t hw_rev, rf_id, gp;
    if (prepare_card() != 0) return -1;

    wr32(CSR_INT_MASK, 0);
    wr32(CSR_INT, 0xFFFFFFFFu);
    wr32(CSR_UCODE_DRV_GP1_CLR, CSR_UCODE_SW_BIT_RFKILL);
    wr32(CSR_UCODE_DRV_GP1_CLR, CSR_UCODE_DRV_GP1_BIT_CMD_BLOCKED);
    wr32(CSR_INT, 0xFFFFFFFFu);

    if (apm_init() != 0) return -1;

    hw_rev = rd32(CSR_HW_REV);
    rf_id = rd32(CSR_HW_RF_ID);
    gp = rd32(CSR_GP_CNTRL);
    kernel_debug("IWL: hw_rev=%08X type=%03X rf_id=%08X GP=%08X%s",
                 hw_rev, (hw_rev & 0x000FFF0u) >> 4, rf_id, gp,
                 (gp & CSR_GP_CNTRL_HW_RF_KILL) ? " RFKILL" : "");
    read_mac();
    radio_note(hw_rev);

    if (!nic_access()) {
        kernel_debug("IWL: cevre erisimi yok");
        return -1;
    }

    if (rings_init() != 0) {
        kernel_debug("IWL: halka ayrilamadi");
        return -1;
    }
    /* Linux iwl_pcie_gen2_rx_init: coalescing disinda RFH'ye dokunmaz;
     * RFH'yi firmware ALIVE aninda kurar. */
    {
        uint32_t coal = rd32(CSR_INT_COALESCING);
        wr32(CSR_INT_COALESCING, (coal & ~0xFFu) | IWL_HOST_INT_TIMEOUT_DEF);
    }
    /* IML imajini DMA'ya al; context info bu adresi gosterecek. */
    g_iml_buf = dma_zalloc(g_iml_len, &g_iml_phys);
    if (!g_iml_buf) {
        kernel_debug("IWL: IML icin DMA yok (%u)", g_iml_len);
        return -1;
    }
    memcpy(g_iml_buf, g_iml, g_iml_len);

    setb(CSR_MAC_SHADOW_REG_CTRL, CSR_MAC_SHADOW_REG_CTRL_VAL);
    if (build_ctxt(hw_rev) != 0) return -1;

    /* ALIVE ve RX kesmelerini ac (legacy yol). Biz kesme beklemiyoruz,
     * kaynak kayitlarini yokluyoruz; yine de Linux ile ayni birak. */
    wr32(CSR_INT_MASK, CSR_INT_BIT_ALIVE | CSR_INT_BIT_FH_RX);

    /* Linux iwl_write64 bu kayitlari iki 32-bit yazimla gonderiyor. */
    wr32(CSR_CTXT_INFO_ADDR, (uint32_t)g_ctxt_phys);
    wr32(CSR_CTXT_INFO_ADDR + 4, (uint32_t)(g_ctxt_phys >> 32));
    wr32(CSR_IML_DATA_ADDR, (uint32_t)g_iml_phys);
    wr32(CSR_IML_DATA_ADDR + 4, (uint32_t)(g_iml_phys >> 32));
    wr32(CSR_IML_SIZE_ADDR, g_iml_len);
    fence();
    kernel_debug("IWL: ctxt=%08X iml=%08X/%u yazildi",
                 (uint32_t)g_ctxt_phys, (uint32_t)g_iml_phys, g_iml_len);
    setb(CSR_CTXT_INFO_BOOT_CTRL, CSR_AUTO_FUNC_BOOT_ENA);

    /* Linux iwl_pcie_set_ltr: ayni ROM hatasi AX210'da da var, bu
     * yuzden LTR yazilmiyor; sadece IML kaynagi temizlenip CPU mesgul
     * tutuluyor. */
    wr32(CSR_MSIX_HW_INT_CAUSES, CSR_MSIX_HW_INT_IML);

    if (!nic_access()) return -1;
    umac_prph_wr(UREG_CPU_INIT_RUN, 1);
    kernel_debug("IWL: CPU_INIT_RUN (%08X)",
                 UREG_CPU_INIT_RUN + IWL_UMAC_PRPH_OFFSET);
    spin_for_iml();
    /* IML tamamlandiysa firmware calisiyor ve RFH'yi kurmus demektir.
     * Kesme bitini hic gormesek bile halkayi dolduruyoruz; yoksa ALIVE
     * bildirimi yazilacak yer bulamaz. */
    rx_restock_if_needed("IML sonrasi");

    if (wait_alive(8000) != 0) return -1;
    return 0;
}

static int drv_init(net_controller_t *nc) {
    uint16_t cmd, subsys;
    if (!nc) return -1;
    if (nc->dev.vendor != PCI_VENDOR_INTEL || nc->dev.device != PCI_DEV_IWL_AX201_SO)
        return -1;
    g_nc = nc;
    g_mmio = (volatile uint32_t *)(uintptr_t)nc->bar[0].address;
    if (!g_mmio || nc->bar[0].address == 0) {
        kernel_debug("IWL: BAR0 yok");
        return -1;
    }
    subsys = pci_config_readw(nc->dev.bus, nc->dev.slot, nc->dev.func, 0x2E);
    cmd = pci_config_readw(nc->dev.bus, nc->dev.slot, nc->dev.func, 0x04);
    /* Bit 10 (Interrupt Disable) temizlenmeli; Linux pci_enable_device
     * bunu yapiyor. Kalirsa ALIVE/FH kesmeleri hic gorunmez. */
    pci_config_writew(nc->dev.bus, nc->dev.slot, nc->dev.func, 0x04,
                      (uint16_t)((cmd & ~0x0400u) | 0x0006u));
    pci_enable_bus_mastering(nc->dev.bus, nc->dev.slot, nc->dev.func);
    kernel_debug("IWL: 8086:51F0 subsys=%04X rev=%02X BAR0=%08X",
                 subsys, nc->dev.rev_id, (uint32_t)nc->bar[0].address);

    g_alive = false;
    g_init_done = false;
    g_state = ASI_WIFI_OFF;
    if (fw_pick() != 0 || fw_parse() != 0) return -1;
    if (start_fw() != 0) return -1;
    read_mac();
    if (mvm_init() != 0)
        kernel_debug("IWL: firmware ayakta, init eksik");
    nc->initialized = true;
    kernel_debug("IWL: AX201 firmware yuklendi");
    return 0;
}

static int drv_scan(asi_wifi_net_t *out, int max) {
    (void)out;
    (void)max;
    if (!g_alive) return 0;
    /* UMAC tarama MAC ve PHY baglami ister. Sahte ag listesi donulmez.
     * Init tamamsa bir sonraki adim o baglami kurmak. */
    kernel_debug("IWL: tarama henuz yok (MAC/PHY baglami kurulmadi, init=%d)",
                 g_init_done);
    return 0;
}

static int drv_connect(const char *ssid, const char *pass) {
    (void)ssid;
    (void)pass;
    g_state = ASI_WIFI_FAILED;
    kernel_debug("IWL: baglanti yok, once gercek tarama gerekli");
    return -1;
}

static int drv_disconnect(void) {
    g_state = ASI_WIFI_OFF;
    g_ssid[0] = 0;
    return 0;
}

static int drv_state(void) { return g_state; }
static const char *drv_ssid(void) {
    return (g_state == ASI_WIFI_CONNECTED) ? g_ssid : "";
}

static bool drv_send(void *ctx, const uint8_t *data, uint16_t len) {
    (void)ctx; (void)data; (void)len;
    return false;
}
static bool drv_has(void *ctx) { (void)ctx; return false; }
static bool drv_recv(void *ctx, uint8_t *buf, uint16_t *len) {
    (void)ctx; (void)buf; (void)len;
    return false;
}

static const wifi_driver_t g_drv = {
    .name = "iwlwifi-ax201",
    .init = drv_init,
    .scan = drv_scan,
    .connect = drv_connect,
    .disconnect = drv_disconnect,
    .state = drv_state,
    .ssid = drv_ssid,
    .send = drv_send,
    .has  = drv_has,
    .recv = drv_recv,
};

void iwl_ax201_register(void) { wifi_register_driver(&g_drv); }
