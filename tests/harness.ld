/* E0 test harness image, below the app window (ENAGA_APP_BASE 0xC0400000). */
OUTPUT_FORMAT("elf32-tic6x-le")
OUTPUT_ARCH(tic6x)
ENTRY(harness_main)
MEMORY { HOST (rwx) : ORIGIN = 0xC0000000, LENGTH = 0x200000 }
SECTIONS {
    .text : { *(.text .text.*) } > HOST
    .rodata : { *(.rodata .rodata.* .const .const.* .switch .switch.*) } > HOST
    .data : { *(.data .data.* .fardata .fardata.* .neardata .neardata.*) } > HOST
    .bss (NOLOAD) : { . = ALIGN(8); *(.bss .bss.* .far .far.*) *(COMMON) } > HOST
    /DISCARD/ : { *(.c6xabi.attributes) *(.c6xabi.exidx*) *(.comment) *(.note*) *(.debug*) }
}
