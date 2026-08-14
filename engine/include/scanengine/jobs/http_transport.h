// http_transport.h — the pluggable seam CloudSubmitClient (jobs/cloud_submit.h)
// is written against.
//
// A15 does not add a network dependency. `HttpTransport` is the interface
// every request in the cloud REST contract (docs/A15-jobs.md) goes through;
// CloudSubmitClient is implemented and fully tested against a scripted fake
// (tests/test_jobs.cpp). The real socket-backed implementation (TLS, actual
// DNS/connect, real timeouts) is D3's — a small adapter over whatever HTTP
// library the cloud workstream picks, satisfying exactly this interface.
//
// One `request()` call is ONE HTTP round trip; resumable upload is built on
// top of it by CloudSubmitClient issuing one call per chunk with a
// Content-Range header, not by this interface streaming internally. That
// keeps the seam trivial to fake: a test transport is a pure function of
// (method, url, headers, body) -> response, with no long-lived connection
// state of its own.
//
// Owner: A15.
#ifndef SCANENGINE_JOBS_HTTP_TRANSPORT_H
#define SCANENGINE_JOBS_HTTP_TRANSPORT_H

#include <cstdint>
#include <string>
#include <vector>

namespace scanengine {
namespace jobs {

enum class HttpMethod : std::uint8_t {
  kGet = 0,
  kPost = 1,
  kPut = 2,
};

const char* to_string(HttpMethod m) noexcept;

struct HttpHeader {
  std::string name;
  std::string value;
};

struct HttpRequest {
  HttpMethod method = HttpMethod::kGet;
  std::string url;
  std::vector<HttpHeader> headers;
  // The body of ONE request — for a resumable-upload chunk this is just
  // that chunk's bytes, never the whole file. Empty for GET and for a
  // Content-Range probe ("bytes */total") used to query resume offset.
  std::vector<std::uint8_t> body;
};

struct HttpResponse {
  // False means the request never got a response at all — a simulated
  // disconnect, a timeout, a DNS failure. status_code/headers/body are
  // meaningless when this is false. This is the flag CloudSubmitClient's
  // retry/resume logic keys off of; it does NOT retry on a real HTTP
  // status (4xx/5xx) it received, only on the absence of one.
  bool transport_ok = false;
  int status_code = 0;
  std::vector<HttpHeader> headers;
  std::vector<std::uint8_t> body;
};

const std::string* find_header(const std::vector<HttpHeader>& headers, const std::string& name);

class HttpTransport {
 public:
  virtual ~HttpTransport() = default;
  virtual HttpResponse request(const HttpRequest& req) = 0;
};

}  // namespace jobs
}  // namespace scanengine

#endif  // SCANENGINE_JOBS_HTTP_TRANSPORT_H
