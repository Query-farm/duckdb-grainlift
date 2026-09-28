// Statically linked ADBC entry points.
//
// The grainlift driver is compiled into this extension, so there is no driver
// manager and no dynamic loading (WASM cannot dlopen an ADBC driver). The
// global Adbc* functions the rest of the extension calls forward to a single
// AdbcDriver function table populated once by AdbcDriverGrainliftInit.

#include "adbc_static_driver.hpp"

#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>

extern "C" AdbcStatusCode AdbcDriverGrainliftInit(int version, void *driver, struct AdbcError *error);

namespace {

struct StaticDriver {
	AdbcDriver driver;
	AdbcStatusCode init_status = ADBC_STATUS_OK;
	std::string init_error;
};

StaticDriver &GetStaticDriver() {
	static StaticDriver instance;
	static std::once_flag once;
	std::call_once(once, [] {
		std::memset(&instance.driver, 0, sizeof(instance.driver));
		AdbcError error;
		std::memset(&error, 0, sizeof(error));
		instance.init_status = AdbcDriverGrainliftInit(ADBC_VERSION_1_1_0, &instance.driver, &error);
		if (instance.init_status != ADBC_STATUS_OK) {
			instance.init_error = error.message ? error.message : "grainlift driver initialization failed";
		}
		if (error.release) {
			error.release(&error);
		}
	});
	return instance;
}

AdbcStatusCode SetStaticError(struct AdbcError *error, const std::string &message) {
	if (!error) {
		return ADBC_STATUS_INTERNAL;
	}
	if (error->release) {
		error->release(error);
	}
	auto buffer = new char[message.size() + 1];
	std::memcpy(buffer, message.c_str(), message.size() + 1);
	error->message = buffer;
	error->vendor_code = 0;
	std::memset(error->sqlstate, 0, sizeof(error->sqlstate));
	error->private_data = nullptr;
	error->private_driver = nullptr;
	error->release = [](struct AdbcError *err) {
		delete[] err->message;
		err->message = nullptr;
		err->release = nullptr;
	};
	return ADBC_STATUS_INTERNAL;
}

} // namespace

// Resolve the driver table, reporting a failed driver init through `error`.
#define GRAINLIFT_DRIVER(error, fn)                                                                                    \
	auto &static_driver = GetStaticDriver();                                                                           \
	if (static_driver.init_status != ADBC_STATUS_OK) {                                                                 \
		return SetStaticError(error, static_driver.init_error);                                                        \
	}                                                                                                                  \
	if (!static_driver.driver.fn) {                                                                                    \
		SetStaticError(error, "grainlift driver does not implement " #fn);                                             \
		return ADBC_STATUS_NOT_IMPLEMENTED;                                                                            \
	}                                                                                                                  \
	auto &driver = static_driver.driver

extern "C" {

AdbcStatusCode AdbcDatabaseNew(struct AdbcDatabase *database, struct AdbcError *error) {
	GRAINLIFT_DRIVER(error, DatabaseNew);
	database->private_driver = &driver;
	return driver.DatabaseNew(database, error);
}

AdbcStatusCode AdbcDatabaseSetOption(struct AdbcDatabase *database, const char *key, const char *value,
                                     struct AdbcError *error) {
	GRAINLIFT_DRIVER(error, DatabaseSetOption);
	return driver.DatabaseSetOption(database, key, value, error);
}

AdbcStatusCode AdbcDatabaseSetOptionBytes(struct AdbcDatabase *database, const char *key, const uint8_t *value,
                                          size_t length, struct AdbcError *error) {
	GRAINLIFT_DRIVER(error, DatabaseSetOptionBytes);
	return driver.DatabaseSetOptionBytes(database, key, value, length, error);
}

AdbcStatusCode AdbcDatabaseSetOptionInt(struct AdbcDatabase *database, const char *key, int64_t value,
                                        struct AdbcError *error) {
	GRAINLIFT_DRIVER(error, DatabaseSetOptionInt);
	return driver.DatabaseSetOptionInt(database, key, value, error);
}

AdbcStatusCode AdbcDatabaseSetOptionDouble(struct AdbcDatabase *database, const char *key, double value,
                                           struct AdbcError *error) {
	GRAINLIFT_DRIVER(error, DatabaseSetOptionDouble);
	return driver.DatabaseSetOptionDouble(database, key, value, error);
}

AdbcStatusCode AdbcDatabaseInit(struct AdbcDatabase *database, struct AdbcError *error) {
	GRAINLIFT_DRIVER(error, DatabaseInit);
	return driver.DatabaseInit(database, error);
}

AdbcStatusCode AdbcDatabaseRelease(struct AdbcDatabase *database, struct AdbcError *error) {
	GRAINLIFT_DRIVER(error, DatabaseRelease);
	auto status = driver.DatabaseRelease(database, error);
	database->private_driver = nullptr;
	return status;
}

AdbcStatusCode AdbcConnectionNew(struct AdbcConnection *connection, struct AdbcError *error) {
	GRAINLIFT_DRIVER(error, ConnectionNew);
	connection->private_driver = &driver;
	return driver.ConnectionNew(connection, error);
}

AdbcStatusCode AdbcConnectionSetOption(struct AdbcConnection *connection, const char *key, const char *value,
                                       struct AdbcError *error) {
	GRAINLIFT_DRIVER(error, ConnectionSetOption);
	return driver.ConnectionSetOption(connection, key, value, error);
}

AdbcStatusCode AdbcConnectionInit(struct AdbcConnection *connection, struct AdbcDatabase *database,
                                  struct AdbcError *error) {
	GRAINLIFT_DRIVER(error, ConnectionInit);
	return driver.ConnectionInit(connection, database, error);
}

AdbcStatusCode AdbcConnectionRelease(struct AdbcConnection *connection, struct AdbcError *error) {
	GRAINLIFT_DRIVER(error, ConnectionRelease);
	auto status = driver.ConnectionRelease(connection, error);
	connection->private_driver = nullptr;
	return status;
}

AdbcStatusCode AdbcConnectionGetInfo(struct AdbcConnection *connection, const uint32_t *info_codes,
                                     size_t info_codes_length, struct ArrowArrayStream *out,
                                     struct AdbcError *error) {
	GRAINLIFT_DRIVER(error, ConnectionGetInfo);
	return driver.ConnectionGetInfo(connection, info_codes, info_codes_length, out, error);
}

AdbcStatusCode AdbcConnectionGetObjects(struct AdbcConnection *connection, int depth, const char *catalog,
                                        const char *db_schema, const char *table_name, const char **table_type,
                                        const char *column_name, struct ArrowArrayStream *out,
                                        struct AdbcError *error) {
	GRAINLIFT_DRIVER(error, ConnectionGetObjects);
	return driver.ConnectionGetObjects(connection, depth, catalog, db_schema, table_name, table_type, column_name,
	                                   out, error);
}

AdbcStatusCode AdbcConnectionGetTableTypes(struct AdbcConnection *connection, struct ArrowArrayStream *out,
                                           struct AdbcError *error) {
	GRAINLIFT_DRIVER(error, ConnectionGetTableTypes);
	return driver.ConnectionGetTableTypes(connection, out, error);
}

AdbcStatusCode AdbcConnectionGetTableSchema(struct AdbcConnection *connection, const char *catalog,
                                            const char *db_schema, const char *table_name,
                                            struct ArrowSchema *schema, struct AdbcError *error) {
	GRAINLIFT_DRIVER(error, ConnectionGetTableSchema);
	return driver.ConnectionGetTableSchema(connection, catalog, db_schema, table_name, schema, error);
}

AdbcStatusCode AdbcConnectionGetStatistics(struct AdbcConnection *connection, const char *catalog,
                                           const char *db_schema, const char *table_name, char approximate,
                                           struct ArrowArrayStream *out, struct AdbcError *error) {
	GRAINLIFT_DRIVER(error, ConnectionGetStatistics);
	return driver.ConnectionGetStatistics(connection, catalog, db_schema, table_name, approximate, out, error);
}

AdbcStatusCode AdbcConnectionCommit(struct AdbcConnection *connection, struct AdbcError *error) {
	GRAINLIFT_DRIVER(error, ConnectionCommit);
	return driver.ConnectionCommit(connection, error);
}

AdbcStatusCode AdbcConnectionRollback(struct AdbcConnection *connection, struct AdbcError *error) {
	GRAINLIFT_DRIVER(error, ConnectionRollback);
	return driver.ConnectionRollback(connection, error);
}

AdbcStatusCode AdbcStatementNew(struct AdbcConnection *connection, struct AdbcStatement *statement,
                                struct AdbcError *error) {
	GRAINLIFT_DRIVER(error, StatementNew);
	statement->private_driver = &driver;
	return driver.StatementNew(connection, statement, error);
}

AdbcStatusCode AdbcStatementSetSqlQuery(struct AdbcStatement *statement, const char *query,
                                        struct AdbcError *error) {
	GRAINLIFT_DRIVER(error, StatementSetSqlQuery);
	return driver.StatementSetSqlQuery(statement, query, error);
}

AdbcStatusCode AdbcStatementSetOption(struct AdbcStatement *statement, const char *key, const char *value,
                                      struct AdbcError *error) {
	GRAINLIFT_DRIVER(error, StatementSetOption);
	return driver.StatementSetOption(statement, key, value, error);
}

AdbcStatusCode AdbcStatementPrepare(struct AdbcStatement *statement, struct AdbcError *error) {
	GRAINLIFT_DRIVER(error, StatementPrepare);
	return driver.StatementPrepare(statement, error);
}

AdbcStatusCode AdbcStatementBind(struct AdbcStatement *statement, struct ArrowArray *values,
                                 struct ArrowSchema *schema, struct AdbcError *error) {
	GRAINLIFT_DRIVER(error, StatementBind);
	return driver.StatementBind(statement, values, schema, error);
}

AdbcStatusCode AdbcStatementBindStream(struct AdbcStatement *statement, struct ArrowArrayStream *stream,
                                       struct AdbcError *error) {
	GRAINLIFT_DRIVER(error, StatementBindStream);
	return driver.StatementBindStream(statement, stream, error);
}

AdbcStatusCode AdbcStatementExecuteSchema(struct AdbcStatement *statement, struct ArrowSchema *schema,
                                          struct AdbcError *error) {
	GRAINLIFT_DRIVER(error, StatementExecuteSchema);
	return driver.StatementExecuteSchema(statement, schema, error);
}

AdbcStatusCode AdbcStatementExecuteQuery(struct AdbcStatement *statement, struct ArrowArrayStream *out,
                                         int64_t *rows_affected, struct AdbcError *error) {
	GRAINLIFT_DRIVER(error, StatementExecuteQuery);
	return driver.StatementExecuteQuery(statement, out, rows_affected, error);
}

AdbcStatusCode AdbcStatementCancel(struct AdbcStatement *statement, struct AdbcError *error) {
	GRAINLIFT_DRIVER(error, StatementCancel);
	return driver.StatementCancel(statement, error);
}

AdbcStatusCode AdbcStatementRelease(struct AdbcStatement *statement, struct AdbcError *error) {
	GRAINLIFT_DRIVER(error, StatementRelease);
	auto status = driver.StatementRelease(statement, error);
	statement->private_driver = nullptr;
	return status;
}

int AdbcErrorGetDetailCount(const struct AdbcError *error) {
	if (!error || error->vendor_code != ADBC_ERROR_VENDOR_CODE_PRIVATE_DATA || !error->private_driver ||
	    !error->private_driver->ErrorGetDetailCount) {
		return 0;
	}
	return error->private_driver->ErrorGetDetailCount(error);
}

struct AdbcErrorDetail AdbcErrorGetDetail(const struct AdbcError *error, int index) {
	if (!error || error->vendor_code != ADBC_ERROR_VENDOR_CODE_PRIVATE_DATA || !error->private_driver ||
	    !error->private_driver->ErrorGetDetail) {
		return {nullptr, nullptr, 0};
	}
	return error->private_driver->ErrorGetDetail(error, index);
}

} // extern "C"

namespace adbc_scanner {

const char *AdbcStatusCodeMessage(AdbcStatusCode code) {
	switch (code) {
	case ADBC_STATUS_OK:
		return "OK";
	case ADBC_STATUS_UNKNOWN:
		return "UNKNOWN";
	case ADBC_STATUS_NOT_IMPLEMENTED:
		return "NOT_IMPLEMENTED";
	case ADBC_STATUS_NOT_FOUND:
		return "NOT_FOUND";
	case ADBC_STATUS_ALREADY_EXISTS:
		return "ALREADY_EXISTS";
	case ADBC_STATUS_INVALID_ARGUMENT:
		return "INVALID_ARGUMENT";
	case ADBC_STATUS_INVALID_STATE:
		return "INVALID_STATE";
	case ADBC_STATUS_INVALID_DATA:
		return "INVALID_DATA";
	case ADBC_STATUS_INTEGRITY:
		return "INTEGRITY";
	case ADBC_STATUS_INTERNAL:
		return "INTERNAL";
	case ADBC_STATUS_IO:
		return "IO";
	case ADBC_STATUS_CANCELLED:
		return "CANCELLED";
	case ADBC_STATUS_TIMEOUT:
		return "TIMEOUT";
	case ADBC_STATUS_UNAUTHENTICATED:
		return "UNAUTHENTICATED";
	case ADBC_STATUS_UNAUTHORIZED:
		return "UNAUTHORIZED";
	default:
		return "(invalid code)";
	}
}

} // namespace adbc_scanner
