# NOTE: REF must point at a tag/commit that contains the ENABLE_TESTS option
# (so we can build lib-only and skip the test/testing submodule). Bump REF and
# SHA512 together on each release; set SHA512 to 0 once to let vcpkg print the
# real value, then paste it back.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO kevinkreiser/prime_server
    REF "${VERSION}"
    SHA512 0
    HEAD_REF master
)

# src/logging is a git submodule and GitHub's source archive does not include
# submodule contents, so fetch the pinned logging header and drop it into place.
# REF is the gitlink commit recorded for src/logging at the release above.
vcpkg_from_git(
    OUT_SOURCE_PATH LOGGING_SOURCE_PATH
    URL https://gist.github.com/39f2e39273c625d96790.git
    REF bf5579771878d9513a4b6992f12677b2e140ba67
)
file(COPY "${LOGGING_SOURCE_PATH}/" DESTINATION "${SOURCE_PATH}/src/logging")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DENABLE_WERROR=OFF
        -DENABLE_TESTS=OFF
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(PACKAGE_NAME libprime_server CONFIG_PATH lib/cmake/libprime_server)

vcpkg_fixup_pkgconfig()

# the standalone daemons are tools, not part of the library consumers link against
vcpkg_copy_tools(
    TOOL_NAMES prime_echod prime_filed prime_httpd prime_serverd prime_proxyd prime_workerd
    AUTO_CLEAN
)

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
