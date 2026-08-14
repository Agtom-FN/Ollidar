#include "scanengine/jobs/http_transport.h"

namespace scanengine {
namespace jobs {

const char* to_string(HttpMethod m) noexcept {
  switch (m) {
    case HttpMethod::kGet: return "GET";
    case HttpMethod::kPost: return "POST";
    case HttpMethod::kPut: return "PUT";
  }
  return "?";
}

const std::string* find_header(const std::vector<HttpHeader>& headers, const std::string& name) {
  for (const auto& h : headers) {
    if (h.name == name) return &h.value;
  }
  return nullptr;
}

}  // namespace jobs
}  // namespace scanengine
