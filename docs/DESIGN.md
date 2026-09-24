# duckdb-jev — design

**Status:** approved 2026-09-17. Idea credit: [pg-jev](https://github.com/realZachi/pg-jev)
(natural-language `WHERE` clauses for PostgreSQL). This is a clean-room DuckDB
implementation of the same idea, not a port of that code.

## Goal

Filter, rank and classify DuckDB rows with plain English, judged by
[TypeSafe's Jev](https://docs.typesafe.ai) System One model, which returns
calibrated probabilities rather than generated text:

```sql
INSTALL jev FROM community; LOAD jev;
create secret(type jev, API_KEY 'your-key');     -- or TYPESAFE_API_KEY in the environment

SELECT * FROM people p WHERE jev(p, 'the name is European');
SELECT subject, jev_prob(t, 'the customer is angry') AS p FROM tickets t ORDER BY p DESC LIMIT 20;
SELECT jev_choice(t, 'which team should handle this?', ['billing','technical','security']) AS team, count(*)
FROM tickets t GROUP BY 1;
SELECT name, jev_score(p, 'how luxurious is this product?', ['budget','mid-range','premium','luxury']) FROM products p;
```

`jev()` is an ordinary boolean scalar, so it composes with `AND`, joins,
`GROUP BY`, `ORDER BY jev_prob(...)`.

## Shape: a native C++ extension

Built from `duckdb/extension-template` (CMake, vcpkg), distributed through
`duckdb/community-extensions` so `INSTALL jev FROM community` works on every
platform DuckDB ships (Linux, macOS, Windows, WASM). HTTPS via DuckDB's
vendored cpp-httplib (`duckdb/third_party/httplib`) linked against OpenSSL
from vcpkg, JSON via DuckDB's vendored yyjson — the same recipe the
`open_prompt` community extension uses. Rust (C API template) was rejected:
narrower API and no HTTPS precedent in the community repo.

## SQL surface

| Function | Returns | Notes |
|---|---|---|
| `jev(rec, condition [, threshold])` | BOOLEAN | `jev_prob(...) >= coalesce(threshold, jev_threshold)` |
| `jev_prob(rec, condition)` | DOUBLE | P(row satisfies condition), 0..1 |
| `jev_score(rec, question, levels VARCHAR[])` | DOUBLE | probability-weighted level index, 0..n-1 |
| `jev_score_norm(rec, question, levels)` | DOUBLE | same, 0..1 |
| `jev_choice(rec, question, options VARCHAR[])` | VARCHAR | most likely option |
| `jev_confidence(rec, question, kind, options)` | DOUBLE | confidence of a score/choice answer |
| `jev_eval(rec, question, kind, options)` | JSON | raw answer object |
| `jev_stats()` | JSON | requests, tokens, est. cost, cache hits (per database instance) |
| `jev_cache_clear()` | BOOLEAN | forget cached judgments |
| `jev_version()` | VARCHAR | extension version |

`rec` is any struct: a table alias (`jev(p, ...)`), a subquery alias, or
`struct_pack(...)`. The row-taking functions are **scalar macros** registered
by the extension over one internal vectorized function:

```
jev_eval_json(row_json VARCHAR, question VARCHAR, kind VARCHAR, options VARCHAR[]) -> VARCHAR (JSON text)
```

with `row_json := to_json(rec)::VARCHAR` (DuckDB's built-in `json`
extension; autoloaded). `row`/`rec` naming: `row` is reserved in DuckDB —
macros use `rec`.

## Settings (extension options, `SET jev_<name> = ...`)

| Setting | Default | Meaning |
|---|---|---|
| `jev_threshold` | `0.5` | probability at which `jev()` is true |
| `jev_batch_size` | `40` | rows per API request |
| `jev_concurrency` | `6` | parallel requests per chunk |
| `jev_timeout` | `90` | seconds per request |
| `jev_notices` | `true` | print one line per batch run (rows, requests, tokens, est. cost, ms) |

Registered with `config.AddExtensionOption(...)`; read per call through
`ClientContext`'s config.

## Evaluation (per vector chunk)

DuckDB calls the scalar function with a chunk of up to 2048 rows, which
replaces pg-jev's whole-table read-ahead. Per chunk:

1. Cache key = `(question, kind, options)`; row key = sha1(row_json)
   (DuckDB has `Blob`/crypto helpers; a 64-bit FNV/xxhash of the JSON is
   acceptable since keys are per-question).
2. Rows not in the cache are grouped `jev_batch_size` per request; requests run
   on `jev_concurrency` threads (`std::thread`, join before returning — no
   TaskScheduler dependency).
3. Request body (TypeSafe System One):
   `{"model", "state": {"condition": q, "rows": [...], "timestamp": now}` (noul) or
   `{"state": {"rows": [...], "timestamp": now}}` (score/choice), `"questions": {"r0": ..., "r1": ...}` —
   `timestamp` is when the request was built: local time with microseconds and
   the local UTC offset, DuckDB TIMESTAMPTZ style (`2026-09-24 12:03:47.461842+08`).
   noul: `{"type":"noul","instructions":"Does the record `rows[i]` satisfy the condition stated in `condition`?","criteria":{"true":..,"false":..}}`;
   score: `{"type":"score","instructions":"Rate the record `rows[i]`: <question>","criteria":[levels]}`;
   choice: `{"type":"choice","instructions":"For the record `rows[i]`: <question>","criteria":{option: null}}`.
   Headers: `Authorization: Bearer`, `Content-Type: application/json`, `User-Agent: duckdb-jev/<version>`.
4. Retry: 429/529/5xx and transport errors with exponential backoff (0.5 s
   doubling to 8 s, 6 attempts); other HTTP errors raise
   `InvalidInputException("jev: TypeSafe API error <code> <body[:300]>")`.
5. Answers stored in the cache; stats updated (requests, input/output tokens,
   rows_evaluated, cache_hits, api_ms, batches, errors); `estimated_cost_usd`
   = input_tokens × 0.042 / 1e6.
6. NULL row → NULL result. Empty condition → error.

State (cache + stats) lives in a `DatabaseInstance`-scoped object (registered
via `ObjectCache`), guarded by a mutex; shared by all connections of that
database, cleared by `jev_cache_clear()`.

## Tests

`test/sql/*.test` (SQLLogicTest, run by `make test`) against a deterministic
mock server `test/mock_api.py` (our own; rules: noul → 0.9 if the last word of
the condition appears in the row JSON else 0.1; score/choice → index = length
of row JSON mod n; `Authorization` must be `Bearer test-key`; `state.timestamp`
must look like a local timestamp else 400; a condition
containing `trigger422` → HTTP 422). The test runner starts the mock on
127.0.0.1:8765 before `make test` (`test/run.sh`; CI job step). Cases: version,
predicate, prob, threshold arg + setting, batch_size → request count,
score/score_norm/choice/eval/confidence, subquery alias, NULL row, cache hits,
cache clear, missing key error, 422 error, plain-HTTP mock URL.

## Repo layout

```
src/jev_extension.cpp, src/include/jev_extension.hpp   registration, macros, options
src/jev_client.cpp/.hpp                                  HTTP + JSON request/response
src/jev_state.hpp                                        cache + stats
test/sql/jev.test, test/mock_api.py, test/run.sh
description.yml                                          community-extensions metadata
docs/DESIGN.md, README.md, CHANGELOG.md, LICENSE (MIT)
```

CI: template's `MainDistributionPipeline.yml` (DuckDB v1.5.5, all platforms)
plus a mock-server step before tests.

## Out of scope (follow-ups)

Secrets-manager integration (`CREATE SECRET`), async/`TaskScheduler` request
fan-out, a persistent on-disk cache, WASM HTTP (httplib is unavailable in
WASM; the WASM build will load but API calls raise), token budgeting.
