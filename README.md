# duckdb-decide

A native DuckDB extension that asks decision models yes/no, multiple-choice, and
rubric questions from SQL. It supports the **OpenAI Decisions API** (`gpt-6-luna`,
the default), **TypeSafe Jev**, and **Cloudflare Clef**. Each provider is a small
JSON file in [`providers/`](providers/), so other hosts with a supported API format
can be added without new code.

## Install from community

`decide` is available in the [DuckDB Community Extensions repository](https://duckdb.org/community_extensions/extensions/decide)
for DuckDB **2.0.0** on supported platforms:

```sql
INSTALL decide FROM community;
LOAD decide;
```

Community builds are signed and load in the normal DuckDB CLI. Configure your
provider credentials before evaluating rows; see [Setting API keys](#setting-api-keys).
TypeSafe Jev, provider files, `decision_providers()`, and per-provider secret
scopes need extension 0.2.1 or later. Check your installed version with
`SELECT extension_version FROM duckdb_extensions() WHERE extension_name = 'decide';`
and run `UPDATE EXTENSIONS (decide);` to pick up the latest community build.

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
Apple Silicon, and Windows (x86_64). Intel Macs and DuckDB-Wasm are not supported.
Fixes land on both lines while 1.5.x is supported.
See [RELEASING.md](RELEASING.md).

## Build from source

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
CREATE OR REPLACE SECRET openai_key (TYPE decisions, API_KEY 'your-openai-key', SCOPE 'decisions://openai');
-- Or export OPENAI_API_KEY before starting DuckDB. See "Setting API keys" for other providers.

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
listing; see [Setting API keys](#setting-api-keys). The `-unsigned` flag above is
for local build artifacts; community installation does not require it. The accepted
submission and maintenance procedure are documented in
[community/SUBMISSION.md](community/SUBMISSION.md).

## Providers

| Provider | `decisions_provider` | Default model | Key variable |
| --- | --- | --- | --- |
| OpenAI Decisions | `openai` (default) | `gpt-6-luna` | `OPENAI_API_KEY` |
| [TypeSafe Jev](https://docs.typesafe.ai/api) | `jev` | `jev-latest` | `TYPESAFE_API_KEY` |
| [Cloudflare Clef](https://developers.cloudflare.com/workers-ai/models/clef/) | `cloudflare` | `clef` | `CLOUDFLARE_API_TOKEN` |

`SELECT * FROM decision_providers()` lists the providers built into your binary,
with their formats, models, secret keys, and limits.

Pick a provider per session (or per query block), and optionally a model:

```sql
SET decisions_provider = 'jev';        -- 'openai' (default), 'jev', or 'cloudflare'
SET decisions_model = 'jev-1.14.0';    -- optional; empty uses the provider's default
SELECT decision_prob('Checkout fails for every customer.', 'Is this urgent?');
RESET decisions_model;                 -- before switching to another provider
```

`decisions_model` applies to whichever provider is selected, so reset it when you
switch.

### Setting API keys

Each provider looks for its key in this order:

1. a `decisions` secret scoped to that provider (`SCOPE 'decisions://<provider>'`);
2. a `decisions` secret with no scope;
3. the provider's environment variable.

> **Version note:** per-provider scopes need extension 0.2.1 or later. Version
> 0.2.0 and the 1.x line only look for secrets without a scope. On those versions,
> omit `SCOPE` (for example `CREATE OR REPLACE SECRET openai_key (TYPE decisions, API_KEY
> 'sk-...')`) or use the environment variables below. Those versions support only
> OpenAI and Cloudflare Clef.

**Scope every secret to its provider.** An unscoped secret is used for every
provider, so switching `decisions_provider` would send that key to a different
company. Keys are redacted in `duckdb_secrets()`, and the HTTP client never logs
them.

**OpenAI** uses an API key from the OpenAI platform.

```sql
CREATE OR REPLACE SECRET openai_key (TYPE decisions, API_KEY 'sk-...', SCOPE 'decisions://openai');
-- openai is the default provider; nothing else to set.
```

**TypeSafe Jev** uses an API key from the TypeSafe developer console.

```sql
CREATE OR REPLACE SECRET jev_key (TYPE decisions, API_KEY 'your-typesafe-key', SCOPE 'decisions://jev');
SET decisions_provider = 'jev';
```

**Cloudflare Clef** uses an API token that can call Workers AI, plus your account
ID (shown in the Cloudflare dashboard).

```sql
CREATE OR REPLACE SECRET cf_key (TYPE decisions, API_KEY 'your-workers-ai-token',
                      ACCOUNT_ID 'your-account-id', SCOPE 'decisions://cloudflare');
SET decisions_provider = 'cloudflare';
```

You can keep all three secrets at once and switch with `SET decisions_provider`.
Each key is sent only to its own provider.

**Environment variables** are used when no secret matches. Set them before
starting DuckDB:

```sh
export OPENAI_API_KEY='sk-...'
export TYPESAFE_API_KEY='your-typesafe-key'
export CLOUDFLARE_API_TOKEN='your-workers-ai-token'
export CLOUDFLARE_ACCOUNT_ID='your-account-id'
duckdb -unsigned
```

**Saving a key across sessions:** secrets are temporary by default. `CREATE
PERSISTENT SECRET` keeps one for later sessions. DuckDB stores it **unencrypted**
under `~/.duckdb/stored_secrets/` (in a file only your user can read). Prefer
environment variables or a secrets manager on shared machines. Run
`LOAD decide` (or the `LOAD` path above) at the start of each session before
using a stored secret.

```sql
CREATE PERSISTENT SECRET jev_key (TYPE decisions, API_KEY 'your-typesafe-key', SCOPE 'decisions://jev');
DROP PERSISTENT SECRET jev_key;   -- to remove it
```

**Checking which key a provider will use:**

```sql
SELECT name, persistent, scope FROM duckdb_secrets() WHERE type = 'decisions';
SELECT name FROM which_secret('decisions://jev', 'decisions');
```

If no secret matches and the environment variable is unset, queries fail with
`Create a decisions secret or set <VARIABLE>`. If you add a provider of your own,
it reads its key the same way, from the scope `decisions://<name>` and its
`key_env` variable.

Jev and Clef share the "System One" request format. Predicates become `noul`
questions, choices `choice`, and scores `score` (a probability-weighted index
starting at 0), so results mean the same as with OpenAI. Neither returns refusal
answers. Clef has extra limits: 64 questions per request (so batches are capped
at 64), question names of letters, digits, `_`, `.`, `-` only, and truncation of
long text to fit its 65,536-token context. Jev has no question cap and a
64K-token request limit (32K for the input plus any one question). Images are
not supported for any provider.

### Adding a provider

A host that speaks the OpenAI Decisions format or the System One format needs only
a JSON file in `providers/` and a rebuild. For example, a file for another Jev host
(an illustration, not shipped or tested) could be:

```json
{
  "name": "electronhub",
  "display_name": "Electron Hub Jev",
  "format": "systemone",
  "url": "https://api.electronhub.ai/v1/systemone",
  "default_model": "jev",
  "key_env": "ELECTRONHUB_API_KEY"
}
```

[`providers/README.md`](providers/README.md) documents every field. Provider files
are compiled into the extension and checked when it loads and in CI. API keys are
only sent to the URLs that shipped with your build. A new wire format needs a
small C++ class registered in `WireFormats()` in `src/include/providers.hpp`.

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
endpoints are not supported.

Default local builds resolve DuckDB symbols from the CLI at load time (verified
with Homebrew on macOS). For Linux, embedding runtimes, or runtimes that hide
C++ symbols, use `EXTENSION_STATIC_BUILD=ON ./scripts/build.sh` to build a larger
binary with DuckDB linked in. CI uses this mode to avoid unresolved RTTI symbols
when loading into the Linux CLI. C++ extension binaries must match
the DuckDB version and platform.

## Tests

```sh
DUCKDB_CLI=/opt/homebrew/bin/duckdb python3 test/integration.py
c++ -std=c++17 -Isrc/include -Ithird_party test/providers_test.cpp -o providers_test
./providers_test providers/*.json
```

Tests use a loopback mock service, replace the API key with a dummy credential,
and exercise the actual loadable extension. No live API calls are made by this
suite. The `decisions_endpoint` setting is empty (use the provider's URL) or
HTTP on `127.0.0.1` for these tests; redirects are disabled. `providers_test`
checks every provider file with the extension's own parser, along with the
validator's rejection of malformed files, without building DuckDB.

## References

- [Official Decisions guide](https://developers.openai.com/api/docs/guides/decisions)
- [Official API reference](https://developers.openai.com/api/reference/resources/decisions/methods/create)
- [TypeSafe System One API reference](https://docs.typesafe.ai/api)
- [Cloudflare Clef model page](https://developers.cloudflare.com/workers-ai/models/clef/)

Probabilities are model estimates; set thresholds using labeled examples for your
application. This project is an independent DuckDB integration, not an official
extension of OpenAI, TypeSafe, Cloudflare, or DuckDB.
