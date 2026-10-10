#pragma once

#include <stdint.h>

#include "sdmmc_cmd.h"

#include <stdbool.h>
#include "esp_err.h"

// Initialize SD card power and mount filesystem at /sd
esp_err_t sdcard_init(void);

// Check if SD card is mounted
bool sdcard_is_mounted(void);

// Replace ESP-IDF's FatFs disk layer for this card with one that RETRIES
// (sdcard_retry.c). IDF's does not, and one disturbed transaction is
// otherwise permanent: FatFs marks the file object dead, leaks the
// cluster it had just allocated, and never repairs either. Called once,
// straight after mounting.
void sdcard_install_retrying_diskio(sdmmc_card_t* card);

// How many disk operations needed more than one attempt, how many ran out
// of attempts, and how many had to run on the bounce buffer because the
// caller's buffer was not one the SDMMC data phase can use. All zero on a
// healthy badge; if `retried` climbs the controller is being disturbed, if
// `failed` climbs the card itself is in trouble, and if `bounced` climbs
// malloc is handing the app buffers in a non-cacheable range.
void sdcard_retry_stats(uint32_t* retried, uint32_t* failed, uint32_t* bounced);
