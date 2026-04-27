/* audio-pcm — drive RVVM's HDA stream worker with a sequence of
 * tones using a continuous-feed pattern.
 *
 * The ring is split conceptually into a "playing" region (between
 * SDnLPIB and our wp) and a "free" region (between wp and SDnLPIB
 * going forward — the slot after wp wraps back around to LPIB). We
 * only ever write into the free region, then advance wp. Bytes we've
 * written are *not* re-touched — the host's next read of that ring
 * slot will see exactly what we wrote and only that. This is how a
 * real emulator core (Speccy beeper, AY, NES APU, …) drives audio:
 * generate samples just-in-time, hand them off, never look back.
 *
 * Compared to the previous in-place-overwrite version: no clicks at
 * format transitions, because the host never reads a half-old half-new
 * slot. The cost is a small lag — by the time we switch generators,
 * the ~ring-size of bytes already-in-flight will keep playing the old
 * tone for one more ring period (~100 ms). Inaudible at this scale.
 *
 * Layout: 4800-frame mono ring at 48 kHz = 100 ms total. Three rounds
 * of {480 Hz square / 240 Hz square / silence} at {4 s / 4 s / 2 s}.
 *
 * To listen: build the parent rvvm-hal, build this firmware, then run
 *   rvvm firmware.bin -nogui -nonet -hda_test
 * via a host with libasound available — the rvvm-hal nix devshell
 * exports ALSA_PLUGIN_DIR + LD_LIBRARY_PATH for that. */

#include "uart.h"
#include "time.h"
#include "fdt.h"
#include "pci.h"
#include "audio.h"
#include "audio_pcm.h"
#include "rvvm.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* RVVM's ALSA backend (src/devices/alsa.c) opens the host PCM with
 * period_size = 960 frames and buffer_size = 3840 frames (80 ms
 * total, 20 ms periods). Each subsystem.write call from the HDA
 * stream worker turns into a snd_pcm_writei of `len` bytes. If `len`
 * isn't an integer multiple of the ALSA period size, every writei
 * lands mid-period and PipeWire's per-period scheduling produces
 * audible glitches at the boundaries.
 *
 * Solution: align BDL entry size with ALSA's period. 5 entries × 960
 * frames each = 4800-frame ring, every writei is exactly one period.
 *
 * With a single BDL entry the firmware can never observe LPIB
 * advancing mid-entry (RVVM only bumps stream->lpib after each
 * subsystem.write returns), so multi-BDL is required for the feed
 * loop to see when the host has consumed something. */
#define RING_FRAMES   4800
#define BDL_ENTRIES   5
#define ALSA_PERIOD   960     /* matches sound-hda.c alsa_sound_init */
#define SAMPLE_RATE   48000

/* PipeWire's first writei into a freshly-opened stream produces a
 * brief startup transient — empirically audible as a very short "ta"
 * click on the leading edge of the first tone. We tried silence-prime
 * and fade-in to mask it; both were worse:
 *   - silence-prime alone: "trrrr" — the abrupt silence→full-tone
 *     transition at the prime boundary sounds buzzier than the bare
 *     pipewire-startup transient.
 *   - 50 ms fade-in: even worse — a ramping square wave at rising
 *     amplitudes is itself a "trrrr" sound for the duration of the
 *     fade.
 * The cleanest result was just full-tone pre-fill aligned with the
 * ALSA period boundary. The residual click is short enough to live
 * with for a demo. */

__attribute__((aligned(128)))
static int16_t ring[RING_FRAMES];

/* A generator produces one frame given a monotonically-increasing
 * phase counter. Phase resets when we switch generators so the new
 * tone starts cleanly at +amplitude. */
typedef int16_t (*gen_fn)(uint32_t phase);

static int16_t gen_480(uint32_t p) {
    /* 480 Hz square at 48 kHz: half-period = 50 frames. */
    return (p / 50) & 1 ? -0x4000 : 0x4000;
}

static int16_t gen_240(uint32_t p) {
    /* 240 Hz square: half-period = 100 frames. */
    return (p / 100) & 1 ? -0x4000 : 0x4000;
}

static int16_t gen_silence(uint32_t p) {
    (void)p;
    return 0;
}

/* Feed the ring continuously until `deadline` (mtime tick value).
 * Each pass: query writable, fill that region, advance wp. When the
 * ring is full (writable == 0), busy-wait briefly so the host worker
 * can drain a chunk. */
static void feed_until(audio_pcm_t *p, uint64_t deadline,
                       gen_fn gen, uint32_t *phase) {
    while (time_now() < deadline) {
        uint32_t free = audio_pcm_writable(p);
        if (free == 0) {
            /* Ring full — sleep about 1 ms (equivalent to ~48 frames
             * at 48 kHz). Worker will have made progress by then. */
            uint64_t spin_until = time_now() + RVVM_TIME_HZ / 1000;
            time_busy_until(spin_until);
            continue;
        }
        /* Cap per-pass writes so we revisit the wp-advance bookkeeping
         * frequently — keeps wp from getting too far ahead of LPIB. */
        if (free > 512) free = 512;
        for (uint32_t i = 0; i < free; i++) {
            uint32_t idx = (p->wp_frames + i) % p->ring_frames;
            ring[idx] = gen((*phase)++);
        }
        audio_pcm_advance(p, free);
    }
}

void kmain(uint64_t hartid, uint64_t fdt_addr) {
    uart_init(0);
    uart_puts("\n=== audio-pcm: HDA PCM streaming demo ===\n");
    uart_printf("hart=%u  fdt=%p\n", hartid, (void *)(uintptr_t)fdt_addr);

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

    if (!audio_init()) {
        uart_puts("audio_init failed — start RVVM with -hda_test\n");
        for (;;) __asm__ volatile ("wfi");
    }

    /* Pre-fill the entire ring with the first tone at full amplitude. */
    {
        uint32_t phase = 0;
        for (uint32_t i = 0; i < RING_FRAMES; i++) ring[i] = gen_480(phase++);
    }

    audio_pcm_t pcm;
    if (!audio_pcm_open(&pcm, ring, RING_FRAMES, BDL_ENTRIES, SAMPLE_RATE)) {
        uart_puts("audio_pcm_open failed\n");
        for (;;) __asm__ volatile ("wfi");
    }
    /* Tell the HAL the ring is already full — wp lands at N-1, leaving
     * one sentinel slot. writable() returns 0 until LPIB advances. */
    audio_pcm_advance(&pcm, RING_FRAMES - 1);

    uart_puts("\nplaying (continuous-feed pattern, no in-place overwrite):\n");
    for (int round = 0; round < 3; round++) {
        uart_printf("[round %d]\n", (uint64_t)(round + 1));

        uart_puts("  480 Hz square (4 s)\n");
        /* On round 1, the pre-fill already laid down phases 0..N-1.
         * The first feed write lands at ring[N-1] — matching pre-fill
         * means feed phase must continue from where pre-fill stopped,
         * which is N-1 itself. Set phase = pcm.wp_frames (== N-1 right
         * after init+advance, then == wherever wp ended up after the
         * previous segment). On subsequent rounds we restart phase
         * from 0 below for tone changes — square waves don't need
         * cross-tone phase continuity, just a clean per-tone attack. */
        uint32_t phase = (round == 0) ? pcm.wp_frames : 0;
        feed_until(&pcm, time_now() + 4 * RVVM_TIME_HZ, gen_480, &phase);

        uart_puts("  240 Hz square (4 s)\n");
        phase = 0;
        feed_until(&pcm, time_now() + 4 * RVVM_TIME_HZ, gen_240, &phase);

        uart_puts("  silence (2 s)\n");
        phase = 0;
        feed_until(&pcm, time_now() + 2 * RVVM_TIME_HZ, gen_silence, &phase);
    }

    uart_puts("\nstopping stream.\n");
    audio_pcm_close(&pcm);

    uart_puts("done. spinning forever.\n");
    for (;;) __asm__ volatile ("wfi");
}
