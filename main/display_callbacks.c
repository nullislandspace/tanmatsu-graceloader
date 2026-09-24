// SPDX-License-Identifier: MIT
// MIPI DPI panel event callbacks for dynamically-loaded apps
//
// graceloader is built with CONFIG_LCD_DSI_ISR_CACHE_SAFE: the DSI interrupts keep running while the cache is
// disabled (flash writes), so the panel refresh never stalls. The price is that the DPI driver only accepts event
// callbacks placed in IRAM, and an app's code lives in PSRAM, so an app cannot register its own callbacks. The BSP
// also already owns the one callback slot the driver has: its on_color_trans_done releases the semaphore that
// bsp_display_blit() waits on.
//
// Mechanism: main/CMakeLists.txt links with -Wl,--wrap=esp_lcd_dpi_panel_register_event_callbacks, so the BSP's
// registration lands in __wrap_ below. Its callbacks are remembered as the "primary" set and graceloader registers
// its own IRAM trampolines with the driver instead. Each trampoline calls the primary callback, then the app's
// callback from graceloader_display_register_callbacks() if one is set. The app's callback is skipped while the
// cache is disabled, because its code could not be fetched then. The on_refresh_done trampoline is only
// registered while someone wants it, as it enables a per-frame interrupt: an app that never registers a callback
// gets exactly the driver setup it had before.

#include <stdbool.h>
#include <stddef.h>
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_private/cache_utils.h"
#include "freertos/FreeRTOS.h"
#include "graceloader.h"
#include "sdkconfig.h"

static char const TAG[] = "display_callbacks";

typedef struct {
    esp_lcd_dpi_panel_color_trans_done_cb_t on_color_trans_done;
    esp_lcd_dpi_panel_refresh_done_cb_t     on_refresh_done;
    void*                                   user_ctx;
} callback_set_t;

esp_err_t __real_esp_lcd_dpi_panel_register_event_callbacks(esp_lcd_panel_handle_t                     panel,
                                                            esp_lcd_dpi_panel_event_callbacks_t const* cbs,
                                                            void*                                      user_ctx);

// Read by the trampolines in interrupt context, possibly with the cache disabled: keep in DRAM.
static DRAM_ATTR portMUX_TYPE           callbacks_lock = portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR callback_set_t         primary_callbacks;  // the BSP's (IRAM-safe, checked on registration)
static DRAM_ATTR callback_set_t         app_callbacks;      // the app's (PSRAM, only called with the cache on)
static DRAM_ATTR esp_lcd_panel_handle_t dpi_panel = NULL;

static IRAM_ATTR bool call_both(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t* edata, bool refresh) {
    portENTER_CRITICAL_ISR(&callbacks_lock);
    esp_lcd_dpi_panel_general_cb_t primary_cb =
        refresh ? primary_callbacks.on_refresh_done : primary_callbacks.on_color_trans_done;
    void*                          primary_ctx = primary_callbacks.user_ctx;
    esp_lcd_dpi_panel_general_cb_t app_cb = refresh ? app_callbacks.on_refresh_done : app_callbacks.on_color_trans_done;
    void*                          app_ctx = app_callbacks.user_ctx;
    portEXIT_CRITICAL_ISR(&callbacks_lock);

    bool need_yield = false;
    if (primary_cb != NULL && primary_cb(panel, edata, primary_ctx)) {
        need_yield = true;
    }
    if (app_cb != NULL && spi_flash_cache_enabled() && app_cb(panel, edata, app_ctx)) {
        need_yield = true;
    }
    return need_yield;
}

static IRAM_ATTR bool trampoline_color_trans_done(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t* edata,
                                                  void* user_ctx) {
    (void)user_ctx;
    return call_both(panel, edata, false);
}

static IRAM_ATTR bool trampoline_refresh_done(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t* edata,
                                              void* user_ctx) {
    (void)user_ctx;
    return call_both(panel, edata, true);
}

// Register the trampolines that are currently needed with the driver.
static esp_err_t apply_callbacks(void) {
    if (dpi_panel == NULL) {
        return ESP_OK;  // display not initialized yet; applied when the BSP registers
    }
    esp_lcd_dpi_panel_event_callbacks_t trampolines = {
        .on_color_trans_done = (primary_callbacks.on_color_trans_done != NULL || app_callbacks.on_color_trans_done != NULL)
                                   ? trampoline_color_trans_done
                                   : NULL,
        .on_refresh_done = (primary_callbacks.on_refresh_done != NULL || app_callbacks.on_refresh_done != NULL)
                               ? trampoline_refresh_done
                               : NULL,
    };
    return __real_esp_lcd_dpi_panel_register_event_callbacks(dpi_panel, &trampolines, NULL);
}

esp_err_t __wrap_esp_lcd_dpi_panel_register_event_callbacks(esp_lcd_panel_handle_t                     panel,
                                                            esp_lcd_dpi_panel_event_callbacks_t const* cbs,
                                                            void*                                      user_ctx) {
    if (panel == NULL || cbs == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_LCD_DSI_ISR_CACHE_SAFE
    // The same checks the driver makes: these callbacks are called with the cache disabled too.
    if ((cbs->on_color_trans_done != NULL && !esp_ptr_in_iram(cbs->on_color_trans_done)) ||
        (cbs->on_refresh_done != NULL && !esp_ptr_in_iram(cbs->on_refresh_done)) ||
        (user_ctx != NULL && !esp_ptr_internal(user_ctx))) {
        ESP_LOGE(TAG, "DPI panel callbacks and their context must be in internal RAM");
        return ESP_ERR_INVALID_ARG;
    }
#endif

    portENTER_CRITICAL(&callbacks_lock);
    dpi_panel                             = panel;
    primary_callbacks.on_color_trans_done = cbs->on_color_trans_done;
    primary_callbacks.on_refresh_done     = cbs->on_refresh_done;
    primary_callbacks.user_ctx            = user_ctx;
    portEXIT_CRITICAL(&callbacks_lock);

    return apply_callbacks();
}

esp_err_t graceloader_display_register_callbacks(esp_lcd_dpi_panel_event_callbacks_t const* cbs, void* user_ctx) {
    portENTER_CRITICAL(&callbacks_lock);
    app_callbacks.on_color_trans_done = cbs != NULL ? cbs->on_color_trans_done : NULL;
    app_callbacks.on_refresh_done     = cbs != NULL ? cbs->on_refresh_done : NULL;
    app_callbacks.user_ctx            = cbs != NULL ? user_ctx : NULL;
    portEXIT_CRITICAL(&callbacks_lock);

    return apply_callbacks();
}
