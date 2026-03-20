idf_component_register(
    SRCS
        "main.c"
        "sdcard.c"
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

idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-const-variable" APPEND)
