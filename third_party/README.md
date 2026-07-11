# third_party — vendored single-file headers

These headers are copied verbatim at a fixed release tag. They are **not**
fetched at build time — the versions below are the ground truth.

| Library | File | Pinned version | Upstream |
|---------|------|----------------|----------|
| toml++ | `toml.hpp` | v3.4.0 | https://github.com/marzer/tomlplusplus |
| nlohmann/json | `json.hpp` | v3.11.3 | https://github.com/nlohmann/json |

## Why vendored?

Both libraries are single-header, actively maintained, and have no transitive
dependencies. Vendoring eliminates network fetches during `cmake` configure and
guarantees reproducible builds on air-gapped hosts (e.g., the ISO builder).

## Updating a vendored header

1. Check the upstream release page for the new version.
2. Download the single-header release asset:
   ```sh
   # toml++
   curl -Lo third_party/toml.hpp \
     https://github.com/marzer/tomlplusplus/releases/download/v<NEW>/toml.hpp

   # nlohmann/json
   curl -Lo third_party/json.hpp \
     https://github.com/nlohmann/json/releases/download/v<NEW>/json.hpp
   ```
3. Update the version table above.
4. Run `cmake --build build && ctest --test-dir build` to confirm nothing broke.
5. Commit as `chore(deps): bump toml++ to vX.Y.Z` (or `nlohmann/json`).

## Security advisories

GitHub's dependency graph detects these vendored headers via the repository's
SBOM and will surface relevant CVEs in the Security tab. **Always update within
7 days of a security advisory affecting either library.**

Dependabot cannot auto-update vendored header files, but it does track all
GitHub Actions versions in `.github/workflows/` automatically — see
`.github/dependabot.yml`.
