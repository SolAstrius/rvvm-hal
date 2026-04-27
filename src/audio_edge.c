/* Edge-driven 1-bit audio. Caller submits level changes timestamped
 * in its own emulator clock; we convert to host-rate samples and feed
 * a private audio_pcm channel.
 *
 * Algorithm (zero-order-hold resampler):
 *
 *   Each emulator frame the consumer calls audio_edge_advance(now).
 *   We compute how many host samples cover [last_advance, now), then
 *   render those samples by walking the queued events in cycle order:
 *
 *     prev_idx = 0
 *     prev_lvl = current_level (level as of last frame)
 *     for each event (cycle, level) in queue:
 *         idx = sample_index_for(cycle)
 *         fill ring[prev_idx .. idx) with prev_lvl
 *         prev_lvl = level
 *         prev_idx = idx
 *     fill ring[prev_idx .. N) with prev_lvl
 *
 * No anti-aliasing — a square at 4 kHz aliases against 48 kHz at
 * audible frequencies. Acceptable for v1 (Speccy beeper has the same
 * aliasing on real hardware speakers); a band-limited variant can be
 * dropped in behind HAL_AUDIO_EDGE_BLEP later.
 *
 * Ring sizing: 4800 frames @ 48 kHz = 100 ms, 5 BDL entries × 960 each
 * to align with RVVM's ALSA period (matches audio-pcm example). */

#include "audio_edge.h"
#include "audio_pcm.h"
#include "audio.h"
#include "audio_internal.h"
#include "time.h"
#include "uart.h"

#define EDGE_RING_FRAMES   4800
#define EDGE_BDL_ENTRIES   5

__attribute__((aligned(128)))
static int16_t edge_ring[EDGE_RING_FRAMES];

static audio_pcm_t edge_pcm;
static bool        edge_pcm_open = false;

bool audio_edge_open(audio_edge_t *e, uint64_t emulator_hz) {
    if (!hda_up) {
        uart_puts("audio_edge: backend not up; call audio_init() first\n");
        return false;
    }
    if (edge_pcm_open) {
        uart_puts("audio_edge: a channel is already open (only one supported)\n");
        return false;
    }
    if (emulator_hz == 0) return false;

    /* Zero the ring so the first BDL pass plays silence rather than
     * BSS garbage before the consumer has fed any events. */
    for (uint32_t i = 0; i < EDGE_RING_FRAMES; i++) edge_ring[i] = 0;

    if (!audio_pcm_open(&edge_pcm, edge_ring, EDGE_RING_FRAMES,
                        EDGE_BDL_ENTRIES, audio_sample_rate())) {
        return false;
    }
    /* Start with the ring marked fully pre-filled so writable() waits
     * for the host to drain before we overwrite. */
    audio_pcm_advance(&edge_pcm, EDGE_RING_FRAMES - 1);

    e->emulator_hz   = emulator_hz;
    e->cycle_origin  = 0;
    e->total_samples = 0;
    e->current_level = 0;
    e->n_events      = 0;
    e->dropped_events = 0;
    e->open          = true;
    edge_pcm_open    = true;
    return true;
}

void audio_edge_set(audio_edge_t *e, int16_t level, uint64_t at_cycle) {
    if (!e->open) return;
    if (e->n_events >= sizeof(e->events) / sizeof(e->events[0])) {
        e->dropped_events++;
        /* Update current_level so we at least track the "final" level
         * even if intermediate edges were dropped. */
        e->current_level = level;
        return;
    }
    e->events[e->n_events].cycle = at_cycle;
    e->events[e->n_events].level = level;
    e->n_events++;
}

/* Map an emulator cycle to a sample index relative to e->cycle_origin.
 * Multiply-then-divide ordering matters: u64 cycles × 48000 fits in
 * u128 territory at very long runs (~1.2 years at 3.5 MHz before
 * overflowing u64), but for any realistic session u64 is fine. We
 * subtract origin first to keep the multiplication compact. */
static uint64_t cycle_to_sample(const audio_edge_t *e, uint64_t cycle) {
    uint64_t delta = cycle - e->cycle_origin;
    return (delta * audio_sample_rate()) / e->emulator_hz;
}

void audio_edge_advance(audio_edge_t *e, uint64_t now_cycle) {
    if (!e->open) return;

    uint64_t total_samples_now = cycle_to_sample(e, now_cycle);
    if (total_samples_now <= e->total_samples) {
        /* No host samples elapsed yet — caller's frame was shorter
         * than one sample period (or the clock didn't move). Drop the
         * events we accumulated; their state is captured in the
         * trailing current_level update below. */
        if (e->n_events) {
            e->current_level = e->events[e->n_events - 1].level;
            e->n_events = 0;
        }
        return;
    }

    uint64_t frame_samples_u = total_samples_now - e->total_samples;
    if (frame_samples_u > EDGE_RING_FRAMES - 1) {
        /* The host hasn't drained nearly fast enough; we'd need more
         * than a full ring to catch up. Drop oldest by jumping wp
         * forward — produces a glitch but prevents stalling. */
        uart_printf("audio_edge: ring overflow (%u frames needed, dropping)\n",
                    (uint64_t)frame_samples_u);
        frame_samples_u = EDGE_RING_FRAMES - 1;
    }
    uint32_t frame_samples = (uint32_t)frame_samples_u;

    /* Wait until the host has freed enough room. wfi-park in 1 ms
     * chunks (well under the 20 ms ALSA period so we don't oversleep
     * past a refill opportunity). Bail after ~50 ms — anything longer
     * means the host audio path is stalled, in which case dropping the
     * frame is better than wedging the emulator. */
    {
        uint32_t free   = audio_pcm_writable(&edge_pcm);
        int      waited = 0;
        while (free < frame_samples) {
            if (waited++ >= 50) {
                uart_puts("audio_edge: host not draining for >50ms; dropping frame\n");
                e->n_events = 0;
                e->total_samples = total_samples_now;
                return;
            }
            time_busy_until(time_now() + RVVM_TIME_HZ / 1000);
            free = audio_pcm_writable(&edge_pcm);
        }
    }

    /* Render. Walk events in submission order — caller is expected to
     * submit in monotonic cycle order; if they didn't, the resulting
     * waveform will be wrong but won't crash. */
    uint32_t wp        = edge_pcm.wp_frames;
    uint32_t prev_idx  = 0;
    int16_t  prev_lvl  = e->current_level;
    for (uint32_t i = 0; i < e->n_events; i++) {
        uint64_t evt_cycle = e->events[i].cycle;
        if (evt_cycle < e->cycle_origin)            evt_cycle = e->cycle_origin;
        if (evt_cycle > now_cycle)                  evt_cycle = now_cycle;
        uint64_t evt_sample_total = cycle_to_sample(e, evt_cycle);
        uint32_t evt_idx = (evt_sample_total > e->total_samples)
                          ? (uint32_t)(evt_sample_total - e->total_samples)
                          : 0;
        if (evt_idx > frame_samples) evt_idx = frame_samples;

        for (uint32_t s = prev_idx; s < evt_idx; s++) {
            edge_ring[(wp + s) % EDGE_RING_FRAMES] = prev_lvl;
        }
        prev_idx = evt_idx;
        prev_lvl = e->events[i].level;
    }
    for (uint32_t s = prev_idx; s < frame_samples; s++) {
        edge_ring[(wp + s) % EDGE_RING_FRAMES] = prev_lvl;
    }

    audio_pcm_advance(&edge_pcm, frame_samples);
    e->total_samples = total_samples_now;
    e->current_level = prev_lvl;
    e->n_events = 0;
}

void audio_edge_close(audio_edge_t *e) {
    if (!e->open) return;
    audio_pcm_close(&edge_pcm);
    edge_pcm_open = false;
    e->open       = false;
}
