#include "adbc_connection.hpp"
#include "adbc_secrets.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "grainlift_host.h"

namespace adbc_scanner {
using namespace duckdb;

struct AdbcClientHandles : public ClientContextState {
    unordered_set<int64_t> handles;
    ~AdbcClientHandles() override {
        for (auto handle : handles) {
            ConnectionRegistry::Get().Remove(handle);
        }
    }
};

static AdbcOptions ExtractOptions(const Value &value) {
    AdbcOptions options;
    if (value.type().id() == LogicalTypeId::STRUCT) {
        const auto &children = StructValue::GetChildren(value);
        for (idx_t i = 0; i < children.size(); i++) {
            if (!children[i].IsNull()) {
                options.emplace_back(StructType::GetChildName(value.type(), i), children[i]);
            }
        }
    } else if (value.type().id() == LogicalTypeId::MAP) {
        for (const auto &entry : MapValue::GetChildren(value)) {
            const auto &pair = StructValue::GetChildren(entry);
            if (!pair[0].IsNull() && !pair[1].IsNull()) {
                options.emplace_back(pair[0].GetValue<string>(), pair[1]);
            }
        }
    } else {
        throw InvalidInputException("grainlift_connect: options must be a STRUCT or MAP");
    }
    for (const auto &option : options) {
        if (option.second.type().id() == LogicalTypeId::STRUCT ||
            option.second.type().id() == LogicalTypeId::MAP ||
            option.second.type().id() == LogicalTypeId::LIST) {
            throw InvalidInputException("grainlift_connect: nested option values are not supported; pass driver options directly");
        }
    }
    return options;
}

static void AdbcConnect(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &context = state.GetContext();
    auto owned = context.registered_state->GetOrCreate<AdbcClientHandles>("adbc.handles");
    // Volatile scalars have per-row semantics even when their input is constant.
    result.SetVectorType(VectorType::FLAT_VECTOR);
    auto values = FlatVector::GetData<int64_t>(result);
    vector<int64_t> created;
    created.reserve(args.size());
    try {
        for (idx_t row = 0; row < args.size(); row++) {
            auto value = args.data[0].GetValue(row);
            if (value.IsNull()) {
                throw InvalidInputException("grainlift_connect: options must not be NULL");
            }
            auto options = MergeSecretOptions(context, ExtractOptions(value));
            auto connection = CreateConnectionFromOptions(context, options);
            auto handle = ConnectionRegistry::Get().Add(std::move(connection), &context);
            created.push_back(handle);
            owned->handles.insert(handle);
            values[row] = handle;
        }
    } catch (...) {
        // No handles from this output vector can reach the caller on failure.
        for (auto handle : created) {
            ConnectionRegistry::Get().Remove(handle);
            owned->handles.erase(handle);
        }
        throw;
    }
}

// Binding runs on the client's own thread; execution may run on any executor
// thread. Some transports (iroh:// in DuckDB-WASM) must be prepared from the
// former, so do it here whenever the options are known at bind time.
static unique_ptr<FunctionData> AdbcConnectBind(ClientContext &context, ScalarFunction &,
                                                vector<unique_ptr<Expression>> &arguments) {
    if (arguments.size() != 1 || !arguments[0]->IsFoldable()) {
        return nullptr;
    }
    try {
        auto value = ExpressionExecutor::EvaluateScalar(context, *arguments[0]);
        if (value.IsNull()) {
            return nullptr;
        }
        for (const auto &option : MergeSecretOptions(context, ExtractOptions(value))) {
            if (StringUtil::CIEquals(option.first, "uri") && !option.second.IsNull()) {
                grainlift_prepare_endpoint(option.second.ToString().c_str());
            }
        }
    } catch (std::exception &) {
        // Execution reports invalid options with full context.
    }
    return nullptr;
}

struct AdbcCommandBindData : public TableFunctionData {
    string command;
    int64_t handle;
    bool enabled = false;
};

struct AdbcCommandState : public GlobalTableFunctionState {
    bool finished = false;
};

static unique_ptr<FunctionData> BindCommand(ClientContext &, TableFunctionBindInput &input,
                                            vector<LogicalType> &types, vector<string> &names) {
    for (const auto &argument : input.inputs) {
        if (argument.IsNull()) {
            throw InvalidInputException("ADBC command arguments must not be NULL");
        }
    }
    auto data = make_uniq<AdbcCommandBindData>();
    data->command = input.table_function.name;
    data->handle = input.inputs[0].GetValue<int64_t>();
    if (input.inputs.size() == 2) {
        data->enabled = input.inputs[1].GetValue<bool>();
    }
    types.emplace_back(LogicalType::BOOLEAN);
    names.emplace_back("success");
    return std::move(data);
}

static unique_ptr<GlobalTableFunctionState> InitCommand(ClientContext &, TableFunctionInitInput &) {
    return make_uniq<AdbcCommandState>();
}

static void RunCommand(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
    auto &state = input.global_state->Cast<AdbcCommandState>();
    if (state.finished) {
        return;
    }
    state.finished = true;
    const auto &data = input.bind_data->Cast<AdbcCommandBindData>();
    auto connection = GetValidatedConnection(context, data.handle, data.command);
    if (data.command == "grainlift_disconnect") {
        connection->Close();
        ConnectionRegistry::Get().Remove(data.handle, &context);
        context.registered_state->GetOrCreate<AdbcClientHandles>("adbc.handles")->handles.erase(data.handle);
    } else if (data.command == "grainlift_commit") {
        connection->Commit();
    } else if (data.command == "grainlift_rollback") {
        connection->Rollback();
    } else {
        connection->SetAutocommit(data.enabled);
    }
    output.SetCardinality(1);
    output.SetValue(0, 0, Value::BOOLEAN(true));
}

void RegisterAdbcScalarFunctions(DatabaseInstance &db) {
    ExtensionLoader loader(db, "grainlift");
    ScalarFunction connect("grainlift_connect", {LogicalType::ANY}, LogicalType::BIGINT, AdbcConnect, AdbcConnectBind);
    connect.stability = FunctionStability::VOLATILE;
    connect.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
    CreateScalarFunctionInfo connect_info(connect);
    FunctionDescription connect_description;
    connect_description.description = "Open an ADBC connection owned by this DuckDB client; evaluated once per input row";
    connect_description.parameter_names = {"options"};
    connect_description.parameter_types = {LogicalType::ANY};
    connect_description.examples = {"SELECT grainlift_connect({'driver': 'sqlite', 'uri': ':memory:'})"};
    connect_description.categories = {"grainlift"};
    connect_info.descriptions.push_back(std::move(connect_description));
    loader.RegisterFunction(connect_info);
    for (auto name : {"grainlift_disconnect", "grainlift_commit", "grainlift_rollback", "grainlift_set_autocommit"}) {
        vector<LogicalType> arguments = {LogicalType::BIGINT};
        if (string(name) == "grainlift_set_autocommit") {
            arguments.push_back(LogicalType::BOOLEAN);
        }
        TableFunction command(name, arguments, RunCommand, BindCommand, InitCommand);
        CreateTableFunctionInfo info(command);
        FunctionDescription description;
        description.description = string(name) + ": perform the connection operation at execution time";
        description.parameter_names = {"connection_handle"};
        if (arguments.size() == 2) {
            description.parameter_names.push_back("enabled");
        }
        description.parameter_types = arguments;
        description.examples = {"CALL " + string(name) + (arguments.size() == 2 ? "(conn, false)" : "(conn)")};
        description.categories = {"grainlift"};
        info.descriptions.push_back(std::move(description));
        loader.RegisterFunction(info);
    }
}
} // namespace adbc_scanner
