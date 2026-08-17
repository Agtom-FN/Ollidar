# E2 golden fixtures — provenance

Both files here are **committed, synthetic, deterministic**, and are read by
`engine/tests/test_e2_replay_golden.cpp` (globbed automatically as
`tests/test_e2_*.cpp` — see that file's header for why it is *not* under
`tests/integration/`). They exist to catch a decode/regression bug that a
purely-generate-at-test-time fixture cannot: byte-for-byte stability across a
refactor. If either file's bytes ever change, the golden test's checksum/count
assertions are the ones that notice.

## `golden_d6.bin` (433 bytes)

One COIN-D6 synthetic revolution: a start packet (angle 0, 1.000 m) followed
by 6 data packets of 20 samples each, spanning 360° at a constant 1.000 m
range and intensity 128. Built by `d6test::build_revolution(6, 20, 1000, 128,
10)` — the exact S1 packet-builder helper (`engine/tests/packet_builder.h`)
every other D6 unit test in this repo already trusts, including the vendor
checksum state-machine replay.

## `golden_mid360.bin` (11,160 bytes)

Eight synthetic Livox Mid-360 Cartesian-high point datagrams (96 points each,
CRC32/ISO-HDLC filled in with the engine's own `mid360::crc32_iso_hdlc`) with
two IMU datagrams interleaved after the 4th and 8th point packet. Concatenated
raw wire bytes — no container framing — because a Mid-360 datagram is
self-delimiting via `DataHeader::length`, exactly how the driver's `kInject`
backend (and a live UDP `recvfrom` loop) consumes it one datagram at a time.

## Regenerating

Both files are produced by the same builder functions the golden test itself
calls to cross-check the committed bytes (`CHECK(committed ==
build_golden_*_bytes())`), so "regenerate" only means something when the
builder's output is *meant* to change:

```sh
cmake --build build --target scanengine_tests
SCANENGINE_REGEN_GOLDEN=1 \
  ./build/scanengine_tests --test-case="e2/regenerate_golden_fixtures"
git diff --stat engine/tests/integration/data/
```

Review the diff before committing — a byte-identical rewrite is a no-op; a
changed rewrite means the builder (or something it depends on, e.g. the CRC32
routine or the D6 checksum replay) changed, which is exactly the event this
fixture is meant to catch.

---

# Real-hardware fixtures

The files below are **not** synthetic: they are bytes off real devices from
the 2026-08-17 field session (`captures/FIELD_SESSION_2026-08-17.md`). They
are never regenerated — regenerating them would mean going back to the
hardware — and a test that disagrees with them is wrong about the hardware,
not the other way around.

## `field_d6_30s.bin`, `field_um982_30s.nmea`

The COIN-D6 30 s capture (vendor checksum, 100.0000% pass) and the UM982's
30 s of NMEA at 230400 (7 sentence types at 1 Hz including `$GPTHS`, so
dual-antenna heading is detectable). `test_discovery.cpp` pushes the NMEA
one through `Um982Sniffer` in 64-byte chunks — the identification path a real
serial read takes.

## `mid360_beacon.bin` (860 bytes) — A16

**Two verbatim Mid-360 heartbeats**, back to back, lifted out of
`captures/mid360_real_30s.livoxdump` (port 56201 records 0 and 1): SN
`ARMCP7K0034759` at 192.168.1.159, netmask 255.255.255.0, gateway
192.168.1.1, with host **192.168.1.5** persisted for both the point stream
(56301) and the IMU (56401), firmware `35010108` built 2025/06/09.

Each record is self-delimiting via the `u16` frame length at offset 2, so the
file is consumed the same way a `recvfrom` loop consumes the wire — no
container framing. The two frames differ only in the sequence number, the
core temperature and the device clock, which is what
`discovery/mid360_receives_and_dedups_a_real_heartbeat` uses to prove the
dedup merges them into ONE lidar.

Extracted with:

```sh
python3 - <<'PY'
import struct
f = open('captures/mid360_real_30s.livoxdump', 'rb')
_, _, n = struct.unpack('<8sHH', f.read(12))
ports = struct.unpack('<%dI' % n, f.read(4 * n))
out = []
while len(out) < 2:
    h = f.read(14)
    if len(h) < 14: break
    _, pi, ln = struct.unpack('<QHI', h)
    p = f.read(ln)
    if ports[pi] == 56201: out.append(p)
open('engine/tests/integration/data/mid360_beacon.bin', 'wb').write(b''.join(out))
PY
```

`sha256 = e1943b9361544d1c178d16ea018b3f4c379849b2d8036c413468f7759b77f887`
