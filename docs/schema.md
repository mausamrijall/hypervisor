# hypercore configuration schema

`hypervisor.cfg` is [TOML](https://toml.io). This document is the normative
reference for every field the parser (Phase 2) will accept. Until Phase 2
lands, treat this as the design contract.

> **Why TOML over a hand-rolled format?** See
> [docs/architecture.md](../docs/architecture.md#config-toml-vs-custom). Short
> version: config correctness is security-relevant for a hypervisor, and a
> hand-written parser is exactly where subtle bugs hide. We pull in a small,
> dependency-free TOML library rather than owning that risk.

## Top-level structure

```toml
[hypercore]        # global daemon settings (one table)
[vm.<name>]        # one table per guest; <name> is the guest's identifier
[vm.<name>.share]  # optional virtiofs share for that guest
```

## `[hypercore]` — global settings

| Key          | Type   | Required | Default                | Notes |
|--------------|--------|----------|------------------------|-------|
| `socket`     | string | no       | `/run/hypercore.sock`  | Control socket path the CLI dials. |
| `state_dir`  | string | no       | `/var/lib/hypercore`   | Base dir for images/runtime state. |
| `log_level`  | string | no       | `info`                 | `trace\|debug\|info\|warn\|error`. |
| `log_format` | string | no       | `logfmt`               | `logfmt\|json`. |

## `[vm.<name>]` — per-guest settings

| Key           | Type            | Required | Notes |
|---------------|-----------------|----------|-------|
| `image`       | string (path)   | **yes**  | Path to the disk image. |
| `disk_type`   | string          | **yes**  | `raw` or `qcow2`. Must match the actual image. |
| `cpus`        | array<int>      | **yes**  | Explicit host core list for pinning (e.g. `[2,3]`). Non-empty; each core must exist on the host; no two VMs should share cores (warned, not rejected). |
| `memory`      | string          | **yes**  | Size with `K`/`M`/`G` suffix (e.g. `2G`). Parsed to bytes. |
| `network`     | string          | **yes**  | `bridge`, `user`, or `virtiofs`. |
| `restart`     | string          | no (default `on-failure`) | `never`, `on-failure`, or `always`. |
| `guest_agent` | string (path)   | no       | QEMU guest-agent socket path. Required for health checks; without it, restart policy falls back to process-liveness only. |

### `[vm.<name>.share]` — optional virtiofs share

| Key         | Type   | Required | Notes |
|-------------|--------|----------|-------|
| `tag`       | string | **yes**  | Mount tag used inside the guest. |
| `host_path` | string | **yes**  | Host directory to export. Must exist. |
| `readonly`  | bool   | no (default `false`) | Export read-only. |

## Validation rules (enforced by the Phase 2 parser)

1. At least one `[vm.<name>]` table, or the daemon warns and idles.
2. Guest `<name>` matches `^[a-z0-9][a-z0-9-]*$` and is unique.
3. `disk_type` ∈ {`raw`, `qcow2`}; `network` ∈ {`bridge`, `user`, `virtiofs`};
   `restart` ∈ {`never`, `on-failure`, `always`}.
4. `cpus` is a non-empty array of non-negative integers.
5. `memory` matches `^[0-9]+[KMG]?$` (bytes if no suffix).
6. `network = "virtiofs"` requires a `[vm.<name>.share]` table.
7. Unknown keys are a hard error (typos must not silently no-op) — this is a
   deliberate strictness choice for a security-sensitive config.

Malformed and missing-field cases each get a dedicated unit test in Phase 2.
