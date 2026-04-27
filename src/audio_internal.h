/* Sibling-only HDA backend access. Lives in src/, not include/, so
 * external consumers can't reach it. Used by audio_pcm.c to program
 * the stream descriptor and send codec verbs without duplicating the
 * MMIO/CORB plumbing in hda.c. */

#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Set after a successful hda_init. Other audio_* code reads this to
 * silently no-op when the backend isn't up. */
extern bool       hda_up;
extern uintptr_t  hda_base;

/* Pack a 12-bit-verb (8-bit payload) HDA codec command. */
uint32_t hda_pack_short(uint8_t cad, uint8_t nid, uint16_t verb, uint8_t pay);

/* Pack a 4-bit-verb (16-bit payload) HDA codec command. SET_AMP_GAIN_MUTE
 * (0x3) and SET_CONV_FMT (0x2) are the common ones audio_pcm.c uses. */
uint32_t hda_pack_long(uint8_t cad, uint8_t nid, uint8_t verb, uint16_t pay);

/* Send a packed command, return the response (0 on timeout/error). */
uint32_t hda_send_cmd(uint32_t cmd);
