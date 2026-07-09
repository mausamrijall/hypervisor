# cli/ — hypercore, the thin CLI client + live dashboard.
#
# Links only the small socket-client lib (hypercore_usock) and the vendored
# JSON reader — NOT the runtime engine. The CLI is a pure control-socket client.

add_executable(hypercore
  main.cpp
  client.cpp
  dashboard.cpp)

target_link_libraries(hypercore
  PRIVATE hypercore_common hypercore_usock nlohmann_json)

# execvpe (used by `ssh`) is a GNU extension.
target_compile_definitions(hypercore PRIVATE _GNU_SOURCE)

set_target_properties(hypercore PROPERTIES OUTPUT_NAME hypercore)

install(TARGETS hypercore RUNTIME DESTINATION bin)
