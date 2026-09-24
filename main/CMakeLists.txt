set(display_srcs "")
if(CONFIG_SOC_MIPI_DSI_SUPPORTED)
    list(APPEND display_srcs "display_callbacks.c")
endif()

idf_component_register(
    SRCS
        ${display_srcs}
        "main.c"
        "sdcard.c"
        "volume_limit.c"
        "timezone.c"
    PRIV_REQUIRES
        fatfs
        nvs_flash
        kbelf
        appfs
        esp_lcd
        badge-bsp
        pax-codecs
        esp_driver_sdmmc
        sdmmc
        vfs
        esp_system
        heap
        gl_input
        esp_pm
        spi_flash
    INCLUDE_DIRS
        "."
)

# Force-include exported symbols so the linker doesn't strip them.
# These symbols are only referenced at runtime by the dynamically-loaded app.
#
# exported_symbols.cmake: manually curated --undefined flags (committed to git,
#   provides baseline symbol inclusion for clean builds)
# exported_symbols.ld: auto-generated EXTERN() directives from extract-symbols
#   (covers all symbols from component archives, uses linker script to avoid
#   command-line length limits)
include(${CMAKE_CURRENT_SOURCE_DIR}/exported_symbols.cmake OPTIONAL)
target_link_options(${COMPONENT_LIB} INTERFACE
    "-T${CMAKE_CURRENT_SOURCE_DIR}/exported_symbols.ld"
)

# Force-include libc/system functions the app.so needs but graceloader doesn't use directly
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--undefined=asprintf"
    "-Wl,--undefined=exit"
    "-Wl,--undefined=getenv"
    "-Wl,--undefined=vsnprintf"
    "-Wl,--undefined=graceloader_get_install_basepath"
)

# Audio volume limit for apps, see volume_limit.c. --wrap redirects every reference to these BSP functions
# (including the kbelf export table used by apps) to the __wrap_ versions. The export table only holds weak
# references, which don't pull objects out of an archive, so the wrappers are force-included explicitly;
# otherwise the table entries would silently resolve to address 0.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=bsp_audio_set_volume"
    "-Wl,--wrap=bsp_audio_get_volume"
    "-Wl,--undefined=__wrap_bsp_audio_set_volume"
    "-Wl,--undefined=__wrap_bsp_audio_get_volume"
)

# DPI panel event callbacks for apps, see display_callbacks.c. The BSP's registration is wrapped so graceloader's
# IRAM trampolines own the driver's callback slot and chain the BSP's callbacks with the app's. Force-included for
# the same reason as the volume wrappers above.
if(CONFIG_SOC_MIPI_DSI_SUPPORTED)
    target_link_options(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=esp_lcd_dpi_panel_register_event_callbacks"
        "-Wl,--undefined=__wrap_esp_lcd_dpi_panel_register_event_callbacks"
        "-Wl,--undefined=graceloader_display_register_callbacks"
    )
endif()

idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-const-variable" APPEND)
