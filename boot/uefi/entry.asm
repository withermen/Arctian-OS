; Arctian UEFI onyukleyici giris noktasi (nasm -f win64).
;
; Firmware bizi MS ABI ile cagirir: RCX = ImageHandle, RDX = SystemTable.
; Kendi yiginimiza gecip efi_main'i cagiryoruz; donerse CPU'yu duraklatiyoruz
; (paket 1'de kernel yok, BootServices cagrilmayacak).

section .data align=64
efi_stack: times 65536 db 0   ; 64 KB kendi yiginimiz (win64 COFF'da .bss yok)

section .text
global efi_entry
extern efi_main

efi_entry:
    lea  rsp, [rel efi_stack + 65536]
    and  rsp, -16              ; 16 bayt hizala
    sub  rsp, 0x20             ; 32 bayt MS ABI golge alani
    call efi_main              ; RCX/RDX zaten yerinde
.halt:
    cli
    hlt
    jmp .halt                  ; donus olmamali; olsin diye bekler

; Paket 3: PIC guvenli 64-bit cekirdek atlama koprusu (0x9000'e kopyalanir).
; MS ABI: RCX = entry (0x100000), RDX = pml4 (0x11000).
global uefi_handoff_stub
global uefi_handoff_stub_end

align 16
uefi_handoff_stub:
    cli
    mov  rsp, 0x9FF0           ; 0..1 GB icinde temiz gecici yigin
    lea  rax, [rel .gdt64]
    sub  rsp, 16
    mov  word [rsp], 23
    mov  qword [rsp + 2], rax
    lgdt [rsp]

    mov  rax, cr4
    or   rax, (1 << 5) | (1 << 9) | (1 << 10)
    mov  cr4, rax

    mov  rax, cr0
    and  rax, ~(1 << 2)
    or   rax, (1 << 1)
    mov  cr0, rax

    mov  cr3, rdx

    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax

    push qword 0x08
    push rcx
    o64 retf

align 8
.gdt64:
    dq 0x0000000000000000
    dq 0x00AF9A000000FFFF
    dq 0x00AF92000000FFFF
uefi_handoff_stub_end:
