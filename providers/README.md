# Provider files

Each `*.json` file here describes one decision-model host and is compiled into the
extension (see `cmake_providers.cmake`). Name the file `<name>.json`. Users select
a provider with `SET decisions_provider = '<name>'`, and
`SELECT * FROM decision_providers()` lists the providers built in.

| Field | Required | Meaning |
| --- | --- | --- |
| `name` | yes | Value of `decisions_provider`, and the secret scope `decisions://<name>`. Lowercase letters, digits, `_`, `-`. |
| `display_name` | yes | Name used in error messages, such as `TypeSafe Jev HTTP 401`. |
| `format` | yes | Wire format: `openai` (OpenAI Decisions) or `systemone` (TypeSafe System One, used by Jev and Clef). |
| `url` | yes | HTTPS endpoint. May contain `{model}` and any declared `params` placeholders. |
| `default_model` | yes | Model id used when `decisions_model` is empty. |
| `models` | no | Allowed model ids. Omit to allow any well-formed id, for hosts that add versions over time. |
| `key_env` | yes | Environment variable holding the API key, used when no `decisions` secret matches. |
| `params` | no | Extra URL values, each `{"name", "pattern", "env"?}`. The value comes from the secret key `name`, or from `env`, and must fully match the regex `pattern`. |
| `max_questions` | no | Questions per request. Typed functions cap their batches at this size. |
| `question_names` | no | `{"pattern", "rule"}`: regex for question names, and the wording used in errors. |
| `response_wrapper` | no | Object key the host wraps its response in, such as `result` for Cloudflare's REST API. A bare response is accepted too. |
| `auth` | no | `{"header", "prefix"?}`. The default is `Authorization: Bearer <key>`. |
| `docs` | no | Link shown in `decision_providers()`. |

Unknown fields are rejected, so a typo fails the build check rather than being
ignored. Model ids are limited to letters, digits, `.`, `_`, `-`, `:`, `/` (no
`/` or `:` when `{model}` is in the URL), and `..` is rejected.

## Checking a file

```sh
c++ -std=c++17 -Isrc/include -Ithird_party test/providers_test.cpp -o providers_test
./providers_test providers/*.json
```

CI runs this check on every push. The extension also refuses to load if a
compiled-in file is invalid. Before you add a provider, confirm its request and
response formats against the host's API reference. Add a mock-server case to
`test/integration.py` if the host differs from an existing provider in more than
its URL, model, and key.

## Security

The URL comes only from these compiled-in files. At runtime, `decisions_endpoint`
can only point at `http://127.0.0.1` for tests, so SQL cannot redirect a stored
API key to another host. A secret without `SCOPE` matches every provider; scope
keys with `SCOPE 'decisions://<name>'` so each key goes only to its own provider.
