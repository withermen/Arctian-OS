; AP trampoline - 16-bit gercek moddan 64-bit uzun moda cikis.
; nasm -f bin ile derlenir; BSP bu blob'u fiziksel 0x8000'e kopyalar ve
; SIPI vektoru 0x08 ile (CS=0x0800, IP=0) AP'yi buraya baslatir.
;
; Parametre bloku 0x8F00 (apboot.h ile ayni yapi):
;   8F00 cr3        u64
;   8F08 entry      u64
;   8F10 stack_top  u64
;   8F18 cpu_index  u32
;   8F1C apic_id    u32
;   8F20 status     u32  ; trampoline 'AP64' yazar; 'APOK'u ap_entry yazar
;
; Iki GDT: PE acikken L=1/D=0 tanimla 32-bit asama adimlanamaz, bu yuzden
; once 32-bit GDT (0x08 kod D=1), sonra 64-bit GDT (0x08 kod L=1) yuklenir.
; Her iki asamada da 0x08 = kod, 0x10 = veri.
;
; Hata yolu yok: bir asamada takilirsa status 0 kalir, BSP zaman asimina
; duser; trampoline hlt dongusunde bekler (uydurma durum yazilmaz).

BITS 16
ORG 0x8000

PARAM_CR3      equ 0x8F00
PARAM_ENTRY    equ 0x8F08
PARAM_STACK    equ 0x8F10
PARAM_CPUIDX   equ 0x8F18
PARAM_STATUS   equ 0x8F20
STATUS_AP64    equ 0x34365041          ; 'AP64' kucuk sonlu (gecis tamam)
ap_start:
    cli
    cld
    lgdt [gdt32_ptr]
    mov eax, cr0
    or eax, 1                           ; CR0.PE
    mov cr0, eax
    jmp dword 0x08:ap32                 ; 32-bit korumali mod

BITS 32
ap32:
    mov ax, 0x10                        ; veri secici
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; SSE/SSE2: GCC -O2 cekirdek kodunda movdqa/movups kullaniyor; bu bitler
    ; olmadan AP ilk SSE komutunda #UD alir (QEMU -d int ile goruldu).
    mov eax, cr0
    and eax, 0xFFFFFFF3                   ; EM(bit2) ve TS(bit3) temizle
    or eax, 1 << 1                        ; MP(bit1) kur; CD/NW'ye dokunma
    mov cr0, eax

    mov eax, cr4
    or eax, (1 << 5) | (1 << 9) | (1 << 10)   ; PAE | OSFXSR | OSXMMEXCPT
    mov cr4, eax

    mov ecx, 0xC0000080                 ; EFER
    rdmsr
    or eax, 1 << 8                      ; EFER.LME
    wrmsr

    mov eax, [PARAM_CR3]                ; cekirdek sayfa tablolari (alt 32 bit)
    mov cr3, eax

    lgdt [gdt64_ptr]                    ; kod tanimini L=1/D=0 yap

    mov eax, cr0
    or eax, 1 << 31                     ; CR0.PG
    mov cr0, eax

    jmp 0x08:ap64                       ; 64-bit uzun mod

BITS 64
ap64:
    mov ax, 0x10                        ; gizli tanim onbelleklerini tazele
    mov ds, ax
    mov es, ax
    mov ss, ax

    mov rax, PARAM_STACK
    mov rsp, [rax]                      ; AP'ye ayrilmis yigin
    mov rbp, 0

    mov rax, PARAM_STATUS
    mov dword [rax], STATUS_AP64        ; yalniz mod gecisi; APOK'u C yazar
    mov rax, PARAM_CPUIDX
    mov edi, [rax]                      ; ap_entry(uint32_t cpu_index)
    mov rax, PARAM_ENTRY
    jmp qword [rax]                     ; C entry'ye gec

.hang:                                  ; ap_entry geri donmez; donerse dur
    cli
    hlt
    jmp .hang

; ---- gecici GDT'ler (duz, base = 0) ----
; Kucuk sonlu dizilim: limit[15:0] base[23:0] erisim limit[19:16]+bayraklar base[31:24].
; Limitin ust 4 biti 6. bayragin alt yarisi oldugu icin base baytlari sifir
; kalmali (0x00CF9A00000FFFFF yanlis; base 0x000F0000 yapardi).
gdt32:
    dq 0                                ; 0x00 bos
    dq 0x00CF9A000000FFFF               ; 0x08 32-bit kod (D=1, L=0)
    dq 0x00CF92000000FFFF               ; 0x10 veri
gdt32_end:

gdt64:
    dq 0                                ; 0x00 bos
    dq 0x00AF9A000000FFFF               ; 0x08 64-bit kod (L=1, D=0)
    dq 0x00CF92000000FFFF               ; 0x10 veri
gdt64_end:

gdt32_ptr:
    dw gdt32_end - gdt32 - 1
    dd gdt32

gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dd gdt64

; Blob 0x8F00'daki parametre blokindan once bitmeli.
%if ($ - $$) > 0xF00
    %error "ap_trampoline 0xF00 bayti asiyor"
%endif
