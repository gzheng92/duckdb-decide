# Submission procedure

Checked against DuckDB's community documentation and current build workflow on
October 12, 2026. The repository remains private; this preparation does not submit
an upstream PR or change repository visibility.

1. Pass `Community distribution compatibility` and `Build and test` for the
   preparation branch. Resolve platform failures or explicitly exclude affected
   platforms in both the distribution workflow and descriptor.
2. Review the source, dependencies, README, and repository history before making
   `gzheng92/duckdb-decide` public. DuckDB requires a public, open-source GitHub repo.
3. Merge the preparation changes. Set `repo.ref` in `description.yml` to the full
   tested commit SHA containing the implementation. Confirm version `0.1.1` and
   maintainer `gzheng92`. A release tag is useful but not required by the documented
   submission procedure; the descriptor should pin an immutable commit.
4. Fork `duckdb/community-extensions`, create a branch, and copy only the completed
   descriptor into `extensions/decide/description.yml`. Open a PR to that repo.
   Explain the paid external API dependency, text-only scope, batch semantics,
   native libcurl dependency, and Wasm exclusions.
5. Address review and upstream build/test results. The community workflow builds
   the pinned public source with DuckDB's toolchain; merging triggers distribution.
   After publication, verify `INSTALL decide FROM community; LOAD decide;` on a
   supported DuckDB version/platform. Users then supply their own OpenAI key.
6. For updates, submit a descriptor PR with a new tested commit and extension
   version. Track new DuckDB releases and use `ref_next` when a separate source
   revision is needed for the upcoming release.

The community repository builds only for DuckDB's latest stable release, using
`repo.ref`; `ref_next` covers DuckDB's development branch before a release.
Submit the `v2.x` line. Releases for DuckDB 1.5.x come from the `v1.x` branch as
GitHub Releases (see RELEASING.md); users on 1.5.x keep the last community build
made for that version.

Current community build inputs are DuckDB `v1.5.6` and extension-ci-tools
`v1.5-variegata`. Recheck these before submitting; C++ extension artifacts are
version- and platform-specific.

The descriptor in this directory is a draft pinned to the prepared source commit.
Require green distribution CI for that source revision before submission and
refresh the ref if implementation changes are needed. CI for this repo does not
read that descriptor; the upstream community repo does.

Sources:
- https://duckdb.org/community_extensions/documentation
- https://duckdb.org/community_extensions/development
- https://github.com/duckdb/community-extensions/blob/main/.github/workflows/build.yml
- https://github.com/duckdb/extension-template
