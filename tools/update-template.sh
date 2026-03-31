#!/bin/bash
# Update the template app with the latest fakelib and headers from the graceloader.
#
# This copies:
#   - output/fakelib/liball.so → <template>/fakelib/liball.so
#   - headers from the build   → <template>/include/
#
# Only headers from components that apps actually use are exported.
# This ensures apps get a compile error (not a runtime error) if they
# try to use an API the graceloader doesn't provide.
#
# The template app path defaults to ../tanmatsu-template-grace (sibling directory).
# Override with: TEMPLATE_PATH=/path/to/template make update-template

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

TEMPLATE_PATH="${TEMPLATE_PATH:-$PROJECT_DIR/../tanmatsu-template-grace}"
DEVICE="${DEVICE:-tanmatsu}"
BUILD_DIR="$PROJECT_DIR/build/$DEVICE"
IDF_PATH="${IDF_PATH:-$PROJECT_DIR/esp-idf}"

if [ ! -d "$TEMPLATE_PATH" ]; then
    echo "ERROR: Template app not found at $TEMPLATE_PATH"
    echo "Set TEMPLATE_PATH to the tanmatsu-template-grace directory"
    exit 1
fi

TEMPLATE_PATH="$(cd "$TEMPLATE_PATH" && pwd)"
echo "Updating template at: $TEMPLATE_PATH"

# --- Copy fakelib ---
FAKELIB_SRC="$PROJECT_DIR/output/fakelib"
FAKELIB_DST="$TEMPLATE_PATH/fakelib"

if [ ! -d "$FAKELIB_SRC" ]; then
    echo "ERROR: $FAKELIB_SRC not found"
    echo "Run 'make regenerate-symbols' first"
    exit 1
fi

mkdir -p "$FAKELIB_DST"
cp "$FAKELIB_SRC"/*.so "$FAKELIB_DST/"
echo "  Copied fakelib"

# --- Extract and copy headers ---
INCLUDE_DST="$TEMPLATE_PATH/include"

if [ ! -f "$BUILD_DIR/compile_commands.json" ]; then
    echo "ERROR: compile_commands.json not found in $BUILD_DIR"
    echo "Build the graceloader first: make build"
    exit 1
fi

echo "  Extracting headers..."
rm -rf "$INCLUDE_DST"
mkdir -p "$INCLUDE_DST"

# Components whose headers should be exported to apps.
# Matches the APIs the graceloader actually provides symbols for.
#
# Categories:
#   - Exported component APIs (BSP, PAX, WiFi, NVS, coprocessor)
#   - ESP-IDF platform essentials (libc, FreeRTOS, logging, types)
#   - Drivers apps use directly (gpio, i2s, i2c, spi, sdmmc, ledc)
#   - Filesystem (vfs, fatfs — graceloader mounts, apps read/write)
#   - Display types (esp_lcd — used in BSP display signatures)
#   - Timer, heap, event (common app utilities)
HEADER_COMPONENTS=(
    # Exported component APIs
    badgeteam__badge-bsp
    badgeteam__custom-certificates
    kbelf
    robotman2412__pax-gfx
    robotman2412__pax-codecs
    nicolaielectronics__tanmatsu_coprocessor
    nvs_flash

    # ESP-IDF platform essentials
    newlib
    freertos
    esp_common
    esp_system
    log
    hal
    soc
    esp_rom
    riscv
    esp_hw_support
    heap
    esp_timer
    esp_event
    pthread

    # Drivers apps use directly
    driver
    esp_driver_gpio
    esp_driver_i2s
    esp_driver_i2c
    esp_driver_spi
    esp_driver_ledc
    esp_driver_sdmmc
    esp_driver_gptimer
    esp_driver_uart
    sdmmc

    # Filesystem
    vfs
    fatfs
    wear_levelling
    esp_partition
    spi_flash

    # Display types (needed by BSP display API signatures)
    esp_lcd

    # Other utilities apps use
    esp_ringbuf
    esp_pm
)

# Extract all -I include paths from compile_commands.json
ALL_DIRS=$(grep -oP '(?<=-I)[^ "]+' "$BUILD_DIR/compile_commands.json" | sort -u)

# Filter to only directories belonging to allowed components
dir_count=0
for dir in $ALL_DIRS; do
    if [ ! -d "$dir" ]; then
        continue
    fi

    # Check if this directory belongs to an allowed component
    allowed=false
    for comp in "${HEADER_COMPONENTS[@]}"; do
        if echo "$dir" | grep -qE "(components|managed_components)/(${comp})[/]?" ; then
            allowed=true
            break
        fi
    done

    # Also allow the build config directory (sdkconfig.h etc.)
    if echo "$dir" | grep -q "/build/.*/config$"; then
        allowed=true
    fi

    if [ "$allowed" = false ]; then
        continue
    fi

    (cd "$dir" && find . -type f \( -name "*.h" -o -name "*.inc" -o -name "*.hpp" \) -print0) | \
    while IFS= read -r -d '' file; do
        dest_dir="$INCLUDE_DST/$(dirname "$file")"
        mkdir -p "$dest_dir"
        cp "$dir/$file" "$dest_dir/"
    done
    dir_count=$((dir_count + 1))
done

echo "  Processed $dir_count include directories (from ${#HEADER_COMPONENTS[@]} allowed components)"

# Copy sdkconfig.h from the build
if [ -f "$BUILD_DIR/config/sdkconfig.h" ]; then
    cp "$BUILD_DIR/config/sdkconfig.h" "$INCLUDE_DST/"
    echo "  Copied sdkconfig.h"
fi

# Copy graceloader's own exported header
cp "$PROJECT_DIR/main/graceloader.h" "$INCLUDE_DST/"
echo "  Copied graceloader.h"

header_count=$(find "$INCLUDE_DST" -type f \( -name "*.h" -o -name "*.inc" \) | wc -l)
echo ""
echo "Done. Updated template with:"
echo "  - fakelib/*.so"
echo "  - $header_count header files (including graceloader.h)"
