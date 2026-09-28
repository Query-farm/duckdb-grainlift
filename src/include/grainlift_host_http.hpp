#pragma once

#include "duckdb.hpp"
#include "grainlift_host.h"

namespace adbc_scanner {
using namespace duckdb;

// Per-database state handed to the Rust driver as an opaque pointer.
struct GrainliftHostContext {
	weak_ptr<DatabaseInstance> db;
};

// Install the HTTPUtil-backed executor in the grainlift driver (once per load).
void RegisterGrainliftHostHttp();

shared_ptr<GrainliftHostContext> CreateGrainliftHostContext(DatabaseInstance &db);

// Value for the GRAINLIFT_HOST_CTX_OPTION database option.
string GrainliftHostContextOption(const GrainliftHostContext &ctx);

} // namespace adbc_scanner
