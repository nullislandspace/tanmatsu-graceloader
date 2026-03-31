// Graceloader — Shared ELF library loader for Tanmatsu apps
// Named after Grace Hopper, pioneer of computer programming
//
// This is installed ONCE to appfs. Multiple apps reference it as their
// "interpreter" (type: "script" in metadata.json). The launcher boots
// graceloader, which mounts filesystems, determines the app path from
// boot selection, loads app.so via kbelf, and jumps to its entry point.
//
// The graceloader LINKS all components (BSP, WiFi, PAX, BT, etc.) so
// their symbols are available to app.so, but does NOT initialize them.
// The app decides what to initialize.

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "hal/cache_hal.h"
#include "soc/soc.h"
#include "kbelf.h"
#include "appfs.h"
#include "sdcard.h"
#include "fastopen.h"
#include "graceloader.h"
#include "esp_system.h"
#include "sdkconfig.h"
#include "bsp/device.h"

static const char TAG[] = "graceloader";

// Install basepath — set before loading app.so, readable by the app
static char install_basepath[256] = "";

const char* graceloader_get_install_basepath(void) {
    return install_basepath;
}

// Restart the device back to the launcher
static void restart_to_launcher(void) {
    ESP_LOGI(TAG, "Restarting to launcher...");
    bsp_device_restart_to_launcher();
}

static wl_handle_t wl_handle = WL_INVALID_HANDLE;

// Check if a file exists
static bool file_exists(const char* path) {
    struct stat st;
    return (stat(path, &st) == 0);
}

void app_main(void) {
    ESP_LOGI(TAG, "Graceloader starting...");

    // 1. Mount internal flash filesystem at /int
    esp_vfs_fat_mount_config_t fat_config = {
        .format_if_mount_failed   = false,
        .max_files                = 10,
        .allocation_unit_size     = CONFIG_WL_SECTOR_SIZE,
        .disk_status_check_enable = false,
        .use_one_fat              = false,
    };

    esp_err_t res = esp_vfs_fat_spiflash_mount_rw_wl("/int", "locfd", &fat_config, &wl_handle);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount /int: %s", esp_err_to_name(res));
    } else {
        ESP_LOGI(TAG, "Internal filesystem mounted at /int");
    }

    // 2. Mount SD card at /sd (non-fatal if absent)
    res = sdcard_init();
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "SD card not available: %s", esp_err_to_name(res));
    } else {
        ESP_LOGI(TAG, "SD card mounted at /sd");
    }

    // 3. Determine app path from boot selection
    char elf_path[256];
    bool found = false;

    bool bootsel_valid       = false;
    char const* bootsel_arg  = NULL;
    appfs_handle_t bootsel_handle = appfsBootselGet(&bootsel_valid, &bootsel_arg);

    ESP_LOGI(TAG, "Boot selection: handle=%d valid=%d arg=%s",
             (int)bootsel_handle, bootsel_valid,
             bootsel_arg ? bootsel_arg : "(null)");

    if (bootsel_handle != APPFS_INVALID_FD && bootsel_arg != NULL && strlen(bootsel_arg) > 0) {
        // Note: bootsel_valid=false means the app crashed last time (crash indicator),
        // it does NOT mean the path is invalid. We use the path regardless.
        if (!bootsel_valid) {
            ESP_LOGW(TAG, "Boot selection marked as crashed (valid=false)");
        }
        // The launcher passes the full path to the script file
        // (e.g., /int/apps/tld.username.gracetemplate/app.so)
        snprintf(elf_path, sizeof(elf_path), "%s", bootsel_arg);
        ESP_LOGI(TAG, "Trying boot selection path: %s", elf_path);
        if (file_exists(elf_path)) {
            found = true;
            // Set install basepath to the directory containing the script file
            snprintf(install_basepath, sizeof(install_basepath), "%s", bootsel_arg);
            // Strip the filename to get the directory
            char* last_slash = strrchr(install_basepath, '/');
            if (last_slash) {
                *last_slash = '\0';
            }
        } else {
            ESP_LOGW(TAG, "Script file not found at boot selection path");
        }
    } else {
        ESP_LOGW(TAG, "No boot selection available");
    }

    if (!found) {
        ESP_LOGE(TAG, "Could not find app.so");
        ESP_LOGE(TAG, "Boot selection arg was: %s", bootsel_arg ? bootsel_arg : "(null)");
        restart_to_launcher();
    }

    ESP_LOGI(TAG, "Loading app from: %s", elf_path);
    ESP_LOGI(TAG, "Install basepath: %s", install_basepath);

    // 4. Load ELF library via kbelf
    kbelf_dyn dyn = kbelf_dyn_create(0);
    if (!dyn) {
        ESP_LOGE(TAG, "Failed to create kbelf dynamic context");
        restart_to_launcher();
    }

    if (!kbelf_dyn_set_exec(dyn, elf_path, NULL)) {
        ESP_LOGE(TAG, "Failed to open ELF file: %s", elf_path);
        kbelf_dyn_destroy(dyn);
        restart_to_launcher();
    }

    if (!kbelf_dyn_load(dyn)) {
        ESP_LOGE(TAG, "Failed to load ELF: %s", elf_path);
        kbelf_dyn_destroy(dyn);
        restart_to_launcher();
    }

    // Per-segment I-cache invalidation is handled by kbelfx_cache_sync()
    // which kbelf calls automatically during kbelf_dyn_load().
    // Additionally, do a full DRAM writeback as a safety measure:
#if SOC_CACHE_WRITEBACK_SUPPORTED
    cache_hal_writeback_addr(SOC_DRAM_LOW, SOC_DRAM_HIGH - SOC_DRAM_LOW);
#endif

    // Run preinit and init arrays
    size_t preinit_count = kbelf_dyn_preinit_len(dyn);
    if (preinit_count > 0) {
        ESP_LOGI(TAG, "Running %zu preinit functions", preinit_count);
        for (size_t i = 0; i < preinit_count; i++) {
            ((void (*)(void))kbelf_dyn_preinit_get(dyn, i))();
        }
    }

    size_t init_count = kbelf_dyn_init_len(dyn);
    if (init_count > 0) {
        ESP_LOGI(TAG, "Running %zu init functions", init_count);
        for (size_t i = 0; i < init_count; i++) {
            ((void (*)(void))kbelf_dyn_init_get(dyn, i))();
        }
    }

    // 5. Jump to app entry point
    void (*entry)(int, char const**, char const**) = (void*)kbelf_dyn_entrypoint(dyn);
    ESP_LOGI(TAG, "Jumping to app @ %p", entry);
    entry(1, (char const*[]){elf_path}, (char const*[]){NULL});

    // 6. Cleanup after app returns
    ESP_LOGI(TAG, "App returned, cleaning up");

    size_t fini_count = kbelf_dyn_fini_len(dyn);
    if (fini_count > 0) {
        ESP_LOGI(TAG, "Running %zu fini functions", fini_count);
        for (size_t i = 0; i < fini_count; i++) {
            ((void (*)(void))kbelf_dyn_fini_get(dyn, i))();
        }
    }

    kbelf_dyn_unload(dyn);
    kbelf_dyn_destroy(dyn);

    ESP_LOGI(TAG, "Graceloader done, returning to launcher");
    restart_to_launcher();
}
