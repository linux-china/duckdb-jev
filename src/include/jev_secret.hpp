#pragma once

#include "duckdb.hpp"

namespace duckdb {

//! Resolved connection settings, read from the `jev` secret.
struct JevSecret {
	string api_key;
	string api_url = "https://api.typesafe.ai/v1/systemone";
	string model = "jev-latest";
};

//! Registers the `jev` secret type: CREATE SECRET (TYPE jev, API_KEY '...', API_URL '...', MODEL '...')
void RegisterJevSecret(ExtensionLoader &loader);

//! Looks up the jev secret. Throws BinderException when none exists, so a query
//! that can never succeed fails before execution.
JevSecret ResolveJevSecret(ClientContext &context);

} // namespace duckdb