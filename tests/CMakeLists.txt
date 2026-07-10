# tests/ — unit and smoke tests.
#
# Phase 1: CTest smoke tests over the built binaries.
# Phase 2: config_test — native C++ unit tests for the parser + validator,
#          using a tiny zero-dependency harness (hctest.hpp). No GoogleTest, per
#          the "dependency-free beyond toml++" constraint.

add_executable(config_test config_test.cpp)
target_link_libraries(config_test PRIVATE hypercore_config)
add_test(NAME config_test COMMAND config_test)

# Reconcile diff + cpu-list parsing: host-independent unit tests (no QEMU/KVM).
add_executable(reconcile_test reconcile_test.cpp)
target_link_libraries(reconcile_test PRIVATE hypercore_core)
add_test(NAME reconcile_test COMMAND reconcile_test)

# Security hardening: control-parser robustness + IPv4 validation (no QEMU/KVM).
add_executable(hardening_test hardening_test.cpp)
target_link_libraries(hardening_test PRIVATE hypercore_core)
add_test(NAME hardening_test COMMAND hardening_test)

# --- Phase 1 binary smoke tests ---------------------------------------------
add_test(
  NAME daemon_version
  COMMAND hypercored --version)
set_tests_properties(daemon_version PROPERTIES
  PASS_REGULAR_EXPRESSION "${PROJECT_VERSION}")

add_test(
  NAME cli_version
  COMMAND hypercore --version)
set_tests_properties(cli_version PROPERTIES
  PASS_REGULAR_EXPRESSION "${PROJECT_VERSION}")

# Help must exit 0 and mention the program.
add_test(
  NAME daemon_help
  COMMAND hypercored --help)
set_tests_properties(daemon_help PROPERTIES
  PASS_REGULAR_EXPRESSION "hypercored")

# Unknown flags are a usage error (exit 2), not a crash.
add_test(
  NAME daemon_bad_flag
  COMMAND hypercored --nonsense)
set_tests_properties(daemon_bad_flag PROPERTIES
  WILL_FAIL TRUE)

# Missing/unreadable config is now a hard error (exit 1), not a silent success.
add_test(
  NAME daemon_missing_config
  COMMAND hypercored --dry-run --config /nonexistent/hypervisor.cfg)
set_tests_properties(daemon_missing_config PROPERTIES WILL_FAIL TRUE)

# A minimal config (pinned to core 0, modest RAM) parses, validates, and
# dry-runs cleanly on ANY host — an end-to-end check of parser+validator
# through the daemon. The richer config/hypervisor.cfg is host-specific (it
# pins high core indices) so is intentionally not used as a portable gate.
add_test(
  NAME daemon_dry_run_minimal_config
  COMMAND hypercored --dry-run --config ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/minimal.cfg)
set_tests_properties(daemon_dry_run_minimal_config PROPERTIES
  PASS_REGULAR_EXPRESSION "dry-run complete")

# --- Phase 3 integration tests (real QEMU; auto-skip without KVM) ------------
# The integration binary drives libcore against a real busybox guest. It is
# gated at runtime by host capability detection and returns 77 (SKIP) when
# /dev/kvm or qemu-system is unavailable — so CI runners without KVM skip
# cleanly instead of failing.

# Fixture test: build the throwaway busybox guest once. Also gated: if the
# build host lacks busybox/cpio/gzip/kernel it fails, and dependent tests skip.
add_test(
  NAME build_test_guest
  COMMAND ${CMAKE_SOURCE_DIR}/iso-builder/testguest/build-testguest.sh
          --out ${CMAKE_BINARY_DIR}/testguest)
set_tests_properties(build_test_guest PROPERTIES
  FIXTURES_SETUP testguest
  # If prerequisites are missing the script exits non-zero; treat as skip so a
  # build host without busybox doesn't red the suite.
  SKIP_RETURN_CODE 1
  TIMEOUT 120)

add_executable(integration_test integration_test.cpp)
target_link_libraries(integration_test PRIVATE hypercore_core)

add_test(
  NAME integration_test
  COMMAND integration_test ${CMAKE_BINARY_DIR}/testguest)
set_tests_properties(integration_test PROPERTIES
  FIXTURES_REQUIRED testguest
  SKIP_RETURN_CODE 77
  TIMEOUT 300)

# Convenience: also expose capability detection as a (never-failing) test so
# `ctest -V` records what the host can do.
add_test(NAME host_capabilities COMMAND hostcaps)
set_tests_properties(host_capabilities PROPERTIES SKIP_RETURN_CODE 1)
