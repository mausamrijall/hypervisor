# init/ — boot-environment init (Phase 5)

Not yet implemented. This directory will hold the minimal init that PID 1 runs
in the booted hypercore environment:

- suppress kernel log noise on the console,
- mount what the daemon needs (`/proc`, `/sys`, `/dev`, cgroups, the state dir),
- start `hypercored`,
- hand off the local TTY to the `hypercore dashboard`.

The choice between a custom PID 1 and a systemd-unit wrapper — with
justification — is made in Phase 5, per the project brief.
