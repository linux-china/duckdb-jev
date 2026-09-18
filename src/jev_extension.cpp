#define DUCKDB_EXTENSION_MAIN

#include "jev_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

// OpenSSL linked through vcpkg
#include <openssl/opensslv.h>

namespace duckdb {

inline void JevScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "...........🦆 " + name.GetString());
	});
}

inline void JevOpenSSLVersionScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "Jev " + name.GetString() + ", my linked OpenSSL version is " +
		                                           OPENSSL_VERSION_TEXT);
	});
}

static void LoadInternal(ExtensionLoader &loader) {
	// Register a scalar function
	auto jev_scalar_function =
	    ScalarFunction("jev", {LogicalType::VARCHAR}, LogicalType::VARCHAR, JevScalarFun);

	loader.RegisterFunction(jev_scalar_function);

	// Register another scalar function
	auto jev_openssl_version_scalar_function = ScalarFunction("jev_openssl_version", {LogicalType::VARCHAR},
	                                                             LogicalType::VARCHAR, JevOpenSSLVersionScalarFun);
	loader.RegisterFunction(jev_openssl_version_scalar_function);
}

void JevExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string JevExtension::Name() {
	return "jev";
}

std::string JevExtension::Version() const {
#ifdef EXT_VERSION_JEV
	return EXT_VERSION_JEV;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(jev, loader) {
	duckdb::LoadInternal(loader);
}
}
