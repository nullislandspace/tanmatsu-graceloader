# ESP-IDF component wrapper for KiloElfLoader (submodule at lib/kbelf)
# Adds graceloader-specific glue: platform callbacks, fastopen, symbol tables

idf_build_get_property(idf_arch IDF_TARGET_ARCH)
if("${idf_arch}" STREQUAL "riscv")

set(KBELF_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/kbelf/src")

# Collect all generated kbelf_lib_*.c files
file(GLOB KBELF_LIB_SRCS "src/kbelf_lib_*.c")

idf_component_register(
    SRCS
        "${KBELF_SRC}/kbelf.c"
        "${KBELF_SRC}/kbelf_dyn.c"
        "${KBELF_SRC}/kbelf_file.c"
        "${KBELF_SRC}/kbelf_inst.c"
        "${KBELF_SRC}/kbelf_reloc.c"
        "${KBELF_SRC}/port/riscv.c"
        "src/kbelfx.c"
        "src/fastopen.c"
        ${KBELF_LIB_SRCS}
    INCLUDE_DIRS
        "../../lib/kbelf/include"
        "src"
    REQUIRES
        freertos
    PRIV_REQUIRES
        esp_hw_support
        heap
)
# Uncomment to log all missing symbols instead of stopping on the first one:
target_compile_definitions(${COMPONENT_LIB} PRIVATE KBELF_CONTINUE_ON_MISSING_SYMBOLS)

endif()
