/* Arctian main_kernel linker script */
ENTRY(kernel_main_asm)

SECTIONS
{
    . = 0x00100000;

    .text : ALIGN(4K) { *(.text.entry) *(.text) *(.text.*) }
    .rodata : ALIGN(4K) { *(.rodata) *(.rodata.*) }
    .data : ALIGN(4K) { *(.data) *(.data.*) }
    .bss : ALIGN(4K)
    {
        bss_start = .;
        *(COMMON)
        *(.bss)
        *(.bss.*)
        bss_end = .;
    }

    /* INSTALL_BASE (memory_map.h: 0x00200000). main_kernel + bss buraya
     * tasarsa install modulu yuklenirken main_kernel'in yigini/verisi ezilir
     * ve boot #GP ile olur. Bunu boot'ta degil link aninda yakala. */
    ASSERT(bss_end <= 0x00200000,
           "main_kernel: veri+yigin INSTALL_BASE (0x200000) bolgesine tasiyor")

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame) }
}
