/* audio-edge — demo of the edge-driven 1-bit speaker abstraction.
 *
 * Simulates a Speccy-style beeper: a fictional emulator running at
 * 3.5 MHz toggles its speaker bit on a fixed cadence. The HAL converts
 * the cycle-stamped level changes into a square wave at the host
 * sample rate.
 *
 * No actual emulator here — we just walk a counter. Plays:
 *   1 s of 1000 Hz, 1 s of 440 Hz, 1 s of 220 Hz, 1 s of silence,
 * three rounds. Listen with -hda_test.
 *
 *   nix develop ../.. -c make
 *   rvvm firmware.bin -nogui -nonet -hda_test  */

#include "uart.h"
#include "time.h"
#include "fdt.h"
#include "pci.h"
#include "audio.h"
#include "audio_edge.h"
#include "rvvm.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define EMULATOR_HZ      3500000ULL    /* fictional Z80-like clock */
#define FRAME_HZ         50            /* fictional vblank rate */
#define CYCLES_PER_FRAME (EMULATOR_HZ / FRAME_HZ)
#define LEVEL_HIGH       0x4000
#define LEVEL_LOW       -0x4000

static audio_edge_t edge;

/* Run for `duration_seconds` of fictional emulator time, toggling the
 * beeper at `tone_hz`. tone_hz=0 means silence (no toggles, level held
 * at LEVEL_LOW). One frame at a time so the host gets a steady stream
 * of advances and we can pace via wfi like a real frame loop. */
static void run_tone(uint64_t *cycle, int duration_seconds, uint32_t tone_hz) {
    uint64_t cycles_per_half = tone_hz ? (EMULATOR_HZ / (2ULL * tone_hz)) : 0;
    uint64_t next_toggle     = *cycle + cycles_per_half;
    int16_t  level           = LEVEL_LOW;
    int      n_frames        = duration_seconds * FRAME_HZ;

    uint64_t deadline = time_now() + time_ticks_per_frame();
    for (int f = 0; f < n_frames; f++) {
        uint64_t frame_end = *cycle + CYCLES_PER_FRAME;

        /* Inject toggles inside this frame. */
        if (cycles_per_half) {
            while (next_toggle < frame_end) {
                level = (level == LEVEL_HIGH) ? LEVEL_LOW : LEVEL_HIGH;
                audio_edge_set(&edge, level, next_toggle);
                next_toggle += cycles_per_half;
            }
        }

        *cycle = frame_end;
        audio_edge_advance(&edge, *cycle);

        time_busy_until(deadline);
        deadline += time_ticks_per_frame();
    }
}

void kmain(uint64_t hartid, uint64_t fdt_addr) {
    uart_init(0);
    uart_puts("\n=== audio-edge: emulator-cycle-driven 1-bit speaker ===\n");
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
    if (!audio_edge_open(&edge, EMULATOR_HZ)) {
        uart_puts("audio_edge_open failed\n");
        for (;;) __asm__ volatile ("wfi");
    }

    uart_puts("\nplaying:\n");
    uint64_t cycle = 0;
    for (int round = 0; round < 3; round++) {
        uart_printf("[round %d]\n", (uint64_t)(round + 1));
        uart_puts("  1000 Hz (1 s)\n"); run_tone(&cycle, 1, 1000);
        uart_puts("   440 Hz (1 s)\n"); run_tone(&cycle, 1, 440);
        uart_puts("   220 Hz (1 s)\n"); run_tone(&cycle, 1, 220);
        uart_puts("   silence (1 s)\n"); run_tone(&cycle, 1, 0);
    }
    if (edge.dropped_events) {
        uart_printf("warning: dropped %u events to overflow\n",
                    (uint64_t)edge.dropped_events);
    }

    uart_puts("\ndone.\n");
    audio_edge_close(&edge);
    for (;;) __asm__ volatile ("wfi");
}
