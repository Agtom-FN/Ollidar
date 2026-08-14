#!/usr/bin/env python3
"""livoxdump.py -- shared reader/writer for the ".livoxdump" container and for
building Mid-360 live-wire-format point/IMU packets (matches sim/livox_wire.h).

Container format (documented originally in
tools/remote-capture/capture_mid360.py, reused as-is here so E2 fixtures and
real remote-capture files share one format and one verifier
(tools/remote-capture/verify_capture.py)):

  Header (fixed):
    8 bytes   magic       b"LX360CAP"
    u16 LE    version     currently 1
    u16 LE    num_ports   N  (number of UDP ports represented)
    N x u32 LE  port_table  the UDP port number for port_idx 0..N-1

  Then zero or more records, back to back, until EOF:
    u64 LE    t_ns        nanoseconds (see NOTE below)
    u16 LE    port_idx    index into port_table
    u32 LE    len         payload length in bytes
    len bytes payload     the raw UDP datagram payload, byte for byte -- i.e.
                          exactly what a live capture on that port would show:
                          36-byte DataHeader + point/IMU data (see
                          sim/livox_wire.h)

  NOTE on t_ns for fixtures (as opposed to a live tools/remote-capture
  recording): a fixture has no real capture wall-clock, so t_ns is a
  synthetic anchor epoch plus the REAL relative inter-packet timing recorded
  in the source data -- i.e. differences between consecutive t_ns values are
  meaningful and reproduce the real pacing; the absolute epoch is not
  meaningful. See FIXTURES.md for the anchor used and per-fixture
  real-vs-synthesized provenance.

Wire packet builder: mirrors sim/livox_wire.h's DataHeader/CartesianHigh/
ImuSample byte layout and its Crc32() (CRC-32/ISO-HDLC, the standard zlib/
gzip/PNG polynomial -- Python's zlib.crc32 implements exactly this variant;
also cross-checked: the SDK's own data-packet handler does not verify this
CRC at all, only the control-frame path does -- grep sdk_core/data_handler.cpp).
"""
import struct
import zlib

MAGIC = b"LX360CAP"
VERSION = 1

_FILE_HDR_FIXED = struct.Struct("<8sHH")
_RECORD_HDR = struct.Struct("<QHI")

# sim/livox_wire.h DataHeader (36 bytes, little endian):
#   version u8, length u16, time_interval u16, dot_num u16, udp_cnt u16,
#   frame_cnt u8, data_type u8, time_type u8, rsvd[12], crc32 u32, timestamp u64
_DATA_HEADER = struct.Struct("<BHHHHBBB12sIQ")
assert _DATA_HEADER.size == 36, _DATA_HEADER.size

# CartesianHigh (data_type == 1): int32 x,y,z (mm), u8 reflectivity, u8 tag
_CARTESIAN = struct.Struct("<iiiBB")
assert _CARTESIAN.size == 14, _CARTESIAN.size

# ImuSample (data_type == 0): 6x float32, gyro xyz (rad/s) then acc xyz (g)
_IMU = struct.Struct("<ffffff")
assert _IMU.size == 24, _IMU.size

POINTS_PER_PACKET = 96
POINT_PACKET_BYTES = _DATA_HEADER.size + POINTS_PER_PACKET * _CARTESIAN.size
assert POINT_PACKET_BYTES == 1380, POINT_PACKET_BYTES
IMU_PACKET_BYTES = _DATA_HEADER.size + _IMU.size
assert IMU_PACKET_BYTES == 60, IMU_PACKET_BYTES


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def build_point_packet(*, udp_cnt, frame_cnt, timestamp_ns, time_interval_01us, points,
                       data_type=1, time_type=0):
    """points: iterable of (x_mm, y_mm, z_mm, reflectivity, tag), any count N.
    Returns the full N*14+36 byte wire packet (DataHeader + CartesianHigh[N])."""
    pts_bytes = b"".join(_CARTESIAN.pack(x, y, z, r, t) for (x, y, z, r, t) in points)
    n = len(pts_bytes) // _CARTESIAN.size
    length = _DATA_HEADER.size + len(pts_bytes)
    body_for_crc = struct.pack("<Q", timestamp_ns) + pts_bytes
    hdr = _DATA_HEADER.pack(0, length, time_interval_01us, n, udp_cnt & 0xFFFF,
                            frame_cnt & 0xFF, data_type, time_type, b"\x00" * 12,
                            crc32(body_for_crc), timestamp_ns)
    return hdr + pts_bytes


def build_imu_packet(*, udp_cnt, frame_cnt, timestamp_ns, gyro_xyz, acc_xyz,
                     data_type=0, time_type=0):
    imu_bytes = _IMU.pack(*gyro_xyz, *acc_xyz)
    length = IMU_PACKET_BYTES
    body_for_crc = struct.pack("<Q", timestamp_ns) + imu_bytes
    hdr = _DATA_HEADER.pack(0, length, 0, 1, udp_cnt & 0xFFFF, frame_cnt & 0xFF, data_type,
                            time_type, b"\x00" * 12, crc32(body_for_crc), timestamp_ns)
    return hdr + imu_bytes


def parse_data_packet(raw: bytes):
    """Inverse of build_point_packet/build_imu_packet -- returns a dict of
    header fields plus 'points' (list of tuples) or 'imu' (gyro, acc)."""
    if len(raw) < _DATA_HEADER.size:
        raise ValueError("packet shorter than DataHeader")
    (version, length, time_interval, dot_num, udp_cnt, frame_cnt, data_type, time_type,
     _rsvd, crc, timestamp) = _DATA_HEADER.unpack_from(raw, 0)
    out = dict(version=version, length=length, time_interval=time_interval, dot_num=dot_num,
               udp_cnt=udp_cnt, frame_cnt=frame_cnt, data_type=data_type, time_type=time_type,
               crc32=crc, timestamp_ns=timestamp)
    payload = raw[_DATA_HEADER.size:]
    if data_type == 1:
        out["points"] = [_CARTESIAN.unpack_from(payload, i * _CARTESIAN.size)
                         for i in range(len(payload) // _CARTESIAN.size)]
    elif data_type == 0 and len(payload) >= _IMU.size:
        vals = _IMU.unpack_from(payload, 0)
        out["gyro"] = vals[0:3]
        out["acc"] = vals[3:6]
    return out


class LivoxDumpWriter:
    """Write records in the SAME order they should be replayed (records must
    already be time-sorted across ports if simultaneous point+IMU streams are
    interleaved -- callers are responsible for merge-sorting by t_ns)."""

    def __init__(self, path, ports):
        self.ports = list(ports)
        self.f = open(path, "wb")
        self.f.write(_FILE_HDR_FIXED.pack(MAGIC, VERSION, len(self.ports)))
        for p in self.ports:
            self.f.write(struct.pack("<I", p))
        self.n_records = 0
        self.n_bytes = 0

    def write(self, t_ns, port_idx, payload: bytes):
        self.f.write(_RECORD_HDR.pack(t_ns, port_idx, len(payload)))
        self.f.write(payload)
        self.n_records += 1
        self.n_bytes += len(payload)

    def close(self):
        self.f.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


class LivoxDumpReader:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        hdr = self.f.read(_FILE_HDR_FIXED.size)
        magic, version, num_ports = _FILE_HDR_FIXED.unpack(hdr)
        if magic != MAGIC:
            raise ValueError(f"{path}: bad magic {magic!r}")
        self.version = version
        self.port_table = []
        for _ in range(num_ports):
            self.port_table.append(struct.unpack("<I", self.f.read(4))[0])

    def records(self):
        while True:
            hdr = self.f.read(_RECORD_HDR.size)
            if len(hdr) == 0:
                break
            if len(hdr) < _RECORD_HDR.size:
                break  # truncated trailing record
            t_ns, port_idx, length = _RECORD_HDR.unpack(hdr)
            payload = self.f.read(length)
            if len(payload) < length:
                break
            yield t_ns, port_idx, payload

    def close(self):
        self.f.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
