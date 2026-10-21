# duckdb-decide

A native DuckDB extension for the **OpenAI Decisions API public beta**.
Uses `https://api.openai.com/v1/decisions` and `gpt-6-luna`.

## DuckDB compatibility

DuckDB loads an extension only into the DuckDB version it was built for, so each
DuckDB line has its own branch and release series:

| DuckDB | Branch | Extension releases |
| --- | --- | --- |
| 2.0.x (LTS) | [`v2.x`](https://github.com/gzheng92/duckdb-decide/tree/v2.x) | `v0.2.*` |
| 1.5.x | [`v1.x`](https://github.com/gzheng92/duckdb-decide/tree/v1.x) | `v0.1.*` |

Prebuilt binaries for each release are attached to
[GitHub Releases](https://github.com/gzheng92/duckdb-decide/releases); pick the release
matching your DuckDB version. Supported platforms: Linux (x86_64, ARM64), macOS on
Apple Silicon, and Windows (x86_64). Intel Macs and DuckDB-Wasm are not supported. Fixes land on both lines while 1.5.x is supported.
See [RELEASING.md](RELEASING.md).

## Build and load

Requires Git, CMake, a C++17 compiler, libcurl development headers, Python 3 for
mock tests, and DuckDB **2.0.0** (LTS). On an Apple Silicon Mac, Homebrew's tools are
usually in `/opt/homebrew/bin`.

Clone with `--recurse-submodules`, or run `git submodule update --init --recursive`.
The repo includes the standard DuckDB extension Makefile and a vcpkg manifest for
portable distribution builds: `make release` then `make test_release`.

```sh
./scripts/build.sh
# Run with your installed DuckDB 2.0.0 CLI:
duckdb -unsigned
```

```sql
LOAD 'build/release/extension/decide/decide.duckdb_extension';
CREATE SECRET openai_decisions (TYPE decisions, API_KEY 'your-key');
-- Alternatively, export OPENAI_API_KEY before starting DuckDB.

SELECT decision('A solar panel stores energy.', 'Does this mitigate climate change?');
SELECT decision_prob('A solar panel stores energy.', 'Does this mitigate climate change?');
SELECT decision('A solar panel stores energy.', 'Does this mitigate climate change?', 0.8);
SELECT decision_choice('A solar panel stores energy.', 'Which sector?',
                       ['energy', 'transportation', 'other']);
SELECT decision_score('The task fails but has a workaround.', 'How severe?',
                      ['cosmetic', 'workaround', 'blocked']);

-- A row STRUCT is passed as DuckDB's textual representation, preserving field names.
SELECT patent_id, decision_prob(p, 'Does this mitigate climate change?') AS green_prob
FROM patents p;
-- Use to_json(p) when you prefer JSON text as evidence.
```

Secrets are temporary by default, with API keys redacted from DuckDB's secret
listing. The extension has no community distribution or signing yet.
Community submission preparation and the draft descriptor are in
[community/SUBMISSION.md](community/SUBMISSION.md). `INSTALL decide FROM community`
will become available only after acceptance and publication.

## Cloudflare Clef (Workers AI)

The same functions can run on Cloudflare's open-weight
[Clef](https://developers.cloudflare.com/workers-ai/models/clef/) decision models
instead of OpenAI:

```sql
SET decisions_provider = 'cloudflare';
SET decisions_model = 'clef';   -- or 'clef-flash' (9B, lower latency)
CREATE SECRET cf_decisions (TYPE decisions, API_KEY 'workers-ai-token', ACCOUNT_ID 'your-account-id');
-- Alternatively export CLOUDFLARE_API_TOKEN and CLOUDFLARE_ACCOUNT_ID.
SELECT decision_prob('Checkout fails for every customer.', 'Is this urgent?');
```

Predicates map to Clef `noul` questions, choices to `choice`, and scores to `score`
(a probability-weighted index starting at 0), so results have the same meaning as
with OpenAI. Differences: batches are capped at Clef's 64 questions per request;
Clef truncates long text state to fit its 65,536-token context, so check long
inputs; Clef has no refusal answers; and `decision_answers` question names may
only use letters, digits, `_`, `.`, `-`. Images are not supported.

## Functions

| Function | SQL result |
| --- | --- |
| `decision(input, question[, threshold])` | BOOLEAN, probability >= threshold (default 0.5) |
| `decision_prob(input, question)` | DOUBLE, probability the condition is true |
| `decision_choice(input, question, labels)` | VARCHAR, one supplied label |
| `decision_score(input, question, ordered_labels)` | DOUBLE, weighted level index starting at 0 |
| `decision_answers(input, questions_json)` | VARCHAR containing the answers JSON array |

Any top-level NULL argument produces SQL NULL without making a request. Empty,
duplicate, or NULL option labels are errors. Typed functions raise an error on
refusal instead of turning it into false or zero. `decision_answers` preserves
refusal objects alongside successful answers. For option descriptions and multiple
independent questions about a shared input, pass the documented questions array:

```sql
SELECT decision_answers('A customer reports a broken screen.', '[
  {"type":"predicate","name":"damaged","instructions":"Does this report damage?"},
  {"type":"choice","name":"route","instructions":"Which team?","choices":[
    {"value":"support","description":"Faulty products"},
    {"value":"billing","description":"Payments and refunds"}
  ]}
]');
```

## Execution and limits

Typed functions batch up to **20 non-NULL rows per request** by default. Within
each DuckDB chunk, rows with identical questions and options are grouped. Each
request contains a JSON array of records, with one uniquely named question that
explicitly targets each record. Results are checked for matching names, types,
and counts before they are assigned back to SQL rows. NULL rows are skipped.
Large batches are split to stay within the 16 MiB request limit.

```sql
SET decisions_batch_size = 20; -- allowed extension range: 1 through 128
SET decisions_batch_size = 1;  -- disable cross-row batching; original input format
```

Batching shares evidence across records, so compare results against batch size 1
on representative labeled data before choosing a larger batch. This is an
extension technique using multiple API questions, not a native multi-input API.
`decision_answers` preserves user-defined question names and makes one request
per row; its explicit questions already share that row's input.

Requests execute sequentially within each chunk. There is no persistent cache or
extension-managed concurrency pool. Functions are marked volatile to prevent the
SQL optimizer from precomputing network calls. Repeating calls can incur repeated
charges, so materialize results you plan to reuse.

Requests time out after 30 seconds (10-second connection timeout). HTTP 429 is
retried at most twice with backoff; transport failures are not retried because
billing status is ambiguous. Other HTTP failures are surfaced by status code;
response bodies, input text, and API keys are not logged by the HTTP client.
The extension limits request and response payloads to 16 MiB. It respects
DuckDB's `enable_external_access=false` setting. Images and custom regional
endpoints are not supported in this first version.

Default local builds resolve DuckDB symbols from the CLI at load time (verified
with Homebrew on macOS). For Linux, embedding runtimes, or runtimes that hide
C++ symbols, use `EXTENSION_STATIC_BUILD=ON ./scripts/build.sh` to build a larger
binary with DuckDB linked in. CI uses this mode to avoid unresolved RTTI symbols
when loading into the Linux CLI. C++ extension binaries must match
the DuckDB version and platform.

## Tests

```sh
DUCKDB_CLI=/opt/homebrew/bin/duckdb python3 test/integration.py
```

Tests use a loopback mock service, replace the API key with a dummy credential,
and exercise the actual loadable extension. No live API calls are made by this
suite. The `decisions_endpoint` setting accepts the provider's fixed endpoint or
HTTP on `127.0.0.1` for these tests; redirects are disabled.

## References

- [Official Decisions guide](https://developers.openai.com/api/docs/guides/decisions)
- [Official API reference](https://developers.openai.com/api/reference/resources/decisions/methods/create)

Probabilities are model estimates; set thresholds using labeled examples for your
application. This project is an independent DuckDB integration, not an OpenAI or
DuckDB official extension.
