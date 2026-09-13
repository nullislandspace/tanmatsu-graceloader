#!/bin/bash
# Extract symbols for the graceloader's dynamic app loading system.
#
# This script produces two outputs, in two separate steps:
#
# 1. main/exported_symbols.ld — EXTERN() directives for every global symbol
#    defined in the component archives listed below. These prevent
#    --gc-sections from stripping symbols that only apps reference. Uses a
#    linker script to avoid command-line length limits.
#
#    The list is derived from the archives ONLY, never from the current ELF.
#    Subtracting the symbols already present in the ELF made the output
#    depend on the previous run's linker script: everything that script had
#    forced in counted as "already linked", dropped out of the new list, and
#    got stripped again on the next build, so the exported set flip-flopped
#    between runs. EXTERN() on a symbol that would be linked anyway is a no-op.
#
# 2. main/symbol_export/all — the complete symbol list for kbelf tables and
#    the fakelib, extracted from the ELF built with the linker script above,
#    so it only contains symbols that actually link.
#
# All output files are sorted alphabetically for minimal diffs.
#
# Usage:
#   extract-symbols.sh              — generate exported_symbols.ld
#   extract-symbols.sh --all        — generate symbol_export/all from the ELF
#   extract-symbols.sh --check      — verify the ELF exports exactly the
#                                     symbols in symbol_export/all
#
# Prerequisites: the graceloader must be built first (make build).

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

DEVICE="${DEVICE:-tanmatsu}"
BUILD_DIR="$PROJECT_DIR/build/$DEVICE"
SYMBOL_LIST="$PROJECT_DIR/main/symbol_export/all"
LD_FILE="$PROJECT_DIR/main/exported_symbols.ld"

MODE="${1:-ld}"
case "$MODE" in
    ld|--all|--check) ;;
    *)
        echo "Usage: $0 [--all|--check]"
        exit 1
        ;;
esac

# Component libraries whose symbols should be force-included for apps.
# Only list high-level app-facing APIs here. Low-level components
# (freertos, heap, hal, esp_driver_*, etc.) are already linked into
# the ELF naturally and their symbols are captured from the ELF
# extraction — no EXTERN forcing needed. Individual symbols from other
# components go into main/exported_symbols.cmake instead (e.g. the
# standard mode I2S API; esp_driver_i2s as a whole would drag in the PDM,
# TDM, ETM and LP I2S drivers, which are of no use on this hardware).
#
# Adding too many components here bloats the binary (kbelf symbol table
# + forced archive objects). The 2MB partition is the hard limit.
COMPONENTS=(
    badgeteam__badge-bsp
    robotman2412__pax-gfx
    robotman2412__pax-codecs
    nicolaielectronics__tanmatsu_coprocessor
    nvs_flash
    gl_input
)

# Find nm from the IDF toolchain
IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$PROJECT_DIR/esp-idf-tools}"
NM="$(find "$IDF_TOOLS_PATH" -name "riscv32-esp-elf-nm" -type f | head -1)"

if [ -z "$NM" ]; then
    echo "ERROR: Could not find riscv32-esp-elf-nm"
    echo "Make sure IDF_TOOLS_PATH is set or esp-idf-tools exists"
    exit 1
fi

if [ ! -d "$BUILD_DIR/esp-idf" ]; then
    echo "ERROR: Build directory not found at $BUILD_DIR"
    echo "Build the graceloader first: make build"
    exit 1
fi

ELF="$BUILD_DIR/application.elf"

TLS_SYMS=$(mktemp)
ELF_SYMS=$(mktemp)
trap 'rm -f "$TLS_SYMS" "$ELF_SYMS"' EXIT

# TLS symbols (e.g. errno under PicoLibC in ESP-IDF 6) cannot be exported:
# referencing them as plain symbols is a link error ("TLS definition ...
# mismatches non-TLS reference"). Build a blacklist to filter them out.
READELF="$(dirname "$NM")/riscv32-esp-elf-readelf"
: > "$TLS_SYMS"
if [ -x "$READELF" ]; then
    # Scan both the linked ELF (when present) and every component archive,
    # so the blacklist is complete even if the ELF failed to link.
    { [ -f "$ELF" ] && "$READELF" -sW "$ELF" 2>/dev/null
      find "$BUILD_DIR" -name '*.a' -exec "$READELF" -sW {} \; 2>/dev/null
      # PicoLibC (ESP-IDF 6) keeps errno/stdio/localtime buffers in TLS
      find "$(dirname "$NM")/../picolibc" -name '*.a' -exec "$READELF" -sW {} \; 2>/dev/null
    } | awk '$4 == "TLS" {print $8}' \
      | grep '^[A-Za-z_][A-Za-z0-9_]*$' | sort -u > "$TLS_SYMS"
    echo "  Excluding $(wc -l < "$TLS_SYMS") TLS symbols"
fi

# Global defined symbols of the ELF, filtered to valid C identifiers only
# (letters, digits, underscores). This excludes compiler internals like
# DW.ref.* that can't be used as variable names in the kbelf symbol table.
extract_elf_symbols() {
    if [ ! -f "$ELF" ]; then
        echo "ERROR: application.elf not found"
        exit 1
    fi
    "$NM" -g --defined-only "$ELF" | awk '{print $3}' | grep '^[A-Za-z_][A-Za-z0-9_]*$' | grep -vxF -f "$TLS_SYMS" | sort -u > "$ELF_SYMS"
}

case "$MODE" in
    ld)
        echo "Extracting archive symbols for exported_symbols.ld..."
        echo "Using nm: $NM"

        ARCHIVE_SYMS=$(mktemp)
        trap 'rm -f "$TLS_SYMS" "$ELF_SYMS" "$ARCHIVE_SYMS"' EXIT

        for comp in "${COMPONENTS[@]}"; do
            archive="$BUILD_DIR/esp-idf/$comp/lib${comp}.a"
            if [ ! -f "$archive" ]; then
                echo "ERROR: $archive not found"
                exit 1
            fi
            echo "  Extracting from: $comp"
            "$NM" -g --defined-only "$archive" | awk '{print $3}' >> "$ARCHIVE_SYMS"
        done

        echo "Generating main/exported_symbols.ld..."
        {
            echo "/* AUTO-GENERATED by tools/extract-symbols.sh -- do not edit */"
            echo "/* EXTERN() prevents --gc-sections from stripping symbols only used by apps */"
            grep '^[A-Za-z_][A-Za-z0-9_]*$' "$ARCHIVE_SYMS" | grep -vxF -f "$TLS_SYMS" | sort -u | sed 's/.*/EXTERN(&)/'
        } > "$LD_FILE"
        echo "Wrote $(grep -c '^EXTERN(' "$LD_FILE") EXTERN() directives to main/exported_symbols.ld"
        ;;

    --all)
        echo "Generating main/symbol_export/all from ELF..."
        extract_elf_symbols
        cp "$ELF_SYMS" "$SYMBOL_LIST"
        echo "Wrote $(wc -l < "$SYMBOL_LIST") symbols to main/symbol_export/all"
        ;;

    --check)
        echo "Checking application.elf against main/symbol_export/all..."
        extract_elf_symbols
        if ! diff -q "$SYMBOL_LIST" "$ELF_SYMS" > /dev/null; then
            echo "ERROR: application.elf does not export exactly the symbols in main/symbol_export/all:"
            diff "$SYMBOL_LIST" "$ELF_SYMS" | grep '^[<>]' | head -20
            exit 1
        fi
        echo "OK: $(wc -l < "$SYMBOL_LIST") symbols match"
        ;;
esac
