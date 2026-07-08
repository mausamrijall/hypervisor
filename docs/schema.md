# hypercore configuration schema

`hypervisor.cfg` is [TOML](https://toml.io). This document is the **normative**
reference for every field the parser accepts and every rule the validator
enforces. It is the contract the Phase 2 parser and tests are written against.

> **Why TOML over a hand-rolled format?** See
> [docs/architecture.md](architecture.md#config-toml-vs-custom). Short version:
> config correctness is security-relevant for a hypervisor, and a hand-written
> parser is exactly where subtle bugs hide. We vendor a small, dependency-free
> TOML library rather than owning that risk.

## Top-level structure

```toml
[hypercore]        # global daemon settings (one table, optional)
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

The whole `[hypercore]` table may be omitted; all fields then take their
defaults.

## `[vm.<name>]` — per-guest settings

| Key           | Type          | Required | Default        | Notes |
|---------------|---------------|----------|----------------|-------|
| `image`       | string (path) | **yes**  | —              | Path to the disk image. |
| `disk_type`   | string        | **yes**  | —              | `raw` or `qcow2`. Must match the actual image. |
| `cpus`        | array<int>    | **yes**  | —              | Explicit host core list for pinning (e.g. `[2,3]`). Non-empty. |
| `memory`      | string        | **yes**  | —              | Size with optional `K`/`M`/`G`/`T` suffix (e.g. `2G`). Parsed to bytes. |
| `network`     | string        | **yes**  | —              | `bridge`, `user`, or `virtiofs`. |
| `restart`     | string        | no       | `on-failure`   | `never`, `on-failure`, or `always`. |
| `guest_agent` | string (path) | no       | *(none)*       | QEMU guest-agent socket path. Required for agent-based health checks; without it, restart policy falls back to process liveness only. |

### `[vm.<name>.share]` — optional virtiofs share

| Key         | Type   | Required | Default | Notes |
|-------------|--------|----------|---------|-------|
| `tag`       | string | **yes**  | —       | Mount tag used inside the guest. |
| `host_path` | string | **yes**  | —       | Host directory to export. |
| `readonly`  | bool   | no       | `false` | Export read-only. |

## Validation

Validation is a **separate pass** from parsing. Parsing turns TOML into typed
structs (or reports syntax/type errors); validation then checks semantics and
returns **all** problems at once — it does not stop at the first. Two severities:

- **error** — the config is rejected; the daemon refuses to start.
- **warning** — surfaced to the operator but the config is still accepted.

### Errors (reject the config)

| # | Rule |
|---|------|
| E1 | TOML is syntactically invalid, or a field has the wrong type. |
| E2 | A required per-VM field is missing (`image`, `disk_type`, `cpus`, `memory`, `network`). |
| E3 | Guest `<name>` does not match `^[a-z0-9][a-z0-9-]*$`. |
| E4 | Duplicate guest name. *(TOML forbids duplicate tables outright, so this is also caught at parse time; the validator still guards it for configs built programmatically.)* |
| E5 | `disk_type` ∉ {`raw`,`qcow2`}, `network` ∉ {`bridge`,`user`,`virtiofs`}, or `restart` ∉ {`never`,`on-failure`,`always`}. |
| E6 | `cpus` is empty, contains a negative value, or contains a duplicate. |
| E7 | A pinned core index is **≥ host core count** (`nproc`). Pinning to a core that doesn't exist can't succeed. |
| E8 | `memory` fails to parse (`^[0-9]+[KMGT]?$`) or is zero. |
| E9 | `network = "virtiofs"` but no `[vm.<name>.share]` table, or a `share` is missing `tag`/`host_path`. |
| E10 | An unknown key appears in any table. Typos must not silently no-op in a security-sensitive config. |

### Warnings (accept, but flag)

| # | Rule |
|---|------|
| W1 | **CPU pin overlap:** two VMs pin the same host core. Allowed (oversubscription is a valid choice) but flagged, because it defeats the point of explicit pinning if unintended. |
| W2 | **RAM overcommit:** the sum of all VMs' `memory` exceeds host physical RAM. Allowed (guests rarely touch all their RAM; the kernel overcommits) but flagged. |
| W3 | No `[vm.*]` tables at all — the daemon has nothing to run and will idle. |
| W4 | A VM sets `restart != "never"` but has no `guest_agent`; health checks degrade to process-liveness only. |

### Host-fact dependencies

E7 and W2 compare against **host facts** (core count, physical RAM). These are
injected into the validator (see `HostInfo`) rather than read globally, so tests
can pin a synthetic host (e.g. "4 cores, 8 GiB") and assert deterministic
outcomes regardless of the machine running the tests.

## Worked example

See [`config/hypervisor.cfg`](../config/hypervisor.cfg) for a complete,
annotated configuration exercising every field above.
