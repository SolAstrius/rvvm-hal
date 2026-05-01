/* link_s.ld — link script for S-mode firmware that runs as the
 * payload of an OpenSBI fw_jump / fw_dynamic boot.
 *
 * RVVM with `<firmware> -k <kernel>` loads the kernel at RAM_BASE +
 * 0x200000 (= 0x80200000 for the default RV64 RAM base 0x80000000;
 * src/rvvm.c:296). OpenSBI's `generic` build defaults to jumping at
 * 0x80200000 too, so the addresses line up: OpenSBI runs at 0x80000000
 * in M-mode, drops to S-mode, and jumps to our payload at 0x80200000
 * with a0=hartid, a1=fdt_phys.
 *
 * Layout otherwise mirrors link.ld (heap, per-hart stacks). LENGTH
 * leaves the first 2 MiB to OpenSBI; the remainder of RAM is ours.
 */

OUTPUT_ARCH(riscv)
OUTPUT_FORMAT("elf64-littleriscv")
ENTRY(_start)

MEMORY {
    RAM (rwx) : ORIGIN = 0x80200000, LENGTH = 254M
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
    __heap_start = .;
    . += 16M;
    __heap_end = .;

    . = ALIGN(16);
    __stack_bottom = .;
    . += 8 * 16K;
    __stack_top = .;

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame*) *(.riscv.attributes) }
}
