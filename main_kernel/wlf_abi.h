/**
 * Arctian - .wlf kullanici modu ABI sozlesmesi (B1)
 * =================================================
 *
 * Izole calisan .wlf gorevleri (wlf_user.c yukleyicisi) ile cekirdek
 * arasindaki TEK sozlesme basligi. Tuketici taraflar:
 *
 *   - main_kernel/wlf_user.c : yukleme + CR3 gecisi + calistirma (S5a)
 *   - main_kernel/syscall.c  : int 0x80 dagitici cekirdek tarafi (S5a)
 *   - kernel_plus/system/ui_broker.c : UI bandi dinleyicisi (S5b)
 *   - SDK / libWolf          : kullanicinin int 0x80 sarmalayicilari
 *
 * Bu basliga dokunurken: alanlar yalnizca SONA eklenir, numaralar
 * HICBIR ZAMAN yeniden kullanilmaz veya kaydirilmaz.
 *
 * Cagri sozlesmesi (mevcut cekirdek ABI'siyle birebir):
 *
 *   int 0x80            IDT kapisi DPL3, tek yon: kullanici -> cekirdek
 *   rax = syscall no    rdi, rsi, rdx = arg1..3 (64-bit)
 *   donus: rax          0 veya pozitiv = basari, negatif = hata
 *
 * Dagitici rdi/rsi/rdx'yi okur, sonuclari rax'e yazar. Stub tum
 * GPR'lari geri yukler; syscall kullanicida C yardimcisiyla bile
 * cagrilabilir. Tampon argumanlari kullanici adres araliginda olmali ve
 * dokunulan HER sayfada U biti kurulu olmali (syscall.c user_range
 * denetimi); cekirdek aksi halde -WLF_EBADF doner, kopyalama YAPMAZ.
 *
 * Cekirdekten kullaniciya cagri (upcall) int 0x80 ile MUMKUN DEGILDIR
 * (kapinin DPL'i yalniz asagi yonlu gecise izin verir). UI olaylari
 * gorev kendi syscall'unda BEKLEYEREK alinir (bkz. UI bandi).
 */
#ifndef ARCTIAN_WLF_ABI_H
#define ARCTIAN_WLF_ABI_H

#include <stdint.h>

#define WLF_ABI_VERSION   1u
#define WLF_ABI_TRAP      0x80u      /* syscall trapezi (int 0x80)       */

/* --- Temel numaralar (syscall.c dagitici sozlesmesi) ---
 * SYS_OPEN/READ/CLOSE 5..7 yol tabanli fd kullanir: fd = slot + 3;
 * fd 0..2 rezervedir (standart akislar), SDK henuz acik degil. */
#define WLF_SYS_EXIT      0          /* kod(rdi) -> 0; gorevi bitirir    */
#define WLF_SYS_WRITE     1          /* fd(rdi), buf(rsi), n(rdx)        */
#define WLF_SYS_PUTCHAR   2          /* ch(rdi) -> 0                     */
#define WLF_SYS_GETPID    3          /* -> pid (baglayici yoksa 1)       */
#define WLF_SYS_TICKS     4          /* -> 64-bit tik (baglayici yoksa 0)*/
#define WLF_SYS_OPEN      5          /* yol(rdi) -> fd | negatif         */
#define WLF_SYS_READ      6          /* fd(rdi), buf(rsi), n(rdx) -> n   */
#define WLF_SYS_CLOSE     7          /* fd(rdi) -> 0                     */

/* --- UI bandi (S5b ui_broker) ---
 * WLF_UI_BASE..WLF_UI_BASE+WLF_UI_COUNT-1 numarali cagrilar dagitici
 * tarafindan cekirdek-ici kancayla broker'a yonlendirilir
 * (wlf_user_set_ui_handler). Kanca kayitli DEGILSE -WLF_ENOSYS doner.
 * Sozlesme: ayni int 0x80 ABI'si; numara rax'te kalir, argumanlar
 * rdi/rsi/rdx. Bant ici numaralari ve yuku S5b tanimlar; buraya
 * sabitlemeden EKLEMEZ (broker paketinde buraya islenir). */
#define WLF_UI_BASE       64
#define WLF_UI_COUNT      32

/* --- Hata kodlari (rax, negatif) --- */
#define WLF_EPERM         (-1)       /* yetenek yok / gecersiz numara    */
#define WLF_EBADF         (-2)       /* gecersiz fd / kullanici araligi  */
#define WLF_ENFILE        (-3)       /* fd tablosu dolu                  */
#define WLF_ENOSYS        (-4)       /* bant bos: handler kayitli degil  */

/* --- Gorev bitis sozlesmesi (wlf_user_run donus degeri) ---
 * >= 0 : WLF_SYS_EXIT'in rdi'si (uygulama cikis kodu).
 * <  0 : WLF_EXIT_FAULT | istisna_no -> CPU istisnasiyla sonlandi
 *        (or. #PF=14 -> 0xC00E). WLF_EXIT_FAULT maskesi cikis kodu
 *        alaniyla cakismaz; ayristirma asagidaki makrolarla yapilir.
 * Gorev sonsuz donguye girerse kernel geri donmez (cron yok);
 * timeout sonraki paketlerin isi, ABI'de yer ayirmak icin yazildi. */
#define WLF_EXIT_FAULT      0xC000u
#define WLF_EXIT_IS_FAULT(r) (((int32_t)(r)) < 0)
#define WLF_EXIT_FAULT_NO(r) ((int32_t)(r) & 0xFF)

/* --- Cekirdek-ici kanca tipleri (S5b baglanma noktasi) ---
 * handler: numara WLF_UI bandindayken cagrilir; regs->rax'i sonucla
 * doldurur, 0 doner. 0 donerse dagitici -WLF_ENOSYS yazar. Baglam:
 * dagiticiyla ayni (interrupt kapatilmis degil; syscall.c gibi
 * davran). Kayit tek eldir; son yazan kazanir. */
typedef int (*wlf_ui_handler_fn)(void *regs);
void wlf_user_set_ui_handler(wlf_ui_handler_fn fn);

/* WLF_SYS_EXIT kancasi: aktif izole gorev varsa dagitici normal
 * donus yerine gorevi bu istekle tamamlar (wlf_user.c). UI broker'i
 * cekirdek ici testleri icin tanimlandi; SDK dogrudan cagirmaz. */
typedef struct {
    uint32_t code;                   /* uygulama cikis kodu (rdi)        */
} wlf_exit_req_t;

/* --- Yetenek eslemesi (asi.h CAP_* ile ayni degerler) ---
 * .wlf header bayraklari (wlf.h WLF_FLAG_*) baslangic yetenek
 * kumesine maplenir; broker/dagitici cap_check ile denetler. */
#define WLF_CAP_NET        (1u << 0) /* WLF_FLAG_NET  (asi.h CAP_NET)    */
#define WLF_CAP_FS         (1u << 1) /* WLF_FLAG_FS   (asi.h CAP_FS)     */
#define WLF_CAP_DEV        (1u << 2) /* WLF_FLAG_DEV  (asi.h CAP_DEV)    */
#define WLF_CAP_UI         (1u << 8) /* UI bandina erisim; bayrak tanimi
                                      * S5b ile netlesince wlf.h'ye
                                      * eklenecek (simdilik rezerve)     */

#endif /* ARCTIAN_WLF_ABI_H */
