#pragma once

#include "duckdb.hpp"

namespace adbc_scanner {
using namespace duckdb;


// Register table functions (grainlift_scan)
void RegisterAdbcTableFunctions(DatabaseInstance &db);

class AdbcConnectionWrapper;
// Bind grainlift_scan_table over an already-resolved connection (ATTACH scans)
unique_ptr<FunctionData> AdbcScanTableBindWithConnection(ClientContext &context, TableFunctionBindInput &input,
                                                         shared_ptr<AdbcConnectionWrapper> connection,
                                                         vector<LogicalType> &return_types, vector<string> &names);

// Register catalog functions (grainlift_info, grainlift_tables)
void RegisterAdbcCatalogFunctions(DatabaseInstance &db);

// Register execute function (grainlift_execute for DDL/DML)
void RegisterAdbcExecuteFunction(DatabaseInstance &db);

// Register insert function (grainlift_insert for bulk ingestion)
void RegisterAdbcInsertFunction(DatabaseInstance &db);

// Register grainlift_clear_cache scalar function
void RegisterAdbcClearCacheFunction(DatabaseInstance &db);


} // namespace adbc_scanner
