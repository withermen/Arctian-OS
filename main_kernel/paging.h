#ifndef ARCTIAN_PAGING_H
#define ARCTIAN_PAGING_H

#include <stdint.h>
#include "memory_map.h"

#define VMM_MAGIC            0x564D4D33u

#define VMM_PTE_P            0x001ull
#define VMM_PTE_W            0x002ull
#define VMM_PTE_U            0x004ull
#define VMM_PTE_PWT          0x008ull
#define VMM_PTE_PCD          0x010ull
#define VMM_PTE_HUGE         0x080ull
#define VMM_PTE_PAT_4K       0x080ull
#define VMM_PTE_PAT_2M       0x1000ull
/* IA32_PAT layout programmed by vmm_pat_init_cpu():
 *   PA0 = WB  (0x06) -> (PAT=0, PCD=0, PWT=0)  normal RAM
 *   PA1 = WC  (0x01) -> (PAT=0, PCD=0, PWT=1)  VRAM / LFB (both 4KB and 2MB!)
 *   PA2 = UC- (0x07) -> (PAT=0, PCD=1, PWT=0)  MMIO (compatible with lib/mmio.c)
 *   PA3 = UC  (0x00) -> (PAT=0, PCD=1, PWT=1)  strong UC MMIO
 *   PA4 = WC  (0x01) -> (PAT=1, PCD=0, PWT=0)  WC alias
 *   PA5 = WT  (0x04) -> (PAT=1, PCD=0, PWT=1)  write-through
 *   PA6 = UC- (0x07) -> (PAT=1, PCD=1, PWT=0)  UC- alias
 *   PA7 = UC  (0x00) -> (PAT=1, PCD=1, PWT=1)  UC alias */
#define VMM_PTE_WC           VMM_PTE_PWT
#define VMM_PTE_UC           (VMM_PTE_PCD | VMM_PTE_PWT)
#define VMM_PAT_MSR_VALUE    0x0007040100070106ull
#define VMM_ADDR_MASK        0x000FFFFFFFFFF000ull
#define VMM_KHEAP_BASE       0xFFFF804000000000ull
#define VMM_KHEAP_MAX        (16ull * 1024ull * 1024ull * 1024ull)

#define VMM_OOM_OK           0
#define VMM_OOM_LOW          1
#define VMM_OOM_CRITICAL     2

typedef uint64_t vmm_space_t;

typedef struct {
    uint32_t magic;
    uint32_t ready;
    uint64_t kernel_space;
    uint64_t kernel_pdpt;
    uint64_t pdpt_hi;
    uint64_t next_frame;
    uint64_t pool_end;
    uint64_t space_count;
    uint64_t fault_count;
    uint64_t last_fault_cr2;
    uint64_t last_fault_rip;
    uint32_t last_fault_err;
    uint32_t last_fault_killed;
    uint64_t frames_used;
    uint64_t frames_free;
    uint64_t free_head;
    uint64_t killed_tasks;
    /* VMM3: dinamik RAM olcekleme alanlari */
    uint64_t phys_ram_total;
    uint64_t refcount_addr;
    uint64_t refcount_count;
    uint64_t frames_base;
    uint64_t low_frame_count;
    uint64_t high_base;
    uint64_t high_next;
    uint64_t high_end;
    uint64_t identity_mapped_end;
    uint32_t pat_enabled;
    uint32_t _pat_pad;
    /* E1: OOM basinc esikleri, acil durum rezervi ve simule edilen basinc */
    uint64_t emergency_reserve_frames;
    uint64_t oom_low_frames;
    uint64_t oom_crit_frames;
    int32_t  oom_force;
    uint32_t oom_latched;
} vmm_state_t;

#define VMM_STATE ((vmm_state_t *)(uintptr_t)VMM_STATE_ADDR)

typedef struct {
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;
    uint64_t frames_total;
    uint64_t frames_used;
    uint64_t frames_free;
} vmm_meminfo_t;

void        vmm_init(void);
vmm_space_t vmm_new_space(void);
int         vmm_map(vmm_space_t sp, uint64_t va, uint64_t pa, uint64_t flags);
int         vmm_unmap(vmm_space_t sp, uint64_t va);
int         vmm_protect(vmm_space_t sp, uint64_t va, uint64_t flags);
uint64_t    vmm_get_pte(vmm_space_t sp, uint64_t va);
void        vmm_switch(vmm_space_t sp);
vmm_space_t vmm_current(void);
uint64_t    vmm_translate(vmm_space_t sp, uint64_t va);
int         vmm_probe_user(vmm_space_t sp, uint64_t va, int write);
uint64_t    vmm_alloc_frame(void);
uint64_t    vmm_alloc_frame_emergency(void);
void        vmm_frame_ref(uint64_t phys);
uint16_t    vmm_frame_refcount(uint64_t phys);
void        vmm_free_frame(uint64_t phys);
void        vmm_pcpu_flush_all(void);
int         vmm_oom_pressure(void);
void        vmm_oom_set_force(int level);
uint64_t    vmm_avail_above_reserve_bytes(void);
uint32_t    vmm_space_ram_kb(vmm_space_t sp);
void        vmm_mem_info(vmm_meminfo_t *out);
int         vmm_map_kernel_page(uint64_t va, uint64_t pa, uint64_t flags);
int         vmm_map_kernel_range(uint64_t va, uint64_t size);
int         vmm_pat_init_cpu(void);
int         vmm_pat_enabled(void);
int         vmm_map_wc_range(uint64_t phys, uint64_t size);
int         vmm_map_mmio_range(uint64_t phys, uint64_t size);
int         vmm_set_region_wc(uint64_t virt, uint64_t size);
int         vmm_handle_page_fault(uint64_t cr2, uint64_t err, uint64_t rip);
int         vmm_selftest(void);
int         vmm_prot_selftest(void);
int         vmm_mem_selftest(void);

#endif
