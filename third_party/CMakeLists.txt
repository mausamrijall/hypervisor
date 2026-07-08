# Vendored third-party dependencies.
#
# toml++ is header-only; we expose it as an INTERFACE target that just adds the
# include directory. Only one translation unit (libconfig parser.cpp) includes
# the 476 KB header, so header-only mode costs us nothing at link time and
# keeps the build free of any codegen or fetch step.

add_library(tomlplusplus INTERFACE)
# SYSTEM: treat toml.hpp as a system header so its own warnings (e.g. deprecated
# literal-operator syntax under Clang) don't pollute our -Wall -Wextra build. We
# do not maintain this code; only OUR warnings should be actionable.
target_include_directories(tomlplusplus
  SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# toml++ needs C++17 minimum; we build at C++20 project-wide already.
target_compile_features(tomlplusplus INTERFACE cxx_std_17)
