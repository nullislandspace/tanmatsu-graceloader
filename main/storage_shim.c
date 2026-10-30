// SPDX-License-Identifier: MIT
// Neutralize the BSP storage subsystem, see main/CMakeLists.txt
//
// Since badge-bsp 1.6.0 the BSP owns the SD card: bsp_storage_initialize() claims the on-chip LDO channel 4 and
// mounts /sd. graceloader's own sdcard.c already does both, so the BSP fails with "can't acquire the channel,
// already in use by others or not adjustable" and an app calling bsp_device_initialize() loses its storage.
//
// bsp_storage_initialize() is wrapped to a no-op so the LDO channel stays with graceloader, and the mount entry
// points are wrapped to a clean failure so no app can re-init the card underneath us. bsp_storage_mount() calls
// bsp_storage_mount_advanced() from the same object file, which --wrap does not redirect, so both names are
// wrapped.
//
// The remaining storage queries are left alone: with no init they report NOT_SUPPORTED / NOT_MOUNTED, and
// bsp_storage_format() / bsp_storage_unmount() bail out safely.

#include "bsp/storage.h"
#include "esp_err.h"
#include "esp_log.h"

static char const TAG[] = "storage_shim";

esp_err_t __wrap_bsp_storage_initialize(void) {
    static bool logged = false;
    if (!logged) {
        logged = true;
        ESP_LOGI(TAG, "BSP storage init skipped, graceloader owns the SD card");
    }
    return ESP_OK;
}

static esp_err_t storage_mount_not_supported(void) {
    ESP_LOGE(TAG, "BSP storage mount is unavailable under graceloader, the card is already mounted on /sd");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t __wrap_bsp_storage_mount(bsp_storage_type_t type, const char* mountpoint) {
    (void)type;
    (void)mountpoint;
    return storage_mount_not_supported();
}

esp_err_t __wrap_bsp_storage_mount_advanced(bsp_storage_type_t type, const char* mountpoint, int max_files,
                                            bool format_if_mount_failed) {
    (void)type;
    (void)mountpoint;
    (void)max_files;
    (void)format_if_mount_failed;
    return storage_mount_not_supported();
}
