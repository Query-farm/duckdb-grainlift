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

//! Names the query whose driver calls run on this thread, for the length of a
//! scope. The driver makes its HTTP requests synchronously on the calling
//! thread, so the executor builds their parameters from this query's
//! ClientContext: an interrupt of the query then ends a request in flight and
//! its retries (HTTPParams::interrupt_context). Without a scope the requests
//! use database-level parameters and can't be interrupted.
class GrainliftQueryScope {
public:
	explicit GrainliftQueryScope(ClientContext &context);
	~GrainliftQueryScope();
	GrainliftQueryScope(const GrainliftQueryScope &) = delete;
	GrainliftQueryScope &operator=(const GrainliftQueryScope &) = delete;

	//! The innermost scope's context on this thread, if any
	static optional_ptr<ClientContext> Current();

private:
	optional_ptr<ClientContext> previous;
};

// Install the HTTPUtil-backed executor in the grainlift driver (once per load).
void RegisterGrainliftHostHttp();

shared_ptr<GrainliftHostContext> CreateGrainliftHostContext(DatabaseInstance &db);

// Value for the GRAINLIFT_HOST_CTX_OPTION database option.
string GrainliftHostContextOption(const GrainliftHostContext &ctx);

} // namespace adbc_scanner
