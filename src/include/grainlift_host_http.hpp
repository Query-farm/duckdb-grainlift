#pragma once

#include "duckdb.hpp"
#include "grainlift_host.h"

namespace adbc_scanner {
using namespace duckdb;

// Per-database state handed to the Rust driver as an opaque pointer.
struct GrainliftHostContext {
	weak_ptr<DatabaseInstance> db;
	//! The same instance, for DuckDB's shutdown: ~DatabaseInstance first
	//! detaches attached databases (ResetDatabases), whose ADBC connections then
	//! close their gateway sessions. `db` has already expired by then, but the
	//! instance and its HTTPUtil are intact. This context is owned by the
	//! attached catalog, so it never outlives that phase.
	DatabaseInstance *instance = nullptr;
};

// Install the HTTPUtil-backed executor in the grainlift driver (once per load).
void RegisterGrainliftHostHttp();

shared_ptr<GrainliftHostContext> CreateGrainliftHostContext(DatabaseInstance &db);

// Value for the GRAINLIFT_HOST_CTX_OPTION database option.
string GrainliftHostContextOption(const GrainliftHostContext &ctx);

} // namespace adbc_scanner
