/* Intel HDA controller driver — minimal, for the codec-side Beep
 * Generator widget (NID 4) added to RVVM in commit a2a4255 on
 * 2026-04-27 (sound-hda.c §7.2.3.8 / §7.3.3.31).
 *
 * Brings the controller out of reset, plumbs CORB+RIRB so verbs go
 * through, exposes hda_beep(divider) — the only thing CHIP-8 cares
 * about. PCM streams not implemented. */

#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Probe via PCI scan, reset controller, set up CORB/RIRB rings, wait
 * for codec discovery, unmute the beep widget. Returns false if the
 * device isn't present or the codec didn't respond. */
bool hda_init(void);

/* Send a single codec verb, return the 32-bit response. */
uint32_t hda_verb(uint8_t cad, uint8_t nid, uint16_t verb, uint8_t pay);

/* Convenience: control the beep widget on NID 4.
 * divider = 0          → silent
 * divider = 1..255     → tone at 48000 / (4 * divider) Hz */
void hda_beep(uint8_t divider);

/* ====================================================================
 *  PCM streaming on NID 2 (the converter widget).
 *
 *  Drives RVVM's HDA stream worker (sound-hda.c sound_hda_stream_drain)
 *  with a Buffer Descriptor List that loops over a caller-provided
 *  ring of 16-bit signed mono samples. The host worker paces playback
 *  to wall-clock at the configured rate, so the firmware just needs to
 *  keep the ring fed faster than sample_rate * 2 bytes/s — underruns
 *  show up as clicks (the ring keeps cycling, but with stale data).
 *
 *  Lifecycle:
 *
 *      static int16_t ring[4800] __attribute__((aligned(128)));
 *      hda_pcm_t pcm;
 *      hda_pcm_init(&pcm, ring, 4800, 4, 48000);
 *      // fill ring with audio
 *      hda_pcm_advance(&pcm, 4800);   // commit: host can play it
 *      // ... refill as hda_pcm_writable() returns space
 *      hda_pcm_stop(&pcm);
 *
 *  Format is fixed: 16-bit signed little-endian, 1 channel (mono).
 *  Sample rate must be one RVVM advertises — 44100, 48000, 88200,
 *  96000 (CODEC_PARAM_SUPP_PCM_SIZE_RATES in sound-hda.c). 48 kHz is
 *  the lowest-overhead choice.
 *
 *  Only one PCM stream at a time (RVVM has a single output stream
 *  descriptor; HDA_PARAM_NO_OUT = 1).
 * ==================================================================== */

typedef struct {
    int16_t   *ring;            /* caller-provided, ≥ ring_frames * 2 bytes */
    uint32_t   ring_frames;     /* total mono frames in the ring */
    uint32_t   sample_rate;     /* in Hz */
    uint32_t   bdl_entries;     /* 1..8; ring split into N equal slices */
    uint32_t   wp_frames;       /* our write head, always wp < ring_frames */
    uint32_t   total_written;   /* monotonic frame counter for debugging */
    uint8_t    stream_tag;      /* HDA stream tag (1..15) */
    bool       running;
} hda_pcm_t;

/* Configure NID 2 (unmute, set format, set stream tag), program the
 * stream descriptor at MMIO 0xA0 with the BDL pointing at slices of
 * `ring`, and start playback. The ring is treated as cyclic — whatever
 * the firmware leaves in it loops forever until hda_pcm_stop().
 *
 * Constraints:
 *   - hda_init() must have succeeded.
 *   - ring_frames must be a multiple of bdl_entries.
 *   - bdl_entries in [1, 8].
 *   - sample_rate ∈ {44100, 48000, 88200, 96000}.
 *   - ring should be 128-byte aligned for HDA spec compliance (BDL
 *     itself is internally aligned).
 *
 * Returns false on argument errors or if the controller isn't up. */
bool hda_pcm_init(hda_pcm_t *p, int16_t *ring, uint32_t ring_frames,
                  uint32_t bdl_entries, uint32_t sample_rate);

/* How many mono frames are free in the ring — i.e. how many the
 * firmware can write into starting at p->wp_frames without overrunning
 * the slot the host is currently reading. Computed from SDnLPIB. */
uint32_t hda_pcm_writable(const hda_pcm_t *p);

/* Commit `frames` newly-written frames. Advances the firmware-side
 * write head (with wraparound). Caller is responsible for filling
 * &p->ring[p->wp_frames] before calling. Frames don't reach the
 * speaker until they fall behind LPIB on the host's next worker pass —
 * usually within a few BDL slice intervals. */
void hda_pcm_advance(hda_pcm_t *p, uint32_t frames);

/* Host's read head, in mono frames within the ring (0..ring_frames-1).
 * Useful for sync — e.g. emulators that want to stay one ring-quarter
 * ahead of LPIB. Reads SDnLPIB and divides by 2. */
uint32_t hda_pcm_position(const hda_pcm_t *p);

/* Stop playback. Clears RUN, leaves the ring contents alone. After
 * stop, the descriptor can be re-init'd via hda_pcm_init for a
 * different rate/ring. */
void hda_pcm_stop(hda_pcm_t *p);
