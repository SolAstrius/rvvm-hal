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

static uint16_t corb_wp = 0;
static uint16_t rirb_rp = 0;

static inline uint8_t  R8 (uint32_t off)            { return mmio_r8 (hda_base + off); }
static inline uint16_t R16(uint32_t off)            { return mmio_r16(hda_base + off); }
static inline uint32_t R32(uint32_t off)            { return mmio_r32(hda_base + off); }
static inline void     W8 (uint32_t off, uint8_t v) { mmio_w8 (hda_base + off, v); }
static inline void     W16(uint32_t off, uint16_t v){ mmio_w16(hda_base + off, v); }
static inline void     W32(uint32_t off, uint32_t v){ mmio_w32(hda_base + off, v); }

/* HDA codec command word format (HDA spec §7.3 Figure 58):
 *   bits 31:28  Codec Address
 *   bits 27:20  Node ID
 *   bits 19:0   Command — encoded as ONE OF:
 *
 *     SHORT verb  (12-bit verb 0x100..0xFFF, 8-bit payload):
 *        bits 19:8  = verb
 *        bits  7:0  = payload
 *
 *     LONG verb  (4-bit verb 0x0..0xF, 16-bit payload):
 *        bits 19:16 = verb
 *        bits 15:0  = payload
 *
 * Which form a verb uses is fixed per-verb. SET_AMP_GAIN_MUTE (0x3),
 * SET_CONV_FMT (0x2), SET_PROC_COEFF (0x4), etc. are 4-bit verbs.
 * GET_PARAMETER (0xF00), SET_BEEP_GENERATION (0x70A), SET_POWER_STATE
 * (0x705), etc. are 12-bit verbs. */
static uint32_t pack_short(uint8_t cad, uint8_t nid, uint16_t verb, uint8_t pay) {
    return ((uint32_t)cad << 28)
         | ((uint32_t)nid << 20)
         | ((uint32_t)(verb & 0xFFF) << 8)
         | pay;
}
static uint32_t pack_long(uint8_t cad, uint8_t nid, uint8_t verb, uint16_t pay) {
    return ((uint32_t)cad << 28)
         | ((uint32_t)nid << 20)
         | ((uint32_t)(verb & 0xF) << 16)
         | pay;
}

static uint32_t send_cmd(uint32_t cmd) {
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
    return send_cmd(pack_short(cad, nid, verb, pay));
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
     * a 16-bit payload, NOT a 12-bit verb — see pack_long.
     *   bit 15: output amp     (1 = applies to output amp)
     *   bit 14: input amp      (1 = applies to input amp)
     *   bit 13: left channel   (1 = update left)
     *   bit 12: right channel  (1 = update right)
     *   bit  7: mute           (1 = muted)
     *   bits 6:0: gain         (0x40 ≈ 0 dB after RVVM's per-step amp)
     *
     * The beep widget is mono per spec, but RVVM's dispatch accepts
     * either LEFT or RIGHT to update the single value — set both. */
    uint16_t amp_payload =
        HDA_AMP_OUTPUT | HDA_AMP_LEFT | HDA_AMP_RIGHT | 0x40;  /* mute=0 */
    send_cmd(pack_long(0, RVVM_HDA_BEEP_NID,
                       (uint8_t)HDA_VERB_SET_AMP_GAIN_MUTE,
                       amp_payload));

    uart_puts("hda: controller up, beep widget unmuted\n");
    return true;
}

void hda_beep(uint8_t divider) {
    if (!hda_up) return;
    /* SET_BEEP_GENERATION is a 12-bit verb (0x70A) with 8-bit payload. */
    send_cmd(pack_short(0, RVVM_HDA_BEEP_NID,
                        HDA_VERB_SET_BEEP_GENERATION, divider));
}

/* ====================================================================
 *  PCM streaming on NID 2 (the converter widget).
 * ==================================================================== */

#define HDA_OUT_NID         2

/* BDL: u64 addr + u32 len + u32 flags-with-IOC. Each entry must be
 * 16-byte aligned; the BDL itself must be 128-byte aligned per HDA
 * spec §3.6.3. We allocate up to 8 entries since we have one stream. */
typedef struct {
    uint64_t addr;
    uint32_t len;
    uint32_t flags;            /* bit 0 = IOC */
} __attribute__((aligned(16))) bdl_entry_t;

__attribute__((aligned(128)))
static bdl_entry_t bdl[8];

/* Stream descriptor MMIO accessors — relative to the output stream
 * descriptor block at HDA_BAR0 + RVVM_HDA_OUT_SD_BASE. */
static inline uintptr_t sd_reg(uint32_t off) {
    return hda_base + RVVM_HDA_OUT_SD_BASE + off;
}
static inline uint32_t SD_R32(uint32_t off) { return mmio_r32(sd_reg(off)); }
static inline void SD_W32(uint32_t off, uint32_t v) { mmio_w32(sd_reg(off), v); }
static inline void SD_W16(uint32_t off, uint16_t v) { mmio_w16(sd_reg(off), v); }

/* Translate sample_rate Hz → SDnFMT bit encoding for 16-bit mono.
 * Returns 0 if the rate isn't one RVVM advertises. */
static uint16_t fmt_for_rate(uint32_t rate) {
    switch (rate) {
        case 44100: return HDA_FMT_16BIT_MONO_44K1;
        case 48000: return HDA_FMT_16BIT_MONO_48K;
        case 88200: return HDA_FMT_16BIT_MONO_88K2;
        case 96000: return HDA_FMT_16BIT_MONO_96K;
        default:    return 0;
    }
}

bool hda_pcm_init(hda_pcm_t *p, int16_t *ring, uint32_t ring_frames,
                  uint32_t bdl_entries, uint32_t sample_rate) {
    if (!hda_up) {
        uart_puts("hda_pcm: controller not up; call hda_init() first\n");
        return false;
    }
    if (bdl_entries == 0 || bdl_entries > 8) return false;
    if (ring_frames == 0 || (ring_frames % bdl_entries) != 0) {
        uart_printf("hda_pcm: ring_frames=%u must be a multiple of bdl_entries=%u\n",
                    (uint64_t)ring_frames, (uint64_t)bdl_entries);
        return false;
    }
    uint16_t fmt = fmt_for_rate(sample_rate);
    if (fmt == 0) {
        uart_printf("hda_pcm: unsupported sample rate %u\n", (uint64_t)sample_rate);
        return false;
    }

    p->ring          = ring;
    p->ring_frames   = ring_frames;
    p->sample_rate   = sample_rate;
    p->bdl_entries   = bdl_entries;
    p->wp_frames     = 0;
    p->total_written = 0;
    p->stream_tag    = 1;
    p->running       = false;

    /* Make sure the stream is stopped before we reprogram it. SDnCTL
     * RUN clear; preserve SRST behaviour by writing 0 outright (RUN=0,
     * SRST=0). */
    SD_W32(HDA_SD_CTL, 0);

    /* Unmute the converter widget's output amp. NID 2 is mono per
     * sound-hda.c codec advertisement, but RVVM accepts either LEFT
     * or RIGHT to update the single value — set both for symmetry.
     * Gain 0x40 ≈ 0 dB in our amp configuration (HDA_AMP_OFFSET = 32,
     * step = 0.5 dB; gain 0x40 above 0x20 ⇒ above offset, clamped to 0 dB). */
    uint16_t amp_payload = HDA_AMP_OUTPUT | HDA_AMP_LEFT | HDA_AMP_RIGHT | 0x40;
    send_cmd(pack_long(0, HDA_OUT_NID,
                       (uint8_t)HDA_VERB_SET_AMP_GAIN_MUTE, amp_payload));

    /* Tell the codec what format it'll see. Must match the SDnFMT we
     * write to the stream descriptor below — sound-hda.c stores them
     * in the same field but reads from stream->fmt for pacing. */
    send_cmd(pack_long(0, HDA_OUT_NID,
                       (uint8_t)HDA_VERB_SET_CONV_FMT, fmt));

    /* Bind the codec converter to our stream tag. payload format:
     * (stream_tag << 4) | channel. We use channel 0 (mono). */
    uint8_t stream_chan_payload =
        (uint8_t)((p->stream_tag & 0xF) << 4);
    send_cmd(pack_short(0, HDA_OUT_NID,
                        HDA_VERB_SET_CONV_STREAM, stream_chan_payload));

    /* Build the BDL — split the ring into bdl_entries equal slices.
     * Each entry has IOC=1 so the host stream worker latches BCIS at
     * boundaries; we don't actually wire an IRQ handler, but having
     * IOC set keeps the hardware-side bookkeeping consistent and
     * advances LPIB the same way. */
    uint32_t frames_per_entry = ring_frames / bdl_entries;
    uint32_t bytes_per_entry  = frames_per_entry * 2;   /* 16-bit mono */
    for (uint32_t i = 0; i < bdl_entries; i++) {
        bdl[i].addr  = (uint64_t)(uintptr_t)&ring[i * frames_per_entry];
        bdl[i].len   = bytes_per_entry;
        bdl[i].flags = 1;     /* IOC */
    }

    /* Program the stream descriptor. */
    uint64_t bdl_addr = (uint64_t)(uintptr_t)bdl;
    SD_W32(HDA_SD_BDPL, (uint32_t)(bdl_addr & 0xFFFFFFFFu));
    SD_W32(HDA_SD_BDPU, (uint32_t)(bdl_addr >> 32));
    SD_W32(HDA_SD_CBL,  ring_frames * 2);                /* bytes */
    SD_W16(HDA_SD_LVI,  (uint16_t)(bdl_entries - 1));
    SD_W16(HDA_SD_FMT,  fmt);

    /* Start: SDnCTL with stream tag in bits 23:20, RUN=1 in bit 1. */
    uint32_t ctl = ((uint32_t)p->stream_tag << 20) | (1u << 1);
    SD_W32(HDA_SD_CTL, ctl);

    p->running = true;
    uart_printf("hda_pcm: ring %u frames @ %u Hz, %u BDL entries (%u frames each)\n",
                (uint64_t)ring_frames, (uint64_t)sample_rate,
                (uint64_t)bdl_entries, (uint64_t)frames_per_entry);
    return true;
}

uint32_t hda_pcm_position(const hda_pcm_t *p) {
    if (!p->running) return 0;
    uint32_t lpib_bytes = SD_R32(HDA_SD_LPIB);
    return (lpib_bytes / 2) % p->ring_frames;
}

uint32_t hda_pcm_writable(const hda_pcm_t *p) {
    if (!p->running) return 0;
    uint32_t rd = hda_pcm_position(p);
    uint32_t wp = p->wp_frames;
    /* Distance forward from wp to rd (the "to-be-played" region we
     * must NOT overwrite). Wrap-aware. Reserve one frame to disambiguate
     * full vs empty. */
    uint32_t inflight = (wp >= rd)
                      ? (p->ring_frames - (wp - rd))
                      : (rd - wp);
    if (inflight == 0) inflight = p->ring_frames;
    /* `inflight` includes the wp slot itself when wp == rd; treat as
     * "fully drained, all empty." Free = inflight - 1 to leave a
     * sentinel slot. */
    return inflight - 1;
}

void hda_pcm_advance(hda_pcm_t *p, uint32_t frames) {
    if (!p->running) return;
    p->wp_frames = (p->wp_frames + frames) % p->ring_frames;
    p->total_written += frames;
}

void hda_pcm_stop(hda_pcm_t *p) {
    if (!p->running) return;
    SD_W32(HDA_SD_CTL, 0);
    p->running = false;
}
