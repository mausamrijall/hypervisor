# tests/ — unit and smoke tests.
#
# Phase 1: a smoke test that exercises the built binaries via CTest, so
# `ctest` is meaningful from day one and CI has a gate. The config-parser unit
# tests (Phase 2) will be added here as native C++ test executables.

# Binaries report the same version string as the build. Assert that contract.
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

# The daemon skeleton runs to a clean exit with a throwaway config path.
add_test(
  NAME daemon_dry_run
  COMMAND hypercored --dry-run --config /nonexistent/hypervisor.cfg)
