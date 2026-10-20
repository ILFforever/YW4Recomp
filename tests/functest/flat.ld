/* Link the function tests into one flat image starting at offset 0, like main:
 * code first, then read-only data. Everything is PC-relative (no relocations), so the
 * image runs at any 4 KiB-aligned address. */
SECTIONS {
    . = 0;
    .text : { *(.text .text.*) }
    __text_end = .;
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.data*) *(.bss*) }
}
