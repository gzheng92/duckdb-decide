#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
DUCKDB_COMMIT=069cc9f9b5be802405797faecc284961b07c70ef
# Both tag and commit are checked.
if [ ! -d duckdb/.git ]; then
  git clone --depth 1 --branch v1.5.6 https://github.com/duckdb/duckdb.git duckdb
fi
if [ "$(git -C duckdb describe --tags --exact-match)" != "v1.5.6" ] || [ "$(git -C duckdb rev-parse HEAD)" != "$DUCKDB_COMMIT" ]; then
  echo 'Expected DuckDB v1.5.6 in duckdb/' >&2
  exit 1
fi
cmake -S duckdb -B build/release -DCMAKE_BUILD_TYPE=Release \
  -DDUCKDB_EXTENSION_CONFIGS="$PWD/extension_config.cmake" \
  -DBUILD_UNITTESTS=OFF -DDISABLE_BUILTIN_EXTENSIONS=ON \
  -DEXTENSION_STATIC_BUILD="${EXTENSION_STATIC_BUILD:-OFF}"
cmake --build build/release --target decide_loadable_extension -j "${BUILD_JOBS:-4}"
