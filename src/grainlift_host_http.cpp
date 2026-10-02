// HTTP executor for the grainlift driver, implemented on DuckDB's HTTPUtil.
//
// In DuckDB-WASM (haybarn-wasm) HTTPUtil is HTTPWasmUtil, a synchronous
// XMLHttpRequest bridge; natively it is httpfs. Either way the Rust driver
// sees one blocking request/response call per vgi-rpc round trip.

#include "grainlift_host_http.hpp"

#include "duckdb/common/http_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/database.hpp"

#include <cstring>

namespace adbc_scanner {
using namespace duckdb;

namespace {

// Owns everything a GrainliftHttpResponse points at.
struct HostResponse {
	vector<pair<string, string>> header_storage;
	vector<GrainliftHttpHeader> headers;
	string body;
	string error;
};

int32_t Fail(GrainliftHttpResponse *out, HostResponse *owned, string message) {
	owned->error = std::move(message);
	out->error = owned->error.c_str();
	out->retry_safe = 0;
	return 1;
}

void SetTimeout(HTTPParams &params, uint32_t timeout_ms) {
	if (timeout_ms == 0) {
		return;
	}
	params.timeout = timeout_ms / 1000;
	params.timeout_usec = (timeout_ms % 1000) * 1000;
}

int32_t Execute(void *host_ctx, const GrainliftHttpRequest *request, GrainliftHttpResponse *out) {
	std::memset(out, 0, sizeof(*out));
	auto owned = new HostResponse();
	out->private_data = owned;
	try {
		auto &ctx = *static_cast<GrainliftHostContext *>(host_ctx);
		auto db = ctx.db.lock();
		// Shutting down: the catalog is closing its sessions (see the context).
		auto shutting_down = !db;
		auto *instance = db ? db.get() : ctx.instance;
		if (!instance) {
			return Fail(out, owned, "grainlift: DuckDB instance is no longer available");
		}
		string method(request->method);
		string url(request->url);

		auto &http_util = HTTPUtil::Get(*instance);
		auto params = http_util.InitializeParameters(*instance, url);
		// Closing sessions is best effort: an unreachable gateway must not stall
		// DuckDB's exit for the full request timeout.
		SetTimeout(*params, shutting_down ? std::min<uint32_t>(request->timeout_ms, 2000) : request->timeout_ms);
		// vgi-rpc owns retry policy; a transparent retry here could replay a
		// non-idempotent stream exchange.
		params->retries = 0;
		params->follow_location = request->follow_redirects != 0;

		HTTPHeaders headers;
		string content_type = "application/octet-stream";
		for (size_t i = 0; i < request->n_headers; i++) {
			auto &header = request->headers[i];
			string name(header.name, header.name_len);
			string value(header.value, header.value_len);
			if (StringUtil::CIEquals(name, "Content-Type")) {
				content_type = value;
				if (method == "PUT") {
					continue;
				}
			}
			headers.Insert(std::move(name), std::move(value));
		}

		auto body = reinterpret_cast<const_data_ptr_t>(request->body);
		auto body_len = static_cast<idx_t>(request->body_len);
		unique_ptr<HTTPResponse> response;
		string streamed_body;
		bool body_in_buffer = false;
		if (method == "POST") {
			PostRequestInfo post(url, headers, *params, body, body_len);
			post.try_request = true;
			response = http_util.Request(post);
			streamed_body = std::move(post.buffer_out);
			body_in_buffer = true;
		} else if (method == "PUT") {
			PutRequestInfo put(url, headers, *params, body, body_len, content_type);
			put.try_request = true;
			response = http_util.Request(put);
		} else if (method == "GET") {
			GetRequestInfo get(
			    url, headers, *params, [](const HTTPResponse &) { return true; },
			    [&streamed_body](const_data_ptr_t data, idx_t length) {
				    streamed_body.append(reinterpret_cast<const char *>(data), length);
				    return true;
			    });
			get.try_request = true;
			response = http_util.Request(get);
			body_in_buffer = true;
		} else if (method == "HEAD") {
			HeadRequestInfo head(url, headers, *params);
			head.try_request = true;
			response = http_util.Request(head);
		} else if (method == "DELETE") {
			DeleteRequestInfo del(url, headers, *params);
			del.try_request = true;
			response = http_util.Request(del);
		} else {
			return Fail(out, owned, "grainlift: HTTP method " + method + " is not supported by DuckDB's HTTP layer");
		}

		if (!response) {
			return Fail(out, owned, "grainlift: HTTP " + method + " " + url + " returned no response");
		}
		if (response->HasRequestError()) {
			return Fail(out, owned,
			            "grainlift: HTTP " + method + " " + url + " failed: " + response->GetRequestError());
		}
#ifdef __EMSCRIPTEN__
		// HTTPWasmClient reports a blocked (CORS) or failed XMLHttpRequest as a
		// synthetic 404 carrying this reason; surface it as a transport error.
		if (StringUtil::Contains(response->reason, "XMLHttpRequest failed")) {
			return Fail(out, owned,
			            "grainlift: HTTP " + method + " " + url + " failed: " + response->reason +
			                ". The grainlift server must allow this page's origin (CORS).");
		}
#endif

		out->status = static_cast<uint16_t>(response->status);
		for (auto &header : response->headers) {
			owned->header_storage.emplace_back(header.first, header.second);
		}
		for (auto &header : owned->header_storage) {
			owned->headers.push_back(
			    {header.first.data(), header.first.size(), header.second.data(), header.second.size()});
		}
		out->headers = owned->headers.data();
		out->n_headers = owned->headers.size();

		owned->body = body_in_buffer && !streamed_body.empty() ? std::move(streamed_body) : std::move(response->body);
		out->body = reinterpret_cast<const uint8_t *>(owned->body.data());
		out->body_len = owned->body.size();
		return 0;
	} catch (std::exception &ex) {
		ErrorData error(ex);
		return Fail(out, owned, "grainlift: HTTP request failed: " + error.RawMessage());
	} catch (...) {
		return Fail(out, owned, "grainlift: HTTP request failed with an unknown error");
	}
}

void Release(GrainliftHttpResponse *response) {
	delete static_cast<HostResponse *>(response->private_data);
	std::memset(response, 0, sizeof(*response));
}

} // namespace

void RegisterGrainliftHostHttp() {
	// DuckDB's HTTP layer has no OPTIONS request type. Compression is
	// negotiated with X-VGI-Accept-Encoding / X-VGI-Content-Encoding, which the
	// browser (and httpfs) pass through untouched, so the driver decodes the
	// payload itself and treats any standard Content-Encoding as already
	// handled by the transport.
	grainlift_register_host_http(Execute, Release, GRAINLIFT_HTTP_CAP_TRANSPARENT_DECOMPRESSION);
}

shared_ptr<GrainliftHostContext> CreateGrainliftHostContext(DatabaseInstance &db) {
	auto ctx = make_shared_ptr<GrainliftHostContext>();
	ctx->db = db.shared_from_this();
	ctx->instance = &db;
	return ctx;
}

string GrainliftHostContextOption(const GrainliftHostContext &ctx) {
	return std::to_string(reinterpret_cast<uintptr_t>(&ctx));
}

} // namespace adbc_scanner
