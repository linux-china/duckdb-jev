//===----------------------------------------------------------------------===//
//                         duck-jev
//
// jev_client.hpp
//
// Settings lookup and one HTTP round trip to the TypeSafe System One API.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"

namespace duckdb {

//! Upper bound on jev_concurrency. Every DuckDB thread evaluating the function opens its
//! own set of connections, so the real fan-out is DuckDB threads x jev_concurrency.
static constexpr idx_t JEV_MAX_CONCURRENCY = 64;

//! Extension settings, resolved once per call from the client context.
struct JevConfig {
	string api_key;
	string api_url;
	string model;
	idx_t batch_size = 40;
	idx_t concurrency = 6;
	idx_t timeout = 90;
	bool notices = true;
};

struct JevUsage {
	int64_t input_tokens = 0;
	int64_t output_tokens = 0;
};

struct JevResponse {
	//! One JSON answer object per row of the request, in request order.
	vector<string> answers;
	JevUsage usage;
	//! Time spent in HTTP round trips, excluding the backoff sleeps between them.
	int64_t api_ms = 0;
	//! Attempts that had to be repeated before this response arrived.
	int64_t retries = 0;
};

//! Reads jev_* settings; the API key falls back to the TYPESAFE_API_KEY environment variable.
JevConfig JevGetConfig(ClientContext &context);

//! The version this extension reports in jev_version() and its User-Agent.
const char *JevVersion();

//! POSTs one batch of rows with one question and returns one answer per row.
//! Retries 429/529/5xx and transport errors; throws InvalidInputException otherwise.
JevResponse JevPostBatch(const JevConfig &config, const string &question, const string &kind,
                         const vector<string> &options, const vector<string> &rows);

} // namespace duckdb
