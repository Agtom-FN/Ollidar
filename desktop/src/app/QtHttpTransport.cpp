#include "app/QtHttpTransport.h"

#include <QByteArray>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace lidarscan {

scanengine::jobs::HttpResponse QtHttpTransport::request(const scanengine::jobs::HttpRequest& req) {
  using scanengine::jobs::HttpHeader;
  using scanengine::jobs::HttpMethod;
  using scanengine::jobs::HttpResponse;

  // Reused across calls on whichever thread calls request() — JobQueue's one
  // worker thread for every real submission — rather than a fresh manager
  // (and its own thread-affine socket engine startup cost) per chunk.
  thread_local QNetworkAccessManager mgr;

  QNetworkRequest qreq{QUrl(QString::fromStdString(req.url))};
  for (const HttpHeader& h : req.headers) {
    qreq.setRawHeader(QByteArray::fromStdString(h.name), QByteArray::fromStdString(h.value));
  }
  qreq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

  const QByteArray body(reinterpret_cast<const char*>(req.body.data()),
                        static_cast<int>(req.body.size()));

  QNetworkReply* reply = nullptr;
  switch (req.method) {
    case HttpMethod::kGet:
      reply = mgr.get(qreq);
      break;
    case HttpMethod::kPost:
      reply = mgr.post(qreq, body);
      break;
    case HttpMethod::kPut:
      reply = mgr.put(qreq, body);
      break;
  }

  HttpResponse resp;
  if (reply == nullptr) return resp;  // transport_ok stays false

  QEventLoop loop;
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
  QTimer timeout_timer;
  timeout_timer.setSingleShot(true);
  bool timed_out = false;
  QObject::connect(&timeout_timer, &QTimer::timeout, &loop, [&] {
    timed_out = true;
    reply->abort();
  });
  timeout_timer.start(timeout_ms_);
  loop.exec();

  const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  const QNetworkReply::NetworkError err = reply->error();

  // "No response at all" — connection refused, DNS failure, timeout, or
  // anything else that never produced a real HTTP status line — is exactly
  // HttpResponse::transport_ok == false's documented meaning (the flag
  // CloudSubmitClient's retry/resume logic keys off). A real 4xx/5xx the
  // server DID send is transport_ok == true with that status_code.
  if (status == 0) {
    resp.transport_ok = false;
    resp.status_code = 0;
    if (timed_out) {
      resp.body.assign({'t', 'i', 'm', 'e', 'o', 'u', 't'});
    } else {
      const QByteArray msg = reply->errorString().toUtf8();
      resp.body.assign(msg.begin(), msg.end());
    }
  } else {
    resp.transport_ok = true;
    resp.status_code = status;
    const auto raw_headers = reply->rawHeaderPairs();
    resp.headers.reserve(static_cast<std::size_t>(raw_headers.size()));
    for (const auto& kv : raw_headers) {
      resp.headers.push_back(HttpHeader{kv.first.toStdString(), kv.second.toStdString()});
    }
    const QByteArray b = reply->readAll();
    resp.body.assign(b.begin(), b.end());
  }
  (void)err;

  reply->deleteLater();
  return resp;
}

}  // namespace lidarscan
