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
