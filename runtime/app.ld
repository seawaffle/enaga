/* Enaga app image: flat, linked at ENAGA_APP_BASE (include/enaga/abi.h). Enaga copies the payload
 * (.text through .data) here, zeroes .bss and calls enaga_entry (runtime.cpp). The stack is Enaga's. */
OUTPUT_FORMAT("elf32-tic6x-le")
OUTPUT_ARCH(tic6x)
ENTRY(enaga_entry)
MEMORY { APP (rwx) : ORIGIN = 0xC0400000, LENGTH = 0x7C00000 }   /* ENAGA_APP_BASE to the end of DDR */
SECTIONS {
    .text : { *(.text .text.*) } > APP
    /* GCC tic6x names read-only data .const and jump tables .switch (TI conventions) */
    .rodata : { *(.rodata .rodata.* .const .const.* .switch .switch.*) } > APP
    .init_array : {
        . = ALIGN(4);
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
        /* This GCC (built without a libc, so without --enable-initfini-array) emits .ctors. Run end to start,
         * as crtstuff does; .ctors.NNNNN sorts so that reverse order runs higher priority numbers last. */
        __ctors_start = .;
        KEEP(*(SORT_BY_NAME(.ctors.*)))
        KEEP(*(.ctors))
        __ctors_end = .;
    } > APP
    .data : { *(.data .data.* .fardata .fardata.* .neardata .neardata.*) . = ALIGN(8); } > APP
    .bss (NOLOAD) : { . = ALIGN(8); __bss_start = .; *(.bss .bss.* .far .far.*) *(COMMON) . = ALIGN(8); __bss_end = .; } > APP
    /* Destructors are never run (an unloaded app's memory is discarded), and nothing unwinds. */
    /DISCARD/ : { *(.fini_array .fini_array.*) *(.dtors .dtors.*) *(.c6xabi.attributes) *(.c6xabi.exidx*) *(.c6xabi.extab*)
                  *(.comment) *(.note*) *(.debug*) }
}
