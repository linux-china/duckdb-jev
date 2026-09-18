#define DUCKDB_EXTENSION_MAIN

#include "jev_extension.hpp"

#include "jev_client.hpp"
#include "jev_state.hpp"

#include "duckdb.hpp"
#include "duckdb/catalog/default/default_functions.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"

#include <iostream>
#include <thread>

namespace duckdb {

namespace {

//! One API request: a slice of the unique rows of one (question, kind, options) group.
struct JevBatch {
	idx_t group;
	idx_t offset;
	idx_t count;
	JevResponse response;
	std::exception_ptr error;
};

//! All rows of a chunk that share a question, a kind and a set of options.
struct JevGroup {
	string cache_key;
	string question;
	string kind;
	vector<string> options;
	//! Unique row JSON texts still to be requested.
	vector<string> rows;
	vector<hash_t> hashes;
	//! Result positions each of those rows has to be written to.
	vector<vector<idx_t>> targets;
	unordered_map<hash_t, idx_t> row_positions;
};

string BuildCacheKey(const string &question, const string &kind, const vector<string> &options) {
	string key = kind;
	key += '\x1f';
	key += question;
	for (auto &option : options) {
		key += '\x1e';
		key += option;
	}
	return key;
}

void RunBatches(const JevConfig &config, vector<JevGroup> &groups, vector<JevBatch> &batches) {
	auto thread_count = MinValue<idx_t>(MaxValue<idx_t>(config.concurrency, 1), batches.size());
	auto run_range = [&](idx_t thread_index) {
		for (idx_t i = thread_index; i < batches.size(); i += thread_count) {
			auto &batch = batches[i];
			auto &group = groups[batch.group];
			vector<string> rows(group.rows.begin() + NumericCast<int64_t>(batch.offset),
			                    group.rows.begin() + NumericCast<int64_t>(batch.offset + batch.count));
			try {
				batch.response = JevPostBatch(config, group.question, group.kind, group.options, rows);
			} catch (...) {
				batch.error = std::current_exception();
			}
		}
	};
	if (thread_count <= 1) {
		run_range(0);
		return;
	}
	vector<std::thread> threads;
	threads.reserve(thread_count);
	for (idx_t i = 0; i < thread_count; i++) {
		threads.emplace_back(run_range, i);
	}
	for (auto &thread : threads) {
		thread.join();
	}
}

//! jev_eval_json(row_json, question, kind, options) -> the raw answer object as JSON text.
void JevEvalJsonFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto config = JevGetConfig(context);
	auto jev_state = JevState::Get(context);
	auto count = args.size();

	UnifiedVectorFormat rows_format, question_format, kind_format, options_format;
	args.data[0].ToUnifiedFormat(count, rows_format);
	args.data[1].ToUnifiedFormat(count, question_format);
	args.data[2].ToUnifiedFormat(count, kind_format);
	args.data[3].ToUnifiedFormat(count, options_format);
	auto rows_data = UnifiedVectorFormat::GetData<string_t>(rows_format);
	auto questions_data = UnifiedVectorFormat::GetData<string_t>(question_format);
	auto kinds_data = UnifiedVectorFormat::GetData<string_t>(kind_format);
	auto options_data = UnifiedVectorFormat::GetData<list_entry_t>(options_format);

	auto options_size = ListVector::GetListSize(args.data[3]);
	UnifiedVectorFormat option_format;
	ListVector::GetEntry(args.data[3]).ToUnifiedFormat(options_size, option_format);
	auto option_data = UnifiedVectorFormat::GetData<string_t>(option_format);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetData<string_t>(result);
	auto &result_validity = FlatVector::Validity(result);

	vector<JevGroup> groups;
	unordered_map<string, idx_t> group_positions;

	for (idx_t i = 0; i < count; i++) {
		auto row_index = rows_format.sel->get_index(i);
		if (!rows_format.validity.RowIsValid(row_index)) {
			result_validity.SetInvalid(i);
			continue;
		}
		auto question_index = question_format.sel->get_index(i);
		if (!question_format.validity.RowIsValid(question_index)) {
			throw InvalidInputException("jev: empty question");
		}
		auto question = questions_data[question_index].GetString();
		if (question.find_first_not_of(" \t\n\r") == string::npos) {
			throw InvalidInputException("jev: empty question");
		}
		auto kind_index = kind_format.sel->get_index(i);
		auto kind = kind_format.validity.RowIsValid(kind_index) ? kinds_data[kind_index].GetString() : string("noul");
		if (kind != "noul" && kind != "score" && kind != "choice") {
			throw InvalidInputException("jev: unknown question kind '%s' - expected noul, score or choice", kind);
		}

		vector<string> options;
		auto options_index = options_format.sel->get_index(i);
		if (options_format.validity.RowIsValid(options_index)) {
			auto list = options_data[options_index];
			for (idx_t o = 0; o < list.length; o++) {
				auto option_index = option_format.sel->get_index(list.offset + o);
				if (option_format.validity.RowIsValid(option_index)) {
					options.push_back(option_data[option_index].GetString());
				}
			}
		}
		if (kind != "noul" && options.empty()) {
			throw InvalidInputException("jev: a %s question needs a non-empty list of options", kind);
		}

		auto row_json = rows_data[row_index].GetString();
		auto cache_key = BuildCacheKey(question, kind, options);
		auto row_hash = JevHashRow(row_json);

		string answer;
		if (jev_state->TryGetAnswer(cache_key, row_hash, answer)) {
			result_data[i] = StringVector::AddString(result, answer);
			continue;
		}

		auto group_entry = group_positions.find(cache_key);
		if (group_entry == group_positions.end()) {
			JevGroup group;
			group.cache_key = cache_key;
			group.question = question;
			group.kind = kind;
			group.options = options;
			group_positions[cache_key] = groups.size();
			group_entry = group_positions.find(cache_key);
			groups.push_back(std::move(group));
		}
		auto &group = groups[group_entry->second];
		auto row_entry = group.row_positions.find(row_hash);
		if (row_entry == group.row_positions.end()) {
			group.row_positions[row_hash] = group.rows.size();
			group.rows.push_back(row_json);
			group.hashes.push_back(row_hash);
			group.targets.emplace_back();
			group.targets.back().push_back(i);
		} else {
			group.targets[row_entry->second].push_back(i);
		}
	}

	if (groups.empty()) {
		return;
	}
	if (config.api_key.empty()) {
		throw InvalidInputException("jev: no API key. SET jev_api_key = '...' or export TYPESAFE_API_KEY.");
	}

	vector<JevBatch> batches;
	auto batch_size = MaxValue<idx_t>(config.batch_size, 1);
	for (idx_t g = 0; g < groups.size(); g++) {
		for (idx_t offset = 0; offset < groups[g].rows.size(); offset += batch_size) {
			JevBatch batch;
			batch.group = g;
			batch.offset = offset;
			batch.count = MinValue<idx_t>(batch_size, groups[g].rows.size() - offset);
			batches.push_back(std::move(batch));
		}
	}

	RunBatches(config, groups, batches);

	std::exception_ptr first_error;
	for (auto &batch : batches) {
		if (batch.error) {
			jev_state->RecordBatch(0, 0, 0, 0, true);
			if (!first_error) {
				first_error = batch.error;
			}
			continue;
		}
		jev_state->RecordBatch(NumericCast<int64_t>(batch.count), batch.response.usage.input_tokens,
		                       batch.response.usage.output_tokens, batch.response.api_ms, false);
		if (config.notices) {
			std::cerr << StringUtil::Format(
			                 "jev: %llu rows, 1 request, %lld input + %lld output tokens, $%.6f, %lld ms\n",
			                 (unsigned long long)batch.count, (long long)batch.response.usage.input_tokens,
			                 (long long)batch.response.usage.output_tokens,
			                 double(batch.response.usage.input_tokens) * JevState::USD_PER_INPUT_TOKEN,
			                 (long long)batch.response.api_ms)
			          << std::flush;
		}
		auto &group = groups[batch.group];
		for (idx_t r = 0; r < batch.count; r++) {
			auto &answer = batch.response.answers[r];
			auto row = batch.offset + r;
			jev_state->PutAnswer(group.cache_key, group.hashes[row], answer);
			for (auto target : group.targets[row]) {
				result_data[target] = StringVector::AddString(result, answer);
			}
		}
	}
	if (first_error) {
		std::rethrow_exception(first_error);
	}
}

void JevStatsFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto jev_state = JevState::Get(state.GetContext());
	auto stats = jev_state->CopyStats();
	auto json = StringUtil::Format(
	    "{\"requests\":%lld,\"batches\":%lld,\"rows_evaluated\":%lld,\"cache_hits\":%lld,\"input_tokens\":%lld,"
	    "\"output_tokens\":%lld,\"estimated_cost_usd\":%.9f,\"api_ms\":%lld,\"errors\":%lld,\"cache_entries\":%llu}",
	    (long long)stats.requests, (long long)stats.batches, (long long)stats.rows_evaluated,
	    (long long)stats.cache_hits, (long long)stats.input_tokens, (long long)stats.output_tokens,
	    double(stats.input_tokens) * JevState::USD_PER_INPUT_TOKEN, (long long)stats.api_ms, (long long)stats.errors,
	    (unsigned long long)jev_state->CacheEntries());
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] = StringVector::AddString(result, json);
}

void JevCacheClearFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	JevState::Get(state.GetContext())->Clear();
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<bool>(result)[0] = true;
}

void JevVersionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] = StringVector::AddString(result, JevVersion());
}

//! jev_set_api_key('...') and friends, for clients that cannot issue SET.
//! Session scope, so they behave exactly like SET jev_api_key = '...'.
void SetSetting(DataChunk &args, ExpressionState &state, Vector &result, const char *setting) {
	auto &context = state.GetContext();
	ExtensionOption option;
	if (!DBConfig::GetConfig(context).TryGetExtensionOption(setting, option)) {
		throw InvalidInputException("jev: setting %s is not registered", setting);
	}
	auto setting_index = option.setting_index.GetIndex();
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t value) {
		ClientConfig::GetConfig(context).user_settings.SetUserSetting(setting_index, Value(value.GetString()));
		return StringVector::AddString(result, string(setting) + " set");
	});
}

void JevSetApiKeyFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	SetSetting(args, state, result, "jev_api_key");
}

void JevSetApiUrlFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	SetSetting(args, state, result, "jev_api_url");
}

void JevSetModelFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	SetSetting(args, state, result, "jev_model");
}

// clang-format off
//! `row` is a reserved word in DuckDB, so the row parameter is called `rec`.
const DefaultMacro JEV_MACROS[] = {
    {DEFAULT_SCHEMA, "jev", {"rec", "condition", nullptr}, {{nullptr, nullptr}},
     "jev_prob(rec, condition) >= current_setting('jev_threshold')::DOUBLE"},
    {DEFAULT_SCHEMA, "jev", {"rec", "condition", "threshold", nullptr}, {{nullptr, nullptr}},
     "jev_prob(rec, condition) >= coalesce(threshold, current_setting('jev_threshold')::DOUBLE)"},
    {DEFAULT_SCHEMA, "jev_prob", {"rec", "condition", nullptr}, {{nullptr, nullptr}},
     "json_extract(jev_eval_json(to_json(rec)::VARCHAR, condition, 'noul', NULL::VARCHAR[]), '$.noul')::DOUBLE"},
    {DEFAULT_SCHEMA, "jev_score", {"rec", "question", "levels", nullptr}, {{nullptr, nullptr}},
     "json_extract(jev_eval_json(to_json(rec)::VARCHAR, question, 'score', levels), '$.score')::DOUBLE"},
    {DEFAULT_SCHEMA, "jev_score_norm", {"rec", "question", "levels", nullptr}, {{nullptr, nullptr}},
     "jev_score(rec, question, levels) / greatest(len(levels) - 1, 1)"},
    {DEFAULT_SCHEMA, "jev_choice", {"rec", "question", "options", nullptr}, {{nullptr, nullptr}},
     "json_extract_string(jev_eval_json(to_json(rec)::VARCHAR, question, 'choice', options), '$.choice')"},
    {DEFAULT_SCHEMA, "jev_confidence", {"rec", "question", "kind", "options", nullptr}, {{nullptr, nullptr}},
     "json_extract(jev_eval_json(to_json(rec)::VARCHAR, question, kind, options), '$.confidence')::DOUBLE"},
    {DEFAULT_SCHEMA, "jev_eval", {"rec", "question", "kind", "options", nullptr}, {{nullptr, nullptr}},
     "jev_eval_json(to_json(rec)::VARCHAR, question, kind, options)"},
};
// clang-format on

void RegisterMacro(ExtensionLoader &loader, idx_t start, idx_t overloads) {
	auto info =
	    DefaultFunctionGenerator::CreateInternalMacroInfo(array_ptr<const DefaultMacro>(JEV_MACROS + start, overloads));
	loader.RegisterFunction(*info);
}

void RegisterOptions(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption("jev_api_key",
	                          "TypeSafe API key; falls back to the TYPESAFE_API_KEY environment variable",
	                          LogicalType::VARCHAR, Value(""));
	config.AddExtensionOption("jev_api_url", "TypeSafe System One endpoint", LogicalType::VARCHAR,
	                          Value("https://api.typesafe.ai/v1/systemone"));
	config.AddExtensionOption("jev_model", "TypeSafe model name", LogicalType::VARCHAR, Value("jev-latest"));
	config.AddExtensionOption("jev_threshold", "Probability at which jev() is true", LogicalType::DOUBLE,
	                          Value::DOUBLE(0.5));
	config.AddExtensionOption("jev_batch_size", "Rows per API request", LogicalType::UBIGINT, Value::UBIGINT(40));
	config.AddExtensionOption("jev_concurrency", "Requests running in parallel per vector", LogicalType::UBIGINT,
	                          Value::UBIGINT(6));
	config.AddExtensionOption("jev_timeout", "Seconds allowed per API request", LogicalType::UBIGINT,
	                          Value::UBIGINT(90));
	config.AddExtensionOption("jev_notices", "Print one line per API request to stderr", LogicalType::BOOLEAN,
	                          Value::BOOLEAN(true));
}

void LoadInternal(ExtensionLoader &loader) {
	loader.SetDescription("Filter, rank and classify rows with plain English, judged by TypeSafe's Jev model");
	RegisterOptions(loader);

	// The one vectorized function every macro goes through.
	ScalarFunction eval_json(
	    "jev_eval_json",
	    {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR)},
	    LogicalType::VARCHAR, JevEvalJsonFunction);
	eval_json.stability = FunctionStability::VOLATILE;
	eval_json.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	loader.RegisterFunction(eval_json);

	ScalarFunction stats("jev_stats", {}, LogicalType::VARCHAR, JevStatsFunction);
	stats.stability = FunctionStability::VOLATILE;
	loader.RegisterFunction(stats);

	ScalarFunction cache_clear("jev_cache_clear", {}, LogicalType::BOOLEAN, JevCacheClearFunction);
	cache_clear.stability = FunctionStability::VOLATILE;
	loader.RegisterFunction(cache_clear);

	loader.RegisterFunction(ScalarFunction("jev_version", {}, LogicalType::VARCHAR, JevVersionFunction));

	ScalarFunction set_api_key("jev_set_api_key", {LogicalType::VARCHAR}, LogicalType::VARCHAR, JevSetApiKeyFunction);
	set_api_key.stability = FunctionStability::VOLATILE;
	loader.RegisterFunction(set_api_key);
	ScalarFunction set_api_url("jev_set_api_url", {LogicalType::VARCHAR}, LogicalType::VARCHAR, JevSetApiUrlFunction);
	set_api_url.stability = FunctionStability::VOLATILE;
	loader.RegisterFunction(set_api_url);
	ScalarFunction set_model("jev_set_model", {LogicalType::VARCHAR}, LogicalType::VARCHAR, JevSetModelFunction);
	set_model.stability = FunctionStability::VOLATILE;
	loader.RegisterFunction(set_model);

	// jev() has two overloads and has to be registered as one macro entry.
	RegisterMacro(loader, 0, 2);
	for (idx_t i = 2; i < sizeof(JEV_MACROS) / sizeof(DefaultMacro); i++) {
		RegisterMacro(loader, i, 1);
	}
}

} // namespace

void JevExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string JevExtension::Name() {
	return "jev";
}

std::string JevExtension::Version() const {
	return JevVersion();
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(jev, loader) {
	duckdb::LoadInternal(loader);
}
}
