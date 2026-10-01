#define DUCKDB_EXTENSION_MAIN

#include "grainlift_extension.hpp"
#include "adbc_functions.hpp"
#include "adbc_secrets.hpp"
#include "grainlift_host_http.hpp"
#include "storage/adbc_storage.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/planner/operator/logical_simple.hpp"
#include "grainlift_host.h"

#ifndef GRAINLIFT_EXTENSION_VERSION
#define GRAINLIFT_EXTENSION_VERSION "0.4.0"
#endif

namespace adbc_scanner {

// In DuckDB-WASM an iroh:// endpoint must be prepared from DuckDB's main worker
// thread (the page's Iroh adapter Worker is only reachable from there).
// grainlift_connect prepares while binding; ATTACH's attach callback runs at
// execution, which with threads > 1 can be a pthread, so prepare from the
// ATTACH plan instead: pre-optimizer hooks run during planning on the calling
// thread. Preparing is a no-op for anything that is not an iroh:// endpoint
// (and natively).
static void PrepareAttachEndpoint(OptimizerExtensionInput &, unique_ptr<LogicalOperator> &plan) {
	if (!plan || plan->type != LogicalOperatorType::LOGICAL_ATTACH) {
		return;
	}
	auto &simple = plan->Cast<LogicalSimple>();
	if (!simple.info || simple.info->info_type != ParseInfoType::ATTACH_INFO) {
		return;
	}
	auto &path = simple.info->Cast<AttachInfo>().path;
	if (StringUtil::Contains(StringUtil::Lower(path), "iroh://")) {
		grainlift_prepare_endpoint(path.c_str());
	}
}

static void LoadInternal(duckdb::ExtensionLoader &loader) {
	// Route the statically linked grainlift driver's HTTP through DuckDB's
	// HTTPUtil (sync XHR in DuckDB-WASM, httpfs natively).
	RegisterGrainliftHostHttp();

	// Register grainlift secret type and create secret function
	RegisterAdbcSecrets(loader);

	// Register volatile grainlift_connect and runtime connection commands
	RegisterAdbcScalarFunctions(loader.GetDatabaseInstance());

	// Register table functions (grainlift_scan, grainlift_scan_table)
	RegisterAdbcTableFunctions(loader.GetDatabaseInstance());

	// Register catalog functions (grainlift_info, grainlift_tables, ...)
	RegisterAdbcCatalogFunctions(loader.GetDatabaseInstance());

	// Register grainlift_execute (DDL/DML)
	RegisterAdbcExecuteFunction(loader.GetDatabaseInstance());

	// Register grainlift_insert (bulk ingestion)
	RegisterAdbcInsertFunction(loader.GetDatabaseInstance());

	// Register grainlift_clear_cache
	RegisterAdbcClearCacheFunction(loader.GetDatabaseInstance());

	// Register storage extension for ATTACH ... (TYPE grainlift)
	auto &config = duckdb::DBConfig::GetConfig(loader.GetDatabaseInstance());
	StorageExtension::Register(config, "grainlift", make_shared_ptr<AdbcStorageExtension>());

	// Prepare iroh:// endpoints for ATTACH on the planning (main) thread
	OptimizerExtension attach_endpoints;
	attach_endpoints.pre_optimize_function = PrepareAttachEndpoint;
	OptimizerExtension::Register(config, attach_endpoints);
}

} // namespace adbc_scanner

namespace duckdb {

void GrainliftExtension::Load(ExtensionLoader &loader) {
	adbc_scanner::LoadInternal(loader);
}

std::string GrainliftExtension::Name() {
	return "grainlift";
}

std::string GrainliftExtension::Version() const {
	return GRAINLIFT_EXTENSION_VERSION;
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(grainlift, loader) {
	adbc_scanner::LoadInternal(loader);
}
}
