#include "adbc_secrets.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/execution/expression_executor_state.hpp"

namespace adbc_scanner {
using namespace duckdb;

unique_ptr<SecretEntry> AdbcGetSecretByName(ClientContext &context, const string &secret_name) {
	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);

	// Try memory storage first
	auto secret_entry = secret_manager.GetSecretByName(transaction, secret_name, "memory");
	if (secret_entry) {
		return secret_entry;
	}

	// Fall back to local_file storage
	secret_entry = secret_manager.GetSecretByName(transaction, secret_name, "local_file");
	if (secret_entry) {
		return secret_entry;
	}

	return nullptr;
}

string NormalizeGrainliftUri(const string &uri) {
	// Mirror the driver's scheme aliases (normalize_endpoint in the grainlift
	// ADBC driver): grainlift:// is HTTPS and grainlift+<transport>:// is
	// <transport>://. Only the scheme is rewritten (and lowercased); the rest of
	// the URI is compared as written.
	auto pos = uri.find("://");
	if (pos == string::npos) {
		return uri;
	}
	auto scheme = StringUtil::Lower(uri.substr(0, pos));
	if (scheme == "grainlift") {
		scheme = "https";
	} else if (StringUtil::StartsWith(scheme, "grainlift+")) {
		scheme = scheme.substr(strlen("grainlift+"));
	}
	return scheme + uri.substr(pos);
}

int64_t GrainliftScopeMatchLength(const string &scope, const string &uri) {
	// An empty scope (an unscoped secret) never matches: one unscoped secret
	// must not be sent to every gateway the user attaches.
	if (scope.empty() || uri.empty()) {
		return -1;
	}
	auto normalized_scope = NormalizeGrainliftUri(scope);
	auto normalized_uri = NormalizeGrainliftUri(uri);
	if (!StringUtil::StartsWith(normalized_uri, normalized_scope)) {
		return -1;
	}
	// The match must end at a URL boundary, so a scope of
	// grainlift+https://gw.example.com does not match gw.example.com.evil.net.
	auto length = normalized_scope.size();
	if (normalized_uri.size() == length || normalized_scope.back() == '/') {
		return NumericCast<int64_t>(length);
	}
	auto next = normalized_uri[length];
	if (next == '/' || next == '?' || next == '#' || next == ':') {
		return NumericCast<int64_t>(length);
	}
	return -1;
}

static int64_t StorageRank(const string &storage) {
	// DuckDB's own tie-break between storages: temporary secrets win over
	// persistent ones.
	if (storage == "memory") {
		return 0;
	}
	if (storage == "local_file") {
		return 1;
	}
	return 2;
}

unique_ptr<SecretEntry> AdbcGetSecretByUri(ClientContext &context, const string &uri) {
	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);

	unique_ptr<SecretEntry> best;
	int64_t best_length = -1;
	int64_t best_rank = 0;
	for (auto &entry : secret_manager.AllSecrets(transaction)) {
		if (!entry.secret || !StringUtil::CIEquals(entry.secret->GetType().GetIdentifierName(), "grainlift")) {
			continue;
		}
		int64_t length = -1;
		for (auto &scope : entry.secret->GetScope()) {
			length = MaxValue(length, GrainliftScopeMatchLength(scope, uri));
		}
		if (length < 0) {
			continue;
		}
		auto rank = StorageRank(entry.storage_mode);
		bool better = !best || length > best_length || (length == best_length && rank < best_rank) ||
		              (length == best_length && rank == best_rank && entry.secret->GetName() < best->secret->GetName());
		if (better) {
			best = make_uniq<SecretEntry>(entry);
			best_length = length;
			best_rank = rank;
		}
	}
	return best;
}

bool IsGrainliftCredentialOption(const string &key) {
	auto lower = StringUtil::Lower(key);
	// `secret` / `attach_secret` name a secret; they are not credentials.
	if (lower == "secret" || lower == "attach_secret") {
		return false;
	}
	for (auto name : {"password", "bearer_token", "token", "auth_token", "oauth_refresh_token", "oauth_client_secret",
	                  "api_key", "apikey", "credential", "credentials"}) {
		if (lower == name) {
			return true;
		}
	}
	// Driver-specific spellings (e.g. adbc.postgresql.password, *.auth.bearer_token).
	return StringUtil::Contains(lower, "password") || StringUtil::Contains(lower, "secret") ||
	       StringUtil::Contains(lower, "credential") || StringUtil::Contains(lower, "api_key") ||
	       StringUtil::Contains(lower, "apikey") || StringUtil::EndsWith(lower, "token");
}

string RedactGrainliftAttachPath(const string &path) {
	// ATTACH 'uri=...;bearer_token=...' carries options in the path, which
	// duckdb_databases() shows. Redact the credential values.
	if (path.find('=') == string::npos) {
		return path;
	}
	auto parts = StringUtil::Split(path, ';');
	string result;
	for (idx_t i = 0; i < parts.size(); i++) {
		auto part = parts[i];
		auto eq_pos = part.find('=');
		if (eq_pos != string::npos) {
			auto key = part.substr(0, eq_pos);
			StringUtil::Trim(key);
			if (IsGrainliftCredentialOption(key)) {
				part = part.substr(0, eq_pos + 1) + "<redacted>";
			}
		}
		if (i > 0) {
			result += ";";
		}
		result += part;
	}
	return result;
}

static bool IsSecretNameOption(const string &key) {
	return key == "secret" || key == "attach_secret";
}

static unique_ptr<SecretEntry> GetGrainliftSecretByName(ClientContext &context, const string &secret_name) {
	auto secret_entry = AdbcGetSecretByName(context, secret_name);
	if (!secret_entry) {
		throw BinderException("Secret with name \"%s\" not found", secret_name);
	}
	if (!StringUtil::CIEquals(secret_entry->secret->GetType().GetIdentifierName(), "grainlift")) {
		throw BinderException("Secret \"%s\" is of type '%s', not 'grainlift'", secret_name,
		                      secret_entry->secret->GetType().GetIdentifierName());
	}
	return secret_entry;
}

unique_ptr<SecretEntry> ResolveGrainliftSecret(ClientContext &context, const AdbcOptions &explicit_options) {
	string uri;
	bool has_secret_option = false;
	string secret_name;
	for (const auto &opt : explicit_options) {
		if (opt.first == "uri") {
			uri = opt.second.IsNull() ? string() : opt.second.GetValue<string>();
		} else if (IsSecretNameOption(opt.first)) {
			auto value = opt.second.IsNull() ? string() : opt.second.GetValue<string>();
			if (has_secret_option && value != secret_name) {
				throw BinderException("grainlift: 'secret' and 'attach_secret' name different secrets (\"%s\" and "
				                      "\"%s\"); use one of them",
				                      secret_name, value);
			}
			has_secret_option = true;
			secret_name = value;
		}
	}
	if (has_secret_option) {
		// Named explicitly (scope is not checked: the user chose it), or ''
		// to turn lookup off for this ATTACH.
		if (secret_name.empty()) {
			return nullptr;
		}
		return GetGrainliftSecretByName(context, secret_name);
	}
	if (uri.empty()) {
		return nullptr;
	}
	return AdbcGetSecretByUri(context, uri);
}

AdbcOptions MergeSecretOptions(ClientContext &context, const AdbcOptions &explicit_options) {
	// Which secret is used, decided once per ATTACH from the user's own inputs:
	//  1. `secret` / `attach_secret` '<name>': that secret (must be a grainlift one).
	//  2. `secret` / `attach_secret` '': no secret at all.
	//  3. Otherwise the grainlift secret whose scope best matches the URI the
	//     user wrote, ending at a URL boundary. Unscoped secrets never match.
	bool explicit_name = false;
	for (const auto &opt : explicit_options) {
		if (IsSecretNameOption(opt.first)) {
			explicit_name = true;
		}
	}
	auto secret_entry = ResolveGrainliftSecret(context, explicit_options);

	AdbcOptions merged_options;
	if (secret_entry) {
		const auto &kv_secret = dynamic_cast<const KeyValueSecret &>(*secret_entry->secret);
		for (const auto &entry : kv_secret.secret_map) {
			if (!entry.second.IsNull()) {
				merged_options.emplace_back(entry.first.GetIdentifierName(), entry.second);
			}
		}
	}
	bool used_uri_for_scope_lookup = secret_entry && !explicit_name;

	// Build set of keys that were explicitly provided (these override secret values)
	unordered_set<string> explicit_keys;
	for (const auto &opt : explicit_options) {
		// The secret-naming options are not passed to the driver
		if (IsSecretNameOption(opt.first)) {
			continue;
		}
		// If we found a secret by URI scope lookup (not by name), and the secret
		// already has a 'uri' option, skip the explicit 'uri' (it was just for lookup)
		if (opt.first == "uri" && used_uri_for_scope_lookup) {
			bool secret_has_uri = false;
			for (const auto &merged : merged_options) {
				if (merged.first == "uri") {
					secret_has_uri = true;
					break;
				}
			}
			if (secret_has_uri) {
				continue;
			}
		}
		explicit_keys.insert(opt.first);
	}

	// Filter out secret options that are overridden by explicit options
	AdbcOptions result;
	for (const auto &opt : merged_options) {
		if (explicit_keys.find(opt.first) == explicit_keys.end()) {
			result.push_back(opt);
		}
	}

	// Add all explicit options (except the secret name and 'uri' when used for scope lookup only)
	for (const auto &opt : explicit_options) {
		if (explicit_keys.find(opt.first) != explicit_keys.end()) {
			result.push_back(opt);
		}
	}

	return result;
}

// Create secret function
static unique_ptr<BaseSecret> CreateAdbcSecretFunction(ClientContext &context, CreateSecretInput &input) {
	(void)context;
	auto scope = input.scope;

	// A concrete URI is also a useful default lookup scope. Keep an explicit
	// scope unchanged: callers may intentionally match a broader URI prefix.
	if (scope.empty()) {
		auto uri = input.options.find("uri");
		if (uri == input.options.end() || uri->second.IsNull() || uri->second.GetValue<string>().empty()) {
			throw InvalidInputException("grainlift secret requires a non-empty URI or an explicit SCOPE");
		}
		scope.push_back(uri->second.GetValue<string>());
	}

	auto result = make_uniq<KeyValueSecret>(scope, "grainlift", "config", input.name);

	// Process known named parameters
	for (const auto &named_param : input.options) {
		auto lower_name = StringUtil::Lower(named_param.first);

		if (lower_name == "uri" || lower_name == "target" || lower_name == "bearer_token" ||
		    lower_name == "oauth_refresh_token" || lower_name == "oauth_token_endpoint" ||
		    lower_name == "oauth_client_id" || lower_name == "oauth_client_secret" || lower_name == "remote_uri" ||
		    lower_name == "username" || lower_name == "password" || lower_name == "database") {
			result->secret_map[Identifier(lower_name)] = named_param.second.ToString();
		} else if (lower_name == "extra_options") {
			// extra_options is a MAP of string -> string for driver-specific options
			auto &map_value = named_param.second;
			if (map_value.type().id() == LogicalTypeId::MAP) {
				auto &map_children = MapValue::GetChildren(map_value);
				for (auto &entry : map_children) {
					auto &entry_children = StructValue::GetChildren(entry);
					if (entry_children.size() == 2 && !entry_children[0].IsNull()) {
						auto key = entry_children[0].ToString();
						auto val = entry_children[1].IsNull() ? "" : entry_children[1].ToString();
						result->secret_map[Identifier(key)] = val;
						result->redact_keys.insert(Identifier(key));
					}
				}
			}
		}
	}

	// Redact sensitive keys by default
	for (auto key : {"password", "bearer_token", "oauth_refresh_token", "oauth_client_secret", "auth_token", "token",
	                 "secret", "api_key", "apikey", "credential"}) {
		result->redact_keys.insert(Identifier(key));
	}
	for (auto &entry : result->secret_map) {
		if (IsGrainliftCredentialOption(entry.first.GetIdentifierName())) {
			result->redact_keys.insert(entry.first);
		}
	}

	return std::move(result);
}

// grainlift_which_secret(uri [, attach_secret]): the name of the grainlift
// secret an ATTACH of `uri` would use (NULL for none), resolved exactly as
// ATTACH resolves it. Like DuckDB's which_secret(), but with grainlift's
// boundary rule.
static string_t WhichSecret(ClientContext &context, Vector &result, const string &uri, const Value &secret_option) {
	AdbcOptions options;
	options.emplace_back("uri", Value(uri));
	if (!secret_option.IsNull()) {
		options.emplace_back("attach_secret", secret_option);
	}
	auto entry = ResolveGrainliftSecret(context, options);
	return StringVector::AddString(result, entry ? entry->secret->GetName().GetIdentifierName() : string());
}

static void WhichSecretFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	if (args.ColumnCount() == 1) {
		// A lambda returning optional adds NULLs (DuckDB 2.0's ExecuteWithNulls)
		UnaryExecutor::Execute<string_t, string_t>(
		    args.data[0], result, args.size(), [&](string_t uri) -> optional<string_t> {
			    auto name = WhichSecret(context, result, uri.GetString(), Value());
			    if (name.GetSize() == 0) {
				    return nullopt;
			    }
			    return name;
		    });
		return;
	}
	BinaryExecutor::Execute<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t uri, string_t secret) -> optional<string_t> {
		    auto name = WhichSecret(context, result, uri.GetString(), Value(secret.GetString()));
		    if (name.GetSize() == 0) {
			    return nullopt;
		    }
		    return name;
	    });
}

void RegisterAdbcSecrets(ExtensionLoader &loader) {
	// Register the secret type
	SecretType secret_type;
	secret_type.name = "grainlift";
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "config";

	loader.RegisterSecretType(secret_type);

	// Register the create secret function with known parameters
	CreateSecretFunction adbc_secret_function = {"grainlift", "config", CreateAdbcSecretFunction, {}};

	// Grainlift connection parameters; username/password/database/remote_uri
	// are forwarded to the downstream driver by the grainlift server.
	for (auto name : {"uri", "target", "bearer_token", "oauth_refresh_token", "oauth_token_endpoint", "oauth_client_id",
	                  "oauth_client_secret", "remote_uri", "username", "password", "database"}) {
		adbc_secret_function.named_parameters[name] = LogicalType::VARCHAR;
	}

	// extra_options allows arbitrary driver-specific key-value pairs
	adbc_secret_function.named_parameters["extra_options"] =
	    LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);

	loader.RegisterFunction(adbc_secret_function);

	ScalarFunctionSet which_secret("grainlift_which_secret");
	for (idx_t arity : {1, 2}) {
		vector<LogicalType> arguments(arity, LogicalType::VARCHAR);
		ScalarFunction function(arguments, LogicalType::VARCHAR, WhichSecretFunction);
		function.SetStability(FunctionStability::VOLATILE);
		which_secret.AddFunction(function);
	}
	loader.RegisterFunction(which_secret);
}

} // namespace adbc_scanner
