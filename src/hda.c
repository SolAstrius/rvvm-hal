#include "hda.h"
#include "rvvm.h"
#include "pci.h"
#include "mmio.h"
#include "uart.h"

/* Static DMA buffers — page-aligned per HDA spec.
 *   CORB: 256 bytes  (64 entries × 4 bytes)
 *   RIRB: 2048 bytes (256 entries × 8 bytes)  */
__attribute__((aligned(4096))) static uint32_t corb[64];
__attribute__((aligned(4096))) static uint64_t rirb[256];

static uintptr_t hda_base = 0;
static bool      hda_up   = false;

/* CORB write pointer in entries. RIRB read pointer in entries. */
static uint16_t corb_wp = 0;
static uint16_t rirb_rp = 0;

static inline uint8_t  R8 (uint32_t off)            { return mmio_r8 (hda_base + off); }
static inline uint16_t R16(uint32_t off)            { return mmio_r16(hda_base + off); }
static inline uint32_t R32(uint32_t off)            { return mmio_r32(hda_base + off); }
static inline void     W8 (uint32_t off, uint8_t v) { mmio_w8 (hda_base + off, v); }
static inline void     W16(uint32_t off, uint16_t v){ mmio_w16(hda_base + off, v); }
static inline void     W32(uint32_t off, uint32_t v){ mmio_w32(hda_base + off, v); }

/* Encode a 4-bit verb (low-nibble at bit 8 + 8-bit payload) — Set verbs
 * use this short form; matches SET_BEEP_GENERATION (0x70A) and
 * SET_AMP_GAIN_MUTE (0x3) usage in our context. */
static inline uint32_t verb_pack(uint8_t cad, uint8_t nid, uint16_t verb, uint8_t pay) {
    return ((uint32_t)cad << 28)
         | ((uint32_t)nid << 20)
         | ((uint32_t)verb << 8)
         | pay;
}

uint32_t hda_verb(uint8_t cad, uint8_t nid, uint16_t verb, uint8_t pay) {
    if (!hda_up) return 0;
    uint32_t v = verb_pack(cad, nid, verb, pay);

    /* Push into CORB at next write slot. */
    corb_wp = (corb_wp + 1) & 0xFF;
    corb[corb_wp] = v;
    W16(HDA_REG_CORBWP, corb_wp);

    /* Wait for the controller to advance RIRBWP — RVVM is synchronous so
     * this happens within the MMIO callback. Bound the wait so a missing
     * response doesn't hang the whole firmware. */
    for (int i = 0; i < 100000; i++) {
        if (R16(HDA_REG_RIRBWP) != rirb_rp) {
            rirb_rp = (rirb_rp + 1) & 0xFF;
            return (uint32_t)(rirb[rirb_rp] & 0xFFFFFFFFULL);
        }
    }
    uart_puts("hda: verb response timeout\n");
    return 0;
}

bool hda_init(void) {
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

    /* 1. Take controller out of reset: write GCTL.CRST = 1, wait for it
     *    to read back as 1 (RVVM clears it back to 0 if the guest tries
     *    to use the device during reset). */
    W32(HDA_REG_GCTL, 0);
    while (R32(HDA_REG_GCTL) & 1) {}
    W32(HDA_REG_GCTL, 1);
    while (!(R32(HDA_REG_GCTL) & 1)) {}

    /* 2. Set up CORB. Point base at our static buffer, set size = 256 B
     *    (64 entries) via CORBSIZE = 2, kick the DMA engine. Spec wants
     *    a write-pointer reset before enable. */
    W32(HDA_REG_CORBLBASE, (uint32_t)(uintptr_t)corb);
    W32(HDA_REG_CORBUBASE, ((uint64_t)(uintptr_t)corb) >> 32);
    W8 (HDA_REG_CORBSIZE,  2);
    W16(HDA_REG_CORBWP,    0);
    W8 (HDA_REG_CORBCTL,   2);              /* DMA run */
    corb_wp = 0;

    /* 3. RIRB: 2048 bytes (256 × 8 = response + extended). */
    W32(HDA_REG_RIRBLBASE, (uint32_t)(uintptr_t)rirb);
    W32(HDA_REG_RIRBUBASE, ((uint64_t)(uintptr_t)rirb) >> 32);
    W8 (HDA_REG_RIRBSIZE,  2);
    W16(HDA_REG_RIRBWP,    0x8000);         /* write-1 to reset */
    W16(HDA_REG_RINTCNT,   1);
    W8 (HDA_REG_RIRBCTL,   2);              /* DMA run */
    rirb_rp = 0;

    /* 4. Codec discovery: STATESTS bit n set means a codec is at addr n.
     *    RVVM emulates one codec at addr 0. */
    for (int i = 0; i < 1000; i++) {
        if (R16(HDA_REG_STATESTS) & 1) goto codec_ok;
    }
    uart_puts("hda: codec did not announce on STATESTS\n");
    return false;
codec_ok:
    hda_up = true;

    /* 5. Unmute the beep widget — RVVM's NID 4 reset state has mute=1,
     *    gain=0 dB. Set output amp, both L+R, mute=0, gain=0x40 (the
     *    AMP_GAIN_MUTE payload is 16-bit but the verb is short-form so
     *    the upper bits ride in the verb word itself, encoded in
     *    rvvm.h's HDA_AMP_OUTPUT|HDA_AMP_LEFT|HDA_AMP_RIGHT. */
    uint16_t amp = HDA_AMP_OUTPUT | HDA_AMP_LEFT | HDA_AMP_RIGHT;
    hda_verb(0, RVVM_HDA_BEEP_NID, HDA_VERB_SET_AMP_GAIN_MUTE,
             (uint8_t)(amp >> 8) | 0x40);
    /* Belt-and-braces: also send with the payload's mute bit cleared
     * via the long-form lower byte. The widget is mono so right-channel
     * write is no-op per spec, but RVVM's dispatch looks at either
     * LEFT or RIGHT. */
    uart_puts("hda: controller up, beep widget unmuted\n");
    return true;
}

void hda_beep(uint8_t divider) {
    if (!hda_up) return;
    hda_verb(0, RVVM_HDA_BEEP_NID, HDA_VERB_SET_BEEP_GENERATION, divider);
}
