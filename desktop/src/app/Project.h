// Project.h — reading, creating and importing `.lscan` projects (Tech Spec
// §3.11) through the engine's record/ module. No format knowledge lives here:
// everything goes through lscan::FileRecordReader / FileRecordWriter / Engine,
// so the desktop can never drift from the container the Android app and the
// cloud worker write.
//
// Owner: C1.
#pragma once

#include <QString>
#include <QVector>

#include "scanengine/record/lscan.h"

namespace lidarscan {

struct StreamInfo {
  scanengine::StreamId id = scanengine::StreamId::kUnknown;
  QString name;
  quint64 chunks = 0;
  quint64 bytes = 0;
  qint64 t_first_ns = 0;
  qint64 t_last_ns = 0;
  double duration_s() const {
    return t_last_ns > t_first_ns ? double(t_last_ns - t_first_ns) / 1e9 : 0.0;
  }
};

struct ProjectInfo {
  QString dir;
  QString name;
  bool valid = false;
  QString error;

  bool manifest_present = false;
  bool manifest_ok = false;
  bool sealed = false;  // false after a crash — a POSITIVE signal per A5 §2
  QString manifest_raw;
  QString profile;
  // A5 manifest "crs" — empty when the session was never georeferenced (the
  // key is written as JSON null). The projects sidebar's georef badge and the
  // status bar's georef segment both read this for a project on disk; the
  // LIVE georef state during a capture comes from Engine::crs_epsg() instead.
  QString crs;

  QVector<StreamInfo> streams;

  quint32 truncated_tail_chunks = 0;
  quint32 crc_mismatch_chunks = 0;
  quint32 unreadable_streams = 0;

  quint64 total_chunks = 0;
  quint64 total_bytes = 0;
  double duration_s = 0.0;

  // Replay (record/replay.h) only forwards ChunkType::kD6Raw today, so this is
  // exactly the "can this project be replayed" test.
  bool has_d6_raw = false;
};

// Opens `dir` with lscan::FileRecordReader and summarises it. A missing or
// corrupt manifest does NOT make the project invalid — A5's reader tolerates
// it on purpose, because raw chunk data must outlive a half-written manifest.
ProjectInfo readProject(const QString& dir);

// Creates an empty `.lscan` skeleton (manifest.json + streams/ + frames/ +
// processed/ + merged/ + exports/) by opening and closing a FileRecordWriter.
bool createProject(const QString& dir, const QString& profile, QString* err);

// Turns a raw COIN-D6 byte capture (e.g. `engine_cli --synth out.bin 30`) into
// a real `.lscan` project by pushing it through a live Engine session — the
// same push_serial_bytes() path a USB serial reader uses, so the recorded
// chunks are byte-identical to a real capture's.
//
// Arrival timestamps are SYNTHESISED at the D6's wire rate (230400 8N1 =
// 23,040 bytes/s, Tech Spec §2.1) rather than stamped "now": the file is
// consumed in milliseconds, and stamping now would collapse the whole capture
// into one instant, making a 1x replay meaningless.
bool importRawD6(const QString& raw_file, const QString& project_dir, const QString& profile,
                 QString* err, quint64* bytes_in = nullptr, quint64* points_out = nullptr);

}  // namespace lidarscan
