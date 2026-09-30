#include "storage/adbc_schema_entry.hpp"
#include "storage/adbc_table_entry.hpp"
#include "storage/adbc_transaction.hpp"
#include "storage/adbc_catalog.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/common/arrow/arrow_converter.hpp"
#include "adbc_filter_pushdown.hpp"
#include "adbc_insert_stream.hpp"

namespace adbc_scanner {
using namespace duckdb;

AdbcSchemaEntry::AdbcSchemaEntry(Catalog &catalog, CreateSchemaInfo &info)
    : SchemaCatalogEntry(catalog, info), tables(*this) {
}

AdbcTransaction &GetAdbcTransaction(CatalogTransaction transaction) {
	if (!transaction.transaction) {
		throw InternalException("No transaction!?");
	}
	return transaction.transaction->Cast<AdbcTransaction>();
}

// The attached schema's name as the remote knows it. "main" is DuckDB's name
// for a schema-less remote's default (e.g. SQLite), so leave it implicit.
static string QualifiedName(const string &schema, const string &name, char quote) {
	auto quote_identifier = [quote](const string &identifier) {
		string quoted(1, quote);
		for (auto c : identifier) {
			quoted += c;
			if (c == quote) {
				quoted += c;
			}
		}
		return quoted + quote;
	};
	if (schema.empty() || schema == DEFAULT_SCHEMA) {
		return quote_identifier(name);
	}
	return quote_identifier(schema) + "." + quote_identifier(name);
}

// CREATE TABLE creates the remote table through ADBC bulk ingestion of an empty
// stream, so the driver writes the DDL in its own dialect and type mapping.
// Constraints and defaults are rejected up front (AdbcCatalog::SupportsCreateTable).
optional_ptr<CatalogEntry> AdbcSchemaEntry::CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) {
	auto &adbc_transaction = GetAdbcTransaction(transaction);
	auto &base = info.Base();
	string mode = "adbc.ingest.mode.create";
	if (auto existing = tables.GetEntry(adbc_transaction, base.table)) {
		switch (base.on_conflict) {
		case OnCreateConflict::IGNORE_ON_CONFLICT:
			return existing;
		case OnCreateConflict::REPLACE_ON_CONFLICT:
			mode = "adbc.ingest.mode.replace";
			break;
		default:
			throw CatalogException("Table with name \"%s\" already exists!", base.table);
		}
	}

	vector<LogicalType> types;
	vector<string> names;
	for (auto &column : base.columns.Logical()) {
		types.push_back(column.GetType());
		names.push_back(column.GetName());
	}
	ArrowSchema arrow_schema;
	auto client_properties = transaction.GetContext().GetClientProperties();
	ArrowConverter::ToArrowSchema(&arrow_schema, types, names, client_properties);
	AdbcInsertStream empty(1);
	empty.SetSchema(&arrow_schema);
	empty.Finish();

	AdbcStatementWrapper statement(adbc_transaction.GetWriteConnection());
	statement.Init();
	statement.SetOption("adbc.ingest.target_table", base.table);
	statement.SetOption("adbc.ingest.mode", mode);
	if (name != DEFAULT_SCHEMA) {
		statement.SetOption("adbc.ingest.target_db_schema", name);
	}
	statement.BindStream(&empty.stream);
	statement.ExecuteUpdate();

	// The new table is listed once the transaction commits; reload lazily.
	tables.ClearEntries();
	return nullptr;
}

optional_ptr<CatalogEntry> AdbcSchemaEntry::CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) {
	throw BinderException("ADBC databases do not support creating functions");
}

optional_ptr<CatalogEntry> AdbcSchemaEntry::CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
                                                        TableCatalogEntry &table) {
	throw BinderException("ADBC databases do not support creating indexes");
}

optional_ptr<CatalogEntry> AdbcSchemaEntry::CreateView(CatalogTransaction transaction, CreateViewInfo &info) {
	throw BinderException("ADBC databases do not support creating views");
}

optional_ptr<CatalogEntry> AdbcSchemaEntry::CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) {
	throw BinderException("ADBC databases do not support creating sequences");
}

optional_ptr<CatalogEntry> AdbcSchemaEntry::CreateTableFunction(CatalogTransaction transaction,
                                                                CreateTableFunctionInfo &info) {
	throw BinderException("ADBC databases do not support creating table functions");
}

optional_ptr<CatalogEntry> AdbcSchemaEntry::CreateCopyFunction(CatalogTransaction transaction,
                                                               CreateCopyFunctionInfo &info) {
	throw BinderException("ADBC databases do not support creating copy functions");
}

optional_ptr<CatalogEntry> AdbcSchemaEntry::CreatePragmaFunction(CatalogTransaction transaction,
                                                                 CreatePragmaFunctionInfo &info) {
	throw BinderException("ADBC databases do not support creating pragma functions");
}

optional_ptr<CatalogEntry> AdbcSchemaEntry::CreateCollation(CatalogTransaction transaction, CreateCollationInfo &info) {
	throw BinderException("ADBC databases do not support creating collations");
}

optional_ptr<CatalogEntry> AdbcSchemaEntry::CreateType(CatalogTransaction transaction, CreateTypeInfo &info) {
	throw BinderException("ADBC databases do not support creating types");
}

void AdbcSchemaEntry::Alter(CatalogTransaction transaction, AlterInfo &info) {
	throw BinderException("ADBC databases do not support ALTER operations");
}

bool CatalogTypeIsSupported(CatalogType type) {
	switch (type) {
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY:
		return true;
	default:
		return false;
	}
}

void AdbcSchemaEntry::Scan(ClientContext &context, CatalogType type,
                           const std::function<void(CatalogEntry &)> &callback) {
	if (!CatalogTypeIsSupported(type)) {
		return;
	}
	auto &adbc_transaction = AdbcTransaction::Get(context, catalog);
	tables.Scan(adbc_transaction, callback);
}

void AdbcSchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	throw NotImplementedException("Scan without context not supported");
}

void AdbcSchemaEntry::DropEntry(ClientContext &context, DropInfo &info) {
	if (info.type != CatalogType::TABLE_ENTRY && info.type != CatalogType::VIEW_ENTRY) {
		throw BinderException("grainlift databases only support dropping tables and views");
	}
	auto &adbc_transaction = AdbcTransaction::Get(context, catalog);
	auto entry = tables.GetEntry(adbc_transaction, info.name);
	if (!entry) {
		if (info.if_not_found == OnEntryNotFound::RETURN_NULL) {
			return;
		}
		throw CatalogException("Table with name \"%s\" does not exist!", info.name);
	}
	auto connection = adbc_transaction.GetWriteConnection();
	auto sql = string(info.type == CatalogType::VIEW_ENTRY ? "DROP VIEW " : "DROP TABLE ") +
	           QualifiedName(name, entry->name, IdentifierQuoteForDriver(connection->GetDriverName()));
	if (info.cascade) {
		sql += " CASCADE";
	}
	AdbcStatementWrapper statement(connection);
	statement.Init();
	statement.SetSqlQuery(sql);
	statement.ExecuteUpdate();
	tables.ClearEntries();
}

optional_ptr<CatalogEntry> AdbcSchemaEntry::LookupEntry(CatalogTransaction transaction,
                                                        const EntryLookupInfo &lookup_info) {
	auto catalog_type = lookup_info.GetCatalogType();
	if (!CatalogTypeIsSupported(catalog_type)) {
		return nullptr;
	}
	auto &adbc_transaction = GetAdbcTransaction(transaction);
	return tables.GetEntry(adbc_transaction, lookup_info.GetEntryName());
}

} // namespace adbc_scanner
