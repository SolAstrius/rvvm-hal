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

    /* Heap region between BSS and the stacks. Picolibc's sbrk
     * (vendor/picolibc/libc/misc/picosbrk.c) reads __heap_start and
     * __heap_end as extern symbols; consumers that don't link
     * picolibc never reference these so the address space costs
     * nothing in their firmware (it doesn't get into the binary).
     *
     * Size: 16 MiB. Comfortable headroom for any picolibc-using
     * consumer, well under RVVM's 256 MiB default RAM. Consumers
     * that need more can override at link time:
     *     -Wl,--defsym=__heap_size=0x4000000
     * — but we don't expose that knob via the script today; just
     * adjust the literal below if a consumer outgrows it.  */
    . = ALIGN(16);
    __heap_start = .;
    . += 16M;
    __heap_end = .;

    /* One 16 KiB stack per possibly-present hart. Per-hart sp is
     * computed in start.S as `__stack_top - mhartid * 16384`. Total
     * = SMP_MAX_HARTS (8) × 16 KiB = 128 KiB; a single-hart firmware
     * just leaves the high stacks unused. Bump SMP_STACK_PER_HART /
     * SMP_MAX_HARTS in include/smp.h together with the size below
     * if changed.  */
    . = ALIGN(16);
    __stack_bottom = .;
    . += 8 * 16K;
    __stack_top = .;

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame*) *(.riscv.attributes) }
}
