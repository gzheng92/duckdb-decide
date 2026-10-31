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
native environment credentials. Intel macOS (`osx_amd64`) is not supported; DuckDB's
2.0 build tools no longer build it by default. Excluded platforms are listed in
`release.env` and checked against the workflows and descriptor. Adding a platform requires successful build and
offline test results, not just a change to the descriptor.

DuckDB's build tools rebuild their Linux Docker image on every run, downloading
packages from public mirrors. When a mirror is down, the build fails before any of
this repository's code compiles. The `Retry infrastructure failures` workflow re-runs
the failed jobs once when the failure log shows such an outage. Any other failure,
or a second failure, still needs a look.

Providers are JSON files in `providers/`, compiled into the extension. Adding a host
that speaks an existing wire format needs only a new file; see
[providers/README.md](providers/README.md). Run `test/providers_test.cpp` against
`providers/*.json` (CI does) and add mock coverage in `test/integration.py` when a
host differs from existing providers in more than its URL, model, and key. Confirm
formats against the host's API reference, and never add live calls to the tests.
