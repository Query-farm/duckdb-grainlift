#include "adbc_connection.hpp"
#include "grainlift_host_http.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/transaction/transaction_context.hpp"
#include "storage/adbc_catalog.hpp"
#include "storage/adbc_transaction.hpp"

#include <nanoarrow/nanoarrow.h>

namespace adbc_scanner {
using namespace duckdb;

shared_ptr<AdbcConnectionWrapper> GetAttachedConnection(ClientContext &context, const Value &database,
                                                        const string &function_name, bool write) {
	if (database.IsNull()) {
		throw InvalidInputException("%s: database name must not be NULL", function_name);
	}
	auto name = database.GetValue<string>();
	auto attached = DatabaseManager::Get(context).GetDatabase(context, name);
	if (!attached) {
		throw BinderException("%s: no attached database named \"%s\"; attach one with "
		                      "ATTACH '<uri>' AS %s (TYPE grainlift, target '...')",
		                      function_name, name, name);
	}
	auto &catalog = attached->GetCatalog();
	if (catalog.GetCatalogType() != "grainlift") {
		throw BinderException("%s: \"%s\" is a %s database, not a grainlift one", function_name, name,
		                      catalog.GetCatalogType());
	}
	auto &adbc_catalog = catalog.Cast<AdbcCatalog>();
	auto &transaction = AdbcTransaction::Get(context, catalog);
	bool explicit_transaction = !context.transaction.IsAutoCommit();
	auto connection = (write ? explicit_transaction : transaction.HasWriteConnection())
	                      ? transaction.GetWriteConnection()
	                      : adbc_catalog.GetConnection();
	if (!connection->IsInitialized()) {
		throw InvalidInputException("%s: the connection for \"%s\" has been closed", function_name, name);
	}
	return connection;
}

// Friendly option names accepted by ATTACH (and grainlift secrets), mapped to the
// grainlift driver's database options.
static const case_insensitive_map_t<string> &GrainliftOptionAliases() {
	static const case_insensitive_map_t<string> aliases = {
	    {"uri", "grainlift.uri"},
	    {"target", "grainlift.target"},
	    {"bearer_token", "grainlift.auth.bearer_token"},
	    {"token", "grainlift.auth.bearer_token"},
	    {"request_timeout_ms", "grainlift.request_timeout_ms"},
	    {"max_response_bytes", "grainlift.max_response_bytes"},
	    {"max_bind_bytes", "grainlift.max_bind_bytes"},
	    // The downstream driver's own `uri` (forwarded by the grainlift server).
	    {"remote_uri", "uri"},
	};
	return aliases;
}

static bool IsUnsupportedLoaderOption(const string &key) {
	return StringUtil::CIEquals(key, "driver") || StringUtil::CIEquals(key, "entrypoint") ||
	       StringUtil::CIEquals(key, "profile") || StringUtil::CIEquals(key, "search_paths") ||
	       StringUtil::CIEquals(key, "use_manifests");
}

// Read string-valued GetInfo entries (vendor/driver name) from the remote
// driver. Used to pick the SQL dialect for filter pushdown.
static string RemoteDriverName(AdbcConnectionWrapper &connection) {
	uint32_t codes[] = {ADBC_INFO_VENDOR_NAME, ADBC_INFO_DRIVER_NAME};
	ArrowArrayStream stream {};
	connection.GetInfo(codes, 2, &stream);
	string names;
	ArrowSchema schema {};
	if (stream.get_schema(&stream, &schema) != 0) {
		stream.release(&stream);
		return names;
	}
	ArrowError error;
	ArrowArrayView view;
	if (ArrowArrayViewInitFromSchema(&view, &schema, &error) != NANOARROW_OK) {
		schema.release(&schema);
		stream.release(&stream);
		return names;
	}
	ForEachArrowBatch(stream, "ATTACH", [&](ArrowArray *batch) {
		if (ArrowArrayViewSetArray(&view, batch, &error) != NANOARROW_OK) {
			return false;
		}
		auto value_union = view.children[1];
		for (int64_t row = 0; row < batch->length; row++) {
			auto type_id = ArrowArrayViewUnionTypeId(value_union, row);
			if (type_id != 0) { // string_value
				continue;
			}
			auto child_index = ArrowArrayViewUnionChildIndex(value_union, row);
			auto offset = ArrowArrayViewUnionChildOffset(value_union, row);
			auto child = value_union->children[child_index];
			if (ArrowArrayViewIsNull(child, offset)) {
				continue;
			}
			auto value = ArrowArrayViewGetStringUnsafe(child, offset);
			if (!names.empty()) {
				names += " ";
			}
			names += CopyDriverBytes(reinterpret_cast<const uint8_t *>(value.data), value.size_bytes);
		}
		return true;
	});
	ArrowArrayViewReset(&view);
	schema.release(&schema);
	stream.release(&stream);
	return names;
}

shared_ptr<AdbcConnectionWrapper> CreateConnectionFromOptions(ClientContext &context, const AdbcOptions &options) {
	AdbcOptions db_options;
	string dialect;
	bool has_uri = false;
	bool has_target = false;

	auto &aliases = GrainliftOptionAliases();
	for (const auto &opt : options) {
		auto key = opt.first;
		if (StringUtil::CIEquals(key, "secret")) {
			continue; // used for lookup only
		}
		if (StringUtil::CIEquals(key, "dialect")) {
			dialect = opt.second.GetValue<string>();
			continue;
		}
		if (IsUnsupportedLoaderOption(key)) {
			throw InvalidInputException("grainlift: option '%s' is not supported; the grainlift driver is built in. "
			                            "Use 'uri' (the grainlift service) and 'target'.",
			                            key);
		}
		if (StringUtil::CIEquals(key, GRAINLIFT_HOST_CTX_OPTION)) {
			throw InvalidInputException("grainlift: option '%s' is reserved", key);
		}
		auto alias = aliases.find(key);
		if (alias != aliases.end()) {
			key = alias->second;
		}
		has_uri |= key == "grainlift.uri";
		has_target |= key == "grainlift.target";
		db_options.emplace_back(key, opt.second);
	}
	if (!has_uri) {
		throw InvalidInputException("grainlift: a 'uri' option naming the grainlift service is required "
		                            "(e.g. 'grainlift+https://host/prefix')");
	}
	if (!has_target) {
		throw InvalidInputException("grainlift: a 'target' option naming the server-side target is required");
	}

#ifndef __EMSCRIPTEN__
	// Natively DuckDB's HTTPUtil only supports GET until httpfs is loaded; in
	// DuckDB-WASM the engine's XHR-based HTTPUtil is always present.
	if (!context.db->ExtensionIsLoaded("httpfs")) {
		try {
			ExtensionHelper::TryAutoLoadExtension(*context.db, "httpfs");
		} catch (...) {
			// Reported below.
		}
		if (!context.db->ExtensionIsLoaded("httpfs")) {
			throw InvalidInputException("grainlift requires the httpfs extension for HTTP transport. "
			                            "Install it with: INSTALL httpfs; LOAD httpfs;");
		}
	}
#endif
	auto host_ctx = CreateGrainliftHostContext(*context.db);

	auto database = make_shared_ptr<AdbcDatabaseWrapper>();
	database->Init();
	database->SetKeepAlive(host_ctx);
	database->SetOption(GRAINLIFT_HOST_CTX_OPTION, GrainliftHostContextOption(*host_ctx));
	for (const auto &opt : db_options) {
		database->SetOption(opt.first, opt.second);
	}
	database->Initialize();

	auto connection = make_shared_ptr<AdbcConnectionWrapper>(database);
	connection->Init();
	connection->Initialize();

	if (dialect.empty()) {
		try {
			dialect = RemoteDriverName(*connection);
		} catch (std::exception &) {
			// GetInfo is optional; fall back to the default (standard SQL) dialect.
		}
	}
	database->SetDriverName(dialect);

	return connection;
}

} // namespace adbc_scanner
