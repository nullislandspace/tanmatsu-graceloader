// SPDX-License-Identifier: MIT
// Graceloader API — functions exported to dynamically-loaded apps
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Returns the install base path of the currently running app.
// For example, if the app was loaded from /int/apps/tld.username.myapp/app.so,
// this returns "/int/apps/tld.username.myapp".
// The returned string is valid for the lifetime of the app.
const char* graceloader_get_install_basepath(void);

// Audio volume limit.
// While an app runs under graceloader, bsp_audio_set_volume() scales the requested 0..100% down to
// 0..GRACELOADER_VOLUME_LIMIT_PERCENT at the codec, and bsp_audio_get_volume() reports the value back on the
// app's 0..100% scale. With badge-bsp 1.5.0, 75% is 0 dB; higher settings only add digital gain and clipping.
// Pass the launcher's speaker/headphone volume setting unchanged; do not compensate for the scaling.
// Calling the ES8156 driver directly (es8156_set_volume_percentage etc.) bypasses the limit.
#define GRACELOADER_VOLUME_LIMIT_PERCENT 75

#ifdef __cplusplus
}
#endif
