# hypercore control-socket protocol

The daemon (`hypercored`) exposes a control API over a **Unix domain stream
socket** (default `/run/hypercore.sock`, `SOCK_STREAM`). The CLI (`hypercore`,
Phase 4) is the only intended client. This document is the normative wire
contract and is versioned so the CLI can detect a mismatch and refuse rather
than misbehave.

## Decision: line-request / ndjson-response (not protobuf, not symmetric JSON)

This is the final decision for the control channel, replacing the Phase 1
placeholder note.

**Requests** are a single, flat, whitespace-delimited line. **Responses** are
newline-delimited JSON (ndjson): exactly one JSON object per line.

### Why not protobuf

The API is tiny (`list`, `start`, `stop`, `status`, `reload`), local-only (same
host, same trust domain, Unix socket with filesystem permissions), and low
frequency. Protobuf buys schema evolution and wire speed we do not need, at the
cost of a codegen toolchain and a generated-code dependency inside a minimal
initramfs. Rejected.

### Why not symmetric JSON (JSON requests too)

A JSON *request* body would force the daemon to run a full recursive JSON
*parser* on input arriving at a privileged control socket. That is exactly the
kind of nested-grammar parser we deliberately avoid hand-rolling — and pulling
in a second heavy vendored parser (on top of toml++) purely to read
`{"command":"stop","vm":"web"}` is not worth it.

Instead, requests use a **flat grammar with no nesting, quoting, or escaping**:

```
<proto> <command> [arg]
```

Tokenizing that is a `split on whitespace` + a fixed allow-list check — not a
"parser" in the risky sense (no recursion, no user-controlled structure). This
**minimizes the attack surface on the daemon's privileged input path**, needs
**zero new dependencies**, and stays trivially debuggable:

```sh
printf '1 status web\n' | socat - UNIX-CONNECT:/run/hypercore.sock
```

Responses, which the daemon *writes* (never parses), are ndjson so they can be
structured, extensible, and machine-read by the CLI. Writing JSON is safe and
small; the CLI parses it (and may vendor a JSON reader in Phase 4). The mild
asymmetry is a deliberate trade for a smaller daemon attack surface.

## Versioning

- The current protocol version is **`1`** (`HYPERCORE_PROTO_VERSION`).
- Every request MUST begin with the client's protocol version integer.
- Every response includes `"proto"`.
- If the daemon receives a `<proto>` it does not support, it replies with an
  `ok:false` / `proto_mismatch` error (see below) and closes the connection.
  The CLI compares the response `proto` to its own and warns the user to update
  if they differ. Version is bumped on any breaking change to request grammar
  or response fields.

## Connection lifecycle

One request → one response, then the daemon closes the connection (no
pipelining in v1). The client connects, writes exactly one request line
terminated by `\n`, reads one ndjson response line, and the socket closes.
Keeping it connection-per-request keeps daemon state trivial and avoids
half-parsed-stream ambiguity.

## Requests

```
<proto> <command> [arg]\n
```

| Command  | Arg            | Meaning |
|----------|----------------|---------|
| `list`   | *(none)*       | Status of every configured guest. |
| `status` | `<name>`       | Detailed status of one guest. |
| `start`  | `<name>`\|`all`| Start a guest (or all stopped guests). |
| `stop`   | `<name>`\|`all`| Gracefully stop a guest (or all running). |
| `reload` | *(none)*       | Re-read config from disk and reconcile; returns the applied diff. |

- `<proto>` is a non-negative integer.
- `<command>` is from the fixed set above; anything else → `unknown_command`.
- `<arg>` must match the guest-name grammar `^[a-z0-9][a-z0-9-]*$` or be the
  literal `all` where allowed; otherwise → `bad_request`.
- Extra tokens, missing required arg, or a malformed line → `bad_request`.

## Responses

One JSON object per line. Success:

```json
{"proto":1,"ok":true,"command":"<cmd>","data":{ ... }}
```

Error:

```json
{"proto":1,"ok":false,"command":"<cmd>","error":{"code":"<code>","message":"..."}}
```

`command` echoes the request verb when known (`"?"` if the line was
unparseable). `data` and `error` are mutually exclusive.

### Guest state machine

`state` is one of:

| State       | Meaning |
|-------------|---------|
| `stopped`   | Configured, no process running. |
| `starting`  | Spawned, not yet confirmed up. |
| `running`   | QEMU process alive; pinning verified. |
| `unhealthy` | Running process, but guest-agent health checks failing. |
| `stopping`  | Graceful shutdown in progress. |
| `failed`    | Last launch or a health-triggered action failed. |

`health` is one of `healthy`, `unhealthy`, `unknown` (no guest agent
configured, or not yet probed).

### Status object

Used by `status` (as `data`) and `list` (as elements of `data.vms`):

```json
{
  "name": "web",
  "state": "running",
  "health": "healthy",
  "pid": 12345,
  "cpus": [2, 3],
  "cpus_verified": true,
  "memory_bytes": 2147483648,
  "rss_bytes": 601358336,
  "cpu_percent": 7.9,
  "network": "user",
  "ip": "127.0.0.1",
  "ssh_port": 27825,
  "console_log": "/run/hypercore/web.console.log",
  "restart": "on-failure",
  "uptime_secs": 3600,
  "restarts": 0,
  "adopted": false
}
```

Phase-4 fields consumed by the CLI/dashboard:
- `rss_bytes` / `cpu_percent` — host-side resident memory and CPU% of the QEMU
  process, sampled from `/proc/<pid>` between health ticks.
- `ip` / `ssh_port` — the SSH endpoint. For user networking, `ip` is
  `127.0.0.1` and `ssh_port` is the per-guest host-forwarded port (guest:22).
  For bridge networking, `ip` is the guest address learned via the guest agent
  and `ssh_port` is 0 (meaning port 22). Empty `ip` means "not yet known".
- `console_log` — path the daemon captured the guest serial console to; `logs`
  reads it.

- `cpus_verified` reflects the read-back check against `/proc/<pid>/status`
  `Cpus_allowed_list` (Phase 3 requirement): `true` only if the kernel confirms
  the affinity mask we requested. If `false`, the guest is reported but flagged.
- `pid` is `null` when not running. `uptime_secs`/`restarts` are `0` when
  stopped.

### `list`

```json
{"proto":1,"ok":true,"command":"list","data":{"vms":[ <status>, ... ]}}
```

### `start` / `stop`

Report per-guest outcomes so `all` is legible:

```json
{"proto":1,"ok":true,"command":"start","data":{"actions":[
  {"name":"web","result":"started","pid":12345},
  {"name":"build","result":"already_running"}
]}}
```

`result` ∈ `started`, `already_running`, `stopped`, `already_stopped`,
`launch_failed`, `stop_timeout` (followed by SIGKILL fallback → `killed`).
A per-guest failure sets that entry's `result` but the overall `ok` stays
`true` — the command ran; individual guests report their own status. A
whole-command failure (e.g. `proto_mismatch`) uses `ok:false`.

### `reload`

Re-reads config, re-validates, and reconciles. Returns the diff that was
applied (same shape the dry-run planner prints):

```json
{"proto":1,"ok":true,"command":"reload","data":{"diff":{
  "start":   ["newvm"],
  "stop":    ["removedvm"],
  "restart": ["changedvm"],
  "unchanged":["web","build"]
}}}
```

If the reloaded config has validation **errors**, `reload` does not apply
anything and returns `ok:false` / `config_invalid` with the diagnostics in
`error.message`; the running set is left untouched.

## Error codes

| Code             | Cause |
|------------------|-------|
| `proto_mismatch` | Unsupported protocol version in the request. |
| `bad_request`    | Malformed line, bad arg, missing/extra tokens. |
| `unknown_command`| Verb not in the command set. |
| `unknown_vm`     | Named guest is not in the config. |
| `config_invalid` | `reload` found validation errors; nothing applied. |
| `internal`       | Unexpected daemon-side failure (logged with detail). |

## Non-goals for v1

- No authentication beyond Unix socket file permissions (single-host, root-run
  daemon; the socket is `0600` by owner).
- No streaming/log-follow on this channel — `logs`/`ssh` are CLI concerns in
  Phase 4 and may use a separate framing.
- No request pipelining or persistent sessions.
