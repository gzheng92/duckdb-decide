# Contributing

Clone with `git clone --recurse-submodules`, or run
`git submodule update --init --recursive` in an existing checkout.
DuckDB and its extension CI tools are pinned submodules. Do not update their refs
without checking API compatibility and distribution builds.

Use CMake, a C++17 compiler, and libcurl development headers for local builds.
The community CI resolves curl and its TLS dependencies through `vcpkg.json`.

```sh
CMAKE_BUILD_PARALLEL_LEVEL=4 make release
make test_release
DUCKDB_CLI="$PWD/build/release/duckdb" python3 test/integration.py
```

The SQLLogicTests run without credentials or network access. The Python suite
uses a loopback mock to check HTTP behavior, batching, and result mapping.
Changes to those paths should pass both suites. Live tests are opt-in, require
`OPENAI_API_KEY`, and incur charges; never include credentials or private inputs
in test fixtures, logs, commits, or issues.

Report bugs through GitHub issues with DuckDB version, platform, and a minimal
synthetic SQL example. For security issues involving credentials or data exposure,
use GitHub's private vulnerability reporting if enabled, or contact the maintainer
privately; do not post secret values in public issues.

Distribution compatibility is checked through DuckDB's reusable native-platform
workflow. Browser/Wasm builds are excluded because the extension uses libcurl and
native environment credentials. Adding a platform requires successful build and
offline test results, not just a change to the descriptor.
