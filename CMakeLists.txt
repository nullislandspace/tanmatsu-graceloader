cmake_minimum_required(VERSION 3.10)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(application)

# badge-bsp is overridden by components/badgeteam__badge-bsp, a checkout of
# https://github.com/nullislandspace/esp32-component-badge-bsp pinned by submodule
# commit. The version this project builds against is declared here, not in the
# component's own idf_component.yml.
set(BADGE_BSP_VERSION "1.6.4")
message(STATUS "badge-bsp override: ${BADGE_BSP_VERSION} (components/badgeteam__badge-bsp)")

# The vendored zlib (pulled in by robotman2412/pax-codecs) uses pre-C99
# K&R style function definitions, which newer GCC versions reject under
# -Werror=old-style-definition. Silence just that warning for zlib.
foreach(zlib_target zlib zlibstatic)
    if(TARGET ${zlib_target})
        target_compile_options(${zlib_target} PRIVATE
            -Wno-error=old-style-definition
            -Wno-error=implicit-fallthrough
        )
    endif()
endforeach()
