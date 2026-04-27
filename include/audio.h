/* Audio HAL — three layers, picked by the consumer:
 *
 *   audio_beep(freq_hz)            — fixed-frequency square via codec
 *                                    beep widget. CHIP-8 sound timer,
 *                                    terminal bell. No mixer involvement.
 *
 *   audio_edge_* (audio_edge.h)    — emulator-cycle-driven 1-bit
 *                                    speaker. ZX Spectrum beeper, Apple
 *                                    II $C030 toggle. Caller submits
 *                                    level changes timed in its own
 *                                    clock; HAL converts to host samples.
 *
 *   audio_pcm_*  (audio_pcm.h)     — raw 16-bit signed mono PCM ring
 *                                    streaming, escape hatch for
 *                                    consumers that already have samples
 *                                    (DPCM, ADPCM, sampled music).
 *
 * Linkage modularity: each layer is its own .c file; the static archive
 * pulls only what the consumer references. A CHIP-8 firmware that calls
 * only audio_init / audio_beep links audio.o + the backend (hda.o);
 * audio_edge.o and audio_pcm.o never come along.
 *
 * Backend: HDA today. The "audio_*" namespace doesn't promise HDA
 * forever — if RVVM grows virtio-sound, the backend can swap underneath
 * without consumer changes. Direct hda.h is still available for callers
 * that want low-level codec verb access. */

#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Bring up the audio backend. Idempotent — second and later calls are
 * cheap no-ops. Returns false if RVVM wasn't started with -hda_test
 * (or whatever attaches an audio device on future backends). All other
 * audio_* functions silently no-op until this returns true. */
bool audio_init(void);

/* Stop the backend cleanly. Mostly for tests. */
void audio_shutdown(void);

/* Host sample rate the backend is running at. Currently fixed to
 * 48000 — exposed so consumers don't hardcode it. */
uint32_t audio_sample_rate(void);

/* Fixed-frequency square wave via the codec's beep generator. Pass
 * freq_hz=0 to silence. RVVM's beep widget covers ~47 Hz–12 kHz; out-
 * of-range values clamp to the nearest supported tone.
 *
 * The beep widget runs in parallel to PCM streaming hardware-wise but
 * RVVM doesn't currently mix beep + PCM at the host. Treat them as
 * mutually exclusive: don't audio_beep() while any audio_edge or
 * audio_pcm channel is active. */
void audio_beep(uint32_t freq_hz);
