cmake_minimum_required(VERSION 3.16)
project(ranarch LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# Position-independent code for the shared library
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ----------------------------------------------------------------------------
# Dependencies (pkg-config)
# ----------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBARCHURE  REQUIRED IMPORTED_TARGET libarchive)
pkg_check_modules(SQLITE3     REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(GPGME      REQUIRED IMPORTED_TARGET gpgme)
pkg_check_modules(LIBALPM     REQUIRED IMPORTED_TARGET libalpm)

# spdlog and nlohmann-json are deliberately optional: the logger is
# self-contained and JSON IPC is handled by a vendored header (src/core/json.h),
# so the project builds on a minimal Arch base without those packages.
find_path(NLOHMANN_JSON_INCLUDE nlohmann/json.hpp PATH_SUFFIXES nlohmann)

# Polkit (optional, only needed by daemon)
pkg_check_modules(POLKIT_GOBJECT polkit-gobject-1)

# ----------------------------------------------------------------------------
# Library: libranarch.so
# ----------------------------------------------------------------------------
add_library(ranarch SHARED
    src/core/error.cpp
    src/core/config.cpp
    src/core/logger.cpp
    src/core/json.cpp
    src/core/db.cpp
    src/core/deb_parser.cpp
    src/core/rpm_parser.cpp
    src/core/dep_map.cpp
    src/core/dep_resolver.cpp
    src/core/alpm_bridge.cpp
    src/core/conflict_checker.cpp
    src/core/keyring.cpp
    src/core/sig_verifier.cpp
    src/core/sandbox.cpp
    src/core/pty_runner.cpp
    src/core/extractor.cpp
    src/core/installer.cpp
    src/daemon/session.cpp
    src/daemon/ipc_server.cpp
    src/daemon/authorize.cpp
)
set_target_properties(ranarch PROPERTIES
    OUTPUT_NAME ranarch
    VERSION ${PROJECT_VERSION}
    SOVERSION 0)
# NOTE: symbols are exported by default. A future refinement should introduce
# a RANARCH_API export macro and switch to CXX_VISIBILITY_PRESET hidden so the
# shared library only exposes its intended surface.
target_include_directories(ranarch
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<$<BOOL:${NLOHMANN_JSON_INCLUDE}>:${NLOHMANN_JSON_INCLUDE}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        $<TARGET_PROPERTY:PkgConfig::LIBARCHURE,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:PkgConfig::SQLITE3,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:PkgConfig::GPGME,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:PkgConfig::LIBALPM,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(ranarch
    PRIVATE
        PkgConfig::LIBARCHURE
        PkgConfig::SQLITE3
        PkgConfig::GPGME
        PkgConfig::LIBALPM
        util)  # forkpty lives in libutil
if(POLKIT_GOBJECT_FOUND)
    # 特权操作前的授权检查（安装 / 卸载 / 管理密钥）在 src/daemon/authorize.cpp 里，
    # 它属于本库（ipc_server.cpp 也在这里），所以探测结果挂到库目标上。
    # 没找到 polkit 时该文件会编成「一律拒绝」的版本。
    target_include_directories(ranarch PRIVATE ${POLKIT_GOBJECT_INCLUDE_DIRS})
    target_link_libraries(ranarch PRIVATE ${POLKIT_GOBJECT_LIBRARIES})
    target_compile_definitions(ranarch PRIVATE RANARCH_HAVE_POLKIT=1)
    message(STATUS "polkit-gobject-1 found: daemon will request authorization for privileged ops")
else()
    message(WARNING "polkit-gobject-1 not found: privileged ops will be refused (no authorization possible)")
endif()

# ----------------------------------------------------------------------------
# Daemon: ranarch-daemon
# ----------------------------------------------------------------------------
add_executable(ranarch-daemon src/daemon/daemon.cpp)
target_include_directories(ranarch-daemon PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ranarch-daemon PRIVATE ranarch)

# ----------------------------------------------------------------------------
# CLI: ranarchctl
# ----------------------------------------------------------------------------
add_executable(ranarchctl src/cli/ranarchctl.cpp)
target_include_directories(ranarchctl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ranarchctl PRIVATE ranarch)

# ----------------------------------------------------------------------------
# Install layout
# ----------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS ranarch LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS ranarch-daemon ranarchctl
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ranarch
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES data/ranarch.conf.example
        DESTINATION /etc/ranarch RENAME ranarch.conf.example
        PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
install(FILES data/dep_map.csv
        DESTINATION /etc/ranarch
        PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
install(FILES polkit/org.ranarch.daemon.policy
        DESTINATION /usr/share/polkit-1/actions)
install(FILES systemd/ranarch-daemon.service
        DESTINATION /usr/lib/systemd/system)

# ----------------------------------------------------------------------------
# Tests
# ----------------------------------------------------------------------------
enable_testing()

# Tests reach into internal headers under src/ which are not part of the
# public API. Expose that directory privately to each test target.
function(ranarch_add_test name source)
    add_executable(${name} ${source})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(${name} PRIVATE ranarch)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

ranarch_add_test(test_config       tests/test_config.cpp)
ranarch_add_test(test_db           tests/test_db.cpp)
ranarch_add_test(test_deb_parser   tests/test_deb_parser.cpp)
ranarch_add_test(test_rpm_parser   tests/test_rpm_parser.cpp)
ranarch_add_test(test_sig_verifier tests/test_sig_verifier.cpp)
# test_deb_parser and test_rpm_parser build fixture packages in-memory using
# libarchive's write API, so they need direct access to libarchive (which is
# PRIVATE to libranarch).
foreach(fixture_test IN ITEMS test_deb_parser test_rpm_parser)
    target_include_directories(${fixture_test} PRIVATE
        $<TARGET_PROPERTY:PkgConfig::LIBARCHURE,INTERFACE_INCLUDE_DIRECTORIES>)
    target_link_libraries(${fixture_test} PRIVATE PkgConfig::LIBARCHURE)
endforeach()
# test_sig_verifier uses the `gpg` command-line for key generation and
# signing; verification is done through the SigVerifier class (gpgme is
# linked transitively via libranarch).
ranarch_add_test(test_dep_map      tests/test_dep_map.cpp)
ranarch_add_test(test_integration  tests/test_integration.cpp)

# test_integration builds a .deb fixture using libarchive's write API.
target_include_directories(test_integration PRIVATE
    $<TARGET_PROPERTY:PkgConfig::LIBARCHURE,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(test_integration PRIVATE PkgConfig::LIBARCHURE)

# All planned module tests are now wired up.
