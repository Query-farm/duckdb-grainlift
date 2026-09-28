#define DUCKDB_EXTENSION_MAIN

#include "grainlift_extension.hpp"
#include "adbc_functions.hpp"
#include "adbc_secrets.hpp"
#include "grainlift_host_http.hpp"
#include "storage/adbc_storage.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/config.hpp"

#ifndef GRAINLIFT_EXTENSION_VERSION
#define GRAINLIFT_EXTENSION_VERSION "0.1.0"
#endif

namespace adbc_scanner {

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
