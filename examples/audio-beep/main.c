/* audio-beep — simplest possible audio demo.
 *
 * Uses ONLY audio_init() + audio_beep(freq_hz). No PCM streaming, no
 * edge tracker. The codec's beep generator widget produces a fixed-
 * pitch square; we just change the pitch over time.
 *
 * Plays a C major scale: C4 D4 E4 F4 G4 A4 B4 C5, 250 ms per note.
 * Then silence for 1 s and exits.
 *
 *   nix develop ../.. -c make
 *   make run                    # plays the scale
 *
 * Footprint: this firmware doesn't link audio_pcm.o or audio_edge.o
 * (verified: only audio.o + hda.o are pulled). It's the canonical
 * shape for "I just want a beep" — CHIP-8 sound timer, terminal bell,
 * status alert, etc. */

#include "uart.h"
#include "time.h"
#include "fdt.h"
#include "pci.h"
#include "audio.h"
#include "rvvm.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Equal-tempered scale starting at middle C (~261.63 Hz). The beep
 * widget's nearest divider rounding will quantise these slightly —
 * RVVM's resolution is 48000/(4*N) so adjacent notes near 440 Hz are
 * ~17 Hz apart. Audible enough to hear "a scale" but it won't be
 * perfectly in tune. That's the cost of using the simple API. */
static const struct {
    uint32_t hz;
    const char *name;
} scale[] = {
    { 262, "C4" },
    { 294, "D4" },
    { 330, "E4" },
    { 349, "F4" },
    { 392, "G4" },
    { 440, "A4" },
    { 494, "B4" },
    { 523, "C5" },
};

static void sleep_ms(uint32_t ms) {
    /* time_busy_until parks on wfi instead of spinning — host CPU
     * stays near 0 %. RVVM_TIME_HZ is 10 MHz, so 10 kHz per ms. */
    uint64_t deadline = time_now() + (uint64_t)ms * (RVVM_TIME_HZ / 1000);
    time_busy_until(deadline);
}

void kmain(uint64_t hartid, uint64_t fdt_addr) {
    uart_init(0);
    uart_puts("\n=== audio-beep: codec beep widget demo ===\n");
    uart_printf("hart=%u  fdt=%p\n", hartid, (void *)(uintptr_t)fdt_addr);

    fdt_t fdt;
    if (fdt_init(&fdt, (const void *)(uintptr_t)fdt_addr)) {
        uint32_t u_off = fdt_find_compatible(&fdt, "ns16550a");
        uint32_t p_off = fdt_find_compatible(&fdt, "pci-host-ecam-generic");
        uint32_t c_off = fdt_find_compatible(&fdt, "sifive,clint0");
        uint64_t u_at = 0, p_at = 0, c_at = 0;
        fdt_node_reg64(&fdt, u_off, 0, &u_at, NULL);
        fdt_node_reg64(&fdt, p_off, 0, &p_at, NULL);
        fdt_node_reg64(&fdt, c_off, 0, &c_at, NULL);
        uart_init((uintptr_t)u_at);
        pci_init((uintptr_t)p_at);
        uint32_t cpus = fdt_find_node_named(&fdt, "cpus");
        uint32_t hz = 0;
        if (cpus != UINT32_MAX) fdt_node_prop_u32(&fdt, cpus, "timebase-frequency", &hz);
        time_init((uintptr_t)c_at, hz);
    }

    if (!audio_init()) {
        uart_puts("audio_init failed — start RVVM with -hda_test\n");
        for (;;) __asm__ volatile ("wfi");
    }

    uart_puts("\nplaying C major scale (250 ms/note):\n");
    for (uint32_t i = 0; i < sizeof(scale) / sizeof(scale[0]); i++) {
        uart_printf("  %s (%u Hz)\n", scale[i].name, (uint64_t)scale[i].hz);
        audio_beep(scale[i].hz);
        sleep_ms(250);
    }
    audio_beep(0);
    uart_puts("  silence\n");
    sleep_ms(1000);

    uart_puts("\ndone.\n");
    for (;;) __asm__ volatile ("wfi");
}
