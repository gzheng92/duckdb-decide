#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
# DuckDB pins for this branch's release line.
source release.env
# Submodule checkouts may omit tags; fetch the release tag before validating it.
if [ ! -e duckdb/.git ]; then
  git submodule update --init duckdb
fi
if ! git -C duckdb rev-parse --verify "refs/tags/$DUCKDB_VERSION" >/dev/null 2>&1; then
  git -C duckdb fetch --depth 1 origin tag "$DUCKDB_VERSION"
fi
if [ "$(git -C duckdb describe --tags --exact-match)" != "$DUCKDB_VERSION" ] || [ "$(git -C duckdb rev-parse HEAD)" != "$DUCKDB_COMMIT" ]; then
  echo "Expected DuckDB $DUCKDB_VERSION in duckdb/ (this is the $RELEASE_BRANCH line)" >&2
  exit 1
fi
# Stamp the release version explicitly. DuckDB 2.0 no longer derives it from git
# tags, and official binaries reject artifacts stamped as dev builds.
cmake -S duckdb -B build/release -DCMAKE_BUILD_TYPE=Release \
  -DDUCKDB_EXTENSION_CONFIGS="$PWD/extension_config.cmake" \
  -DOVERRIDE_GIT_DESCRIBE="$DUCKDB_VERSION" -DBUILD_UNITTESTS=OFF -DDISABLE_BUILTIN_EXTENSIONS=ON \
  -DEXTENSION_STATIC_BUILD="${EXTENSION_STATIC_BUILD:-OFF}"
cmake --build build/release --target decide_loadable_extension -j "${BUILD_JOBS:-4}"
