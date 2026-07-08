# cli/ — hypercore, the thin CLI client.
#
# Phase 1 scope: a runnable binary that shares versioning/logging with the
# daemon and stubs its subcommands. The Unix-socket client, `ssh <name>`, and
# the live TTY dashboard land in Phase 4.

add_executable(hypercore
  main.cpp)

target_link_libraries(hypercore
  PRIVATE hypercore_common)

set_target_properties(hypercore PROPERTIES OUTPUT_NAME hypercore)

install(TARGETS hypercore RUNTIME DESTINATION bin)
