# Sürüm kayıtları

Sürüm numarasının tek kaynağı [`version.json`](version.json)'dur; Makefile derleme
anında okuyup `ARCTIAN_VERSION` olarak gömer.

## 1.1 — UEFI/Dual-Boot Kurulum, AFS v7, xHCI USB 3.0, Masaüstü UX & Günlük Uygulamalar

Güncel 1.1 durum özeti: [docs/1.0-durum.md](docs/1.0-durum.md).

**Tam UTF-8 Türkçe Klavye, Fare & I2C-HID (`kernel_plus/drivers/`)**

- `keyboard.c`: `TR-Q` (TS 2118), `TR-F` (TS 2117) ve `EN-Q` için tam Unicode/UTF-8
  karakter haritaları (`ğ, Ğ, ü, Ü, ş, Ş, ı, İ, ö, Ö, ç, Ç, €, ₺`).
- `mouse.c` & `i2c_hid.h`: Synaptics mutlak koordinat + iki parmak kaydırma desteği
  ve Intel LPSS I2C-HID sürücü başlığı.

**Sınırsız AFS v7 & Güvenli Disk Şifreleme (`lib/afs.c`, `lib/storage.c`, `kernel_plus/system/diskcrypt.c`)**

- **AFS v7:** 1.024 dosya sınırı kaldırıldı → **65.536 dosya** kapasiteli dinamik parçalı
  inode blokları (256×256) ve **çoklu-extent** (`EXT7` = `0x37545845`) dosya depolama;
  mevcut AFS v6 diskleriyle geriye dönük uyumlu.
- Gerçek `uid`/`gid`/`mode` çoklu kullanıcı dosya yalıtımı (`caps.c`, `afs_set_subject`),
  GPT/MBR dinamik bölüm keşfi (`storage.c`) ve `diskcrypt.c` içinde OTA Slot B
  (`32768..65535`) koruma sınırı.

**xHCI USB 3.0, FAT32 RW & exFAT (`kernel_plus/drivers/`, `lib/`, `kernel_plus/system/volfs.c`)**

- Çalışan **xHCI USB 3.0** denetleyicisi (`usb_xhci.c`, `qemu-xhci` doğrulamalı),
  USB HID klavye/fare (`usb_hid.c`) ve USB yığın depolama (`usb_msd.c`).
- MBR/GPT bölümlü **FAT32 okuma & yazma** + UTF-8 Türkçe LFN (`lib/fat32.c`),
  **exFAT okuma** (`lib/exfat.h`) ve `D:/` sıcak takma (*hot-plug*, `volfs.c`).

**UEFI Disk Kurulumu, Dual-Boot ("Yanına Kur") & Temiz Önyükleme (`install/`, `boot/`)**

- Temiz UEFI kurulumunda GPT + **64 MiB FAT32 ESP** (`BOOTX64.EFI` + `KERNEL.ELF`) ile
  kurulum sonrası doğrudan UEFI SSD önyükleme (`install/installer.c`, `boot/uefi/`).
- Mevcut GPT diskindeki Windows/Linux bölümlerini ve ESP'yi koruyup `>= 5.0 GiB` boş
  alanda yeni Arctian bölümü açan **"Yanına Kur (Dual-Boot)"** modu.
- Kurulum arayüzünde (`install/inst_ui.h`) Türkçe bitmap font ve PS/2 fare desteği;
  `boot/stage1.asm` içinde tüm yapay bekleme/sahte boot mesajları kaldırıldı.

**Gerçek PC Ağ, Ses (`/dev/dsp`) & Laptop Güç (`kernel_plus/drivers/`, `kernel_plus/posix/`)**

- Gerçek donanım register/DMA **RTL8169** (`rtl8169.c`), Intel **I225/I219-V/I226-V**
  PCI kimlikleri (`pci.c`) + 64-bit adresli **e1000** (`e1000.c`), dürüst DHCP hata
  bildirimi (`net.c`), `iwl_ax201.c` UMAC tarama/WPA2 CCMP kod yolu ve `ath9k.c`
  çoklu hız desteği.
- Ring-3 POSIX OSS `/dev/dsp` ve `/dev/audio` PCM akış arayüzü (`posix_fd.c`), çok pinli
  HDA jack-sense (`hda.c`), `virtio_snd.c` S16 PCM ses ölçekleme (`virtio_snd_scale_pcm_s16`),
  Intel PWM ekran parlaklığı (`gpu_intel.c`: `BLC_PCH_SPLIT`/`BLC_PCH_PAIR`/`BLC_CPU_PAIR`/
  `BLC_LEGACY`), SMBus BAR4 ve geçerli FACS waking vector denetimli güvenli S3 uyku
  koruması (`acpi.c`).

**Masaüstü UX 1.1 & VirtualBox Düzeltmesi (`desktop/`, `kernel_plus/`)**

- 8 yönlü kenar/köşe pencere boyutlandırma (`desktop/windows.c`), 4 adımlı **OOBE İlk
  Kurulum Sihirbazı** (`desktop/oobe.h`: klavye düzeni, saat dilimi, tema/vurgu,
  masaüstü turu), VirtualBox `VBoxVGA`/`VMSVGA` ilk-kare `Y_OFFSET` düzeltmesi (`gpu_bga.c`,
  `gpu.c`), PNG ekran görüntüsü (`desktop/quickctl.c`), %100 UTF-8 Türkçe arayüz, çoklu
  kullanıcı kilit/oturum yalıtımı (`lock.c`, `caps.c`), UEFI ESP OTA güncellemesi (`update.c`)
  ve gerçek süreç RAM/CPU metrikleri (`proc.c`).

**Günlük Uygulamalar, ZIP, ISO'ya Hazır Ofis & WebKit 1.1 (`desktop/apps/*`, `lib/`, `sdk/`)**

- **Yeni ve güncellenmiş yerleşik uygulamalar:** Kaydet/Geri Al/Bul-Değiştir destekli `metin`;
  [`lib/imgdec.h`](lib/imgdec.h) ile PNG/JPEG/GIF/BMP + yakınlaştırma/döndürmeli `foto`;
  WAV/MP3/OGG/FLAC + ses/sarma/duraklatmalı `medya`; yeni **Hesap Makinesi** (`hesap`) ve
  **Saat/Kronometre/Zamanlayıcı** (`saat`); [`lib/miniz.h`](lib/miniz.h) PKZIP sıkıştır/çıkar
  destekli responsive Dosya Yöneticisi ve Terminal; çalışan aramalı & kullanıcı hesaplı Ayarlar;
  gerçek Performans sekmeli Görev Yöneticisi; `C:/etc/calendar.txt` kalıcı etkinlikli Takvim;
  özgün yola geri yükleyen Çöp Kovası; birleşik HTTPS + çevrimdışı DuckStore.
- **ISO'ya hazır Ofis paketi (`sdk/office/`):** `safak.wlf`, `murekkep.wlf`, `abakus.wlf`,
  `sahne.wlf` ve `mercek.wlf` paketleri kurulum ISO'suna (`tools/arctianpack.py`) ve
  kurulum akışına (`installer.c`) eklendi; ortak Aç/Farklı Kaydet dosya seçicisi (`office_picker.h`).
- **Sistem içi WebKit 1.1 (`sdk/webkit/`, `desktop/posix_win.c`):** 8 sekmeye kadar çoklu sekme,
  `Ctrl+C/V` sistem panosu köprüsü, canlı pencere boyutlandırma, Türkçe UTF-8 girdi, kalıcı
  çerezler, yer imleri, geçmiş, `C:/indirilenler/` indirme yöneticisi, yakınlaştırma ve sayfada bul.

## 1.0 — Donanım Hızlandırma, Dinamik Ölçekleme, Ağ & OOM Kurtarma

**Bellek Yönetimi, Dinamik Görev Ölçekleme & OOM Kurtarma**

- `vmm_init()` ile `1 GB` üstü kurulu RAM için 2 MB huge page dinamik kimlik eşlemesi ve
  `>4 GB` yüksek fiziksel bellek havuzu; per-CPU çerçeve önbelleği (`VMM_PCPU_MAX_CPUS = 16`)
  ve `PAT` Write-Combining (`vmm_map_wc_range`).
- Bellek haritası (`include/arctian/memory_map.h`): çekirdek heap `HEAP_BASE = 0x0A100000`
  (63 MB) ve Ring-3 fiziksel çerçeve havuzu `VMM_POOL_BASE = 0x0E000000` (96 MB yedek
  varsayılan + dinamik yüksek RAM havuzu) ayrıldı.
- Dinamik RAM-ölçekli görev ve süreç tabloları: `sched.c` (`SCHED_MAX_TASKS = 16384`,
  `SCHED_TASK_HARD_CAP = 4096`), `proc.c` dinamik dilimli süreç tablosu (8192 sürece kadar),
  `posix_proc.c` dinamik dilimli POSIX tabloları (2048 süreç, 8192 thread, 1024 pencere).
- **"RAM Doldu!" OOM Kurtarma Arayüzü (`desktop/oom_ui.c`, `sched.c`, `proc.c`):** Kritik bellek
  eşiğinde (`VMM_OOM_CRITICAL`) veya `Ctrl+Shift+M` ile açılır; kullanıcı/WLF/POSIX görevlerini
  `TASK_FROZEN` ile dondurup masaüstünü ayakta tutar, uygulamaları RAM kullanımına göre sıralayıp
  sonlandırmaya veya bellek açılınca masaüstüne geri çözmeye izin verir.

**Grafik, GPU & Ses Alt Sistemi**

- Modüler GPU yöneticisi (`kernel_plus/drivers/gpu*.c`): PCI Display sınıfı taramasıyla
  **VirtIO-GPU 2D** (`gpu_virtio.c`), **Intel UHD/Iris Xe/Arc** (`gpu_intel.c`), **NVIDIA**
  (`gpu_nvidia.c`), **Bochs/QEMU BGA** (`gpu_bga.c`) ve UEFI GOP / BIOS VBE LFB yedeği;
  SSE2/AVX2 hızlandırmalı blitter (`gpu_simd.c`), VSync `flip()` ve kirli bölge `present_rect()`.
- Ses arka uçları (`kernel_plus/drivers/audio.c`): **Intel HDA** (`hda.c`), **VirtIO-Sound**
  (`virtio_snd.c`), **AC97** (`ac97.c`) ve **PC Speaker** yedek yolu.

**Depolama & AFS v6**

- **AHCI** (`lib/blk_ahci.c`): 64-PRDT 64-bit scatter-gather DMA (`AHCI_MAX_PRDT = 64`).
- **NVMe** (`lib/blk_nvme.c`): 64-bit PRP1/PRP2/PRP-List DMA ve dinamik `kmalloc` kuyrukları.
- **AFS v6 (`lib/afs.c`):** Bellek içi sıralı boş-alan (`free-extent`) geri kazanımı (bitişik
  blok birleştirme + `data_next` kuyruk geri çekme), **255 bayta** kadar uzun yol desteği
  (`AFS_PATH_MAX = 256`, `.afs_longnames` tablosu) ve 1024 dosyaya kadar dinamik `kmalloc` tablosu.

**Ağ Yığını & Sürücüler**

- **VirtIO-Net (`kernel_plus/drivers/virtio_net.c`):** Modern (1.0/1.2) + Transitional PCI sürücüsü.
- **IPv6 Dual-Stack Temeli (`kernel_plus/net/ipv6.c`):** EUI-64 link-local, ICMPv6 Echo, NDP
  NS/NA/RS/RA ve otomatik SLAAC önek yapılandırması.
- **TCP (`kernel_plus/net/tcp.c`):** RFC 6582 NewReno tıkanıklık denetimi, dinamik sıra dışı
  (OOO) yeniden birleştirme (`TCP_OOO_SLOTS = 8`), entropi tabanlı ISN, **64 eşzamanlı TCP
  bağlantısı** (`TCP_MAX_CONNECTIONS = 64`) ve 128 KB dairesel RX halkası (`TCP_RXBUF_SIZE`).
- **HTTP/HTTPS & WebSocket (`kernel_plus/tls/https.c`):** Tam HTTP yöntemleri (`http_request`),
  `gzip`/`deflate` açma (`inflate_gzip.h`) ve RFC 6455 WebSocket (`ws_open`/`ws_send`/`ws_recv`).

**UEFI, Hibrit ISO, Kurulum & Uygulamalar**

- **UEFI & Hibrit ISO (`boot/uefi/`, `tools/arctianpack.py`):** 64-bit `>4 GB` GOP çerçeve
  tamponu (`uefi_info.fb_phys/fb_size`), Çift El Torito + ISOHybrid MBR/GPT **Hibrit BIOS+UEFI ISO**
  (FAT32 ESP içinde `BOOTX64.EFI` + `KERNEL.ELF`) ve isteğe bağlı kurulum paketleri
  (`WEBKIT.ELF`, `DEJAVU.TTF`, `CACERT.PEM`).
- **Kurulum Sihirbazı (`install/installer.c`):** Grafiksel disk seçimi ve ilerleme ekranı, en az
  **5 GiB** (`10485760` sektör) kurulum diski denetimi, kurulum sonrası LBA 0/superblock/tablo/AFS
  doğrulaması ve isteğe bağlı `C:/webkit.elf` kurulumu.
- **Masaüstü, SDK & Tarayıcı:** Pencere kaydı limiti `MAX_WINS = 256` + geri dönüşüm havuzu
  (`desktop/windows.c`); `sdk/webkit/` WPE WebKit köprüsü + `arctian-pkg-config` / `arctian.cmake`;
  yerleşik Tarayıcı için JPEG/GIF/BMP çözücü (`browser_img.h`), `<canvas>` 2D rasterizer
  (`browser_canvas.h`), yerleşik PDF görüntüleyici (`browser_pdf.h`) ve `PAGE_RAW_MAX = 2 MB`.

## 0.9.2 — POSIX/C++20, B3 İzolasyonu ve Ağ Hızlandırma

Güncel 1.0 durum özeti: [docs/1.0-durum.md](docs/1.0-durum.md).

**Ring-3 POSIX & C++20 Alt Sistemi (`kernel_plus/posix/`, `sdk/toolchain/`)**

- `SYSCALL/SYSRET` (`EFER.SCE`, `STAR`, `LSTAR`, `FMASK`, `KERNEL_GS_BASE`) Linux x86-64 System V ABI
  dağıtıcısı; mevcut `int 0x80` `.wlf` yolunu bozmadan birlikte çalışır.
- ELF64 yükleyici (`ET_DYN` statik PIE + `ET_EXEC`), `fork`/`vfork`, `execve`, `wait4`, `exit_group`
  ve terminalde `elfrun <dosya.elf>` komutu.
- Sanal bellek: `mmap` (anonim, dosya, `MAP_SHARED` sayfa refcount), `munmap`, `mprotect`
  (`PROT_EXEC` JIT uyumlu), `brk`.
- İş parçacıkları & senkronizasyon: `clone`, `arch_prctl(ARCH_SET_FS)` TLS, `futex` (`WAIT`/`WAKE`),
  `libstdc++` `std::thread`, `mutex`, `condition_variable` ve POSIX `sem_t`.
- Çapraz araç zinciri (`sdk/toolchain/`): Alpine v3.21 pure-musl 1.2.5 + `libstdc++` 14.2 (C++20,
  istisnalar ve RTTI destekli) + `arctian-cc` / `arctian-cxx`.
- Dosya, zaman & olaylar: `openat`, `pipe2`, `dup2`/`dup3`, `fcntl(O_NONBLOCK)`, `getdents64`,
  `poll`/`ppoll`, `epoll_create1`/`epoll_ctl`/`epoll_wait`, `clock_gettime`, `nanosleep`, `getrandom`.
- Soket & IPC: `AF_UNIX` (`socketpair`, `SCM_RIGHTS`, `bind`/`listen`/`accept`) ve `AF_INET`
  TCP (`getaddrinfo`, non-blocking + `epoll`).
- Paylaşımlı grafik & girdi köprüsü (`posix_gfx.c`, `desktop/posix_win.c`, `arctian/gfx.h`):
  ARGB8888 `mmap` pencere tamponu, `present_rect` ve fare/klavye/tekerlek/kapanma olay akışı.
- Doğrulama: `tests/posix/test_suite.cpp` **23/23 PASS**, `EXC=0`.

**`.wlf` B3 Ring-3 GUI İzolasyonu**

- Native GUI `.wlf` uygulamaları ayrı CR3 / Ring-3 alanında çalışır: kullanıcı uzayında
  94-API `wolf_api_t` trampoline köprüsü, `SYS_WOLF_CALL`/`SYS_WOLF_WAIT`, LVGL masaüstü broker
  kuyruğu ve `wolf_main` dönüş kancalı olay döngüsü (mevcut paketler yeniden derlenmeden çalışır).

**Ağ Performansı**

- 4 MB HTTP indirme QEMU'da **76 sn (55 KB/sn) → 1,11 sn (3,77 MB/sn, 68× hızlanma)**.
- TCP: 128 KB dairesel alıcı halkası (sıfır `memmove`), SYN `MSS=1460` + RFC 7323 pencere ölçekleme
  (`shift=3`), 32-bit `peer_window`, birleştirilmiş ACK.
- e1000: 128 RX / 32 TX halka, toplu RDT (16'lı) ve ITR=256µs, `netbg` 16 ms yield, bekleme
  döngülerinde `sched_sleep_ms(1)`.
- TLS: CPUID leaf1 ECX bit25 AES-NI çalışma zamanı şifre sırası (TCG'de ChaCha20, AES-NI'da AES-GCM).

**SMP, Önyükleme, Giriş & Güvenlik**

- `-smp 4` `mutex`/`sem`/`pipe`/`mq`/`kthread` yarış koşulları kapatıldı (`on_cpu` yığın koruması,
  AP `s_ap_idle`), `lib/serial.c` IRQ-güvenli spinlock eklendi (`EXC=0`).
- UEFI Paket 3: OVMF altında GOP + ACPI RSDP (`0x7100`) + ESP'den `KERNEL.ELF` + `ExitBootServices`
  ile 64-bit çekirdeğe atlama doğrulandı.
- Giriş ekranları (`giris.md`) Manrope TTF fontlarıyla iki ekran akışına geçirildi; öz-testler
  `SELFTEST=1` / `WFL_SELFTEST=1` bayraklarına ayrıldı; `rtc.c` CMOS kilidi eklendi.
- `build.bat` ext4 kopya derleme (`tools/build-release.sh`); QuickJS tamamen kaldırıldı (`wolf_engine`
  yer tutucu ABI stub'a indirildi); `tools/sig_keygen.py` (`secrets.SystemRandom`) ile RSA-2048
  anahtar çifti yenilendi.

## 0.9 — UEFI iskeleti, DPI ölçekleme ve SMP

Bu bölüm 0.9 dalgasında tamamlanan altyapıyı toplar. Güncel 1.0 hazırlık durumu için
[docs/1.0-durum.md](docs/1.0-durum.md).

**UEFI ve grafik**

- UEFI önyükleyici iskeleti (`boot/uefi/`): PE/COFF `x86_64` girişi, kendi
  `entry.asm`/`linker.ld` düzeni ve COM1 üzerinden `ARCTIAN-UEFI-OK` test işareti.
- GOP (Graphics Output Protocol) keşfi ve mod bilgisinin (genişlik, yükseklik,
  stride, piksel biçimi) ortak bootinfo sözleşmesine yazılması; BIOS ve UEFI
  yolları aynı ekran sözleşmesini paylaşıyor.
- PE/COFF başlığında alt sistem `10` (EFI uygulaması) ve sabit image base
  (`0x400000`) düzeltmesi; firmware imajı artık geçerli bir EFI uygulaması olarak
  yükleyebiliyor.
- `tests/uefi/run.sh`: `make efi` → MBR + FAT32 ESP imajı (mtools) → OVMF/QEMU →
  COM1'de `ARCTIAN-UEFI-OK` doğrulaması. Yalnızca QEMU/OVMF üzerinde çalıştırıldı.

**DPI ölçekleme**

- Ölçekleme çekirdeği: 100/125/150 yüzdeleri için `px`/`unpx` ve font boyutu
  dönüşümleri; sabit ekran boyutu varsayımı kaldırıldı, ayar `kv` üzerinden
  `uiscale` olarak saklanıyor ve açılış öncesi `dpi_init` ile okunuyor.
- Masaüstü metrikleri (pencere başlığı, düğme boyutu, varsayılan pencere ölçüsü,
  snap mesafesi, sürükleme sınırları) ölçek çarpanına bağlandı; yoğunluk seçimi
  arayüzü de ölçekleniyor.
- Birim testleri host `gcc` ile geçti (round-trip dönüşümleri dahil).

**SMP**

- CPUID ve MADT ile topoloji keşfi (x2APIC kimlikleri MADT type 9'dan), LAPIC
  etkinleştirme ve IPI temelleri; per-CPU veri, BSP kaydı, spinlock ve atomik
  ilkelleri.
- AP trampoline blob'u (16→32→64 bit geçişi) ve AP başlatma sürücüsü; AP'ler için
  per-CPU GDT/TSS ve IDT devri. QEMU `-smp 4` altında 3/3 AP çevrimiçi.
- `tests/smp/run.sh`: derleme, trampoline boyut/sembol kontrolü ve QEMU `-smp 4`
  çalıştırmasında `AP online=` satırının doğrulanması.

## 0.9 — Açılış animasyonu ve AX201 ürün yazılımı

**Açılış animasyonu**

- Metin logu ve yaylı yükleme ekranı kaldırıldı. Çekirdek yüklenmeye başladığında
  1280×720 "Arctian OS" sahnesi geliyor: 288 parçalı arka plan, logo, başlık ve altta
  ilerleme çubuğu. Çizim tamamen C tarafında, çekirdeğin kendi arka tampon
  rasterleştiricisiyle yapılıyor; LVGL veya tarayıcı yok.
- İlerleme çubuğu gerçek init adımlarına bağlı: her adım ağırlığı kadar ilerletiyor,
  uydurma zamanlayıcı yok. En az gösterim süresi dolduktan sonra sahne sönüyor ve
  masaüstüne geçiliyor.
- Animasyon ayrı bir görevde ~16 ms kareyle dönüyor; ana init iş parçacığı yalnızca
  ilerleme bildiriyor. Tam ekran kopyalama 64 bitlik depolara çevrildi, TCG altında
  kare süresi ~340 ms'den ~45 ms'ye indi.
- Parça geometrisi, renkleri ve zamanlamaları derleme anında üretiliyor
  (`tools/bootanim_gen.py` → `kernel_plus/system/bootanim_data.c`); çekirdekte
  kayan nokta yok.

**İlk açılış**

- "Hadi seni tanıyalım!" 5 slaytlık özelleştirme sihirbazı kaldırıldı; kod, Makefile
  girdileri, `dsk.h` bildirimleri ve tasarım varlıkları birlikte gitti. Disk kurulum
  sihirbazı ve hoş geldin turu yerinde duruyor.

**Intel AX201**

- Sürücü ürün yazılımını gerçek gen3 akışıyla yüklüyor: TLV ayrıştırma, DMA'ya
  yerleştirme, ALIVE el sıkışması (durum yalnızca RX halkasındaki `0xCAFE`
  işaretinden okunuyor), FH/UMAC yapılandırması.
- Tarama ve bağlantı adımları henüz iskelet; ALIVE dışında sahte bir hazır durumu
  bildirilmiyor. Gerçek donanımda doğrulanmayı bekliyor.

## 0.8.2 — Çekirdek güncellemesi ve disk yerleşimi düzeltmesi

**Çekirdek güncellemesi (`update kur` → B yuvası)**

- Manifestoda `kind=system` ile anılan sistem dosyaları (örn. `kernel_plus`) B
  yuvasına akışla indirilir: gövde RAM'den geçmeden disk sektörlerine yazılır,
  HTTPS tamponunun 320 KB tavanı çekirdek boyutunu artık sınır değildir. Boyut +
  CRC32 doğrulanır, AMOD başlığı manifestodaki yük/bss/type alanlarıyla yerinde
  üretilir.
- Yuva devri en son yapılır: veri → B tablosu (LBA 32768) → durum kaydı. Hangi
  adımda elektrik kesilirse kesilsin eski çekirdek boot eder. Yeni çekirdek
  masaüstüne ulaşamazsa iki denemede otomatik olarak eski yuvaya dönülür; bu
  bekçi artık yükleyicinin erken adımıdır, çekirdek erken çökerse de çalışır.
  Ayarlar ve uç birim aynı `update kur` yolunu kullanır; kurulumdan sonra
  yeniden başlatma beklenir.
- Eski düzen disklerde (AFS 20000'den formatlı) B bölgesi kullanıcı dosyalarına
  denk gelir; çekirdek güncellemesi o disklerde yazmadan "temiz kurulum gerekir"
  der.
- `make_update.py --system kernel_plus=<bin>` alanları manifestsize yazar
  (layout.json + ELF'ten); `tools/publish_os_update.sh` ile aynı şekilde
  yayınlanır.

**Disk yerleşimi (kritik düzeltme)**

- AFS 20000'den formatlanıyordu ama modül bölgesi 27836'da bitiyor: yeni
  kurulan diskte kullanıcı dosyaları diskteki desktop modülünün üstüne
  yazılabiliyordu. Yeni kurulumlarda AFS 65536'dan formatlanır (başlık sürümü
  6); eski diskler 20000'den mount edilmeye devam eder, ilk açılışta yalnız
  sürüm kayıtları yenilenir, veri yerinden kıpırdamaz.
- Paketleyici bekçisi: modül bölgesi 32768'i aşarsa paketleme hata verir.
- Kurulum temiz yuva durumu yazar: diske bulaşmış eski `pending=B` kaydı yeni
  kuruluma miras kalmaz.
- `slot_switch_pending` her geçişte deneme sayacını 1'e çeker; sayaç
  biriktiğinde bir sonraki güncelleme hiç denenmeden geri alınıyordu.
- Uç birimde `touch <yol>` düzeltmesi: tam yol "ad" olarak verildiği için
  `C:/C:/...` kaydı oluşuyor, dosya hiçbir klasör listesinde görünmüyordu.

## 0.8 — Yeni arayüz dili, yeni uygulamalar, CC BY lisansı

**Arayüz: tasarım şartnamesi 8 ekrana uygulandı**

- Tema jetonları ve pencere çerçevesi tasarım şartnamesine çekildi; Ayarlar, Medya,
  Görev Yöneticisi, Dosya Yöneticisi ve DuckStore aynı dille yeniden yazıldı.
- DuckStore: hero kutusu kısaltıldı, ikon kutusu ve düğmeler küçültüldü.
- Görev Yöneticisi: sürücüsünde ölçüm bulunmayan değerlerde (bellek/disk/ağ) uydurma
  sayı yerine dürüst "-" gösterilir; CPU kullanım yüzdesi gerçektir.

**Yeni uygulamalar**

- Ofis paketi (`.wlf`, GitHub sürümünden kurulur): **Şafak** (toplayıcı), **Mürekkep**
  (belge), **Abaküs** (tablo), **Sahne** (sunum), **Mercek** (PDF). Hepsi tam ekran
  kendi başlık çubuğuyla çalışır, 1280×720 referanslı ölçekleme her çözünürlükte net.
- Tam ekran uygulama desteği: bayraklı `.wlf` penceresi çerçevesiz açılır, dock gizlenir.
- `wf install <ad>` uygulamayı katalogdan indirir; katalog adresi `wf store` ile değişir.
  Katalog GitHub sürümünde durur, yeni uygulama için sistemi yeniden derlemek gerekmez.
- Beş ofis uygulaması artık gerçek fotoğraflardan üretilen ikonlarla geliyor (128 px
  PNG, paketin içinde taşınır; eski kutu çizimi ikonlar kalktı).
- `.wlf` paket tavanı kalktı: eski 256 KB sınırı yok; paketler dosya boyutu kadar
  dinamik okunuyor. Biçimin kendi tavanı ~4 GB (32 bitlik kaydırmalar), pratikte
  sınır artık bellek ve disk. 2,2 MB'lık test paketi HTTP üzerinden kurulup
  çalıştırıldı; indirmeden önce HEAD ile boy soruluyor, tampon baştan doğru ayrılıyor.
- Metin: düz metin düzenleyici (birlikte açma ve dosya argümanını destekler).
- Takvim: aylık ızgara, gün adları ve bugün vurgusu.
- Foto: görüntü görüntüleyici, dosya listesi ve ayrıntı satırıyla.

**Tarayıcı kaplaması**

- 36 px sekme şeridi (tek gerçek sekme, canlı sayfa başlığı), 52 px hap biçimli adres
  satırı, SVG simge düğmeleri ve 28 px alt durum çubuğu; pencere 880×560.

**Sistem güncellemesi GitHub sürümlerinden**

- Güncelleme kaynağı DuckStore sunucusundan bu deponun GitHub sürümlerine taşındı:
  OS en son sürümdeki `update.txt` varlığını (imzalı manifest) çeker, modülleri aynı
  sürümden indirir; boyut + CRC ve imza doğrulanır. Yeni sürüm yayınlamak için
  `tools/publish_os_update.sh <sürüm> <not> <modüller>` yeter — OS'i yeniden derlemek
  gerekmez. Eski DuckStore beslemesi (`/v1/update`) taban adresiyle korunur.
- Terminal `update url <adres|varsayilan>`: kaynağı değiştirir, `C:/etc/update.cfg`
  içinde kalıcıdır. Modüller kurulurken tek tek imza + CRC doğrulandığından yanlış
  bir ayna sahte sürüm üretemez; adresi açmak bu yüzden güvenlidir.
- Yerel HTTP sunucunun GitHub varlık düzenini taklit ettiği testte `update kontrol`
  → `update kur` akışı uçtan uca doğrulandı (kurulum, kalıcı sürüm, yeniden
  açılışta "Sistem guncel"). İlk taban sürüm olarak `os-0.8` yayınlandı; varlıklar
  ve imza OS'un sabitlediği anahtarla dışarıdan doğrulandı. Gerçek GitHub'a HTTPS
  el sıkışması aynı gün bulunan bir TLS hatası yüzünden takılıyordu; düzeltmeden
  sonra `update kontrol` canlı GitHub kaynağıyla uçtan uca doğrulandı (aşağıya
  bakınız).

**Düzeltmeler**

- Her yerde "bağlantı hatası" veren HTTPS düzeltildi: TLS gönderim BIO'su 1400
  bayttan küçük kayıtları (ClientHello dahil) fiilen göndermeden "tamam" dönüyordu;
  el sıkışma boşta bekleyip zaman aşımına düşüyordu. Hata, want_write sayaçlarının
  eklendiği parçalama yeniden yazımında girmişti.
- Yönlendirme tamponları 512'den 1024 bayta çıkarıldı: GitHub'ın imzalı varlık
  bağlantıları bu sığaya sığmayıp kesiliyor, sunucu "jwt-not-provided" dönüyordu;
  artık GitHub'dan modül indirme de aynı yoldan yürüyor.
- TR-Q ve EN klavye haritalarında eksi ile eşit tuşu yer değiştirmişti (AT set1'de
  0x0C "-/*", 0x0D "=?"); "-" yazan her yerde "*" çıkıyordu.

- Onay penceresi açıkken uygulama isteklerinin otomatik reddedilmesine yol açan izin
  bayrağı hatası düzeltildi; bayrak artık yalnız diyalog pompalanırken devrede.
- C'nin açgözlü onaltılık kaçışları (`\xHH` sonrasındaki onaltılık karakteri yutar)
  kaynak genelinde tarandı ve temizlendi; Takvim'de "Çar" etiketini bozan hata buydu.
- AFS: dosya büyüyen bir yazım aldığında komşusunun LBA'si yeni aralığın içinde
  kalıyordu ve komşunun üstüne yazılıp o bozuluyordu (örneğin yeni sürüm `.wlf`
  kurulumunda). Artık komşu çakışıyorsa dosya boş bölgeye taşınıyor; kurulum
  zinciri bozulmadan tamamlanıyor.

**Lisans**

- Apache-2.0 → **CC BY 4.0**: ticari kullanım dahil her türlü kullanımda atıf
  zorunlu; `third_party/` altındaki kütüphaneler kendi lisanslarında kalmaya devam eder.

## 0.7 — Koruma sınırı, güncelleme ve `.wlf` uygulama modeli

**Güvenlik / koruma sınırı**

- Her açılan uygulamaya bir **özne** (kimlik) kaydı: kaynak (gömülü / kutulu / güvenilir)
  + ad + yetenek maskesi. Kutulu paket varsayılan olarak `CAP_ALL` **almaz**; paket ne
  bildirdiyse o.
- **Karar deposu:** (özne, tür, kaynak) anahtarıyla *bir kez / her zaman / reddet*
  kararları; "bir kez" kararları çağrı bitince silinir.
- **Onay penceresi:** kullanıcıya "Bir kez / Her zaman / Reddet" seçenekleriyle ulaşan
  gerçek bir pencere; olay döngüsü gerçek zamanda ilerler (eskiden pencere kullanıcı
  görmeden kapanıyordu).
- **Dosya kapısı:** tüm dosya erişimi tek noktadan (`caps_fs_guard`) geçer; tutamac
  açıldığı yola bağlıdır, bu yüzden `read`/`write` çağrıları da kapıdan geçer.
- **Komut kapısı:** ağ ve komut çağrıları aynı politikaya bağlandı; uygulama başına tek
  ağ izni.
- **İmza:** `.wlf`/manifesto gövdesi RSA-2048 + SHA-256 ile imzalanır; imzasız paket ve
  imzasız manifesto kabul edilmez.

**Güncelleme ve A/B yuva**

- Ayarlar → Güncelleme sekmesi: otomatik kontrol + gecikme sınırı ayarı.
- İmzalı sürüm manifestosu (modül başına boyut + CRC32); imzasız manifesto reddedilir.
- Kurulum kapanışta/açılışta yapılır; yarım kalan kurulum açılışta tamamlanır.
- **A/B sistem yuvası:** kesik yuva algılanırsa önceki yuvaya otomatik geri dönüş;
  kullanıcı diski yuvalardan bağımsızdır.

**Uygulama modeli: yalnızca `.wlf`**

- Gömülü çekirdek uygulamalar dışında uygulama biçimi tek: `.wlf`. `.dpk` tarama, kurulum
  ve uzantısı kaldırıldı; `C:/apps` yalnızca `.wlf` taşır.
- Başlığa `role` alanı eklendi: `ui` (pencere), `cli` (terminalde koşar), `runtime`
  (yalnızca "birlikte aç" ile dosya alarak koşar).
- Meta bloğu (`"WLFM" + key=value`): `role`, `handles`, `entry`, `terminal`. Dosya
  eşleştirmesi `appviewer` kategorisi yerine `handles` üzerinden yapılır; tek aday varsa
  doğrudan açılır, birden fazlaysa seçim penceresi çıkar.
- `wf` komutları: `wf list/info` rol ve uzantı listesi gösterir, `wf run <ad> [dosya]`
  role göre çalıştırır, `wf build <ad> [ui|cli|runtime] [uzantılar]` meta bloğunu üretir.
- SDK: `wlfpack.py` rol/meta yazar ve `--inspect` ile gösterir; `wolf_api_t` sonuna
  `arg()` eklendi (çalışma zamanı dosya argümanı).
- Örnekler: `uidemo` (ui), `sayac` (cli), `pyrt` (runtime, `handles=py,pyw`).

**Masaüstü ve arayüz**

- Dock, arayüz şartnamesine göre yeniden yazıldı: 1600×900 referanslı `dk_s()` ölçek
  birimi, düz opak zemin, 44×44 düğmeler, durum göstergeleri, seviyeye göre çizilen pil
  ikonu, `IBM Plex Sans` saat/tarih.
- Yeni imlec: 32×32 SVG'den, piksel başına 8×8 alt örnekleme ile kenar yumuşatma; işaretçi
  ucu tam fare konumuna oturur.
- Varsayılan çözünürlük 1280×720; 720p'de arayüz taban ölçeği 0.7, masaüstü simgeleri ayrı
  ölçeklenir, satır aralıkları yazı yüksekliğinden hesaplanır.
- Asistan ayrı bir uygulama olarak da açılır (başlat menüsünde "Asistan", `run asistan`).
- Dosya yöneticisi: çizili 16px klasör/dosya ikonları (eksik TTF glyph'leri yüzünden boş
  kutu çiziyordu), uzantıya göre renk, ayırt edilebilir hover/seçim rengi, geri oku, arama
  kutusu, boyut sütunu ve detay paneli.

**Düzeltilen hatalar**

- **Sürüm 0.0 görünüyordu:** komut satırından `EXTRA_CFLAGS=...` verildiğinde GNU make,
  Makefile içindeki `EXTRA_CFLAGS +=` satırlarını yok sayıyordu; sürüm ayrı bir değişkende
  (`VERSION_CFLAGS`) tutuluyor.
- **Hedefsiz `make` hiçbir şey derlemeden başarılı dönüyordu:** `-include $(shell find build
  -name '*.d')` satırı `all:` hedefinden önce durduğu için GNU Make 4.x `.d` dosyasındaki ilk
  hedefi varsayılan hedef sayıyordu; imaj hiç yenilenmiyordu. Makefile başına
  `.DEFAULT_GOAL := all` eklendi, `-include` kapsamı `$(BUILD)` ile sınırlandı.
- **ISO sessizce atlanıyordu:** paketleyici `xorriso` hatasını yutup RC=0 dönüyordu. Artık
  hata derlemeyi durdurur; `.img` ve `.iso` `.part` üzerinden atomik taşınır + boyut
  doğrulaması yapılır. `build.bat` da RC=0'a güvenmez, ISO'nun bu derlemeden sonra
  yenilendiğini işaret dosyasıyla doğrular.
- **Saat yanlış saat dilimindeydi:** RTC/NTP UTC tutarken masaüstü UTC gösteriyordu;
  `clock_set_tz_offset()` eklendi (varsayılan `Europe/Istanbul`), saat artık
  saat/dakika/saniyeyi birlikte karşılaştırıyor.
- **Masaüstü ikonları kayboluyordu:** ikon katmanı sabit indekse taşınıyordu; perde
  nesneleri arka plana alınınca katman duvar kâğıdının altına düşüyordu. Artık katman her
  zaman duvar kâğıdının hemen üstüne taşınır.
- **Parlaklık/gece modu hiç görünmüyordu:** perdeler arka plana alınıyordu; etkin perdeler
  en öne alınır, ayar yazıldıktan hemen sonra uygulanır.
- **Hızlı ayarlar/bildirim listesi tek satıra yığılıyordu:** LVGL'de varsayılan layout
  olmadığı için flex flow gerekiyordu.
- **`ls C:/` alt klasör dosyalarını gösteriyordu:** AFS'de torun yollar kök listesine
  sızıyordu; yalnızca gerçek doğrudan çocuklar listelenir.
- **Türkçe metinler bozuk görünüyordu:** birkaç kaynak dosyada UTF-8 → CP1252 → UTF-8
  zinciriyle iki kez kodlanmış 68 metin geri çevrildi; kurulum ve hesap ekranı doğru
  çiziliyor.
- **Uygulama ikonları testere kenarlıydı:** en yakın komşu ile küçültme yerine alan
  ortalaması (`scale_hq`) kullanılıyor.
- **Asistan yanıt üretmiyordu:** giriş metni temizlendikten *sonra* okunuyordu; artık önce
  kopyalanıyor.
- Terminal yol çözümlemesi (`path_resolve` / `path_normalize`) sürücü harfli mutlak yol,
  ters bölü ve `..` için sağlamlaştırıldı.

**Yapılmayanlar (bilerek açık)**

- Ring 3 + süreç başına ayrı adres alanı ve çizim mesaj kuyruğu bu sürümde açılmadı: mevcut
  uygulama modeli tüm kodu masaüstüyle aynı adres alanında çalıştırıyor ve bu, çekirdek +
  çekirdek artısı + kabuğun birlikte yeniden yazılmasını gerektiriyor. Yarım bir izolasyon
  güvenlik vaadi vermez. Sıradaki adım: syscall ABI + çizim kuyruğu, sonra süreç başına
  adres alanı.

## 0.6 — Ağdan kurulum, ABI koruması, arayüz cila

- Ağdan `.wlf`/`.dpk` kurulumu, depo feed iyileştirmeleri.
- **ABI imzası:** paketler masaüstü sembol adreslerine mutlak linklendiği için, masaüstü her
  derlendiğinde ABI imzası değişir ve paketler yeniden üretilir; aksi hâlde eski paketler
  sayfa hatası veriyordu.
- `.wlf` native uygulamalar için `APP_BASE`'den ayrı bir yükleme bölgesi: üçüncü paket ile
  adres çakışması giderildi.
- Dock arama kutusu gerçek arama yapıyor (menüyü açıyor ama klavye odağını çalmıyor).
- Açık tema paleti; LVGL taban teması da doğru değiştiriliyor.
- Tarayıcı: yer imleri, indirme yöneticisi, ileri yığını, `fetch`/`XMLHttpRequest` köprüsü.
- AFS'de yerel yeniden adlandırma; dosya yeniden adlandırma/taşıma akışı.
- Çalışma alanı göstergesi, pencereyi çalışma alanına taşıma (Ctrl+Alt+Shift+,/.), kare hızı
  HUD'u, klavye düzeni seçimi, Özellikler penceresi, 0.6 öz-testleri.

## 0.5 — Kullanıcı deneyimi

- Kurulum sihirbazı (Kur / Dene, dil-klavye-saat seçimi, disk seçimi, kalıcı AFS).
- Keşfedilebilirlik: "Tümü" uygulamalar, kategoriler, sağ tık menüleri.
- Dosya deneyimi: uzantı ilişkilendirme, kopyala/taşı, sürükle-bırak, geri al, arama.
- Hızlı Ayarlar + OSD + bildirim merkezi.
- Pencere yönetimi: snapping reflow, Alt+Tab önizleme, boyut hatırlama.
- Pano ve metin düzenleme kısayolları (Ctrl+C/V/X/A, geri al).
- Sistem özelliklerini arayüze taşıma (NTP, yazdırma, dmesg, sürücüler).
- Erişilebilirlik: yazı ölçeği, yüksek kontrast, odak halkası, büyük simge.
- İlk kullanım: hoş geldin turu, yardım paneli, boş durum ekranları.
- Sürücü çalışması: Atheros ath9k (AR928x) uçtan uca (EEPROM/reset/halka/kanal/tarama/
  auth-assoc/WPA2 el sıkışma/CCMP); Intel AX201 firmware + tarama-bağlantı iskeleti.

## 0.4 ve öncesi — çekirdek işlevler

Önyükleyici (MBR/El Torito), uzun mod + sayfalama, modüler imaj biçimi, temel sürücüler
(PCI, VESA, blok aygıtları, klavye-fare), AFS, LVGL tabanlı kabuk, ASI arayüzü, ses,
ağ yığını ve kurulum akışının çekirdeği.
