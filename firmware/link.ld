/* Enaga image: loaded by the bootloader at the bottom of DDR, where the original firmware also loads (so
 * the bootloader is known not to use this range while loading). Enaga owns 0xC0000000-0xC0400000 (4 MiB); the
 * app's image and heap take the rest (ENAGA_APP_BASE, include/enaga/abi.h). The stack sits at the top of
 * Enaga's range. */
OUTPUT_FORMAT("elf32-tic6x-le")
OUTPUT_ARCH(tic6x)
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0xC0000000, LENGTH = 0x400000 }
SECTIONS {
    .text : { KEEP(*start.o(.text)) *(.text .text.*) } > RAM
    .vectors : { . = ALIGN(1024); KEEP(*(.vectors)) } > RAM
    .rodata : { *(.rodata .rodata.* .const .const.* .switch .switch.*) } > RAM
    .data : { *(.data .data.* .fardata .fardata.* .neardata .neardata.*) . = ALIGN(8); __image_end = .; } > RAM
    .bss (NOLOAD) : { . = ALIGN(8); __bss_start = .; *(.bss .bss.* .far .far.*) *(COMMON) . = ALIGN(8); __bss_end = .; } > RAM
    _stack_top = ORIGIN(RAM) + LENGTH(RAM);
    /DISCARD/ : { *(.c6xabi.attributes) *(.c6xabi.exidx*) *(.comment) *(.note*) *(.debug*) }
}
ASSERT(_start == ORIGIN(RAM), "start.s must be first")
ASSERT((vectors & 0x3FF) == 0, "the vector table must be 1 KiB aligned")
ASSERT(__bss_end + 0x40000 < _stack_top, "less than 256 KiB left for the stack")
