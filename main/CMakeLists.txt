idf_build_get_property(idf_target IDF_TARGET)
if("${idf_target}" STREQUAL "esp32p4")
  list(APPEND extra_requires esp-hosted-tanmatsu)
endif()

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
        bt
        badge-bsp
        tanmatsu-wifi
        esp_driver_sdmmc
        sdmmc
        vfs
        esp_system
        heap
        ${extra_requires}
    INCLUDE_DIRS
        "."
)

# Force-include exported symbols so the linker doesn't strip them.
# These symbols are only referenced at runtime by the dynamically-loaded app.
include(${CMAKE_CURRENT_SOURCE_DIR}/exported_symbols.cmake OPTIONAL)

# Force-include libc functions the app.so needs but graceloader doesn't use directly
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--undefined=asprintf"
    "-Wl,--undefined=exit"
    "-Wl,--undefined=getenv"
    "-Wl,--undefined=vsnprintf"
    "-Wl,--undefined=graceloader_get_install_basepath"
)

idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-const-variable" APPEND)
