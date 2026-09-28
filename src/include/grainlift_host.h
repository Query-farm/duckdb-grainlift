// C ABI between the grainlift Rust driver and the host DuckDB extension.
//
// The driver performs no network I/O of its own for http(s) endpoints when
// built with the `host-http` feature: every HTTP request goes through the
// executor registered here, which the extension implements on top of DuckDB's
// HTTPUtil (sync XMLHttpRequest in DuckDB-WASM, httpfs natively).
//
// Keep in sync with crates/adbc-driver-grainlift/src/host_http.rs.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GrainliftHttpHeader {
	const char *name;
	size_t name_len;
	const char *value;
	size_t value_len;
} GrainliftHttpHeader;

typedef struct GrainliftHttpRequest {
	const char *method; // NUL-terminated, upper case
	const char *url;    // NUL-terminated absolute URL
	const GrainliftHttpHeader *headers;
	size_t n_headers;
	const uint8_t *body;
	size_t body_len;
	uint32_t timeout_ms;
	int32_t follow_redirects;
} GrainliftHttpRequest;

// Filled by the executor; released with the registered release callback.
typedef struct GrainliftHttpResponse {
	uint16_t status;
	const GrainliftHttpHeader *headers;
	size_t n_headers;
	const uint8_t *body;
	size_t body_len;
	// Transport failure (no HTTP response): NUL-terminated message.
	const char *error;
	int32_t retry_safe;
	void *private_data;
} GrainliftHttpResponse;

// Returns 0 when `out` holds an HTTP response (any status), non-zero on a
// transport failure (out->error set). `out->private_data` is owned by the host
// and freed by the release callback in both cases.
typedef int32_t (*GrainliftHttpExecuteFn)(void *host_ctx, const GrainliftHttpRequest *request,
                                          GrainliftHttpResponse *out);
typedef void (*GrainliftHttpReleaseFn)(GrainliftHttpResponse *response);

#define GRAINLIFT_HTTP_CAP_OPTIONS 1u                   // executor can send OPTIONS
#define GRAINLIFT_HTTP_CAP_TRANSPARENT_DECOMPRESSION 2u // transport decodes Content-Encoding

// Install the process-wide HTTP executor. `host_ctx` for each request comes
// from the database option GRAINLIFT_HOST_CTX_OPTION (decimal pointer value).
void grainlift_register_host_http(GrainliftHttpExecuteFn execute, GrainliftHttpReleaseFn release, uint32_t caps);

#define GRAINLIFT_HOST_CTX_OPTION "grainlift.internal.host_ctx"

// Prepare transport resources for a grainlift URI on the calling thread (0 on
// success/no-op). In DuckDB-WASM, iroh:// needs the page's Iroh adapter Worker,
// which can only be requested from DuckDB's main worker thread, so call this
// while binding.
int32_t grainlift_prepare_endpoint(const char *uri);

#ifdef __cplusplus
}
#endif
