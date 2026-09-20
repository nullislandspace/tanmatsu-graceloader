// Graceloader — local time for the app and its files
//
// Newlib starts every process at UTC, and an app launched through
// graceloader is a fresh boot: nothing has run tzset() yet. That is not
// only cosmetic, because ESP-IDF's FATFS takes a file's timestamp from
// localtime_r() (components/fatfs/diskio/diskio.c, get_fattime), so
// every file an app writes to /sd or /int would be stamped in UTC and
// show up an hour or two out in a card reader.
//
// The launcher already solved the hard half: when the user picks a zone
// in Settings > Clock it stores both the zone name and the raw POSIX TZ
// string in NVS ("system" / "tz"), the second one, in its own words,
// "for use in applications without this library" — no zone table needed
// here, just the string.
//
// So: read it once, before anything is mounted or written, and apply it
// to the process. The app inherits it, since it runs in this very
// process, and so does anything it writes.

#include "timezone.h"
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char TAG[] = "graceloader";

#define TZ_NAMESPACE "system"
#define TZ_KEY       "tz"
#define TZ_MAX       64  // as the launcher's TIMEZONE_TZ_LEN

void graceloader_apply_timezone(void) {
    // Deliberately no erase-and-retry on a damaged NVS: the user's
    // settings live in there, and losing them to fix a clock is a bad
    // trade. UTC is a survivable outcome; an erased partition is not.
    esp_err_t res = nvs_flash_init();
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "NVS unavailable (%s): keeping UTC", esp_err_to_name(res));
        return;
    }

    nvs_handle_t handle;
    res = nvs_open(TZ_NAMESPACE, NVS_READONLY, &handle);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "No '" TZ_NAMESPACE "' settings (%s): keeping UTC", esp_err_to_name(res));
        return;
    }

    char   tz[TZ_MAX];
    size_t len = sizeof(tz);
    res        = nvs_get_str(handle, TZ_KEY, tz, &len);
    nvs_close(handle);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "No timezone set (%s): keeping UTC. Set one in the launcher: Settings > Clock > Set timezone",
                 esp_err_to_name(res));
        return;
    }

    setenv("TZ", tz, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone: %s", tz);
}
