# Dependencies

- DuckDB v2.0.0 LTS, commit `d54cbbe3df17e8e5ce68312f483b4155836112aa` (pinned submodule; MIT).
- DuckDB extension-ci-tools v2.0-cyanoptera, commit `78bd1e4abbd543822166d5b07644751ba1fc6e8d` (pinned build-tool submodule; MIT).
- nlohmann/json v3.12.0 (vendored in third_party/json.hpp; MIT notice retained).
- libcurl (system library for local builds; vcpkg-managed for distribution; curl license).
- OpenSSL (curl TLS backend in vcpkg distribution builds; Apache-2.0 for current releases).
