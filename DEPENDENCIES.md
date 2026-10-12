# Dependencies

- DuckDB v1.5.6, commit `069cc9f9b5be802405797faecc284961b07c70ef` (pinned submodule; MIT).
- DuckDB extension-ci-tools v1.5-variegata, commit `3fd6109fc01555673b7c7123eba8d0d143fe8ce4` (pinned build-tool submodule; MIT).
- nlohmann/json v3.12.0 (vendored in third_party/json.hpp; MIT notice retained).
- libcurl (system library for local builds; vcpkg-managed for distribution; curl license).
- OpenSSL (curl TLS backend in vcpkg distribution builds; Apache-2.0 for current releases).
