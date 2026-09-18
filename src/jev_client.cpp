#include "jev_client.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"

#include "yyjson.hpp"

#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <thread>

namespace duckdb {

using namespace duckdb_yyjson; // NOLINT

namespace {

//! Retry policy from docs/DESIGN.md: 6 attempts, 0.5s doubling to 8s.
constexpr idx_t MAX_ATTEMPTS = 6;
constexpr int64_t FIRST_BACKOFF_MS = 500;
constexpr int64_t MAX_BACKOFF_MS = 8000;
//! Upper bound on a Retry-After the server asks for.
constexpr int64_t MAX_RETRY_AFTER_SECONDS = 30;

string GetStringSetting(ClientContext &context, const char *name, const string &fallback) {
	Value value;
	if (!context.TryGetCurrentSetting(name, value) || value.IsNull()) {
		return fallback;
	}
	auto result = value.ToString();
	return result.empty() ? fallback : result;
}

idx_t GetIdxSetting(ClientContext &context, const char *name, idx_t fallback, idx_t maximum) {
	Value value;
	if (!context.TryGetCurrentSetting(name, value) || value.IsNull()) {
		return fallback;
	}
	auto result = value.GetValue<int64_t>();
	if (result <= 0) {
		throw InvalidInputException("jev: %s must be >= 1", name);
	}
	return MinValue<idx_t>(NumericCast<idx_t>(result), maximum);
}

//! Splits "https://host:port/path" into the part httplib's Client takes and the request path.
void SplitUrl(const string &url, string &base, string &path) {
	auto rest = url;
	string scheme;
	auto scheme_end = rest.find("://");
	if (scheme_end != string::npos) {
		scheme = rest.substr(0, scheme_end);
		rest = rest.substr(scheme_end + 3);
	}
	auto path_start = rest.find('/');
	if (path_start == string::npos) {
		base = rest;
		path = "/";
	} else {
		base = rest.substr(0, path_start);
		path = rest.substr(path_start);
	}
	// No scheme means plain HTTP - a mock or a local proxy.
	base = (scheme.empty() ? "http" : scheme) + "://" + base;
}

bool ShouldRetry(int status) {
	return status == 429 || status == 529 || status >= 500;
}

//! Retry-After in whole seconds, capped; 0 when the header is absent or unusable
//! (the HTTP-date form is not supported). Only 429 and 503 carry it in practice.
int64_t RetryAfterMs(const duckdb_httplib_openssl::Response &res) {
	if (res.status != 429 && res.status != 503) {
		return 0;
	}
	auto header = res.get_header_value("Retry-After");
	if (header.empty()) {
		return 0;
	}
	try {
		auto seconds = std::stoll(header);
		if (seconds <= 0) {
			return 0;
		}
		return MinValue<int64_t>(seconds, MAX_RETRY_AFTER_SECONDS) * 1000;
	} catch (std::exception &) {
		return 0;
	}
}

string Truncate(const string &body, idx_t limit) {
	return body.size() <= limit ? body : body.substr(0, limit);
}

string BuildRequestBody(const JevConfig &config, const string &question, const string &kind,
                        const vector<string> &options, const vector<string> &rows) {
	auto doc = yyjson_mut_doc_new(nullptr);
	if (!doc) {
		throw InvalidInputException("jev: out of memory building the request");
	}
	auto root = yyjson_mut_obj(doc);
	yyjson_mut_doc_set_root(doc, root);
	yyjson_mut_obj_add_strcpy(doc, root, "model", config.model.c_str());

	auto state = yyjson_mut_obj(doc);
	if (kind == "noul") {
		yyjson_mut_obj_add_strcpy(doc, state, "condition", question.c_str());
	}
	auto row_array = yyjson_mut_arr(doc);
	for (auto &row : rows) {
		// The row is already JSON text (to_json(rec)), so it goes in verbatim.
		yyjson_mut_arr_append(row_array, yyjson_mut_rawcpy(doc, row.c_str()));
	}
	yyjson_mut_obj_add_val(doc, state, "rows", row_array);
	yyjson_mut_obj_add_val(doc, root, "state", state);

	auto questions = yyjson_mut_obj(doc);
	for (idx_t i = 0; i < rows.size(); i++) {
		auto row_ref = "`rows[" + to_string(i) + "]`";
		auto question_obj = yyjson_mut_obj(doc);
		yyjson_mut_obj_add_strcpy(doc, question_obj, "type", kind.c_str());
		if (kind == "noul") {
			auto instructions = "Does the record " + row_ref + " satisfy the condition stated in `condition`?";
			yyjson_mut_obj_add_strcpy(doc, question_obj, "instructions", instructions.c_str());
			auto criteria = yyjson_mut_obj(doc);
			yyjson_mut_obj_add_strcpy(doc, criteria, "true", "The record satisfies the condition.");
			yyjson_mut_obj_add_strcpy(doc, criteria, "false", "The record does not satisfy the condition.");
			yyjson_mut_obj_add_val(doc, question_obj, "criteria", criteria);
		} else if (kind == "score") {
			auto instructions = "Rate the record " + row_ref + ": " + question;
			yyjson_mut_obj_add_strcpy(doc, question_obj, "instructions", instructions.c_str());
			auto criteria = yyjson_mut_arr(doc);
			for (auto &level : options) {
				yyjson_mut_arr_append(criteria, yyjson_mut_strcpy(doc, level.c_str()));
			}
			yyjson_mut_obj_add_val(doc, question_obj, "criteria", criteria);
		} else {
			auto instructions = "For the record " + row_ref + ": " + question;
			yyjson_mut_obj_add_strcpy(doc, question_obj, "instructions", instructions.c_str());
			auto criteria = yyjson_mut_obj(doc);
			for (auto &option : options) {
				yyjson_mut_obj_add(criteria, yyjson_mut_strcpy(doc, option.c_str()), yyjson_mut_null(doc));
			}
			yyjson_mut_obj_add_val(doc, question_obj, "criteria", criteria);
		}
		auto key = "r" + to_string(i);
		yyjson_mut_obj_add(questions, yyjson_mut_strcpy(doc, key.c_str()), question_obj);
	}
	yyjson_mut_obj_add_val(doc, root, "questions", questions);

	size_t length = 0;
	auto text = yyjson_mut_write(doc, 0, &length);
	yyjson_mut_doc_free(doc);
	if (!text) {
		throw InvalidInputException("jev: could not serialize the request body");
	}
	string body(text, length);
	free(text);
	return body;
}

//! Pulls the per-row answers and the usage block out of a 200 response.
void ParseResponse(const string &body, idx_t row_count, JevResponse &response) {
	auto doc = yyjson_read(body.c_str(), body.size(), 0);
	if (!doc) {
		throw InvalidInputException("jev: TypeSafe API returned invalid JSON: %s", Truncate(body, 300));
	}
	auto root = yyjson_doc_get_root(doc);
	auto answers = root ? yyjson_obj_get(root, "answers") : nullptr;
	if (!answers || !yyjson_is_obj(answers)) {
		yyjson_doc_free(doc);
		throw InvalidInputException("jev: TypeSafe API response has no answers: %s", Truncate(body, 300));
	}
	for (idx_t i = 0; i < row_count; i++) {
		auto key = "r" + to_string(i);
		auto answer = yyjson_obj_get(answers, key.c_str());
		if (!answer) {
			yyjson_doc_free(doc);
			throw InvalidInputException("jev: TypeSafe API response is missing answer %s", key);
		}
		size_t length = 0;
		auto text = yyjson_val_write(answer, 0, &length);
		if (!text) {
			yyjson_doc_free(doc);
			throw InvalidInputException("jev: could not serialize answer %s", key);
		}
		response.answers.emplace_back(text, length);
		free(text);
	}
	auto usage = yyjson_obj_get(root, "usage");
	if (usage && yyjson_is_obj(usage)) {
		auto input_tokens = yyjson_obj_get(usage, "input_tokens");
		auto output_tokens = yyjson_obj_get(usage, "output_tokens");
		if (input_tokens) {
			response.usage.input_tokens = yyjson_get_sint(input_tokens);
		}
		if (output_tokens) {
			response.usage.output_tokens = yyjson_get_sint(output_tokens);
		}
	}
	yyjson_doc_free(doc);
}

} // namespace

const char *JevVersion() {
#ifdef EXT_VERSION_JEV
	return EXT_VERSION_JEV;
#else
	return "0.1.0-dev";
#endif
}

JevConfig JevGetConfig(ClientContext &context) {
	JevConfig config;
	config.api_key = GetStringSetting(context, "jev_api_key", "");
	if (config.api_key.empty()) {
		auto from_env = std::getenv("TYPESAFE_API_KEY");
		if (from_env) {
			config.api_key = from_env;
		}
	}
	config.api_url = GetStringSetting(context, "jev_api_url", "https://api.typesafe.ai/v1/systemone");
	config.model = GetStringSetting(context, "jev_model", "jev-latest");
	config.batch_size = GetIdxSetting(context, "jev_batch_size", 40, NumericLimits<idx_t>::Maximum());
	config.concurrency = GetIdxSetting(context, "jev_concurrency", 6, JEV_MAX_CONCURRENCY);
	config.timeout = GetIdxSetting(context, "jev_timeout", 90, NumericLimits<idx_t>::Maximum());

	Value notices;
	if (context.TryGetCurrentSetting("jev_notices", notices) && !notices.IsNull()) {
		config.notices = notices.GetValue<bool>();
	}
	return config;
}

JevResponse JevPostBatch(const JevConfig &config, const string &question, const string &kind,
                         const vector<string> &options, const vector<string> &rows) {
	auto request_body = BuildRequestBody(config, question, kind, options, rows);

	string base, path;
	SplitUrl(config.api_url, base, path);

	duckdb_httplib_openssl::Client client(base.c_str());
	client.set_read_timeout(NumericCast<time_t>(config.timeout), 0);
	client.set_write_timeout(NumericCast<time_t>(config.timeout), 0);
	client.set_connection_timeout(NumericCast<time_t>(config.timeout), 0);
	client.set_follow_location(true);
	client.set_keep_alive(true);

	duckdb_httplib_openssl::Headers headers = {
	    {"Authorization", "Bearer " + config.api_key},
	    // Content-Type is passed to Post() below; listing it here too would send the
	    // header twice, which api.typesafe.ai rejects with a 422 (body read as a string).
	    {"User-Agent", string("duck-jev/") + JevVersion()},
	};

	string last_error;
	auto backoff_ms = FIRST_BACKOFF_MS;
	//! Only the round trips count towards api_ms - the sleeps in between do not.
	int64_t api_ms = 0;
	int64_t sleep_ms = 0;
	for (idx_t attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
		if (attempt > 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
		}
		auto started = std::chrono::steady_clock::now();
		auto res = client.Post(path.c_str(), headers, request_body, "application/json");
		api_ms += NumericCast<int64_t>(
		    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
		sleep_ms = backoff_ms;
		backoff_ms = MinValue<int64_t>(backoff_ms * 2, MAX_BACKOFF_MS);
		if (!res) {
			last_error = StringUtil::Format("transport error (%s) contacting %s",
			                                duckdb_httplib_openssl::to_string(res.error()), config.api_url);
			continue;
		}
		if (res->status == 200) {
			JevResponse response;
			ParseResponse(res->body, rows.size(), response);
			response.api_ms = api_ms;
			response.retries = NumericCast<int64_t>(attempt);
			return response;
		}
		if (!ShouldRetry(res->status)) {
			throw InvalidInputException("jev: TypeSafe API error %d %s", res->status, Truncate(res->body, 300));
		}
		// A server that says when to come back wins over our own schedule.
		auto retry_after = RetryAfterMs(*res);
		if (retry_after > 0) {
			sleep_ms = retry_after;
		}
		last_error = StringUtil::Format("TypeSafe API error %d %s", res->status, Truncate(res->body, 300));
	}
	throw InvalidInputException("jev: %s (gave up after %llu attempts)", last_error, MAX_ATTEMPTS);
}

} // namespace duckdb
