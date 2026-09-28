#pragma once

#include "duckdb.hpp"

namespace adbc_scanner {
using namespace duckdb;

// Register scalar functions (grainlift_connect, grainlift_disconnect)
void RegisterAdbcScalarFunctions(DatabaseInstance &db);

// Register table functions (grainlift_scan)
void RegisterAdbcTableFunctions(DatabaseInstance &db);

// Register catalog functions (grainlift_info, grainlift_tables)
void RegisterAdbcCatalogFunctions(DatabaseInstance &db);

// Register execute function (grainlift_execute for DDL/DML)
void RegisterAdbcExecuteFunction(DatabaseInstance &db);

// Register insert function (grainlift_insert for bulk ingestion)
void RegisterAdbcInsertFunction(DatabaseInstance &db);

// Register grainlift_clear_cache scalar function
void RegisterAdbcClearCacheFunction(DatabaseInstance &db);


} // namespace adbc_scanner
