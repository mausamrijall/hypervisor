# common/ — utilities shared by the daemon and the CLI.
#
# Header-only for now (logging). Kept as an INTERFACE library so it adds no
# link-time cost and both binaries pull in the same headers.

add_library(hypercore_common INTERFACE)
target_include_directories(hypercore_common
  INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hypercore_common INTERFACE hypercore_version)
