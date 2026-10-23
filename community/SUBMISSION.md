# Community distribution and maintenance

`decide` was accepted into DuckDB Community Extensions on October 23, 2026:
[submission PR #3037](https://github.com/duckdb/community-extensions/pull/3037).
The source repository is public, and signed community binaries are available
for DuckDB 2.0.0 on supported platforms:

```sql
INSTALL decide FROM community;
LOAD decide;
```

The accepted source includes both OpenAI Decisions and Cloudflare Clef support.
A [documentation correction, PR #3347](https://github.com/duckdb/community-extensions/pull/3347),
updates the generated listing to describe Cloudflare as well.

## Updating the community package

1. Make source changes on the supported release line and pass `Build and test`
   and `Community distribution compatibility` CI. Keep credentials and private
   inputs out of source, fixtures, and logs.
2. Update the descriptor's extension version and pin `repo.ref` to a full tested
   public commit SHA. Keep provider documentation and platform exclusions aligned
   with that source revision.
3. Open a PR to `duckdb/community-extensions` changing only
   `extensions/decide/description.yml`, and address upstream review and build
   results. Documentation-only updates can retain the current source pin/version.
4. After deployment, verify installation and loading in the matching official
   DuckDB runtime. API evaluation requires the user's own provider credentials.

The community repository builds for DuckDB's latest stable release. The current
line is DuckDB `v2.0.0` with extension-ci-tools `v2.0-cyanoptera`, and extension
releases `0.2.*`. Recheck upstream build inputs when upgrading; C++ extension
artifacts are version- and platform-specific.

DuckDB 1.5.x releases remain on `v1.x` as `0.1.*` GitHub releases; see
[RELEASING.md](../RELEASING.md). GitHub release and local build artifacts are
installed separately from signed community packages.

The descriptor in this directory is the maintained template for registry
updates. The upstream registry reads its own copy, not this file.

Sources:
- https://duckdb.org/community_extensions/documentation
- https://duckdb.org/community_extensions/development
- https://github.com/duckdb/community-extensions/blob/main/.github/workflows/build.yml
