# hypercore — a developer-first Type-1 hypervisor.
#
# Top-level build. Subdirectories build independently so each phase of the
# project stays incrementally runnable:
#   - src/  -> hypercored   (the daemon)
#   - cli/  -> hypercore    (the thin CLI client)
#
# Deliberately minimal for Phase 1: we only require a C++ toolchain. QEMU,
# xorriso, mksquashfs etc. are runtime/packaging concerns handled in later
# phases and are NOT build dependencies of the daemon.

cmake_minimum_required(VERSION 3.16)

project(hypercore
  VERSION 0.1.0
  DESCRIPTION "Developer-first Type-1 hypervisor (KVM/QEMU)"
  LANGUAGES CXX)

# --- Global toolchain policy -------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Put all binaries in build/bin so the CLI and daemon sit side by side and the
# ISO builder has a single, predictable place to pick them up from.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

# Warnings-as-signal (not -Werror yet; that lands in CI once the surface grows).
add_compile_options(-Wall -Wextra -Wpedantic)

# Shared version header, generated from the project version above so the daemon
# and CLI always report the same string.
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.hpp.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/hypercore/version.hpp
  @ONLY)
add_library(hypercore_version INTERFACE)
target_include_directories(hypercore_version
  INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/generated)

# Header-only common utilities (logging, etc.) shared by daemon and CLI.
add_subdirectory(common)
add_subdirectory(src)
add_subdirectory(cli)

# --- Testing -----------------------------------------------------------------
# Unit tests arrive in Phase 2 (config parser). We wire up CTest now so the
# harness exists and CI can call it unconditionally.
include(CTest)          # defines BUILD_TESTING (default ON) and enable_testing()
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

message(STATUS "hypercore ${PROJECT_VERSION} configured (${CMAKE_BUILD_TYPE})")
