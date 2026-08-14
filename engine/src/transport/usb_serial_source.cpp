#include "scanengine/transport/usb_serial_source.h"

#include "scanengine/core/log.h"

namespace scanengine {
namespace {
constexpr const char* kMod = "serial";
}

UsbSerialSource::UsbSerialSource(const UsbSerialConfig& cfg) : cfg_(cfg) {
  if (cfg_.port_name == nullptr) cfg_.port_name = "";
}

UsbSerialSource::~UsbSerialSource() = default;

Status UsbSerialSource::start() {
  ByteSink sink_copy;
  {
    std::lock_guard<std::mutex> lock(m_);
    if (running_) return kOkStatus;
    sink_copy = sink_;
  }
  if (!sink_copy) {
    return set_last_error(ScanError::kInvalidState,
                          "usb serial source '%s': no sink installed", cfg_.port_name);
  }
  {
    std::lock_guard<std::mutex> lock(m_);
    running_ = true;
  }
  SCAN_LOG_INFO(kMod, "started '%s' @ %u baud", cfg_.port_name, cfg_.baud);
  return kOkStatus;
}

Status UsbSerialSource::stop() {
  std::lock_guard<std::mutex> lock(m_);
  running_ = false;
  return kOkStatus;
}

bool UsbSerialSource::running() const {
  std::lock_guard<std::mutex> lock(m_);
  return running_;
}

void UsbSerialSource::set_sink(ByteSink sink) {
  std::lock_guard<std::mutex> lock(m_);
  sink_ = std::move(sink);
}

Status UsbSerialSource::write(ByteSpan bytes) {
  SerialWriteFn fn = cfg_.write_fn;
  if (fn == nullptr) {
    return set_last_error(ScanError::kNotSupported,
                          "usb serial source '%s' has no write function (app did not "
                          "provide one)", cfg_.port_name);
  }
  if (bytes.empty()) return kOkStatus;
  const ScanError e = fn(bytes.data(), bytes.size(), cfg_.write_user_data);
  std::lock_guard<std::mutex> lock(m_);
  if (e != ScanError::kOk) {
    ++stats_.write_errors;
    return set_last_error(e, "usb serial source '%s': write of %zu bytes failed",
                          cfg_.port_name, bytes.size());
  }
  stats_.bytes_out += bytes.size();
  return kOkStatus;
}

Status UsbSerialSource::push(ByteSpan bytes, TimePoint t_arrival) {
  if (bytes.empty()) return kOkStatus;
  if (bytes.data() == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "usb serial source: null buffer");
  }
  if (t_arrival.nanos == 0) t_arrival = SteadyClock::now();

  ByteSink sink_copy;
  {
    std::lock_guard<std::mutex> lock(m_);
    if (!running_) {
      return set_last_error(ScanError::kInvalidState,
                            "usb serial source '%s': push() before start()", cfg_.port_name);
    }
    stats_.bytes_in += bytes.size();
    ++stats_.chunks_in;
    stats_.t_last_rx_ns = t_arrival.nanos;
    sink_copy = sink_;
  }
  // Sink runs outside the lock: it is the whole parse pipeline and may take
  // milliseconds; holding the transport lock across it would serialize
  // stats() calls from a UI thread against capture.
  if (sink_copy) sink_copy(bytes, t_arrival);
  return kOkStatus;
}

TransportStats UsbSerialSource::stats() const {
  std::lock_guard<std::mutex> lock(m_);
  return stats_;
}

}  // namespace scanengine
