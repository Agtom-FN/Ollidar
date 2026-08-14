#!/usr/bin/env python3
"""make_fixtures.py -- regenerate E2's small real-data fixtures from the
public datasets in datasets/ (see DATASETS.md to re-fetch them).

Produces (see FIXTURES.md for the full provenance/licence table):
  fixtures/indoor_livox_5s.livoxdump        -- from Livox's own Indoor_sampledata.lvx2
                                               (regenerate-only, not committed)
  fixtures/outdoor_livox_5s.livoxdump       -- from Livox's own Outdoor_sampledata.lvx2,
                                               single-device slice
                                               (regenerate-only, not committed)
  fixtures/outdoor_imu_ccby_6s.livoxdump    -- from the Zenodo CC-BY-4.0 rosbag2
                                               (10.5281/zenodo.14841855), point+IMU
                                               (COMMITTABLE -- CC-BY-4.0)

All three share the .livoxdump container (scripts/livoxdump.py, same framing
tools/remote-capture/capture_mid360.py and verify_capture.py already use) and
the same live-wire packet format (sim/livox_wire.h). scripts/fixture_stats.py
reads any of them uniformly.

Usage:
  python3 scripts/make_fixtures.py --all
  python3 scripts/make_fixtures.py --indoor --outdoor       # skip the Zenodo one (needs `rosbags`)
  python3 scripts/make_fixtures.py --zenodo --zenodo-extract-dir /path/to/unzipped/rosbag2_dir

The Zenodo step needs the pure-Python `rosbags` package (no ROS install
required): pip install rosbags
"""
import argparse
import os
import struct
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import livoxdump as ld  # noqa: E402
from lvx2_reader import Lvx2Reader  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SPIKE_ROOT = os.path.dirname(HERE)
DATASETS = os.path.join(SPIKE_ROOT, "datasets")
FIXTURES = os.path.join(SPIKE_ROOT, "fixtures")

POINT_PORT = 56300
IMU_PORT = 56400

# A fixed, clearly-synthetic anchor epoch for t_ns (see livoxdump.py's NOTE).
# Chosen as an arbitrary readable date; only relative deltas are meaningful.
ANCHOR_NS = 1_700_000_000_000_000_000


def lvx2_to_livoxdump(src_path, out_path, target_duration_s, lidar_id_filter=None, label=""):
    """Slice the first `target_duration_s` seconds (by device clock) of
    data_type==1 packages from an .lvx2 file into a single-port .livoxdump
    fixture. Every field except the wrapper crc32/time_interval is REAL,
    replayed byte-for-byte from the recording (see FIXTURES.md)."""
    reader = Lvx2Reader(src_path)
    print(f"[{label}] source: {src_path}")
    print(f"[{label}]   signature={reader.signature} devices={reader.devices} "
          f"frame_duration_ms={reader.frame_duration_ms}")

    kept = []
    t0 = None
    for pkg in reader.packages():
        if pkg.data_type != 1:
            continue
        if lidar_id_filter is not None and pkg.lidar_id != lidar_id_filter:
            continue
        if t0 is None:
            t0 = pkg.timestamp_ns
        kept.append(pkg)
        if (pkg.timestamp_ns - t0) / 1e9 >= target_duration_s:
            break
    reader.close()

    if not kept:
        raise RuntimeError(f"{src_path}: no matching packages found (lidar_id_filter={lidar_id_filter})")

    n_points = 0
    n_zero = 0
    with ld.LivoxDumpWriter(out_path, ports=[POINT_PORT]) as w:
        for pkg in kept:
            n = len(pkg.payload) // 14
            points = struct.iter_unpack("<iiiBB", pkg.payload)
            wire = ld.build_point_packet(
                udp_cnt=pkg.udp_cnt,          # REAL
                frame_cnt=pkg.frame_cnt,      # REAL
                timestamp_ns=pkg.timestamp_ns - t0,  # real deltas, rebased origin
                time_interval_01us=int((n - 1) * 1e7 / 200000.0) if n > 1 else 0,  # SYNTHESIZED
                points=points,                # REAL x/y/z/reflectivity/tag
            )
            t_ns = ANCHOR_NS + (pkg.timestamp_ns - t0)
            w.write(t_ns, 0, wire)
            n_points += n
            n_zero += sum(1 for (x, y, z, _r, _t) in struct.iter_unpack("<iiiBB", pkg.payload)
                          if x == 0 and y == 0 and z == 0)

    dur_s = (kept[-1].timestamp_ns - t0) / 1e9
    size = os.path.getsize(out_path)
    print(f"[{label}] wrote {out_path}")
    print(f"[{label}]   {len(kept)} packets, {n_points} points, {dur_s:.2f}s device-clock span, "
          f"{size/1e6:.2f} MB, no-return {100.0*n_zero/n_points:.2f}%")
    return out_path


def make_indoor(target_s=5.0):
    return lvx2_to_livoxdump(
        os.path.join(DATASETS, "Indoor_sampledata.lvx2"),
        os.path.join(FIXTURES, "indoor_livox_5s.livoxdump"),
        target_duration_s=target_s,
        label="indoor",
    )


def make_outdoor(target_s=5.0):
    # Outdoor_sampledata.lvx2 is a genuine 3-device capture (see DATASETS.md).
    # A fixture is meant to look like ONE Mid-360's live stream, so we take a
    # single device's packages (lidar_id 738306240 = SN 47MDK9DF710030, the
    # same physical unit as the indoor sample) rather than interleaving three
    # independent udp_cnt sequences into one stream, which no live driver
    # would ever see on a single socket pair.
    return lvx2_to_livoxdump(
        os.path.join(DATASETS, "Outdoor_sampledata.lvx2"),
        os.path.join(FIXTURES, "outdoor_livox_5s.livoxdump"),
        target_duration_s=target_s,
        lidar_id_filter=738306240,
        label="outdoor",
    )


def _extract_zenodo_db3(zip_path, extract_dir):
    with zipfile.ZipFile(zip_path) as zf:
        names = zf.namelist()
        db3 = next(n for n in names if n.endswith(".db3"))
        yaml = next(n for n in names if n.endswith("metadata.yaml"))
        bag_dir = os.path.join(extract_dir, os.path.dirname(db3))
        os.makedirs(bag_dir, exist_ok=True)
        for n in (db3, yaml):
            target = os.path.join(extract_dir, n)
            if os.path.exists(target) and os.path.getsize(target) == zf.getinfo(n).file_size:
                continue
            print(f"[zenodo] extracting {n} ...")
            zf.extract(n, extract_dir)
        return bag_dir


def make_zenodo_imu(target_s=6.0, window_start_s=90.0, extract_dir=None, keep_extract=False):
    try:
        from rosbags.highlevel import AnyReader
        import numpy as np
    except ImportError as exc:
        raise RuntimeError(
            "the Zenodo fixture needs the pure-Python 'rosbags' package (and numpy): "
            "pip install rosbags numpy"
        ) from exc

    zip_path = os.path.join(DATASETS, "rosbag2_2024_04_16-14_17_01.zip")
    tmp_created = False
    if extract_dir is None:
        extract_dir = os.path.join(FIXTURES, ".zenodo_extract_tmp")
        tmp_created = True
    bag_dir = _extract_zenodo_db3(zip_path, extract_dir)
    print(f"[zenodo] bag dir: {bag_dir}")

    dtype = np.dtype([("x", "<f4"), ("y", "<f4"), ("z", "<f4"), ("intensity", "<f4"),
                      ("tag", "u1"), ("line", "u1"), ("timestamp", "<u8")])

    out_path = os.path.join(FIXTURES, "outdoor_imu_ccby_6s.livoxdump")
    n_pc_msgs = 0
    n_imu_msgs = 0
    n_points = 0
    n_zero = 0
    n_point_pkts = 0

    with AnyReader([__import__("pathlib").Path(bag_dir)]) as reader:
        start_ns = reader.start_time
        window_lo = start_ns + int(window_start_s * 1e9)
        window_hi = window_lo + int(target_s * 1e9)
        t0 = window_lo

        pc_conns = [c for c in reader.connections if c.topic == "/livox/lidar"]
        imu_conns = [c for c in reader.connections if c.topic == "/livox/imu"]

        # Collect and merge-sort by header timestamp so replay order is correct.
        records = []  # (t_ns_absolute, kind, msg_or_arr)

        udp_cnt = 0
        pending_pts = []  # list of (x_mm,y_mm,z_mm,refl,tag) not yet flushed into a 96-pt packet
        pending_t_ns = None

        for conn, ts, rawdata in reader.messages(connections=pc_conns):
            if ts < window_lo or ts > window_hi:
                continue
            msg = reader.deserialize(rawdata, conn.msgtype)
            hdr_ns = msg.header.stamp.sec * 1_000_000_000 + msg.header.stamp.nanosec
            arr = np.frombuffer(bytes(msg.data), dtype=dtype)
            n_pc_msgs += 1
            npts = len(arr)
            # SYNTHESIZED even 200,000 pts/s spacing within the message's real
            # 10 Hz header timestamp (see FIXTURES.md: the ROS message's raw
            # per-point 'timestamp' field's absolute scale did not reconcile
            # cleanly with the known 10 Hz cadence during validation, so we
            # do not trust it for inter-packet pacing here -- x/y/z/
            # intensity/tag themselves ARE real, unmodified per-point data).
            for i in range(npts):
                t_pt_ns = hdr_ns + int(i * 1e9 / 200000.0) - t0
                x_mm = int(round(float(arr["x"][i]) * 1000.0))
                y_mm = int(round(float(arr["y"][i]) * 1000.0))
                z_mm = int(round(float(arr["z"][i]) * 1000.0))
                refl = max(0, min(255, int(round(float(arr["intensity"][i])))))
                tag = int(arr["tag"][i])
                pending_pts.append((x_mm, y_mm, z_mm, refl, tag))
                if pending_t_ns is None:
                    pending_t_ns = t_pt_ns
                if x_mm == 0 and y_mm == 0 and z_mm == 0:
                    n_zero += 1
                if len(pending_pts) == ld.POINTS_PER_PACKET:
                    wire = ld.build_point_packet(
                        udp_cnt=udp_cnt, frame_cnt=0, timestamp_ns=max(0, pending_t_ns),
                        time_interval_01us=int((ld.POINTS_PER_PACKET - 1) * 1e7 / 200000.0),
                        points=pending_pts,
                    )
                    records.append((ANCHOR_NS + max(0, pending_t_ns), 0, wire))
                    udp_cnt += 1
                    n_point_pkts += 1
                    n_points += len(pending_pts)
                    pending_pts = []
                    pending_t_ns = None
            # trailing partial group (<96 points) at end of window is dropped
            # by design -- every packet in this fixture has dot_num==96, same
            # as the real wire format guarantees.

        imu_udp_cnt = 0
        for conn, ts, rawdata in reader.messages(connections=imu_conns):
            if ts < window_lo or ts > window_hi:
                continue
            msg = reader.deserialize(rawdata, conn.msgtype)
            hdr_ns = msg.header.stamp.sec * 1_000_000_000 + msg.header.stamp.nanosec
            n_imu_msgs += 1
            t_ns = hdr_ns - t0
            wire = ld.build_imu_packet(
                udp_cnt=imu_udp_cnt, frame_cnt=0, timestamp_ns=max(0, t_ns),
                gyro_xyz=(msg.angular_velocity.x, msg.angular_velocity.y, msg.angular_velocity.z),
                acc_xyz=(msg.linear_acceleration.x, msg.linear_acceleration.y,
                        msg.linear_acceleration.z),
            )
            records.append((ANCHOR_NS + max(0, t_ns), 1, wire))
            imu_udp_cnt += 1

    records.sort(key=lambda r: r[0])
    with ld.LivoxDumpWriter(out_path, ports=[POINT_PORT, IMU_PORT]) as w:
        for t_ns, port_idx, wire in records:
            w.write(t_ns, port_idx, wire)

    size = os.path.getsize(out_path)
    print(f"[zenodo] wrote {out_path}")
    print(f"[zenodo]   window {window_start_s:.1f}s..{window_start_s+target_s:.1f}s into the bag "
          f"(bag duration ~277.2s)")
    no_return_pct = (100.0 * n_zero / n_points) if n_points else 0.0
    print(f"[zenodo]   {n_pc_msgs} PointCloud2 msgs -> {n_point_pkts} 96-pt packets "
          f"({n_points} points), {n_imu_msgs} IMU msgs, {size/1e6:.2f} MB, "
          f"no-return {no_return_pct:.2f}%")

    if tmp_created and not keep_extract:
        import shutil
        print(f"[zenodo] cleaning up extracted bag ({bag_dir}) -- pass --keep-extract to keep it")
        shutil.rmtree(extract_dir, ignore_errors=True)

    return out_path


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--indoor", action="store_true")
    ap.add_argument("--outdoor", action="store_true")
    ap.add_argument("--zenodo", action="store_true")
    ap.add_argument("--duration", type=float, default=None, help="override target slice duration (s)")
    ap.add_argument("--zenodo-window-start", type=float, default=90.0,
                    help="offset in seconds into the Zenodo bag to start the IMU slice (default 90s, "
                         "skips the startup/parking-lot segment)")
    ap.add_argument("--zenodo-extract-dir", default=None,
                    help="reuse an already-extracted rosbag2 dir instead of unzipping to a temp dir")
    ap.add_argument("--keep-extract", action="store_true",
                    help="keep the temporary unzipped rosbag2 .db3 (large, ~1.4 GB) instead of deleting it")
    args = ap.parse_args()

    if not (args.all or args.indoor or args.outdoor or args.zenodo):
        ap.error("pass --all, or one or more of --indoor/--outdoor/--zenodo")

    os.makedirs(FIXTURES, exist_ok=True)

    if args.all or args.indoor:
        make_indoor(target_s=args.duration or 5.0)
    if args.all or args.outdoor:
        make_outdoor(target_s=args.duration or 5.0)
    if args.all or args.zenodo:
        make_zenodo_imu(target_s=args.duration or 6.0, window_start_s=args.zenodo_window_start,
                        extract_dir=args.zenodo_extract_dir, keep_extract=args.keep_extract)


if __name__ == "__main__":
    main()
