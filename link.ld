/* Bare-metal RISC-V link script for RVVM mtd-physmap firmware.
 * RVVM defaults: RAM base = reset PC = 0x80000000.
 * mtd-physmap copies the flash image into RAM at reset_pc on boot.
 *
 * LENGTH matches RVVM's -mem default (256M). This is just the address
 * space we may use — actual memory use is whatever the consumer's BSS
 * + stack add up to. Smaller consumers (chip-8 ~50 KB, zx-spectrum
 * ~50 KB) ignore the headroom; bigger ones (game-boy with up to 8 MB
 * static ROM buffer) use it. If you point RVVM at a smaller -mem,
 * make sure your firmware's footprint still fits.
 */

OUTPUT_ARCH(riscv)
OUTPUT_FORMAT("elf64-littleriscv")
ENTRY(_start)

MEMORY {
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 256M
}

SECTIONS {
    . = ORIGIN(RAM);

    .text : {
        KEEP(*(.text.boot))
        *(.text .text.*)
    } > RAM

    .rodata : ALIGN(8) {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    } > RAM

    .data : ALIGN(8) {
        *(.data .data.*)
        *(.sdata .sdata.*)
    } > RAM

    .bss : ALIGN(8) {
        __bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        __bss_end = .;
    } > RAM

    . = ALIGN(16);
    __stack_bottom = .;
    . += 64K;
    __stack_top = .;

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame*) *(.riscv.attributes) }
}
