#!/usr/bin/env python3
"""lvx2_reader.py -- minimal Livox .lvx2 container reader, Python port.

Mirrors sim/lvx2_reader.h (the C++ reader used by replay/lvx2_replay.cpp) so
the two implementations describe the same format independently -- this one
is used by scripts/make_fixtures.py and scripts/fixture_stats.py, which have
no reason to link/build the C++ tree. See sim/lvx2_reader.h for the format
citation and DATASETS.md for the byte-level provenance.

Standard library only (struct), no third-party dependencies.
"""
import struct
from dataclasses import dataclass

_PUB_HDR = struct.Struct("<16s4sI")          # signature, version, magic
_PRIV_HDR = struct.Struct("<IB")             # frame_duration_ms, device_count
_DEV_INFO_LEN = 63
_FRAME_HDR = struct.Struct("<QQQ")           # current_offset, next_offset, frame_index
_PKG_HDR = struct.Struct("<BIBBQHBIB4s")     # version, lidar_id, lidar_type, ts_type,
                                              # timestamp, udp_cnt, data_type, length,
                                              # frame_cnt, reserve[4]
LVX2_MAGIC = 0xAC0EA767

assert _PKG_HDR.size == 27, _PKG_HDR.size


@dataclass
class Lvx2Package:
    frame_index: int
    lidar_id: int
    timestamp_type: int
    timestamp_ns: int   # device clock, ns (time_type=0 semantics on the live wire)
    udp_cnt: int
    data_type: int       # 0 = IMU, 1 = cartesian 32-bit
    frame_cnt: int
    payload: bytes        # raw point/IMU payload, NOT the 36 B on-wire DataHeader


class Lvx2Reader:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        data = self.f.read(_PUB_HDR.size)
        if len(data) < _PUB_HDR.size:
            raise ValueError(f"{path}: truncated public header")
        sig, ver, magic = _PUB_HDR.unpack(data)
        if magic != LVX2_MAGIC:
            raise ValueError(f"{path}: bad magic {magic:#x} (expected {LVX2_MAGIC:#x})")
        self.signature = sig.split(b"\x00")[0].decode(errors="replace")
        self.version = tuple(ver)
        priv = self.f.read(_PRIV_HDR.size)
        self.frame_duration_ms, self.device_count = _PRIV_HDR.unpack(priv)
        self.devices = []
        for _ in range(self.device_count):
            dev = self.f.read(_DEV_INFO_LEN)
            sn = dev[:16].split(b"\x00")[0].decode(errors="replace")
            self.devices.append(sn)
        self.frame_block_start = self.f.tell()

    def packages(self):
        """Yield Lvx2Package for every package in the file, in file order."""
        off = self.frame_block_start
        while True:
            self.f.seek(off)
            hdr = self.f.read(_FRAME_HDR.size)
            if len(hdr) < _FRAME_HDR.size:
                break
            cur_off, next_off, frame_idx = _FRAME_HDR.unpack(hdr)
            if cur_off != off:
                raise ValueError(f"{self.path}: frame self-offset mismatch at {off}: {cur_off}")
            pkg_end = next_off if next_off != 0 else None
            pos = off + _FRAME_HDR.size
            while pkg_end is None or pos < pkg_end:
                self.f.seek(pos)
                ph = self.f.read(_PKG_HDR.size)
                if len(ph) < _PKG_HDR.size:
                    break
                (version, lidar_id, lidar_type, ts_type, ts, udp_cnt, data_type,
                 length, frame_cnt, _rsvd) = _PKG_HDR.unpack(ph)
                payload = self.f.read(length)
                if len(payload) < length:
                    break
                yield Lvx2Package(frame_idx, lidar_id, ts_type, ts, udp_cnt, data_type,
                                   frame_cnt, payload)
                pos += _PKG_HDR.size + length
            if next_off == 0:
                break
            off = next_off

    def close(self):
        self.f.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
