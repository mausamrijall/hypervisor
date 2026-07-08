# src/ — hypercored, the hypervisor daemon.
#
# Phase 1 scope: a real binary that starts, parses global flags, emits
# structured logs, and exits cleanly. The reconcile loop, QEMU launcher, and
# Unix-socket API land in Phase 3 — each will be its own translation unit added
# to this target.

add_executable(hypercored
  main.cpp)

target_link_libraries(hypercored
  PRIVATE hypercore_common hypercore_config)

set_target_properties(hypercored PROPERTIES OUTPUT_NAME hypercored)

install(TARGETS hypercored RUNTIME DESTINATION bin)
