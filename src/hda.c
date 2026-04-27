/* HDA controller backend — controller reset, CORB/RIRB plumbing, codec
 * verb dispatch, simple beep widget. The PCM streaming portion has
 * moved to audio_pcm.c so consumers that don't want streaming don't
 * pull it in.
 *
 * Public surface kept here for back-compat:
 *   hda_init(), hda_verb(), hda_beep(divider)  — see hda.h
 *
 * Higher-level entry points (audio_init, audio_beep with Hz argument)
 * live in audio.c and delegate to these. */

#include "hda.h"
#include "audio_internal.h"
#include "rvvm.h"
#include "pci.h"
#include "mmio.h"
#include "uart.h"

/* Static DMA buffers — page-aligned per HDA spec.
 *   CORB: 256 bytes  (64 entries × 4 bytes)
 *   RIRB: 2048 bytes (256 entries × 8 bytes)  */
__attribute__((aligned(4096))) static uint32_t corb[64];
__attribute__((aligned(4096))) static uint64_t rirb[256];

/* Exposed via audio_internal.h to sibling .c files. */
uintptr_t hda_base = 0;
bool      hda_up   = false;

static uint16_t corb_wp = 0;
static uint16_t rirb_rp = 0;

static inline uint8_t  R8 (uint32_t off)            { return mmio_r8 (hda_base + off); }
static inline uint16_t R16(uint32_t off)            { return mmio_r16(hda_base + off); }
static inline uint32_t R32(uint32_t off)            { return mmio_r32(hda_base + off); }
static inline void     W8 (uint32_t off, uint8_t v) { mmio_w8 (hda_base + off, v); }
static inline void     W16(uint32_t off, uint16_t v){ mmio_w16(hda_base + off, v); }
static inline void     W32(uint32_t off, uint32_t v){ mmio_w32(hda_base + off, v); }

/* HDA codec command word packing — see HDA spec §7.3 Figure 58.
 * Verbs come in two flavours: 12-bit verb + 8-bit payload (short), or
 * 4-bit verb + 16-bit payload (long). Which form a verb uses is fixed
 * per-verb. SET_AMP_GAIN_MUTE (0x3), SET_CONV_FMT (0x2), etc. are 4-bit
 * verbs; GET_PARAMETER (0xF00), SET_BEEP_GENERATION (0x70A), etc. are
 * 12-bit verbs. */
uint32_t hda_pack_short(uint8_t cad, uint8_t nid, uint16_t verb, uint8_t pay) {
    return ((uint32_t)cad << 28)
         | ((uint32_t)nid << 20)
         | ((uint32_t)(verb & 0xFFF) << 8)
         | pay;
}
uint32_t hda_pack_long(uint8_t cad, uint8_t nid, uint8_t verb, uint16_t pay) {
    return ((uint32_t)cad << 28)
         | ((uint32_t)nid << 20)
         | ((uint32_t)(verb & 0xF) << 16)
         | pay;
}

uint32_t hda_send_cmd(uint32_t cmd) {
    if (!hda_up) return 0;
    corb_wp = (corb_wp + 1) & 0xFF;
    corb[corb_wp] = cmd;
    W16(HDA_REG_CORBWP, corb_wp);

    /* RVVM is synchronous so the response should land within a tick of
     * the CORBWP write. Bound the spin so a missing response doesn't
     * hang the firmware. */
    for (int i = 0; i < 100000; i++) {
        if (R16(HDA_REG_RIRBWP) != rirb_rp) {
            rirb_rp = (rirb_rp + 1) & 0xFF;
            return (uint32_t)(rirb[rirb_rp] & 0xFFFFFFFFULL);
        }
    }
    uart_puts("hda: verb response timeout\n");
    return 0;
}

uint32_t hda_verb(uint8_t cad, uint8_t nid, uint16_t verb, uint8_t pay) {
    return hda_send_cmd(hda_pack_short(cad, nid, verb, pay));
}

bool hda_init(void) {
    if (hda_up) return true;

    pci_func_t pf;
    if (!pci_find_device(RVVM_PCI_ID_HDA, &pf)) {
        uart_puts("hda: device not found (start RVVM with -hda_test)\n");
        return false;
    }
    pci_setup_bars(&pf);
    if (pf.bar[0] == 0) {
        uart_puts("hda: BAR0 not mapped\n");
        return false;
    }
    hda_base = pf.bar[0];

    /* Take controller out of reset. */
    W32(HDA_REG_GCTL, 0);
    while (R32(HDA_REG_GCTL) & 1) {}
    W32(HDA_REG_GCTL, 1);
    while (!(R32(HDA_REG_GCTL) & 1)) {}

    /* CORB: 256 B, 64 entries. */
    W32(HDA_REG_CORBLBASE, (uint32_t)(uintptr_t)corb);
    W32(HDA_REG_CORBUBASE, ((uint64_t)(uintptr_t)corb) >> 32);
    W8 (HDA_REG_CORBSIZE,  2);
    W16(HDA_REG_CORBWP,    0);
    W8 (HDA_REG_CORBCTL,   2);
    corb_wp = 0;

    /* RIRB: 2048 B, 256 entries. */
    W32(HDA_REG_RIRBLBASE, (uint32_t)(uintptr_t)rirb);
    W32(HDA_REG_RIRBUBASE, ((uint64_t)(uintptr_t)rirb) >> 32);
    W8 (HDA_REG_RIRBSIZE,  2);
    W16(HDA_REG_RIRBWP,    0x8000);
    W16(HDA_REG_RINTCNT,   1);
    W8 (HDA_REG_RIRBCTL,   2);
    rirb_rp = 0;

    /* Codec discovery. */
    for (int i = 0; i < 1000; i++) {
        if (R16(HDA_REG_STATESTS) & 1) goto codec_ok;
    }
    uart_puts("hda: codec did not announce on STATESTS\n");
    return false;
codec_ok:
    hda_up = true;

    /* Unmute the beep widget. SET_AMP_GAIN_MUTE is a 4-bit verb with
     * a 16-bit payload, NOT a 12-bit verb — see hda_pack_long. */
    uint16_t amp_payload =
        HDA_AMP_OUTPUT | HDA_AMP_LEFT | HDA_AMP_RIGHT | 0x40;  /* mute=0 */
    hda_send_cmd(hda_pack_long(0, RVVM_HDA_BEEP_NID,
                               (uint8_t)HDA_VERB_SET_AMP_GAIN_MUTE,
                               amp_payload));

    uart_puts("hda: controller up, beep widget unmuted\n");
    return true;
}

void hda_beep(uint8_t divider) {
    if (!hda_up) return;
    /* SET_BEEP_GENERATION is a 12-bit verb (0x70A) with 8-bit payload. */
    hda_send_cmd(hda_pack_short(0, RVVM_HDA_BEEP_NID,
                                HDA_VERB_SET_BEEP_GENERATION, divider));
}
