/* audio-pcm — drive RVVM's HDA stream worker with a sequence of
 * tones. Demonstrates the full hda_pcm_* surface: init at 48 kHz mono,
 * fill a cyclic ring, watch SDnLPIB advance.
 *
 * The ring is 4800 frames (100 ms). It contains one 100 ms loop of
 * audio that RVVM's stream worker plays forever once we set RUN. We
 * cycle through three patterns at 4-second intervals:
 *
 *   1. 480 Hz square wave   — buzzy mid pitch
 *   2. 240 Hz square wave   — octave down, deeper buzz
 *   3. silence              — confirm the host stops generating noise
 *
 * Each pattern is loaded by overwriting the entire ring while the
 * stream is still RUNning. There's a brief moment where the host may
 * read half-old, half-new bytes — that shows up as a click. The point
 * is to prove playback works, not to be discontinuity-free.
 *
 * To listen: build the parent rvvm-hal, build this firmware, then run
 *   rvvm firmware.bin -nogui -nonet -hda_test
 * via a host with libasound available (the rvvm-hal nix devshell
 * exports ALSA_PLUGIN_DIR + LD_LIBRARY_PATH for that).
 *
 * RVVM's HDA emulator advertises 16-bit PCM at 44.1/48/88.2/96 kHz on
 * a single mono converter (NID 2). We stick to 48 kHz mono — fewest
 * surprises. */

#include "uart.h"
#include "time.h"
#include "fdt.h"
#include "pci.h"
#include "hda.h"
#include "rvvm.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* 100 ms at 48 kHz mono. 480 Hz fits in 100 cycles per ring; 240 Hz in
 * 50 cycles — both whole numbers, so the ring loops without a click
 * at the boundary (other than the format change moments). */
#define RING_FRAMES   4800
#define SAMPLE_RATE   48000

__attribute__((aligned(128)))
static int16_t ring[RING_FRAMES];

/* Fill `ring` with a square wave at the given Hz. Half-period in
 * frames = SAMPLE_RATE / (2 * hz). For 480 Hz that's 50; for 240 Hz,
 * 100. Amplitude is ±0x4000 (half-scale) — loud enough to hear, not
 * deafening. */
static void fill_square(uint32_t hz) {
    uint32_t half_period = SAMPLE_RATE / (2 * hz);
    int16_t  level       = 0x4000;
    uint32_t toggle_at   = half_period;
    for (uint32_t i = 0; i < RING_FRAMES; i++) {
        ring[i] = level;
        if (i + 1 == toggle_at) {
            level     = -level;
            toggle_at += half_period;
        }
    }
}

static void fill_silence(void) {
    for (uint32_t i = 0; i < RING_FRAMES; i++) ring[i] = 0;
}

static void wait_seconds(uint32_t s) {
    uint64_t deadline = time_now() + (uint64_t)s * RVVM_TIME_HZ;
    time_busy_until(deadline);
}

void kmain(uint64_t hartid, uint64_t fdt_addr) {
    uart_init(0);
    uart_puts("\n=== audio-pcm: HDA PCM streaming demo ===\n");
    uart_printf("hart=%u  fdt=%p\n", hartid, (void *)(uintptr_t)fdt_addr);

    /* FDT walk for fallback addresses. */
    fdt_t fdt;
    if (fdt_init(&fdt, (const void *)(uintptr_t)fdt_addr)) {
        uint32_t u_off = fdt_find_compatible(&fdt, "ns16550a");
        uint32_t p_off = fdt_find_compatible(&fdt, "pci-host-ecam-generic");
        uint64_t u_at = 0, p_at = 0;
        fdt_node_reg64(&fdt, u_off, 0, &u_at, NULL);
        fdt_node_reg64(&fdt, p_off, 0, &p_at, NULL);
        uart_init((uintptr_t)u_at);
        pci_init((uintptr_t)p_at);
    }

    if (!hda_init()) {
        uart_puts("hda_init failed — start RVVM with -hda_test\n");
        for (;;) __asm__ volatile ("wfi");
    }

    /* Pre-fill ring with the first tone before starting playback so
     * the first audible frame is the desired one (not whatever was in
     * BSS). */
    fill_square(480);

    hda_pcm_t pcm;
    if (!hda_pcm_init(&pcm, ring, RING_FRAMES, /*bdl_entries*/4, SAMPLE_RATE)) {
        uart_puts("hda_pcm_init failed\n");
        for (;;) __asm__ volatile ("wfi");
    }
    /* Tell the host the entire ring is filled and ready to play. */
    hda_pcm_advance(&pcm, RING_FRAMES);

    uart_puts("\nplaying:\n");
    /* Heads-up: we overwrite the ring in place between tones without
     * syncing to LPIB, so each transition has a brief "click" as the
     * host reads half-old, half-new bytes. That click is percussive
     * enough to sound like a ta-daaaaah onset before the new sustained
     * pitch settles. Double-buffered demos don't have this; ringtones
     * with cross-fades don't have this. The point here is to prove the
     * PCM path works, not to be discontinuity-free. */
    for (int round = 0; round < 3; round++) {
        uart_puts("  480 Hz square (4 s)\n");
        fill_square(480);
        wait_seconds(4);

        uart_puts("  240 Hz square (4 s)\n");
        fill_square(240);
        for (int s = 0; s < 4; s++) wait_seconds(1);

        uart_puts("  silence (2 s)\n");
        fill_silence();
        wait_seconds(2);
    }

    uart_puts("\nstopping stream.\n");
    hda_pcm_stop(&pcm);

    uart_puts("done. spinning forever.\n");
    for (;;) __asm__ volatile ("wfi");
}
