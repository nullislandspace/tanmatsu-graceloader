#!/bin/bash
# Generate kbelf symbol tables and fake .so libraries
# Run from main/ directory: bash symbol_export.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/symbol_export"

assembler="$(find ${IDF_TOOLS_PATH:-../../esp-idf-tools} -name riscv32-esp-elf-cc | head -1)"
if [ -z "$assembler" ]; then
    echo "ERROR: Could not find riscv32-esp-elf-cc assembler"
    echo "Make sure IDF_TOOLS_PATH is set or esp-idf-tools exists"
    exit 1
fi
echo "Assembler: $assembler"

# Ensure output directories exist
mkdir -p ../../output/fakelib

for libname in *; do
    [ -f "$libname" ] || continue
    echo "Exporting symbols in $libname"
    ident=$(echo "$libname" | sed 's/-/_/g')
    ../symbol_export.py \
        --symbols "$libname" \
        --kbelf "../../components/kbelf/src/kbelf_lib_${ident}.c" --kbelf-id "app_elf_lib_${ident}" --kbelf-path "lib${libname}.so" \
        --lib "../../output/fakelib/lib${libname}.so" \
        --assembler "$assembler" \
        -F=-march=rv32imafc_zicsr_zifencei_xesppie -F=-mabi=ilp32f
done

echo "Done. Generated files:"
echo "  components/kbelf/src/kbelf_lib_*.c  (symbol tables for graceloader)"
echo "  output/fakelib/lib*.so              (fake libraries for app template)"
