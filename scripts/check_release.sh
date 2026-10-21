#!/usr/bin/env bash
# Checks that this branch's version, pins, and workflows agree with release.env.
# Usage: scripts/check_release.sh [tag]   (with a tag, also checks it matches)
set -euo pipefail
cd "$(dirname "$0")/.."
source release.env
fail() { echo "release check: $*" >&2; exit 1; }

version=$(sed -n 's/.*EXTENSION_VERSION \([0-9][0-9.]*\).*/\1/p' extension_config.cmake)
[ -n "$version" ] || fail "no EXTENSION_VERSION in extension_config.cmake"
case "$version" in "$SERIES".*) ;; *) fail "version $version is outside the $SERIES.x series for DuckDB $DUCKDB_VERSION" ;; esac
grep -q "^  version: $version$" community/description.yml || fail "community/description.yml version differs from $version"
grep -q "^  excluded_platforms: \"$EXCLUDED_PLATFORMS\"$" community/description.yml || fail "community/description.yml excluded_platforms differs from $EXCLUDED_PLATFORMS"

submodule=$(git ls-tree HEAD duckdb | awk '{print $3}')
[ "$submodule" = "$DUCKDB_COMMIT" ] || fail "duckdb submodule is $submodule, expected $DUCKDB_COMMIT"
grep -q "^\s*branch = $CI_TOOLS_VERSION$" .gitmodules || fail ".gitmodules ci-tools branch differs from $CI_TOOLS_VERSION"
for workflow in .github/workflows/distribution.yml .github/workflows/release.yml; do
  grep -q "_extension_distribution.yml@$CI_TOOLS_VERSION$" "$workflow" || fail "$workflow does not use ci-tools $CI_TOOLS_VERSION"
  grep -q "duckdb_version: $DUCKDB_VERSION$" "$workflow" || fail "$workflow does not build DuckDB $DUCKDB_VERSION"
  grep -q "ci_tools_version: $CI_TOOLS_VERSION$" "$workflow" || fail "$workflow ci_tools_version differs from $CI_TOOLS_VERSION"
  grep -q "exclude_archs: \"$EXCLUDED_PLATFORMS\"$" "$workflow" || fail "$workflow exclude_archs differs from $EXCLUDED_PLATFORMS"
done

if [ $# -gt 0 ]; then
  [ "$1" = "v$version" ] || fail "tag $1 does not match extension version v$version"
fi
echo "decide $version for DuckDB $DUCKDB_VERSION ($RELEASE_BRANCH)"
