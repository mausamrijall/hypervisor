# Vendored third-party code

## `toml.hpp` — toml++

- **Upstream:** https://github.com/marzer/tomlplusplus
- **Version:** v3.4.0 (pinned)
- **Form:** official amalgamated single header (`toml.hpp`).
- **License:** MIT (SPDX-License-Identifier at the top of `toml.hpp`; full text
  in `LICENSE.toml++`).

### Why a committed single header, not a git submodule

The project requires **no network fetch at build time** and must build in a
minimal/offline environment (the ISO builder in Phase 6). A git submodule would
still need `git submodule update --init` — a network fetch and an extra build
step — and complicates offline packaging. The amalgamated header is fully
self-contained, needs no fetch or submodule init ever, and is header-only so it
adds no link-time dependency. The tradeoff is a 476 KB file in the tree and
manual version bumps; both are acceptable for a pinned, security-sensitive
dependency.

### Updating

Replace `toml.hpp` with the amalgamation from the desired upstream release tag
and update the version above. Do not hand-edit the header.

## `json.hpp` — nlohmann/json

- **Upstream:** https://github.com/nlohmann/json
- **Version:** v3.11.3 (pinned)
- **Form:** official single-header release (`json.hpp`).
- **License:** MIT (SPDX header in the file).

### Why committed single header

Same reasoning as toml++: no build-time fetch, offline-friendly for the ISO
build, header-only. Used **only by the CLI** to parse the daemon's ndjson
responses — the daemon itself never links it (it does not parse JSON on its
privileged control path).
