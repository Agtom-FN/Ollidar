// serial_port.cpp — POSIX termios / Win32 DCB, and nothing else.
//
// Owner: A16. See serial_port.h for why this file is allowed to exist.
#include "serial_port.h"

#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace scanengine {
namespace discovery_serial {

const char* to_string(OpenResult r) {
  switch (r) {
    case OpenResult::kOk: return "ok";
    case OpenResult::kBusy: return "busy";
    case OpenResult::kNoAccess: return "no-access";
    case OpenResult::kNotFound: return "not-found";
    case OpenResult::kError: return "error";
  }
  return "?";
}

SerialPort::~SerialPort() { Close(); }

#if !defined(_WIN32)

namespace {

// The B-constants that exist everywhere we build. On Linux these are small
// opaque codes (B460800 == 0x1004) and the numeric value is NOT accepted; on
// Darwin they are the literal rates and arbitrary values work, which is how
// 460800 is reachable on a Mac whose termios.h stops at B230400.
speed_t platform_speed(std::uint32_t baud) {
  switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
#ifdef B230400
    case 230400: return B230400;
#endif
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
    default: break;
  }
#if defined(__APPLE__)
  return static_cast<speed_t>(baud);  // Darwin takes the rate itself
#else
  return 0;
#endif
}

// Darwin's "any baud rate" ioctl. Declared here rather than pulled from
// <IOKit/serial/ioss.h> so this file needs no IOKit headers and no framework.
#if defined(__APPLE__) && !defined(IOSSIOSPEED)
#define IOSSIOSPEED _IOW('T', 2, speed_t)
#endif

}  // namespace

OpenResult SerialPort::Open(const std::string& path, std::uint32_t baud) {
  Close();
  // O_NONBLOCK on the open() itself matters: without it a cu.* device with no
  // carrier blocks in open() until DCD asserts, which for a probe over four
  // ports is an indefinite hang rather than a scan.
  const int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) {
    switch (errno) {
      case EBUSY:
      case EAGAIN:
#ifdef ETXTBSY
      case ETXTBSY:
#endif
        return OpenResult::kBusy;
      case EACCES:
      case EPERM:
        return OpenResult::kNoAccess;
      case ENOENT:
      case ENXIO:
      case ENODEV:
        return OpenResult::kNotFound;
      default:
        return OpenResult::kError;
    }
  }

  // Claim it exclusively so we do not interleave reads with, say, a running
  // Livox tool or a second probe. Advisory; failure is not fatal.
  (void)::ioctl(fd, TIOCEXCL);

  termios t;
  std::memset(&t, 0, sizeof(t));
  if (::tcgetattr(fd, &t) != 0) {
    ::close(fd);
    return OpenResult::kError;
  }
  ::cfmakeraw(&t);
  t.c_cflag |= (CLOCAL | CREAD);
  t.c_cflag &= static_cast<tcflag_t>(~(PARENB | CSTOPB | CSIZE));
  t.c_cflag |= CS8;  // 8N1 — the D6's and the UM982's framing both
#ifdef CRTSCTS
  t.c_cflag &= static_cast<tcflag_t>(~CRTSCTS);
#endif
  t.c_cc[VMIN] = 0;
  t.c_cc[VTIME] = 0;

  const speed_t sp = platform_speed(baud);
  if (sp == 0) {
    ::close(fd);
    return OpenResult::kError;  // a rate this platform cannot express
  }
  (void)::cfsetispeed(&t, sp);
  (void)::cfsetospeed(&t, sp);
  if (::tcsetattr(fd, TCSANOW, &t) != 0) {
#if defined(__APPLE__)
    // Second chance on Darwin: standard rate first, then the exact one.
    (void)::cfsetispeed(&t, B9600);
    (void)::cfsetospeed(&t, B9600);
    if (::tcsetattr(fd, TCSANOW, &t) != 0) {
      ::close(fd);
      return OpenResult::kError;
    }
    speed_t want = static_cast<speed_t>(baud);
    if (::ioctl(fd, IOSSIOSPEED, &want) != 0) {
      ::close(fd);
      return OpenResult::kError;
    }
#else
    ::close(fd);
    return OpenResult::kError;
#endif
  }

  fd_ = fd;
  open_ = true;
  FlushInput();
  return OpenResult::kOk;
}

void SerialPort::Close() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  open_ = false;
}

void SerialPort::FlushInput() {
  if (fd_ >= 0) (void)::tcflush(fd_, TCIFLUSH);
}

int SerialPort::Read(std::uint8_t* buf, std::size_t cap, int timeout_ms) {
  if (fd_ < 0 || buf == nullptr || cap == 0) return -1;
  fd_set rs;
  FD_ZERO(&rs);
  FD_SET(fd_, &rs);
  timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  const int rc = ::select(fd_ + 1, &rs, nullptr, nullptr, &tv);
  if (rc == 0) return 0;
  if (rc < 0) return (errno == EINTR) ? 0 : -1;
  const ssize_t n = ::read(fd_, buf, cap);
  if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
  // A read of 0 on a tty means "nothing right now" (VMIN=0), not EOF.
  return static_cast<int>(n);
}

bool SerialPort::Write(const std::uint8_t* data, std::size_t n) {
  if (fd_ < 0 || data == nullptr) return false;
  std::size_t sent = 0;
  while (sent < n) {
    const ssize_t w = ::write(fd_, data + sent, n - sent);
    if (w > 0) {
      sent += static_cast<std::size_t>(w);
      continue;
    }
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
    return false;
  }
  (void)::tcdrain(fd_);
  return true;
}

#else  // ------------------------------- Windows -------------------------------

OpenResult SerialPort::Open(const std::string& path, std::uint32_t baud) {
  Close();
  // COM10 and above need the \\.\ prefix; COM1..9 tolerate it, so always use it.
  std::string full = path;
  if (full.rfind("\\\\.\\", 0) != 0) full = "\\\\.\\" + path;

  HANDLE h = ::CreateFileA(full.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    switch (::GetLastError()) {
      case ERROR_ACCESS_DENIED:
      case ERROR_SHARING_VIOLATION:
        return OpenResult::kBusy;  // Windows reports a held COM port this way
      case ERROR_FILE_NOT_FOUND:
      case ERROR_PATH_NOT_FOUND:
        return OpenResult::kNotFound;
      default:
        return OpenResult::kError;
    }
  }

  DCB dcb;
  std::memset(&dcb, 0, sizeof(dcb));
  dcb.DCBlength = sizeof(dcb);
  if (!::GetCommState(h, &dcb)) {
    ::CloseHandle(h);
    return OpenResult::kError;
  }
  dcb.BaudRate = baud;
  dcb.ByteSize = 8;
  dcb.Parity = NOPARITY;
  dcb.StopBits = ONESTOPBIT;
  dcb.fBinary = TRUE;
  dcb.fParity = FALSE;
  dcb.fOutxCtsFlow = FALSE;
  dcb.fOutxDsrFlow = FALSE;
  dcb.fDtrControl = DTR_CONTROL_ENABLE;
  dcb.fRtsControl = RTS_CONTROL_ENABLE;
  dcb.fOutX = FALSE;
  dcb.fInX = FALSE;
  if (!::SetCommState(h, &dcb)) {
    ::CloseHandle(h);
    return OpenResult::kError;
  }

  // Return whatever has arrived as soon as the interval or the total elapses:
  // a probe wants "what is on the wire now", never a blocking wait for a full
  // buffer.
  COMMTIMEOUTS to;
  std::memset(&to, 0, sizeof(to));
  to.ReadIntervalTimeout = 20;
  to.ReadTotalTimeoutConstant = 50;
  to.ReadTotalTimeoutMultiplier = 0;
  to.WriteTotalTimeoutConstant = 500;
  (void)::SetCommTimeouts(h, &to);

  handle_ = h;
  open_ = true;
  FlushInput();
  return OpenResult::kOk;
}

void SerialPort::Close() {
  if (handle_ != nullptr) ::CloseHandle(static_cast<HANDLE>(handle_));
  handle_ = nullptr;
  open_ = false;
}

void SerialPort::FlushInput() {
  if (handle_ != nullptr) {
    (void)::PurgeComm(static_cast<HANDLE>(handle_), PURGE_RXCLEAR | PURGE_RXABORT);
  }
}

int SerialPort::Read(std::uint8_t* buf, std::size_t cap, int timeout_ms) {
  if (handle_ == nullptr || buf == nullptr || cap == 0) return -1;
  COMMTIMEOUTS to;
  std::memset(&to, 0, sizeof(to));
  to.ReadIntervalTimeout = 20;
  to.ReadTotalTimeoutConstant = static_cast<DWORD>(timeout_ms < 0 ? 0 : timeout_ms);
  (void)::SetCommTimeouts(static_cast<HANDLE>(handle_), &to);
  DWORD got = 0;
  if (!::ReadFile(static_cast<HANDLE>(handle_), buf, static_cast<DWORD>(cap), &got, nullptr)) {
    return -1;
  }
  return static_cast<int>(got);
}

bool SerialPort::Write(const std::uint8_t* data, std::size_t n) {
  if (handle_ == nullptr || data == nullptr) return false;
  DWORD written = 0;
  if (!::WriteFile(static_cast<HANDLE>(handle_), data, static_cast<DWORD>(n), &written,
                   nullptr)) {
    return false;
  }
  return written == n;
}

#endif

}  // namespace discovery_serial
}  // namespace scanengine
