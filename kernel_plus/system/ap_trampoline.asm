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
;   8F20 status     u32  ; AP basariyla gecince 'APOK' yazar
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
STATUS_APOK    equ 0x4B4F5041          ; 'APOK' kucuk sonlu

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

    mov eax, cr4
    or eax, 1 << 5                      ; CR4.PAE
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
    mov dword [rax], STATUS_APOK        ; BSP'ye "gecis tamam"
    mov rax, PARAM_CPUIDX
    mov edi, [rax]                      ; ap_entry(uint32_t cpu_index)
    mov rax, PARAM_ENTRY
    jmp qword [rax]                     ; C entry'ye gec

.hang:                                  ; ap_entry geri donmez; donerse dur
    cli
    hlt
    jmp .hang

; ---- gecici GDT'ler (duz, base 0) ----
gdt32:
    dq 0                                ; 0x00 bos
    dq 0x00CF9A00000FFFFF               ; 0x08 32-bit kod (D=1, L=0)
    dq 0x00CF9200000FFFFF               ; 0x10 veri
gdt32_end:

gdt64:
    dq 0                                ; 0x00 bos
    dq 0x00AF9A00000FFFFF               ; 0x08 64-bit kod (L=1, D=0)
    dq 0x00CF9200000FFFFF               ; 0x10 veri
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
