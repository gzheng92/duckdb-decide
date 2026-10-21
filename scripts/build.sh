#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
DUCKDB_COMMIT=d54cbbe3df17e8e5ce68312f483b4155836112aa
# Submodule checkouts may omit tags; fetch the release tag before validating it.
if [ ! -e duckdb/.git ]; then
  git submodule update --init duckdb
fi
if ! git -C duckdb rev-parse --verify refs/tags/v2.0.0 >/dev/null 2>&1; then
  git -C duckdb fetch --depth 1 origin tag v2.0.0
fi
if [ "$(git -C duckdb describe --tags --exact-match)" != "v2.0.0" ] || [ "$(git -C duckdb rev-parse HEAD)" != "$DUCKDB_COMMIT" ]; then
  echo 'Expected DuckDB v2.0.0 in duckdb/' >&2
  exit 1
fi
# DuckDB 2.0 no longer derives its version from git tags; stamp the release so
# the artifact loads in official v2.0.0 binaries rather than only dev builds.
cmake -S duckdb -B build/release -DCMAKE_BUILD_TYPE=Release \
  -DDUCKDB_EXTENSION_CONFIGS="$PWD/extension_config.cmake" \
  -DDUCKDB_EXPLICIT_VERSION=v2.0.0 -DBUILD_UNITTESTS=OFF -DDISABLE_BUILTIN_EXTENSIONS=ON \
  -DEXTENSION_STATIC_BUILD="${EXTENSION_STATIC_BUILD:-OFF}"
cmake --build build/release --target decide_loadable_extension -j "${BUILD_JOBS:-4}"
