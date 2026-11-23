# Arctian mimarisi

Bu belge sistemin parçalarını ve aralarındaki sözleşmeleri anlatır. Uygulama yazmak için
gereken API bilgisi burada değil: [uygulama-gelistirme.md](uygulama-gelistirme.md).

## 1. Genel şekil

Arctian çekirdeği modüler bir çekirdektir: çekirdek katmanları sabit fiziksel adreslere
yüklenen bağımsız `.bin` imajlarına bölünmüştür, her birinin başında 512 baytlık bir
**AMOD** başlığı bulunur ve modüller birbirini C sembolü / `asi_t` köprüsü üzerinden
çağırır (ring 0). Kullanıcı uygulamaları ise **Ring 3**'te, süreç başına ayrı sayfa
tablosu (CR3) içinde çalışır (`.wlf` için `int 0x80` + `wolf_api_t` köprüsü, POSIX ELF64
için Linux x86-64 `SYSCALL/SYSRET` ABI).

```
 BIOS/UEFI(CSM)
   ├─ stage1 (MBR 0x7C00)              boot/stage1.asm        diskten
   └─ stage1_iso (El Torito 0x7C00)    boot/stage1_iso.asm    CD'den
        └─ stage2 (0x10000)            boot/stage2.asm        uzun mod, sayfalama, VESA
             └─ main_kernel (0x100000) main_kernel/           GDT/IDT, VMM, yükleyici
 UEFI (doğrudan 64-bit)
   └─ boot/uefi/ (BOOTX64.EFI)         GOP + ACPI RSDP (0x7100) + ESP KERNEL.ELF → 0x100000
                  ├─ install    (0x200000)  install/          kurulum sihirbazı
                  ├─ kernel_plus (0x300000) kernel_plus/      sürücüler + ağ + POSIX + ASI
                  └─ desktop    (0x1000000) desktop/          LVGL kabuğu + uygulamalar
```

Adreslerin tek kaynağı [`include/arctian/memory_map.h`](../include/arctian/memory_map.h).
Bölgeler ve sınırları:

| Bölge | Taban | Azami |
|---|---|---|
| `main_kernel` | `0x00100000` | 1 MB |
| `install` | `0x00200000` | 1 MB |
| `kernel_plus` | `0x00300000` | 13 MB |
| `desktop` (imaj + bss) | `0x01000000` | 48 MB |
| `.dpk` uygulama bölgesi (tarihsel) | `0x04000000` | 40 MB (2 MB dilimler) |
| `.wlf` uygulama bölgesi | `0x06800000` | 8 MB |
| disk tamponu | `0x07000000` | 16 MB |
| blok/DMA geçici alan | `0x08000000` | 1 MB |
| backbuffer | `0x09000000` | 16 MB |
| VMM havuzu (`state + refcount + frames`) | `0x0A000000` | 96 MB |
| yığın (heap) | `0x10000000` | 768 MB |

Kimlik eşlemesi: `0–1 GB` arası 2 MB huge page'lerle kimlik-eşlemeli (`IDENTITY_MAP_*`),
yani modüller yüklenirken sanal=fiziksel çalışır. Sanal adres alanı yerleşiminde kullanıcı
uzayı `VMM_USER_BASE = 0x0000008000000000` (PML4 index 1), yüksek yarı eşlemesi ise
`VMM_HIGHER_BASE = 0xFFFF800000000000` (PML4 index 256) tabanındadır. `VMM_REFCOUNT_ADDR`
(`0x0A001000`) üzerinde fiziksel çerçeve başına 16-bit referans sayacı tutulur (`fork`
ve `MAP_SHARED` paylaşımı için).

## 2. İmaj biçimi

### Disk imajı (`build/arctian.img`)

```
LBA 0..     : boot imaji (stage1 + stage2 + main_kernel)
LBA 2048    : superblock  (sihir "ARCT", sürüm 1)
LBA 2049    : modul tablosu
LBA 2050    : durum sektoru (kapanis/açilis kurulumu için)
LBA 4096..  : modul imajlari  (512B AMOD başlığı + binary, 512B'ye yastalanmış)
LBA 65536.. : AFS v6 (kullanıcı diski C:/; 0.8.2 öncesi eski v5 diskler 20000'den mount edilir)
```

### ISO (`build/arctian.iso`)

El Torito, no-emulation, önyüklenebilir; `boot_iso.img` (stage1_iso + stage2 + main_kernel)
2048 bayt hizalı biçimli kayıttan sonra modüller ISO9660 dosyaları olarak durur. Paketleyici
(`tools/arctianpack.py`) `xorriso` ile üretir; hata olursa derlemeyi durdurur ve çıktı
`.part` üzerinden atomik taşınır.

### AMOD başlığı

[`include/arctian/amod.h`](../include/arctian/amod.h) — 512 bayt: sihir (`AMOD`), ABI sürümü,
tür (kernel / install / driver_svc / desktop / app), tercih edilen yükleme adresi, imaj boyutu,
`.bss` boyutu, giriş offseti, basit toplama sağlama toplamı, ad ve sürüm. `.bss` yükleyici
tarafından sıfırlanır; `entry_off` adrese eklenip atılır.

## 3. Modüller

| Modül | İçerik |
|---|---|
| `boot/` | NASM ile yazılmış BIOS/El Torito üç aşaması (`stage1`, `stage1_iso`, `stage2`) ve `boot/uefi/` (`BOOTX64.EFI`: GOP, ACPI RSDP, ESP'den `KERNEL.ELF` + `ExitBootServices`) |
| `main_kernel/` | GDT/IDT, ISR'ler, PIC, sayfalama ve refcount'lu VMM, modül yükleyici, `int 0x80` syscall, ring 3 geçişi (`ring3_user.asm`); `wlf_user.c` + `vmm_space.c` **yalnız `kernel_plus`a bağlanır** |
| `install/` | Kurulum sihirbazı: bölümleme, AFS v6 biçimlendirme, modül kopyalama, önyükleme kaydı |
| `kernel_plus/` | Sürücüler (`drivers/`), ağ yığını (`net/`), SMP zamanlayıcı/iplik/senkronizasyon, yetenek kapıları (`system/caps.c`), disk şifreleme, sanal FS, TLS, açılış animasyonu (`system/bootanim.c`), **Ring-3 POSIX/ELF64 alt sistemi (`posix/`)** |
| `desktop/` | LVGL kabuğu (dock, pencereler, hızlı ayarlar, iki ekran giriş), gömülü uygulamalar, `.wlf` yükleyici/VM/broker (`wlf*.c`), Ring-3 POSIX pencere köprüsü (`posix_win.c`), tema (ADL-Wolf). (`wolf_engine` yalnızca geriye dönük yer tutucu stub'dır; QuickJS kaldırılmıştır.) |
| `lib/` | AFS v6, FAT32, ISO9660, blok katmanı (AHCI/ATA/NVMe), ses/medya kodekleri, PNG/SVG, malloc, `printf`, IRQ-güvenli seri port/klog |
| `include/arctian/` | Kamu başlıkları: `asi.h`, `app.h`, `wlf.h`, `posix_gfx.h`, `amod.h`, `dpk.h`, `update.h`, `sig.h`, `slot.h`, `memory_map.h`, ... |
| `third_party/` | Vendored kütüphaneler: LVGL (arayüz), mbedTLS (TLS). bkz. `third_party/README.md` |

## 4. ASI — uygulama arayüzü

`asi_t` ([`include/arctian/asi.h`](../include/arctian/asi.h)) gömülü uygulama ile sistemin
temas noktasıdır; her gömülü uygulama `app_entry(lv_obj_t *win, asi_t *asi)` içinde bu
köprüyü alır. Alt sistemler:

`console`, `mem`, `storage`, `time`, `gfx`, `input`, `system`, `fs`, `net`, `proc`, `lab`,
`sandbox`, `audio`, `crypt`, `firewall`, `power`, `wlf`, `posix`.

Gömülü modül/uygulamalarda libc yoktur (`memcpy`/`memset`/`memmove`/`memcmp`/`strlen` çekirdek
tarafından sağlanır). Kullanıcı alanı POSIX ELF64 ikilileri ise `sdk/toolchain/` ile musl libc
ve libstdc++ bağlar.

## 5. Uygulama modeli: `.wlf`

Gömülü çekirdek uygulamaları dışındaki yerel Arctian paketleri `.wlf` biçimindedir
([`include/arctian/wlf.h`](../include/arctian/wlf.h)):

```
[wlf_header_t] [meta: "WLFM" + u16 uzunluk + key=value satırları]
               [ikon (PNG/SVG, isteğe bağlı)] [payload]
```

- **Tür:** `WLF_KIND_NATIVE` (x86-64 kod, libWolf'a linkli) veya `WLF_KIND_SCRIPT` (Wolf bytecode).
- **Rol** (`header.role`): `ui` (pencere), `cli` (terminalde çalışır), `runtime` ("birlikte aç" ile çalışır).
- **Meta anahtarları:** `role`, `handles=py,pyw`, `entry=python`, `terminal=1`.
- **B3 Ring-3 GUI köprüsü (`kernel_plus/system/wolf_bridge.c`):** Native `.wlf` imajı kendi
  PML4'ündeki kullanıcı aralığına (`VMM_USER_BASE`) yüklenir. Kullanıcı alanındaki 3 köprü
  sayfası üzerinde `wolf_api_t` ve 94 trampoline stub yer alır (`SYS_WOLF_CALL`/`SYS_WOLF_WAIT`);
  LVGL çizimleri masaüstü broker kuyruğuna, kullanıcı olayları (click/change/timer/resize)
  `wolf_main` dönüşüne bağlanan olay döngüsüne taşınır. Mevcut `.wlf` paketleri yeniden
  derlenmeden ring-3 yalıtımda çizim yapar.

## 6. Koruma sınırı ve imza

`.wlf` ve POSIX ELF64 uygulamaları için sınır **donanımsaldır** (CPU ring 3 + süreç başına CR3).
Kabuk ve gömülü uygulamalar için sınır **politika** düzeyindedir (yetenek kapıları).

Yalıtım mekanizması:

- Süreç başına PML4 (`vmm_space_create`). Çekirdek eşlemeleri `U=0` olarak kopyalanır;
  kullanıcı kodu çekirdek belleğini okuyamaz/yazamaz.
- W^X: kod sayfaları yazılamaz (`X`), veri ve yığın `NX + yazılabilir` (POSIX `mprotect(PROT_EXEC)`
  ile JIT sayfaları kontrollü şekilde yürütülebilir yapılabilir).
- Giriş noktaları: `.wlf` için `int 0x80` (`vmm_probe_user`), POSIX ELF64 için `SYSCALL/SYSRET`
  (`posix_uprobe` / `posix_ucopy_in` / `posix_ucopy_out`).
- Çıkış ve CPU istisnaları çekirdeği çökertmeden süreci sonlandırıp kabuğu sürdürür.
- Paket ve güncelleme manifestosu **RSA-2048 + SHA-256** ile imzalanır; imzasız paket/manifesto
  reddedilir. Anahtar üretimi: `tools/sig_keygen.py` (`secrets.SystemRandom`).

## 6.5 Ring-3 POSIX / ELF64 Alt Sistemi (`kernel_plus/posix/`)

Modern C/C++20 yazılımlarını (tarayıcı motorları dahil) çalıştırabilmek için `kernel_plus/posix/`
altında Linux x86-64 System V ABI uyumlu kullanıcı alanı alt sistemi bulunur:

- **`SYSCALL/SYSRET` (`posix_syscall.c`, `syscall_entry.S`):** `EFER.SCE`, `STAR`, `LSTAR`, `FMASK`
  ve `KERNEL_GS_BASE` (per-CPU `syscall_kstack`) ile düşük gecikmeli çağrı girişi; mevcut `int 0x80`
  yoluyla çakışmadan çalışır.
- **Süreç ve ELF64 Yükleyici (`posix_elf.c`, `posix_proc.c`):** `ET_DYN` (statik PIE) ve `ET_EXEC`
  ELF64 segmentlerini (`PT_LOAD`, `PT_TLS`, yardımcı vektör `AT_*`) kullanıcı CR3'üne yükler;
  `fork`/`vfork` (sayfa kopyalama + `MAP_SHARED` refcount), `execve`, `wait4`, `exit`/`exit_group`
  destekler. Terminalde `elfrun <yol.elf>` ile başlatılır.
- **Sanal Bellek (`posix_vm.c`):** `mmap` (anonim, dosya eşlemeli, `MAP_SHARED`), `munmap`,
  `mprotect` (`PROT_READ | PROT_WRITE | PROT_EXEC` — JIT uyumlu), `brk`.
- **İş Parçacığı ve Senkronizasyon (`posix_proc.c`):** `clone` (`CLONE_VM | CLONE_THREAD | CLONE_SETTLS`),
  `arch_prctl(ARCH_SET_FS)` ile TLS, `futex` (`FUTEX_WAIT`/`FUTEX_WAKE`), `nanosleep`,
  `sched_yield`; `libstdc++` `std::thread`, `std::mutex`, `std::condition_variable` ve POSIX `sem_t` çalışır.
- **Dosya, Zaman ve Olay Çoğullama (`posix_fd.c`):** Süreç başına FD tablosu (`openat`, `read`, `write`,
  `pread64`/`pwrite64`, `readv`/`writev`, `lseek`, `stat`/`fstat`, `getdents64`, `pipe`/`pipe2`, `dup`/`dup2`/`dup3`,
  `fcntl(O_NONBLOCK)`, `memfd_create`), `poll`/`ppoll`, `epoll_create1`/`epoll_ctl`/`epoll_wait`,
  `clock_gettime`, `getrandom`.
- **Soket & IPC (`posix_sock.c`):** `AF_UNIX` (`socketpair`, `SCM_RIGHTS` FD aktarımı, `bind`/`listen`/`accept`/`connect`)
  ve `AF_INET` (TCP soketleri, non-blocking + `epoll` entegrasyonu, `getaddrinfo`).
- **Paylaşımlı Grafik & Girdi Köprüsü (`posix_gfx.c`, `desktop/posix_win.c`):** `NR_arctian_gfx_*`
  (`500..505`) sistem çağrıları ile Ring-3 süreci paylaşımlı ARGB8888 tamponunu (`mmap`) masaüstü
  penceresine kirli dikdörtgen (`present_rect`) olarak basar ve fare/klavye/tekerlek/kapanma olaylarını
  `poll` üzerinden alır.

## 7. Güncelleme ve A/B yuva

- Güncelleme kaynağı varsayılan olarak GitHub sürümleridir: `update.txt` imzalı manifestosu
  her modül için boyut + CRC32 bildirir; `update url` ile kaynak değiştirilebilir.
  Yayın: `tools/publish_os_update.sh`.
- Kurulum **kapanışta** başlar, bitmezse **açılışta** tamamlanır (`slot` durum sektörü).
- Sistem iki yuvada (A/B) durur; yarıda kesilmiş güncellemede açılış önceki yuvaya döner.
  Kullanıcı diski (AFS) yuvalardan bağımsızdır.

## 8. Dosya sistemi

- **AFS v6** kullanıcı diskidir (`C:/`), yeni kurulumlarda LBA `65536`'dan başlar; 64 bayta kadar ad,
  64-bit kapasite, nesil sayaçlı tutamaklar, yazma günlüğü (journal) ve bütünlük denetimi (`fsck`)
  içerir. Eski v5 diskler (`LBA 20000`) açılışta uyumlu biçimde bağlanır.
- Sistem dosyaları (ayarlar, kurulu paketler) kullanıcı arayüzünden ayrı tutulur; `C:/` altında
  `C:/apps`, `C:/belgeler`, `C:/indirilenler`, `C:/masaustu`, `C:/logs` yer alır.
- Ayrıca **FAT32** (USB/taşınabilir), **ISO9660** (CD) ve **USB MSD** desteklenir.
- İsteğe bağlı **AES-XTS disk şifreleme** (`kernel_plus/system/diskcrypt.c`).

## 9. Ağ

`kernel_plus/net/` sıfırdan yazılmış bir TCP/IP yığınıdır: ARP, IPv4, TCP, UDP, DHCP, DNS, NTP,
HTTP/HTTPS (mbedTLS ile TLS), uygulama seviyesi güvenlik duvarı. Performans iyileştirmeleriyle:
- e1000: 128 RX / 32 TX halka, toplu RDT (16'lı) ve ITR=256µs kesme birleştirme, `netbg` 16 ms yield.
- TCP: `kmalloc` ile 256 KB dairesel alıcı tamponu (sıfır `memmove`), SYN `MSS=1460` + RFC 7323 pencere
  ölçekleme (`shift=3`), 32-bit `peer_window`, birleştirilmiş ACK.
- TLS: çalışma zamanı CPUID `AES-NI` tespiti ile TCG/eski CPU'da ChaCha20, donanım AES'li CPU'da AES-GCM önceliği.

Sürücüler: e1000, PCnet, RTL8139, RTL8169, Atheros ath9k (uçtan uca WPA2/CCMP: `kernel_plus/drivers/wpa2.c`),
Intel AX201 (gen3 ürün yazılımı yükleme; tarama/bağlantı iskelet).

## 10. Masaüstü kabuğu

- `desktop/main.c` + `dsk_state.c` + `taskbar.c`: kabuk kurulumu ve dock (`dk_s()` ölçek birimi).
- `desktop/windows.c`: pencere yöneticisi (taşıma, yakınlaştırma, snap, boyut hatırlama, çalışma alanı).
- `desktop/quickctl.c`: hızlı ayarlar, OSD, bildirim merkezi, parlaklık/gece modu perdeleri.
- `desktop/wlf*.c`: `.wlf` yükleme, rol yönlendirme, terminal köprüsü, B3 broker kuyruğu.
- `desktop/posix_win.c`: Ring-3 POSIX süreçlerinin ARGB8888 pencere/girdi köprüsü.
- `desktop/perm.c` + `consent.c`: özne/karar deposu ve onay penceresi.
- `desktop/theme/adl_wolf.c`: ADL-Wolf tema motoru ve fontlar (Inter, IBM Plex Sans, Manrope).

## 11. Öz-testler

Üretim açılışını yavaşlatmamak için öz-testler derleme bayraklarıyla ayrılmıştır:

```sh
make pack SELFTEST=1           # çekirdek sched/proc/caps/afs/lab + -smp 4 sync testleri
make pack WFL_SELFTEST=1       # B1 ring-3 izolasyon öz-testi
make pack DEBUG=1 T07_TEST=1   # 0.7 güvenlik/imza öz-testleri
```

Ayrı test betikleri: `tests/uefi/run.sh`, `tests/smp/run.sh`, `tests/isolation/run.sh` ve
`tests/posix/test_suite.cpp` (Ring-3 POSIX/C++20 23 maddelik doğrulama paketi).
