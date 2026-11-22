/**
 * Arctian - .wlf native ikili yukleyici (kullanici adres alani)
 * ============================================================
 *
 * Duz (flat) WLF_KIND_NATIVE imajini ayri bir PML4'te kullanici sanal
 * adres araligina yukler. Sayfa duzeni:
 *
 *   [ kod  : image_size, X (yazilamaz) ]
 *   [ veri : bss_size,   NX + W       ]
 *   [ yigin: 16 KB,      NX + W       ]
 *
 * Kaynak .wlf imaji cekirdek baglaminda (kimlik eslemeli) okunur; hedef
 * sayfalar vmm_alloc_frame ile ayrilir ve fiziksel adresleri kimlik
 * eslemeli oldugundan dogrudan kopyalanabilir. W^X: kod sayfalari W
 * biti olmadan, veri/yigin NX ile eslenir. NX yalnizca IA32_EFER.NXE=1
 * iken yazilir.
 */
#include "wlf_user.h"
#include "vmm_space.h"
#include "alloc.h"
#include "wlf_abi.h"
#include "wolf.h"
#include "serial.h"
#include <stddef.h>

extern void *memcpy(void *dst, const void *src, size_t n);
extern void *memset(void *s, int c, size_t n);

#define WLF_PAGE_SIZE   0x1000ull
#define WLF_CODE_PAGES(size)  (((uint64_t)(size) + WLF_PAGE_SIZE - 1ull) / WLF_PAGE_SIZE)

/* Basligi dogrular. 0 = gecerli, aksi halde WLF_USER_ERR_*. */
static int wlf_header_check(const wlf_header_t *h, uint64_t size) {
    if (h->magic != WLF_MAGIC) return WLF_USER_ERR_HEADER;
    if (h->version != WLF_VERSION) return WLF_USER_ERR_HEADER;
    if (h->kind != WLF_KIND_NATIVE) return WLF_USER_ERR_HEADER;
    if (h->image_size == 0) return WLF_USER_ERR_HEADER;

    /* Baslik + payload dosyanin icinde olmali (tasma denetimi). */
    if (h->payload_off < WLF_HEADER_SIZE) return WLF_USER_ERR_HEADER;
    if ((uint64_t)h->payload_off + (uint64_t)h->payload_len > size)
        return WLF_USER_ERR_RANGE;
    if ((uint64_t)h->payload_off + (uint64_t)h->image_size > size)
        return WLF_USER_ERR_RANGE;
    if (h->image_size > h->payload_len) return WLF_USER_ERR_RANGE;

    /* Giris noktasi kod araliginda olmali. */
    if (h->entry_off >= h->image_size) return WLF_USER_ERR_ENTRY;

    return WLF_USER_OK;
}

/* Yalnizca eslenmemis cerceveleri serbest birakir; eslenmis olanlari
 * vmm_space_destroy zaten geri verir (cift serbest birakma olmaz). */
static void wlf_free_frames(wlf_user_t *out, uint32_t mapped) {
    uint32_t i;

    if (!out->frames) return;
    for (i = mapped; i < out->frame_count; i++) {
        if (out->frames[i]) vmm_free_frame(out->frames[i]);
    }
    kfree(out->frames);
    out->frames = NULL;
    out->frame_count = 0;
}

uint64_t wlf_user_start(const void *image, uint64_t size, wlf_user_t *out) {
    wlf_header_t hdr;
    uint64_t code_pages, data_pages, stack_pages, total_pages, total_size;
    uint64_t base_va, data_va, stack_va, stack_top, va;
    uint64_t flags_data, nx;
    uint32_t mapped = 0;
    uint32_t i;
    int rc;

    if (out) {
        memset(out, 0, sizeof(*out));
        out->magic = WLF_USER_MAGIC;
    }
    if (!image || !out || size < (uint64_t)WLF_HEADER_SIZE) {
        if (out) out->status = WLF_USER_ERR_ARG;
        return 0;
    }

    /* Baslik packed; hizasiz isaretciden guvenli kopya. */
    memcpy(&hdr, image, sizeof(hdr));

    rc = wlf_header_check(&hdr, size);
    if (rc != WLF_USER_OK) {
        out->status = rc;
        return 0;
    }

    code_pages  = WLF_CODE_PAGES(hdr.image_size);
    data_pages  = WLF_CODE_PAGES(hdr.bss_size);
    stack_pages = (uint64_t)WLF_USER_STACK_SIZE / WLF_PAGE_SIZE;
    /* B3: +3 sayfa: api tablosu+stublar (X), olay dongusu (X), tamponlar (W+NX) */
    total_pages = code_pages + data_pages + stack_pages + 3;

    /* Tasma / aralik denetimi: toplam alan kullanici VA'sina sigmali. */
    if (total_pages == 0 || total_pages > 0xFFFFFFFFull) {
        out->status = WLF_USER_ERR_TOO_BIG;
        return 0;
    }
    total_size = total_pages * WLF_PAGE_SIZE;
    if (total_size < total_pages || !vmm_space_user_range(VMM_USER_BASE,
                                                          total_size)) {
        out->status = WLF_USER_ERR_TOO_BIG;
        return 0;
    }

    base_va  = (uint64_t)VMM_USER_BASE;
    data_va  = base_va + code_pages * WLF_PAGE_SIZE;
    stack_va = data_va + data_pages * WLF_PAGE_SIZE;
    stack_top = stack_va + stack_pages * WLF_PAGE_SIZE;

    /* Adres alani. Basarisizlikta cerceve ayrilmadan cikilir. */
    out->space = (uint64_t)vmm_space_create();
    if (!out->space) {
        out->status = WLF_USER_ERR_NO_SPACE;
        return 0;
    }

    out->frames = (uint64_t *)kmalloc((size_t)total_pages * sizeof(uint64_t));
    if (!out->frames) {
        out->status = WLF_USER_ERR_NO_MEM;
        goto fail;
    }
    out->frame_count = 0;

    nx = vmm_space_nx_ok() ? VMM_PTE_NX : 0ull;
    flags_data = VMM_PTE_W | nx;              /* veri + yigin: W, NX */

    for (i = 0; i < (uint32_t)total_pages; i++) {
        uint64_t pa = vmm_alloc_frame();
        uint64_t flags;

        if (!pa) {
            out->status = WLF_USER_ERR_NO_MEM;
            goto fail;
        }
        out->frames[i] = pa;
        out->frame_count = i + 1u;

        va = base_va + (uint64_t)i * WLF_PAGE_SIZE;
        /* Kod: yazilamaz (X), veri/yigin: W + NX. B3: tablo+stub ve dongu
         * sayfalari X (salt okunur calisma), tampon sayfasi W + NX. */
        if (i < (uint32_t)code_pages) flags = 0ull;
        else if (i >= (uint32_t)(code_pages + data_pages + stack_pages))
            flags = (i == (uint32_t)total_pages - 1u) ? flags_data : 0ull;
        else flags = flags_data;

        if (vmm_space_map_user((vmm_space_t)out->space, va, pa, flags) != 0) {
            out->status = WLF_USER_ERR_MAP;
            goto fail;
        }
        mapped++;
    }

    /* Kodu fiziksel cercevelere kopyala (kimlik eslemeli). Kalan sayfa
     * kuyrugu ve bss zaten vmm_alloc_frame tarafindan sifirlanmistir. */
    {
        const uint8_t *src = (const uint8_t *)image + hdr.payload_off;
        uint32_t left = hdr.image_size;

        i = 0;
        while (left) {
            uint32_t chunk = left > (uint32_t)WLF_PAGE_SIZE
                           ? (uint32_t)WLF_PAGE_SIZE : left;
            memcpy((void *)(uintptr_t)out->frames[i], src, chunk);
            src  += chunk;
            left -= chunk;
            i++;
        }
    }

    /* ---- B3: GUI kopru sayfalari (fiziksel cerceve uzerinden yazilir;
     * kullanici eslemesi yukarida kuruldu). Tablo+stublar X, dongu X,
     * tamponlar W+NX. wolf_main'in ret adresi olay dongusune baglanir:
     * mevcut .wlf paketleri yeniden paketlenmeden calisir. ---- */
    {
        uint64_t pa_tab  = out->frames[code_pages + data_pages + stack_pages];
        uint64_t pa_loop = out->frames[code_pages + data_pages + stack_pages + 1];
        uint64_t pa_buf  = out->frames[code_pages + data_pages + stack_pages + 2];
        uint64_t api_va  = stack_top;
        uint64_t loop_va = stack_top + WLF_PAGE_SIZE;
        uint64_t buf_va  = stack_top + 2 * WLF_PAGE_SIZE;

        wolf_api_t *t = (wolf_api_t *)(uintptr_t)pa_tab;
        memset(t, 0, sizeof(*t));
        t->magic   = WOLF_API_MAGIC;
        t->version = WOLF_ABI_VERSION;
        t->size    = (uint32_t)sizeof(wolf_api_t);
        uint64_t *fn = (uint64_t *)((uint8_t *)t + 16); /* 3*u32 + 4 bayt hizalama */
        /* Stub konvansiyonu: 8 C argumanini (6 register + 2 yigin)
         * kullanici argbuf'ina yazar, sonra SYS_WOLF_CALL+i ile syscall.
         * Boylece 7-8 argumanli canvas_* API'leri de tasınır.
         * Yerlesim: sayfa0 tablo@0 + stub[0..40]@0x400 (74 bayt/adet);
         * sayfa1 stub[41..93]@0 + olay dongusu@0xF60; sayfa2 tamponlar. */
        uint64_t argbuf = buf_va + 640;
        uint8_t *s0 = (uint8_t *)(uintptr_t)(pa_tab + 0x400);
        uint8_t *s1 = (uint8_t *)(uintptr_t)pa_loop;
        for (uint32_t i = 0; i < 94u; i++) {
            uint64_t va = (i < 41u) ? (api_va + 0x400u + 74u * i)
                                    : (loop_va + 74u * (i - 41u));
            uint8_t *s = (i < 41u) ? (s0 + 74u * i) : (s1 + 74u * (i - 41u));
            fn[i] = va;
            uint32_t o = 0;
            /* mov [rip+d],rdi / rsi / rdx / rcx / r8 / r9 */
            static const uint8_t pre[6][3] = {
                {0x48,0x89,0x3D}, {0x48,0x89,0x35}, {0x48,0x89,0x15},
                {0x48,0x89,0x0D}, {0x4C,0x89,0x05}, {0x4C,0x89,0x0D} };
            for (int r = 0; r < 6; r++) {
                s[o] = pre[r][0]; s[o+1] = pre[r][1]; s[o+2] = pre[r][2];
                int32_t d = (int32_t)(argbuf + 8u * r - (va + o + 7));
                s[o+3] = (uint8_t)d; s[o+4] = (uint8_t)(d >> 8);
                s[o+5] = (uint8_t)(d >> 16); s[o+6] = (uint8_t)(d >> 24);
                o += 7;
            }
            /* mov rax,[rsp+8] ; mov [rip+d],rax */
            s[o++] = 0x48; s[o++] = 0x8B; s[o++] = 0x44; s[o++] = 0x24; s[o++] = 0x08;
            s[o++] = 0x48; s[o++] = 0x89; s[o++] = 0x05;
            { int32_t d = (int32_t)(argbuf + 48 - (va + o + 7));
              s[o++] = (uint8_t)d; s[o++] = (uint8_t)(d >> 8);
              s[o++] = (uint8_t)(d >> 16); s[o++] = (uint8_t)(d >> 24); }
            /* mov rax,[rsp+16] ; mov [rip+d],rax */
            s[o++] = 0x48; s[o++] = 0x8B; s[o++] = 0x44; s[o++] = 0x24; s[o++] = 0x10;
            s[o++] = 0x48; s[o++] = 0x89; s[o++] = 0x05;
            { int32_t d = (int32_t)(argbuf + 56 - (va + o + 7));
              s[o++] = (uint8_t)d; s[o++] = (uint8_t)(d >> 8);
              s[o++] = (uint8_t)(d >> 16); s[o++] = (uint8_t)(d >> 24); }
            uint32_t nr = 8u + i;
            s[o++] = 0xB8;
            s[o++] = (uint8_t)nr; s[o++] = (uint8_t)(nr >> 8);
            s[o++] = (uint8_t)(nr >> 16); s[o++] = (uint8_t)(nr >> 24);
            s[o++] = 0x0F; s[o++] = 0x05;
            s[o++] = 0xC3;
        }

        uint8_t *L = (uint8_t *)(uintptr_t)(pa_loop + 0xF60);
        loop_va += 0xF60;
        uint32_t o = 0;
        int32_t d1 = (int32_t)(buf_va - (loop_va + 7));
        int32_t d2 = (int32_t)(buf_va - (loop_va + 43));
        L[o++] = 0x48; L[o++] = 0x8D; L[o++] = 0x3D;
        L[o++] = (uint8_t)d1; L[o++] = (uint8_t)(d1 >> 8);
        L[o++] = (uint8_t)(d1 >> 16); L[o++] = (uint8_t)(d1 >> 24);
        L[o++] = 0xB8; L[o++] = 9; L[o++] = 0; L[o++] = 0; L[o++] = 0;
        L[o++] = 0x0F; L[o++] = 0x05;
        L[o++] = 0x48; L[o++] = 0x85; L[o++] = 0xC0;
        L[o++] = 0x78; L[o++] = 0x1A;
        L[o++] = 0x48; L[o++] = 0x8B; L[o++] = 0x47; L[o++] = 0x18;
        L[o++] = 0x48; L[o++] = 0x83; L[o++] = 0xF8; L[o++] = 0x01;
        L[o++] = 0x74; L[o++] = 0x10;
        L[o++] = 0x48; L[o++] = 0x8B; L[o++] = 0x77; L[o++] = 0x08;
        L[o++] = 0x48; L[o++] = 0x8B; L[o++] = 0x7F; L[o++] = 0x10;
        L[o++] = 0xFF; L[o++] = 0x15;
        L[o++] = (uint8_t)d2; L[o++] = (uint8_t)(d2 >> 8);
        L[o++] = (uint8_t)(d2 >> 16); L[o++] = (uint8_t)(d2 >> 24);
        L[o++] = 0xEB; L[o++] = 0xD3;
        L[o++] = 0xBF; L[o++] = 0; L[o++] = 0; L[o++] = 0; L[o++] = 0;
        L[o++] = 0xB8; L[o++] = 0; L[o++] = 0; L[o++] = 0; L[o++] = 0;
        L[o++] = 0x0F; L[o++] = 0x05;
        L[o++] = 0x0F; L[o++] = 0x0B;

        /* wolf_main ret adresi = olay dongusu (yiginin en ust sozcugu) */
        uint64_t pa_st = out->frames[code_pages + data_pages + stack_pages - 1];
        *(uint64_t *)(uintptr_t)(pa_st + WLF_PAGE_SIZE - 8) = loop_va;

        out->api_va  = api_va;
        out->loop_va = loop_va;
        out->buf_va  = buf_va;
    }

    out->base_va     = base_va;
    out->entry_va    = base_va + hdr.entry_off;
    out->stack_top   = stack_top;
    out->code_pages  = (uint32_t)code_pages;
    out->data_pages  = (uint32_t)data_pages;
    out->stack_pages = (uint32_t)stack_pages;
    out->image_size  = hdr.image_size;
    out->bss_size    = hdr.bss_size;
    out->entry_off   = hdr.entry_off;
    out->load_addr   = hdr.load_addr;
    out->nx          = nx ? 1u : 0u;
    out->status      = WLF_USER_OK;

    return out->entry_va;

fail:
    /* Once alan (eslenmis cerceveler + tablolar), sonra eslenmemis
     * cerceveler. mapped sayaci cift serbest birakmayi onler. */
    if (out->space) {
        vmm_space_destroy((vmm_space_t)out->space);
        out->space = 0;
    }
    wlf_free_frames(out, mapped);
    out->base_va = out->entry_va = out->stack_top = 0;
    return 0;
}

int wlf_user_free(wlf_user_t *out) {
    if (!out) return WLF_USER_ERR_ARG;
    if (out->magic == WLF_USER_FREED) return WLF_USER_ERR_STATE;
    if (out->magic != WLF_USER_MAGIC) return WLF_USER_ERR_STATE;

    if (out->space) {
        vmm_space_destroy((vmm_space_t)out->space);
        out->space = 0;
    }
    /* Basarili yuklemede cerceveler alanla birlikte serbest birakildi;
     * burada yalnizca kmalloc listesi geri verilir. */
    if (out->frames) {
        kfree(out->frames);
        out->frames = NULL;
    }
    out->frame_count = 0;

    memset(out, 0, sizeof(*out));
    out->magic  = WLF_USER_FREED;
    out->status = WLF_USER_ERR_STATE;
    return WLF_USER_OK;
}

/* ================= B1: ring 3 yurutme =================
 *
 * Sozlesme: wlf_user_run() cagiran gorev baglaminda calisir. Yuklenmis
 * alan aktif CR3 yapilir, enter_user() ile CPL3'e inilir. Uygulama
 * int 0x80 SYS_EXIT cagirirsa syscall.c'deki zayif kanca buraya gelir ve
 * kurtarma noktasina (longjmp) doner; CPU istisnasi olursa isr.c zaten
 * ayni kurtarma noktasina doner (main_kernel/ring3.c ile ayni mekanizma,
 * orada #PF testiyle kanitli).
 *
 * Kritik: longjmp sirasinda hala UYGULAMA CR3'undeyiz; once cekirdek
 * alanina donulur, sonra alan serbest birakilir. Cekirdek sayfalari
 * uygulama alaninda U=0'dir ama ring 0 erisebildigi icin setjmp donusu
 * guvenlidir. */
#include "recover.h"
#include "gdt.h"

extern void enter_user(uint64_t rip, uint64_t rsp, uint64_t rflags);
extern void enter_user_arg(uint64_t rip, uint64_t rsp, uint64_t rflags,
                           uint64_t arg);
extern arctian_recover_t *g_arctian_recover;

static wlf_user_t *volatile s_active;   /* calisan izole uygulama (tek) */
static uint64_t          s_kernel_space; /* uygulama CR3'unden donus icin */
static volatile int      s_exit_seen;
static volatile uint32_t s_exit_code;

/* int 0x80 SYS_EXIT → syscall.c zayif kancasi. Aktif izole gorev yoksa 0
 * doner (normal yol isler). Varsa beklemeden kurtarma noktasina atlar. */
int wlf_exit_hook(uint32_t code) {
    if (!s_active) return 0;
    if (!g_arctian_recover || !g_arctian_recover->active) return 0;
    s_active = 0;
    s_exit_code = code;
    s_exit_seen = 1;
    arctian_longjmp(g_arctian_recover, 1);   /* donmez */
}

int wlf_user_run(wlf_user_t *u) {
    if (!u || u->magic != WLF_USER_MAGIC || u->status != WLF_USER_OK)
        return WLF_USER_ERR_ARG;
    if (!u->space || !u->entry_va || !u->stack_top) return WLF_USER_ERR_STATE;
    if (s_active) return WLF_USER_ERR_STATE;        /* ic ice yurutme yok */
    if (g_arctian_recover) return WLF_USER_ERR_STATE;

    vmm_state_t *st = VMM_STATE;
    if (!st->ready || !st->kernel_space) return WLF_USER_ERR_STATE;

    s_kernel_space = vmm_current();

    static arctian_recover_t rec;   /* gorev basina tek: ic ice yok */
    rec.active = 0;
    rec.code = 0;
    rec.reason[0] = '\0';
    s_exit_seen = 0;
    g_arctian_recover = &rec;

    int rc = arctian_setjmp(&rec);
    if (rc == 0) {
        rec.active = 1;
        s_active = u;
        vmm_switch((vmm_space_t)u->space);   /* ayri adres alani */
        __asm__ volatile("cli");
        /* IF=0: CPL3'te kesme almiyoruz. Kesme/planlayici girisinin ring 3
         * baglaminda guvenli olmasi (TSS.rsp0, preemption) B2'nin isi;
         * kanitlanmis ring3.c yolu da ayni sekilde 0x2 kullaniyor.
         *
         * RDI = kullanici alanindaki wolf_api_t trampoline tablosu (B3).
         * Tablo yoksa (eski yukleyici) 0 gecer: libWolf uygulamalari
         * wolf_ready()=0 gorup durustce hata koduyla cikar. */
        enter_user_arg(u->entry_va, u->stack_top, 0x2, u->api_va);
        __asm__ volatile("ud2");             /* iretq donmez */
    }

    /* Geri donus: SYS_EXIT (s_exit_seen) veya CPU istisnasi. Hala
     * uygulama CR3'undeyiz; once cekirdege don. */
    __asm__ volatile("cli");
    {
        uint16_t kd = GDT_KERNEL_DATA;
        __asm__ volatile("movw %0, %%ds; movw %0, %%es; movw %0, %%ss"
                         :: "r"(kd) : "memory");
    }
    s_active = 0;
    rec.active = 0;
    g_arctian_recover = 0;
    vmm_switch((vmm_space_t)s_kernel_space);
    wlf_user_free(u);
    __asm__ volatile("sti");

    if (s_exit_seen) return (int)s_exit_code;
    /* Istisna: numara recover kaydinda metin olarak duruyor; ABI'de
     * ayri alan yok (bkz. wlf_abi.h). Seri logda sebep yazilir. */
    serial_printf("[wlf] izole gorev istisna ile oldu: %s\n",
                  rec.reason[0] ? rec.reason : "?");
    return (int)(WLF_EXIT_FAULT | 0u);
}

/* ============ B1 kanit: ring 3 izolasyon selftesti ============
 *
 * Disk gerektirmeyen, PIC bir kullanici imaji uretip yukleyici + ring-3
 * yurutucusunu bastan sona calistirir. Payload:
 *
 *   mov edi, 1            ; fd = stdout
 *   lea rsi, [rip+0x17]   ; "izole wlf ring3 ok\n"
 *   mov edx, 19           ; uzunluk
 *   mov eax, 1            ; SYS_WRITE
 *   int 0x80
 *   xor edi, edi          ; cikis kodu 0
 *   mov eax, 0            ; SYS_EXIT
 *   int 0x80
 *   jmp $                 ; (int 0x80 donmez; guvenlik agi)
 *   "izole wlf ring3 ok\n"
 *
 * Basarili kanit: seri logda satir WLF_WRITE'tan (ring 3) cikar, exit
 * kancasi doner, donus kodu 0 olur ve masaustu acilmaya devam eder.
 * Basarisizlikta donus negatiftir; cagiran loglar, sistem ayakta kalir. */
int wlf_user_selftest(void) {
    static const uint8_t payload[] = {
        0xBF, 0x01, 0x00, 0x00, 0x00,             /* mov edi,1            */
        0x48, 0x8D, 0x35, 0x17, 0x00, 0x00, 0x00, /* lea rsi,[rip+0x17]   */
        0xBA, 0x13, 0x00, 0x00, 0x00,             /* mov edx,19           */
        0xB8, 0x01, 0x00, 0x00, 0x00,             /* mov eax,1 (WRITE)    */
        0xCD, 0x80,                               /* int 0x80             */
        0x31, 0xFF,                               /* xor edi,edi          */
        0xB8, 0x00, 0x00, 0x00, 0x00,             /* mov eax,0 (EXIT)     */
        0xCD, 0x80,                               /* int 0x80             */
        0xEB, 0xFE,                               /* jmp $                */
        'i','z','o','l','e',' ','w','l','f',' ','r',
        'i','n','g','3',' ','o','k','\n'
    };
    const uint32_t plen = (uint32_t)sizeof(payload);
    const uint32_t total = WLF_HEADER_SIZE + (uint32_t)sizeof(payload);
    wlf_header_t *hdr;
    wlf_user_t u;
    uint8_t *img;
    int rc;

    img = (uint8_t *)kmalloc(total);
    if (!img) {
        serial_puts("[wlf] selftest: imaj icin bellek yok\n");
        return WLF_USER_ERR_NO_MEM;
    }
    memset(img, 0, total);
    hdr = (wlf_header_t *)img;
    hdr->magic       = WLF_MAGIC;
    hdr->version     = WLF_VERSION;
    hdr->kind        = WLF_KIND_NATIVE;
    hdr->image_size  = plen;
    hdr->bss_size    = 0;
    hdr->entry_off   = 0;
    hdr->payload_off = WLF_HEADER_SIZE;
    hdr->payload_len = plen;
    memcpy(img + WLF_HEADER_SIZE, payload, sizeof(payload));

    serial_puts("[wlf] izolasyon selftest: yukleniyor + ring 3...\n");
    if (!wlf_user_start(img, total, &u)) {
        rc = u.status;
        serial_printf("[wlf] selftest: yukleme basarisiz status=%d\n", rc);
        kfree(img);
        return rc;
    }
    kfree(img);   /* yukleyici kopyaladi; kaynak artik gerekmiyor */

    rc = wlf_user_run(&u);
    if (rc == 0)
        serial_puts("[wlf] izolasyon selftest GECTI: ring 3 yazdi, cikis 0, sistem ayakta\n");
    else
        serial_printf("[wlf] izolasyon selftest BASARISIZ: donus=%d\n", rc);
    return rc;
}
