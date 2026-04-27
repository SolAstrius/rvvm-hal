/* Edge-driven 1-bit speaker abstraction.
 *
 * For the canonical retro-emulator audio shape: "the speaker is at
 * level X starting at emulator cycle T." Caller submits level changes
 * in its own clock; HAL converts to host PCM samples and streams via
 * audio_pcm internally. A single ring is allocated when the channel
 * opens — opaque to the caller.
 *
 * One channel at a time in this v1.
 *
 * Typical usage (Speccy beeper, port $FE bit 4):
 *
 *     static audio_edge_t spk;
 *     audio_init();
 *     audio_edge_open(&spk, 3500000);   // 3.5 MHz Z80
 *
 *     // inside speccy_out, when port FE bit 4 changes:
 *     audio_edge_set(&spk, bit4 ? +0x4000 : -0x4000, t_states_so_far);
 *
 *     // once per emulator frame, after the frame ran:
 *     audio_edge_advance(&spk, t_states_at_frame_end);
 *
 * Latency is ~ring-size frames of audio (default 100 ms) plus the
 * ALSA buffer at the host. */

#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct audio_edge_s audio_edge_t;

/* Internal layout — fields are not part of the API but exposed so
 * the struct can be allocated by the caller (no malloc in HAL). */
struct audio_edge_s {
    uint64_t  emulator_hz;          /* caller's clock rate */
    uint64_t  cycle_origin;         /* emulator cycle that maps to sample 0 */
    uint64_t  total_samples;        /* host samples emitted since open */
    int16_t   current_level;        /* level as of the most recent set */

    /* Pending level-change events for the in-flight frame. Drained on
     * each audio_edge_advance call. Capacity is plenty for a Speccy
     * frame's worth of beeper toggles (a tight square at 4 kHz over
     * 20 ms = 160 toggles). */
    struct {
        uint64_t cycle;
        int16_t  level;
    } events[256];
    uint32_t   n_events;
    uint32_t   dropped_events;      /* diagnostics: events lost to overflow */
    bool       open;
};

/* Open a channel running at the given emulator clock rate (e.g.
 * 3500000 for the Speccy, 1023000 for the Apple II 6502, 4194304 for
 * the Game Boy). audio_init() must have succeeded. Returns false if
 * a channel is already open or the backend isn't up. */
bool audio_edge_open(audio_edge_t *e, uint64_t emulator_hz);

/* Submit a level change. `level` is signed 16-bit; for a 1-bit speaker
 * use ±0x4000 (half-scale leaves headroom for any future mixing).
 * `at_cycle` is the emulator's cycle counter at the moment the level
 * changed — must be monotonic within a frame. Submissions outside
 * [last_advance_cycle, current_advance_cycle] are clamped at advance
 * time. */
void audio_edge_set(audio_edge_t *e, int16_t level, uint64_t at_cycle);

/* Render samples from the previous advance up to `now_cycle` and push
 * them to the backend. Call once per emulator frame after the frame's
 * step has run, with the cycle counter at frame end.
 *
 * If the host hasn't drained the ring fast enough (writable < required),
 * the oldest samples are dropped silently — this prevents a stuck
 * audio sink from stalling the emulator. */
void audio_edge_advance(audio_edge_t *e, uint64_t now_cycle);

/* Close the channel. Stops the underlying PCM stream. */
void audio_edge_close(audio_edge_t *e);
