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
Linux, Windows, embedded-runtime loading, and static builds have not been tested.

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
