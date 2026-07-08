# libcore — the daemon's runtime engine (Phase 3).
#
# Everything that touches the host and real processes lives here, kept as a
# static library so the daemon binary and the test binaries link the exact same
# code paths (no divergence between "what CI tests" and "what runs").
#
#   capabilities.cpp  host capability detection (/dev/kvm + qemu). Req #1
#   qemu.cpp          QEMU argv construction + spawn + PID files. Req #3a
#   affinity.cpp      sched_setaffinity + /proc readback.          Req #3b
#   qmp.cpp           QMP client + graceful shutdown.              Req #3c
#   guest_agent.cpp   guest-agent health probe.                    Req #4
#   reconcile.cpp     desired-vs-actual diff (host-independent).   Req #5
#   supervisor.cpp    ties launch/pin/health/restart together.
#   control_server.cpp  ndjson control socket.                     protocol.md

add_library(hypercore_core STATIC
  src/capabilities.cpp
  src/affinity.cpp
  src/pidfile.cpp
  src/usock.cpp
  src/qemu.cpp
  src/qmp.cpp
  src/guest_agent.cpp
  src/reconcile.cpp
  src/supervisor.cpp
  src/control_server.cpp)

target_include_directories(hypercore_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(hypercore_core
  PUBLIC hypercore_config
  PRIVATE hypercore_common)

# Diagnostic helper: prints detected host capabilities as JSON. Used by the
# test harness's gate and handy for humans debugging a box. (Req #1 output.)
add_executable(hostcaps tools/hostcaps.cpp)
target_link_libraries(hostcaps PRIVATE hypercore_core hypercore_common)
