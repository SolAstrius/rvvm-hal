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
