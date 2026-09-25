# Changelog

## 0.1.1

* Add jev secret type: `create secret (type jev, API_URL 'https://api.typesafe.ai/v1/systemone', API_KEY 'xxx', MODEL 'jev-latest' );`
* Remove `jev_api_key`, `jev_api_url`, `jev_set_api_key()`, `jev_set_api_url()`, and use jev secret instead.
* Every request's `state` carries a `timestamp` of when it was built: local
  time with microseconds and the local UTC offset, DuckDB TIMESTAMPTZ style
  (`2026-09-24 12:03:47.461842+08`).

## 0.1.0

First release.

* `jev(rec, condition [, threshold])`, `jev_prob`, `jev_score`, `jev_score_norm`,
  `jev_choice`, `jev_confidence` and `jev_eval`: scalar macros that judge a row
  with plain English through TypeSafe's Jev System One model.
* `jev_stats()`, `jev_cache_clear()` and `jev_version()`.
* `jev_set_api_key()`, `jev_set_api_url()` and `jev_set_model()` for clients that
  cannot issue `SET`.
* Settings `jev_api_key` (falling back to `TYPESAFE_API_KEY`), `jev_api_url`,
  `jev_model`, `jev_threshold`, `jev_batch_size`, `jev_concurrency`,
  `jev_timeout` and `jev_notices`.
* Rows are batched per request and requested in parallel; answers are cached per
  `(question, kind, options)` and row for the lifetime of the database instance.
* Requests retry 429, 529, 5xx and transport errors with exponential backoff, or
  after the `Retry-After` a 429/503 asks for (capped at 30 s); every other HTTP
  status raises `jev: TypeSafe API error <code> <body>`.
* `jev_stats()` returns `requests` (successful responses), `retries`, `batches`,
  `rows_evaluated`, `cache_hits`, token counts, `estimated_cost_usd`, `api_ms`
  (round trips only), `errors` and `cache_entries`.
* Built against DuckDB 1.5.5.

Not yet covered: `CREATE SECRET` integration, a persistent on-disk cache, and
HTTP from a WASM build (the WASM platforms are excluded from the release).
