/* Raw 16-bit signed mono PCM streaming. Direct rename of the prior
 * hda_pcm_* surface, decoupled from the HDA name so the abstraction
 * survives a backend swap. See audio.h for the layered context. */

#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int16_t   *ring;
    uint32_t   ring_frames;
    uint32_t   sample_rate;
    uint32_t   bdl_entries;
    uint32_t   wp_frames;
    uint32_t   total_written;
    uint8_t    stream_tag;
    bool       running;
} audio_pcm_t;

/* Open a PCM stream backed by the caller-provided ring. ring_frames
 * must be a multiple of bdl_entries; bdl_entries in [1, 8]; sample_rate
 * one of {44100, 48000, 88200, 96000}. Ring should be 128-byte aligned
 * (HDA spec §3.6.3). audio_init() must have succeeded.
 *
 * Constraints unchanged from the previous hda_pcm_init. */
bool audio_pcm_open(audio_pcm_t *p, int16_t *ring, uint32_t ring_frames,
                    uint32_t bdl_entries, uint32_t sample_rate);

/* Free frames the caller can write to from p->wp_frames without
 * overrunning the slot the host is currently consuming. Computed from
 * the stream descriptor's LPIB. */
uint32_t audio_pcm_writable(const audio_pcm_t *p);

/* Host's current read head, in frames within the ring. Useful for
 * keeping wp ahead of LPIB by a fixed safety margin. */
uint32_t audio_pcm_position(const audio_pcm_t *p);

/* Commit `frames` newly-filled frames. Caller must have already
 * written &p->ring[p->wp_frames] for the corresponding slots. */
void audio_pcm_advance(audio_pcm_t *p, uint32_t frames);

/* Stop playback. Ring contents preserved. */
void audio_pcm_close(audio_pcm_t *p);
