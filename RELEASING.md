# Releasing

Each supported DuckDB line is maintained on its own branch with its own extension
version series. A built extension loads only into the DuckDB version it was built
for, so one release cannot serve both lines.

| Branch | DuckDB | ci-tools | Versions | GitHub "latest" |
| --- | --- | --- | --- | --- |
| `v2.x` | v2.0.0 (LTS) | `v2.0-cyanoptera` | `0.2.*` | yes |
| `v1.x` | v1.5.6 | `v1.5-variegata` | `0.1.*` | no |

`main` is the default branch and mirrors the newest line (`v2.x`). Land changes
for that line on `v2.x`, then fast-forward `main` to it
(`git push origin v2.x:main`). Release from `v2.x`, not `main`.

`release.env` on each branch records its line. `scripts/check_release.sh` runs in CI
and before every release. It fails when the extension version, community descriptor,
DuckDB submodule, `.gitmodules`, or workflow pins disagree with that file.

## Cutting a release

1. Commit the fix on the branch for the newest affected line. Cherry-pick it to the
   other line if it applies there, adapting it to that DuckDB API if needed.
2. On each branch being released, bump `EXTENSION_VERSION` in
   `extension_config.cmake` and `version` in `community/description.yml` within the
   branch's series. Patch for fixes (`0.2.0` → `0.2.1`), minor only when starting a
   new DuckDB line.
3. Push, and wait for `Build and test` and `Community distribution compatibility`
   to pass on that commit.
4. Release it in one of two ways:
   - In the Actions tab, run the `Release` workflow on the branch (`v1.x` or `v2.x`).
     It releases the branch's current version and creates the tag.
   - Or tag the tested commit and push the tag:
     `git tag v0.2.1 && git push origin v0.2.1`.

   The workflow checks that the version belongs to the branch and isn't already
   released. It then builds and tests every supported platform for that DuckDB
   version and publishes a GitHub Release with one binary per platform and a
   `SHA256SUMS` file. Releases from `v1.x` are not marked as the repository's
   latest release.

## Starting a new DuckDB line

1. Branch from the newest line, for example `v3.x` from `v2.x`.
2. Move the `duckdb` submodule to the new release tag. Move `extension-ci-tools`
   to the matching branch and set it in `.gitmodules`.
3. Update `release.env`, the workflow pins in `distribution.yml` and `release.yml`,
   and start a new minor series.
4. Port API changes, run `make release`, `make test_release`, and the mock
   integration suite, then add the line to this table and the README.
5. Point the community descriptor's `ref` at the new line once DuckDB ships it as
   latest stable. The older line keeps receiving GitHub Releases for as long as
   it is supported.
