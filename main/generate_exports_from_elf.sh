#!/bin/bash
# Generate a kbelf symbol export table from the graceloader ELF binary.
# This exports ALL symbols the graceloader has, so the app.so can use them.
# Weak references mean zero cost — only symbols that exist get exported.
set -e

ELF="${1:?Usage: generate_exports_from_elf.sh <graceloader.elf> <output.c> <lib_id> <lib_path>}"
OUTPUT="${2:?}"
LIB_ID="${3:?}"
LIB_PATH="${4:?}"

# Extract all global defined symbols (T=text, D=data, B=bss, R=rodata, A=absolute)
SYMBOLS=$(riscv32-esp-elf-nm "$ELF" 2>/dev/null | grep " [TDBRA] " | awk '{print $3}' | sort -u | grep -v '^\.' | grep -v '^\$')

COUNT=$(echo "$SYMBOLS" | wc -l)
echo "Generating export table with $COUNT symbols from $(basename $ELF)"

cat > "$OUTPUT" << 'HEADER'
// WARNING: Auto-generated from graceloader ELF - do not edit!
// Exports all graceloader symbols so app.so can resolve them via kbelf.
// clang-format off

#include <kbelf.h>

HEADER

# Write weak extern declarations
for sym in $SYMBOLS; do
    echo "extern char const __attribute__((weak)) sym_${sym}[] asm(\"${sym}\");" >> "$OUTPUT"
done

echo "" >> "$OUTPUT"
echo "static kbelf_builtin_sym const symbols[] = {" >> "$OUTPUT"
for sym in $SYMBOLS; do
    echo "    { .name = \"${sym}\", .vaddr = (size_t) sym_${sym} }," >> "$OUTPUT"
done
echo "};" >> "$OUTPUT"

cat >> "$OUTPUT" << EOF

kbelf_builtin_lib const ${LIB_ID} = {
    .path        = "${LIB_PATH}",
    .symbols_len = ${COUNT},
    .symbols     = symbols,
};
EOF

echo "Generated $OUTPUT ($COUNT symbols)"
