# libconfig — parsing and validation of hypervisor.cfg.
#
# A static library so both the daemon (Phase 3) and the unit tests link the
# exact same code. The parse/validate split is reflected in the source files:
#   strings.cpp    pure vocabulary: enum<->string, memory sizes, code names
#   host_info.cpp  host fact probing (cores, RAM)
#   parser.cpp     TOML -> typed structs; value-level diagnostics (E1/E5/E8/E10)
#   validator.cpp  semantic checks; set-level diagnostics (E2-E4,E6,E7,E9,W1-W4)

add_library(hypercore_config STATIC
  src/strings.cpp
  src/host_info.cpp
  src/parser.cpp
  src/validator.cpp)

target_include_directories(hypercore_config
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# toml++ is an implementation detail of parser.cpp — PRIVATE so it does not leak
# into consumers' include paths.
target_link_libraries(hypercore_config
  PRIVATE tomlplusplus)
