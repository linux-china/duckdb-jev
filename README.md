# jev — natural-language `WHERE` clauses for DuckDB

`jev` lets you filter, rank and classify rows with plain English. Every row is judged by
[TypeSafe's Jev](https://docs.typesafe.ai), a System One model that returns calibrated
probabilities instead of generated text. No index, no embeddings, no vector column.

```sql
INSTALL jev FROM community;
LOAD jev;
SET jev_api_key = 'your-key';   -- or export TYPESAFE_API_KEY

SELECT * FROM people p WHERE jev(p, 'the name is European');
SELECT subject, jev_prob(t, 'the customer is angry') AS p FROM tickets t ORDER BY p DESC LIMIT 20;
SELECT jev_choice(t, 'which team should handle this?', ['billing', 'technical', 'security']) AS team, count(*)
FROM tickets t GROUP BY 1;
```

`jev()` is an ordinary boolean scalar, so it composes with `AND`, joins, `GROUP BY` and
`ORDER BY jev_prob(...)`.

## Functions

| Function | Returns | Notes |
|---|---|---|
| `jev(rec, condition)` | BOOLEAN | `jev_prob(...) >= jev_threshold` |
| `jev(rec, condition, threshold)` | BOOLEAN | `jev_prob(...) >= coalesce(threshold, jev_threshold)` |
| `jev_prob(rec, condition)` | DOUBLE | P(row satisfies condition), 0..1 |
| `jev_score(rec, question, levels)` | DOUBLE | probability-weighted level index, 0..n-1 |
| `jev_score_norm(rec, question, levels)` | DOUBLE | the same, rescaled to 0..1 |
| `jev_choice(rec, question, options)` | VARCHAR | most likely option |
| `jev_confidence(rec, question, kind, options)` | DOUBLE | confidence of a score or choice answer |
| `jev_eval(rec, question, kind, options)` | JSON | the raw answer object |
| `jev_stats()` | JSON | requests, tokens, estimated cost, cache hits (per database instance) |
| `jev_cache_clear()` | BOOLEAN | forget cached judgments |
| `jev_version()` | VARCHAR | extension version |
| `jev_set_api_key(v)`, `jev_set_api_url(v)`, `jev_set_model(v)` | VARCHAR | for clients that cannot issue `SET` |

`rec` is any struct: a table alias (`jev(p, ...)`), a subquery alias, or
`struct_pack(...)`. `kind` is `'noul'`, `'score'` or `'choice'`; `options` is the list of
levels or choices and may be `NULL` for `'noul'`.

`jev` builds on DuckDB's `json` extension (autoloaded) to turn a row into the JSON
document the API sees.

## Settings

| Setting | Default | Meaning |
|---|---|---|
| `jev_api_key` | env `TYPESAFE_API_KEY` | TypeSafe API key |
| `jev_api_url` | `https://api.typesafe.ai/v1/systemone` | endpoint (proxies, mocks) |
| `jev_model` | `jev-latest` | model name |
| `jev_threshold` | `0.5` | probability at which `jev()` is true |
| `jev_batch_size` | `40` | rows per API request |
| `jev_concurrency` | `6` | parallel requests per vector |
| `jev_timeout` | `90` | seconds per request |
| `jev_notices` | `true` | print one line per API request to stderr |

## How it works

DuckDB hands the extension up to 2048 rows at a time. Rows that share a question, a kind
and a set of options are grouped, identical rows are judged once, and whatever is not
already cached is split into `jev_batch_size` batches that run on up to `jev_concurrency`
threads. Answers and usage counters live in a database-scoped cache that every connection
of that database shares; `jev_cache_clear()` empties it.

Requests retry 429, 529, 5xx and transport errors with exponential backoff (0.5 s doubling
to 8 s, six attempts). Any other status raises
`jev: TypeSafe API error <code> <body>`.

See [docs/DESIGN.md](docs/DESIGN.md) for the full design.

## Building and testing

```bash
GEN=ninja make release      # first build compiles DuckDB itself
./test/run.sh               # starts test/mock_api.py, runs make test, stops it again
```

The SQL tests never touch the real API: `test/mock_api.py` is a deterministic stand-in on
`127.0.0.1:8765`.

## Status

Work in progress — not yet published to the community extension repository.

The idea comes from [pg-jev](https://github.com/realZachi/pg-jev) for PostgreSQL by Zachi;
this is an independent DuckDB implementation.
