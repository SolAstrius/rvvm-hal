/* Raw 16-bit signed PCM streaming, mono or stereo. Direct successor
 * to the prior hda_pcm_* surface, decoupled from the HDA name so the
 * abstraction survives a backend swap. See audio.h for the layered
 * context.
 *
 * Frame vs sample: throughout this API a "frame" is one timepoint of
 * audio across all channels — i.e. for stereo, one frame is an L/R
 * pair (two int16). The caller's ring is `int16 ring[ring_frames *
 * channels]` (interleaved L,R,L,R,…); ring_frames, wp_frames,
 * audio_pcm_writable, audio_pcm_position, audio_pcm_advance all
 * count frames, never raw samples. */

#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int16_t   *ring;
    uint32_t   ring_frames;
    uint32_t   sample_rate;
    uint32_t   channels;       /* 1 = mono, 2 = stereo */
    uint32_t   bdl_entries;
    uint32_t   wp_frames;
    uint32_t   total_written;
    uint8_t    stream_tag;
    bool       running;
} audio_pcm_t;

/* Open a PCM stream backed by the caller-provided ring. The ring must
 * hold `ring_frames * channels` int16 samples (interleaved). Constraints:
 *   - ring_frames a multiple of bdl_entries
 *   - bdl_entries in [1, 8]
 *   - sample_rate in {44100, 48000, 88200, 96000}
 *   - channels in {1, 2}
 *   - ring 128-byte aligned (HDA spec §3.6.3)
 * audio_init() must have succeeded.
 *
 * Note: RVVM today downmixes anything > 1 channel to mono before the
 * ALSA backend (see sound-hda.c — channels averaged). When RVVM grows
 * a stereo widget the same call delivers true stereo with no firmware
 * change. */
bool audio_pcm_open(audio_pcm_t *p, int16_t *ring, uint32_t ring_frames,
                    uint32_t bdl_entries, uint32_t sample_rate,
                    uint32_t channels);

/* Free frames the caller can write to from p->wp_frames without
 * overrunning the slot the host is currently consuming. Computed from
 * the stream descriptor's LPIB. */
uint32_t audio_pcm_writable(const audio_pcm_t *p);

/* Host's current read head, in frames within the ring. Useful for
 * keeping wp ahead of LPIB by a fixed safety margin. */
uint32_t audio_pcm_position(const audio_pcm_t *p);

/* Commit `frames` newly-filled frames. Caller must have already
 * written &p->ring[p->wp_frames * p->channels] for the corresponding
 * slots. */
void audio_pcm_advance(audio_pcm_t *p, uint32_t frames);

/* Stop playback. Ring contents preserved. */
void audio_pcm_close(audio_pcm_t *p);
