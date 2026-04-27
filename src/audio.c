/* Audio HAL front layer — backend-agnostic entry points. Currently a
 * thin wrapper over hda.c; the indirection exists so consumer code
 * doesn't bake "HDA" into its includes / namespace and we can swap the
 * backend later (virtio-sound, PC speaker, etc.) without churn. */

#include "audio.h"
#include "hda.h"
#include "audio_internal.h"
#include "uart.h"

#define HOST_SAMPLE_RATE  48000U

bool audio_init(void) {
    return hda_init();
}

void audio_shutdown(void) {
    /* No backend teardown today — RVVM doesn't care if we leave the
     * controller running on firmware exit. Provided for symmetry and
     * for future backends that need cleanup. */
}

uint32_t audio_sample_rate(void) {
    return HOST_SAMPLE_RATE;
}

/* Hz → beep-widget divider. The widget runs at 48 kHz nominal and the
 * tone is 48000 / (4 * divider), so divider = 12000 / freq_hz, clamped
 * to [1, 255] (≈ 47 Hz to 12 kHz). */
void audio_beep(uint32_t freq_hz) {
    if (!hda_up) return;
    if (freq_hz == 0) {
        hda_beep(0);
        return;
    }
    uint32_t div = 12000u / freq_hz;
    if (div < 1)   div = 1;
    if (div > 255) div = 255;
    hda_beep((uint8_t)div);
}
