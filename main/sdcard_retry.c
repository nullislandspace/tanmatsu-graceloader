// =====================================================================
//  A retrying disk layer for the SD card
// ---------------------------------------------------------------------
//  ESP-IDF's own FatFs disk driver does not retry. One transaction that
//  does not complete is the end of it:
//
//      esp_err_t err = sdmmc_write_sectors(card, buff, sector, count);
//      if (unlikely(err != ESP_OK)) { ESP_LOGE(...); return RES_ERROR; }
//
//  and the consequences inside FatFs are out of all proportion to one
//  glitch, because FatFs has no journal and no repair:
//
//    * f_write does ABORT(fs, FR_DISK_ERR), which sets fp->err -- and
//      every later f_read and f_write on that file object returns
//      immediately with the stored error, for ever, until the file is
//      closed and opened again.
//    * a cluster allocated by create_chain() before the failure stays
//      marked in use while the directory entry that would reference it
//      is never written. The space is LOST -- allocated, referenced by
//      nothing, and only a host-side fsck will ever reclaim it.
//      Measured on this badge: 45 lost clusters, 720 KB, in six chains.
//    * sync_window() mirrors the FAT to the second copy and DISCARDS
//      that write's result, so the two copies can silently diverge.
//
//  WHY A RETRY IS THE RIGHT ANSWER, and not merely a bandage: the card
//  is not the thing that failed. The driver reads the card's status
//  (CMD13) immediately after a failed transaction and logs it, and on
//  this badge it comes back 0x900 every time -- READY_FOR_DATA set,
//  state 4, `tran`. The card is idle and well a microsecond after
//  "timing out", and the write timeout is five seconds to begin with,
//  so this is not a card busy with an erase cycle. It is the
//  transaction that was disturbed, and the obvious suspect is that the
//  ESP32-P4 has ONE SDMMC controller which the card shares with
//  ESP-Hosted (see sdcard.c, and the workaround it already needs).
//
//  Writing the same sectors again is idempotent, and the card has just
//  told us it is ready, so trying again is safe and nearly always
//  works. What it buys is that a glitch stays a glitch instead of
//  becoming a dead file handle, a leaked cluster and, one layer up, a
//  world that stops streaming.
//
//  This does NOT fork ESP-IDF. ff_diskio_register() is public API and
//  the whole disk layer is swappable, so the retry lives here and the
//  IDF component stays untouched.
// =====================================================================

#include "sdcard.h"

#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "esp_log.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

static char const* TAG = "sdretry";

// ESCALATING, because the cause is not pinned down and the two
// candidates want opposite things.
//
// ESP_ERR_TIMEOUT arrives here from three different hardware
// interrupts, and the driver does not say which at default log level
// (the line that would, "error 0x%x (status=%08x)" in
// process_cmd_response_status, is ESP_LOGD):
//
//   RTO  the card never answered the command
//   DTO  the data phase ran past the controller's timeout counter
//   EBE  the card's CRC-status token came back malformed on a write
//
// DTO is a card that is genuinely busy -- finishing a program or erase
// cycle, which on a cheap card is 100-250 ms and on a bad one more. A
// retry 5 ms later would land in the middle of the same cycle and fail
// again, and four of those would burn the whole allowance in 20 ms and
// give up while the card was still working. RTO and EBE are the
// opposite: signalling, where the fast retry is the one that works and
// waiting achieves nothing.
//
// So: fast first, then long enough to outlast a program cycle. The
// first retry is 2 ms and costs nothing; the last leaves a third of a
// second of quiet, which is past the point where a healthy card is
// still thinking.
static uint16_t const SD_BACKOFF_MS[] = {0, 2, 20, 100, 250};
#define SD_TRIES ((int)(sizeof SD_BACKOFF_MS / sizeof SD_BACKOFF_MS[0]))

static sdmmc_card_t* s_card;

// How long the backoff had spent waiting by attempt `try`. Printed with
// a success, because "worked on attempt 4, after 122 ms" says the card
// needed the time, and "attempt 2, after 2 ms" says it did not -- which
// is the difference between DTO and RTO/EBE without needing debug
// logging turned on.
static unsigned waited_ms(int try) {
    unsigned ms = 0;
    for (int i = 0; i <= try && i < SD_TRIES; i++) ms += SD_BACKOFF_MS[i];
    return ms;
}

// What it cost, so a card or a controller that is genuinely unwell
// cannot hide behind the retries.
static uint32_t s_retried;   // operations that needed more than one go
static uint32_t s_failed;    // ... and the ones that ran out of goes

static DRESULT sd_read(BYTE pdrv, BYTE* buff, DWORD sector, UINT count) {
    (void)pdrv;
    esp_err_t err = ESP_OK;
    for (int try = 0; try < SD_TRIES; try++) {
        if (SD_BACKOFF_MS[try] > 0) vTaskDelay(pdMS_TO_TICKS(SD_BACKOFF_MS[try]));
        err = sdmmc_read_sectors(s_card, buff, sector, count);
        if (err == ESP_OK) {
            if (try > 0) {
                s_retried++;
                // Which attempt worked is the diagnosis: attempt 2 is a
                // glitch, attempt 4 or 5 is a card that needed the time.
                ESP_LOGW(TAG, "read of sector %" PRIu32 " succeeded on attempt %d (after %u ms of waiting)",
                         (uint32_t)sector, try + 1, (unsigned)waited_ms(try));
            }
            return RES_OK;
        }
    }
    s_failed++;
    ESP_LOGE(TAG, "read of %u sector(s) at %" PRIu32 " failed %d times (0x%x)", (unsigned)count, (uint32_t)sector,
             SD_TRIES, err);
    return RES_ERROR;
}

static DRESULT sd_write(BYTE pdrv, BYTE const* buff, DWORD sector, UINT count) {
    (void)pdrv;
    esp_err_t err = ESP_OK;
    for (int try = 0; try < SD_TRIES; try++) {
        if (SD_BACKOFF_MS[try] > 0) vTaskDelay(pdMS_TO_TICKS(SD_BACKOFF_MS[try]));
        // Idempotent: the same bytes to the same sectors. A partially
        // written multi-sector run is simply written again from the
        // start.
        err = sdmmc_write_sectors(s_card, buff, sector, count);
        if (err == ESP_OK) {
            if (try > 0) {
                s_retried++;
                ESP_LOGW(TAG, "write of sector %" PRIu32 " succeeded on attempt %d (after %u ms of waiting)",
                         (uint32_t)sector, try + 1, (unsigned)waited_ms(try));
            }
            return RES_OK;
        }
    }
    s_failed++;
    ESP_LOGE(TAG, "write of %u sector(s) at %" PRIu32 " failed %d times (0x%x) -- FatFs will leak the cluster",
             (unsigned)count, (uint32_t)sector, SD_TRIES, err);
    return RES_ERROR;
}

static DSTATUS sd_status(BYTE pdrv) {
    (void)pdrv;
    return s_card != NULL ? 0 : STA_NOINIT;
}

static DSTATUS sd_initialize(BYTE pdrv) {
    return sd_status(pdrv);
}

// The same answers ESP-IDF's ff_sdmmc_ioctl gives. TRIM is left out on
// purpose: FF_USE_TRIM is off in this build, and a discard that fails
// half way is not something to retry blindly.
static DRESULT sd_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    (void)pdrv;
    if (s_card == NULL) return RES_ERROR;
    switch (cmd) {
        case CTRL_SYNC: return RES_OK;
        case GET_SECTOR_COUNT: *((DWORD*)buff) = (DWORD)s_card->csd.capacity; return RES_OK;
        case GET_SECTOR_SIZE: *((WORD*)buff) = (WORD)s_card->csd.sector_size; return RES_OK;
        case GET_BLOCK_SIZE: return RES_ERROR;  // as IDF's does
        default: break;
    }
    return RES_ERROR;
}

void sdcard_install_retrying_diskio(sdmmc_card_t* card) {
    if (card == NULL) return;
    s_card = card;

    BYTE const pdrv = ff_diskio_get_pdrv_card(card);
    if (pdrv == 0xFF) {
        ESP_LOGE(TAG, "the card has no drive number; leaving ESP-IDF's disk layer in place");
        return;
    }

    static ff_diskio_impl_t const impl = {
        .init   = &sd_initialize,
        .status = &sd_status,
        .read   = &sd_read,
        .write  = &sd_write,
        .ioctl  = &sd_ioctl,
    };
    ff_diskio_register(pdrv, &impl);
    ESP_LOGI(TAG, "retrying disk layer installed on pdrv %u (%d tries, up to %u ms of backoff)", (unsigned)pdrv,
             SD_TRIES, waited_ms(SD_TRIES - 1));
}

void sdcard_retry_stats(uint32_t* retried, uint32_t* failed) {
    if (retried != NULL) *retried = s_retried;
    if (failed != NULL) *failed = s_failed;
}
