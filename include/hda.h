/* HDA controller backend — low-level access for callers that want to
 * speak codec verbs directly. Most consumers should use audio.h /
 * audio_pcm.h / audio_edge.h instead, which abstract over the backend
 * choice.
 *
 * What's here is the slice that's HDA-specific:
 *   hda_init()      — controller bring-up. audio_init() calls this.
 *   hda_verb()      — send a codec verb, get the response.
 *   hda_beep()      — divider-based beep widget. audio_beep(freq_hz)
 *                     wraps this with a Hz-based interface.
 *
 * The PCM streaming surface previously here (hda_pcm_*) now lives in
 * audio_pcm.h with names audio_pcm_*. */

#pragma once
#include <stdint.h>
#include <stdbool.h>

bool     hda_init(void);
uint32_t hda_verb(uint8_t cad, uint8_t nid, uint16_t verb, uint8_t pay);
void     hda_beep(uint8_t divider);
