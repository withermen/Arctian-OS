/**
 * Arctian - Ayri kullanici adres alani (PML4) yardimcilari
 * ======================================================
 *
 * Her gorev/uygulama icin ayri bir PML4 tutulur. Cekirdek eylemeleri
 * (kimlik eslemesi + higher-half) yeni alana kopyalanir ama U (user)
 * biti temizlenerek yalnizca supervisor erisimine acilir. Boylece
 * kullanici modu cekirdek belleGini goremez.
 *
 * Sabitler paging.h / memory_map.h'den alinir; burada adres uydurulmaz.
 * vmm_space_t, paging.h'de uint64_t olarak tanimlidir.
 */
#ifndef ARCTIAN_VMM_SPACE_H
#define ARCTIAN_VMM_SPACE_H

#include <stdint.h>
#include "paging.h"

/* NX (execute-disable) biti. Mimaride PTE bit 63'tur; ancak yalnizca
 * IA32_EFER.NXE=1 iken anlamlidir. NXE kapaliyken bu biti yazmak
 * reserved-bit #PF uretir; bu yuzden kullanicisi once vmm_space_nx_ok()
 * ile kontrol etmelidir. paging.h'de NX makrosu yok, tek eksik bit bu. */
#define VMM_PTE_NX           0x8000000000000000ull

/* Kullanici sanal adres uzayi: VMM_USER_BASE'den baslar, higher-half
 * PML4 girisine (VMM_HIGHER_PML4_INDEX) kadar surer. Top sabiti
 * uydurulmadi; paging.c'deki sayfa hatasi denetimiyle ayni sinirdir
 * (0x0000800000000000). */
#define VMM_USER_TOP         (((uint64_t)VMM_HIGHER_PML4_INDEX) << 39)
#define VMM_USER_END         (VMM_USER_TOP - 1ull)
#define VMM_USER_SIZE        (VMM_USER_TOP - (uint64_t)VMM_USER_BASE)

/* Yeni PML4. Cekirdek eylemeleri supervisor-only olarak kopyalanir.
 * Basarisizlikta 0 doner (cagiran hata olarak yorumlamali). */
vmm_space_t vmm_space_create(void);

/* Kullanici VA'sina fiziksel sayfa esler. va VMM_USER_BASE..VMM_USER_TOP
 * araliginda ve sayfa hizali olmali; P ve U bitleri zorunlu eklenir.
 * 0 = basarili, <0 = hata. */
int         vmm_space_map_user(vmm_space_t sp, uint64_t va, uint64_t pa,
                               uint64_t flags);

/* Tek bir kullanici eslemesini kaldirir (cerceveyi SERBEST BIRAKMAZ).
 * 0 = basarili, <0 = hata. */
int         vmm_space_unmap_user(vmm_space_t sp, uint64_t va);
int         vmm_space_protect_user(vmm_space_t sp, uint64_t va, uint64_t flags);
int         vmm_space_protect_none(vmm_space_t sp, uint64_t va);
uint64_t    vmm_space_get_user_pte(vmm_space_t sp, uint64_t va);
uint64_t    vmm_space_next_present(vmm_space_t sp, uint64_t va, uint64_t end);

/* Alani ve ona ait sayfa tablolarini geri verir. Halen eslenmis kullanici
 * yapragi varsa cerceveleri de serbest birakir; bu yuzden cagiran taraf
 * cercevelerini ayrica serbest birakacaksa once vmm_space_unmap_user
 * cagirmalidir (cift serbest birakma olmaz). Cekirdek alani (st->kernel_space)
 * ve aktif CR3 yikilmaz. */
void        vmm_space_destroy(vmm_space_t sp);

/* CR3 yaz / oku. Gecersiz (0) alan yok sayilir. */
void        vmm_space_switch(vmm_space_t sp);
vmm_space_t vmm_space_current(void);

/* [va, va+size) tamamen kullanici araliginda mi (tasma denetimi).
 * 1 = evet, 0 = hayir. */
int         vmm_space_user_range(uint64_t va, uint64_t size);

/* NX biti bu CPU'da kullanilabilir mi (IA32_EFER.NXE).
 * 1 = evet, 0 = hayir. */
int         vmm_space_nx_ok(void);

#endif /* ARCTIAN_VMM_SPACE_H */
