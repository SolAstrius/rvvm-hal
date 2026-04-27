/* Raw 16-bit signed mono PCM streaming on HDA NID 2 (the converter
 * widget). Drives RVVM's HDA stream worker via a Buffer Descriptor
 * List that loops over a caller-provided ring.
 *
 * Was the second half of hda.c; split out so consumers that don't
 * stream PCM (e.g. the CHIP-8 firmware, which only beeps) don't pull
 * this code into their link. */

#include "audio_pcm.h"
#include "audio_internal.h"
#include "rvvm.h"
#include "mmio.h"
#include "uart.h"

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

/* Translate sample_rate Hz → SDnFMT bit encoding for 16-bit mono. */
static uint16_t fmt_for_rate(uint32_t rate) {
    switch (rate) {
        case 44100: return HDA_FMT_16BIT_MONO_44K1;
        case 48000: return HDA_FMT_16BIT_MONO_48K;
        case 88200: return HDA_FMT_16BIT_MONO_88K2;
        case 96000: return HDA_FMT_16BIT_MONO_96K;
        default:    return 0;
    }
}

bool audio_pcm_open(audio_pcm_t *p, int16_t *ring, uint32_t ring_frames,
                    uint32_t bdl_entries, uint32_t sample_rate) {
    if (!hda_up) {
        uart_puts("audio_pcm: backend not up; call audio_init() first\n");
        return false;
    }
    if (bdl_entries == 0 || bdl_entries > 8) return false;
    if (ring_frames == 0 || (ring_frames % bdl_entries) != 0) {
        uart_printf("audio_pcm: ring_frames=%u must be multiple of bdl_entries=%u\n",
                    (uint64_t)ring_frames, (uint64_t)bdl_entries);
        return false;
    }
    uint16_t fmt = fmt_for_rate(sample_rate);
    if (fmt == 0) {
        uart_printf("audio_pcm: unsupported sample rate %u\n", (uint64_t)sample_rate);
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

    /* Stop any prior stream before reprogramming. */
    SD_W32(HDA_SD_CTL, 0);

    /* Unmute converter widget output amp. NID 2 is mono per RVVM's
     * codec advertisement; updating both LEFT and RIGHT is symmetric
     * and harmless. Gain 0x40 ≈ 0 dB given HDA_AMP_OFFSET=32 with
     * 0.5 dB steps. */
    uint16_t amp_payload = HDA_AMP_OUTPUT | HDA_AMP_LEFT | HDA_AMP_RIGHT | 0x40;
    hda_send_cmd(hda_pack_long(0, HDA_OUT_NID,
                               (uint8_t)HDA_VERB_SET_AMP_GAIN_MUTE, amp_payload));

    /* Format must match the SDnFMT we write below — RVVM stores both
     * in the same field but reads from stream->fmt for pacing. */
    hda_send_cmd(hda_pack_long(0, HDA_OUT_NID,
                               (uint8_t)HDA_VERB_SET_CONV_FMT, fmt));

    /* Bind the codec converter to our stream tag. payload format:
     * (stream_tag << 4) | channel. Channel 0 = mono. */
    uint8_t stream_chan_payload = (uint8_t)((p->stream_tag & 0xF) << 4);
    hda_send_cmd(hda_pack_short(0, HDA_OUT_NID,
                                HDA_VERB_SET_CONV_STREAM, stream_chan_payload));

    /* Build the BDL — split the ring into bdl_entries equal slices.
     * IOC=1 keeps host-side bookkeeping consistent (LPIB updates at
     * boundaries) even though we don't wire an IRQ. */
    uint32_t frames_per_entry = ring_frames / bdl_entries;
    uint32_t bytes_per_entry  = frames_per_entry * 2;   /* 16-bit mono */
    for (uint32_t i = 0; i < bdl_entries; i++) {
        bdl[i].addr  = (uint64_t)(uintptr_t)&ring[i * frames_per_entry];
        bdl[i].len   = bytes_per_entry;
        bdl[i].flags = 1;
    }

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
    uart_printf("audio_pcm: ring %u frames @ %u Hz, %u BDL entries (%u frames each)\n",
                (uint64_t)ring_frames, (uint64_t)sample_rate,
                (uint64_t)bdl_entries, (uint64_t)frames_per_entry);
    return true;
}

uint32_t audio_pcm_position(const audio_pcm_t *p) {
    if (!p->running) return 0;
    uint32_t lpib_bytes = SD_R32(HDA_SD_LPIB);
    return (lpib_bytes / 2) % p->ring_frames;
}

uint32_t audio_pcm_writable(const audio_pcm_t *p) {
    if (!p->running) return 0;
    uint32_t rd = audio_pcm_position(p);
    uint32_t wp = p->wp_frames;
    /* Standard ring: in-flight = (wp - rd + N) % N; free = N - 1 -
     * inflight. The sentinel slot disambiguates "wp == rd && empty"
     * from "wp == rd && full". Caller pre-fill convention is
     * advance(ring_frames - 1) right after open, leaving free = 0
     * until LPIB advances. */
    uint32_t inflight = (wp + p->ring_frames - rd) % p->ring_frames;
    return p->ring_frames - 1 - inflight;
}

void audio_pcm_advance(audio_pcm_t *p, uint32_t frames) {
    if (!p->running) return;
    p->wp_frames = (p->wp_frames + frames) % p->ring_frames;
    p->total_written += frames;
}

void audio_pcm_close(audio_pcm_t *p) {
    if (!p->running) return;
    SD_W32(HDA_SD_CTL, 0);
    p->running = false;
}
