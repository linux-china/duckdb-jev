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

The idea comes from [pg-jev](https://github.com/realZachi/pg-jev) for PostgreSQL by Zachi;
this is an independent DuckDB implementation. See [docs/DESIGN.md](docs/DESIGN.md).

## Status

Work in progress — not yet published to the community extension repository.
