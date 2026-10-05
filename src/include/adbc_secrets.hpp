#pragma once

#include "duckdb.hpp"
#include "adbc_options.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace adbc_scanner {
using namespace duckdb;

// Get a secret by name, checking both memory and local_file storage
unique_ptr<SecretEntry> AdbcGetSecretByName(ClientContext &context, const string &secret_name);

// Normalize a grainlift URI's scheme the way the driver does
// (grainlift:// -> https://, grainlift+<transport>:// -> <transport>://).
string NormalizeGrainliftUri(const string &uri);

// Length of the (normalized) scope when it matches the URI at a URL boundary:
// the scope equals the URI, ends in '/', or the URI continues with '/', '?',
// '#' or ':'. -1 when it does not match. An empty scope never matches.
int64_t GrainliftScopeMatchLength(const string &scope, const string &uri);

// The grainlift secret whose scope best matches the URI under the boundary
// rule (unscoped secrets never match), or nullptr.
unique_ptr<SecretEntry> AdbcGetSecretByUri(ClientContext &context, const string &uri);

// The secret an ATTACH with these options uses: `secret` / `attach_secret`
// '<name>' names one, '' disables lookup, otherwise lookup by `uri`. nullptr
// when none.
unique_ptr<SecretEntry> ResolveGrainliftSecret(ClientContext &context, const AdbcOptions &explicit_options);

// Whether an option (ATTACH or secret key) holds a credential, whose value must
// never be shown in duckdb_databases(), logs or cache keys.
bool IsGrainliftCredentialOption(const string &key);

// An ATTACH path with key=value credentials replaced by <redacted>.
string RedactGrainliftAttachPath(const string &path);

// Merge secret options with explicitly provided options
// Returns combined options with explicit options taking precedence
AdbcOptions MergeSecretOptions(ClientContext &context, const AdbcOptions &explicit_options);

// Register the ADBC secret type and create secret function
void RegisterAdbcSecrets(ExtensionLoader &loader);

} // namespace adbc_scanner
