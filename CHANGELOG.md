# Changelog

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
* Requests retry 429, 529, 5xx and transport errors with exponential backoff;
  every other HTTP status raises `jev: TypeSafe API error <code> <body>`.

Not yet covered: `CREATE SECRET` integration, a persistent on-disk cache, and
HTTP from a WASM build (the WASM platforms are excluded from the release).
