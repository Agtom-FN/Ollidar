#!/usr/bin/env python3
"""fixture_stats.py -- print per-fixture stats for one or more .livoxdump
fixtures: duration, pts/s, no-return %, tag histogram, IMU rate.

Usage:
  python3 scripts/fixture_stats.py fixtures/*.livoxdump
  python3 scripts/fixture_stats.py fixtures/indoor_livox_5s.livoxdump --json
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import livoxdump as ld  # noqa: E402


def stats_for_file(path):
    reader = ld.LivoxDumpReader(path)
    n_point_pkts = 0
    n_imu_pkts = 0
    n_points = 0
    n_zero = 0
    tag_hist = {}
    dev_ts_min = None
    dev_ts_max = None
    imu_ts_min = None
    imu_ts_max = None
    rec_t_min = None
    rec_t_max = None
    udp_cnt_gaps = 0
    prev_udp_cnt = None
    dot_num_bad = 0
    per_port_pkts = [0] * len(reader.port_table)
    per_port_bytes = [0] * len(reader.port_table)

    for t_ns, port_idx, payload in reader.records():
        rec_t_min = t_ns if rec_t_min is None else min(rec_t_min, t_ns)
        rec_t_max = t_ns if rec_t_max is None else max(rec_t_max, t_ns)
        if port_idx < len(per_port_pkts):
            per_port_pkts[port_idx] += 1
            per_port_bytes[port_idx] += len(payload)
        pkt = ld.parse_data_packet(payload)
        if pkt["data_type"] == 1:
            n_point_pkts += 1
            if pkt["dot_num"] != 96:
                dot_num_bad += 1
            dev_ts_min = pkt["timestamp_ns"] if dev_ts_min is None else min(dev_ts_min, pkt["timestamp_ns"])
            dev_ts_max = pkt["timestamp_ns"] if dev_ts_max is None else max(dev_ts_max, pkt["timestamp_ns"])
            if prev_udp_cnt is not None:
                gap = (pkt["udp_cnt"] - prev_udp_cnt) & 0xFFFF
                if gap > 1:
                    udp_cnt_gaps += 1
            prev_udp_cnt = pkt["udp_cnt"]
            for (x, y, z, _refl, tag) in pkt["points"]:
                n_points += 1
                if x == 0 and y == 0 and z == 0:
                    n_zero += 1
                tag_hist[tag] = tag_hist.get(tag, 0) + 1
        elif pkt["data_type"] == 0:
            n_imu_pkts += 1
            imu_ts_min = pkt["timestamp_ns"] if imu_ts_min is None else min(imu_ts_min, pkt["timestamp_ns"])
            imu_ts_max = pkt["timestamp_ns"] if imu_ts_max is None else max(imu_ts_max, pkt["timestamp_ns"])
    reader.close()

    dev_duration_s = (dev_ts_max - dev_ts_min) / 1e9 if dev_ts_min is not None and dev_ts_max is not None and dev_ts_max > dev_ts_min else 0.0
    rec_duration_s = (rec_t_max - rec_t_min) / 1e9 if rec_t_min is not None and rec_t_max is not None else 0.0
    duration_s = dev_duration_s or rec_duration_s
    imu_duration_s = (imu_ts_max - imu_ts_min) / 1e9 if imu_ts_min is not None and imu_ts_max is not None and imu_ts_max > imu_ts_min else 0.0

    return dict(
        path=path,
        size_bytes=os.path.getsize(path),
        port_table=reader.port_table,
        per_port_packets=per_port_pkts,
        per_port_bytes=per_port_bytes,
        duration_s=duration_s,
        point_packets=n_point_pkts,
        points=n_points,
        pts_per_s=(n_points / duration_s) if duration_s > 0 else 0.0,
        no_return_pct=(100.0 * n_zero / n_points) if n_points else 0.0,
        tag_histogram=dict(sorted(tag_hist.items())),
        imu_packets=n_imu_pkts,
        imu_hz=(n_imu_pkts / imu_duration_s) if imu_duration_s > 0 else 0.0,
        udp_cnt_gaps=udp_cnt_gaps,
        packets_with_bad_dot_num=dot_num_bad,
    )


def print_human(s):
    print(f"=== {s['path']} ===")
    print(f"  size                : {s['size_bytes']:,} bytes ({s['size_bytes']/1e6:.2f} MB)")
    print(f"  ports               : {s['port_table']}  packets/port {s['per_port_packets']}  "
          f"bytes/port {s['per_port_bytes']}")
    print(f"  duration (dev clock): {s['duration_s']:.2f} s")
    print(f"  point packets       : {s['point_packets']:,}  (bad dot_num: {s['packets_with_bad_dot_num']})")
    print(f"  points              : {s['points']:,}")
    print(f"  pts/s               : {s['pts_per_s']:.0f}")
    print(f"  no-return           : {s['no_return_pct']:.2f}%")
    print(f"  tag histogram       : {s['tag_histogram']}")
    print(f"  IMU packets         : {s['imu_packets']:,}  ({s['imu_hz']:.2f} Hz)")
    print(f"  udp_cnt gaps (>1)   : {s['udp_cnt_gaps']}")
    print()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--json", action="store_true", help="emit JSON instead of a human-readable report")
    args = ap.parse_args()

    results = [stats_for_file(f) for f in args.files]
    if args.json:
        print(json.dumps(results, indent=2))
    else:
        for s in results:
            print_human(s)


if __name__ == "__main__":
    main()
