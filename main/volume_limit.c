// SPDX-License-Identifier: MIT
// Audio volume limit for dynamically-loaded apps
//
// Since badge-bsp 1.5.0, bsp_audio_set_volume() maps 0..100% linearly onto the full ES8156 DAC volume range of
// -95.5 dB .. +32 dB. 75% is 0 dB; everything above that is digital gain that makes full-scale audio clip hard.
// Apps pass the launcher's speaker/headphone volume setting (0..100%) straight to the BSP, so instead of changing
// every app, graceloader scales the percentage: the app's 0..100% becomes 0..GRACELOADER_VOLUME_LIMIT_PERCENT
// at the codec. Every volume step stays audible and "100%" in an app means "loudest allowed". The launcher and
// the stored settings are unaffected; the scaling only applies while an app runs under graceloader.
//
// Mechanism: main/CMakeLists.txt links with -Wl,--wrap=bsp_audio_set_volume (and bsp_audio_get_volume). The
// linker then resolves every reference to those functions outside the BSP's own audio source file, including the
// kbelf export table entries apps are linked against, to the __wrap_ functions below; __real_ calls the original
// BSP function. The BSP's internal default volume set during bsp_audio_initialize() is not scaled.
//
// Apps calling the ES8156 driver directly (es8156_set_volume_percentage, es8156_write_volume_control) bypass
// the limit.

#include <stdbool.h>
#include "bsp/audio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "graceloader.h"

static char const TAG[] = "volume_limit";

esp_err_t __real_bsp_audio_set_volume(float percentage);
esp_err_t __real_bsp_audio_get_volume(float* out_percentage);

esp_err_t __wrap_bsp_audio_set_volume(float percentage) {
    // Also catches NaN
    if (!(percentage > 0.0f)) {
        percentage = 0.0f;
    } else if (percentage > 100.0f) {
        percentage = 100.0f;
    }
    float     applied = percentage * GRACELOADER_VOLUME_LIMIT_PERCENT / 100.0f;
    esp_err_t res     = __real_bsp_audio_set_volume(applied);
    ESP_LOGI(TAG, "Volume %.1f%% requested, %.1f%% applied (limit %d%%): %s", percentage, applied,
             GRACELOADER_VOLUME_LIMIT_PERCENT, esp_err_to_name(res));
    return res;
}

esp_err_t __wrap_bsp_audio_get_volume(float* out_percentage) {
    esp_err_t res = __real_bsp_audio_get_volume(out_percentage);
    if (res == ESP_OK && out_percentage != NULL) {
        // Report the value on the app's 0..100% scale
        float percentage = *out_percentage * 100.0f / GRACELOADER_VOLUME_LIMIT_PERCENT;
        *out_percentage  = percentage > 100.0f ? 100.0f : percentage;
    }
    return res;
}
