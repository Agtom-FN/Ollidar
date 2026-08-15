// QtHttpTransport.h — a real socket-backed scanengine::jobs::HttpTransport
// (engine/include/scanengine/jobs/http_transport.h), over Qt's
// QNetworkAccessManager.
//
// A15's own doc is explicit that CloudSubmitClient is tested ONLY against a
// scripted fake transport, and that "the real socket/TLS-backed
// implementation is D3's". The desktop is not D3, but the task is explicit
// that "the fake-transport path is not for UI" — the Submit-to-cloud dialog
// has to make a REAL network attempt (and fail gracefully, since no server
// exists yet) rather than simulate one. This is that adapter: nothing more
// than QNetworkAccessManager wired to HttpTransport::request()'s one-call-one-
// round-trip contract.
//
// THREADING. JobQueue runs a kCloudSubmit job on its own single worker
// thread (not the Qt GUI thread), and CloudSubmitClient calls request()
// synchronously, once per resumable-upload chunk / poll / download. A
// QNetworkAccessManager works on any thread as long as that thread can run a
// local Qt event loop; request() below spins a QEventLoop until the reply
// finishes (or a timeout fires), which is the standard pattern for making an
// asynchronous QNetworkReply behave like a blocking call. A thread_local
// QNetworkAccessManager is reused across calls on the queue's one worker
// thread rather than rebuilt per request.
//
// Owner: C4.
#pragma once

#include <QString>

#include "scanengine/jobs/http_transport.h"

namespace lidarscan {

class QtHttpTransport : public scanengine::jobs::HttpTransport {
 public:
  // Per-request wall-clock timeout — bounds how long a kCloudSubmit job can
  // sit inside one request() call when nothing is listening at all (the
  // expected case, since no server exists yet: connection refused usually
  // returns fast, but a black-holed address needs a ceiling).
  explicit QtHttpTransport(int timeout_ms = 8000) : timeout_ms_(timeout_ms) {}

  scanengine::jobs::HttpResponse request(const scanengine::jobs::HttpRequest& req) override;

 private:
  int timeout_ms_;
};

}  // namespace lidarscan
