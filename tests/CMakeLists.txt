# tests/ — unit and smoke tests.
#
# Phase 1: CTest smoke tests over the built binaries.
# Phase 2: config_test — native C++ unit tests for the parser + validator,
#          using a tiny zero-dependency harness (hctest.hpp). No GoogleTest, per
#          the "dependency-free beyond toml++" constraint.

add_executable(config_test config_test.cpp)
target_link_libraries(config_test PRIVATE hypercore_config)
add_test(NAME config_test COMMAND config_test)

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
