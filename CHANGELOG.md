# Sürüm kayıtları

Sürüm numarasının tek kaynağı [`version.json`](version.json)'dur; Makefile derleme
anında okuyup `ARCTIAN_VERSION` olarak gömer.

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
