#include "scanengine/jobs/cloud_submit.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace scanengine {
namespace jobs {

namespace {

// --- a tiny, deliberately non-general JSON reader/writer -------------------
// Both sides of this contract (CloudSubmitClient and, in tests, the fake
// server) are ours: the shape is always one flat object, string/number
// values only, no nesting, no arrays. Hand-rolled rather than a vcpkg JSON
// dependency for the same reason A7 hand-rolled its pose graph solver
// (docs/A7-post.md §4) — one narrow, fully-controlled shape does not need a
// general parser, and this task adds no network/parsing dependency either.
std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '"' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

std::string json_string_value(const std::string& s) { return "\"" + json_escape(s) + "\""; }

std::string json_object(const std::vector<std::pair<std::string, std::string>>& fields) {
  std::string out = "{";
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (i != 0) out += ",";
    out += "\"" + fields[i].first + "\":" + fields[i].second;
  }
  out += "}";
  return out;
}

// Finds "key": <value> at the top level and returns it unquoted (for a
// string) or verbatim (for a bare number/true/false/null token).
bool json_find_raw(const std::string& body, const std::string& key, std::string* raw_out) {
  const std::string needle = "\"" + key + "\"";
  std::size_t pos = body.find(needle);
  if (pos == std::string::npos) return false;
  pos = body.find(':', pos + needle.size());
  if (pos == std::string::npos) return false;
  ++pos;
  while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) ++pos;
  if (pos >= body.size()) return false;
  if (body[pos] == '"') {
    std::string out;
    ++pos;
    while (pos < body.size() && body[pos] != '"') {
      if (body[pos] == '\\' && pos + 1 < body.size()) ++pos;
      out.push_back(body[pos]);
      ++pos;
    }
    *raw_out = out;
    return true;
  }
  std::size_t end = pos;
  while (end < body.size() && body[end] != ',' && body[end] != '}' && body[end] != ' ' &&
         body[end] != '\n' && body[end] != '\r' && body[end] != '\t') {
    ++end;
  }
  *raw_out = body.substr(pos, end - pos);
  return true;
}

bool json_get_string(const std::string& body, const std::string& key, std::string* out) {
  return json_find_raw(body, key, out);
}

bool json_get_double(const std::string& body, const std::string& key, double* out) {
  std::string raw;
  if (!json_find_raw(body, key, &raw)) return false;
  char* end = nullptr;
  *out = std::strtod(raw.c_str(), &end);
  return end != raw.c_str();
}

std::string bytes_to_string(const std::vector<std::uint8_t>& body) {
  return std::string(reinterpret_cast<const char*>(body.data()), body.size());
}

std::vector<std::uint8_t> string_to_bytes(const std::string& s) {
  return std::vector<std::uint8_t>(s.begin(), s.end());
}

CloudJobState state_from_string(const std::string& s) {
  if (s == "queued") return CloudJobState::kQueued;
  if (s == "uploading") return CloudJobState::kUploading;
  if (s == "processing") return CloudJobState::kProcessing;
  if (s == "done") return CloudJobState::kDone;
  if (s == "failed") return CloudJobState::kFailed;
  return CloudJobState::kUnknown;
}

// Sleeps `total_ms`, checking `cancelled` every `slice_ms` so a cancellation
// during a backoff/poll wait is prompt rather than blocking for the whole
// interval — the same granularity discipline post::CancelToken and
// export/'s ExportCancelToken use for their own loops.
bool sleep_cancellable(std::uint32_t total_ms, const CloudCancelledFn& cancelled) {
  std::uint32_t waited = 0;
  const std::uint32_t slice_ms = 20;
  while (waited < total_ms) {
    if (cancelled && cancelled()) return true;
    const std::uint32_t step = std::min(slice_ms, total_ms - waited);
    if (step > 0) std::this_thread::sleep_for(std::chrono::milliseconds(step));
    waited += step;
  }
  return cancelled && cancelled();
}

}  // namespace

const char* to_string(CloudJobState s) noexcept {
  switch (s) {
    case CloudJobState::kUnknown: return "unknown";
    case CloudJobState::kQueued: return "queued";
    case CloudJobState::kUploading: return "uploading";
    case CloudJobState::kProcessing: return "processing";
    case CloudJobState::kDone: return "done";
    case CloudJobState::kFailed: return "failed";
  }
  return "unknown";
}

CloudSubmitClient::CloudSubmitClient(HttpTransport& transport, CloudSubmitConfig config)
    : transport_(transport), cfg_(std::move(config)) {}

std::string CloudSubmitClient::auth_header_value() const { return "Bearer " + cfg_.auth_token; }

// Retries a transport-level failure (no response at all) or a 5xx response
// up to `max_retries` times with exponential backoff. A real HTTP status the
// transport DID deliver (401, 413, 4xx in general) is never retried here —
// those are the caller's decision, made in submit()/poll()/download_result().
HttpResponse CloudSubmitClient::send_with_retry(const HttpRequest& req, CloudCancelledFn cancelled,
                                                 bool* cancelled_out) {
  *cancelled_out = false;
  std::uint32_t backoff = cfg_.backoff_initial_ms;
  HttpResponse resp;
  for (int attempt = 0; attempt <= cfg_.max_retries; ++attempt) {
    if (cancelled && cancelled()) {
      *cancelled_out = true;
      return resp;
    }
    resp = transport_.request(req);
    const bool retryable = !resp.transport_ok || resp.status_code >= 500;
    if (!retryable || attempt == cfg_.max_retries) return resp;
    if (sleep_cancellable(backoff, cancelled)) {
      *cancelled_out = true;
      return resp;
    }
    backoff = static_cast<std::uint32_t>(
        std::min<double>(cfg_.backoff_max_ms, static_cast<double>(backoff) * cfg_.backoff_multiplier));
  }
  return resp;
}

Result<std::string> CloudSubmitClient::submit(const std::string& local_zip_path,
                                               CloudProgressFn progress_cb, CloudCancelledFn cancelled) {
  std::error_code ec;
  const auto sz = std::filesystem::file_size(local_zip_path, ec);
  if (ec) {
    return set_last_error(ScanError::kIoError, "jobs/cloud_submit: cannot stat '%s'",
                           local_zip_path.c_str());
  }
  const std::uint64_t total_bytes = static_cast<std::uint64_t>(sz);
  if (total_bytes > cfg_.max_upload_bytes) {
    return set_last_error(ScanError::kCapacityExceeded,
                           "jobs/cloud_submit: %llu bytes exceeds the %llu-byte MVP cap (Tech Spec §3.8)",
                           static_cast<unsigned long long>(total_bytes),
                           static_cast<unsigned long long>(cfg_.max_upload_bytes));
  }

  std::ifstream file(local_zip_path, std::ios::binary);
  if (!file) {
    return set_last_error(ScanError::kIoError, "jobs/cloud_submit: cannot open '%s'",
                           local_zip_path.c_str());
  }

  // --- POST /jobs ---------------------------------------------------------
  HttpRequest create_req;
  create_req.method = HttpMethod::kPost;
  create_req.url = cfg_.base_url + "/jobs";
  create_req.headers = {{"Authorization", auth_header_value()}, {"Content-Type", "application/json"}};
  create_req.body = string_to_bytes(
      json_object({{"kind", json_string_value("lscan")}, {"size_bytes", std::to_string(total_bytes)}}));

  bool cancelled_flag = false;
  const HttpResponse create_resp = send_with_retry(create_req, cancelled, &cancelled_flag);
  if (cancelled_flag) return set_last_error(ScanError::kCancelled, "jobs/cloud_submit: cancelled");
  if (!create_resp.transport_ok) {
    return set_last_error(ScanError::kNetworkError,
                           "jobs/cloud_submit: POST /jobs: no response (disconnected)");
  }
  if (create_resp.status_code == 401) {
    return set_last_error(ScanError::kPermissionDenied, "jobs/cloud_submit: POST /jobs: token rejected");
  }
  if (create_resp.status_code == 413) {
    return set_last_error(ScanError::kCapacityExceeded,
                           "jobs/cloud_submit: POST /jobs: server rejected upload size");
  }
  if (create_resp.status_code != 201) {
    return set_last_error(ScanError::kUnknown, "jobs/cloud_submit: POST /jobs: unexpected status %d",
                           create_resp.status_code);
  }
  const std::string create_body = bytes_to_string(create_resp.body);
  std::string job_id, upload_path;
  if (!json_get_string(create_body, "id", &job_id) ||
      !json_get_string(create_body, "upload_url", &upload_path)) {
    return set_last_error(ScanError::kCorruptData,
                           "jobs/cloud_submit: POST /jobs: malformed response body");
  }
  const std::string upload_url = cfg_.base_url + upload_path;

  // --- chunked upload, Content-Range per chunk ----------------------------
  std::uint64_t offset = 0;
  std::vector<std::uint8_t> chunk;
  chunk.reserve(cfg_.chunk_bytes);
  while (offset < total_bytes) {
    if (cancelled && cancelled()) {
      return set_last_error(ScanError::kCancelled, "jobs/cloud_submit: cancelled mid-upload");
    }
    const std::uint64_t remaining = total_bytes - offset;
    const std::uint64_t n = std::min<std::uint64_t>(cfg_.chunk_bytes, remaining);
    chunk.resize(static_cast<std::size_t>(n));
    file.seekg(static_cast<std::streamoff>(offset));
    file.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(n));
    if (static_cast<std::uint64_t>(file.gcount()) != n) {
      return set_last_error(ScanError::kIoError,
                             "jobs/cloud_submit: short read staging chunk at offset %llu",
                             static_cast<unsigned long long>(offset));
    }

    HttpRequest put_req;
    put_req.method = HttpMethod::kPut;
    put_req.url = upload_url;
    std::ostringstream range;
    range << "bytes " << offset << "-" << (offset + n - 1) << "/" << total_bytes;
    put_req.headers = {{"Authorization", auth_header_value()}, {"Content-Range", range.str()}};
    put_req.body = chunk;

    const HttpResponse resp = send_with_retry(put_req, cancelled, &cancelled_flag);
    if (cancelled_flag) return set_last_error(ScanError::kCancelled, "jobs/cloud_submit: cancelled");

    if (!resp.transport_ok) {
      // Retries exhausted on this chunk. Query the server for the
      // authoritative received-offset and continue from there — the
      // "disconnect mid-upload, then resume" path.
      HttpRequest probe;
      probe.method = HttpMethod::kPut;
      probe.url = upload_url;
      std::ostringstream probe_range;
      probe_range << "bytes */" << total_bytes;
      probe.headers = {{"Authorization", auth_header_value()}, {"Content-Range", probe_range.str()}};

      bool probe_cancelled = false;
      const HttpResponse probe_resp = send_with_retry(probe, cancelled, &probe_cancelled);
      if (probe_cancelled) return set_last_error(ScanError::kCancelled, "jobs/cloud_submit: cancelled");
      if (!probe_resp.transport_ok) {
        return set_last_error(ScanError::kDisconnected,
                               "jobs/cloud_submit: upload disconnected and the resume probe also failed");
      }
      const std::string* off_hdr = find_header(probe_resp.headers, "Upload-Offset");
      if ((probe_resp.status_code != 308 && probe_resp.status_code != 200) || off_hdr == nullptr) {
        return set_last_error(ScanError::kNetworkError,
                               "jobs/cloud_submit: resume probe returned no Upload-Offset (status %d)",
                               probe_resp.status_code);
      }
      offset = std::strtoull(off_hdr->c_str(), nullptr, 10);
      if (offset > total_bytes) {
        return set_last_error(ScanError::kCorruptData,
                               "jobs/cloud_submit: resume probe offset %llu exceeds file size %llu",
                               static_cast<unsigned long long>(offset),
                               static_cast<unsigned long long>(total_bytes));
      }
      if (progress_cb) {
        progress_cb(total_bytes == 0 ? 1.f : static_cast<float>(offset) / static_cast<float>(total_bytes));
      }
      continue;  // resume the loop from the server's offset
    }

    if (resp.status_code == 401) {
      return set_last_error(ScanError::kPermissionDenied, "jobs/cloud_submit: PUT upload: token rejected");
    }
    if (resp.status_code != 200 && resp.status_code != 201) {
      return set_last_error(ScanError::kUnknown, "jobs/cloud_submit: PUT upload: unexpected status %d",
                             resp.status_code);
    }

    offset += n;
    if (progress_cb) {
      progress_cb(total_bytes == 0 ? 1.f : static_cast<float>(offset) / static_cast<float>(total_bytes));
    }
  }

  if (progress_cb) progress_cb(1.f);
  return job_id;
}

Result<CloudJobStatus> CloudSubmitClient::poll(const std::string& cloud_job_id) {
  HttpRequest req;
  req.method = HttpMethod::kGet;
  req.url = cfg_.base_url + "/jobs/" + cloud_job_id;
  req.headers = {{"Authorization", auth_header_value()}};

  bool cancelled_flag = false;
  const HttpResponse resp = send_with_retry(req, nullptr, &cancelled_flag);
  if (!resp.transport_ok) {
    return set_last_error(ScanError::kNetworkError, "jobs/cloud_submit: GET /jobs/%s: no response",
                           cloud_job_id.c_str());
  }
  if (resp.status_code == 401) {
    return set_last_error(ScanError::kPermissionDenied, "jobs/cloud_submit: GET /jobs/%s: token rejected",
                           cloud_job_id.c_str());
  }
  if (resp.status_code == 404) {
    return set_last_error(ScanError::kNotFound, "jobs/cloud_submit: GET /jobs/%s: not found",
                           cloud_job_id.c_str());
  }
  if (resp.status_code != 200) {
    return set_last_error(ScanError::kUnknown, "jobs/cloud_submit: GET /jobs/%s: unexpected status %d",
                           cloud_job_id.c_str(), resp.status_code);
  }

  const std::string body = bytes_to_string(resp.body);
  CloudJobStatus st;
  st.cloud_job_id = cloud_job_id;
  std::string state_str;
  json_get_string(body, "state", &state_str);
  st.state = state_from_string(state_str);
  double p = 0.0;
  json_get_double(body, "progress", &p);
  st.progress = static_cast<float>(p);
  json_get_string(body, "message", &st.message);
  return st;
}

Result<CloudJobStatus> CloudSubmitClient::wait_until_terminal(const std::string& cloud_job_id,
                                                               CloudStatusFn status_cb,
                                                               CloudCancelledFn cancelled) {
  std::uint32_t attempts = 0;
  for (;;) {
    if (cancelled && cancelled()) {
      return set_last_error(ScanError::kCancelled, "jobs/cloud_submit: cancelled while polling");
    }
    const Result<CloudJobStatus> r = poll(cloud_job_id);
    if (!r.ok()) return r.status();
    if (status_cb) status_cb(r.value());
    if (r.value().state == CloudJobState::kDone || r.value().state == CloudJobState::kFailed) return r;

    ++attempts;
    if (cfg_.max_poll_attempts != 0 && attempts >= cfg_.max_poll_attempts) {
      return set_last_error(ScanError::kTimeout, "jobs/cloud_submit: exceeded max_poll_attempts (%u)",
                             cfg_.max_poll_attempts);
    }
    if (sleep_cancellable(cfg_.poll_interval_ms, cancelled)) {
      return set_last_error(ScanError::kCancelled, "jobs/cloud_submit: cancelled while polling");
    }
  }
}

Status CloudSubmitClient::download_result(const std::string& cloud_job_id, const std::string& dest_path) {
  HttpRequest req;
  req.method = HttpMethod::kGet;
  req.url = cfg_.base_url + "/jobs/" + cloud_job_id + "/result";
  req.headers = {{"Authorization", auth_header_value()}};

  bool cancelled_flag = false;
  const HttpResponse resp = send_with_retry(req, nullptr, &cancelled_flag);
  if (!resp.transport_ok) {
    return set_last_error(ScanError::kNetworkError, "jobs/cloud_submit: download: no response");
  }
  if (resp.status_code == 401) {
    return set_last_error(ScanError::kPermissionDenied, "jobs/cloud_submit: download: token rejected");
  }
  if (resp.status_code == 404) {
    return set_last_error(ScanError::kNotFound, "jobs/cloud_submit: download: result not ready");
  }
  if (resp.status_code != 200) {
    return set_last_error(ScanError::kUnknown, "jobs/cloud_submit: download: unexpected status %d",
                           resp.status_code);
  }

  std::ofstream out(dest_path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return set_last_error(ScanError::kIoError, "jobs/cloud_submit: cannot open '%s' for write",
                           dest_path.c_str());
  }
  out.write(reinterpret_cast<const char*>(resp.body.data()), static_cast<std::streamsize>(resp.body.size()));
  if (!out) {
    return set_last_error(ScanError::kIoError, "jobs/cloud_submit: write failed for '%s'",
                           dest_path.c_str());
  }
  return kOkStatus;
}

}  // namespace jobs
}  // namespace scanengine
