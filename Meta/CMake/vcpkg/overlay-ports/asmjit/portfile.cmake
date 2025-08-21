vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO asmjit/asmjit
    REF a3199e8857792cd10b7589ff5d58343d2c9008ea # commited on 2025-01-22
    SHA512 09c5285eac5e613f8289985bbf9134c25e138b1609aee4127fac978cba3af55dd73596fb12727e2a43f041feee8cacc01efc1995a719bfc5e78662fe3b0b0b97
    HEAD_REF master
)

string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "static" ASMJIT_STATIC)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DASMJIT_STATIC=${ASMJIT_STATIC}
 )

vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/asmjit)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/asmjit/core/api-config.h"
        "#if !defined(ASMJIT_STATIC)"
        "#if 0"
    )
endif()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.md")
