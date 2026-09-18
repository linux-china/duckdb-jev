//===----------------------------------------------------------------------===//
//                         duck-jev
//
// jev_state.hpp
//
// Per-database judgment cache and usage counters, kept in DuckDB's ObjectCache
// so every connection of a database instance shares them.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/storage/object_cache.hpp"

namespace duckdb {

//! 64-bit FNV-1a over the row JSON. DuckDB's own string Hash() collides on rows as
//! similar as {"id":60,...} and {"id":68,...}, which would hand one row another row's
//! judgment - see docs/DESIGN.md.
inline hash_t JevHashRow(const string &row_json) {
	hash_t hash = 14695981039346656037ULL;
	for (auto c : row_json) {
		hash ^= static_cast<hash_t>(static_cast<unsigned char>(c));
		hash *= 1099511628211ULL;
	}
	return hash;
}

struct JevStats {
	//! Successful responses only - a batch that gave up after its retries counts in errors.
	int64_t requests = 0;
	//! Attempts that had to be repeated before a response arrived.
	int64_t retries = 0;
	int64_t batches = 0;
	int64_t rows_evaluated = 0;
	int64_t cache_hits = 0;
	int64_t input_tokens = 0;
	int64_t output_tokens = 0;
	int64_t api_ms = 0;
	int64_t errors = 0;
};

//! One cached judgment. The row JSON is kept so a hash collision can be told apart from a
//! genuine hit - two different rows must never share an answer.
struct JevCachedAnswer {
	string row_json;
	string answer;
};

//! Judgments are cached per (question, kind, options) - the outer key - and per row hash within it.
class JevState : public ObjectCacheEntry {
public:
	//! Price of a million input tokens, in USD - see docs/DESIGN.md.
	static constexpr double USD_PER_INPUT_TOKEN = 0.042 / 1000000.0;

	static string ObjectType() {
		return "jev_state";
	}

	string GetObjectType() override {
		return ObjectType();
	}

	//! Never evict: the cache is small and losing it would silently cost money.
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx::Invalid();
	}

	static shared_ptr<JevState> Get(ClientContext &context) {
		return ObjectCache::GetObjectCache(context).GetOrCreate<JevState>(ObjectType());
	}

	//! Returns the cached answer for a row, or false when it has to be requested. A hash that
	//! belongs to a different row is a miss, not a hit.
	bool TryGetAnswer(const string &cache_key, hash_t row_hash, const string &row_json, string &answer) {
		lock_guard<mutex> guard(lock);
		auto entry = cache.find(cache_key);
		if (entry == cache.end()) {
			return false;
		}
		auto answer_entry = entry->second.find(row_hash);
		if (answer_entry == entry->second.end() || answer_entry->second.row_json != row_json) {
			return false;
		}
		answer = answer_entry->second.answer;
		stats.cache_hits++;
		return true;
	}

	void PutAnswer(const string &cache_key, hash_t row_hash, const string &row_json, const string &answer) {
		lock_guard<mutex> guard(lock);
		cache[cache_key][row_hash] = JevCachedAnswer {row_json, answer};
	}

	void Clear() {
		lock_guard<mutex> guard(lock);
		cache.clear();
	}

	idx_t CacheEntries() {
		lock_guard<mutex> guard(lock);
		idx_t total = 0;
		for (auto &entry : cache) {
			total += entry.second.size();
		}
		return total;
	}

	JevStats CopyStats() {
		lock_guard<mutex> guard(lock);
		return stats;
	}

	void RecordBatch(int64_t rows, int64_t input_tokens, int64_t output_tokens, int64_t api_ms, int64_t retries) {
		lock_guard<mutex> guard(lock);
		stats.batches++;
		stats.requests++;
		stats.retries += retries;
		stats.rows_evaluated += rows;
		stats.input_tokens += input_tokens;
		stats.output_tokens += output_tokens;
		stats.api_ms += api_ms;
	}

	//! A batch that never got an answer: it counts as an attempted batch and an error,
	//! but not as a request.
	void RecordFailedBatch() {
		lock_guard<mutex> guard(lock);
		stats.batches++;
		stats.errors++;
	}

private:
	mutex lock;
	unordered_map<string, unordered_map<hash_t, JevCachedAnswer>> cache;
	JevStats stats;
};

} // namespace duckdb
