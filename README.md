# duckdb-decide

A native DuckDB extension for the **OpenAI Decisions API public beta**.
Uses `https://api.openai.com/v1/decisions` and `gpt-6-luna`.

## Build and load

Requires Git, CMake, a C++17 compiler, libcurl development headers, Python 3 for
mock tests, and DuckDB **1.5.6**. On an Apple Silicon Mac, Homebrew's tools are
usually in `/opt/homebrew/bin`.

```sh
./scripts/build.sh
# Run with your installed DuckDB 1.5.6 CLI:
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

This initial version supports text and SQL records. It makes one synchronous
HTTP request per non-NULL row, with no persistent cache or cross-row batching.
The API's questions share one input; combining unrelated records would change
the question's evidence. Functions are marked volatile to prevent the SQL
optimizer from precomputing network calls. Repeating calls can incur repeated
charges, so materialize results you plan to reuse.

Requests time out after 30 seconds (10-second connection timeout). HTTP 429 is
retried at most twice with backoff; transport failures are not retried because
billing status is ambiguous. Other HTTP failures are surfaced by status code;
response bodies, input text, and API keys are not logged by the HTTP client.
The extension limits request and response payloads to 16 MiB. It respects
DuckDB's `enable_external_access=false` setting. Images and custom regional
endpoints are not supported in this first version.

Default builds resolve DuckDB symbols from the CLI at load time. For embedding
runtimes that hide those symbols, use `EXTENSION_STATIC_BUILD=ON ./scripts/build.sh`
to build a larger binary with DuckDB linked in. C++ extension binaries must match
the DuckDB version and platform.

## Tests

```sh
DUCKDB_CLI=/opt/homebrew/bin/duckdb python3 test/integration.py
```

Tests use a loopback mock service, replace the API key with a dummy credential,
and exercise the actual loadable extension. No live API calls are made by this
suite. The `decisions_endpoint` setting accepts the fixed OpenAI endpoint or
HTTP on `127.0.0.1` for these tests; redirects are disabled.

## References

- [Official Decisions guide](https://developers.openai.com/api/docs/guides/decisions)
- [Official API reference](https://developers.openai.com/api/reference/resources/decisions/methods/create)

Probabilities are model estimates; set thresholds using labeled examples for your
application. This project is an independent DuckDB integration, not an OpenAI or
DuckDB official extension.
