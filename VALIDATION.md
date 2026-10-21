# Validation — October 7, 2026

- Compiled the native loadable extension with Apple Clang on Apple Silicon.
- Loaded it into the Homebrew DuckDB 1.5.6 CLI.
- Passed the SQL/HTTP mock integration suite: predicates and thresholds,
  probabilities, choices, scores, row STRUCT input, SQL NULL, multiple rows,
  HTTP 429 retry, HTTP 401 without retry, invalid probabilities, mismatched
  names, unknown choice values, duplicate and NULL options, refusal handling,
  multiple questions, credential fallback, secret redaction, and external
  access restrictions.
- One live request to OpenAI `/v1/decisions` with `gpt-6-luna` and synthetic
  damaged-parcel text succeeded. `decision_prob` returned 0.98.

The live result verifies endpoint access and parsing, not statistical calibration.
Subsequent Linux static build and mock integration CI passed:
https://github.com/gzheng92/duckdb-decide/actions/runs/37670454336.
Windows and embedded-runtime loading remain unverified.

## Batching update

- Added configurable batches of 20 rows by default (extension range 1–128).
- Mock tests confirm 45 rows use three requests of 20, 20, and 5 questions,
  while batch size 1 uses individual requests.
- Verified distinct row results, row-specific thresholds, varying questions,
  choices, scores, skipped NULLs, 2,065 rows across DuckDB chunk boundaries,
  per-question refusals and invalid answers, and splitting oversized requests.
- Diagnosed the first Linux CI failure: loading the dynamically resolved
  extension failed with missing RTTI symbol `_ZTIN6duckdb18BaseScalarFunctionE`.
  CI now builds with `EXTENSION_STATIC_BUILD=ON`, linking DuckDB into the binary.

## Prompt caching probe — October 8, 2026

Ran `test/prompt_cache_live.py` against the live Decisions endpoint using synthetic
parcel descriptions and the extension's current 20-row batch payload. This is a
direct HTTP probe of equivalent requests, not instrumentation inside DuckDB.
Each of three conditions used four sequential requests with two-second gaps;
no cache controls were added. Full response usage is saved in
`test/prompt_cache_results.json`.

| Condition | Input tokens/request | Median seconds | Reported cached tokens |
| --- | ---: | ---: | ---: |
| Long rubric, identical repeated batch | 57,629 | 0.326 | 0 |
| Long rubric, changing rows | 57,629 | 0.336 | 0 |
| Short question, changing rows | 4,529 | 0.179 | 0 |

All 12 requests reported zero cache-write tokens as well. All 240 row decisions
matched the synthetic labels at threshold 0.5. Total reported input was 479,148
tokens (approximately $0.048 at the published base Decisions input rate).
The first identical request took 1.062 seconds; its repeats took 0.295–0.338
seconds. That timing change alone does not prove prompt caching. This small,
sequential probe shows no reported cache benefit for the current request format;
it does not establish that Decisions never caches or that another layout could
not benefit. The long rubric deliberately repeats criteria to stress reusable
instructions and is not a representative accuracy benchmark.

## Community preparation — October 12, 2026

- Standard `make release` completed on Apple Silicon using pinned DuckDB 1.5.6
  and extension-ci-tools v1.5-variegata. Both static registration and the loadable
  artifact are built; local dependencies came from the system curl library.
- Standard `make test_release` ran the offline SQLLogicTest: 17 assertions passed,
  with no skipped tests. Static registration is required for the pinned DuckDB
  generated loader to discover the extension through `require decide`.
- The HTTP mock suite passed with the installed Homebrew CLI loading the newly
  built static-linked artifact, exercising the loadable entry point separately.
- The current community descriptor parser accepted the draft descriptor. The
  vcpkg curl feature names were checked against the official curl port manifest.
- First preparation mock CI failed before compilation because its shallow
  submodule checkout omitted the release tag. Checkout now fetches full history,
  and the local build script fetches the expected tag when it is missing.
- The official native distribution workflow has been configured. Its current
  source revision must pass before submission; Windows, Intel macOS, Linux ARM,
  and the vcpkg distribution dependency resolution are not yet locally verified.

## DuckDB 2.0.0 LTS port — October 21, 2026

- Moved the pinned DuckDB submodule to `v2.0.0` (`d54cbbe3df`) and
  extension-ci-tools to `v2.0-cyanoptera` (`78bd1e4abb`).
- Source changes for the 2.0 C++ API: secret option keys are now `Identifier`
  values, and the threshold overload of `decision` is built without the
  brace-list `ScalarFunction` constructor that 2.0 made ambiguous.
- DuckDB 2.0 requires scalar functions that can throw to call `SetFallible()`.
  Without it, every HTTP, validation, or refusal error became an
  `INTERNAL Error`. All functions are now marked fallible. The mock suite
  rejects `INTERNAL Error` output, and the SQLLogicTest expects
  `Invalid Input Error`. The tightened mock suite failed before this fix.
- DuckDB 2.0 no longer derives its version from git tags. `scripts/build.sh`
  passes `DUCKDB_EXPLICIT_VERSION=v2.0.0`; otherwise local artifacts are stamped
  `v2.0.0-dev1` and official 2.0.0 binaries reject them. The distribution workflow
  exports `DUCKDB_VERSION` itself.
- On Linux x86_64 (GCC), a static build with the bundled 2.0.0 CLI passed the HTTP
  mock integration suite. The same artifact loaded in the official
  `duckdb==2.0.0` Python wheel (`source_id d54cbbe3df`). Functions registered,
  API keys were redacted, errors surfaced as `InvalidInputException`, and the
  connection stayed usable afterward.
- DuckDB errors raised inside the functions, such as the external-access
  `PermissionException`, now pass through unchanged. Previously they were
  re-wrapped as an `Invalid Input Error` that held a raw JSON error body.
- Standard `make release` and `make test_release` passed, including the
  SQLLogicTest. Windows, macOS, Linux ARM, and the vcpkg dependency resolution
  are covered only by the distribution workflow, not by local runs.
- Release lines: this branch (`v2.x`) ships `0.2.*` for DuckDB 2.0. DuckDB 1.5.6
  stays on `v1.x` as `0.1.*`. The 2.0 artifact reports version `0.2.0`; official
  DuckDB 2.0.0 rejects 1.5.6 builds, so the lines need separate releases.
