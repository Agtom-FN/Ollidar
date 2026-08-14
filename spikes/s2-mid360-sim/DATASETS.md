# S2-sim — real Mid-360 recordings (stretch goal)

> **Follow-up (2026-08-15):** small (<25 MB) slices of these datasets are now
> extracted into `fixtures/` for E2's golden-dataset regression tests, and
> `datasets/Indoor_sampledata.lvx2` can be streamed live via
> `replay/lvx2_replay.cpp` against the real SDK2 client. See `FIXTURES.md`
> and `FOLLOWUP_NOTES.md`.

`datasets/` is **gitignored**. Re-fetch with the URLs below; every file is a direct
HTTPS download with no account or click-through.

Total footprint: **~1.31 GB**.

| File | Size | SHA-256 (first 16) | Source | Licence |
| --- | --- | --- | --- | --- |
| `Indoor_sampledata.lvx2` | 212.2 MB | `f892732ff43882b5` | Livox official Mid-360 downloads | Livox sample data, no explicit licence — vendor-published demo material |
| `Outdoor_sampledata.lvx2` | 569.0 MB | `49ef8712dcf31146` | Livox official Mid-360 downloads | as above |
| `rosbag2_2024_04_16-14_17_01.zip` | 493.1 MB (517,088,133 B) | `f8f89eebf2aaf9cc` | Zenodo 10.5281/zenodo.14841855 | **CC-BY-4.0** |

## 1. Livox official `.lvx2` samples (primary — used to validate this spike)

```
https://terra-1-g.djicdn.com/65c028cd298f4669a7f0e40e50ba1131/Mid360/Indoor_sampledata.lvx2
https://terra-1-g.djicdn.com/65c028cd298f4669a7f0e40e50ba1131/Mid360/Outdoor_sampledata.lvx2
```
Listed on <https://www.livoxtech.com/mid-360/downloads> as "Mid-360 Point Cloud Data —
Indoor / Outdoor" (published 2023-01-10). These are recordings made by Livox with
Livox Viewer 2 from real Mid-360 units.

### Measured contents (parsed directly, not taken from documentation)

| | Indoor | Outdoor |
| --- | --- | --- |
| LVX2 signature / version / magic | `livox_tech` / 2.0.0.0 / `0xAC0EA767` | same |
| Container frame duration | 50 ms | 50 ms |
| Devices | 1 (`47MDK9DF710030`) | **3** (`…710030`, `…710195`, `…710124`) |
| Container frames | 1,534 | 1,373 |
| Packages | 162,293 | 435,155 |
| Points | 15,580,128 | 41,774,880 |
| Duration (device clock) | 77.9 s | 69.6 s |
| Point rate | **200,001 pts/s** | 600,015 pts/s (3 × 200,005) |
| `data_type` present | `1` only (Cartesian 32-bit) | `1` only |
| Points per package | **exactly 96** | exactly 96 |
| IMU samples | **0** | 0 |

The outdoor file is a **three-sensor** capture — directly useful later for A13
(multi-session merge) and for exercising the SDK's multi-device handling.

### File layout as verified on disk

```
0x00  public header   : signature[16]="livox_tech", version[4]={2,0,0,0}, magic u32=0xAC0EA767
0x18  private header  : frame_duration u32 (ms), device_count u8
0x1D  device info     : 63 bytes per device (lidar_sn[16] first)
      frame blocks    : frame header {current_offset u64, next_offset u64, frame_index u64}
                        then N packages, each
                        {version u8, lidar_id u32, lidar_type u8, timestamp_type u8,
                         timestamp u64, udp_cnt u16, data_type u8, length u32,
                         frame_cnt u8, reserve[4]} = 27 B header + `length` bytes of payload
```
`current_offset` equals the frame's own byte offset, which is how the frame-block start
is located (`datasets/` probe scans for the first self-referencing `u64`). The payload
is byte-identical to the UDP payload of a live capture, which is what makes these files
a valid ground truth for the wire format.

### Caveats

- **No IMU.** Livox Viewer 2 does not write IMU packets into `.lvx2`. Lidar-inertial
  work (A6/A7) cannot use these files alone — use the Zenodo rosbag2 below, or a real
  device.
- **No licence statement.** Livox publishes these as sample data on the product
  download page but attaches no explicit licence. Treat as vendor demo material: fine
  for internal development and CI fixtures, do **not** redistribute in a shipped
  product or a public repo.

## 2. Zenodo — "Driving SLAM test with Livox MID360" (secondary, has IMU)

```
https://zenodo.org/records/14841855
https://zenodo.org/records/14841855/files/rosbag2_2024_04_16-14_17_01.zip?download=1
```
- Author: Kenji Koide (AIST) — author of `glim` / `hdl_graph_slam`.
- DOI `10.5281/zenodo.14841855`, published 2025-02-10.
- Livox Mid-360 **with IMU**, ROS 2 `rosbag2` (sqlite3 v5), 493.1 MB zipped →
  1,400.9 MB `.db3`. Zip integrity verified (`testzip` clean).
- **Licence: CC-BY-4.0** — the only one of the three that is safely redistributable.

Verified from the bag's own `metadata.yaml`:

| Topic | Type | Messages | Rate |
| --- | --- | --- | --- |
| `/livox/lidar` | `sensor_msgs/msg/PointCloud2` | 2,772 | **10.00 Hz** |
| `/livox/imu` | `sensor_msgs/msg/Imu` | 55,435 | **200.0 Hz** |
| `/rosout` | `rcl_interfaces/msg/Log` | 10 | — |

Duration 277.167 s, 58,217 messages total, recorded 2024-04-16. The 10 Hz PointCloud2
cadence and 200 Hz IMU independently corroborate the Mid-360 rates this spike targets.

This is the right fixture for A6/A7 lidar-inertial work and for E2 golden-dataset
regression tests, because it is the only candidate that carries synchronised IMU.
It is a driving (outdoor, vehicle-speed) capture, so it does not substitute for an
indoor handheld walk — which is exactly what the LidarScan capture profile targets.

Note: the Zenodo endpoint throttled and dropped the connection twice during this
spike; fetch it with `curl -C -` (resume) rather than a plain download.

## 3. Candidates evaluated and not fetched

| Candidate | Why not |
| --- | --- |
| FAST-LIO2 / Point-LIO published bags | Their released datasets are Avia / Horizon / Ouster, not Mid-360 — different FOV and scan pattern, so not a substitute for Mid-360 front-end tuning. |
| `DHA-Tappuri/lvx2_to_rosbag` | A converter, not data. Useful later if A5's replay harness wants ROS 2 interop; no dataset attached. |
| Livox Viewer 2 (Win/Ubuntu) | Not a dataset, and no macOS build — noted because it means there is **no vendor reference viewer on the S2 host OS** for cross-checking captures. |
| Livox forum thread 343 ("where to download mid360 pointcloud bag") | No usable links; answers point back at the official downloads page. |

## How to re-fetch

```sh
mkdir -p datasets && cd datasets
curl -sSL -C - -O https://terra-1-g.djicdn.com/65c028cd298f4669a7f0e40e50ba1131/Mid360/Indoor_sampledata.lvx2
curl -sSL -C - -O https://terra-1-g.djicdn.com/65c028cd298f4669a7f0e40e50ba1131/Mid360/Outdoor_sampledata.lvx2
curl -sSL -C - -o rosbag2_2024_04_16-14_17_01.zip \
  "https://zenodo.org/records/14841855/files/rosbag2_2024_04_16-14_17_01.zip?download=1"
```
