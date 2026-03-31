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

#ifdef __cplusplus
}
#endif
